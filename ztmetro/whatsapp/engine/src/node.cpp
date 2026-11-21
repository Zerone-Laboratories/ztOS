#include "node.h"

#include <cstring>
#include <unordered_map>

#include "tokens.inc"

namespace zw {

const char* const kServer = "s.whatsapp.net";
int dictVersion() { return WA_DICT_VERSION; }

namespace {
enum : int {
    ListEmpty = 0, Dict0 = 236, Dict3 = 239, InteropJID = 245, FBJID = 246, ADJID = 247,
    List8 = 248, List16 = 249, JIDPair = 250, Hex8 = 251, Binary8 = 252, Binary20 = 253,
    Binary32 = 254, Nibble8 = 255, PackedMax = 127
};

struct Dict {
    const char* const* t;
    size_t n;
};
template <size_t N>
Dict D(const char* const (&a)[N]) { return {a, N}; }
const Dict kDoubles[4] = {D(kDouble0), D(kDouble1), D(kDouble2), D(kDouble3)};
constexpr size_t kSingleN = sizeof kSingle / sizeof *kSingle;

struct Index {
    std::unordered_map<std::string_view, uint8_t> single;
    std::unordered_map<std::string_view, std::pair<uint8_t, uint8_t>> dbl;
    Index() {
        for (size_t i = 1; i < kSingleN; i++) single[kSingle[i]] = (uint8_t)i;
        for (int d = 0; d < 4; d++)
            for (size_t i = 0; i < kDoubles[d].n; i++)
                dbl[kDoubles[d].t[i]] = {(uint8_t)d, (uint8_t)i};
    }
};
const Index& idx() {
    static const Index i;
    return i;
}

uint8_t agentOf(const std::string& server, uint8_t raw) {
    if (server == "s.whatsapp.net") return 0;
    if (server == "lid") return 1;
    if (server == "hosted") return 128;
    if (server == "hosted.lid") return 129;
    return raw;
}

struct Enc {
    Bytes o{0};
    void u8(int v) { o.push_back((uint8_t)v); }
    void u16(int v) { u8(v >> 8); u8(v); }
    void len(size_t n) {
        if (n < 256) {
            u8(Binary8);
            u8((int)n);
        } else if (n < (1u << 20)) {
            u8(Binary20);
            u8((int)((n >> 16) & 0x0f));
            u8((int)(n >> 8));
            u8((int)n);
        } else {
            u8(Binary32);
            u8((int)(n >> 24));
            u8((int)(n >> 16));
            u8((int)(n >> 8));
            u8((int)n);
        }
    }
    static bool isNibble(std::string_view s) {
        if (s.size() > PackedMax) return false;
        for (char c : s)
            if (!((c >= '0' && c <= '9') || c == '-' || c == '.')) return false;
        return true;
    }
    static bool isHex(std::string_view s) {
        if (s.size() > PackedMax) return false;
        for (char c : s)
            if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return false;
        return true;
    }
    static uint8_t packN(char c) {
        if (c == '-') return 10;
        if (c == '.') return 11;
        if (c == 0) return 15;
        return (uint8_t)(c - '0');
    }
    static uint8_t packH(char c) {
        if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
        if (c >= 'A' && c <= 'F') return (uint8_t)(10 + c - 'A');
        return 15;
    }
    void packed(std::string_view s, int type) {
        u8(type);
        int rounded = (int)((s.size() + 1) / 2);
        if (s.size() % 2) rounded |= 128;
        u8(rounded);
        auto pk = type == Nibble8 ? packN : packH;
        for (size_t i = 0; i + 1 < s.size(); i += 2) u8(pk(s[i]) << 4 | pk(s[i + 1]));
        if (s.size() % 2) u8(pk(s.back()) << 4 | pk(0));
    }
    void str(std::string_view s) {
        auto& ix = idx();
        if (auto it = ix.single.find(s); it != ix.single.end()) {
            u8(it->second);
        } else if (auto d = ix.dbl.find(s); d != ix.dbl.end()) {
            u8(Dict0 + d->second.first);
            u8(d->second.second);
        } else if (isNibble(s)) {
            packed(s, Nibble8);
        } else if (isHex(s)) {
            packed(s, Hex8);
        } else {
            len(s.size());
            o.insert(o.end(), s.begin(), s.end());
        }
    }
    void jid(const std::string& text) {
        Jid j = Jid::parse(text);
        if (((j.server == "s.whatsapp.net" || j.server == "lid") && j.device > 0) ||
            j.server == "hosted" || j.server == "hosted.lid") {
            u8(ADJID);
            u8(agentOf(j.server, j.agent));
            u8(j.device);
            str(j.user);
        } else {
            u8(JIDPair);
            if (j.user.empty())
                u8(ListEmpty);
            else
                str(j.user);
            str(j.server);
        }
    }
    void list(size_t n) {
        if (n == 0) {
            u8(ListEmpty);
        } else if (n < 256) {
            u8(List8);
            u8((int)n);
        } else {
            u8(List16);
            u16((int)n);
        }
    }
    void node(const Node& n) {
        size_t cnt = 0;
        for (auto& a : n.attrs)
            if (!a.val.empty()) cnt++;
        list(2 * cnt + 1 + (n.kind != Node::Kind::None));
        str(n.tag);
        for (auto& a : n.attrs) {
            if (a.val.empty()) continue;
            str(a.key);
            if (a.jid)
                jid(a.val);
            else
                str(a.val);
        }
        switch (n.kind) {
            case Node::Kind::None:
                break;
            case Node::Kind::Bytes:
                len(n.data.size());
                append(o, n.data);
                break;
            case Node::Kind::Str:
                str(S(n.data));
                break;
            case Node::Kind::Kids:
                list(n.kids.size());
                for (auto& k : n.kids) node(k);
                break;
        }
    }
};

struct Dec {
    const uint8_t* d;
    size_t n, i = 0;
    int depth = 0;
    struct Val {
        enum { Nil, Str, Bin, List } t = Nil;
        std::string s;
        Bytes b;
        std::vector<Node> l;
        bool jid = false;
    };
    uint8_t u8() {
        if (i >= n) throw std::runtime_error("codec: eof");
        return d[i++];
    }
    int beN(int c) {
        int r = 0;
        for (int k = 0; k < c; k++) r = (r << 8) | u8();
        return r;
    }
    int listSize(int tag) {
        if (tag == ListEmpty) return 0;
        if (tag == List8) return u8();
        if (tag == List16) return beN(2);
        throw std::runtime_error("codec: list tag");
    }
    static char unN(int v) {
        if (v < 10) return (char)('0' + v);
        if (v == 10) return '-';
        if (v == 11) return '.';
        if (v == 15) return 0;
        throw std::runtime_error("codec: nibble");
    }
    static char unH(int v) { return v < 10 ? (char)('0' + v) : (char)('A' + v - 10); }
    std::string packed(int tag) {
        int st = u8();
        std::string s;
        for (int k = 0; k < (st & 127); k++) {
            int c = u8();
            s += tag == Nibble8 ? unN(c >> 4) : unH(c >> 4);
            s += tag == Nibble8 ? unN(c & 15) : unH(c & 15);
        }
        if ((st >> 7) && !s.empty()) s.pop_back();
        return s;
    }
    std::string reqStr() {
        Val v = read(true);
        if (v.t != Val::Str) throw std::runtime_error("codec: expected string");
        return v.s;
    }
    Val readRaw(size_t len, bool asStr) {
        if (len > n - i) throw std::runtime_error("codec: eof");
        Val v;
        if (asStr) {
            v.t = Val::Str;
            v.s.assign((const char*)d + i, len);
        } else {
            v.t = Val::Bin;
            v.b.assign(d + i, d + i + len);
        }
        i += len;
        return v;
    }
    Val jidVal(const Jid& j) {
        Val v;
        v.t = Val::Str;
        v.s = j.str();
        v.jid = true;
        return v;
    }
    Val read(bool asStr) {
        int tag = u8();
        switch (tag) {
            case ListEmpty:
                return {};
            case List8:
            case List16: {
                int c = listSize(tag);
                Val v;
                v.t = Val::List;
                if ((size_t)c > n - i) throw std::runtime_error("codec: list too long");
                v.l.reserve(c);
                for (int k = 0; k < c; k++) v.l.push_back(node());
                return v;
            }
            case Binary8:
                return readRaw(u8(), asStr);
            case Binary20: {
                int a = u8() & 15, b = u8(), c = u8();
                return readRaw((size_t)(a << 16 | b << 8 | c), asStr);
            }
            case Binary32: {
                uint32_t l = 0;
                for (int k = 0; k < 4; k++) l = l << 8 | u8();
                return readRaw(l, asStr);
            }
            case 236: case 237: case 238: case 239: {
                int k = u8();
                const Dict& dd = kDoubles[tag - Dict0];
                if ((size_t)k >= dd.n) throw std::runtime_error("codec: double token");
                Val v;
                v.t = Val::Str;
                v.s = dd.t[k];
                return v;
            }
            case JIDPair: {
                Val u = read(true);
                std::string srv = reqStr();
                return jidVal(Jid{u.t == Val::Str ? u.s : "", srv, 0, 0});
            }
            case ADJID: {
                int ag = u8(), dev = u8();
                std::string user = reqStr();
                Jid j;
                j.user = user;
                j.device = (uint16_t)dev;
                switch (ag) {
                    case 1: j.server = "lid"; break;
                    case 128: j.server = "hosted"; break;
                    case 129: j.server = "hosted.lid"; break;
                    default: j.server = "s.whatsapp.net"; j.agent = (uint8_t)ag; break;
                }
                return jidVal(j);
            }
            case FBJID: {
                std::string user = reqStr();
                int dev = beN(2);
                std::string srv = reqStr();
                return jidVal(Jid{user, srv, (uint16_t)dev, 0});
            }
            case InteropJID: {
                std::string user = reqStr();
                int dev = beN(2);
                beN(2);
                std::string srv = reqStr();
                return jidVal(Jid{user, srv, (uint16_t)dev, 0});
            }
            case Nibble8:
            case Hex8: {
                Val v;
                v.t = Val::Str;
                v.s = packed(tag);
                return v;
            }
            default:
                if (tag >= 1 && (size_t)tag < kSingleN) {
                    Val v;
                    v.t = Val::Str;
                    v.s = kSingle[tag];
                    return v;
                }
                throw std::runtime_error("codec: bad token");
        }
    }
    Node node() {
        if (++depth > 64) throw std::runtime_error("codec: too deep");
        int sz = listSize(u8());
        Node r;
        r.tag = reqStr();
        if (sz == 0 || r.tag.empty()) throw std::runtime_error("codec: bad node");
        for (int k = 0; k < (sz - 1) >> 1; k++) {
            std::string key = reqStr();
            Val v = read(true);
            if (v.t == Val::Str) r.attrs.push_back({key, v.s, v.jid});
        }
        if (sz % 2 == 0) {
            Val v = read(false);
            switch (v.t) {
                case Val::Nil: break;
                case Val::Str:
                    r.kind = Node::Kind::Str;
                    r.data = B(v.s);
                    break;
                case Val::Bin:
                    r.kind = Node::Kind::Bytes;
                    r.data = std::move(v.b);
                    break;
                case Val::List:
                    r.kind = Node::Kind::Kids;
                    r.kids = std::move(v.l);
                    break;
            }
        }
        depth--;
        return r;
    }
};
}  // namespace

std::string Jid::str() const {
    if (server.empty()) return "";
    std::string s = user;
    if (agent) s += "." + std::to_string(agent);
    if (agent || device) s += ":" + std::to_string(device);
    if (!s.empty()) s += "@";
    return s + server;
}

Jid Jid::parse(const std::string& s) {
    Jid j;
    auto at = s.find('@');
    if (at == std::string::npos) {
        j.server = s;
        return j;
    }
    j.server = s.substr(at + 1);
    std::string u = s.substr(0, at);
    if (auto c = u.find(':'); c != std::string::npos) {
        try {
            j.device = (uint16_t)std::stoi(u.substr(c + 1));
        } catch (...) {
            return Jid{};
        }
        u.resize(c);
    }
    // A dot-suffixed agent exists only on phone-number addresses.
    if (j.server == "s.whatsapp.net") {
        if (auto d = u.find('.'); d != std::string::npos) {
            try {
                j.agent = (uint8_t)std::stoi(u.substr(d + 1));
            } catch (...) {
                return Jid{};
            }
            u.resize(d);
        }
    }
    j.user = u;
    return j;
}

std::string Node::attr(std::string_view k) const {
    for (auto& a : attrs)
        if (a.key == k) return a.val;
    return "";
}
Jid Node::jid(std::string_view k) const {
    std::string v = attr(k);
    return v.empty() ? Jid{} : Jid::parse(v);
}
const Node* Node::child(std::string_view t) const {
    for (auto& k : kids)
        if (k.tag == t) return &k;
    return nullptr;
}
std::vector<const Node*> Node::children(std::string_view t) const {
    std::vector<const Node*> r;
    for (auto& k : kids)
        if (k.tag == t) r.push_back(&k);
    return r;
}

Bytes encodeNode(const Node& n) {
    Enc e;
    e.node(n);
    return std::move(e.o);
}

Node decodeFrame(const uint8_t* d, size_t n) {
    if (n < 1) throw std::runtime_error("codec: empty frame");
    if (d[0] & 2) {
        Bytes inflated = zlibInflate(d + 1, n - 1, 64u << 20);
        Dec dec{inflated.data(), inflated.size()};
        return dec.node();
    }
    Dec dec{d + 1, n - 1};
    return dec.node();
}

std::string shape(const Node& n) {
    std::string s = "<" + n.tag;
    for (auto& a : n.attrs)
        if (a.key == "type" || a.key == "class" || a.key == "xmlns" || a.key == "code" || a.key == "reason")
            s += " " + a.key + "=" + a.val;
    s += ">";
    for (auto& k : n.kids) s += shape(k);
    return s;
}

}  // namespace zw
