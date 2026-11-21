// Primitive helpers: byte strings, encodings, and the crypto the protocol uses.
#pragma once
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace zw {

using Bytes = std::vector<uint8_t>;
using Key32 = std::array<uint8_t, 32>;

struct KeyPair {
    Key32 pub{};
    Key32 priv{};
};

inline Bytes B(std::string_view s) { return Bytes(s.begin(), s.end()); }
inline std::string S(const Bytes& b) { return std::string(b.begin(), b.end()); }
inline Bytes B(const Key32& k) { return Bytes(k.begin(), k.end()); }
inline void append(Bytes& a, const Bytes& b) { a.insert(a.end(), b.begin(), b.end()); }
inline void append(Bytes& a, const Key32& b) { a.insert(a.end(), b.begin(), b.end()); }
Key32 toKey(const Bytes& b);  // throws unless exactly 32 bytes

std::string hex(const Bytes& b);
Bytes unhex(std::string_view s);
std::string b64(const Bytes& b);
std::string b64url(const Bytes& b);  // padded URL-safe alphabet
Bytes b64dec(std::string_view s);

void cryptoInit();
Bytes randomBytes(size_t n);
uint32_t randomU32();

Bytes sha256(const Bytes& d);
Bytes hmacSha256(const Bytes& key, const Bytes& d);
Bytes hkdf(const Bytes& ikm, const Bytes& salt, std::string_view info, size_t len);
Bytes pbkdf2Sha256(std::string_view pass, const Bytes& salt, int iters, size_t len);
Bytes md5(const Bytes& d);
bool ctEqual(const Bytes& a, const Bytes& b);

Bytes aesGcmEncrypt(const Bytes& key, const Bytes& iv, const Bytes& pt, const Bytes& aad);
// Throws on tag mismatch.
Bytes aesGcmDecrypt(const Bytes& key, const Bytes& iv, const Bytes& ct, const Bytes& aad);
Bytes aesCbcEncrypt(const Bytes& key, const Bytes& iv, const Bytes& pt);  // PKCS#7
Bytes aesCbcDecrypt(const Bytes& key, const Bytes& iv, const Bytes& ct);  // throws on bad padding
Bytes aesCtr(const Bytes& key, const Bytes& iv, const Bytes& data);

KeyPair genKeyPair();
Key32 x25519Pub(const Key32& priv);
Key32 x25519(const Key32& priv, const Key32& pub);  // throws on a low-order result

// XEdDSA as used by Signal: an Ed25519-form signature made with a Curve25519 key.
std::array<uint8_t, 64> xedSign(const Key32& priv, const Bytes& msg);
bool xedVerify(const Key32& pub, const Bytes& msg, const uint8_t* sig64);

Bytes zlibInflate(const uint8_t* d, size_t n, size_t maxOut = 256u << 20);

}  // namespace zw
