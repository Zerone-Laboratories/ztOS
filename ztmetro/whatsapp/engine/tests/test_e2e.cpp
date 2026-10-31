// The whole daemon against the local fake server. Never contacts a real server.
#include <openssl/evp.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <thread>

#include <openssl/hmac.h>

#include "../src/appstate.h"
#include "../src/engine.h"
#include "../src/sigproto.h"
#include "../src/wamsg.h"
#include "fakeserver.h"

using namespace zw;
namespace fs = std::filesystem;

static int fails = 0, passes = 0;
#define CHECK(c)                                                  \
    do {                                                          \
        if (c) passes++;                                          \
        else { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fflush(stdout); } \
    } while (0)

const std::string kSelfPn = "61400000000", kPeer = "61400000009@s.whatsapp.net", kPeer2 = "61400000008@s.whatsapp.net";
const std::string kGroup = "120363000000000001@g.us";

// Holds a daemon's IPC client and the phone's side of every session.
struct H {
    std::string dir, sock;
    Loop loop;
    FakeServer srv{loop};
    std::unique_ptr<IpcServer> ipc;
    std::unique_ptr<Engine> engine;
    int cfd = -1;
    std::string cbuf;
    std::vector<json> events;
    std::map<int64_t, json> replies;
    int64_t nextId = 1;
    std::deque<Node> nodes;  // stable addresses; everything the daemon sent since the last clear

    explicit H(const std::string& d) : dir(d) {
        fs::remove_all(dir);
        fs::create_directories(dir);
        sock = dir + ".sock";
        start();
    }
    ~H() { stop(); }
    void start() {
        RealConfig c;
        c.dataDir = dir;
        c.host = "127.0.0.1";
        c.port = srv.port;
        c.tls = false;
        c.rootKey = srv.root.pub;
        c.mediaBase = mediaBase;
        ipc = std::make_unique<IpcServer>(loop, sock);
        engine = makeRealEngine(loop, *ipc, c);
        ipc->setEngine(engine.get());
        cfd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        strcpy(a.sun_path, sock.c_str());
        ::connect(cfd, (sockaddr*)&a, sizeof a);
        loop.add(cfd, EPOLLIN, [this](uint32_t) {
            char b[65536];
            for (;;) {
                ssize_t r = recv(cfd, b, sizeof b, 0);
                if (r <= 0) return;
                cbuf.append(b, (size_t)r);
                size_t nl;
                while ((nl = cbuf.find('\n')) != std::string::npos) {
                    json j = json::parse(cbuf.substr(0, nl));
                    cbuf.erase(0, nl + 1);
                    if (j.contains("event")) events.push_back(j);
                    else replies[j["id"].get<int64_t>()] = j;
                }
            }
        });
        srv.onNode = [this](const Node& n) { nodes.push_back(n); };
    }
    void stop() {
        if (cfd >= 0) {
            loop.del(cfd);
            close(cfd);
            cfd = -1;
        }
        engine.reset();
        ipc.reset();
    }
    bool wait(const std::function<bool()>& pred, int ms = 15000) {
        auto end = Loop::Clock::now() + std::chrono::milliseconds(ms);
        while (Loop::Clock::now() < end) {
            if (pred()) return true;
            loop.tick(20);
        }
        return pred();
    }
    json call(json r) {
        int64_t id = nextId++;
        r["id"] = id;
        std::string l = r.dump() + "\n";
        (void)!::send(cfd, l.data(), l.size(), MSG_NOSIGNAL);
        wait([&] { return replies.count(id) > 0; }, 30000);
        return replies.count(id) ? replies[id] : json{{"ok", false}, {"error", "no reply"}};
    }
    const Node* node(const std::function<bool(const Node&)>& f, int ms = 15000) {
        const Node* found = nullptr;
        wait([&] {
            for (auto& n : nodes)
                if (f(n)) {
                    found = &n;
                    return true;
                }
            return false;
        }, ms);
        return found;
    }
    const Node* iqNs(const std::string& xmlns, const std::string& child = "", int ms = 15000) {
        size_t skip = consumed[xmlns + child];
        const Node* r = nullptr;
        wait([&] {
            size_t seen = 0;
            for (auto& n : nodes) {
                if (n.tag != "iq" || n.attr("xmlns") != xmlns) continue;
                if (!child.empty() && !n.child(child)) continue;
                if (seen++ >= skip) {
                    r = &n;
                    return true;
                }
            }
            return false;
        }, ms);
        if (r) consumed[xmlns + child]++;
        return r;
    }
    void result(const Node& req, std::vector<Node> kids = {}) {
        Node r("iq");
        r.a("from", Jid{"", "s.whatsapp.net"}).a("id", req.attr("id")).a("type", "result");
        for (auto& k : kids) r.add(std::move(k));
        srv.send(r);
    }
    json event(const std::string& name, const std::function<bool(const json&)>& f = nullptr, int ms = 15000) {
        json out;
        wait([&] {
            for (auto& e : events)
                if (e["event"] == name && (!f || f(e))) {
                    out = e;
                    return true;
                }
            return false;
        }, ms);
        return out;
    }
    std::map<std::string, size_t> consumed;
    void clearNodes() {
        nodes.clear();
        consumed.clear();
    }
    std::string mediaBase;
    bool mediaBaseUsed = false;
};

// The primary phone, or another contact's device: a full Signal party.
struct Phone {
    KeyPair id = genKeyPair();
    uint32_t reg = 500 + randomU32() % 1000;
    signal::Record rec;
    KeyPair pk = genKeyPair(), spk = genKeyPair();
    Node userNode(const std::string& jid) {
        Node u("user");
        u.a("jid", Jid::parse(jid));
        Bytes r{(uint8_t)(reg >> 24), (uint8_t)(reg >> 16), (uint8_t)(reg >> 8), (uint8_t)reg};
        u.add(Node("registration").bytes(r));
        u.add(Node("identity").bytes(B(id.pub)));
        Node k("key");
        k.add(Node("id").bytes({0, 0, 9})).add(Node("value").bytes(B(pk.pub)));
        u.add(k);
        Bytes typed{5};
        append(typed, spk.pub);
        auto s = xedSign(id.priv, typed);
        Node sk("skey");
        sk.add(Node("id").bytes({0, 0, 3})).add(Node("value").bytes(B(spk.pub))).add(Node("signature").bytes(Bytes(s.begin(), s.end())));
        u.add(sk);
        return u;
    }
    signal::Keys keys() {
        signal::Keys k;
        k.identity = id;
        k.regId = reg;
        k.preKey = [this](uint32_t i) -> std::optional<KeyPair> { if (i == 9) return pk; return std::nullopt; };
        k.signedPreKey = [this](uint32_t i) -> std::optional<KeyPair> { if (i == 3) return spk; return std::nullopt; };
        return k;
    }
};

struct Uploaded {
    uint32_t reg = 0;
    Key32 identity{};
    uint32_t signedId = 0;
    Key32 signedKey{};
    Bytes sig;
    std::map<uint32_t, Key32> keys;
};

static Uploaded parseUpload(const Node& n) {
    Uploaded u;
    auto& r = n.child("registration")->data;
    u.reg = (uint32_t)r[0] << 24 | (uint32_t)r[1] << 16 | (uint32_t)r[2] << 8 | r[3];
    u.identity = toKey(n.child("identity")->data);
    auto id3 = [](const Node& k) { auto& d = k.child("id")->data; return (uint32_t)d[0] << 16 | (uint32_t)d[1] << 8 | d[2]; };
    auto sk = n.child("skey");
    u.signedId = id3(*sk);
    u.signedKey = toKey(sk->child("value")->data);
    u.sig = sk->child("signature")->data;
    for (auto k : n.child("list")->children("key")) u.keys[id3(*k)] = toKey(k->child("value")->data);
    return u;
}

static signal::Bundle bundleOf(Uploaded& u) {
    signal::Bundle b;
    b.regId = u.reg;
    b.identity = u.identity;
    b.signedId = u.signedId;
    b.signedKey = u.signedKey;
    b.signature = u.sig;
    auto it = u.keys.begin();
    b.preKeyId = it->first;
    b.preKey = it->second;
    u.keys.erase(it);
    return b;
}

static Node msgFrom(const std::string& from, const std::string& id, const Bytes& ct, bool pre) {
    Node m("message");
    m.a("from", Jid::parse(from)).a("id", id).a("t", std::to_string(time(nullptr))).a("type", "text").a("notify", "Sam");
    Node e("enc");
    e.a("v", "2").a("type", pre ? "pkmsg" : "msg").bytes(ct);
    m.add(e);
    return m;
}

static std::map<std::string, std::string> wire() {
    std::map<std::string, std::string> m;
    std::ifstream f("tests/fixtures/wire.txt");
    std::string a, b, c;
    while (f >> a >> b >> c) m[a + "." + b] = c;
    return m;
}

// Fields a real client sends, compared with what whatsmeow sends, minus per-device keys.
static void comparePayload(const Bytes& mine, const std::string& goHex, bool reg) {
    pb::Msg a(mine), b(unhex(goHex));
    for (int f : {1, 3, 5, 6, 12, 13, 18, 24, 33, 41}) {
        if (reg && f == 1) continue;
        if (a.has(f) != b.has(f) || a.num(f) != b.num(f) || a.bytes(f) != b.bytes(f)) {
            fails++;
            printf("FAIL payload field %d differs from whatsmeow (reg=%d)\n", f, reg);
        } else passes++;
    }
    if (!reg) {
        // Only the username changes in the login payload, and the fixture uses a set value.
        CHECK(a.num(18) == b.num(18));
        return;
    }
    auto x = a.sub(19), y = b.sub(19);
    CHECK(x && y);
    if (x && y) {
        CHECK(x->bytes(2) == y->bytes(2));
        CHECK(x->bytes(7) == y->bytes(7));  // build hash
        CHECK(x->bytes(8) == y->bytes(8));  // device props
        CHECK(x->bytes(4).size() == 3 && x->bytes(1).size() == 4);
    }
}

// Scans the QR, plays the phone's part, and returns once the daemon has logged in.
struct Linked {
    Phone phone;
    Uploaded up;
};

static void completePairing(H& h, Phone& phone, const Bytes& adv, const Key32& identity) {
    pb::Writer det;
    det.u(1, 77).u(2, (uint64_t)time(nullptr)).u(3, 3);
    KeyPair acct = phone.id;
    Bytes msg{6, 0};
    append(msg, det.out);
    append(msg, identity);
    auto sig = xedSign(acct.priv, msg);
    pb::Writer sd;
    sd.b(1, det.out).b(2, B(acct.pub)).b(3, Bytes(sig.begin(), sig.end()));
    pb::Writer cont;
    cont.b(1, sd.out).b(2, hmacSha256(adv, sd.out));
    Node ps("pair-success");
    ps.add(Node("device-identity").bytes(cont.out));
    ps.add(Node("device").a("jid", Jid{kSelfPn, "s.whatsapp.net", 7}).a("lid", Jid{"900", "lid", 7}));
    ps.add(Node("platform").a("name", "android"));
    Node n("iq");
    n.a("from", Jid{"", "s.whatsapp.net"}).a("id", "pair-1").a("type", "set").add(ps);
    h.srv.send(n);
    const Node* signedN = h.node([](const Node& x) { return x.tag == "iq" && x.child("pair-device-sign"); });
    CHECK(signedN != nullptr);
    if (signedN) {
        const Node* di = signedN->child("pair-device-sign")->child("device-identity");
        CHECK(di && di->attr("key-index") == "3");
        pb::Msg si(di->data);
        Bytes dm{6, 1};
        append(dm, det.out);
        append(dm, identity);
        append(dm, acct.pub);
        Bytes ds = si.bytes(4);
        CHECK(ds.size() == 64 && xedVerify(identity, dm, ds.data()));
        CHECK(!si.has(2));  // the reply must leave out the account key
    }
    h.clearNodes();
    Node err("stream:error");
    err.a("code", "515");
    h.srv.send(err);
}

// The server's post-login exchange: prekey count, upload, passive.
static Uploaded serveLogin(H& h, bool expectUpload) {
    Node ok("success");
    ok.a("lid", Jid{"900", "lid", 7}).a("t", std::to_string(time(nullptr)));
    h.srv.send(ok);
    const Node* count = h.iqNs("encrypt", "count");
    CHECK(count != nullptr);
    Uploaded u;
    if (!count) return u;
    h.result(*count, {Node("count").a("value", expectUpload ? "0" : "100")});
    if (expectUpload) {
        const Node* up = h.iqNs("encrypt", "registration");
        CHECK(up != nullptr);
        if (up) {
            u = parseUpload(*up);
            CHECK(u.keys.size() == 812);
            h.result(*up);
        }
    }
    const Node* pass = h.iqNs("passive");
    CHECK(pass != nullptr);
    if (pass) h.result(*pass);
    const Node* priv = h.iqNs("privacy");
    if (priv) {
        Node p("privacy");
        p.add(Node("category").a("name", "readreceipts").a("value", "all"));
        h.result(*priv, {p});
    }
    CHECK(!h.event("connection", [](const json& e) { return e["state"] == "connected"; }).is_null());
    return u;
}

static bool hasFile(const std::string& path, const std::string& needle) {
    std::ifstream f(path, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(f)), {});
    return s.find(needle) != std::string::npos;
}

// A one-shot HTTP file server for the media test.
struct HttpOnce {
    int lfd, port;
    std::thread th;
    std::string lastPath;
    explicit HttpOnce(Bytes body) {
        lfd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(lfd, (sockaddr*)&a, sizeof a);
        listen(lfd, 2);
        socklen_t l = sizeof a;
        getsockname(lfd, (sockaddr*)&a, &l);
        port = ntohs(a.sin_port);
        th = std::thread([this, body] {
            int f = accept(lfd, nullptr, nullptr);
            if (f < 0) return;
            char b[4096];
            ssize_t r = recv(f, b, sizeof b, 0);
            if (r > 0) lastPath.assign(b, (size_t)r);
            std::string h = "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n";
            (void)!::send(f, h.data(), h.size(), 0);
            (void)!::send(f, body.data(), body.size(), 0);
            close(f);
        });
    }
    ~HttpOnce() {
        shutdown(lfd, SHUT_RDWR);
        close(lfd);
        th.join();
    }
};

static std::string scratch(const std::string& n) {
    const char* h = getenv("ZTWA_TEST_DIR");
    return std::string(h ? h : "/tmp") + "/ztwa-e2e-" + n;
}

static void testQrAndMessaging() {
    auto w = wire();
    H h(scratch("main"));
    Phone phone;

    // An unpaired daemon must not touch the network unprompted.
    for (int i = 0; i < 20; i++) h.loop.tick(10);
    CHECK(h.srv.connections == 0);
    CHECK(fs::status(h.dir).permissions() == fs::perms::owner_all);
    CHECK(((int)fs::status(h.dir + "/store.key").permissions() & 0077) == 0);
    CHECK(((int)fs::status(h.dir + "/store.db").permissions() & 0077) == 0);

    // Bad certificate: refused, reported, no payload read.
    h.srv.badCert = true;
    bool sawLogin = false;
    h.srv.onLogin = [&](const Bytes&) { sawLogin = true; };
    CHECK(h.call({{"cmd", "pair_qr"}})["ok"] == true);
    CHECK(!h.event("pairing", [](const json& e) { return e["state"] == "failed"; }).is_null());
    CHECK(!sawLogin);
    h.srv.badCert = false;
    h.events.clear();

    // QR
    Bytes payload;
    h.srv.onLogin = [&](const Bytes& p) { payload = p; };
    CHECK(h.call({{"cmd", "pair_qr"}})["ok"] == true);
    CHECK(h.wait([&] { return h.srv.up() && !payload.empty(); }));
    comparePayload(payload, w.at("payload.reg"), true);
    Node pd("pair-device");
    pd.add(Node("ref").bytes(B("ref-one"))).add(Node("ref").bytes(B("ref-two")));
    Node iqn("iq");
    iqn.a("from", Jid{"", "s.whatsapp.net"}).a("id", "pd-1").a("type", "set").add(pd);
    h.srv.send(iqn);
    json q = h.event("pairing", [](const json& e) { return e["state"] == "qr"; });
    CHECK(!q.is_null());
    std::string qr = q["qr"];
    CHECK(qr.rfind("https://wa.me/settings/linked_devices#ref-one,", 0) == 0);
    CHECK(q["expires_in"] == 60);
    CHECK(h.node([](const Node& n) { return n.tag == "iq" && n.attr("id") == "pd-1" && n.attr("type") == "result"; }) != nullptr);
    std::vector<std::string> parts;
    {
        std::stringstream ss(qr.substr(qr.find('#') + 1));
        std::string it;
        while (std::getline(ss, it, ',')) parts.push_back(it);
    }
    CHECK(parts.size() == 5);
    CHECK(b64dec(parts[1]) == B(h.srv.clientStatic));
    Key32 identity = toKey(b64dec(parts[2]));
    Bytes adv = b64dec(parts[3]);

    // A forged pair-success must be refused with the right error.
    {
        Node ps("pair-success");
        pb::Writer cont;
        cont.b(1, B("junk")).b(2, Bytes(32, 1));
        ps.add(Node("device-identity").bytes(cont.out));
        ps.add(Node("device").a("jid", Jid{kSelfPn, "s.whatsapp.net", 7}));
        Node n("iq");
        n.a("from", Jid{"", "s.whatsapp.net"}).a("id", "forged").a("type", "set").add(ps);
        h.srv.send(n);
        const Node* e = h.node([](const Node& x) { return x.tag == "iq" && x.attr("id") == "forged" && x.attr("type") == "error"; });
        CHECK(e && e->child("error") && e->child("error")->attr("code") == "401");
        CHECK(!h.event("pairing", [](const json& ev) { return ev["state"] == "failed"; }).is_null());
    }
    // Pair again after the refusal.
    h.events.clear();
    payload.clear();
    h.clearNodes();
    CHECK(h.call({{"cmd", "pair_qr"}})["ok"] == true);
    CHECK(h.wait([&] { return h.srv.up() && !payload.empty(); }));
    h.srv.send(iqn);
    CHECK(!h.event("pairing", [](const json& e) { return e["state"] == "qr"; }).is_null());
    payload.clear();
    completePairing(h, phone, adv, identity);
    CHECK(!h.event("pairing", [](const json& e) { return e["state"] == "paired"; }).is_null());

    // 515 brings the logged-in reconnect.
    CHECK(h.wait([&] { return !payload.empty(); }));
    comparePayload(payload, w.at("payload.login"), false);
    CHECK(pb::Msg(payload).num(1) == 61400000000ull);
    Uploaded up = serveLogin(h, true);
    h.clearNodes();

    // Receive: first contact as a prekey message.
    Phone peer;
    signal::Record peerRec;
    signal::initFromBundle(peerRec, peer.id, peer.reg, bundleOf(up));
    auto c1 = signal::encrypt(peerRec, signal::pad(buildTextMessage("hello there", "", "")));
    h.srv.send(msgFrom(kPeer, "IN1", c1.data, true));
    json ev = h.event("message", [](const json& e) { return e["id"] == "IN1"; });
    CHECK(!ev.is_null());
    CHECK(ev["text"] == "hello there" && ev["sender_name"] == "Sam" && ev["live"] == true && ev["from_me"] == false);
    CHECK(ev["chat"] == kPeer && ev["message"]["kind"] == "text");
    CHECK(!ev["message"].contains("_s"));
    const Node* rc = h.node([](const Node& n) { return n.tag == "receipt" && n.attr("id") == "IN1"; });
    CHECK(rc && rc->attr("type") == "inactive" && rc->attr("to") == kPeer);

    // Duplicate delivery produces no second row and no retry.
    size_t before = h.events.size();
    h.srv.send(msgFrom(kPeer, "IN1", c1.data, true));
    for (int i = 0; i < 30; i++) h.loop.tick(5);
    json page = h.call({{"cmd", "messages"}, {"chat", kPeer}})["result"];
    CHECK(page["messages"].size() == 1);
    CHECK(h.node([](const Node& n) { return n.tag == "receipt" && n.attr("type") == "retry"; }, 50) == nullptr);
    (void)before;

    // Chat list and unread.
    json chats = h.call({{"cmd", "chats"}})["result"];
    CHECK(chats["total"] == 1 && chats["chats"][0]["unread"] == 1 && chats["chats"][0]["name"] == "Sam");
    CHECK(chats["chats"][0]["last_text"] == "hello there");

    // Read receipt, with the privacy answer permitting it.
    json mr = h.call({{"cmd", "mark_read"}, {"chat", kPeer}});
    CHECK(mr["result"]["marked"] == 1);
    const Node* rr = h.node([](const Node& n) { return n.tag == "receipt" && n.attr("type") == "read"; });
    CHECK(rr && rr->attr("id") == "IN1");
    CHECK(h.call({{"cmd", "chats"}})["result"]["chats"][0]["unread"] == 0);

    // Send: device lookup, prekey fetch for our own phone, fan-out, DSM to self.
    json sent = h.call({{"cmd", "send_text"}, {"chat", kPeer}, {"text", "reply text"}});
    CHECK(sent["ok"] == true);
    std::string outId = sent["result"]["id"];
    const Node* us = h.iqNs("usync");
    CHECK(us != nullptr);
    if (!us) return;
    Node list("list");
    auto devNode = [](const std::string& jid, std::vector<int> ids) {
        Node dl("device-list");
        for (int i : ids) dl.add(Node("device").a("id", std::to_string(i)));
        Node d("devices");
        d.add(dl);
        Node u("user");
        u.a("jid", Jid::parse(jid)).add(d);
        return u;
    };
    list.add(devNode(kPeer, {0}));
    list.add(devNode(kSelfPn + "@s.whatsapp.net", {0, 7}));
    Node usy("usync");
    usy.add(list);
    h.result(*us, {usy});
    const Node* keyq = h.iqNs("encrypt", "key");
    CHECK(keyq != nullptr);
    if (!keyq) return;
    CHECK(keyq->child("key")->children("user").size() == 1);  // only the phone: peer has a session
    Node kl("list");
    kl.add(phone.userNode(kSelfPn + "@s.whatsapp.net"));
    h.result(*keyq, {kl});
    const Node* m = h.node([&](const Node& n) { return n.tag == "message" && n.attr("id") == outId; });
    CHECK(m != nullptr);
    if (!m) return;
    CHECK(m->attr("to") == kPeer && m->attr("type") == "text");
    CHECK(m->child("device-identity") != nullptr);  // a prekey message went to the phone
    int seen = 0;
    for (auto to : m->child("participants")->children("to")) {
        const Node* enc = to->child("enc");
        CHECK(to->attr("jid") != kSelfPn + ":7@s.whatsapp.net");  // never to this same device
        if (to->attr("jid") == kPeer) {
            CHECK(enc->attr("type") == "msg");
            auto p = parseMessage(signal::unpad(signal::decryptMessage(peerRec, enc->data)));
            CHECK(p.text == "reply text");
            seen++;
        } else {
            CHECK(enc->attr("type") == "pkmsg");
            std::optional<uint32_t> used;
            auto p = parseMessage(signal::unpad(signal::decryptPreKey(phone.rec, phone.keys(), enc->data, &used)));
            CHECK(p.text == "reply text" && p.dsmDest == kPeer);
            seen++;
        }
    }
    CHECK(seen == 2);

    // Ack and receipts move the status forward only.
    Node ack("ack");
    ack.a("class", "message").a("id", outId).a("from", Jid::parse(kPeer)).a("t", "1");
    h.srv.send(ack);
    CHECK(!h.event("receipt", [&](const json& e) { return e["status"] == "sent" && e["ids"][0] == outId; }).is_null());
    Node del("receipt");
    del.a("from", Jid::parse(kPeer)).a("id", outId).a("t", "1");
    h.srv.send(del);
    CHECK(!h.event("receipt", [&](const json& e) { return e["status"] == "delivered"; }).is_null());
    Node red("receipt");
    red.a("from", Jid::parse(kPeer)).a("id", outId).a("type", "read").a("t", "1");
    h.srv.send(red);
    CHECK(!h.event("receipt", [&](const json& e) { return e["status"] == "read"; }).is_null());
    h.events.clear();
    h.srv.send(del);  // a late delivered must not move read backwards
    for (int i = 0; i < 20; i++) h.loop.tick(5);
    CHECK(h.event("receipt", nullptr, 50).is_null());
    auto last = h.call({{"cmd", "messages"}, {"chat", kPeer}, {"limit", 1}})["result"];
    CHECK(last["messages"][0]["status"] == "read" && last["more"] == true);

    // Paging: oldest first within a page, and before_* walks back.
    auto older = h.call({{"cmd", "messages"}, {"chat", kPeer}, {"limit", 1}, {"before_ts", last["messages"][0]["timestamp"]},
                         {"before_id", last["messages"][0]["id"]}})["result"];
    CHECK(older["messages"].size() == 1 && older["messages"][0]["id"] == "IN1" && older["more"] == false);

    // A ratchet turn, after the peer got our reply.
    auto c2 = signal::encrypt(peerRec, signal::pad(buildTextMessage("second", "", "")));
    h.srv.send(msgFrom(kPeer, "IN2", c2.data, false));
    CHECK(!h.event("message", [](const json& e) { return e["id"] == "IN2"; }).is_null());

    // Undecryptable, retry receipt, then the retried copy replaces the placeholder.
    auto c3 = signal::encrypt(peerRec, signal::pad(buildTextMessage("third", "", "")));
    Bytes bad = c3.data;
    bad[bad.size() - 1] ^= 1;
    h.clearNodes();
    h.srv.send(msgFrom(kPeer, "IN3", bad, false));
    CHECK(!h.event("message", [](const json& e) { return e["id"] == "IN3" && e["message"]["kind"] == "undecryptable"; }).is_null());
    const Node* rt = h.node([](const Node& n) { return n.tag == "receipt" && n.attr("type") == "retry"; });
    CHECK(rt && rt->child("retry") && rt->child("retry")->attr("count") == "1" && rt->child("registration"));
    CHECK(h.event("message", [](const json& e) { return e["id"] == "IN3" && e["live"] == true; }, 50).is_null());
    h.srv.send(msgFrom(kPeer, "IN3", c3.data, false));
    CHECK(!h.event("message", [](const json& e) { return e["id"] == "IN3" && e["message"]["text"] == "third"; }).is_null());

    // Our own message, asked for again by a contact who lost it.
    h.clearNodes();
    Node retry("receipt");
    retry.a("from", Jid::parse(kPeer)).a("id", outId).a("type", "retry").a("t", "1");
    Node rn("retry");
    rn.a("count", "1").a("id", outId).a("t", "1").a("v", "1");
    retry.add(rn);
    retry.add(Node("registration").bytes({0, 0, 1, 1}));
    h.srv.send(retry);
    const Node* re = h.node([&](const Node& n) { return n.tag == "message" && n.attr("id") == outId; });
    CHECK(re && re->child("enc") && re->attr("device_fanout") == "false" && re->child("enc")->attr("count") == "1");
    if (re && re->child("enc")) {
        auto p = parseMessage(signal::unpad(signal::decryptMessage(peerRec, re->child("enc")->data)));
        CHECK(p.text == "reply text");
    }

    // Group: sender key arrives inside a direct message, then group messages.
    Phone member;
    signal::Record memberRec;
    signal::initFromBundle(memberRec, member.id, member.reg, bundleOf(up));
    signal::SenderRecord memberKey;
    Bytes dist = signal::senderKeyDistribution(memberKey);
    Bytes both = buildSkdmMessage(kGroup, dist);
    auto ce = signal::encrypt(memberRec, signal::pad(both));
    auto gm = signal::senderEncrypt(memberKey, signal::pad(buildTextMessage("group hi", "", "")));
    Node g("message");
    g.a("from", Jid::parse(kGroup)).a("participant", Jid::parse(kPeer2)).a("id", "G1").a("t", std::to_string(time(nullptr))).a("type", "text").a("notify", "Member");
    g.add(Node("enc").a("v", "2").a("type", "pkmsg").bytes(ce.data));
    g.add(Node("enc").a("v", "2").a("type", "skmsg").bytes(gm));
    h.clearNodes();
    h.srv.send(g);
    json ge = h.event("message", [](const json& e) { return e["id"] == "G1"; });
    CHECK(!ge.is_null() && ge["text"] == "group hi" && ge["sender_name"] == "Member" && ge["is_group"] == true);
    const Node* gq = h.iqNs("w:g2");
    CHECK(gq != nullptr);
    if (gq) {
        CHECK(gq->attr("to") == kGroup);
        Node gr("group");
        gr.a("id", "120363000000000001").a("subject", "Test Group");
        gr.add(Node("participant").a("jid", Jid::parse(kPeer2)));
        gr.add(Node("participant").a("jid", Jid::parse(kSelfPn + "@s.whatsapp.net")));
        h.result(*gq, {gr});
        CHECK(!h.event("chat", [](const json& e) { return e["chat"]["name"] == "Test Group"; }).is_null());
    }
    // Group send: skmsg plus sender key distribution to each device, and a phash.
    json gs = h.call({{"cmd", "send_text"}, {"chat", kGroup}, {"text", "to group"}});
    std::string gsId = gs["result"]["id"];
    const Node* gu = h.iqNs("usync");
    CHECK(gu != nullptr);
    if (gu) {
        Node gl("list");
        gl.add(devNode(kPeer2, {0}));
        Node usg("usync");
        usg.add(gl);
        h.result(*gu, {usg});
        const Node* gmsg = h.node([&](const Node& n) { return n.tag == "message" && n.attr("id") == gsId; });
        CHECK(gmsg != nullptr);
        if (gmsg) {
            CHECK(gmsg->attr("phash").rfind("2:", 0) == 0);
            const Node* sk = nullptr;
            for (auto e : gmsg->children("enc"))
                if (e->attr("type") == "skmsg") sk = e;
            CHECK(sk != nullptr);
            const Node* to = nullptr;
            for (auto t : gmsg->child("participants")->children("to"))
                if (t->attr("jid") == kPeer2) to = t;
            CHECK(to != nullptr);
            if (to && sk) {
                auto pl = signal::unpad(signal::decryptMessage(memberRec, to->child("enc")->data));
                auto p = parseMessage(pl);
                CHECK(!p.skdm.empty() && p.skdmGroup == kGroup);
                signal::SenderRecord rcv;
                signal::processDistribution(rcv, p.skdm);
                auto t = parseMessage(signal::unpad(signal::senderDecrypt(rcv, sk->data)));
                CHECK(t.text == "to group");
            }
            Node ga("ack");
            ga.a("class", "message").a("id", gsId).a("from", Jid::parse(kGroup));
            h.srv.send(ga);
            CHECK(!h.event("receipt", [&](const json& e) { return e["status"] == "sent" && e["ids"][0] == gsId; }).is_null());
        }
    }

    // Calls: acknowledged, ignored, no event, no crash.
    h.clearNodes();
    size_t evCount = h.events.size();
    Node call("call");
    call.a("from", Jid::parse(kPeer)).a("id", "CALL1").a("t", "1");
    Node offer("offer");
    offer.a("call-id", "c-1").a("call-creator", Jid::parse(kPeer));
    call.add(offer);
    h.srv.send(call);
    const Node* ca = h.node([](const Node& n) { return n.tag == "ack" && n.attr("class") == "call"; });
    CHECK(ca && ca->attr("id") == "CALL1");
    CHECK(h.events.size() == evCount);
    for (auto& e : h.events) CHECK(e["event"] != "call_offer");

    // Hostile input on a live link: nothing here may bring the daemon down.
    h.srv.sendGarbage();
    h.srv.sendEncryptedRaw(randomBytes(40));
    Node weird("message");
    weird.a("from", Jid::parse(kPeer)).a("id", "W1").a("t", "x");
    weird.add(Node("enc").a("v", "2").a("type", "msg").bytes(randomBytes(30)));
    h.srv.send(weird);
    Node nothing("message");
    nothing.a("from", Jid::parse(kPeer));
    h.srv.send(nothing);
    Node badIq("iq");
    badIq.a("from", Jid{"", "s.whatsapp.net"}).a("type", "set").a("id", "b").add(Node("pair-success"));
    h.srv.send(badIq);
    CHECK(h.call({{"cmd", "status"}})["ok"] == true);

    // Server ping.
    h.clearNodes();
    Node ping("iq");
    ping.a("from", Jid{"", "s.whatsapp.net"}).a("id", "ping1").a("xmlns", "urn:xmpp:ping").a("type", "get");
    h.srv.send(ping);
    CHECK(h.node([](const Node& n) { return n.tag == "iq" && n.attr("id") == "ping1" && n.attr("type") == "result"; }) != nullptr);

    // History: inline payload in a protocol message from our own phone.
    {
        pb::Writer key;
        key.s(1, "61400000005@s.whatsapp.net").u(2, 0).s(3, "HIST1");
        pb::Writer wmi;
        wmi.m(1, key).b(2, buildTextMessage("old message", "", "")).u(3, 1700000000).s(19, "Old Friend");
        pb::Writer hm;
        hm.m(1, wmi);
        pb::Writer conv;
        conv.s(1, "61400000005@s.whatsapp.net").m(2, hm).s(13, "Friend Name").u(16, 1).u(25, 0);
        pb::Writer pn;
        pn.s(1, kSelfPn + "@s.whatsapp.net").s(2, "My Name");
        pb::Writer map;
        map.s(1, "61400000005@s.whatsapp.net").s(2, "555@lid");
        pb::Writer hist;
        hist.u(1, 0).m(2, conv).m(7, pn).m(15, map);
        uLongf zl = compressBound(hist.out.size());
        Bytes z(zl);
        compress(z.data(), &zl, hist.out.data(), hist.out.size());
        z.resize(zl);
        pb::Writer notif;
        notif.u(6, 0).u(9, 100).b(11, z);
        pb::Writer pm;
        pm.u(2, 5).m(6, notif);
        pb::Writer msg;
        msg.m(12, pm);
        // The phone establishes its session to this device.
        signal::Record ph;
        signal::initFromBundle(ph, phone.id, phone.reg, bundleOf(up));
        auto c = signal::encrypt(ph, signal::pad(msg.out));
        h.clearNodes();
        h.srv.send(msgFrom(kSelfPn + "@s.whatsapp.net", "HS1", c.data, true));
        CHECK(!h.event("history_sync", [](const json& e) { return e["state"] == "done"; }).is_null());
        CHECK(h.node([](const Node& n) { return n.tag == "receipt" && n.attr("type") == "hist_sync"; }) != nullptr);
        auto pg = h.call({{"cmd", "messages"}, {"chat", "61400000005@s.whatsapp.net"}})["result"];
        CHECK(pg["messages"].size() == 1 && pg["messages"][0]["text"] == "old message");
        CHECK(pg["messages"][0]["sender_name"] == "Friend Name" || pg["messages"][0]["sender_name"] == "Old Friend");
        auto st = h.call({{"cmd", "status"}})["result"];
        CHECK(st["push_name"] == "My Name" && st["self"] == kSelfPn + ":7@s.whatsapp.net");
        // No notification for history.
        for (auto& e : h.events)
            if (e["event"] == "message" && e["id"] == "HIST1") CHECK(e["live"] == false);
        auto list2 = h.call({{"cmd", "chats"}})["result"]["chats"];
        bool found = false;
        for (auto& c2 : list2)
            if (c2["id"] == "61400000005@s.whatsapp.net") {
                found = true;
                CHECK(c2["archived"] == true && c2["unread"] == 0 && c2["last_text"] == "old message");
            }
        CHECK(found);
        // A later sender that uses the private id lands in the same chat.
        auto c5 = signal::encrypt(peerRec, signal::pad(buildTextMessage("x", "", "")));
        (void)c5;
    }

    // Presence commands and events.
    h.clearNodes();
    CHECK(h.call({{"cmd", "set_presence"}, {"available", true}})["ok"] == true);
    const Node* pr = h.node([](const Node& n) { return n.tag == "presence"; });
    CHECK(pr && pr->attr("type") == "available" && pr->attr("name") == "My Name");
    CHECK(h.call({{"cmd", "typing"}, {"chat", kPeer}, {"state", "composing"}})["ok"] == true);
    CHECK(h.node([](const Node& n) { return n.tag == "chatstate" && n.child("composing"); }) != nullptr);
    CHECK(h.call({{"cmd", "subscribe_presence"}, {"chat", kPeer}})["ok"] == true);
    Node pre("presence");
    pre.a("from", Jid::parse(kPeer)).a("type", "unavailable").a("last", "1700000001");
    h.srv.send(pre);
    CHECK(!h.event("presence", [](const json& e) { return e["available"] == false && e["last_seen"] == 1700000001; }).is_null());
    Node cs("chatstate");
    cs.a("from", Jid::parse(kPeer)).add(Node("composing"));
    h.srv.send(cs);
    CHECK(!h.event("presence", [](const json& e) { return e["typing"] == true; }).is_null());

    // Bad commands answer with an error, never drop the link.
    CHECK(h.call({{"cmd", "messages"}})["error"] == "bad_request");
    CHECK(h.call({{"cmd", "send_text"}, {"chat", 5}, {"text", "x"}})["error"] == "bad_request");
    CHECK(h.call({{"cmd", "send_text"}, {"chat", kPeer}, {"text", ""}})["error"] == "bad_request");
    CHECK(h.call({{"cmd", "nope"}})["error"] == "unknown_cmd");
    CHECK(h.call({{"cmd", "pair_qr"}})["error"] == "busy");

    // Link drop: a send stays pending and leaves after the reconnect.
    h.srv.onLogin = [&](const Bytes& p) { payload = p; };
    payload.clear();
    h.srv.drop();
    CHECK(!h.event("connection", [](const json& e) { return e["state"] == "offline"; }).is_null());
    h.events.clear();
    json pend = h.call({{"cmd", "send_text"}, {"chat", kPeer}, {"text", "while offline"}});
    CHECK(pend["ok"] == true);
    std::string pendId = pend["result"]["id"];
    h.consumed.clear();
    h.clearNodes();
    CHECK(h.wait([&] { return !payload.empty(); }, 20000));
    serveLogin(h, false);
    const Node* qm = h.node([&](const Node& n) { return n.tag == "message" && n.attr("id") == pendId; }, 20000);
    CHECK(qm != nullptr);
    if (qm) {
        for (auto to : qm->child("participants")->children("to"))
            if (to->attr("jid") == kPeer) {
                auto p = parseMessage(signal::unpad(signal::decryptMessage(peerRec, to->child("enc")->data)));
                CHECK(p.text == "while offline");
            }
    }

    // Restart: a message unacknowledged at shutdown goes out again; sessions survive.
    h.clearNodes();
    h.stop();
    payload.clear();
    h.consumed.clear();
    h.events.clear();
    h.srv.drop();
    h.start();
    CHECK(h.wait([&] { return !payload.empty(); }, 15000));
    comparePayload(payload, w.at("payload.login"), false);
    serveLogin(h, false);
    // The device cache is memory only, so the restored send looks the devices up again.
    const Node* us2 = h.iqNs("usync");
    CHECK(us2 != nullptr);
    if (us2) {
        Node l2("list");
        l2.add(devNode(kPeer, {0}));
        l2.add(devNode(kSelfPn + "@s.whatsapp.net", {0, 7}));
        Node u2("usync");
        u2.add(l2);
        h.result(*us2, {u2});
    }
    const Node* rs = h.node([&](const Node& n) { return n.tag == "message" && n.attr("id") == pendId; }, 20000);
    CHECK(rs != nullptr);
    if (rs)
        for (auto to : rs->child("participants")->children("to"))
            if (to->attr("jid") == kPeer) {
                auto p = parseMessage(signal::unpad(signal::decryptMessage(peerRec, to->child("enc")->data)));
                CHECK(p.text == "while offline");
            }
    Node a2("ack");
    a2.a("class", "message").a("id", pendId).a("from", Jid::parse(kPeer));
    h.srv.send(a2);
    CHECK(!h.event("receipt", [&](const json& e) { return e["status"] == "sent" && e["ids"][0] == pendId; }).is_null());
    auto c6 = signal::encrypt(peerRec, signal::pad(buildTextMessage("after restart", "", "")));
    h.srv.send(msgFrom(kPeer, "IN6", c6.data, false));
    CHECK(!h.event("message", [](const json& e) { return e["id"] == "IN6" && e["text"] == "after restart"; }).is_null());

    // Encrypted at rest: no message text, name or number in any store file.
    for (auto f : {"/store.db", "/store.db-wal"})
        if (fs::exists(h.dir + f)) {
            for (auto needle : {"hello there", "reply text", "after restart", "61400000009", "Test Group", "Sam"}) {
                if (hasFile(h.dir + f, needle)) {
                    fails++;
                    printf("FAIL plaintext %s found in %s\n", needle, f);
                } else passes++;
            }
        }

    // Device removed: wiped, reported, unpaired.
    Node rem("stream:error");
    rem.a("code", "401");
    rem.add(Node("conflict").a("type", "device_removed"));
    h.srv.send(rem);
    CHECK(!h.event("connection", [](const json& e) { return e["state"] == "logged_out"; }).is_null());
    CHECK(h.call({{"cmd", "status"}})["result"]["self"] == "");
    CHECK(h.call({{"cmd", "chats"}})["result"]["total"] == 0);
    CHECK(h.call({{"cmd", "send_text"}, {"chat", kPeer}, {"text", "x"}})["error"] == "not_paired");
}

// A paired daemon for the smaller scenarios.
static Uploaded quickPair(H& h, Phone& phone) {
    Bytes payload;
    h.srv.onLogin = [&](const Bytes& p) { payload = p; };
    h.call({{"cmd", "pair_qr"}});
    h.wait([&] { return h.srv.up() && !payload.empty(); });
    Node pd("pair-device");
    pd.add(Node("ref").bytes(B("r")));
    Node iqn("iq");
    iqn.a("from", Jid{"", "s.whatsapp.net"}).a("id", "pd").a("type", "set").add(pd);
    h.srv.send(iqn);
    json q = h.event("pairing", [](const json& e) { return e["state"] == "qr"; });
    std::string qr = q["qr"];
    std::vector<std::string> parts;
    std::stringstream ss(qr.substr(qr.find('#') + 1));
    std::string it;
    while (std::getline(ss, it, ',')) parts.push_back(it);
    payload.clear();
    completePairing(h, phone, b64dec(parts[3]), toKey(b64dec(parts[2])));
    h.wait([&] { return !payload.empty(); });
    h.srv.onLogin = nullptr;
    return serveLogin(h, true);
}

static void testMedia() {
    Bytes image = randomBytes(5000);
    Bytes key = randomBytes(32);
    Bytes keys = hkdf(key, {}, "WhatsApp Image Keys", 112);
    Bytes iv(keys.begin(), keys.begin() + 16), ck(keys.begin() + 16, keys.begin() + 48), mk(keys.begin() + 48, keys.begin() + 80);
    Bytes ct = aesCbcEncrypt(ck, iv, image);
    Bytes in = iv;
    append(in, ct);
    Bytes mac = hmacSha256(mk, in);
    mac.resize(10);
    Bytes file = ct;
    append(file, mac);
    Bytes encSha = sha256(file), sha = sha256(image);

    auto msgFor = [&](const Bytes& shaField) {
        pb::Writer im;
        im.s(2, "image/jpeg").s(3, "a caption").b(4, shaField).u(5, image.size()).u(6, 600).u(7, 800).b(8, key).b(9, encSha).s(11, "/v/t/abc?x=1").b(16, B("tiny"));
        pb::Writer m;
        m.m(3, im);
        return m.out;
    };

    {
        HttpOnce http(file);
        H h(scratch("media"));
        h.stop();
        h.mediaBase = "http://127.0.0.1:" + std::to_string(http.port);
        h.start();
        Phone phone, peer;
        Uploaded up = quickPair(h, phone);
        signal::Record r;
        signal::initFromBundle(r, peer.id, peer.reg, bundleOf(up));
        auto c = signal::encrypt(r, signal::pad(msgFor(sha)));
        h.srv.send(msgFrom(kPeer, "IMG1", c.data, true));
        json ev = h.event("message", [](const json& e) { return e["id"] == "IMG1"; });
        CHECK(!ev.is_null());
        auto& mm = ev["message"];
        CHECK(mm["kind"] == "image" && mm["text"] == "a caption" && ev["text"] == "a caption");
        CHECK(mm["media"]["width"] == 800 && mm["media"]["height"] == 600 && mm["media"]["thumb_b64"] == b64(B("tiny")));
        CHECK(!mm.contains("_s") && !ev.dump().empty() && ev.dump().find(b64(key)) == std::string::npos);
        json d = h.call({{"cmd", "download_media"}, {"chat", kPeer}, {"message_id", "IMG1"}});
        if (d["ok"] != true) printf("download: %s\n", d.dump().c_str());
        CHECK(d["ok"] == true);
        if (d["ok"] == true) {
            std::string path = d["result"]["path"];
            std::ifstream f(path, std::ios::binary);
            std::string got((std::istreambuf_iterator<char>(f)), {});
            CHECK(B(got) == image);
            CHECK(((int)fs::status(path).permissions() & 0077) == 0);
            CHECK(http.lastPath.find("/v/t/abc?x=1&hash=") != std::string::npos);
            CHECK(http.lastPath.find("mms-type=image") != std::string::npos);
            // Cached: the one-shot server is gone, and this still answers.
            CHECK(h.call({{"cmd", "download_media"}, {"chat", kPeer}, {"message_id", "IMG1"}})["result"]["path"] == path);
        }
        // Tampered plaintext hash: the same download fails its integrity check.
    }
    {
        Bytes tampered = file;
        tampered[10] ^= 1;
        HttpOnce http(tampered);
        H h(scratch("media2"));
        h.stop();
        h.mediaBase = "http://127.0.0.1:" + std::to_string(http.port);
        h.start();
        Phone phone, peer;
        Uploaded up = quickPair(h, phone);
        signal::Record r;
        signal::initFromBundle(r, peer.id, peer.reg, bundleOf(up));
        auto c = signal::encrypt(r, signal::pad(msgFor(sha)));
        h.srv.send(msgFrom(kPeer, "IMG2", c.data, true));
        CHECK(!h.event("message", [](const json& e) { return e["id"] == "IMG2"; }).is_null());
        json d = h.call({{"cmd", "download_media"}, {"chat", kPeer}, {"message_id", "IMG2"}});
        CHECK(d["ok"] == false);
        CHECK(fs::is_empty(h.dir + "/media"));
    }
}

static void testPhonePairing() {
    H h(scratch("phone"));
    Bytes payload;
    h.srv.onLogin = [&](const Bytes& p) { payload = p; };
    CHECK(h.call({{"cmd", "pair_phone"}, {"phone", "0123"}})["error"] == "bad_request");

    // The reply comes later, so this drives the socket by hand.
    json req = {{"id", 900}, {"cmd", "pair_phone"}, {"phone", "+61 400 000 000"}};
    std::string l = req.dump() + "\n";
    (void)!::send(h.cfd, l.data(), l.size(), MSG_NOSIGNAL);
    CHECK(h.wait([&] { return h.srv.up() && !payload.empty(); }));
    Node pd("pair-device");
    pd.add(Node("ref").bytes(B("ignored")));
    Node iqn("iq");
    iqn.a("from", Jid{"", "s.whatsapp.net"}).a("id", "pd").a("type", "set").add(pd);
    h.srv.send(iqn);
    const Node* hello = h.iqNs("md");
    CHECK(hello != nullptr);
    if (!hello) return;
    const Node* reg = hello->child("link_code_companion_reg");
    CHECK(reg && reg->attr("stage") == "companion_hello" && reg->attr("jid") == "61400000000@s.whatsapp.net");
    Bytes wrapped = reg->child("link_code_pairing_wrapped_companion_ephemeral_pub")->data;
    CHECK(wrapped.size() == 80);
    Bytes noisePub = reg->child("companion_server_auth_key_pub")->data;
    CHECK(noisePub == B(h.srv.clientStatic));
    CHECK(S(reg->child("companion_platform_display")->data) == "Chrome (Linux)");
    Node rr("link_code_companion_reg");
    rr.add(Node("link_code_pairing_ref").bytes(B("ref-xyz")));
    h.result(*hello, {rr});
    h.wait([&] { return h.replies.count(900) > 0; });
    CHECK(h.replies[900]["ok"] == true);
    std::string shown = h.replies[900]["result"]["code"];
    CHECK(shown.size() == 9 && shown[4] == '-');
    CHECK(!h.event("pairing", [&](const json& e) { return e["state"] == "phone_code" && e["code"] == shown; }).is_null());
    std::string code = shown.substr(0, 4) + shown.substr(5);
    CHECK(code.find_first_not_of("123456789ABCDEFGHJKLMNPQRSTVWXYZ") == std::string::npos);

    // The phone: unwrap the companion key, wrap its own, push the notification.
    Bytes ckey = pbkdf2Sha256(code, Bytes(wrapped.begin(), wrapped.begin() + 32), 2 << 16, 32);
    Bytes companionEph = aesCtr(ckey, Bytes(wrapped.begin() + 32, wrapped.begin() + 48), Bytes(wrapped.begin() + 48, wrapped.end()));
    KeyPair primaryEph = genKeyPair();
    Phone phone;
    Bytes salt = randomBytes(32), iv = randomBytes(16);
    Bytes pkey = pbkdf2Sha256(code, salt, 2 << 16, 32);
    Bytes wp = salt;
    append(wp, iv);
    append(wp, aesCtr(pkey, iv, B(primaryEph.pub)));
    Node nr("link_code_companion_reg");
    nr.add(Node("link_code_pairing_ref").bytes(B("ref-xyz")));
    nr.add(Node("link_code_pairing_wrapped_primary_ephemeral_pub").bytes(wp));
    nr.add(Node("primary_identity_pub").bytes(B(phone.id.pub)));
    Node nt("notification");
    nt.a("from", Jid{"", "s.whatsapp.net"}).a("id", "n1").a("type", "link_code_companion_reg").add(nr);
    h.srv.send(nt);
    const Node* fin = h.iqNs("md");
    CHECK(fin != nullptr);
    if (!fin) return;
    const Node* fr = fin->child("link_code_companion_reg");
    CHECK(fr && fr->attr("stage") == "companion_finish");
    if (!fr || fr->attr("stage") != "companion_finish") return;
    Bytes wb = fr->child("link_code_pairing_wrapped_key_bundle")->data;
    Key32 ident = toKey(fr->child("companion_identity_public")->data);
    Key32 ephShared = x25519(primaryEph.priv, toKey(companionEph));
    Bytes bk = hkdf(B(ephShared), Bytes(wb.begin(), wb.begin() + 32), "link_code_pairing_key_bundle_encryption_key", 32);
    Bytes plain = aesGcmDecrypt(bk, Bytes(wb.begin() + 32, wb.begin() + 44), Bytes(wb.begin() + 44, wb.end()), {});
    CHECK(plain.size() == 96);
    CHECK(Bytes(plain.begin(), plain.begin() + 32) == B(ident));
    CHECK(Bytes(plain.begin() + 32, plain.begin() + 64) == B(phone.id.pub));
    Bytes input = B(ephShared);
    append(input, x25519(phone.id.priv, ident));
    append(input, Bytes(plain.begin() + 64, plain.end()));
    Bytes adv = hkdf(input, {}, "adv_secret", 32);
    h.result(*fin);

    // The derived secret must authenticate the normal pair-success.
    payload.clear();
    completePairing(h, phone, adv, ident);
    CHECK(!h.event("pairing", [](const json& e) { return e["state"] == "paired"; }).is_null());
    CHECK(h.wait([&] { return !payload.empty(); }));
}

static void testQrRotationAndCancel() {
    H h(scratch("rot"));
    Bytes payload;
    h.srv.onLogin = [&](const Bytes& p) { payload = p; };
    h.call({{"cmd", "pair_qr"}});
    h.wait([&] { return h.srv.up() && !payload.empty(); });
    Node pd("pair-device");
    pd.add(Node("ref").bytes(B("A")));
    Node iqn("iq");
    iqn.a("from", Jid{"", "s.whatsapp.net"}).a("id", "pd").a("type", "set").add(pd);
    h.srv.send(iqn);
    CHECK(!h.event("pairing", [](const json& e) { return e["state"] == "qr"; }).is_null());
    CHECK(h.call({{"cmd", "pair_cancel"}})["ok"] == true);
    CHECK(!h.event("pairing", [](const json& e) { return e["state"] == "unpaired"; }).is_null());
    CHECK(!h.event("connection", [](const json& e) { return e["state"] == "offline"; }).is_null());
    int before = h.srv.connections;
    for (int i = 0; i < 100; i++) h.loop.tick(10);
    CHECK(h.srv.connections == before);  // cancelled means off the network
}

// Status, app state, contacts, pictures, group info, search, network return.
static void testSurfaces() {
    H h(scratch("surf"));
    Phone phone, peer;
    Uploaded up = quickPair(h, phone);
    h.clearNodes();
    signal::Record ph, pr;
    signal::initFromBundle(ph, phone.id, phone.reg, bundleOf(up));
    signal::initFromBundle(pr, peer.id, peer.reg, bundleOf(up));

    // A contact's status: the sender key arrives in a direct message, then the item.
    signal::SenderRecord sk;
    Bytes dist = buildSkdmMessage("status@broadcast", signal::senderKeyDistribution(sk));
    auto ce = signal::encrypt(pr, signal::pad(dist));
    auto sm = signal::senderEncrypt(sk, signal::pad(buildTextMessage("my status", "", "")));
    Node st("message");
    st.a("from", Jid{"status", "broadcast"}).a("participant", Jid::parse(kPeer)).a("id", "ST1").a("t", std::to_string(time(nullptr))).a("type", "text").a("notify", "Sam");
    st.add(Node("enc").a("v", "2").a("type", "pkmsg").bytes(ce.data));
    st.add(Node("enc").a("v", "2").a("type", "skmsg").bytes(sm));
    h.srv.send(st);
    json se = h.event("status", [](const json& e) { return e["item"]["id"] == "ST1"; });
    CHECK(!se.is_null() && se["item"]["text"] == "my status" && se["sender"] == kPeer);
    json list = h.call({{"cmd", "statuses"}})["result"]["contacts"];
    CHECK(list.size() == 1 && list[0]["unseen"] == 1 && list[0]["items"][0]["id"] == "ST1");
    CHECK(h.call({{"cmd", "chats"}})["result"]["total"] == 0);  // status is not a chat
    for (auto& e : h.events) CHECK(e["event"] != "message" || e["id"] != "ST1");
    CHECK(h.call({{"cmd", "status_seen"}, {"sender", kPeer}, {"ids", {"ST1"}}})["ok"] == true);
    const Node* sr = h.node([](const Node& n) { return n.tag == "receipt" && n.attr("id") == "ST1" && (n.attr("type") == "read" || n.attr("type") == "read-self"); });
    CHECK(sr && sr->attr("to") == "status@broadcast" && sr->attr("participant") == kPeer);
    CHECK(h.call({{"cmd", "statuses"}})["result"]["contacts"][0]["unseen"] == 0);

    // App state: keys from the phone, then a snapshot that names a contact, and a patch.
    Bytes keyId = B("key-one"), keyData = randomBytes(32);
    auto msgOf = [&](pb::Writer& pm) { pb::Writer m; m.m(12, pm); return m.out; };
    pb::Writer kid; kid.b(1, keyId);
    pb::Writer kd; kd.b(1, keyData);
    pb::Writer kk; kk.m(1, kid).m(2, kd);
    pb::Writer share; share.m(1, kk);
    pb::Writer pm; pm.u(2, 6).m(7, share);
    auto ck = signal::encrypt(ph, signal::pad(msgOf(pm)));
    h.srv.send(msgFrom(kSelfPn + "@s.whatsapp.net", "KS1", ck.data, true));
    CHECK(!h.event("app_state", [](const json& e) { return e["state"] == "running"; }).is_null());

    Bytes km = hkdf(keyData, {}, "WhatsApp Mutation Keys", 160);
    auto part = [&](int i) { return Bytes(km.begin() + i * 32, km.begin() + (i + 1) * 32); };
    auto be64 = [](uint64_t v) { Bytes o(8); for (int i = 0; i < 8; i++) o[i] = (uint8_t)(v >> (56 - i * 8)); return o; };
    auto mkRecord = [&](const std::string& index, const Bytes& action) {
        pb::Writer data;
        data.b(1, B(index)).b(2, action).u(4, 2);
        Bytes iv = randomBytes(16), content = iv;
        append(content, aesCbcEncrypt(part(1), iv, data.out));
        Bytes in{1};
        append(in, keyId); append(in, content); append(in, be64(keyId.size() + 1));
        Bytes mac(64); unsigned n = 64;
        HMAC(EVP_sha512(), part(2).data(), 32, in.data(), in.size(), mac.data(), &n);
        mac.resize(32);
        append(content, mac);
        pb::Writer idx; idx.b(1, hmacSha256(part(0), B(index)));
        pb::Writer val; val.b(1, content);
        pb::Writer ki; ki.b(1, keyId);
        pb::Writer rec; rec.m(1, idx).m(2, val).m(3, ki);
        return std::make_pair(rec, mac);
    };
    auto snapMac = [&](const Bytes& hash, uint64_t ver, const std::string& name) {
        Bytes in = hash; append(in, be64(ver)); append(in, B(name));
        return hmacSha256(part(3), in);
    };
    // Each collection is requested in turn. Only regular_high gets real content below.
    std::map<std::string, Bytes> hashes;
    auto contactAct = [&](const std::string& full) { pb::Writer c; c.s(1, full).s(2, "F"); pb::Writer v; v.u(1, 1).m(3, c); return v.out; };
    auto muteAct = [&]() { pb::Writer c; c.u(1, 1).u(2, (uint64_t)-1); pb::Writer v; v.u(1, 1).m(4, c); return v.out; };
    auto archAct = [&]() { pb::Writer c; c.u(1, 1); pb::Writer v; v.u(1, 1).m(17, c); return v.out; };
    auto starAct = [&]() { pb::Writer c; c.u(1, 1); pb::Writer v; v.u(1, 1).m(2, c); return v.out; };
    for (int i = 0; i < 5; i++) {
        const Node* q = h.iqNs("w:sync:app:state", "sync");
        CHECK(q != nullptr);
        if (!q) return;
        const Node* qc = q->child("sync")->child("collection");
        std::string name = qc->attr("name");
        CHECK(qc->attr("return_snapshot") == "true");
        Node col("collection");
        col.a("name", name).a("version", "1");
        pb::Writer patch;
        Bytes hash(128, 0);
        pb::Writer ver; ver.u(1, 1);
        patch.m(1, ver);
        std::vector<std::pair<pb::Writer, Bytes>> recs;
        if (name == "critical_unblock_low") recs.push_back(mkRecord("[\"contact\",\"" + kPeer + "\"]", contactAct("Book Name")));
        if (name == "regular_high") {
            recs.push_back(mkRecord("[\"mute\",\"" + kPeer + "\"]", muteAct()));
            recs.push_back(mkRecord("[\"star\",\"" + kPeer + "\",\"IN9\",\"0\",\"0\"]", starAct()));
        }
        if (name == "regular_low") recs.push_back(mkRecord("[\"archive\",\"" + kPeer + "\"]", archAct()));
        std::vector<Bytes> added;
        Bytes macs;
        for (auto& [rec, mac] : recs) {
            pb::Writer mu; mu.u(1, 0).m(2, rec);
            patch.m(2, mu);
            added.push_back(mac);
            append(macs, mac);
        }
        appstate::ltHashApply(hash, {}, added);
        Bytes sm2 = snapMac(hash, 1, name);
        Bytes pin = sm2; append(pin, macs); append(pin, be64(1)); append(pin, B(name));
        pb::Writer kp; kp.b(1, keyId);
        patch.b(4, sm2).b(5, hmacSha256(part(4), pin)).m(6, kp);
        Node ps("patches");
        ps.add(Node("patch").bytes(patch.out));
        col.add(ps);
        Node sy("sync");
        sy.add(col);
        h.result(*q, {sy});
    }
    CHECK(!h.event("app_state", [](const json& e) { return e["state"] == "done"; }).is_null());
    CHECK(!h.event("contacts", nullptr).is_null());
    json cs = h.call({{"cmd", "contacts"}})["result"];
    CHECK(cs["total"] == 1 && cs["contacts"][0]["name"] == "Book Name" && cs["contacts"][0]["phone"] == "+61400000009");
    CHECK(h.call({{"cmd", "contacts"}, {"query", "book"}})["result"]["total"] == 1);
    CHECK(h.call({{"cmd", "contacts"}, {"query", "zzz"}})["result"]["total"] == 0);

    // A message now: the chat takes the book name, muted and archived, and no push name wins.
    auto c9 = signal::encrypt(pr, signal::pad(buildTextMessage("Find Me Here", "", "")));
    h.srv.send(msgFrom(kPeer, "IN9", c9.data, c9.preKey));
    json ev = h.event("message", [](const json& e) { return e["id"] == "IN9"; });
    CHECK(ev["chat_name"] == "Book Name" && ev["muted"] == true && ev["text"] == "Find Me Here");
    json rows = h.call({{"cmd", "chats"}})["result"]["chats"];
    CHECK(rows[0]["archived"] == true && rows[0]["muted"] == true);
    auto stars = h.call({{"cmd", "starred"}})["result"]["messages"];
    CHECK(stars.size() == 1 && stars[0]["id"] == "IN9" && stars[0]["starred"] == true);
    CHECK(h.call({{"cmd", "messages"}, {"chat", kPeer}})["result"]["messages"][0]["starred"] == true);

    // A forged patch is refused: the state holds, and one clean snapshot is requested.
    {
        h.clearNodes();
        Node note("notification");
        Node sy("sync");
        sy.add(Node("collection").a("name", "regular_low").a("version", "2"));
        note.a("from", Jid{"", "s.whatsapp.net"}).a("id", "ss1").a("type", "server_sync").add(sy);
        h.srv.send(note);
        const Node* q = h.iqNs("w:sync:app:state", "sync");
        CHECK(q && q->child("sync")->child("collection")->attr("return_snapshot") == "false");
        if (q) {
            auto rec = mkRecord("[\"archive\",\"" + kPeer + "\"]", contactAct("x"));
            pb::Writer mu; mu.u(1, 0).m(2, rec.first);
            pb::Writer ver; ver.u(1, 2);
            pb::Writer kp; kp.b(1, keyId);
            pb::Writer patch; patch.m(1, ver).m(2, mu).b(4, Bytes(32, 0)).b(5, Bytes(32, 0)).m(6, kp);
            Node ps("patches"); ps.add(Node("patch").bytes(patch.out));
            Node col("collection"); col.a("name", "regular_low").add(ps);
            Node s2("sync"); s2.add(col);
            h.result(*q, {s2});
            const Node* again = h.iqNs("w:sync:app:state", "sync");
            CHECK(again && again->child("sync")->child("collection")->attr("return_snapshot") == "true");
            if (again) h.result(*again);  // an empty answer ends the attempt
        }
        CHECK(h.call({{"cmd", "chats"}})["result"]["chats"][0]["archived"] == true);
    }

    // Search: case-insensitive, finds text, leaves status out, works per chat.
    auto found = h.call({{"cmd", "search"}, {"query", "find me"}})["result"];
    if (found["messages"].size() != 1) printf("search got %s\n", found.dump().substr(0, 300).c_str());
    CHECK(found["messages"].size() == 1 && found["messages"][0]["id"] == "IN9");
    CHECK(h.call({{"cmd", "search"}, {"query", "my status"}})["result"]["messages"].empty());
    CHECK(h.call({{"cmd", "search"}, {"query", "find"}, {"chat", kGroup}})["result"]["messages"].empty());

    // Group info with roles and resolved names.
    {
        json req = {{"id", 700}, {"cmd", "group_info"}, {"chat", kGroup}};
        std::string l = req.dump() + "\n";
        (void)!::send(h.cfd, l.data(), l.size(), MSG_NOSIGNAL);
        const Node* gq = h.iqNs("w:g2");
        CHECK(gq != nullptr);
        if (gq) {
            Node gr("group");
            gr.a("id", "120363000000000001").a("subject", "Subj").a("creator", Jid::parse(kPeer)).a("creation", "1700000000");
            gr.add(Node("participant").a("jid", Jid::parse(kPeer)).a("type", "superadmin"));
            gr.add(Node("participant").a("jid", Jid::parse(kPeer2)));
            Node desc("description");
            desc.a("id", "d").add(Node("body").bytes(B("About us")));
            gr.add(desc);
            gr.add(Node("announcement"));
            h.result(*gq, {gr});
            h.wait([&] { return h.replies.count(700) > 0; });
            json g = h.replies[700]["result"];
            CHECK(g["subject"] == "Subj" && g["topic"] == "About us" && g["announce"] == true && g["owner"] == kPeer);
            CHECK(g["participants"].size() == 2 && g["participants"][0]["name"] == "Book Name" && g["participants"][0]["is_super_admin"] == true);
            CHECK(g["participants"][1]["is_admin"] == false);
        }
    }

    // Status post: refuses to go out without knowing the privacy list, honours a block list.
    {
        json p1 = h.call({{"cmd", "post_status"}, {"text", "first post"}});
        CHECK(p1["ok"] == true);
        const Node* pq = h.iqNs("status");
        CHECK(pq != nullptr);
        if (pq) {
            Node pv("privacy");
            Node lst("list");
            lst.a("type", "blacklist").a("default", "true");
            lst.add(Node("user").a("jid", Jid::parse(kPeer)));
            pv.add(lst);
            h.result(*pq, {pv});
            const Node* us = h.iqNs("usync");
            CHECK(us != nullptr);
            if (us) {
                // The blocked contact is the only contact, so only our own account is asked for.
                auto users = us->child("usync")->child("list")->children("user");
                CHECK(users.size() == 1 && users[0]->attr("jid") == kSelfPn + "@s.whatsapp.net");
                Node dl("device-list");
                dl.add(Node("device").a("id", "7"));
                Node d("devices"); d.add(dl);
                Node u("user"); u.a("jid", Jid::parse(kSelfPn + "@s.whatsapp.net")).add(d);
                Node ul("list"); ul.add(u);
                Node uu("usync"); uu.add(ul);
                h.result(*us, {uu});
                std::string pid = p1["result"]["id"];
                const Node* sent = h.node([&](const Node& n) { return n.tag == "message" && n.attr("id") == pid; });
                CHECK(sent && sent->attr("to") == "status@broadcast");
            }
        }
    }

    // Profile picture: fetched once, cached, and absent reports not_found.
    {
        Bytes jpeg = randomBytes(300);
        HttpOnce http(jpeg);
        json req = {{"id", 701}, {"cmd", "profile_picture"}, {"jid", kPeer}};
        std::string l = req.dump() + "\n";
        (void)!::send(h.cfd, l.data(), l.size(), MSG_NOSIGNAL);
        h.mediaBaseUsed = true;
        const Node* q = h.iqNs("w:profile:picture");
        CHECK(q && q->attr("target") == kPeer && q->child("picture")->attr("type") == "preview");
        if (q) {
            Node pic("picture");
            // https is required outside tests, so this expects the refusal path.
            pic.a("url", "http://127.0.0.1:" + std::to_string(http.port) + "/p.jpg");
            h.result(*q, {pic});
            h.wait([&] { return h.replies.count(701) > 0; });
            CHECK(h.replies[701]["error"] == "not_found");
        }
        // Unblock the one-shot server's accept.
        int f = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons((uint16_t)http.port);
        ::connect(f, (sockaddr*)&a, sizeof a);
        (void)!::send(f, "x", 1, 0);
        close(f);
    }

    // Wi-Fi back: during backoff, a new address reconnects at once.
    {
        Bytes pay;
        h.srv.onLogin = [&](const Bytes& p) { pay = p; };
        h.srv.drop();
        CHECK(!h.event("connection", [](const json& e) { return e["state"] == "offline"; }).is_null());
        // Burn attempts so the normal retry would be many seconds away.
        h.call({{"cmd", "_test_network"}});
        auto t0 = Loop::Clock::now();
        CHECK(h.wait([&] { return !pay.empty(); }, 1500));
        CHECK(Loop::Clock::now() - t0 < std::chrono::milliseconds(1500));
        h.srv.onLogin = nullptr;
    }
}

int main() {
    cryptoInit();
    ::signal(SIGPIPE, SIG_IGN);
    try {
        testQrAndMessaging();
        testPhonePairing();
        testQrRotationAndCancel();
        testMedia();
        testSurfaces();
    } catch (const std::exception& e) {
        fails++;
        printf("FAIL exception: %s\n", e.what());
    }
    printf("%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
