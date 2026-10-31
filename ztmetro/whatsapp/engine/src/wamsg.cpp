#include "wamsg.h"

namespace zw {

namespace {
std::string quotedOf(const pb::Msg& m, int ctxField) {
    if (auto c = m.sub(ctxField)) return c->str(1);
    return "";
}

void mediaInto(ParsedMsg& p, const pb::Msg& m, const char* kind, const char* mmsKind, int mime, int size, int w,
               int h, int thumb, int key, int fileSha, int enc, int path, int caption, int ctx, int fname) {
    p.kind = kind;
    if (caption) p.text = m.str(caption);
    p.quotedId = quotedOf(m, ctx);
    json j = {{"mime", m.str(mime)}, {"size", m.num(size)}, {"width", w ? m.num(w) : 0}, {"height", h ? m.num(h) : 0},
              {"file_name", fname ? m.str(fname) : ""}, {"thumb_b64", ""}, {"path", ""}};
    if (thumb && m.has(thumb)) {
        Bytes t = m.bytes(thumb);
        if (t.size() <= 64 * 1024) j["thumb_b64"] = b64(t);
    }
    p.media = j;
    if (m.has(key))
        p.secret = {{"type", mmsKind}, {"key", b64(m.bytes(key))}, {"sha", b64(m.bytes(fileSha))},
                    {"enc", b64(m.bytes(enc))}, {"path", m.str(path)}};
}
}  // namespace

ParsedMsg parseMessage(const Bytes& proto) {
    ParsedMsg p;
    pb::Msg m(proto);
    // Wrappers that hold a whole Message. Depth is capped against hostile nesting.
    for (int depth = 0; depth < 6; depth++) {
        if (auto s = m.sub(2)) {  // sender key distribution can sit beside any content
            p.skdmGroup = s->str(1);
            p.skdm = s->bytes(2);
        }
        std::optional<pb::Msg> inner;
        if (auto d = m.sub(31)) {
            p.dsmDest = d->str(1);
            inner = d->sub(2);
        } else {
            for (int f : {40, 37, 55, 53, 58, 62, 67})
                if (auto w = m.sub(f)) {
                    inner = w->sub(1);
                    break;
                }
        }
        if (!inner) break;
        m = std::move(*inner);
    }
    if (auto pm = m.sub(12)) {
        p.isProtocol = true;
        p.protoType = pm->num(2);
        if (auto k = pm->sub(1)) p.targetId = k->str(3);
        if (auto h = pm->sub(6)) p.history = std::move(*h);
        if (auto share = pm->sub(7))
            for (auto& k : share->repeated(1)) {
                auto id = k.sub(1);
                auto data = k.sub(2);
                if (id && data && !id->bytes(1).empty() && !data->bytes(1).empty()) p.appKeys.push_back({id->bytes(1), data->bytes(1)});
            }
        if (auto e = pm->sub(14)) {
            ParsedMsg ed = parseMessage(e->fields.empty() ? Bytes{} : pm->bytes(14));
            p.editText = ed.text;
        }
        return p;
    }
    if (m.has(1)) {
        p.kind = "text";
        p.text = m.str(1);
    } else if (auto e = m.sub(6)) {
        p.kind = "text";
        p.text = e->str(1);
        p.quotedId = quotedOf(*e, 17);
    } else if (auto i = m.sub(3)) {
        mediaInto(p, *i, "image", "WhatsApp Image Keys", 2, 5, 7, 6, 16, 8, 4, 9, 11, 3, 17, 0);
    } else if (auto v = m.sub(9)) {
        p.kind = "video";
        p.text = v->str(7);
        p.media = {{"mime", v->str(2)}, {"size", v->num(4)}, {"width", v->num(10)}, {"height", v->num(9)},
                   {"file_name", ""}, {"thumb_b64", v->has(16) ? b64(v->bytes(16)) : ""}, {"path", ""}};
    } else if (auto a = m.sub(8)) {
        p.kind = "audio";
        p.media = {{"mime", a->str(2)}, {"size", a->num(4)}, {"width", 0}, {"height", 0}, {"file_name", ""},
                   {"thumb_b64", ""}, {"path", ""}};
    } else if (auto d = m.sub(7)) {
        p.kind = "document";
        p.text = d->str(20);
        p.media = {{"mime", d->str(2)}, {"size", d->num(5)}, {"width", 0}, {"height", 0}, {"file_name", d->str(8)},
                   {"thumb_b64", d->has(16) ? b64(d->bytes(16)) : ""}, {"path", ""}};
    } else if (auto s = m.sub(26)) {
        p.kind = "sticker";
        p.media = {{"mime", s->str(5)}, {"size", s->num(9)}, {"width", s->num(7)}, {"height", s->num(6)},
                   {"file_name", ""}, {"thumb_b64", ""}, {"path", ""}};
    } else if (auto c = m.sub(4)) {
        p.kind = "contact";
        p.text = c->str(1);
    } else if (m.has(5)) {
        p.kind = "location";
    } else if (auto r = m.sub(46)) {
        p.kind = "reaction";
        p.text = r->str(2);
        if (auto k = r->sub(1)) p.targetId = k->str(3);
    } else {
        // Only side-channel fields such as context info or a sender key mean nothing to show.
        bool content = false;
        for (auto& f : m.fields)
            if (f.num != 2 && f.num != 35) content = true;
        if (content) {
            p.kind = "unsupported";
            p.text = "This message type is not supported yet";
        }
    }
    return p;
}

std::string previewOf(const std::string& kind, const std::string& text) {
    if (!text.empty() && kind != "unsupported" && kind != "undecryptable") return text.substr(0, 200);
    if (kind == "image") return "Photo";
    if (kind == "video") return "Video";
    if (kind == "audio") return "Voice message";
    if (kind == "document") return "Document";
    if (kind == "sticker") return "Sticker";
    if (kind == "location") return "Location";
    if (kind == "contact") return "Contact";
    return text;
}

Bytes buildTextMessage(const std::string& text, const std::string& quotedId, const std::string& quotedSender) {
    pb::Writer w;
    if (quotedId.empty()) {
        w.s(1, text);
        return w.out;
    }
    pb::Writer ctx;
    ctx.s(1, quotedId);
    if (!quotedSender.empty()) ctx.s(2, quotedSender);
    pb::Writer e;
    e.s(1, text).m(17, ctx);
    w.m(6, e);
    return w.out;
}

Bytes buildStatusText(const std::string& text) {
    pb::Writer e;
    e.s(1, text).f32(7, 0xFFFFFFFFu).f32(8, 0xFF00796Bu).u(9, 0);  // white text on a plain background
    pb::Writer w;
    w.m(6, e);
    return w.out;
}

Bytes buildDeviceSent(const std::string& dest, const Bytes& inner) {
    pb::Writer d;
    d.s(1, dest).b(2, inner);
    pb::Writer w;
    w.m(31, d);
    return w.out;
}

Bytes buildSkdmMessage(const std::string& group, const Bytes& skdm) {
    pb::Writer s;
    s.s(1, group).b(2, skdm);
    pb::Writer w;
    w.m(2, s);
    return w.out;
}

}  // namespace zw
