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
 *					(link qr code chats chat account)
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

enum Page { P_LINK, P_CHATS, P_CHAT, P_ACCOUNT, P_IMAGE };
enum Action { ACT_OPEN = 1, ACT_PAIR_QR, ACT_PAIR_PHONE, ACT_PHONE_BOX, ACT_CANCEL, ACT_IMAGE, ACT_RETRY, ACT_LOGOUT, ACT_LOGOUT_SURE };

/* ---- what is shown ------------------------------------------------------- */

struct Chat {
	std::string id, name, last_text;
	bool is_group = false, last_from_me = false, muted = false, pinned = false, archived = false;
	int unread = 0;
	long last_ts = 0;
};

struct Msg {
	std::string id, sender_name, text, kind, status, thumb_b64, path;
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

std::string draft, phone;
int focus = -1;			/* 0 the message, 1 the phone number */
double box_h = 44;
unsigned footer_rev = 1;
bool logout_ask, foreground;
long last_typing;

cairo_surface_t *full_image;
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
	else if (now - ts < 6 * 86400) strftime(out, sizeof(out), "%a", &a);
	else strftime(out, sizeof(out), "%d/%m/%y", &a);
	for (char *c = out; *c; c++) *c = char(tolower((unsigned char)*c));
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
	std::string s = when_text(m.ts);
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
	if (page == P_CHATS) return 76;
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

void list_changed() { list_rev += 2; }

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

/* ---- the chat list -------------------------------------------------------- */

void draw_chat_row(cairo_t *cr, int row) {
	if (row < 0 || size_t(row) >= chats.size()) return;
	const Chat &c = chats[size_t(row)];
	const double tile = 56, left = MARGIN + tile + 12, y = 6;
	bool unread = c.unread > 0;
	/* who: a square with their initial, as Messaging has */
	std::string initial = "#";
	if (!c.name.empty()) {
		const char *next = g_utf8_next_char(c.name.c_str());
		gchar *up = g_utf8_strup(c.name.c_str(), next - c.name.c_str());
		initial = up;
		g_free(up);
	}
	zmc_accent(cr);
	cairo_rectangle(cr, MARGIN, y, tile, tile);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text_centred(cr, initial.c_str(), FONT_SEMILIGHT, 26, MARGIN + tile / 2, y + 12);
	cairo_new_path(cr);

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

bool chat_row_hit(int row, double, double, struct zmc_hit *hit) {
	if (row < 0 || size_t(row) >= chats.size()) return false;
	*hit = zmc_hit{.x = 0, .y = 2, .w = double(zmc.width), .h = 66, .action = ACT_OPEN, .arg = row, .slider = false, .button = false};
	return true;
}

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
	for (auto &[id, c] : all_chats) if (!c.archived) chats.push_back(c);
	std::stable_sort(chats.begin(), chats.end(), [](const Chat &a, const Chat &b) {
		if (a.pinned != b.pinned) return a.pinned;
		return a.last_ts > b.last_ts;
	});
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
void glide_step() {
	double t = std::min(1.0, (zmc_now_ms() - glide_start) / double(STEP_MS));
	double e = 1 - (1 - t) * (1 - t) * (1 - t);
	if (zmc.touching) { zmc_on_frame(nullptr); return; }
	zmc.scroll = glide_from + (glide_to - glide_from) * e;
	if (t >= 1) zmc_on_frame(nullptr);
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
	zmc_on_frame(glide_step);
}

void mark_open_read() {
	if (open_id.empty() || zmc.page != P_CHAT || !zmc.active) return;
	auto it = all_chats.find(open_id);
	if (it == all_chats.end() || it->second.unread == 0) return;
	request("mark_read", {{"chat", str(open_id)}});
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
	});
}

void go_to(int page, int dir) {
	zmc_list_lock(true);
	list_changed();
	zmc_list_lock(false);
	zmc_go(page, dir);
}

void show_chat(const std::string &id) {
	static bool opening;
	if (opening) return;	/* a second tap while the first is on its way */
	opening = true;
	auto it = all_chats.find(id);
	open_id = id;
	open_name = it != all_chats.end() ? it->second.name : "";
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
	if (older_pending || !older_more || open_id.empty() || msgs.empty()) return;
	older_pending = true;
	const Msg &oldest = *msgs.front();
	std::string id = open_id;
	request("messages", {{"chat", str(id)}, {"limit", json_object_new_int(PAGE_SIZE)},
			{"before_ts", json_object_new_int64(oldest.ts)}, {"before_id", str(oldest.id)}},
			[id](json_object *r, const char *err) {
		older_pending = false;
		if (id != open_id) return;
		json_object *list = err ? nullptr : jget(r, "messages");
		if (!list) return;
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
		batch_page = true;
	});
}

void older_wake(int fd, void *) {
	char drain[16];
	while (read(fd, drain, sizeof(drain)) > 0) {}
	older_asked = false;
	load_older();
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
		if (connection == "connected" && was != "connected") load_chats();
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
	} else if (ev == "history_sync") {
		if (jstr(o, "state") == "done") load_chats();
	}
	/* call_offer and call_end: calls are not part of this app */
}

void disconnect_daemon();
void tick();

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

double draw_chats(cairo_t *cr) {
	double y = zmc_header(cr, "chats");
	if (connection != "connected") {
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		const char *s = connection == "offline" ? "offline, waiting to reconnect" : "connecting...";
		zmc_text(cr, s, FONT_REGULAR, 12.5, MARGIN, y - 18, 0);
		cairo_new_path(cr);
	}
	if (chats.empty()) return centred_note(cr, "No chats yet.", y);
	struct zmc_list l{};
	l.count = int(chats.size());
	l.row_h = ROW_H;
	l.revision = list_rev;
	l.draw_row = draw_chat_row;
	l.row_hit = chat_row_hit;
	return zmc_list(cr, y, &l) + 76 + 12;
}

double draw_chat(cairo_t *cr) {
	double y = zmc_header(cr, "");
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text_line(cr, open_name.c_str(), FONT_SEMILIGHT, 28, MARGIN, presence_line.empty() ? 46 : 38, zmc.width - 2 * MARGIN);
	cairo_new_path(cr);
	if (!presence_line.empty()) {
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		zmc_text_line(cr, presence_line.c_str(), FONT_REGULAR, 13, MARGIN, 76, zmc.width - 2 * MARGIN);
		cairo_new_path(cr);
	}
	struct zmc_list l{};
	l.count = std::max(1, int(std::ceil(list_total / ROW_H)));
	l.row_h = ROW_H;
	l.revision = list_rev + 1;
	l.draw_row = draw_strip;
	l.row_hit = strip_hit;
	return zmc_list(cr, y, &l) + footer_height(P_CHAT) + 12;
}

double draw_account(cairo_t *cr) {
	double y = zmc_header(cr, "account");
	y = zmc_field(cr, y, "connection", connection.c_str());
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
void draw_chats_footer(cairo_t *cr);

double render(cairo_t *cr, int page) {
	switch (page) {
	case P_LINK: return draw_link(cr);
	case P_CHATS:
	case P_CHAT: {
		double y = page == P_CHATS ? draw_chats(cr) : draw_chat(cr);
		if (sample) {	/* a PNG has no overlay: the footer in its place */
			cairo_save(cr);
			cairo_translate(cr, 0, zmc.height - footer_height(page));
			if (page == P_CHAT) draw_compose(cr); else draw_chats_footer(cr);
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
	double h = dm_text_height(box_shown(), BOX_FONT, zmc.width - 2 * MARGIN - 20) + 22;
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
}

void draw_chats_footer(cairo_t *cr) {
	cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
	cairo_rectangle(cr, 0, 0, zmc.width, 76);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_round_button(cr, 62, 8, "people", "account", 0, 0);
	cairo_new_path(cr);
}

void overlay(int page) {
	if (!foreground && zmc.active && sock_fd >= 0) {
		foreground = true;
		request("set_presence", {{"available", json_object_new_boolean(true)}});
		mark_open_read();
	}
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
		if (page == P_CHAT) draw_compose(cr); else draw_chats_footer(cr);
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
	if (zmc.page != P_IMAGE) { if (s) cairo_surface_destroy(s); return; }
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
		request("download_media", {{"chat", str(open_id)}, {"id", str(id)}}, [id](json_object *r, const char *err) {
			std::string path = err ? "" : jstr(r, "path");
			if (path.empty()) {
				image_note = "Could not download this photo.";
			} else {
				for (auto &x : msgs) if (x->id == id) x->path = path;
				image_load(path);
			}
			if (zmc.page == P_IMAGE) zmc_redraw();
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

void activate(int action, int arg) {
	switch (action) {
	case ACT_OPEN:
		if (arg >= 0 && size_t(arg) < chats.size()) show_chat(chats[size_t(arg)].id);
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
		if (zmc.page == P_CHATS) {
			if (down_x < zmc.width / 2) { logout_ask = false; go_to(P_ACCOUNT, 1); }
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
	std::string &f = focus == 1 ? phone : draft;
	size_t cap = focus == 1 ? 24 : MAX_DRAFT;
	if (text[0] == '\b') {
		while (!f.empty() && (static_cast<unsigned char>(f.back()) & 0xc0) == 0x80) f.pop_back();
		if (!f.empty()) f.pop_back();
	} else if (f.size() + strlen(text) <= cap) {
		f += text;
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
		zmc_redraw();
	}
	if (page == P_CHAT) mark_open_read();
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
	chats_sort();
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
		if (i % 17 == 5) {
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
	printf("after older %zu messages, scroll moved by %.0f, height grew by %.0f\n", msgs.size(), zmc.scroll - was_scroll, list_total - height);
	bool sorted = true;
	for (size_t i = 1; i < msgs.size(); i++) sorted = sorted && msgs[i - 1]->ts <= msgs[i]->ts;
	draft = "self test";
	size_t before = msgs.size();
	send_draft();
	pump(0.5);
	printf("sent: %zu new, status %s, box empty %d, oldest first %d\n", msgs.size() - before, msgs.empty() ? "" : msgs.back()->status.c_str(), draft.empty(), sorted);
	return msgs.size() == before + 1 && msgs.size() > first && sorted && msgs.back()->status != "pending" ? 0 : 1;
}

int render_page(const char *name, const char *file) {
	sample = true;
	zmc_config_style();
	zmc.height = 740;
	sample_fill(80);
	connection = "connected";
	int page = P_CHATS;
	std::string n = name;
	if (n == "link") { page = P_LINK; pair_state = "unpaired"; }
	else if (n == "qr") { page = P_LINK; pair_state = "qr"; qr_set("2@InventedPairingStringForLookingAtTheLayoutOnly0123456789abcdefghijklmnopqrstuvwxyz,0123456789abcdefghijklmnopqrstuvwxyz0123456789,abcdefghijklmnopqrstuvwxyz0123456789"); }
	else if (n == "code") { page = P_LINK; pair_state = "phone_code"; pair_code = "ABCD-EFGH"; }
	else if (n == "chat") page = P_CHAT;
	else if (n == "account") page = P_ACCOUNT;
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
	if (argc == 2 && argv[1][0] != '-') {
		/* from a notification: once the list has it, or straight away */
		std::string id = argv[1];
		request("chats", {{"limit", json_object_new_int(500)}}, [id](json_object *r, const char *err) {
			json_object *list = err ? nullptr : jget(r, "chats");
			zmc_list_lock(true);
			for (size_t i = 0; list && i < json_object_array_length(list); i++) chat_from(json_object_array_get_idx(list, i));
			chats_sort();
			zmc_list_lock(false);
			show_chat(id);
		});
	}
	return zmc_run(&app, P_CHATS);
}
