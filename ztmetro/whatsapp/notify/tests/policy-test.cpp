// SPDX-License-Identifier: GPL-2.0-or-later
// Invented data only.
#include <cstdio>
#include <cstdlib>

#include "json.hpp"
#include "policy.hpp"

using namespace zwn;

static int failed;
#define CHECK(x) do { if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); failed++; } } while (0)

static Incoming msg(const std::string &id, int64_t ts = 2000, bool live = true)
{
	Incoming m;
	m.chat = "a@test";
	m.chat_name = "Invented";
	m.sender_name = "Someone";
	m.text = "hello";
	m.kind = "text";
	m.id = id;
	m.timestamp = ts;
	m.live = live;
	return m;
}

int main()
{
	{	// a burst becomes one row, and one alert
		Policy p(1000);
		CHECK(p.message(msg("1"), "", 0));
		CHECK(p.message(msg("2"), "", 100));
		CHECK(p.flush(100).empty());
		auto out = p.flush(600);
		CHECK(out.size() == 1 && out[0].alert && out[0].body == "2 new messages. hello");
		p.assigned("a@test", 7);
		p.message(msg("3"), "", 2000);
		out = p.flush(2600);
		CHECK(out.size() == 1 && !out[0].alert && out[0].replaces == 7);	// inside the gap: quiet
		p.message(msg("4"), "", 20000);
		out = p.flush(20600);
		CHECK(out.size() == 1 && out[0].alert && out[0].replaces == 7);
		CHECK(p.chat_of(7) == "a@test");
		CHECK(p.chat_read("a@test") == 7);
		CHECK(p.chat_of(7).empty());
		p.message(msg("5"), "", 30000);
		out = p.flush(30600);
		CHECK(out.size() == 1 && out[0].replaces == 0 && out[0].body == "hello");
	}
	{	// what must stay silent
		Policy p(1000);
		auto m = msg("1");
		m.from_me = true;
		CHECK(!p.message(m, "", 0));
		m = msg("2");
		m.muted = true;
		CHECK(!p.message(m, "", 0));
		CHECK(!p.message(msg("3"), "a@test", 0));	// on screen
		m = msg("4");
		m.kind = "reaction";
		CHECK(!p.message(m, "", 0));
		CHECK(p.message(msg("5"), "", 0));
		CHECK(!p.message(msg("5"), "", 0));		// a change to one
	}
	{	// backlog: slept-through messages notify, history does not
		Policy p(1000);
		CHECK(p.message(msg("1", 1500, false), "", 0));
		CHECK(!p.message(msg("2", 900, false), "", 0));
		CHECK(!p.message(msg("3", 1000, false), "", 0));
		p.history_running(true);
		CHECK(!p.message(msg("4", 1600, false), "", 0));
		p.history_running(false);
		// out of order between chats, within one connection
		auto b = msg("5", 1200, false);
		b.chat = "b@test";
		CHECK(p.message(b, "", 0));
		p.connected();
		CHECK(p.high_water() == 1600);
		CHECK(!p.message(msg("6", 1550, false), "", 0));
		CHECK(p.message(msg("7", 1700, false), "", 0));
	}
	{	// two chats at once: one sound
		Policy p(1000);
		p.message(msg("1"), "", 0);
		auto b = msg("2");
		b.chat = "b@test";
		b.is_group = true;
		p.message(b, "", 0);
		auto out = p.flush(600);
		CHECK(out.size() == 2 && out[0].alert != out[1].alert);
		CHECK(out[1].body == "Someone: hello");
		p.retry(out[0], 600);	// no service: comes back and still alerts
		CHECK(p.next_due() == 2600);
		out = p.flush(2600);
		CHECK(out.size() == 1 && out[0].alert);
	}
	{	// labels, and cuts on a character boundary
		Policy p(1000);
		auto m = msg("1");
		m.text = "";
		m.kind = "image";
		p.message(m, "", 0);
		CHECK(p.flush(600)[0].body == "Photo");
		CHECK(trim_utf8("a\xc3\xa9", 2) == "a");
		CHECK(trim_utf8("abc", 3) == "abc");
	}
	{	// the wake: bounded in every case
		CHECK(WakeHold::worth_holding("rtc", 300));
		CHECK(!WakeHold::worth_holding("rtc", 30));
		CHECK(!WakeHold::worth_holding("hub", 300));
		CHECK(!WakeHold::worth_holding("key", 300));
		WakeHold h;
		h.start(0, "offline");
		CHECK(!h.should_release(14000));
		CHECK(h.should_release(15000));		// no network
		h.start(0, "offline");
		h.connection("connecting", 6000);
		CHECK(!h.should_release(24000));
		CHECK(h.should_release(25000));		// the cap
		h.start(0, "offline");
		h.connection("connected", 9000);
		h.activity(10000);
		CHECK(!h.should_release(13900));
		CHECK(h.should_release(14000));		// live and quiet
		h.start(0, "connected");
		CHECK(!h.should_release(11000));
		CHECK(h.should_release(12000));		// its socket lived
		h.start(0, "connected");
		h.activity(11000);
		CHECK(!h.should_release(12000));
		h.stop();
		CHECK(!h.should_release(99999));
	}
	{	// JSON
		auto v = json::parse(R"({"a":"x\né😀","n":12,"t":true,"o":{"k":"s"},"l":[1,{}],"u":null})");
		CHECK(v && v->str("a") == "x\n\xc3\xa9\xf0\x9f\x98\x80" && v->integer("n") == 12 && v->flag("t"));
		CHECK(v->find("o")->str("k") == "s");
		CHECK(!json::parse("{"));
		CHECK(!json::parse("{\"a\":1} x"));
		CHECK(!json::parse("\"\\ud83d\\u0041\""));
		CHECK(!json::parse(std::string(1000, '[')));
	}
	printf(failed ? "%d failed\n" : "policy: ok\n", failed);
	return failed ? 1 : 0;
}
