/*
 * Switching Wi-Fi networks from the settings page.  wpa_supplicant's control socket is for root and the wifi group, and
 * hassmic faces the network, so as with adb over Wi-Fi (adbwifi.c) it only asks: state/wifi-request, one line, which
 * root's watcher (main.sh ota_watch, every 2 s) hands to scripts/device/wifi.sh:
 *   scan <id>                          scan, then answer with the networks seen
 *   join <id> <ssid hex> <psk hex|->   switch to that network (- = open), and back if the Echo does not get on it
 * The answers are in a directory only root writes (HASSMIC_WIFI, /data/local/hassmic/wifi), as files the daemon reads:
 *   status   what wpa_supplicant says now (wpa_state, id, ssid, ip_address, freq; after each job and when the link comes up)
 *   scan     "id <id>", then wpa_cli scan_results lines: bssid, frequency, signal, flags, ssid (wpa_supplicant's escapes)
 *   result   the last switch: "<id> switching <hex>", "<id> ok <hex> <ip> <saved 1|0>", "<id> failed <hex> <why> <back ssid>"
 *   lock/    exists while wifi.sh works: a scan, or a switch
 * The id ties an answer to its request: a random number, so that what an earlier run left is never taken for the answer.
 *
 * The password: the page sends it sealed with the browser's key (web.c unseal: the page is plain HTTP), and only the
 * PSK derived from it (PBKDF2-HMAC-SHA1 over the SSID, 4096 rounds, IEEE 802.11i) goes on to root and into
 * wpa_supplicant.conf.  The PSK opens the network as well as the password would, so the request file is 0600 and root
 * removes it as it reads it; but the password, which people use elsewhere too, never leaves this process.
 * WPA2-PSK and open networks only: WPA3-only (SAE), WEP and enterprise networks are shown, not offered.
 */
#include "wifi.h"
#include "hash.h"
#include "ws.h"
#include "../third_party/monocypher.h"
#include <ctype.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ANSWER_SECS 15          /* the watcher looks every 2 s; a request not taken after this long: nobody is there */
#define MAX_NETS    48

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static time_t asked_at;         /* monotonic: when the request now waiting was written */

static const char *state_dir(void) { const char *e = getenv("HASSMIC_STATE"); return e ? e : "/data/local/hassmic/state"; }
static const char *out_dir(void) { const char *e = getenv("HASSMIC_WIFI"); return e ? e : "/data/local/hassmic/wifi"; }
static time_t mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec; }
static void req_path(char *p, size_t cap) { snprintf(p, cap, "%s/wifi-request", state_dir()); }
static int out_exists(const char *name) { char p[320]; snprintf(p, sizeof p, "%s/%s", out_dir(), name); return access(p, F_OK) == 0; }

static FILE *out_open(const char *name)
{
    char p[320];
    snprintf(p, sizeof p, "%s/%s", out_dir(), name);
    return fopen(p, "r");
}

/* JSON string body (no quotes) of N bytes */
static size_t jesc(char *out, size_t cap, const uint8_t *s, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 7 < cap; i++) {
        if (s[i] == '"' || s[i] == '\\') { out[o++] = '\\'; out[o++] = (char)s[i]; }
        else if (s[i] < 0x20 || s[i] == 0x7f) o += (size_t)snprintf(out + o, cap - o, "\\u%04x", s[i]);
        else out[o++] = (char)s[i];
    }
    out[o] = 0;
    return o;
}
static void hexs(char *out, const uint8_t *b, size_t n) { for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]); out[2 * n] = 0; }
static int unhexs(uint8_t *out, size_t cap, const char *s)
{
    size_t n = strlen(s);
    if (n % 2 || n / 2 > cap) return -1;
    for (size_t i = 0; i < n / 2; i++) {
        unsigned v;
        if (!isxdigit((unsigned char)s[2 * i]) || !isxdigit((unsigned char)s[2 * i + 1]) || sscanf(s + 2 * i, "%2x", &v) != 1) return -1;
        out[i] = (uint8_t)v;
    }
    return (int)(n / 2);
}

/* An SSID as wpa_supplicant prints it (printf_encode: \\ \" \e \n \r \t \xHH), back to its bytes */
static size_t wpa_unescape(uint8_t *out, size_t cap, const char *s)
{
    size_t n = 0;
    while (*s && n < cap) {
        if (*s != '\\' || !s[1]) { out[n++] = (uint8_t)*s++; continue; }
        s++;
        switch (*s) {
        case 'n': out[n++] = '\n'; s++; break;
        case 'r': out[n++] = '\r'; s++; break;
        case 't': out[n++] = '\t'; s++; break;
        case 'e': out[n++] = 0x1b; s++; break;
        case 'x': {
            unsigned v;
            if (isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2]) && sscanf(s + 1, "%2x", &v) == 1) { out[n++] = (uint8_t)v; s += 3; }
            else out[n++] = (uint8_t)*s++;
            break;
        }
        default: out[n++] = (uint8_t)*s++;
        }
    }
    return n;
}

/* ---------------------------------------------------------------- the PSK */

static void hmac_sha1(const uint8_t *key, size_t kl, const uint8_t *m, size_t ml, uint8_t out[20])
{
    uint8_t k[64] = { 0 }, buf[64 + 64], ih[20];       /* m: at most the SSID and a block number, or a previous U */
    if (kl > 64) { sha1(key, kl, k); kl = 20; } else memcpy(k, key, kl);
    for (int i = 0; i < 64; i++) buf[i] = k[i] ^ 0x36;
    memcpy(buf + 64, m, ml); sha1(buf, 64 + ml, ih);
    for (int i = 0; i < 64; i++) buf[i] = k[i] ^ 0x5c;
    memcpy(buf + 64, ih, 20); sha1(buf, 84, out);
    crypto_wipe(k, sizeof k); crypto_wipe(buf, sizeof buf);
}

/* IEEE 802.11i: PSK = PBKDF2-HMAC-SHA1(passphrase, SSID, 4096, 32 bytes).  Some 16k SHA-1 blocks: tens of ms on the Echo */
static void wpa_psk(const char *pass, const uint8_t *ssid, size_t sl, uint8_t psk[32])
{
    uint8_t s[36], u[20], t[20];
    for (uint8_t blk = 1; blk <= 2; blk++) {
        memcpy(s, ssid, sl); s[sl] = 0; s[sl + 1] = 0; s[sl + 2] = 0; s[sl + 3] = blk;
        hmac_sha1((const uint8_t *)pass, strlen(pass), s, sl + 4, u); memcpy(t, u, 20);
        for (int i = 1; i < 4096; i++) { hmac_sha1((const uint8_t *)pass, strlen(pass), u, 20, u); for (int j = 0; j < 20; j++) t[j] ^= u[j]; }
        memcpy(psk + 20 * (blk - 1), t, blk == 1 ? 20 : 12);
    }
    crypto_wipe(u, sizeof u); crypto_wipe(t, sizeof t);
}

/* ---------------------------------------------------------------- requests */

struct result { unsigned id; char state[16], hex[72], ip[48], why[24], back[200]; int saved; };

static int read_result(struct result *r)
{
    char line[400]; FILE *f = out_open("result"); int k = 0;
    memset(r, 0, sizeof *r);
    if (!f) return -1;
    if (!fgets(line, sizeof line, f)) { fclose(f); return -1; }
    fclose(f);
    line[strcspn(line, "\r\n")] = 0;
    if (sscanf(line, "%u %15s %71s %n", &r->id, r->state, r->hex, &k) < 3) return -1;
    if (!strcmp(r->state, "ok")) sscanf(line + k, "%47s %d", r->ip, &r->saved);
    else if (!strcmp(r->state, "failed")) { int b = 0; sscanf(line + k, "%23s %n", r->why, &b); if (b) snprintf(r->back, sizeof r->back, "%s", line + k + b); }
    if (!strcmp(r->state, "switching") && !out_exists("lock")) snprintf(r->state, sizeof r->state, "interrupted");   /* that run died (a reboot) */
    return 0;
}

/* the request waiting for root, if any: its kind and id */
static int pending(char kind[8], unsigned *id)
{
    char p[300], line[64]; FILE *f;
    req_path(p, sizeof p);
    if (!(f = fopen(p, "r"))) return 0;
    int ok = fgets(line, sizeof line, f) && sscanf(line, "%7s %u", kind, id) == 2;     /* the first two words: not the PSK */
    fclose(f); crypto_wipe(line, sizeof line);
    return ok;
}

/* mu held.  0 if a new request may go now; else -1 with the reason */
static int may_ask(const char *kind, unsigned *id, int *busy, char *err, size_t errsz)
{
    char k[8]; unsigned pid; struct result r;
    *busy = 1;
    if (pending(k, &pid)) {
        if (!strcmp(kind, "scan") && !strcmp(k, "scan")) { *id = pid; return 1; }           /* the same scan, asked twice */
        snprintf(err, errsz, "The Echo has not taken the last request yet: try again in a few seconds"); return -1;
    }
    if (out_exists("lock") && !read_result(&r) && !strcmp(r.state, "switching")) {
        snprintf(err, errsz, "The Echo is switching networks right now"); return -1;
    }
    *busy = 0;
    return 0;
}

static int write_req(const char *line, char *err, size_t errsz)
{
    char p[300], tmp[310]; int fd, ok;
    req_path(p, sizeof p); snprintf(tmp, sizeof tmp, "%s.tmp", p);
    unlink(tmp);
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    ok = fd >= 0 && write(fd, line, strlen(line)) == (ssize_t)strlen(line);
    if (fd >= 0 && close(fd)) ok = 0;
    if (!ok || rename(tmp, p)) { unlink(tmp); snprintf(err, errsz, "cannot write the request for root"); fprintf(stderr, "wifi: cannot write %s\n", p); return -1; }
    asked_at = mono();
    return 0;
}

static unsigned new_id(void) { unsigned v; ws_random(&v, sizeof v); return (v & 0x7fffffff) | 1; }

int wifi_scan(unsigned *id, int *busy, char *err, size_t errsz)
{
    char line[40]; int r;
    pthread_mutex_lock(&mu);
    if ((r = may_ask("scan", id, busy, err, errsz))) { pthread_mutex_unlock(&mu); return r > 0 ? 0 : -1; }
    *id = new_id();
    snprintf(line, sizeof line, "scan %u\n", *id);
    r = write_req(line, err, errsz);
    pthread_mutex_unlock(&mu);
    return r;
}

int wifi_join(const uint8_t *ssid, size_t n, const char *pass, unsigned *id, int *busy, char *err, size_t errsz)
{
    char line[200], sh[65], ph[65], name[140]; uint8_t psk[32]; size_t pl = strlen(pass); int r;
    *busy = 0;
    if (!n || n > 32) { snprintf(err, errsz, "A network name has 1 to 32 bytes"); return -1; }
    if (pl == 64 && strspn(pass, "0123456789abcdefABCDEF") == 64) {                     /* the PSK itself */
        for (int i = 0; i < 64; i++) ph[i] = (char)tolower((unsigned char)pass[i]);
        ph[64] = 0;
    } else if (pl) {
        if (pl < 8 || pl > 63) { snprintf(err, errsz, "A Wi-Fi password has 8 to 63 characters"); return -1; }
        for (size_t i = 0; i < pl; i++) if ((unsigned char)pass[i] < 0x20 || (unsigned char)pass[i] > 0x7e) {
            snprintf(err, errsz, "A Wi-Fi password has only the characters of an English keyboard (ASCII)"); return -1; }
        wpa_psk(pass, ssid, n, psk); hexs(ph, psk, 32); crypto_wipe(psk, sizeof psk);
    } else snprintf(ph, sizeof ph, "-");
    hexs(sh, ssid, n);
    pthread_mutex_lock(&mu);
    if (may_ask("join", id, busy, err, errsz)) { pthread_mutex_unlock(&mu); crypto_wipe(ph, sizeof ph); return -1; }
    *id = new_id();
    snprintf(line, sizeof line, "join %u %s %s\n", *id, sh, ph);
    r = write_req(line, err, errsz);
    pthread_mutex_unlock(&mu);
    crypto_wipe(line, sizeof line); crypto_wipe(ph, sizeof ph);
    jesc(name, sizeof name, ssid, n);
    if (!r) fprintf(stderr, "wifi: asked root to switch to \"%s\" (%s)\n", name, pl ? "with a password" : "open");
    return r;
}

/* ---------------------------------------------------------------- answers */

static size_t current(char *o, size_t cap, int full)
{
    char line[300], st[32] = "", ip[48] = "", e[140]; uint8_t ssid[32]; size_t sl = 0; int have = 0;
    FILE *f = out_open("status");
    if (!f) return (size_t)snprintf(o, cap, "null");
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "wpa_state=", 10)) snprintf(st, sizeof st, "%.31s", line + 10);
        else if (!strncmp(line, "ssid=", 5)) { sl = wpa_unescape(ssid, sizeof ssid, line + 5); have = 1; }
        else if (!strncmp(line, "ip_address=", 11)) snprintf(ip, sizeof ip, "%.47s", line + 11);
    }
    fclose(f);
    if (!have || strcmp(st, "COMPLETED")) return (size_t)snprintf(o, cap, full ? "{\"state\":\"%s\"}" : "null", st);
    jesc(e, sizeof e, ssid, sl);
    if (!full) return (size_t)snprintf(o, cap, "{\"ssid\":\"%s\",\"ip\":\"%s\"}", e, ip);
    char h[65]; hexs(h, ssid, sl);
    return (size_t)snprintf(o, cap, "{\"ssid\":\"%s\",\"hex\":\"%s\",\"state\":\"%s\",\"ip\":\"%s\"}", e, h, st, ip);
}

size_t wifi_current_json(char *out, size_t cap) { size_t n = current(out, cap, 0); return n < cap ? n : cap - 1; }

struct net { uint8_t ssid[32]; size_t n; int signal, b24, b5; const char *sec; };

/* what the network asks of a client, from wpa_supplicant's flags: [WPA2-PSK-CCMP][ESS], [RSN-SAE-CCMP], [WEP], ... */
static const char *security(const char *fl)
{
    if (strstr(fl, "PSK")) return "psk";                /* WPA2 (or WPA2 and WPA3 side by side, "PSK+SAE") */
    if (strstr(fl, "SAE")) return "sae";                /* WPA3 only: wpa_supplicant here takes a PSK network */
    if (strstr(fl, "EAP")) return "eap";
    if (strstr(fl, "WEP")) return "wep";
    if (strstr(fl, "OWE")) return "owe";
    return "open";
}

static size_t scan_json(char *o, size_t cap)
{
    char line[400]; unsigned sid = 0; size_t k = 0, n; struct net *nets; FILE *f = out_open("scan");
    if (!f) return (size_t)snprintf(o, cap, "null");
    if (!(nets = calloc(MAX_NETS, sizeof *nets))) { fclose(f); return (size_t)snprintf(o, cap, "null"); }
    while (fgets(line, sizeof line, f)) {
        char *fld[5], *s = line; int i;
        line[strcspn(line, "\r\n")] = 0;
        if (sscanf(line, "id %u", &sid) == 1) continue;
        for (i = 0; i < 5 && s; i++) { fld[i] = s; s = i < 4 ? strchr(s, '\t') : NULL; if (s) *s++ = 0; }
        if (i < 5) continue;
        struct net x = { .signal = atoi(fld[2]), .sec = security(fld[3]) };
        int freq = atoi(fld[1]);
        x.n = wpa_unescape(x.ssid, sizeof x.ssid, fld[4]);
        if (!x.n || !x.ssid[0]) continue;                /* hidden: no name to show (typed by hand instead) */
        size_t j;
        for (j = 0; j < k && (nets[j].n != x.n || memcmp(nets[j].ssid, x.ssid, x.n)); j++) ;
        if (j == k) { if (k == MAX_NETS) continue; nets[k++] = x; }
        else if (x.signal > nets[j].signal) { nets[j].signal = x.signal; nets[j].sec = x.sec; }     /* the strongest access point speaks */
        if (freq >= 4900) nets[j].b5 = 1; else nets[j].b24 = 1;
    }
    fclose(f);
    for (size_t a = 1; a < k; a++) for (size_t b = a; b > 0 && nets[b].signal > nets[b - 1].signal; b--) { struct net t = nets[b]; nets[b] = nets[b - 1]; nets[b - 1] = t; }
    n = (size_t)snprintf(o, cap, "{\"id\":%u,\"networks\":[", sid);
    for (size_t j = 0; j < k && n + 300 < cap; j++) {
        char e[140], h[65];
        jesc(e, sizeof e, nets[j].ssid, nets[j].n); hexs(h, nets[j].ssid, nets[j].n);
        n += (size_t)snprintf(o + n, cap - n, "%s{\"ssid\":\"%s\",\"hex\":\"%s\",\"signal\":%d,\"band\":\"%s\",\"sec\":\"%s\"}", j ? "," : "", e, h,
                              nets[j].signal, nets[j].b24 && nets[j].b5 ? "both" : nets[j].b5 ? "5" : "2.4", nets[j].sec);
    }
    free(nets);
    return n + (size_t)snprintf(o + n, cap - n, "]}");
}

size_t wifi_status_json(char *o, size_t cap)
{
    size_t n = 0; char kind[8]; unsigned pid; struct result r;
    pthread_mutex_lock(&mu);
    int busy = out_exists("lock"), waiting = pending(kind, &pid), stale = waiting && !busy && mono() - asked_at > ANSWER_SECS;
    pthread_mutex_unlock(&mu);
    if (stale) fprintf(stderr, "wifi: root has not taken the request (is the hassmic_fw service running?)\n");
    n += (size_t)snprintf(o + n, cap - n, "{\"current\":");
    if (n < cap) n += current(o + n, cap - n, 1);
    if (n < cap) n += (size_t)snprintf(o + n, cap - n, ",\"busy\":%s,\"pending\":", busy ? "true" : "false");
    if (n < cap) n += waiting ? (size_t)snprintf(o + n, cap - n, "{\"kind\":\"%s\",\"id\":%u,\"stale\":%s}", kind, pid, stale ? "true" : "false")
                              : (size_t)snprintf(o + n, cap - n, "null");
    if (n < cap) n += (size_t)snprintf(o + n, cap - n, ",\"scan\":");
    if (n < cap) n += scan_json(o + n, cap - n);
    if (n < cap) n += (size_t)snprintf(o + n, cap - n, ",\"result\":");
    if (n < cap) {
        if (read_result(&r)) n += (size_t)snprintf(o + n, cap - n, "null");
        else {
            uint8_t b[32], bb[32]; char e[140], be[400]; int sl = unhexs(b, sizeof b, r.hex);
            jesc(e, sizeof e, b, sl > 0 ? (size_t)sl : 0);
            jesc(be, sizeof be, bb, wpa_unescape(bb, sizeof bb, r.back));
            n += (size_t)snprintf(o + n, cap - n, "{\"id\":%u,\"state\":\"%s\",\"ssid\":\"%s\",\"hex\":\"%s\",\"ip\":\"%s\",\"saved\":%s,\"reason\":\"%s\",\"back\":\"%s\"}",
                                  r.id, r.state, e, r.hex, r.ip, r.saved ? "true" : "false", r.why, be);
        }
    }
    if (n < cap) n += (size_t)snprintf(o + n, cap - n, "}");
    return n < cap ? n : cap - 1;
}
