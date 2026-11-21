// The Unix socket server of IPC.md, and the interface both engines implement.
#pragma once
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <string>

#include "loop.h"

namespace zw {

using nlohmann::json;

struct IpcError {
    std::string code, message;
};

// Sends exactly one response. May be kept and called later; harmless if the client left.
struct Reply {
    std::function<void(const json&)> ok;
    std::function<void(const std::string& code, const std::string& msg)> err;
};

class Engine {
public:
    virtual ~Engine() = default;
    virtual void command(const std::string& cmd, const json& req, Reply reply) = 0;
    // Lines sent to each new client straight after hello.
    virtual std::vector<json> greeting() = 0;
};

class IpcServer {
public:
    IpcServer(Loop& l, const std::string& path);
    ~IpcServer();
    void setEngine(Engine* e) { engine_ = e; }
    void broadcast(const json& ev);

private:
    struct Client {
        std::string in, out;
        size_t outPos = 0;
    };
    void accept_();
    void io(int fd, uint32_t ev);
    void line(int fd, const std::string& l);
    void sendTo(int fd, const json& j);
    void flush(int fd);
    void drop(int fd);

    Loop& loop_;
    std::string path_;
    int lfd_ = -1;
    Engine* engine_ = nullptr;
    std::map<int, Client> clients_;
    uint64_t gen_ = 0;
    std::map<int, uint64_t> gens_;
};

inline constexpr int kIpcVersion = 1;

}  // namespace zw
