// `ztwhatsappd --fake`: the IPC contract with invented data and no network or disk.
// It exists so the UI and notifier can be built without a real account.
#include <ctime>
#include <deque>

#include "engine.h"

namespace zw {

namespace {
class FakeEngine : public Engine {
public:
    FakeEngine(Loop& l, IpcServer& ipc) : loop_(l), ipc_(ipc) {
        add("61400000001@s.whatsapp.net", "Sam Example", false);
        add("61400000002@s.whatsapp.net", "Alex Test", false);
        add("120363000000000001@g.us", "Family (fake)", true);
        int64_t t = time(nullptr) - 7200;
        for (auto& [id, c] : chats_)
            for (int i = 0; i < 80; i++) {
                bool me = i % 3 == 0;
                std::string sender = me ? kSelf : (c["is_group"].get<bool>() ? "61400000001@s.whatsapp.net" : id);
                push(id, sender, me, "Fake message " + std::to_string(i + 1), t + i * 60, false);
            }
        timer();
    }

    std::vector<json> greeting() override { return {connection(), pairing()}; }

    void command(const std::string& cmd, const json& r, Reply rp) override {
        if (cmd == "status") {
            rp.ok({{"ipc_version", kIpcVersion}, {"connection", connection()}, {"pairing", pairing()},
                   {"self", paired_ ? kSelf : ""}, {"push_name", paired_ ? "Fake Me" : ""}});
        } else if (cmd == "pair_qr") {
            rp.ok(json::object());
            state_ = "qr";
            ipc_.broadcast(pairing());
            // Pretend the phone scans after a short wait.
            loop_.after(8000, [this] { finish(); });
        } else if (cmd == "pair_phone") {
            code_ = "ABCD-EFGH";
            state_ = "phone_code";
            rp.ok({{"code", code_}});
            ipc_.broadcast(pairing());
            loop_.after(8000, [this] { finish(); });
        } else if (cmd == "pair_cancel") {
            state_ = paired_ ? "paired" : "unpaired";
            rp.ok(json::object());
            ipc_.broadcast(pairing());
        } else if (cmd == "logout") {
            paired_ = false;
            state_ = "unpaired";
            rp.ok(json::object());
            ipc_.broadcast(connection());
            ipc_.broadcast(pairing());
        } else if (cmd == "chats") {
            int limit = r.value("limit", 100), off = r.value("offset", 0), i = 0;
            json out = json::array();
            for (auto& [id, c] : chats_)
                if (i++ >= off && (int)out.size() < limit) out.push_back(c);
            rp.ok({{"chats", out}, {"total", chats_.size()}});
        } else if (cmd == "messages") {
            auto it = msgs_.find(r.at("chat").get<std::string>());
            if (it == msgs_.end()) return rp.err("not_found", "no such chat");
            int limit = r.value("limit", 50);
            int64_t before = r.value("before_ts", (int64_t)INT64_MAX);
            json page = json::array();
            bool more = false;
            auto& v = it->second;
            for (int i = (int)v.size() - 1; i >= 0; i--) {
                if (v[i]["timestamp"].get<int64_t>() >= before) continue;
                if ((int)page.size() == limit) {
                    more = true;
                    break;
                }
                page.insert(page.begin(), v[i]);
            }
            rp.ok({{"messages", page}, {"more", more}});
        } else if (cmd == "send_text") {
            std::string chat = r.at("chat").get<std::string>();
            if (!chats_.count(chat)) return rp.err("not_found", "no such chat");
            std::string id = push(chat, kSelf, true, r.at("text").get<std::string>(), time(nullptr), true);
            rp.ok({{"id", id}, {"timestamp", (int64_t)time(nullptr)}});
            for (auto [ms, st] : {std::pair{500, "sent"}, {1500, "delivered"}, {4000, "read"}})
                loop_.after(ms, [this, chat, id, st = std::string(st)] {
                    ipc_.broadcast({{"event", "receipt"}, {"chat", chat}, {"ids", {id}}, {"status", st}, {"sender", ""}});
                });
        } else if (cmd == "mark_read") {
            std::string chat = r.at("chat").get<std::string>();
            int n = chats_.count(chat) ? chats_[chat]["unread"].get<int>() : 0;
            if (n) {
                chats_[chat]["unread"] = 0;
                ipc_.broadcast({{"event", "chat"}, {"chat", chats_[chat]}});
            }
            rp.ok({{"marked", n}});
        } else if (cmd == "download_media") {
            rp.err("unsupported", "the fake engine has no media");
        } else if (cmd == "typing" || cmd == "set_presence" || cmd == "subscribe_presence") {
            rp.ok(json::object());
        } else {
            rp.err("unknown_cmd", "unknown command");
        }
    }

private:
    static constexpr const char* kSelf = "61400000000@s.whatsapp.net";

    json connection() {
        return {{"event", "connection"}, {"state", paired_ ? "connected" : "offline"}, {"detail", "fake"}};
    }
    json pairing() {
        json j = {{"event", "pairing"}, {"state", state_}, {"qr", ""}, {"expires_in", 0}, {"code", ""}, {"error", ""}};
        if (state_ == "qr") {
            j["qr"] = "https://example.invalid/ztwhatsapp-fake-qr";
            j["expires_in"] = 20;
        }
        if (state_ == "phone_code") j["code"] = code_;
        if (state_ == "paired") j["self"] = kSelf;
        return j;
    }
    void finish() {
        if (state_ != "qr" && state_ != "phone_code") return;
        paired_ = true;
        state_ = "paired";
        ipc_.broadcast(pairing());
        ipc_.broadcast(connection());
        ipc_.broadcast({{"event", "history_sync"}, {"state", "done"}, {"progress", 100}, {"chats", chats_.size()}, {"messages", 240}});
    }
    void add(const std::string& id, const std::string& name, bool group) {
        chats_[id] = {{"id", id}, {"name", name}, {"is_group", group}, {"unread", 0}, {"last_text", ""},
                      {"last_ts", 0}, {"last_from_me", false}, {"muted", false}, {"pinned", false}, {"archived", false}};
    }
    std::string push(const std::string& chat, const std::string& sender, bool me, const std::string& text, int64_t ts, bool live) {
        std::string id = "FAKE" + std::to_string(++seq_);
        auto& c = chats_[chat];
        std::string sname = me ? "You" : (sender == chat ? c["name"].get<std::string>() : "Sam Example");
        json m = {{"chat", chat}, {"id", id}, {"from_me", me}, {"sender", sender}, {"sender_name", sname},
                  {"timestamp", ts}, {"kind", "text"}, {"text", text}, {"status", me ? "pending" : "received"},
                  {"quoted_id", ""}, {"media", nullptr}};
        msgs_[chat].push_back(m);
        c["last_text"] = text;
        c["last_ts"] = ts;
        c["last_from_me"] = me;
        if (live) {
            if (!me) c["unread"] = c["unread"].get<int>() + 1;
            ipc_.broadcast({{"event", "message"}, {"chat", chat}, {"chat_name", c["name"]}, {"sender", sender},
                            {"sender_name", sname}, {"text", text}, {"timestamp", ts}, {"id", id}, {"from_me", me},
                            {"is_group", c["is_group"]}, {"muted", false}, {"live", true}, {"message", m}});
            ipc_.broadcast({{"event", "chat"}, {"chat", c}});
        }
        return id;
    }
    void timer() {
        loop_.after(20000, [this] {
            if (paired_) {
                auto it = chats_.begin();
                std::advance(it, tick_++ % chats_.size());
                bool g = it->second["is_group"].get<bool>();
                push(it->first, g ? "61400000001@s.whatsapp.net" : it->first, false,
                     "Scripted fake message " + std::to_string(tick_), time(nullptr), true);
            }
            timer();
        });
    }

    Loop& loop_;
    IpcServer& ipc_;
    bool paired_ = false;
    std::string state_ = "unpaired", code_;
    std::map<std::string, json> chats_;
    std::map<std::string, std::vector<json>> msgs_;
    int seq_ = 0, tick_ = 0;
};
}  // namespace

std::unique_ptr<Engine> makeFakeEngine(Loop& l, IpcServer& ipc) { return std::make_unique<FakeEngine>(l, ipc); }

}  // namespace zw
