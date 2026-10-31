// The Signal protocol as WhatsApp uses it: X3DH session setup, the Double Ratchet at
// wire version 3, and sender keys. Own implementation; see NOTICE.md.
#pragma once
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "crypto.h"

namespace zw::signal {

struct DecryptError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
// Counter already used: a duplicate. The caller drops it rather than asking for a retry.
struct DuplicateError : DecryptError {
    using DecryptError::DecryptError;
};

struct SkippedKey {
    uint32_t index;
    Bytes cipher, mac, iv;
};

struct RecvChain {
    Key32 ratchet{};
    Bytes key;
    uint32_t index = 0;
    std::vector<SkippedKey> skipped;
};

struct State {
    Key32 localIdentity{}, remoteIdentity{};
    Bytes root;
    uint32_t previousCounter = 0;
    KeyPair senderRatchet;
    Bytes senderKey;
    uint32_t senderIndex = 0;
    std::vector<RecvChain> recv;
    bool pending = false;
    uint32_t pendingPreKey = 0;  // 0 means none, as real ids start at 1
    uint32_t pendingSigned = 0;
    Key32 pendingBase{};
    uint32_t localReg = 0, remoteReg = 0;
    Key32 baseKey{};  // Alice's base key, to spot re-delivered prekey messages
    bool valid = false;
};

struct Record {
    State cur;
    std::vector<State> prev;
    bool empty() const { return !cur.valid; }
    void archive();
};

Bytes serialize(const Record& r);
Record deserialize(const Bytes& d);

struct Bundle {
    uint32_t regId = 0;
    std::optional<uint32_t> preKeyId;
    Key32 preKey{};
    uint32_t signedId = 0;
    Key32 signedKey{};
    Bytes signature;
    Key32 identity{};
};

// What decryption needs from storage.
struct Keys {
    KeyPair identity;
    uint32_t regId = 0;
    std::function<std::optional<KeyPair>(uint32_t)> preKey;
    std::function<std::optional<KeyPair>(uint32_t)> signedPreKey;
};

// Alice's half: verifies the signed prekey, then archives any old state.
void initFromBundle(Record& r, const KeyPair& ourIdentity, uint32_t ourReg, const Bundle& b);

struct Ciphertext {
    bool preKey = false;
    Bytes data;
};
Ciphertext encrypt(Record& r, const Bytes& plaintext);

// On success mutates r and reports any one-time prekey to delete. On failure r is unchanged.
Bytes decryptPreKey(Record& r, const Keys& k, const Bytes& msg, std::optional<uint32_t>* usedPreKey);
Bytes decryptMessage(Record& r, const Bytes& msg);
// The identity key a prekey message claims, to record it as this peer's identity.
Key32 preKeyMessageIdentity(const Bytes& msg);

// Sender keys.
struct SenderState {
    uint32_t id = 0, iteration = 0;
    Bytes chain;
    Key32 signPub{};
    std::optional<Key32> signPriv;
    struct Skipped {
        uint32_t iteration;
        Bytes seed;
    };
    std::vector<Skipped> skipped;
};
struct SenderRecord {
    std::vector<SenderState> states;  // newest first, at most 5
};
Bytes serialize(const SenderRecord& r);
SenderRecord deserializeSender(const Bytes& d);
// Creates our chain if the record has none, and returns the distribution message.
Bytes senderKeyDistribution(SenderRecord& r);
void processDistribution(SenderRecord& r, const Bytes& msg);
Bytes senderEncrypt(SenderRecord& r, const Bytes& plaintext);
Bytes senderDecrypt(SenderRecord& r, const Bytes& msg);

Bytes pad(const Bytes& plaintext);
Bytes unpad(const Bytes& plaintext);

}  // namespace zw::signal
