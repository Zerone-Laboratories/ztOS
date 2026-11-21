// SPDX-License-Identifier: GPL-2.0-or-later
// What deserves a notification, and how long a wake is held. No I/O here, so
// all of it runs in tests with invented data.
#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace zwn {

struct Incoming {
	std::string chat, chat_name, sender_name, text, kind, id;
	int64_t timestamp = 0;
	bool from_me = false, is_group = false, muted = false, live = false;
};

struct Shown {			// one notification to send or to replace
	std::string chat, title, body;
	bool alert = false;
	uint32_t replaces = 0;
};

// Cut on a character boundary: the shell's fields are byte buffers (48, 200).
std::string trim_utf8(std::string text, size_t bytes);

class Policy {
public:
	static constexpr int64_t COALESCE_MS = 500;	// a burst becomes one update
	static constexpr int64_t ALERT_GAP_MS = 10000;	// one chat alerts this often at most
	static constexpr size_t SEEN_MAX = 2048;

	explicit Policy(int64_t watermark) : watermark_(watermark), high_(watermark) {}

	// A message event. `focus` is the chat on screen, or empty. True when
	// something waits for flush().
	bool message(const Incoming &m, const std::string &focus, int64_t now_ms);
	// The engine went live on a connection: backlog is measured against what
	// had been seen before it, as backlog arrives out of order between chats.
	void connected() { watermark_ = high_; }
	void history_running(bool running) { history_ = running; }
	// Everything in a chat was read (here or on another device): the group ends.
	// Returns the notification to close, or 0.
	uint32_t chat_read(const std::string &chat);
	// The notification went away by itself: its group starts afresh.
	void closed(uint32_t id);
	void assigned(const std::string &chat, uint32_t id);
	std::string chat_of(uint32_t id) const;

	// Due updates; at most one alerts per flush.
	std::vector<Shown> flush(int64_t now_ms);
	void retry(const Shown &shown, int64_t now_ms);	// the service did not take it
	// When flush() next has work, or -1.
	int64_t next_due() const;
	int64_t high_water() const { return high_; }

private:
	struct Group {
		std::string title, last;
		int count = 0;
		uint32_t id = 0;
		int64_t due = -1, alerted = -1000000;
		bool alert = false;
	};
	bool seen_before(const std::string &chat, const std::string &id);

	int64_t watermark_, high_;
	bool history_ = false;
	std::map<std::string, Group> groups_;
	std::set<std::pair<std::string, std::string>> seen_;
	std::deque<std::pair<std::string, std::string>> order_;
};

// How long one wake is held so the engine can reconnect and take its backlog.
// Times are in ms on one clock; the numbers are explained in README.md.
class WakeHold {
public:
	static constexpr int64_t CAP_MS = 25000;	// never more than this per wake
	static constexpr int64_t QUIET_MS = 4000;	// live, then nothing for this long
	static constexpr int64_t SURVIVED_MS = 12000;	// still live without an event: LTE
	static constexpr int64_t NO_NET_MS = 15000;	// as zrn-sleepd's DOZE_NO_NET_S
	static constexpr int64_t MIN_SLEPT_S = 60;	// a shorter sleep gets no hold

	// A sleep ended. False when no hold is due: a storm of short wakes (the
	// sensor hub) or the power key must never add up to a veto.
	static bool worth_holding(const std::string &reason, int64_t slept_s);

	void start(int64_t now, const std::string &state);
	void connection(const std::string &state, int64_t now);
	void activity(int64_t now) { last_activity_ = now; }
	bool active() const { return started_ >= 0; }
	bool should_release(int64_t now) const;
	int64_t next_check(int64_t now) const;
	void stop() { started_ = -1; }

private:
	int64_t started_ = -1, connected_ = -1, last_activity_ = -1;
	std::string state_;
};

}
