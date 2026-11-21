#include "noise.h"

#include "pb.h"

namespace zw {

static Bytes ivOf(uint32_t c) {
    Bytes iv(12, 0);
    iv[8] = (uint8_t)(c >> 24);
    iv[9] = (uint8_t)(c >> 16);
    iv[10] = (uint8_t)(c >> 8);
    iv[11] = (uint8_t)c;
    return iv;
}

void NoiseState::start(const Bytes& header) {
    // The protocol name padded to 32 bytes is used directly as the initial hash.
    static const char name[33] = "Noise_XX_25519_AESGCM_SHA256\0\0\0\0";
    hash_.assign(name, name + 32);
    salt_ = hash_;
    key_ = hash_;
    counter_ = 0;
    authenticate(header);
}

void NoiseState::authenticate(const Bytes& d) {
    Bytes in = hash_;
    append(in, d);
    hash_ = sha256(in);
}

void NoiseState::mixKey(const Bytes& ikm) {
    counter_ = 0;
    Bytes o = hkdf(ikm, salt_, "", 64);
    salt_.assign(o.begin(), o.begin() + 32);
    key_.assign(o.begin() + 32, o.end());
}

void NoiseState::mix(const Key32& priv, const Key32& pub) { mixKey(B(x25519(priv, pub))); }

Bytes NoiseState::iv() { return ivOf(counter_++); }

Bytes NoiseState::encrypt(const Bytes& pt) {
    Bytes ct = aesGcmEncrypt(key_, iv(), pt, hash_);
    authenticate(ct);
    return ct;
}

Bytes NoiseState::decrypt(const Bytes& ct) {
    Bytes pt = aesGcmDecrypt(key_, iv(), ct, hash_);
    authenticate(ct);
    return pt;
}

void NoiseState::finish(Bytes& w, Bytes& r) const {
    Bytes o = hkdf({}, salt_, "", 64);
    w.assign(o.begin(), o.begin() + 32);
    r.assign(o.begin() + 32, o.end());
}

Bytes NoiseTransport::seal(const Bytes& pt) { return aesGcmEncrypt(writeKey, ivOf(writeCtr++), pt, {}); }
Bytes NoiseTransport::open(const Bytes& ct) { return aesGcmDecrypt(readKey, ivOf(readCtr++), ct, {}); }

// The published WhatsApp noise root, pinned as whatsmeow and Baileys do.
static const Key32 kRoot = {0x14, 0x23, 0x75, 0x57, 0x4d, 0x0a, 0x58, 0x71, 0x66, 0xaa, 0xe7,
                            0x1e, 0xbe, 0x51, 0x64, 0x37, 0xc4, 0xa2, 0x8b, 0x73, 0xe3, 0x69,
                            0x5c, 0x6c, 0xe1, 0xf7, 0xf9, 0x54, 0x5d, 0xa8, 0xee, 0x6b};

bool verifyNoiseCert(const Bytes& chain, const Bytes& staticKey, uint64_t now, const Key32* root) {
    try {
        pb::Msg c(chain);
        auto leaf = c.sub(1), inter = c.sub(2);
        if (!leaf || !inter) return false;
        Bytes interRaw = inter->bytes(1), interSig = inter->bytes(2);
        Bytes leafRaw = leaf->bytes(1), leafSig = leaf->bytes(2);
        if (interSig.size() != 64 || leafSig.size() != 64) return false;
        if (!xedVerify(root ? *root : kRoot, interRaw, interSig.data())) return false;
        pb::Msg id(interRaw);
        if (id.num(2) != 0) return false;  // intermediate issued by the root, serial 0
        Bytes ik = id.bytes(3);
        if (ik.size() != 32) return false;
        if (!xedVerify(toKey(ik), leafRaw, leafSig.data())) return false;
        pb::Msg ld(leafRaw);
        if (ld.num(2) != id.num(1)) return false;
        if (!ctEqual(ld.bytes(3), staticKey)) return false;
        for (auto* d : {&id, &ld})
            if (now < d->num(4) || now > d->num(5)) return false;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

Bytes clientHelloFrame(const Key32& e) {
    pb::Writer h;
    h.b(1, B(e));
    pb::Writer w;
    w.m(2, h);
    return w.out;
}

ServerHello parseServerHello(const Bytes& frame) {
    pb::Msg m(frame);
    auto s = m.sub(3);
    if (!s) throw std::runtime_error("noise: no server hello");
    ServerHello h{s->bytes(1), s->bytes(2), s->bytes(3)};
    if (h.ephemeral.size() != 32 || h.staticCt.empty() || h.payloadCt.empty())
        throw std::runtime_error("noise: incomplete server hello");
    return h;
}

Bytes clientFinishFrame(const Bytes& st, const Bytes& pl) {
    pb::Writer f;
    f.b(1, st).b(2, pl);
    pb::Writer w;
    w.m(4, f);
    return w.out;
}

}  // namespace zw
