#include "store.h"

#include <fcntl.h>
#include <sodium.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

namespace zw {

namespace {
struct Stmt {
    sqlite3_stmt* s = nullptr;
    Stmt(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK)
            throw std::runtime_error(std::string("sqlite prepare: ") + sqlite3_errmsg(db));
    }
    ~Stmt() { sqlite3_finalize(s); }
    Stmt& b(int i, const Bytes& v) {
        sqlite3_bind_blob(s, i, v.data(), (int)v.size(), SQLITE_TRANSIENT);
        return *this;
    }
    Stmt& i(int idx, int64_t v) {
        sqlite3_bind_int64(s, idx, v);
        return *this;
    }
    bool step() {
        int r = sqlite3_step(s);
        if (r == SQLITE_ROW) return true;
        if (r != SQLITE_DONE) throw std::runtime_error("sqlite step failed");
        return false;
    }
};
}  // namespace

Store::Store(const std::string& dir) : dir_(dir) {
    fs::create_directories(dir_);
    chmod(dir_.c_str(), 0700);
    fs::create_directories(dir_ + "/media");
    chmod((dir_ + "/media").c_str(), 0700);
    open();
}

Store::~Store() {
    if (db_) sqlite3_close(db_);
    sodium_memzero(key_.data(), key_.size());
    sodium_memzero(nameKey_.data(), nameKey_.size());
}

void Store::open() {
    std::string kp = dir_ + "/store.key";
    int fd = ::open(kp.c_str(), O_RDONLY | O_CLOEXEC);
    Bytes master(32);
    if (fd >= 0) {
        ssize_t r = read(fd, master.data(), 32);
        close(fd);
        if (r != 32) throw std::runtime_error("store key is damaged");
    } else {
        randombytes_buf(master.data(), 32);
        // Written to a temporary name first so a crash cannot leave a short key.
        std::string tmp = kp + ".new";
        fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0 || write(fd, master.data(), 32) != 32) throw std::runtime_error("cannot write store key");
        fsync(fd);
        close(fd);
        rename(tmp.c_str(), kp.c_str());
    }
    key_ = hkdf(master, {}, "ztwhatsapp seal", 32);
    nameKey_ = hkdf(master, {}, "ztwhatsapp name", 32);
    sodium_memzero(master.data(), 32);

    std::string dbp = dir_ + "/store.db";
    mode_t old = umask(0077);
    int rc = sqlite3_open(dbp.c_str(), &db_);
    umask(old);
    if (rc != SQLITE_OK) throw std::runtime_error("cannot open store");
    sqlite3_busy_timeout(db_, 2000);
    exec("PRAGMA journal_mode=WAL");
    exec("PRAGMA synchronous=NORMAL");
    exec("PRAGMA secure_delete=ON");
    exec("CREATE TABLE IF NOT EXISTS kv(ns TEXT, k BLOB, grp BLOB, v BLOB, PRIMARY KEY(ns,k))");
    exec("CREATE INDEX IF NOT EXISTS kv_grp ON kv(ns,grp)");
    exec("CREATE TABLE IF NOT EXISTS chats(id BLOB PRIMARY KEY, pin INT, ts INT, v BLOB)");
    exec("CREATE TABLE IF NOT EXISTS msgs(seq INTEGER PRIMARY KEY AUTOINCREMENT, chat BLOB, id BLOB,"
         " ts INT, unread INT, v BLOB, UNIQUE(chat,id))");
    exec("CREATE INDEX IF NOT EXISTS msgs_page ON msgs(chat,ts,seq)");
    exec("CREATE TABLE IF NOT EXISTS contacts(id BLOB PRIMARY KEY, v BLOB)");
}

void Store::wipe() {
    sqlite3_close(db_);
    db_ = nullptr;
    for (auto n : {"store.db", "store.db-wal", "store.db-shm", "store.key"}) {
        std::error_code ec;
        fs::remove(dir_ + "/" + n, ec);
    }
    std::error_code ec;
    fs::remove_all(dir_ + "/media", ec);
    fs::create_directories(dir_ + "/media");
    chmod((dir_ + "/media").c_str(), 0700);
    open();
}

void Store::exec(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string e = err ? err : "sqlite";
        sqlite3_free(err);
        throw std::runtime_error(e);
    }
}

void Store::begin() { exec("BEGIN IMMEDIATE"); }
void Store::commit() { exec("COMMIT"); }
void Store::rollback() { exec("ROLLBACK"); }

Bytes Store::seal(const Bytes& pt) {
    Bytes o(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES + pt.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES);
    randombytes_buf(o.data(), crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
    unsigned long long n = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(o.data() + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES, &n, pt.data(),
                                               pt.size(), nullptr, 0, nullptr, o.data(), key_.data());
    return o;
}

Bytes Store::unseal(const uint8_t* d, size_t n) {
    const size_t np = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES, ab = crypto_aead_xchacha20poly1305_ietf_ABYTES;
    if (n < np + ab) throw std::runtime_error("store: short row");
    Bytes o(n - np - ab);
    unsigned long long m = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(o.data(), &m, nullptr, d + np, n - np, nullptr, 0, d, key_.data()) != 0)
        throw std::runtime_error("store: row failed authentication");
    return o;
}

Bytes Store::name(const std::string& s) {
    Bytes o(16);
    crypto_generichash(o.data(), o.size(), (const uint8_t*)s.data(), s.size(), nameKey_.data(), nameKey_.size());
    return o;
}

Bytes Store::sealJson(const json& j) { return seal(B(j.dump())); }

json Store::openJson(sqlite3_stmt* st, int col) {
    auto* d = (const uint8_t*)sqlite3_column_blob(st, col);
    size_t n = (size_t)sqlite3_column_bytes(st, col);
    Bytes p = unseal(d, n);
    return json::parse(p.begin(), p.end());
}

std::optional<Bytes> Store::get(const std::string& ns, const std::string& key) {
    Stmt s(db_, "SELECT v FROM kv WHERE ns=? AND k=?");
    sqlite3_bind_text(s.s, 1, ns.c_str(), -1, SQLITE_TRANSIENT);
    s.b(2, name(key));
    if (!s.step()) return std::nullopt;
    return unseal((const uint8_t*)sqlite3_column_blob(s.s, 0), (size_t)sqlite3_column_bytes(s.s, 0));
}

void Store::put(const std::string& ns, const std::string& key, const Bytes& val, const std::string& group) {
    Stmt s(db_, "INSERT OR REPLACE INTO kv(ns,k,grp,v) VALUES(?,?,?,?)");
    sqlite3_bind_text(s.s, 1, ns.c_str(), -1, SQLITE_TRANSIENT);
    s.b(2, name(key)).b(3, name(group)).b(4, seal(val));
    s.step();
}

void Store::del(const std::string& ns, const std::string& key) {
    Stmt s(db_, "DELETE FROM kv WHERE ns=? AND k=?");
    sqlite3_bind_text(s.s, 1, ns.c_str(), -1, SQLITE_TRANSIENT);
    s.b(2, name(key));
    s.step();
}

void Store::delGroup(const std::string& ns, const std::string& group) {
    Stmt s(db_, "DELETE FROM kv WHERE ns=? AND grp=?");
    sqlite3_bind_text(s.s, 1, ns.c_str(), -1, SQLITE_TRANSIENT);
    s.b(2, name(group));
    s.step();
}

void Store::putChat(const std::string& id, const json& c) {
    Stmt s(db_, "INSERT OR REPLACE INTO chats(id,pin,ts,v) VALUES(?,?,?,?)");
    s.b(1, name(id)).i(2, c.value("pinned", false) ? 1 : 0).i(3, c.value("last_ts", (int64_t)0)).b(4, sealJson(c));
    s.step();
}

std::optional<json> Store::getChat(const std::string& id) {
    Stmt s(db_, "SELECT v FROM chats WHERE id=?");
    s.b(1, name(id));
    if (!s.step()) return std::nullopt;
    return openJson(s.s, 0);
}

std::vector<json> Store::listChats(int limit, int offset, int* total) {
    {
        Stmt c(db_, "SELECT count(*) FROM chats");
        c.step();
        *total = sqlite3_column_int(c.s, 0);
    }
    Stmt s(db_, "SELECT v FROM chats ORDER BY pin DESC, ts DESC LIMIT ? OFFSET ?");
    s.i(1, limit).i(2, offset);
    std::vector<json> r;
    while (s.step()) r.push_back(openJson(s.s, 0));
    return r;
}

bool Store::insertMessage(const std::string& chat, const std::string& id, int64_t ts, bool unread, const json& m) {
    Stmt s(db_, "INSERT OR IGNORE INTO msgs(chat,id,ts,unread,v) VALUES(?,?,?,?,?)");
    s.b(1, name(chat)).b(2, name(id)).i(3, ts).i(4, unread).b(5, sealJson(m));
    s.step();
    return sqlite3_changes(db_) > 0;
}

void Store::updateMessage(const std::string& chat, const std::string& id, const json& m) {
    Stmt s(db_, "UPDATE msgs SET v=? WHERE chat=? AND id=?");
    s.b(1, sealJson(m)).b(2, name(chat)).b(3, name(id));
    s.step();
}

std::optional<json> Store::getMessage(const std::string& chat, const std::string& id) {
    Stmt s(db_, "SELECT v FROM msgs WHERE chat=? AND id=?");
    s.b(1, name(chat)).b(2, name(id));
    if (!s.step()) return std::nullopt;
    return openJson(s.s, 0);
}

std::vector<json> Store::pageMessages(const std::string& chat, int limit, std::optional<int64_t> beforeTs,
                                      const std::string& beforeId, bool* more) {
    int64_t seq = INT64_MAX;
    if (!beforeId.empty()) {
        Stmt q(db_, "SELECT seq FROM msgs WHERE chat=? AND id=?");
        q.b(1, name(chat)).b(2, name(beforeId));
        if (q.step()) seq = sqlite3_column_int64(q.s, 0);
    }
    Stmt s(db_, "SELECT v FROM msgs WHERE chat=? AND (ts<? OR (ts=? AND seq<?)) ORDER BY ts DESC, seq DESC LIMIT ?");
    int64_t ts = beforeTs.value_or(INT64_MAX);
    s.b(1, name(chat)).i(2, ts).i(3, ts).i(4, seq).i(5, limit + 1);
    std::vector<json> r;
    while (s.step()) r.push_back(openJson(s.s, 0));
    *more = (int)r.size() > limit;
    if (*more) r.pop_back();
    std::reverse(r.begin(), r.end());
    return r;
}

std::vector<json> Store::unreadMessages(const std::string& chat) {
    Stmt s(db_, "SELECT v FROM msgs WHERE chat=? AND unread=1 ORDER BY ts, seq");
    s.b(1, name(chat));
    std::vector<json> r;
    while (s.step()) r.push_back(openJson(s.s, 0));
    return r;
}

void Store::clearUnread(const std::string& chat, const std::string& id) {
    Stmt s(db_, "UPDATE msgs SET unread=0 WHERE chat=? AND id=?");
    s.b(1, name(chat)).b(2, name(id));
    s.step();
}

int Store::unreadCount(const std::string& chat) {
    Stmt s(db_, "SELECT count(*) FROM msgs WHERE chat=? AND unread=1");
    s.b(1, name(chat));
    s.step();
    return sqlite3_column_int(s.s, 0);
}

std::optional<json> Store::lastMessage(const std::string& chat) {
    Stmt s(db_, "SELECT v FROM msgs WHERE chat=? ORDER BY ts DESC, seq DESC LIMIT 1");
    s.b(1, name(chat));
    if (!s.step()) return std::nullopt;
    return openJson(s.s, 0);
}

void Store::putContact(const std::string& jid, const json& c) {
    Stmt s(db_, "INSERT OR REPLACE INTO contacts(id,v) VALUES(?,?)");
    s.b(1, name(jid)).b(2, sealJson(c));
    s.step();
}

std::vector<json> Store::allContacts() {
    Stmt s(db_, "SELECT v FROM contacts");
    std::vector<json> r;
    while (s.step()) r.push_back(openJson(s.s, 0));
    return r;
}

static std::string lowerAscii(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return s;
}

Store::SearchFn Store::searchJob(const std::string& query, const std::string& chat, int limit, std::optional<int64_t> beforeTs) {
    std::string path = dir_ + "/store.db";
    Bytes key = key_, chatName = chat.empty() ? Bytes{} : name(chat);
    std::string q = lowerAscii(query);
    int64_t before = beforeTs.value_or(INT64_MAX);
    return [=](bool* more) {
        std::vector<json> out;
        *more = false;
        sqlite3* db = nullptr;
        if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
            sqlite3_close(db);
            return out;
        }
        sqlite3_busy_timeout(db, 2000);
        const char* sql = chatName.empty() ? "SELECT v FROM msgs WHERE ts<? ORDER BY ts DESC, seq DESC"
                                           : "SELECT v FROM msgs WHERE ts<? AND chat=? ORDER BY ts DESC, seq DESC";
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
            fprintf(stderr, "ztwhatsappd: search: %s\n", sqlite3_errmsg(db));
        } else {
            sqlite3_bind_int64(st, 1, before);
            if (!chatName.empty()) sqlite3_bind_blob(st, 2, chatName.data(), (int)chatName.size(), SQLITE_TRANSIENT);
            const size_t np = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES, ab = crypto_aead_xchacha20poly1305_ietf_ABYTES;
            int rc;
            while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
                auto* d = (const uint8_t*)sqlite3_column_blob(st, 0);
                size_t n = (size_t)sqlite3_column_bytes(st, 0);
                if (n < np + ab) continue;
                Bytes p(n - np - ab);
                unsigned long long m = 0;
                if (crypto_aead_xchacha20poly1305_ietf_decrypt(p.data(), &m, nullptr, d + np, n - np, nullptr, 0, d, key.data()) != 0) continue;
                json j = json::parse(p.begin(), p.end(), nullptr, false);
                if (!j.is_object() || j.value("chat", "") == "status@broadcast") continue;
                if (lowerAscii(j.value("text", "")).find(q) == std::string::npos) continue;
                if ((int)out.size() == limit) {
                    *more = true;
                    break;
                }
                j.erase("_s");
                out.push_back(std::move(j));
            }
            if (rc != SQLITE_ROW && rc != SQLITE_DONE) fprintf(stderr, "ztwhatsappd: search: %s\n", sqlite3_errmsg(db));
        }
        sqlite3_finalize(st);
        sqlite3_close(db);
        return out;
    };
}

}  // namespace zw
