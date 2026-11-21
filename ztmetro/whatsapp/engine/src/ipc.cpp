#include "ipc.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <stdexcept>

namespace zw {

static constexpr size_t kMaxLine = 4u << 20;
static constexpr size_t kMaxBacklog = 1u << 20;

IpcServer::IpcServer(Loop& l, const std::string& path) : loop_(l), path_(path) {
    lfd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    if (path.size() >= sizeof a.sun_path) throw std::runtime_error("socket path too long");
    std::strcpy(a.sun_path, path.c_str());
    unlink(path.c_str());
    mode_t old = umask(0077);
    int r = bind(lfd_, (sockaddr*)&a, sizeof a);
    umask(old);
    if (r != 0 || listen(lfd_, 8) != 0) throw std::runtime_error("cannot bind IPC socket");
    chmod(path.c_str(), 0600);
    loop_.add(lfd_, EPOLLIN, [this](uint32_t) { accept_(); });
}

IpcServer::~IpcServer() {
    while (!clients_.empty()) drop(clients_.begin()->first);
    loop_.del(lfd_);
    close(lfd_);
    unlink(path_.c_str());
}

void IpcServer::accept_() {
    for (;;) {
        int fd = accept4(lfd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) return;
        clients_[fd] = {};
        gens_[fd] = ++gen_;
        loop_.add(fd, EPOLLIN, [this, fd](uint32_t ev) { io(fd, ev); });
        sendTo(fd, {{"event", "hello"}, {"ipc_version", kIpcVersion}, {"daemon_pid", (int)getpid()}});
        if (engine_ && clients_.count(fd))
            for (auto& g : engine_->greeting()) sendTo(fd, g);
    }
}

void IpcServer::drop(int fd) {
    loop_.del(fd);
    close(fd);
    clients_.erase(fd);
    gens_.erase(fd);
}

void IpcServer::io(int fd, uint32_t ev) {
    if (ev & EPOLLOUT) flush(fd);
    if (!clients_.count(fd)) return;
    if (!(ev & (EPOLLIN | EPOLLHUP | EPOLLERR))) return;
    char buf[16384];
    for (;;) {
        ssize_t r = recv(fd, buf, sizeof buf, 0);
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            drop(fd);
            return;
        }
        if (r < 0) return;
        auto& c = clients_[fd];
        c.in.append(buf, (size_t)r);
        size_t nl;
        while ((nl = c.in.find('\n')) != std::string::npos) {
            std::string l = c.in.substr(0, nl);
            c.in.erase(0, nl + 1);
            line(fd, l);
            if (!clients_.count(fd)) return;
        }
        if (clients_[fd].in.size() > kMaxLine) {
            drop(fd);
            return;
        }
    }
}

void IpcServer::line(int fd, const std::string& l) {
    if (l.empty()) return;
    json req = json::parse(l, nullptr, false);
    json idv = (req.is_object() && req.contains("id")) ? req["id"] : json();
    uint64_t g = gens_[fd];
    Reply rp;
    rp.ok = [this, fd, g, idv](const json& result) {
        auto it = gens_.find(fd);
        if (it == gens_.end() || it->second != g) return;
        sendTo(fd, {{"id", idv}, {"ok", true}, {"result", result}});
    };
    rp.err = [this, fd, g, idv](const std::string& code, const std::string& msg) {
        auto it = gens_.find(fd);
        if (it == gens_.end() || it->second != g) return;
        sendTo(fd, {{"id", idv}, {"ok", false}, {"error", code}, {"message", msg}});
    };
    if (!req.is_object() || !req.contains("cmd") || !req["cmd"].is_string()) {
        rp.err("bad_request", "expected a JSON object with cmd");
        return;
    }
    if (!engine_) {
        rp.err("internal", "engine not ready");
        return;
    }
    try {
        engine_->command(req["cmd"].get<std::string>(), req, rp);
    } catch (const json::exception&) {
        rp.err("bad_request", "bad field type");
    } catch (const std::exception&) {
        rp.err("internal", "command failed");
    }
}

void IpcServer::sendTo(int fd, const json& j) {
    auto it = clients_.find(fd);
    if (it == clients_.end()) return;
    // Replacement mode: a stored invalid UTF-8 string must not throw here.
    it->second.out += j.dump(-1, ' ', false, json::error_handler_t::replace);
    it->second.out += '\n';
    if (it->second.out.size() - it->second.outPos > kMaxBacklog) {
        drop(fd);
        return;
    }
    flush(fd);
}

void IpcServer::flush(int fd) {
    auto it = clients_.find(fd);
    if (it == clients_.end()) return;
    auto& c = it->second;
    while (c.outPos < c.out.size()) {
        ssize_t r = send(fd, c.out.data() + c.outPos, c.out.size() - c.outPos, MSG_NOSIGNAL);
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            loop_.mod(fd, EPOLLIN | EPOLLOUT);
            return;
        }
        if (r <= 0) {
            drop(fd);
            return;
        }
        c.outPos += (size_t)r;
    }
    c.out.clear();
    c.outPos = 0;
    loop_.mod(fd, EPOLLIN);
}

void IpcServer::broadcast(const json& ev) {
    std::vector<int> fds;
    for (auto& [fd, _] : clients_) fds.push_back(fd);
    for (int fd : fds) sendTo(fd, ev);
}

}  // namespace zw
