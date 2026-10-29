/*
 * zrn-gnss-nmea: the stock Broadcom GNSS HAL (gps.default.so) driven from a
 * tiny bionic program, its NMEA handed to Linux.
 *
 * Runs inside /run/android (zrn-android-root), next to lhd and the vendor's
 * gpsd.  Chain: BCM4773 -> zrn-bbd /dev/ttyBCM -> lhd -> named pipe
 * /data/vendor/gps/.pipe.gpsd_to_lhd -> gpsd (Broadcom GLL engine) -> pipes
 * /data/vendor/gps/.gps.interface.pipe.to_{gpsd,jni} -> gps.default.so (this
 * process) -> nmea_cb -> TCP 127.0.0.1:10110 and a Unix socket, for Linux
 * gpsd / geoclue.
 *
 * Built without the NDK: no bionic headers, so everything this file needs
 * from libc is declared below with bionic's arm64 ABI.  See build.sh.
 * Layouts (sensors/GPS-REA.md): GpsInterface size 0x50 at 0x3c1a8 of
 * gps.default.so; GpsCallbacks is AOSP's (nmea_cb at +0x20, location_cb at +0x8).
 *
 * Start-up aiding: -t injects the host's clock (only passed when it is NTP
 * synchronised), -l FILE the last fix ("lat lon accuracy_m unix_time", kept
 * by ztos-gnss-gate), with an uncertainty that grows with its age.
 * On SIGTERM the engine is stopped through the HAL and the HAL closed before
 * exiting, after which gpsd exits by itself; killed together with us, it
 * now and then crashed on its way out.
 *
 * Accuracy: NMEA from this engine has no error estimate (no GST), so each
 * fix that carries one (GPS_LOCATION_HAS_ACCURACY) also goes out as
 * $PZTFX,accuracy_m,, (horizontal, metres; the two empty fields are kept for
 * vertical and speed accuracy, which GpsLocation does not have). It holds no
 * position. Readers that do not know it skip it, as with any P sentence.
 *
 * Readers get whole sentences only: what a slow reader cannot take yet waits
 * in its own backlog (BACKLOG bytes), and a reader that falls further behind
 * is dropped.
 */
typedef unsigned long size_t;
typedef long ssize_t;
typedef long int64_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef long pthread_t;

/* ---- bionic, declared by hand ---- */
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern char *dlerror(void);
extern int pthread_create(pthread_t *, const void *, void *(*)(void *), void *);
extern int pthread_mutex_lock(void *);
extern int pthread_mutex_unlock(void *);
extern int socket(int, int, int);
extern int bind(int, const void *, unsigned);
extern int listen(int, int);
extern int accept4(int, void *, void *, int);
extern int setsockopt(int, int, int, const void *, unsigned);
extern ssize_t send(int, const void *, size_t, int);
extern int close(int);
extern int unlink(const char *);
extern int chmod(const char *, unsigned);
extern int dprintf(int, const char *, ...);
extern int sleep(unsigned);
extern int strcmp(const char *, const char *);
extern size_t strlen(const char *);
extern char *strncpy(char *, const char *, size_t);
extern void exit(int);
extern int *__errno(void);
extern void __libc_init(void *, void (*)(void), int (*)(int, char **, char **), const void *);
extern void *signal(int, void (*)(int));
extern int clock_gettime(int, void *);
extern int open(const char *, int, ...);
extern ssize_t read(int, void *, size_t);
extern double strtod(const char *, char **);
extern int usleep(unsigned);
extern int snprintf(char *, size_t, const char *, ...);

#define RTLD_NOW 2
#define AF_UNIX 1
#define AF_INET 2
#define SOCK_STREAM 1
#define SOCK_CLOEXEC 02000000
#define SOCK_NONBLOCK 04000
#define SOL_SOCKET 1
#define SO_REUSEADDR 2
#define MSG_NOSIGNAL 0x4000
#define MSG_DONTWAIT 0x40
#define EAGAIN 11
#define GPS_LOCATION_HAS_ACCURACY 0x0010
#define SIGINT 2
#define SIGTERM 15
#define CLOCK_REALTIME 0
#define CLOCK_BOOTTIME 7
#define O_RDONLY 0
struct timespec { long tv_sec, tv_nsec; };

/* ---- program entry without crtbegin_dynamic.o ---- */
static const void *structors[6];	/* preinit, init, fini (+ counts on newer bionic): all empty */
int main(int, char **, char **);
__attribute__((used)) static void start_c(void *raw)
{
	__libc_init(raw, 0, main, structors);
}
__asm__(".text\n.global _start\n_start:\n mov x0, sp\n b start_c\n");

/* ---- AOSP gps.h, the parts used ---- */
typedef struct {
	size_t size;
	uint16_t flags;
	double latitude, longitude, altitude;
	float speed, bearing, accuracy;
	int64_t timestamp;
} GpsLocation;

typedef struct {
	size_t size;
	void (*location_cb)(GpsLocation *);
	void (*status_cb)(void *);
	void (*sv_status_cb)(void *);
	void (*nmea_cb)(int64_t, const char *, int);
	void (*set_capabilities_cb)(uint32_t);
	void (*acquire_wakelock_cb)(void);
	void (*release_wakelock_cb)(void);
	pthread_t (*create_thread_cb)(const char *, void (*)(void *), void *);
	void (*request_utc_time_cb)(void);
	void (*set_system_info_cb)(void *);
	void (*gnss_sv_status_cb)(void *);
} GpsCallbacks;				/* 0x60 */

typedef struct {
	size_t size;
	int (*init)(GpsCallbacks *);
	int (*start)(void);
	int (*stop)(void);
	void (*cleanup)(void);
	int (*inject_time)(int64_t, int64_t, int);
	int (*inject_location)(double, double, float);
	void (*delete_aiding_data)(uint16_t);
	/* Samsung/Broadcom: one extra bool (proxy__gps_set_position_mode(jjjjjb)) */
	int (*set_position_mode)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, int);
	const void *(*get_extension)(const char *);
} GpsInterface;				/* 0x50 */

/* ---- NMEA fan-out ---- */
#define MAXC 16
#define BACKLOG 65536
static int clients[MAXC];
static char backlog[MAXC][BACKLOG];	/* what each reader has not taken yet */
static int queued[MAXC];
static long lock[8];			/* pthread_mutex_t, 40 bytes on arm64 bionic; zero = default */
static int debug;

static void drop(int i)
{
	close(clients[i]);
	clients[i] = 0;
	queued[i] = 0;
}

/* send what reader i takes of buf; -1: it is gone; else the bytes sent */
static int put(int i, const char *buf, int n)
{
	int done = 0;
	while (done < n) {
		ssize_t k = send(clients[i], buf + done, n - done, MSG_NOSIGNAL | MSG_DONTWAIT);
		if (k < 0)
			return *__errno() == EAGAIN ? done : -1;
		done += (int)k;
	}
	return done;
}

/* one whole sentence (or more) to every reader; called with the lock held */
static void fanout_locked(const char *s, int n)
{
	for (int i = 0; i < MAXC; i++) {
		if (clients[i] <= 0) continue;
		if (queued[i]) {
			int k = put(i, backlog[i], queued[i]);
			if (k < 0) { drop(i); continue; }
			for (int j = k; j < queued[i]; j++) backlog[i][j - k] = backlog[i][j];
			queued[i] -= k;
		}
		int k = queued[i] ? 0 : put(i, s, n);
		if (k < 0) { drop(i); continue; }
		if (n - k > BACKLOG - queued[i]) {
			dprintf(2, "zrn-gnss-nmea: a reader fell too far behind; dropped it\n");
			drop(i);
			continue;
		}
		for (int j = k; j < n; j++) backlog[i][queued[i]++] = s[j];
	}
}

static void fanout(const char *s, int n)
{
	pthread_mutex_lock(lock);
	fanout_locked(s, n);
	pthread_mutex_unlock(lock);
}

static void *acceptor(void *arg)
{
	int ls = (int)(long)arg;
	for (;;) {
		int c = accept4(ls, 0, 0, SOCK_CLOEXEC | SOCK_NONBLOCK);
		if (c < 0) { sleep(1); continue; }
		pthread_mutex_lock(lock);
		int i;
		for (i = 0; i < MAXC && clients[i] > 0; i++);
		if (i < MAXC) { clients[i] = c; queued[i] = 0; } else close(c);
		pthread_mutex_unlock(lock);
	}
	return 0;
}

static int listen_tcp(int port)
{
	struct { uint16_t fam, port; uint32_t addr; char z[8]; } a = { AF_INET, 0, 0, {0} };
	int one = 1, s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	a.port = (uint16_t)((port >> 8) | ((port & 0xff) << 8));
	a.addr = 0x0100007f;		/* 127.0.0.1 */
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	if (s < 0 || bind(s, &a, sizeof a) || listen(s, 4)) return -1;
	return s;
}

static int listen_unix(const char *path)
{
	struct { uint16_t fam; char path[108]; } a = { AF_UNIX, {0} };
	int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	strncpy(a.path, path, sizeof a.path - 1);
	unlink(path);
	if (s < 0 || bind(s, &a, sizeof a) || listen(s, 4)) return -1;
	chmod(path, 0666);
	return s;
}

/* ---- HAL callbacks ---- */
static void cb_nmea(int64_t ts, const char *s, int n)
{
	(void)ts;
	if (n <= 0) return;
	/* the sentence and its line end in one piece */
	char b[512];
	if (s[n - 1] != '\n' && n + 2 <= (int)sizeof b) {
		for (int i = 0; i < n; i++) b[i] = s[i];
		b[n] = '\r'; b[n + 1] = '\n';
		fanout(b, n + 2);
	} else {
		pthread_mutex_lock(lock);
		fanout_locked(s, n);
		if (s[n - 1] != '\n') fanout_locked("\r\n", 2);
		pthread_mutex_unlock(lock);
	}
	if (debug) dprintf(2, "%.*s%s", n, s, s[n - 1] == '\n' ? "" : "\n");
}
static int fixes;
static void cb_location(GpsLocation *l)
{
	/* layout of broadcom::GpsLocation assumed AOSP; diagnostic only. The
	 * journal gets the first fix of a session without its position (the
	 * owner's whereabouts do not belong in logs); -d prints every fix. */
	if (!l) return;
	if ((l->flags & GPS_LOCATION_HAS_ACCURACY) && l->accuracy > 0 && l->accuracy < 100000) {
		char body[48], out[64];
		int n = snprintf(body, sizeof body, "PZTFX,%.1f,,", (double)l->accuracy);
		if (n > 0 && n < (int)sizeof body) {
			unsigned x = 0;
			for (int i = 0; i < n; i++) x ^= (unsigned char)body[i];
			n = snprintf(out, sizeof out, "$%s*%02X\r\n", body, x);
			if (n > 0 && n < (int)sizeof out) fanout(out, n);
		}
	}
	if (debug)
		dprintf(2, "zrn-gnss-nmea: fix flags=0x%x lat=%f lon=%f acc=%f\n",
			l->flags, l->latitude, l->longitude, (double)l->accuracy);
	else if (!fixes++)
		dprintf(2, "zrn-gnss-nmea: first fix, accuracy %d m%s\n", (int)l->accuracy,
			l->flags & GPS_LOCATION_HAS_ACCURACY ? "" : " (flag not set: no estimate)");
}
static volatile int engine_status;	/* GPS_STATUS_*: 1 session begin, 2 end, 3 engine on, 4 off */
static void cb_status(void *st)
{
	if (!st) return;
	engine_status = ((uint16_t *)st)[4];
	dprintf(2, "zrn-gnss-nmea: status %d\n", engine_status);
}
static void cb_void(void) {}
static void cb_ptr(void *p) { (void)p; }
static void cb_caps(uint32_t c) { dprintf(2, "zrn-gnss-nmea: capabilities 0x%x\n", c); }

struct trampo { void (*fn)(void *); void *arg; };
static struct trampo tr[16];
static int ntr;
static void *thread_main(void *p) { struct trampo *t = p; t->fn(t->arg); return 0; }
static pthread_t cb_thread(const char *name, void (*fn)(void *), void *arg)
{
	pthread_t t = 0;
	dprintf(2, "zrn-gnss-nmea: HAL thread %s\n", name ? name : "?");
	if (ntr >= 16) return 0;
	tr[ntr].fn = fn; tr[ntr].arg = arg;
	pthread_create(&t, 0, thread_main, &tr[ntr++]);
	return t;
}

static GpsCallbacks cbs = {
	sizeof(GpsCallbacks), cb_location, cb_status, cb_ptr, cb_nmea, cb_caps,
	cb_void, cb_void, cb_thread, cb_void, cb_ptr, cb_ptr,
};

static volatile int stopping;
static void on_term(int sig) { (void)sig; stopping = 1; }

static int64_t clock_ms(int clock)
{
	struct timespec ts;
	clock_gettime(clock, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* The last fix, "lat lon accuracy_m unix_time": aiding for a hot start. */
static void inject_last(const GpsInterface *g, const char *path)
{
	char b[160];
	int fd = open(path, O_RDONLY), n;
	if (fd < 0) return;
	n = (int)read(fd, b, sizeof b - 1);
	close(fd);
	if (n <= 0) return;
	b[n] = 0;
	char *e;
	double lat = strtod(b, &e), lon = strtod(e, &e), acc = strtod(e, &e), t = strtod(e, &e);
	double age = (double)clock_ms(CLOCK_REALTIME) / 1000.0 - t;
	if (!(lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180) || (lat == 0 && lon == 0)) return;
	if (age < 0) age = 0;
	/* moved since then at up to 30 m/s (a car); at most 300 km */
	acc += age * 30;
	if (acc > 300000) acc = 300000;
	if (acc < 50) acc = 50;
	g->inject_location(lat, lon, (float)acc);
	dprintf(2, "zrn-gnss-nmea: injected the last fix, %ld s old, +-%ld m\n", (long)age, (long)acc);
}

int main(int argc, char **argv, char **envp)
{
	const char *hal = "/vendor/lib64/hw/gps.default.so";
	const char *usock = "/data/vendor/gps/ztos-nmea.sock";
	const char *last = 0;
	int port = 10110, inject_clock = 0;
	(void)envp;
	/* -p PORT -u SOCKET: ztos-gnss-gate owns the public ones and starts us on demand */
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-d")) debug = 1;
		else if (!strcmp(argv[i], "-p") && i + 1 < argc) {
			port = 0;
			for (const char *c = argv[++i]; *c >= '0' && *c <= '9'; c++) port = port * 10 + *c - '0';
		} else if (!strcmp(argv[i], "-u") && i + 1 < argc) usock = argv[++i];
		else if (!strcmp(argv[i], "-l") && i + 1 < argc) last = argv[++i];
		else if (!strcmp(argv[i], "-t")) inject_clock = 1;
	}
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);

	int t = listen_tcp(port), u = listen_unix(usock);
	if (t < 0 && u < 0) { dprintf(2, "zrn-gnss-nmea: no listening socket\n"); return 1; }
	pthread_t th;
	if (t >= 0) pthread_create(&th, 0, acceptor, (void *)(long)t);
	if (u >= 0) pthread_create(&th, 0, acceptor, (void *)(long)u);

	void *h = dlopen(hal, RTLD_NOW);
	if (!h) { dprintf(2, "zrn-gnss-nmea: dlopen: %s\n", dlerror()); return 1; }
	const GpsInterface *(*get)(void) = (const GpsInterface *(*)(void))dlsym(h, "gps_proxy_get_gps_interface");
	if (!get) { dprintf(2, "zrn-gnss-nmea: no gps_proxy_get_gps_interface\n"); return 1; }
	const GpsInterface *g = get();
	if (!g || g->size != sizeof(GpsInterface)) {
		dprintf(2, "zrn-gnss-nmea: unexpected GpsInterface size %ld\n", g ? (long)g->size : -1L);
		return 1;
	}
	if (g->init(&cbs)) { dprintf(2, "zrn-gnss-nmea: init failed (is gpsd up?)\n"); return 1; }
	/* the host's clock (NTP), +-1 s; gps.xml must not set IgnoreJniTime */
	if (inject_clock) {
		g->inject_time(clock_ms(CLOCK_REALTIME), clock_ms(CLOCK_BOOTTIME), 1000);
		dprintf(2, "zrn-gnss-nmea: injected the time\n");
	}
	if (last) inject_last(g, last);
	/* standalone, periodic, 1 s; no SUPL (no RIL in ztOS) */
	g->set_position_mode(0, 0, 1000, 0, 0, 0);
	if (g->start()) { dprintf(2, "zrn-gnss-nmea: start failed\n"); return 1; }
	dprintf(2, "zrn-gnss-nmea: started; NMEA on 127.0.0.1:%d and %s\n", port, usock);
	while (!stopping) sleep(1);
	/* stop the engine so that gpsd saves what it knows, then let it go */
	g->stop();
	for (int i = 0; i < 50 && engine_status != 4; i++) usleep(100000);
	dprintf(2, "zrn-gnss-nmea: stopped (engine %s)\n", engine_status == 4 ? "off" : "still on");
	g->cleanup();
	return 0;
}
