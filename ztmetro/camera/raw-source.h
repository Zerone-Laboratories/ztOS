// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The real cameras, from /dev/zrn-cam (camera/driver/zrn-cam-live.h): 16-bit
 * Bayer with no ISP. The GPU reads the driver's memory directly as a DMA-BUF and
 * does all of it, once per sensor frame, into a texture of the frame's own size:
 * black level, lens shading, demosaic (Malvar-He-Cutler), white balance, colour
 * matrix, sRGB. The screen then draws that texture. The CPU reads only a thin grid
 * of pixels for exposure, white balance and focus.
 *
 * Numbers from Samsung's stock firmware (see camera/README.md): black 64 and white
 * 1023 (libexynoscamera3.so), sensor orientation rear 90, front 270 (the same), rear
 * colour from its forward matrices; front colour from a matrix in setfile_3h1.bin.
 * Included by ztcamera.c, inside its GL frame.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#define RAW_DEV		"/dev/zrn-cam"
#define RAW_BUFS	4
#define RAW_FULL_AREA	(4032u * 2 * 3024)
#define RAW_POOL	(2 * (size_t)RAW_FULL_AREA)
#define RAW_BLACK	64
#define RAW_WHITE	1023
#define AE_TARGET	170.0		/* mean green above black, of 959: about 0.18 linear, mid grey */
#define AE_EVERY	4		/* frames: the sensor takes a new exposure two frames later */
#define AF_SETTLE	3		/* frames at a lens position before it is scored */
#define AF_STEP		40
#define AF_FINE		12
#define RAW_DMABUF	_IO('Z', 1)
#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

struct zrn_cam_frame {
	uint32_t seq, index, w, h, stride, bayer, errors, stopped, area;
};

enum raw_cam { RAW_REAR, RAW_FRONT, RAW_IR, RAW_N };
enum af_state { AF_IDLE, AF_WAIT_AE, AF_SWEEP, AF_REFINE, AF_HOLD };

static struct {
	int fd, dmabuf;
	void *map;
	enum raw_cam cam;
	uint32_t seq, drawn;
	struct zrn_cam_frame info;
	/* the frame as the GPU reads it */
	void *img[RAW_BUFS];
	unsigned tex[RAW_BUFS];
	float unpack[4];
	bool zero_copy;
	unsigned upload_tex;
	/* what the frame becomes */
	unsigned out_tex, fbo;
	int out_w, out_h;
	int prog, u_size, u_unpack, u_first, u_gain, u_ccm, u_black, u_shade;
	int disp, d_pitch, d_step;
	/* the loops */
	float gain[3];
	float ccm[9];
	int coarse, again, coarse_max;
	bool f24;		/* the aperture: F2.4 asked for last, F1.5 otherwise */
	bool wb_set;
	int af_pos, af_best, af_wait, af_lo, af_hi;
	double af_score, af_held;
	uint32_t af_seq;
	enum af_state af;
	double af_low_since, af_asked;
	bool ae_settled;
} raw = { .fd = -1, .dmabuf = -1, .gain = { 1, 1, 1 }, .coarse = 8000, .again = 64 };

static const char *const raw_stop_why[] = { "running", "stopped", "idle", "time limit",
	"too warm", "receiver errors", "failed" };

/* Forward matrices of libexynoscamera3.so, 2L3, into linear sRGB (D50 adapted):
 * illuminant A's and D65's. The rows sum to 1, as they should. */
static const float rear_fm_a[9] = { 1.602f, -0.598f, -0.007f, -0.145f, 1.331f, -0.184f, 0.040f, -0.723f, 1.684f };
static const float rear_fm_d[9] = { 1.546f, -0.498f, -0.046f, -0.244f, 1.424f, -0.181f, 0.035f, -1.115f, 2.081f };
/* setfile_3h1.bin, at 0x18137 and repeated, over 256: which illuminant is not labelled */
static const float front_ccm[9] = { 330 / 256.f, -80 / 256.f, 6 / 256.f, -72 / 256.f, 346 / 256.f, -18 / 256.f,
	-12 / 256.f, -119 / 256.f, 387 / 256.f };

static double raw_now(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static void raw_images_free(void) {
	static PFNEGLDESTROYIMAGEKHRPROC destroy;
	if (!destroy) {
		destroy = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
	}
	for (int i = 0; i < RAW_BUFS; i++) {
		if (raw.img[i] && destroy) {
			destroy(eglGetCurrentDisplay(), raw.img[i]);
		}
		raw.img[i] = NULL;
	}
}

static void raw_close(void) {
	raw_images_free();
	if (raw.map) {
		munmap(raw.map, RAW_POOL);
		raw.map = NULL;
	}
	if (raw.dmabuf >= 0) {
		close(raw.dmabuf);
		raw.dmabuf = -1;
	}
	if (raw.fd >= 0) {
		close(raw.fd);	/* the driver powers the camera down */
		raw.fd = -1;
	}
}

static bool raw_say(const char *cmd) {
	return raw.fd >= 0 && write(raw.fd, cmd, strlen(cmd)) == (ssize_t)strlen(cmd);
}

static void raw_set_exposure(int c, int g) {
	char cmd[40];
	snprintf(cmd, sizeof(cmd), "exp %d %d", c, g);
	raw_say(cmd);
}

static void raw_set_focus(int pos) {
	char cmd[24];
	snprintf(cmd, sizeof(cmd), "af %d", pos);
	raw_say(cmd);
}

/* Focus waits for exposure: a dark frame's noise scores as sharp at any lens position. */
static void raw_focus_start(void) {
	raw.af = AF_WAIT_AE;
	raw.af_asked = raw_now();
	raw.ae_settled = false;
}

static void raw_focus_sweep(void) {
	raw.af = AF_SWEEP;
	raw.af_pos = 0;
	raw.af_best = 0;
	raw.af_score = -1;
	raw.af_wait = AF_SETTLE;
	raw.af_lo = 0;
	raw.af_hi = 511;
	raw_set_focus(0);
}

static bool raw_open(enum raw_cam cam) {
	raw_close();
	raw.cam = cam;
	raw.seq = 0;
	raw.drawn = 0;
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
	raw.map = mmap(NULL, RAW_POOL, PROT_READ, MAP_SHARED, raw.fd, 0);
	if (raw.map == MAP_FAILED) {
		raw.map = NULL;
		raw_close();
		return false;
	}
	if (read(raw.fd, &raw.info, sizeof(raw.info)) != sizeof(raw.info) || !raw.info.w) {
		raw_close();
		return false;
	}
	raw.dmabuf = ioctl(raw.fd, RAW_DMABUF);
	if (raw.dmabuf < 0) {
		fprintf(stderr, "ztcamera: no DMA-BUF (%s), the frames will be copied\n", strerror(errno));
	}
	/* preview's own frame length: 25456 and 1266 lines (the vendor's mode tables) */
	raw.coarse_max = cam == RAW_FRONT ? 1250 : 25000;
	raw.coarse = cam == RAW_FRONT ? 1000 : 8000;
	raw.again = 64;
	raw.wb_set = false;
	raw.gain[0] = raw.gain[1] = raw.gain[2] = 1;
	memcpy(raw.ccm, cam == RAW_FRONT ? front_ccm : rear_fm_d, sizeof(raw.ccm));
	raw_set_exposure(raw.coarse, raw.again);
	raw_focus_start();
	return true;
}

/* Exposure towards the target, gain only once exposure is at its limit. */
static void raw_expose(double green, double clipped) {
	double target = AE_TARGET;
	if (clipped > 0.03) {
		target *= 0.75;		/* hold the highlights */
	}
	double r = green > 1 ? target / green : 2;
	if (r > 0.92 && r < 1.08) {
		raw.ae_settled = true;
		return;
	}
	if (r > 1 && raw.coarse >= raw.coarse_max && raw.again >= 512) {
		raw.ae_settled = true;	/* as bright as it goes */
		return;
	}
	raw.ae_settled = false;
	r = fmin(1.5, fmax(0.6, r));
	double want = (double)raw.coarse * raw.again / 32 * r;
	int c = (int)fmin(raw.coarse_max, fmax(4, want));
	int g = (int)fmin(512, fmax(32, want / c * 32));
	if (c != raw.coarse || g != raw.again) {
		raw.coarse = c;
		raw.again = g;
		raw_set_exposure(c, g);
	}
}

/* A thin grid across the frame, centre counted twice. */
static void raw_stats(const uint8_t *base) {
	int w = raw.info.w, h = raw.info.h, st = raw.info.stride;
	double sum[3] = { 0, 0, 0 }, n = 0, seen = 0, clip = 0;
	for (int y = h / 64 & ~1; y < h - 1; y += h / 32 & ~1) {
		const uint16_t *a = (const uint16_t *)(base + (size_t)y * st);
		const uint16_t *b = (const uint16_t *)(base + (size_t)(y + 1) * st);
		bool ymid = y > h / 4 && y < h * 3 / 4;
		for (int x = w / 64 & ~1; x < w - 1; x += w / 32 & ~1) {
			int g1 = a[x], one = a[x + 1], other = b[x], g2 = b[x + 1];
			int r = raw.info.bayer ? one : other, bl = raw.info.bayer ? other : one;
			double wt = ymid && x > w / 4 && x < w * 3 / 4 ? 2 : 1;
			seen += wt;
			if (g1 >= 990 || g2 >= 990 || r >= 990 || bl >= 990) {
				clip += wt;
				continue;
			}
			sum[0] += wt * (r - RAW_BLACK);
			sum[1] += wt * ((g1 + g2) / 2.0 - RAW_BLACK);
			sum[2] += wt * (bl - RAW_BLACK);
			n += wt;
		}
	}
	if (!n) {
		raw_expose(1e6, 1);
		return;
	}
	double r = sum[0] / n, g = sum[1] / n, bl = sum[2] / n;
	if (raw.af != AF_SWEEP && raw.af != AF_REFINE) {	/* a steady picture to score */
		raw_expose(g, clip / seen);
	}
	/* grey world, moved gently, sane in the dark */
	if (g > 6 && r > 2 && bl > 2) {
		double kr = fmin(4, fmax(0.8, g / r)), kb = fmin(4, fmax(0.8, g / bl));
		double m = raw.wb_set ? 0.2 : 1;
		raw.gain[0] += (kr - raw.gain[0]) * m;
		raw.gain[2] += (kb - raw.gain[2]) * m;
		raw.gain[1] = 1;
		raw.wb_set = true;
		if (raw.cam == RAW_REAR) {
			/* warm light has the larger red gain: A's matrix there, D65's in daylight */
			double t = fmin(1, fmax(0, (raw.gain[0] - 1.5) / 0.8));
			for (int i = 0; i < 9; i++) {
				raw.ccm[i] = (float)(rear_fm_d[i] + (rear_fm_a[i] - rear_fm_d[i]) * t);
			}
		}
	}
}

/* Green differences in the middle of the frame, over its brightness. */
static double raw_sharpness(const uint8_t *base) {
	int w = raw.info.w, h = raw.info.h, st = raw.info.stride;
	double d = 0, s = 1;
	for (int y = (h * 3 / 8) & ~1; y < h * 5 / 8; y += 8) {
		const uint16_t *a = (const uint16_t *)(base + (size_t)y * st);
		for (int x = (w * 3 / 8) & ~1; x < w * 5 / 8; x += 8) {
			/* pairs of green pixels against the next pair: less noise than one against one */
			int v = (int)a[x] + a[x + 2] - 2 * RAW_BLACK, u = (int)a[x + 4] + a[x + 6] - 2 * RAW_BLACK;
			int m = (int)a[x + 8] + a[x + 10] - 2 * RAW_BLACK;
			d += (v - u) * (v - u) + (u - m) * (u - m);
			s += v > 0 ? v : 0;
		}
	}
	return d / (s * s);
}

/* Sweep, then fine steps round the best, then stay until the picture softens. */
static void raw_focus(const uint8_t *base) {
	if (raw.af == AF_IDLE) {
		return;
	}
	if (raw.af == AF_WAIT_AE) {
		if (raw.ae_settled || raw_now() - raw.af_asked > 2) {
			raw_focus_sweep();
		}
		return;
	}
	if (raw.af == AF_HOLD) {
		if (raw.seq % 15 == 0) {
			double s = raw_sharpness(base), now = raw_now();
			if (s > raw.af_held) {
				raw.af_held = s;
			}
			if (s < raw.af_held * 0.6) {
				if (raw.af_low_since == 0) {
					raw.af_low_since = now;
				} else if (now - raw.af_low_since > 2) {
					raw.af_low_since = 0;
					raw_focus_start();
				}
			} else {
				raw.af_low_since = 0;
			}
		}
		return;
	}
	if (--raw.af_wait > 0) {
		return;
	}
	double s = raw_sharpness(base);
	if (s > raw.af_score) {
		raw.af_score = s;
		raw.af_best = raw.af_pos;
	}
	int step = raw.af == AF_SWEEP ? AF_STEP : AF_FINE;
	raw.af_pos += step;
	if (raw.af_pos > raw.af_hi) {
		if (raw.af == AF_SWEEP) {
			raw.af = AF_REFINE;
			raw.af_lo = raw.af_best - AF_STEP > 0 ? raw.af_best - AF_STEP : 0;
			raw.af_hi = raw.af_best + AF_STEP < 511 ? raw.af_best + AF_STEP : 511;
			raw.af_pos = raw.af_lo;
		} else {
			raw.af = AF_HOLD;
			raw.af_held = raw.af_score;
			raw.af_low_since = 0;
			raw_set_focus(raw.af_best);
			return;
		}
	}
	raw_set_focus(raw.af_pos);
	raw.af_wait = AF_SETTLE;
}

/* Newest frame's statistics; a stopped camera returns its reason. */
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
	uint32_t was = raw.seq;
	raw.seq = raw.info.seq;
	const uint8_t *base = (const uint8_t *)raw.map + raw.info.index * (size_t)raw.info.area;
	if (raw.seq / AE_EVERY != was / AE_EVERY) {
		raw_stats(base);
	}
	raw_focus(base);
	return NULL;
}

static GLuint raw_shader(GLenum type, const char *text) {
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &text, NULL);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[800] = "";
		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		fprintf(stderr, "ztcamera: shader: %s\n", log);
	}
	return s;
}

static GLuint raw_program(const char *vs, const char *fs) {
	GLuint p = glCreateProgram();
	glAttachShader(p, raw_shader(GL_VERTEX_SHADER, vs));
	glAttachShader(p, raw_shader(GL_FRAGMENT_SHADER, fs));
	glBindAttribLocation(p, 0, "position");
	glBindAttribLocation(p, 1, "texcoord");
	glLinkProgram(p);
	GLint ok = 0;
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	return ok ? p : 0;
}

static bool raw_gl(void) {
	if (raw.prog) {
		return raw.prog > 0;
	}
	static const char vs[] =
		"attribute vec2 position; attribute vec2 texcoord; varying vec2 uv;"
		"void main(){ uv=texcoord; gl_Position=vec4(position,0.0,1.0); }";
	/* Malvar, He and Cutler's 5x5 filters. first: where in the 2x2 cell red is. */
	static const char fs[] =
		"precision highp float; varying vec2 uv; uniform sampler2D bayer;"
		"uniform vec2 size; uniform vec4 unpack; uniform vec2 first; uniform vec3 gain;"
		"uniform mat3 ccm; uniform float black; uniform float shade;"
		"float px(float x,float y){ return dot(texture2D(bayer,uv+vec2(x,y)/size),unpack); }"
		"void main(){"
		" float c=px(0.,0.);"
		" float n1=px(0.,-1.),s1=px(0.,1.),w1=px(-1.,0.),e1=px(1.,0.);"
		" float n2=px(0.,-2.),s2=px(0.,2.),w2=px(-2.,0.),e2=px(2.,0.);"
		" float d=px(-1.,-1.)+px(1.,-1.)+px(-1.,1.)+px(1.,1.);"
		" vec2 p=mod(floor(uv*size)+first,2.);"	/* 0,0 is red; 1,1 is blue */
		" float gc=(4.*c+2.*(n1+s1+w1+e1)-(n2+s2+w2+e2))/8.;"
		" float opp=(6.*c+2.*d-1.5*(n2+s2+w2+e2))/8.;"
		" float rowh=(5.*c+4.*(w1+e1)-(w2+e2)-d+.5*(n2+s2))/8.;"	/* same colour left and right */
		" float rowv=(5.*c+4.*(n1+s1)-(n2+s2)-d+.5*(w2+e2))/8.;"
		" vec3 v;"
		" if(p.x<.5&&p.y<.5) v=vec3(c,gc,opp);"
		" else if(p.x>.5&&p.y>.5) v=vec3(opp,gc,c);"
		" else if(p.y<.5) v=vec3(rowh,c,rowv);"		/* green on red's row */
		" else v=vec3(rowv,c,rowh);"			/* green on blue's row */
		" v=(v-black)/(1.-black);"
		" vec2 q=uv-.5; float r2=dot(q,q)*shade;"
		" v*=1.+r2*(1.+r2);"
		" v=clamp(v*gain,0.,1.);"
		" v=clamp(ccm*v,0.,1.);"
		" v=mix(v*12.92,1.055*pow(v,vec3(.41667))-.055,step(.0031308,v));"
		" v=mix(v,v*v*(3.-2.*v),.35);"		/* the light S curve a phone's tone map gives */
		" gl_FragColor=vec4(v,1.); }";
	/* the finished frame on the screen: a light unsharp mask, or DotMatrix's dots */
	static const char fd[] =
		"#extension GL_OES_standard_derivatives : enable\n"
		"precision highp float; varying vec2 uv; uniform sampler2D frame;"
		"uniform float pitch; uniform vec2 step1;"
		"void main(){ if(pitch<=0.0){"
		"  vec3 c=texture2D(frame,uv).rgb;"
		"  vec3 b=texture2D(frame,uv+vec2(step1.x,0.)).rgb+texture2D(frame,uv-vec2(step1.x,0.)).rgb"
		"    +texture2D(frame,uv+vec2(0.,step1.y)).rgb+texture2D(frame,uv-vec2(0.,step1.y)).rgb;"
		"  gl_FragColor=vec4(clamp(c+(c-b*.25)*.5,0.,1.),1.); return; }"
		/* DotMatrix: a lattice of dots, each as bright as what is under it */
		" vec2 cell=floor(gl_FragCoord.xy/pitch); vec2 mid=(cell+0.5)*pitch;"
		" float r=length(gl_FragCoord.xy-mid)/(pitch*0.5);"
		" vec2 p=uv+(mid-gl_FragCoord.xy)*vec2(dFdx(uv.x)+dFdy(uv.x),dFdx(uv.y)+dFdy(uv.y));"
		" vec3 c=texture2D(frame,p).rgb; float y=dot(c,vec3(0.3,0.59,0.11));"
		" gl_FragColor=vec4(vec3(y*(1.0-smoothstep(0.55,0.8,r))),1.0); }";
	GLuint p = raw_program(vs, fs), d = raw_program(vs, fd);
	if (!p || !d) {
		raw.prog = -1;
		return false;
	}
	raw.prog = p;
	raw.u_size = glGetUniformLocation(p, "size");
	raw.u_unpack = glGetUniformLocation(p, "unpack");
	raw.u_first = glGetUniformLocation(p, "first");
	raw.u_gain = glGetUniformLocation(p, "gain");
	raw.u_ccm = glGetUniformLocation(p, "ccm");
	raw.u_black = glGetUniformLocation(p, "black");
	raw.u_shade = glGetUniformLocation(p, "shade");
	raw.disp = d;
	raw.d_pitch = glGetUniformLocation(d, "pitch");
	raw.d_step = glGetUniformLocation(d, "step1");
	return true;
}

static void raw_tex_params(GLenum filter) {
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/* This frame buffer as a texture with no copy, once; 16-bit red, or two 8-bit channels. */
static bool raw_import(unsigned i) {
	static PFNEGLCREATEIMAGEKHRPROC create;
	static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC target;
	if (raw.img[i]) {
		return true;
	}
	if (!create) {
		create = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
		target = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
	}
	if (raw.dmabuf < 0 || !create || !target) {
		return false;
	}
	static const struct { uint32_t fourcc; float unpack[4]; } formats[] = {
		{ FOURCC('R', '1', '6', ' '), { 65535.f / 1023, 0, 0, 0 } },
		{ FOURCC('G', 'R', '8', '8'), { 255.f / 1023, 65280.f / 1023, 0, 0 } },
	};
	for (unsigned f = 0; f < sizeof(formats) / sizeof(formats[0]); f++) {
		EGLint at[] = {
			EGL_WIDTH, (EGLint)raw.info.w, EGL_HEIGHT, (EGLint)raw.info.h,
			EGL_LINUX_DRM_FOURCC_EXT, (EGLint)formats[f].fourcc,
			EGL_DMA_BUF_PLANE0_FD_EXT, raw.dmabuf,
			EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)(i * raw.info.area),
			EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)raw.info.stride,
			EGL_NONE,
		};
		EGLImageKHR im = create(eglGetCurrentDisplay(), EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, at);
		if (im == EGL_NO_IMAGE_KHR) {
			continue;
		}
		if (!raw.tex[i]) {
			GLuint t;
			glGenTextures(1, &t);
			raw.tex[i] = t;
		}
		glBindTexture(GL_TEXTURE_2D, raw.tex[i]);
		raw_tex_params(GL_NEAREST);
		while (glGetError() != GL_NO_ERROR) {
		}
		target(GL_TEXTURE_2D, im);
		if (glGetError() != GL_NO_ERROR) {
			continue;
		}
		raw.img[i] = im;
		fprintf(stderr, "ztcamera: frame %u read directly as %.4s\n", i, (const char *)&formats[f].fourcc);
		memcpy(raw.unpack, formats[f].unpack, sizeof(raw.unpack));
		raw.zero_copy = true;
		return true;
	}
	return false;
}

/* The fallback if the driver cannot import it: a copy, low byte and high byte. */
static void raw_upload(const uint8_t *base) {
	static int tw, th;
	if (!raw.upload_tex) {
		GLuint t;
		glGenTextures(1, &t);
		raw.upload_tex = t;
		glBindTexture(GL_TEXTURE_2D, t);
		raw_tex_params(GL_NEAREST);
	}
	glBindTexture(GL_TEXTURE_2D, raw.upload_tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
	if (tw != (int)raw.info.w || th != (int)raw.info.h) {
		tw = raw.info.w;
		th = raw.info.h;
		glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, tw, th, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, base);
	} else {
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tw, th, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, base);
	}
	raw.unpack[0] = 255.f / 1023;
	raw.unpack[1] = raw.unpack[2] = 0;
	raw.unpack[3] = 65280.f / 1023;
	raw.zero_copy = false;
}

/* The newest frame through the whole pipeline into the output texture. */
static void raw_develop(void) {
	unsigned i = raw.info.index;
	const uint8_t *base = (const uint8_t *)raw.map + i * (size_t)raw.info.area;
	glActiveTexture(GL_TEXTURE0);
	if (i < RAW_BUFS && raw_import(i)) {
		glBindTexture(GL_TEXTURE_2D, raw.tex[i]);
	} else {
		raw_upload(base);
	}
	int w = raw.info.w, h = raw.info.h;
	GLint old_fbo = 0, vp[4];
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fbo);
	glGetIntegerv(GL_VIEWPORT, vp);
	GLboolean blend = glIsEnabled(GL_BLEND), scissor = glIsEnabled(GL_SCISSOR_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_SCISSOR_TEST);
	if (!raw.fbo) {
		GLuint f;
		glGenFramebuffers(1, &f);
		raw.fbo = f;
	}
	if (raw.out_w != w || raw.out_h != h) {
		GLint held;
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &held);
		if (!raw.out_tex) {
			GLuint t;
			glGenTextures(1, &t);
			raw.out_tex = t;
		}
		glBindTexture(GL_TEXTURE_2D, raw.out_tex);
		raw_tex_params(GL_LINEAR);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		raw.out_w = w;
		raw.out_h = h;
		glBindTexture(GL_TEXTURE_2D, held);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, raw.fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, raw.out_tex, 0);
	glViewport(0, 0, w, h);
	glUseProgram(raw.prog);
	glUniform2f(raw.u_size, (float)w, (float)h);
	glUniform4fv(raw.u_unpack, 1, raw.unpack);
	/* rear GB/RG: red at x 0, y 1. Front GR/BG: red at x 1, y 0. */
	if (raw.info.bayer) {
		glUniform2f(raw.u_first, 1, 0);
	} else {
		glUniform2f(raw.u_first, 0, 1);
	}
	/* no gain below 1, or a clipped white would come out tinted */
	float low = fmin(fmin(raw.gain[0], raw.gain[1]), raw.gain[2]);
	glUniform3f(raw.u_gain, raw.gain[0] / low, raw.gain[1] / low, raw.gain[2] / low);
	const float m[9] = { raw.ccm[0], raw.ccm[3], raw.ccm[6], raw.ccm[1], raw.ccm[4], raw.ccm[7],
		raw.ccm[2], raw.ccm[5], raw.ccm[8] };	/* GL is column first */
	glUniformMatrix3fv(raw.u_ccm, 1, GL_FALSE, m);
	glUniform1f(raw.u_black, RAW_BLACK / (float)RAW_WHITE);
	/* no calibration read from the module's memory: a mild fall-off for this lens size */
	glUniform1f(raw.u_shade, raw.cam == RAW_FRONT ? 1.2f : 1.4f);
	static const float q[16] = { -1, -1, 0, 0, 1, -1, 1, 0, -1, 1, 0, 1, 1, 1, 1, 1 };
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), q);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), q + 2);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glBindFramebuffer(GL_FRAMEBUFFER, old_fbo);
	glViewport(vp[0], vp[1], vp[2], vp[3]);
	if (blend) {
		glEnable(GL_BLEND);
	}
	if (scissor) {
		glEnable(GL_SCISSOR_TEST);
	}
}

/*
 * Over the whole window, cropped to fill. The sensors lie on their sides, by
 * stock's orientation: rear 90, front 270, so the front's picture is the rear's
 * turned half round. The front is then mirrored like a mirror, as stock previews it;
 * that leaves its v the same as the rear's and its u reversed.
 * The core's own program and buffers come back afterwards; every core draw sets
 * its texture and vertices again.
 */
static void raw_draw(void) {
	if (!raw.seq || !raw_gl()) {
		return;
	}
	GLint old = 0;
	glGetIntegerv(GL_CURRENT_PROGRAM, &old);
	if (raw.drawn != raw.seq || raw.out_w != (int)raw.info.w) {
		raw_develop();
		raw.drawn = raw.seq;
	}
	glUseProgram(raw.disp);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, raw.out_tex);
	double span = fmin(1, ((double)zmc.width / zmc.height) * ((double)raw.info.w / raw.info.h));
	float v0 = 0.5f - span / 2, v1 = 0.5f + span / 2;
	bool front = raw.cam == RAW_FRONT;
	float top = front ? 1 : 0, bottom = front ? 0 : 1;
	/* position x,y, then the frame's u,v */
	const float q[16] = {
		-1,  1, top, v1,
		 1,  1, top, v0,
		-1, -1, bottom, v1,
		 1, -1, bottom, v0,
	};
	glUniform1f(raw.d_pitch, zmc.style == ZMC_STYLE_DOTMATRIX ? (float)(zmg_dot_pitch * zmc.scale) : 0.0f);
	glUniform2f(raw.d_step, 1.0f / raw.info.w, 1.0f / raw.info.h);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), q);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), q + 2);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glUseProgram(old);
}

/*
 * A photo: the sensor at full size for a moment, developed by the same shaders,
 * upright, the front mirrored as in the viewfinder. Falls back to the preview's size if the full mode gives no frame. Returns a new
 * surface or NULL.
 */
static cairo_surface_t *raw_capture(void) {
	if (!raw_gl() || raw.fd < 0) {
		return NULL;
	}
	bool front = raw.cam == RAW_FRONT;
	int c = raw.coarse, g = raw.again, focus = raw.af_best;
	bool full = raw_say(front ? "frontfull" : "rearfull");
	raw_images_free();
	if (full) {
		/* the same time in lines: preview's line length over the full mode's */
		double k = front ? (double)0x2970 / 0x13D0 : (double)0x0528 / 0x0A50;
		int cf = (int)(c * k);
		(void)read(raw.fd, &raw.info, sizeof(raw.info));
		raw_set_exposure(cf < 2 ? 2 : cf, g);
		raw_set_focus(focus);
		double until = raw_now() + 4;
		uint32_t first = raw.info.seq;
		full = false;
		while (raw_now() < until) {
			struct timespec nap = { 0, 15000000 };
			nanosleep(&nap, NULL);
			if (read(raw.fd, &raw.info, sizeof(raw.info)) != sizeof(raw.info) || raw.info.stopped) {
				break;
			}
			if (raw.info.seq >= first + 5) {
				full = true;
				break;
			}
		}
	}
	if (!full) {
		fprintf(stderr, "ztcamera: no full-size frame (stopped %u, errors %u)\n", raw.info.stopped, raw.info.errors);
	}
	if (!full) {	/* back to preview, then its next frame */
		raw_say(front ? "front" : "rear");
		raw_images_free();
		struct timespec nap = { 0, 300000000 };
		nanosleep(&nap, NULL);
		if (read(raw.fd, &raw.info, sizeof(raw.info)) != sizeof(raw.info) || !raw.info.seq) {
			return NULL;
		}
		raw_set_exposure(c, g);
		raw_set_focus(focus);
	}
	raw_develop();
	raw.drawn = 0;
	int w = raw.info.w, h = raw.info.h;
	uint8_t *px = malloc((size_t)w * h * 4);
	cairo_surface_t *sf = px ? cairo_image_surface_create(CAIRO_FORMAT_ARGB32, h, w) : NULL;
	if (px && cairo_surface_status(sf) == CAIRO_STATUS_SUCCESS) {
		GLint old_fbo = 0;
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, raw.fbo);
		glPixelStorei(GL_PACK_ALIGNMENT, 4);
		glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
		glBindFramebuffer(GL_FRAMEBUFFER, old_fbo);
		uint8_t *d = cairo_image_surface_get_data(sf);
		int st = cairo_image_surface_get_stride(sf);
		cairo_surface_flush(sf);
		/* the same mapping as raw_draw: screen row y is the frame's u, screen column x its v */
		for (int y = 0; y < w; y++) {
			uint32_t *row = (uint32_t *)(d + (size_t)y * st);
			int u = front ? w - 1 - y : y;
			for (int x = 0; x < h; x++) {
				const uint8_t *s = px + ((size_t)(h - 1 - x) * w + u) * 4;
				row[x] = 0xff000000u | (uint32_t)s[0] << 16 | (uint32_t)s[1] << 8 | s[2];
			}
		}
		cairo_surface_mark_dirty(sf);
	} else if (sf) {
		cairo_surface_destroy(sf);
		sf = NULL;
	}
	free(px);
	if (full) {	/* back to the viewfinder */
		raw_say(front ? "front" : "rear");
		raw_images_free();
		raw.seq = 0;
		if (read(raw.fd, &raw.info, sizeof(raw.info)) == sizeof(raw.info)) {
			raw_set_exposure(c, g);
			raw_set_focus(focus);
			raw.coarse = c;
			raw.again = g;
		}
	}
	return sf;
}
