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
static int clients[MAXC];
static long lock[8];			/* pthread_mutex_t, 40 bytes on arm64 bionic; zero = default */
static int debug;

static void fanout(const char *s, int n)
{
	pthread_mutex_lock(lock);
	for (int i = 0; i < MAXC; i++)
		if (clients[i] > 0 && send(clients[i], s, n, MSG_NOSIGNAL | MSG_DONTWAIT) < 0 &&
		    *__errno() != 11 /* EAGAIN: slow reader loses a line */) {
			close(clients[i]);
			clients[i] = 0;
		}
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
		if (i < MAXC) clients[i] = c; else close(c);
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
	fanout(s, n);
	if (s[n - 1] != '\n') fanout("\r\n", 2);
	if (debug) dprintf(2, "%.*s%s", n, s, s[n - 1] == '\n' ? "" : "\n");
}
static void cb_location(GpsLocation *l)
{
	/* layout of broadcom::GpsLocation assumed AOSP; diagnostic only */
	if (l) dprintf(2, "zrn-gnss-nmea: fix flags=0x%x lat=%f lon=%f acc=%f\n",
		       l->flags, l->latitude, l->longitude, (double)l->accuracy);
}
static void cb_status(void *st)
{
	if (st) dprintf(2, "zrn-gnss-nmea: status %d\n", (int)((uint16_t *)st)[4]);
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

int main(int argc, char **argv, char **envp)
{
	const char *hal = "/vendor/lib64/hw/gps.default.so";
	const char *usock = "/data/vendor/gps/ztos-nmea.sock";
	int port = 10110;
	(void)envp;
	for (int i = 1; i < argc; i++)
		if (!strcmp(argv[i], "-d")) debug = 1;

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
	/* standalone, periodic, 1 s; no SUPL (no RIL in ztOS) */
	g->set_position_mode(0, 0, 1000, 0, 0, 0);
	if (g->start()) { dprintf(2, "zrn-gnss-nmea: start failed\n"); return 1; }
	dprintf(2, "zrn-gnss-nmea: started; NMEA on 127.0.0.1:%d and %s\n", port, usock);
	for (;;) sleep(3600);
}
