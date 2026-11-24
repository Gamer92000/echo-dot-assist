/* A stand-in music plugin for hassmic's own tests (tests/fake_plugins_music.py): what docs/plugins.md phase 4 gives a
 * plugin, used as a Lyrion-like player would, against the SDK's client.  It plays a tone (16 kHz mono, 440 Hz at
 * -12 dBFS) once it runs, as the newest music source; does what MUSIC_CMD says and says so (hm_music_state); keeps
 * Home Assistant's entities in step (the position, the volume, the assistant's state, a switch, a number, a select, a
 * button's event); answers on its UDP port whoever writes to it; searches the network on its action "search" and
 * proposes what answered for its server row.  Its stderr says what it did, line by line, for the test to read in
 * hassmic's log ("plugin testmusic: ...").  The action "try" with an argument names what it may not do, each tried
 * and its answer said:
 *   udp_public, udp_own, udp_5555, peer_public: to a public address, the Echo's own, port 5555 (refused: DENIED)
 *   propose_public, propose_row: a public address, a row its net.server does not name (DENIED)
 *   entity_bad, event_bad: a number out of its range, an event type it does not list (said in hassmic's log)
 *   open_twice: a second stream while one is open (BUSY); open_bad: a rate it does not take (BAD)
 *   volume_flood: twenty volumes at once (five a second: BUSY); udp_twice: its port opened again (BUSY)
 *   broadcast_flood: three searches at once (one a second: the rest refused)
 *   udp_local, propose_local: an address a "local" route gives the Echo itself (10.251.0.9: refused, DENIED)
 *   badutf: its text sensor's state in each form of text that is not UTF-8 (each refused: hassmic's log), then a
 *           good one; a title that is not UTF-8 (cut) */
#include "hassmic_plugin.h"
#include "plugmsg.h"
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define RATE 16000
static int music = -1, state = HM_MUSIC_STOPPED, udp = -1, udp_h = -1, disc = -1, disc_h = -1, opened, ticks, level = 5;
static uint32_t me;
static double phase;
static unsigned long long pos;

static void say_state(void) { hm_music_state(state, "A tone", "testmusic"); hm_log("music state %d", state); }

static void writable(void *ctx, int fd)
{
    static int16_t b[1600];
    (void)ctx;
    for (int k = 0; k < 8; k++) {
        for (int i = 0; i < 1600; i++) { b[i] = (int16_t)(8192 * sin(phase)); phase += 2 * M_PI * 440 / RATE; if (phase > 2 * M_PI) phase -= 2 * M_PI; }
        if (write(fd, b, sizeof b) < (long)sizeof b) return;            /* full: the loop asks again */
    }
}

static void open_music(void)
{
    int why = 0;
    music = hm_music_open(RATE, 1, &why);
    hm_log("music open: %s (%d)", music >= 0 ? "ok" : "refused", why);
    if (music >= 0) { state = HM_MUSIC_PLAYING; say_state(); }
}

static void on_cmd(void *ctx, int cmd, int why)
{
    (void)ctx;
    hm_log("music cmd %d why %d", cmd, why);
    if (cmd == HM_MCMD_PAUSE) { state = HM_MUSIC_PAUSED; say_state(); }
    else if (cmd == HM_MCMD_RESUME) { state = HM_MUSIC_PLAYING; say_state(); }
    else if (cmd == HM_MCMD_STOP) { state = HM_MUSIC_STOPPED; say_state(); hm_music_close(); music = -1; }
}

static void on_pos(void *ctx, uint64_t frames, uint64_t at, uint32_t queued)
{
    (void)ctx; (void)at;
    if (frames / RATE != pos / RATE) { char v[32]; snprintf(v, sizeof v, "%llu", (unsigned long long)(frames / RATE)); hm_entity_state("position", v); hm_log("position %s s, queued %u us", v, queued); }
    pos = frames;
}

static void on_volume(void *ctx, int v) { char t[8]; (void)ctx; snprintf(t, sizeof t, "%d", v); hm_entity_state("volume", t); hm_log("volume %d", v); }
static void on_state(void *ctx, int st, int muted, int v)
{
    static const char *const names[] = { "idle", "listening", "thinking", "speaking" };
    (void)ctx; hm_entity_state("assistant", names[st & 3]); hm_log("state %s muted %d volume %d", names[st & 3], muted, v);
}
static void on_net(void *ctx, uint32_t a, uint32_t m, int up) { (void)ctx; me = a; hm_log("net %u.%u.%u.%u/%u.%u.%u.%u up %d", a >> 24, a >> 16 & 255, a >> 8 & 255, a & 255, m >> 24, m >> 16 & 255, m >> 8 & 255, m & 255, up); }

static void on_udp(void *ctx, int fd)
{
    char b[512], out[600]; struct sockaddr_in a; socklen_t al = sizeof a; long n;
    (void)ctx;
    while ((n = recvfrom(fd, b, sizeof b - 1, MSG_DONTWAIT, (struct sockaddr *)&a, &al)) > 0) {
        uint32_t from = ntohl(a.sin_addr.s_addr); b[n] = 0;
        if (fd == disc) {
            char ip[16]; snprintf(ip, sizeof ip, "%u.%u.%u.%u", from >> 24, from >> 16 & 255, from >> 8 & 255, from & 255);
            hm_log("found %s: %s", ip, b);
            hm_log("propose %s: %d", ip, hm_propose("server", ip));
        } else {
            int k = snprintf(out, sizeof out, "echo:%s", b);
            hm_log("udp from %u.%u.%u.%u: %s", from >> 24, from >> 16 & 255, from >> 8 & 255, from & 255, b);
            hm_log("answer: %d", hm_udp_sendto(udp_h, from, ntohs(a.sin_port), out, (size_t)k));
        }
        al = sizeof a;
    }
}

static void on_set(void *ctx, const char *name, const char *value)
{
    (void)ctx;
    hm_log("set %s = %s", name, value);
    if (!strcmp(name, "ping")) hm_event("pressed", "ping");
    else if (!strcmp(name, "level")) { level = atoi(value); hm_entity_state("level", value); }
    else hm_entity_state(name, value);
}

static void try_(const char *what)
{
    int why = 0, r = 0;
    if (!strcmp(what, "udp_public")) r = hm_udp_peer(udp_h, 0xC6336407u, 4000);                       /* 198.51.100.7 */
    else if (!strcmp(what, "udp_own")) r = hm_udp_peer(udp_h, me, 4000);
    else if (!strcmp(what, "udp_5555")) r = hm_udp_peer(udp_h, 0x0AFA0002u, 5555);
    else if (!strcmp(what, "propose_public")) r = hm_propose("server", "198.51.100.7");
    else if (!strcmp(what, "propose_row")) r = hm_propose("mode", "10.250.0.2");
    else if (!strcmp(what, "entity_bad")) r = hm_entity_state("level", "500");
    else if (!strcmp(what, "event_bad")) r = hm_event("pressed", "nonsense");
    else if (!strcmp(what, "open_twice")) { int fd = hm_music_open(RATE, 1, &why); r = fd >= 0 ? 0 : -1; if (fd >= 0) close(fd); }
    else if (!strcmp(what, "open_bad")) { int fd = hm_music_open(12345, 1, &why); r = fd >= 0 ? 0 : -1; if (fd >= 0) close(fd); }
    else if (!strcmp(what, "volume_flood")) { int ok = 0; for (int i = 0; i < 20; i++) ok += !hm_volume_set(30 + i); r = ok; }
    else if (!strcmp(what, "udp_twice")) { int h, p, fd = hm_udp_open(HM_CAP_NET_UDP, 0, &h, &p, &why); r = fd >= 0 ? 0 : -1; if (fd >= 0) close(fd); }
    else if (!strcmp(what, "broadcast_flood")) { for (int i = 0; i < 3; i++) r += !hm_broadcast(disc_h, "x", 1); }
    else if (!strcmp(what, "udp_local")) r = hm_udp_peer(udp_h, 0x0AFB0009u, 4000);                    /* 10.251.0.9 */
    else if (!strcmp(what, "propose_local")) r = hm_propose("server", "10.251.0.9");
    else if (!strcmp(what, "badutf")) {
        static const char *const bad[] = { "x\xc0\x80y", "\xe0\x80\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xf0\x80\x80\x80", "a\x80", "\xe2\x82", "\xff" };
        for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) r += hm_entity_state("assistant", bad[i]) != 0;     /* (not waited for: hassmic's log says) */
        hm_music_state(state, "Tone \xed\xa0\x80 bad", "testmusic");
        hm_log("badutf sent %u", (unsigned)(sizeof bad / sizeof bad[0]));
        r = hm_entity_state("assistant", "fine \xe2\x9c\x93");
    }
    else if (!strcmp(what, "close")) { r = hm_music_close(); music = -1; state = HM_MUSIC_STOPPED; }
    else if (!strcmp(what, "reopen")) { open_music(); r = music >= 0 ? 0 : -1; }
    hm_log("try %s: %d (%d)", what, r, why);
}

static void on_action(void *ctx, const char *name, int from, const struct hm_arg *a, int n)
{
    (void)ctx;
    hm_log("action %s from %d (%d args%s%s%s%s)", name, from, n, n ? ": " : "", n ? a[0].name : "", n ? "=" : "", n ? a[0].value : "");
    if (!strcmp(name, "search")) hm_log("search: %d", hm_broadcast(disc_h, "testmusic?", 10));
    else if (!strcmp(name, "set_volume") && n) hm_log("volume set: %d", hm_volume_set(atoi(a[0].value)));
    else if (!strcmp(name, "try") && n) try_(a[0].value);
}

static void on_ready(void *ctx) { (void)ctx; hm_log("ready"); }

static void on_tick(void *ctx)
{
    int why = 0, port = 0;
    (void)ctx;
    ticks++;
    if (!opened) {
        opened = 1;
        udp = hm_udp_open(HM_CAP_NET_UDP, 0, &udp_h, &port, &why); hm_log("udp open: %d port %d (%d)", udp >= 0, port, why);
        if (udp >= 0) hm_watch(udp, on_udp);
        disc = hm_udp_open(HM_CAP_NET_DISCOVER, 0, &disc_h, &port, &why); hm_log("discover open: %d port %d (%d)", disc >= 0, port, why);
        if (disc >= 0) hm_watch(disc, on_udp);
        hm_entity_state("loud", "off"); hm_entity_state("mode", "tone");
        { char v[8]; snprintf(v, sizeof v, "%d", level); hm_entity_state("level", v); }
        open_music();
    }
}

int main(void)
{
    static const struct hm_callbacks cb = { .ready = on_ready, .tick = on_tick, .music_writable = writable, .music_pos = on_pos, .music_cmd = on_cmd,
                                            .volume = on_volume, .state = on_state, .net = on_net, .entity_set = on_set, .action = on_action };
    return hm_run(&cb, NULL) ? 1 : 0;
}
