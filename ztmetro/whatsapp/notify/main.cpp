// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztwhatsapp-notify: ztwhatsappd's new messages as freedesktop notifications.
 *
 * A client of $XDG_RUNTIME_DIR/ztwhatsapp.sock (IPC.md) and of
 * org.freedesktop.Notifications (ztnotifyd). It blocks in poll: no periodic
 * timer runs while nothing is due, and none of its clocks can wake the SoC.
 * After a long enough sleep it holds a short inhibit through org.ztos.ial's
 * Sleep1, so the engine has time to reconnect. See README.md.
 *
 * It logs counts and states only, never a name, number or message.
 */
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <systemd/sd-bus.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "json.hpp"
#include "policy.hpp"

namespace fs = std::filesystem;
using namespace zwn;

namespace {

constexpr uint64_t BUS_TIMEOUT_US = 5000000;
constexpr size_t MAX_LINE = 4 * 1024 * 1024;
constexpr const char *APP = "WhatsApp";
constexpr const char *IAL_NAME = "org.ztos.ial";
constexpr const char *IAL_PATH = "/org/ztos/ial/Sleep";
constexpr const char *IAL_IFACE = "org.ztos.ial.Sleep1";

volatile sig_atomic_t stopping;

int64_t now_ms()
{
	timespec ts{};
	clock_gettime(CLOCK_MONOTONIC, &ts);	// stops in sleep, never wakes it
	return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

std::string env_or(const char *name, const std::string &otherwise)
{
	const char *v = getenv(name);
	return v && *v ? v : otherwise;
}

bool write_atomic(const fs::path &path, const std::string &text)
{
	fs::path tmp = path;
	tmp += ".new";
	int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0)
		return false;
	bool ok = write(fd, text.data(), text.size()) == ssize_t(text.size());
	close(fd);
	return (ok && rename(tmp.c_str(), path.c_str()) == 0) || (unlink(tmp.c_str()), false);
}

struct Options {
	std::string runtime, socket, sleep_status, ui_command = "ztwhatsapp", state_dir;
	bool ial_on_user_bus = false;	// tests only: no system bus to fake
};

class App {
public:
	explicit App(Options o);
	~App();
	int run();

	int action_invoked(uint32_t id, const std::string &action);
	void notification_closed(uint32_t id);

private:
	// the engine
	void engine_connect();
	void engine_drop();
	void engine_read();
	void engine_line(std::string_view line);
	void engine_event(const json::Value &v);
	// the notification service
	void bus_connect();
	void bus_drop();
	void flush();
	bool send(const Shown &s, uint32_t &id);
	void close_note(uint32_t id);
	std::string focus() const;
	// sleep
	void sleep_watch();
	void sleep_read();
	void hold_begin();
	void hold_end();
	void status_write();
	void watermark_save();

	Options o_;
	Policy policy_;
	WakeHold hold_;
	int engine_ = -1;
	std::string buffer_;
	int64_t engine_retry_ = 0, engine_wait_ = 1000;
	bool skipping_ = false;
	sd_bus *bus_ = nullptr, *ial_ = nullptr;
	int64_t bus_retry_ = 0;
	bool bus_warned_ = false;
	int inotify_ = -1, watch_ = -1;
	int64_t watch_retry_ = 0;
	int64_t wakes_ = -1;
	uint32_t cookie_ = 0;
	bool ial_warned_ = false;
	std::string state_ = "offline", pairing_ = "unknown";
	int64_t saved_ = 0;
	unsigned long told_ = 0;

	static int64_t load_watermark(const Options &o);
};

fs::path watermark_path(const Options &o) { return fs::path(o.state_dir) / "notify-watermark"; }

int64_t App::load_watermark(const Options &o)
{
	std::ifstream in(watermark_path(o));
	int64_t v = 0;
	if (in >> v && v > 0)
		return v;
	return int64_t(time(nullptr));	// first run: history is not news
}

App::App(Options o) : o_(std::move(o)), policy_(load_watermark(o_))
{
	saved_ = policy_.high_water();
	inotify_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
}

App::~App()
{
	hold_end();
	watermark_save();
	fs::path status = fs::path(o_.runtime) / "ztwhatsapp-notify.status";
	unlink(status.c_str());
	if (engine_ >= 0)
		close(engine_);
	if (inotify_ >= 0)
		close(inotify_);
	sd_bus_flush_close_unref(bus_);
	sd_bus_flush_close_unref(ial_);
}

void App::watermark_save()
{
	int64_t high = policy_.high_water();
	if (high == saved_)
		return;
	std::error_code ec;
	fs::create_directories(o_.state_dir, ec);
	if (write_atomic(watermark_path(o_), std::to_string(high) + "\n"))
		saved_ = high;
}

// zrn-sleepd reads paired= to know the engine wants doze windows.
void App::status_write()
{
	bool paired = pairing_ == "paired" && state_ != "logged_out";
	write_atomic(fs::path(o_.runtime) / "ztwhatsapp-notify.status",
		"paired=" + std::string(paired ? "1" : "0") + "\nconnection=" + state_ +
		"\nnotified=" + std::to_string(told_) + "\n");
}

// ---- the engine ------------------------------------------------------------

void App::engine_connect()
{
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	if (fd < 0 || o_.socket.size() >= sizeof(addr.sun_path)) {
		if (fd >= 0)
			close(fd);
		engine_retry_ = now_ms() + 30000;
		return;
	}
	memcpy(addr.sun_path, o_.socket.c_str(), o_.socket.size() + 1);
	if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
		close(fd);
		engine_retry_ = now_ms() + engine_wait_;
		engine_wait_ = std::min<int64_t>(engine_wait_ * 2, 30000);
		return;
	}
	engine_ = fd;
	engine_wait_ = 1000;
	buffer_.clear();
	skipping_ = false;
	fprintf(stderr, "ztwhatsapp-notify: connected to the engine\n");
}

void App::engine_drop()
{
	if (engine_ >= 0)
		close(engine_);
	engine_ = -1;
	state_ = "offline";
	hold_.connection(state_, now_ms());
	engine_retry_ = now_ms() + engine_wait_;
	status_write();
	fprintf(stderr, "ztwhatsapp-notify: the engine went away\n");
}

void App::engine_read()
{
	char chunk[65536];
	for (;;) {
		ssize_t n = read(engine_, chunk, sizeof(chunk));
		if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
			engine_drop();
			return;
		}
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return;
		}
		size_t start = buffer_.size();
		buffer_.append(chunk, size_t(n));
		size_t from = 0, nl;
		while ((nl = buffer_.find('\n', std::max(from, start))) != std::string::npos) {
			if (!skipping_)
				engine_line(std::string_view(buffer_).substr(from, nl - from));
			skipping_ = false;
			from = nl + 1;
			start = from;
		}
		buffer_.erase(0, from);
		if (buffer_.size() > MAX_LINE) {	// past the contract: drop to its end
			buffer_.clear();
			skipping_ = true;
		}
	}
}

void App::engine_line(std::string_view line)
{
	auto v = json::parse(line);
	if (!v || v->type != json::Value::Type::Object) {
		fprintf(stderr, "ztwhatsapp-notify: a line that is not JSON (%zu bytes)\n", line.size());
		return;
	}
	if (v->find("event"))
		engine_event(*v);
}

void App::engine_event(const json::Value &v)
{
	std::string event = v.str("event");
	int64_t now = now_ms();
	if (event == "message") {
		Incoming m;
		m.chat = v.str("chat");
		m.chat_name = v.str("chat_name");
		m.sender_name = v.str("sender_name");
		m.text = v.str("text");
		m.id = v.str("id");
		m.timestamp = v.integer("timestamp");
		m.from_me = v.flag("from_me");
		m.is_group = v.flag("is_group");
		m.muted = v.flag("muted");
		m.live = v.flag("live");
		// the top-level copies are a convenience: the Message object is the record
		if (const json::Value *inner = v.find("message")) {
			m.kind = inner->str("kind");
			if (m.text.empty())
				m.text = inner->str("text");
			if (m.sender_name.empty())
				m.sender_name = inner->str("sender_name");
			if (!m.timestamp)
				m.timestamp = inner->integer("timestamp");
		}
		hold_.activity(now);
		policy_.message(m, focus(), now);
	} else if (event == "chat") {
		if (const json::Value *chat = v.find("chat")) {
			if (chat->integer("unread", 1) == 0)
				close_note(policy_.chat_read(chat->str("id")));
		}
	} else if (event == "connection") {
		std::string state = v.str("state");
		if (state == "connected" && state_ != "connected")
			policy_.connected();
		if (state != "connected" && state_ == "connected")
			watermark_save();
		state_ = state;
		hold_.connection(state_, now);
		status_write();
	} else if (event == "pairing") {
		std::string state = v.str("state");
		if (state == "paired" || state == "unpaired") {
			pairing_ = state;
			status_write();
		}
	} else if (event == "history_sync") {
		policy_.history_running(v.str("state") == "running");
	} else if (event == "hello") {
		if (v.integer("ipc_version") != 1)
			fprintf(stderr, "ztwhatsapp-notify: IPC version %lld, expected 1\n",
				static_cast<long long>(v.integer("ipc_version")));
	}
}

// The UI writes "PID CHAT" while a chat is on screen, and removes the file when
// hidden. Read per message: a crashed UI's file is dead by its PID.
std::string App::focus() const
{
	std::ifstream in(fs::path(o_.runtime) / "ztwhatsapp.focus");
	long pid = 0;
	std::string chat;
	if (!(in >> pid >> chat) || pid <= 0)
		return {};
	if (kill(pid_t(pid), 0) < 0 && errno == ESRCH)
		return {};
	return chat;
}

// ---- the notification service ----------------------------------------------

int on_action(sd_bus_message *m, void *data, sd_bus_error *)
{
	uint32_t id = 0;
	const char *action = nullptr;
	if (sd_bus_message_read(m, "us", &id, &action) >= 0 && action)
		static_cast<App *>(data)->action_invoked(id, action);
	return 0;
}

int on_closed(sd_bus_message *m, void *data, sd_bus_error *)
{
	uint32_t id = 0, reason = 0;
	if (sd_bus_message_read(m, "uu", &id, &reason) >= 0)
		static_cast<App *>(data)->notification_closed(id);
	return 0;
}

void App::bus_connect()
{
	if (sd_bus_open_user(&bus_) < 0) {
		bus_ = nullptr;
		bus_retry_ = now_ms() + 5000;
		return;
	}
	sd_bus_add_match(bus_, nullptr, "type='signal',interface='org.freedesktop.Notifications',"
		"member='ActionInvoked',path='/org/freedesktop/Notifications'", on_action, this);
	sd_bus_add_match(bus_, nullptr, "type='signal',interface='org.freedesktop.Notifications',"
		"member='NotificationClosed',path='/org/freedesktop/Notifications'", on_closed, this);
}

void App::bus_drop()
{
	sd_bus_flush_close_unref(bus_);
	bus_ = nullptr;
	bus_retry_ = now_ms() + 5000;
	fprintf(stderr, "ztwhatsapp-notify: the session bus went away\n");
}

bool App::send(const Shown &s, uint32_t &id)
{
	sd_bus_message *call = nullptr, *reply = nullptr;
	sd_bus_error error = SD_BUS_ERROR_NULL;
	bool ok = false;
	int r = sd_bus_message_new_method_call(bus_, &call, "org.freedesktop.Notifications",
		"/org/freedesktop/Notifications", "org.freedesktop.Notifications", "Notify");
	if (r >= 0)
		r = sd_bus_message_append(call, "susss", APP, s.replaces, "", s.title.c_str(), s.body.c_str());
	if (r >= 0)
		r = sd_bus_message_append(call, "as", 2, "default", "Open");
	if (r >= 0)
		r = sd_bus_message_open_container(call, 'a', "{sv}");
	const auto hint = [&](const char *key, auto... value) {
		if (r >= 0)
			r = sd_bus_message_append(call, "{sv}", key, value...);
	};
	hint("desktop-entry", "s", "ztwhatsapp");
	hint("category", "s", "im.received");
	if (!s.alert)
		hint("suppress-sound", "b", 1);
	if (r >= 0)
		r = sd_bus_message_close_container(call);
	if (r >= 0)
		r = sd_bus_message_append(call, "i", -1);
	if (r >= 0)
		r = sd_bus_call(bus_, call, BUS_TIMEOUT_US, &error, &reply);
	if (r >= 0 && sd_bus_message_read(reply, "u", &id) >= 0) {
		ok = true;
		bus_warned_ = false;
	} else if (!bus_warned_) {
		fprintf(stderr, "ztwhatsapp-notify: no notification service yet: %s\n",
			error.name ? error.name : strerror(-r));
		bus_warned_ = true;
	}
	sd_bus_error_free(&error);
	sd_bus_message_unref(reply);
	sd_bus_message_unref(call);
	return ok;
}

void App::close_note(uint32_t id)
{
	if (!id || !bus_)
		return;
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_call_method(bus_, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
		"org.freedesktop.Notifications", "CloseNotification", &error, nullptr, "u", id);
	sd_bus_error_free(&error);
}

void App::flush()
{
	int64_t now = now_ms();
	std::vector<Shown> due = policy_.flush(now);
	for (const Shown &s : due) {
		uint32_t id = 0;
		if (bus_ && send(s, id)) {
			policy_.assigned(s.chat, id);
			told_++;
		} else {
			policy_.retry(s, now);
		}
	}
	if (!due.empty()) {
		status_write();
		watermark_save();
	}
}

int App::action_invoked(uint32_t id, const std::string &action)
{
	std::string chat = policy_.chat_of(id);
	if (action != "default" || chat.empty())
		return 0;
	// the UI reads this at start and on change, so it works with the UI closed
	write_atomic(fs::path(o_.runtime) / "ztwhatsapp.open", chat + "\n");
	fs::path ctl = fs::path(o_.runtime) / "ztmetro.ctl";
	int fd = open(ctl.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd >= 0) {
		std::string line = "launch " + o_.ui_command + "\n";
		if (write(fd, line.data(), line.size()) < 0)
			fprintf(stderr, "ztwhatsapp-notify: the shell took no line: %s\n", strerror(errno));
		close(fd);
	}
	return 1;
}

void App::notification_closed(uint32_t id)
{
	policy_.closed(id);
}

// ---- sleep -----------------------------------------------------------------

void App::sleep_watch()
{
	if (inotify_ < 0 || watch_ >= 0 || now_ms() < watch_retry_)
		return;
	fs::path dir = fs::path(o_.sleep_status).parent_path();
	watch_ = inotify_add_watch(inotify_, dir.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO);
	if (watch_ < 0) {
		watch_retry_ = now_ms() + 30000;	// a phone without zrn-sleepd
		return;
	}
	sleep_read();
}

void App::sleep_read()
{
	std::ifstream in(o_.sleep_status);
	std::map<std::string, std::string> kv;
	std::string line;
	while (std::getline(in, line)) {
		auto eq = line.find('=');
		if (eq != std::string::npos)
			kv[line.substr(0, eq)] = line.substr(eq + 1);
	}
	auto it = kv.find("wakes");
	if (it == kv.end())
		return;
	int64_t wakes = atoll(it->second.c_str());
	bool woke = wakes_ >= 0 && wakes > wakes_;
	wakes_ = wakes;
	if (!woke)
		return;
	if (WakeHold::worth_holding(kv["last"], atoll(kv["slept"].c_str())))
		hold_begin();
}

void App::hold_begin()
{
	if (pairing_ != "paired" || state_ == "logged_out" || hold_.active())
		return;
	if (!ial_) {
		int r = o_.ial_on_user_bus ? sd_bus_open_user(&ial_) : sd_bus_open_system(&ial_);
		if (r < 0) {
			ial_ = nullptr;
			return;
		}
	}
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = nullptr;
	int r = sd_bus_call_method(ial_, IAL_NAME, IAL_PATH, IAL_IFACE, "Inhibit", &error, &reply,
		"ss", "ztwhatsapp-notify", "reconnect after sleep");
	if (r >= 0 && sd_bus_message_read(reply, "u", &cookie_) >= 0) {
		hold_.start(now_ms(), state_);
		fprintf(stderr, "ztwhatsapp-notify: holding the wake\n");
	} else if (!ial_warned_) {
		fprintf(stderr, "ztwhatsapp-notify: no Sleep1 inhibit: %s\n", error.name ? error.name : strerror(-r));
		ial_warned_ = true;
	}
	sd_bus_error_free(&error);
	sd_bus_message_unref(reply);
}

void App::hold_end()
{
	if (!hold_.active())
		return;
	hold_.stop();
	if (ial_ && cookie_) {
		sd_bus_error error = SD_BUS_ERROR_NULL;
		int r = sd_bus_call_method(ial_, IAL_NAME, IAL_PATH, IAL_IFACE, "Uninhibit", &error, nullptr, "u", cookie_);
		if (r < 0) {
			// its caller going ends it at once, and it lapses by itself anyway
			fprintf(stderr, "ztwhatsapp-notify: Uninhibit failed, dropping the connection\n");
			sd_bus_flush_close_unref(ial_);
			ial_ = nullptr;
		}
		sd_bus_error_free(&error);
	}
	cookie_ = 0;
	fprintf(stderr, "ztwhatsapp-notify: wake released\n");
}

// ---- the loop --------------------------------------------------------------

int App::run()
{
	status_write();
	while (!stopping) {
		int64_t now = now_ms();
		if (!bus_ && now >= bus_retry_)
			bus_connect();
		if (engine_ < 0 && now >= engine_retry_)
			engine_connect();
		sleep_watch();
		now = now_ms();
		if (policy_.next_due() >= 0 && policy_.next_due() <= now)
			flush();
		if (hold_.should_release(now))
			hold_end();

		if (bus_) {
			int r;
			while ((r = sd_bus_process(bus_, nullptr)) > 0) {
			}
			if (r < 0 && r != -EINTR)
				bus_drop();
		}
		now = now_ms();
		int64_t wake = INT64_MAX;
		const auto soon = [&](int64_t at) {
			if (at >= 0)
				wake = std::min(wake, at);
		};
		soon(policy_.next_due());
		soon(hold_.next_check(now));
		if (engine_ < 0)
			soon(engine_retry_);
		if (!bus_)
			soon(bus_retry_);
		if (watch_ < 0 && inotify_ >= 0)
			soon(watch_retry_);

		pollfd fds[3];
		nfds_t n = 0;
		int engine_at = -1, bus_at = -1, inotify_at = -1;
		if (engine_ >= 0) {
			engine_at = int(n);
			fds[n++] = {engine_, POLLIN, 0};
		}
		if (bus_) {
			bus_at = int(n);
			int events = sd_bus_get_events(bus_);
			fds[n++] = {sd_bus_get_fd(bus_), short(events > 0 ? events : 0), 0};
			uint64_t until = 0;
			if (sd_bus_get_timeout(bus_, &until) >= 0 && until != UINT64_MAX) {
				uint64_t usec = uint64_t(now) * 1000;	// its clock is CLOCK_MONOTONIC
				soon(now + int64_t(until > usec ? (until - usec) / 1000 : 0));
			}
		}
		if (watch_ >= 0) {
			inotify_at = int(n);
			fds[n++] = {inotify_, POLLIN, 0};
		}
		int timeout = -1;	// nothing due: sleep until something speaks
		if (wake != INT64_MAX)
			timeout = int(std::clamp<int64_t>(wake - now, 0, INT_MAX));
		if (poll(fds, n, timeout) < 0 && errno != EINTR)
			return 1;
		if (engine_at >= 0 && fds[engine_at].revents)
			engine_read();
		if (inotify_at >= 0 && (fds[inotify_at].revents & POLLIN)) {
			char events[4096];
			bool changed = false, gone = false;
			ssize_t got;
			while ((got = read(inotify_, events, sizeof(events))) > 0) {
				for (char *p = events; p < events + got;) {
					auto *e = reinterpret_cast<inotify_event *>(p);
					if (e->mask & IN_IGNORED)
						gone = true;
					else if (e->len && fs::path(o_.sleep_status).filename() == e->name)
						changed = true;
					p += sizeof(inotify_event) + e->len;
				}
			}
			if (gone) {
				watch_ = -1;
				watch_retry_ = now_ms() + 30000;
			}
			if (changed)
				sleep_read();
		}
		(void)bus_at;
	}
	return 0;
}

void on_stop(int)
{
	stopping = 1;
}

}

int main(int argc, char **argv)
{
	setvbuf(stderr, nullptr, _IOLBF, 0);
	Options o;
	o.runtime = env_or("XDG_RUNTIME_DIR", "");
	if (o.runtime.empty()) {
		fprintf(stderr, "ztwhatsapp-notify: no XDG_RUNTIME_DIR\n");
		return 1;
	}
	o.socket = o.runtime + "/ztwhatsapp.sock";
	o.sleep_status = "/run/zrn-sleepd/status";
	o.state_dir = env_or("XDG_STATE_HOME", env_or("HOME", "") + "/.local/state") + "/ztwhatsapp";
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		const auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
		if (a == "--socket")
			o.socket = next();
		else if (a == "--sleep-status")
			o.sleep_status = next();
		else if (a == "--state-dir")
			o.state_dir = next();
		else if (a == "--ui-command")
			o.ui_command = next();
		else if (a == "--ial-on-user-bus")
			o.ial_on_user_bus = true;
		else {
			fprintf(stderr, "usage: ztwhatsapp-notify [--socket P] [--sleep-status P] [--state-dir D] [--ui-command C]\n");
			return 2;
		}
	}
	struct sigaction sa{};
	sa.sa_handler = on_stop;	// no SA_RESTART: poll must return
	sigaction(SIGTERM, &sa, nullptr);
	sigaction(SIGINT, &sa, nullptr);
	signal(SIGPIPE, SIG_IGN);
	App app(std::move(o));
	return app.run();
}
