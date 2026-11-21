// App-state sync decoding: LTHash, MAC checks and mutation decryption. Holds no I/O;
// the caller supplies keys and previous value MACs. Read-only: nothing here encodes patches.
#pragma once
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "crypto.h"
#include "pb.h"

namespace zw::appstate {

struct IntegrityError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct MissingKey : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct HashState {
    uint64_t version = 0;
    Bytes hash = Bytes(128, 0);
};

struct Mutation {
    bool set = true;
    std::vector<std::string> index;
    Bytes indexMac, valueMac;
    Bytes action;  // SyncActionValue
};

struct Env {
    std::function<std::optional<Bytes>(const Bytes& keyId)> key;
    std::function<std::optional<Bytes>(const Bytes& indexMac)> prevValueMac;
};

void ltHashApply(Bytes& base, const std::vector<Bytes>& remove, const std::vector<Bytes>& add);

// Both verify every MAC and throw IntegrityError or MissingKey before returning anything.
std::vector<Mutation> decodeSnapshot(const std::string& name, const Bytes& snapshot, HashState& st, const Env& env);
// mutationsOverride carries the externally stored SyncdMutations when the patch has them.
std::vector<Mutation> decodePatch(const std::string& name, const pb::Msg& patch, const std::vector<pb::Msg>& mutations,
                                  HashState& st, const Env& env);

}  // namespace zw::appstate
