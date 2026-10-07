/*
 * The settings page, served by hassmic on its own port.  Plain HTTP: the Echo has no certificate a browser would take,
 * and a self-signed one means a warning on every Echo.  So nothing secret is ever sent (no API key, no Sendspin token,
 * no network keys), and what changes something is signed:
 *
 *   Login: the page makes an X25519 key pair once (kept in the browser) and asks POST /api/login with its public key.
 *   The ring shows that a login waits (hooks->attention); a short press of the action button approves it (main.c
 *   on_action asks web_approve() first).  One at a time: a second browser asking while one waits refuses both, so
 *   someone on the network cannot slip in beside the owner.  Approved keys are kept in state/web_clients.
 *   Requests: X-HM-Pub (the browser's key, hex), X-HM-Ctr (a number above the last one this browser used) and X-HM-Mac:
 *   keyed BLAKE2b-128 of "METHOD\nPATH\nCTR\nBODY" with K = BLAKE2b-256 keyed with X25519(echo key, browser key) over
 *   "hassmic web 1" + echo key + browser key.  A sniffer has neither secret key; a recorded request does not count
 *   twice (the counter is written to disk for every request that changes something).
 *   Answers: what answers a signed request carries X-HM-Mac as well, keyed BLAKE2b-128 of "RESP\nCTR\nBODY" with the
 *   same K, so the page knows an export or an artifact it carries from one Echo to another is what that Echo sent.
 *   Through another Echo: Echos in one arbitration network trust each other (they share its key K), so a browser
 *   approved on one is let in on the others without their buttons.  The target hands out a nonce for the browser's key
 *   (POST /api/vouch/nonce); the Echo the browser is approved on, asked with a signed request, MACs "hassmic voucher 1",
 *   the target's web key, the browser's key, the nonce and its own name with a key every member derives from K
 *   (arb_web_key); the target checks that with its own copy, uses the nonce up, and approves the browser.  Outside the
 *   network it is the button again.  adb still takes the button of that very Echo.
 *   adb over Wi-Fi (a root shell for the network) takes a press of its own: POST /api/adb "on" waits like a login.
 *   Artifacts (artifacts.c): listed, read and written in pieces over signed requests, so a page logged in to two Echos
 *   copies models from one to the other; root installs them.
 *   microWakeWord's models (mww_store.c): listed, added (uploaded whole, checked by loading them), renamed, tuned,
 *   deleted and picked over signed requests; they are hassmic's own, nothing waits for root.
 *   Amazon itself (davs.c): POST /api/davs/login starts a code pair login (the code on the page, GET /api/davs tells
 *   where it stands), /api/davs/fetch downloads an artifact; installing is the artifacts install.  The tokens of the
 *   registration never reach the page — plain HTTP.
 *   Wi-Fi (wifi.c): GET /api/wifi, POST /api/wifi/scan and /api/wifi/join, signed; root scans and switches.  The one
 *   secret that comes from the page, the network's password, travels sealed (unseal below), and only the PSK made from
 *   it goes on to root.
 *   The log: GET /api/log/0 (boot.log) and /1 (its rotated part), signed, for the page's viewer.  What it says is
 *   plain on the network then, like the rest of the page: names, addresses, when the Echo was spoken to.  Secrets do
 *   not go out: the one hassmic logs, the Sendspin pairing token (scripts/lib/setup.sh reads it there over adb), is
 *   blanked on the way.
 *   Limits: someone who can change traffic (not only read it) can change the page itself, as with any plain HTTP
 *   page; and the Echo's public key comes from GET /api/hello unsigned.
 *
 * The page may come from another Echo: answers allow any origin (CORS), as the signature, not a cookie, is what counts.
 */
#include "web.h"
#include "core.h"
#include "adbwifi.h"
#include "arb.h"
#include "artifacts.h"
#include "board.h"
#include "davs.h"
#include "diag.h"
#include "sound.h"
#include "hash.h"
#include "mww.h"
#include "netio.h"
#include "settings.h"
#include "wifi.h"
#include "ws.h"
#include "../third_party/monocypher.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

struct web_asset { const char *path, *type; const unsigned char *data; unsigned len; };
extern const struct web_asset web_assets[];            /* build/web_assets.c (tools/embed.py): gzip'd files of web/ */
extern const int web_nassets;

#define MAX_CONN    8
#define MAX_CLIENTS 32               /* a browser has a key per Echo page it opened, and each Echo may know all of them */
#define MAX_BODY    16384
#define LOGIN_MS    60000           /* a login waits this long for the button */

static const struct web_hooks *hooks;
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;  /* everything below; never held while taking core_lock */
static uint8_t sk[32], pk[32];
static struct client { uint8_t pub[32]; unsigned long long ctr; char label[64]; } clients[MAX_CLIENTS];
static int nclients;
/* what waits for the button: a login, or adb for an approved browser.  state: 0 none, 1 waits, 2 approved, 3 refused */
enum { W_LOGIN = 1, W_ADB };
static struct { uint8_t pub[32]; char label[64]; long long at; int state, kind; } login;
static atomic_int nconn;
#define NONCE_MS 120000
static struct { uint8_t pub[32], nonce[16]; long long at; } nonces[8];      /* handed out for a login through another Echo */
static unsigned nonce_next;

static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static const char *state_dir(void) { const char *e = getenv("HASSMIC_STATE"); return e ? e : "/data/local/hassmic/state"; }

static void hex(char *out, const uint8_t *b, size_t n) { for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]); }
static int unhex(uint8_t *out, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) { unsigned v; if (!isxdigit((unsigned char)s[2 * i]) || !isxdigit((unsigned char)s[2 * i + 1]) || sscanf(s + 2 * i, "%2x", &v) != 1) return -1; out[i] = (uint8_t)v; }
    return s[2 * n] ? -1 : 0;
}
/* hex of any even length up to CAP bytes: the byte count, or -1 */
static int unhex_any(uint8_t *out, size_t cap, const char *s)
{
    size_t n = strlen(s);
    return n % 2 || n / 2 > cap || unhex(out, s, n / 2) ? -1 : (int)(n / 2);
}

/* ---------------------------------------------------------------- keys and approved browsers (lk held) */

static int load_key(void)
{
    char p[300], t[64] = ""; FILE *f; int ok = 0;
    snprintf(p, sizeof p, "%s/web_key", state_dir());
    if ((f = fopen(p, "r"))) { ok = fgets(t, sizeof t, f) && b64_decode(t, strcspn(t, "\r\n"), sk, 32) == 32; fclose(f); }
    if (!ok) {
        int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        ws_random(sk, 32); b64_encode(sk, 32, t, 0, 1); strcat(t, "\n");
        if (fd < 0 || write(fd, t, strlen(t)) != (ssize_t)strlen(t)) { if (fd >= 0) close(fd); fprintf(stderr, "web: cannot write %s\n", p); return -1; }
        close(fd);
    }
    crypto_wipe(t, sizeof t);
    crypto_x25519_public_key(pk, sk);
    return 0;
}

static void save_clients(void)
{
    char p[300], tmp[310], h[65]; FILE *f; int fd;
    snprintf(p, sizeof p, "%s/web_clients", state_dir()); snprintf(tmp, sizeof tmp, "%s.tmp", p);
    if ((fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600)) < 0 || !(f = fdopen(fd, "w"))) { if (fd >= 0) close(fd); fprintf(stderr, "web: cannot write %s\n", tmp); return; }
    for (int i = 0; i < nclients; i++) { hex(h, clients[i].pub, 32); fprintf(f, "%s %llu %s\n", h, clients[i].ctr, clients[i].label); }
    if (fclose(f) || rename(tmp, p)) { unlink(tmp); fprintf(stderr, "web: cannot write %s\n", p); }
}

static void load_clients(void)
{
    char p[300], line[200], h[80]; unsigned long long c; int n; FILE *f;
    snprintf(p, sizeof p, "%s/web_clients", state_dir());
    if (!(f = fopen(p, "r"))) return;
    while (nclients < MAX_CLIENTS && fgets(line, sizeof line, f)) {
        struct client *k = &clients[nclients];
        line[strcspn(line, "\r\n")] = 0;
        if (sscanf(line, "%79s %llu %n", h, &c, &n) < 2 || unhex(k->pub, h, 32)) continue;
        k->ctr = c; snprintf(k->label, sizeof k->label, "%s", line + n); nclients++;
    }
    fclose(f);
}

static struct client *client_of(const uint8_t pub[32])
{
    for (int i = 0; i < nclients; i++) if (!memcmp(clients[i].pub, pub, 32)) return &clients[i];
    return NULL;
}

static int session_key(uint8_t k[32], const uint8_t pub[32])
{
    uint8_t shared[32], zero[32] = { 0 }, msg[13 + 64];
    crypto_x25519(shared, sk, pub);
    if (!memcmp(shared, zero, 32)) return -1;           /* a low-order point: no secret at all */
    memcpy(msg, "hassmic web 1", 13); memcpy(msg + 13, pk, 32); memcpy(msg + 45, pub, 32);
    crypto_blake2b_keyed(k, 32, shared, 32, msg, sizeof msg);
    crypto_wipe(shared, sizeof shared);
    return 0;
}

/* ---------------------------------------------------------------- login */

static void add_client(const uint8_t pub[32], const char *label)     /* lk held */
{
    struct client *k = client_of(pub);
    if (!k) {
        if (nclients == MAX_CLIENTS) { memmove(clients, clients + 1, sizeof clients[0] * (MAX_CLIENTS - 1)); nclients--; }   /* the oldest goes */
        k = &clients[nclients++]; memcpy(k->pub, pub, 32); k->ctr = 0;
    }
    snprintf(k->label, sizeof k->label, "%s", label);
    save_clients();
}

/* The voucher another Echo of our network gives for a browser: keyed with arb_web_key over what it binds */
static int voucher(uint8_t out[16], const uint8_t target[32], const uint8_t browser[32], const uint8_t nonce[16], const char *via)
{
    uint8_t k[32]; crypto_blake2b_ctx ctx;
    if (arb_web_key(k)) return -1;
    crypto_blake2b_keyed_init(&ctx, 16, k, 32);
    crypto_blake2b_update(&ctx, (const uint8_t *)"hassmic voucher 1", 17);
    crypto_blake2b_update(&ctx, target, 32); crypto_blake2b_update(&ctx, browser, 32); crypto_blake2b_update(&ctx, nonce, 16);
    crypto_blake2b_update(&ctx, (const uint8_t *)via, strlen(via));
    crypto_blake2b_final(&ctx, out); crypto_wipe(k, sizeof k);
    return 0;
}

static void login_expire(long long now)     /* lk held */
{
    if (login.state == 1 && now - login.at > LOGIN_MS) {
        login.state = 3; fprintf(stderr, "web: %s ran out without the button\n", login.kind == W_ADB ? "adb request" : "login");
        if (hooks->attention) hooks->attention(0);
        if (hooks->approved) hooks->approved(0);
    }
}

int web_approve(void)
{
    int r = 0;
    pthread_mutex_lock(&lk);
    login_expire(now_ms());
    if (login.state == 1 && login.kind == W_ADB) {
        login.state = 2; r = 2;
        fprintf(stderr, "web: adb over Wi-Fi approved with the button\n");
        if (hooks->attention) hooks->attention(0);
        if (hooks->approved) hooks->approved(1);
    } else if (login.state == 1) {
        add_client(login.pub, login.label);
        login.state = 2; r = 1;
        fprintf(stderr, "web: login approved (%s)\n", login.label);
        if (hooks->attention) hooks->attention(0);
        if (hooks->approved) hooks->approved(1);
    }
    pthread_mutex_unlock(&lk);
    if (r == 2) { adbwifi_ask(1); r = 1; }              /* root's firewall watcher opens it for 30 min */
    return r;
}

/* adb for an approved browser: "waiting" (for the button), "busy" (something else waits), or what it is now */
static const char *adb_ask(const uint8_t pub[32], int on)
{
    const char *r; long long now = now_ms();
    if (!on) { adbwifi_ask(0); return "closed"; }
    pthread_mutex_lock(&lk);
    login_expire(now);
    if (login.state == 1 && login.kind == W_ADB && !memcmp(login.pub, pub, 32)) r = "waiting";
    else if (login.state == 1) r = "busy";
    else {
        memcpy(login.pub, pub, 32); snprintf(login.label, sizeof login.label, "adb"); login.at = now; login.state = 1; login.kind = W_ADB;
        r = "waiting";
        fprintf(stderr, "web: adb over Wi-Fi waits for the action button\n");
        if (hooks->attention) hooks->attention(1);
    }
    pthread_mutex_unlock(&lk);
    return r;
}

/* "approved", "waiting", "refused" */
static const char *login_ask(const uint8_t pub[32], const char *label)
{
    const char *r; long long now = now_ms();
    pthread_mutex_lock(&lk);
    login_expire(now);
    if (client_of(pub)) r = "approved";
    else if (login.state && login.kind == W_LOGIN && !memcmp(login.pub, pub, 32)) r = login.state == 1 ? "waiting" : login.state == 2 ? "approved" : "refused";
    else if (login.state == 1) {                /* two at once: neither */
        login.state = 3; r = "refused";
        fprintf(stderr, "web: two logins at once, both refused\n");
        if (hooks->attention) hooks->attention(0);
        if (hooks->approved) hooks->approved(0);
    } else {
        memcpy(login.pub, pub, 32); snprintf(login.label, sizeof login.label, "%s", label); login.at = now; login.state = 1; login.kind = W_LOGIN;
        r = "waiting";
        fprintf(stderr, "web: login waits for the action button (%s)\n", label);
        if (hooks->attention) hooks->attention(1);
    }
    pthread_mutex_unlock(&lk);
    return r;
}

/* ---------------------------------------------------------------- HTTP */

struct req { char method[8], path[256], pub[80], ctr[24], mac[48], origin[8]; char *body; size_t blen; };

static int send_all(int fd, const void *p, size_t n)
{
    const char *c = p;
    while (n) { ssize_t w = send(fd, c, n, MSG_NOSIGNAL); if (w <= 0) return -1; c += w; n -= (size_t)w; }
    return 0;
}

static void respond(int fd, int code, const char *type, const char *extra, const void *body, size_t len)
{
    char h[1024];
    const char *msg = code == 200 ? "OK" : code == 204 ? "No Content" : code == 400 ? "Bad Request" : code == 401 ? "Unauthorized"
                    : code == 404 ? "Not Found" : code == 409 ? "Conflict" : code == 413 ? "Payload Too Large" : code == 503 ? "Service Unavailable" : "Error";
    int n = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\n"
                     "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Headers: X-HM-Pub, X-HM-Ctr, X-HM-Mac, Content-Type\r\n"
                     "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\nAccess-Control-Allow-Private-Network: true\r\n%s\r\n",
                     code, msg, type, len, extra ? extra : "");
    if (send_all(fd, h, (size_t)n) == 0 && len) send_all(fd, body, len);
}

static void respond_json(int fd, int code, const char *json) { respond(fd, code, "application/json", NULL, json, strlen(json)); }

/* The answer to a signed request (r passed signed_ok), signed in turn with the browser's K over its counter */
static void respond_s(int fd, const struct req *r, int code, const char *type, const char *extra, const void *body, size_t len)
{
    uint8_t pub[32], k[32], mac[16]; char x[600], h[33]; crypto_blake2b_ctx ctx; int bad;
    if (unhex(pub, r->pub, 32)) { respond(fd, code, type, extra, body, len); return; }
    pthread_mutex_lock(&lk); bad = session_key(k, pub); pthread_mutex_unlock(&lk);
    if (bad) { respond(fd, code, type, extra, body, len); return; }
    crypto_blake2b_keyed_init(&ctx, 16, k, 32);
    crypto_blake2b_update(&ctx, (const uint8_t *)"RESP\n", 5);
    crypto_blake2b_update(&ctx, (const uint8_t *)r->ctr, strlen(r->ctr)); crypto_blake2b_update(&ctx, (const uint8_t *)"\n", 1);
    if (len) crypto_blake2b_update(&ctx, body, len);
    crypto_blake2b_final(&ctx, mac); crypto_wipe(k, sizeof k);
    hex(h, mac, 16);
    snprintf(x, sizeof x, "X-HM-Mac: %s\r\nAccess-Control-Expose-Headers: X-HM-Mac\r\n%s", h, extra ? extra : "");
    respond(fd, code, type, x, body, len);
}
static void respond_sjson(int fd, const struct req *r, int code, const char *json) { respond_s(fd, r, code, "application/json", NULL, json, strlen(json)); }

/* JSON string body (no quotes) */
static size_t jesc(char *out, size_t cap, const char *s)
{
    size_t n = 0;
    for (; *s && n + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = (char)c; }
        else if (c < 0x20) n += (size_t)snprintf(out + n, cap - n, "\\u%04x", c);
        else out[n++] = (char)c;
    }
    out[n] = 0;
    return n;
}

static int read_request(int fd, struct req *r)
{
    char hdr[8192]; size_t n = 0; char *end = NULL, *line, *save; long cl = 0;
    while (n < sizeof hdr - 1 && !end) {
        ssize_t k = recv(fd, hdr + n, sizeof hdr - 1 - n, 0);
        if (k <= 0) return -1;
        n += (size_t)k; hdr[n] = 0;
        end = strstr(hdr, "\r\n\r\n");
    }
    if (!end) return -1;
    size_t hl = (size_t)(end - hdr) + 4;
    *end = 0;
    memset(r, 0, sizeof *r);
    if (sscanf(hdr, "%7s %255s", r->method, r->path) != 2) return -1;
    for (line = strtok_r(strchr(hdr, '\n') ? strchr(hdr, '\n') + 1 : hdr + strlen(hdr), "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        char *c = strchr(line, ':'), *v;
        if (!c) continue;
        *c = 0; v = c + 1; while (*v == ' ') v++;
        if (!strcasecmp(line, "Content-Length")) cl = strtol(v, NULL, 10);
        else if (!strcasecmp(line, "X-HM-Pub")) snprintf(r->pub, sizeof r->pub, "%s", v);
        else if (!strcasecmp(line, "X-HM-Ctr")) snprintf(r->ctr, sizeof r->ctr, "%s", v);
        else if (!strcasecmp(line, "X-HM-Mac")) snprintf(r->mac, sizeof r->mac, "%s", v);
    }
    /* a piece of an artifact and a microWakeWord model are the big bodies: everything else is a few lines */
    if (cl < 0 || cl > (!strncmp(r->path, "/api/artifact/chunk/", 20) ? ART_CHUNK_MAX : !strncmp(r->path, "/api/mww/add/", 13) ? MWW_MODEL_MAX + 16384 : MAX_BODY)) return -2;
    if (!(r->body = malloc((size_t)cl + 1))) return -1;
    size_t have = n - hl < (size_t)cl ? n - hl : (size_t)cl;
    memcpy(r->body, hdr + hl, have);
    while (have < (size_t)cl) { ssize_t k = recv(fd, r->body + have, (size_t)cl - have, 0); if (k <= 0) { free(r->body); r->body = NULL; return -1; } have += (size_t)k; }
    r->body[cl] = 0; r->blen = (size_t)cl;
    return 0;
}

/* 1 if the request is signed by an approved browser with a fresh counter (written to disk when persist) */
static int signed_ok(const struct req *r, int persist)
{
    uint8_t pub[32], mac[16], want[16], k[32]; crypto_blake2b_ctx ctx; char *end; struct client *c; int ok = 0;
    unsigned long long ctr = strtoull(r->ctr, &end, 10);
    if (unhex(pub, r->pub, 32) || unhex(mac, r->mac, 16) || !r->ctr[0] || *end) return 0;
    pthread_mutex_lock(&lk);
    if ((c = client_of(pub)) && ctr > c->ctr && !session_key(k, pub)) {
        crypto_blake2b_keyed_init(&ctx, 16, k, 32);
        crypto_blake2b_update(&ctx, (const uint8_t *)r->method, strlen(r->method)); crypto_blake2b_update(&ctx, (const uint8_t *)"\n", 1);
        crypto_blake2b_update(&ctx, (const uint8_t *)r->path, strlen(r->path)); crypto_blake2b_update(&ctx, (const uint8_t *)"\n", 1);
        crypto_blake2b_update(&ctx, (const uint8_t *)r->ctr, strlen(r->ctr)); crypto_blake2b_update(&ctx, (const uint8_t *)"\n", 1);
        crypto_blake2b_update(&ctx, (const uint8_t *)r->body, r->blen);
        crypto_blake2b_final(&ctx, want);
        if (!crypto_verify16(want, mac)) { ok = 1; c->ctr = ctr; if (persist) save_clients(); }
        crypto_wipe(k, sizeof k);
    }
    pthread_mutex_unlock(&lk);
    return ok;
}

/* ---------------------------------------------------------------- API */

/* members: the Echos of our network (for the login page's "through another Echo"; what beacons say anyway) */
static size_t hello_json(char *o, size_t cap, int members)
{
    char p[65], name[160], node[160]; size_t n;
    hex(p, pk, 32); jesc(name, sizeof name, core_name); jesc(node, sizeof node, core_node_name());
    n = (size_t)snprintf(o, cap, "{\"name\":\"%s\",\"node\":\"%s\",\"model\":\"%s\",\"codename\":\"%s\",\"version\":\"%s\",\"pub\":\"%s\"",
                         name, node, board.model, board.codename, VERSION, p);
    if (members && n < cap) { n += (size_t)snprintf(o + n, cap - n, ",\"members\":"); if (n < cap) n += arb_members_json(o + n, cap - n); }
    if (n < cap) n += (size_t)snprintf(o + n, cap - n, "}");
    return n < cap ? n : cap - 1;
}

/* lock order: core_lock first (settings), lk only inside the helpers above */
static void state_json(int fd, const struct req *r, const uint8_t me[32])
{
    size_t cap = 32768, n = 0; char *o = malloc(cap), v[300];
    if (!o) { respond_json(fd, 503, "{}"); return; }
    n += (size_t)snprintf(o + n, cap - n, "{\"device\":");
    n += hello_json(o + n, cap - n, 0);
    pthread_mutex_lock(&core_lock);
    n += (size_t)snprintf(o + n, cap - n, ",\"ha\":%s,\"settings\":[", core_ha_linked() ? "true" : "false");
    for (int i = 0, k = settings_count(); i < k && n < cap - 600; i++) {
        const struct setting *s = settings_at(i); const char *const *names; int nc = settings_choices(s, &names);
        n += (size_t)snprintf(o + n, cap - n, "%s{\"name\":\"%s\",\"label\":\"%s\",\"group\":\"%s\",\"type\":\"%s\",\"value\":%d,\"min\":%d,\"max\":%d,\"export\":%s,\"feature\":%s,\"unit\":\"%s\",\"choices\":[",
                              i ? "," : "", s->name, s->label, s->group, s->type == S_BOOL ? "bool" : s->type == S_INT ? "int" : "choice",
                              settings_get(s), s->min, s->max, s->export_ ? "true" : "false", s->feature ? "true" : "false", s->unit ? s->unit : "");
        for (int c = 0; c < nc && n < cap - 200; c++) { jesc(v, sizeof v, names[c]); n += (size_t)snprintf(o + n, cap - n, "%s\"%s\"", c ? "," : "", v); }
        n += (size_t)snprintf(o + n, cap - n, "]}");
    }
    /* what does not work, and why */
    n += (size_t)snprintf(o + n, cap - n, "],\"warnings\":[");
    int w = 0;
    if (!core_ha_linked()) n += (size_t)snprintf(o + n, cap - n, "%s\"Home Assistant is not connected\"", w++ ? "," : "");
    if (core_local_wake && core_whispered() == -2)
        n += (size_t)snprintf(o + n, cap - n, "%s\"Whisper detection: no model on this Echo (scripts/artifacts.sh installs it)\"", w++ ? "," : "");
    if (core_local_wake && sound_model() == SOUND_NONE)
        n += (size_t)snprintf(o + n, cap - n, "%s\"Sound detection: no model on this Echo (neither the firmware's nor one from scripts/artifacts.sh)\"", w++ ? "," : "");
    if (core_sound_failed())
        n += (size_t)snprintf(o + n, cap - n, "%s\"Sound detection could not start: its model did not load, so it was switched off (boot.log says why)\"", w++ ? "," : "");
    { const struct core_wake_word *l; if (core_local_wake && core_wake_engine(-1) == WAKE_AMAZON && core_wake_words(&l) <= 1)
        n += (size_t)snprintf(o + n, cap - n, "%s\"Wake words: only Alexa, Amazon's others are not installed (scripts/artifacts.sh)\"", w++ ? "," : ""); }
    pthread_mutex_unlock(&core_lock);
    float t = diag_soc_temp(), cpu = diag_cpu();
    pthread_mutex_lock(&lk); int adb_wait = login.state == 1 && login.kind == W_ADB; pthread_mutex_unlock(&lk);
    n += (size_t)snprintf(o + n, cap - n, "],\"adb\":{\"open\":%s,\"waiting\":%s},\"diag\":{\"soc_temp\":", adbwifi_open() ? "true" : "false", adb_wait ? "true" : "false");
    n += isnan(t) ? (size_t)snprintf(o + n, cap - n, "null") : (size_t)snprintf(o + n, cap - n, "%.1f", t);
    n += (size_t)snprintf(o + n, cap - n, ",\"cpu\":");
    n += isnan(cpu) ? (size_t)snprintf(o + n, cap - n, "null") : (size_t)snprintf(o + n, cap - n, "%.0f", cpu);
    { static const char *const models[] = { "none", "firmware", "newer" };
      n += (size_t)snprintf(o + n, cap - n, "},\"sound\":{\"model\":\"%s\",\"failed\":%s", models[sound_model()], core_sound_failed() ? "true" : "false"); }
    n += (size_t)snprintf(o + n, cap - n, "},\"arbitration\":");
    if (arb_running()) n += arb_status_json(o + n, cap - n); else n += (size_t)snprintf(o + n, cap - n, "null");
    n += (size_t)snprintf(o + n, cap - n, ",\"wifi\":"); n += wifi_current_json(o + n, cap - n);
    n += (size_t)snprintf(o + n, cap - n, ",\"clients\":[");
    pthread_mutex_lock(&lk);
    for (int i = 0; i < nclients && n < cap - 300; i++) {
        char h[65]; hex(h, clients[i].pub, 32); jesc(v, sizeof v, clients[i].label);
        n += (size_t)snprintf(o + n, cap - n, "%s{\"pub\":\"%s\",\"label\":\"%s\",\"me\":%s}", i ? "," : "", h, v, memcmp(clients[i].pub, me, 32) ? "false" : "true");
    }
    pthread_mutex_unlock(&lk);
    n += (size_t)snprintf(o + n, cap - n, "]}");
    respond_s(fd, r, 200, "application/json", NULL, o, n < cap ? n : cap);
    free(o);
}

/* /api/artifacts                          GET   the list (artifacts.c)
 * /api/artifact/read/<id>/<file>/<off>/<len>  GET   a piece
 * /api/artifact/begin                      POST  "<id> <digest>\n<file> <size>\n..."
 * /api/artifact/chunk/<id>/<file>/<off>    POST  a piece (the body, raw)
 * /api/artifact/commit/<id>                POST
 * /api/artifact/install                    POST  root installs what is ready; hassmic restarts
 * All signed; reads and pieces do not write the counter to disk (a replayed piece lands in a staged copy whose digest
 * then fails) */
/* a file name out of the path: "sub/file" travels as "sub%2Ffile" (the page's encodeURIComponent), so the one escape
 * is undone; artifacts.c checks the name */
static void unslash(char *s)
{
    char *w = s;
    for (; *s; s++) {
        if (s[0] == '%' && s[1] == '2' && (s[2] == 'F' || s[2] == 'f')) { *w++ = '/'; s += 2; }
        else *w++ = *s;
    }
    *w = 0;
}

static void artifact_api(int fd, struct req *r)
{
    char err[200], o[300], ej[260], id[80] = "", file[160] = ""; long a = 0, b = 0; int rc = -1, write = strcmp(r->method, "GET");
    int piece = !strncmp(r->path, "/api/artifact/read/", 19) || !strncmp(r->path, "/api/artifact/chunk/", 20);
    if (!signed_ok(r, write && !piece)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); return; }
    err[0] = 0;
    if (!write && !strcmp(r->path, "/api/artifacts")) {
        size_t cap = 65536; char *t = malloc(cap);
        if (!t) { respond_sjson(fd, r, 503, "{\"error\":\"no memory\"}"); return; }
        size_t n = art_list_json(t, cap);
        respond_s(fd, r, 200, "application/json", NULL, t, n); free(t); return;
    }
    if (!write && sscanf(r->path, "/api/artifact/read/%79[^/]/%159[^/]/%ld/%ld", id, file, &a, &b) == 4) {
        void *data; size_t n;
        unslash(file);
        if (!art_read(id, file, a, b, &data, &n, err, sizeof err)) { respond_s(fd, r, 200, "application/octet-stream", NULL, data, n); free(data); return; }
    }
    else if (write && !strcmp(r->path, "/api/artifact/begin")) rc = art_begin(r->body, err, sizeof err);
    else if (write && sscanf(r->path, "/api/artifact/chunk/%79[^/]/%159[^/]/%ld", id, file, &a) == 3) { unslash(file); rc = art_chunk(id, file, a, r->body, r->blen, err, sizeof err); }
    else if (write && sscanf(r->path, "/api/artifact/commit/%79[^/]", id) == 1) rc = art_commit(id, err, sizeof err);
    else if (write && !strcmp(r->path, "/api/artifact/install")) rc = art_install(err, sizeof err);
    else snprintf(err, sizeof err, "not found");
    if (!rc) { respond_sjson(fd, r, 200, "{\"ok\":true}"); return; }
    jesc(ej, sizeof ej, err); snprintf(o, sizeof o, "{\"error\":\"%s\"}", ej);
    respond_sjson(fd, r, 400, o);
}

/* /api/mww                              GET   {"engine","active","models":[...]}: microWakeWord's models (mww_store.c)
 * /api/mww/add/<id>                     POST  "<manifest length>\n<manifest JSON><model.tflite>": a new model, or a new
 *                                             version of <id> (length 0: no manifest, defaults)
 * /api/mww/edit/<id>                    POST  "name=..", "cutoff=0.85", "window=5" lines
 * /api/mww/delete/<id>                  POST
 * /api/mww/use/<id>                     POST  the active wake word (with microWakeWord on)
 * /api/mww/file/<id>/<manifest.json|model.tflite>  GET  the file, to save it
 * All signed. */
static void mww_api(int fd, struct req *r)
{
    char err[200] = "", o[300], ej[260], id[64] = "", file[24] = ""; int rc = -1, write = !strcmp(r->method, "POST");
    if (!signed_ok(r, write)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); return; }
    if (!write && !strcmp(r->path, "/api/mww")) {
        size_t cap = 16384, n = 0; char *t = malloc(cap);
        if (!t) { respond_sjson(fd, r, 503, "{\"error\":\"no memory\"}"); return; }
        pthread_mutex_lock(&core_lock);
        const struct core_wake_word *w; int k = core_wake_words(&w), mww = core_wake_engine(-1) == WAKE_MWW;
        int eng = core_wake_engine(-1);
        n += (size_t)snprintf(t + n, cap - n, "{\"engine\":\"%s\",\"active\":", eng == WAKE_HA ? "homeassistant" : mww ? "microwakeword" : "amazon");
        if (mww && k) { jesc(ej, sizeof ej, w[core_wake_word(-1)].id); n += (size_t)snprintf(t + n, cap - n, "\"%s\"", ej); }
        else n += (size_t)snprintf(t + n, cap - n, "null");
        pthread_mutex_unlock(&core_lock);
        n += (size_t)snprintf(t + n, cap - n, ",\"max\":%d,\"models\":", MWW_MODEL_MAX);
        n += mww_list_json(t + n, cap - n - 2);
        n += (size_t)snprintf(t + n, cap - n, "}");
        respond_s(fd, r, 200, "application/json", NULL, t, n); free(t); return;
    }
    if (!write && sscanf(r->path, "/api/mww/file/%63[^/]/%23s", id, file) == 2) {
        char p[400], *data; long len; FILE *f;
        if (!mww_good_id(id) || (strcmp(file, "manifest.json") && strcmp(file, "model.tflite"))) snprintf(err, sizeof err, "no such file");
        else {
            snprintf(p, sizeof p, "%s/%s/%s", mww_dir(), id, file);
            if (!(f = fopen(p, "rb"))) snprintf(err, sizeof err, "no such model");
            else {
                fseek(f, 0, SEEK_END); len = ftell(f); rewind(f);
                data = len >= 0 && len <= MWW_MODEL_MAX ? malloc((size_t)len + 1) : NULL;
                if (data && fread(data, 1, (size_t)len, f) == (size_t)len) {
                    fclose(f);
                    respond_s(fd, r, 200, file[0] == 'm' && file[1] == 'a' ? "application/json" : "application/octet-stream", NULL, data, (size_t)len);
                    free(data); return;
                }
                fclose(f); free(data); snprintf(err, sizeof err, "cannot read it");
            }
        }
    }
    else if (write && sscanf(r->path, "/api/mww/add/%63s", id) == 1) {
        char *nl = memchr(r->body, '\n', r->blen < 16 ? r->blen : 16), *end; long jl = nl ? strtol(r->body, &end, 10) : -1;
        if (!nl || end != nl || jl < 0 || (size_t)jl > r->blen - (size_t)(nl + 1 - r->body)) snprintf(err, sizeof err, "bad request");
        else {
            const char *j = nl + 1; size_t ml = r->blen - (size_t)(nl + 1 - r->body) - (size_t)jl;
            rc = mww_add(id, j, (size_t)jl, j + jl, ml, err, sizeof err);
        }
    }
    else if (write && sscanf(r->path, "/api/mww/edit/%63s", id) == 1) rc = mww_edit(id, r->body, err, sizeof err);
    else if (write && sscanf(r->path, "/api/mww/delete/%63s", id) == 1) rc = mww_delete(id, err, sizeof err);
    else if (write && sscanf(r->path, "/api/mww/use/%63s", id) == 1) {
        pthread_mutex_lock(&core_lock);
        const struct core_wake_word *w; int k = core_wake_words(&w), i;
        for (i = 0; i < k && strcmp(w[i].id, id); i++) ;
        if (core_wake_engine(-1) != WAKE_MWW) snprintf(err, sizeof err, "switch the wake word engine to microWakeWord first");
        else if (i == k) snprintf(err, sizeof err, "no such model");
        else { core_wake_word(i); core_entities_changed(); rc = 0; }  /* Home Assistant's select reads the pick on reconnect */
        pthread_mutex_unlock(&core_lock);
    }
    else snprintf(err, sizeof err, "not found");
    if (!rc) { respond_sjson(fd, r, 200, "{\"ok\":true}"); return; }
    jesc(ej, sizeof ej, err); snprintf(o, sizeof o, "{\"error\":\"%s\"}", ej);
    respond_sjson(fd, r, 400, o);
}

/* /api/davs                       GET   where the Amazon login stands, and the code while one waits (davs.c)
 * /api/davs/login                 POST  an Amazon site ("de", "co.uk", ...): start a login (the code shows in the status)
 * /api/davs/cancel                POST  stop a login that waits for its code
 * /api/davs/logout                POST  deregister from the account, forget the tokens
 * /api/davs/fetch                 POST  "echo de-DE" / "aed de-DE" / "whisper": download, check, stage; install as for
 *                                      a copy (POST /api/artifact/install).  All signed; tokens never in an answer. */
static void davs_api(int fd, struct req *r)
{
    char o[1024], ej[256], err[200] = "";             /* o: the status with its error, name and artifact at full length */
    int write = !strcmp(r->method, "POST"), rc = 0;
    if (!signed_ok(r, write)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); return; }
    if (!write && !strcmp(r->path, "/api/davs"))
    {
        size_t n = davs_status_json(o, sizeof o);
        respond_s(fd, r, 200, "application/json", NULL, o, n);
        return;
    }
    if (write && !strcmp(r->path, "/api/davs/login"))
    {
        char dom[8] = "";
        sscanf(r->body, "%7s", dom);
        rc = davs_login(dom, err, sizeof err);
    }
    else if (write && !strcmp(r->path, "/api/davs/logout")) rc = davs_logout(err, sizeof err);
    else if (write && !strcmp(r->path, "/api/davs/cancel")) rc = davs_cancel(err, sizeof err);
    else if (write && !strcmp(r->path, "/api/davs/fetch"))
    {
        char key[32] = "", loc[8] = "";
        if (sscanf(r->body, "%31s %7s", key, loc) == 2) rc = davs_fetch(key, loc, err, sizeof err);
        else { snprintf(err, sizeof err, "what shall this Echo download?"); rc = -1; }
    }
    else snprintf(err, sizeof err, "not found");
    if (!rc) { respond_sjson(fd, r, 200, "{\"ok\":true}"); return; }
    jesc(ej, sizeof ej, err); snprintf(o, sizeof o, "{\"error\":\"%s\"}", ej);
    respond_sjson(fd, r, 400, o);
}

/* /api/vouch/nonce   "<browser key>"                                    -> {"nonce"}       unsigned
 * /api/vouch/issue   "<target's web key> <browser key> <nonce>"         -> {"voucher","via"}  signed: by a browser approved here
 * /api/vouch/login   "<browser key> <nonce> <voucher> <via, hex> <label>" -> {"login"}       unsigned: the voucher is what counts */
static void vouch_api(int fd, struct req *r)
{
    char a[80] = "", b[80] = "", c[80] = "", d[200] = "", o[400], vh[33], nh[33]; int k = 0;
    uint8_t pub[32], tgt[32], nonce[16], v[16], want[16];
    if (!strcmp(r->path, "/api/vouch/nonce")) {
        if (sscanf(r->body, "%79s", a) != 1 || unhex(pub, a, 32)) { respond_json(fd, 400, "{\"error\":\"key\"}"); return; }
        pthread_mutex_lock(&lk);
        unsigned i = nonce_next++ % 8;
        memcpy(nonces[i].pub, pub, 32); ws_random(nonces[i].nonce, 16); nonces[i].at = now_ms(); hex(nh, nonces[i].nonce, 16);
        pthread_mutex_unlock(&lk);
        snprintf(o, sizeof o, "{\"nonce\":\"%s\"}", nh); respond_json(fd, 200, o);
    } else if (!strcmp(r->path, "/api/vouch/issue")) {
        char via[160];
        if (!signed_ok(r, 1)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); return; }
        if (sscanf(r->body, "%79s %79s %79s", a, b, c) != 3 || unhex(tgt, a, 32) || unhex(pub, b, 32) || unhex(nonce, c, 16)) {
            respond_sjson(fd, r, 400, "{\"error\":\"bad request\"}"); return;
        }
        snprintf(via, sizeof via, "%s", core_name);
        if (voucher(v, tgt, pub, nonce, via)) { respond_sjson(fd, r, 400, "{\"error\":\"this Echo is in no network\"}"); return; }
        char vj[200]; jesc(vj, sizeof vj, via); hex(vh, v, 16);
        snprintf(o, sizeof o, "{\"voucher\":\"%s\",\"via\":\"%s\"}", vh, vj);
        fprintf(stderr, "web: vouched for a browser on another Echo\n");
        respond_sjson(fd, r, 200, o);
    } else if (!strcmp(r->path, "/api/vouch/login")) {
        char via[100] = "", label[64] = "browser"; uint8_t vb[100]; size_t vl;
        if (sscanf(r->body, "%79s %79s %79s %199s %n", a, b, c, d, &k) < 4 || unhex(pub, a, 32) || unhex(nonce, b, 16) || unhex(v, c, 16)
            || (vl = strlen(d) / 2) >= sizeof via || unhex(vb, d, vl)) { respond_json(fd, 400, "{\"error\":\"bad request\"}"); return; }
        memcpy(via, vb, vl); via[vl] = 0;
        if (k) { snprintf(label, sizeof label, "%s", r->body + k); for (char *x = label; *x; x++) if ((unsigned char)*x < 0x20) *x = ' '; }
        int ok = 0; long long now = now_ms();
        pthread_mutex_lock(&lk);
        for (int i = 0; i < 8; i++) {
            if (!nonces[i].at || now - nonces[i].at > NONCE_MS || memcmp(nonces[i].pub, pub, 32) || crypto_verify16(nonces[i].nonce, nonce)) continue;
            nonces[i].at = 0;                                   /* once */
            ok = !voucher(want, pk, pub, nonce, via) && !crypto_verify16(want, v);
        }
        if (ok) { char l[64]; snprintf(l, sizeof l, "%.30s via %.28s", label, via); add_client(pub, l); }
        pthread_mutex_unlock(&lk);
        fprintf(stderr, "web: login through %s %s\n", via, ok ? "approved" : "refused");
        respond_json(fd, 200, ok ? "{\"login\":\"approved\"}" : "{\"login\":\"refused\"}");
    } else respond_json(fd, 404, "{\"error\":\"not found\"}");
}

/* ---------------------------------------------------------------- Wi-Fi */

/* What the page seals for this Echo alone (a Wi-Fi password: the page is plain HTTP).  XOR with a key stream: block i
 * (64 bytes) = BLAKE2b-512 keyed with EK over the request's counter, "\n" and the byte i, EK = BLAKE2b-256 keyed with K
 * over "hassmic seal 1".  The counter is new with every request of a browser (signed_ok refuses an old one), so no
 * stream serves twice; the request's MAC covers the sealed bytes.  web/crypto.js seal does the other half. */
static int unseal(const struct req *r, const uint8_t *in, size_t n, uint8_t *out)
{
    uint8_t pub[32], k[32], ek[32], ks[64]; crypto_blake2b_ctx ctx; int bad;
    if (n > 64 * 4 || unhex(pub, r->pub, 32)) return -1;
    pthread_mutex_lock(&lk); bad = session_key(k, pub); pthread_mutex_unlock(&lk);
    if (bad) return -1;
    crypto_blake2b_keyed(ek, 32, k, 32, (const uint8_t *)"hassmic seal 1", 14);
    for (size_t i = 0; i * 64 < n; i++) {
        uint8_t blk = (uint8_t)i;
        crypto_blake2b_keyed_init(&ctx, 64, ek, 32);
        crypto_blake2b_update(&ctx, (const uint8_t *)r->ctr, strlen(r->ctr)); crypto_blake2b_update(&ctx, (const uint8_t *)"\n", 1);
        crypto_blake2b_update(&ctx, &blk, 1);
        crypto_blake2b_final(&ctx, ks);
        for (size_t j = 0; j < 64 && i * 64 + j < n; j++) out[i * 64 + j] = in[i * 64 + j] ^ ks[j];
    }
    crypto_wipe(k, sizeof k); crypto_wipe(ek, sizeof ek); crypto_wipe(ks, sizeof ks);
    return 0;
}

/* /api/wifi        GET   the network now, the last scan, the last switch (wifi.c)
 * /api/wifi/scan   POST  root scans                                              -> {"id"}
 * /api/wifi/join   POST  "<ssid hex> <password sealed, hex | ->": root switches,  -> {"id"}
 *                        and goes back to the network it was on if the Echo does not get on the new one
 * All signed; 409 while root is busy switching or a request waits. */
static void wifi_api(int fd, struct req *r)
{
    char err[200], ej[260], o[300]; unsigned id = 0; int write = !strcmp(r->method, "POST"), busy = 0, rc = -1;
    if (!signed_ok(r, write)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); return; }
    err[0] = 0;
    if (!write && !strcmp(r->path, "/api/wifi")) {
        size_t cap = 16384; char *t = malloc(cap);
        if (!t) { respond_sjson(fd, r, 503, "{\"error\":\"no memory\"}"); return; }
        size_t n = wifi_status_json(t, cap);
        respond_s(fd, r, 200, "application/json", NULL, t, n); free(t); return;
    }
    if (write && !strcmp(r->path, "/api/wifi/scan")) rc = wifi_scan(&id, &busy, err, sizeof err);
    else if (write && !strcmp(r->path, "/api/wifi/join")) {
        char sh[80] = "", ph[600] = ""; uint8_t ssid[40], sealed[256], pass[257]; int sl, pl = 0;
        if (sscanf(r->body, "%79s %599s", sh, ph) != 2 || (sl = unhex_any(ssid, sizeof ssid, sh)) < 0
            || (strcmp(ph, "-") && ((pl = unhex_any(sealed, sizeof sealed, ph)) < 0 || unseal(r, sealed, (size_t)pl, pass))))
            snprintf(err, sizeof err, "bad request");
        else {
            if (!strcmp(ph, "-")) pl = 0;
            pass[pl] = 0;                               /* the page pads with zeros: a password has none */
            rc = wifi_join(ssid, (size_t)sl, (const char *)pass, &id, &busy, err, sizeof err);
        }
        crypto_wipe(pass, sizeof pass); crypto_wipe(sealed, sizeof sealed);
    }
    else snprintf(err, sizeof err, "not found");
    if (!rc) { snprintf(o, sizeof o, "{\"id\":%u}", id); respond_sjson(fd, r, 200, o); return; }
    jesc(ej, sizeof ej, err); snprintf(o, sizeof o, "{\"error\":\"%s\"}", ej);
    respond_sjson(fd, r, busy ? 409 : 400, o);
}

/* ---------------------------------------------------------------- the log */

#define LOG_SEND_MAX (2 << 20)      /* main.sh rotates at 1 MB, checked every 10 s: the tail of this much is plenty */

static const char *log_path(void) { const char *e = getenv("HASSMIC_LOG"); return e ? e : "/data/local/hassmic/boot.log"; }

/* The Sendspin pairing token ("SP:0" and base32, sendspin_pairing_token) blanked in place: it lets a server pair */
static void log_redact(char *t, size_t n)
{
    for (size_t i = 0; i + 4 <= n; i++) {
        if (memcmp(t + i, "SP:0", 4)) continue;
        size_t j = i + 4;
        while (j < n && (isupper((unsigned char)t[j]) || isdigit((unsigned char)t[j]))) t[j++] = '*';
        i = j;
    }
}

static void log_api(int fd, struct req *r)
{
    char path[300]; int part; FILE *f; long size, from; char *t; size_t n;
    if (!signed_ok(r, 0)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); return; }
    if (sscanf(r->path, "/api/log/%d", &part) != 1 || part < 0 || part > 1) { respond_sjson(fd, r, 404, "{\"error\":\"not found\"}"); return; }
    snprintf(path, sizeof path, part ? "%s.1" : "%s", log_path());
    if (!(f = fopen(path, "r"))) { respond_s(fd, r, 200, "text/plain; charset=utf-8", NULL, "", 0); return; }    /* no rotated part yet */
    fseek(f, 0, SEEK_END); size = ftell(f);
    from = size > LOG_SEND_MAX ? size - LOG_SEND_MAX : 0;
    if (size < 0 || !(t = malloc((size_t)(size - from) + 1))) { fclose(f); respond_sjson(fd, r, 503, "{\"error\":\"no memory\"}"); return; }
    fseek(f, from, SEEK_SET);
    n = fread(t, 1, (size_t)(size - from), f);         /* others append meanwhile: what was there when we looked */
    fclose(f);
    size_t skip = 0;
    if (from) { char *nl = memchr(t, '\n', n); skip = nl ? (size_t)(nl - t) + 1 : 0; }       /* whole lines only */
    log_redact(t + skip, n - skip);
    respond_s(fd, r, 200, "text/plain; charset=utf-8", NULL, t + skip, n - skip);
    free(t);
}

static void handle(int fd)
{
    struct req r; int e; uint8_t pub[32];
    struct timeval tv = { 10, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if ((e = read_request(fd, &r))) { if (e == -2) respond_json(fd, 413, "{\"error\":\"too large\"}"); return; }
    char *q = strchr(r.path, '?'); if (q) *q = 0;       /* no query strings in what we serve */

    if (!strcmp(r.method, "OPTIONS")) respond(fd, 204, "text/plain", NULL, NULL, 0);
    else if (!strcmp(r.method, "GET") && strncmp(r.path, "/api/", 5)) {
        int found = 0;
        for (int i = 0; i < web_nassets; i++) if (!strcmp(web_assets[i].path, r.path)) {
            respond(fd, 200, web_assets[i].type, "Content-Encoding: gzip\r\n", web_assets[i].data, web_assets[i].len); found = 1; break;
        }
        if (!found) respond_json(fd, 404, "{\"error\":\"not found\"}");
    }
    else if (!strcmp(r.path, "/api/hello")) { char o[2400]; hello_json(o, sizeof o, 1); respond_json(fd, 200, o); }
    else if (!strncmp(r.path, "/api/vouch/", 11) && !strcmp(r.method, "POST")) vouch_api(fd, &r);
    else if (!strcmp(r.path, "/api/login") && !strcmp(r.method, "POST")) {
        /* body: "<browser key hex> <label>" */
        char h[80] = "", label[64] = "browser", o[64]; int k = 0;
        sscanf(r.body, "%79s %n", h, &k);
        if (k) { snprintf(label, sizeof label, "%s", r.body + k); for (char *c = label; *c; c++) if ((unsigned char)*c < 0x20) *c = ' '; }
        if (unhex(pub, h, 32)) respond_json(fd, 400, "{\"error\":\"key\"}");
        else { snprintf(o, sizeof o, "{\"login\":\"%s\"}", login_ask(pub, label)); respond_json(fd, 200, o); }
    }
    else if (!strcmp(r.method, "GET") && !strcmp(r.path, "/api/state")) {
        if (!signed_ok(&r, 0) || unhex(pub, r.pub, 32)) respond_json(fd, 401, "{\"error\":\"not logged in\"}");
        else state_json(fd, &r, pub);
    }
    else if (!strcmp(r.method, "GET") && !strcmp(r.path, "/api/export")) {
        char t[4096], head[200]; size_t n;
        if (!signed_ok(&r, 0)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); free(r.body); return; }
        n = (size_t)snprintf(t, sizeof t, "# hassmic settings, exported from %s (%s %s)\n# not in it: the Echo's name, keys, pairings\n", core_name, board.model, VERSION);
        pthread_mutex_lock(&core_lock); n += settings_text(t + n, sizeof t - n, 1); pthread_mutex_unlock(&core_lock);
        snprintf(head, sizeof head, "Content-Disposition: attachment; filename=\"hassmic-settings.conf\"\r\n");
        respond_s(fd, &r, 200, "text/plain; charset=utf-8", head, t, n);
    }
    else if (!strcmp(r.method, "POST") && !strcmp(r.path, "/api/set")) {
        /* body: "name=value" lines, one setting or a whole export */
        char err[2048], o[2400], ej[2200]; int applied;
        if (!signed_ok(&r, 1)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); free(r.body); return; }
        pthread_mutex_lock(&core_lock); applied = settings_apply_text(r.body, err, sizeof err); pthread_mutex_unlock(&core_lock);
        jesc(ej, sizeof ej, err);
        snprintf(o, sizeof o, "{\"applied\":%d,\"errors\":\"%s\"}", applied, ej);
        fprintf(stderr, "web: %d setting%s changed%s%s", applied, applied == 1 ? "" : "s", err[0] ? ", refused: " : "\n", err);
        respond_sjson(fd, &r, 200, o);
    }
    else if (!strcmp(r.method, "POST") && !strcmp(r.path, "/api/identify")) {
        if (!signed_ok(&r, 1)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); free(r.body); return; }
        core_identify();
        respond_sjson(fd, &r, 200, "{\"identify\":true}");
    }
    else if (!strcmp(r.method, "POST") && !strcmp(r.path, "/api/reset")) {       /* body "reset": not by a stray request */
        if (!signed_ok(&r, 1)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); free(r.body); return; }
        if (!r.body || r.blen != 5 || memcmp(r.body, "reset", 5)) respond_sjson(fd, &r, 400, "{\"error\":\"say reset\"}");
        else { respond_sjson(fd, &r, 200, "{\"reset\":true}"); core_reset("settings page"); }
    }
    else if (!strcmp(r.method, "POST") && !strcmp(r.path, "/api/adb")) {
        char o[64];
        if (!signed_ok(&r, 1) || unhex(pub, r.pub, 32)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); free(r.body); return; }
        snprintf(o, sizeof o, "{\"adb\":\"%s\"}", adb_ask(pub, !strncmp(r.body, "on", 2)));
        respond_sjson(fd, &r, 200, o);
    }
    else if (!strcmp(r.method, "POST") && !strcmp(r.path, "/api/revoke")) {
        uint8_t who[32]; int found = 0;
        if (!signed_ok(&r, 1)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); free(r.body); return; }
        r.body[strcspn(r.body, " \r\n")] = 0;
        if (unhex(who, r.body, 32)) { respond_json(fd, 400, "{\"error\":\"key\"}"); free(r.body); return; }
        pthread_mutex_lock(&lk);
        for (int i = 0; i < nclients; i++) if (!memcmp(clients[i].pub, who, 32)) {
            memmove(clients + i, clients + i + 1, sizeof clients[0] * (size_t)(nclients - i - 1)); nclients--; found = 1; break;
        }
        if (found) save_clients();
        pthread_mutex_unlock(&lk);
        fprintf(stderr, "web: a browser's login %s\n", found ? "revoked" : "to revoke not found");
        respond_sjson(fd, &r, 200, found ? "{\"revoked\":true}" : "{\"revoked\":false}");
    }
    else if (!strncmp(r.path, "/api/artifact", 13)) artifact_api(fd, &r);
    else if (!strncmp(r.path, "/api/davs", 9)) davs_api(fd, &r);
    else if (!strncmp(r.path, "/api/mww", 8)) mww_api(fd, &r);
    else if (!strncmp(r.path, "/api/wifi", 9)) wifi_api(fd, &r);
    else if (!strcmp(r.method, "GET") && !strncmp(r.path, "/api/log/", 9)) log_api(fd, &r);
    else respond_json(fd, 404, "{\"error\":\"not found\"}");
    free(r.body);
}

static void *conn_thread(void *arg)
{
    int fd = (int)(long)arg;
    handle(fd); close(fd);
    atomic_fetch_sub(&nconn, 1);
    return NULL;
}

static int port;
static void *listener(void *arg)
{
    int ls = net_listen(port);
    (void)arg;
    if (ls < 0) { perror("web: listen"); return NULL; }
    fprintf(stderr, "web: settings page on port %d\n", port);
    for (;;) {
        int c = net_accept(ls); pthread_t t;
        if (c < 0) { if (errno == EINTR) continue; break; }
        if (atomic_fetch_add(&nconn, 1) >= MAX_CONN) { atomic_fetch_sub(&nconn, 1); close(c); continue; }   /* a browser opens few */
        if (pthread_create(&t, NULL, conn_thread, (void *)(long)c)) { atomic_fetch_sub(&nconn, 1); close(c); }
        else pthread_detach(t);
    }
    return NULL;
}

int web_start(int p, const struct web_hooks *h)
{
    pthread_t t;
    hooks = h; port = p;
    pthread_mutex_lock(&lk);
    int r = load_key();
    if (!r) load_clients();
    pthread_mutex_unlock(&lk);
    if (r) return -1;
    return pthread_create(&t, NULL, listener, NULL) ? -1 : (pthread_detach(t), 0);
}
