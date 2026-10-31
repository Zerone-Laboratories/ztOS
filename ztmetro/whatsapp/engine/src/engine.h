#pragma once
#include <memory>
#include <optional>
#include <string>

#include "crypto.h"
#include "ipc.h"

namespace zw {

struct RealConfig {
    std::string dataDir;
    std::string host = "web.whatsapp.com";
    int port = 443;
    bool tls = true;
    std::optional<Key32> rootKey;  // tests only: replaces the pinned noise root
    std::string mediaBase;         // tests only: replaces the https://host prefix
    bool probeOnly = false;        // stop at the first QR ref, for --probe-handshake
};

std::unique_ptr<Engine> makeFakeEngine(Loop& l, IpcServer& ipc);
std::unique_ptr<Engine> makeRealEngine(Loop& l, IpcServer& ipc, const RealConfig& cfg);

}  // namespace zw
