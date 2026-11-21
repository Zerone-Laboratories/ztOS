// Published vectors, whatsmeow-generated fixtures, and optional live interop against
// go-libsignal (pass the path of the gofix binary as argv[2]).
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

#include "../src/appstate.h"
#include "../src/noise.h"
#include "../src/node.h"
#include "../src/pb.h"
#include "../src/sigproto.h"

using namespace zw;
static int fails = 0, passes = 0;
#define CHECK(c)                                                  \
    do {                                                          \
        if (c) passes++;                                          \
        else { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } \
    } while (0)
#define EQH(a, h) CHECK(hex(a) == std::string(h))

static void vectors() {
    // RFC 4231 test case 2
    EQH(hmacSha256(B("Jefe"), B("what do ya want for nothing?")),
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    // RFC 5869 test case 1
    EQH(hkdf(Bytes(22, 0x0b), unhex("000102030405060708090a0b0c"),
             S(unhex("f0f1f2f3f4f5f6f7f8f9")), 42),
        "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");
    // RFC 5869 test case 3 (empty salt and info)
    EQH(hkdf(Bytes(22, 0x0b), {}, "", 42),
        "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8");
    // RFC 7748 section 5.2
    Key32 k = toKey(unhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4"));
    Key32 u = toKey(unhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c"));
    EQH(B(x25519(k, u)), "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");
    // NIST GCM test case 16 style: AES-256, checked by round trip plus a known vector
    Bytes key = unhex("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308");
    Bytes iv = unhex("cafebabefacedbaddecaf888");
    Bytes pt = unhex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39");
    Bytes aad = unhex("feedfacedeadbeeffeedfacedeadbeefabaddad2");
    Bytes ct = aesGcmEncrypt(key, iv, pt, aad);
    EQH(ct, "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662"
            "76fc6ece0f4e1768cddf8853bb2d551b");
    CHECK(aesGcmDecrypt(key, iv, ct, aad) == pt);
    Bytes bad = ct;
    bad[0] ^= 1;
    bool threw = false;
    try { aesGcmDecrypt(key, iv, bad, aad); } catch (...) { threw = true; }
    CHECK(threw);
    // NIST SP 800-38A F.5.5 CTR-AES256
    EQH(aesCtr(unhex("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4"),
               unhex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff"), unhex("6bc1bee22e409f96e93d7e117393172a")),
        "601ec313775789a5b7a7f504bbf3d228");
    // NIST SP 800-38A F.2.5 CBC-AES256, compared without the padding block
    Bytes cbc = aesCbcEncrypt(unhex("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4"),
                              unhex("000102030405060708090a0b0c0d0e0f"), unhex("6bc1bee22e409f96e93d7e117393172a"));
    CHECK(hex(cbc).substr(0, 32) == "f58c4c04d6e5f1ba779eabfb5f7bfbd6");
    // XEdDSA round trip with both sign-bit cases and rejection of tampering
    for (int i = 0; i < 16; i++) {
        KeyPair kp = genKeyPair();
        auto s = xedSign(kp.priv, B("message"));
        CHECK(xedVerify(kp.pub, B("message"), s.data()));
        CHECK(!xedVerify(kp.pub, B("messagE"), s.data()));
        s[3] ^= 1;
        CHECK(!xedVerify(kp.pub, B("message"), s.data()));
    }
}

static std::map<std::string, std::string> wire(const std::string& dir) {
    std::map<std::string, std::string> m;
    std::ifstream f(dir + "/wire.txt");
    std::string a, b, c;
    while (f >> a >> b >> c) m[a + "." + b] = c;
    CHECK(m.size() >= 20);
    return m;
}

static Node expected(const std::string& name) {
    Jid pn{"61400000000", "s.whatsapp.net"};
    if (name == "empty") return Node("iq");
    if (name == "attr") return Node("iq").a("type", "get");
    if (name == "rawtag") return Node("not-a-known-token");
    if (name == "nibble") return Node("a").a("id", "1234-5.6");
    if (name == "nibbleodd") return Node("a").a("id", "123");
    if (name == "hex") return Node("a").a("id", "3EB0ABCDEF");
    if (name == "longstr") {
        std::string s;
        for (int i = 0; i < 100; i++) s += "xyz";
        return Node("a").a("id", s);
    }
    if (name == "bytes") return Node("enc").bytes({1, 2, 3});
    if (name == "bytes300") return Node("enc").bytes(Bytes(300, 0));
    if (name == "strcont") return Node("a").text("companion");
    if (name == "jidpn") return Node("a").a("to", pn);
    if (name == "jidad") return Node("a").a("to", Jid{pn.user, pn.server, 3});
    if (name == "jidlid") return Node("a").a("to", Jid{"99887766", "lid", 5});
    if (name == "jidgroup") return Node("a").a("to", Jid{"120363000000", "g.us"});
    if (name == "jidserver") return Node("a").a("to", Jid{"", "s.whatsapp.net"});
    if (name == "nested") {
        Node list("list");
        list.add(Node("key").bytes({9})).add(Node("value"));
        return Node("iq").add(list).add(Node("registration").bytes({0, 0, 0, 1}));
    }
    throw std::runtime_error("unknown fixture " + name);
}

static bool same(const Node& a, const Node& b) {
    auto norm = [](Node::Kind k) { return k == Node::Kind::Str ? Node::Kind::Bytes : k; };
    if (a.tag != b.tag || norm(a.kind) != norm(b.kind) || a.data != b.data || a.attrs.size() != b.attrs.size() ||
        a.kids.size() != b.kids.size())
        return false;
    for (size_t i = 0; i < a.attrs.size(); i++)
        if (a.attrs[i].key != b.attrs[i].key || a.attrs[i].val != b.attrs[i].val) return false;
    for (size_t i = 0; i < a.kids.size(); i++)
        if (!same(a.kids[i], b.kids[i])) return false;
    return true;
}

static void codec(const std::map<std::string, std::string>& w) {
    int n = 0;
    for (auto& [k, v] : w) {
        if (k.rfind("codec.", 0)) continue;
        std::string name = k.substr(6);
        Node e = expected(name);
        if (hex(encodeNode(e)) != v) {
            fails++;
            printf("FAIL encode %s\n want %s\n got  %s\n", name.c_str(), v.c_str(), hex(encodeNode(e)).c_str());
        } else passes++;
        Node d = decodeFrame(unhex(v));
        if (!same(d, e)) {
            fails++;
            printf("FAIL decode %s\n", name.c_str());
        } else passes++;
        n++;
    }
    CHECK(n == 16);
    // Malformed input must throw, never crash or overread.
    Bytes seed = unhex(w.at("codec.nested"));
    int threw = 0;
    for (size_t cut = 0; cut < seed.size(); cut++) {
        try { decodeFrame(seed.data(), cut); } catch (const std::exception&) { threw++; }
    }
    CHECK(threw == (int)seed.size());
    for (int i = 0; i < 20000; i++) {
        Bytes r = randomBytes(1 + randomU32() % 64);
        r[0] = 0;
        try { decodeFrame(r); } catch (const std::exception&) {}
    }
    // Deep nesting is refused rather than recursing without limit.
    Bytes deep{0};
    for (int i = 0; i < 5000; i++) { deep.push_back(248); deep.push_back(2); deep.push_back(4); deep.push_back(248); deep.push_back(1); }
    bool t = false;
    try { decodeFrame(deep); } catch (const std::exception&) { t = true; }
    CHECK(t);
    Jid j = Jid::parse("123.5:7@s.whatsapp.net");
    CHECK(j.user == "123" && j.agent == 5 && j.device == 7 && j.str() == "123.5:7@s.whatsapp.net");
}

static void noise(const std::map<std::string, std::string>& w) {
    Key32 e = toKey(unhex(w.at("noise.epriv"))), cs = toKey(unhex(w.at("noise.cspriv")));
    Key32 sePub = toKey(unhex(w.at("noise.sepub")));
    NoiseState n;
    n.start({'W', 'A', 6, 3});
    n.authenticate(B(x25519Pub(e)));
    n.authenticate(B(sePub));
    n.mix(e, sePub);
    EQH(n.encrypt(B("first plaintext")), w.at("noise.enc1").c_str());
    EQH(n.encrypt(B("second")), w.at("noise.enc2").c_str());
    n.mix(cs, sePub);
    EQH(n.encrypt(B(x25519Pub(cs))), w.at("noise.enc3").c_str());

    // Certificate chain: a test root signs an intermediate, which signs the leaf.
    KeyPair root = genKeyPair(), inter = genKeyPair(), server = genKeyPair();
    uint64_t now = 1000;
    pb::Writer id, ld;
    id.u(1, 7).u(2, 0).b(3, B(inter.pub)).u(4, 1).u(5, 2000);
    ld.u(1, 8).u(2, 7).b(3, B(server.pub)).u(4, 1).u(5, 2000);
    auto mk = [](const Bytes& det, const Key32& sk) {
        pb::Writer c;
        auto s = xedSign(sk, det);
        c.b(1, det).b(2, Bytes(s.begin(), s.end()));
        return c;
    };
    pb::Writer chain;
    chain.m(1, mk(ld.out, inter.priv)).m(2, mk(id.out, root.priv));
    CHECK(verifyNoiseCert(chain.out, B(server.pub), now, &root.pub));
    CHECK(!verifyNoiseCert(chain.out, B(server.pub), 3000, &root.pub));  // expired
    CHECK(!verifyNoiseCert(chain.out, B(inter.pub), now, &root.pub));    // wrong static
    CHECK(!verifyNoiseCert(chain.out, B(server.pub), now));              // not the real root
    KeyPair other = genKeyPair();
    CHECK(!verifyNoiseCert(chain.out, B(server.pub), now, &other.pub));
}

static void sigFixture(const std::map<std::string, std::string>& w) {
    Bytes sig = unhex(w.at("sig.val"));
    Bytes msg = unhex(w.at("sig.msg"));
    Key32 pub = toKey(unhex(w.at("sig.pub")));
    CHECK(xedVerify(pub, msg, sig.data()));
    msg[0] ^= 1;
    CHECK(!xedVerify(pub, msg, sig.data()));
}

static void appState(const std::map<std::string, std::string>& w) {
    using namespace appstate;
    Bytes keyId = unhex(w.at("appstate.keyid")), key = unhex(w.at("appstate.key"));
    std::map<Bytes, Bytes> macs;
    Env env;
    env.key = [&](const Bytes& id) -> std::optional<Bytes> { if (id == keyId) return key; return std::nullopt; };
    env.prevValueMac = [&](const Bytes& i) -> std::optional<Bytes> { auto it = macs.find(i); if (it == macs.end()) return std::nullopt; return it->second; };
    HashState st;
    for (int i = 0; i < 3; i++) {
        std::string n = std::to_string(i);
        Bytes raw = unhex(w.at("appstate.patch" + n));
        pb::Msg patch(raw);
        std::string name = S(unhex(w.at("appstate.name" + n)));
        if (i == 2) { st = HashState{}; macs.clear(); }  // whatsmeow's third patch is a new collection
        if (i == 2) continue;
        auto muts = decodePatch(name, patch, patch.repeated(2), st, env);
        EQH(st.hash, w.at("appstate.hash" + n).c_str());
        CHECK(!muts.empty() && muts[0].index[0] + "|" + muts[0].index[1] == S(unhex(w.at("appstate.index" + n))));
        for (auto& m : muts) { if (m.set) macs[m.indexMac] = m.valueMac; else macs.erase(m.indexMac); }
    }
    // Tampering is refused and leaves the state untouched.
    HashState fresh;
    macs.clear();
    Bytes raw = unhex(w.at("appstate.patch0"));
    raw[raw.size() / 2] ^= 1;
    bool threw = false;
    try { pb::Msg p(raw); decodePatch(S(unhex(w.at("appstate.name0"))), p, p.repeated(2), fresh, env); } catch (const std::exception&) { threw = true; }
    CHECK(threw && fresh.version == 0);
    // Unknown key.
    Env none = env;
    none.key = [](const Bytes&) -> std::optional<Bytes> { return std::nullopt; };
    threw = false;
    pb::Msg ok(unhex(w.at("appstate.patch0")));
    try { HashState s; decodePatch(S(unhex(w.at("appstate.name0"))), ok, ok.repeated(2), s, none); } catch (const MissingKey&) { threw = true; }
    CHECK(threw);
}

// A local party for self tests and interop.
struct Party {
    KeyPair id = genKeyPair();
    uint32_t reg = 1000 + randomU32() % 10000;
    KeyPair pk = genKeyPair(), spk = genKeyPair();
    signal::Bundle bundle() {
        signal::Bundle b;
        b.regId = reg;
        b.preKeyId = 11;
        b.preKey = pk.pub;
        b.signedId = 22;
        b.signedKey = spk.pub;
        Bytes typed{5};
        append(typed, spk.pub);
        auto s = xedSign(id.priv, typed);
        b.signature.assign(s.begin(), s.end());
        b.identity = id.pub;
        return b;
    }
    signal::Keys keys() {
        signal::Keys k;
        k.identity = id;
        k.regId = reg;
        k.preKey = [this](uint32_t i) -> std::optional<KeyPair> { if (i == 11) return pk; return std::nullopt; };
        k.signedPreKey = [this](uint32_t i) -> std::optional<KeyPair> { if (i == 22) return spk; return std::nullopt; };
        return k;
    }
};

static void selfSession() {
    using namespace signal;
    Party a, b;
    Record ra, rb;
    initFromBundle(ra, a.id, a.reg, b.bundle());
    auto c1 = encrypt(ra, B("one"));
    CHECK(c1.preKey);
    std::optional<uint32_t> used;
    CHECK(S(decryptPreKey(rb, b.keys(), c1.data, &used)) == "one");
    CHECK(used && *used == 11);
    // Re-delivery of a used prekey message counts as a duplicate, not a new session.
    bool dup = false;
    try { decryptPreKey(rb, b.keys(), c1.data, &used); } catch (const DuplicateError&) { dup = true; }
    CHECK(dup);
    auto c2 = encrypt(rb, B("two"));
    CHECK(!c2.preKey);
    CHECK(S(decryptMessage(ra, c2.data)) == "two");
    // Out of order within a chain, across serialisation, and several ratchet turns.
    auto m1 = encrypt(ra, B("m1")), m2 = encrypt(ra, B("m2")), m3 = encrypt(ra, B("m3"));
    CHECK(!m1.preKey);
    CHECK(S(decryptMessage(rb, m3.data)) == "m3");
    rb = deserialize(serialize(rb));
    CHECK(S(decryptMessage(rb, m1.data)) == "m1");
    CHECK(S(decryptMessage(rb, m2.data)) == "m2");
    for (int i = 0; i < 10; i++) {
        ra = deserialize(serialize(ra));
        auto x = encrypt(ra, B("a" + std::to_string(i)));
        CHECK(S(decryptMessage(rb, x.data)) == "a" + std::to_string(i));
        auto y = encrypt(rb, B("b" + std::to_string(i)));
        CHECK(S(decryptMessage(ra, y.data)) == "b" + std::to_string(i));
    }
    // A failed decrypt leaves the record untouched.
    auto good = encrypt(ra, B("good"));
    Bytes tampered = good.data;
    tampered[tampered.size() - 1] ^= 1;
    Bytes before = serialize(rb);
    bool threw = false;
    try { decryptMessage(rb, tampered); } catch (const DecryptError&) { threw = true; }
    CHECK(threw && serialize(rb) == before);
    CHECK(S(decryptMessage(rb, good.data)) == "good");
    // Bob restarted with a fresh session: the old state in prev still decrypts.
    auto late = encrypt(rb, B("late"));
    Party a2id;
    Record rb2;
    initFromBundle(ra, a.id, a.reg, b.bundle());  // a re-establishes, archiving the old state
    CHECK(S(decryptMessage(ra, late.data)) == "late");
    // Bad signed prekey signature refused.
    auto bb = b.bundle();
    bb.signature[0] ^= 1;
    threw = false;
    try { initFromBundle(rb2, a.id, a.reg, bb); } catch (const DecryptError&) { threw = true; }
    CHECK(threw);
    // Padding
    for (int i = 0; i < 50; i++) CHECK(S(unpad(pad(B("abc")))) == "abc");

    // Sender keys
    SenderRecord sa, rcv;
    Bytes dist = senderKeyDistribution(sa);
    processDistribution(rcv, dist);
    auto s1 = senderEncrypt(sa, B("g1")), s2 = senderEncrypt(sa, B("g2")), s3 = senderEncrypt(sa, B("g3"));
    CHECK(S(senderDecrypt(rcv, s2)) == "g2");
    rcv = deserializeSender(serialize(rcv));
    CHECK(S(senderDecrypt(rcv, s1)) == "g1");
    CHECK(S(senderDecrypt(rcv, s3)) == "g3");
    dup = false;
    try { senderDecrypt(rcv, s3); } catch (const DuplicateError&) { dup = true; }
    CHECK(dup);
    auto s4 = senderEncrypt(sa, B("g4"));
    s4[5] ^= 1;
    threw = false;
    try { senderDecrypt(rcv, s4); } catch (const DecryptError&) { threw = true; }
    CHECK(threw);
}

struct Peer {
    FILE* in = nullptr;
    FILE* out = nullptr;
    pid_t pid = -1;
    explicit Peer(const std::string& path) {
        int a[2], b[2];
        if (pipe(a) || pipe(b)) throw std::runtime_error("pipe");
        pid = fork();
        if (pid == 0) {
            dup2(a[0], 0);
            dup2(b[1], 1);
            int nul = open("/dev/null", O_WRONLY);
            dup2(nul, 2);
            execl(path.c_str(), path.c_str(), "peer", nullptr);
            _exit(127);
        }
        close(a[0]);
        close(b[1]);
        in = fdopen(a[1], "w");
        out = fdopen(b[0], "r");
    }
    std::vector<std::string> ask(const std::string& q) {
        fprintf(in, "%s\n", q.c_str());
        fflush(in);
        static char buf[1 << 22];
        if (!fgets(buf, sizeof buf, out)) throw std::runtime_error("peer closed");
        std::istringstream ss(buf);
        std::vector<std::string> r;
        std::string w;
        while (ss >> w) r.push_back(w);
        return r;
    }
    ~Peer() {
        fprintf(in, "quit\n");
        fclose(in);
        fclose(out);
        waitpid(pid, nullptr, 0);
    }
};

// Both directions against go-libsignal, the implementation whatsmeow runs in production.
static void interop(const std::string& path) {
    using namespace signal;
    {   // C++ initiates to Go.
        Peer go(path);
        auto f = go.ask("bundle");
        CHECK(f.size() == 7);
        Bundle b;
        b.regId = (uint32_t)std::stoul(f[0]);
        b.preKeyId = (uint32_t)std::stoul(f[1]);
        b.preKey = toKey(unhex(f[2]));
        b.signedId = (uint32_t)std::stoul(f[3]);
        b.signedKey = toKey(unhex(f[4]));
        b.signature = unhex(f[5]);
        b.identity = toKey(unhex(f[6]));
        Party me;
        Record r;
        initFromBundle(r, me.id, me.reg, b);
        auto c = encrypt(r, B("hello go"));
        auto d = go.ask("dec pkmsg " + hex(c.data));
        CHECK(d.size() == 2 && d[0] == "ok" && S(unhex(d[1])) == "hello go");
        for (int i = 0; i < 20; i++) {
            auto e = go.ask("enc " + hex(B("from go " + std::to_string(i))));
            CHECK(e.size() == 2 && e[0] == "msg");
            CHECK(S(decryptMessage(r, unhex(e[1]))) == "from go " + std::to_string(i));
            r = deserialize(serialize(r));
            if (i % 3 == 0) continue;  // sometimes Go sends twice in a row, no ratchet turn
            auto c2 = encrypt(r, B("from cpp " + std::to_string(i)));
            auto d2 = go.ask(std::string(c2.preKey ? "dec pkmsg " : "dec msg ") + hex(c2.data));
            CHECK(d2.size() == 2 && d2[0] == "ok" && S(unhex(d2[1])) == "from cpp " + std::to_string(i));
        }
    }
    {   // Go initiates to C++, then sender keys both ways.
        Peer go(path);
        Party me;
        auto b = me.bundle();
        std::string q = "process " + std::to_string(b.regId) + " 11 " + hex(B(b.preKey)) + " 22 " +
                        hex(B(b.signedKey)) + " " + hex(b.signature) + " " + hex(B(b.identity));
        auto ok = go.ask(q);
        CHECK(ok.size() >= 1 && ok[0] == "ok");
        auto e = go.ask("enc " + hex(B("hello cpp")));
        CHECK(e.size() == 2 && e[0] == "pkmsg");
        Record r;
        std::optional<uint32_t> used;
        CHECK(S(decryptPreKey(r, me.keys(), unhex(e[1]), &used)) == "hello cpp");
        CHECK(used && *used == 11);
        auto c = encrypt(r, B("reply"));
        auto d = go.ask("dec msg " + hex(c.data));
        CHECK(d.size() == 2 && d[0] == "ok" && S(unhex(d[1])) == "reply");
        // Skip one in the chain.
        auto e1 = go.ask("enc " + hex(B("skip1")));
        auto e2 = go.ask("enc " + hex(B("skip2")));
        CHECK(S(decryptMessage(r, unhex(e2[1]))) == "skip2");
        CHECK(S(decryptMessage(r, unhex(e1[1]))) == "skip1");

        // Go's sender key to C++.
        auto dist = go.ask("skdm");
        SenderRecord rcv;
        processDistribution(rcv, unhex(dist[0]));
        for (int i = 0; i < 5; i++) {
            auto m = go.ask("skenc " + hex(B("group " + std::to_string(i))));
            CHECK(S(senderDecrypt(rcv, unhex(m[0]))) == "group " + std::to_string(i));
        }
        // C++ sender key to Go.
        SenderRecord mine;
        auto a = go.ask("skproc " + hex(senderKeyDistribution(mine)));
        CHECK(a.size() >= 1 && a[0] == "ok");
        for (int i = 0; i < 5; i++) {
            auto m = senderEncrypt(mine, B("cpp group " + std::to_string(i)));
            auto x = go.ask("skdec " + hex(m));
            CHECK(x.size() == 2 && x[0] == "ok" && S(unhex(x[1])) == "cpp group " + std::to_string(i));
        }
    }
    printf("interop: go-libsignal exchanges passed\n");
}

int main(int argc, char** argv) {
    cryptoInit();
    ::signal(SIGPIPE, SIG_IGN);
    std::string dir = argc > 1 ? argv[1] : "tests/fixtures";
    try {
        vectors();
        auto w = wire(dir);
        codec(w);
        noise(w);
        sigFixture(w);
        appState(w);
        selfSession();
        if (argc > 2) interop(argv[2]);
        else printf("interop: skipped, no gofix binary given\n");
    } catch (const std::exception& e) {
        fails++;
        printf("FAIL exception: %s\n", e.what());
    }
    printf("%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
