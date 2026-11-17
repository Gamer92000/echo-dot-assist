/*
 * boot.log, followed: every line that lands in it goes on to a syslog server on the network (settings syslog,
 * syslog_server, syslog_protocol, syslog_format) and, as it comes, to the ESPHome API clients that subscribed to logs
 * (proto->log: Home Assistant's "subscribe to logs from the device", aioesphomeapi-logs, the ESPHome dashboard).
 *
 * The file, not the pipe hassmic's own lines go through (clock.c): boot.log has root's lines too (main.sh: the
 * firewall, updates, rotation, "hassmic exited, restart in 3 s"), and those are what explains an Echo that fell off the
 * network.  And the file is the queue.  The sender keeps an offset into it and moves it on only once a line went out:
 * while the server or the network is away nothing piles up in memory, and once it is back what was written meanwhile
 * follows, with the times it was written at.  The offset, with the 32 bytes just before it (to know it still points at
 * the same place), is saved at most once a minute (state/syslog.pos: /data is flash), so a restart of hassmic or a
 * reboot carries on from there: at most the last minute goes out twice, with its own times, never a gap.
 *
 * Rotation (main.sh rotate_log, at 1 MB): boot.log copied to boot.log.1, then emptied.  The bytes before the offset no
 * longer match then; when they do in boot.log.1, the rest comes from there and boot.log follows from its start.  Rotated
 * twice before the sender got there (a server away for a day): what is gone is gone, and a line in the log says so.
 *
 * Times: the stamp in front of each line (clock.c) becomes the message's own (RFC 5424 TIMESTAMP).  "boot+S" (before
 * the clock came from Home Assistant) is put in real time once the clock is right, but only for lines of this boot:
 * main.sh's "== satellite start" line names the boot (its boot id), and the saved offset carries the boot it was
 * written in.  Lines of an earlier boot keep the server's time of arrival ("-"), their "boot+" stays in the text.
 *
 * UDP is best effort, as syslog over UDP always is: a connected socket, so the kernel reports what it learns (no route:
 * the Echo is off the network; a refused port: no server) on the next send, which then holds the line and tries again,
 * 2 s doubling to a minute.  What a server never got without saying so is lost.  TCP (RFC 6587 octet counting for 5424,
 * a newline after each 3164 message) moves the offset only once the kernel took the whole message.  Either way the
 * backlog goes out at RATE lines a second at most, so a day's worth after an outage does not flood the server or the
 * Wi-Fi.  One datagram stays under UDP_MAX bytes (no IP fragments on any LAN); a longer line is cut on a UTF-8 boundary.
 *
 * Everything runs in one thread of its own, woken by inotify on boot.log (nothing polls while nothing is written), by a
 * pipe when settings or subscribers change, and by the socket.  Names are resolved in it (getaddrinfo, ".local" by
 * mDNS): only when it (re)connects, every 10 minutes at most otherwise, never per line.  It writes to the log itself only
 * when something changes (reached, unreachable, lines lost), never per line sent: a line about a line sent would be
 * sent too.
 */
#include "logfwd.h"
#include "clock.h"
#include "net.h"
#include "statefile.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CHUNK 65536                     /* read at a time */
#define LINE_CAP 4096                   /* a longer "line" (no newline in it) goes out in pieces of this */
#define UDP_MAX 1400                    /* one datagram: Ethernet's 1500 less IPv6's 40 and UDP's 8, some to spare */
#define TCP_MAX 8192
#define TAIL 32                         /* bytes before the offset that say it is still the same place */
#define RATE 500                        /* lines a second at most */
#define BURST 200
#define BATCH 50                        /* out of tokens: wait for this many, so a backlog is read in batches, not a
                                         * 64 KB read per line */
#define RETRY_MIN_MS 2000
#define RETRY_MAX_MS 60000
#define CONNECT_MS 5000
#define RESOLVE_MS (10 * 60 * 1000)
#define MAX_ADDR 4
#define FACILITY 3                      /* daemon */

static int SAVE_MS = 60000;             /* state/syslog.pos at most this often (HASSMIC_SYSLOG_SAVE_MS for tests) */

/* ---------------------------------------------------------------- pieces (no state) */

static const char *state_dir(void) { const char *e = getenv("HASSMIC_STATE"); return e ? e : "/data/local/hassmic/state"; }
const char *logfwd_path(void) { const char *e = getenv("HASSMIC_LOG"); return e ? e : "/data/local/hassmic/boot.log"; }

void logfwd_redact(char *t, size_t n)
{
    for (size_t i = 0; i + 4 <= n; i++) {
        if (memcmp(t + i, "SP:0", 4)) continue;
        size_t j = i + 4;
        while (j < n && (isupper((unsigned char)t[j]) || isdigit((unsigned char)t[j]))) t[j++] = '*';
        i = j;
    }
}

int logfwd_server_parse(const char *s, char *host, size_t cap, int *port)
{
    size_t n = strlen(s), hl; const char *h = s, *p = NULL;
    *port = LOGFWD_PORT; if (cap) host[0] = 0;
    if (!n) return 0;
    if (n > LOGFWD_SERVER_MAX) return -1;
    if (s[0] == '[') {                                  /* [v6] or [v6]:port */
        const char *e = strchr(s, ']');
        if (!e || e == s + 1) return -1;
        h = s + 1; hl = (size_t)(e - h);
        if (e[1] == ':') p = e + 2; else if (e[1]) return -1;
    } else {
        const char *c = strchr(s, ':');
        if (c && !strchr(c + 1, ':')) { hl = (size_t)(c - s); p = c + 1; }     /* one colon: host:port */
        else hl = n;                                    /* none, or a bare IPv6 address */
    }
    if (!hl || hl >= cap) return -1;
    for (size_t i = 0; i < hl; i++) if (!isalnum((unsigned char)h[i]) && !strchr(".-_:", h[i])) return -1;
    if (h[0] == '-' || h[0] == '.') return -1;
    if (p) {
        char *end; long v = strtol(p, &end, 10);
        if (!*p || *end || v < 1 || v > 65535) return -1;
        *port = (int)v;
    }
    memcpy(host, h, hl); host[hl] = 0;
    return 0;
}

static int digits(const char *s, int n) { for (int i = 0; i < n; i++) if (!isdigit((unsigned char)s[i])) return 0; return 1; }
static int num(const char *s, int n) { int v = 0; for (int i = 0; i < n; i++) v = v * 10 + s[i] - '0'; return v; }

int logfwd_stamp(const char *l, size_t n, struct logfwd_time *t, double *boot_s, size_t *skip)
{
    t->known = 0; *skip = 0;
    /* "2026-10-06 15:18:02.417Z " (clock.c) or "2026-10-06 15:18:02Z " (main.sh's date) */
    if (n >= 21 && digits(l, 4) && l[4] == '-' && digits(l + 5, 2) && l[7] == '-' && digits(l + 8, 2) && l[10] == ' '
        && digits(l + 11, 2) && l[13] == ':' && digits(l + 14, 2) && l[16] == ':' && digits(l + 17, 2)) {
        size_t k = 19; int ms = 0;
        if (k < n && l[k] == '.') { k++; int d = 0; while (k < n && isdigit((unsigned char)l[k])) { if (d < 3) ms = ms * 10 + l[k] - '0'; d++; k++; } while (d < 3) { ms *= 10; d++; } }
        if (k + 1 >= n || l[k] != 'Z' || l[k + 1] != ' ') return 0;
        struct tm tm = { 0 };
        tm.tm_year = num(l, 4) - 1900; tm.tm_mon = num(l + 5, 2) - 1; tm.tm_mday = num(l + 8, 2);
        tm.tm_hour = num(l + 11, 2); tm.tm_min = num(l + 14, 2); tm.tm_sec = num(l + 17, 2);
        if (tm.tm_mon > 11 || tm.tm_mday < 1 || tm.tm_mday > 31 || tm.tm_hour > 23 || tm.tm_min > 59 || tm.tm_sec > 60) return 0;
        t->known = 1; t->sec = (long long)timegm(&tm); t->ms = ms; *skip = k + 2;
        return 1;
    }
    if (n >= 7 && !memcmp(l, "boot+", 5) && isdigit((unsigned char)l[5])) {
        size_t k = 5; double v = 0, f = 0.1;
        while (k < n && isdigit((unsigned char)l[k])) v = v * 10 + (l[k++] - '0');
        if (k < n && l[k] == '.') for (k++; k < n && isdigit((unsigned char)l[k]); k++, f /= 10) v += (l[k] - '0') * f;
        if (k >= n || l[k] != ' ') return 0;
        *boot_s = v; *skip = k + 1;
        return 2;
    }
    return 0;
}

static int word_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

/* web/app.js logLine: /^!!|\b(error|failed|cannot|refused)\b/i is "bad", /^==/ a mark */
int logfwd_level(const char *m, size_t n)
{
    static const char *const bad[] = { "error", "failed", "cannot", "refused" };
    if (n >= 2 && m[0] == '!' && m[1] == '!') return LOGFWD_WARN;
    for (size_t i = 0; i < n; i++) {
        if (i && word_char(m[i - 1])) continue;
        for (size_t w = 0; w < sizeof bad / sizeof *bad; w++) {
            size_t k = strlen(bad[w]);
            if (i + k <= n && !strncasecmp(m + i, bad[w], k) && (i + k == n || !word_char(m[i + k]))) return LOGFWD_WARN;
        }
    }
    return n >= 2 && m[0] == '=' && m[1] == '=' ? LOGFWD_NOTICE : LOGFWD_INFO;
}

/* n cut back so that it does not end inside a UTF-8 sequence */
static size_t utf8_cut(const char *s, size_t n, size_t max)
{
    if (n <= max) return n;
    n = max;
    while (n && ((unsigned char)s[n] & 0xc0) == 0x80) n--;      /* s[n] continues a sequence: cut before its lead */
    return n;
}

size_t logfwd_format(char *o, size_t cap, int rfc3164, int level, const struct logfwd_time *t, const char *host, const char *msg, size_t n)
{
    static const int sev[] = { 4, 5, 6 };               /* warning, notice, informational */
    static const char mon[12][4] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    char head[160]; struct tm tm; int h;
    int pri = FACILITY * 8 + sev[level < 0 || level > 2 ? 2 : level];
    if (!cap) return 0;
    if (rfc3164) {
        /* the time as the Echo's own zone has it (3164 has none to name); none known: now */
        time_t s = t->known ? (time_t)t->sec : time(NULL);
        localtime_r(&s, &tm);
        h = snprintf(head, sizeof head, "<%d>%s %2d %02d:%02d:%02d %s hassmic: ", pri, mon[tm.tm_mon], tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, host);
    } else if (t->known) {
        time_t s = (time_t)t->sec; gmtime_r(&s, &tm);
        h = snprintf(head, sizeof head, "<%d>1 %04d-%02d-%02dT%02d:%02d:%02d.%03dZ %s hassmic - - - ", pri, tm.tm_year + 1900, tm.tm_mon + 1,
                     tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, t->ms, host);
    } else h = snprintf(head, sizeof head, "<%d>1 - %s hassmic - - - ", pri, host);
    if (h < 0 || (size_t)h >= cap || (size_t)h >= sizeof head) { o[0] = 0; return 0; }
    memcpy(o, head, (size_t)h);
    size_t m = utf8_cut(msg, n, cap - 1 - (size_t)h);
    memcpy(o + h, msg, m);
    o[h + m] = 0;
    return (size_t)h + m;
}

/* ---------------------------------------------------------------- state */

static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static struct cfg { int on, tcp, rfc3164, port; char host[LOGFWD_SERVER_MAX + 1]; } cfg;    /* lk */
static unsigned cfg_gen;                                        /* lk */
enum { ST_OFF, ST_NO_SERVER, ST_BAD_NAME, ST_CONNECTING, ST_UP, ST_DOWN };
static const char *const st_names[] = { "off", "no_server", "bad_name", "connecting", "up", "down" };
static struct { int state; char err[96]; unsigned long long sent; long long behind; } st;   /* lk: what the page shows */
static int wake_fd[2] = { -1, -1 };
static atomic_int live_n;
static void (*live_fn)(int, const char *, size_t);
static const char *node = "echo";
static char my_boot[40];

static void wake(void) { if (wake_fd[1] >= 0) { char c = 1; ssize_t r = write(wake_fd[1], &c, 1); (void)r; } }

void logfwd_live(int delta) { atomic_fetch_add(&live_n, delta); wake(); }

void logfwd_syslog(int on, const char *server, int tcp, int rfc3164)
{
    struct cfg c = { on, tcp, rfc3164, LOGFWD_PORT, "" };
    if (logfwd_server_parse(server, c.host, sizeof c.host, &c.port)) c.host[0] = 0;    /* settings.c checked it */
    pthread_mutex_lock(&lk);
    if (c.on != cfg.on || c.tcp != cfg.tcp || c.rfc3164 != cfg.rfc3164 || c.port != cfg.port || strcmp(c.host, cfg.host)) { cfg = c; cfg_gen++; }
    pthread_mutex_unlock(&lk);
    wake();
}

static size_t jesc(char *o, size_t cap, const char *s)
{
    size_t n = 0;
    for (; *s && n + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { o[n++] = '\\'; o[n++] = (char)c; }
        else if (c < 0x20) n += (size_t)snprintf(o + n, cap - n, "\\u%04x", c);
        else o[n++] = (char)c;
    }
    o[n] = 0;
    return n;
}

size_t logfwd_status_json(char *o, size_t cap)
{
    char e[200], h[2 * LOGFWD_SERVER_MAX + 8], hp[LOGFWD_SERVER_MAX + 16];
    pthread_mutex_lock(&lk);
    snprintf(hp, sizeof hp, strchr(cfg.host, ':') ? "[%s]:%d" : "%s:%d", cfg.host, cfg.port);
    jesc(e, sizeof e, st.err); jesc(h, sizeof h, cfg.host[0] ? hp : "");
    int n = snprintf(o, cap, "{\"state\":\"%s\",\"server\":\"%s\",\"protocol\":\"%s\",\"sent\":%llu,\"behind\":%lld,\"error\":\"%s\"}",
                     st_names[st.state], h, cfg.tcp ? "tcp" : "udp", st.sent, st.behind, e);
    pthread_mutex_unlock(&lk);
    return n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap - 1;
}

static void status(int state, const char *err)
{
    pthread_mutex_lock(&lk);
    st.state = state; snprintf(st.err, sizeof st.err, "%s", err ? err : "");
    pthread_mutex_unlock(&lk);
}

static long long mono_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

/* ---------------------------------------------------------------- reading the log */

/* A place in the log: boot.log, or boot.log.1 while the part rotated away is read to its end; the bytes before it */
struct cur {
    long long off; int old;
    unsigned char tail[TAIL]; int tl;
    int boot;                           /* its lines are of this boot: 1, else 0 (unknown counts as not) */
};

static void old_path(char *p, size_t cap) { snprintf(p, cap, "%s.1", logfwd_path()); }
static const char *cur_path(const struct cur *c, char *buf, size_t cap) { if (!c->old) return logfwd_path(); old_path(buf, cap); return buf; }

static long long file_size(const char *p) { struct stat s; return stat(p, &s) ? -1 : (long long)s.st_size; }

static ssize_t pread_path(const char *p, void *b, size_t n, long long at)
{
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t r = pread(fd, b, n, (off_t)at);
    close(fd);
    return r;
}

/* p has c's tail just before c->off */
static int holds(const char *p, const struct cur *c)
{
    unsigned char b[TAIL];
    if (!c->tl) return !c->off || file_size(p) >= c->off;
    return c->off >= c->tl && pread_path(p, b, (size_t)c->tl, c->off - c->tl) == c->tl && !memcmp(b, c->tail, (size_t)c->tl);
}

/* The end of p, at a line's start (a line still being written is not ours yet) */
static void cur_end(struct cur *c, const char *p)
{
    char b[4096]; long long sz = file_size(p);
    c->old = 0; c->off = 0; c->tl = 0; c->boot = 1;
    if (sz <= 0) return;
    long long from = sz > (long long)sizeof b ? sz - (long long)sizeof b : 0;
    ssize_t r = pread_path(p, b, (size_t)(sz - from), from);
    if (r <= 0) return;
    ssize_t i = r; while (i > 0 && b[i - 1] != '\n') i--;
    if (!i) { c->off = sz; return; }                    /* no line end in the last 4 KB: take the end as it is */
    c->off = from + i;
    c->tl = (int)(i < TAIL ? i : TAIL);
    memcpy(c->tail, b + i - c->tl, (size_t)c->tl);
}

/* Still where it was?  Else boot.log was rotated: the rest is in boot.log.1 when that has our bytes, else (rotated
 * twice) from boot.log.1's start, the oldest there is.  Returns 1 when lines were lost on the way */
static int cur_check(struct cur *c)
{
    char o[300];
    if (holds(cur_path(c, o, sizeof o), c)) return 0;
    old_path(o, sizeof o);
    if (!c->old && holds(o, c)) { c->old = 1; return 0; }
    int lost = c->off > 0;
    c->off = 0; c->tl = 0; c->old = file_size(o) > 0;
    return lost;
}

/* The bytes before end, from what was before the batch (t0) and the batch itself (b, from boff) */
static void cur_tail(struct cur *c, const unsigned char *t0, int t0l, const char *b, long long boff, long long end)
{
    unsigned char tmp[2 * TAIL]; int n = 0;
    long long in_b = end - boff;                        /* bytes of the batch up to end */
    if (in_b < TAIL) { int k = TAIL - (int)in_b; if (k > t0l) k = t0l; memcpy(tmp, t0 + t0l - k, (size_t)k); n = k; }
    int k = (int)(in_b < TAIL ? in_b : TAIL);
    memcpy(tmp + n, b + in_b - k, (size_t)k); n += k;
    c->tl = n; memcpy(c->tail, tmp, (size_t)n); c->off = end;
}

/* "== satellite start, ... boot <id>" (main.sh): the lines after it are of that boot */
static void boot_mark(struct cur *c, const char *m, size_t n)
{
    static const char start[] = "== satellite start";
    if (n < sizeof start - 1 || memcmp(m, start, sizeof start - 1)) return;
    for (size_t i = 0; i + 5 + 36 <= n; i++)
        if (!memcmp(m + i, " boot ", 6) && i + 6 + 36 <= n) { c->boot = my_boot[0] && !memcmp(m + i + 6, my_boot, 36); return; }
}

/* A line split into its parts: time, level, text (redacted, in buf) */
struct line { struct logfwd_time t; int level; const char *msg; size_t n; };

static void line_of(struct cur *c, const char *l, size_t n, char *buf, struct line *out)
{
    double bs = 0; size_t skip;
    int k = logfwd_stamp(l, n, &out->t, &bs, &skip);
    boot_mark(c, l + skip, n - skip);                   /* first: the start line is of the boot it names */
    if (k == 2 && c->boot && clock_synced()) {         /* seconds since this boot, while the clock is right */
        struct timespec rt, bt; clock_gettime(CLOCK_REALTIME, &rt); clock_gettime(CLOCK_BOOTTIME, &bt);
        double now_b = (double)bt.tv_sec + bt.tv_nsec / 1e9, real = (double)rt.tv_sec + rt.tv_nsec / 1e9 - (now_b - bs);
        if (bs <= now_b + 1) { out->t.known = 1; out->t.sec = (long long)real; out->t.ms = (int)((real - (double)out->t.sec) * 1000); }
    }
    if (k == 2 && !out->t.known) skip = 0;              /* its "boot+" stays in the text: the only time it has */
    memcpy(buf, l + skip, n - skip);
    out->n = n - skip; out->msg = buf;
    logfwd_redact(buf, out->n);
    out->level = logfwd_level(buf, out->n);
}

/* Reads from c on, hands each whole line to take(); stops where take() says it cannot take one now (0).  At boot.log.1's
 * end it goes on in boot.log.  Returns 0 when it stopped there, 1 when all there was is taken */
static int cur_run(struct cur *c, int (*take)(struct cur *, const struct line *, long long end, const unsigned char *t, int tl), char *b)
{
    char o[300], msg[LINE_CAP];
    for (;;) {
        const char *p = cur_path(c, o, sizeof o);
        ssize_t r = pread_path(p, b, CHUNK, c->off);
        if (r <= 0) {
            if (!c->old || (r < 0 && file_size(p) >= 0)) return 1;
            c->old = 0; c->off = 0; c->tl = 0;          /* boot.log.1 read to its end: boot.log from its start */
            continue;
        }
        long long boff = c->off; unsigned char t0[TAIL]; int t0l = c->tl; memcpy(t0, c->tail, (size_t)t0l);
        size_t i = 0;
        while (i < (size_t)r) {
            const char *nl = memchr(b + i, '\n', (size_t)r - i);
            size_t len = nl ? (size_t)(nl - (b + i)) : (size_t)r - i, used = len + (nl != NULL);
            /* a line without its end yet is still being written; boot.log.1's last one never gets it */
            if (!nl && len < LINE_CAP && !(c->old && boff + r >= file_size(p))) break;
            if (len > LINE_CAP) len = used = LINE_CAP;
            struct line ln; struct cur next = *c;
            cur_tail(&next, t0, t0l, b, boff, boff + (long long)(i + used));
            if (len && b[i + len - 1] == '\r') len--;
            line_of(&next, b + i, len, msg, &ln);
            if (len && !take(c, &ln, next.off, next.tail, next.tl)) return 0;
            *c = next;
            i += used;
        }
        if (r < CHUNK && !c->old) return 1;             /* the end of boot.log (a line being written may be left) */
        if (!i) return 1;
    }
}

/* ---------------------------------------------------------------- live lines (ESPHome) */

static struct cur live;

static int take_live(struct cur *c, const struct line *l, long long end, const unsigned char *t, int tl)
{
    (void)c; (void)end; (void)t; (void)tl;
    if (atomic_load(&live_n) > 0 && live_fn) live_fn(l->level, l->msg, l->n);
    return 1;
}

/* ---------------------------------------------------------------- syslog */

static struct cur ship;
static int ship_active;                 /* cfg on with a server: ship follows the log */
static struct cfg now;                  /* the thread's copy */
static int sock = -1, connecting, down_said, name_said, up, blocked;  /* blocked: the socket's buffer was full */
/* UDP has no connection to say the server is there: the first line after a (re)start goes out as a probe, and counts
 * as sent once PROBE_MS passed without the kernel reporting an error for it (a refused port, a host that does not
 * answer ARP: ~3 s).  Until then nothing else goes, so an absent server costs that one line per try, and is not taken
 * for back at every try */
#define PROBE_MS 3500
static int probing; static long long probe_by;
static long long retry_at, retry_ms = RETRY_MIN_MS, connect_by, resolved_at = -RESOLVE_MS, saved_at, tokens_at;
static double tokens = BURST;
static struct sockaddr_storage addrs[MAX_ADDR]; static socklen_t addr_len[MAX_ADDR]; static int n_addr, addr_i;
static char pend[TCP_MAX + 16]; static size_t pend_n, pend_sent;      /* a TCP message the kernel took part of */
static long long pend_end; static unsigned char pend_tail[TAIL]; static int pend_tl, pend_boot;
static int saved_old = -1; static long long saved_off = -1;

static void pos_path(char *p, size_t cap) { snprintf(p, cap, "%s/syslog.pos", state_dir()); }

static void pos_save(int force)
{
    char p[300], t[200], hex[2 * TAIL + 1]; int n;
    if (!force && (ship.old == saved_old && ship.off == saved_off)) return;
    if (!force && mono_ms() - saved_at < SAVE_MS) return;
    for (int i = 0; i < ship.tl; i++) snprintf(hex + 2 * i, 3, "%02x", ship.tail[i]);
    hex[2 * ship.tl] = 0;
    n = snprintf(t, sizeof t, "1 %s %d %lld %d %s\n", my_boot[0] ? my_boot : "-", ship.old, ship.off, ship.boot, ship.tl ? hex : "-");
    pos_path(p, sizeof p);
    saved_at = mono_ms();
    if (statefile_write(p, 0600, t, (size_t)n)) return;
    saved_old = ship.old; saved_off = ship.off;
}

static int pos_load(void)
{
    char p[300], boot[40], hex[2 * TAIL + 4]; int v, old, b; long long off; FILE *f;
    pos_path(p, sizeof p);
    if (!(f = fopen(p, "r"))) return -1;
    int k = fscanf(f, "%d %39s %d %lld %d %66s", &v, boot, &old, &off, &b, hex);
    fclose(f);
    if (k != 6 || v != 1 || off < 0) return -1;
    ship.old = old != 0; ship.off = off; ship.tl = 0;
    ship.boot = b && my_boot[0] && !strcmp(boot, my_boot);
    size_t hl = strcmp(hex, "-") ? strlen(hex) : 0;
    if (hl % 2 || hl / 2 > TAIL) return -1;
    for (size_t i = 0; i < hl / 2; i++) { unsigned x; if (sscanf(hex + 2 * i, "%2x", &x) != 1) return -1; ship.tail[i] = (unsigned char)x; }
    ship.tl = (int)(hl / 2);
    saved_old = ship.old; saved_off = ship.off;
    return 0;
}

static void pos_drop(void) { char p[300]; pos_path(p, sizeof p); unlink(p); saved_old = -1; saved_off = -1; }

static void hostport(char *o, size_t cap) { snprintf(o, cap, strchr(now.host, ':') ? "[%s]:%d" : "%s:%d", now.host, now.port); }

static void sock_close(void)
{
    if (sock >= 0) close(sock);
    sock = -1; connecting = up = blocked = probing = 0; pend_n = pend_sent = 0;  /* a message half sent goes again whole: ship did not move */
}

static long long behind(void)
{
    char o[300]; long long b = 0, m = file_size(logfwd_path());
    if (!ship_active) return 0;
    if (ship.old) { old_path(o, sizeof o); long long s = file_size(o); b = (s > ship.off ? s - ship.off : 0) + (m > 0 ? m : 0); }
    else b = m > ship.off ? m - ship.off : 0;
    return b;
}

/* Something went wrong with the server: hold the log, try again later */
static void failed(const char *what, int e)
{
    char hp[LOGFWD_SERVER_MAX + 16], err[96];
    snprintf(err, sizeof err, "%s: %s", what, strerror(e));
    sock_close();
    if (!down_said++) { hostport(hp, sizeof hp); fprintf(stderr, "syslog: can't reach %s (%s); keeping the log on the Echo until it's back\n", hp, err); }
    status(ST_DOWN, err);
    retry_at = mono_ms() + retry_ms;
    retry_ms = retry_ms * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : retry_ms * 2;
}

static int resolve(void)
{
    char port[8]; struct addrinfo hints = { 0 }, *ai = NULL; size_t hl = strlen(now.host);
    n_addr = 0; addr_i = 0; resolved_at = mono_ms();
    snprintf(port, sizeof port, "%d", now.port);
    if (hl > 6 && !strcasecmp(now.host + hl - 6, ".local")) {         /* the system resolver does not do mDNS */
        unsigned a = mdns_resolve4(now.host);
        if (a) {
            struct sockaddr_in *s = (struct sockaddr_in *)&addrs[0];
            memset(s, 0, sizeof *s); s->sin_family = AF_INET; s->sin_port = htons((uint16_t)now.port); s->sin_addr.s_addr = a;
            addr_len[0] = sizeof *s; n_addr = 1;
            return 0;
        }
    }
    hints.ai_socktype = now.tcp ? SOCK_STREAM : SOCK_DGRAM;
    if (getaddrinfo(now.host, port, &hints, &ai) || !ai) return -1;
    for (struct addrinfo *p = ai; p && n_addr < MAX_ADDR; p = p->ai_next) {
        if ((size_t)p->ai_addrlen > sizeof addrs[0]) continue;
        memcpy(&addrs[n_addr], p->ai_addr, p->ai_addrlen); addr_len[n_addr++] = p->ai_addrlen;
    }
    freeaddrinfo(ai);
    return n_addr ? 0 : -1;
}

static void sock_open(void)
{
    if (!n_addr || mono_ms() - resolved_at > RESOLVE_MS) {
        status(ST_CONNECTING, NULL);
        if (resolve()) {
            if (!name_said++) fprintf(stderr, "syslog: can't resolve %s (DNS); retrying\n", now.host);
            status(ST_BAD_NAME, "name not found");
            retry_at = mono_ms() + retry_ms; retry_ms = retry_ms * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : retry_ms * 2;
            return;
        }
        name_said = 0;
    }
    struct sockaddr *a = (struct sockaddr *)&addrs[addr_i % n_addr]; socklen_t al = addr_len[addr_i % n_addr];
    addr_i++;                                           /* the next try takes the next address */
    sock = net_socket(a->sa_family, (now.tcp ? SOCK_STREAM : SOCK_DGRAM) | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (sock < 0) { failed("socket", errno); return; }
    if (now.tcp) {
        int on = 1; unsigned user_ms = 30000;           /* a server gone while messages wait: noticed in 30 s, not 15 min */
        setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on);
        setsockopt(sock, IPPROTO_TCP, TCP_USER_TIMEOUT, &user_ms, sizeof user_ms);
    }
    if (connect(sock, a, al) == 0) { connecting = 0; return; }        /* UDP: up once a send went through */
    if (now.tcp && errno == EINPROGRESS) { connecting = 1; connect_by = mono_ms() + CONNECT_MS; status(ST_CONNECTING, NULL); return; }
    failed("connect", errno);
}

static void reached(void)
{
    char hp[LOGFWD_SERVER_MAX + 16];
    up = 1; retry_ms = RETRY_MIN_MS;
    if (down_said) { hostport(hp, sizeof hp); fprintf(stderr, "syslog: %s is reachable again, sending the %lld bytes held back\n", hp, behind()); }
    down_said = 0;
    status(ST_UP, NULL);
}

/* The rest of a TCP message; 1 when it is all out */
static int pend_flush(void)
{
    while (pend_sent < pend_n) {
        ssize_t w = send(sock, pend + pend_sent, pend_n - pend_sent, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { blocked = 1; return 0; }
        if (w <= 0) { failed("send", w < 0 ? errno : EPIPE); return 0; }
        pend_sent += (size_t)w;
    }
    if (pend_n) {                                       /* all of it out: the place after its line is ship's */
        ship.off = pend_end; ship.tl = pend_tl; memcpy(ship.tail, pend_tail, (size_t)pend_tl); ship.boot = pend_boot;
        pthread_mutex_lock(&lk); st.sent++; pthread_mutex_unlock(&lk);
    }
    pend_n = pend_sent = 0;
    return 1;
}

static int take_ship(struct cur *c, const struct line *l, long long end, const unsigned char *t, int tl)
{
    char m[TCP_MAX];
    if (sock < 0 || connecting || pend_n || probing) return 0;
    long long ms = mono_ms();
    tokens += (double)(ms - tokens_at) * RATE / 1000.0; tokens_at = ms;
    if (tokens > BURST) tokens = BURST;
    if (tokens < 1) return 0;                           /* the thread waits until BATCH are there again */
    size_t n = logfwd_format(m, now.tcp ? sizeof m : UDP_MAX + 1, now.rfc3164, l->level, &l->t, node, l->msg, l->n);
    if (!n) return 1;                                   /* nothing to send of it (cannot happen with a sane name) */
    if (now.tcp) {
        /* RFC 6587: octet counting for 5424 (any byte may be in it), a newline after a 3164 message (what its receivers
         * take).  cur_run moves c only on 1: a message the kernel took part of moves ship once the rest is out */
        int h = now.rfc3164 ? 0 : snprintf(pend, 16, "%zu ", n);
        memcpy(pend + h, m, n); pend_n = (size_t)h + n;
        if (now.rfc3164) pend[pend_n++] = '\n';
        pend_sent = 0; pend_end = end; pend_tl = tl; memcpy(pend_tail, t, (size_t)tl); pend_boot = c->boot;
        tokens -= 1;
        return pend_flush();
    }
    for (;;) {
        ssize_t w = send(sock, m, n, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { blocked = 1; return 0; }
        if (w < 0 && errno == ENOBUFS) { tokens = 1 - BATCH; return 0; }    /* the device's queue: POLLOUT would not wait */
        if (w < 0) { failed("send", errno); return 0; }
        break;
    }
    tokens -= 1;
    if (!up) {                                          /* the probe: ship moves once it held (thread) */
        probing = 1; probe_by = mono_ms() + PROBE_MS;
        pend_end = end; pend_tl = tl; memcpy(pend_tail, t, (size_t)tl); pend_boot = c->boot;
        return 0;
    }
    pthread_mutex_lock(&lk); st.sent++; pthread_mutex_unlock(&lk);
    return 1;
}

/* settings changed: start, stop, or carry on to another server */
static void reconfigure(int first)
{
    struct cfg c; char hp[LOGFWD_SERVER_MAX + 16];
    pthread_mutex_lock(&lk); c = cfg; pthread_mutex_unlock(&lk);
    int active = c.on && c.host[0], was = ship_active;
    int same = was && active && !strcmp(c.host, now.host) && c.port == now.port && c.tcp == now.tcp;
    now = c;
    if (!same) { sock_close(); n_addr = 0; resolved_at = -RESOLVE_MS; retry_at = 0; retry_ms = RETRY_MIN_MS; down_said = name_said = 0; }
    ship_active = active;
    if (!active) {
        if (was || first) pos_drop();                   /* switched on again later: from then on, not from now */
        status(c.on ? ST_NO_SERVER : ST_OFF, NULL);
        if (was) fprintf(stderr, c.on ? "syslog: no server set; nothing is sent\n" : "syslog: off\n");
        return;
    }
    if (!was) {
        /* a start of hassmic carries on where the last one was (state/syslog.pos); switched on now: from here on */
        int resumed = first && !pos_load();
        if (!resumed) { cur_end(&ship, logfwd_path()); pos_save(1); }
        hostport(hp, sizeof hp);
        fprintf(stderr, "syslog: sending to %s via %s, %s%s\n", hp, c.tcp ? "TCP" : "UDP", c.rfc3164 ? "RFC 3164" : "RFC 5424",
                resumed ? ", resuming where the last run stopped" : "");
    } else if (!same) { hostport(hp, sizeof hp); fprintf(stderr, "syslog: now sending to %s via %s, %s\n", hp, c.tcp ? "TCP" : "UDP", c.rfc3164 ? "RFC 3164" : "RFC 5424"); }
    status(ST_CONNECTING, NULL);
}

/* ---------------------------------------------------------------- the thread */

static int watch(int in, int *wd)
{
    if (in < 0) return -1;
    if (*wd >= 0) inotify_rm_watch(in, *wd);
    *wd = inotify_add_watch(in, logfwd_path(), IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE | IN_MOVE_SELF | IN_DELETE_SELF);
    return *wd;
}

static void *thread(void *arg)
{
    (void)arg;
    char *b = malloc(CHUNK); unsigned gen = 0; int first = 1, wd = -1, live_was = 0;
    int in = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (!b) return NULL;
    for (;;) {
        pthread_mutex_lock(&lk); unsigned g = cfg_gen; pthread_mutex_unlock(&lk);
        if (first || g != gen) { gen = g; reconfigure(first); first = 0; }

        /* Nothing to do (the default: no server, nobody subscribed): no watch on boot.log, the thread sleeps on the pipe
         * until a setting or a subscriber wakes it, and live lines start from the end then */
        int listening = atomic_load(&live_n) > 0;
        if (!ship_active && !listening) {
            if (wd >= 0) { inotify_rm_watch(in, wd); wd = -1; }
            struct pollfd pw = { wake_fd[0], POLLIN, 0 };
            if (poll(&pw, 1, -1) > 0) { char d[64]; while (read(wake_fd[0], d, sizeof d) > 0) {} }
            live_was = 0;
            continue;
        }
        if (listening && !live_was) cur_end(&live, logfwd_path());        /* a first subscriber: from now on */
        live_was = listening;
        if (listening) { cur_check(&live); cur_run(&live, take_live, b); }

        long long ms = mono_ms(), next = 30000;
        if (ship_active) {
            if (cur_check(&ship)) fprintf(stderr, "syslog: the log rotated twice while the server was unreachable; some lines could not be sent\n");
            if (sock >= 0 && connecting && ms >= connect_by) failed("connect", ETIMEDOUT);
            if (sock < 0 && ms >= retry_at) { sock_open(); if (sock >= 0 && now.tcp && !connecting) reached(); }
            if (probing && ms >= probe_by) {                /* no error came back: the server is there */
                probing = 0;
                ship.off = pend_end; ship.tl = pend_tl; memcpy(ship.tail, pend_tail, (size_t)pend_tl); ship.boot = pend_boot;
                pthread_mutex_lock(&lk); st.sent++; pthread_mutex_unlock(&lk);
                reached();
            }
            if (sock >= 0 && !connecting && !probing && (!pend_n || pend_flush())) { blocked = 0; cur_run(&ship, take_ship, b); }
            pos_save(0);
            long long bh = behind();
            pthread_mutex_lock(&lk); st.behind = bh; pthread_mutex_unlock(&lk);
            ms = mono_ms();
            if (sock < 0) next = retry_at - ms;
            else if (connecting) next = connect_by - ms;
            else if (probing) next = probe_by - ms;
            else if (bh > 0 && !blocked && tokens < 1) next = (long long)((BATCH - tokens) * 1000 / RATE) + 1;
            if (saved_off != ship.off || saved_old != ship.old) { long long s = saved_at + SAVE_MS - ms; if (s < next) next = s; }
        }
        if (wd < 0 && watch(in, &wd) < 0 && next > 1000) next = 1000;   /* no boot.log yet, or no inotify: look each second */
        if (next < 0) next = 0;

        /* the socket: its connect, the rest of a message or room after a full buffer (POLLOUT, only then: it is
         * nearly always writable), a TCP server's end (POLLIN), a UDP error the kernel learnt (POLLERR, always) */
        struct pollfd pf[3] = { { wake_fd[0], POLLIN, 0 }, { in, POLLIN, 0 }, { sock, 0, 0 } };
        if (sock >= 0) pf[2].events = (short)((connecting || pend_n || blocked ? POLLOUT : 0) | (now.tcp && !connecting ? POLLIN : 0));
        int r = poll(pf, sock >= 0 ? 3 : 2, (int)(next > 30000 ? 30000 : next));
        if (r < 0 && errno != EINTR) { usleep(100000); continue; }
        if (pf[0].revents) { char d[64]; while (read(wake_fd[0], d, sizeof d) > 0) {} }
        if (pf[1].revents) {
            char e[4096] __attribute__((aligned(__alignof__(struct inotify_event)))); ssize_t k;
            while ((k = read(in, e, sizeof e)) > 0)
                for (char *p = e; p < e + k; p += sizeof(struct inotify_event) + ((struct inotify_event *)p)->len)
                    if (((struct inotify_event *)p)->wd == wd && ((struct inotify_event *)p)->mask & (IN_IGNORED | IN_MOVE_SELF | IN_DELETE_SELF)) wd = -1;
        }
        if (sock >= 0 && pf[2].revents) {
            if (connecting) {
                int e = 0; socklen_t el = sizeof e;
                getsockopt(sock, SOL_SOCKET, SO_ERROR, &e, &el);
                if (e) failed("connect", e); else { connecting = 0; reached(); }
            } else if (now.tcp && pf[2].revents & (POLLIN | POLLHUP | POLLERR)) {
                char d[256]; ssize_t k = recv(sock, d, sizeof d, MSG_DONTWAIT);   /* servers say nothing: this is its end */
                if (k == 0 || (k < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) failed("connection", k ? errno : ECONNRESET);
            } else if (!now.tcp && pf[2].revents & POLLERR) {
                int e = 0; socklen_t el = sizeof e;
                getsockopt(sock, SOL_SOCKET, SO_ERROR, &e, &el);
                if (e) failed("send", e);
            }
        }
    }
    return NULL;
}

void logfwd_start(const char *n, void (*live)(int, const char *, size_t))
{
    static int started; pthread_t t; FILE *f;
    if (started++) return;
    if (n && *n) node = n;
    live_fn = live;
    const char *e = getenv("HASSMIC_SYSLOG_SAVE_MS"); if (e) SAVE_MS = atoi(e);
    if ((e = getenv("HASSMIC_BOOT_ID"))) snprintf(my_boot, sizeof my_boot, "%s", e);
    else if ((f = fopen("/proc/sys/kernel/random/boot_id", "r"))) { if (!fgets(my_boot, sizeof my_boot, f)) my_boot[0] = 0; fclose(f); my_boot[strcspn(my_boot, "\n")] = 0; }
    if (pipe(wake_fd) == 0) {
        fcntl(wake_fd[0], F_SETFL, O_NONBLOCK); fcntl(wake_fd[1], F_SETFL, O_NONBLOCK);
        fcntl(wake_fd[0], F_SETFD, FD_CLOEXEC); fcntl(wake_fd[1], F_SETFD, FD_CLOEXEC);
    }
    tokens_at = mono_ms();
    if (pthread_create(&t, NULL, thread, NULL)) { fprintf(stderr, "syslog: no thread, no log forwarding\n"); return; }
    pthread_detach(t);
}
