// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The real cameras, from /dev/zrn-cam (camera/driver/zrn-cam-live.h): 16-bit
 * Bayer with no ISP. The app keeps every fourth pixel's 2x2 cell as one RGBA
 * texel and the GPU makes colour from it: black level, white balance, gamma.
 * Included by ztcamera.c, inside its GL frame.
 */
#include <GLES2/gl2.h>

#define RAW_DEV		"/dev/zrn-cam"
#define RAW_BUFS	4
#define RAW_AREA	(2016u * 2 * 1512)
#define RAW_STEP	4		/* source pixels per texel, an even number */
#define RAW_BLACK	64		/* of 1023, the frame mean seen with no light */
#define AE_TARGET	110
#define AE_EVERY	15		/* frames */

struct zrn_cam_frame {
	uint32_t seq, index, w, h, stride, bayer, errors, stopped;
};

enum raw_cam { RAW_REAR, RAW_FRONT, RAW_IR, RAW_N };

static struct {
	int fd;
	void *map;
	enum raw_cam cam;
	uint32_t seq;
	struct zrn_cam_frame info;
	uint8_t *cells;
	int cw, ch;
	bool fresh;
	unsigned tex;
	int program, u_gain, u_front, u_pitch, u_black;
	float gain[3];
	int coarse, again;
	bool f24;		/* the aperture: F2.4 asked for last, F1.5 otherwise */
} raw = { .fd = -1, .gain = { 1, 1, 1 }, .coarse = 8000, .again = 64 };

static const char *const raw_stop_why[] = { "running", "stopped", "idle", "time limit",
	"too warm", "receiver errors", "failed" };

static void raw_close(void) {
	if (raw.map) {
		munmap(raw.map, RAW_BUFS * RAW_AREA);
		raw.map = NULL;
	}
	if (raw.fd >= 0) {
		close(raw.fd);	/* the driver powers the camera down */
		raw.fd = -1;
	}
}

static bool raw_say(const char *cmd) {
	return raw.fd >= 0 && write(raw.fd, cmd, strlen(cmd)) == (ssize_t)strlen(cmd);
}

static bool raw_open(enum raw_cam cam) {
	raw_close();
	raw.cam = cam;
	raw.seq = 0;
	raw.info.seq = 0;
	if (cam == RAW_IR) {
		return false;
	}
	raw.fd = open(RAW_DEV, O_RDWR | O_CLOEXEC);
	if (raw.fd < 0) {
		return false;
	}
	if (!raw_say(cam == RAW_FRONT ? "front" : "rear")) {
		raw_close();
		return false;
	}
	raw.map = mmap(NULL, RAW_BUFS * RAW_AREA, PROT_READ, MAP_SHARED, raw.fd, 0);
	if (raw.map == MAP_FAILED) {
		raw.map = NULL;
		raw_close();
		return false;
	}
	if (read(raw.fd, &raw.info, sizeof(raw.info)) != sizeof(raw.info) || !raw.info.w) {
		raw_close();
		return false;
	}
	free(raw.cells);
	raw.cw = raw.info.w / RAW_STEP;
	raw.ch = raw.info.h / RAW_STEP;
	raw.cells = malloc((size_t)raw.cw * raw.ch * 4);
	raw.coarse = cam == RAW_FRONT ? 1000 : 8000;
	raw.again = 64;
	return raw.cells != NULL;
}

/* One stop towards the target at a time: exposure first, then gain. */
static void raw_expose(double mean) {
	int max = raw.cam == RAW_FRONT ? 1200 : 25000;
	int c = raw.coarse, g = raw.again;
	if (mean < AE_TARGET * 0.8) {
		if (c < max) c = c * 3 / 2 > max ? max : c * 3 / 2;
		else if (g < 512) g = g * 3 / 2 > 512 ? 512 : g * 3 / 2;
	} else if (mean > AE_TARGET * 1.25) {
		if (g > 32) g = g * 2 / 3 < 32 ? 32 : g * 2 / 3;
		else if (c > 2) c = c * 2 / 3 < 2 ? 2 : c * 2 / 3;
	}
	if (c != raw.coarse || g != raw.again) {
		char cmd[40];
		snprintf(cmd, sizeof(cmd), "exp %d %d", c, g);
		raw.coarse = c;
		raw.again = g;
		raw_say(cmd);
	}
}

/* Newest frame to cells; a stopped camera returns its reason. */
static const char *raw_poll(void) {
	if (raw.fd < 0 || read(raw.fd, &raw.info, sizeof(raw.info)) != sizeof(raw.info)) {
		return raw.fd < 0 ? NULL : "gone";
	}
	if (raw.info.stopped) {
		return raw_stop_why[raw.info.stopped < 7 ? raw.info.stopped : 6];
	}
	if (raw.info.seq == raw.seq || !raw.info.seq) {
		return NULL;
	}
	raw.seq = raw.info.seq;
	const uint8_t *base = (const uint8_t *)raw.map + raw.info.index * (size_t)RAW_AREA;
	uint64_t sum[4] = { 0, 0, 0, 0 };
	uint8_t *out = raw.cells;
	for (int y = 0; y < raw.ch; y++) {
		const uint16_t *a = (const uint16_t *)(base + (size_t)(y * RAW_STEP) * raw.info.stride);
		const uint16_t *b = (const uint16_t *)((const uint8_t *)a + raw.info.stride);
		for (int x = 0; x < raw.cw; x++) {
			int i = x * RAW_STEP;
			out[0] = a[i] >> 2;
			out[1] = a[i + 1] >> 2;
			out[2] = b[i] >> 2;
			out[3] = b[i + 1] >> 2;
			sum[0] += out[0];
			sum[1] += out[1];
			sum[2] += out[2];
			sum[3] += out[3];
			out += 4;
		}
	}
	raw.fresh = true;
	if (raw.seq % AE_EVERY == 0) {
		double n = (double)raw.cw * raw.ch, k = RAW_BLACK / 4.0;
		double g = (sum[0] + sum[3]) / (2 * n) - k;
		double s1 = sum[1] / n - k, s2 = sum[2] / n - k;
		double r = raw.info.bayer ? s1 : s2, bl = raw.info.bayer ? s2 : s1;
		/* grey world, kept sane in the dark */
		raw.gain[0] = g > 4 && r > 2 ? fmin(3, fmax(0.5, g / r)) : 1;
		raw.gain[1] = 1;
		raw.gain[2] = g > 4 && bl > 2 ? fmin(3, fmax(0.5, g / bl)) : 1;
		raw_expose((sum[0] + sum[3]) / (2 * n));
	}
	return NULL;
}

static GLuint raw_shader(GLenum type, const char *text) {
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &text, NULL);
	glCompileShader(s);
	return s;
}

static bool raw_gl(void) {
	if (raw.program) {
		return raw.program > 0;
	}
	static const char vs[] =
		"attribute vec2 position; attribute vec2 texcoord; varying vec2 uv;"
		"void main(){ uv=texcoord; gl_Position=vec4(position,0.0,1.0); }";
	static const char fs[] =
		"precision mediump float; varying vec2 uv; uniform sampler2D cells;"
		"uniform vec3 gain; uniform float front; uniform float pitch; uniform float black;"
		"vec3 at(vec2 p){ vec4 c=texture2D(cells,p);"
		" float g=(c.r+c.a)*0.5; float one=mix(c.b,c.g,front); float other=mix(c.g,c.b,front);"
		" vec3 v=(vec3(one,g,other)-vec3(black))/(1.0-black);"
		" return pow(clamp(v*gain,0.0,1.0),vec3(0.4545)); }"
		"void main(){ if(pitch<=0.0){ gl_FragColor=vec4(at(uv),1.0); return; }"
		/* DotMatrix: a lattice of dots, each as bright as what is under it */
		" vec2 cell=floor(gl_FragCoord.xy/pitch); vec2 mid=(cell+0.5)*pitch;"
		" float r=length(gl_FragCoord.xy-mid)/(pitch*0.5);"
		" vec2 p=uv+(mid-gl_FragCoord.xy)*vec2(dFdx(uv.x)+dFdy(uv.x),dFdx(uv.y)+dFdy(uv.y));"
		" vec3 c=at(p); float y=dot(c,vec3(0.3,0.59,0.11));"
		" gl_FragColor=vec4(vec3(y*(1.0-smoothstep(0.55,0.8,r))),1.0); }";
	static const char fs_ext[] = "#extension GL_OES_standard_derivatives : enable\n";
	char *full = malloc(sizeof(fs_ext) + sizeof(fs));
	if (!full) {
		return false;
	}
	snprintf(full, sizeof(fs_ext) + sizeof(fs), "%s%s", fs_ext, fs);
	GLuint p = glCreateProgram();
	glAttachShader(p, raw_shader(GL_VERTEX_SHADER, vs));
	glAttachShader(p, raw_shader(GL_FRAGMENT_SHADER, full));
	free(full);
	glBindAttribLocation(p, 0, "position");
	glBindAttribLocation(p, 1, "texcoord");
	glLinkProgram(p);
	GLint ok = 0;
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		raw.program = -1;
		return false;
	}
	raw.program = p;
	raw.u_gain = glGetUniformLocation(p, "gain");
	raw.u_front = glGetUniformLocation(p, "front");
	raw.u_pitch = glGetUniformLocation(p, "pitch");
	raw.u_black = glGetUniformLocation(p, "black");
	GLuint t;
	glGenTextures(1, &t);
	raw.tex = t;
	glBindTexture(GL_TEXTURE_2D, t);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return true;
}

/*
 * Over the whole window, cropped to fill. The sensors lie on their sides: the
 * frame's width runs down the phone. The core's own program and buffers come
 * back afterwards; every core draw sets its texture and vertices again.
 */
static void raw_draw(void) {
	if (!raw.cells || !raw.seq || !raw_gl()) {
		return;
	}
	GLint old = 0;
	glGetIntegerv(GL_CURRENT_PROGRAM, &old);
	glUseProgram(raw.program);
	glBindTexture(GL_TEXTURE_2D, raw.tex);
	static int tw, th;
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	if (tw != raw.cw || th != raw.ch) {
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, raw.cw, raw.ch, 0, GL_RGBA, GL_UNSIGNED_BYTE, raw.cells);
		tw = raw.cw;
		th = raw.ch;
	} else if (raw.fresh) {
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, raw.cw, raw.ch, GL_RGBA, GL_UNSIGNED_BYTE, raw.cells);
	}
	raw.fresh = false;
	double span = fmin(1, ((double)zmc.width / zmc.height) * ((double)raw.info.w / raw.info.h));
	float v0 = 0.5f - span / 2, v1 = 0.5f + span / 2;
	bool mirror = raw.cam == RAW_FRONT;
	/* position x,y, then the frame's u,v: top of the screen is u=0 */
	const float q[16] = {
		-1,  1, 0, mirror ? v0 : v1,
		 1,  1, 0, mirror ? v1 : v0,
		-1, -1, 1, mirror ? v0 : v1,
		 1, -1, 1, mirror ? v1 : v0,
	};
	glUniform3f(raw.u_gain, raw.gain[0], raw.gain[1], raw.gain[2]);
	glUniform1f(raw.u_front, raw.info.bayer ? 1.0f : 0.0f);
	glUniform1f(raw.u_black, RAW_BLACK / 1023.0f);
	glUniform1f(raw.u_pitch, zmc.style == ZMC_STYLE_DOTMATRIX ? (float)(zmg_dot_pitch * zmc.scale) : 0.0f);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), q);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), q + 2);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glUseProgram(old);
}

/* For a photo: the latest cells as RGB by the same steps, on the CPU, once. */
static void raw_to_surface(cairo_surface_t *sf) {
	int w = cairo_image_surface_get_width(sf), h = cairo_image_surface_get_height(sf);
	int st = cairo_image_surface_get_stride(sf);
	uint8_t *d = cairo_image_surface_get_data(sf);
	cairo_surface_flush(sf);
	for (int y = 0; y < h && y < raw.ch; y++) {
		uint32_t *row = (uint32_t *)(d + y * st);
		const uint8_t *c = raw.cells + (size_t)y * raw.cw * 4;
		for (int x = 0; x < w && x < raw.cw; x++, c += 4) {
			double k = 16, one = raw.info.bayer ? c[1] : c[2], other = raw.info.bayer ? c[2] : c[1];
			double rgb[3] = { (one - k) * raw.gain[0], ((c[0] + c[3]) / 2.0 - k) * raw.gain[1], (other - k) * raw.gain[2] };
			uint32_t px = 0xff000000u;
			for (int i = 0; i < 3; i++) {
				double v = pow(fmin(1, fmax(0, rgb[i] / 239)), 0.4545);
				px |= (uint32_t)(v * 255) << (16 - 8 * i);
			}
			row[x] = px;
		}
	}
	cairo_surface_mark_dirty(sf);
}
