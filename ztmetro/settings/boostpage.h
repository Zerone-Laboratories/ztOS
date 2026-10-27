/* Settings > Dynamic Boost System: the two halves of zrnperfd's boost.
 *
 * RapidBoost is the ladder (idle0 .. rapid) that steps up on load; its time on
 * each rung and its steps come from /run/zrnperfd.ib. IntelligentBoost is the
 * network that predicts the rung the ladder will reach in two seconds and lifts
 * it early (perfd/ibnet.c): its counters, and its last forward pass, drawn as a
 * multilayer perceptron, input layer to hidden layer to output layer, every
 * connection drawn. The connections are the model's own weights
 * (/var/lib/zrnperfd/ib/model.bin), each as strong as the signal it carried in
 * the last pass; a neuron glows as it fires (a ReLU above zero), and with every
 * new sample (four a second) pulses run along the strongest paths to the rung
 * that won. Included by ztsettings.c, after picker() and perf_tell().
 *
 * What it costs: the page itself (text, bars) is drawn on the processor only
 * when its numbers change, at most every 2 s; the network is drawn into its own
 * surface once per sample; and every frame the GPU puts that surface over the
 * page and moves the pulses (ib_overlay). Redrawing the whole page fifteen times
 * a second on the processor, as the first version did, crawled.
 */
#include <math.h>
#include <sys/stat.h>

#define IB_FILE		"/run/zrnperfd.ib"
#define IB_MODEL	"/var/lib/zrnperfd/ib/model.bin"
#define IB_IN		12
#define IB_HID_MAX	64
#define IB_OUT		5
#define IB_FRAME_MS	33	/* the pulses, on the GPU, while the page is open */
#define IB_TEXT_MS	2000	/* the page's numbers: at most this often */
#define IB_NET_MS	1000	/* the network's picture: at most this often (samples come 4 a second) */
#define IB_FAINT	0.06f	/* a connection weaker than this (of the strongest) is not drawn */

static const char *const ib_rungs[IB_OUT] = { "idle0", "nominal", "interactive", "big", "rapid" };
static const char *const ib_inputs[IB_IN] = {
	"front app load", "shell load", "little load", "big load", "touching", "Android app",
	"charging", "front, 1 s", "shell, 1 s", "big, 1 s", "front, 2 s peak", "front, rising",
};

static struct {
	/* from /run/zrnperfd.ib */
	char mode[16], engine[16], prefer[16];
	int ladder, rung, pick;
	long rung_ms[IB_OUT];
	unsigned long ups, downs, lifts, predictions, on_score, on_cpu, checks, check_failures;
	unsigned latency_us, latency_avg_us;
	float x[IB_IN], h[IB_HID_MAX], o[IB_OUT];
	int n_hid;
	bool live;			/* the file is there */
	struct timespec stamp;		/* its mtime: a new sample when it moves */
	uint32_t sample_at;		/* when the page saw it */
	/* shown: the last pass, the winner's softmax */
	float sx[IB_IN], sh[IB_HID_MAX], sp[IB_OUT];
	/* the network's own surface (pixels), drawn once per sample, on the GPU every frame */
	cairo_surface_t *net, *dot;
	unsigned net_rev;
	bool net_dirty;
	double net_y, net_h;		/* where on the page (points) */
	double x0, x1, x2, yi[IB_IN], yh[IB_HID_MAX], yo[IB_OUT];	/* from the network's top */
	int top1[12], t1, top2[8], t2, win;	/* the pulses' paths */
	uint32_t text_at;		/* when the page's numbers were last drawn */
	uint32_t net_at;		/* when the network's picture was last drawn */
	/* the model's weights, read once (again if the file changes) */
	float w1[IB_IN * IB_HID_MAX], w2[IB_HID_MAX * IB_OUT];
	int m_hid;
	struct timespec model_stamp;
	double frozen_phase;		/* --render: the pulses where they are; < 0 live */
} ib = { .frozen_phase = -1 };

static void ib_load_model(void) {
	struct stat st;
	if (stat(IB_MODEL, &st) != 0) {
		ib.m_hid = 0;
		return;
	}
	if (st.st_mtim.tv_sec == ib.model_stamp.tv_sec && st.st_mtim.tv_nsec == ib.model_stamp.tv_nsec)
		return;
	ib.model_stamp = st.st_mtim;
	ib.m_hid = 0;
	FILE *f = fopen(IB_MODEL, "rb");
	if (!f) return;
	char magic[4];
	uint32_t dims[3];
	if (fread(magic, 4, 1, f) == 1 && !memcmp(magic, "ZIB1", 4) && fread(dims, sizeof(dims), 1, f) == 1 &&
			dims[0] == IB_IN && dims[1] > 0 && dims[1] <= IB_HID_MAX && dims[2] == IB_OUT) {
		int n = dims[1];
		int32_t q[IB_IN * IB_HID_MAX];
		fseek(f, 16 + 2 * IB_IN * 4, SEEK_SET);	/* past the inputs' mean and scale */
		bool ok = fread(q, 4, IB_IN * n, f) == (size_t)(IB_IN * n);
		for (int i = 0; ok && i < IB_IN * n; i++) ib.w1[i] = q[i] / 65536.0f;
		fseek(f, n * 4, SEEK_CUR);			/* past b1 */
		ok = ok && fread(q, 4, n * IB_OUT, f) == (size_t)(n * IB_OUT);
		for (int i = 0; ok && i < n * IB_OUT; i++) ib.w2[i] = q[i] / 65536.0f;
		if (ok) ib.m_hid = n;
	}
	fclose(f);
}

static void ib_read_floats(const char *s, float *out, int max, int *n) {
	int k = 0;
	char *end;
	while (k < max) {
		float v = strtof(s, &end);
		if (end == s) break;
		out[k++] = v;
		s = end;
	}
	if (n) *n = k;
}

/* the file, when it has moved on; true for a new sample */
static bool ib_load(void) {
	struct stat st;
	ib.live = stat(IB_FILE, &st) == 0;
	if (!ib.live) return false;
	if (st.st_mtim.tv_sec == ib.stamp.tv_sec && st.st_mtim.tv_nsec == ib.stamp.tv_nsec) return false;
	ib.stamp = st.st_mtim;
	FILE *f = fopen(IB_FILE, "r");
	char line[1024];
	while (f && fgets(line, sizeof(line), f)) {
		char *v = strchr(line, ' ');
		if (!v) continue;
		*v++ = 0;
		v[strcspn(v, "\n")] = 0;
		if (!strcmp(line, "mode")) snprintf(ib.mode, sizeof(ib.mode), "%s", v);
		else if (!strcmp(line, "engine")) snprintf(ib.engine, sizeof(ib.engine), "%s", v);
		else if (!strcmp(line, "prefer")) snprintf(ib.prefer, sizeof(ib.prefer), "%s", v);
		else if (!strcmp(line, "ladder")) ib.ladder = atoi(v);
		else if (!strcmp(line, "rung")) ib.rung = atoi(v);
		else if (!strcmp(line, "pick")) ib.pick = atoi(v);
		else if (!strcmp(line, "rung_ms")) sscanf(v, "%ld %ld %ld %ld %ld", &ib.rung_ms[0], &ib.rung_ms[1],
			&ib.rung_ms[2], &ib.rung_ms[3], &ib.rung_ms[4]);
		else if (!strcmp(line, "ups")) ib.ups = strtoul(v, NULL, 10);
		else if (!strcmp(line, "downs")) ib.downs = strtoul(v, NULL, 10);
		else if (!strcmp(line, "lifts")) ib.lifts = strtoul(v, NULL, 10);
		else if (!strcmp(line, "predictions")) ib.predictions = strtoul(v, NULL, 10);
		else if (!strcmp(line, "score")) ib.on_score = strtoul(v, NULL, 10);
		else if (!strcmp(line, "cpu")) ib.on_cpu = strtoul(v, NULL, 10);
		else if (!strcmp(line, "checks")) ib.checks = strtoul(v, NULL, 10);
		else if (!strcmp(line, "check_failures")) ib.check_failures = strtoul(v, NULL, 10);
		else if (!strcmp(line, "latency_us")) ib.latency_us = atoi(v);
		else if (!strcmp(line, "latency_avg_us")) ib.latency_avg_us = atoi(v);
		else if (!strcmp(line, "x")) ib_read_floats(v, ib.x, IB_IN, NULL);
		else if (!strcmp(line, "h")) ib_read_floats(v, ib.h, IB_HID_MAX, &ib.n_hid);
		else if (!strcmp(line, "o")) ib_read_floats(v, ib.o, IB_OUT, NULL);
	}
	if (f) fclose(f);
	ib.sample_at = zmc_now_ms();
	return true;
}

/* what is shown: the last pass as it is, its outputs as a softmax */
static void ib_take(void) {
	float omax = ib.o[0];
	for (int k = 1; k < IB_OUT; k++) if (ib.o[k] > omax) omax = ib.o[k];
	float sum = 0, p[IB_OUT];
	for (int k = 0; k < IB_OUT; k++) sum += p[k] = expf(ib.o[k] - omax);
	for (int k = 0; k < IB_OUT; k++) ib.sp[k] = p[k] / sum;
	for (int i = 0; i < IB_IN; i++) ib.sx[i] = ib.x[i];
	for (int j = 0; j < ib.n_hid; j++) ib.sh[j] = ib.h[j];
	ib.net_dirty = true;
}

/* every frame while the page is open: a new sample redraws the network's
 * surface (not the page); the page's numbers at most every IB_TEXT_MS; the
 * pulses move on the GPU (zmc_present: no drawing on the processor) */
static void ib_tick(void) {
	uint32_t now = zmc_now_ms();
	ib_load_model();
	if (ib_load()) ib_take();
	if (now - ib.text_at >= IB_TEXT_MS) {
		ib.text_at = now;
		zmc_redraw();
	} else {
		zmc_present();
	}
}

/* --render: the last pass as it is, caught with its pulses half way */
static void ib_render_prepare(void) {
	ib_load();
	ib_load_model();
	ib_take();
	ib.frozen_phase = 0.32;
}

static void ib_rgba(cairo_t *cr, uint32_t rgb, double a) {
	cairo_set_source_rgba(cr, ((rgb >> 16) & 255) / 255.0, ((rgb >> 8) & 255) / 255.0, (rgb & 255) / 255.0, a);
}

#define IB_NEGATIVE	0x8FA3B8	/* a cool grey, against any accent: a connection that holds a neuron back */

/* a neuron: a dark disc, filled with the accent as far as it fires, a halo when it does */
static void ib_neuron(cairo_t *cr, double x, double y, double r, double level, bool lit) {
	level = level < 0 ? 0 : level > 1 ? 1 : level;
	if (level > 0.02) {	/* the halo: two flat discs (a gradient per neuron cost too much) */
		ib_rgba(cr, zmc.accent, 0.12 * level);
		cairo_arc(cr, x, y, r * (2.2 + 1.2 * level), 0, 2 * M_PI);
		cairo_fill(cr);
		ib_rgba(cr, zmc.accent, 0.2 * level);
		cairo_arc(cr, x, y, r * 1.5, 0, 2 * M_PI);
		cairo_fill(cr);
	}
	cairo_set_source_rgb(cr, 0.13, 0.13, 0.13);
	cairo_arc(cr, x, y, r, 0, 2 * M_PI);
	cairo_fill(cr);
	ib_rgba(cr, zmc.accent, 0.15 + 0.85 * level);
	cairo_arc(cr, x, y, r * (0.25 + 0.75 * level), 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_set_line_width(cr, lit ? 2.2 : 1);
	if (lit) zmc_accent(cr);
	else cairo_set_source_rgba(cr, 1, 1, 1, 0.55);
	cairo_arc(cr, x, y, r, 0, 2 * M_PI);
	cairo_stroke(cr);
}

/* the strongest `n` of `count` flows (by absolute size), their indices in order */
static int ib_top(const float *flow, int count, int n, int *out) {
	int k = 0;
	for (int i = 0; i < count; i++) {
		float a = fabsf(flow[i]);
		if (a < 1e-4f || (k == n && a <= fabsf(flow[out[n - 1]])))
			continue;
		int at = k < n ? k++ : n - 1;
		while (at > 0 && fabsf(flow[out[at - 1]]) < a) {
			out[at] = out[at - 1];
			at--;
		}
		out[at] = i;
	}
	return k;
}

/* a pulse: a bright dot `t` of the way from one neuron to the next, with a
 * tail. On the GPU (a small white disc, scaled), or into `cr` for --render. */
static void ib_dot(cairo_t *cr, double x, double y, double r, double a) {
	if (cr) {
		ib_rgba(cr, 0xffffff, a);
		cairo_arc(cr, x, y, r, 0, 2 * M_PI);
		cairo_fill(cr);
		return;
	}
	if (!ib.dot) {
		int px = 32;
		ib.dot = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
		cairo_t *d = cairo_create(ib.dot);
		cairo_pattern_t *g = cairo_pattern_create_radial(px / 2.0, px / 2.0, 0, px / 2.0, px / 2.0, px / 2.0);
		cairo_pattern_add_color_stop_rgba(g, 0, 1, 1, 1, 1);
		cairo_pattern_add_color_stop_rgba(g, 0.55, 1, 1, 1, 1);
		cairo_pattern_add_color_stop_rgba(g, 1, 1, 1, 1, 0);
		cairo_set_source(d, g);
		cairo_paint(d);
		cairo_pattern_destroy(g);
		cairo_destroy(d);
		cairo_surface_flush(ib.dot);
	}
	double c[4][3] = { { x - r, y - r, 0 }, { x + r, y - r, 0 }, { x - r, y + r, 0 }, { x + r, y + r, 0 } };
	zmc_gpu_quad(ib.dot, 1, c, a);
}

static void ib_pulse(cairo_t *cr, double x0, double y0, double x1, double y1, double t) {
	if (t <= 0 || t >= 1) return;
	for (int s = 0; s < 4; s++) {
		double u = t - s * 0.04;
		if (u <= 0) break;
		ib_dot(cr, x0 + (x1 - x0) * u, y0 + (y1 - y0) * u, 2.8 - s * 0.5, 0.9 - s * 0.22);
	}
}

/* the pulses of the last sample, `oy` the network's top on screen: in the first
 * half to the hidden neurons it fired, in the second from them to the winner */
static void ib_pulses(cairo_t *cr, double oy) {
	int n = ib.n_hid < ib.m_hid ? ib.n_hid : ib.m_hid;
	if (n <= 0) return;
	double phase = ib.frozen_phase >= 0 ? ib.frozen_phase : (zmc_now_ms() - ib.sample_at) / 500.0;
	for (int s = 0; s < ib.t1; s++)
		ib_pulse(cr, ib.x0, oy + ib.yi[ib.top1[s] / n], ib.x1, oy + ib.yh[ib.top1[s] % n], phase * 2);
	for (int s = 0; s < ib.t2; s++)
		ib_pulse(cr, ib.x1, oy + ib.yh[ib.top2[s]], ib.x2, oy + ib.yo[ib.win], phase * 2 - 1);
}

/* how tall the network is, and where everything in it goes (from its top) */
static double ib_layout(void) {
	int n = ib.n_hid < ib.m_hid ? ib.n_hid : ib.m_hid;
	double w = zmc.width - 2 * MARGIN, top = 34, height = 15.0 * (n > 0 ? n : 24);
	ib.x0 = MARGIN + 96, ib.x2 = MARGIN + w - 74, ib.x1 = (ib.x0 + ib.x2) / 2;
	for (int i = 0; i < IB_IN; i++) ib.yi[i] = top + height * (i + 0.5) / IB_IN;
	for (int j = 0; j < n; j++) ib.yh[j] = top + height * (j + 0.5) / n;
	for (int k = 0; k < IB_OUT; k++) ib.yo[k] = top + height * (k + 0.5) / IB_OUT;
	return n > 0 ? top + height + 40 : 200;
}

/* the network as of the last pass, its top at y = 0 of `cr`: every connection,
 * the neurons, the labels. The pulses are not here (ib_pulses). */
static void ib_network(cairo_t *cr) {
	int n = ib.n_hid < ib.m_hid ? ib.n_hid : ib.m_hid;
	double w = zmc.width - 2 * MARGIN, x0 = ib.x0, x1 = ib.x1, x2 = ib.x2;
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text_centred(cr, "input layer", FONT_REGULAR, 12, x0, 6);
	zmc_text_centred(cr, "hidden layer", FONT_REGULAR, 12, x1, 6);
	zmc_text_centred(cr, "output layer", FONT_REGULAR, 12, x2, 6);
	ib.t1 = ib.t2 = 0;
	if (n <= 0) {
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text_centred(cr, ib.live ? "no model yet" : "zrnperfd is not publishing", FONT_SEMILIGHT, 17,
			x1, 110);
		return;
	}

	/* every connection, as strong as what it carried: input * weight, hidden * weight */
	float f1[IB_IN * IB_HID_MAX], f2[IB_HID_MAX * IB_OUT], m1 = 1e-6f, m2 = 1e-6f;
	for (int i = 0; i < IB_IN; i++)
		for (int j = 0; j < n; j++) {
			f1[i * n + j] = ib.sx[i] * ib.w1[i * n + j];
			if (fabsf(f1[i * n + j]) > m1) m1 = fabsf(f1[i * n + j]);
		}
	for (int j = 0; j < n; j++)
		for (int k = 0; k < IB_OUT; k++) {
			f2[j * IB_OUT + k] = ib.sh[j] * ib.w2[j * IB_OUT + k];
			if (fabsf(f2[j * IB_OUT + k]) > m2) m2 = fabsf(f2[j * IB_OUT + k]);
		}
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	for (int i = 0; i < IB_IN; i++)
		for (int j = 0; j < n; j++) {
			float v = f1[i * n + j] / m1;
			if (fabsf(v) < IB_FAINT) continue;
			ib_rgba(cr, v >= 0 ? zmc.accent : IB_NEGATIVE, 0.05 + 0.6 * fabsf(v));
			cairo_set_line_width(cr, 0.5 + 1.6 * fabsf(v));
			cairo_move_to(cr, x0, ib.yi[i]);
			cairo_line_to(cr, x1, ib.yh[j]);
			cairo_stroke(cr);
		}
	for (int j = 0; j < n; j++)
		for (int k = 0; k < IB_OUT; k++) {
			float v = f2[j * IB_OUT + k] / m2;
			if (fabsf(v) < IB_FAINT) continue;
			ib_rgba(cr, v >= 0 ? zmc.accent : IB_NEGATIVE, 0.05 + 0.6 * fabsf(v));
			cairo_set_line_width(cr, 0.5 + 1.8 * fabsf(v));
			cairo_move_to(cr, x1, ib.yh[j]);
			cairo_line_to(cr, x2, ib.yo[k]);
			cairo_stroke(cr);
		}

	/* the pulses' paths: the strongest inputs into the hidden layer, and the
	 * hidden neurons that pushed the winner up */
	ib.win = 0;
	for (int k = 1; k < IB_OUT; k++) if (ib.sp[k] > ib.sp[ib.win]) ib.win = k;
	ib.t1 = ib_top(f1, IB_IN * n, 12, ib.top1);
	float to_win[IB_HID_MAX];
	for (int j = 0; j < n; j++) to_win[j] = f2[j * IB_OUT + ib.win] > 0 ? f2[j * IB_OUT + ib.win] : 0;
	ib.t2 = ib_top(to_win, n, 8, ib.top2);

	/* the neurons: inputs by their size (standard deviations), hidden by how hard they fire */
	float hmax = 1e-6f;
	for (int j = 0; j < n; j++) if (ib.sh[j] > hmax) hmax = ib.sh[j];
	for (int i = 0; i < IB_IN; i++) {
		ib_neuron(cr, x0, ib.yi[i], 7, fabsf(ib.sx[i]) / 3, false);
		cairo_set_source_rgb(cr, 0.75, 0.75, 0.75);
		double tw = zmc_text_width(ib_inputs[i], FONT_REGULAR, 10);
		zmc_text(cr, ib_inputs[i], FONT_REGULAR, 10, x0 - 12 - tw, ib.yi[i] - 7, 0);
	}
	int firing = 0;
	for (int j = 0; j < n; j++) {
		ib_neuron(cr, x1, ib.yh[j], 5, ib.sh[j] / hmax, ib.h[j] > 0);
		firing += ib.h[j] > 0;
	}
	for (int k = 0; k < IB_OUT; k++) {
		ib_neuron(cr, x2, ib.yo[k], 11, ib.sp[k], k == ib.win);
		char label[32];
		if (k == ib.win) zmc_accent(cr);
		else cairo_set_source_rgb(cr, 0.75, 0.75, 0.75);
		zmc_text(cr, ib_rungs[k], k == ib.win ? FONT_SEMIBOLD : FONT_REGULAR, 12, x2 + 18, ib.yo[k] - 15, 0);
		snprintf(label, sizeof(label), "%.0f %%", ib.sp[k] * 100);
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		zmc_text(cr, label, FONT_REGULAR, 10, x2 + 18, ib.yo[k] + 1, 0);
	}
	char line[160];
	snprintf(line, sizeof(line), "%d inputs, %d hidden (ReLU), %d outputs (softmax): %d of %d firing",
		IB_IN, n, IB_OUT, firing, n);
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, line, FONT_REGULAR, 12, MARGIN, 34 + 15.0 * n + 14, w);
}

/* the network over the page, on the GPU, every frame; its surface drawn again
 * only when a sample has come in */
static void ib_overlay(int page) {
	if (page != PAGE_BOOST || ib.frozen_phase >= 0 || ib.net_h <= 0) return;
	double oy = ib.net_y - zmc.scroll;
	if (oy + ib.net_h < 0 || oy > zmc.height) return;
	int sw = zmc.width * zmc.scale, sh = ib.net_h * zmc.scale;
	if (!ib.net || cairo_image_surface_get_width(ib.net) != sw || cairo_image_surface_get_height(ib.net) != sh) {
		if (ib.net) cairo_surface_destroy(ib.net);
		ib.net = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, sw, sh);
		ib.net_dirty = true;
	}
	uint32_t now = zmc_now_ms();
	if (ib.net_dirty && (now - ib.net_at >= IB_NET_MS || !ib.net_rev)) {
		ib.net_at = now;
		cairo_t *cr = cairo_create(ib.net);
		cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
		cairo_paint(cr);
		cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
		cairo_scale(cr, zmc.scale, zmc.scale);
		ib_network(cr);
		cairo_destroy(cr);
		cairo_surface_flush(ib.net);
		ib.net_rev++;
		ib.net_dirty = false;
	}
	double c[4][3] = { { 0, oy, 0 }, { zmc.width, oy, 0 }, { 0, oy + ib.net_h, 0 }, { zmc.width, oy + ib.net_h, 0 } };
	zmc_gpu_quad(ib.net, ib.net_rev, c, 1);
	ib_pulses(NULL, oy);
}

/* the share of time on each rung, a bar each */
static double ib_rung_bars(cairo_t *cr, double y) {
	double total = 0, w = zmc.width - 2 * MARGIN;
	for (int k = 0; k < IB_OUT; k++) total += ib.rung_ms[k];
	for (int k = 0; k < IB_OUT; k++) {
		double share = total > 0 ? ib.rung_ms[k] / total : 0;
		char pct[16];
		snprintf(pct, sizeof(pct), "%.0f %%", share * 100);
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text(cr, ib_rungs[k], FONT_REGULAR, 14, MARGIN, y, 0);
		cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
		cairo_rectangle(cr, MARGIN + 92, y + 6, w - 92 - 46, 10);
		cairo_fill(cr);
		if (k == ib.rung) zmc_accent(cr);
		else ib_rgba(cr, zmc.accent, 0.55);
		cairo_rectangle(cr, MARGIN + 92, y + 6, (w - 92 - 46) * share, 10);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		zmc_text(cr, pct, FONT_REGULAR, 12, MARGIN + w - 40, y + 2, 0);
		y += 26;
	}
	return y + 10;
}

static double draw_boost(cairo_t *cr) {
	static const char *const modes[] = { "Off", "Shadow (only watch)", "On (lift early)" };
	char value[160];
	double y = zmc_header(cr, "dynamic boost");
	if (!ib.live) {
		y = zmc_field(cr, y, "zrnperfd", "not running, or the screen has been off since it started");
	}

	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, "RapidBoost", FONT_SEMILIGHT, 26, MARGIN, y, 0);
	y += 44;
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, "rung now", FONT_REGULAR, 13, MARGIN, y, 0);
	zmc_accent(cr);
	zmc_text(cr, ib_rungs[ib.rung >= 0 && ib.rung < IB_OUT ? ib.rung : 0], FONT_LIGHT, 40, MARGIN, y + 14, 0);
	if (ib.rung > ib.ladder && ib.ladder >= 0) {
		snprintf(value, sizeof(value), "lifted by IntelligentBoost (the ladder said %s)", ib_rungs[ib.ladder]);
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		zmc_text(cr, value, FONT_REGULAR, 12, MARGIN, y + 66, 0);
	}
	y += 92;
	y = ib_rung_bars(cr, y);
	snprintf(value, sizeof(value), "%lu up, %lu down", ib.ups, ib.downs);
	y = zmc_field(cr, y, "Steps", value);

	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, "IntelligentBoost", FONT_SEMILIGHT, 26, MARGIN, y, 0);
	y += 44;
	int mode = !strcmp(ib.mode, "on") ? 2 : !strcmp(ib.mode, "shadow") ? 1 : 0;
	y = picker(cr, y, "Mode", modes, 3, mode, ACT_IB_MODE);
	static const char *const engines[] = { "Processor (about 5 us)", "SCORE vision DSP (about 1.7 ms)" };
	y = picker(cr, y, "Run the network on", engines, 2, !strcmp(ib.prefer, "score"), ACT_IB_ENGINE);
	snprintf(value, sizeof(value), "%s", !strcmp(ib.engine, "score") ? "SCORE, the vision DSP (33 MHz)" :
		!strcmp(ib.engine, "cpu") ? (!strcmp(ib.prefer, "score") ? "the processor (SCORE unavailable)" :
		"the processor") : "nothing yet: no model, or no samples");
	y = zmc_field(cr, y, "Running on", value);
	snprintf(value, sizeof(value), "%lu (SCORE %lu, processor %lu)", ib.predictions, ib.on_score, ib.on_cpu);
	y = zmc_field(cr, y, "Predictions", value);
	snprintf(value, sizeof(value), "%.2f ms (average %.2f ms)", ib.latency_us / 1000.0, ib.latency_avg_us / 1000.0);
	y = zmc_field(cr, y, "One prediction", value);
	snprintf(value, sizeof(value), "%lu times ahead of the ladder", ib.lifts);
	y = zmc_field(cr, y, "Early lifts", value);
	snprintf(value, sizeof(value), "%lu checked against the processor, %lu wrong", ib.checks, ib.check_failures);
	y = zmc_field(cr, y, "SCORE self-check", value);

	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, "the network", FONT_SEMILIGHT, 26, MARGIN, y, 0);
	y += 44;
	ib.net_y = y;
	ib.net_h = ib_layout();
	if (ib.frozen_phase >= 0) {	/* --render has no GPU pass: draw it into the page */
		cairo_save(cr);
		cairo_translate(cr, 0, y);
		ib_network(cr);
		ib_pulses(cr, 0);
		cairo_restore(cr);
	}
	y += ib.net_h;
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	y += zmc_text(cr, "A multilayer perceptron trained on this phone's own use. Four times a second it reads "
		"the load, the clocks and the touch, and predicts the rung RapidBoost will reach in the next two "
		"seconds; in On it lifts the rung there at once, and leaves stepping down to the ladder.",
		FONT_REGULAR, 12, MARGIN, y + 6, zmc.width - 2 * MARGIN) + 20;
	return y;
}
