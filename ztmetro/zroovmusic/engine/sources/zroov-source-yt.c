/*
 * zroov-source-yt: YouTube source helper for zroovd. See ../../ENGINE.md.
 *
 * resolve has a fast path. YouTube's player API, asked as the visionOS client
 * with an anonymous visitor ID, returns plain stream links in one HTTPS request:
 * no watch page, no player script to run. yt-dlp, which does all of that in
 * Python, is the fallback, and does search and fetch.
 */
#define _GNU_SOURCE
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define API "https://youtubei.googleapis.com/youtubei/v1/"
#define CLIENT_VERSION "1.04"
#define USER_AGENT "com.google.visionos.youtube/1.04(RealityDevice17,1; U; CPU visionOS 26_6_0 like Mac OS X; US)"
#define VISITOR_MAX_AGE (24 * 3600)

struct buf { char *p; size_t n; };

static size_t buf_write(char *d, size_t s, size_t m, void *u)
{
	struct buf *b = u;
	char *np = realloc(b->p, b->n + s * m + 1);
	if (!np)
		return 0;
	b->p = np;
	memcpy(b->p + b->n, d, s * m);
	b->n += s * m;
	b->p[b->n] = 0;
	return s * m;
}

static json_object *client_context(const char *visitor)
{
	json_object *c = json_object_new_object();
	static const char *kv[][2] = {
		{"clientName", "VISIONOS"}, {"clientVersion", CLIENT_VERSION},
		{"deviceMake", "Apple"}, {"deviceModel", "RealityDevice17,1"},
		{"osName", "visionOS"}, {"osVersion", "26.6.0.23O770"},
		{"platform", "MOBILE"}, {"clientScreen", "WATCH"}, {"hl", "en"}, {"gl", "US"},
	};
	for (size_t i = 0; i < sizeof(kv) / sizeof(kv[0]); i++)
		json_object_object_add(c, kv[i][0], json_object_new_string(kv[i][1]));
	if (visitor)
		json_object_object_add(c, "visitorData", json_object_new_string(visitor));
	json_object *ctx = json_object_new_object();
	json_object_object_add(ctx, "client", c);
	return ctx;
}

/* POST body to the API; the parsed reply, or NULL with *why set. */
static json_object *api_post(const char *path, json_object *body, const char *visitor, const char **why)
{
	char url[256], vh[1024];
	struct buf b = {0};
	json_object *r = NULL;
	long code = 0;
	snprintf(url, sizeof(url), API "%s", path);
	CURL *c = curl_easy_init();
	struct curl_slist *h = NULL;
	h = curl_slist_append(h, "Content-Type: application/json");
	h = curl_slist_append(h, "X-Goog-Api-Format-Version: 2");
	h = curl_slist_append(h, "X-Youtube-Client-Name: 101");
	h = curl_slist_append(h, "X-Youtube-Client-Version: " CLIENT_VERSION);
	if (visitor) {
		snprintf(vh, sizeof(vh), "X-Goog-Visitor-Id: %s", visitor);
		h = curl_slist_append(h, vh);
	}
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
	curl_easy_setopt(c, CURLOPT_USERAGENT, USER_AGENT);
	curl_easy_setopt(c, CURLOPT_POSTFIELDS, json_object_to_json_string_ext(body, JSON_C_TO_STRING_PLAIN));
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, buf_write);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 8L);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
	CURLcode rc = curl_easy_perform(c);
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	if (rc != CURLE_OK)
		*why = rc == CURLE_OPERATION_TIMEDOUT ? "timed out" : "network error";
	else if (code != 200)
		*why = "HTTP error";
	else if (!(r = json_tokener_parse(b.p ? b.p : "")))
		*why = "unreadable reply";
	curl_slist_free_all(h);
	curl_easy_cleanup(c);
	free(b.p);
	return r;
}

static void visitor_path(char *out, size_t n)
{
	const char *x = getenv("XDG_CACHE_HOME");
	if (x && *x)
		snprintf(out, n, "%s/zroovmusic/visitor", x);
	else
		snprintf(out, n, "%s/.cache/zroovmusic/visitor", getenv("HOME") ? getenv("HOME") : "/tmp");
}

/* The anonymous visitor ID: kept on disk, since a request needs one every time. */
static char *visitor_get(int fresh)
{
	char path[512], line[1024];
	struct stat st;
	visitor_path(path, sizeof(path));
	if (!fresh && !stat(path, &st) && time(NULL) - st.st_mtime < VISITOR_MAX_AGE) {
		FILE *f = fopen(path, "r");
		if (f) {
			char *got = fgets(line, sizeof(line), f);
			fclose(f);
			if (got && *line) {
				line[strcspn(line, "\n")] = 0;
				return strdup(line);
			}
		}
	}
	json_object *body = json_object_new_object();
	json_object_object_add(body, "context", client_context(NULL));
	const char *why = NULL;
	json_object *r = api_post("visitor_id?prettyPrint=false", body, NULL, &why);
	json_object_put(body);
	json_object *rc, *v;
	char *out = NULL;
	if (r && json_object_object_get_ex(r, "responseContext", &rc)
	    && json_object_object_get_ex(rc, "visitorData", &v))
		out = strdup(json_object_get_string(v));
	if (r)
		json_object_put(r);
	if (out) {
		char dir[512];
		snprintf(dir, sizeof(dir), "%s", path);
		*strrchr(dir, '/') = 0;
		char *up = strrchr(dir, '/');
		if (up) {	/* ~/.cache may not exist either */
			*up = 0;
			mkdir(dir, 0700);
			*up = '/';
		}
		mkdir(dir, 0700);
		FILE *f = fopen(path, "w");
		if (f) {
			fprintf(f, "%s\n", out);
			fclose(f);
		}
	}
	return out;
}

/* Ours is audio-only: Opus 251, else AAC 140, else the richest other audio. */
static const char *pick_audio(json_object *adaptive)
{
	const char *best = NULL;
	int best_rank = -1;
	long best_rate = -1;
	size_t n = adaptive ? json_object_array_length(adaptive) : 0;
	for (size_t i = 0; i < n; i++) {
		json_object *f = json_object_array_get_idx(adaptive, i), *x;
		if (!json_object_object_get_ex(f, "mimeType", &x)
		    || strncmp(json_object_get_string(x), "audio/", 6))
			continue;
		if (!json_object_object_get_ex(f, "url", &x))
			continue;
		const char *u = json_object_get_string(x);
		int itag = json_object_object_get_ex(f, "itag", &x) ? json_object_get_int(x) : 0;
		long rate = json_object_object_get_ex(f, "bitrate", &x) ? json_object_get_int64(x) : 0;
		int rank = itag == 251 ? 2 : itag == 140 ? 1 : 0;
		if (rank > best_rank || (rank == best_rank && rate > best_rate)) {
			best = u;
			best_rank = rank;
			best_rate = rate;
		}
	}
	return best;
}

static char *fast_link;		/* what the fast path found */
static char fast_ext[8];

static int fast_attempt(const char *id, const char *visitor, const char **why)
{
	static const char cs[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	unsigned char rnd[16];
	char cpn[17], path[128];
	if (getrandom(rnd, sizeof(rnd), 0) != sizeof(rnd))
		memset(rnd, 7, sizeof(rnd));
	for (int i = 0; i < 16; i++)
		cpn[i] = cs[rnd[i] & 63];
	cpn[16] = 0;
	json_object *body = json_object_new_object();
	json_object_object_add(body, "videoId", json_object_new_string(id));
	json_object_object_add(body, "cpn", json_object_new_string(cpn));
	json_object_object_add(body, "contentCheckOk", json_object_new_boolean(1));
	json_object_object_add(body, "racyCheckOk", json_object_new_boolean(1));
	json_object_object_add(body, "context", client_context(visitor));
	snprintf(path, sizeof(path), "player?prettyPrint=false&id=%s", id);
	json_object *r = api_post(path, body, visitor, why);
	json_object_put(body);
	if (!r)
		return -1;
	int ok = -1;
	json_object *st, *x, *sd, *ad;
	if (!json_object_object_get_ex(r, "playabilityStatus", &st)
	    || !json_object_object_get_ex(st, "status", &x)) {
		*why = "unreadable reply";
	} else if (strcmp(json_object_get_string(x), "OK")) {
		*why = "not playable";
		fprintf(stderr, "fast: status %s\n", json_object_get_string(x));
	} else if (!json_object_object_get_ex(r, "streamingData", &sd)
		   || !json_object_object_get_ex(sd, "adaptiveFormats", &ad)) {
		*why = "no streams";
	} else {
		const char *u = pick_audio(ad);
		if (!u) {
			*why = "no plain links";
		} else {
			free(fast_link);
			if (asprintf(&fast_link, "%s&cpn=%s", u, cpn) < 0)
				fast_link = NULL;
			snprintf(fast_ext, sizeof(fast_ext), "%s", strstr(u, "mime=audio%2Fwebm") ? "webm" : "m4a");
			ok = fast_link ? 0 : -1;
		}
	}
	json_object_put(r);
	return ok;
}

static int fast_resolve(const char *id)
{
	const char *why = "no visitor ID";
	char *v = visitor_get(0);
	if (v && !fast_attempt(id, v, &why)) {
		free(v);
		return 0;
	}
	/* a stale or rejected visitor ID is the usual failure: one fresh try */
	free(v);
	v = visitor_get(1);
	int rc = -1;
	if (v)
		rc = fast_attempt(id, v, &why);
	free(v);
	if (rc)
		fprintf(stderr, "fast path gave up: %s\n", why);
	return rc;
}

static void stream_line(const char *url, const char *via, json_object *headers)
{
	json_object *o = json_object_new_object();
	const char *e = strstr(url, "expire=");
	json_object_object_add(o, "type", json_object_new_string("stream"));
	json_object_object_add(o, "url", json_object_new_string(url));
	json_object_object_add(o, "via", json_object_new_string(via));
	if (e)
		json_object_object_add(o, "expires", json_object_new_int64(atoll(e + 7)));
	if (headers)
		json_object_object_add(o, "headers", headers);
	puts(json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN));
	json_object_put(o);
}

static void error_line(const char *reason)
{
	json_object *o = json_object_new_object();
	json_object_object_add(o, "type", json_object_new_string("error"));
	json_object_object_add(o, "reason", json_object_new_string(reason));
	puts(json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN));
	json_object_put(o);
}

/* Runs yt-dlp with its stdout on a stream; *pid to wait on. */
static FILE *ytdlp(char *const argv[], pid_t *pid)
{
	int p[2];
	if (pipe2(p, O_CLOEXEC))
		return NULL;
	*pid = fork();
	if (*pid < 0)
		return NULL;
	if (!*pid) {
		dup2(p[1], 1);
		execvp("yt-dlp", argv);
		_exit(127);
	}
	close(p[1]);
	return fdopen(p[0], "r");
}

/*
 * The fallback. Without a JS runtime yt-dlp is left with ANDROID links, and those
 * answer 403 past the first megabyte for the adaptive formats: "bestaudio" gets a
 * link that cannot play, which is the old app's failure. Muxed format 18 serves
 * the whole file, so that is what is asked for, at about 96 kb/s AAC.
 */
#define YTDLP_FALLBACK "--extractor-args", "youtube:player_client=android", "-f", "18"

static int cmd_resolve(const char *id)
{
	if (!getenv("ZROOV_NO_FAST") && !fast_resolve(id)) {
		stream_line(fast_link, "fast", NULL);
		return 0;
	}
	if (getenv("ZROOV_PREFETCH")) {	/* not worth a Python start for a track not yet asked for */
		error_line("no prefetch");
		return 0;
	}
	/* links from yt-dlp may need the headers it chose */
	char url[128], line[8192] = "", hdr[4096] = "";
	snprintf(url, sizeof(url), "https://www.youtube.com/watch?v=%s", id);
	char *argv[] = {"yt-dlp", "-q", "--no-warnings", "--no-playlist", YTDLP_FALLBACK,
		"-O", "%(url)s", "-O", "%(http_headers)j", url, NULL};
	pid_t pid;
	FILE *f = ytdlp(argv, &pid);
	if (!f) {
		error_line("cannot run yt-dlp");
		return 0;
	}
	char *got = fgets(line, sizeof(line), f);
	char *goth = got ? fgets(hdr, sizeof(hdr), f) : NULL;
	fclose(f);
	waitpid(pid, NULL, 0);
	if (!got || strncmp(line, "http", 4)) {
		error_line("yt-dlp found no stream");
		return 0;
	}
	line[strcspn(line, "\n")] = 0;
	stream_line(line, "yt-dlp", goth ? json_tokener_parse(hdr) : NULL);
	return 0;
}

static int cmd_search(const char *q, const char *limit)
{
	char term[1024], line[1 << 16];
	snprintf(term, sizeof(term), "ytsearch%d:%s", atoi(limit) > 0 ? atoi(limit) : 8, q);
	char *argv[] = {"yt-dlp", "-q", "--no-warnings", "--flat-playlist", "-j", "--", term, NULL};
	pid_t pid;
	FILE *f = ytdlp(argv, &pid);
	if (!f) {
		error_line("cannot run yt-dlp");
		return 0;
	}
	while (fgets(line, sizeof(line), f)) {
		json_object *e = json_tokener_parse(line), *x;
		if (!e)
			continue;
		if (json_object_object_get_ex(e, "id", &x)) {
			char uri[128], art[160];
			const char *id = json_object_get_string(x);
			json_object *o = json_object_new_object();
			snprintf(uri, sizeof(uri), "yt:%s", id);
			snprintf(art, sizeof(art), "https://i.ytimg.com/vi/%s/hqdefault.jpg", id);
			json_object_object_add(o, "type", json_object_new_string("result"));
			json_object_object_add(o, "uri", json_object_new_string(uri));
			json_object_object_add(o, "art", json_object_new_string(art));
			if (json_object_object_get_ex(e, "title", &x))
				json_object_object_add(o, "title", json_object_get(x));
			if ((json_object_object_get_ex(e, "uploader", &x) && x)
			    || (json_object_object_get_ex(e, "channel", &x) && x))
				json_object_object_add(o, "artist", json_object_get(x));
			if (json_object_object_get_ex(e, "duration", &x) && x)
				json_object_object_add(o, "duration", json_object_get(x));
			puts(json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN));
			fflush(stdout);
			json_object_put(o);
		}
		json_object_put(e);
	}
	fclose(f);
	waitpid(pid, NULL, 0);
	puts("{\"type\":\"done\"}");
	return 0;
}

static int fetch_progress(void *u, curl_off_t total, curl_off_t now, curl_off_t ut, curl_off_t un)
{
	int *last = u;
	(void)ut; (void)un;
	int pct = total > 0 ? (int)(now * 100 / total) : 0;
	if (pct != *last) {
		*last = pct;
		printf("{\"type\":\"progress\",\"percent\":%d}\n", pct);
		fflush(stdout);
	}
	return 0;
}

static void saved_line(const char *path)
{
	json_object *o = json_object_new_object();
	json_object_object_add(o, "type", json_object_new_string("saved"));
	json_object_object_add(o, "path", json_object_new_string(path));
	puts(json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN));
	json_object_put(o);
}

/* A fast link serves the whole file: the best audio, one plain download. */
static int fetch_fast(const char *id, const char *dest)
{
	char path[1024], part[1040];
	if (fast_resolve(id))
		return -1;
	snprintf(path, sizeof(path), "%s/%s.%s", dest, id, fast_ext);
	snprintf(part, sizeof(part), "%s.part", path);
	FILE *f = fopen(part, "w");
	if (!f)
		return -1;
	int last = -1;
	long code = 0;
	CURL *c = curl_easy_init();
	curl_easy_setopt(c, CURLOPT_URL, fast_link);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
	curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, fetch_progress);
	curl_easy_setopt(c, CURLOPT_XFERINFODATA, &last);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	CURLcode rc = curl_easy_perform(c);
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_cleanup(c);
	fclose(f);
	if (rc != CURLE_OK || code != 200 || rename(part, path)) {
		unlink(part);
		return -1;
	}
	saved_line(path);
	return 0;
}

static int cmd_fetch(const char *id, const char *dest)
{
	if (!fetch_fast(id, dest))
		return 0;
	char url[128], out[1024], line[2048], saved[2048] = "";
	snprintf(url, sizeof(url), "https://www.youtube.com/watch?v=%s", id);
	snprintf(out, sizeof(out), "%s/%s.%%(ext)s", dest, id);
	char *argv[] = {"yt-dlp", "-q", "--no-warnings", "--no-playlist", YTDLP_FALLBACK,
		"-O", "after_move:F %(filepath)s", "--no-simulate", "-o", out, url, NULL};
	pid_t pid;
	FILE *f = ytdlp(argv, &pid);
	if (!f) {
		error_line("cannot run yt-dlp");
		return 0;
	}
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = 0;
		if (line[0] == 'F' && line[1] == ' ')
			snprintf(saved, sizeof(saved), "%s", line + 2);
	}
	fclose(f);
	int st = 0;
	waitpid(pid, &st, 0);
	if (!*saved || !WIFEXITED(st) || WEXITSTATUS(st)) {
		error_line("download failed");
		return 0;
	}
	saved_line(saved);
	return 0;
}

static int valid_id(const char *s)
{
	if (!*s || strlen(s) > 32)
		return 0;
	for (; *s; s++)
		if (!strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_", *s))
			return 0;
	return 1;
}

int main(int argc, char **argv)
{
	if (argc >= 3 && !strcmp(argv[1], "search"))
		return cmd_search(argv[2], argc > 3 ? argv[3] : "8");
	if (argc >= 3 && !valid_id(argv[2])) {
		error_line("bad id");
		return 0;
	}
	curl_global_init(CURL_GLOBAL_DEFAULT);
	if (argc >= 3 && !strcmp(argv[1], "resolve"))
		return cmd_resolve(argv[2]);
	if (argc >= 4 && !strcmp(argv[1], "fetch"))
		return cmd_fetch(argv[2], argv[3]);
	fprintf(stderr, "usage: zroov-source-yt resolve ID | search QUERY [LIMIT] | fetch ID DIR\n");
	return 2;
}
