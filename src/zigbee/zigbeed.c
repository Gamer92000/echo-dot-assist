/* zigbeed -r coordinator|router|enddevice -R <rcp socket> -d <folder> [-H <host socket>] [-m <channel mask>] [-e <ext PAN>]
 * A Zigbee PRO node on the Echo 4's radio (zb.h).  Root's main.sh runs it as the radio's user, a client of rcpbridge;
 * as the coordinator it serves the deCONZ protocol on -H for hassmic to forward to Home Assistant (radio.c).  One
 * thread, one poll loop: the radio, the host, timers. */
#define _GNU_SOURCE
#include "zb.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct zb zb;
extern int dz_want_connected;
void nwk_begin_join(void);
void nwk_resume_enddev(void);

static volatile sig_atomic_t stop;
static void on_term(int s) { (void)s; stop = 1; }

int64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }

void logf_(const char *fmt, ...)
{
    char b[400]; va_list a;
    va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    fprintf(stderr, "zigbee: %s\n", b);
}

uint32_t zb_random(void)
{
    static int fd = -1;
    uint32_t r = 0;
    if (fd < 0) fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0 || read(fd, &r, sizeof r) != sizeof r) r = (uint32_t)rand() ^ (uint32_t)now_ms();
    return r;
}

/* ---------------------------------------------------------------- timers */

#define TIMERS 96
static struct { int used; int64_t when; void (*fn)(void *); void *arg; } timers[TIMERS];

int timer_at(int64_t when, void (*fn)(void *), void *arg)
{
    int free_ = -1;
    for (int i = 0; i < TIMERS; i++) {
        if (timers[i].used && timers[i].fn == fn && timers[i].arg == arg) { timers[i].when = when; return 0; }
        if (!timers[i].used && free_ < 0) free_ = i;
    }
    if (free_ < 0) { static int64_t said; if (now_ms() - said > 10000) { said = now_ms(); LOG("timers: full"); } return -1; }
    timers[free_].used = 1; timers[free_].when = when; timers[free_].fn = fn; timers[free_].arg = arg;
    return 0;
}

void timer_cancel(void (*fn)(void *), void *arg)
{
    for (int i = 0; i < TIMERS; i++) if (timers[i].used && timers[i].fn == fn && timers[i].arg == arg) timers[i].used = 0;
}

static int64_t timers_run(void)
{
    int64_t now = now_ms(), next = now + 1000;
    for (int i = 0; i < TIMERS; i++) {
        if (!timers[i].used) continue;
        if (timers[i].when <= now) {
            void (*fn)(void *) = timers[i].fn; void *arg = timers[i].arg;
            timers[i].used = 0;
            fn(arg);
            now = now_ms();
        }
    }
    for (int i = 0; i < TIMERS; i++) if (timers[i].used && timers[i].when < next) next = timers[i].when;
    return next;
}

/* ---------------------------------------------------------------- state */

static int save_due;
void zb_save(void) { store_save(); save_due = 0; }
static void save_now(void *unused) { (void)unused; if (save_due) zb_save(); }
void zb_save_soon(void) { if (!save_due) { save_due = 1; timer_at(now_ms() + 2000, save_now, NULL); } }

/* the radio as the network has it */
static int radio_apply(void)
{
    int r = 0;
    r |= rcp_set_u8(SP_PHY_ENABLED, 1);
    r |= rcp_set_u8(SP_PHY_CHAN, (uint8_t)zb.channel);
    r |= rcp_set_u16(SP_MAC_PANID, zb.pan);
    r |= rcp_set_u16(SP_MAC_SADDR, zb.nwk);
    r |= rcp_set_eui(SP_MAC_LADDR, zb.ieee);
    r |= rcp_set_u8(SP_MAC_RAW_STREAM, 1);
    r |= rcp_set_u8(SP_SRC_MATCH_ENABLED, 1);
    rcp_src_match_clear();
    rcp_set_u8(SP_MAC_RX_ON_IDLE, 1);
    return r;
}

/* the coordinator's network: the host's parameters, or chosen here (a channel by energy, a PAN ID at random) */
int zb_form_network(void)
{
    if (!zb.ieee) zb.ieee = zb.hw_ieee;
    int zero = 1;
    for (int i = 0; i < 16; i++) zero &= !zb.nwk_key[i];
    if (zero) for (int i = 0; i < 16; i += 4) { uint32_t r = zb_random(); memcpy(zb.nwk_key + i, &r, 4); }
    if (!zb.predefined_pan || !zb.pan || zb.pan >= 0xFFFE) zb.pan = (uint16_t)(zb_random() % 0xFFFD + 1);
    if (!zb.epan || zb.epan == ~0ULL) zb.epan = zb.ieee;
    uint32_t mask = zb.channel_mask & 0x03FFF800;     /* 11..25: the RCP has no 26 */
    if (!mask) mask = 0x03FFF800;
    if (!(zb.channel >= 11 && zb.channel <= 25 && mask & 1u << zb.channel)) {
        int pick = -1;
        if (!(mask & (mask - 1))) { for (int c = 11; c <= 25; c++) if (mask & 1u << c) pick = c; }
        else {
            int8_t e[27];
            if (!rcp_energy_scan(mask, 100, e)) for (int c = 11; c <= 25; c++) if (mask & 1u << c && (pick < 0 || e[c] < e[pick])) pick = c;
            if (pick < 0) for (int c = 11; c <= 25; c++) if (mask & 1u << c) { pick = c; break; }
        }
        zb.channel = pick;
    }
    zb.nwk = 0x0000; zb.tc_ieee = zb.ieee; zb.depth = 0;
    if (radio_apply()) { LOG("form: the radio refused the settings"); return -1; }
    LOG("network up: channel %d, PAN %04x, extended PAN %016llx, coordinator %016llx", zb.channel, zb.pan, (unsigned long long)zb.epan, (unsigned long long)zb.ieee);
    nwk_start();
    zb_save();
    return 0;
}

void zb_stop_network(void)
{
    nwk_stop();
    LOG("network down (the host asked)");
    zb_save();
}

/* a router or end device leaves its network: forgotten, and it looks for one to join again */
static void begin_join_later(void *unused) { (void)unused; nwk_begin_join(); }
void zb_reset_network(int rejoin)
{
    (void)rejoin;
    nwk_stop();
    for (int i = 0; i < zb.nnodes; i++) if (zb.nodes[i].used) mac_drop_indirect(zb.nodes[i].ieee);
    memset(zb.nodes, 0, sizeof zb.nodes); zb.nnodes = 0;
    memset(zb.nwk_key, 0, 16); zb.have_prev_key = 0;
    gp_reset();
    aps_reset();
    /* any network again (the owner's choice of one stays: prefer_epan) */
    zb.joined = 0; zb.nwk = 0xFFFF; zb.pan = 0xFFFF; zb.epan = 0; zb.tc_ieee = 0; zb.parent = 0xFFFF;
    zb_save();
    timer_at(now_ms() + 2000, begin_join_later, NULL);
}

/* ---------------------------------------------------------------- the control socket (hassmic's radio.c)
 * One line a connection: "status" answers a JSON line (what the settings page shows), "leave" has a router or end
 * device leave its network and look for one again, "permit <seconds>" opens joining here (and, through the
 * coordinator, the network's), "rotate" has the coordinator give the network a new key, "prefer <extended PAN ID
 * in hex>|any" has a router join only that network (leaving the one it is in where that is another).  The socket
 * is the radio's group's, as the others. */
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

static int ctl_listen(const char *path)
{
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    unlink(path);
    mode_t m = umask(007);
    int r = s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 4);
    umask(m);
    if (r) { if (s >= 0) close(s); return -1; }
    return s;
}

static void ctl_serve(int ls)
{
    int c = accept4(ls, NULL, NULL, SOCK_CLOEXEC);
    if (c < 0) return;
    /* the line up to its end, within 500 ms in all: a client that writes it in pieces gets the answer to all of it */
    char line[64] = ""; size_t n = 0;
    for (int64_t until = now_ms() + 500; n + 1 < sizeof line && !memchr(line, '\n', n); ) {
        struct pollfd p = { c, POLLIN, 0 };
        int64_t left = until - now_ms();
        if (left <= 0 || poll(&p, 1, (int)left) != 1) break;
        ssize_t r = read(c, line + n, sizeof line - 1 - n);
        if (r <= 0) break;
        n += (size_t)r; line[n] = 0;
    }
    line[strcspn(line, "\r\n")] = 0;
    char o[2400]; int k = 0;
    if (!strcmp(line, "status")) {
        int children = 0, neighbors = 0, devices = 0;
        for (int i = 0; i < zb.nnodes; i++) {
            struct node *x = &zb.nodes[i];
            if (!x->used || !x->ieee) continue;
            devices++; children += x->child; neighbors += x->neighbor && now_ms() - x->seen < 60000;
        }
        int64_t pj = (zb.permit_until - now_ms()) / 1000;
        k = snprintf(o, sizeof o, "{\"role\":\"%s\",\"state\":\"%s\",\"ieee\":\"%016llx\",\"nwk\":%d,\"channel\":%d,"
                     "\"pan\":%d,\"epan\":\"%016llx\",\"devices\":%d,\"children\":%d,\"neighbors\":%d,\"permit\":%lld,\"host\":%s,"
                     "\"parent\":%d,\"key_confirmed\":%s,\"key_seq\":%u,\"rotating\":%s,\"prefer\":\"%016llx\",\"gp\":%d,\"gp_commissioning\":%s,\"networks\":[",
                     zb.role == R_COORD ? "coordinator" : zb.role == R_ROUTER ? "router" : "enddevice",
                     zb.state == NS_CONNECTED ? "connected" : zb.state == NS_JOINING ? "joining" : "offline",
                     (unsigned long long)zb.ieee, zb.state == NS_CONNECTED ? zb.nwk : -1, zb.state == NS_CONNECTED ? zb.channel : 0,
                     zb.state == NS_CONNECTED ? zb.pan : -1, (unsigned long long)(zb.state == NS_CONNECTED ? zb.epan : 0),
                     devices, children, neighbors, (long long)(pj > 0 ? pj : 0), dz_client >= 0 ? "true" : "false",
                     zb.role != R_COORD && zb.state == NS_CONNECTED ? zb.parent : -1,
                     zb.role != R_COORD && node_by_ieee(zb.tc_ieee) && node_by_ieee(zb.tc_ieee)->key_verified ? "true" : "false",
                     zb.key_seq, aps_rotating() ? "true" : "false", (unsigned long long)zb.prefer_epan, gp_entries(),
                     gp_commissioning() ? "true" : "false");
        /* the networks the last scans heard (a router's choice) */
        int first = 1;
        for (int i = 0; i < 16 && k > 0 && k < (int)sizeof o - 120; i++) {
            struct seen_net *e = &zb.nets[i];
            if (!e->at) continue;
            k += snprintf(o + k, sizeof o - (size_t)k, "%s{\"epan\":\"%016llx\",\"pan\":%u,\"channel\":%u,\"lqi\":%u,\"permit\":%s,\"room\":%s,\"age\":%lld}",
                          first ? "" : ",", (unsigned long long)e->epan, e->pan, e->channel, e->lqi, e->permit ? "true" : "false",
                          e->room ? "true" : "false", (long long)((now_ms() - e->at) / 1000));
            first = 0;
        }
        if (k > 0 && k < (int)sizeof o - 4) k += snprintf(o + k, sizeof o - (size_t)k, "]}\n");
    } else if (!strcmp(line, "leave") && zb.role != R_COORD) {
        LOG("leaving the network (asked on the settings page)");
        if (zb.state == NS_CONNECTED) nwk_leave(zb.nwk, zb.ieee, 0, 0);
        zb_reset_network(0);
        k = snprintf(o, sizeof o, "ok\n");
    } else if (!strncmp(line, "permit ", 7)) {
        int sec = atoi(line + 7);
        if (sec < 0) sec = 0;
        if (sec > 254) sec = 254;
        zb.permit_until = sec ? now_ms() + sec * 1000LL : 0;
        if (zb.permit_until > zb.net_permit_until || !sec) zb.net_permit_until = zb.permit_until;
        if (zb.state == NS_CONNECTED) {
            /* the rest of the network too: Mgmt_Permit_Joining_req to every router */
            uint8_t z[3] = { zb.zdo_seq++, (uint8_t)sec, 1 };
            aps_send(2, NWK_BCAST_ROUTERS, 0, 0, 0, 0x0000, 0x0036, z, 3, 0, 0, NULL, 0, NULL, NULL);
        }
        LOG("joining %s for %d s (the settings page)", sec ? "open" : "closed", sec);
        k = snprintf(o, sizeof o, "ok\n");
    } else if (!strcmp(line, "rotate")) {
        int r = aps_rotate_key();
        k = snprintf(o, sizeof o, r ? "error\n" : "ok\n");
    } else if (!strncmp(line, "prefer ", 7) && zb.role != R_COORD) {
        unsigned long long x = 0;
        if (!strcmp(line + 7, "any")) x = 0;
        else if (sscanf(line + 7, "%llx", &x) != 1 || !x) { k = snprintf(o, sizeof o, "error\n"); goto out; }
        zb.prefer_epan = x;
        LOG("the network to join: %s%.16s (the settings page)", x ? "" : "any open one", x ? line + 7 : "");
        if (x && zb.state == NS_CONNECTED && zb.epan != x) {
            LOG("leaving %016llx for it", (unsigned long long)zb.epan);
            nwk_leave(zb.nwk, zb.ieee, 0, 0);
            zb_reset_network(0);
        } else zb_save();
        k = snprintf(o, sizeof o, "ok\n");
    } else k = snprintf(o, sizeof o, "error\n");
out:
    if (k > 0 && write(c, o, (size_t)k) < 0) {}
    close(c);
}

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    const char *rcp = NULL, *host = NULL, *role = "coordinator", *ctl = NULL;
    unsigned long long epan = 0;
    zb.dir = ".";
    zb.channel_mask = 0x03FFF800;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "-r")) role = argv[i + 1];
        else if (!strcmp(argv[i], "-R")) rcp = argv[i + 1];
        else if (!strcmp(argv[i], "-H")) host = argv[i + 1];
        else if (!strcmp(argv[i], "-C")) ctl = argv[i + 1];
        else if (!strcmp(argv[i], "-d")) zb.dir = argv[i + 1];
        else if (!strcmp(argv[i], "-m")) zb.channel_mask = (uint32_t)strtoul(argv[i + 1], NULL, 0);
        else if (!strcmp(argv[i], "-e")) epan = strtoull(argv[i + 1], NULL, 16);
        else { fprintf(stderr, "usage: zigbeed -r coordinator|router|enddevice -R <rcp socket> -d <folder> [-H <host socket>] [-C <control socket>] [-m <mask>] [-e <ext PAN>]\n"); return 2; }
    }
    zb.role = !strcmp(role, "router") ? R_ROUTER : !strcmp(role, "enddevice") ? R_ENDDEV : R_COORD;
    if (!rcp) { fprintf(stderr, "zigbeed: -R <rcp socket>\n"); return 2; }
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa = { .sa_handler = on_term };
    sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL);
    memcpy(zb.tclk, "ZigBeeAlliance09", 16);
    zb.nwk = 0xFFFF; zb.pan = 0xFFFF; zb.parent = 0xFFFF; zb.mto_next = 0xFFFF; zb.channel = 11;
    zb.nwk_seq = (uint8_t)zb_random(); zb.aps_seq = (uint8_t)zb_random(); zb.mac_seq = (uint8_t)zb_random(); zb.zdo_seq = (uint8_t)zb_random();
    /* the route requests' ids too: a neighbour keeps (originator, id) for 10 s and answers no request it has seen, so
     * a restart counting from 1 again went unanswered (tests/fake_zigbee.py, "the router still answers", under load) */
    zb.rreq_id = (uint8_t)zb_random();
    int was = store_load();
    if (zb.role != R_COORD && epan) zb.epan = epan;

    for (int tries = 0; rcp_open(rcp); tries++) {
        if (tries == 50) { LOG("the radio's socket %s: not there", rcp); return 1; }
        usleep(200000);
    }
    if (rcp_init()) { LOG("the radio did not answer as an OpenThread RCP"); return 1; }
    if (!zb.ieee) zb.ieee = zb.hw_ieee;
    int ls = -1;
    if (zb.role == R_COORD && host && (ls = dz_listen(host)) < 0) { LOG("%s: cannot listen", host); return 1; }
    int cs = ctl ? ctl_listen(ctl) : -1;
    if (ctl && cs < 0) LOG("%s: cannot listen", ctl);
    LOG("%s %016llx", zb.role == R_COORD ? "coordinator" : zb.role == R_ROUTER ? "router" : "end device", (unsigned long long)zb.ieee);

    if (zb.role == R_COORD) {
        if (was > 0) zb_form_network();          /* the host had it running: it runs, host or not */
        else { radio_apply(); LOG("waiting for the host to start the network"); }
    } else if (zb.joined && zb.nwk < 0xFFF8 && zb.pan < 0xFFFF) {
        radio_apply();
        LOG("back on the network %016llx as %04x (channel %d)", (unsigned long long)zb.epan, zb.nwk, zb.channel);
        nwk_start();
        nwk_resume_enddev();
    } else {
        rcp_set_u8(SP_PHY_ENABLED, 1); rcp_set_u8(SP_MAC_RAW_STREAM, 1); rcp_set_eui(SP_MAC_LADDR, zb.ieee);
        rcp_set_u8(SP_SRC_MATCH_ENABLED, 1);
        nwk_begin_join();
    }

    while (!stop) {
        int64_t next = timers_run();
        rcp_dispatch();
        struct pollfd p[4] = { { zb.rcp, POLLIN, 0 }, { ls, POLLIN, 0 }, { dz_client, POLLIN, 0 }, { cs, POLLIN, 0 } };
        int64_t wait = next - now_ms();
        if (wait < 0) wait = 0;
        if (wait > 1000) wait = 1000;
        int r = poll(p, 4, (int)wait);
        if (r < 0 && errno != EINTR) break;
        if (r <= 0) continue;
        if (p[0].revents) rcp_input();
        if (ls >= 0 && p[1].revents) dz_accept(ls);
        if (dz_client >= 0 && p[2].revents) dz_input(dz_client);
        if (cs >= 0 && p[3].revents) ctl_serve(cs);
    }
    zb_save();
    LOG("stopped");
    return 0;
}
