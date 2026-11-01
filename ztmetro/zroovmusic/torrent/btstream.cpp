// zrn-zroov-btstream: fetches one file of one torrent in order and serves it
// to the player over loopback HTTP. Started per verify or play, never idle.
// stdout: one JSON event per line. stdin: select <n> | pause | resume | stop
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/load_torrent.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace lt = libtorrent;
using clk = std::chrono::steady_clock;

struct Opts {
    std::string torrent, magnet, dir, iface;
    std::vector<std::string> webseeds;
    std::string mode = "play";
    int upload_kib = 64, download_kib = 0, dht = 0, wifi_only = 1;
    int idle_stop_s = 30, seed_after_s = 0;
    long long buffer_bytes = 1 << 20;
    long long head_bytes = 64 * 1024;
};
static Opts O;

static std::mutex out_mu;
static std::string esc(const std::string &s) {
    std::string r;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { r += '\\'; r += char(c); }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); r += b; }
        else r += char(c);
    }
    return r;
}
static void emit(const std::string &json) {
    std::lock_guard<std::mutex> l(out_mu);
    fputs(json.c_str(), stdout); fputc('\n', stdout); fflush(stdout);
}
static std::string hexs(const std::string &in) {
    static const char *hx = "0123456789abcdef";
    std::string out;
    for (unsigned char c : in) { out += hx[c >> 4]; out += hx[c & 15]; }
    return out;
}
static void err(const std::string &what) {
    emit("{\"ev\":\"error\",\"what\":\"" + esc(what) + "\"}");
}

static clk::time_point T0;
static long long ms_since_start() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - T0).count();
}

// --- shared piece state ----------------------------------------------------
struct State {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<char> have;
    std::map<int, std::shared_ptr<std::vector<char>>> cache;
    std::list<int> lru;
    std::set<int> pending;
    std::set<int> read_failed;
    bool selected = false;
    int file_index = -1;
    long long file_size = 0, file_offset = 0;
    int piece_len = 0;
    int first_piece = 0, last_piece = 0;
    int playhead = 0;
    bool playhead_moved = false;
    int connections = 0;
    int waiting = 0;
    clk::time_point last_http = clk::now();
};
static State S;
static std::atomic<bool> g_stop{false};
static bool g_debug = false;
static std::atomic<bool> g_paused{false};
static lt::torrent_handle g_h;
static std::string g_token;
static int g_port = 0;

static int piece_of(long long off) { return int((S.file_offset + off) / S.piece_len); }

static void cache_put(int p, std::shared_ptr<std::vector<char>> d) {
    S.cache[p] = std::move(d);
    S.lru.remove(p);
    S.lru.push_back(p);
    size_t cap = std::max<size_t>(size_t(S.piece_len) * 2, 32u << 20);
    size_t used = 0;
    for (auto &kv : S.cache) used += kv.second->size();
    while (used > cap && S.lru.size() > 2) {
        int v = S.lru.front();
        S.lru.pop_front();
        used -= S.cache[v]->size();
        S.cache.erase(v);
    }
}

// Blocks until the piece data is here. Null when the client left or on stop.
static std::shared_ptr<std::vector<char>> get_piece(int p, int fd) {
    std::unique_lock<std::mutex> l(S.mu);
    struct Waiting {
        Waiting() { S.waiting++; }
        ~Waiting() { S.waiting--; }
    } waiting;
    for (;;) {
        if (g_stop) return nullptr;
        auto it = S.cache.find(p);
        if (it != S.cache.end()) return it->second;
        if (S.read_failed.count(p)) return nullptr;
        if (S.have[p] && !S.pending.count(p) && !g_paused) {
            S.pending.insert(p);
            g_h.read_piece(lt::piece_index_t(p));
        }
        S.cv.wait_for(l, std::chrono::milliseconds(250));
        if (fd >= 0) {
            char c;
            ssize_t r = recv(fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
            if (r == 0) return nullptr;
        }
    }
}

// --- loopback HTTP ---------------------------------------------------------
static bool send_all(int fd, const char *p, size_t n) {
    while (n) {
        pollfd pf{fd, POLLOUT, 0};
        int pr = poll(&pf, 1, 1000);
        if (g_stop) return false;
        if (pr <= 0) continue;
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w <= 0) return false;
        p += w; n -= size_t(w);
    }
    return true;
}

static void serve(int fd) {
    {
        std::lock_guard<std::mutex> l(S.mu);
        S.connections++;
        S.last_http = clk::now();
    }
    S.cv.notify_all();
    std::string req;
    char buf[2048];
    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 8192) {
        ssize_t r = recv(fd, buf, sizeof buf, 0);
        if (r <= 0) break;
        req.append(buf, size_t(r));
    }
    std::istringstream rs(req);
    std::string method, path;
    rs >> method >> path;
    std::string want = "/" + g_token + "/stream.flac";
    if (path != want || (method != "GET" && method != "HEAD")) {
        const char *nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(fd, nf, strlen(nf));
    } else {
        long long size = S.file_size, from = 0, to = size - 1;
        bool ranged = false;
        std::string low = req;
        std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        auto rp = low.find("\r\nrange: bytes=");
        if (rp != std::string::npos) {
            const char *q = low.c_str() + rp + 15;
            char *e;
            long long a = strtoll(q, &e, 10);
            if (e != q) {
                from = a;
                if (*e == '-' && e[1] >= '0' && e[1] <= '9') to = std::min(strtoll(e + 1, nullptr, 10), size - 1);
                ranged = true;
            }
        }
        if (from >= size || from > to) {
            std::string h = "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */" +
                            std::to_string(size) + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send_all(fd, h.data(), h.size());
        } else {
            std::string h = std::string("HTTP/1.1 ") + (ranged ? "206 Partial Content" : "200 OK") +
                            "\r\nContent-Type: audio/flac\r\nAccept-Ranges: bytes\r\nConnection: close\r\nContent-Length: " +
                            std::to_string(to - from + 1) + "\r\n";
            if (ranged)
                h += "Content-Range: bytes " + std::to_string(from) + "-" + std::to_string(to) + "/" + std::to_string(size) + "\r\n";
            h += "\r\n";
            bool ok = send_all(fd, h.data(), h.size());
            long long off = from;
            while (ok && method == "GET" && off <= to) {
                int p = piece_of(off);
                {
                    std::lock_guard<std::mutex> l(S.mu);
                    if (S.playhead != p) { S.playhead = p; S.playhead_moved = true; }
                    S.last_http = clk::now();
                }
                auto d = get_piece(p, fd);
                if (!d) break;
                long long abs = S.file_offset + off;
                long long in = abs - (long long)p * S.piece_len;
                long long n = std::min<long long>((long long)d->size() - in, to - off + 1);
                if (n <= 0) break;
                ok = send_all(fd, d->data() + in, size_t(n));
                off += n;
                std::lock_guard<std::mutex> l(S.mu);
                S.last_http = clk::now();
            }
        }
    }
    close(fd);
    {
        std::lock_guard<std::mutex> l(S.mu);
        S.connections--;
        S.last_http = clk::now();
    }
}

static int start_http() {
    int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s < 0 || bind(s, (sockaddr *)&a, sizeof a) || listen(s, 8)) return -1;
    socklen_t al = sizeof a;
    getsockname(s, (sockaddr *)&a, &al);
    g_port = ntohs(a.sin_port);
    std::thread([s] {
        for (;;) {
            int c = accept4(s, nullptr, nullptr, SOCK_CLOEXEC);
            if (c < 0) { if (g_stop) return; continue; }
            std::thread(serve, c).detach();
        }
    }).detach();
    return 0;
}

// --- policy: mobile data guard ---------------------------------------------
static std::string default_iface() {
    std::ifstream f("/proc/net/route");
    std::string line, best;
    std::getline(f, line);
    long best_metric = -1;
    while (std::getline(f, line)) {
        std::istringstream l(line);
        std::string ifn, dest, gw, flags, refc, use, metric;
        l >> ifn >> dest >> gw >> flags >> refc >> use >> metric;
        if (dest != "00000000") continue;
        long m = atol(metric.c_str());
        if (best_metric < 0 || m < best_metric) { best_metric = m; best = ifn; }
    }
    return best;
}
static bool is_wifi(const std::string &ifn) {
    if (ifn.empty()) return false;
    struct stat st;
    return stat(("/sys/class/net/" + ifn + "/wireless").c_str(), &st) == 0;
}

static void pick_file(lt::torrent_handle &h, int idx) {
    auto ti = h.torrent_file();
    auto const &fs = ti->layout();
    lt::file_index_t fi(idx);
    if (idx < 0 || idx >= fs.num_files()) { err("bad file index"); return; }
    std::lock_guard<std::mutex> l(S.mu);
    S.file_index = idx;
    S.file_size = fs.file_size(fi);
    S.file_offset = fs.file_offset(fi);
    S.piece_len = ti->piece_length();
    S.first_piece = piece_of(0);
    S.last_piece = piece_of(S.file_size - 1);
    S.playhead = S.first_piece;
    S.playhead_moved = true;
    S.have.assign(size_t(ti->num_pieces()), 0);
    for (int p = S.first_piece; p <= S.last_piece; p++)
        S.have[p] = h.have_piece(lt::piece_index_t(p)) ? 1 : 0;
    if (O.mode == "probe") {
        int last = std::min(S.last_piece, piece_of(std::min(O.head_bytes, S.file_size) - 1));
        for (int p = S.first_piece; p <= last; p++) {
            h.piece_priority(lt::piece_index_t(p), lt::top_priority);
            h.set_piece_deadline(lt::piece_index_t(p), 200);
        }
    } else {
        h.file_priority(fi, lt::default_priority);
        h.set_flags(lt::torrent_flags::sequential_download);
    }
    S.selected = true;
}

int main(int argc, char **argv) {
    T0 = clk::now();
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    signal(SIGPIPE, SIG_IGN);
    static auto on_sig = [](int) { g_stop = true; };
    signal(SIGTERM, +on_sig);
    signal(SIGINT, +on_sig);
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i], v = argv[i + 1];
        if (k == "--torrent") O.torrent = v;
        else if (k == "--magnet") O.magnet = v;
        else if (k == "--dir") O.dir = v;
        else if (k == "--mode") O.mode = v;
        else if (k == "--webseed") O.webseeds.push_back(v);
        else if (k == "--upload-kib") O.upload_kib = atoi(v.c_str());
        else if (k == "--download-kib") O.download_kib = atoi(v.c_str());
        else if (k == "--dht") O.dht = atoi(v.c_str());
        else if (k == "--wifi-only") O.wifi_only = atoi(v.c_str());
        else if (k == "--idle-stop-s") O.idle_stop_s = atoi(v.c_str());
        else if (k == "--seed-after-s") O.seed_after_s = atoi(v.c_str());
        else if (k == "--buffer-bytes") O.buffer_bytes = atoll(v.c_str());
        else { err("unknown option " + k); return 2; }
    }
    if (O.dir.empty() || (O.torrent.empty() && O.magnet.empty())) { err("need --dir and --torrent or --magnet"); return 2; }

    std::string iface = default_iface();
    if (O.wifi_only && !is_wifi(iface)) {
        emit("{\"ev\":\"network_blocked\",\"iface\":\"" + esc(iface) + "\"}");
        return 3;
    }
    O.iface = iface;

    unsigned char rnd[16];
    {
        std::ifstream u("/dev/urandom", std::ios::binary);
        u.read((char *)rnd, sizeof rnd);
        char b[3];
        for (unsigned char c : rnd) { snprintf(b, sizeof b, "%02x", c); g_token += b; }
    }

    g_debug = getenv("ZROOV_BT_DEBUG") != nullptr;
    lt::settings_pack sp;
    sp.set_int(lt::settings_pack::alert_mask,
               lt::alert_category::status | lt::alert_category::error | lt::alert_category::storage |
                   lt::alert_category::tracker | lt::alert_category::piece_progress |
                   (g_debug ? lt::alert_category::connect | lt::alert_category::peer | lt::alert_category::peer_log : lt::alert_category_t{}));
    // Nothing that opens router ports or announces beyond what the torrent asks for.
    sp.set_bool(lt::settings_pack::enable_upnp, false);
    sp.set_bool(lt::settings_pack::enable_natpmp, false);
    sp.set_bool(lt::settings_pack::enable_lsd, false);
    sp.set_bool(lt::settings_pack::enable_dht, O.dht != 0);
    // libtorrent reads a limit of 0 as unlimited, so "no upload" is 1 KiB/s.
    sp.set_int(lt::settings_pack::upload_rate_limit, std::max(O.upload_kib, 1) * 1024);
    sp.set_int(lt::settings_pack::download_rate_limit, O.download_kib * 1024);
    sp.set_int(lt::settings_pack::connections_limit, 30);
    sp.set_int(lt::settings_pack::aio_threads, 2);
    sp.set_int(lt::settings_pack::hashing_threads, 1);
    if (!iface.empty()) {
        sp.set_str(lt::settings_pack::outgoing_interfaces, iface);
        sp.set_str(lt::settings_pack::listen_interfaces, iface + ":0");
    }
    lt::session ses{lt::session_params(sp)};

    lt::add_torrent_params atp;
    lt::error_code ec;
    if (!O.torrent.empty()) {
        try { atp = lt::load_torrent_file(O.torrent); }
        catch (std::exception const &e) { err(std::string("bad torrent: ") + e.what()); return 4; }
    } else {
        atp = lt::parse_magnet_uri(O.magnet, ec);
        if (ec) { err("bad magnet: " + ec.message()); return 4; }
    }
    atp.save_path = O.dir;
    atp.flags |= lt::torrent_flags::default_dont_download;
    atp.flags &= ~lt::torrent_flags::auto_managed;
    atp.flags &= ~lt::torrent_flags::paused;
    atp.file_priorities.clear();
    lt::torrent_handle h = ses.add_torrent(std::move(atp), ec);
    if (ec) { err("add: " + ec.message()); return 4; }
    g_h = h;
    if (!O.webseeds.empty()) {
        // libtorrent follows a redirect by treating the target as holding only that
        // one file, and a piece that spans two files then looks unavailable. Web seeds
        // named by the source point straight at a server, so replace the torrent's.
        for (auto const &u : h.url_seeds()) h.remove_url_seed(u);
        for (auto const &u : O.webseeds) h.add_url_seed(u);
    }
    if (O.mode == "play" && start_http()) { err("http bind"); return 5; }

    bool meta_sent = false, ready_sent = false, head_sent = false, scrape_got = false;
    bool user_paused = false, idle_paused = false, net_paused = false, stopping = false;
    long long first_data_ms = -1;
    auto last_stats = clk::now(), last_net = clk::now(), head_at = clk::now();
    clk::time_point seed_until{};
    std::string in_buf;
    int exit_code = 0;

    auto apply_pause = [&] {
        bool want = user_paused || idle_paused || net_paused;
        bool is = bool(h.flags() & lt::torrent_flags::paused);
        g_paused = want;
        if (want && !is) h.pause();
        else if (!want && is) h.resume();
    };

    auto send_meta = [&] {
        auto ti = h.torrent_file();
        if (!ti) return;
        auto const &fs = ti->layout();
        std::ostringstream o;
        o << "{\"ev\":\"meta\",\"infohash\":\"" << hexs(h.info_hash().to_string())
          << "\",\"piece_length\":" << ti->piece_length() << ",\"web_seeds\":" << h.url_seeds().size() << ",\"ms\":" << ms_since_start() << ",\"files\":[";
        for (int i = 0; i < fs.num_files(); i++) {
            lt::file_index_t fi(i);
            o << (i ? "," : "") << "{\"i\":" << i << ",\"path\":\"" << esc(fs.file_path(fi)) << "\",\"size\":" << fs.file_size(fi) << "}";
        }
        o << "]}";
        emit(o.str());
        meta_sent = true;
    };

    while (!g_stop) {
        ses.wait_for_alert(lt::milliseconds(250));
        std::vector<lt::alert *> alerts;
        ses.pop_alerts(&alerts);
        bool state_changed = false;
        for (lt::alert *a : alerts) {
            if (g_debug) fprintf(stderr, "%s: %s\n", a->what(), a->message().c_str());
            if (auto *m = lt::alert_cast<lt::metadata_received_alert>(a)) { (void)m; if (!meta_sent) send_meta(); }
            else if (auto *c = lt::alert_cast<lt::torrent_checked_alert>(a)) {
                (void)c;
                if (!meta_sent && h.torrent_file()) send_meta();
                std::lock_guard<std::mutex> l(S.mu);
                if (S.selected)
                    for (int p = S.first_piece; p <= S.last_piece; p++)
                        S.have[p] = h.have_piece(lt::piece_index_t(p)) ? 1 : 0;
                state_changed = true;
            } else if (auto *p = lt::alert_cast<lt::piece_finished_alert>(a)) {
                std::lock_guard<std::mutex> l(S.mu);
                int i = int(static_cast<int>(p->piece_index));
                if (i >= 0 && size_t(i) < S.have.size()) S.have[i] = 1;
                if (first_data_ms < 0) first_data_ms = ms_since_start();
                state_changed = true;
            } else if (auto *r = lt::alert_cast<lt::read_piece_alert>(a)) {
                std::lock_guard<std::mutex> l(S.mu);
                int i = int(static_cast<int>(r->piece));
                S.pending.erase(i);
                if (r->error) { S.read_failed.insert(i); err("read piece " + std::to_string(i) + ": " + r->error.message()); }
                else cache_put(i, std::make_shared<std::vector<char>>(r->buffer.get(), r->buffer.get() + r->size));
                state_changed = true;
            } else if (auto *s = lt::alert_cast<lt::scrape_reply_alert>(a)) {
                emit("{\"ev\":\"swarm\",\"seeders\":" + std::to_string(s->complete) + ",\"leechers\":" + std::to_string(s->incomplete) + "}");
                scrape_got = true;
            } else if (lt::alert_cast<lt::scrape_failed_alert>(a)) {
                scrape_got = true;
            } else if (auto *e = lt::alert_cast<lt::torrent_error_alert>(a)) {
                err("torrent: " + e->error.message());
                exit_code = 6; g_stop = true;
            } else if (auto *e2 = lt::alert_cast<lt::file_error_alert>(a)) {
                err("file: " + e2->error.message());
                exit_code = 6; g_stop = true;
            } else if (auto *w = lt::alert_cast<lt::url_seed_alert>(a)) {
                emit("{\"ev\":\"webseed_error\",\"msg\":\"" + esc(w->message()) + "\"}");
            }
        }
        if (state_changed) S.cv.notify_all();
        if (!meta_sent && h.torrent_file() && h.status().has_metadata) send_meta();

        // stdin commands
        pollfd pf{0, POLLIN, 0};
        while (poll(&pf, 1, 0) > 0 && (pf.revents & (POLLIN | POLLHUP))) {
            char b[256];
            ssize_t n = read(0, b, sizeof b);
            if (n <= 0) { stopping = true; seed_until = clk::now(); pf.fd = -1; break; }
            in_buf.append(b, size_t(n));
            size_t nl;
            while ((nl = in_buf.find('\n')) != std::string::npos) {
                std::string line = in_buf.substr(0, nl);
                in_buf.erase(0, nl + 1);
                if (line.rfind("select ", 0) == 0 && meta_sent) {
                    pick_file(h, atoi(line.c_str() + 7));
                    h.scrape_tracker();
                    apply_pause();
                } else if (line == "pause") { user_paused = true; apply_pause(); }
                else if (line == "resume") { user_paused = false; apply_pause(); }
                else if (line == "stop") {
                    stopping = true;
                    seed_until = clk::now() + std::chrono::seconds(O.seed_after_s);
                }
            }
        }

        bool selected;
        { std::lock_guard<std::mutex> l(S.mu); selected = S.selected; }

        if (selected && O.mode == "probe" && !head_sent) {
            int last = std::min(S.last_piece, piece_of(std::min(O.head_bytes, S.file_size) - 1));
            bool all = true;
            { std::lock_guard<std::mutex> l(S.mu); for (int p = S.first_piece; p <= last; p++) if (!S.have[p]) all = false; }
            if (all) {
                std::string head;
                long long want = std::min(O.head_bytes, S.file_size);
                long long off = 0;
                bool bad = false;
                while (off < want) {
                    // Alerts are handled on this thread, so read_piece is drained here.
                    int p = piece_of(off);
                    h.read_piece(lt::piece_index_t(p));
                    std::shared_ptr<std::vector<char>> d;
                    auto t = clk::now();
                    while (!d && clk::now() - t < std::chrono::seconds(20)) {
                        ses.wait_for_alert(lt::milliseconds(200));
                        std::vector<lt::alert *> as;
                        ses.pop_alerts(&as);
                        for (lt::alert *a : as)
                            if (auto *r = lt::alert_cast<lt::read_piece_alert>(a))
                                if (static_cast<int>(r->piece) == p && !r->error)
                                    d = std::make_shared<std::vector<char>>(r->buffer.get(), r->buffer.get() + r->size);
                    }
                    if (!d) { bad = true; break; }
                    long long in = S.file_offset + off - (long long)p * S.piece_len;
                    long long n = std::min<long long>((long long)d->size() - in, want - off);
                    head.append(d->data() + in, size_t(n));
                    off += n;
                }
                if (bad) { err("could not read head"); exit_code = 6; break; }
                std::string out = hexs(head);
                auto st = h.status();
                emit("{\"ev\":\"head\",\"ms\":" + std::to_string(ms_since_start()) + ",\"file_size\":" + std::to_string(S.file_size) +
                     ",\"down\":" + std::to_string(st.total_download) + ",\"up\":" + std::to_string(st.total_upload) + ",\"hex\":\"" + out + "\"}");
                head_sent = true;
                head_at = clk::now();
            }
        }
        if (head_sent && (scrape_got || clk::now() - head_at > std::chrono::seconds(5))) break;

        if (selected && O.mode == "play") {
            std::lock_guard<std::mutex> l(S.mu);
            if (S.playhead_moved) {
                S.playhead_moved = false;
                h.clear_piece_deadlines();
                int window = std::max(4, int((4 << 20) / std::max(1, S.piece_len)));
                for (int i = 0; i < window && S.playhead + i <= S.last_piece; i++)
                    h.set_piece_deadline(lt::piece_index_t(S.playhead + i), 300 + i * 500);
            }
            if (!ready_sent) {
                long long contig = 0;
                for (int p = S.first_piece; p <= S.last_piece && S.have[p]; p++) {
                    long long start = (long long)p * S.piece_len;
                    long long end = start + S.piece_len;
                    contig += std::min(end, S.file_offset + S.file_size) - std::max(start, S.file_offset);
                }
                if (contig >= std::min(O.buffer_bytes, S.file_size)) {
                    emit("{\"ev\":\"ready\",\"url\":\"http://127.0.0.1:" + std::to_string(g_port) + "/" + g_token +
                         "/stream.flac\",\"ms\":" + std::to_string(ms_since_start()) + ",\"buffered\":" + std::to_string(contig) +
                         ",\"file_size\":" + std::to_string(S.file_size) + "}");
                    ready_sent = true;
                }
            }
            if (ready_sent && O.idle_stop_s > 0) {
                // An open socket proves nothing: a paused player keeps it open and stops
                // reading. Idle means nothing was served and nobody waits for a piece.
                bool idle = S.waiting == 0 && clk::now() - S.last_http > std::chrono::seconds(O.idle_stop_s);
                if (idle != idle_paused) {
                    idle_paused = idle;
                    emit(std::string("{\"ev\":\"idle\",\"paused\":") + (idle ? "true" : "false") + "}");
                    apply_pause();
                }
            }
        }

        if (clk::now() - last_net > std::chrono::seconds(2)) {
            last_net = clk::now();
            std::string cur = default_iface();
            bool blocked = O.wifi_only && !is_wifi(cur);
            if (blocked != net_paused) {
                net_paused = blocked;
                emit(std::string("{\"ev\":\"") + (blocked ? "network_blocked" : "network_ok") + "\",\"iface\":\"" + esc(cur) + "\"}");
                apply_pause();
            }
        }

        if (clk::now() - last_stats > std::chrono::seconds(2)) {
            last_stats = clk::now();
            auto st = h.status();
            long long contig = 0;
            int hp = 0;
            {
                std::lock_guard<std::mutex> l(S.mu);
                if (S.selected) {
                    for (int p = S.first_piece; p <= S.last_piece && S.have[p]; p++) contig++;
                    contig *= S.piece_len;
                    for (int p = S.first_piece; p <= S.last_piece; p++) hp += S.have[p];
                }
            }
            std::ostringstream o;
            o << "{\"ev\":\"stats\",\"ms\":" << ms_since_start() << ",\"down_bps\":" << st.download_payload_rate << ",\"up_bps\":" << st.upload_payload_rate
              << ",\"down\":" << st.total_download << ",\"up\":" << st.total_upload << ",\"peers\":" << st.num_peers << ",\"seeds\":" << st.num_seeds
              << ",\"contig\":" << contig << ",\"pieces\":" << hp << ",\"paused\":" << (bool(st.flags & lt::torrent_flags::paused) ? "true" : "false")
              << ",\"file_size\":" << S.file_size << "}";
            emit(o.str());
        }

        if (stopping && clk::now() >= seed_until) break;
    }

    g_stop = true;
    S.cv.notify_all();
    auto st = h.status();
    emit("{\"ev\":\"end\",\"down\":" + std::to_string(st.total_download) + ",\"up\":" + std::to_string(st.total_upload) + "}");
    ses.remove_torrent(h);
    // Announces "stopped" with a short bound, then the process is gone.
    ses.abort();
    fflush(stdout);
    _exit(exit_code);
}
