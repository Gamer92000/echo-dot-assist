/*
 * Improv Wi-Fi over Bluetooth LE (https://www.improv-wifi.com/ble/): how a phone sets the Echo up without a network.
 * The Home Assistant app and Home Assistant's improv_ble integration (py-improv-ble-client) find it by its advertising,
 * connect, may scan for networks, send a network name and password, and get the settings page's address back once the
 * Echo is on that network; Home Assistant then finds it by mDNS like any ESPHome device.
 *
 * When it advertises (worker thread, every second):
 *   - not set up yet (core_oobe: after an install or a reset, until Home Assistant took the Echo on) and no network for
 *     OFFLINE_OOBE s: authorized from the start, nobody needs to press anything on a device that has nothing to lose;
 *   - set up, but no network for OFFLINE_LOST s (the router changed, the Echo moved): "authorization required", and
 *     the action button authorizes a phone for AUTH_SECS, as the spec suggests.  Anyone in Bluetooth range could
 *     otherwise move an Echo that works onto a network of theirs;
 *   - once on a network: "provisioned" for PROVISIONED_SECS more (Home Assistant drops its discovery), then not at all.
 * A phone that is connected keeps it going until it leaves.
 *
 * The network: wifi.c asks root (scripts/device/wifi.sh), as the settings page's switch does: the Echo goes onto the
 * network, and only once it has an address there and its router answers is it saved; else wpa_supplicant goes back to
 * what it had.  Only the PSK goes to root.  Improv itself sends the password in the clear over Bluetooth: that is the
 * protocol (ESPHome and the Voice PE do the same), and why an Echo that is set up wants the button first.
 * WPA2-PSK and open networks only (wifi.c), so the scan answer leaves out the networks it could not join.
 *
 * Names: the Echo's is made from the model and its MAC address (main.c name_make) and never set; what people call it is
 * given in Home Assistant when they add it.  So no host name or device name commands (capabilities without bit 3), the
 * device info tells the name.
 *
 * Characteristics, in ble.c's order (notifications go out lowest first: the state before the result): state, error,
 * RPC command, RPC result, capabilities.  An RPC packet: command, length, data, checksum (sum of the bytes before it);
 * data is strings, each a length and its bytes.  Writes may come in pieces (20 bytes each at the default MTU): they are
 * put together until the length says the packet is whole.
 */
#include "improv.h"
#include "ble.h"
#include "board.h"
#include "core.h"
#include "wifi.h"
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* HASSMIC_IMPROV_TIMES="<oobe> <lost> <auth> <provisioned>" (tests) */
static int OFFLINE_OOBE = 20;       /* s without a network before a new Echo advertises: one with a saved network gets on first */
static int OFFLINE_LOST = 600;      /* s without a network before an Echo that is set up advertises */
static int AUTH_SECS = 60;          /* the button's authorization (the spec's suggestion) */
static int PROVISIONED_SECS = 60;   /* "provisioned" advertised this long after it worked */
#define SCAN_SECS        25     /* root looks every 2 s and scans for 5 s */
#define JOIN_SECS       240     /* wifi.sh: 30 s to join, 30 s for an address, 45 s back; then it surely answered */

enum { ST_AUTH_REQUIRED = 1, ST_AUTHORIZED, ST_PROVISIONING, ST_PROVISIONED };
enum { E_NONE, E_INVALID_RPC, E_UNKNOWN_RPC, E_UNABLE_TO_CONNECT, E_NOT_AUTHORIZED, E_BAD_HOSTNAME, E_UNKNOWN = 0xff };
enum { CMD_WIFI = 1, CMD_IDENTIFY, CMD_INFO, CMD_SCAN };
enum { C_STATE, C_ERROR, C_RPC, C_RESULT, C_CAPS };
enum { CAPS = 0x07 };           /* identify, device info, Wi-Fi scan */
enum { M_OFF, M_OOBE, M_LOST };

/* 00467768-6228-2272-4663-27747826800x, least significant octet first */
#define IMPROV_UUID(x) { x, 0x80, 0x26, 0x78, 0x74, 0x27, 0x63, 0x46, 0x72, 0x22, 0x28, 0x62, 0x68, 0x77, 0x46, 0x00 }

static const struct improv_hooks *H;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
/* mu */
static int mode, st = ST_AUTHORIZED, err, connected;
static time_t auth_until, provisioned_at;
static unsigned char rx[3 + 256], result[3 + 256]; static size_t rxn, resn;   /* command being put together; last result */
static unsigned char cmd[3 + 256]; static size_t cmdn;                          /* a whole command for the worker */
static unsigned scan_id, join_id; static time_t scan_at, join_at;

static time_t mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec; }

/* An IPv4 address on an interface with a link (IFF_RUNNING: carrier), other than loopback.  The address alone says
 * nothing: dhcpcd runs with -K and keeps it on wlan0 when the network is gone (Dot 2 after a reset, 2026-10-07: no
 * network in wpa_supplicant, INACTIVE, carrier 0, flags 0x1003, the old address still there).  HASSMIC_FAKE_ONLINE
 * (tests): a file, online while it exists */
static int online(void)
{
    const char *f = getenv("HASSMIC_FAKE_ONLINE");
    if (f) return access(f, F_OK) == 0;
    struct ifaddrs *ifs, *i; int on = 0;
    if (getifaddrs(&ifs)) return 1;                     /* cannot tell: assume it is fine, never advertise for nothing */
    for (i = ifs; i && !on; i = i->ifa_next)
        on = i->ifa_addr && i->ifa_addr->sa_family == AF_INET && (i->ifa_flags & IFF_UP) && (i->ifa_flags & IFF_RUNNING) && !(i->ifa_flags & IFF_LOOPBACK);
    freeifaddrs(ifs);
    return on;
}

/* ---------------------------------------------------------------- packets */

static unsigned char sum(const unsigned char *p, size_t n) { unsigned s = 0; while (n--) s += *p++; return (unsigned char)s; }

/* the RPC result: command, then strings (NULL-terminated list of pointers, lengths in lens or strlen) */
static void set_result(int c, const char *const *s, const size_t *lens)
{
    size_t n = 2;
    for (int i = 0; s && s[i]; i++) {
        size_t l = lens ? lens[i] : strlen(s[i]);
        if (l > 255 || n + 1 + l > 2 + 255) break;
        result[n++] = (unsigned char)l; memcpy(result + n, s[i], l); n += l;
    }
    result[0] = (unsigned char)c; result[1] = (unsigned char)(n - 2);
    result[n] = sum(result, n); resn = n + 1;
    ble_server_notify(C_RESULT);
}
static void result1(int c, const char *s) { const char *l[] = { s, NULL }; set_result(c, l, NULL); }

static void set_state(int s) { if (s != st) { st = s; ble_server_notify(C_STATE); } }
static void set_error(int e) { if (e != err) { err = e; ble_server_notify(C_ERROR); } }

/* mu held.  What it advertises: flags, the service, its data (state, capabilities); the name in the scan response */
static void advertise(void)
{
    static const unsigned char head[] = { 2, 0x01, 0x06, 17, 0x07, 0x00, 0x80, 0x26, 0x78, 0x74, 0x27, 0x63, 0x46, 0x72, 0x22, 0x28, 0x62, 0x68, 0x77, 0x46, 0x00 };
    unsigned char a[31], r[31]; size_t n = sizeof head, k = strlen(core_name);
    if (mode == M_OFF) { ble_advertise(NULL, 0, NULL, 0); return; }
    memcpy(a, head, n);
    a[n++] = 9; a[n++] = 0x16; a[n++] = 0x77; a[n++] = 0x46; a[n++] = (unsigned char)st; a[n++] = CAPS;
    memset(a + n, 0, 4); n += 4;
    int cut = k > 29;
    if (cut) { k = 29; while (k && ((unsigned char)core_name[k] & 0xc0) == 0x80) k--; }  /* not inside a UTF-8 character */
    r[0] = (unsigned char)(k + 1); r[1] = cut ? 0x08 : 0x09; memcpy(r + 2, core_name, k);
    ble_advertise(a, n, r, k + 2);
}

/* ---------------------------------------------------------------- GATT (controller thread) */

static size_t on_read(int c, uint8_t *out, size_t cap)
{
    size_t n = 0;
    if (!cap) return 0;
    pthread_mutex_lock(&mu);
    switch (c) {
    case -1: n = strlen(core_name); if (n > cap) n = cap; memcpy(out, core_name, n); break;
    case C_STATE: out[0] = (uint8_t)st; n = 1; break;
    case C_ERROR: out[0] = (uint8_t)err; n = 1; break;
    case C_CAPS: out[0] = CAPS; n = 1; break;
    case C_RESULT: n = resn < cap ? resn : cap; memcpy(out, result, n); break;
    }
    pthread_mutex_unlock(&mu);
    return n;
}

static void on_write(int c, const uint8_t *d, size_t n)
{
    if (c != C_RPC) return;
    pthread_mutex_lock(&mu);
    if (rxn + n > sizeof rx) rxn = 0;                   /* longer than any packet: what came before was garbage */
    if (n <= sizeof rx) { memcpy(rx + rxn, d, n); rxn += n; }
    if (rxn >= 3 && rxn >= (size_t)rx[1] + 3) {
        size_t len = (size_t)rx[1] + 3;
        if (rxn > len || rx[len - 1] != sum(rx, len - 1)) set_error(E_INVALID_RPC);
        else if (cmdn) set_error(E_UNKNOWN);            /* the one before is still being handled */
        else { memcpy(cmd, rx, len); cmdn = len; pthread_cond_signal(&cv); }
        rxn = 0;
    }
    pthread_mutex_unlock(&mu);
}

static void on_connection(int up)
{
    pthread_mutex_lock(&mu);
    connected = up; rxn = 0;
    if (up) fprintf(stderr, "improv: a phone connected\n");
    else fprintf(stderr, "improv: the phone left\n");
    if (H && H->attention) H->attention(up && st == ST_AUTH_REQUIRED);
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mu);
}

static const struct ble_server server = {
    .uuid = IMPROV_UUID(0x00), .nchr = 5,
    .chr = { { IMPROV_UUID(0x01), BLE_PROP_READ | BLE_PROP_NOTIFY }, { IMPROV_UUID(0x02), BLE_PROP_READ | BLE_PROP_NOTIFY },
             { IMPROV_UUID(0x03), BLE_PROP_WRITE | BLE_PROP_WRITE_NR }, { IMPROV_UUID(0x04), BLE_PROP_READ | BLE_PROP_NOTIFY },
             { IMPROV_UUID(0x05), BLE_PROP_READ } },
    .read = on_read, .write = on_write, .connection = on_connection,
};

/* ---------------------------------------------------------------- commands (worker, mu held) */

/* the strings of a packet, at most max; -1 if they do not fill it exactly */
static int strings(const unsigned char *p, size_t n, const unsigned char **s, size_t *l, int max)
{
    size_t i = 2, end = n - 1; int k = 0;
    while (i < end) {
        if (k == max || i + 1 + p[i] > end) return -1;
        l[k] = p[i]; s[k++] = p + i + 1; i += 1 + p[i];
    }
    return i == end ? k : -1;
}

static int authorized(void) { return st == ST_AUTHORIZED || st == ST_PROVISIONED; }

static void command(const unsigned char *p, size_t n)
{
    const unsigned char *s[8]; size_t l[8]; int k = strings(p, n, s, l, 8);
    char e[200]; int busy;
    if (k < 0) { set_error(E_INVALID_RPC); return; }
    set_error(E_NONE);
    switch (p[0]) {
    case CMD_WIFI: {
        char pass[65];
        if (k != 2) { set_error(E_INVALID_RPC); return; }
        if (!authorized()) { set_error(E_NOT_AUTHORIZED); return; }
        if (join_id) { set_error(E_UNKNOWN); return; }  /* one switch at a time */
        if (l[1] >= sizeof pass) { set_error(E_UNABLE_TO_CONNECT); return; }
        memcpy(pass, s[1], l[1]); pass[l[1]] = 0;
        /* A name typed on a phone may end in a space its keyboard added after a word it completed (Dot 2, 2026-10-07:
         * "...NetzDerDinge " from the Home Assistant app, notfound).  Spaces at either end go unless a network of exactly
         * that name was seen in the last scan; a network named so on purpose is far rarer than that keyboard. */
        const unsigned char *sp = s[0]; size_t sl = l[0];
        while (sl && sp[sl - 1] == ' ') sl--;
        while (sl && sp[0] == ' ') { sp++; sl--; }
        if (sl && sl != l[0] && wifi_seen(s[0], l[0]) != 1) {
            fprintf(stderr, "improv: the network name came with spaces at its end, taken without (no network of that name seen)\n");
            s[0] = sp; l[0] = sl;
        }
        if (strlen(pass) != l[1] || wifi_join(s[0], l[0], pass, &join_id, &busy, e, sizeof e)) {
            if (strlen(pass) == l[1]) fprintf(stderr, "improv: no network switch: %s\n", e);
            memset(pass, 0, sizeof pass); join_id = 0; set_error(E_UNABLE_TO_CONNECT); return;
        }
        memset(pass, 0, sizeof pass);
        join_at = mono();
        set_state(ST_PROVISIONING);
        if (mode != M_OFF) advertise();
        break; }
    case CMD_IDENTIFY: core_identify(); break;
    case CMD_INFO: {
        const char *l4[] = { "hassmic", VERSION, board.model, core_name, NULL };
        set_result(CMD_INFO, l4, NULL);
        break; }
    case CMD_SCAN:
        if (scan_id) break;                             /* the answer to the one running answers this one too */
        if (wifi_scan(&scan_id, &busy, e, sizeof e)) { fprintf(stderr, "improv: no scan: %s\n", e); scan_id = 0; set_error(E_UNKNOWN); }
        else scan_at = mono();
        break;
    default: set_error(E_UNKNOWN_RPC);
    }
}

/* The scan's networks into one result (the client takes one): SSID, RSSI, security, as many as fit, strongest first */
static void scan_answer(const struct wifi_net *nets, int k)
{
    const char *s[3 * 32 + 1]; size_t l[3 * 32]; char rssi[32][8]; int m = 0, j = 0; size_t room = 255;
    for (int i = 0; i < k && j < 32; i++) {
        const char *sec = !strcmp(nets[i].sec, "open") ? "NO" : !strcmp(nets[i].sec, "psk") ? "WPA2" : NULL;
        if (!sec) continue;                             /* WPA3 only, enterprise, WEP: wifi.c cannot join them */
        snprintf(rssi[j], sizeof rssi[j], "%d", nets[i].signal);
        size_t need = 3 + nets[i].n + strlen(rssi[j]) + strlen(sec);
        if (need > room) break;
        room -= need;
        s[m] = (const char *)nets[i].ssid; l[m++] = nets[i].n;
        s[m] = rssi[j]; l[m++] = strlen(rssi[j]);
        s[m] = sec; l[m++] = strlen(sec);
        j++;
    }
    s[m] = NULL;
    set_result(CMD_SCAN, s, l);
}

/* ---------------------------------------------------------------- worker */

static void *worker(void *arg)
{
    (void)arg;
    time_t offline_since = mono();
    pthread_mutex_lock(&mu);
    for (;;) {
        struct timespec t; clock_gettime(CLOCK_REALTIME, &t); t.tv_sec += 1;
        if (!cmdn) pthread_cond_timedwait(&cv, &mu, &t);
        time_t now = mono();
        if (cmdn) { unsigned char c[sizeof cmd]; size_t n = cmdn; memcpy(c, cmd, n); command(c, n); cmdn = 0; }

        if (scan_id) {
            struct wifi_net nets[48]; int k = wifi_scan_result(scan_id, nets, 48);
            if (k >= 0) { scan_answer(nets, k); scan_id = 0; }
            else if (now - scan_at > SCAN_SECS) { fprintf(stderr, "improv: the scan did not answer\n"); scan_id = 0; set_error(E_UNKNOWN); }
        }
        if (join_id) {
            char ip[48] = "", why[32] = ""; int r = wifi_join_result(join_id, ip, sizeof ip, why, sizeof why);
            if (!r && now - join_at > JOIN_SECS) { r = -1; snprintf(why, sizeof why, "timeout"); }
            if (r > 0) {
                char url[80] = "";
                if (core_web_port) snprintf(url, sizeof url, "http://%s:%d/", ip, core_web_port);
                fprintf(stderr, "improv: on the network, address %s\n", ip);
                set_state(ST_PROVISIONED); provisioned_at = now;
                if (url[0]) result1(CMD_WIFI, url); else set_result(CMD_WIFI, NULL, NULL);
                join_id = 0;
            } else if (r < 0) {
                fprintf(stderr, "improv: the network switch failed (%s)\n", why);
                set_error(E_UNABLE_TO_CONNECT);
                set_state(mode == M_LOST && now >= auth_until ? ST_AUTH_REQUIRED : ST_AUTHORIZED);
                join_id = 0;
            }
        }
        if (st == ST_AUTHORIZED && mode == M_LOST && now >= auth_until) {
            set_state(ST_AUTH_REQUIRED);
            if (connected && H && H->attention) H->attention(1);
        }

        int on = online();
        if (on) offline_since = 0; else if (!offline_since) offline_since = now;
        int busy = connected || join_id || scan_id, want = mode;
        if (!busy && st == ST_PROVISIONED && now - provisioned_at >= PROVISIONED_SECS)
            set_state(mode == M_LOST && now >= auth_until ? ST_AUTH_REQUIRED : ST_AUTHORIZED);  /* and as below from now */
        if (!busy && st != ST_PROVISIONED) {
            if (!on && core_oobe() && now - offline_since >= OFFLINE_OOBE) want = M_OOBE;
            else if (!on && !core_oobe() && now - offline_since >= OFFLINE_LOST) want = M_LOST;
            else want = M_OFF;
        }
        if (want != mode) {
            mode = want;
            fprintf(stderr, "improv: %s\n", mode == M_OOBE ? "not set up and no network: advertising, open to any phone" :
                    mode == M_LOST ? "no network for a while: advertising, a phone needs the action button" : "advertising stopped");
            if (mode == M_OOBE) set_state(ST_AUTHORIZED);
            else if (mode == M_LOST) set_state(now < auth_until ? ST_AUTHORIZED : ST_AUTH_REQUIRED);
            else st = ST_AUTHORIZED;
            err = E_NONE;
            advertise();
        }
        static int last_st;
        if (mode != M_OFF && st != last_st) advertise();    /* the state is in the advertising too */
        last_st = st;

    }
    return NULL;
}

void improv_start(const struct improv_hooks *h)
{
    pthread_t t;
    H = h;
    if (!ble_present()) return;
    const char *times = getenv("HASSMIC_IMPROV_TIMES");
    if (times) sscanf(times, "%d %d %d %d", &OFFLINE_OOBE, &OFFLINE_LOST, &AUTH_SECS, &PROVISIONED_SECS);
    ble_serve(&server);
    if (!pthread_create(&t, NULL, worker, NULL)) pthread_detach(t);
}

int improv_authorize(void)
{
    pthread_mutex_lock(&mu);
    int take = mode == M_LOST && st == ST_AUTH_REQUIRED;
    if (take) {
        auth_until = mono() + AUTH_SECS;
        set_state(ST_AUTHORIZED);
        fprintf(stderr, "improv: authorized for %d s\n", AUTH_SECS);
        if (H && H->attention) H->attention(0);
        if (H && H->authorized) H->authorized();
        pthread_cond_signal(&cv);
    }
    pthread_mutex_unlock(&mu);
    return take;
}
