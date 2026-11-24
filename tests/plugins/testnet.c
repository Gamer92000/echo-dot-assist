/* A stand-in forwarder for hassmic's own tests (tests/fake_plugins_net.py): what docs/plugins.md phase 2 gives a
 * plugin, used the way #193's syslog forwarder would, against the SDK's client.  It follows the log from the cursor it
 * kept in its storage (else from the oldest line), sends each line it gets to the server the owner entered (UDP,
 * entry 0; TCP, entry 1, with the setting `mode` tcp) and keeps the cursor again.  Its stderr says what it did, line
 * by line, for the test to read in hassmic's log ("plugin testnet: ...").
 * `mode` also makes it try what it may not, each answered and said:
 *   probe:  a socket of its own (the filter: EPERM), a CONNECT to another host than the one entered and one to a port
 *           its entry does not list (made by hand: the client has no such call), each refused;
 *   badcall: a socket of its own at once in ready (with the development flag: SIGSYS, and the client names the call);
 *   readyconn: a connect in ready, before READY (refused at once, HM_ERR_BAD: hassmic takes none there), then from tick;
 *   bigput: two values of 3000 bytes (4 kB accepted: the second refused), then twelve writes in a row (the Echo takes
 *           ten, the client keeps the latest of the rest and writes it later);
 *   reach:  (tests/fake_plugins_lan.py, as testlan): the setting `reach`, "<lan|internet> <entry> <host> <port>
 *           [keep]", is one hm_connect_lan or hm_connect_internet each time it changes, said ("reach ...: ok (0)" or
 *           "refused (why)"); a socket it got sends "hello <reach>" and is closed, or with "keep" is the one the log goes
 *           to (a line each).  No net.server connect in this mode. */
#include "hassmic_plugin.h"
#include "plugmsg.h"
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

static char mode[16] = "forward", cursor[64], reach[300], reached[300];
static int sock = -1, tried, probed, dirty, ticks;
static long sent, unsent;
/* Lines that came before its socket: the broker hands the log over from the follow on, which is in ready, and the
 * socket comes with the first tick; on a CI runner's four loaded cores the first LOG came before it, and a forwarder
 * that dropped them lost "what was written while it was off" (as #193's would have, a line the owner then never sees).
 * Kept until the socket is there (not in mode reach: its socket is the one a setting keeps) */
#define EARLY_MAX 1024
static char early[EARLY_MAX][1024]; static int nearly;

static void on_config(void *ctx, const struct hm_setting *s, int n)
{
    (void)ctx;
    for (int i = 0; i < n; i++) {
        if (!strcmp(s[i].name, "mode")) snprintf(mode, sizeof mode, "%s", s[i].value);
        if (!strcmp(s[i].name, "reach")) snprintf(reach, sizeof reach, "%s", s[i].value);
    }
}

static void on_ready(void *ctx)
{
    char c[64]; size_t n = 0; int r;
    (void)ctx;
    if (!strcmp(mode, "badcall")) { long k = syscall(SYS_socket, AF_INET, SOCK_DGRAM, 0); hm_log("own socket: %ld (%s)", k, k < 0 ? strerror(errno) : "made"); }
    if (!strcmp(mode, "readyconn")) { int why = 0, fd = hm_connect(0, 0, &why); hm_log("connect in ready: %d (%d)", fd, why); }
    r = hm_store_get("cursor", c, sizeof c - 1, &n);
    if (!r && n < sizeof c) { c[n] = 0; snprintf(cursor, sizeof cursor, "%s", c); }
    hm_log("kept cursor: %s", r ? (r == 1 ? "none" : "error") : cursor);
    r = hm_log_follow(!r ? cursor : "start");
    hm_log("follows the log: %s", r ? "refused" : "yes");
    if (!strcmp(mode, "bigput")) {
        static char big[3000]; int why = 0, k;
        memset(big, 'x', sizeof big);
        k = hm_store_put("big1", big, sizeof big, &why); hm_log("big1: %d (%d)", k, why);
        k = hm_store_put("big2", big, sizeof big, &why); hm_log("big2: %d (%d)", k, why);
        for (int i = 0; i < 12; i++) { char v[16]; snprintf(v, sizeof v, "%d", i); k = hm_store_put("count", v, strlen(v), &why); hm_log("count %d: %d", i, k); }
        k = hm_store_put("Bad Key", "x", 1, &why); hm_log("bad key: %d (%d)", k, why);
    }
}

/* a CONNECT the client would not make, sent by hand on the control socket: the broker's answer goes to the client's
 * loop, which drops answers nobody waits for; what the broker did is in hassmic's log */
static void connect_raw(int entry, const char *host, int port)
{
    uint8_t b[400]; struct pm_w w; struct pm_connect m = { PM_CAP_NET_SERVER, (uint8_t)entry, pm_s(host), (uint16_t)port };
    pm_winit(&w, b, sizeof b); pm_put_connect(&w, 0x7f000000u + (uint32_t)port, &m);
    if (send(3, b, (size_t)pm_wlen(&w), MSG_NOSIGNAL) < 0) hm_log("raw connect: %s", strerror(errno));
}

/* a line to the server.  A send that would block waits for room, up to a second: UDP's buffer is the sender's until the
 * receiving host's stack took the datagram, and on a loaded runner that took long enough for a burst of the log to
 * fill it (MSG_DONTWAIT alone dropped lines there) */
static void forward(const char *l)
{
    size_t k = strlen(l); int tcp = !strcmp(mode, "tcp") || !strcmp(mode, "reach");
    if (sock < 0) return;                           /* no server: nothing goes anywhere, as before */
    for (int tries = 0; ; tries++) {
        if (send(sock, l, k, MSG_DONTWAIT | MSG_NOSIGNAL) > 0) { sent++; break; }
        if ((errno != EAGAIN && errno != EWOULDBLOCK) || tries) { unsent++; break; }
        struct pollfd pf = { sock, POLLOUT, 0 };
        poll(&pf, 1, 1000);
    }
    if (tcp) send(sock, "\n", 1, MSG_DONTWAIT | MSG_NOSIGNAL);
}

static void on_tick(void *ctx)
{
    int why = 0;
    (void)ctx;
    ticks++;
    if (!strcmp(mode, "probe") && !probed) {
        long k = syscall(SYS_socket, AF_INET, SOCK_DGRAM, 0);
        hm_log("own socket: %ld (%s)", k, k < 0 ? strerror(errno) : "made");
        if (k >= 0) close((int)k);
        connect_raw(0, "elsewhere.example", 0);
        connect_raw(1, "", 22);
        probed = 1;
    }
    if (!strcmp(mode, "reach") && strcmp(reach, reached)) {
        char kind[16] = "", host[254] = "", keep[8] = ""; int entry = 0, port = 0, fd;
        snprintf(reached, sizeof reached, "%s", reach);
        if (sscanf(reach, "%15s %d %253s %d %7s", kind, &entry, host, &port, keep) >= 4) {
            fd = !strcmp(kind, "lan") ? hm_connect_lan(entry, host, port, &why) : hm_connect_internet(entry, host, port, &why);
            hm_log("reach %s: %s (%d)", reach, fd >= 0 ? "ok" : "refused", why);
            if (fd >= 0) {
                char hello[320]; int k = snprintf(hello, sizeof hello, "hello %s\n", reach);
                if (send(fd, hello, (size_t)k, MSG_NOSIGNAL) != k) hm_log("reach: hello not sent (%s)", strerror(errno));
                if (!strcmp(keep, "keep")) { if (sock >= 0) close(sock); sock = fd; } else close(fd);
            }
        }
    }
    if (strcmp(mode, "reach") && sock < 0 && tried < 3) {
        tried++;
        sock = hm_connect(!strcmp(mode, "tcp") ? 1 : 0, 0, &why);
        hm_log("connect: %s (%d)", sock >= 0 ? "ok" : "refused", why);
    }
    if (nearly && (sock >= 0 || tried >= 3 || !strcmp(mode, "reach"))) {         /* the lines kept for it, or given up */
        for (int i = 0; i < nearly; i++) forward(early[i]);
        nearly = 0;
    }
    /* said only when lines were lost, so a test that fails for a line that never came shows where it went */
    static long said;
    if (unsent != said) { hm_log("lines not sent: %ld", unsent); said = unsent; }
    /* the cursor at most every 2 s: the Echo's bucket takes ten writes, then one a minute (the test makes it faster) */
    if (dirty && ticks % 2 == 0) { int w; int r = hm_store_put("cursor", cursor, strlen(cursor), &w); dirty = 0; if (r < 0) hm_log("cursor not kept: %d", w); }
}

static void on_log(void *ctx, const char *cur, int gap, const char *const *lines, int n)
{
    (void)ctx;
    if (gap) hm_log("gap in the log");
    for (int i = 0; i < n; i++) {
        /* its own lines would come back to it: not sent on (a forwarder that sends what it says makes a loop) */
        if (strstr(lines[i], "plugin testnet: ") || strstr(lines[i], "plugin testlan: ")) continue;
        if (sock < 0 && strcmp(mode, "reach") && tried < 3) {
            if (nearly < EARLY_MAX) snprintf(early[nearly++], sizeof early[0], "%s", lines[i]); else unsent++;
            continue;
        }
        forward(lines[i]);
    }
    snprintf(cursor, sizeof cursor, "%s", cur); dirty = 1;
}

/* told to stop (hassmic ends, an update, the owner's switch): its cursor kept as it ends, as a forwarder does, and a
 * mark of the stop with it for the test to find (hassmic's SIGTERM: plugin.c plugin_shutdown) */
static void on_stop(void *ctx)
{
    char v[32]; int why = 0, r, k;
    (void)ctx;
    if (cursor[0]) hm_store_put("cursor", cursor, strlen(cursor), &why);
    k = snprintf(v, sizeof v, "after %d ticks", ticks);
    r = hm_store_put("stopped", v, (size_t)k, &why);
    hm_log("stop: kept (%d, %d)", r, why);
}

int main(void)
{
    static const struct hm_callbacks cb = { .config = on_config, .ready = on_ready, .log = on_log, .tick = on_tick, .stop = on_stop };
    return hm_run(&cb, NULL) ? 1 : 0;
}
