// Decodes the few parts of WhatsApp's Message protobuf that the daemon shows, and
// builds the few it sends. Field numbers are protocol facts.
#pragma once
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

#include "crypto.h"
#include "pb.h"

namespace zw {

using nlohmann::json;

struct ParsedMsg {
    std::string kind;  // empty means no displayable content
    std::string text, quotedId;
    json media;   // the public part of IPC.md, or null
    json secret;  // media keys, kept only in the sealed store row
    // Side channels carried in the same message.
    std::string skdmGroup;
    Bytes skdm;
    std::string dsmDest;
    bool isProtocol = false;
    uint64_t protoType = 0;
    std::string targetId;  // revoke, edit or reaction target
    std::string editText;
    std::optional<pb::Msg> history;  // HistorySyncNotification
    std::vector<std::pair<Bytes, Bytes>> appKeys;  // app state key id, key data
};

// Throws on a malformed protobuf.
ParsedMsg parseMessage(const Bytes& proto);
std::string previewOf(const std::string& kind, const std::string& text);

Bytes buildTextMessage(const std::string& text, const std::string& quotedId, const std::string& quotedSender);
Bytes buildStatusText(const std::string& text);
Bytes buildDeviceSent(const std::string& dest, const Bytes& inner);
Bytes buildSkdmMessage(const std::string& group, const Bytes& skdm);

}  // namespace zw
