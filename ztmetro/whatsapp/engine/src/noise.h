// Noise_XX_25519_AESGCM_SHA256 as WhatsApp frames it: symmetric state, the client
// side of the handshake, and verification of the server's certificate chain.
#pragma once
#include "crypto.h"

namespace zw {

class NoiseState {
public:
    void start(const Bytes& header);
    void authenticate(const Bytes& d);
    void mix(const Key32& priv, const Key32& pub);
    Bytes encrypt(const Bytes& pt);
    Bytes decrypt(const Bytes& ct);  // throws on a bad tag
    void finish(Bytes& writeKey, Bytes& readKey) const;
    const Bytes& hash() const { return hash_; }

private:
    void mixKey(const Bytes& ikm);
    Bytes iv();
    Bytes hash_, salt_, key_;
    uint32_t counter_ = 0;
};

// Transport after the handshake: one counter per direction, IV = 8 zero bytes + counter.
struct NoiseTransport {
    Bytes writeKey, readKey;
    uint32_t writeCtr = 0, readCtr = 0;
    Bytes seal(const Bytes& pt);
    Bytes open(const Bytes& ct);
};

// The pinned WhatsApp root key by default. Tests pass their own.
bool verifyNoiseCert(const Bytes& certChain, const Bytes& staticKey, uint64_t now,
                     const Key32* rootOverride = nullptr);

Bytes clientHelloFrame(const Key32& ephemeralPub);
struct ServerHello {
    Bytes ephemeral, staticCt, payloadCt;
};
ServerHello parseServerHello(const Bytes& frame);
Bytes clientFinishFrame(const Bytes& staticCt, const Bytes& payloadCt);

}  // namespace zw
