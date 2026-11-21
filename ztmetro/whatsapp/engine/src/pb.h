// A small protobuf wire reader and writer. The daemon only touches a few fields of
// a few messages, so it needs no code generator and keeps no copy of the schemas.
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "crypto.h"

namespace zw::pb {

class Writer {
public:
    Bytes out;
    void varint(uint64_t v) {
        while (v >= 0x80) {
            out.push_back((uint8_t)v | 0x80);
            v >>= 7;
        }
        out.push_back((uint8_t)v);
    }
    Writer& u(int f, uint64_t v) {
        varint((uint64_t)f << 3);
        varint(v);
        return *this;
    }
    Writer& f32(int f, uint32_t v) {
        varint((uint64_t)f << 3 | 5);
        for (int i = 0; i < 4; i++) out.push_back((uint8_t)(v >> (i * 8)));
        return *this;
    }
    Writer& b(int f, const Bytes& v) {
        varint((uint64_t)f << 3 | 2);
        varint(v.size());
        append(out, v);
        return *this;
    }
    Writer& s(int f, std::string_view v) { return b(f, B(v)); }
    Writer& m(int f, const Writer& w) { return b(f, w.out); }
};

// One field-indexed view of a message. Unknown fields are kept and ignored, and any
// malformed input throws, so callers treat a thrown parse as a bad message.
class Msg {
public:
    struct F {
        int num;
        int wire;
        uint64_t v;
        Bytes d;
    };
    std::vector<F> fields;

    Msg() = default;
    explicit Msg(const Bytes& d) { parse(d.data(), d.size()); }

    void parse(const uint8_t* p, size_t n) {
        size_t i = 0;
        auto vi = [&]() -> uint64_t {
            uint64_t r = 0;
            for (int s = 0; s < 64; s += 7) {
                if (i >= n) throw std::runtime_error("pb: truncated");
                uint8_t c = p[i++];
                r |= (uint64_t)(c & 0x7f) << s;
                if (!(c & 0x80)) return r;
            }
            throw std::runtime_error("pb: long varint");
        };
        while (i < n) {
            uint64_t k = vi();
            F f{(int)(k >> 3), (int)(k & 7), 0, {}};
            switch (f.wire) {
                case 0:
                    f.v = vi();
                    break;
                case 1:
                    if (n - i < 8) throw std::runtime_error("pb: truncated");
                    i += 8;
                    break;
                case 2: {
                    uint64_t l = vi();
                    if (l > n - i) throw std::runtime_error("pb: truncated");
                    f.d.assign(p + i, p + i + l);
                    i += l;
                    break;
                }
                case 5:
                    if (n - i < 4) throw std::runtime_error("pb: truncated");
                    i += 4;
                    break;
                default:
                    throw std::runtime_error("pb: wire type");
            }
            fields.push_back(std::move(f));
        }
    }

    const F* get(int n) const {
        const F* r = nullptr;
        for (auto& f : fields)
            if (f.num == n) r = &f;  // last one wins, per the protobuf rules
        return r;
    }
    bool has(int n) const { return get(n) != nullptr; }
    Bytes bytes(int n) const {
        auto f = get(n);
        return f ? f->d : Bytes{};
    }
    std::string str(int n) const { return S(bytes(n)); }
    uint64_t num(int n, uint64_t def = 0) const {
        auto f = get(n);
        return f ? f->v : def;
    }
    std::optional<Msg> sub(int n) const {
        auto f = get(n);
        if (!f) return std::nullopt;
        return Msg(f->d);
    }
    std::vector<Msg> repeated(int n) const {
        std::vector<Msg> r;
        for (auto& f : fields)
            if (f.num == n) r.emplace_back(f.d);
        return r;
    }
};

}  // namespace zw::pb
