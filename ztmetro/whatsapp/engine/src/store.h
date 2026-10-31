// Encrypted-at-rest local store. SQLite holds only opaque rows: values are sealed with
// XChaCha20-Poly1305, and names and ids are keyed hashes. Timestamps and row sizes stay
// in the clear, so sorting works. See README for what this does and does not protect.
#pragma once
#include <sqlite3.h>

#include <nlohmann/json.hpp>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "crypto.h"

namespace zw {

using nlohmann::json;

class Store {
public:
    // Creates dir with mode 0700 and the key file with mode 0600 if needed.
    explicit Store(const std::string& dir);
    ~Store();
    Store(const Store&) = delete;

    // Removes everything: database and key. The object reopens with a new key.
    void wipe();
    const std::string& dir() const { return dir_; }

    std::optional<Bytes> get(const std::string& ns, const std::string& key);
    void put(const std::string& ns, const std::string& key, const Bytes& val, const std::string& group = "");
    void del(const std::string& ns, const std::string& key);
    void delGroup(const std::string& ns, const std::string& group);

    void begin();
    void commit();
    void rollback();

    void putChat(const std::string& chatId, const json& chat);
    std::optional<json> getChat(const std::string& chatId);
    // Newest first, pinned first.
    std::vector<json> listChats(int limit, int offset, int* total);

    // Returns false when (chat,id) already existed, leaving it unchanged.
    bool insertMessage(const std::string& chat, const std::string& id, int64_t ts, bool unread, const json& m);
    void updateMessage(const std::string& chat, const std::string& id, const json& m);
    std::optional<json> getMessage(const std::string& chat, const std::string& id);
    // Oldest first within the page.
    std::vector<json> pageMessages(const std::string& chat, int limit, std::optional<int64_t> beforeTs,
                                   const std::string& beforeId, bool* more);
    std::vector<json> unreadMessages(const std::string& chat);
    void clearUnread(const std::string& chat, const std::string& id);
    int unreadCount(const std::string& chat);
    std::optional<json> lastMessage(const std::string& chat);

    void putContact(const std::string& jid, const json& c);
    std::vector<json> allContacts();
    // Returns a self-contained scan for a worker thread: it opens its own read connection,
    // so a search over a large store never stalls the socket.
    using SearchFn = std::function<std::vector<json>(bool* more)>;
    SearchFn searchJob(const std::string& query, const std::string& chat, int limit, std::optional<int64_t> beforeTs);

private:
    void open();
    void exec(const char* sql);
    Bytes seal(const Bytes& pt);
    Bytes unseal(const uint8_t* d, size_t n);
    Bytes name(const std::string& s);
    Bytes sealJson(const json& j);
    json openJson(sqlite3_stmt* st, int col);

    std::string dir_;
    Bytes key_, nameKey_;
    sqlite3* db_ = nullptr;
};

}  // namespace zw
