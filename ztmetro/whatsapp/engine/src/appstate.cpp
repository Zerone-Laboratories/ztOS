#include "appstate.h"

#include <openssl/hmac.h>

#include <map>
#include <nlohmann/json.hpp>

namespace zw::appstate {

namespace {
struct Keys {
    Bytes index, value, valueMac, snapshotMac, patchMac;
};

Keys expand(const Env& env, const Bytes& id) {
    auto k = env.key(id);
    if (!k) throw MissingKey("app state key not held");
    Bytes o = hkdf(*k, {}, "WhatsApp Mutation Keys", 160);
    auto part = [&](int i) { return Bytes(o.begin() + i * 32, o.begin() + (i + 1) * 32); };
    return {part(0), part(1), part(2), part(3), part(4)};
}

Bytes be64(uint64_t v) {
    Bytes o(8);
    for (int i = 0; i < 8; i++) o[i] = (uint8_t)(v >> (56 - i * 8));
    return o;
}

Bytes contentMac(bool set, const Bytes& data, const Bytes& keyId, const Bytes& key) {
    Bytes in{(uint8_t)(set ? 1 : 2)};  // operation + 1, with SET=0 and REMOVE=1
    append(in, keyId);
    append(in, data);
    append(in, be64(keyId.size() + 1));
    Bytes o(64);
    unsigned n = 64;
    HMAC(EVP_sha512(), key.data(), (int)key.size(), in.data(), in.size(), o.data(), &n);
    o.resize(32);
    return o;
}

Bytes snapshotMac(const HashState& st, const std::string& name, const Bytes& key) {
    Bytes in = st.hash;
    append(in, be64(st.version));
    append(in, B(name));
    return hmacSha256(key, in);
}

struct Rec {
    bool set;
    Bytes index, value, keyId;
};

Rec readRecord(const pb::Msg& r, bool set) {
    Rec o{set, {}, {}, {}};
    if (auto i = r.sub(1)) o.index = i->bytes(1);
    if (auto v = r.sub(2)) o.value = v->bytes(1);
    if (auto k = r.sub(3)) o.keyId = k->bytes(1);
    if (o.value.size() < 16 + 32) throw IntegrityError("short mutation value");
    return o;
}

Mutation decodeOne(const Rec& r, const Env& env) {
    Keys k = expand(env, r.keyId);
    Bytes content(r.value.begin(), r.value.end() - 32), mac(r.value.end() - 32, r.value.end());
    if (!ctEqual(contentMac(r.set, content, r.keyId, k.valueMac), mac)) throw IntegrityError("value mac");
    Bytes pt;
    try {
        pt = aesCbcDecrypt(k.value, Bytes(content.begin(), content.begin() + 16), Bytes(content.begin() + 16, content.end()));
    } catch (const std::exception&) {
        throw IntegrityError("mutation ciphertext");
    }
    pb::Msg data(pt);
    Bytes idx = data.bytes(1);
    if (!ctEqual(hmacSha256(k.index, idx), r.index)) throw IntegrityError("index mac");
    Mutation m;
    m.set = r.set;
    m.indexMac = r.index;
    m.valueMac = mac;
    m.action = data.bytes(2);
    auto j = nlohmann::json::parse(idx.begin(), idx.end(), nullptr, false);
    if (!j.is_array()) throw IntegrityError("index is not a list");
    for (auto& e : j)
        if (e.is_string()) m.index.push_back(e.get<std::string>());
    return m;
}
}  // namespace

void ltHashApply(Bytes& base, const std::vector<Bytes>& remove, const std::vector<Bytes>& add) {
    auto op = [&](const Bytes& item, bool sub) {
        Bytes h = hkdf(item, {}, "WhatsApp Patch Integrity", 128);
        for (size_t i = 0; i + 1 < base.size(); i += 2) {
            uint16_t x = (uint16_t)(base[i] | base[i + 1] << 8), y = (uint16_t)(h[i] | h[i + 1] << 8);
            uint16_t r = sub ? (uint16_t)(x - y) : (uint16_t)(x + y);
            base[i] = (uint8_t)r;
            base[i + 1] = (uint8_t)(r >> 8);
        }
    };
    for (auto& r : remove) op(r, true);
    for (auto& a : add) op(a, false);
}

std::vector<Mutation> decodeSnapshot(const std::string& name, const Bytes& snapshot, HashState& st, const Env& env) {
    pb::Msg s(snapshot);
    HashState next;  // a snapshot replaces the state outright
    if (auto v = s.sub(1)) next.version = v->num(1);
    std::vector<Rec> recs;
    std::vector<Bytes> added;
    for (auto& r : s.repeated(2)) {
        recs.push_back(readRecord(r, true));
        added.emplace_back(recs.back().value.end() - 32, recs.back().value.end());
    }
    ltHashApply(next.hash, {}, added);
    Bytes keyId;
    if (auto k = s.sub(4)) keyId = k->bytes(1);
    Keys k = expand(env, keyId);
    if (!ctEqual(snapshotMac(next, name, k.snapshotMac), s.bytes(3))) throw IntegrityError("snapshot mac");
    std::vector<Mutation> out;
    for (auto& r : recs) out.push_back(decodeOne(r, env));
    st = next;
    return out;
}

std::vector<Mutation> decodePatch(const std::string& name, const pb::Msg& patch, const std::vector<pb::Msg>& mutations,
                                  HashState& st, const Env& env) {
    HashState next = st;
    if (auto v = patch.sub(1)) next.version = v->num(1);
    std::vector<Rec> recs;
    for (auto& m : mutations) {
        auto r = m.sub(2);
        if (!r) throw IntegrityError("mutation without record");
        recs.push_back(readRecord(*r, m.num(1) == 0));
    }
    std::vector<Bytes> removed, added;
    for (size_t i = 0; i < recs.size(); i++) {
        if (recs[i].set) added.emplace_back(recs[i].value.end() - 32, recs[i].value.end());
        // The value being replaced may sit earlier in this same patch.
        std::optional<Bytes> prev;
        bool found = false;
        for (size_t j = i; j-- > 0;) {
            if (recs[j].index == recs[i].index) {
                if (recs[j].set) prev = Bytes(recs[j].value.end() - 32, recs[j].value.end());
                found = true;
                break;
            }
        }
        if (!found) prev = env.prevValueMac(recs[i].index);
        if (prev) removed.push_back(*prev);
    }
    ltHashApply(next.hash, removed, added);
    Bytes keyId;
    if (auto k = patch.sub(6)) keyId = k->bytes(1);
    Keys k = expand(env, keyId);
    if (!ctEqual(snapshotMac(next, name, k.snapshotMac), patch.bytes(4))) throw IntegrityError("patch snapshot mac");
    Bytes in = patch.bytes(4);
    for (auto& r : recs) in.insert(in.end(), r.value.end() - 32, r.value.end());
    append(in, be64(next.version));
    append(in, B(name));
    if (!ctEqual(hmacSha256(k.patchMac, in), patch.bytes(5))) throw IntegrityError("patch mac");
    std::vector<Mutation> out;
    for (auto& r : recs) out.push_back(decodeOne(r, env));
    st = next;
    return out;
}

}  // namespace zw::appstate
