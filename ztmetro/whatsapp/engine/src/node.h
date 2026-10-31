// WhatsApp's binary XML: nodes, JIDs and the codec.
#pragma once
#include <string>
#include <vector>

#include "crypto.h"

namespace zw {

struct Jid {
    std::string user, server;
    uint16_t device = 0;
    uint8_t agent = 0;

    bool empty() const { return server.empty(); }
    Jid bare() const { return Jid{user, server, 0, 0}; }
    std::string str() const;   // user[.agent][:device]@server
    bool isGroup() const { return server == "g.us"; }
    bool isLid() const { return server == "lid"; }
    bool isPn() const { return server == "s.whatsapp.net"; }
    bool groupish() const { return server == "g.us" || server == "broadcast"; }
    bool operator==(const Jid& o) const {
        return user == o.user && server == o.server && device == o.device && agent == o.agent;
    }
    static Jid parse(const std::string& s);  // empty Jid when it does not parse
};

extern const char* const kServer;  // s.whatsapp.net

struct Attr {
    std::string key, val;
    bool jid = false;
};

struct Node {
    enum class Kind { None, Bytes, Str, Kids };
    std::string tag;
    std::vector<Attr> attrs;
    Kind kind = Kind::None;
    Bytes data;
    std::vector<Node> kids;

    Node() = default;
    explicit Node(std::string t) : tag(std::move(t)) {}
    Node& a(std::string k, std::string v) {
        attrs.push_back({std::move(k), std::move(v), false});
        return *this;
    }
    Node& a(std::string k, const Jid& v) {
        attrs.push_back({std::move(k), v.str(), true});
        return *this;
    }
    Node& bytes(Bytes b) {
        kind = Kind::Bytes;
        data = std::move(b);
        return *this;
    }
    Node& text(std::string_view s) {  // content encoded through the string path
        kind = Kind::Str;
        data = B(s);
        return *this;
    }
    Node& add(Node n) {
        kind = Kind::Kids;
        kids.push_back(std::move(n));
        return *this;
    }

    std::string attr(std::string_view k) const;
    Jid jid(std::string_view k) const;  // parses the attribute, empty if absent
    const Node* child(std::string_view t) const;
    std::vector<const Node*> children(std::string_view t) const;
};

// Serialise with the leading flag byte (always 0: uncompressed).
Bytes encodeNode(const Node& n);
// Parse a received frame including its flag byte, inflating if flagged. Throws on bad input.
Node decodeFrame(const uint8_t* d, size_t n);
inline Node decodeFrame(const Bytes& b) { return decodeFrame(b.data(), b.size()); }
int dictVersion();
// Shape only (tags and attribute names), safe to log. Never includes values.
std::string shape(const Node& n);

}  // namespace zw
