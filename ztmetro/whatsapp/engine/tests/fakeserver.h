// A local stand-in for the WhatsApp server: WebSocket, framing, the responder side of
// Noise, and node exchange. It encodes what we believe the server does, so it proves
// the client is self-consistent and robust, not that the real server agrees.
#pragma once
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <deque>
#include <functional>

#include "../src/loop.h"
#include <openssl/evp.h>

#include <algorithm>
#include "../src/node.h"
#include "../src/noise.h"
#include "../src/pb.h"

namespace zw {

class FakeServer {
public:
    KeyPair root = genKeyPair(), inter = genKeyPair(), stat = genKeyPair();
    std::function<void(const Bytes& payload)> onLogin;
    std::function<void(const Node&)> onNode;
    int port = 0;
    int connections = 0;
    bool badCert = false;

    explicit FakeServer(Loop& l) : loop_(l) {
        lfd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        int one = 1;
        setsockopt(lfd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(lfd_, (sockaddr*)&a, sizeof a);
        listen(lfd_, 4);
        socklen_t al = sizeof a;
        getsockname(lfd_, (sockaddr*)&a, &al);
        port = ntohs(a.sin_port);
        loop_.add(lfd_, EPOLLIN, [this](uint32_t) { accept_(); });
    }
    ~FakeServer() {
        drop();
        loop_.del(lfd_);
        close(lfd_);
    }
    bool up() const { return fd_ >= 0 && st_ == St::Live; }
    void send(const Node& n) {
        if (up()) rawSend(tp_.seal(encodeNode(n)));
    }
    void sendGarbage() {
        if (up()) rawSend(tp_.seal(Bytes{0, 255, 3, 3}));
    }
    void sendEncryptedRaw(const Bytes& plain) {
        if (up()) rawSend(tp_.seal(plain));
    }
    void drop() {
        if (fd_ >= 0) {
            loop_.del(fd_);
            close(fd_);
            fd_ = -1;
        }
        st_ = St::Http;
    }

private:
    enum class St { Http, Header, Hello, Finish, Live };

    void accept_() {
        int f = accept4(lfd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (f < 0) return;
        drop();
        fd_ = f;
        connections++;
        st_ = St::Http;
        in_.clear();
        ws_.clear();
        loop_.add(fd_, EPOLLIN, [this](uint32_t) { read_(); });
    }
    void read_() {
        uint8_t b[65536];
        for (;;) {
            ssize_t r = recv(fd_, b, sizeof b, 0);
            if (r == 0) {
                drop();
                return;
            }
            if (r < 0) return;
            in_.insert(in_.end(), b, b + r);
            int f = fd_;
            parseWs();
            if (fd_ != f) return;
        }
    }
    void parseWs() {
        if (st_ == St::Http) {
            static const char end[] = "\r\n\r\n";
            auto it = std::search(in_.begin(), in_.end(), end, end + 4);
            if (it == in_.end()) return;
            std::string h(in_.begin(), it);
            in_.erase(in_.begin(), it + 4);
            auto p = h.find("Sec-WebSocket-Key: ");
            std::string key = h.substr(p + 19, h.find("\r\n", p) - p - 19);
            Bytes src = B(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
            Bytes dg(20);
            unsigned n = 20;
            EVP_Digest(src.data(), src.size(), dg.data(), &n, EVP_sha1(), nullptr);
            std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + b64(dg) + "\r\n\r\n";
            ::send(fd_, resp.data(), resp.size(), MSG_NOSIGNAL);
            st_ = St::Header;
        }
        for (;;) {
            if (in_.size() < 2) return;
            uint64_t len = in_[1] & 0x7f;
            size_t hdr = 2;
            if (len == 126) {
                if (in_.size() < 4) return;
                len = (uint64_t)in_[2] << 8 | in_[3];
                hdr = 4;
            } else if (len == 127) {
                if (in_.size() < 10) return;
                len = 0;
                for (int i = 0; i < 8; i++) len = len << 8 | in_[2 + i];
                hdr = 10;
            }
            if (!(in_[1] & 0x80)) throw std::runtime_error("client frames must be masked");
            if (in_.size() < hdr + 4 + len) return;
            Bytes pl(in_.begin() + (long)hdr + 4, in_.begin() + (long)(hdr + 4 + len));
            for (size_t i = 0; i < len; i++) pl[i] ^= in_[hdr + (i & 3)];
            in_.erase(in_.begin(), in_.begin() + (long)(hdr + 4 + len));
            append(ws_, pl);
            int f = fd_;
            frames();
            if (fd_ != f) return;
        }
    }
    void frames() {
        if (st_ == St::Header) {
            if (ws_.size() < 4) return;
            if (ws_[0] != 'W' || ws_[1] != 'A' || ws_[2] != 6 || ws_[3] != dictVersion()) throw std::runtime_error("bad WA header");
            header_.assign(ws_.begin(), ws_.begin() + 4);
            ws_.erase(ws_.begin(), ws_.begin() + 4);
            st_ = St::Hello;
        }
        while (ws_.size() >= 3) {
            size_t l = (size_t)ws_[0] << 16 | (size_t)ws_[1] << 8 | ws_[2];
            if (ws_.size() < 3 + l) return;
            Bytes f(ws_.begin() + 3, ws_.begin() + 3 + (long)l);
            ws_.erase(ws_.begin(), ws_.begin() + 3 + (long)l);
            int cur = fd_;
            frame(f);
            if (fd_ != cur) return;
        }
    }
    Bytes chain() {
        uint64_t t = (uint64_t)time(nullptr);
        pb::Writer id, ld;
        id.u(1, 7).u(2, 0).b(3, B(inter.pub)).u(4, t - 100).u(5, t + 100000);
        ld.u(1, 8).u(2, 7).b(3, B(stat.pub)).u(4, t - 100).u(5, t + 100000);
        auto mk = [](const Bytes& det, const Key32& sk) {
            pb::Writer c;
            auto s = xedSign(sk, det);
            c.b(1, det).b(2, Bytes(s.begin(), s.end()));
            return c;
        };
        pb::Writer ch;
        ch.m(1, mk(ld.out, badCert ? stat.priv : inter.priv)).m(2, mk(id.out, root.priv));
        return ch.out;
    }
    void frame(const Bytes& f) {
        if (st_ == St::Hello) {
            pb::Msg m(f);
            auto hello = m.sub(2);
            if (!hello) throw std::runtime_error("expected client hello");
            Key32 ce = toKey(hello->bytes(1));
            se_ = genKeyPair();
            n_ = NoiseState{};
            n_.start(header_);
            n_.authenticate(B(ce));
            n_.authenticate(B(se_.pub));
            n_.mix(se_.priv, ce);
            Bytes ct1 = n_.encrypt(B(stat.pub));
            n_.mix(stat.priv, ce);
            Bytes ct2 = n_.encrypt(chain());
            pb::Writer sh;
            sh.b(1, B(se_.pub)).b(2, ct1).b(3, ct2);
            pb::Writer w;
            w.m(3, sh);
            rawSend(w.out);
            st_ = St::Finish;
        } else if (st_ == St::Finish) {
            pb::Msg m(f);
            auto fin = m.sub(4);
            if (!fin) throw std::runtime_error("expected client finish");
            Bytes cs = n_.decrypt(fin->bytes(1));
            n_.mix(se_.priv, toKey(cs));
            Bytes payload = n_.decrypt(fin->bytes(2));
            tp_ = NoiseTransport{};
            n_.finish(tp_.readKey, tp_.writeKey);  // mirrored from the client
            st_ = St::Live;
            clientStatic = toKey(cs);
            if (onLogin) onLogin(payload);
        } else if (st_ == St::Live) {
            Node n = decodeFrame(tp_.open(f));
            if (onNode) onNode(n);
        }
    }
    void rawSend(const Bytes& p) {
        Bytes f{0x82};
        size_t n = p.size() + 3;
        if (n < 126) {
            f.push_back((uint8_t)n);
        } else if (n < 65536) {
            f.push_back(126);
            f.push_back((uint8_t)(n >> 8));
            f.push_back((uint8_t)n);
        } else {
            f.push_back(127);
            for (int i = 7; i >= 0; i--) f.push_back((uint8_t)((uint64_t)n >> (i * 8)));
        }
        f.push_back((uint8_t)(p.size() >> 16));
        f.push_back((uint8_t)(p.size() >> 8));
        f.push_back((uint8_t)p.size());
        append(f, p);
        size_t off = 0;
        while (off < f.size()) {
            ssize_t r = ::send(fd_, f.data() + off, f.size() - off, MSG_NOSIGNAL);
            if (r <= 0) {
                if (errno == EAGAIN) continue;
                return;
            }
            off += (size_t)r;
        }
    }

public:
    Key32 clientStatic{};

private:
    Loop& loop_;
    int lfd_ = -1, fd_ = -1;
    St st_ = St::Http;
    Bytes in_, ws_, header_;
    KeyPair se_;
    NoiseState n_;
    NoiseTransport tp_;
};

}  // namespace zw
