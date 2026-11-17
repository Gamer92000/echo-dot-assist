/* logfwd.c's pieces: the server setting, a line's time and level, the syslog messages it makes (RFC 5424 and 3164, cut
 * on a UTF-8 boundary), the token blanked.  The thread itself: tests/fake_syslog.py */
#include "logfwd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void server(const char *s, int ok, const char *host, int port)
{
    char h[LOGFWD_SERVER_MAX + 1] = "x"; int p = 0;
    int r = logfwd_server_parse(s, h, sizeof h, &p);
    CHECK((r == 0) == ok, "\"%s\": %s expected", s, ok ? "taken" : "refused");
    if (ok && !r) CHECK(!strcmp(h, host) && p == port, "\"%s\": %s:%d, not %s:%d", s, h, p, host, port);
}

static void level(const char *m, int want)
{
    int l = logfwd_level(m, strlen(m));
    CHECK(l == want, "level of \"%s\": %d, not %d", m, l, want);
}

int main(void)
{
    setenv("TZ", "UTC", 1); tzset();

    server("", 1, "", LOGFWD_PORT);
    server("192.168.1.10", 1, "192.168.1.10", 514);
    server("192.168.1.10:5514", 1, "192.168.1.10", 5514);
    server("logs.lan", 1, "logs.lan", 514);
    server("nas.local:601", 1, "nas.local", 601);
    server("fd00::1", 1, "fd00::1", 514);                       /* a bare IPv6 address: its colons are no port */
    server("[fd00::1]:6514", 1, "fd00::1", 6514);
    server("[fd00::1]", 1, "fd00::1", 514);
    server("host:0", 0, "", 0);
    server("host:65536", 0, "", 0);
    server("host:", 0, "", 0);
    server("host:51x", 0, "", 0);
    server("[fd00::1", 0, "", 0);
    server("[]:514", 0, "", 0);
    server("[fd00::1]x", 0, "", 0);
    server("udp://host", 0, "", 0);
    server("bad host", 0, "", 0);
    server("-host", 0, "", 0);
    server("ho\nst", 0, "", 0);
    char longname[LOGFWD_SERVER_MAX + 2]; memset(longname, 'a', sizeof longname - 1); longname[sizeof longname - 1] = 0;
    server(longname, 0, "", 0);

    struct logfwd_time t; double b = -1; size_t skip;
    const char *l = "2026-10-06 15:18:02.417Z hello";
    CHECK(logfwd_stamp(l, strlen(l), &t, &b, &skip) == 1 && t.known && t.sec == 1791299882 && t.ms == 417 && skip == 25,
          "clock.c's stamp: %lld.%03d, skip %zu", t.sec, t.ms, skip);
    l = "2026-10-06 15:18:02Z == satellite start";
    CHECK(logfwd_stamp(l, strlen(l), &t, &b, &skip) == 1 && t.sec == 1791299882 && t.ms == 0 && skip == 21, "main.sh's stamp (no ms)");
    l = "boot+29935.512 web: 1 setting changed";
    CHECK(logfwd_stamp(l, strlen(l), &t, &b, &skip) == 2 && !t.known && b > 29935.511 && b < 29935.513 && skip == 15, "boot+: %f", b);
    l = "boot+12 x";
    CHECK(logfwd_stamp(l, strlen(l), &t, &b, &skip) == 2 && b == 12 && skip == 8, "boot+ without a fraction");
    l = "client connected (1, encrypted)";
    CHECK(logfwd_stamp(l, strlen(l), &t, &b, &skip) == 0 && skip == 0, "no stamp: an old line, or a program's");
    l = "2026-13-06 15:18:02Z x";
    CHECK(logfwd_stamp(l, strlen(l), &t, &b, &skip) == 0, "month 13 is no stamp");
    l = "2026-10-06 15:18:02.4Z";
    CHECK(logfwd_stamp(l, strlen(l), &t, &b, &skip) == 0, "a stamp with nothing after it is no stamp");

    level("settings: cannot write /x", LOGFWD_WARN);
    level("Error in hardware", LOGFWD_WARN);
    level("encryption: key change refused (plaintext)", LOGFWD_WARN);
    level("!! something", LOGFWD_WARN);
    level("== satellite start", LOGFWD_NOTICE);
    level("== update failed", LOGFWD_WARN);                     /* as the page: "bad" before "mark" */
    level("errors: none", LOGFWD_INFO);                         /* whole words only */
    level("cannotx", LOGFWD_INFO);
    level("not_failed", LOGFWD_INFO);
    level("client connected (1, encrypted)", LOGFWD_INFO);

    char o[2048]; size_t n;
    t.known = 1; t.sec = 1791299882; t.ms = 17;
    n = logfwd_format(o, sizeof o, 0, LOGFWD_WARN, &t, "echo-dot-3-5695c4", "settings: cannot", 16);
    CHECK(!strcmp(o, "<28>1 2026-10-06T15:18:02.017Z echo-dot-3-5695c4 hassmic - - - settings: cannot") && n == strlen(o), "5424: %s", o);
    t.known = 0;
    logfwd_format(o, sizeof o, 0, LOGFWD_INFO, &t, "echo", "boot+3.2 x", 10);
    CHECK(!strcmp(o, "<30>1 - echo hassmic - - - boot+3.2 x"), "5424 without a time: %s", o);
    t.known = 1; t.sec = 1791299882;
    logfwd_format(o, sizeof o, 1, LOGFWD_NOTICE, &t, "echo", "== start", 8);
    CHECK(!strcmp(o, "<29>Oct  6 15:18:02 echo hassmic: == start"), "3164: %s", o);
    /* cut where a UTF-8 sequence would be split: "ü" is 2 bytes; the head is 27 bytes */
    const char *u = "aüü";
    n = logfwd_format(o, 27 + 3 + 1, 0, LOGFWD_INFO, &(struct logfwd_time){ 0 }, "echo", u, strlen(u));
    CHECK(n == 27 + 3 && !strcmp(o + 27, "aü"), "a whole character fits: %zu \"%s\"", n, o + 27);
    n = logfwd_format(o, 27 + 4 + 1 - 1, 0, LOGFWD_INFO, &(struct logfwd_time){ 0 }, "echo", u, strlen(u));
    CHECK(n == 27 + 3 && !strcmp(o + 27, "aü"), "half a character does not go: %zu \"%s\"", n, o + 27);
    n = logfwd_format(o, 10, 0, LOGFWD_INFO, &(struct logfwd_time){ 0 }, "echo", u, strlen(u));
    CHECK(n == 0 && !o[0], "no room for the head: nothing");

    char tok[] = "pairing token SP:0ABCDEF234 for MA; SP:0 alone; xSP:0Z9";
    logfwd_redact(tok, strlen(tok));
    CHECK(!strcmp(tok, "pairing token SP:0********* for MA; SP:0 alone; xSP:0**"), "token blanked: %s", tok);

    if (!fails) printf("logfwd: all good\n");
    return fails != 0;
}
