// SPDX-License-Identifier: GPL-2.0-or-later
#include "policy.hpp"

#include <algorithm>

namespace zwn {

std::string trim_utf8(std::string text, size_t bytes)
{
	if (text.size() <= bytes)
		return text;
	size_t end = bytes;
	while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80)
		end--;
	text.resize(end);
	return text;
}

static std::string one_line(const std::string &text)
{
	std::string out;
	out.reserve(text.size());
	for (char c : text)
		out += c == '\n' || c == '\r' || c == '\t' ? ' ' : c;
	return out;
}

static std::string label(const Incoming &m)
{
	if (!m.text.empty())
		return one_line(m.text);
	static const std::map<std::string, std::string> LABELS = {
		{"image", "Photo"}, {"video", "Video"}, {"audio", "Voice message"},
		{"document", "Document"}, {"sticker", "Sticker"}, {"contact", "Contact"},
		{"location", "Location"},
	};
	auto it = LABELS.find(m.kind);
	return it == LABELS.end() ? "New message" : it->second;
}

bool Policy::seen_before(const std::string &chat, const std::string &id)
{
	auto key = std::make_pair(chat, id);
	if (!seen_.insert(key).second)
		return true;
	order_.push_back(std::move(key));
	if (order_.size() > SEEN_MAX) {
		seen_.erase(order_.front());
		order_.pop_front();
	}
	return false;
}

bool Policy::message(const Incoming &m, const std::string &focus, int64_t now_ms)
{
	if (m.from_me) {
		// answered from here or another device: read in practice. The caller
		// closes it through chat_read() when the chat's unread count falls.
		return false;
	}
	if (seen_before(m.chat, m.id))
		return false;	// "new message or a change to one": a change
	// Backlog is what arrived while asleep, the very case this exists for.
	// Older history is told apart by being at or under the watermark.
	bool fresh = m.live || (!history_ && m.timestamp > watermark_);
	high_ = std::max(high_, m.timestamp);
	if (!fresh || m.muted || m.kind == "system" || m.kind == "reaction" || m.chat == focus)
		return false;
	Group &g = groups_[m.chat];
	g.title = trim_utf8(one_line(m.chat_name.empty() ? "WhatsApp" : m.chat_name), 46);
	g.count++;
	g.last = m.is_group && !m.sender_name.empty() ? one_line(m.sender_name) + ": " + label(m) : label(m);
	g.alert = true;
	if (g.due < 0)
		g.due = now_ms + COALESCE_MS;
	return true;
}

uint32_t Policy::chat_read(const std::string &chat)
{
	auto it = groups_.find(chat);
	if (it == groups_.end())
		return 0;
	uint32_t id = it->second.id;
	groups_.erase(it);
	return id;
}

void Policy::closed(uint32_t id)
{
	for (auto it = groups_.begin(); it != groups_.end(); ++it) {
		if (it->second.id == id) {
			groups_.erase(it);
			return;
		}
	}
}

void Policy::assigned(const std::string &chat, uint32_t id)
{
	auto it = groups_.find(chat);
	if (it != groups_.end())
		it->second.id = id;
}

std::string Policy::chat_of(uint32_t id) const
{
	for (const auto &[chat, g] : groups_)
		if (g.id == id)
			return chat;
	return {};
}

std::vector<Shown> Policy::flush(int64_t now_ms)
{
	std::vector<Shown> out;
	bool alerted = false;
	for (auto &[chat, g] : groups_) {
		if (g.due < 0 || g.due > now_ms)
			continue;
		Shown s;
		s.chat = chat;
		s.title = g.title;
		s.body = g.count > 1 ? std::to_string(g.count) + " new messages. " + g.last : g.last;
		s.body = trim_utf8(std::move(s.body), 190);
		s.replaces = g.id;
		s.alert = g.alert && !alerted && now_ms - g.alerted >= ALERT_GAP_MS;
		if (s.alert) {
			alerted = true;
			g.alerted = now_ms;
		}
		g.alert = false;
		g.due = -1;
		out.push_back(std::move(s));
	}
	return out;
}

void Policy::retry(const Shown &shown, int64_t now_ms)
{
	auto it = groups_.find(shown.chat);
	if (it == groups_.end())
		return;
	it->second.due = now_ms + 2000;
	if (shown.alert) {
		it->second.alert = true;
		it->second.alerted = -1000000;
	}
}

int64_t Policy::next_due() const
{
	int64_t next = -1;
	for (const auto &[chat, g] : groups_)
		if (g.due >= 0 && (next < 0 || g.due < next))
			next = g.due;
	return next;
}

bool WakeHold::worth_holding(const std::string &reason, int64_t slept_s)
{
	if (slept_s < MIN_SLEPT_S)
		return false;
	return reason == "rtc" || reason == "alarm" || reason == "other" || reason == "modem";
}

void WakeHold::start(int64_t now, const std::string &state)
{
	started_ = now;
	connected_ = -1;
	last_activity_ = -1;
	state_ = state;
}

void WakeHold::connection(const std::string &state, int64_t now)
{
	state_ = state;
	if (!active())
		return;
	connected_ = state == "connected" ? now : -1;
}

bool WakeHold::should_release(int64_t now) const
{
	if (!active())
		return false;
	int64_t age = now - started_;
	if (age >= CAP_MS)
		return true;
	if (state_ == "logged_out")
		return true;
	if (connected_ >= 0)
		return now >= std::max(connected_, last_activity_) + QUIET_MS;
	if (state_ == "connected")	// no event since the wake: its socket lived
		return age >= SURVIVED_MS && now >= last_activity_ + QUIET_MS;
	return age >= NO_NET_MS && state_ == "offline";
}

int64_t WakeHold::next_check(int64_t now) const
{
	if (!active())
		return -1;
	return std::min<int64_t>(now + 1000, started_ + CAP_MS);
}

}
