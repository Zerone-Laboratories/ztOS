// Non-blocking WebSocket client over TLS (or plain TCP for local tests).
#pragma once
#include <openssl/ssl.h>

#include <deque>
#include <functional>
#include <memory>
#include <string>

#include "crypto.h"
#include "loop.h"

namespace zw {

class WebSocket {
public:
    struct Opts {
        std::string host = "web.whatsapp.com";
        int port = 443;
        std::string path = "/ws/chat";
        std::string origin = "https://web.whatsapp.com";
        bool tls = true;
    };
    std::function<void()> onOpen;
    std::function<void(Bytes&&)> onMessage;
    std::function<void(const std::string&)> onClose;  // reason is safe to log

    explicit WebSocket(Loop& l) : loop_(l), alive_(std::make_shared<bool>(true)) {}
    ~WebSocket();
    void connect(const Opts& o);
    void send(const Bytes& payload);  // one binary message
    void close();                     // silent: onClose does not fire

private:
    enum class St { Idle, Resolving, Connecting, Tls, Http, Open };
    void connected();
    void io(uint32_t ev);
    void fail(const std::string& why);
    void pumpRead();
    void pumpWrite();
    int rawRead(uint8_t* b, size_t n);
    int rawWrite(const uint8_t* b, size_t n);
    void parse();
    void sendFrame(uint8_t op, const Bytes& p);
    void interest();
    void cleanup();

    Loop& loop_;
    Opts o_;
    St st_ = St::Idle;
    int fd_ = -1;
    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
    bool wantWrite_ = false;  // TLS asked for writability
    Bytes in_, out_;
    size_t outPos_ = 0;
    Bytes partial_;
    std::string wsKey_;
    std::shared_ptr<bool> alive_;
};

}  // namespace zw
