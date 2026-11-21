// A single-threaded epoll loop. It sleeps until a socket is ready or a timer falls
// due, and never polls. Worker threads hand results back with post().
#pragma once
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace zw {

class Loop {
public:
    using Clock = std::chrono::steady_clock;
    using FdFn = std::function<void(uint32_t)>;
    using Fn = std::function<void()>;

    Loop() {
        ep_ = epoll_create1(EPOLL_CLOEXEC);
        ev_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        add(ev_, EPOLLIN, [this](uint32_t) {
            uint64_t v;
            (void)!read(ev_, &v, sizeof v);
            std::vector<Fn> q;
            {
                std::lock_guard l(m_);
                q.swap(posted_);
            }
            for (auto& f : q) f();
        });
    }
    ~Loop() {
        close(ev_);
        close(ep_);
    }
    void add(int fd, uint32_t events, FdFn fn) {
        fds_[fd] = std::move(fn);
        epoll_event e{};
        e.events = events;
        e.data.fd = fd;
        epoll_ctl(ep_, EPOLL_CTL_ADD, fd, &e);
    }
    void mod(int fd, uint32_t events) {
        epoll_event e{};
        e.events = events;
        e.data.fd = fd;
        epoll_ctl(ep_, EPOLL_CTL_MOD, fd, &e);
    }
    void del(int fd) {
        epoll_ctl(ep_, EPOLL_CTL_DEL, fd, nullptr);
        fds_.erase(fd);
    }
    uint64_t after(int ms, Fn fn) {
        uint64_t id = ++nextTimer_;
        timers_.emplace(Clock::now() + std::chrono::milliseconds(ms), Timer{id, std::move(fn)});
        return id;
    }
    void cancel(uint64_t id) {
        if (!id) return;
        for (auto it = timers_.begin(); it != timers_.end(); ++it)
            if (it->second.id == id) {
                timers_.erase(it);
                return;
            }
    }
    // Safe from any thread.
    void post(Fn fn) {
        {
            std::lock_guard l(m_);
            posted_.push_back(std::move(fn));
        }
        uint64_t one = 1;
        (void)!write(ev_, &one, sizeof one);
    }
    void stop() { stop_ = true; }
    void run() {
        while (!stop_) tick(-1);
    }
    // For tests: blocks at most maxMs in one pass.
    void tick(int maxMs) {
        epoll_event evs[16];
        {
            int to = maxMs;
            if (!timers_.empty()) {
                auto d = std::chrono::duration_cast<std::chrono::milliseconds>(timers_.begin()->first - Clock::now()).count();
                int due = d <= 0 ? 0 : (int)(d > 3600000 ? 3600000 : d + 1);
                to = (to < 0 || due < to) ? due : to;
            }
            int n = epoll_wait(ep_, evs, 16, to);
            for (int i = 0; i < n; i++) {
                auto it = fds_.find(evs[i].data.fd);
                if (it == fds_.end()) continue;
                auto fn = it->second;  // the callback may remove itself
                fn(evs[i].events);
            }
            auto now = Clock::now();
            while (!timers_.empty() && timers_.begin()->first <= now) {
                Fn f = std::move(timers_.begin()->second.fn);
                timers_.erase(timers_.begin());
                f();
            }
        }
    }

private:
    struct Timer {
        uint64_t id;
        Fn fn;
    };
    int ep_, ev_;
    bool stop_ = false;
    uint64_t nextTimer_ = 0;
    std::unordered_map<int, FdFn> fds_;
    std::multimap<Clock::time_point, Timer> timers_;
    std::mutex m_;
    std::vector<Fn> posted_;
};

}  // namespace zw
