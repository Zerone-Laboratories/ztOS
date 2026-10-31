// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztwhatsapp: WhatsApp for ztMETRO, in C++ over ZrnMetroCore.
 *
 * All data comes from ztwhatsappd over the socket in ../IPC.md; nothing here
 * talks to WhatsApp. Calls are not part of this app.
 *
 * Why a long conversation stays smooth: its bubbles have different heights but
 * zmc_list wants rows of one height, so the conversation is cut into strips of
 * ROW_H points. Each strip draws whichever bubbles cross it, clipped, and the
 * core keeps each strip as a texture that is only moved, drawing the next ones
 * ahead on its thread. Bubble sizes are measured once; laid-out text is kept for
 * the bubbles drawn lately. Typing touches only the footer, its own texture.
 *
 *	ztwhatsapp			the chats
 *	ztwhatsapp JID			that chat (from a notification)
 *	ztwhatsapp --render PAGE FILE	a page to a PNG, invented data
 *					(link qr code chats status starred archived chat
 *					view post search new info account)
 *	ztwhatsapp --bench N [PNG]	cost of strips for N invented messages
 *	ztwhatsapp --selftest		the socket with no window, against an engine
 *
 * ZTWHATSAPP_SOCK overrides the socket. Never log message text, names or numbers.
 */
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <fcntl.h>
#include <functional>
#include <map>
#include <memory>
#include <poll.h>
#include <signal.h>
#include <string>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <json-c/json.h>
#include <pango/pangocairo.h>

extern "C" {
#include "zrnmetrocore.h"
extern double dm_wrap_origin_y;
extern int dm_wrap_raw;
}
#include "qr.h"

namespace {

constexpr double MARGIN = ZMC_MARGIN;
constexpr double ROW_H = 76;	/* chat rows and strips alike: the textures are reused */
constexpr double PAD = 10, TEXT_SIZE = 15, STAMP_SIZE = 11.5, BOX_FONT = 16, BOX_MAX = 104;
constexpr double THUMB_W = 240;
constexpr size_t MAX_DRAFT = 8000, LAYOUT_KEEP = 96, THUMB_KEEP = 48;
constexpr int PAGE_SIZE = 60;
constexpr int STEP_MS = 220;

enum Page { P_LINK, P_CHATS, P_CHAT, P_ACCOUNT, P_IMAGE, P_STATUS, P_STARRED, P_ARCHIVED, P_STATUS_VIEW, P_STATUS_POST, P_INFO, P_SEARCH, P_NEW };
enum Action { ACT_OPEN = 1, ACT_PAIR_QR, ACT_PAIR_PHONE, ACT_PHONE_BOX, ACT_CANCEL, ACT_IMAGE, ACT_RETRY, ACT_LOGOUT, ACT_LOGOUT_SURE,
	ACT_ACCENT, ACT_OPEN_ARCHIVED, ACT_OPEN_STARRED, ACT_OPEN_ID, ACT_STATUS, ACT_STATUS_NEXT, ACT_POST_BOX, ACT_POST, ACT_INFO_BOX,
	ACT_SEARCH_BOX, ACT_SEARCH_MESSAGES, ACT_NEW_BOX, ACT_NEW_PICK, ACT_NONE };

/* The app's own colour, WhatsApp's teal green: its brighter brand green is too
 * pale under the white text of a bubble. A choice on the account page gives the
 * system's accent instead. DotMatrix keeps its palette either way. */
constexpr uint32_t WA_GREEN = 0x00a884;

/* ---- what is shown ------------------------------------------------------- */

struct Chat {
	std::string id, name, last_text;
	bool is_group = false, last_from_me = false, muted = false, pinned = false, archived = false;
	int unread = 0;
	long last_ts = 0;
};

struct Msg {
	std::string id, sender_name, text, kind, status, thumb_b64, path;
	bool starred = false;
	bool from_me = false, system = false, is_image = false, new_day = false;
	long ts = 0;
	int media_w = 0, media_h = 0;
	/* measured once */
	bool measured = false;
	double bw = 0, bh = 0, text_h = 0, pic_w = 0, pic_h = 0, name_h = 0;
	double y = 0, total = 0, nudge = 0;
	PangoLayout *layout = nullptr;
};

std::vector<Chat> chats;		/* the ones on the list */
std::map<std::string, Chat> all_chats;
std::string connection = "connecting", pair_state = "connecting", pair_qr_text, pair_code, pair_error;
qr::Code qr_code;
std::string self_jid, open_id, open_name, presence_line;
bool open_group;
bool older_more, older_pending;
std::deque<std::unique_ptr<Msg>> msgs;
double list_total;
uint64_t list_rev = 2;		/* even: chats list +0, conversation +1 */
double entry_off;		/* a page turn in shows the end of the conversation */
bool sample;

std::string draft, phone, post_text, search_text, new_text;
int focus = -1;			/* 0 the message, 1 the phone number, 2 the status, 3 search, 4 new chat */

struct StatusItem { std::string id, text, kind, thumb_b64, path; long ts = 0; bool seen = false; };
struct StatusPerson { std::string sender, name; long last_ts = 0; int unseen = 0; std::vector<StatusItem> items; };
struct Contact { std::string jid, name, phone; };
struct Found { std::string chat, chat_name, who, text; long ts = 0; };
struct Member { std::string name; bool admin = false; };

std::vector<Chat> archived;
std::vector<StatusPerson> statuses;
std::vector<Contact> contacts, contacts_shown;
std::vector<Found> starred_list, search_found;
std::vector<Member> members;
std::string info_topic, info_note, post_note;
int status_person = -1, status_item = 0;
bool follow_system_accent;

/* pictures by JID; nullptr: asked, there is none */
std::map<std::string, cairo_surface_t *> pictures;
std::vector<std::string> picture_queue;
bool picture_busy;
double box_h = 44;
unsigned footer_rev = 1;
bool logout_ask, foreground;
long last_typing;

cairo_surface_t *full_image;
bool image_wanted;
std::string image_note;

/* ---- text ---------------------------------------------------------------- */

bool dots() { return zmc.style == ZMC_STYLE_DOTMATRIX; }

PangoContext *text_context() {
	static PangoContext *context;
	if (!context) {
		context = pango_font_map_create_context(pango_cairo_font_map_get_default());
		cairo_font_options_t *o = cairo_font_options_create();
		cairo_font_options_set_hint_metrics(o, CAIRO_HINT_METRICS_OFF);
		cairo_font_options_set_hint_style(o, CAIRO_HINT_STYLE_NONE);
		pango_cairo_context_set_font_options(context, o);
		cairo_font_options_destroy(o);
		pango_context_set_round_glyph_positions(context, FALSE);
	}
	return context;
}

PangoFontDescription *text_font() {
	static PangoFontDescription *d;
	if (!d) {
		d = pango_font_description_from_string(FONT_REGULAR);
		pango_font_description_set_absolute_size(d, TEXT_SIZE * PANGO_SCALE);
	}
	return d;
}

cairo_t *measure_cr() {
	static cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
	static cairo_t *cr = cairo_create(s);
	return cr;
}

double dm_text_height(const char *text, double size, double width) {
	cairo_t *cr = measure_cr();
	cairo_save(cr);
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	double h = zmc_text(cr, text, FONT_REGULAR, size, 0, -10000, width);
	cairo_restore(cr);
	return h;
}

/* recently laid-out bubbles; the rest give theirs back */
std::deque<Msg *> layouts_held;

void layout_drop(Msg *m) {
	if (m->layout) g_object_unref(m->layout);
	m->layout = nullptr;
}

PangoLayout *layout_of(Msg *m, double width) {
	if (m->layout) return m->layout;
	PangoLayout *l = pango_layout_new(text_context());
	pango_layout_set_font_description(l, text_font());
	pango_layout_set_text(l, m->text.c_str(), -1);
	if (width > 0) {
		pango_layout_set_width(l, int(width * PANGO_SCALE));
		pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
	}
	m->layout = l;
	layouts_held.push_back(m);
	while (layouts_held.size() > LAYOUT_KEEP) {
		layout_drop(layouts_held.front());
		layouts_held.pop_front();
	}
	return l;
}

std::string clean(const char *s) {
	if (!s) return {};
	gchar *v = g_utf8_make_valid(s, -1);
	std::string out = v;
	g_free(v);
	return out;
}

std::string when_text(long ts) {
	time_t t = ts, now = time(nullptr);
	struct tm a, b;
	localtime_r(&t, &a);
	localtime_r(&now, &b);
	char out[32];
	if (a.tm_yday == b.tm_yday && a.tm_year == b.tm_year) strftime(out, sizeof(out), "%H:%M", &a);
	else if (now - ts < 6 * 86400) strftime(out, sizeof(out), "%a %H:%M", &a);
	else strftime(out, sizeof(out), "%d %b", &a);
	return out;
}

std::string day_text(long ts) {
	time_t t = ts;
	struct tm a;
	localtime_r(&t, &a);
	char out[48];
	strftime(out, sizeof(out), "%A %d %B %Y", &a);
	for (char *c = out; *c; c++) *c = char(tolower((unsigned char)*c));
	return out;
}

std::string stamp_of(const Msg &m) {
	std::string s = (m.starred ? "* " : "") + when_text(m.ts);
	if (!m.from_me) return s;
	if (m.status == "pending") s += "  sending...";
	else if (m.status == "sent") s += "  sent";
	else if (m.status == "delivered") s += "  delivered";
	else if (m.status == "read") s += "  read";
	else if (m.status == "failed") s += "  not sent, tap to edit";
	return s;
}

/* ---- a bubble's size, and where everything lies --------------------------- */

void measure(Msg &m) {
	if (m.measured) return;
	m.measured = true;
	if (m.system) {
		m.text_h = dots() ? dm_text_height(m.text.c_str(), 12.5, zmc.width - 2 * MARGIN) : 18;
		m.bw = zmc.width - 2 * MARGIN;
		m.bh = m.text_h;
		return;
	}
	const double most = zmc.width * 0.78;
	double w = 64;
	if (m.is_image) {
		double aspect = m.media_w > 0 && m.media_h > 0 ? double(m.media_h) / m.media_w : 0.75;
		m.pic_w = THUMB_W;
		m.pic_h = std::clamp(THUMB_W * aspect, 90.0, 300.0);
		if (dots()) m.pic_h = m.pic_w;	/* zmc_picture's square of dots */
		w = m.pic_w + 2 * PAD;
	}
	if (!m.sender_name.empty() && !m.from_me && open_group) m.name_h = 18;
	if (!m.text.empty()) {
		if (dots()) {
			double one = m.text.find('\n') == std::string::npos ? zmc_text_width(m.text.c_str(), FONT_REGULAR, TEXT_SIZE) + 2 * PAD + 2 : most;
			w = m.is_image ? w : std::min(one, most);
		} else {
			if (!m.is_image && m.text.size() < 200 && m.text.find('\n') == std::string::npos) {
				PangoLayout *l = pango_layout_new(text_context());
				pango_layout_set_font_description(l, text_font());
				pango_layout_set_text(l, m.text.c_str(), -1);
				int pw, ph;
				pango_layout_get_pixel_size(l, &pw, &ph);
				g_object_unref(l);
				w = std::min(pw + 2 * PAD + 2, most);
			} else if (!m.is_image) {
				w = most;
			}
		}
	}
	w = std::max(w, zmc_text_width(stamp_of(m).c_str(), FONT_REGULAR, STAMP_SIZE) + 2 * PAD + 2);
	if (m.name_h > 0) w = std::max(w, std::min(most, zmc_text_width(m.sender_name.c_str(), FONT_SEMIBOLD, 12.5) + 2 * PAD));
	w = std::min(w, zmc.width - 2 * MARGIN);
	m.bw = std::ceil(w);
	double inner = m.bw - 2 * PAD;
	if (!m.text.empty()) {
		if (dots()) {
			m.text_h = dm_text_height(m.text.c_str(), TEXT_SIZE, inner);
		} else {
			int pw, ph;
			pango_layout_get_pixel_size(layout_of(&m, inner), &pw, &ph);
			m.text_h = ph;
		}
	}
	m.bh = std::ceil(2 * PAD + 17 + m.name_h + (m.is_image ? m.pic_h + (m.text.empty() ? 0 : 6) : 0) + m.text_h);
}

int footer_height(int page) {
	if (page == P_CHATS || page == P_STATUS) return 76;
	if (page == P_CHAT) return int(box_h) + 94;
	return 0;
}

/* every y, from the sizes: cheap, sizes are memoised. Caller holds the list lock. */
void place() {
	double y = 0;
	long day = -1;
	for (auto &p : msgs) {
		Msg &m = *p;
		time_t t = m.ts;
		struct tm a;
		localtime_r(&t, &a);
		long d = a.tm_year * 1000L + a.tm_yday;
		m.new_day = d != day;
		day = d;
		measure(m);
		double head = m.new_day ? 32 : 0, nudge = 0;
		/* Under DotMatrix a line of dots lying on the seam of two strips is half in
		 * each and lit in neither: keep a bubble's top and bottom off the seams. */
		if (dots() && !m.system) {
			auto near_seam = [](double at) {
				double r = std::fmod(at, ROW_H);
				return r < 5 || r > ROW_H - 5;
			};
			while (nudge < 12 && (near_seam(y + nudge + head + 8) || near_seam(y + nudge + head + 8 + m.bh))) nudge += 2;
		}
		m.y = y;
		m.nudge = nudge;
		m.total = nudge + m.bh + head + (m.system ? 14 : m.from_me ? 26 : 18);
		y += m.total;
	}
	/* a short conversation sits on the box, not under the title */
	double room = zmc.height - ZMC_HEADER_H - 138;
	if (y < room) {
		double pad = room - y;
		for (auto &p : msgs) p->y += pad;
		y = room;
	}
	list_total = y;
}

void list_changed() { list_rev += 32; }	/* page + rev: no two lists share a texture */

void accent_apply() {
	if (dots()) return;
	zmc.accent = follow_system_accent ? zmc_config_accent() : WA_GREEN;
}

std::string prefs_path() {
	const char *home = getenv("HOME");
	return std::string(home ? home : "") + "/.config/ztmetro/ztwhatsapp";
}
void prefs_load() {
	char line[64];
	if (FILE *f = fopen(prefs_path().c_str(), "r")) {
		while (fgets(line, sizeof(line), f)) if (!strncmp(line, "follow_system_accent=1", 22)) follow_system_accent = true;
		fclose(f);
	}
}
void prefs_save() {
	if (FILE *f = fopen(prefs_path().c_str(), "w")) {
		fprintf(f, "follow_system_accent=%d\n", follow_system_accent);
		fclose(f);
	}
}

/* ---- drawing a bubble: cut to its strip by the caller ---------------------- */

GdkPixbuf *pixbuf_of_b64(const std::string &b64) {
	gsize len = 0;
	guchar *raw = g_base64_decode(b64.c_str(), &len);
	GInputStream *in = g_memory_input_stream_new_from_data(raw, gssize(len), g_free);
	GdkPixbuf *p = gdk_pixbuf_new_from_stream(in, nullptr, nullptr);
	g_object_unref(in);
	return p;
}

cairo_surface_t *surface_of_pixbuf(GdkPixbuf *p) {
	int w = gdk_pixbuf_get_width(p), h = gdk_pixbuf_get_height(p), n = gdk_pixbuf_get_n_channels(p), rs = gdk_pixbuf_get_rowstride(p);
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
	const guchar *src = gdk_pixbuf_read_pixels(p);
	unsigned char *dst = cairo_image_surface_get_data(s);
	int ds = cairo_image_surface_get_stride(s);
	for (int y = 0; y < h; y++) {
		auto *out = reinterpret_cast<uint32_t *>(dst + y * ds);
		const guchar *in = src + y * rs;
		for (int x = 0; x < w; x++, in += n) out[x] = uint32_t(in[0]) << 16 | uint32_t(in[1]) << 8 | in[2];
	}
	cairo_surface_mark_dirty(s);
	return s;
}

struct ThumbEntry { std::string key; cairo_surface_t *surface; };
std::deque<ThumbEntry> thumbs;

cairo_surface_t *thumb_of(const Msg &m) {
	if (m.thumb_b64.empty()) return nullptr;
	for (auto &t : thumbs) if (t.key == m.id) return t.surface;
	cairo_surface_t *s = nullptr;
	if (GdkPixbuf *p = pixbuf_of_b64(m.thumb_b64)) {
		s = surface_of_pixbuf(p);
		g_object_unref(p);
	}
	thumbs.push_back({m.id, s});
	if (thumbs.size() > THUMB_KEEP) {
		if (thumbs.front().surface) cairo_surface_destroy(thumbs.front().surface);
		thumbs.pop_front();
	}
	return s;
}

void draw_message(cairo_t *cr, Msg &m) {
	double y = 0;
	if (m.nudge > 0) y += m.nudge;
	if (m.new_day) {
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		std::string day = day_text(m.ts);
		zmc_text_centred(cr, day.c_str(), FONT_REGULAR, 12.5, zmc.width / 2, y + 6);
		cairo_new_path(cr);
		y += 32;
	}
	if (m.system) {
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		if (dots()) zmc_text(cr, m.text.c_str(), FONT_REGULAR, 12.5, MARGIN, y, m.bw);
		else zmc_text_line(cr, m.text.c_str(), FONT_REGULAR, 12.5, MARGIN, y, m.bw);
		cairo_new_path(cr);
		return;
	}
	bool in = !m.from_me;
	double x = in ? MARGIN : zmc.width - MARGIN - m.bw, w = m.bw, h = m.bh;
	if (in || dots()) {
		if (in) zmc_accent(cr); else cairo_set_source_rgb(cr, 1, 1, 1);
	} else {
		zmc_set_rgb(cr, (zmc.accent & 0xfefefe) >> 1);
	}
	/* the tail: theirs at the top left, ours at the bottom right */
	cairo_new_path(cr);
	if (in) {
		cairo_move_to(cr, x, y + 8);
		cairo_line_to(cr, x, y);
		cairo_line_to(cr, x + 12, y + 8);
	} else {
		cairo_move_to(cr, x + w, y + 8 + h);
		cairo_line_to(cr, x + w, y + 16 + h);
		cairo_line_to(cr, x + w - 12, y + 8 + h);
	}
	cairo_close_path(cr);
	cairo_fill(cr);
	cairo_rectangle(cr, x, y + 8, w, h);
	if (dots()) {
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);	/* filled dots would drown the text */
	} else {
		cairo_fill(cr);
	}
	cairo_new_path(cr);
	double at = y + 8 + PAD, inner = w - 2 * PAD;
	cairo_set_source_rgb(cr, 1, 1, 1);
	if (m.name_h > 0) {
		zmc_text_line(cr, m.sender_name.c_str(), FONT_SEMIBOLD, 12.5, x + PAD, at - 2, inner);
		cairo_new_path(cr);
		at += m.name_h;
	}
	if (m.is_image) {
		cairo_surface_t *t = thumb_of(m);
		if (t && dots()) {
			zmc_picture(cr, t, x + PAD, at, m.pic_w);
		} else if (t) {
			cairo_save(cr);
			cairo_rectangle(cr, x + PAD, at, m.pic_w, m.pic_h);
			cairo_clip(cr);
			cairo_translate(cr, x + PAD, at);
			cairo_scale(cr, m.pic_w / cairo_image_surface_get_width(t), m.pic_h / cairo_image_surface_get_height(t));
			cairo_set_source_surface(cr, t, 0, 0);
			cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
			cairo_paint(cr);
			cairo_restore(cr);
		} else {
			cairo_set_source_rgb(cr, 0.25, 0.25, 0.25);
			cairo_rectangle(cr, x + PAD, at, m.pic_w, m.pic_h);
			if (dots()) { cairo_set_line_width(cr, 1); cairo_stroke(cr); } else cairo_fill(cr);
			cairo_new_path(cr);
			cairo_set_source_rgb(cr, 1, 1, 1);
			zmc_text_centred(cr, "photo", FONT_REGULAR, 14, x + PAD + m.pic_w / 2, at + m.pic_h / 2 - 9);
			cairo_new_path(cr);
		}
		at += m.pic_h + (m.text.empty() ? 0 : 6);
		cairo_set_source_rgb(cr, 1, 1, 1);
	}
	if (!m.text.empty()) {
		if (dots()) {
			zmc_text(cr, m.text.c_str(), FONT_REGULAR, TEXT_SIZE, x + PAD, at, inner);
		} else {
			cairo_move_to(cr, x + PAD, at);
			pango_cairo_show_layout(cr, layout_of(&m, inner));
		}
		cairo_new_path(cr);
		at += m.text_h;
	}
	std::string stamp = stamp_of(m);
	double sw = zmc_text_width(stamp.c_str(), FONT_REGULAR, STAMP_SIZE);
	if (m.status == "failed" && dots()) zmc_accent(cr);
	else if (m.status == "failed") cairo_set_source_rgb(cr, 1, 1, 1);
	else cairo_set_source_rgb(cr, 0.82, 0.82, 0.82);
	zmc_text(cr, stamp.c_str(), FONT_REGULAR, STAMP_SIZE, in ? x + PAD : x + w - PAD - sw, at + 3, 0);
	cairo_new_path(cr);
}

int older_pipe[2] = {-1, -1};
std::atomic<bool> older_asked;

/* first message whose extent reaches past y */
size_t first_at(double y) {
	size_t lo = 0, hi = msgs.size();
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		if (msgs[mid]->y + msgs[mid]->total <= y) lo = mid + 1;
		else hi = mid;
	}
	return lo;
}

void draw_strip(cairo_t *cr, int row) {
	double y0 = row * ROW_H + entry_off, y1 = y0 + ROW_H;
	/* near the top: ask for older ones. This may run on the core's drawing
	 * thread, so the main loop is woken to do it. */
	if (row < 12 && older_more && !older_pending && !older_asked.exchange(true) && older_pipe[1] >= 0)
		(void)!write(older_pipe[1], "o", 1);
	cairo_save(cr);
	cairo_rectangle(cr, 0, 0, zmc.width, ROW_H);
	cairo_clip(cr);
	for (size_t i = first_at(y0); i < msgs.size() && msgs[i]->y < y1; i++) {
		cairo_save(cr);
		cairo_translate(cr, 0, msgs[i]->y - y0);
		draw_message(cr, *msgs[i]);
		cairo_restore(cr);
	}
	cairo_restore(cr);
}

bool strip_hit(int row, double x, double y, struct zmc_hit *hit) {
	double at = row * ROW_H + entry_off + y;
	size_t i = first_at(at);
	if (i >= msgs.size() || msgs[i]->y > at) return false;
	Msg &m = *msgs[i];
	double by = m.y + m.nudge + (m.new_day ? 32 : 0) + 8, bx = m.from_me ? zmc.width - MARGIN - m.bw : MARGIN;
	if (at < by || at >= by + m.bh || x < bx || x >= bx + m.bw) return false;
	int action = m.status == "failed" ? ACT_RETRY : m.is_image ? ACT_IMAGE : 0;
	if (!action) return false;
	*hit = zmc_hit{.x = 0, .y = 0, .w = 0, .h = 0, .action = action, .arg = int(i), .slider = false, .button = false};
	return true;
}

/* Called while drawing, maybe on the core's thread: the main loop does the asking. */
cairo_surface_t *picture_of(const std::string &jid) {
	auto it = pictures.find(jid);
	if (it != pictures.end()) return it->second;
	pictures[jid] = nullptr;
	picture_queue.push_back(jid);
	if (older_pipe[1] >= 0) (void)!write(older_pipe[1], "p", 1);
	return nullptr;
}

/* Messaging's square, with the person's picture in it when there is one. */
void draw_tile(cairo_t *cr, const std::string &jid, const std::string &name, double y, double tile = 56) {
	if (cairo_surface_t *pic = sample || !jid.empty() ? picture_of(jid) : nullptr) {
		zmc_picture(cr, pic, MARGIN, y, tile);
		return;
	}
	std::string initial = "#";
	if (!name.empty() && !isdigit(static_cast<unsigned char>(name[0])) && name[0] != '+') {
		const char *next = g_utf8_next_char(name.c_str());
		gchar *up = g_utf8_strup(name.c_str(), next - name.c_str());
		initial = up;
		g_free(up);
	}
	zmc_accent(cr);
	cairo_rectangle(cr, MARGIN, y, tile, tile);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text_centred(cr, initial.c_str(), FONT_SEMILIGHT, 26, MARGIN + tile / 2, y + 12);
	cairo_new_path(cr);
}

/* ---- the chat list -------------------------------------------------------- */

void draw_chat(cairo_t *cr, const Chat &c) {
	const double tile = 56, left = MARGIN + tile + 12, y = 0;
	bool unread = c.unread > 0;
	draw_tile(cr, c.id, c.name, y);

	std::string when = when_text(c.last_ts);
	double when_w = zmc_text_width(when.c_str(), FONT_REGULAR, 12.5);
	if (unread) zmc_accent(cr); else cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text_line(cr, c.name.c_str(), unread ? FONT_SEMIBOLD : FONT_SEMILIGHT, 24, left, y - 3, zmc.width - left - MARGIN - when_w - 10);
	cairo_new_path(cr);
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, when.c_str(), FONT_REGULAR, 12.5, zmc.width - MARGIN - when_w, y + 8, 0);
	cairo_new_path(cr);

	double count_w = 0;
	if (unread) {
		std::string n = c.unread > 999 ? "999+" : std::to_string(c.unread);
		double tw = zmc_text_width(n.c_str(), FONT_SEMIBOLD, 13);
		count_w = std::max(22.0, tw + 10);
		double bx = zmc.width - MARGIN - count_w;
		zmc_accent(cr);
		cairo_rectangle(cr, bx, y + 31, count_w, 20);
		if (dots()) { cairo_set_line_width(cr, 1); cairo_stroke(cr); } else cairo_fill(cr);
		cairo_new_path(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text_centred(cr, n.c_str(), FONT_SEMIBOLD, 13, bx + count_w / 2, y + 32);
		cairo_new_path(cr);
		count_w += 8;
	}
	std::string line = (c.last_from_me ? "you: " : "") + c.last_text;
	if (line.size() > 160) {
		size_t cut = 160;
		while (cut && (static_cast<unsigned char>(line[cut]) & 0xc0) == 0x80) cut--;
		line.resize(cut);
	}
	std::replace(line.begin(), line.end(), '\n', ' ');
	if (unread) cairo_set_source_rgb(cr, 1, 1, 1); else cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text_line(cr, line.c_str(), FONT_REGULAR, 14, left, y + 30, zmc.width - left - MARGIN - count_w);
	cairo_new_path(cr);
}

void draw_chat_row(cairo_t *cr, int row) {
	if (row >= 0 && size_t(row) < chats.size()) draw_chat(cr, chats[size_t(row)]);
}
void draw_archived_row(cairo_t *cr, int row) {
	if (row >= 0 && size_t(row) < archived.size()) draw_chat(cr, archived[size_t(row)]);
}

bool row_hit_as(int action, size_t count, int row, struct zmc_hit *hit) {
	if (row < 0 || size_t(row) >= count) return false;
	*hit = zmc_hit{.x = 0, .y = -2, .w = double(zmc.width), .h = 58, .action = action, .arg = row, .slider = false, .button = false};
	return true;
}
bool chat_row_hit(int row, double, double, struct zmc_hit *hit) { return row_hit_as(ACT_OPEN, chats.size(), row, hit); }
bool archived_row_hit(int row, double, double, struct zmc_hit *hit) { return row_hit_as(ACT_OPEN_ARCHIVED, archived.size(), row, hit); }

/* a row with a tile, a name, when, and a grey line: status, starred, new chat */
void draw_plain_row(cairo_t *cr, const std::string &jid, const std::string &name, const std::string &when, const std::string &line, bool lit) {
	const double left = MARGIN + 56 + 12;
	draw_tile(cr, jid, name, 0);
	double when_w = when.empty() ? 0 : zmc_text_width(when.c_str(), FONT_REGULAR, 12.5);
	if (lit) zmc_accent(cr); else cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text_line(cr, name.c_str(), lit ? FONT_SEMIBOLD : FONT_SEMILIGHT, 24, left, -3, zmc.width - left - MARGIN - when_w - 10);
	cairo_new_path(cr);
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	if (!when.empty()) zmc_text(cr, when.c_str(), FONT_REGULAR, 12.5, zmc.width - MARGIN - when_w, 8, 0);
	cairo_new_path(cr);
	std::string flat = line;
	std::replace(flat.begin(), flat.end(), '\n', ' ');
	if (flat.size() > 160) { size_t cut = 160; while (cut && (static_cast<unsigned char>(flat[cut]) & 0xc0) == 0x80) cut--; flat.resize(cut); }
	zmc_text_line(cr, flat.c_str(), FONT_REGULAR, 14, left, 30, zmc.width - left - MARGIN);
	cairo_new_path(cr);
}

void draw_status_row(cairo_t *cr, int row) {
	if (row < 0 || size_t(row) >= statuses.size()) return;
	const StatusPerson &p = statuses[size_t(row)];
	std::string line = p.unseen ? std::to_string(p.unseen) + " new" : "seen";
	draw_plain_row(cr, p.sender, p.name, when_text(p.last_ts), line, p.unseen > 0);
}
bool status_row_hit(int row, double, double, struct zmc_hit *hit) { return row_hit_as(ACT_STATUS, statuses.size(), row, hit); }

void draw_starred_row(cairo_t *cr, int row) {
	if (row < 0 || size_t(row) >= starred_list.size()) return;
	const Found &f = starred_list[size_t(row)];
	draw_plain_row(cr, f.chat, f.chat_name, when_text(f.ts), f.text, false);
}
bool starred_row_hit(int row, double, double, struct zmc_hit *hit) { return row_hit_as(ACT_OPEN_STARRED, starred_list.size(), row, hit); }

void draw_contact_row(cairo_t *cr, int row) {
	if (row < 0 || size_t(row) >= contacts_shown.size()) return;
	const Contact &c = contacts_shown[size_t(row)];
	draw_plain_row(cr, c.jid, c.name, "", c.phone, false);
}
bool contact_row_hit(int row, double, double, struct zmc_hit *hit) { return row_hit_as(ACT_NEW_PICK, contacts_shown.size(), row, hit); }

/* ---- the socket ----------------------------------------------------------- */

using Reply = std::function<void(json_object *result, const char *error)>;
int sock_fd = -1, next_id = 1;
std::string in_buf, out_buf;
std::map<int, Reply> waiting;
bool daemon_started, batch_chats, batch_page;

void flush_out() {
	while (sock_fd >= 0 && !out_buf.empty()) {
		ssize_t n = send(sock_fd, out_buf.data(), out_buf.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
		if (n <= 0) break;
		out_buf.erase(0, size_t(n));
	}
}

void request(const char *cmd, std::initializer_list<std::pair<const char *, json_object *>> fields, Reply reply = nullptr) {
	json_object *o = json_object_new_object();
	int id = next_id++;
	json_object_object_add(o, "id", json_object_new_int(id));
	json_object_object_add(o, "cmd", json_object_new_string(cmd));
	for (auto &f : fields) json_object_object_add(o, f.first, f.second);
	if (sock_fd < 0) {
		json_object_put(o);
		if (reply) reply(nullptr, "not_connected");
		return;
	}
	if (reply) waiting[id] = std::move(reply);
	out_buf += json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN);
	out_buf += '\n';
	json_object_put(o);
	flush_out();
}

json_object *str(const std::string &s) { return json_object_new_string_len(s.data(), int(s.size())); }

std::string jstr(json_object *o, const char *key) {
	json_object *v;
	return o && json_object_object_get_ex(o, key, &v) && json_object_is_type(v, json_type_string) ? clean(json_object_get_string(v)) : std::string();
}
long jint(json_object *o, const char *key) {
	json_object *v;
	return o && json_object_object_get_ex(o, key, &v) ? json_object_get_int64(v) : 0;
}
bool jbool(json_object *o, const char *key) {
	json_object *v;
	return o && json_object_object_get_ex(o, key, &v) && json_object_get_boolean(v);
}
json_object *jget(json_object *o, const char *key) {
	json_object *v;
	return o && json_object_object_get_ex(o, key, &v) && !json_object_is_type(v, json_type_null) ? v : nullptr;
}

void chats_sort() {
	chats.clear();
	archived.clear();
	for (auto &[id, c] : all_chats) (c.archived ? archived : chats).push_back(c);
	auto order = [](const Chat &a, const Chat &b) {
		if (a.pinned != b.pinned) return a.pinned;
		return a.last_ts > b.last_ts;
	};
	std::stable_sort(chats.begin(), chats.end(), order);
	std::stable_sort(archived.begin(), archived.end(), order);
	list_changed();
}

void chat_from(json_object *o) {
	Chat c;
	c.id = jstr(o, "id");
	if (c.id.empty()) return;
	c.name = jstr(o, "name");
	c.last_text = jstr(o, "last_text");
	c.is_group = jbool(o, "is_group");
	c.last_from_me = jbool(o, "last_from_me");
	c.muted = jbool(o, "muted");
	c.pinned = jbool(o, "pinned");
	c.archived = jbool(o, "archived");
	c.unread = int(jint(o, "unread"));
	c.last_ts = jint(o, "last_ts");
	all_chats[c.id] = std::move(c);
}

std::unique_ptr<Msg> msg_from(json_object *o) {
	std::string kind = jstr(o, "kind");
	if (kind == "reaction") return nullptr;	/* no place for them yet */
	auto m = std::make_unique<Msg>();
	m->id = jstr(o, "id");
	m->from_me = jbool(o, "from_me");
	m->sender_name = jstr(o, "sender_name");
	m->ts = jint(o, "timestamp");
	m->kind = kind;
	m->text = jstr(o, "text");
	m->status = jstr(o, "status");
	m->starred = jbool(o, "starred");
	m->system = kind == "system";
	if (kind == "unsupported" || kind == "undecryptable") { /* text is a placeholder */ }
	else if (kind == "video" && m->text.empty()) m->text = "video";
	else if (kind == "audio" && m->text.empty()) m->text = "voice message";
	else if (kind == "document" && m->text.empty()) m->text = "document";
	else if (kind == "contact" && m->text.empty()) m->text = "contact";
	else if (kind == "location" && m->text.empty()) m->text = "location";
	else if (kind == "sticker" && m->text.empty()) m->text = "sticker";
	if (json_object *media = jget(o, "media")) {
		if (kind == "image") {
			m->is_image = true;
			m->media_w = int(jint(media, "width"));
			m->media_h = int(jint(media, "height"));
			m->thumb_b64 = jstr(media, "thumb_b64");
			m->path = jstr(media, "path");
		}
	}
	return m;
}

void msgs_clear() {
	layouts_held.clear();
	for (auto &m : msgs) layout_drop(m.get());
	msgs.clear();
	for (auto &t : thumbs) if (t.surface) cairo_surface_destroy(t.surface);
	thumbs.clear();
}

/* the list glides to its new end rather than jumping */
double glide_from, glide_to;
uint32_t glide_start;
bool gliding;
json_object *older_held;	/* a page of older messages, waiting for the list to stop */
void older_apply();

/* one hook for both: never asks for a frame itself, the core draws while it is set */
void frame_step() {
	if (gliding) {
		double t = std::min(1.0, (zmc_now_ms() - glide_start) / double(STEP_MS));
		double e = 1 - (1 - t) * (1 - t) * (1 - t);
		if (zmc.touching) gliding = false;
		else zmc.scroll = glide_from + (glide_to - glide_from) * e;
		if (t >= 1) gliding = false;
	}
	/* Applying it repaints every strip in sight: do that with nothing moving,
	 * unless the list is about to run out of what it has. */
	if (older_held && ((!zmc.fling && !zmc.touching) || zmc.scroll < 400)) {
		older_apply();
		zmc.cache_stale = true;
		zmc.dirty = true;
	}
	if (!gliding && !older_held) zmc_on_frame(nullptr);
}

double end_scroll() {
	double top = ZMC_HEADER_H, content = top + std::ceil(list_total / ROW_H) * ROW_H + footer_height(P_CHAT) + 12;
	return content > zmc.height ? content - zmc.height : 0;
}

bool at_end() { return zmc.scroll >= end_scroll() - 120; }

void glide_to_end() {
	glide_from = zmc.scroll;
	glide_to = end_scroll();
	glide_start = zmc_now_ms();
	gliding = true;
	zmc_on_frame(frame_step);
}

void mark_open_read() {
	if (open_id.empty() || zmc.page != P_CHAT || !zmc.active) return;
	auto it = all_chats.find(open_id);
	if (it == all_chats.end() || it->second.unread == 0) return;
	request("mark_read", {{"chat", str(open_id)}});
}

std::string pending_open;
void show_chat(const std::string &id, const std::string &name_if_new = "");
void load_extras();

void open_pending() {
	if (pending_open.empty() || sock_fd < 0 || (pair_state != "paired" && pair_state != "connecting")) return;
	std::string id = pending_open;
	pending_open.clear();
	if (id != open_id || zmc.page != P_CHAT) show_chat(id, "");
}

void load_chats() {
	request("chats", {{"limit", json_object_new_int(500)}}, [](json_object *r, const char *err) {
		if (err || !r) return;
		json_object *list = jget(r, "chats");
		if (!list) return;
		zmc_list_lock(true);
		all_chats.clear();
		for (size_t i = 0; i < json_object_array_length(list); i++) chat_from(json_object_array_get_idx(list, i));
		chats_sort();
		zmc_list_lock(false);
		batch_page = true;
		open_pending();
	});
}

Found found_from(json_object *o) {
	Found f;
	f.chat = jstr(o, "chat");
	auto it = all_chats.find(f.chat);
	f.chat_name = it != all_chats.end() ? it->second.name : jstr(o, "sender_name");
	f.who = jstr(o, "sender_name");
	f.text = jstr(o, "text");
	if (f.text.empty()) f.text = jstr(o, "kind");
	f.ts = jint(o, "timestamp");
	return f;
}

void load_statuses() {
	request("statuses", {}, [](json_object *r, const char *err) {
		json_object *list = err ? nullptr : jget(r, "contacts");
		if (!list) return;
		zmc_list_lock(true);
		statuses.clear();
		for (size_t i = 0; i < json_object_array_length(list); i++) {
			json_object *c = json_object_array_get_idx(list, i);
			StatusPerson p;
			p.sender = jstr(c, "sender");
			p.name = jstr(c, "name");
			p.last_ts = jint(c, "last_ts");
			p.unseen = int(jint(c, "unseen"));
			json_object *items = jget(c, "items");
			for (size_t k = 0; items && k < json_object_array_length(items); k++) {
				json_object *it = json_object_array_get_idx(items, k);
				StatusItem si;
				si.id = jstr(it, "id");
				si.text = jstr(it, "text");
				si.kind = jstr(it, "kind");
				si.ts = jint(it, "timestamp");
				si.seen = jbool(it, "seen");
				if (json_object *media = jget(it, "media")) {
					si.thumb_b64 = jstr(media, "thumb_b64");
					si.path = jstr(media, "path");
				}
				p.items.push_back(std::move(si));
			}
			statuses.push_back(std::move(p));
		}
		list_changed();
		zmc_list_lock(false);
		batch_page = true;
	});
}

void load_starred() {
	request("starred", {{"limit", json_object_new_int(200)}}, [](json_object *r, const char *err) {
		json_object *list = err ? nullptr : jget(r, "messages");
		if (!list) return;
		zmc_list_lock(true);
		starred_list.clear();
		for (size_t i = 0; i < json_object_array_length(list); i++) starred_list.push_back(found_from(json_object_array_get_idx(list, i)));
		list_changed();
		zmc_list_lock(false);
		batch_page = true;
	});
}

void contacts_filter() {
	std::string q = new_text;
	gchar *fq = g_utf8_casefold(q.c_str(), -1);
	contacts_shown.clear();
	for (auto &c : contacts) {
		gchar *fn = g_utf8_casefold(c.name.c_str(), -1);
		if (!fq[0] || strstr(fn, fq) || c.phone.find(q) != std::string::npos) contacts_shown.push_back(c);
		g_free(fn);
	}
	g_free(fq);
	list_changed();
}

void load_contacts() {
	request("contacts", {{"limit", json_object_new_int(5000)}}, [](json_object *r, const char *err) {
		json_object *list = err ? nullptr : jget(r, "contacts");
		if (!list) return;
		zmc_list_lock(true);
		contacts.clear();
		for (size_t i = 0; i < json_object_array_length(list); i++) {
			json_object *c = json_object_array_get_idx(list, i);
			contacts.push_back({jstr(c, "jid"), jstr(c, "name"), jstr(c, "phone")});
		}
		contacts_filter();
		zmc_list_lock(false);
		batch_page = true;
	});
}

void load_extras() {
	load_statuses();
	load_starred();
}

/* one picture at a time: each may repaint the rows in sight */
void pictures_next() {
	while (!picture_busy && sock_fd >= 0) {
		zmc_list_lock(true);
		bool none = picture_queue.empty();
		zmc_list_lock(false);
		if (none) return;
		zmc_list_lock(true);	/* the drawing thread adds to these */
		std::string jid = picture_queue.back();
		picture_queue.pop_back();
		zmc_list_lock(false);
		picture_busy = true;
		request("profile_picture", {{"jid", str(jid)}, {"preview", json_object_new_boolean(true)}}, [jid](json_object *r, const char *err) {
			picture_busy = false;
			std::string path = err ? "" : jstr(r, "path");
			GdkPixbuf *p = path.empty() ? nullptr : gdk_pixbuf_new_from_file_at_scale(path.c_str(), 224, 224, TRUE, nullptr);
			if (p) {
				cairo_surface_t *s = surface_of_pixbuf(p);
				g_object_unref(p);
				zmc_list_lock(true);
				if (pictures[jid]) cairo_surface_destroy(pictures[jid]);
				pictures[jid] = s;
				list_changed();
				zmc_list_lock(false);
				batch_page = true;
			}
			pictures_next();
		});
	}
}

void go_to(int page, int dir) {
	zmc_list_lock(true);
	list_changed();
	zmc_list_lock(false);
	zmc_go(page, dir);
}

void show_chat(const std::string &id, const std::string &name_if_new) {
	static bool opening;
	if (opening) return;	/* a second tap while the first is on its way */
	opening = true;
	auto it = all_chats.find(id);
	open_id = id;
	open_name = it != all_chats.end() ? it->second.name : name_if_new;
	open_group = it != all_chats.end() && it->second.is_group;
	presence_line.clear();
	draft.clear();
	box_h = 44;
	focus = -1;
	footer_rev++;
	older_more = older_pending = false;
	/* the page turns once its messages are here, showing their end */
	request("messages", {{"chat", str(id)}, {"limit", json_object_new_int(PAGE_SIZE)}}, [id](json_object *r, const char *err) {
		opening = false;
		if (id != open_id) return;
		zmc_list_lock(true);
		if (older_held) { json_object_put(older_held); older_held = nullptr; }
		msgs_clear();
		json_object *list = err ? nullptr : jget(r, "messages");
		for (size_t i = 0; list && i < json_object_array_length(list); i++)
			if (auto m = msg_from(json_object_array_get_idx(list, i))) msgs.push_back(std::move(m));
		older_more = !err && jbool(r, "more");
		place();
		list_changed();
		/* zmc_go does nothing mid-turn or on the same page, and then no arrived()
		 * would ever take the shifted view off again */
		bool turns = !zmc.anim_active && zmc.page != P_CHAT;
		entry_off = turns ? end_scroll() : 0;
		zmc_list_lock(false);
		if (turns) zmc_go(P_CHAT, 1);
		else zmc_scroll_end();
		request("subscribe_presence", {{"chat", str(id)}});
	});
}

void load_older() {
	if (older_pending || older_held || !older_more || open_id.empty() || msgs.empty()) return;
	older_pending = true;
	const Msg &oldest = *msgs.front();
	std::string id = open_id;
	request("messages", {{"chat", str(id)}, {"limit", json_object_new_int(PAGE_SIZE)},
			{"before_ts", json_object_new_int64(oldest.ts)}, {"before_id", str(oldest.id)}},
			[id](json_object *r, const char *err) {
		older_pending = false;
		if (id != open_id || err || !r || !jget(r, "messages")) return;
		older_held = json_object_get(r);
		zmc_on_frame(frame_step);
	});
}

void older_apply() {
	json_object *r = older_held;
	older_held = nullptr;
	json_object *list = jget(r, "messages");
	zmc_list_lock(true);
	double before = list_total;
	size_t count = json_object_array_length(list);
	for (size_t i = count; i-- > 0;)
		if (auto m = msg_from(json_object_array_get_idx(list, i))) msgs.push_front(std::move(m));
	older_more = jbool(r, "more");
	place();
	list_changed();
	/* what is on the screen stays where it is */
	zmc.scroll += list_total - before;
	zmc_list_lock(false);
	json_object_put(r);
}

void older_wake(int fd, void *) {
	char drain[16];
	bool older = false;
	ssize_t n;
	while ((n = read(fd, drain, sizeof(drain))) > 0) for (ssize_t i = 0; i < n; i++) older = older || drain[i] == 'o';
	if (older) { older_asked = false; load_older(); }
	pictures_next();
}

void qr_set(const std::string &text) {
	pair_qr_text = text;
	qr_code = qr::encode(text);
}

void on_event(json_object *o) {
	std::string ev = jstr(o, "event");
	if (ev == "hello") {
		foreground = false;
	} else if (ev == "connection") {
		std::string was = connection;
		connection = jstr(o, "state");
		if (connection == "connected" && was != "connected") { load_chats(); load_extras(); }
		batch_page = true;
	} else if (ev == "pairing") {
		std::string state = jstr(o, "state");
		pair_error = jstr(o, "error");
		if (state == "failed") state = "unpaired";
		if (state == "qr") qr_set(jstr(o, "qr"));
		if (state == "phone_code") pair_code = jstr(o, "code");
		pair_state = state;
		if (state == "paired") {
			self_jid = jstr(o, "self");
			load_chats();
			if (zmc.page == P_LINK) go_to(P_CHATS, 1);
		} else if (zmc.page != P_LINK) {
			focus = -1;
			go_to(P_LINK, -1);
		}
		batch_page = true;
	} else if (ev == "chat") {
		zmc_list_lock(true);
		chat_from(jget(o, "chat"));
		zmc_list_lock(false);
		batch_chats = true;
	} else if (ev == "message") {
		json_object *mo = jget(o, "message");
		if (!mo || jstr(o, "chat") != open_id) return;
		auto m = msg_from(mo);
		if (!m) return;
		bool end = at_end() || m->from_me;
		zmc_list_lock(true);
		Msg *known = nullptr;
		for (auto it = msgs.rbegin(); it != msgs.rend() && !known; ++it) if ((*it)->id == m->id) known = it->get();
		if (known) {
			layouts_held.erase(std::remove(layouts_held.begin(), layouts_held.end(), known), layouts_held.end());
			layout_drop(known);
			*known = std::move(*m);
		} else {
			msgs.push_back(std::move(m));
		}
		place();
		list_changed();
		zmc_list_lock(false);
		if (zmc.page == P_CHAT) {
			zmc_redraw();
			if (!known && end) glide_to_end();
			if (zmc.active) request("mark_read", {{"chat", str(open_id)}});
		}
	} else if (ev == "receipt") {
		if (jstr(o, "chat") != open_id) return;
		json_object *ids = jget(o, "ids");
		std::string status = jstr(o, "status");
		zmc_list_lock(true);
		bool any = false;
		for (size_t i = 0; ids && i < json_object_array_length(ids); i++) {
			const char *id = json_object_get_string(json_object_array_get_idx(ids, i));
			for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) {
				if ((*it)->id == id) {
					(*it)->status = status;
					any = true;
					break;
				}
			}
		}
		if (any) list_changed();
		zmc_list_lock(false);
		if (any) batch_page = true;
	} else if (ev == "presence") {
		if (jstr(o, "chat") != open_id) return;
		if (jbool(o, "typing")) presence_line = "typing...";
		else if (jbool(o, "available")) presence_line = "online";
		else if (long seen = jint(o, "last_seen")) presence_line = "last seen " + when_text(seen);
		else presence_line.clear();
		batch_page = true;
	} else if (ev == "history_sync" || ev == "app_state") {
		if (jstr(o, "state") == "done") { load_chats(); load_extras(); }
	} else if (ev == "status") {
		load_statuses();
	} else if (ev == "contacts") {
		if (zmc.page == P_NEW) load_contacts();
	} else if (ev == "picture") {
		zmc_list_lock(true);
		std::string jid = jstr(o, "jid");
		auto it = pictures.find(jid);
		if (it != pictures.end() && !it->second) { pictures.erase(it); list_changed(); batch_page = true; }
		else if (it != pictures.end()) { picture_queue.push_back(jid); }
		zmc_list_lock(false);
		pictures_next();
	}
}

void disconnect_daemon();
void tick();
void focus_sync();

void sock_ready(int fd, void *) {
	char chunk[65536];
	bool gone = false;
	for (;;) {
		ssize_t n = recv(fd, chunk, sizeof(chunk), MSG_DONTWAIT);
		if (n > 0) { in_buf.append(chunk, size_t(n)); continue; }
		if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) gone = true;
		break;
	}
	size_t start = 0, nl;
	while ((nl = in_buf.find('\n', start)) != std::string::npos) {
		json_object *o = json_tokener_parse(in_buf.substr(start, nl - start).c_str());
		start = nl + 1;
		if (!o) continue;
		json_object *idv;
		/* (a message event has an "id" too: the message's, a string) */
		if (json_object_object_get_ex(o, "event", &idv)) {
			on_event(o);
		} else if (json_object_object_get_ex(o, "id", &idv)) {
			auto it = waiting.find(json_object_get_int(idv));
			if (it != waiting.end()) {
				Reply r = std::move(it->second);
				waiting.erase(it);
				if (jbool(o, "ok")) r(jget(o, "result"), nullptr);
				else { std::string e = jstr(o, "error"); r(nullptr, e.empty() ? "internal" : e.c_str()); }
			}
		}
		json_object_put(o);
	}
	in_buf.erase(0, start);
	flush_out();
	/* a burst of events is one redraw */
	if (batch_chats) {
		zmc_list_lock(true);
		chats_sort();
		zmc_list_lock(false);
		batch_page = true;
		batch_chats = false;
	}
	if (batch_page) {
		batch_page = false;
		zmc_redraw();
	}
	if (gone) disconnect_daemon();
}

std::string sock_path() {
	if (const char *o = getenv("ZTWHATSAPP_SOCK")) return o;
	const char *run = getenv("XDG_RUNTIME_DIR");
	return std::string(run ? run : "/tmp") + "/ztwhatsapp.sock";
}

bool connect_daemon() {
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	struct sockaddr_un a{};
	a.sun_family = AF_UNIX;
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", sock_path().c_str());
	if (fd < 0 || connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) < 0) {
		if (fd >= 0) close(fd);
		return false;
	}
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	sock_fd = fd;
	zmc_watch_fd(fd, sock_ready, nullptr);
	zmc_every(5000, tick);
	return true;
}

void start_daemon() {
	if (daemon_started || getenv("ZTWHATSAPP_SOCK")) return;
	daemon_started = true;
	if (fork() == 0) {
		setsid();
		if (fork() == 0) {
			execlp("ztwhatsappd", "ztwhatsappd", nullptr);
		}
		_exit(0);
	}
	(void)!wait(nullptr);
}

void disconnect_daemon() {
	if (sock_fd >= 0) {
		zmc_watch_fd(sock_fd, nullptr, nullptr);
		close(sock_fd);
	}
	sock_fd = -1;
	in_buf.clear();
	out_buf.clear();
	auto pending = std::move(waiting);
	waiting.clear();
	for (auto &[id, r] : pending) r(nullptr, "not_connected");
	connection = "connecting";
	foreground = false;
	zmc_every(500, tick);
	zmc_redraw();
}

/* Only while the socket is away, or every 5 s to tell the daemon we went behind
 * other windows: the core has no call for that. */
void tick() {
	if (sock_fd < 0) {
		if (!connect_daemon()) start_daemon();
		return;
	}
	focus_sync();
	if (foreground && !zmc.active) {
		foreground = false;
		request("set_presence", {{"available", json_object_new_boolean(false)}});
	}
}

/* ---- pages ---------------------------------------------------------------- */

double centred_note(cairo_t *cr, const std::string &s, double y) {
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	double h = zmc_text(cr, s.c_str(), FONT_REGULAR, 15, MARGIN, y, zmc.width - 2 * MARGIN);
	cairo_new_path(cr);
	return y + h + 14;
}

/* Modules on whole device pixels, no anti-aliasing, a white quiet zone: dark and
 * light must scan, so under DotMatrix too this is the one thing not in dots. */
void draw_qr(cairo_t *cr, const qr::Code &code, double y, double side) {
	if (code.n == 0) return;
	double dx = 1, dy = 1;
	cairo_user_to_device_distance(cr, &dx, &dy);
	double scale = dx > 0 ? dx : 1;
	const int quiet = 4;
	int px = std::max(1, int(side * scale) / (code.n + 2 * quiet));
	double module = px / scale, total = module * (code.n + 2 * quiet), x0 = std::floor((zmc.width - total) / 2 * scale) / scale;
	y = std::floor(y * scale) / scale;
	dm_wrap_raw++;
	cairo_save(cr);
	cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_rectangle(cr, x0, y, total, total);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 0, 0, 0);
	for (int r = 0; r < code.n; r++) for (int c = 0; c < code.n; c++)
		if (code.at(c, r)) cairo_rectangle(cr, x0 + (c + quiet) * module, y + (r + quiet) * module, module, module);
	cairo_fill(cr);
	cairo_restore(cr);
	dm_wrap_raw--;
}

double draw_link(cairo_t *cr) {
	double y = zmc_header(cr, "link device");
	double w = zmc.width - 2 * MARGIN;
	if (pair_state == "qr") {
		y = centred_note(cr, "On your phone, open WhatsApp, then Linked devices, then Link a device, and scan this.", y);
		draw_qr(cr, qr_code, y, w);
		y += w + 14;
		cairo_set_source_rgb(cr, 1, 1, 1);
		y = zmc_button(cr, MARGIN, y, 160, "cancel", ACT_CANCEL, 0);
	} else if (pair_state == "phone_code") {
		y = centred_note(cr, "On your phone, open WhatsApp, then Linked devices, then Link a device, then Link with phone number, and type this code.", y);
		cairo_set_source_rgb(cr, 1, 1, 1);
		y += zmc_text(cr, pair_code.c_str(), FONT_SEMILIGHT, 40, MARGIN, y, 0) + 20;
		cairo_new_path(cr);
		y = zmc_button(cr, MARGIN, y, 160, "cancel", ACT_CANCEL, 0);
	} else if (pair_state == "pairing") {
		y = centred_note(cr, "Finishing the link...", y);
	} else if (pair_state == "unpaired") {
		if (!pair_error.empty()) {
			zmc_accent(cr);
			y += zmc_text(cr, pair_error.c_str(), FONT_REGULAR, 14, MARGIN, y, w) + 12;
			cairo_new_path(cr);
		}
		y = centred_note(cr, "Link this phone to your WhatsApp account. Your other phone keeps the account; this one becomes a linked device.", y);
		cairo_set_source_rgb(cr, 1, 1, 1);
		y = zmc_button(cr, MARGIN, y, 220, "show a QR code", ACT_PAIR_QR, 0) + 18;
		y = zmc_textbox(cr, y, "or your number, with country code", phone.c_str(), focus == 1, ACT_PHONE_BOX, 0);
		cairo_set_source_rgb(cr, 1, 1, 1);
		y = zmc_button(cr, MARGIN, y, 220, "use phone number", ACT_PAIR_PHONE, 0);
	} else {
		y = centred_note(cr, sock_fd < 0 ? "Waiting for the WhatsApp service..." : "Connecting...", y);
	}
	return y + 24;
}

/* four tabs going round, as the phone's */
double tab_header(cairo_t *cr, const char *title, const char *next, const char *after, int next_page) {
	zmc.pivot_after = after;
	return zmc_pivot_next(cr, title, next, ZMC_ACT_PIVOT, next_page);
}

double tab_list(cairo_t *cr, double y, int page, int count, void (*draw)(cairo_t *, int), bool (*hit)(int, double, double, struct zmc_hit *)) {
	struct zmc_list l{};
	l.count = count;
	l.row_h = ROW_H;
	l.revision = list_rev + uint64_t(page);
	l.draw_row = draw;
	l.row_hit = hit;
	return zmc_list(cr, y, &l);
}

double draw_chats(cairo_t *cr) {
	double y = tab_header(cr, "chats", "status", "starred", P_STATUS);
	if (connection != "connected") {
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		const char *s = connection == "offline" ? "offline, waiting to reconnect" : "connecting...";
		zmc_text(cr, s, FONT_REGULAR, 12.5, MARGIN, y - 20, 0);
		cairo_new_path(cr);
	}
	if (chats.empty()) return centred_note(cr, "No chats yet.", y);
	return tab_list(cr, y, P_CHATS, int(chats.size()), draw_chat_row, chat_row_hit) + 76 + 12;
}

double draw_status(cairo_t *cr) {
	double y = tab_header(cr, "status", "starred", "archived", P_STARRED);
	if (statuses.empty()) return centred_note(cr, "No status updates in the last 24 hours.", y);
	return tab_list(cr, y, P_STATUS, int(statuses.size()), draw_status_row, status_row_hit) + 76 + 12;
}

double draw_starred(cairo_t *cr) {
	double y = tab_header(cr, "starred", "archived", "chats", P_ARCHIVED);
	if (starred_list.empty()) return centred_note(cr, "Messages you star on your phone show here.", y);
	return tab_list(cr, y, P_STARRED, int(starred_list.size()), draw_starred_row, starred_row_hit) + 24;
}

double draw_archived(cairo_t *cr) {
	double y = tab_header(cr, "archived", "chats", "status", P_CHATS);
	if (archived.empty()) return centred_note(cr, "No archived chats.", y);
	return tab_list(cr, y, P_ARCHIVED, int(archived.size()), draw_archived_row, archived_row_hit) + 24;
}

double draw_chat(cairo_t *cr) {
	double y = zmc_header(cr, "");
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text_line(cr, open_name.c_str(), FONT_SEMILIGHT, 28, MARGIN, 46, zmc.width - 2 * MARGIN);
	cairo_new_path(cr);
	if (!presence_line.empty()) {
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		zmc_text_line(cr, presence_line.c_str(), FONT_REGULAR, 13, MARGIN, 82, zmc.width - 2 * MARGIN);
		cairo_new_path(cr);
	}
	struct zmc_list l{};
	l.count = std::max(1, int(std::ceil(list_total / ROW_H)));
	l.row_h = ROW_H;
	l.revision = list_rev + P_CHAT;
	l.draw_row = draw_strip;
	l.row_hit = strip_hit;
	return zmc_list(cr, y, &l) + footer_height(P_CHAT) + 12;
}

double draw_account(cairo_t *cr) {
	double y = zmc_header(cr, "account");
	y = zmc_field(cr, y, "connection", connection.c_str());
	if (!dots()) y = zmc_switch(cr, y, "system accent", follow_system_accent, ACT_ACCENT);
	if (!self_jid.empty()) y = zmc_field(cr, y, "signed in", "yes");
	cairo_set_source_rgb(cr, 1, 1, 1);
	if (logout_ask) {
		y = centred_note(cr, "This unlinks the phone and deletes the messages stored here.", y + 10);
		cairo_set_source_rgb(cr, 1, 1, 1);
		y = zmc_button(cr, MARGIN, y, 220, "yes, unlink", ACT_LOGOUT_SURE, 0);
	} else {
		y = zmc_button(cr, MARGIN, y + 10, 220, "unlink this phone", ACT_LOGOUT, 0);
	}
	return y + 24;
}

/* Whole-screen photo: scaled once into the page's cache, then only moved. */
double draw_image(cairo_t *cr) {
	double y = zmc_header(cr, "");
	if (full_image) {
		double iw = cairo_image_surface_get_width(full_image), ih = cairo_image_surface_get_height(full_image);
		double room = zmc.height - y - MARGIN, k = std::min(zmc.width / iw, room / ih);
		if (dots()) {
			zmc_picture(cr, full_image, 0, y, zmc.width);
		} else {
			cairo_save(cr);
			cairo_translate(cr, (zmc.width - iw * k) / 2, y + (room - ih * k) / 2);
			cairo_scale(cr, k, k);
			cairo_set_source_surface(cr, full_image, 0, 0);
			cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
			cairo_paint(cr);
			cairo_restore(cr);
		}
	} else {
		centred_note(cr, image_note, y);
	}
	return zmc.height;
}

void draw_compose(cairo_t *cr);
void draw_chats_footer(cairo_t *cr, int page);

double draw_status_view(cairo_t *cr) {
	if (status_person < 0 || size_t(status_person) >= statuses.size() || statuses[size_t(status_person)].items.empty()) return zmc_header(cr, "status");
	const StatusPerson &p = statuses[size_t(status_person)];
	const StatusItem &it = p.items[size_t(status_item)];
	double y = zmc_header(cr, "");
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text_line(cr, p.name.c_str(), FONT_SEMILIGHT, 28, MARGIN, 46, zmc.width - 2 * MARGIN);
	cairo_new_path(cr);
	std::string line = std::to_string(status_item + 1) + " of " + std::to_string(p.items.size()) + ", " + when_text(it.ts);
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text_line(cr, line.c_str(), FONT_REGULAR, 13, MARGIN, 82, zmc.width - 2 * MARGIN);
	cairo_new_path(cr);
	double room = zmc.height - y - MARGIN, w = zmc.width - 2 * MARGIN;
	if (it.kind == "image" && full_image) {
		if (dots()) {
			zmc_picture(cr, full_image, MARGIN, y, w);
			y += w + 12;
		} else {
			double iw = cairo_image_surface_get_width(full_image), ih = cairo_image_surface_get_height(full_image);
			double k = std::min(w / iw, (room - 80) / ih);
			cairo_save(cr);
			cairo_translate(cr, (zmc.width - iw * k) / 2, y);
			cairo_scale(cr, k, k);
			cairo_set_source_surface(cr, full_image, 0, 0);
			cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
			cairo_paint(cr);
			cairo_restore(cr);
			y += ih * k + 12;
		}
	} else if (it.kind == "image" || it.kind == "video") {
		y = centred_note(cr, it.kind == "video" ? "Video updates can only be seen on your phone." : image_note, y);
	}
	if (!it.text.empty()) {
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text(cr, it.text.c_str(), FONT_SEMILIGHT, it.kind == "text" ? 28 : 16, MARGIN, y, w);
		cairo_new_path(cr);
	}
	zmc_add_hit(0, ZMC_HEADER_H, zmc.width, zmc.height - ZMC_HEADER_H, ACT_STATUS_NEXT, 0);
	return zmc.height;
}

double draw_status_post(cairo_t *cr) {
	double y = zmc_header(cr, "new status");
	y = zmc_textbox(cr, y, "what is on your mind", post_text.c_str(), focus == 2, ACT_POST_BOX, 0);
	cairo_set_source_rgb(cr, 1, 1, 1);
	y = zmc_button(cr, MARGIN, y, 160, "post", ACT_POST, 0);
	if (!post_note.empty()) y = centred_note(cr, post_note, y + 12);
	return y + 24;
}

double draw_info(cairo_t *cr) {
	double y = zmc_header(cr, "");
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text_line(cr, open_name.c_str(), FONT_SEMILIGHT, 28, MARGIN, 46, zmc.width - 2 * MARGIN);
	cairo_new_path(cr);
	double side = 180;
	if (cairo_surface_t *pic = picture_of(open_id)) {
		zmc_picture(cr, pic, MARGIN, y, side);
		y += side + 14;
	}
	auto it = all_chats.find(open_id);
	if (!open_group) {
		size_t at = open_id.find("@s.whatsapp.net");
		if (at != std::string::npos) y = zmc_field(cr, y, "number", ("+" + open_id.substr(0, at)).c_str());
	}
	if (!info_topic.empty()) y = zmc_field(cr, y, "about", info_topic.c_str());
	if (it != all_chats.end()) {
		y = zmc_field(cr, y, "notifications", it->second.muted ? "muted" : "on");
		if (it->second.pinned) y = zmc_field(cr, y, "pinned", "yes");
		if (it->second.archived) y = zmc_field(cr, y, "archived", "yes");
	}
	if (open_group && !members.empty()) {
		std::string caption = std::to_string(members.size()) + " participants";
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		zmc_text(cr, caption.c_str(), FONT_REGULAR, 14, MARGIN, y + 6, 0);
		cairo_new_path(cr);
		y += 32;
		cairo_set_source_rgb(cr, 1, 1, 1);
		for (const Member &m : members) {
			zmc_text_line(cr, m.name.c_str(), FONT_SEMILIGHT, 20, MARGIN, y, zmc.width - 2 * MARGIN - (m.admin ? 60 : 0));
			if (m.admin) {
				cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
				zmc_text(cr, "admin", FONT_REGULAR, 12.5, zmc.width - MARGIN - 40, y + 7, 0);
				cairo_set_source_rgb(cr, 1, 1, 1);
			}
			cairo_new_path(cr);
			y += 34;
		}
	}
	if (!info_note.empty()) y = centred_note(cr, info_note, y);
	y += 8;
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	y += zmc_text(cr, "Mute, pin and archive are changed on your phone.", FONT_REGULAR, 13, MARGIN, y, zmc.width - 2 * MARGIN);
	cairo_new_path(cr);
	return y + 24;
}

double draw_search(cairo_t *cr) {
	double y = zmc_header(cr, "search");
	y = zmc_textbox(cr, y, "names, or messages with the button", search_text.c_str(), focus == 3, ACT_SEARCH_BOX, 0);
	cairo_set_source_rgb(cr, 1, 1, 1);
	if (search_text.empty()) return y + 24;
	y = zmc_button(cr, MARGIN, y, 220, "search messages", ACT_SEARCH_MESSAGES, 0) + 10;
	gchar *q = g_utf8_casefold(search_text.c_str(), -1);
	int shown = 0;
	for (size_t i = 0; i < chats.size() + archived.size() && shown < 8; i++) {
		const Chat &c = i < chats.size() ? chats[i] : archived[i - chats.size()];
		gchar *n = g_utf8_casefold(c.name.c_str(), -1);
		if (strstr(n, q)) {
			y = zmc_entry(cr, y, c.name.c_str(), c.last_text.c_str(), i < chats.size() ? ACT_OPEN : ACT_OPEN_ARCHIVED, int(i < chats.size() ? i : i - chats.size()));
			shown++;
		}
		g_free(n);
	}
	g_free(q);
	for (size_t i = 0; i < search_found.size() && i < 40; i++)
		y = zmc_entry(cr, y, search_found[i].chat_name.c_str(), search_found[i].text.c_str(), ACT_OPEN_ID, int(i));
	return y + 24;
}

double draw_new(cairo_t *cr) {
	double y = zmc_header(cr, "new chat");
	y = zmc_textbox(cr, y, "a name or a number", new_text.c_str(), focus == 4, ACT_NEW_BOX, 0);
	if (contacts_shown.empty()) return centred_note(cr, contacts.empty() ? "Your contacts have not reached this phone yet." : "Nobody by that name.", y + 8);
	return tab_list(cr, y + 8, P_NEW, int(contacts_shown.size()), draw_contact_row, contact_row_hit) + 24;
}

double render(cairo_t *cr, int page) {
	accent_apply();
	switch (page) {
	case P_LINK: return draw_link(cr);
	case P_STATUS_VIEW: return draw_status_view(cr);
	case P_STATUS_POST: return draw_status_post(cr);
	case P_INFO: return draw_info(cr);
	case P_SEARCH: return draw_search(cr);
	case P_NEW: return draw_new(cr);
	case P_STARRED: return draw_starred(cr);
	case P_ARCHIVED: return draw_archived(cr);
	case P_CHATS:
	case P_STATUS:
	case P_CHAT: {
		double y = page == P_CHATS ? draw_chats(cr) : page == P_STATUS ? draw_status(cr) : draw_chat(cr);
		if (sample) {	/* a PNG has no overlay: the footer in its place */
			cairo_save(cr);
			cairo_translate(cr, 0, zmc.height - footer_height(page));
			if (page == P_CHAT) draw_compose(cr); else draw_chats_footer(cr, page);
			cairo_restore(cr);
		}
		return y;
	}
	case P_ACCOUNT: return draw_account(cr);
	case P_IMAGE: return draw_image(cr);
	}
	return 0;
}

/* ---- the footers: their own texture ---------------------------------------- */

const char *box_shown() {
	static std::string shown;
	shown = draft + (focus == 0 ? "|" : "");
	return shown.c_str();
}

void box_measure() {
	zmc_list_lock(true);
	double h = dm_text_height(box_shown(), BOX_FONT, zmc.width - 2 * MARGIN - 20) + 22;
	zmc_list_lock(false);
	box_h = draft.empty() ? 44 : std::clamp(h, 44.0, BOX_MAX);
}

void draw_compose(cairo_t *cr) {
	const double box_w = zmc.width - 2 * MARGIN, y = 0;
	bool d = dots();
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_rectangle(cr, 0, 0, zmc.width, box_h + 94);
	cairo_fill(cr);
	cairo_rectangle(cr, MARGIN, y + 8, box_w, box_h);
	if (d) {
		cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
	} else {
		double g = focus == 0 ? 1 : 0.82;
		cairo_set_source_rgb(cr, g, g, g);
		cairo_fill(cr);
	}
	cairo_new_path(cr);
	bool have = !draft.empty() || focus == 0;
	double ink = d ? (have ? 1 : 0.6) : (have ? 0 : 0.35);
	cairo_set_source_rgb(cr, ink, ink, ink);
	if (have) {
		double inner = box_w - 20, whole = dm_text_height(box_shown(), BOX_FONT, inner);
		cairo_save(cr);
		cairo_rectangle(cr, MARGIN, y + 8, box_w, box_h);
		cairo_clip(cr);
		zmc_text(cr, box_shown(), FONT_REGULAR, BOX_FONT, MARGIN + 10, y + 19 - std::max(0.0, whole + 22 - box_h), inner);
		cairo_restore(cr);
	} else {
		zmc_text_line(cr, "type a message", FONT_REGULAR, BOX_FONT, MARGIN + 10, y + 19, box_w - 20);
	}
	cairo_new_path(cr);
	double dim = draft.empty() ? 0.45 : 1;
	cairo_set_source_rgb(cr, dim, dim, dim);
	zmc_round_button(cr, 46, y + box_h + 18, "send", "send", 0, 0);
	cairo_new_path(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, "...", FONT_SEMIBOLD, 24, zmc.width - 40, y + box_h + 19, 0);	/* who this is */
	cairo_new_path(cr);
}

void draw_chats_footer(cairo_t *cr, int page) {
	cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
	cairo_rectangle(cr, 0, 0, zmc.width, 76);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	if (page == P_STATUS) {
		zmc_round_button(cr, 62, 8, "add", "new", 0, 0);
	} else {
		zmc_round_button(cr, 62, 8, "add", "new", 0, 0);
		cairo_new_path(cr);
		zmc_round_button(cr, zmc.width / 2.0, 8, "search", "search", 0, 0);
		cairo_new_path(cr);
		zmc_round_button(cr, zmc.width - 62, 8, "people", "account", 0, 0);
	}
	cairo_new_path(cr);
}

/* ---- the notifier's two files (notify/README.md) ---------------------------- */

std::string run_file(const char *name) {
	const char *run = getenv("XDG_RUNTIME_DIR");
	return std::string(run ? run : "/tmp") + "/" + name;
}

/* PID CHAT, only while a chat is on screen and the app in front */
void focus_sync() {
	static std::string written;
	std::string want = zmc.page == P_CHAT && zmc.active && !open_id.empty() ? open_id : "";
	if (want == written) return;
	written = want;
	std::string path = run_file("ztwhatsapp.focus");
	if (want.empty()) { unlink(path.c_str()); return; }
	std::string temp = path + ".new";
	if (FILE *f = fopen(temp.c_str(), "w")) {
		fprintf(f, "%d %s\n", int(getpid()), want.c_str());
		fclose(f);
		rename(temp.c_str(), path.c_str());
	}
}

void focus_clear() { unlink(run_file("ztwhatsapp.focus").c_str()); }

void open_file_check() {
	std::string path = run_file("ztwhatsapp.open");
	FILE *f = fopen(path.c_str(), "r");
	if (!f) return;
	char line[512] = "";
	char *got = fgets(line, sizeof(line), f);
	fclose(f);
	unlink(path.c_str());
	if (!got) return;
	line[strcspn(line, " \r\n")] = 0;
	if (line[0]) {
		pending_open = line;
		if (!all_chats.empty()) open_pending();
	}
}

void run_dir_event(int fd, void *) {
	char drain[1024];
	while (read(fd, drain, sizeof(drain)) > 0) {}
	open_file_check();
}

void overlay(int page) {
	if (!foreground && zmc.active && sock_fd >= 0) {
		foreground = true;
		request("set_presence", {{"available", json_object_new_boolean(true)}});
		mark_open_read();
	}
	focus_sync();
	accent_apply();
	int h = footer_height(page);
	if (!h) return;
	static cairo_surface_t *strip;
	static unsigned rev;
	static int width, height, drawn_page = -1;
	static unsigned tex_rev = 1;
	if (!strip || width != zmc.width || height != h || rev != footer_rev || drawn_page != page) {
		if (strip) cairo_surface_destroy(strip);
		width = zmc.width;
		height = h;
		rev = footer_rev;
		drawn_page = page;
		tex_rev++;
		strip = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width * zmc.scale, h * zmc.scale);
		cairo_t *cr = cairo_create(strip);
		cairo_scale(cr, zmc.scale, zmc.scale);
		int hits = zmc.n_hits;
		/* Pango is one thread's at a time, and the core's row thread may be drawing */
		zmc_list_lock(true);
		if (page == P_CHAT) draw_compose(cr); else draw_chats_footer(cr, page);
		zmc_list_lock(false);
		zmc.n_hits = hits;
		cairo_destroy(cr);
		cairo_surface_flush(strip);
	}
	double top = zmc.height - h;
	const double corners[4][3] = {{0, top, 0}, {double(zmc.width), top, 0}, {0, double(zmc.height), 0}, {double(zmc.width), double(zmc.height), 0}};
	zmc_gpu_quad(strip, tex_rev, corners, 1);
	if (page == P_CHAT && focus == 0) zmc_osk_box(zmc.height);
}

/* ---- what taps do ---------------------------------------------------------- */

int image_pipe[2] = {-1, -1};

/* Decoded off the main loop: a camera photo takes a good part of a second here. */
void image_load(const std::string &path) {
	if (full_image) { cairo_surface_destroy(full_image); full_image = nullptr; }
	image_note = "Opening...";
	image_wanted = true;
	int w = zmc.width * zmc.scale, h = zmc.height * zmc.scale, fd = image_pipe[1];
	std::thread([path, w, h, fd] {
		GdkPixbuf *p = gdk_pixbuf_new_from_file_at_scale(path.c_str(), w, h, TRUE, nullptr);
		cairo_surface_t *s = p ? surface_of_pixbuf(p) : nullptr;
		if (p) g_object_unref(p);
		(void)!write(fd, &s, sizeof(s));
	}).detach();
}

void image_ready(int fd, void *) {
	cairo_surface_t *s = nullptr;
	if (read(fd, &s, sizeof(s)) != sizeof(s)) return;
	/* not by zmc.page: a quick load finishes while the page is still turning to it */
	if (!image_wanted) { if (s) cairo_surface_destroy(s); return; }
	if (full_image) cairo_surface_destroy(full_image);
	full_image = s;
	if (!s) image_note = "Could not open this photo.";
	zmc_redraw();
}

void open_image(Msg &m) {
	std::string id = m.id;
	image_note = "Downloading...";
	if (full_image) { cairo_surface_destroy(full_image); full_image = nullptr; }
	if (!m.path.empty()) {
		image_load(m.path);
	} else {
		request("download_media", {{"chat", str(open_id)}, {"message_id", str(id)}}, [id](json_object *r, const char *err) {
			std::string path = err ? "" : jstr(r, "path");
			if (path.empty()) {
				image_note = "Could not download this photo.";
			} else {
				for (auto &x : msgs) if (x->id == id) x->path = path;
				image_load(path);
			}
			zmc_redraw();	/* maybe mid-turn: arrived() redraws too, so the note cannot stick */
		});
	}
	zmc_go(P_IMAGE, 1);
}

void send_draft() {
	if (draft.empty() || open_id.empty()) return;
	std::string text = draft;
	request("send_text", {{"chat", str(open_id)}, {"text", str(text)}}, [text](json_object *, const char *err) {
		if (err && draft.empty()) {	/* nothing lost: back in the box */
			draft = text;
			box_measure();
			footer_rev++;
			zmc_redraw();
		}
	});
	request("typing", {{"chat", str(open_id)}, {"state", str("paused")}});
	last_typing = 0;
	draft.clear();
	zmc_haptic("click");
	box_measure();
	footer_rev++;
}

void status_show() {
	StatusPerson &p = statuses[size_t(status_person)];
	StatusItem &it = p.items[size_t(status_item)];
	if (full_image) { cairo_surface_destroy(full_image); full_image = nullptr; }
	image_note = "Downloading...";
	if (it.kind == "image") {
		if (!it.path.empty()) {
			image_load(it.path);
		} else {
			int who = status_person, which = status_item;
			request("download_media", {{"chat", str("status@broadcast")}, {"message_id", str(it.id)}}, [who, which](json_object *r, const char *err) {
				std::string path = err ? "" : jstr(r, "path");
				if (who != status_person || which != status_item) return;
				if (path.empty()) image_note = "Could not download this update.";
				else image_load(path);
				if (zmc.page == P_STATUS_VIEW) zmc_redraw();
			});
		}
	}
	if (!it.seen) {
		it.seen = true;
		if (p.unseen > 0) p.unseen--;
		json_object *ids = json_object_new_array();
		json_object_array_add(ids, str(it.id));
		request("status_seen", {{"sender", str(p.sender)}, {"ids", ids}});
		zmc_list_lock(true);
		list_changed();
		zmc_list_lock(false);
	}
}

void back_from_status() {
	status_person = -1;
	if (full_image) { cairo_surface_destroy(full_image); full_image = nullptr; }
	go_to(P_STATUS, -1);
}

void activate(int action, int arg) {
	switch (action) {
	case ACT_OPEN:
		if (arg >= 0 && size_t(arg) < chats.size()) show_chat(chats[size_t(arg)].id);
		break;
	case ACT_OPEN_ARCHIVED:
		if (arg >= 0 && size_t(arg) < archived.size()) show_chat(archived[size_t(arg)].id);
		break;
	case ACT_OPEN_STARRED:
		if (arg >= 0 && size_t(arg) < starred_list.size()) show_chat(starred_list[size_t(arg)].chat);
		break;
	case ACT_OPEN_ID:
		if (arg >= 0 && size_t(arg) < search_found.size()) show_chat(search_found[size_t(arg)].chat);
		break;
	case ACT_ACCENT:
		follow_system_accent = !follow_system_accent;
		prefs_save();
		accent_apply();
		zmc_list_lock(true);
		list_changed();
		zmc_list_lock(false);
		footer_rev++;
		break;
	case ACT_STATUS:
		if (arg >= 0 && size_t(arg) < statuses.size()) {
			status_person = arg;
			status_item = 0;
			for (size_t i = 0; i < statuses[size_t(arg)].items.size(); i++)
				if (!statuses[size_t(arg)].items[i].seen) { status_item = int(i); break; }
			status_show();
			zmc_go(P_STATUS_VIEW, 1);
		}
		break;
	case ACT_STATUS_NEXT:
		if (status_person >= 0 && size_t(status_item + 1) < statuses[size_t(status_person)].items.size()) {
			status_item++;
			status_show();
		} else {
			back_from_status();
		}
		break;
	case ACT_POST_BOX: focus = 2; zmc_keyboard(true); break;
	case ACT_POST:
		if (post_text.empty()) break;
		if (post_text.size() > 700) { post_note = "That is too long: a status holds 700 bytes."; break; }
		focus = -1;
		zmc_keyboard(false);
		request("post_status", {{"text", str(post_text)}}, [](json_object *, const char *err) {
			if (err) { post_note = "Not posted. Your text is kept."; zmc_redraw(); return; }
			post_text.clear();
			post_note.clear();
			load_statuses();
			go_to(P_STATUS, -1);
		});
		break;
	case ACT_SEARCH_BOX: focus = 3; zmc_keyboard(true); break;
	case ACT_SEARCH_MESSAGES:
		focus = -1;
		zmc_keyboard(false);
		zmc_busy(true);
		request("search", {{"query", str(search_text)}, {"limit", json_object_new_int(40)}}, [](json_object *r, const char *err) {
			zmc_busy(false);
			search_found.clear();
			json_object *list = err ? nullptr : jget(r, "messages");
			for (size_t i = 0; list && i < json_object_array_length(list); i++) search_found.push_back(found_from(json_object_array_get_idx(list, i)));
			batch_page = true;
		});
		break;
	case ACT_NEW_BOX: focus = 4; zmc_keyboard(true); break;
	case ACT_NEW_PICK:
		if (arg >= 0 && size_t(arg) < contacts_shown.size()) {
			focus = -1;
			zmc_keyboard(false);
			show_chat(contacts_shown[size_t(arg)].jid, contacts_shown[size_t(arg)].name);
		}
		break;
	case ACT_PAIR_QR:
		pair_error.clear();
		request("pair_qr", {});
		break;
	case ACT_PHONE_BOX:
		focus = 1;
		zmc_keyboard(true);
		break;
	case ACT_PAIR_PHONE: {
		std::string digits;
		for (char c : phone) if (isdigit(static_cast<unsigned char>(c))) digits += c;
		if (digits.size() < 8) { pair_error = "Type your whole number first, with its country code."; break; }
		focus = -1;
		zmc_keyboard(false);
		request("pair_phone", {{"phone", str(digits)}}, [](json_object *, const char *err) {
			if (err) { pair_error = "That did not work. Check the number and the connection."; zmc_redraw(); }
		});
		break;
	}
	case ACT_CANCEL:
		request("pair_cancel", {});
		break;
	case ACT_IMAGE:
		if (arg >= 0 && size_t(arg) < msgs.size()) open_image(*msgs[size_t(arg)]);
		break;
	case ACT_RETRY:
		if (arg >= 0 && size_t(arg) < msgs.size()) {
			draft = msgs[size_t(arg)]->text;
			focus = 0;
			box_measure();
			footer_rev++;
			zmc_keyboard(true);
		}
		break;
	case ACT_LOGOUT:
		logout_ask = true;
		break;
	case ACT_LOGOUT_SURE:
		logout_ask = false;
		request("logout", {});
		break;
	}
}

void open_info() {
	focus = -1;
	zmc_keyboard(false);
	info_topic.clear();
	info_note.clear();
	members.clear();
	if (open_group) {
		std::string id = open_id;
		request("group_info", {{"chat", str(id)}}, [id](json_object *r, const char *err) {
			if (id != open_id) return;
			if (err) { info_note = "Could not get the group's details."; zmc_redraw(); return; }
			info_topic = jstr(r, "topic");
			json_object *list = jget(r, "participants");
			for (size_t i = 0; list && i < json_object_array_length(list) && i < 80; i++) {
				json_object *m = json_object_array_get_idx(list, i);
				members.push_back({jstr(m, "name"), jbool(m, "is_admin") || jbool(m, "is_super_admin")});
			}
			zmc_redraw();
		});
	}
	zmc_go(P_INFO, 1);
}

bool touch(int phase, int id, double x, double y, uint32_t) {
	static double down_x, down_y;
	static int finger = -1;
	int h = footer_height(zmc.page);
	double top = zmc.height - h;
	if (phase == 0) {
		if (!h || y < top) return false;
		down_x = x;
		down_y = y;
		finger = id;
		return true;
	}
	if (id != finger) return true;
	if (phase == 1) { down_x = std::abs(x - down_x) >= 20 || std::abs(y - down_y) >= 20 ? -1000 : down_x; return true; }
	if (phase == 2 && down_x > -1000) {
		double fy = down_y - top;
		if (zmc.page == P_STATUS) {
			if (down_x < zmc.width / 3) { post_note.clear(); focus = 2; go_to(P_STATUS_POST, 1); zmc_keyboard(true); }
		} else if (zmc.page == P_CHATS) {
			if (down_x < zmc.width / 3) { new_text.clear(); zmc_list_lock(true); contacts_filter(); zmc_list_lock(false); load_contacts(); focus = 4; go_to(P_NEW, 1); zmc_keyboard(true); }
			else if (down_x < 2 * zmc.width / 3) { search_found.clear(); focus = 3; go_to(P_SEARCH, 1); zmc_keyboard(true); }
			else { logout_ask = false; go_to(P_ACCOUNT, 1); }
		} else if (fy >= box_h + 14 && down_x >= zmc.width - 54) {
			open_info();
		} else if (fy < box_h + 14) {
			focus = 0;
			zmc_keyboard(true);
			box_measure();
			footer_rev++;
			if (at_end()) zmc_scroll_end();
			zmc_redraw();
		} else if (down_x < 82) {
			send_draft();
			zmc_redraw();
		}
	}
	return true;
}

void typed(const char *text) {
	if (focus < 0) return;
	std::string &f = focus == 1 ? phone : focus == 2 ? post_text : focus == 3 ? search_text : focus == 4 ? new_text : draft;
	size_t cap = focus == 1 ? 24 : focus == 2 ? 700 : focus >= 3 ? 80 : MAX_DRAFT;
	if (text[0] == '\b') {
		while (!f.empty() && (static_cast<unsigned char>(f.back()) & 0xc0) == 0x80) f.pop_back();
		if (!f.empty()) f.pop_back();
	} else if (f.size() + strlen(text) <= cap) {
		f += text;
	}
	if (focus == 4) {
		zmc_list_lock(true);
		contacts_filter();
		zmc_list_lock(false);
	}
	if (focus == 0) {
		box_measure();
		footer_rev++;
		long now = time(nullptr);
		if (!draft.empty() && now - last_typing >= 5 && !open_id.empty()) {
			last_typing = now;
			request("typing", {{"chat", str(open_id)}, {"state", str("composing")}});
		}
	}
}

bool back() {
	if (zmc.page == P_IMAGE) { go_to(P_CHAT, -1); return true; }
	if (zmc.page == P_INFO) { go_to(P_CHAT, -1); return true; }
	if (zmc.page == P_STATUS_VIEW) { back_from_status(); return true; }
	if (zmc.page == P_STATUS_POST) { focus = -1; zmc_keyboard(false); go_to(P_STATUS, -1); return true; }
	if (zmc.page == P_SEARCH || zmc.page == P_NEW) { focus = -1; zmc_keyboard(false); go_to(P_CHATS, -1); return true; }
	if (zmc.page == P_CHAT) {
		if (focus == 0) {
			focus = -1;
			zmc_keyboard(false);
			box_measure();
			footer_rev++;
			zmc_redraw();
			return true;
		}
		if (!open_id.empty()) request("typing", {{"chat", str(open_id)}, {"state", str("paused")}});
		std::string was = open_id;
		open_id.clear();
		go_to(P_CHATS, -1);
		return true;
	}
	if (zmc.page == P_ACCOUNT) { go_to(P_CHATS, -1); return true; }
	return false;
}

/* the page has finished turning: from the shifted view of the end to the real one */
void arrived(int page) {
	if (page == P_CHAT && entry_off != 0) {
		zmc_list_lock(true);
		double to = entry_off;
		entry_off = 0;
		list_changed();
		zmc.scroll = to;
		zmc_list_lock(false);
		/* the core's limit is still the previous page's until a draw measures this
		 * one, and it held the view mid-list: ask for the end after that measure */
		zmc_scroll_end();
	}
	if (page == P_CHAT) mark_open_read();
	if (page == P_IMAGE || page == P_STATUS_VIEW) zmc_redraw();
	else image_wanted = false;
	if (page == P_CHATS || page == P_STATUS || page == P_STARRED || page == P_ARCHIVED) {
		zmc_pivot_changed();
		footer_rev++;	/* each tab has its own buttons */
	}
	focus_sync();
}

const struct zmc_app app = {
	.app_id = "ztwhatsapp", .title = "WhatsApp", .caps = "WHATSAPP",
	.render = render, .activate = activate, .back = back, .arrived = arrived,
	.text = typed, .overlay = overlay, .touch = touch,
};

/* ---- invented data, for looking without a phone ----------------------------- */

const char *const sample_lines[] = {
	"Are you coming today?", "Yes, around ten.",
	"Bring the blue folder if you can, the one from the top shelf, because we will need it for the meeting and nobody else knows where it went.",
	"ok", "Sounds good", "Line one\nLine two\nLine three", "Running late, sorry. The bus never came and I had to walk most of the way there.",
};

void sample_fill(int n) {
	const char *names[] = {"Alex Example", "Sam Sample", "Family", "Robin Test", "Jo Placeholder", "Book Club", "Kim Madeup"};
	long now = time(nullptr);
	for (int i = 0; i < 7; i++) {
		Chat c;
		c.id = "c" + std::to_string(i);
		c.name = names[i];
		c.last_text = sample_lines[i];
		c.last_ts = now - i * 40000;
		c.unread = i == 0 ? 2 : i == 2 ? 11 : 0;
		c.is_group = i == 2;
		c.last_from_me = i == 1;
		all_chats[c.id] = c;
	}
	all_chats["c6"].archived = true;
	chats_sort();
	{
		const char *who[] = {"Alex Example", "Robin Test", "Book Club"};
		long ago[] = {600, 7000, 30000};
		int unseen[] = {2, 0, 1};
		for (int i = 0; i < 3; i++) {
			StatusPerson p;
			p.sender = "s" + std::to_string(i);
			p.name = who[i];
			p.last_ts = now - ago[i];
			p.unseen = unseen[i];
			StatusItem it;
			it.id = "i" + std::to_string(i);
			it.text = "Invented status text, about nothing at all";
			it.kind = "text";
			it.ts = p.last_ts;
			p.items.push_back(it);
			statuses.push_back(p);
		}
		starred_list.push_back({"c1", "Sam Sample", "", sample_lines[2], now - 90000});
		starred_list.push_back({"c3", "Robin Test", "", sample_lines[0], now - 400000});
		contacts = {{"c1", "Sam Sample", "+61400000001"}, {"c3", "Robin Test", "+61400000003"}, {"n1", "Lee Invented", "+61400000011"}};
		contacts_shown = contacts;
		members = {{"Alex Example", true}, {"Sam Sample", false}, {"Robin Test", false}};
		info_topic = "An invented group, for looking at the page";
		/* a made-up picture, to see one in place */
		cairo_surface_t *pic = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 224, 224);
		cairo_t *pc = cairo_create(pic);
		cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, 224, 224);
		cairo_pattern_add_color_stop_rgb(g, 0, 0.9, 0.5, 0.2);
		cairo_pattern_add_color_stop_rgb(g, 1, 0.2, 0.3, 0.7);
		cairo_set_source(pc, g);
		dm_wrap_raw++;
		cairo_paint(pc);
		dm_wrap_raw--;
		cairo_pattern_destroy(g);
		cairo_destroy(pc);
		pictures["c1"] = pic;
		pictures["s1"] = pic;
	}
	open_id = "c0";
	open_name = names[0];
	presence_line = "online";
	for (int i = 0; i < n; i++) {
		auto m = std::make_unique<Msg>();
		m->id = "m" + std::to_string(i);
		m->from_me = i % 3 == 1;
		m->status = m->from_me ? (i % 12 == 1 ? "failed" : "read") : "received";
		m->kind = "text";
		m->ts = now - (n - i) * 3000;
		m->text = sample_lines[size_t(i) % 7];
		if (!m->from_me) m->sender_name = i % 2 ? "Sam Sample" : "Robin Test";
		if (i == n - 2) {
			m->kind = "image";
			m->is_image = true;
			m->media_w = 800;
			m->media_h = 600;
			m->text = "Caption of a photo";
			m->thumb_b64 = "sample";
			thumbs.push_back({m->id, pictures["c1"]});
		} else if (i % 17 == 5) {
			m->kind = "image";
			m->is_image = true;
			m->media_w = 800;
			m->media_h = 600;
			m->text = i % 2 ? "Caption of a photo" : "";
		}
		msgs.push_back(std::move(m));
	}
	place();
	entry_off = end_scroll();
}

double seconds() {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return double(t.tv_sec) + double(t.tv_nsec) / 1e9;
}

/* What a frame in a scroll cannot avoid: painting a strip as the core does. */
int bench(int n, const char *png) {
	zmc_config_style();
	zmc.width = 360;
	zmc.height = 740;
	zmc.scale = 4;
	zmc.accent = zmc_config_accent();
	sample_fill(0);
	double t0 = seconds();
	for (int i = 0; i < n; i++) {
		auto m = std::make_unique<Msg>();
		m->id = "m" + std::to_string(i);
		m->from_me = i % 3 == 1;
		m->status = "read";
		m->kind = "text";
		m->ts = time(nullptr) - (n - i) * 3000;
		m->text = sample_lines[size_t(i) % 7];
		if (i % 17 == 5) { m->is_image = true; m->media_w = 800; m->media_h = 600; }
		msgs.push_back(std::move(m));
	}
	place();
	double t1 = seconds();
	int rows = int(std::ceil(list_total / ROW_H));
	int w = zmc.width * zmc.scale, h = int(ROW_H) * zmc.scale;
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
	std::vector<double> took;
	entry_off = 0;
	int shown = std::min(rows, 400);
	for (int pass = 0; pass < 2; pass++) {
		for (int r = rows - shown; r < rows; r++) {
			double a = seconds();
			cairo_t *cr = cairo_create(s);
			cairo_set_source_rgb(cr, 0, 0, 0);
			cairo_paint(cr);
			cairo_scale(cr, zmc.scale, zmc.scale);
			dm_wrap_origin_y = r * ROW_H * zmc.scale;
			draw_strip(cr, r);
			dm_wrap_origin_y = 0;
			cairo_destroy(cr);
			cairo_surface_flush(s);
			if (pass == 1) took.push_back((seconds() - a) * 1e3);
		}
	}
	std::sort(took.begin(), took.end());
	printf("messages %d, strips %d, height %.0f pt\nmeasure and place: %.1f ms total, %.1f us each\n", n, rows, list_total, (t1 - t0) * 1e3, (t1 - t0) * 1e6 / std::max(n, 1));
	if (!took.empty())
		printf("strip paint, %zu samples at scale %d: median %.2f ms, p95 %.2f ms, max %.2f ms\n", took.size(), zmc.scale,
			took[took.size() / 2], took[took.size() * 95 / 100], took.back());
	if (png) {
		/* strips side by side make one column, as the core shows them: seams show */
		int count = std::min(rows, 10), from = std::max(0, rows - count - 40);
		cairo_surface_t *all = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h * count);
		cairo_t *ac = cairo_create(all);
		for (int i = 0; i < count; i++) {
			cairo_t *cr = cairo_create(s);
			cairo_set_source_rgb(cr, 0, 0, 0);
			cairo_paint(cr);
			cairo_scale(cr, zmc.scale, zmc.scale);
			dm_wrap_origin_y = (from + i) * ROW_H * zmc.scale;
			draw_strip(cr, from + i);
			dm_wrap_origin_y = 0;
			cairo_destroy(cr);
			cairo_surface_flush(s);
			dm_wrap_raw++;
			cairo_set_source_surface(ac, s, 0, i * h);
			cairo_paint(ac);
			dm_wrap_raw--;
		}
		cairo_destroy(ac);
		cairo_surface_write_to_png(all, png);
		cairo_surface_destroy(all);
	}
	cairo_surface_destroy(s);
	return 0;
}

/* The socket with no window: everything up to the drawing, against an engine
 * (fake_engine.py or ztwhatsappd --fake). Prints counts only, never content. */
void pump(double secs) {
	double end = seconds() + secs;
	while (seconds() < end && sock_fd >= 0) {
		struct pollfd p{sock_fd, POLLIN, 0};
		if (poll(&p, 1, 50) > 0) sock_ready(sock_fd, nullptr);
	}
}

int selftest() {
	zmc.width = 360; zmc.height = 740; zmc.scale = 4;
	zmc.app = &app;
	zmc.page = P_CHATS;
	if (!connect_daemon()) { puts("FAIL connect"); return 1; }
	pump(0.5);
	printf("connection known: %d, pairing known: %d, chats %zu\n", connection == "connected", pair_state != "connecting", chats.size());
	if (chats.empty()) { puts("FAIL no chats"); return 1; }
	show_chat(chats[0].id);
	pump(0.5);
	size_t first = msgs.size();
	double height = list_total, was_scroll = 500;
	zmc.scroll = was_scroll;
	printf("first page %zu messages, more %d, height %.0f, page %d\n", first, older_more, height, zmc.page);
	load_older();
	pump(0.5);
	if (older_held) older_apply();
	printf("after older %zu messages, scroll moved by %.0f, height grew by %.0f\n", msgs.size(), zmc.scroll - was_scroll, list_total - height);
	bool sorted = true;
	for (size_t i = 1; i < msgs.size(); i++) sorted = sorted && msgs[i - 1]->ts <= msgs[i]->ts;
	draft = "self test";
	size_t before = msgs.size();
	send_draft();
	pump(0.5);
	printf("sent: %zu new, status %s, box empty %d, oldest first %d\n", msgs.size() - before, msgs.empty() ? "" : msgs.back()->status.c_str(), draft.empty(), sorted);
	bool core_ok = msgs.size() == before + 1 && msgs.size() > first && sorted && msgs.back()->status != "pending";
	/* a thumbnail as the engine sends it, decoded */
	int thumbs_seen = 0, thumbs_ok = 0;
	for (auto &m : msgs) if (m->is_image && !m->thumb_b64.empty()) { thumbs_seen++; thumbs_ok += thumb_of(*m) != nullptr; }
	printf("thumbnails %d, decoded %d\n", thumbs_seen, thumbs_ok);
	if (!thumbs_seen || thumbs_ok != thumbs_seen) return 1;
	/* the notifier's two files */
	zmc.active = true;
	zmc.page = P_CHAT;
	focus_sync();
	char line[300] = "";
	if (FILE *f = fopen(run_file("ztwhatsapp.focus").c_str(), "r")) { (void)!fgets(line, sizeof(line), f); fclose(f); }
	int pid = 0;
	sscanf(line, "%d", &pid);
	bool focus_ok = pid == getpid() && strstr(line, open_id.c_str());
	zmc.active = false;
	focus_sync();
	bool focus_gone = access(run_file("ztwhatsapp.focus").c_str(), F_OK) != 0;
	zmc.active = true;
	if (FILE *f = fopen(run_file("ztwhatsapp.open").c_str(), "w")) { fprintf(f, "%s\n", chats[1].id.c_str()); fclose(f); }
	zmc.page = P_CHATS;
	open_file_check();
	pump(0.3);
	bool open_ok = access(run_file("ztwhatsapp.open").c_str(), F_OK) != 0 && open_id == chats[1].id;
	printf("focus written %d, removed when hidden %d, open file taken and chat opened %d\n", focus_ok, focus_gone, open_ok);
	if (!focus_ok || !focus_gone || !open_ok) return 1;
	zmc.active = false;
	statuses.clear(); starred_list.clear();
	load_extras();
	load_contacts();
	pump(0.5);
	printf("statuses %zu, starred %zu, contacts %zu, archived %zu\n", statuses.size(), starred_list.size(), contacts.size(), archived.size());
	if (!statuses.empty()) {
		status_person = 0; status_item = 0;
		int was = statuses[0].unseen;
		status_show();
		pump(0.3);
		printf("status seen: unseen %d to %d\n", was, statuses[0].unseen);
	}
	picture_of(chats[0].id);
	pictures_next();
	pump(0.5);
	printf("picture fetched: %d\n", pictures[chats[0].id] != nullptr);
	bool extra = !statuses.empty() && !contacts.empty() && !starred_list.empty();
	return extra && core_ok ? 0 : 1;
}

int render_page(const char *name, const char *file) {
	sample = true;
	zmc_config_style();
	zmc.height = 740;
	open_group = !strcmp(name, "chat");
	sample_fill(80);
	connection = "connected";
	int page = P_CHATS;
	std::string n = name;
	if (n == "link") { page = P_LINK; pair_state = "unpaired"; }
	else if (n == "qr") { page = P_LINK; pair_state = "qr"; qr_set("2@InventedPairingStringForLookingAtTheLayoutOnly0123456789abcdefghijklmnopqrstuvwxyz,0123456789abcdefghijklmnopqrstuvwxyz0123456789,abcdefghijklmnopqrstuvwxyz0123456789"); }
	else if (n == "code") { page = P_LINK; pair_state = "phone_code"; pair_code = "ABCD-EFGH"; }
	else if (n == "chat") { page = P_CHAT; open_group = true; }
	else if (n == "account") page = P_ACCOUNT;
	else if (n == "status") page = P_STATUS;
	else if (n == "starred") page = P_STARRED;
	else if (n == "archived") page = P_ARCHIVED;
	else if (n == "view") { page = P_STATUS_VIEW; status_person = 0; }
	else if (n == "post") { page = P_STATUS_POST; focus = 2; post_text = "Invented status"; }
	else if (n == "search") { page = P_SEARCH; search_text = "a"; }
	else if (n == "new") page = P_NEW;
	else if (n == "info") { page = P_INFO; open_group = true; open_name = "Family"; }

	return zmc_render_png(&app, page, 740, file);
}

}	// namespace

int main(int argc, char **argv) {
	signal(SIGPIPE, SIG_IGN);
	if (argc >= 3 && !strcmp(argv[1], "--bench")) return bench(atoi(argv[2]), argc > 3 ? argv[3] : nullptr);
	if (argc == 2 && !strcmp(argv[1], "--selftest")) return selftest();
	if (argc == 4 && !strcmp(argv[1], "--render")) return render_page(argv[2], argv[3]);
	if (pipe2(older_pipe, O_NONBLOCK | O_CLOEXEC) == 0) zmc_watch_fd(older_pipe[0], older_wake, nullptr);
	if (pipe2(image_pipe, O_CLOEXEC) == 0) zmc_watch_fd(image_pipe[0], image_ready, nullptr);
	if (!connect_daemon()) {
		start_daemon();
		zmc_every(500, tick);
	}
	if (argc == 2 && argv[1][0] != '-') pending_open = argv[1];
	open_file_check();
	{
		int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
		if (fd >= 0 && inotify_add_watch(fd, run_file("x").substr(0, run_file("x").size() - 2).c_str(), IN_CLOSE_WRITE | IN_MOVED_TO) >= 0)
			zmc_watch_fd(fd, run_dir_event, nullptr);
	}
	prefs_load();
	atexit(focus_clear);
	return zmc_run(&app, P_CHATS);
}
