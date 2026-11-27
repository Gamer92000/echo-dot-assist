/* wallclock.c: the Alexa Gadgets frames an Echo sends the Echo Wall Clock, byte for byte as Second Hand's golden frames
 * have them (made from a capture of a real Echo Dot: github.com/mpjanofsky/second-hand-integration
 * fixtures/golden-frames.txt, Apache 2.0); the order on connect, a timer's life (set, paused, resumed, time added,
 * cancelled, rung and stopped), the half-hourly time, a jump of the clock, the Discover timeout, frames from the clock
 * in pieces and with escapes; and local time by POSIX TZ rules against glibc's. */
#define _GNU_SOURCE
#include "wallclock.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails;
#define CHECK(c, what) do { if (c) printf("ok   %s\n", what); else { printf("FAIL %s\n", what); fails++; } } while (0)

static unsigned char sent[64][1200]; static size_t slen[64]; static int nsent, refuse;
static int sendf(void *c, const void *p, size_t n) { (void)c; if (refuse || nsent == 64) return -1; memcpy(sent[nsent], p, n); slen[nsent++] = n; return 0; }

static size_t unhex(const char *h, unsigned char *out)
{
    size_t n = 0; unsigned v;
    while (*h) { while (*h == ' ') h++; if (!*h || sscanf(h, "%2x", &v) != 1) break; out[n++] = v; h += 2; }
    return n;
}

static int is(int i, const char *hex)
{
    unsigned char b[1200]; size_t n = unhex(hex, b);
    if (i >= nsent || slen[i] != n || memcmp(sent[i], b, n)) {
        if (i < nsent) { printf("     got  "); for (size_t k = 0; k < slen[i]; k++) printf("%02x ", sent[i][k]); printf("\n"); }
        return 0;
    }
    return 1;
}

/* the frames' payloads, unescaped, as text: to find names and values in them */
static int has(int i, const char *text)
{
    if (i >= nsent) return 0;
    unsigned char b[1200]; size_t n = 0;
    for (size_t k = 4; k + 1 < slen[i]; k++) { if (sent[i][k] == 0xf2) { b[n++] = sent[i][++k] ^ 0xf2; } else b[n++] = sent[i][k]; }
    return memmem(b, n, text, strlen(text)) != NULL;
}

/* a protobuf length-delimited field, written here on its own (not wallclock.c's) */
static size_t fld(unsigned char *o, int f, const void *d, size_t n) { o[0] = f << 3 | 2; o[1] = n; memcpy(o + 2, d, n); return n + 2; }
static size_t set_alert_payload(unsigned char *o, const char *token, const char *end)
{
    unsigned char h[64], p[128], d[256]; size_t hn = 0, pn = 0, dn = 0;
    hn += fld(h + hn, 1, "Alerts", 6); hn += fld(h + hn, 2, "SetAlert", 8);
    pn += fld(p + pn, 1, token, strlen(token)); pn += fld(p + pn, 2, "TIMER", 5); pn += fld(p + pn, 3, end, strlen(end));
    dn += fld(d + dn, 1, h, hn); dn += fld(d + dn, 2, p, pn);
    return fld(o, 1, d, dn);
}

static const char *G_DISCOVER = "f0 02 00 01 0a 1d 0a 1b 0a 0f 41 6c 65 78 61 2e 44 69 73 63 6f 76 65 72 79 12 08 44 69 73 63 6f 76 65 72 09 91 f1";
static const char *G_TIMEINFO = "f0 02 00 02 0a 54 0a 29 0a 1a 41 6c 65 78 61 2e 47 61 64 67 65 74 2e 53 74 61 74 65 4c 69 73 74 65 6e 65 72 12 0b 53 74 61 74 65 55 70 64 61 74 65 12 27 0a 25 0a 08 74 69 6d 65 69 6e 66 6f 12 19 32 30 32 36 2d 31 30 2d 31 34 54 31 32 3a 30 30 3a 30 30 2d 30 34 3a 30 30 18 12 f1";
static const char *G_CLEAR = "f0 02 00 03 0a 21 0a 1f 0a 0d 4e 6f 74 69 66 69 63 61 74 69 6f 6e 73 12 0e 43 6c 65 61 72 49 6e 64 69 63 61 74 6f 72 0b 6b f1";
static const char *G_SET = "f0 02 00 04 0a 44 0a 12 0a 06 41 6c 65 72 74 73 12 08 53 65 74 41 6c 65 72 74 12 2e 0a 0a 31 32 33 34 35 36 37 38 39 30 12 05 54 49 4d 45 52 1a 19 32 30 32 36 2d 31 30 2d 31 34 54 31 32 3a 30 32 3a 30 30 2d 30 34 3a 30 30 0f 53 f1";
static const char *G_ACTIVE = "f0 02 00 05 0a 3f 0a 29 0a 1a 41 6c 65 78 61 2e 47 61 64 67 65 74 2e 53 74 61 74 65 4c 69 73 74 65 6e 65 72 12 0b 53 74 61 74 65 55 70 64 61 74 65 12 12 0a 10 0a 06 74 69 6d 65 72 73 12 06 61 63 74 69 76 65 14 73 f1";
static const char *G_CLEARED = "f0 02 00 07 0a 40 0a 29 0a 1a 41 6c 65 78 61 2e 47 61 64 67 65 74 2e 53 74 61 74 65 4c 69 73 74 65 6e 65 72 12 0b 53 74 61 74 65 55 70 64 61 74 65 12 13 0a 11 0a 06 74 69 6d 65 72 73 12 07 63 6c 65 61 72 65 64 14 cb f1";
static const char *G_RESPONSE = "f0 02 00 00 0a 2e 0a 24 0a 0f 41 6c 65 78 61 2e 44 69 73 63 6f 76 65 72 79 12 11 44 69 73 63 6f 76 65 72 2e 52 65 73 70 6f 6e 73 65 12 06 0a 04 74 65 73 74 0f 17 f1";

#define T0 1791993600000LL              /* 2026-10-14T16:00:00Z = 12:00 in New York */

static long long now0, jump;           /* the wall clock follows the monotonic one from now0, plus jump */
static int tick(long long t) { wallclock_fake_time(T0 + t - now0 + jump); return wallclock_tick(t); }
static void timers1(const char *id, int st, long long end) { struct wc_timer t = { .state = st, .end_ms = end }; snprintf(t.id, sizeof t.id, "%s", id); wallclock_timers(&t, 1); }

int main(void)
{
    /* ---- framing on its own: escapes and the checksum (Second Hand's vectors) */
    { unsigned char f[600], b[600]; size_t n;
      n = wallclock_frame(f, sizeof f, 1, (const unsigned char[]){ 0x01, 0xf0, 0x02, 0xf1, 0x03, 0xf2, 0x04 }, 7);
      CHECK(n == unhex("f0 02 00 01 01 f2 02 02 f2 03 03 f2 00 04 02 df f1", b) && !memcmp(f, b, n), "frame: F0 F1 F2 in the payload escaped");
      n = wallclock_frame(f, sizeof f, 1, (const unsigned char[]){ 0xee }, 1);
      CHECK(n == unhex("f0 02 00 01 ee 00 f2 02 f1", b) && !memcmp(f, b, n), "frame: the checksum's low byte F0 escaped");
      unsigned char ff[241]; memset(ff, 0xff, 240); ff[240] = 0xee;
      n = wallclock_frame(f, sizeof f, 1, ff, 241);
      CHECK(n == 4 + 241 + 3 + 1 && f[245] == 0xf2 && f[246] == 0x02 && f[247] == 0x00 && f[248] == 0xf1, "frame: the checksum's high byte F0 escaped"); }

    /* ---- connect: Discover; the answer; the time, the light, the alerts; seq 1, 2, 3...  (New York time) */
    long long now = now0 = 50000;
    wallclock_pace(0);                  /* the messages' order and bytes here; the pace below */
    wallclock_fake_time(T0);
    wallclock_tz("EST5EDT,M3.2.0,M11.1.0");
    timers1("01JTIMER", WC_RUNNING, now + 120000);
    wallclock_up(sendf, NULL, now);
    CHECK(nsent == 1 && is(0, G_DISCOVER), "connect: Discover, byte for byte the Echo's");
    CHECK(tick(now + 100) == 0 && nsent == 1 && !wallclock_ready(), "nothing more until the clock answers");
    { unsigned char r[200]; size_t n = unhex(G_RESPONSE, r);
      wallclock_rx(r, 20, now + 200); wallclock_rx(r + 20, n - 20, now + 210); }   /* in two pieces */
    CHECK(wallclock_ready(), "Discover.Response (in two pieces): a clock on the line");
    tick(now + 300);
    CHECK(nsent == 4, "then three messages");
    CHECK(is(1, G_TIMEINFO), "timeinfo = 2026-10-14T12:00:00-04:00, byte for byte");
    CHECK(is(2, G_CLEAR), "ClearIndicator, byte for byte");
    { unsigned char p[200], f[400]; size_t pn = set_alert_payload(p, "1234567890", "2026-10-14T12:02:00-04:00"), fn = wallclock_frame(f, sizeof f, 4, p, pn), gn;
      unsigned char g[400]; gn = unhex(G_SET, g);
      CHECK(fn == gn && !memcmp(f, g, gn), "(the test's own SetAlert matches the golden frame)");
      char tok[16] = ""; const unsigned char *t = memmem(sent[3], slen[3], "\x12\x2e\x0a\x0a", 4);
      if (t) memcpy(tok, t + 4, 10);
      pn = set_alert_payload(p, tok, "2026-10-14T12:02:00-04:00"); fn = wallclock_frame(f, sizeof f, 4, p, pn);
      CHECK(t && slen[3] == fn && !memcmp(sent[3], f, fn) && strspn(tok, "0123456789") == 10, "SetAlert TIMER, its end at 12:02:00-04:00, a ten digit token");
      tick(now + 400);
      CHECK(nsent == 4, "nothing again while nothing changed");

      /* ---- rung: timers = active once; stopped: DeleteAlert and timers = cleared */
      timers1("01JTIMER", WC_RINGING, 0);
      tick(now + 120500);
      CHECK(nsent == 5 && is(4, G_ACTIVE), "the timer rings: timers = active, byte for byte");
      tick(now + 121000);
      CHECK(nsent == 5, "active only once (the clock pulses ~10 s by itself)");
      wallclock_timers(NULL, 0);
      tick(now + 130000);
      unsigned char d[200]; size_t dn = 0; { unsigned char h[64], pl[32], dd[128]; size_t hn = 0, pln = 0, ddn = 0;
        hn += fld(h + hn, 1, "Alerts", 6); hn += fld(h + hn, 2, "DeleteAlert", 11); pln = fld(pl, 1, tok, 10);
        ddn += fld(dd + ddn, 1, h, hn); ddn += fld(dd + ddn, 2, pl, pln); dn = fld(d, 1, dd, ddn); }
      fn = wallclock_frame(f, sizeof f, 6, d, dn);
      CHECK(nsent == 7 && slen[5] == fn && !memcmp(sent[5], f, fn), "stopped: DeleteAlert (seq 6, as the golden one)");
      CHECK(is(6, G_CLEARED), "and timers = cleared, byte for byte"); }

    /* ---- a timer paused, resumed, given time, cancelled */
    nsent = 0;
    timers1("01JTEA", WC_RUNNING, now + 300000);
    tick(now + 131000);
    CHECK(nsent == 1 && has(0, "SetAlert") && has(0, "12:05:00-04:00"), "a 5 minute timer: SetAlert, end in New York time");
    timers1("01JTEA", WC_PAUSED, 0);
    tick(now + 140000);
    CHECK(nsent == 2 && has(1, "DeleteAlert"), "paused: DeleteAlert");
    timers1("01JTEA", WC_RUNNING, now + 400000);
    tick(now + 150000);
    CHECK(nsent == 3 && has(2, "SetAlert") && has(2, "12:06:40") && !memcmp(sent[2] + 30, sent[0] + 30, 10), "resumed: SetAlert, the same token");
    timers1("01JTEA", WC_RUNNING, now + 400400);
    tick(now + 151000);
    CHECK(nsent == 3, "an end a moment off (rounding): not sent again");
    timers1("01JTEA", WC_RUNNING, now + 460000);
    tick(now + 152000);
    CHECK(nsent == 4 && has(3, "SetAlert"), "a minute added: SetAlert again");
    wallclock_timers(NULL, 0);
    tick(now + 153000);
    CHECK(nsent == 5 && has(4, "DeleteAlert") && !has(4, "cleared"), "cancelled: DeleteAlert only");
    tick(now + 154000);
    CHECK(nsent == 5, "and nothing after");

    /* ---- the time every 30 minutes, and again when the zone or the clock change */
    tick(now + 1800200);
    CHECK(nsent == 5, "29 minutes 59.9 on: not yet");
    tick(now + 1800300);
    CHECK(nsent == 6 && has(5, "timeinfo") && has(5, "12:30:00-04:00"), "30 minutes on: timeinfo again");
    wallclock_tz("CET-1CEST,M3.5.0,M10.5.0/3");
    tick(now + 1801300);
    CHECK(nsent == 7 && has(6, "18:30:01+02:00"), "a new zone: timeinfo at once, in it");
    jump = 3600000;
    tick(now + 1802300);
    CHECK(nsent == 8 && has(7, "timeinfo"), "the Echo's clock jumped an hour: timeinfo at once");
    timers1("01JZONE", WC_RUNNING, now + 1802300 + 600000); tick(now + 1802310);
    wallclock_tz("EST5EDT,M3.2.0,M11.1.0"); int before = nsent; tick(now + 1802320);
    CHECK(nsent == before + 2 && has(before, "timeinfo") && has(before + 1, "SetAlert") && has(before + 1, "-04:00"),
          "a zone change: the time and the running timer's end again, in the new zone");
    wallclock_timers(NULL, 0); tick(now + 1802330); nsent = 8;

    /* ---- a send that fails is tried again */
    timers1("01JEGG", WC_RUNNING, now + 1802300 + 180000);
    refuse = 1; tick(now + 1802400); refuse = 0;
    CHECK(nsent == 8, "the channel full: nothing marked as sent");
    tick(now + 1802500);
    CHECK(nsent == 9 && has(8, "SetAlert"), "sent on the next tick");

    /* ---- the pace: a message at most every 300 ms */
    wallclock_pace(300);
    { struct wc_timer t[2] = { { "01JEGG", WC_PAUSED, 0 }, { "01JHAM", WC_RUNNING, now + 1802500 + 300000 } }; wallclock_timers(t, 2); }
    nsent = 0; tick(now + 1803000);
    CHECK(nsent == 1, "two changes at once: one message now");
    tick(now + 1803200);
    CHECK(nsent == 1, "200 ms on: still one");
    tick(now + 1803300);
    CHECK(nsent == 2, "300 ms on: the second");
    wallclock_pace(0);

    /* ---- frames from the clock: a bad checksum, other events, escapes */
    { unsigned char r[200]; size_t n = unhex(G_RESPONSE, r); r[n - 2] ^= 1; wallclock_rx(r, n, 0); CHECK(wallclock_ready(), "a frame with a bad checksum changes nothing"); }

    /* ---- a reconnect starts over; no answer to Discover in 10 s ends it */
    wallclock_down();
    CHECK(!wallclock_ready(), "down: no clock");
    nsent = 0;
    wallclock_up(sendf, NULL, 900000);
    CHECK(nsent == 1 && is(0, G_DISCOVER), "up again: Discover with seq 1");
    CHECK(wallclock_tick(900000 + 9000) == 0 && wallclock_tick(900000 + 10001) < 0, "no Discover.Response in 10 s: the tick says drop it");
    wallclock_down();

    /* ---- a richer answer: the name, firmware and model logged; the name kept */
    { unsigned char id[64], ep[128], pay[160], hd[64], ev[256], m[300], f[700]; size_t idn = 0, epn = 0, payn, hdn = 0, evn = 0, mn, fn; char name[40];
      idn += fld(id + idn, 1, "102", 3); idn += fld(id + idn, 5, "AlexaGadgetsEWC", 15);
      epn += fld(ep + epn, 1, "TEST000000000000", 16); epn += fld(ep + epn, 2, "EchoWallClk", 11); epn += fld(ep + epn, 12, id, idn);
      payn = fld(pay, 1, ep, epn);
      hdn += fld(hd + hdn, 1, "Alexa.Discovery", 15); hdn += fld(hd + hdn, 2, "Discover.Response", 17);
      evn += fld(ev + evn, 1, hd, hdn); evn += fld(ev + evn, 2, pay, payn); mn = fld(m, 1, ev, evn);
      fn = wallclock_frame(f, sizeof f, 0, m, mn);
      nsent = 0; wallclock_up(sendf, NULL, 1000000); wallclock_rx(f, fn, 1000100);
      wallclock_name(name, sizeof name);
      CHECK(wallclock_ready() && !strcmp(name, "EchoWallClk"), "its name from Discover.Response (endpoint field 2)");
      wallclock_down(); }

    /* ---- summer time ends (New York, 2026-11-01 06:00 UTC: 02:00 EDT -> 01:00 EST): the time and the ends again */
    { long long t1 = 2000000; unsigned char r[200]; size_t rn = unhex(G_RESPONSE, r);
      wallclock_tz("EST5EDT,M3.2.0,M11.1.0");
      now0 = t1; jump = (1793512800000LL - 60000) - T0;          /* the wall clock one minute before the change */
      nsent = 0; wallclock_up(sendf, NULL, t1); wallclock_rx(r, rn, t1 + 10);
      timers1("01JDST", WC_RUNNING, t1 + 600000);
      tick(t1 + 20); tick(t1 + 30);
      int b = nsent;
      tick(t1 + 30000);
      CHECK(b >= 4 && nsent == b, "half a minute before summer time ends: nothing new");
      tick(t1 + 61000);
      CHECK(nsent == b + 2 && has(b, "01:00:") && has(b, "-05:00") && has(b + 1, "SetAlert") && has(b + 1, "-05:00"),
            "it ends: timeinfo at once in standard time, the running timer's end again");
      wallclock_timers(NULL, 0); wallclock_down(); }

    /* ---- local time by POSIX rules, against glibc */
    static const char *rules[] = {
        "EST5EDT,M3.2.0,M11.1.0", "CET-1CEST,M3.5.0,M10.5.0/3", "AEST-10AEDT,M10.1.0,M4.1.0/3", "<+0545>-5:45", "<-03>3",
        "IST-1GMT0,M10.5.0,M3.5.0/1", "NZST-12NZDT,M9.5.0,M4.1.0/3", "PST8PDT,M3.2.0,M11.1.0", "WET0WEST,M3.5.0/1,M10.5.0",
        "<+1030>-10:30<+11>-11,M10.1.0,M4.1.0", "<-01>1<+00>,M3.5.0/0,M10.5.0/1", "GMT0BST,M3.5.0/1,M10.5.0", "MST7",
        "<-02>2<-01>,M3.5.0/-1,M10.5.0/0", "AKST9AKDT,M3.2.0,M11.1.0", "HST10", "<+12>-12", "<-11>11", "EET-2EEST,M3.5.0/3,M10.5.0/4",
        "XXX3YYY,J60/2,J300/2", "XXX3YYY,59,299/26",
    };
    int bad = 0, total = 0;
    for (size_t r = 0; r < sizeof rules / sizeof *rules; r++) {
        setenv("TZ", rules[r], 1); tzset();
        for (long long t = 1577836800; t < 2082758400; t += 3 * 3600 + 7) {        /* 2020 to 2036, every ~3 hours */
            for (int d = -1; d <= 1; d++) {
                time_t u = t + d; struct tm tm; int east;
                localtime_r(&u, &tm);
                if (wallclock_offset(rules[r], u, &east) < 0 || east != tm.tm_gmtoff) {
                    if (bad++ < 5) printf("     %s at %lld: ours %d, glibc %ld\n", rules[r], (long long)u, east, tm.tm_gmtoff);
                }
                total++;
            }
        }
    }
    { char m[96]; snprintf(m, sizeof m, "POSIX rules: the offset glibc gives, %d of %d instants", total - bad, total); CHECK(!bad, m); }
    /* every transition of 2026 to the second, both sides */
    bad = 0; total = 0;
    for (size_t r = 0; r < sizeof rules / sizeof *rules; r++) {
        setenv("TZ", rules[r], 1); tzset();
        struct tm prev; time_t u = 1767225600; localtime_r(&u, &prev);
        for (; u < 1798761600; u += 900) {
            struct tm tm; localtime_r(&u, &tm);
            if (tm.tm_gmtoff == prev.tm_gmtoff) continue;
            for (time_t v = u - 901; v <= u + 1; v++) {
                struct tm a; int east; localtime_r(&v, &a);
                if (wallclock_offset(rules[r], v, &east) < 0 || east != a.tm_gmtoff) bad++;
                total++;
            }
            prev = tm;
        }
    }
    { char m[96]; snprintf(m, sizeof m, "every transition of 2026 to the second: %d of %d", total - bad, total); CHECK(!bad && total > 0, m); }
    { int e; CHECK(wallclock_offset("12abc", 0, &e) < 0 && wallclock_offset("EST5EDT,M13.1.0,M11.1.0", 0, &e) < 0 && wallclock_offset("", 0, &e) < 0,
               "rules not understood are refused"); }
    { char iso[40]; wallclock_iso(iso, sizeof iso, 1791993600, "<+0545>-5:45"); CHECK(!strcmp(iso, "2026-10-14T21:45:00+05:45"), "ISO text with a 45 minute offset");
      wallclock_iso(iso, sizeof iso, 1791993600, "GMT0"); CHECK(!strcmp(iso, "2026-10-14T16:00:00+00:00"), "ISO text at UTC: +00:00"); }

    printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails != 0;
}
