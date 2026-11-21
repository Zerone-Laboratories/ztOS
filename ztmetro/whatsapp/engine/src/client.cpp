// The linked-device client: connection, pairing, sessions, send and receive, and the
// mapping of all that onto IPC.md. Runs entirely on the Loop thread; only DNS and HTTP
// downloads run on short-lived worker threads.
#include <curl/curl.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <deque>
#include <fstream>
#include <set>
#include <thread>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/socket.h>

#include "appstate.h"
#include "engine.h"
#include "node.h"
#include "noise.h"
#include "pb.h"
#include "sigproto.h"
#include "store.h"
#include "wamsg.h"
#include "ws.h"

namespace zw {

namespace {

// The web client version the server expects. A stale value gets connect failure 405.
// Override without a rebuild by putting "a.b.c" in <data>/wa-version.
const char* const kDefaultVersion = "2.3000.1045649367";
const Jid kServerJid{"", "s.whatsapp.net"};

void logLine(const std::string& s) { fprintf(stderr, "ztwhatsappd: %s\n", s.c_str()); }

Bytes be32(uint32_t v) { return {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v}; }
Bytes be24(uint32_t v) { return {(uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v}; }
int64_t now() { return (int64_t)time(nullptr); }

struct Device {
    KeyPair noise, identity, signedPre;
    Bytes signedSig, adv, account;
    uint32_t regId = 0, signedId = 1, nextPreKey = 1;
    bool preKeysUploaded = false;
    std::string jid, lid, pushName;

    json toJson() const {
        auto kp = [](const KeyPair& k) { return json{b64(B(k.pub)), b64(B(k.priv))}; };
        return {{"noise", kp(noise)}, {"identity", kp(identity)}, {"spk", kp(signedPre)}, {"sig", b64(signedSig)},
                {"adv", b64(adv)}, {"account", b64(account)}, {"reg", regId}, {"sid", signedId},
                {"next", nextPreKey}, {"up", preKeysUploaded}, {"jid", jid}, {"lid", lid}, {"push", pushName}};
    }
    static Device fromJson(const json& j) {
        auto kp = [](const json& a) {
            KeyPair k;
            k.pub = toKey(b64dec(a[0].get<std::string>()));
            k.priv = toKey(b64dec(a[1].get<std::string>()));
            return k;
        };
        Device d;
        d.noise = kp(j["noise"]);
        d.identity = kp(j["identity"]);
        d.signedPre = kp(j["spk"]);
        d.signedSig = b64dec(j["sig"].get<std::string>());
        d.adv = b64dec(j["adv"].get<std::string>());
        d.account = b64dec(j["account"].get<std::string>());
        d.regId = j["reg"];
        d.signedId = j["sid"];
        d.nextPreKey = j["next"];
        d.preKeysUploaded = j["up"];
        d.jid = j["jid"];
        d.lid = j["lid"];
        d.pushName = j["push"];
        return d;
    }
    static Device fresh() {
        Device d;
        d.noise = genKeyPair();
        d.identity = genKeyPair();
        d.signedPre = genKeyPair();
        Bytes typed{5};
        append(typed, d.signedPre.pub);
        auto s = xedSign(d.identity.priv, typed);
        d.signedSig.assign(s.begin(), s.end());
        d.adv = randomBytes(32);
        d.regId = randomU32() % 16380 + 1;
        return d;
    }
};

class RealEngine : public Engine {
public:
    RealEngine(Loop& l, IpcServer& ipc, const RealConfig& cfg)
        : loop_(l), ipc_(ipc), cfg_(cfg), store_(cfg.dataDir), ws_(l) {
        loadDevice();
        std::ifstream vf(cfg_.dataDir + "/wa-version");
        std::string v;
        version_ = (vf >> v) ? v : kDefaultVersion;
        ws_.onOpen = [this] { wsOpen(); };
        ws_.onMessage = [this](Bytes&& m) { wsMessage(std::move(m)); };
        ws_.onClose = [this](const std::string& why) { wsClosed(why); };
        pairState_ = paired() ? "paired" : "unpaired";
        loadStarred();
        restoreQueueOnly_ = true;
        watchNetwork();
        // An unpaired daemon stays off the network until a client asks to pair.
        if (paired()) connect();
    }

    ~RealEngine() override {
        *alive_ = false;
        gen_++;
        ws_.close();
        if (nlFd_ >= 0) {
            loop_.del(nlFd_);
            close(nlFd_);
        }
    }

    std::vector<json> greeting() override { return {connectionEv(), pairingEv()}; }
    void command(const std::string& cmd, const json& r, Reply rp) override;

    // Exposed for the client payload fixture test through a static below.
    Bytes clientPayload() const;

private:
    // Timers and worker results may outlive this object, so each checks a shared flag.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    uint64_t later(int ms, std::function<void()> fn) {
        return loop_.after(ms, [a = alive_, fn = std::move(fn)] {
            if (*a) fn();
        });
    }
    void posted(std::function<void()> fn) {
        loop_.post([a = alive_, fn = std::move(fn)] {
            if (*a) fn();
        });
    }

    // Connection
    void connect();
    void disconnect();
    void wsOpen();
    void wsMessage(Bytes&& m);
    void wsClosed(const std::string& why);
    void scheduleReconnect();
    void setConn(const std::string& s, const std::string& detail = "");
    void handshakeStep(const Bytes& frame);
    void sendNode(const Node& n);
    void sendFrame(const Bytes& payload);
    void keepalive(uint64_t gen);
    using IqCb = std::function<void(const Node*)>;  // null on timeout or disconnect
    void iq(const std::string& xmlns, const std::string& type, std::vector<Node> content, IqCb cb, Jid to = kServerJid, Jid target = {});
    std::string nextId();
    void clearIqs();
    json connectionEv() const { return {{"event", "connection"}, {"state", conn_}, {"detail", connDetail_}}; }
    json pairingEv() const;
    void emitPairing() { ipc_.broadcast(pairingEv()); }

    // Dispatch
    void handleNode(const Node& n);
    void handleIq(const Node& n);
    void handleSuccess(const Node& n);
    void handleFailure(const Node& n);
    void handleStreamError(const Node& n);
    void handleNotification(const Node& n);
    void handleMessage(const Node& n);
    void handleReceipt(const Node& n);
    void handleAck(const Node& n);
    void handlePresence(const Node& n);
    void handleChatstate(const Node& n);
    void sendAck(const Node& n, int error = 0);

    // Pairing
    void pairDevice(const Node& n);
    void pairSuccess(const Node& n);
    void pairError(const std::string& id, int code, const std::string& text);
    void showQr();
    void endPairing(const std::string& error);
    void startPhonePairing();
    void codePairNotification(const Node& n);
    void wipe();

    // Keys
    void loadDevice();
    void saveDevice();
    void afterLogin();
    void uploadPreKeys(int count);
    Node signedPreKeyNode();
    Node preKeyNode(uint32_t id, const Key32& pub);
    uint32_t genPreKey();
    signal::Keys sigKeys();

    // Addressing
    void mapLid(const Jid& pn, const Jid& lid);
    Jid canon(const Jid& j);
    std::string sessionKey(const Jid& j);
    signal::Record loadSession(const std::string& k);
    void saveSession(const std::string& k, const signal::Record& r, const Jid& j);
    std::string nameOf(const Jid& bare);
    void setName(const Jid& bare, const std::string& name);
    bool isSelf(const Jid& j);

    // Receive
    void processPlain(const Node& stanza, const Bytes& plain, const Jid& chat, const Jid& sender, bool fromMe, int64_t ts,
                      const std::string& id, bool live, const std::string& pushName);
    void storeIncoming(const Jid& chat, const Jid& sender, bool fromMe, int64_t ts, const std::string& id, ParsedMsg& p,
                       bool live, bool replaceUndecryptable, const std::string& pushName);
    void touchChat(const Jid& chat, const std::string& nameHint);
    void sendRetryReceipt(const Node& n, bool forceKeys);
    void sendMessageReceipt(const Node& n, bool fromMe);
    void fetchGroup(const Jid& g, std::function<void(bool)> cb = nullptr);

    // Send
    struct Pending {
        Jid chat;
        std::string id;
        Bytes proto;
        int64_t ts;
    };
    void enqueueSend(Pending p);
    std::vector<std::string> queueIds();
    void setQueueIds(const std::vector<std::string>& ids);
    void dequeue(const std::string& id);
    void restoreQueue();
    void pumpSend();
    void sendOne(const Pending& p);
    void devicesFor(const std::vector<Jid>& users, std::function<void(std::optional<std::vector<Jid>>)> cb);
    void ensureSessions(const std::vector<Jid>& devices, std::function<void()> cb);
    bool applyBundle(const Jid& device, const Node& userNode);
    void finishSend(const Pending& p, const std::vector<Jid>& devices);
    Node encryptFor(const Jid& device, const Bytes& plain, bool* preKey);
    void markStatus(const std::string& chat, const std::vector<std::string>& ids, const std::string& status, const std::string& sender,
                    const std::string& error = "");
    void handleRetry(const Node& n);
    void rememberOutgoing(const Pending& p);

    // History and media
    void onHistory(const pb::Msg& notif, const std::string& msgId);
    void importHistory(const Bytes& inflated);
    void importBatch();
    void ensureMediaConn(std::function<void(bool)> cb);
    void cmdDownload(const json& r, Reply rp);
    void markRead(const json& r, Reply rp);
    void reportProbe(bool ok, const std::string& what);

    // Network watch: the kernel says when an address appears, so nothing polls.
    void watchNetwork();
    void networkChanged();
    int nlFd_ = -1;
    uint64_t netT_ = 0;
    bool restoreQueueOnly_ = false;

    // App state
    void syncAppState(std::vector<std::string> names);
    void appStateNext();
    void appStateFetch(const std::string& name, bool snapshot);
    void appStateReply(const std::string& name, const Node* r, bool snapshot);
    void appStateApply(const std::string& name, const Bytes& snapshot, const std::vector<std::pair<Bytes, std::vector<pb::Msg>>>& patches, bool hasMore);
    void applyMutation(const appstate::Mutation& m);
    void setChatFlag(const Jid& chat, const char* flag, const json& value);
    void overlayFlags(json& chat);
    void loadStarred();
    void saveStarred();
    std::deque<std::string> asQueue_;
    bool asRunning_ = false, asContacts_ = false;
    std::set<std::string> asRetried_, starred_;

    // Status and other surfaces
    void storeStatus(const Jid& sender, bool fromMe, int64_t ts, const std::string& id, ParsedMsg& p, bool live, const std::string& push);
    json statusList();
    void cmdStatusSeen(const json& r, Reply rp);
    void cmdPostStatus(const json& r, Reply rp);
    void cmdContacts(const json& r, Reply rp);
    void cmdPicture(const json& r, Reply rp);
    void cmdGroupInfo(const json& r, Reply rp);
    void cmdSearch(const json& r, Reply rp);
    void cmdStarred(const json& r, Reply rp);
    void statusAudience(std::function<void(std::optional<std::vector<Jid>>)> cb);
    json groupJson(const std::string& gid);

    Loop& loop_;
    IpcServer& ipc_;
    RealConfig cfg_;
    Store store_;
    WebSocket ws_;
    Device dev_;
    std::string version_;

    bool paired() const { return !dev_.jid.empty(); }
    Jid self() const { return Jid::parse(dev_.jid); }
    Jid selfLid() const { return Jid::parse(dev_.lid); }

    std::string conn_ = "offline", connDetail_;
    std::string pairState_ = "unpaired", pairError_, pairCode_;
    uint64_t gen_ = 0;
    bool wanted_ = false, loggedIn_ = false;
    int attempts_ = 0;
    uint64_t reconnectT_ = 0, keepT_ = 0;
    int64_t lastPong_ = 0;

    // Handshake
    enum class Hs { None, WaitHello, Done } hs_ = Hs::None;
    NoiseState noise_;
    NoiseTransport tp_;
    KeyPair eph_;
    Bytes header_, frameBuf_;
    bool sentHeader_ = false;

    std::map<std::string, std::pair<IqCb, uint64_t>> iqs_;
    uint64_t idCounter_ = 0;
    std::string idPrefix_ = hex(randomBytes(4));

    // Pairing
    std::deque<Bytes> qrRefs_;
    std::string currentQr_;
    int qrExpires_ = 0;
    uint64_t qrT_ = 0;
    bool qrFlow_ = false;
    bool refsReady_ = false;
    std::optional<std::string> phoneWanted_;
    std::optional<Reply> phoneReply_;
    struct LinkCache {
        Jid jid;
        KeyPair kp;
        std::string code;
        Bytes ref;
    };
    std::optional<LinkCache> link_;

    // Runtime state
    bool active_ = false;
    bool readReceipts_ = false;  // conservative until the privacy query answers
    std::map<std::string, std::vector<Jid>> deviceCache_;
    struct GroupInfo {
        std::vector<Jid> participants;
        bool lid = false;
        json info;
    };
    std::map<std::string, GroupInfo> groups_;
    std::set<std::string> groupFetching_;
    std::map<std::string, int> retryCount_;
    std::deque<Pending> sendQ_;
    bool sending_ = false;
    std::map<std::string, std::string> inFlight_;  // message id -> chat, awaiting the server ack

    // History import queue, drained in small batches so the socket keeps being served.
    struct HistItem {
        Jid chat;
        std::string id;
        Jid sender;
        bool fromMe;
        int64_t ts;
        Bytes proto;
        int status;
        std::string push;
    };
    std::deque<HistItem> hist_;
    bool histRunning_ = false;
    int histProgress_ = 0, histChats_ = 0, histMsgs_ = 0;
    std::set<std::string> histTouched_;

    struct MediaConn {
        std::vector<std::string> hosts;
        int64_t expires = 0;
    } media_;
};

// ---------------------------------------------------------------- device

void RealEngine::loadDevice() {
    if (auto d = store_.get("device", "self")) {
        dev_ = Device::fromJson(json::parse(S(*d)));
    } else {
        dev_ = Device::fresh();
        saveDevice();
    }
}

void RealEngine::saveDevice() { store_.put("device", "self", B(dev_.toJson().dump())); }

json RealEngine::pairingEv() const {
    json j = {{"event", "pairing"}, {"state", pairState_}, {"qr", ""}, {"expires_in", 0}, {"code", ""}, {"error", pairError_}};
    if (pairState_ == "qr") {
        j["qr"] = currentQr_;
        j["expires_in"] = qrExpires_;
    }
    if (pairState_ == "phone_code") j["code"] = pairCode_;
    if (pairState_ == "paired") j["self"] = dev_.jid;
    return j;
}

Bytes RealEngine::clientPayload() const {
    pb::Writer ver;
    int a = 0, b = 0, c = 0;
    sscanf(version_.c_str(), "%d.%d.%d", &a, &b, &c);
    ver.u(1, (uint32_t)a).u(2, (uint32_t)b).u(3, (uint32_t)c);
    pb::Writer ua;
    ua.u(1, 14).m(2, ver).s(3, "000").s(4, "000").s(5, "1.0.0").s(6, "").s(7, "Desktop").s(8, "1.0.0");
    ua.u(10, 0).s(11, "en").s(12, "US");
    pb::Writer web;
    web.u(4, 0);
    pb::Writer w;
    if (paired()) {
        Jid me = self();
        w.u(1, std::stoull(me.user)).u(3, 1).m(5, ua).m(6, web).u(12, 1).u(13, 1);
        w.u(18, me.device).u(24, 1).u(33, 1).u(41, 1);
        return w.out;
    }
    pb::Writer hist;
    hist.u(3, 10240).u(4, 1).u(6, 1).u(7, 1).u(8, 1).u(9, 1).u(10, 1).u(11, 1).u(12, 1).u(14, 1).u(15, 1).u(19, 60).u(21, 1).u(22, 1);
    pb::Writer pv;
    pv.u(1, 1).u(2, 0).u(3, 0);
    pb::Writer props;
    props.s(1, "ztOS").m(2, pv).u(3, 0).u(4, 0).m(5, hist);
    Bytes sid = be32(dev_.signedId);
    pb::Writer reg;
    reg.b(1, be32(dev_.regId)).b(2, Bytes{5}).b(3, B(dev_.identity.pub)).b(4, Bytes(sid.begin() + 1, sid.end()));
    reg.b(5, B(dev_.signedPre.pub)).b(6, dev_.signedSig).b(7, md5(B(version_))).b(8, props.out);
    w.u(3, 0).m(5, ua).m(6, web).u(12, 1).u(13, 1).m(19, reg).u(33, 0);
    return w.out;
}

// ---------------------------------------------------------------- connection

void RealEngine::setConn(const std::string& s, const std::string& detail) {
    if (conn_ == s && connDetail_ == detail) return;
    conn_ = s;
    connDetail_ = detail;
    ipc_.broadcast(connectionEv());
}

void RealEngine::connect() {
    loop_.cancel(reconnectT_);
    reconnectT_ = 0;
    wanted_ = true;
    gen_++;
    hs_ = Hs::None;
    loggedIn_ = false;
    refsReady_ = false;
    frameBuf_.clear();
    sentHeader_ = false;
    setConn("connecting");
    WebSocket::Opts o;
    o.host = cfg_.host;
    o.port = cfg_.port;
    o.tls = cfg_.tls;
    ws_.connect(o);
}

void RealEngine::disconnect() {
    wanted_ = false;
    gen_++;
    ws_.close();
    hs_ = Hs::None;
    loggedIn_ = false;
    loop_.cancel(reconnectT_);
    reconnectT_ = 0;
    loop_.cancel(keepT_);
    keepT_ = 0;
    loop_.cancel(qrT_);
    qrT_ = 0;
    clearIqs();
    setConn("offline");
}

void RealEngine::clearIqs() {
    auto all = std::move(iqs_);
    iqs_.clear();
    for (auto& [_, v] : all) {
        loop_.cancel(v.second);
        if (v.first) v.first(nullptr);
    }
}

void RealEngine::wsClosed(const std::string& why) {
    bool wasLogged = loggedIn_;
    loggedIn_ = false;
    gen_++;
    hs_ = Hs::None;
    loop_.cancel(keepT_);
    keepT_ = 0;
    clearIqs();
    logLine("connection closed: " + why.substr(0, 40));
    if (cfg_.probeOnly) {
        reportProbe(false, "connection closed before any QR ref (" + why + ")");
        return;
    }
    if (!wanted_) return;
    if (qrFlow_ && !paired()) {
        endPairing("connection lost");
        return;
    }
    if (wasLogged) attempts_ = 0;
    setConn("offline", "retrying");
    scheduleReconnect();
}

void RealEngine::scheduleReconnect() {
    if (!wanted_ || reconnectT_) return;
    int delay = std::min(2000 << std::min(attempts_, 6), 120000);
    delay += (int)(randomU32() % 1000);
    attempts_++;
    reconnectT_ = later(delay, [this] {
        reconnectT_ = 0;
        if (wanted_) connect();
    });
}

void RealEngine::wsOpen() {
    header_ = {'W', 'A', 6, (uint8_t)dictVersion()};
    eph_ = genKeyPair();
    noise_ = NoiseState{};
    noise_.start(header_);
    noise_.authenticate(B(eph_.pub));
    hs_ = Hs::WaitHello;
    sendFrame(clientHelloFrame(eph_.pub));
}

void RealEngine::sendFrame(const Bytes& payload) {
    Bytes out;
    out.reserve(payload.size() + 7);
    if (!sentHeader_) {
        append(out, header_);
        sentHeader_ = true;
    }
    if (payload.size() >= (1u << 24)) throw std::runtime_error("frame too large");
    append(out, be24((uint32_t)payload.size()));
    append(out, payload);
    ws_.send(out);
}

void RealEngine::wsMessage(Bytes&& m) {
    append(frameBuf_, m);
    size_t pos = 0;
    uint64_t gen = gen_;
    while (frameBuf_.size() - pos >= 3) {
        size_t len = (size_t)frameBuf_[pos] << 16 | (size_t)frameBuf_[pos + 1] << 8 | frameBuf_[pos + 2];
        if (frameBuf_.size() - pos - 3 < len) break;
        Bytes f(frameBuf_.begin() + (long)pos + 3, frameBuf_.begin() + (long)(pos + 3 + len));
        pos += 3 + len;
        try {
            if (hs_ == Hs::WaitHello) {
                handshakeStep(f);
            } else if (hs_ == Hs::Done) {
                Bytes plain = tp_.open(f);
                Node n = decodeFrame(plain);
                handleNode(n);
            }
        } catch (const std::exception& e) {
            logLine(std::string("frame error: ") + e.what());
            if (hs_ != Hs::Done || gen == gen_) {
                // A handshake or transport failure cannot be resumed on this socket.
                if (hs_ == Hs::WaitHello) {
                    if (cfg_.probeOnly) {
                        reportProbe(false, std::string("handshake failed: ") + e.what());
                        return;
                    }
                    ws_.close();
                    wsClosed("handshake failed");
                    return;
                }
            }
        }
        if (gen != gen_) return;  // reconnected or torn down inside a handler
    }
    frameBuf_.erase(frameBuf_.begin(), frameBuf_.begin() + (long)pos);
}

void RealEngine::handshakeStep(const Bytes& frame) {
    ServerHello h = parseServerHello(frame);
    Key32 se = toKey(h.ephemeral);
    noise_.authenticate(h.ephemeral);
    noise_.mix(eph_.priv, se);
    Bytes staticKey = noise_.decrypt(h.staticCt);
    if (staticKey.size() != 32) throw std::runtime_error("bad server static");
    noise_.mix(eph_.priv, toKey(staticKey));
    Bytes cert = noise_.decrypt(h.payloadCt);
    if (!verifyNoiseCert(cert, staticKey, (uint64_t)now(), cfg_.rootKey ? &*cfg_.rootKey : nullptr))
        throw std::runtime_error("server certificate rejected");
    Bytes encPub = noise_.encrypt(B(dev_.noise.pub));
    noise_.mix(dev_.noise.priv, se);
    Bytes encPayload = noise_.encrypt(clientPayload());
    sendFrame(clientFinishFrame(encPub, encPayload));
    tp_ = NoiseTransport{};
    noise_.finish(tp_.writeKey, tp_.readKey);
    hs_ = Hs::Done;
    lastPong_ = now();
    uint64_t gen = gen_;
    keepT_ = later(20000 + (int)(randomU32() % 10000), [this, gen] { keepalive(gen); });
}

void RealEngine::sendNode(const Node& n) {
    if (hs_ != Hs::Done) return;
    sendFrame(tp_.seal(encodeNode(n)));
}

std::string RealEngine::nextId() { return idPrefix_ + "." + std::to_string(++idCounter_); }

void RealEngine::iq(const std::string& xmlns, const std::string& type, std::vector<Node> content, IqCb cb, Jid to, Jid target) {
    std::string id = nextId();
    Node n("iq");
    n.a("id", id).a("xmlns", xmlns).a("type", type).a("to", to);
    if (!target.empty()) n.a("target", target);
    for (auto& k : content) n.add(std::move(k));
    uint64_t t = later(30000, [this, id] {
        auto it = iqs_.find(id);
        if (it == iqs_.end()) return;
        auto f = std::move(it->second.first);
        iqs_.erase(it);
        if (f) f(nullptr);
    });
    iqs_[id] = {std::move(cb), t};
    sendNode(n);
}

void RealEngine::keepalive(uint64_t gen) {
    if (gen != gen_ || hs_ != Hs::Done) return;
    // A silent link, typically after sleep, is noticed here. Replacing the socket beats waiting.
    if (now() - lastPong_ > 75) {
        logLine("keepalive lost, reconnecting");
        ws_.close();
        wsClosed("keepalive");
        return;
    }
    iq("w:p", "get", {}, [this, gen](const Node* r) {
        if (gen == gen_ && r) lastPong_ = now();
    });
    keepT_ = later(20000 + (int)(randomU32() % 10000), [this, gen] { keepalive(gen); });
}

void RealEngine::reportProbe(bool ok, const std::string& what) {
    printf("probe: %s: %s\n", ok ? "ok" : "failed", what.c_str());
    fflush(stdout);
    ws_.close();
    loop_.stop();
}

// ---------------------------------------------------------------- dispatch

void RealEngine::handleNode(const Node& n) {
    lastPong_ = now();
    const std::string& t = n.tag;
    if (t == "iq") handleIq(n);
    else if (t == "message") handleMessage(n);
    else if (t == "receipt") handleReceipt(n);
    else if (t == "ack") handleAck(n);
    else if (t == "notification") handleNotification(n);
    else if (t == "presence") handlePresence(n);
    else if (t == "chatstate") handleChatstate(n);
    else if (t == "call") sendAck(n);  // calls are out of scope: acknowledged and ignored
    else if (t == "success") handleSuccess(n);
    else if (t == "failure") handleFailure(n);
    else if (t == "stream:error") handleStreamError(n);
    else if (t == "ib") {
        if (n.child("offline")) logLine("offline backlog finished");
    } else if (t == "xmlstreamend") {
        ws_.close();
        wsClosed("stream end");
    }
}

void RealEngine::sendAck(const Node& n, int error) {
    Node a("ack");
    a.a("class", n.tag);
    a.a("id", n.attr("id"));
    auto copy = [&](const char* from, const char* to) {
        for (auto& x : n.attrs)
            if (x.key == from) a.attrs.push_back({to, x.val, x.jid});
    };
    copy("from", "to");
    copy("participant", "participant");
    copy("recipient", "recipient");
    if (n.tag != "message") copy("type", "type");
    if (error) a.a("error", std::to_string(error));
    sendNode(a);
}

void RealEngine::handleIq(const Node& n) {
    std::string type = n.attr("type");
    if (type == "result" || type == "error") {
        auto it = iqs_.find(n.attr("id"));
        if (it == iqs_.end()) return;
        auto f = std::move(it->second.first);
        loop_.cancel(it->second.second);
        iqs_.erase(it);
        if (f) f(&n);
        return;
    }
    if (n.attr("xmlns") == "urn:xmpp:ping") {
        Node r("iq");
        r.a("to", n.jid("from")).a("id", n.attr("id")).a("type", "result");
        sendNode(r);
        return;
    }
    if (n.kids.size() == 1 && n.jid("from") == kServerJid) {
        if (n.kids[0].tag == "pair-device") pairDevice(n);
        else if (n.kids[0].tag == "pair-success") pairSuccess(n);
    }
}

void RealEngine::handleSuccess(const Node& n) {
    attempts_ = 0;
    loggedIn_ = true;
    Jid lid = n.jid("lid");
    if (!lid.empty() && dev_.lid.empty()) {
        dev_.lid = lid.str();
        saveDevice();
    }
    if (!lid.empty()) mapLid(self().bare(), lid.bare());
    afterLogin();
}

void RealEngine::afterLogin() {
    uint64_t gen = gen_;
    iq("encrypt", "get", {Node("count")}, [this, gen](const Node* r) {
        if (gen != gen_) return;
        int count = 0;
        if (r)
            if (auto c = r->child("count")) count = atoi(c->attr("value").c_str());
        if (r && count < 5) uploadPreKeys(dev_.preKeysUploaded ? 50 : 812);
        iq("passive", "set", {Node("active")}, [this, gen](const Node*) {
            if (gen != gen_) return;
            restoreQueue();
            setConn("connected");
            if (store_.get("meta", "askeys"))
                syncAppState({"critical_block", "critical_unblock_low", "regular_high", "regular", "regular_low"});
            if (active_ && !dev_.pushName.empty()) {
                Node p("presence");
                p.a("type", "available").a("name", dev_.pushName);
                sendNode(p);
            }
            pumpSend();
        });
    });
    iq("privacy", "get", {Node("privacy")}, [this](const Node* r) {
        if (!r) return;
        if (auto p = r->child("privacy"))
            for (auto c : p->children("category"))
                if (c->attr("name") == "readreceipts") readReceipts_ = c->attr("value") == "all";
    });
}

void RealEngine::handleFailure(const Node& n) {
    int reason = atoi(n.attr("reason").c_str());
    logLine("login failure, reason " + std::to_string(reason));
    switch (reason) {
        case 401:
        case 406:
            wipe();
            setConn("logged_out", "device removed");
            return;
        case 403:
            // The account or client is blocked. Retrying could make that worse.
            disconnect();
            setConn("offline", "blocked by server (403), not retrying");
            return;
        case 405:
            disconnect();
            setConn("offline", "client version out of date (405), update wa-version");
            return;
        default:
            if (reason >= 500) return;  // the socket closes and the backoff takes over
            disconnect();
            setConn("offline", "login failed (" + std::to_string(reason) + ")");
    }
}

void RealEngine::handleStreamError(const Node& n) {
    std::string code = n.attr("code");
    const Node* conflict = n.child("conflict");
    std::string ctype = conflict ? conflict->attr("type") : "";
    logLine("stream error " + code + " " + ctype);
    if (code == "515") {
        ws_.close();
        loop_.cancel(keepT_);
        clearIqs();
        attempts_ = 0;
        connect();
    } else if (code == "401" && ctype == "device_removed") {
        wipe();
        setConn("logged_out", "device removed");
    } else if (ctype == "replaced") {
        // Another session took over. Reconnecting would just fight it.
        disconnect();
        setConn("offline", "replaced by another session");
    }
}

void RealEngine::wipe() {
    disconnect();
    store_.wipe();
    dev_ = Device::fresh();
    saveDevice();
    deviceCache_.clear();
    groups_.clear();
    sendQ_.clear();
    inFlight_.clear();
    retryCount_.clear();
    hist_.clear();
    pairState_ = "unpaired";
    pairError_.clear();
    emitPairing();
}

// ---------------------------------------------------------------- pairing

void RealEngine::pairDevice(const Node& n) {
    Node ack("iq");
    ack.a("to", n.jid("from")).a("id", n.attr("id")).a("type", "result");
    sendNode(ack);
    qrRefs_.clear();
    for (auto r : n.kids[0].children("ref")) qrRefs_.push_back(r->data);
    if (cfg_.probeOnly) {
        // The refs and QR text are not printed: they are live pairing material.
        reportProbe(!qrRefs_.empty(), "noise handshake, certificate chain and framing good; server sent " +
                                          std::to_string(qrRefs_.size()) + " QR refs");
        return;
    }
    refsReady_ = true;
    if (phoneWanted_) startPhonePairing();
    if (qrFlow_) showQr();
}

void RealEngine::showQr() {
    loop_.cancel(qrT_);
    qrT_ = 0;
    if (!qrFlow_ || paired()) return;
    if (pairState_ == "phone_code") return;
    if (qrRefs_.empty()) {
        endPairing("QR expired");
        return;
    }
    Bytes ref = qrRefs_.front();
    bool first = currentQr_.empty();
    qrRefs_.pop_front();
    currentQr_ = "https://wa.me/settings/linked_devices#" + S(ref) + "," + b64(B(dev_.noise.pub)) + "," +
                 b64(B(dev_.identity.pub)) + "," + b64(dev_.adv) + ",9";
    qrExpires_ = first ? 60 : 20;
    pairState_ = "qr";
    pairError_.clear();
    emitPairing();
    qrT_ = later(qrExpires_ * 1000, [this] { showQr(); });
}

void RealEngine::endPairing(const std::string& error) {
    qrFlow_ = false;
    currentQr_.clear();
    phoneWanted_.reset();
    link_.reset();
    if (phoneReply_) {
        phoneReply_->err("timeout", error);
        phoneReply_.reset();
    }
    if (!paired()) {
        disconnect();
        pairState_ = "failed";
        pairError_ = error;
        emitPairing();
        pairState_ = "unpaired";
        pairError_.clear();
        emitPairing();
    }
}

void RealEngine::pairError(const std::string& id, int code, const std::string& text) {
    Node e("error");
    e.a("code", std::to_string(code)).a("text", text);
    Node r("iq");
    r.a("to", kServerJid).a("type", "error").a("id", id).add(e);
    sendNode(r);
}

void RealEngine::pairSuccess(const Node& n) {
    std::string id = n.attr("id");
    const Node& ps = n.kids[0];
    const Node* identity = ps.child("device-identity");
    const Node* device = ps.child("device");
    if (!identity || !device) {
        pairError(id, 500, "internal-error");
        endPairing("incomplete pairing answer");
        return;
    }
    Jid jid = device->jid("jid"), lid = device->jid("lid");
    try {
        pb::Msg container(identity->data);
        Bytes details = container.bytes(1), mac = container.bytes(2);
        Bytes in;
        if (container.num(3) == 1) append(in, Bytes{6, 5});  // hosted account
        append(in, details);
        if (!ctEqual(hmacSha256(dev_.adv, in), mac)) {
            pairError(id, 401, "hmac-mismatch");
            endPairing("pairing answer failed authentication");
            return;
        }
        pb::Msg signedId(details);
        Bytes idDetails = signedId.bytes(1), accKey = signedId.bytes(2), accSig = signedId.bytes(3);
        pb::Msg idd(idDetails);
        if (accKey.size() != 32 || accSig.size() != 64) {
            pairError(id, 401, "signature-mismatch");
            endPairing("bad account signature");
            return;
        }
        Bytes msg = idd.num(5) == 1 ? Bytes{6, 5} : Bytes{6, 0};
        append(msg, idDetails);
        append(msg, dev_.identity.pub);
        if (!xedVerify(toKey(accKey), msg, accSig.data())) {
            pairError(id, 401, "signature-mismatch");
            endPairing("bad account signature");
            return;
        }
        Bytes dm{6, 1};
        append(dm, idDetails);
        append(dm, dev_.identity.pub);
        append(dm, accKey);
        auto ds = xedSign(dev_.identity.priv, dm);
        Bytes devSig(ds.begin(), ds.end());

        pb::Writer account;
        account.b(1, idDetails).b(2, accKey).b(3, accSig).b(4, devSig);
        dev_.account = account.out;
        pb::Writer selfSigned;  // the account key stays out of the reply
        selfSigned.b(1, idDetails).b(3, accSig).b(4, devSig);
        dev_.jid = jid.str();
        dev_.lid = lid.str();
        saveDevice();
        if (!lid.empty()) mapLid(jid.bare(), lid.bare());

        Node di("device-identity");
        di.a("key-index", std::to_string(idd.num(3))).bytes(selfSigned.out);
        Node sign("pair-device-sign");
        sign.add(di);
        Node r("iq");
        r.a("to", kServerJid).a("type", "result").a("id", id).add(sign);
        sendNode(r);
    } catch (const std::exception&) {
        pairError(id, 500, "internal-error");
        endPairing("malformed pairing answer");
        return;
    }
    loop_.cancel(qrT_);
    qrT_ = 0;
    qrFlow_ = false;
    phoneWanted_.reset();
    link_.reset();
    pairState_ = "paired";
    pairError_.clear();
    currentQr_.clear();
    emitPairing();
    // The server now closes the stream with code 515, which brings the logged-in reconnect.
}

static std::string linkBase32(const Bytes& five) {
    static const char* a = "123456789ABCDEFGHJKLMNPQRSTVWXYZ";
    uint64_t v = 0;
    for (auto c : five) v = v << 8 | c;
    std::string s;
    for (int i = 7; i >= 0; i--) s += a[(v >> (i * 5)) & 31];
    return s;
}

void RealEngine::startPhonePairing() {
    std::string phone = *phoneWanted_;
    phoneWanted_.reset();
    LinkCache lc;
    lc.kp = genKeyPair();
    lc.code = linkBase32(randomBytes(5));
    lc.jid = Jid{phone, "s.whatsapp.net"};
    Bytes salt = randomBytes(32), iv = randomBytes(16);
    Bytes key = pbkdf2Sha256(lc.code, salt, 2 << 16, 32);
    Bytes enc = aesCtr(key, iv, B(lc.kp.pub));
    Bytes wrapped = salt;
    append(wrapped, iv);
    append(wrapped, enc);
    Node reg("link_code_companion_reg");
    reg.a("jid", lc.jid).a("stage", "companion_hello").a("should_show_push_notification", "true");
    reg.add(Node("link_code_pairing_wrapped_companion_ephemeral_pub").bytes(wrapped));
    reg.add(Node("companion_server_auth_key_pub").bytes(B(dev_.noise.pub)));
    reg.add(Node("companion_platform_id").text("1"));
    reg.add(Node("companion_platform_display").text("Chrome (Linux)"));
    reg.add(Node("link_code_pairing_nonce").bytes(Bytes{0}));
    link_ = std::move(lc);
    iq("md", "set", {reg}, [this](const Node* r) {
        const Node* c = r ? r->child("link_code_companion_reg") : nullptr;
        const Node* ref = c ? c->child("link_code_pairing_ref") : nullptr;
        if (!ref || !link_) {
            endPairing(r ? "server refused the number" : "no answer from server");
            return;
        }
        link_->ref = ref->data;
        std::string shown = link_->code.substr(0, 4) + "-" + link_->code.substr(4);
        pairCode_ = shown;
        pairState_ = "phone_code";
        pairError_.clear();
        qrFlow_ = false;
        loop_.cancel(qrT_);
        qrT_ = 0;
        if (phoneReply_) {
            phoneReply_->ok({{"code", shown}});
            phoneReply_.reset();
        }
        emitPairing();
    });
}

void RealEngine::codePairNotification(const Node& parent) {
    const Node* n = parent.child("link_code_companion_reg");
    if (!n || !link_) return;
    const Node* ref = n->child("link_code_pairing_ref");
    const Node* wrappedN = n->child("link_code_pairing_wrapped_primary_ephemeral_pub");
    const Node* primaryN = n->child("primary_identity_pub");
    if (!ref || !wrappedN || !primaryN || ref->data != link_->ref || wrappedN->data.size() < 80 || primaryN->data.size() != 32) {
        logLine("ignoring a code pairing notification that does not match");
        return;
    }
    const Bytes& w = wrappedN->data;
    pairState_ = "pairing";
    emitPairing();
    Bytes key = pbkdf2Sha256(link_->code, Bytes(w.begin(), w.begin() + 32), 2 << 16, 32);
    Bytes primaryPub = aesCtr(key, Bytes(w.begin() + 32, w.begin() + 48), Bytes(w.begin() + 48, w.begin() + 80));
    Key32 ephShared = x25519(link_->kp.priv, toKey(primaryPub));
    Bytes advRandom = randomBytes(32), bundleSalt = randomBytes(32), nonce = randomBytes(12);
    Bytes bundleKey = hkdf(B(ephShared), bundleSalt, "link_code_pairing_key_bundle_encryption_key", 32);
    Bytes plain = B(dev_.identity.pub);
    append(plain, primaryN->data);
    append(plain, advRandom);
    Bytes wrapped = bundleSalt;
    append(wrapped, nonce);
    append(wrapped, aesGcmEncrypt(bundleKey, nonce, plain, {}));
    Key32 idShared = x25519(dev_.identity.priv, toKey(primaryN->data));
    Bytes input = B(ephShared);
    append(input, idShared);
    append(input, advRandom);
    dev_.adv = hkdf(input, {}, "adv_secret", 32);
    saveDevice();
    Node fin("link_code_companion_reg");
    fin.a("jid", link_->jid).a("stage", "companion_finish");
    fin.add(Node("link_code_pairing_wrapped_key_bundle").bytes(wrapped));
    fin.add(Node("companion_identity_public").bytes(B(dev_.identity.pub)));
    fin.add(Node("link_code_pairing_ref").bytes(ref->data));
    iq("md", "set", {fin}, [](const Node*) {});
}

// ---------------------------------------------------------------- prekeys

Node RealEngine::preKeyNode(uint32_t id, const Key32& pub) {
    Node k("key");
    k.add(Node("id").bytes(be24(id))).add(Node("value").bytes(B(pub)));
    return k;
}

Node RealEngine::signedPreKeyNode() {
    Node k("skey");
    k.add(Node("id").bytes(be24(dev_.signedId)));
    k.add(Node("value").bytes(B(dev_.signedPre.pub)));
    k.add(Node("signature").bytes(dev_.signedSig));
    return k;
}

uint32_t RealEngine::genPreKey() {
    uint32_t id = dev_.nextPreKey++;
    KeyPair kp = genKeyPair();
    Bytes v = B(kp.priv);
    store_.put("prekey", std::to_string(id), v);
    return id;
}

void RealEngine::uploadPreKeys(int count) {
    Node list("list");
    store_.begin();
    for (int i = 0; i < count; i++) {
        uint32_t id = genPreKey();
        Key32 priv = toKey(*store_.get("prekey", std::to_string(id)));
        list.add(preKeyNode(id, x25519Pub(priv)));
    }
    saveDevice();
    store_.commit();
    std::vector<Node> c;
    c.push_back(Node("registration").bytes(be32(dev_.regId)));
    c.push_back(Node("type").bytes(Bytes{5}));
    c.push_back(Node("identity").bytes(B(dev_.identity.pub)));
    c.push_back(std::move(list));
    c.push_back(signedPreKeyNode());
    iq("encrypt", "set", std::move(c), [this](const Node* r) {
        if (r && r->attr("type") == "result") {
            dev_.preKeysUploaded = true;
            saveDevice();
        } else {
            logLine("prekey upload failed");
        }
    });
}

signal::Keys RealEngine::sigKeys() {
    signal::Keys k;
    k.identity = dev_.identity;
    k.regId = dev_.regId;
    k.preKey = [this](uint32_t id) -> std::optional<KeyPair> {
        auto v = store_.get("prekey", std::to_string(id));
        if (!v) return std::nullopt;
        KeyPair p;
        p.priv = toKey(*v);
        p.pub = x25519Pub(p.priv);
        return p;
    };
    k.signedPreKey = [this](uint32_t id) -> std::optional<KeyPair> {
        if (id != dev_.signedId) return std::nullopt;
        return dev_.signedPre;
    };
    return k;
}

// ---------------------------------------------------------------- addressing

void RealEngine::mapLid(const Jid& pn, const Jid& lid) {
    if (pn.empty() || lid.empty() || !pn.isPn() || !lid.isLid()) return;
    store_.put("lid", pn.bare().str(), B(lid.bare().str()));
    store_.put("pn", lid.bare().str(), B(pn.bare().str()));
}

Jid RealEngine::canon(const Jid& j) {
    Jid b = j.bare();
    if (b.isLid())
        if (auto p = store_.get("pn", b.str())) return Jid::parse(S(*p));
    return b;
}

// Sessions live under the privacy id when one is known, as the server migrates them there.
std::string RealEngine::sessionKey(const Jid& j) {
    if (j.isPn()) {
        if (auto l = store_.get("lid", j.bare().str())) {
            Jid lid = Jid::parse(S(*l));
            lid.device = j.device;
            std::string lk = lid.str(), pk = j.str();
            if (!store_.get("session", lk))
                if (auto old = store_.get("session", pk)) {
                    store_.put("session", lk, *old, lid.bare().str());
                    store_.del("session", pk);
                }
            return lk;
        }
    }
    return j.str();
}

signal::Record RealEngine::loadSession(const std::string& k) {
    if (auto v = store_.get("session", k)) return signal::deserialize(*v);
    return {};
}

void RealEngine::saveSession(const std::string& k, const signal::Record& r, const Jid& j) {
    Jid g = Jid::parse(k);
    store_.put("session", k, signal::serialize(r), g.empty() ? j.bare().str() : g.bare().str());
}

std::string RealEngine::nameOf(const Jid& bare) {
    if (bare.user.empty()) return bare.str();
    Jid c = canon(bare);
    for (const char* ns : {"cname", "name"}) {
        if (auto v = store_.get(ns, bare.str())) return S(*v);
        if (!(c == bare))
            if (auto v = store_.get(ns, c.str())) return S(*v);
    }
    return bare.isGroup() ? "Group" : (bare.isPn() ? "+" + bare.user : "Unknown");
}

void RealEngine::setName(const Jid& bare, const std::string& name) {
    if (name.empty() || name == "-") return;
    store_.put("name", bare.bare().str(), B(name));
}

bool RealEngine::isSelf(const Jid& j) {
    return (!dev_.jid.empty() && j.user == self().user) || (!dev_.lid.empty() && j.user == selfLid().user);
}

// ---------------------------------------------------------------- receive

void RealEngine::sendMessageReceipt(const Node& n, bool fromMe) {
    Node r("receipt");
    r.a("id", n.attr("id"));
    for (auto& a : n.attrs) {
        if (a.key == "from") r.attrs.push_back({"to", a.val, a.jid});
        if (a.key == "recipient" || a.key == "participant") r.attrs.push_back(a);
    }
    if (fromMe) r.a("type", n.attr("type") == "peer_msg" ? "peer_msg" : "sender");
    else if (!active_) r.a("type", "inactive");
    sendNode(r);
}

void RealEngine::sendRetryReceipt(const Node& n, bool forceKeys) {
    std::string id = n.attr("id");
    int inMsg = 0;
    if (n.kids.size() == 1 && n.kids[0].tag == "enc") inMsg = atoi(n.kids[0].attr("count").c_str());
    int& c = retryCount_[id];
    c++;
    if (c == 1 && inMsg > 0) c = inMsg + 1;
    int count = c;
    if (count >= 5) {
        logLine("giving up on retries for one message");
        return;
    }
    Node r("receipt");
    r.a("id", id);
    for (auto& a : n.attrs) {
        if (a.key == "from") r.attrs.push_back({"to", a.val, a.jid});
        if (a.key == "recipient" || a.key == "participant") r.attrs.push_back(a);
    }
    r.a("type", "retry");
    Node retry("retry");
    retry.a("count", std::to_string(count)).a("id", id).a("t", n.attr("t")).a("v", "1");
    r.add(retry);
    r.add(Node("registration").bytes(be32(dev_.regId)));
    if (count > 1 || forceKeys) {
        uint32_t pid = genPreKey();
        saveDevice();
        Key32 pub = x25519Pub(toKey(*store_.get("prekey", std::to_string(pid))));
        Node keys("keys");
        keys.add(Node("type").bytes(Bytes{5}));
        keys.add(Node("identity").bytes(B(dev_.identity.pub)));
        keys.add(preKeyNode(pid, pub));
        keys.add(signedPreKeyNode());
        keys.add(Node("device-identity").bytes(dev_.account));
        r.add(keys);
    }
    sendNode(r);
}

void RealEngine::handleMessage(const Node& n) {
    Jid from = n.jid("from");
    std::string id = n.attr("id");
    int64_t ts = atoll(n.attr("t").c_str());
    if (from.empty() || id.empty()) {
        sendAck(n);
        return;
    }
    // Newsletters and broadcast lists are not part of this app. Status is.
    if (from.server == "newsletter" || (from.server == "broadcast" && from.user != "status")) {
        sendAck(n);
        return;
    }
    Jid chat, sender;
    bool fromMe = false;
    if (from.groupish()) {
        chat = from;
        sender = n.jid("participant");
        fromMe = isSelf(sender);
        Jid pnAlt = n.jid("participant_pn"), lidAlt = n.jid("participant_lid");
        if (sender.isLid()) mapLid(pnAlt, sender);
        if (sender.isPn()) mapLid(sender, lidAlt);
    } else if (isSelf(from)) {
        fromMe = true;
        sender = from;
        Jid rec = n.jid("recipient");
        chat = rec.empty() ? from.bare() : rec.bare();
        if (rec.isLid()) mapLid(n.jid("peer_recipient_pn"), rec);
        if (rec.isPn()) mapLid(rec, n.jid("peer_recipient_lid"));
    } else {
        sender = from;
        chat = from.bare();
        if (from.isLid()) mapLid(n.jid("sender_pn"), from);
        if (from.isPn()) mapLid(from, n.jid("sender_lid"));
    }
    chat = chat.groupish() ? chat : canon(chat);
    std::string push = n.attr("notify");
    bool live = n.attr("offline").empty() && now() - ts < 300;

    if (n.child("unavailable") && n.children("enc").empty()) {
        sendAck(n);
        return;
    }
    bool any = false, failed = false, skMissing = false, direct = false;
    for (auto enc : n.children("enc")) {
        any = true;
        std::string t = enc->attr("type");
        Bytes plain;
        try {
            if (t == "pkmsg" || t == "msg") {
                direct = true;
                std::string k = sessionKey(sender);
                signal::Record rec = loadSession(k);
                std::optional<uint32_t> used;
                if (t == "pkmsg") plain = signal::decryptPreKey(rec, sigKeys(), enc->data, &used);
                else plain = signal::decryptMessage(rec, enc->data);
                saveSession(k, rec, sender);
                if (used) store_.del("prekey", std::to_string(*used));
            } else if (t == "skmsg" && chat.groupish()) {
                std::string k = chat.str() + "|" + sessionKey(sender);
                auto v = store_.get("sender", k);
                if (!v) {
                    skMissing = true;
                    throw signal::DecryptError("no sender key");
                }
                signal::SenderRecord rec = signal::deserializeSender(*v);
                plain = signal::senderDecrypt(rec, enc->data);
                store_.put("sender", k, signal::serialize(rec), sender.bare().str());
            } else {
                continue;
            }
            std::string ver = enc->attr("v");
            if (ver == "3") {
                ParsedMsg p;
                p.kind = "unsupported";
                p.text = "This message type is not supported yet";
                storeIncoming(chat, sender, fromMe, ts, id, p, live, true, push);
                continue;
            }
            plain = signal::unpad(plain);
            processPlain(n, plain, chat, sender, fromMe, ts, id, live, push);
        } catch (const signal::DuplicateError&) {
            continue;
        } catch (const signal::DecryptError& e) {
            logLine(std::string("decrypt failed: ") + e.what());
            failed = true;
            break;
        } catch (const std::runtime_error& e) {
            logLine(std::string("bad message content: ") + e.what());
            failed = true;
            break;
        }
    }
    if (failed) {
        sendRetryReceipt(n, skMissing && !direct);
        sendAck(n);
        ParsedMsg p;
        p.kind = "undecryptable";
        p.text = "Waiting for this message. Check your phone.";
        storeIncoming(chat, sender, fromMe, ts, id, p, false, false, push);
        return;
    }
    if (!any) {
        sendAck(n);
        return;
    }
    retryCount_.erase(id);
    sendMessageReceipt(n, fromMe);
}

void RealEngine::processPlain(const Node& stanza, const Bytes& plain, const Jid& chat, const Jid& sender, bool fromMe,
                              int64_t ts, const std::string& id, bool live, const std::string& push) {
    ParsedMsg p = parseMessage(plain);
    if (!p.skdm.empty()) {
        std::string grp = p.skdmGroup.empty() ? chat.str() : p.skdmGroup;
        std::string k = grp + "|" + sessionKey(sender);
        signal::SenderRecord rec;
        if (auto v = store_.get("sender", k)) rec = signal::deserializeSender(*v);
        signal::processDistribution(rec, p.skdm);
        store_.put("sender", k, signal::serialize(rec), sender.bare().str());
    }
    Jid realChat = chat;
    if (!p.dsmDest.empty()) {
        Jid d = Jid::parse(p.dsmDest);
        if (!d.empty()) realChat = d.groupish() ? d : canon(d);
    }
    if (p.isProtocol) {
        if (fromMe && !p.appKeys.empty()) {
            for (auto& [id, data] : p.appKeys) store_.put("askey", hex(id), data);
            store_.put("meta", "askeys", B("1"));
            syncAppState({"critical_block", "critical_unblock_low", "regular_high", "regular", "regular_low"});
        }
        if (fromMe && p.history) {
            Node r("receipt");
            r.a("id", id).a("type", "hist_sync").a("to", self().bare());
            sendNode(r);
            onHistory(*p.history, id);
        } else if (p.protoType == 0 && !p.targetId.empty()) {  // revoke
            if (auto m = store_.getMessage(realChat.str(), p.targetId)) {
                (*m)["kind"] = "system";
                (*m)["text"] = "This message was deleted";
                (*m)["media"] = nullptr;
                m->erase("_s");
                store_.updateMessage(realChat.str(), p.targetId, *m);
                json pub = *m;
                ipc_.broadcast({{"event", "message"}, {"chat", realChat.str()}, {"chat_name", nameOf(realChat)},
                                {"sender", pub["sender"]}, {"sender_name", pub["sender_name"]}, {"text", pub["text"]},
                                {"timestamp", pub["timestamp"]}, {"id", p.targetId}, {"from_me", pub["from_me"]},
                                {"is_group", realChat.isGroup()}, {"muted", false}, {"live", false}, {"message", pub}});
            }
        } else if (p.protoType == 14 && !p.targetId.empty() && !p.editText.empty()) {  // edit
            if (auto m = store_.getMessage(realChat.str(), p.targetId)) {
                (*m)["text"] = p.editText;
                store_.updateMessage(realChat.str(), p.targetId, *m);
                json pub = *m;
                pub.erase("_s");
                ipc_.broadcast({{"event", "message"}, {"chat", realChat.str()}, {"chat_name", nameOf(realChat)},
                                {"sender", pub["sender"]}, {"sender_name", pub["sender_name"]}, {"text", pub["text"]},
                                {"timestamp", pub["timestamp"]}, {"id", p.targetId}, {"from_me", pub["from_me"]},
                                {"is_group", realChat.isGroup()}, {"muted", false}, {"live", false}, {"message", pub}});
            }
        }
        return;
    }
    if (p.kind.empty()) return;
    storeIncoming(realChat, sender, fromMe, ts, id, p, live, true, push);
    (void)stanza;
}

void RealEngine::touchChat(const Jid& chat, const std::string& nameHint) {
    std::string cid = chat.str();
    json c;
    if (auto ex = store_.getChat(cid)) c = *ex;
    else
        c = {{"id", cid}, {"name", ""}, {"is_group", chat.isGroup()}, {"unread", 0}, {"last_text", ""}, {"last_ts", 0},
             {"last_from_me", false}, {"muted", false}, {"pinned", false}, {"archived", false}};
    if (!nameHint.empty()) setName(chat, nameHint);
    c["name"] = nameOf(chat);
    c["unread"] = store_.unreadCount(cid);
    overlayFlags(c);
    if (auto m = store_.lastMessage(cid)) {
        c["last_text"] = previewOf((*m)["kind"].get<std::string>(), (*m)["text"].get<std::string>());
        c["last_ts"] = (*m)["timestamp"];
        c["last_from_me"] = (*m)["from_me"];
    }
    store_.putChat(cid, c);
    ipc_.broadcast({{"event", "chat"}, {"chat", c}});
    if (chat.isGroup() && !store_.get("name", cid)) fetchGroup(chat);
}

void RealEngine::storeIncoming(const Jid& chat, const Jid& sender, bool fromMe, int64_t ts, const std::string& id,
                               ParsedMsg& p, bool live, bool replaceUndecryptable, const std::string& push) {
    if (chat.server == "broadcast") {
        storeStatus(sender, fromMe, ts, id, p, live, push);
        return;
    }
    std::string cid = chat.str();
    Jid sBare = canon(sender);
    if (fromMe && !push.empty() && dev_.pushName != push && push != "-") {
        dev_.pushName = push;
        saveDevice();
    }
    if (!fromMe && !push.empty()) setName(sBare, push);
    json m = {{"chat", cid}, {"id", id}, {"from_me", fromMe}, {"sender", sBare.str()},
              {"sender_name", fromMe ? "You" : nameOf(sBare)}, {"timestamp", ts}, {"kind", p.kind}, {"text", p.text},
              {"status", fromMe ? "sent" : "received"}, {"quoted_id", p.quotedId}, {"media", p.media}};
    if (!p.secret.is_null()) m["_s"] = p.secret;
    bool isNew = store_.insertMessage(cid, id, ts, !fromMe && p.kind != "reaction" && p.kind != "undecryptable", m);
    if (!isNew) {
        auto old = store_.getMessage(cid, id);
        if (!old || (*old)["kind"] != "undecryptable" || !replaceUndecryptable) return;
        store_.updateMessage(cid, id, m);
    }
    if (!chat.isGroup() && !fromMe && !push.empty()) setName(chat, push);
    touchChat(chat, "");
    auto chatRow = store_.getChat(cid);
    bool muted = chatRow ? chatRow->value("muted", false) : false;
    json pub = m;
    pub.erase("_s");
    ipc_.broadcast({{"event", "message"}, {"chat", cid}, {"chat_name", nameOf(chat)}, {"sender", sBare.str()},
                    {"sender_name", pub["sender_name"]}, {"text", previewOf(p.kind, p.text)}, {"timestamp", ts}, {"id", id},
                    {"from_me", fromMe}, {"is_group", chat.isGroup()}, {"muted", muted},
                    {"live", live && p.kind != "reaction" && p.kind != "undecryptable"}, {"message", pub}});
}

void RealEngine::fetchGroup(const Jid& g, std::function<void(bool)> cb) {
    std::string gid = g.str();
    if (hs_ != Hs::Done) {
        if (cb) cb(false);
        return;
    }
    if (!cb) {
        if (groupFetching_.count(gid)) return;
        groupFetching_.insert(gid);
    }
    Node q("query");
    q.a("request", "interactive");
    iq("w:g2", "get", {q}, [this, g, gid, cb](const Node* r) {
        groupFetching_.erase(gid);
        const Node* gn = (r && r->attr("type") == "result") ? r->child("group") : nullptr;
        if (!gn) {
            if (cb) cb(false);
            return;
        }
        GroupInfo info;
        info.lid = gn->attr("addressing_mode") == "lid";
        for (auto p : gn->children("participant")) {
            Jid j = p->jid("jid");
            if (j.empty()) continue;
            info.participants.push_back(j.bare());
            Jid pn = p->jid("phone_number"), lid = p->jid("lid");
            if (j.isLid()) mapLid(pn, j);
            if (j.isPn()) mapLid(j, lid);
        }
        json parts = json::array();
        for (auto p : gn->children("participant")) {
            Jid j = p->jid("jid");
            if (j.empty()) continue;
            std::string kind = p->attr("type");
            parts.push_back({{"jid", canon(j).str()}, {"name", isSelf(j) ? "You" : nameOf(canon(j))},
                             {"is_admin", kind == "admin" || kind == "superadmin"}, {"is_super_admin", kind == "superadmin"}});
        }
        std::string topic;
        if (auto d = gn->child("description"))
            if (auto b = d->child("body")) topic = S(b->data);
        Jid owner = gn->jid("creator");
        info.info = {{"id", gid}, {"subject", gn->attr("subject")}, {"topic", topic}, {"owner", owner.empty() ? "" : canon(owner).str()},
                     {"created", atoll(gn->attr("creation").c_str())}, {"announce", gn->child("announcement") != nullptr},
                     {"locked", gn->child("locked") != nullptr}, {"participants", parts}};
        groups_[gid] = info;
        std::string subject = gn->attr("subject");
        if (!subject.empty() && store_.get("name", gid).value_or(Bytes{}) != B(subject)) {
            setName(g, subject);
            touchChat(g, "");
        }
        if (cb) cb(true);
    }, g);
}

// ---------------------------------------------------------------- receipts, acks, notifications

void RealEngine::markStatus(const std::string& chat, const std::vector<std::string>& ids, const std::string& status,
                            const std::string& sender, const std::string& error) {
    static const std::map<std::string, int> rank = {{"pending", 0}, {"sent", 1}, {"delivered", 2}, {"read", 3}, {"failed", 4}};
    std::vector<std::string> moved;
    for (auto& id : ids) {
        auto m = store_.getMessage(chat, id);
        if (!m || !(*m)["from_me"].get<bool>()) continue;
        std::string cur = (*m)["status"];
        auto cr = rank.find(cur), nr = rank.find(status);
        if (cr == rank.end() || nr == rank.end() || (nr->second <= cr->second && status != "failed") || cur == "read") continue;
        (*m)["status"] = status;
        store_.updateMessage(chat, id, *m);
        moved.push_back(id);
    }
    if (moved.empty()) return;
    json e = {{"event", "receipt"}, {"chat", chat}, {"ids", moved}, {"status", status}, {"sender", sender}};
    if (!error.empty()) e["error"] = error;
    ipc_.broadcast(e);
}

void RealEngine::handleReceipt(const Node& n) {
    std::string type = n.attr("type");
    if (type == "retry") {
        try {
            handleRetry(n);
        } catch (const std::exception& e) {
            logLine(std::string("retry handling failed: ") + e.what());
        }
        sendAck(n);
        return;
    }
    Jid from = n.jid("from");
    Jid chatJ = from.isGroup() ? from : from.bare();
    std::vector<std::string> ids;
    if (!n.attr("id").empty()) ids.push_back(n.attr("id"));
    if (auto l = n.child("list"))
        for (auto it : l->children("item")) ids.push_back(it->attr("id"));
    if (isSelf(from)) {
        // Another of our devices read something: clear the unread marks here.
        Jid rec = n.jid("recipient");
        if ((type == "read" || type == "read-self") && !rec.empty()) {
            Jid c = rec.isGroup() ? rec : canon(rec);
            for (auto& id : ids) store_.clearUnread(c.str(), id);
            touchChat(c, "");
        }
    } else {
        std::string chat = chatJ.isGroup() ? chatJ.str() : canon(chatJ).str();
        if (type.empty() || type == "inactive") markStatus(chat, ids, "delivered", n.attr("participant"));
        else if (type == "read" || type == "played") markStatus(chat, ids, "read", n.attr("participant"));
    }
    sendAck(n);
}

void RealEngine::handleAck(const Node& n) {
    if (n.attr("class") != "message") return;
    auto it = inFlight_.find(n.attr("id"));
    if (it == inFlight_.end()) return;
    std::string chat = it->second, id = it->first;
    inFlight_.erase(it);
    dequeue(id);
    std::string err = n.attr("error");
    if (!err.empty()) {
        logLine("server refused a message, code " + err);
        markStatus(chat, {id}, "failed", "", "server error " + err);
    } else {
        markStatus(chat, {id}, "sent", "");
    }
}

void RealEngine::handleNotification(const Node& n) {
    std::string type = n.attr("type");
    Jid from = n.jid("from");
    if (type == "encrypt") {
        if (from == kServerJid) {
            if (auto c = n.child("count"))
                if (atoi(c->attr("value").c_str()) < 5) uploadPreKeys(50);
        } else if (n.child("identity")) {
            store_.delGroup("session", from.bare().str());
            if (auto l = store_.get("lid", from.bare().str())) store_.delGroup("session", S(*l));
            deviceCache_.erase(from.bare().str());
        }
    } else if (type == "devices") {
        deviceCache_.erase(from.bare().str());
        Jid lid = n.jid("lid");
        if (!lid.empty()) {
            mapLid(from.bare(), lid.bare());
            deviceCache_.erase(lid.bare().str());
        }
    } else if (type == "link_code_companion_reg") {
        try {
            codePairNotification(n);
        } catch (const std::exception& e) {
            logLine(std::string("code pairing failed: ") + e.what());
            endPairing("code pairing failed");
        }
    } else if (type == "privacy_token") {
        if (auto toks = n.child("tokens"))
            for (auto t : toks->children("token"))
                if (t->attr("type") == "trusted_contact" && !t->data.empty()) store_.put("tctoken", from.bare().str(), t->data);
    } else if (type == "server_sync") {
        std::vector<std::string> names;
        if (auto col = n.child("sync"))
            for (auto c : col->children("collection")) names.push_back(c->attr("name"));
        for (auto c : n.children("collection")) names.push_back(c->attr("name"));
        syncAppState(names);
    } else if (type == "picture") {
        store_.del("picture", from.bare().str());
        ipc_.broadcast({{"event", "picture"}, {"jid", from.isGroup() ? from.str() : canon(from).str()}});
    } else if (type == "w:gp2") {
        groups_.erase(from.str());
        if (auto s = n.child("subject")) {
            std::string subj = s->attr("subject");
            if (!subj.empty()) {
                setName(from, subj);
                if (store_.getChat(from.str())) touchChat(from, "");
            }
        }
    }
    sendAck(n);
}

void RealEngine::handlePresence(const Node& n) {
    Jid from = n.jid("from");
    if (from.empty()) return;
    std::string last = n.attr("last");
    ipc_.broadcast({{"event", "presence"}, {"chat", canon(from).str()}, {"available", n.attr("type") != "unavailable"},
                    {"last_seen", (last.empty() || last == "deny") ? 0 : atoll(last.c_str())}, {"typing", false}});
}

void RealEngine::handleChatstate(const Node& n) {
    Jid from = n.jid("from");
    if (from.empty()) return;
    bool typing = n.child("composing") != nullptr;
    std::string chat = from.isGroup() ? from.str() : canon(from).str();
    ipc_.broadcast({{"event", "presence"}, {"chat", chat}, {"available", true}, {"last_seen", 0}, {"typing", typing}});
}

// ---------------------------------------------------------------- send

void RealEngine::rememberOutgoing(const Pending& p) {
    json o = {{"chat", p.chat.str()}, {"proto", b64(p.proto)}, {"ts", p.ts}};
    store_.put("out", p.id, B(o.dump()));
    // Only recent sends can be asked for again, so old ones are pruned on a ring of 256.
    uint64_t seq = 0;
    if (auto v = store_.get("meta", "outseq")) seq = std::stoull(S(*v));
    std::string slot = std::to_string(seq % 256);
    if (auto old = store_.get("outring", slot)) store_.del("out", S(*old));
    store_.put("outring", slot, B(p.id));
    store_.put("meta", "outseq", B(std::to_string(seq + 1)));
}

std::vector<std::string> RealEngine::queueIds() {
    std::vector<std::string> ids;
    if (auto v = store_.get("meta", "outq"))
        for (auto& i : json::parse(S(*v))) ids.push_back(i.get<std::string>());
    return ids;
}

void RealEngine::setQueueIds(const std::vector<std::string>& ids) { store_.put("meta", "outq", B(json(ids).dump())); }

void RealEngine::dequeue(const std::string& id) {
    auto ids = queueIds();
    ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
    setQueueIds(ids);
}

// Whatever the server never acknowledged goes out again: the message id lets the
// receiver drop a duplicate if the first attempt did arrive.
void RealEngine::restoreQueue() {
    sendQ_.clear();
    inFlight_.clear();
    sending_ = false;
    for (auto& id : queueIds()) {
        auto o = store_.get("out", id);
        if (!o) {
            dequeue(id);
            continue;
        }
        json j = json::parse(S(*o));
        sendQ_.push_back({Jid::parse(j["chat"].get<std::string>()), id, b64dec(j["proto"].get<std::string>()), j["ts"].get<int64_t>()});
    }
}

void RealEngine::enqueueSend(Pending p) {
    rememberOutgoing(p);
    auto ids = queueIds();
    ids.push_back(p.id);
    setQueueIds(ids);
    sendQ_.push_back(std::move(p));
    pumpSend();
}

void RealEngine::pumpSend() {
    if (sending_ || sendQ_.empty() || conn_ != "connected") return;
    sending_ = true;
    Pending p = sendQ_.front();
    sendQ_.pop_front();
    sendOne(p);
}

void RealEngine::sendOne(const Pending& p) {
    auto done = [this, p](bool ok, const std::string& why) {
        if (!ok && conn_ == "connected") {
            logLine("send failed: " + why);
            dequeue(p.id);
            markStatus(p.chat.str(), {p.id}, "failed", "", why);
        }
        // With the link down it stays queued and goes out after the next login.
        sending_ = false;
        pumpSend();
    };
    auto withUsers = [this, p, done](std::vector<Jid> users) {
        devicesFor(users, [this, p, done](std::optional<std::vector<Jid>> devs) {
            if (!devs) return done(false, "could not fetch the device list");
            std::vector<Jid> targets;
            for (auto& d : *devs)
                if (!(d == self()) && !(d == selfLid())) targets.push_back(d);
            ensureSessions(targets, [this, p, done, targets] {
                try {
                    finishSend(p, targets);
                    done(true, "");
                } catch (const std::exception& e) {
                    done(false, e.what());
                }
            });
        });
    };
    if (p.chat.server == "broadcast") {
        statusAudience([withUsers, done](std::optional<std::vector<Jid>> users) {
            if (!users) return done(false, "could not read the status privacy setting");
            withUsers(*users);
        });
    } else if (p.chat.isGroup()) {
        auto proceed = [this, p, withUsers, done](bool ok) {
            auto it = groups_.find(p.chat.str());
            if (!ok || it == groups_.end()) return done(false, "could not fetch the group members");
            withUsers(it->second.participants);
        };
        if (groups_.count(p.chat.str())) proceed(true);
        else fetchGroup(p.chat, proceed);
    } else {
        withUsers({p.chat, self().bare()});
    }
}

void RealEngine::devicesFor(const std::vector<Jid>& users, std::function<void(std::optional<std::vector<Jid>>)> cb) {
    std::vector<Jid> out, missing;
    for (auto& u : users) {
        auto it = deviceCache_.find(u.bare().str());
        if (it != deviceCache_.end()) out.insert(out.end(), it->second.begin(), it->second.end());
        else missing.push_back(u.bare());
    }
    if (missing.empty()) {
        cb(out);
        return;
    }
    Node q("query");
    q.add(Node("devices").a("version", "2"));
    Node l("list");
    for (auto& u : missing) l.add(Node("user").a("jid", u));
    Node us("usync");
    us.a("sid", nextId()).a("mode", "query").a("last", "true").a("index", "0").a("context", "message").add(q).add(l);
    iq("usync", "get", {us}, [this, out, cb](const Node* r) mutable {
        const Node* us2 = (r && r->attr("type") == "result") ? r->child("usync") : nullptr;
        const Node* l2 = us2 ? us2->child("list") : nullptr;
        if (!l2) {
            cb(std::nullopt);
            return;
        }
        for (auto u : l2->children("user")) {
            Jid j = u->jid("jid");
            const Node* ds = u->child("devices");
            const Node* dl = ds ? ds->child("device-list") : nullptr;
            if (j.empty() || !dl) continue;
            std::vector<Jid> v;
            for (auto d : dl->children("device")) {
                if (d->attr("id").empty()) continue;
                Jid x = j.bare();
                x.device = (uint16_t)atoi(d->attr("id").c_str());
                if (d->attr("is_hosted") == "true") x.server = j.isLid() ? "hosted.lid" : "hosted";
                v.push_back(x);
            }
            deviceCache_[j.bare().str()] = v;
            out.insert(out.end(), v.begin(), v.end());
        }
        cb(out);
    });
}

bool RealEngine::applyBundle(const Jid& device, const Node& user) {
    if (user.child("error")) return false;
    const Node* reg = user.child("registration");
    const Node* keys = user.child("keys");
    const Node& kn = keys ? *keys : user;
    const Node* identity = kn.child("identity");
    const Node* skey = kn.child("skey");
    if (!reg || reg->data.size() != 4 || !identity || identity->data.size() != 32 || !skey) return false;
    auto id3 = [](const Node& k) -> std::optional<uint32_t> {
        const Node* i = k.child("id");
        if (!i || i->data.size() != 3) return std::nullopt;
        return (uint32_t)i->data[0] << 16 | (uint32_t)i->data[1] << 8 | i->data[2];
    };
    signal::Bundle b;
    b.regId = (uint32_t)reg->data[0] << 24 | (uint32_t)reg->data[1] << 16 | (uint32_t)reg->data[2] << 8 | reg->data[3];
    b.identity = toKey(identity->data);
    auto sid = id3(*skey);
    const Node* sv = skey->child("value");
    const Node* sg = skey->child("signature");
    if (!sid || !sv || sv->data.size() != 32 || !sg || sg->data.size() != 64) return false;
    b.signedId = *sid;
    b.signedKey = toKey(sv->data);
    b.signature = sg->data;
    if (const Node* pk = kn.child("key")) {
        auto pid = id3(*pk);
        const Node* pv = pk->child("value");
        if (!pid || !pv || pv->data.size() != 32) return false;
        b.preKeyId = *pid;
        b.preKey = toKey(pv->data);
    }
    std::string k = sessionKey(device);
    signal::Record rec = loadSession(k);
    signal::initFromBundle(rec, dev_.identity, dev_.regId, b);
    saveSession(k, rec, device);
    return true;
}

void RealEngine::ensureSessions(const std::vector<Jid>& devices, std::function<void()> cb) {
    std::vector<Jid> missing;
    for (auto& d : devices)
        if (!store_.get("session", sessionKey(d))) missing.push_back(d);
    if (missing.empty()) {
        cb();
        return;
    }
    Node key("key");
    for (auto& d : missing) key.add(Node("user").a("jid", d).a("reason", "identity"));
    iq("encrypt", "get", {key}, [this, cb](const Node* r) {
        const Node* l = (r && r->attr("type") == "result") ? r->child("list") : nullptr;
        if (l)
            for (auto u : l->children("user")) {
                Jid j = u->jid("jid");
                try {
                    if (!j.empty() && !applyBundle(j, *u)) logLine("no usable keys for one device");
                } catch (const std::exception& e) {
                    logLine(std::string("bad key bundle: ") + e.what());
                }
            }
        cb();  // devices still without a session are skipped when encrypting
    });
}

Node RealEngine::encryptFor(const Jid& device, const Bytes& plain, bool* preKey) {
    std::string k = sessionKey(device);
    signal::Record rec = loadSession(k);
    signal::Ciphertext c = signal::encrypt(rec, signal::pad(plain));
    saveSession(k, rec, device);
    *preKey = c.preKey;
    Node enc("enc");
    enc.a("v", "2").a("type", c.preKey ? "pkmsg" : "msg").bytes(c.data);
    return enc;
}

void RealEngine::finishSend(const Pending& p, const std::vector<Jid>& devices) {
    bool group = p.chat.groupish();
    Bytes forPeers = p.proto, forSelf;
    Bytes skmsg;
    if (group) {
        std::string k = p.chat.str() + "|self";
        signal::SenderRecord rec;
        if (auto v = store_.get("sender", k)) rec = signal::deserializeSender(*v);
        Bytes dist = signal::senderKeyDistribution(rec);
        forPeers = buildSkdmMessage(p.chat.str(), dist);
        skmsg = signal::senderEncrypt(rec, signal::pad(p.proto));
        store_.put("sender", k, signal::serialize(rec));
    } else {
        forSelf = buildDeviceSent(p.chat.str(), p.proto);
    }
    Node parts("participants");
    bool identity = false;
    int delivered = 0;
    for (auto& d : devices) {
        if (!store_.get("session", sessionKey(d))) continue;
        bool own = isSelf(d) && !forSelf.empty();
        bool pre = false;
        try {
            Node enc = encryptFor(d, own ? forSelf : forPeers, &pre);
            Node to("to");
            to.a("jid", d).add(std::move(enc));
            parts.add(std::move(to));
            identity = identity || pre;
            delivered++;
        } catch (const std::exception& e) {
            logLine(std::string("could not encrypt for one device: ") + e.what());
        }
    }
    if (!group && delivered == 0) throw std::runtime_error("no recipient device could be reached");
    Node m("message");
    m.a("id", p.id).a("type", "text").a("to", p.chat);
    if (group) {
        auto it = groups_.find(p.chat.str());
        if (it != groups_.end() && it->second.lid) m.a("addressing_mode", "lid");
        std::vector<std::string> ads;
        for (auto& d : devices) ads.push_back(d.str());
        std::sort(ads.begin(), ads.end());
        std::string joined;
        for (auto& s : ads) joined += s;
        Bytes h = sha256(B(joined));
        h.resize(6);
        std::string ph = b64(h);
        while (!ph.empty() && ph.back() == '=') ph.pop_back();
        m.a("phash", "2:" + ph);
    }
    m.add(std::move(parts));
    if (identity) m.add(Node("device-identity").bytes(dev_.account));
    if (group) {
        Node sk("enc");
        sk.a("v", "2").a("type", "skmsg").bytes(skmsg);
        m.add(std::move(sk));
    } else if (auto tok = store_.get("tctoken", p.chat.str())) {
        m.add(Node("tctoken").bytes(*tok));
    }
    inFlight_[p.id] = p.chat.str();
    sendNode(m);
}

void RealEngine::handleRetry(const Node& n) {
    const Node* retry = n.child("retry");
    if (!retry) return;
    std::string id = retry->attr("id");
    auto o = store_.get("out", id);
    if (!o) {
        logLine("retry asked for a message no longer kept");
        return;
    }
    json out = json::parse(S(*o));
    Jid chat = Jid::parse(out["chat"].get<std::string>());
    Bytes proto = b64dec(out["proto"].get<std::string>());
    Jid from = n.jid("from");
    Jid target = chat.groupish() ? n.jid("participant") : from;
    if (target.empty()) return;
    int count = atoi(retry->attr("count").c_str());
    int& tries = retryCount_["in:" + id + "|" + target.str()];
    if (++tries >= 10) return;

    Bytes plain = proto;
    if (chat.groupish()) {
        std::string k = chat.str() + "|self";
        signal::SenderRecord rec;
        if (auto v = store_.get("sender", k)) rec = signal::deserializeSender(*v);
        Bytes dist = signal::senderKeyDistribution(rec);
        store_.put("sender", k, signal::serialize(rec));
        plain = buildSkdmMessage(chat.str(), dist);
        append(plain, proto);  // protobuf concatenation merges the two messages
    } else if (isSelf(target)) {
        plain = buildDeviceSent(chat.str(), proto);
    }

    auto send = [this, id, plain, target, chat, count, n] {
        if (!store_.get("session", sessionKey(target))) {
            logLine("no session to answer a retry");
            return;
        }
        bool pre = false;
        Node enc = encryptFor(target, plain, &pre);
        enc.a("count", std::to_string(count));
        Node m("message");
        for (auto& a : n.attrs)
            if (a.key == "from") m.attrs.push_back({"to", a.val, a.jid});
        m.a("type", "text").a("id", id).a("t", n.child("retry")->attr("t"));
        if (!chat.groupish()) m.a("device_fanout", "false");
        for (auto& a : n.attrs)
            if (a.key == "participant" || a.key == "recipient") m.attrs.push_back(a);
        m.add(std::move(enc));
        if (pre) m.add(Node("device-identity").bytes(dev_.account));
        sendNode(m);
    };
    if (n.child("keys")) {
        const Node* keys = n.child("keys");
        Node user("user");  // the bundle's fields sit under <keys>, the registration beside it
        if (auto r = n.child("registration")) user.add(*r);
        user.add(*keys);
        if (!applyBundle(target, user)) return;
        send();
    } else if (count >= 2 || !store_.get("session", sessionKey(target))) {
        store_.del("session", sessionKey(target));
        ensureSessions({target}, send);
    } else {
        send();
    }
}

// ---------------------------------------------------------------- history and media

struct HttpResult {
    long status = 0;
    Bytes body;
};

size_t curlWrite(char* p, size_t s, size_t n, void* u) {
    auto* b = static_cast<Bytes*>(u);
    if (b->size() + s * n > (256u << 20)) return 0;
    b->insert(b->end(), p, p + s * n);
    return s * n;
}

HttpResult httpGet(const std::string& url) {
    HttpResult r;
    CURL* c = curl_easy_init();
    if (!c) return r;
    curl_slist* h = curl_slist_append(nullptr, "Origin: https://web.whatsapp.com");
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curlWrite);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 256L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
    if (curl_easy_perform(c) == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    return r;
}

// Decrypts the WhatsApp media envelope. Throws if any integrity check fails.
Bytes openMedia(const Bytes& data, const Bytes& mediaKey, const std::string& info, const Bytes& encSha, const Bytes& plainSha) {
    if (data.size() < 10) throw std::runtime_error("media too short");
    if (!encSha.empty() && !ctEqual(sha256(data), encSha)) throw std::runtime_error("media hash mismatch");
    Bytes keys = hkdf(mediaKey, {}, info, 112);
    Bytes iv(keys.begin(), keys.begin() + 16), ck(keys.begin() + 16, keys.begin() + 48), mk(keys.begin() + 48, keys.begin() + 80);
    Bytes file(data.begin(), data.end() - 10), mac(data.end() - 10, data.end());
    Bytes in = iv;
    append(in, file);
    Bytes h = hmacSha256(mk, in);
    h.resize(10);
    if (!ctEqual(h, mac)) throw std::runtime_error("media mac mismatch");
    Bytes pt = aesCbcDecrypt(ck, iv, file);
    if (!plainSha.empty() && !ctEqual(sha256(pt), plainSha)) throw std::runtime_error("media plaintext hash mismatch");
    return pt;
}

void RealEngine::ensureMediaConn(std::function<void(bool)> cb) {
    if (!cfg_.mediaBase.empty()) {
        cb(true);
        return;
    }
    if (!media_.hosts.empty() && now() < media_.expires) {
        cb(true);
        return;
    }
    iq("w:m", "set", {Node("media_conn")}, [this, cb](const Node* r) {
        const Node* mc = (r && r->attr("type") == "result") ? r->child("media_conn") : nullptr;
        if (!mc) {
            cb(false);
            return;
        }
        media_.hosts.clear();
        for (auto h : mc->children("host"))
            if (!h->attr("hostname").empty()) media_.hosts.push_back(h->attr("hostname"));
        media_.expires = now() + std::max(60, atoi(mc->attr("ttl").c_str()));
        cb(!media_.hosts.empty());
    });
}

void RealEngine::onHistory(const pb::Msg& notif, const std::string&) {
    histProgress_ = (int)notif.num(9);
    if (notif.has(11)) {
        // Parsing runs on a worker so a large payload cannot stall the socket.
        Bytes inline_ = notif.bytes(11);
        std::thread([this, inline_] {
            try {
                Bytes raw = zlibInflate(inline_.data(), inline_.size());
                posted([this, raw = std::move(raw)] { importHistory(raw); });
            } catch (const std::exception& e) {
                std::string why = e.what();
                posted([why] { logLine("history inflate failed: " + why); });
            }
        }).detach();
        return;
    }
    Bytes key = notif.bytes(3), enc = notif.bytes(4), sha = notif.bytes(1);
    std::string path = notif.str(5);
    if (path.empty() || key.empty()) return;
    ensureMediaConn([this, key, enc, sha, path](bool ok) {
        if (!ok) {
            logLine("no media connection for history sync");
            return;
        }
        std::vector<std::string> bases;
        if (!cfg_.mediaBase.empty()) bases.push_back(cfg_.mediaBase);
        else
            for (auto& h : media_.hosts) bases.push_back("https://" + h);
        std::string tail = path + "&hash=" + b64url(enc) + "&mms-type=md-msg-hist&__wa-mms=";
        std::thread([this, bases, tail, key, enc, sha] {
            for (auto& b : bases) {
                HttpResult r = httpGet(b + tail);
                if (r.status != 200) continue;
                try {
                    Bytes pt = openMedia(r.body, key, "WhatsApp History Keys", enc, sha);
                    Bytes raw = zlibInflate(pt.data(), pt.size());
                    posted([this, raw = std::move(raw)] { importHistory(raw); });
                    return;
                } catch (const std::exception& e) {
                    std::string why = e.what();
                    posted([why] { logLine("history blob failed: " + why); });
                }
            }
            posted([] { logLine("history download failed on every host"); });
        }).detach();
    });
}

void RealEngine::importHistory(const Bytes& raw) {
    try {
        pb::Msg h(raw);
        for (auto& m : h.repeated(15)) mapLid(Jid::parse(m.str(1)), Jid::parse(m.str(2)));
        Jid me = self();
        for (auto& pn : h.repeated(7)) {
            std::string id = pn.str(1), name = pn.str(2);
            Jid j = Jid::parse(id);
            if (j.empty()) continue;
            if (isSelf(j)) {
                if (!name.empty() && name != dev_.pushName) {
                    dev_.pushName = name;
                    saveDevice();
                }
            } else {
                setName(j, name);
            }
        }
        for (auto& c : h.repeated(2)) {
            Jid raw_ = Jid::parse(c.str(1));
            if (raw_.empty() || raw_.server == "broadcast" || raw_.server == "newsletter") continue;
            mapLid(Jid::parse(c.str(39)), raw_.isLid() ? raw_ : Jid{});
            mapLid(raw_.isPn() ? raw_ : Jid{}, Jid::parse(c.str(42)));
            Jid chat = raw_.isGroup() ? raw_ : canon(raw_);
            std::string cid = chat.str();
            std::string nm = c.str(38).empty() ? c.str(13) : c.str(38);
            if (!nm.empty()) setName(chat, nm);
            json row;
            if (auto ex = store_.getChat(cid)) row = *ex;
            else
                row = {{"id", cid}, {"name", ""}, {"is_group", chat.isGroup()}, {"unread", 0}, {"last_text", ""},
                       {"last_ts", 0}, {"last_from_me", false}, {"muted", false}, {"pinned", false}, {"archived", false}};
            row["name"] = nameOf(chat);
            row["archived"] = c.num(16) != 0;
            row["pinned"] = c.num(24) != 0;
            row["muted"] = (int64_t)c.num(25) > now() || c.num(25) == (uint64_t)-1;
            if (row["last_ts"].get<int64_t>() == 0) row["last_ts"] = (int64_t)std::max(c.num(5), c.num(12));
            store_.putChat(cid, row);
            histChats_++;
            for (auto& hm : c.repeated(2)) {
                auto wi = hm.sub(1);
                if (!wi) continue;
                auto key = wi->sub(1);
                if (!key || !wi->has(2)) continue;
                HistItem it;
                it.chat = chat;
                it.id = key->str(3);
                it.fromMe = key->num(2) != 0;
                std::string part = key->str(4);
                if (part.empty()) part = wi->str(5);
                it.sender = it.fromMe ? me.bare() : (part.empty() ? chat : Jid::parse(part));
                it.ts = (int64_t)wi->num(3);
                it.proto = wi->bytes(2);
                it.status = (int)wi->num(4);
                it.push = wi->str(19);
                if (!it.id.empty()) hist_.push_back(std::move(it));
            }
        }
    } catch (const std::exception& e) {
        logLine(std::string("history parse failed: ") + e.what());
    }
    ipc_.broadcast({{"event", "history_sync"}, {"state", "running"}, {"progress", histProgress_}, {"chats", histChats_}, {"messages", histMsgs_}});
    if (!histRunning_) {
        histRunning_ = true;
        later(0, [this] { importBatch(); });
    }
}

void RealEngine::importBatch() {
    store_.begin();
    try {
        for (int i = 0; i < 100 && !hist_.empty(); i++) {
            HistItem it = std::move(hist_.front());
            hist_.pop_front();
            try {
                ParsedMsg p = parseMessage(it.proto);
                if (p.kind.empty() || p.isProtocol || p.kind == "reaction") continue;
                if (!it.fromMe && !it.push.empty()) setName(it.sender.bare(), it.push);
                Jid sBare = canon(it.sender);
                static const char* st[] = {"failed", "pending", "sent", "delivered", "read", "read"};
                json m = {{"chat", it.chat.str()}, {"id", it.id}, {"from_me", it.fromMe}, {"sender", sBare.str()},
                          {"sender_name", it.fromMe ? "You" : nameOf(sBare)}, {"timestamp", it.ts}, {"kind", p.kind},
                          {"text", p.text}, {"status", it.fromMe ? st[std::min(it.status, 5)] : "read"},
                          {"quoted_id", p.quotedId}, {"media", p.media}};
                if (!p.secret.is_null()) m["_s"] = p.secret;
                if (store_.insertMessage(it.chat.str(), it.id, it.ts, false, m)) histMsgs_++;
                histTouched_.insert(it.chat.str());
            } catch (const std::exception&) {
            }
        }
        store_.commit();
    } catch (const std::exception& e) {
        logLine(std::string("history batch failed: ") + e.what());
    }
    if (!hist_.empty()) {
        later(5, [this] { importBatch(); });
        return;
    }
    for (auto& cid : histTouched_) {
        Jid c = Jid::parse(cid);
        auto row = store_.getChat(cid);
        if (!row) continue;
        if (auto m = store_.lastMessage(cid)) {
            (*row)["last_text"] = previewOf((*m)["kind"].get<std::string>(), (*m)["text"].get<std::string>());
            (*row)["last_ts"] = (*m)["timestamp"];
            (*row)["last_from_me"] = (*m)["from_me"];
        }
        (*row)["name"] = nameOf(c);
        overlayFlags(*row);
        store_.putChat(cid, *row);
    }
    histTouched_.clear();
    histRunning_ = false;
    ipc_.broadcast({{"event", "history_sync"}, {"state", "done"}, {"progress", histProgress_}, {"chats", histChats_}, {"messages", histMsgs_}});
}

void RealEngine::cmdDownload(const json& r, Reply rp) {
    std::string chat = r.at("chat").get<std::string>(), id = r.at("message_id").get<std::string>();
    auto m = store_.getMessage(chat, id);
    if (!m) return rp.err("not_found", "no such message");
    if (m->contains("media") && (*m)["media"].is_object()) {
        std::string done = (*m)["media"].value("path", "");
        if (!done.empty() && access(done.c_str(), R_OK) == 0) return rp.ok({{"path", done}});
    }
    if ((*m)["kind"] != "image" || !m->contains("_s")) return rp.err("unsupported", "only images can be downloaded");
    json s = (*m)["_s"];
    std::string path = s.value("path", "");
    if (path.empty()) return rp.err("not_found", "the message has no media path");
    if (conn_ != "connected") return rp.err("not_connected", "not connected");
    std::string mime = (*m)["media"].value("mime", "");
    ensureMediaConn([this, rp, chat, id, s, path, mime](bool ok) {
        if (!ok) return rp.err("timeout", "no media servers");
        Bytes key = b64dec(s["key"].get<std::string>()), enc = b64dec(s["enc"].get<std::string>()), sha = b64dec(s["sha"].get<std::string>());
        std::string info = s["type"];
        std::vector<std::string> bases;
        if (!cfg_.mediaBase.empty()) bases.push_back(cfg_.mediaBase);
        else
            for (auto& h : media_.hosts) bases.push_back("https://" + h);
        std::string tail = path + "&hash=" + b64url(enc) + "&mms-type=image&__wa-mms=";
        std::string fname = hex(sha256(B(chat + "|" + id))).substr(0, 40) + (mime.find("png") != std::string::npos ? ".png" : mime.find("webp") != std::string::npos ? ".webp" : ".jpg");
        std::string file = cfg_.dataDir + "/media/" + fname;
        std::thread([this, rp, chat, id, bases, tail, key, enc, sha, info, file] {
            std::string err = "download failed";
            bool stored = false;
            for (auto& b : bases) {
                HttpResult r2 = httpGet(b + tail);
                if (r2.status != 200) continue;
                try {
                    Bytes pt = openMedia(r2.body, key, info, enc, sha);
                    int fd = open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
                    if (fd < 0 || write(fd, pt.data(), pt.size()) != (ssize_t)pt.size()) {
                        if (fd >= 0) close(fd);
                        err = "could not write file";
                        break;
                    }
                    close(fd);
                    stored = true;
                    break;
                } catch (const std::exception& e) {
                    err = e.what();
                }
            }
            posted([this, rp, chat, id, file, stored, err] {
                if (!stored) return rp.err("send_failed", err);
                if (auto cur = store_.getMessage(chat, id)) {
                    (*cur)["media"]["path"] = file;
                    store_.updateMessage(chat, id, *cur);
                }
                rp.ok({{"path", file}});
            });
        }).detach();
    });
}

// ---------------------------------------------------------------- network

void RealEngine::watchNetwork() {
    nlFd_ = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (nlFd_ < 0) return;
    sockaddr_nl a{};
    a.nl_family = AF_NETLINK;
    a.nl_groups = RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR;
    if (bind(nlFd_, (sockaddr*)&a, sizeof a) != 0) {
        close(nlFd_);
        nlFd_ = -1;
        return;
    }
    loop_.add(nlFd_, EPOLLIN, [this](uint32_t) {
        char buf[8192];
        bool added = false;
        for (;;) {
            ssize_t n = recv(nlFd_, buf, sizeof buf, 0);
            if (n <= 0) break;
            for (auto* h = (nlmsghdr*)buf; NLMSG_OK(h, (unsigned)n); h = NLMSG_NEXT(h, n))
                if (h->nlmsg_type == RTM_NEWADDR) added = true;
        }
        if (added) networkChanged();
    });
}

// A new address means Wi-Fi or mobile data came back. Coalesced, as a join sends several.
void RealEngine::networkChanged() {
    if (netT_) return;
    netT_ = later(cfg_.rootKey ? 10 : 500, [this] {
        netT_ = 0;
        if (!wanted_) return;
        if (reconnectT_ || conn_ == "offline") {
            attempts_ = 0;
            connect();
        } else if (hs_ == Hs::Done) {
            // The old socket may be dead on the old network. Ask, and replace it if silent.
            uint64_t gen = gen_;
            iq("w:p", "get", {}, [this, gen](const Node* r) {
                if (r || gen != gen_) return;
                ws_.close();
                wsClosed("no answer after network change");
                attempts_ = 0;
            });
        }
    });
}

// ---------------------------------------------------------------- app state

void RealEngine::syncAppState(std::vector<std::string> names) {
    for (auto& n : names) {
        if (n.empty()) continue;
        if (std::find(asQueue_.begin(), asQueue_.end(), n) == asQueue_.end()) asQueue_.push_back(n);
    }
    if (asRunning_ || asQueue_.empty() || hs_ != Hs::Done) return;
    asRunning_ = true;
    asContacts_ = false;
    ipc_.broadcast({{"event", "app_state"}, {"state", "running"}});
    appStateNext();
}

void RealEngine::appStateNext() {
    if (asQueue_.empty()) {
        asRunning_ = false;
        ipc_.broadcast({{"event", "app_state"}, {"state", "done"}});
        if (asContacts_) {
            ipc_.broadcast({{"event", "contacts"}, {"total", store_.allContacts().size()}});
            asContacts_ = false;
        }
        return;
    }
    std::string name = asQueue_.front();
    asQueue_.pop_front();
    appStateFetch(name, !store_.get("asver", name));
}

void RealEngine::appStateFetch(const std::string& name, bool snapshot) {
    Node col("collection");
    col.a("name", name).a("return_snapshot", snapshot ? "true" : "false");
    if (!snapshot) {
        json v = json::parse(S(*store_.get("asver", name)));
        col.a("version", std::to_string(v["v"].get<uint64_t>()));
    }
    Node sync("sync");
    sync.add(col);
    iq("w:sync:app:state", "set", {sync}, [this, name, snapshot](const Node* r) { appStateReply(name, r, snapshot); });
}

// Fetches one external blob, verifying it. Worker threads only.
static std::optional<Bytes> fetchBlob(const pb::Msg& ref, const std::vector<std::string>& bases) {
    Bytes key = ref.bytes(1), sha = ref.bytes(5), enc = ref.bytes(6);
    std::string tail = ref.str(2) + "&hash=" + b64url(enc) + "&mms-type=md-app-state&__wa-mms=";
    for (auto& b : bases) {
        HttpResult r = httpGet(b + tail);
        if (r.status != 200) continue;
        try {
            return openMedia(r.body, key, "WhatsApp App State Keys", enc, sha);
        } catch (const std::exception&) {
        }
    }
    return std::nullopt;
}

void RealEngine::appStateReply(const std::string& name, const Node* r, bool) {
    const Node* sync = (r && r->attr("type") == "result") ? r->child("sync") : nullptr;
    const Node* col = sync ? sync->child("collection") : nullptr;
    if (!col) {
        logLine("app state fetch failed for " + name);
        appStateNext();
        return;
    }
    bool hasMore = col->attr("has_more_patches") == "true";
    std::optional<pb::Msg> snapRef;
    std::vector<pb::Msg> patches;
    try {
        if (auto s = col->child("snapshot")) snapRef = pb::Msg(s->data);
        if (auto ps = col->child("patches"))
            for (auto p : ps->children("patch")) patches.emplace_back(p->data);
    } catch (const std::exception&) {
        logLine("bad app state reply for " + name);
        appStateNext();
        return;
    }
    bool external = snapRef.has_value();
    for (auto& p : patches) external = external || p.has(3);
    auto finish = [this, name, hasMore](std::optional<Bytes> snap, std::vector<std::pair<Bytes, std::vector<pb::Msg>>> out) {
        appStateApply(name, snap.value_or(Bytes{}), out, hasMore);
    };
    // Patches are re-serialised by keeping each field's bytes, so the original is held for MACs.
    std::vector<Bytes> rawPatches;
    if (auto ps = col->child("patches"))
        for (auto p : ps->children("patch")) rawPatches.push_back(p->data);
    if (!external) {
        std::vector<std::pair<Bytes, std::vector<pb::Msg>>> out;
        for (size_t i = 0; i < patches.size(); i++) out.push_back({rawPatches[i], patches[i].repeated(2)});
        finish(std::nullopt, out);
        return;
    }
    ensureMediaConn([this, name, snapRef, patches, rawPatches, finish](bool ok) {
        if (!ok) {
            appStateNext();
            return;
        }
        std::vector<std::string> bases;
        if (!cfg_.mediaBase.empty()) bases.push_back(cfg_.mediaBase);
        else
            for (auto& h : media_.hosts) bases.push_back("https://" + h);
        std::thread([this, name, snapRef, patches, rawPatches, finish, bases] {
            std::optional<Bytes> snap;
            std::vector<std::pair<Bytes, std::vector<pb::Msg>>> out;
            bool good = true;
            try {
                if (snapRef) {
                    snap = fetchBlob(*snapRef, bases);
                    good = snap.has_value();
                }
                for (size_t i = 0; good && i < patches.size(); i++) {
                    std::vector<pb::Msg> muts = patches[i].repeated(2);
                    if (auto ext = patches[i].sub(3)) {
                        auto b = fetchBlob(*ext, bases);
                        if (!b) {
                            good = false;
                            break;
                        }
                        muts = pb::Msg(*b).repeated(1);
                    }
                    out.push_back({rawPatches[i], std::move(muts)});
                }
            } catch (const std::exception&) {
                good = false;
            }
            posted([this, name, good, snap, out, finish] {
                if (!good) {
                    logLine("app state download failed for " + name);
                    appStateNext();
                    return;
                }
                finish(snap, out);
            });
        }).detach();
    });
}

void RealEngine::appStateApply(const std::string& name, const Bytes& snapshot,
                               const std::vector<std::pair<Bytes, std::vector<pb::Msg>>>& patches, bool hasMore) {
    appstate::HashState st;
    if (auto v = store_.get("asver", name)) {
        json j = json::parse(S(*v));
        st.version = j["v"];
        st.hash = b64dec(j["h"].get<std::string>());
    }
    appstate::Env env;
    env.key = [this](const Bytes& id) { return store_.get("askey", hex(id)); };
    env.prevValueMac = [this, name](const Bytes& i) { return store_.get("asmac", name + "|" + hex(i)); };
    store_.begin();
    bool ok = true;
    auto commitMuts = [&](const std::vector<appstate::Mutation>& ms) {
        for (auto& m : ms) {
            std::string k = name + "|" + hex(m.indexMac);
            if (m.set) store_.put("asmac", k, m.valueMac, name);
            else store_.del("asmac", k);
            try {
                applyMutation(m);
            } catch (const std::exception& e) {
                logLine(std::string("skipped one app state change: ") + e.what());
            }
        }
    };
    try {
        if (!snapshot.empty()) {
            store_.delGroup("asmac", name);
            commitMuts(appstate::decodeSnapshot(name, snapshot, st, env));
        }
        for (auto& [raw, muts] : patches) {
            pb::Msg patch(raw);
            commitMuts(appstate::decodePatch(name, patch, muts, st, env));
        }
        store_.put("asver", name, B(json{{"v", st.version}, {"h", b64(st.hash)}}.dump()));
    } catch (const appstate::MissingKey&) {
        // Nothing was committed. The phone's next key share restarts this collection.
        ok = false;
        store_.rollback();
        logLine("app state key not held yet for " + name);
        appStateNext();
        return;
    } catch (const std::exception& e) {
        ok = false;
        store_.rollback();
        logLine(std::string("app state rejected for ") + name + ": " + e.what());
        // One clean restart from a snapshot, then give up until the next notification.
        if (!asRetried_.count(name)) {
            asRetried_.insert(name);
            store_.del("asver", name);
            store_.delGroup("asmac", name);
            appStateFetch(name, true);
        } else {
            appStateNext();
        }
        return;
    }
    if (ok) store_.commit();
    if (hasMore) appStateFetch(name, false);
    else {
        asRetried_.erase(name);
        appStateNext();
    }
}

void RealEngine::overlayFlags(json& c) {
    auto v = store_.get("chatflags", c["id"].get<std::string>());
    if (!v) return;
    json f = json::parse(S(*v));
    if (f.contains("pinned")) c["pinned"] = f["pinned"];
    if (f.contains("archived")) c["archived"] = f["archived"];
    if (f.contains("muted_until")) {
        int64_t u = f["muted_until"];
        c["muted"] = u != 0 && (u < 0 || u > now() * 1000);
    }
}

void RealEngine::setChatFlag(const Jid& chat, const char* flag, const json& value) {
    std::string cid = chat.str();
    json f = json::object();
    if (auto v = store_.get("chatflags", cid)) f = json::parse(S(*v));
    f[flag] = value;
    store_.put("chatflags", cid, B(f.dump()));
    if (store_.getChat(cid)) touchChat(chat, "");
}

void RealEngine::loadStarred() {
    starred_.clear();
    if (auto v = store_.get("meta", "starred"))
        for (auto& s : json::parse(S(*v))) starred_.insert(s.get<std::string>());
}

void RealEngine::saveStarred() { store_.put("meta", "starred", B(json(starred_).dump())); }

void RealEngine::applyMutation(const appstate::Mutation& m) {
    if (m.index.empty()) return;
    pb::Msg act(m.action);
    const std::string& kind = m.index[0];
    Jid jid = m.index.size() > 1 ? Jid::parse(m.index[1]) : Jid{};
    if (kind == "setting_pushName") {
        if (!m.set) return;
        auto s = act.sub(7);
        if (s && !s->str(1).empty() && s->str(1) != dev_.pushName) {
            dev_.pushName = s->str(1);
            saveDevice();
        }
        return;
    }
    if (jid.empty()) return;
    if (kind == "contact" || kind == "lid_contact") {
        if (!m.set) return;
        auto c = act.sub(3);
        if (!c) return;
        if (jid.isLid()) mapLid(Jid::parse(c->str(5)), jid);
        if (jid.isPn()) mapLid(jid, Jid::parse(c->str(3)));
        Jid can = canon(jid);
        if (isSelf(can)) return;
        std::string nm = c->str(1).empty() ? c->str(2) : c->str(1);
        if (!nm.empty()) store_.put("cname", can.str(), B(nm));
        std::string push;
        if (auto v = store_.get("name", can.str())) push = S(*v);
        store_.putContact(can.str(), {{"jid", can.str()}, {"name", nameOf(can)}, {"push_name", push},
                                      {"phone", can.isPn() ? "+" + can.user : ""}});
        asContacts_ = true;
        if (store_.getChat(can.str())) touchChat(can, "");
    } else if (kind == "mute") {
        auto a = act.sub(4);
        int64_t until = 0;
        if (m.set && a && a->num(1)) until = (int64_t)a->num(2) == 0 ? -1 : (int64_t)a->num(2);
        setChatFlag(jid.isGroup() ? jid : canon(jid), "muted_until", until);
    } else if (kind == "pin_v1") {
        auto a = act.sub(5);
        setChatFlag(jid.isGroup() ? jid : canon(jid), "pinned", m.set && a && a->num(1));
    } else if (kind == "archive") {
        auto a = act.sub(17);
        setChatFlag(jid.isGroup() ? jid : canon(jid), "archived", m.set && a && a->num(1));
    } else if (kind == "star" && m.index.size() >= 3) {
        auto a = act.sub(2);
        std::string k = (jid.isGroup() ? jid : canon(jid)).str() + "|" + m.index[2];
        if (m.set && a && a->num(1)) starred_.insert(k);
        else starred_.erase(k);
        saveStarred();
    }
}

// ---------------------------------------------------------------- status

static const char* const kStatusChat = "status@broadcast";

void RealEngine::storeStatus(const Jid& sender, bool fromMe, int64_t ts, const std::string& id, ParsedMsg& p, bool live,
                             const std::string& push) {
    if (p.kind == "undecryptable" || p.kind == "reaction" || p.kind.empty()) return;
    Jid sBare = canon(sender);
    if (!fromMe && !push.empty()) setName(sBare, push);
    json m = {{"chat", kStatusChat}, {"id", id}, {"from_me", fromMe}, {"sender", sBare.str()},
              {"sender_name", fromMe ? "You" : nameOf(sBare)}, {"timestamp", ts}, {"kind", p.kind}, {"text", p.text},
              {"status", fromMe ? "sent" : "received"}, {"quoted_id", ""}, {"media", p.media}, {"seen", fromMe}};
    if (!p.secret.is_null()) m["_s"] = p.secret;
    if (!store_.insertMessage(kStatusChat, id, ts, false, m)) return;
    json pub = m;
    pub.erase("_s");
    ipc_.broadcast({{"event", "status"}, {"sender", sBare.str()}, {"sender_name", pub["sender_name"]}, {"item", pub}, {"live", live}});
}

json RealEngine::statusList() {
    bool more = false;
    auto rows = store_.pageMessages(kStatusChat, 500, std::nullopt, "", &more);
    std::map<std::string, json> by;
    int64_t cutoff = now() - 86400;
    for (auto& m : rows) {
        if (m["timestamp"].get<int64_t>() < cutoff) continue;
        m.erase("_s");
        std::string s = m["sender"];
        if (!by.count(s)) by[s] = {{"sender", s}, {"name", m["sender_name"]}, {"last_ts", 0}, {"unseen", 0}, {"items", json::array()}};
        json& c = by[s];
        if (m["timestamp"].get<int64_t>() > c["last_ts"].get<int64_t>()) c["last_ts"] = m["timestamp"];
        if (!m.value("seen", false)) c["unseen"] = c["unseen"].get<int>() + 1;
        c["items"].push_back(m);
    }
    std::vector<json> v;
    for (auto& [_, c] : by) v.push_back(c);
    std::sort(v.begin(), v.end(), [](const json& a, const json& b) { return a["last_ts"].get<int64_t>() > b["last_ts"].get<int64_t>(); });
    return v;
}

void RealEngine::cmdStatusSeen(const json& r, Reply rp) {
    if (hs_ != Hs::Done) return rp.err("not_connected", "not connected");
    Jid sender = Jid::parse(r.at("sender").get<std::string>());
    std::vector<std::string> ids;
    for (auto& i : r.at("ids")) {
        std::string id = i.get<std::string>();
        auto m = store_.getMessage(kStatusChat, id);
        if (!m) continue;
        (*m)["seen"] = true;
        store_.updateMessage(kStatusChat, id, *m);
        ids.push_back(id);
    }
    if (!ids.empty()) {
        Node n("receipt");
        n.a("id", ids[0]).a("type", readReceipts_ ? "read" : "read-self").a("to", Jid::parse(kStatusChat)).a("t", std::to_string(now()));
        n.a("participant", sender.bare());
        if (ids.size() > 1) {
            Node l("list");
            for (size_t i = 1; i < ids.size(); i++) l.add(Node("item").a("id", ids[i]));
            n.add(std::move(l));
        }
        sendNode(n);
    }
    rp.ok(json::object());
}

// Honours the account's status privacy. If that cannot be read, nothing is posted.
void RealEngine::statusAudience(std::function<void(std::optional<std::vector<Jid>>)> cb) {
    iq("status", "get", {Node("privacy")}, [this, cb](const Node* r) {
        const Node* pr = (r && r->attr("type") == "result") ? r->child("privacy") : nullptr;
        const Node* def = nullptr;
        if (pr)
            for (auto l : pr->children("list"))
                if (l->attr("default") == "true" || !def) {
                    def = l;
                    if (l->attr("default") == "true") break;
                }
        if (!def) {
            cb(std::nullopt);
            return;
        }
        std::string type = def->attr("type");
        std::set<std::string> listed;
        for (auto u : def->children("user")) listed.insert(canon(u->jid("jid")).str());
        std::vector<Jid> out{self().bare()};
        if (type == "whitelist") {
            for (auto& s : listed) out.push_back(Jid::parse(s));
        } else if (type == "contacts" || type == "blacklist") {
            for (auto& c : store_.allContacts()) {
                std::string j = c["jid"];
                if (!listed.count(j)) out.push_back(Jid::parse(j));
            }
        } else {
            cb(std::nullopt);
            return;
        }
        cb(out);
    });
}

void RealEngine::cmdPostStatus(const json& r, Reply rp) {
    if (!paired()) return rp.err("not_paired", "not paired");
    std::string text = r.at("text").get<std::string>();
    if (text.empty() || text.size() > 700) return rp.err("bad_request", "bad text");
    Pending p;
    p.chat = Jid::parse(kStatusChat);
    std::string h = hex(randomBytes(9));
    for (auto& c : h) c = (char)toupper(c);
    p.id = "3EB0" + h;
    p.proto = buildStatusText(text);
    p.ts = now();
    ParsedMsg pm;
    pm.kind = "text";
    pm.text = text;
    storeStatus(self(), true, p.ts, p.id, pm, false, "");
    rp.ok({{"id", p.id}, {"timestamp", p.ts}});
    enqueueSend(std::move(p));
}

// ---------------------------------------------------------------- contacts, pictures, groups, search

void RealEngine::cmdContacts(const json& r, Reply rp) {
    std::string q = r.value("query", "");
    for (auto& c : q) c = (char)tolower((unsigned char)c);
    std::vector<json> all = store_.allContacts(), hit;
    for (auto& c : all) {
        std::string n = c.value("name", "") + " " + c.value("push_name", "") + " " + c.value("phone", "");
        for (auto& x : n) x = (char)tolower((unsigned char)x);
        if (q.empty() || n.find(q) != std::string::npos) {
            c["name"] = nameOf(Jid::parse(c["jid"].get<std::string>()));
            hit.push_back(c);
        }
    }
    std::sort(hit.begin(), hit.end(), [](const json& a, const json& b) { return a["name"].get<std::string>() < b["name"].get<std::string>(); });
    int total = (int)hit.size(), off = r.value("offset", 0), lim = r.value("limit", 200);
    json out = json::array();
    for (int i = off; i < total && (int)out.size() < lim; i++) out.push_back(hit[(size_t)i]);
    rp.ok({{"contacts", out}, {"total", total}});
}

void RealEngine::cmdPicture(const json& r, Reply rp) {
    if (hs_ != Hs::Done) return rp.err("not_connected", "not connected");
    std::string js = r.at("jid").get<std::string>();
    Jid j = Jid::parse(js);
    if (j.empty() || j.user.empty()) return rp.err("bad_request", "bad jid");
    bool preview = r.value("preview", true);
    std::string file = cfg_.dataDir + "/media/pic-" + hex(sha256(B(js + (preview ? "|p" : "|f")))).substr(0, 40) + ".jpg";
    if (store_.get("picture", j.bare().str()) && access(file.c_str(), R_OK) == 0) return rp.ok({{"path", file}});
    Node pic("picture");
    pic.a("query", "url").a("type", preview ? "preview" : "image");
    if (auto tok = store_.get("tctoken", j.bare().str())) pic.add(Node("tctoken").bytes(*tok));
    iq("w:profile:picture", "get", {pic}, [this, rp, file, j](const Node* resp) {
        const Node* p = (resp && resp->attr("type") == "result") ? resp->child("picture") : nullptr;
        if (!resp) return rp.err("timeout", "no answer");
        std::string url = p ? p->attr("url") : "";
        // Only WhatsApp's own https links, unless a local test server stands in.
        if (url.empty() || (url.rfind("https://", 0) != 0 && cfg_.mediaBase.empty())) return rp.err("not_found", "no picture");
        std::thread([this, rp, file, url, j] {
            HttpResult h = httpGet(url);
            bool good = false;
            if (h.status == 200 && !h.body.empty() && h.body.size() < (8u << 20)) {
                int fd = open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
                if (fd >= 0) {
                    good = write(fd, h.body.data(), h.body.size()) == (ssize_t)h.body.size();
                    close(fd);
                }
            }
            posted([this, rp, file, good, j] {
                if (!good) return rp.err("not_found", "could not fetch the picture");
                store_.put("picture", j.bare().str(), B("1"));
                rp.ok({{"path", file}});
            });
        }).detach();
    }, kServerJid, j.bare());
}

void RealEngine::cmdGroupInfo(const json& r, Reply rp) {
    if (hs_ != Hs::Done) return rp.err("not_connected", "not connected");
    std::string gid = r.at("chat").get<std::string>();
    Jid g = Jid::parse(gid);
    if (!g.isGroup()) return rp.err("bad_request", "not a group");
    fetchGroup(g, [this, rp, gid](bool ok) {
        auto it = groups_.find(gid);
        if (!ok || it == groups_.end()) return rp.err("not_found", "could not fetch the group");
        rp.ok(it->second.info);
    });
}

void RealEngine::cmdSearch(const json& r, Reply rp) {
    std::string q = r.at("query").get<std::string>();
    if (q.empty()) return rp.err("bad_request", "empty query");
    std::optional<int64_t> before;
    if (r.contains("before_ts")) before = r["before_ts"].get<int64_t>();
    auto job = store_.searchJob(q, r.value("chat", ""), std::min(r.value("limit", 50), 500), before);
    std::thread([this, job, rp] {
        bool more = false;
        auto rows = job(&more);
        posted([rows = std::move(rows), more, rp] { rp.ok({{"messages", rows}, {"more", more}}); });
    }).detach();
}

void RealEngine::cmdStarred(const json& r, Reply rp) {
    std::vector<json> out;
    for (auto& k : starred_) {
        auto bar = k.find('|');
        auto m = store_.getMessage(k.substr(0, bar), k.substr(bar + 1));
        if (!m) continue;
        m->erase("_s");
        (*m)["starred"] = true;
        out.push_back(*m);
    }
    std::sort(out.begin(), out.end(), [](const json& a, const json& b) { return a["timestamp"].get<int64_t>() > b["timestamp"].get<int64_t>(); });
    int lim = r.value("limit", 100);
    if ((int)out.size() > lim) out.resize((size_t)lim);
    rp.ok({{"messages", out}});
}

// ---------------------------------------------------------------- commands

void RealEngine::markRead(const json& r, Reply rp) {
    std::string chat = r.at("chat").get<std::string>();
    std::vector<json> msgs = store_.unreadMessages(chat);
    std::set<std::string> only;
    if (r.contains("ids") && r["ids"].is_array())
        for (auto& i : r["ids"]) only.insert(i.get<std::string>());
    std::map<std::string, std::vector<std::string>> bySender;
    int marked = 0;
    for (auto& m : msgs) {
        std::string id = m["id"];
        if (!only.empty() && !only.count(id)) continue;
        store_.clearUnread(chat, id);
        marked++;
        bySender[m["sender"].get<std::string>()].push_back(id);
    }
    Jid cj = Jid::parse(chat);
    if (marked && hs_ == Hs::Done) {
        for (auto& [sender, ids] : bySender) {
            Node n("receipt");
            n.a("id", ids[0]).a("type", readReceipts_ ? "read" : "read-self").a("to", cj).a("t", std::to_string(now()));
            if (cj.isGroup()) n.a("participant", Jid::parse(sender).bare());
            if (ids.size() > 1) {
                Node l("list");
                for (size_t i = 1; i < ids.size(); i++) l.add(Node("item").a("id", ids[i]));
                n.add(std::move(l));
            }
            sendNode(n);
        }
    }
    if (marked) touchChat(cj, "");
    rp.ok({{"marked", marked}});
}

void RealEngine::command(const std::string& cmd, const json& r, Reply rp) {
    if (cmd == "status") {
        rp.ok({{"ipc_version", kIpcVersion}, {"connection", connectionEv()}, {"pairing", pairingEv()}, {"self", dev_.jid},
               {"push_name", dev_.pushName}});
    } else if (cmd == "pair_qr") {
        if (paired()) return rp.err("busy", "already paired");
        qrFlow_ = true;
        currentQr_.clear();
        pairError_.clear();
        rp.ok(json::object());
        if (!wanted_ || conn_ == "offline" || conn_ == "logged_out") connect();
        else if (refsReady_) showQr();
    } else if (cmd == "pair_phone") {
        if (paired()) return rp.err("busy", "already paired");
        std::string digits;
        for (char c : r.at("phone").get<std::string>())
            if (c >= '0' && c <= '9') digits += c;
        if (digits.size() <= 6 || digits[0] == '0') return rp.err("bad_request", "use the full number with country code");
        phoneWanted_ = digits;
        phoneReply_ = rp;
        if (!wanted_ || conn_ == "offline" || conn_ == "logged_out") connect();
        else if (refsReady_) startPhonePairing();
    } else if (cmd == "pair_cancel") {
        if (!paired()) {
            qrFlow_ = false;
            phoneWanted_.reset();
            link_.reset();
            if (phoneReply_) {
                phoneReply_->err("timeout", "cancelled");
                phoneReply_.reset();
            }
            disconnect();
            pairState_ = "unpaired";
            pairError_.clear();
            currentQr_.clear();
            emitPairing();
        }
        rp.ok(json::object());
    } else if (cmd == "logout") {
        if (!paired()) return rp.ok(json::object());
        if (hs_ == Hs::Done) {
            Node rm("remove-companion-device");
            rm.a("jid", self()).a("reason", "user_initiated");
            iq("md", "set", {rm}, nullptr);
        }
        wipe();
        rp.ok(json::object());
    } else if (cmd == "chats") {
        int total = 0;
        json chats = store_.listChats(r.value("limit", 100), r.value("offset", 0), &total);
        rp.ok({{"chats", chats}, {"total", total}});
    } else if (cmd == "messages") {
        bool more = false;
        std::optional<int64_t> ts;
        if (r.contains("before_ts")) ts = r["before_ts"].get<int64_t>();
        auto page = store_.pageMessages(r.at("chat").get<std::string>(), std::min(r.value("limit", 50), 500), ts, r.value("before_id", ""), &more);
        std::string pc = r.at("chat").get<std::string>();
        for (auto& m : page) {
            m.erase("_s");
            if (starred_.count(pc + "|" + m.value("id", ""))) m["starred"] = true;
        }
        rp.ok({{"messages", page}, {"more", more}});
    } else if (cmd == "send_text") {
        if (!paired()) return rp.err("not_paired", "not paired");
        std::string chat = r.at("chat").get<std::string>(), text = r.at("text").get<std::string>();
        Jid cj = Jid::parse(chat);
        if (cj.empty() || cj.user.empty() || text.empty() || text.size() > 65536) return rp.err("bad_request", "bad chat or text");
        std::string quoted = r.value("quoted_id", ""), qs;
        if (!quoted.empty())
            if (auto q = store_.getMessage(chat, quoted)) qs = (*q)["sender"];
        Pending p;
        p.chat = cj;
        p.id = "3EB0" + [] { std::string h = hex(randomBytes(9)); for (auto& c : h) c = (char)toupper(c); return h; }();
        p.proto = buildTextMessage(text, quoted, qs);
        p.ts = now();
        Jid me = self().bare();
        json m = {{"chat", chat}, {"id", p.id}, {"from_me", true}, {"sender", me.str()}, {"sender_name", "You"},
                  {"timestamp", p.ts}, {"kind", "text"}, {"text", text}, {"status", "pending"}, {"quoted_id", quoted}, {"media", nullptr}};
        store_.insertMessage(chat, p.id, p.ts, false, m);
        touchChat(cj, "");
        rp.ok({{"id", p.id}, {"timestamp", p.ts}});
        ipc_.broadcast({{"event", "message"}, {"chat", chat}, {"chat_name", nameOf(cj)}, {"sender", me.str()}, {"sender_name", "You"},
                        {"text", text}, {"timestamp", p.ts}, {"id", p.id}, {"from_me", true}, {"is_group", cj.isGroup()},
                        {"muted", false}, {"live", true}, {"message", m}});
        enqueueSend(std::move(p));
    } else if (cmd == "statuses") {
        rp.ok({{"contacts", statusList()}});
    } else if (cmd == "status_seen") {
        cmdStatusSeen(r, rp);
    } else if (cmd == "post_status") {
        cmdPostStatus(r, rp);
    } else if (cmd == "contacts") {
        cmdContacts(r, rp);
    } else if (cmd == "profile_picture") {
        cmdPicture(r, rp);
    } else if (cmd == "group_info") {
        cmdGroupInfo(r, rp);
    } else if (cmd == "search") {
        cmdSearch(r, rp);
    } else if (cmd == "starred") {
        cmdStarred(r, rp);
    } else if (cmd == "_test_network" && cfg_.rootKey) {
        networkChanged();
        rp.ok(json::object());
    } else if (cmd == "mark_read") {
        markRead(r, rp);
    } else if (cmd == "download_media") {
        cmdDownload(r, rp);
    } else if (cmd == "typing") {
        if (hs_ != Hs::Done) return rp.err("not_connected", "not connected");
        std::string st = r.at("state").get<std::string>();
        if (st != "composing" && st != "paused") return rp.err("bad_request", "state");
        Node n("chatstate");
        n.a("to", Jid::parse(r.at("chat").get<std::string>())).add(Node(st));
        sendNode(n);
        rp.ok(json::object());
    } else if (cmd == "set_presence") {
        active_ = r.at("available").get<bool>();
        if (hs_ == Hs::Done && !dev_.pushName.empty()) {
            Node p("presence");
            p.a("type", active_ ? "available" : "unavailable").a("name", dev_.pushName);
            sendNode(p);
        }
        rp.ok(json::object());
    } else if (cmd == "subscribe_presence") {
        if (hs_ != Hs::Done) return rp.err("not_connected", "not connected");
        Jid to = Jid::parse(r.at("chat").get<std::string>());
        Node n("presence");
        n.a("type", "subscribe").a("to", to);
        if (auto tok = store_.get("tctoken", to.bare().str())) n.add(Node("tctoken").bytes(*tok));
        sendNode(n);
        rp.ok(json::object());
    } else {
        rp.err("unknown_cmd", "unknown command");
    }
}

}  // namespace

std::unique_ptr<Engine> makeRealEngine(Loop& l, IpcServer& ipc, const RealConfig& cfg) {
    static bool once = [] {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        return true;
    }();
    (void)once;
    return std::make_unique<RealEngine>(l, ipc, cfg);
}

}  // namespace zw
