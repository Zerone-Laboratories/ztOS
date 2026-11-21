#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include "engine.h"

using namespace zw;

static std::string env(const char* k, const std::string& def = "") {
    const char* v = getenv(k);
    return v && *v ? v : def;
}

int main(int argc, char** argv) {
    bool fake = false, probe = false;
    std::string sock, data;
    RealConfig cfg;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s needs a value\n", a.c_str());
                exit(2);
            }
            return argv[++i];
        };
        if (a == "--fake") fake = true;
        else if (a == "--socket") sock = next();
        else if (a == "--data") data = next();
        else if (a == "--probe-handshake") probe = true;
        else if (a == "--test-server") {  // host:port, plain TCP. Local fake server only.
            std::string v = next();
            auto c = v.rfind(':');
            cfg.host = v.substr(0, c);
            cfg.port = std::stoi(v.substr(c + 1));
            cfg.tls = false;
        } else if (a == "--test-root") {
            cfg.rootKey = toKey(unhex(next()));
        } else if (a == "--test-media") {
            cfg.mediaBase = next();
        } else {
            fprintf(stderr,
                    "usage: ztwhatsappd [--fake] [--socket PATH] [--data DIR] [--probe-handshake]\n"
                    "See ztmetro/whatsapp/IPC.md.\n");
            return 2;
        }
    }
    cryptoInit();
    umask(0077);

    std::string runtime = env("XDG_RUNTIME_DIR", "/tmp");
    if (sock.empty()) sock = runtime + "/ztwhatsapp.sock";
    if (data.empty()) data = env("XDG_DATA_HOME", env("HOME", ".") + "/.local/share") + "/ztwhatsapp";
    cfg.dataDir = data;

    // The probe uses a throwaway identity and must never see the real store or socket.
    std::string probeDir;
    if (probe) {
        std::string t = env("TMPDIR", "/tmp") + "/ztwa-probe-XXXXXX";
        char* d = mkdtemp(t.data());
        if (!d) {
            perror("mkdtemp");
            return 1;
        }
        probeDir = d;
        cfg.dataDir = probeDir;
        cfg.probeOnly = true;
        sock = probeDir + "/s.sock";
    }

    std::string lockPath = sock + ".lock";
    int lock = open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "ztwhatsappd: another instance holds %s\n", lockPath.c_str());
        return 3;
    }

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    sigprocmask(SIG_BLOCK, &set, nullptr);
    signal(SIGPIPE, SIG_IGN);
    int sfd = signalfd(-1, &set, SFD_CLOEXEC | SFD_NONBLOCK);

    int rc = 0;
    try {
        // Deliberately never freed: a detached download thread may still post to it at exit.
        Loop& loop = *new Loop;
        loop.add(sfd, EPOLLIN, [&](uint32_t) { loop.stop(); });
        IpcServer ipc(loop, sock);
        std::unique_ptr<Engine> engine = fake ? makeFakeEngine(loop, ipc) : makeRealEngine(loop, ipc, cfg);
        ipc.setEngine(engine.get());
        if (probe) {
            engine->command("pair_qr", json::object(), Reply{[](const json&) {}, [](const std::string&, const std::string&) {}});
            loop.after(30000, [&] {
                printf("probe: no result within 30 s\n");
                rc = 1;
                loop.stop();
            });
        }
        loop.run();
    } catch (const std::exception& e) {
        fprintf(stderr, "ztwhatsappd: %s\n", e.what());
        rc = 1;
    }
    unlink(lockPath.c_str());
    if (!probeDir.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(probeDir, ec);
    }
    return rc;
}
