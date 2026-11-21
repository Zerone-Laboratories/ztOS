#include "crypto.h"

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <sodium.h>
#include <zlib.h>

#include <cstring>
#include <memory>

namespace zw {

Key32 toKey(const Bytes& b) {
    if (b.size() != 32) throw std::runtime_error("bad key length");
    Key32 k;
    std::memcpy(k.data(), b.data(), 32);
    return k;
}

std::string hex(const Bytes& b) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(b.size() * 2);
    for (auto c : b) {
        s += d[c >> 4];
        s += d[c & 15];
    }
    return s;
}

Bytes unhex(std::string_view s) {
    Bytes o(s.size() / 2 + 1);
    size_t n = 0;
    if (sodium_hex2bin(o.data(), o.size(), s.data(), s.size(), nullptr, &n, nullptr) != 0)
        throw std::runtime_error("bad hex");
    o.resize(n);
    return o;
}

static std::string b64v(const Bytes& b, int variant) {
    if (b.empty()) return "";
    std::string o(sodium_base64_encoded_len(b.size(), variant), 0);
    sodium_bin2base64(o.data(), o.size(), b.data(), b.size(), variant);
    o.resize(std::strlen(o.c_str()));
    return o;
}
std::string b64(const Bytes& b) { return b64v(b, sodium_base64_VARIANT_ORIGINAL); }
std::string b64url(const Bytes& b) { return b64v(b, sodium_base64_VARIANT_URLSAFE); }

Bytes b64dec(std::string_view s) {
    if (s.empty()) return {};
    Bytes o(s.size());
    size_t n = 0;
    if (sodium_base642bin(o.data(), o.size(), s.data(), s.size(), nullptr, &n, nullptr,
                          sodium_base64_VARIANT_ORIGINAL) != 0)
        throw std::runtime_error("bad base64");
    o.resize(n);
    return o;
}

void cryptoInit() {
    if (sodium_init() < 0) throw std::runtime_error("libsodium init failed");
}

Bytes randomBytes(size_t n) {
    Bytes b(n);
    randombytes_buf(b.data(), n);
    return b;
}
uint32_t randomU32() { return randombytes_random(); }

Bytes sha256(const Bytes& d) {
    Bytes o(32);
    crypto_hash_sha256(o.data(), d.data(), d.size());
    return o;
}

Bytes md5(const Bytes& d) {
    Bytes o(16);
    unsigned n = 16;
    EVP_Digest(d.data(), d.size(), o.data(), &n, EVP_md5(), nullptr);
    return o;
}

Bytes hmacSha256(const Bytes& key, const Bytes& d) {
    Bytes o(32);
    unsigned n = 32;
    static const uint8_t none = 0;
    if (!HMAC(EVP_sha256(), key.empty() ? &none : key.data(), (int)key.size(), d.data(), d.size(),
              o.data(), &n))
        throw std::runtime_error("hmac failed");
    return o;
}

Bytes hkdf(const Bytes& ikm, const Bytes& salt, std::string_view info, size_t len) {
    // RFC 5869. An empty salt means HashLen zero bytes, which HMAC treats the same way.
    Bytes prk = hmacSha256(salt, ikm);
    Bytes out, t;
    for (uint8_t i = 1; out.size() < len; i++) {
        Bytes in = t;
        in.insert(in.end(), info.begin(), info.end());
        in.push_back(i);
        t = hmacSha256(prk, in);
        append(out, t);
    }
    out.resize(len);
    return out;
}

Bytes pbkdf2Sha256(std::string_view pass, const Bytes& salt, int iters, size_t len) {
    Bytes o(len);
    if (PKCS5_PBKDF2_HMAC(pass.data(), (int)pass.size(), salt.data(), (int)salt.size(), iters,
                          EVP_sha256(), (int)len, o.data()) != 1)
        throw std::runtime_error("pbkdf2 failed");
    return o;
}

bool ctEqual(const Bytes& a, const Bytes& b) {
    return a.size() == b.size() && sodium_memcmp(a.data(), b.data(), a.size()) == 0;
}

namespace {
struct Ctx {
    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
    ~Ctx() { EVP_CIPHER_CTX_free(c); }
};
}  // namespace

Bytes aesGcmEncrypt(const Bytes& key, const Bytes& iv, const Bytes& pt, const Bytes& aad) {
    Ctx x;
    int n = 0;
    Bytes o(pt.size() + 16);
    if (key.size() != 32 || iv.size() != 12) throw std::runtime_error("gcm params");
    EVP_EncryptInit_ex(x.c, EVP_aes_256_gcm(), nullptr, key.data(), iv.data());
    if (!aad.empty()) EVP_EncryptUpdate(x.c, nullptr, &n, aad.data(), (int)aad.size());
    if (!pt.empty()) EVP_EncryptUpdate(x.c, o.data(), &n, pt.data(), (int)pt.size());
    int m = 0;
    if (EVP_EncryptFinal_ex(x.c, o.data() + n, &m) != 1) throw std::runtime_error("gcm encrypt");
    EVP_CIPHER_CTX_ctrl(x.c, EVP_CTRL_GCM_GET_TAG, 16, o.data() + pt.size());
    return o;
}

Bytes aesGcmDecrypt(const Bytes& key, const Bytes& iv, const Bytes& ct, const Bytes& aad) {
    if (ct.size() < 16 || key.size() != 32 || iv.size() != 12) throw std::runtime_error("gcm params");
    Ctx x;
    int n = 0;
    size_t len = ct.size() - 16;
    Bytes o(len);
    EVP_DecryptInit_ex(x.c, EVP_aes_256_gcm(), nullptr, key.data(), iv.data());
    if (!aad.empty()) EVP_DecryptUpdate(x.c, nullptr, &n, aad.data(), (int)aad.size());
    if (len) EVP_DecryptUpdate(x.c, o.data(), &n, ct.data(), (int)len);
    Bytes tag(ct.end() - 16, ct.end());
    EVP_CIPHER_CTX_ctrl(x.c, EVP_CTRL_GCM_SET_TAG, 16, tag.data());
    int m = 0;
    uint8_t scratch[16];
    if (EVP_DecryptFinal_ex(x.c, scratch, &m) != 1) throw std::runtime_error("gcm tag mismatch");
    return o;
}

Bytes aesCbcEncrypt(const Bytes& key, const Bytes& iv, const Bytes& pt) {
    Ctx x;
    Bytes o(pt.size() + 16);
    int n = 0, m = 0;
    if (key.size() != 32 || iv.size() != 16) throw std::runtime_error("cbc params");
    EVP_EncryptInit_ex(x.c, EVP_aes_256_cbc(), nullptr, key.data(), iv.data());
    EVP_EncryptUpdate(x.c, o.data(), &n, pt.data(), (int)pt.size());
    EVP_EncryptFinal_ex(x.c, o.data() + n, &m);
    o.resize(n + m);
    return o;
}

Bytes aesCbcDecrypt(const Bytes& key, const Bytes& iv, const Bytes& ct) {
    if (key.size() != 32 || iv.size() != 16 || ct.empty() || ct.size() % 16)
        throw std::runtime_error("cbc params");
    Ctx x;
    Bytes o(ct.size() + 16);
    int n = 0, m = 0;
    EVP_DecryptInit_ex(x.c, EVP_aes_256_cbc(), nullptr, key.data(), iv.data());
    EVP_DecryptUpdate(x.c, o.data(), &n, ct.data(), (int)ct.size());
    if (EVP_DecryptFinal_ex(x.c, o.data() + n, &m) != 1) throw std::runtime_error("cbc padding");
    o.resize(n + m);
    return o;
}

Bytes aesCtr(const Bytes& key, const Bytes& iv, const Bytes& d) {
    if (key.size() != 32 || iv.size() != 16) throw std::runtime_error("ctr params");
    Ctx x;
    Bytes o(d.size() + 16);
    int n = 0, m = 0;
    EVP_EncryptInit_ex(x.c, EVP_aes_256_ctr(), nullptr, key.data(), iv.data());
    EVP_EncryptUpdate(x.c, o.data(), &n, d.data(), (int)d.size());
    EVP_EncryptFinal_ex(x.c, o.data() + n, &m);
    o.resize(n + m);
    return o;
}

KeyPair genKeyPair() {
    KeyPair k;
    randombytes_buf(k.priv.data(), 32);
    k.pub = x25519Pub(k.priv);
    return k;
}

Key32 x25519Pub(const Key32& priv) {
    Key32 p;
    crypto_scalarmult_curve25519_base(p.data(), priv.data());
    return p;
}

Key32 x25519(const Key32& priv, const Key32& pub) {
    Key32 s;
    if (crypto_scalarmult_curve25519(s.data(), priv.data(), pub.data()) != 0)
        throw std::runtime_error("x25519 low-order point");
    return s;
}

// Sign per the XEdDSA spec. Libsodium exposes the scalar and point operations needed.
std::array<uint8_t, 64> xedSign(const Key32& priv, const Bytes& msg) {
    uint8_t a[32];
    std::memcpy(a, priv.data(), 32);
    a[0] &= 248;
    a[31] &= 127;
    a[31] |= 64;
    uint8_t A[32];
    if (crypto_scalarmult_ed25519_base_noclamp(A, a) != 0) throw std::runtime_error("xed base");
    uint8_t signBit = A[31] & 0x80;
    if (signBit) {
        uint8_t reduced[64] = {0}, neg[32];
        std::memcpy(reduced, a, 32);
        crypto_core_ed25519_scalar_reduce(a, reduced);
        crypto_core_ed25519_scalar_negate(neg, a);
        std::memcpy(a, neg, 32);
        A[31] &= 0x7f;
    }
    // r = H(0xFE || 0xFF*31 || a || msg || Z) mod L
    uint8_t Z[64], h[64], r[32];
    randombytes_buf(Z, 64);
    crypto_hash_sha512_state st;
    uint8_t prefix[32];
    std::memset(prefix, 0xff, 32);
    prefix[0] = 0xfe;
    crypto_hash_sha512_init(&st);
    crypto_hash_sha512_update(&st, prefix, 32);
    crypto_hash_sha512_update(&st, a, 32);
    crypto_hash_sha512_update(&st, msg.data(), msg.size());
    crypto_hash_sha512_update(&st, Z, 64);
    crypto_hash_sha512_final(&st, h);
    crypto_core_ed25519_scalar_reduce(r, h);
    uint8_t R[32];
    crypto_scalarmult_ed25519_base_noclamp(R, r);
    // k = H(R || A || msg) mod L, s = r + k*a mod L
    uint8_t k[32], ka[32], s[32];
    crypto_hash_sha512_init(&st);
    crypto_hash_sha512_update(&st, R, 32);
    crypto_hash_sha512_update(&st, A, 32);
    crypto_hash_sha512_update(&st, msg.data(), msg.size());
    crypto_hash_sha512_final(&st, h);
    crypto_core_ed25519_scalar_reduce(k, h);
    crypto_core_ed25519_scalar_mul(ka, k, a);
    crypto_core_ed25519_scalar_add(s, r, ka);
    std::array<uint8_t, 64> sig;
    std::memcpy(sig.data(), R, 32);
    std::memcpy(sig.data() + 32, s, 32);
    sodium_memzero(a, 32);
    sodium_memzero(r, 32);
    return sig;
}

bool xedVerify(const Key32& pub, const Bytes& msg, const uint8_t* sig64) {
    // Montgomery u to Edwards y = (u-1)/(u+1) mod 2^255-19, sign bit taken from the signature.
    uint8_t u[32];
    std::memcpy(u, pub.data(), 32);
    u[31] &= 0x7f;
    std::unique_ptr<BN_CTX, decltype(&BN_CTX_free)> ctx(BN_CTX_new(), BN_CTX_free);
    auto mk = [] { return std::unique_ptr<BIGNUM, decltype(&BN_free)>(BN_new(), BN_free); };
    auto p = mk(), bu = mk(), one = mk(), num = mk(), den = mk(), inv = mk(), y = mk();
    BN_set_bit(p.get(), 255);
    BN_sub_word(p.get(), 19);
    BN_lebin2bn(u, 32, bu.get());
    BN_one(one.get());
    BN_mod_sub(num.get(), bu.get(), one.get(), p.get(), ctx.get());
    BN_mod_add(den.get(), bu.get(), one.get(), p.get(), ctx.get());
    if (BN_is_zero(den.get())) return false;
    if (!BN_mod_inverse(inv.get(), den.get(), p.get(), ctx.get())) return false;
    BN_mod_mul(y.get(), num.get(), inv.get(), p.get(), ctx.get());
    uint8_t A[32];
    if (BN_bn2lebinpad(y.get(), A, 32) != 32) return false;
    A[31] |= sig64[63] & 0x80;
    uint8_t sig[64];
    std::memcpy(sig, sig64, 64);
    sig[63] &= 0x7f;
    return crypto_sign_ed25519_verify_detached(sig, msg.data(), msg.size(), A) == 0;
}

Bytes zlibInflate(const uint8_t* d, size_t n, size_t maxOut) {
    z_stream z{};
    if (inflateInit(&z) != Z_OK) throw std::runtime_error("inflate init");
    Bytes out;
    uint8_t buf[65536];
    z.next_in = const_cast<uint8_t*>(d);
    z.avail_in = (uInt)n;
    int r;
    do {
        z.next_out = buf;
        z.avail_out = sizeof buf;
        r = inflate(&z, Z_NO_FLUSH);
        if (r != Z_OK && r != Z_STREAM_END) {
            inflateEnd(&z);
            throw std::runtime_error("inflate failed");
        }
        out.insert(out.end(), buf, buf + (sizeof buf - z.avail_out));
        if (out.size() > maxOut) {
            inflateEnd(&z);
            throw std::runtime_error("inflate too large");
        }
    } while (r != Z_STREAM_END);
    inflateEnd(&z);
    return out;
}

}  // namespace zw
