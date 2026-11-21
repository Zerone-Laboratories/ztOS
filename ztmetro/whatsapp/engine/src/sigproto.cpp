#include "sigproto.h"

#include <algorithm>

#include "pb.h"

namespace zw::signal {

namespace {
constexpr uint8_t kVersion = 0x33;  // current=3, min=3
constexpr uint32_t kMaxFuture = 2000;
constexpr size_t kMaxSkipped = 2000;
constexpr size_t kMaxPrevStates = 40;
constexpr size_t kMaxRecvChains = 5;
constexpr size_t kMaxSenderStates = 5;

Bytes withType(const Key32& k) {
    Bytes o{5};
    append(o, k);
    return o;
}
Key32 fromTyped(const Bytes& b) {
    if (b.size() == 33 && b[0] == 5) return toKey(Bytes(b.begin() + 1, b.end()));
    if (b.size() == 32) return toKey(b);
    throw DecryptError("bad public key");
}

struct MsgKeys {
    Bytes cipher, mac, iv;
    uint32_t index;
};
MsgKeys deriveMsgKeys(const Bytes& chain, uint32_t index) {
    Bytes seed = hmacSha256(chain, Bytes{1});
    Bytes o = hkdf(seed, {}, "WhisperMessageKeys", 80);
    return {Bytes(o.begin(), o.begin() + 32), Bytes(o.begin() + 32, o.begin() + 64),
            Bytes(o.begin() + 64, o.end()), index};
}
Bytes nextChain(const Bytes& chain) { return hmacSha256(chain, Bytes{2}); }

void createChain(const Bytes& root, const Key32& theirs, const KeyPair& ours, Bytes& newRoot, Bytes& chain) {
    Bytes o = hkdf(B(x25519(ours.priv, theirs)), root, "WhisperRatchet", 64);
    newRoot.assign(o.begin(), o.begin() + 32);
    chain.assign(o.begin() + 32, o.end());
}

Bytes macOf(const Key32& sender, const Key32& receiver, const Bytes& mackey, const Bytes& body) {
    Bytes in = withType(sender);
    append(in, withType(receiver));
    append(in, body);
    Bytes m = hmacSha256(mackey, in);
    m.resize(8);
    return m;
}

Bytes buildSignalMessage(const State& s, const MsgKeys& k, const Bytes& ct) {
    pb::Writer w;
    w.b(1, withType(s.senderRatchet.pub)).u(2, k.index).u(3, s.previousCounter).b(4, ct);
    Bytes body{kVersion};
    append(body, w.out);
    Bytes mac = macOf(s.localIdentity, s.remoteIdentity, k.mac, body);
    append(body, mac);
    return body;
}

void initCommon(State& s, const Bytes& master) {
    Bytes o = hkdf(master, {}, "WhisperText", 64);
    s.root.assign(o.begin(), o.begin() + 32);
    s.senderKey.assign(o.begin() + 32, o.end());
}

void addRecv(State& s, const Key32& ratchet, Bytes key) {
    RecvChain c;
    c.ratchet = ratchet;
    c.key = std::move(key);
    s.recv.insert(s.recv.begin(), std::move(c));
    if (s.recv.size() > kMaxRecvChains) s.recv.pop_back();
}

// Decrypts against one state, mutating it. The caller works on a copy.
Bytes decryptWith(State& s, const Bytes& wire) {
    if (wire.size() < 1 + 8) throw DecryptError("short message");
    if ((wire[0] >> 4) != 3) throw DecryptError("unsupported version");
    pb::Msg m;
    try {
        m.parse(wire.data() + 1, wire.size() - 9);
    } catch (const std::exception&) {
        throw DecryptError("bad message");
    }
    Key32 ratchet = fromTyped(m.bytes(1));
    uint32_t counter = (uint32_t)m.num(2);
    Bytes ct = m.bytes(4);
    if (!m.has(1) || !m.has(4)) throw DecryptError("incomplete message");
    if (s.senderKey.empty()) throw DecryptError("no sender chain");

    RecvChain* ch = nullptr;
    for (auto& c : s.recv)
        if (c.ratchet == ratchet) ch = &c;
    if (!ch) {
        Bytes newRoot, recvKey;
        createChain(s.root, ratchet, s.senderRatchet, newRoot, recvKey);
        KeyPair fresh = genKeyPair();
        Bytes root2, sendKey;
        createChain(newRoot, ratchet, fresh, root2, sendKey);
        s.root = root2;
        addRecv(s, ratchet, recvKey);
        ch = &s.recv.front();
        s.previousCounter = s.senderIndex > 0 ? s.senderIndex - 1 : 0;
        s.senderRatchet = fresh;
        s.senderKey = sendKey;
        s.senderIndex = 0;
    }

    MsgKeys k;
    if (counter < ch->index) {
        auto it = std::find_if(ch->skipped.begin(), ch->skipped.end(),
                               [&](auto& x) { return x.index == counter; });
        if (it == ch->skipped.end()) throw DuplicateError("old counter");
        k = {it->cipher, it->mac, it->iv, it->index};
        ch->skipped.erase(it);
    } else {
        if (counter - ch->index > kMaxFuture) throw DecryptError("too far ahead");
        Bytes key = ch->key;
        uint32_t i = ch->index;
        while (i < counter) {
            MsgKeys sk = deriveMsgKeys(key, i);
            ch->skipped.push_back({i, sk.cipher, sk.mac, sk.iv});
            key = nextChain(key);
            i++;
        }
        if (ch->skipped.size() > kMaxSkipped)
            ch->skipped.erase(ch->skipped.begin(), ch->skipped.end() - kMaxSkipped);
        k = deriveMsgKeys(key, counter);
        ch->key = nextChain(key);
        ch->index = counter + 1;
    }
    Bytes body(wire.begin(), wire.end() - 8), mac(wire.end() - 8, wire.end());
    if (!ctEqual(macOf(s.remoteIdentity, s.localIdentity, k.mac, body), mac))
        throw DecryptError("bad mac");
    Bytes pt;
    try {
        pt = aesCbcDecrypt(k.cipher, k.iv, ct);
    } catch (const std::exception&) {
        throw DecryptError("bad ciphertext");
    }
    s.pending = false;
    return pt;
}

pb::Writer wKeys(const SkippedKey& k) {
    pb::Writer w;
    w.u(1, k.index).b(2, k.cipher).b(3, k.mac).b(4, k.iv);
    return w;
}

pb::Writer wState(const State& s) {
    pb::Writer w;
    w.b(1, B(s.localIdentity)).b(2, B(s.remoteIdentity)).b(3, s.root).u(4, s.previousCounter);
    w.b(5, B(s.senderRatchet.pub)).b(6, B(s.senderRatchet.priv)).b(7, s.senderKey).u(8, s.senderIndex);
    for (auto& c : s.recv) {
        pb::Writer r;
        r.b(1, B(c.ratchet)).b(2, c.key).u(3, c.index);
        for (auto& k : c.skipped) r.m(4, wKeys(k));
        w.m(9, r);
    }
    w.u(10, s.pending).u(11, s.pendingPreKey).u(12, s.pendingSigned).b(13, B(s.pendingBase));
    w.u(14, s.localReg).u(15, s.remoteReg).b(16, B(s.baseKey));
    return w;
}

State rState(const pb::Msg& m) {
    State s;
    s.valid = true;
    s.localIdentity = toKey(m.bytes(1));
    s.remoteIdentity = toKey(m.bytes(2));
    s.root = m.bytes(3);
    s.previousCounter = (uint32_t)m.num(4);
    s.senderRatchet.pub = toKey(m.bytes(5));
    s.senderRatchet.priv = toKey(m.bytes(6));
    s.senderKey = m.bytes(7);
    s.senderIndex = (uint32_t)m.num(8);
    for (auto& rm : m.repeated(9)) {
        RecvChain c;
        c.ratchet = toKey(rm.bytes(1));
        c.key = rm.bytes(2);
        c.index = (uint32_t)rm.num(3);
        for (auto& k : rm.repeated(4))
            c.skipped.push_back({(uint32_t)k.num(1), k.bytes(2), k.bytes(3), k.bytes(4)});
        s.recv.push_back(std::move(c));
    }
    s.pending = m.num(10);
    s.pendingPreKey = (uint32_t)m.num(11);
    s.pendingSigned = (uint32_t)m.num(12);
    s.pendingBase = toKey(m.bytes(13));
    s.localReg = (uint32_t)m.num(14);
    s.remoteReg = (uint32_t)m.num(15);
    s.baseKey = toKey(m.bytes(16));
    return s;
}
}  // namespace

void Record::archive() {
    if (!cur.valid) return;
    prev.insert(prev.begin(), std::move(cur));
    if (prev.size() > kMaxPrevStates) prev.resize(kMaxPrevStates);
    cur = State{};
}

Bytes serialize(const Record& r) {
    pb::Writer w;
    if (r.cur.valid) w.m(1, wState(r.cur));
    for (auto& s : r.prev) w.m(2, wState(s));
    return w.out;
}

Record deserialize(const Bytes& d) {
    pb::Msg m(d);
    Record r;
    if (auto c = m.sub(1)) r.cur = rState(*c);
    for (auto& s : m.repeated(2)) r.prev.push_back(rState(s));
    return r;
}

void initFromBundle(Record& r, const KeyPair& us, uint32_t ourReg, const Bundle& b) {
    if (!xedVerify(b.identity, withType(b.signedKey), b.signature.data()) || b.signature.size() != 64)
        throw DecryptError("bad signed prekey signature");
    KeyPair base = genKeyPair();
    Bytes master(32, 0xff);
    append(master, x25519(us.priv, b.signedKey));
    append(master, x25519(base.priv, b.identity));
    append(master, x25519(base.priv, b.signedKey));
    if (b.preKeyId) append(master, x25519(base.priv, b.preKey));

    State s;
    s.valid = true;
    s.localIdentity = us.pub;
    s.remoteIdentity = b.identity;
    s.localReg = ourReg;
    s.remoteReg = b.regId;
    initCommon(s, master);
    // The chain from the master secret is the receiving chain for their signed prekey.
    Bytes firstRecv = s.senderKey;
    addRecv(s, b.signedKey, firstRecv);
    s.senderRatchet = genKeyPair();
    Bytes newRoot, sendKey;
    createChain(s.root, b.signedKey, s.senderRatchet, newRoot, sendKey);
    s.root = newRoot;
    s.senderKey = sendKey;
    s.senderIndex = 0;
    s.pending = true;
    s.pendingPreKey = b.preKeyId.value_or(0);
    s.pendingSigned = b.signedId;
    s.pendingBase = base.pub;
    s.baseKey = base.pub;
    r.archive();
    r.cur = std::move(s);
}

Ciphertext encrypt(Record& r, const Bytes& plaintext) {
    State& s = r.cur;
    if (!s.valid || s.senderKey.empty()) throw std::runtime_error("no session");
    MsgKeys k = deriveMsgKeys(s.senderKey, s.senderIndex);
    Bytes ct = aesCbcEncrypt(k.cipher, k.iv, plaintext);
    Bytes body = buildSignalMessage(s, k, ct);
    s.senderKey = nextChain(s.senderKey);
    s.senderIndex++;
    if (!s.pending) return {false, std::move(body)};
    pb::Writer w;
    if (s.pendingPreKey) w.u(1, s.pendingPreKey);
    w.b(2, withType(s.pendingBase)).b(3, withType(s.localIdentity)).b(4, body).u(5, s.localReg).u(6, s.pendingSigned);
    Bytes o{kVersion};
    append(o, w.out);
    return {true, std::move(o)};
}

Bytes decryptMessage(Record& r, const Bytes& wire) {
    if (r.empty()) throw DecryptError("no session");
    {
        State copy = r.cur;
        try {
            Bytes pt = decryptWith(copy, wire);
            r.cur = std::move(copy);
            return pt;
        } catch (const DuplicateError&) {
            throw;
        } catch (const DecryptError&) {
        }
    }
    for (size_t i = 0; i < r.prev.size(); i++) {
        State copy = r.prev[i];
        try {
            Bytes pt = decryptWith(copy, wire);
            // A previous state proved current: promote it.
            r.prev.erase(r.prev.begin() + (long)i);
            r.archive();
            r.cur = std::move(copy);
            return pt;
        } catch (const DuplicateError&) {
            throw;
        } catch (const DecryptError&) {
        }
    }
    throw DecryptError("no state could decrypt");
}

namespace {
struct PreKeyMsg {
    std::optional<uint32_t> preKey;
    Key32 base{}, identity{};
    Bytes inner;
    uint32_t reg = 0, signedId = 0;
};
PreKeyMsg parsePreKey(const Bytes& w) {
    if (w.size() < 2 || (w[0] >> 4) != 3) throw DecryptError("bad prekey version");
    pb::Msg m;
    try {
        m.parse(w.data() + 1, w.size() - 1);
    } catch (const std::exception&) {
        throw DecryptError("bad prekey message");
    }
    PreKeyMsg p;
    if (m.has(1)) p.preKey = (uint32_t)m.num(1);
    p.base = fromTyped(m.bytes(2));
    p.identity = fromTyped(m.bytes(3));
    p.inner = m.bytes(4);
    p.reg = (uint32_t)m.num(5);
    p.signedId = (uint32_t)m.num(6);
    return p;
}
}  // namespace

Key32 preKeyMessageIdentity(const Bytes& msg) { return parsePreKey(msg).identity; }

Bytes decryptPreKey(Record& r, const Keys& k, const Bytes& wire, std::optional<uint32_t>* used) {
    PreKeyMsg p = parsePreKey(wire);
    bool known = r.cur.valid && r.cur.baseKey == p.base;
    for (auto& s : r.prev) known = known || s.baseKey == p.base;
    if (known) {
        if (used) used->reset();
        return decryptMessage(r, p.inner);
    }
    auto sp = k.signedPreKey(p.signedId);
    if (!sp) throw DecryptError("unknown signed prekey");
    std::optional<KeyPair> otk;
    if (p.preKey) {
        otk = k.preKey(*p.preKey);
        if (!otk) throw DecryptError("unknown one-time prekey");
    }
    Bytes master(32, 0xff);
    append(master, x25519(sp->priv, p.identity));
    append(master, x25519(k.identity.priv, p.base));
    append(master, x25519(sp->priv, p.base));
    if (otk) append(master, x25519(otk->priv, p.base));

    State s;
    s.valid = true;
    s.localIdentity = k.identity.pub;
    s.remoteIdentity = p.identity;
    s.localReg = k.regId;
    s.remoteReg = p.reg;
    s.baseKey = p.base;
    initCommon(s, master);
    s.senderRatchet = *sp;
    s.senderIndex = 0;
    Bytes pt = decryptWith(s, p.inner);  // throws before anything is committed
    r.archive();
    r.cur = std::move(s);
    if (used) *used = p.preKey;
    return pt;
}

// Sender keys.
namespace {
struct SkKeys {
    Bytes iv, key;
};
SkKeys skDerive(const Bytes& seed) {
    Bytes o = hkdf(seed, {}, "WhisperGroup", 48);
    return {Bytes(o.begin(), o.begin() + 16), Bytes(o.begin() + 16, o.end())};
}
}  // namespace

Bytes serialize(const SenderRecord& r) {
    pb::Writer w;
    for (auto& s : r.states) {
        pb::Writer x;
        x.u(1, s.id).u(2, s.iteration).b(3, s.chain).b(4, B(s.signPub));
        if (s.signPriv) x.b(5, B(*s.signPriv));
        for (auto& k : s.skipped) {
            pb::Writer y;
            y.u(1, k.iteration).b(2, k.seed);
            x.m(6, y);
        }
        w.m(1, x);
    }
    return w.out;
}

SenderRecord deserializeSender(const Bytes& d) {
    SenderRecord r;
    pb::Msg m(d);
    for (auto& x : m.repeated(1)) {
        SenderState s;
        s.id = (uint32_t)x.num(1);
        s.iteration = (uint32_t)x.num(2);
        s.chain = x.bytes(3);
        s.signPub = toKey(x.bytes(4));
        if (x.has(5)) s.signPriv = toKey(x.bytes(5));
        for (auto& y : x.repeated(6)) s.skipped.push_back({(uint32_t)y.num(1), y.bytes(2)});
        r.states.push_back(std::move(s));
    }
    return r;
}

Bytes senderKeyDistribution(SenderRecord& r) {
    if (r.states.empty() || !r.states[0].signPriv) {
        SenderState s;
        s.id = randomU32() & 0x7fffffff;
        s.chain = randomBytes(32);
        KeyPair kp = genKeyPair();
        s.signPub = kp.pub;
        s.signPriv = kp.priv;
        r.states.insert(r.states.begin(), std::move(s));
    }
    auto& s = r.states[0];
    pb::Writer w;
    w.u(1, s.id).u(2, s.iteration).b(3, s.chain).b(4, withType(s.signPub));
    Bytes o{kVersion};
    append(o, w.out);
    return o;
}

void processDistribution(SenderRecord& r, const Bytes& msg) {
    if (msg.size() < 2 || (msg[0] >> 4) != 3) throw DecryptError("bad distribution version");
    pb::Msg m;
    try {
        m.parse(msg.data() + 1, msg.size() - 1);
    } catch (const std::exception&) {
        throw DecryptError("bad distribution");
    }
    SenderState s;
    s.id = (uint32_t)m.num(1);
    s.iteration = (uint32_t)m.num(2);
    s.chain = m.bytes(3);
    s.signPub = fromTyped(m.bytes(4));
    if (s.chain.size() != 32) throw DecryptError("bad chain key");
    r.states.erase(std::remove_if(r.states.begin(), r.states.end(), [&](auto& o) { return o.id == s.id; }),
                   r.states.end());
    r.states.insert(r.states.begin(), std::move(s));
    if (r.states.size() > kMaxSenderStates) r.states.resize(kMaxSenderStates);
}

Bytes senderEncrypt(SenderRecord& r, const Bytes& pt) {
    if (r.states.empty() || !r.states[0].signPriv) throw std::runtime_error("no sender key");
    auto& s = r.states[0];
    SkKeys k = skDerive(hmacSha256(s.chain, Bytes{1}));
    Bytes ct = aesCbcEncrypt(k.key, k.iv, pt);
    pb::Writer w;
    w.u(1, s.id).u(2, s.iteration).b(3, ct);
    Bytes o{kVersion};
    append(o, w.out);
    auto sig = xedSign(*s.signPriv, o);
    o.insert(o.end(), sig.begin(), sig.end());
    s.chain = hmacSha256(s.chain, Bytes{2});
    s.iteration++;
    return o;
}

Bytes senderDecrypt(SenderRecord& r, const Bytes& msg) {
    if (msg.size() < 1 + 64 || (msg[0] >> 4) != 3) throw DecryptError("bad sender message");
    pb::Msg m;
    try {
        m.parse(msg.data() + 1, msg.size() - 65);
    } catch (const std::exception&) {
        throw DecryptError("bad sender message");
    }
    uint32_t id = (uint32_t)m.num(1), iter = (uint32_t)m.num(2);
    Bytes ct = m.bytes(3);
    auto st = std::find_if(r.states.begin(), r.states.end(), [&](auto& s) { return s.id == id; });
    if (st == r.states.end()) throw DecryptError("no sender key");
    Bytes signedPart(msg.begin(), msg.end() - 64);
    if (!xedVerify(st->signPub, signedPart, msg.data() + msg.size() - 64)) throw DecryptError("bad signature");
    SenderState s = *st;  // committed only after a good decrypt
    Bytes seed;
    if (iter < s.iteration) {
        auto it = std::find_if(s.skipped.begin(), s.skipped.end(), [&](auto& x) { return x.iteration == iter; });
        if (it == s.skipped.end()) throw DuplicateError("old sender iteration");
        seed = it->seed;
        s.skipped.erase(it);
    } else {
        if (iter - s.iteration > kMaxFuture) throw DecryptError("sender too far ahead");
        while (s.iteration < iter) {
            s.skipped.push_back({s.iteration, hmacSha256(s.chain, Bytes{1})});
            s.chain = hmacSha256(s.chain, Bytes{2});
            s.iteration++;
        }
        if (s.skipped.size() > kMaxSkipped) s.skipped.erase(s.skipped.begin(), s.skipped.end() - kMaxSkipped);
        seed = hmacSha256(s.chain, Bytes{1});
        s.chain = hmacSha256(s.chain, Bytes{2});
        s.iteration++;
    }
    SkKeys k = skDerive(seed);
    Bytes pt;
    try {
        pt = aesCbcDecrypt(k.key, k.iv, ct);
    } catch (const std::exception&) {
        throw DecryptError("bad sender ciphertext");
    }
    *st = std::move(s);
    return pt;
}

Bytes pad(const Bytes& p) {
    uint8_t n = randomBytes(1)[0] & 0x0f;
    if (!n) n = 0x0f;
    Bytes o = p;
    o.insert(o.end(), n, n);
    return o;
}

Bytes unpad(const Bytes& p) {
    if (p.empty() || p.back() == 0 || p.back() > p.size()) throw DecryptError("bad padding");
    return Bytes(p.begin(), p.end() - p.back());
}

}  // namespace zw::signal
