/*
 * The Echo Wall Clock (Amazon, 2018; FCC ID UUU-8459): an Alexa Gadget on Bluetooth Classic.  Its partner Echo sets its
 * hands and shows the Echo's timers on the 60 LEDs around its face; it has no clock source and no timers of its own.
 * What goes over the line is mpjanofsky/echo-wall-clock's PROTOCOL.md (reverse-engineered October 2026 on a real clock
 * against an Echo Dot, Apache 2.0), which matches Amazon's archived Alexa Gadgets Toolkit:
 *
 *   transport  RFCOMM channel 4 on the clock ("SPP Port 4 DATA"; channel 2 is its firmware update: never written to).
 *   frame      F0 | 02 | 00 | seq | escaped(payload | sum16) | F1.  sum16 = 02 + 00 + the payload's bytes, big-endian;
 *              F0, F1, F2 inside go as F2, byte ^ F2.  Our seq starts at 1 and skips F0..F2 (never escaped).
 *   payload    protobuf: Message { 1: Directive { 1: Header { 1 namespace, 2 name }, 2: payload } }.  The clock
 *              silently ignores a directive with fields a real Echo leaves out (messageId, dialogRequestId, an empty
 *              payload): these are built byte for byte as the capture has them (tests/unit/wallclock_test.c).
 *   connect    Alexa.Discovery Discover; the clock answers Discover.Response (its name, firmware, model, device token);
 *              then StateListener StateUpdate timeinfo = local time, Notifications ClearIndicator, the alerts.
 *   timers     Alerts SetAlert { token, "TIMER", end as local ISO 8601 with offset }: the ring counts down to it by
 *              itself (one minute per LED, the last minute by the second).  Paused or cancelled: DeleteAlert; resumed
 *              or time added: SetAlert again with the same token.  At the end the clock just goes dark; the Echo says
 *              StateUpdate timers = active once (the clock pulses ~10 s), and when the ring is stopped DeleteAlert
 *              plus timers = cleared.  Exactly what an Echo Dot sent in the capture (PROTOCOL.md 5b).
 *   time       the clock shows the digits it is sent and ignores the offset, so it must be local time: Home
 *              Assistant's POSIX TZ rule (the screen's, proto_esphome.c) evaluated here, the system's zone until then.
 *              Sent again every 30 minutes, as the Echo Dot did (that also kept the link from dying at ~31 minutes
 *              idle, Second Hand found), and at once when the zone changes or the clock jumps.
 *
 * Timer tokens are ten decimal digits made from Home Assistant's timer id (stable across reconnects); the Echo's were
 * 8-10 digit numbers and the clock takes any string.  The core hands the whole set over on every change (main.c
 * timers_tell); the controller thread compares it with what the clock was told and sends the difference.
 */
#include "wallclock.h"
#include <ctype.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TIMERS 12                   /* the core's running and ringing ones together */
#define TIMEINFO_MS (30 * 60 * 1000LL)
#define DISCOVER_MS 10000               /* Second Hand's handshake timeout; the clock answered in ~1 s */
#define WALL_MIN 1735689600             /* 2025-01-01: the Echo's clock was set (from Home Assistant: clock.c) */
static int pace_ms;                     /* between two messages (wallclock_pace): none, as an Echo Show sends them */

/* ---------------------------------------------------------------- what the core says (any thread) */

static pthread_mutex_t wl = PTHREAD_MUTEX_INITIALIZER;
static struct wc_timer want[MAX_TIMERS];
static int nwant, want_gen, tz_gen;     /* wl */
static char tz_rule[128];               /* wl */
static void (*poke_fn)(void);

void wallclock_poke(void (*poke)(void)) { poke_fn = poke; }

void wallclock_timers(const struct wc_timer *t, int n)
{
    pthread_mutex_lock(&wl);
    nwant = n < MAX_TIMERS ? n : MAX_TIMERS;
    if (nwant) memcpy(want, t, sizeof *want * (size_t)nwant);
    want_gen++;
    pthread_mutex_unlock(&wl);
    if (poke_fn) poke_fn();
}

void wallclock_tz(const char *posix)
{
    pthread_mutex_lock(&wl);
    int changed = strlen(posix) < sizeof tz_rule && strcmp(posix, tz_rule);
    if (changed) { snprintf(tz_rule, sizeof tz_rule, "%s", posix); tz_gen++; }
    pthread_mutex_unlock(&wl);
    if (changed && poke_fn) poke_fn();
}

/* ---------------------------------------------------------------- local time by a POSIX TZ rule */

struct rule { int kind, m, w, d, n, t; };       /* kind 'J', 'n' (day of year from 0) or 'M'; t: seconds of the local day */

static const char *tz_name(const char *p)
{
    if (*p == '<') { const char *q = strchr(p, '>'); return q && q > p + 1 ? q + 1 : NULL; }
    const char *q = p; while (isalpha((unsigned char)*q)) q++;
    return q - p >= 3 ? q : NULL;
}

static const char *tz_hms(const char *p, int *sec, int max_h)   /* [+-]hh[:mm[:ss]] */
{
    int sign = 1, v[3] = { 0, 0, 0 };
    if (*p == '+' || *p == '-') sign = *p++ == '-' ? -1 : 1;
    for (int i = 0; i < 3; i++) {
        if (!isdigit((unsigned char)*p)) return i ? p - 1 : NULL;          /* "hh:" without minutes: not ours */
        while (isdigit((unsigned char)*p)) { v[i] = v[i] * 10 + (*p++ - '0'); if (v[i] > 999) return NULL; }
        if (*p != ':' || i == 2) break;
        p++;
    }
    if (v[0] > max_h || v[1] > 59 || v[2] > 59) return NULL;
    *sec = sign * (v[0] * 3600 + v[1] * 60 + v[2]);
    return p;
}

static const char *tz_rule_of(const char *p, struct rule *r)
{
    char *e;
    r->t = 7200;
    if (*p == 'M') {
        r->kind = 'M';
        r->m = (int)strtol(p + 1, &e, 10); if (*e != '.') return NULL;
        r->w = (int)strtol(e + 1, &e, 10); if (*e != '.') return NULL;
        r->d = (int)strtol(e + 1, &e, 10);
        if (r->m < 1 || r->m > 12 || r->w < 1 || r->w > 5 || r->d < 0 || r->d > 6) return NULL;
    } else {
        r->kind = *p == 'J' ? 'J' : 'n';
        if (*p == 'J') p++;
        if (!isdigit((unsigned char)*p)) return NULL;
        r->n = (int)strtol(p, &e, 10);
        if (r->n < (r->kind == 'J') || r->n > 365) return NULL;
    }
    p = e;
    if (*p == '/' && !(p = tz_hms(p + 1, &r->t, 167))) return NULL;
    return p;
}

static long long days_from_civil(int y, int m, int d)          /* days since 1970-01-01 (Howard Hinnant's) */
{
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - (int)era * 400, doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int leap(int y) { return (y % 4 == 0 && y % 100) || y % 400 == 0; }

static long long rule_day(const struct rule *r, int y)          /* the day the rule names in year y, days since 1970 */
{
    static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    long long jan1 = days_from_civil(y, 1, 1);
    if (r->kind == 'J') return jan1 + r->n - 1 + (leap(y) && r->n >= 60);
    if (r->kind == 'n') return jan1 + r->n;
    long long first = days_from_civil(y, r->m, 1);
    int wd = (int)((first % 7 + 11) % 7);                       /* 1970-01-01 was a Thursday: 4 */
    int day = 1 + (r->d - wd + 7) % 7 + (r->w - 1) * 7, len = mdays[r->m - 1] + (r->m == 2 && leap(y));
    while (day > len) day -= 7;
    return first + day - 1;
}

int wallclock_offset(const char *posix, time_t utc, int *east)
{
    const char *p = tz_name(posix); int std, dst; struct rule a = { 'M', 3, 2, 0, 0, 7200 }, b = { 'M', 11, 1, 0, 0, 7200 };
    *east = 0;
    if (!p || !(p = tz_hms(p, &std, 24))) return -1;
    std = -std;                                                 /* POSIX counts west of Greenwich */
    if (!*p) { *east = std; return 0; }
    if (!(p = tz_name(p))) return -1;
    dst = std + 3600;
    if (*p && *p != ',') { if (!(p = tz_hms(p, &dst, 24))) return -1; dst = -dst; }
    if (*p == ',') {                                            /* no rule: the US one, as glibc and tzcode assume */
        if (!(p = tz_rule_of(p + 1, &a)) || *p != ',' || !(p = tz_rule_of(p + 1, &b))) return -1;
    }
    if (*p) return -1;
    struct tm tm; time_t t = utc; gmtime_r(&t, &tm);
    int y = tm.tm_year + 1900;
    long long start = rule_day(&a, y) * 86400 + a.t - std, end = rule_day(&b, y) * 86400 + b.t - dst, u = utc;
    int in = start < end ? u >= start && u < end : !(u >= end && u < start);
    *east = in ? dst : std;
    return 0;
}

void wallclock_iso(char *out, size_t cap, time_t utc, const char *posix)
{
    int east = 0; struct tm tm;
    if (!posix || !posix[0] || wallclock_offset(posix, utc, &east) < 0) {        /* the system's zone */
        localtime_r(&utc, &tm);
        east = (int)tm.tm_gmtoff;
    }
    time_t l = utc + east; gmtime_r(&l, &tm);
    int a = east < 0 ? -east : east;
    snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, east < 0 ? '-' : '+', a / 3600, a / 60 % 60);
}

/* ---------------------------------------------------------------- frames and messages */

size_t wallclock_frame(unsigned char *out, size_t cap, int seq, const unsigned char *payload, size_t n)
{
    unsigned sum = 0x02; size_t o = 0;
    if (cap < 6 + 2 * (n + 2)) return 0;
    out[o++] = 0xf0; out[o++] = 0x02; out[o++] = 0x00; out[o++] = seq;
    for (size_t i = 0; i < n + 2; i++) {
        unsigned char b = i < n ? payload[i] : i == n ? (sum >> 8) & 0xff : sum & 0xff;
        if (i < n) sum += payload[i];
        if (b >= 0xf0 && b <= 0xf2) { out[o++] = 0xf2; out[o++] = b ^ 0xf2; } else out[o++] = b;
    }
    out[o++] = 0xf1;
    return o;
}

struct pb { unsigned char b[512]; size_t n; int bad; };

static void pb_raw(struct pb *p, const void *d, size_t n) { if (p->n + n > sizeof p->b) { p->bad = 1; return; } memcpy(p->b + p->n, d, n); p->n += n; }
static void pb_varint(struct pb *p, unsigned long v) { do { unsigned char c = (v & 0x7f) | (v > 0x7f ? 0x80 : 0); pb_raw(p, &c, 1); v >>= 7; } while (v); }
static void pb_bytes(struct pb *p, int field, const void *d, size_t n) { pb_varint(p, (unsigned long)field << 3 | 2); pb_varint(p, n); pb_raw(p, d, n); }
static void pb_str(struct pb *p, int field, const char *s) { pb_bytes(p, field, s, strlen(s)); }

/* Message { Directive { Header { namespace, name }, payload (left out when empty, as the Echo does) } } */
static size_t directive(unsigned char *out, const char *ns, const char *name, const struct pb *payload)
{
    struct pb h = { .n = 0 }, d = { .n = 0 }, m = { .n = 0 };
    pb_str(&h, 1, ns); pb_str(&h, 2, name);
    pb_bytes(&d, 1, h.b, h.n);
    if (payload && payload->n) pb_bytes(&d, 2, payload->b, payload->n);
    pb_bytes(&m, 1, d.b, d.n);
    if (h.bad || d.bad || m.bad || (payload && payload->bad)) return 0;
    memcpy(out, m.b, m.n);
    return m.n;
}

/* field `want` of a protobuf message as bytes: 1 found */
static int pb_get(const unsigned char *p, size_t n, int want, const unsigned char **v, size_t *vn)
{
    size_t i = 0;
    while (i < n) {
        unsigned long key = 0, len = 0; int sh = 0;
        do { if (i >= n || sh > 28) return 0; key |= (unsigned long)(p[i] & 0x7f) << sh; sh += 7; } while (p[i++] & 0x80);
        int wt = key & 7;
        if (wt == 0) { while (i < n && p[i] & 0x80) i++; i++; continue; }
        if (wt == 1) { i += 8; continue; }
        if (wt == 5) { i += 4; continue; }
        if (wt != 2) return 0;
        sh = 0;
        do { if (i >= n || sh > 28) return 0; len |= (unsigned long)(p[i] & 0x7f) << sh; sh += 7; } while (p[i++] & 0x80);
        if (len > n - i) return 0;
        if ((int)(key >> 3) == want) { *v = p + i; *vn = len; return 1; }
        i += len;
    }
    return 0;
}

static void pb_text(const unsigned char *p, size_t n, int field, char *out, size_t cap)
{
    const unsigned char *v; size_t vn; out[0] = 0;
    if (!pb_get(p, n, field, &v, &vn)) return;
    size_t k = vn < cap - 1 ? vn : cap - 1;
    for (size_t i = 0; i < k; i++) out[i] = v[i] >= 0x20 && v[i] < 0x7f ? v[i] : '?';
    out[k] = 0;
}

/* ---------------------------------------------------------------- the session (controller thread) */

enum { T_NONE, T_SET, T_PAUSED, T_RINGING, T_CLEARING };   /* T_CLEARING: rung, DeleteAlert sent, "cleared" not yet */
static struct { char id[40], token[12]; int st, seen; long long end_s; } sent[MAX_TIMERS];
static int nsent;
static int (*send_fn)(void *, const void *, size_t); static void *send_ctx;
static int up, discovered, greeted, seq, sent_tz_gen = -1, need_time, sent_off;   /* greeted: 1 time sent, 2 the light too;
                                                                                    sent_off: the UTC offset it went in */
static long long disc_at, next_timeinfo, wall_minus_mono;       /* ms */
static unsigned char rx[1024]; static size_t rxn; static int in_frame, esc;
static char clock_name[64];

static long long fake_real;
void wallclock_fake_time(long long ms) { fake_real = ms; }
static long long real_ms(void) { struct timespec t; if (fake_real) return fake_real; clock_gettime(CLOCK_REALTIME, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

static long long cur_ms, last_sent_ms;     /* the tick's time; when the last message went */

void wallclock_pace(int ms) { pace_ms = ms; }

static int send_msg(const unsigned char *payload, size_t n, const char *what)
{
    unsigned char f[1100]; size_t fn;
    if (last_sent_ms && cur_ms - last_sent_ms < pace_ms) return -1;      /* the next tick sends it */
    if (!n || !send_fn || !(fn = wallclock_frame(f, sizeof f, seq, payload, n))) return -1;
    if (send_fn(send_ctx, f, fn) < 0) return -1;
    last_sent_ms = cur_ms ? cur_ms : 1;
    if (++seq > 0xff) seq = 0;
    while (seq >= 0xf0 && seq <= 0xf2) seq++;
    fprintf(stderr, "wallclock: %s\n", what);
    return 0;
}

static int send_directive(const char *ns, const char *name, const struct pb *payload, const char *what)
{
    unsigned char m[600]; size_t n = directive(m, ns, name, payload);
    return send_msg(m, n, what);
}

static int send_state(const char *name, const char *value)      /* StateListener StateUpdate { states { name, value } } */
{
    struct pb s = { .n = 0 }, p = { .n = 0 }; char what[96];
    pb_str(&s, 1, name); pb_str(&s, 2, value);
    pb_bytes(&p, 1, s.b, s.n);
    snprintf(what, sizeof what, "%s = %s", name, value);
    return send_directive("Alexa.Gadget.StateListener", "StateUpdate", &p, what);
}

static int send_set(const char *token, long long end_s, const char *tz)
{
    char iso[40], what[96]; struct pb p = { .n = 0 };
    wallclock_iso(iso, sizeof iso, (time_t)end_s, tz);
    pb_str(&p, 1, token); pb_str(&p, 2, "TIMER"); pb_str(&p, 3, iso);
    snprintf(what, sizeof what, "timer %.11s ends %.39s", token, iso);
    return send_directive("Alerts", "SetAlert", &p, what);
}

static int send_delete(const char *token)
{
    char what[64]; struct pb p = { .n = 0 };
    pb_str(&p, 1, token);
    snprintf(what, sizeof what, "timer %s off", token);
    return send_directive("Alerts", "DeleteAlert", &p, what);
}

static void token_of(const char *id, char *out)                 /* FNV-1a of the id, as ten digits */
{
    uint32_t h = 2166136261u;
    for (const char *c = id; *c; c++) { h ^= (unsigned char)*c; h *= 16777619u; }
    snprintf(out, 12, "%lu", 1000000000UL + (unsigned long)(h % 3000000000UL));
}

void wallclock_up(int (*send)(void *ctx, const void *p, size_t n), void *ctx, long long now)
{
    send_fn = send; send_ctx = ctx; up = 1; discovered = greeted = need_time = 0; seq = 1; nsent = 0; rxn = 0; in_frame = esc = 0;
    sent_tz_gen = -1; clock_name[0] = 0;
    disc_at = now ? now : 1; cur_ms = now; last_sent_ms = 0;
    send_directive("Alexa.Discovery", "Discover", NULL, "Discover");
}

void wallclock_down(void)
{
    if (up && discovered) fprintf(stderr, "wallclock: gone\n");
    up = discovered = greeted = 0; send_fn = NULL; nsent = 0; disc_at = 0;
}

int wallclock_ready(void) { return up && discovered; }
void wallclock_name(char *buf, size_t n) { snprintf(buf, n, "%s", discovered ? clock_name : ""); }

static void on_frame(const unsigned char *p, size_t n, long long now)
{
    (void)now;
    if (n < 5) return;
    unsigned sum = p[0] + p[1]; size_t pn = n - 5;
    const unsigned char *pl = p + 3;
    for (size_t i = 0; i < pn; i++) sum += pl[i];
    if ((sum & 0xffff) != (unsigned)(p[n - 2] << 8 | p[n - 1])) { fprintf(stderr, "wallclock: frame with a bad checksum, ignored\n"); return; }
    const unsigned char *ev, *hd, *pay = NULL; size_t evn, hdn, payn = 0; char ns[48], name[48];
    if (!pb_get(pl, pn, 1, &ev, &evn) || !pb_get(ev, evn, 1, &hd, &hdn)) { fprintf(stderr, "wallclock: %zu bytes not understood\n", pn); return; }
    pb_text(hd, hdn, 1, ns, sizeof ns); pb_text(hd, hdn, 2, name, sizeof name);
    pb_get(ev, evn, 2, &pay, &payn);
    if (strcmp(ns, "Alexa.Discovery") || strcmp(name, "Discover.Response")) { fprintf(stderr, "wallclock: %s %s\n", ns, name); return; }
    const unsigned char *ep = NULL, *id = NULL; size_t epn = 0, idn = 0; char fw[24] = "", model[48] = "";
    if (pay && pb_get(pay, payn, 1, &ep, &epn)) {               /* endpoints[0]: 2 friendlyName, 12 additionalIdentification */
        pb_text(ep, epn, 2, clock_name, sizeof clock_name);
        if (pb_get(ep, epn, 12, &id, &idn)) { pb_text(id, idn, 1, fw, sizeof fw); pb_text(id, idn, 5, model, sizeof model); }
    }
    if (!clock_name[0]) snprintf(clock_name, sizeof clock_name, "Wall clock");
    if (!discovered) fprintf(stderr, "wallclock: %s here (%s, firmware %s)\n", clock_name, model[0] ? model : "model unknown", fw[0] ? fw : "unknown");
    discovered = 1; disc_at = 0;
    if (poke_fn) poke_fn();
}

void wallclock_rx(const unsigned char *p, size_t n, long long now)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char b = p[i];
        if (b == 0xf0) { in_frame = 1; esc = 0; rxn = 0; continue; }
        if (!in_frame) continue;
        if (b == 0xf1) { in_frame = 0; on_frame(rx, rxn, now); continue; }
        if (b == 0xf2) { esc = 1; continue; }
        if (esc) { b ^= 0xf2; esc = 0; }
        if (rxn == sizeof rx) { in_frame = 0; fprintf(stderr, "wallclock: frame too long, dropped\n"); continue; }
        rx[rxn++] = b;
    }
}

int wallclock_tick(long long now)
{
    if (!up) return 0;
    cur_ms = now;
    if (!discovered) {
        if (disc_at && now - disc_at > DISCOVER_MS) { fprintf(stderr, "wallclock: no answer to Discover\n"); disc_at = 0; return -1; }
        return 0;
    }
    long long real = real_ms();
    if (real / 1000 < WALL_MIN) return 0;                       /* the Echo's own clock is not set yet */
    char tz[128]; struct wc_timer w[MAX_TIMERS]; int nw, tzg;
    pthread_mutex_lock(&wl);
    snprintf(tz, sizeof tz, "%s", tz_rule); tzg = tz_gen; nw = nwant; memcpy(w, want, sizeof *w * (size_t)nw);
    pthread_mutex_unlock(&wl);
    long long wm = real - now;
    if (greeted == 2 && llabs(wm - wall_minus_mono) > 5000) {    /* the Echo's clock jumped: the time and the ends again */
        need_time = 1;
        for (int i = 0; i < nsent; i++) if (sent[i].st == T_SET) sent[i].end_s = 0;
    }
    wall_minus_mono = wm;
    int off = 0;                                        /* summer time began or ended: the clock shows digits, so again */
    { time_t t = (time_t)(real / 1000); struct tm tm;
      if (!tz[0] || wallclock_offset(tz, t, &off) < 0) { localtime_r(&t, &tm); off = (int)tm.tm_gmtoff; } }
    if (greeted == 2 && off != sent_off) { need_time = 1; for (int i = 0; i < nsent; i++) if (sent[i].st == T_SET) sent[i].end_s = 0; }
    if (greeted && tzg != sent_tz_gen)                  /* a new zone: the ends' local digits are new too */
        for (int i = 0; i < nsent; i++) if (sent[i].st == T_SET) sent[i].end_s = 0;
    if (!greeted || need_time || now >= next_timeinfo || tzg != sent_tz_gen) {
        char iso[40]; wallclock_iso(iso, sizeof iso, (time_t)(real / 1000), tz);
        if (send_state("timeinfo", iso) < 0) return 0;
        next_timeinfo = now + TIMEINFO_MS; sent_tz_gen = tzg; need_time = 0; sent_off = off;
        if (!greeted) greeted = 1;
    }
    if (greeted == 1) {                                         /* then, once a connection: the light off */
        if (send_directive("Notifications", "ClearIndicator", NULL, "notification light off") < 0) return 0;
        greeted = 2;
    }
    for (int i = 0; i < nsent; i++) sent[i].seen = 0;
    for (int k = 0; k < nw; k++) {
        int i = 0;
        while (i < nsent && strcmp(sent[i].id, w[k].id)) i++;
        if (i == nsent) {
            if (nsent == MAX_TIMERS) continue;
            memset(&sent[i], 0, sizeof sent[i]); nsent++;
            memcpy(sent[i].id, w[k].id, sizeof sent[i].id); sent[i].id[sizeof sent[i].id - 1] = 0; token_of(sent[i].id, sent[i].token);
        }
        sent[i].seen = 1;
        if (w[k].state == WC_RUNNING) {
            long long end_s = (real + (w[k].end_ms - now) + 500) / 1000;
            if (sent[i].st != T_SET || llabs(end_s - sent[i].end_s) > 1) {
                if (send_set(sent[i].token, end_s, tz) < 0) return 0;
                sent[i].st = T_SET; sent[i].end_s = end_s;
            }
        } else if (w[k].state == WC_PAUSED) {
            if (sent[i].st == T_SET && send_delete(sent[i].token) < 0) return 0;
            sent[i].st = T_PAUSED;
        } else if (sent[i].st != T_RINGING) {
            if (send_state("timers", "active") < 0) return 0;
            sent[i].st = T_RINGING;
        }
    }
    for (int i = 0; i < nsent; ) {
        if (sent[i].seen) { i++; continue; }
        if (sent[i].st == T_SET || sent[i].st == T_RINGING) {
            if (send_delete(sent[i].token) < 0) return 0;
            sent[i].st = sent[i].st == T_RINGING ? T_CLEARING : T_NONE;
        }
        if (sent[i].st == T_CLEARING && send_state("timers", "cleared") < 0) return 0;
        sent[i] = sent[--nsent];
    }
    return 0;
}
