#include "ws.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/x509v3.h>
#include <sys/socket.h>

#include <cstring>
#include <algorithm>
#include <thread>

namespace zw {

static constexpr size_t kMaxMessage = 64u << 20;

WebSocket::~WebSocket() {
    *alive_ = false;
    cleanup();
}

void WebSocket::cleanup() {
    if (fd_ >= 0) {
        loop_.del(fd_);
        ::close(fd_);
        fd_ = -1;
    }
    if (ssl_) SSL_free(ssl_);
    ssl_ = nullptr;
    if (ctx_) SSL_CTX_free(ctx_);
    ctx_ = nullptr;
    in_.clear();
    out_.clear();
    outPos_ = 0;
    partial_.clear();
    wantWrite_ = false;
    st_ = St::Idle;
}

void WebSocket::close() {
    *alive_ = false;
    alive_ = std::make_shared<bool>(true);
    cleanup();
}

void WebSocket::fail(const std::string& why) {
    cleanup();
    if (onClose) onClose(why);
}

void WebSocket::connect(const Opts& o) {
    cleanup();
    o_ = o;
    st_ = St::Resolving;
    auto alive = alive_;
    // getaddrinfo blocks, so it runs off the loop.
    std::thread([this, alive, host = o.host, port = o.port, &loop = loop_] {
        addrinfo hints{}, *res = nullptr;
        hints.ai_socktype = SOCK_STREAM;
        int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
        loop.post([this, alive, rc, res] {
            if (!*alive) {
                if (res) freeaddrinfo(res);
                return;
            }
            if (rc != 0 || !res) {
                fail("dns");
                return;
            }
            int fd = socket(res->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (fd < 0) {
                freeaddrinfo(res);
                fail("socket");
                return;
            }
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            int r = ::connect(fd, res->ai_addr, res->ai_addrlen);
            freeaddrinfo(res);
            if (r != 0 && errno != EINPROGRESS) {
                ::close(fd);
                fail("connect");
                return;
            }
            fd_ = fd;
            st_ = St::Connecting;
            loop_.add(fd_, EPOLLOUT, [this](uint32_t ev) { io(ev); });
        });
    }).detach();
}

void WebSocket::connected() {
    if (o_.tls) {
        ctx_ = SSL_CTX_new(TLS_client_method());
        SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
        SSL_CTX_set_default_verify_paths(ctx_);
        SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
        ssl_ = SSL_new(ctx_);
        SSL_set_fd(ssl_, fd_);
        SSL_set_tlsext_host_name(ssl_, o_.host.c_str());
        SSL_set1_host(ssl_, o_.host.c_str());
        SSL_set_connect_state(ssl_);
        st_ = St::Tls;
    }
    if (!o_.tls) st_ = St::Http;
}

int WebSocket::rawRead(uint8_t* b, size_t n) {
    if (!ssl_) {
        ssize_t r = recv(fd_, b, n, 0);
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return -1;
        return r <= 0 ? 0 : (int)r;
    }
    int r = SSL_read(ssl_, b, (int)n);
    if (r > 0) return r;
    int e = SSL_get_error(ssl_, r);
    if (e == SSL_ERROR_WANT_READ) return -1;
    if (e == SSL_ERROR_WANT_WRITE) {
        wantWrite_ = true;
        return -1;
    }
    return 0;
}

int WebSocket::rawWrite(const uint8_t* b, size_t n) {
    if (!ssl_) {
        ssize_t r = ::send(fd_, b, n, MSG_NOSIGNAL);
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return -1;
        return r <= 0 ? 0 : (int)r;
    }
    int r = SSL_write(ssl_, b, (int)n);
    if (r > 0) return r;
    int e = SSL_get_error(ssl_, r);
    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
        if (e == SSL_ERROR_WANT_WRITE) wantWrite_ = true;
        return -1;
    }
    return 0;
}

void WebSocket::interest() {
    if (fd_ < 0) return;
    uint32_t ev = EPOLLIN;
    if (outPos_ < out_.size() || wantWrite_ || st_ == St::Connecting) ev |= EPOLLOUT;
    loop_.mod(fd_, ev);
}

void WebSocket::io(uint32_t ev) {
    auto alive = alive_;
    if (st_ == St::Connecting) {
        int err = 0;
        socklen_t l = sizeof err;
        getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &l);
        if (err || (ev & (EPOLLERR | EPOLLHUP))) {
            fail("connect");
            return;
        }
        connected();
    }
    if (st_ == St::Tls) {
        wantWrite_ = false;
        int r = SSL_do_handshake(ssl_);
        if (r != 1) {
            int e = SSL_get_error(ssl_, r);
            if (e == SSL_ERROR_WANT_READ) {
                interest();
                return;
            }
            if (e == SSL_ERROR_WANT_WRITE) {
                wantWrite_ = true;
                interest();
                return;
            }
            fail("tls");
            return;
        }
        st_ = St::Http;
    }
    if (st_ == St::Http && wsKey_.empty()) {
        wsKey_ = b64(randomBytes(16));
        std::string req = "GET " + o_.path + " HTTP/1.1\r\nHost: " + o_.host +
                          "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + wsKey_ +
                          "\r\nSec-WebSocket-Version: 13\r\nOrigin: " + o_.origin +
                          "\r\nCache-Control: no-cache\r\nPragma: no-cache\r\n\r\n";
        append(out_, B(req));
    }
    wantWrite_ = false;
    pumpWrite();
    if (!*alive || fd_ < 0) return;
    pumpRead();
    if (!*alive || fd_ < 0) return;
    interest();
}

void WebSocket::pumpWrite() {
    while (outPos_ < out_.size()) {
        int r = rawWrite(out_.data() + outPos_, out_.size() - outPos_);
        if (r < 0) return;
        if (r == 0) {
            fail("write");
            return;
        }
        outPos_ += (size_t)r;
    }
    out_.clear();
    outPos_ = 0;
}

void WebSocket::pumpRead() {
    uint8_t buf[16384];
    auto alive = alive_;
    for (;;) {
        int r = rawRead(buf, sizeof buf);
        if (r < 0) return;
        if (r == 0) {
            fail("closed");
            return;
        }
        in_.insert(in_.end(), buf, buf + r);
        parse();
        if (!*alive || fd_ < 0) return;
    }
}

void WebSocket::parse() {
    auto alive = alive_;
    if (st_ == St::Http) {
        static const char end[] = "\r\n\r\n";
        auto it = std::search(in_.begin(), in_.end(), end, end + 4);
        if (it == in_.end()) {
            if (in_.size() > 65536) fail("http");
            return;
        }
        std::string head(in_.begin(), it);
        in_.erase(in_.begin(), it + 4);
        if (head.rfind("HTTP/1.1 101", 0) != 0) {
            fail("http status " + head.substr(9, 3));
            return;
        }
        // Check the accept token.
        Bytes src = B(wsKey_ + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
        Bytes dg(20);
        unsigned n = 20;
        EVP_Digest(src.data(), src.size(), dg.data(), &n, EVP_sha1(), nullptr);
        std::string want = b64(dg), lower;
        for (char c : head) lower += (char)std::tolower((unsigned char)c);
        auto p = lower.find("sec-websocket-accept:");
        if (p == std::string::npos || head.find(want, p) == std::string::npos) {
            fail("ws accept");
            return;
        }
        wsKey_.clear();
        st_ = St::Open;
        if (onOpen) onOpen();
        if (!*alive || fd_ < 0) return;
    }
    size_t pos = 0;
    while (st_ == St::Open) {
        size_t av = in_.size() - pos;
        if (av < 2) break;
        const uint8_t* d = in_.data() + pos;
        bool fin = d[0] & 0x80;
        uint8_t op = d[0] & 0x0f;
        bool masked = d[1] & 0x80;
        uint64_t len = d[1] & 0x7f;
        size_t hdr = 2;
        if (len == 126) {
            if (av < 4) break;
            len = (uint64_t)d[2] << 8 | d[3];
            hdr = 4;
        } else if (len == 127) {
            if (av < 10) break;
            len = 0;
            for (int i = 0; i < 8; i++) len = len << 8 | d[2 + i];
            hdr = 10;
        }
        if (len > kMaxMessage) {
            fail("oversize");
            return;
        }
        if (masked) hdr += 4;
        if (av < hdr + len) break;
        Bytes payload(d + hdr, d + hdr + len);
        if (masked)
            for (size_t i = 0; i < len; i++) payload[i] ^= d[hdr - 4 + (i & 3)];
        pos += hdr + len;
        switch (op) {
            case 0:
            case 2:
                append(partial_, payload);
                if (partial_.size() > kMaxMessage) {
                    fail("oversize");
                    return;
                }
                if (fin) {
                    Bytes m;
                    m.swap(partial_);
                    if (onMessage) onMessage(std::move(m));
                    if (!*alive || fd_ < 0) return;
                }
                break;
            case 8:
                fail("ws close");
                return;
            case 9:
                sendFrame(10, payload);
                break;
            default:
                break;
        }
    }
    if (fd_ >= 0 && pos) in_.erase(in_.begin(), in_.begin() + (long)pos);
}

void WebSocket::sendFrame(uint8_t op, const Bytes& p) {
    Bytes f;
    f.reserve(p.size() + 14);
    f.push_back(0x80 | op);
    if (p.size() < 126) {
        f.push_back(0x80 | (uint8_t)p.size());
    } else if (p.size() < 65536) {
        f.push_back(0x80 | 126);
        f.push_back((uint8_t)(p.size() >> 8));
        f.push_back((uint8_t)p.size());
    } else {
        f.push_back(0x80 | 127);
        for (int i = 7; i >= 0; i--) f.push_back((uint8_t)((uint64_t)p.size() >> (i * 8)));
    }
    Bytes mask = randomBytes(4);
    append(f, mask);
    size_t start = f.size();
    append(f, p);
    for (size_t i = 0; i < p.size(); i++) f[start + i] ^= mask[i & 3];
    append(out_, f);
    auto alive = alive_;
    pumpWrite();
    if (*alive && fd_ >= 0) interest();
}

void WebSocket::send(const Bytes& payload) {
    if (st_ != St::Open) return;
    sendFrame(2, payload);
}

}  // namespace zw
