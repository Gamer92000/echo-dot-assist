/*
 * The settings page, served by hassmic on its own port.  Plain HTTP: the Echo has no certificate a browser would take,
 * and a self-signed one means a warning on every Echo.  So nothing secret is ever sent (no API key, no Sendspin token,
 * no network keys), and what changes something is signed:
 *
 *   Login: the page makes an X25519 key pair once (kept in the browser) and asks POST /api/login with its public key.
 *   The ring shows that a login waits (hooks->attention); a short press of the action button approves it (main.c
 *   on_action asks web_approve() first); unanswered for LOGIN_MS, it runs out, asked about or not.  One at a time: a
 *   second browser asking while one waits refuses both, so someone on the network cannot slip in beside the owner.  Approved keys are kept in state/web_clients.
 *   Requests: X-HM-Pub (the browser's key, hex), X-HM-Ctr (a number new for this browser, see signed_ok) and X-HM-Mac:
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
 *   The timer's own sound (sounds.c): POST /api/timer-sound, a WAV the page made of the file picked, signed; the
 *   Echo checks the format only, it decodes nothing else.
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
#include "alarms.h"
#include "apps.h"
#include "kiosk.h"
#include "adbwifi.h"
#include "arb.h"
#include "dropin.h"
#include "video.h"
#include "artifacts.h"
#include "board.h"
#include "davs.h"
#include "diag.h"
#include "sound.h"
#include "hash.h"
#include "mww.h"
#include "ringdesign.h"
#include "netio.h"
#include "settings.h"
#include "sounds.h"
#include "soundpack.h"
#include "update.h"
#include "wifi.h"
#include "ws.h"
#include "statefile.h"
#include "../third_party/monocypher.h"
#include <ctype.h>
#include "mass.h"
#include "screen.h"
#include "camera.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
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
static int LOGIN_MS = 60000;        /* a login waits this long for the button (HASSMIC_LOGIN_MS for the tests) */

static const struct web_hooks *hooks;
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;  /* everything below; never held while taking core_lock */
static uint8_t sk[32], pk[32];
/* Counters a browser may have in flight at once.  The page sends its first calls together (state, export, artifacts,
 * models, Amazon) and they arrive in any order: taking only a counter above the last, the Echo refused the ones that
 * came late, and a card said "cannot list its models" until its next round (forum #172).  ctr: the highest taken (the
 * one on disk); floor: nothing at or below it counts (the persisted ctr after a start, raised to each counter that
 * leaves seen[]); seen: the last CTR_WINDOW taken, so none counts twice */
#define CTR_WINDOW 64
static struct client { uint8_t pub[32]; unsigned long long ctr, floor, seen[CTR_WINDOW]; int nseen; char label[64]; } clients[MAX_CLIENTS];
static int nclients;
/* what waits for the button: a login, or adb for an approved browser.  state: 0 none, 1 waits, 2 approved, 3 refused */
enum { W_LOGIN = 1, W_ADB };
static struct { uint8_t pub[32]; char label[64]; long long at; int state, kind; } login;
static atomic_int nconn;
static char start_id[17];           /* new at every start: the page knows a restart from an answer of the old process */
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
        ws_random(sk, 32); b64_encode(sk, 32, t, 0, 1); strcat(t, "\n");
        if (statefile_write(p, 0600, t, strlen(t))) { crypto_wipe(t, sizeof t); fprintf(stderr, "web: cannot write %s\n", p); return -1; }
    }
    crypto_wipe(t, sizeof t);
    crypto_x25519_public_key(pk, sk);
    return 0;
}

static void save_clients(void)
{
    char p[300], h[65]; struct statefile s;
    snprintf(p, sizeof p, "%s/web_clients", state_dir());
    if (statefile_open(&s, p, 0600)) { fprintf(stderr, "web: cannot write %s.tmp\n", p); return; }
    for (int i = 0; i < nclients; i++) { hex(h, clients[i].pub, 32); fprintf(s.f, "%s %llu %s\n", h, clients[i].ctr, clients[i].label); }
    if (statefile_commit(&s)) fprintf(stderr, "web: cannot write %s\n", p);
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
        k->ctr = k->floor = c; k->nseen = 0; snprintf(k->label, sizeof k->label, "%s", line + n); nclients++;
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
        k = &clients[nclients++]; memcpy(k->pub, pub, 32); k->ctr = k->floor = 0; k->nseen = 0;
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

/* cors 0: no Access-Control headers, so that no page's script may read the answer (the kiosk's config: the token) */
static void respond_c(int fd, int cors, int code, const char *type, const char *extra, const void *body, size_t len)
{
    char h[1024];
    const char *msg = code == 200 ? "OK" : code == 204 ? "No Content" : code == 400 ? "Bad Request" : code == 401 ? "Unauthorized"
                    : code == 404 ? "Not Found" : code == 409 ? "Conflict" : code == 413 ? "Payload Too Large" : code == 503 ? "Service Unavailable" : "Error";
    int n = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\n"
                     "%s%s\r\n", code, msg, type, len,
                     cors ? "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Headers: X-HM-Pub, X-HM-Ctr, X-HM-Mac, Content-Type\r\n"
                            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\nAccess-Control-Allow-Private-Network: true\r\n" : "",
                     extra ? extra : "");
    if (send_all(fd, h, (size_t)n) == 0 && len) send_all(fd, body, len);
}
static void respond(int fd, int code, const char *type, const char *extra, const void *body, size_t len) { respond_c(fd, 1, code, type, extra, body, len); }

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

/* An approved browser's key in X-HM-Pub.  Not yet whether the request is signed: the MAC (signed_ok) is over the body,
 * so nothing short of reading it tells.  The big bodies are read only for these keys, and the requests they carry are
 * all signed ones: else anyone on the LAN could have the Echo hold 1 MB per connection, 8 at once.  The keys travel in
 * the clear (plain HTTP), so this keeps strangers out, not someone who listens on the LAN: that one gets as far as the
 * read, then the 401 */
static int approved_key(const char *hexpub)
{
    uint8_t pub[32]; int ok;
    if (unhex(pub, hexpub, 32)) return 0;
    pthread_mutex_lock(&lk); ok = client_of(pub) != NULL; pthread_mutex_unlock(&lk);
    return ok;
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
    /* a piece of an artifact, a microWakeWord model, a light ring design and a sound of one's own are the big bodies:
     * everything else is a few lines */
    long max = !strncmp(r->path, "/api/artifact/chunk/", 20) ? ART_CHUNK_MAX : !strncmp(r->path, "/api/mww/add/", 13) ? MWW_MODEL_MAX + 16384
             : !strncmp(r->path, "/api/ring/design/", 17) ? RD_TEXT_MAX
             : !strncmp(r->path, "/api/apps/apk/", 14) ? APPS_CHUNK
             : !strcmp(r->path, "/api/timer-sound") || !strncmp(r->path, "/api/sound/", 11) || !strncmp(r->path, "/api/sound-pack/", 16) ? SOUND_CUSTOM_MAX : MAX_BODY;
    if (cl < 0 || (cl > MAX_BODY && (cl > max || !approved_key(r->pub)))) return -2;
    if (!(r->body = malloc((size_t)cl + 1))) return -1;
    size_t have = n - hl < (size_t)cl ? n - hl : (size_t)cl;
    memcpy(r->body, hdr + hl, have);
    while (have < (size_t)cl) { ssize_t k = recv(fd, r->body + have, (size_t)cl - have, 0); if (k <= 0) { free(r->body); r->body = NULL; return -1; } have += (size_t)k; }
    r->body[cl] = 0; r->blen = (size_t)cl;
    return 0;
}

/* a counter not taken before: above the floor and not among the last ones taken */
static int ctr_fresh(const struct client *c, unsigned long long ctr)
{
    if (ctr <= c->floor) return 0;
    for (int i = 0; i < c->nseen; i++) if (c->seen[i] == ctr) return 0;
    return 1;
}

/* taken: into seen[], the oldest of a full window out and the floor up to it */
static void ctr_take(struct client *c, unsigned long long ctr)
{
    if (c->nseen < CTR_WINDOW) c->seen[c->nseen++] = ctr;
    else {
        int lo = 0;
        for (int i = 1; i < CTR_WINDOW; i++) if (c->seen[i] < c->seen[lo]) lo = i;
        if (c->seen[lo] > c->floor) c->floor = c->seen[lo];
        c->seen[lo] = ctr;
    }
    if (ctr > c->ctr) c->ctr = ctr;
}

/* 1 if the request is signed by an approved browser with a fresh counter (the highest written to disk when persist) */
static int signed_ok(const struct req *r, int persist)
{
    uint8_t pub[32], mac[16], want[16], k[32]; crypto_blake2b_ctx ctx; char *end; struct client *c; int ok = 0;
    unsigned long long ctr = strtoull(r->ctr, &end, 10);
    if (unhex(pub, r->pub, 32) || unhex(mac, r->mac, 16) || !r->ctr[0] || *end) return 0;
    pthread_mutex_lock(&lk);
    if ((c = client_of(pub)) && ctr_fresh(c, ctr) && !session_key(k, pub)) {
        crypto_blake2b_keyed_init(&ctx, 16, k, 32);
        crypto_blake2b_update(&ctx, (const uint8_t *)r->method, strlen(r->method)); crypto_blake2b_update(&ctx, (const uint8_t *)"\n", 1);
        crypto_blake2b_update(&ctx, (const uint8_t *)r->path, strlen(r->path)); crypto_blake2b_update(&ctx, (const uint8_t *)"\n", 1);
        crypto_blake2b_update(&ctx, (const uint8_t *)r->ctr, strlen(r->ctr)); crypto_blake2b_update(&ctx, (const uint8_t *)"\n", 1);
        crypto_blake2b_update(&ctx, (const uint8_t *)r->body, r->blen);
        crypto_blake2b_final(&ctx, want);
        if (!crypto_verify16(want, mac)) { ok = 1; ctr_take(c, ctr); if (persist) save_clients(); }
        crypto_wipe(k, sizeof k);
    }
    pthread_mutex_unlock(&lk);
    return ok;
}

/* ---------------------------------------------------------------- API */

/* members: the Echos of our network (for the login page's "through another Echo"; what beacons say anyway).
 * port: the page's own, which it calls the other Echos on; the URL it was opened at may be a reverse proxy's (80, 443) */
static size_t hello_json(char *o, size_t cap, int members)
{
    char p[65], name[160], node[160]; size_t n;
    hex(p, pk, 32); jesc(name, sizeof name, core_name); jesc(node, sizeof node, core_node_name());
    n = (size_t)snprintf(o, cap, "{\"name\":\"%s\",\"node\":\"%s\",\"model\":\"%s\",\"codename\":\"%s\",\"version\":\"%s\",\"pub\":\"%s\",\"start\":\"%s\",\"port\":%d",
                         name, node, board.model, board.codename, VERSION, p, start_id, core_web_port);
    /* no action button (board.action_combo): the page names what stands in for it */
    if (board.action_combo && n < cap) n += (size_t)snprintf(o + n, cap - n, ",\"action\":\"volume_keys\"");
    /* the Wi-Fi MAC, which ESPHome's mDNS says anyway: the Home Assistant integration (custom_components/echo_local) puts its
     * entities on the device the esphome integration made, which Home Assistant keys on it */
    { char m[24] = ""; FILE *f = fopen(getenv("HASSMIC_MAC_FILE") ? getenv("HASSMIC_MAC_FILE") : "/sys/class/net/wlan0/address", "r");
      if (f) { if (fscanf(f, "%17s", m) != 1) m[0] = 0; fclose(f); }
      for (char *c = m; *c; c++) if (!isxdigit((unsigned char)*c) && *c != ':') { m[0] = 0; break; }
      if (m[0] && n < cap) n += (size_t)snprintf(o + n, cap - n, ",\"mac\":\"%s\"", m); }
    if (members && n < cap) { n += (size_t)snprintf(o + n, cap - n, ",\"members\":"); if (n < cap) n += arb_members_json(o + n, cap - n); }
    if (n < cap) n += (size_t)snprintf(o + n, cap - n, "}");
    return n < cap ? n : cap - 1;
}

/* root's kiosk_tick keeps this file while the Kiosk Satellite app is installed: the daemon's user cannot read packages.list */
static int kiosk_app(void)
{
    const char *e = getenv("HASSMIC_KIOSK_APP");
    return board.kiosk && !access(e ? e : "/data/local/hassmic/kiosk-app", F_OK);
}

/* lock order: core_lock first (settings), lk only inside the helpers above */
/* appended at n of o (cap bytes), never past its end: what does not fit is cut (the page then refuses the answer;
 * the parts are bounded, the screen's apps by what fits) */
static size_t app(char *o, size_t n, size_t cap, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static size_t app(char *o, size_t n, size_t cap, const char *fmt, ...)
{
    if (n + 1 >= cap) return cap ? cap - 1 : 0;
    va_list ap; va_start(ap, fmt); int k = vsnprintf(o + n, cap - n, fmt, ap); va_end(ap);
    return k < 0 ? n : n + (size_t)k < cap ? n + (size_t)k : cap - 1;
}

static void state_json(int fd, const struct req *r, const uint8_t me[32])
{
    size_t cap = 65536, n = 0;   /* the settings ~25 KB, the screen's apps up to 10 */ char *o = malloc(cap), v[300];
    int whisper_refused = davs_refused("whisper");          /* davs.c's lock, never under core_lock */
    if (!o) { respond_json(fd, 503, "{}"); return; }
    n = app(o, n, cap, "{\"device\":");
    n += hello_json(o + n, cap - n, 0);
    pthread_mutex_lock(&core_lock);
    /* volume: the main one, for the page's Volume card to say what each kind of audio plays at now */
    n = app(o, n, cap, ",\"ha\":%s,\"volume\":%d,\"kiosk_app\":%s,\"settings\":[", core_ha_linked() ? "true" : "false", core_volume(), kiosk_app() ? "true" : "false");
    for (int i = 0, k = settings_count(); i < k && n < cap - 600; i++) {
        const struct setting *s = settings_at(i); const char *const *names; int nc = settings_choices(s, &names);
        n = app(o, n, cap, "%s{\"name\":\"%s\",\"label\":\"%s\",\"group\":\"%s\",\"type\":\"%s\",\"value\":%d,\"min\":%d,\"max\":%d,\"export\":%s,\"feature\":%s,\"unit\":\"%s\",\"choices\":[",
                              i ? "," : "", s->name, s->label, s->group, s->type == S_BOOL ? "bool" : s->type == S_INT ? "int" : "choice",
                              settings_get(s), settings_min(s), settings_max(s), s->export_ ? "true" : "false", s->feature ? "true" : "false", s->unit ? s->unit : "");
        for (int c = 0; c < nc && n < cap - 200; c++) { jesc(v, sizeof v, names[c]); n = app(o, n, cap, "%s\"%s\"", c ? "," : "", v); }
        n = app(o, n, cap, "]}");
    }
    /* what does not work, and why.  Models point at the page's Download from Amazon: these said scripts/artifacts.sh
     * after the page could do it, and people who had taken the USB wires off asked how (forum, 2026-10-17) */
    n = app(o, n, cap, "],\"warnings\":[");
    int w = 0;
    if (!core_ha_linked()) n = app(o, n, cap, "%s\"Home Assistant is not connected\"", w++ ? "," : "");
    /* Amazon refuses the whisper model to some models (the Echo Show 5: issue #100): then only a copy brings it */
    if (core_local_wake && core_whispered() == -2)
        n = app(o, n, cap, "%s\"%s\"", w++ ? "," : "", whisper_refused
                   ? "Whisper detection: no model on this Echo. Amazon does not hand it out to this model of Echo: copy it from another Echo that has it with Copy models (Models page)"
                   : "Whisper detection: no model on this Echo yet. Get it with Download from Amazon (Models page), or copy it from another Echo with Copy models");
    if (core_local_wake && sound_model() == SOUND_NONE)
        n = app(o, n, cap, "%s\"Sound detection: no model on this Echo (neither the firmware's nor one from Download from Amazon)\"", w++ ? "," : "");
    if (core_sound_failed())
        n = app(o, n, cap, "%s\"Sound detection could not start: its model did not load, so it was switched off (boot.log says why)\"", w++ ? "," : "");
    { const struct core_wake_word *l; if (core_local_wake && core_wake_engine(-1) == WAKE_AMAZON && core_wake_words(&l) <= 1)
        n = app(o, n, cap, "%s\"Wake words: only Alexa. Amazon's others (Echo, Computer, ...) come with Download from Amazon (Models page), or copy them from another Echo\"", w++ ? "," : ""); }
    pthread_mutex_unlock(&core_lock);
    float t = diag_soc_temp(), cpu = diag_cpu();
    pthread_mutex_lock(&lk); int adb_wait = login.state == 1 && login.kind == W_ADB; pthread_mutex_unlock(&lk);
    n = app(o, n, cap, "],\"adb\":{\"open\":%s,\"waiting\":%s},\"diag\":{\"soc_temp\":", adbwifi_open() ? "true" : "false", adb_wait ? "true" : "false");
    n = isnan(t) ? app(o, n, cap, "null") : app(o, n, cap, "%.1f", t);
    n = app(o, n, cap, ",\"cpu\":");
    n = isnan(cpu) ? app(o, n, cap, "null") : app(o, n, cap, "%.0f", cpu);
    { static const char *const models[] = { "none", "firmware", "newer" };
      n = app(o, n, cap, "},\"sound\":{\"model\":\"%s\",\"failed\":%s", models[sound_model()], core_sound_failed() ? "true" : "false"); }
    /* the timer sounds this Echo has (the stock ones where its firmware keeps them), and the length of its own */
    { static const enum sound snd[] = { SND_TIMER, SND_ALARM, SND_CUSTOM }; static const char *const nm[] = { "timer", "alarm", "custom" };
      n = app(o, n, cap, "},\"timer_sound\":{\"custom_ms\":%u,\"present\":[\"beep\"", sound_own_ms(SND_CUSTOM));
      for (int i = 0; i < 3; i++) if (sound_present(snd[i])) n = app(o, n, cap, ",\"%s\"", nm[i]);
      n = app(o, n, cap, "]"); }
    /* the sounds one's own can stand in for: its length (0: stock's plays), whether stock's is on this image, the longest */
    n = app(o, n, cap, "},\"sounds\":[");
    for (int s = 0, k = 0; s < SND_COUNT; s++) if (sound_id(s))
        n = app(o, n, cap, "%s{\"id\":\"%s\",\"own_ms\":%u,\"stock\":%s,\"max_s\":%u}", k++ ? "," : "", sound_id(s),
                              sound_own_ms(s), sound_stock_present(s) ? "true" : "false", sound_own_secs(s));
    n = app(o, n, cap, "],\"arbitration\":");
    if (arb_running()) n += arb_status_json(o + n, cap - n); else n = app(o, n, cap, "null");
    if (dropin_running()) { char peer[64]; int st = dropin_status(peer, sizeof peer); unsigned at; int res = dropin_last(&at);
        n = app(o, n, cap, ",\"dropin\":{\"state\":\"%s\",\"peer\":\"%s\",\"result\":\"%s\",\"attempt\":%u}",
                              dropin_states[st], peer, res >= 0 ? dropin_results[res] : "", at); }   /* peer: a node, [a-z0-9-] */
    n = app(o, n, cap, ",\"wifi\":"); n += wifi_current_json(o + n, cap - n);
    if (camera_present()) { n = app(o, n, cap, ",\"camera\":"); n += camera_json(o + n, cap - n); }
    if (screen_present()) {                                       /* the screen: its home app, its music pages */
        n = app(o, n, cap, ",\"screen\":"); n += screen_status_json(o + n, cap - n);
        n = app(o, n, cap, ",\"mass\":"); n += mass_status_json(o + n, cap - n);
    }
    n = app(o, n, cap, ",\"clients\":[");
    pthread_mutex_lock(&lk);
    for (int i = 0; i < nclients && n < cap - 300; i++) {
        char h[65]; hex(h, clients[i].pub, 32); jesc(v, sizeof v, clients[i].label);
        n = app(o, n, cap, "%s{\"pub\":\"%s\",\"label\":\"%s\",\"me\":%s}", i ? "," : "", h, v, memcmp(clients[i].pub, me, 32) ? "false" : "true");
    }
    pthread_mutex_unlock(&lk);
    n = app(o, n, cap, "]}");
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
        size_t cap = 262144; char *t = malloc(cap);           /* en-US wake word sets list ~4.5 KB each (69 files) */
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

/* /api/timer-sound       POST  a WAV, 16-bit mono at 48 kHz, up to 30 s (sounds.h): this Echo's own timer sound, which
 *                               the ring then plays; an empty body deletes it (the ring back to the default if it played it)
 * /api/timer-sound/play  POST  one ring of the chosen sound, 10 s at most, at the timer volume
 * Signed. */
static void timer_sound_api(int fd, struct req *r)
{
    char err[200] = "", o[300], ej[260]; int rc = -1;
    if (!signed_ok(r, 1)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); return; }
    if (strcmp(r->method, "POST")) snprintf(err, sizeof err, "not found");
    else if (!strcmp(r->path, "/api/timer-sound/play")) { core_timer_sound_preview(); rc = 0; }
    else if (strcmp(r->path, "/api/timer-sound")) snprintf(err, sizeof err, "not found");
    else if (r->blen) rc = sound_own_save(SND_CUSTOM, r->body, r->blen, err, sizeof err);
    else {
        pthread_mutex_lock(&core_lock);
        int chosen = core_timer_sound(-1) == TIMER_SOUND_CUSTOM;
        if ((rc = sound_own_delete(SND_CUSTOM))) snprintf(err, sizeof err, "cannot delete it");     /* it stays, and stays chosen */
        else if (chosen) { core_timer_sound_default(); settings_save(); core_settings_changed(); }
        pthread_mutex_unlock(&core_lock);
    }
    if (!rc) { respond_sjson(fd, r, 200, "{\"ok\":true}"); return; }
    jesc(ej, sizeof ej, err); snprintf(o, sizeof o, "{\"error\":\"%s\"}", ej);
    respond_sjson(fd, r, 400, o);
}

/* /api/sound/<id>        POST  a WAV as the timer's (sounds.h), up to the sound's max_s: played in place of stock's from
 *                               then on; an empty body deletes it (stock's again)
 * /api/sound/<id>/play   POST  it once, as it plays now (one's own or stock's), at the sounds volume
 * /api/sound/<id>/stock  GET   stock's as a WAV (16-bit mono at its own rate), for the page to measure its loudness
 * <id>: sounds.c's ("wake", "ring_in", ...).  Signed. */
static void sound_api(int fd, struct req *r)
{
    char err[200] = "", o[300], ej[260], id[32]; int rc = -1;
    if (!signed_ok(r, 1)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); return; }
    const char *p = r->path + 11, *slash = strchr(p, '/');
    size_t l = slash ? (size_t)(slash - p) : strlen(p);
    snprintf(id, sizeof id, "%.*s", (int)(l < sizeof id ? l : sizeof id - 1), p);
    int s = sound_by_id(id); const char *what = slash ? slash + 1 : "";
    if (s < 0) snprintf(err, sizeof err, "no such sound");
    else if (!strcmp(r->method, "GET") && !strcmp(what, "stock")) {
        size_t n; unsigned rate; short *pcm = sound_stock(s, &n, &rate);
        if (!pcm) { respond_sjson(fd, r, 404, "{\"error\":\"not on this firmware\"}"); return; }
        unsigned char *w = malloc(44 + n * 2);
        if (!w) { free(pcm); respond_sjson(fd, r, 500, "{\"error\":\"out of memory\"}"); return; }
        uint32_t v[] = { 36 + (uint32_t)n * 2, 16, 1 | 1u << 16, rate, rate * 2, 2 | 16u << 16, (uint32_t)n * 2 };
        memcpy(w, "RIFF", 4); memcpy(w + 4, &v[0], 4); memcpy(w + 8, "WAVEfmt ", 8); memcpy(w + 16, &v[1], 20);
        memcpy(w + 36, "data", 4); memcpy(w + 40, &v[6], 4); memcpy(w + 44, pcm, n * 2);     /* little-endian on every Echo and PC */
        respond_s(fd, r, 200, "audio/wav", NULL, w, 44 + n * 2);
        free(w); free(pcm); return;
    }
    else if (strcmp(r->method, "POST")) snprintf(err, sizeof err, "not found");
    else if (!strcmp(what, "play")) { core_sound_preview(s); rc = 0; }
    else if (*what) snprintf(err, sizeof err, "not found");
    else if (r->blen) rc = sound_own_save(s, r->body, r->blen, err, sizeof err);
    else if ((rc = sound_own_delete(s))) snprintf(err, sizeof err, "cannot delete it");
    if (!rc) { respond_sjson(fd, r, 200, "{\"ok\":true}"); return; }
    jesc(ej, sizeof ej, err); snprintf(o, sizeof o, "{\"error\":\"%s\"}", ej);
    respond_sjson(fd, r, 400, o);
}

/* /api/sound-pack                 GET   {"packs":[{id,name,bytes,sounds}],"bytes","max","packs_max"}
 * /api/sound-pack/new             POST  the name: an empty pack, {"id"}
 * /api/sound-pack/save            POST  the name: one of the sounds of one's own in use now, {"id"}
 * /api/sound-pack/<id>/file/<sound>  POST  a WAV as /api/sound/<sound> takes it; GET  it, to save
 * /api/sound-pack/<id>/apply      POST  its sounds in use, Alexa's where it has none
 * /api/sound-pack/<id>/delete     POST
 * Signed.  Not in exports, as the sounds. */
/* The home app's config and claims (kiosk.h): Home Assistant's integration sets it up, the home app reads it.
 * /api/kiosk         GET   the config without the token (signed)
 *                    POST  "key value" lines (kiosk.c), "token <sealed hex>" sealed with K as a Wi-Fi password is
 * /kiosk/config[/front[/draws[/<list>]]|/back]  GET  for the home app alone: from 127.0.0.1 and its uid, the token
 *                    in it; front/back: its activity is resumed or paused; draws: what it draws itself while in front
 * /kiosk/state[/<v>] GET  what to draw (a request's words, a ring, the next alarm), a long poll with <v> */
static int from_loopback(int fd)
{
    struct sockaddr_storage a; socklen_t n = sizeof a;
    if (getpeername(fd, (struct sockaddr *)&a, &n)) return 0;
    if (a.ss_family == AF_INET) return ((struct sockaddr_in *)&a)->sin_addr.s_addr == htonl(INADDR_LOOPBACK);
    if (a.ss_family == AF_INET6) {
        const struct in6_addr *x = &((struct sockaddr_in6 *)&a)->sin6_addr;
        static const uint8_t mapped[16] = { 0,0,0,0,0,0,0,0,0,0,0xff,0xff,127,0,0,1 };
        return IN6_IS_ADDR_LOOPBACK(x) || !memcmp(x->s6_addr, mapped, 16);
    }
    return 0;
}

static int unseal(const struct req *r, const uint8_t *in, size_t n, uint8_t *out);

static void kiosk_api(int fd, struct req *r)
{
    int write = !strcmp(r->method, "POST");
    if (!signed_ok(r, write)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); return; }
    if (!write) {
        size_t n; char *j = kiosk_json(0, &n);
        if (!j) { respond_sjson(fd, r, 503, "{\"error\":\"out of memory\"}"); return; }
        respond_s(fd, r, 200, "application/json", NULL, j, n); free(j); return;
    }
    /* the token's line unsealed in place of its sealed one; "sealed <hex>": lines sealed together (the token and the
     * screen app's settings, some of them secrets: one key stream for all of it, never one per line); every other
     * line as it came */
    size_t cap = r->blen + 4096; char *t = malloc(cap), err[160], ej[200], o[260]; size_t n = 0;
    if (!t) { respond_sjson(fd, r, 503, "{\"error\":\"out of memory\"}"); return; }
    const char *p = r->body ? r->body : "";
    while (*p) {
        const char *e = strchr(p, '\n'); size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len > 6 && !strncmp(p, "token ", 6)) {
            uint8_t sealed[256], plain[257]; char h[520]; int sl;
            snprintf(h, sizeof h, "%.*s", (int)(len - 6), p + 6); h[strcspn(h, "\r")] = 0;
            if (strcmp(h, "-")) {
                if ((sl = unhex_any(sealed, sizeof sealed, h)) < 0 || unseal(r, sealed, (size_t)sl, plain)) {
                    free(t); respond_sjson(fd, r, 400, "{\"error\":\"token: not sealed for this Echo\"}"); return;
                }
                plain[sl] = 0;
                n += (size_t)snprintf(t + n, cap - n, "token %s\n", (char *)plain);
                crypto_wipe(plain, sizeof plain);
            }
        } else if (len > 7 && !strncmp(p, "sealed ", 7)) {
            static uint8_t sealed[2048], plain[2049]; static char h[4100]; int sl;
            snprintf(h, sizeof h, "%.*s", (int)(len - 7), p + 7); h[strcspn(h, "\r")] = 0;
            if ((sl = unhex_any(sealed, sizeof sealed, h)) < 0 || unseal(r, sealed, (size_t)sl, plain)) {
                free(t); respond_sjson(fd, r, 400, "{\"error\":\"sealed: not sealed for this Echo\"}"); return;
            }
            plain[sl] = 0;
            n += (size_t)snprintf(t + n, cap - n, "%s\n", (char *)plain);
            crypto_wipe(plain, sizeof plain);
        } else n += (size_t)snprintf(t + n, cap - n, "%.*s\n", (int)len, p);
        p = e ? e + 1 : p + len;
    }
    int bad = kiosk_store(t, err, sizeof err);
    crypto_wipe(t, cap); free(t);
    if (bad) { jesc(ej, sizeof ej, err); snprintf(o, sizeof o, "{\"error\":\"%s\"}", ej); respond_sjson(fd, r, 400, o); return; }
    respond_sjson(fd, r, 200, "{\"ok\":true}");
}

/* The owner's apps (apps.h): GET /api/apps the installed ones, which may reach the internet and how the last install
 * or removal went; POST "net <package> 0|1" (the internet) or "lan <package> 0|1" (reachable from the home network).  POST /api/apps/apk/<offset> a piece of an APK (APPS_CHUNK at most, in
 * order), /api/apps/apk/install "<size> <BLAKE2b-256 hex>", /api/apps/uninstall "<package>": root's apps.sh does them,
 * GET /api/apps says when (install.state).  An approved client only (this page, the Echo Local integration): an app
 * runs code on the Echo, and the internet switch opens the firewall for it */
static void apps_api(int fd, struct req *r)
{
    int write = !strcmp(r->method, "POST");
    if (!signed_ok(r, write)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); return; }
    const char *sub = r->path + 9;
    if (write && !strncmp(sub, "/apk/", 5)) {
        int e;
        if (!strcmp(sub + 5, "install")) {
            long size = 0; char hex[80] = "";
            if (!r->body || sscanf(r->body, "%ld %79s", &size, hex) != 2) { respond_sjson(fd, r, 400, "{\"error\":\"<size> <blake2b hex>\"}"); return; }
            e = apps_install(size, hex);
            if (e == -1) { respond_sjson(fd, r, 400, "{\"error\":\"not the APK that was sent: size or digest differ\"}"); return; }
        } else {
            char *end; long off = strtol(sub + 5, &end, 10);
            if (*end || end == sub + 5) { respond_sjson(fd, r, 404, "{\"error\":\"not found\"}"); return; }
            e = apps_chunk(off, r->body ? r->body : "", r->blen);
            if (e == -1) { respond_sjson(fd, r, 409, "{\"error\":\"not where the APK ends\"}"); return; }
            if (e == -2) { respond_sjson(fd, r, 413, "{\"error\":\"an APK of 200 MB at most\"}"); return; }
        }
        if (e == -2) respond_sjson(fd, r, 409, "{\"error\":\"an install or removal is under way\"}");
        else if (e) respond_sjson(fd, r, 500, "{\"error\":\"cannot write the APK\"}");
        else respond_sjson(fd, r, 200, "{\"ok\":true}");
        return;
    }
    if (write && !strcmp(sub, "/uninstall")) {
        char pkg[160] = "";
        if (r->body) sscanf(r->body, "%159s", pkg);
        int e = apps_uninstall(pkg);
        if (e == -1) respond_sjson(fd, r, 400, "{\"error\":\"not an app installed on this Echo\"}");
        else if (e == -2) respond_sjson(fd, r, 409, "{\"error\":\"an install or removal is under way\"}");
        else if (e) respond_sjson(fd, r, 500, "{\"error\":\"cannot write the request\"}");
        else respond_sjson(fd, r, 200, "{\"ok\":true}");
        return;
    }
    if (*sub) { respond_sjson(fd, r, 404, "{\"error\":\"not found\"}"); return; }
    if (!write) {
        size_t n; char *j = apps_json(&n);
        if (!j) { respond_sjson(fd, r, 200, "{\"apps\":[],\"none\":true}"); return; }    /* no Android here */
        respond_s(fd, r, 200, "application/json", NULL, j, n); free(j); return;
    }
    char pkg[160]; int on = -1;
    char kind[4] = "";
    if (!r->body || sscanf(r->body, "%3s %159s %d", kind, pkg, &on) != 3 || (strcmp(kind, "net") && strcmp(kind, "lan")) || (on != 0 && on != 1)) {
        respond_sjson(fd, r, 400, "{\"error\":\"net|lan <package> 0|1\"}"); return;
    }
    int e = kind[0] == 'n' ? apps_set_net(pkg, on) : apps_set_lan(pkg, on);
    if (e == -1) respond_sjson(fd, r, 400, "{\"error\":\"not an app installed on this Echo\"}");
    else if (e) respond_sjson(fd, r, 500, "{\"error\":\"cannot write state/apps\"}");
    else respond_sjson(fd, r, 200, "{\"ok\":true}");
}

/* The Echo's own alarms (alarms.h): GET /api/alarms the list; POST /api/alarms/put "id hour min days enabled label" (id 0
 * adds one), /api/alarms/<id>/delete, /api/alarms/snooze, /api/alarms/stop (what rings now) */
static void alarms_api(int fd, struct req *r)
{
    char err[160] = "", o[260], ej[200]; int get = !strcmp(r->method, "GET");
    if (!signed_ok(r, 1)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); return; }
    const char *p = r->path + 11;
    if (!*p && get) {
        size_t n = 0; char *j = alarms_json(&n), *o2;
        if (!j || n < 2) { free(j); respond_sjson(fd, r, 500, "{\"error\":\"out of memory\"}"); return; }
        /* what rings now is the core's: in front of alarms.c's own */
        pthread_mutex_lock(&core_lock); int rang = core_alarm_ringing() || core_timers_ringing(NULL, 0); pthread_mutex_unlock(&core_lock);
        if (asprintf(&o2, "{\"ringing\":%s,%s", rang ? "true" : "false", j + 1) < 0) { free(j); respond_sjson(fd, r, 500, "{\"error\":\"out of memory\"}"); return; }
        respond_s(fd, r, 200, "application/json", NULL, o2, strlen(o2)); free(o2); free(j); return;
    }
    if (get) { respond_sjson(fd, r, 405, "{\"error\":\"POST\"}"); return; }
    if (!strcmp(p, "/put")) {
        char line[160]; snprintf(line, sizeof line, "%.*s", (int)(r->blen < sizeof line - 1 ? r->blen : sizeof line - 1), r->body ? r->body : "");
        if (!alarms_put_text(line, err, sizeof err)) { respond_sjson(fd, r, 200, "{\"ok\":true}"); return; }
    } else if (!strcmp(p, "/snooze") || !strcmp(p, "/stop")) {
        pthread_mutex_lock(&core_lock);
        int rang = core_alarm_ringing() || core_timers_ringing(NULL, 0);
        if (p[1] == 's' && p[2] == 'n') { if (!core_alarm_snooze("settings page")) core_alarm_stop("settings page"); }
        else core_alarm_stop("settings page");
        pthread_mutex_unlock(&core_lock);
        respond_sjson(fd, r, 200, rang ? "{\"ok\":true}" : "{\"ok\":false}"); return;
    } else {
        char *end; long id = *p == '/' ? strtol(p + 1, &end, 10) : 0;
        if (id > 0 && !strcmp(end, "/delete")) { if (!alarms_delete((int)id)) { respond_sjson(fd, r, 200, "{\"ok\":true}"); return; } snprintf(err, sizeof err, "no alarm %ld", id); }
        else snprintf(err, sizeof err, "not found");
    }
    jesc(ej, sizeof ej, err[0] ? err : "refused"); snprintf(o, sizeof o, "{\"error\":\"%s\"}", ej);
    respond_sjson(fd, r, 400, o);
}

static void sound_pack_api(int fd, struct req *r)
{
    char err[200] = "", o[300], ej[260]; int rc = -1, get = !strcmp(r->method, "GET");
    if (!signed_ok(r, 1)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); return; }
    const char *p = r->path + 15;
    if (!*p && get) {
        size_t cap = 16384, n = 0; char *j = malloc(cap); int k = 0;
        if (!j) { respond_sjson(fd, r, 500, "{\"error\":\"out of memory\"}"); return; }
        n += (size_t)snprintf(j + n, cap - n, "{\"packs\":[");
        for (int id = 1; id <= SP_PACKS_MAX; id++) {
            char name[SP_NAME_MAX + 1], q[400]; size_t b; unsigned s; int c = 0;
            if (soundpack_info(id, name, &b, &s)) continue;
            jesc(q, sizeof q, name);
            n += (size_t)snprintf(j + n, cap - n, "%s{\"id\":%d,\"name\":\"%s\",\"bytes\":%zu,\"sounds\":[", k++ ? "," : "", id, q, b);
            for (int i = 0; i < SND_COUNT; i++) if (s >> i & 1) n += (size_t)snprintf(j + n, cap - n, "%s\"%s\"", c++ ? "," : "", sound_id(i));
            n += (size_t)snprintf(j + n, cap - n, "]}");
        }
        n += (size_t)snprintf(j + n, cap - n, "],\"bytes\":%zu,\"max\":%zu,\"packs_max\":%d}", soundpack_bytes(), soundpack_max(), SP_PACKS_MAX);
        respond_s(fd, r, 200, "application/json", NULL, j, n);
        free(j); return;
    }
    if (!strcmp(p, "/new") || !strcmp(p, "/save")) {
        int id = get ? -1 : soundpack_new(r->body ? r->body : "", r->blen, p[1] == 's', err, sizeof err);
        if (id > 0) { snprintf(o, sizeof o, "{\"id\":%d}", id); respond_sjson(fd, r, 200, o); return; }
    } else {
        char *end; long id = *p == '/' ? strtol(p + 1, &end, 10) : 0;
        if (id <= 0 || id > SP_PACKS_MAX || *end != '/') snprintf(err, sizeof err, "no such pack");
        else if (!strncmp(end, "/file/", 6)) {
            int s = sound_by_id(end + 6);
            if (get) {
                size_t n; unsigned char *w = soundpack_get((int)id, s, &n);
                if (!w) { respond_sjson(fd, r, 404, "{\"error\":\"not in this pack\"}"); return; }
                respond_s(fd, r, 200, "audio/wav", NULL, w, n);
                free(w); return;
            }
            rc = soundpack_put((int)id, s, r->body, r->blen, err, sizeof err);
        }
        else if (get) snprintf(err, sizeof err, "not found");
        else if (!strcmp(end, "/apply")) rc = soundpack_apply((int)id, err, sizeof err);
        else if (!strcmp(end, "/delete")) { if ((rc = soundpack_delete((int)id))) snprintf(err, sizeof err, "no such pack"); }
        else snprintf(err, sizeof err, "not found");
    }
    if (!rc) { respond_sjson(fd, r, 200, "{\"ok\":true}"); return; }
    jesc(ej, sizeof ej, err[0] ? err : "not found"); snprintf(o, sizeof o, "{\"error\":\"%s\"}", ej);
    respond_sjson(fd, r, 400, o);
}

/* /api/mww                              GET   {"engine","active","second","models":[...]}: microWakeWord's models (mww_store.c)
 * /api/mww/add/<id>                     POST  "<manifest length>\n<manifest JSON><model.tflite>": a new model, or a new
 *                                             version of <id> (length 0: no manifest, defaults)
 * /api/mww/edit/<id>                    POST  "name=..", "cutoff=0.85", "window=5" lines
 * /api/mww/delete/<id>                  POST
 * /api/mww/use/<id>                     POST  the active wake word, then the only one (with microWakeWord on)
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
        int act[CORE_WAKE_ACTIVE_MAX], na = mww && k ? core_wake_active(act) : 0;
        if (na) { jesc(ej, sizeof ej, w[act[0]].id); n += (size_t)snprintf(t + n, cap - n, "\"%s\"", ej); }
        else n += (size_t)snprintf(t + n, cap - n, "null");
        n += (size_t)snprintf(t + n, cap - n, ",\"second\":");    /* Home Assistant's "Wake word 2" */
        if (na > 1) { jesc(ej, sizeof ej, w[act[1]].id); n += (size_t)snprintf(t + n, cap - n, "\"%s\"", ej); }
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

/* /api/ring              GET   the slots, what each has (drafted and on the ring), the library's names (ringdesign.c)
 * /api/ring/designs      GET   every design as text, "design <id>" before each
 * /api/ring/stock/<slot> GET   stock's animation for a slot, as its file has it (the designer starts from it; volume:
 *                              every step, "step NN" before each)
 * /api/ring/design/<id>  POST  a design's text (ringdesign.h); id 0: a new one                        -> {"id"}
 * /api/ring/delete/<id>  POST
 * /api/ring/assign       POST  "<slot> <design id>" lines, 0: stock's
 * /api/ring/packs        GET   every theme pack as text, "pack <id>" before each
 * /api/ring/pack/<id>    POST  a pack as the Echo keeps it (ringdesign.h, version 2), checked; id 0: a new one -> {"id"}
 * /api/ring/pack-delete/<id> POST
 * /api/ring/apply        POST  the assignments onto the ring: root installs them, the ring goes dark for a second
 * All signed. */
static void ring_api(int fd, struct req *r)
{
    char err[200] = "", o[300], ej[260], slot[32]; int rc = -1, id = 0, got = 0, write = !strcmp(r->method, "POST");
    if (!signed_ok(r, write)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); return; }
    if (!write && !strcmp(r->path, "/api/ring")) {
        size_t cap = 65536; char *t = malloc(cap);
        size_t n = t ? rd_status_json(t, cap) : 0;
        if (!n) { free(t); respond_sjson(fd, r, 503, "{\"error\":\"no memory\"}"); return; }
        respond_s(fd, r, 200, "application/json", NULL, t, n); free(t); return;
    }
    if (!write && (!strcmp(r->path, "/api/ring/designs") || !strcmp(r->path, "/api/ring/packs") || sscanf(r->path, "/api/ring/stock/%31s", slot) == 1)) {
        size_t n = 0; char *t = r->path[10] == 'd' ? rd_library_text(&n) : r->path[10] == 'p' ? rd_packs_text(&n) : rd_stock(slot, &n);
        if (!t) { respond_sjson(fd, r, 404, "{\"error\":\"not found\"}"); return; }
        respond_s(fd, r, 200, "text/plain; charset=utf-8", NULL, t, n); free(t); return;
    }
    if (write && sscanf(r->path, "/api/ring/design/%d", &id) == 1) {
        if (!(rc = rd_save(id, r->body, r->blen, &got, err, sizeof err))) { snprintf(o, sizeof o, "{\"id\":%d}", got); respond_sjson(fd, r, 200, o); return; }
    }
    else if (write && sscanf(r->path, "/api/ring/pack/%d", &id) == 1) {
        if (!(rc = rd_pack_save(id, r->body, r->blen, &got, err, sizeof err))) { snprintf(o, sizeof o, "{\"id\":%d}", got); respond_sjson(fd, r, 200, o); return; }
    }
    else if (write && sscanf(r->path, "/api/ring/pack-delete/%d", &id) == 1) rc = rd_pack_delete(id, err, sizeof err);
    else if (write && sscanf(r->path, "/api/ring/delete/%d", &id) == 1) rc = rd_delete(id, err, sizeof err);
    else if (write && !strcmp(r->path, "/api/ring/assign")) rc = rd_assign(r->body, err, sizeof err);
    else if (write && !strcmp(r->path, "/api/ring/apply")) rc = rd_apply(err, sizeof err);
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
 * over "hassmic seal 1".  The counter is new with every request of a browser (signed_ok takes each once), so no
 * stream serves twice; the request's MAC covers the sealed bytes.  web/crypto.js seal does the other half. */
static int unseal(const struct req *r, const uint8_t *in, size_t n, uint8_t *out)
{
    uint8_t pub[32], k[32], ek[32], ks[64]; crypto_blake2b_ctx ctx; int bad;
    if (n > 64 * 40 || unhex(pub, r->pub, 32)) return -1;   /* callers bound n by their buffers: a Wi-Fi password, a token */
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

/* /api/screen  POST "home <package>" | "home" (the face) | "home -" (none chosen: Kiosk Satellite where installed).  Signed */
static void screen_api(int fd, struct req *r)
{
    if (!signed_ok(r, 1)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); return; }
    if (!screen_present() || strcmp(r->method, "POST")) { respond_sjson(fd, r, 404, "{\"error\":\"not found\"}"); return; }
    char what[16] = "", v[128] = ""; int k = sscanf(r->body, "%15s %127s", what, v);
    if (k < 1 || strcmp(what, "home") || screen_set_home(k < 2 ? "" : !strcmp(v, "-") ? NULL : v)) { respond_sjson(fd, r, 400, "{\"error\":\"bad request\"}"); return; }
    size_t cap = 16384; char *o = malloc(cap);
    if (!o) { respond_sjson(fd, r, 503, "{\"error\":\"no memory\"}"); return; }
    size_t n = screen_status_json(o, cap);
    respond_s(fd, r, 200, "application/json", NULL, o, n); free(o);
}

/* /api/mass  POST "<url hex | -> <token sealed, hex | - | =>": Music Assistant's API for the screen (mass.c): its
 * address ("-": none; without a token the Echo looks at the one Sendspin played from) and a long-lived token, sealed
 * as a Wi-Fi password ("-": forget it, "=": keep it); a token needs an address (409).  Signed.  The token is never
 * sent back: /api/state says only whether there is one. */
static void mass_api(int fd, struct req *r)
{
    char uh[600] = "", th[5000] = "", url[260] = ""; uint8_t sealed[2400], tok[2401]; int tl = 0, ul = 0;
    if (!signed_ok(r, 1)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); return; }
    if (!screen_present() || strcmp(r->method, "POST")) { respond_sjson(fd, r, 404, "{\"error\":\"not found\"}"); return; }
    if (sscanf(r->body, "%599s %4999s", uh, th) != 2 || (strcmp(uh, "-") && (ul = unhex_any((uint8_t *)url, sizeof url - 1, uh)) < 0)
        || (strcmp(th, "-") && strcmp(th, "=") && ((tl = unhex_any(sealed, sizeof sealed, th)) < 0 || unseal(r, sealed, (size_t)tl, tok)))) {
        respond_sjson(fd, r, 400, "{\"error\":\"bad request\"}"); return;
    }
    if (strcmp(uh, "-")) url[ul] = 0;
    for (char *c = url; *c; c++) if ((unsigned char)*c < 0x21 || *c == '"' || *c == '\\' || (unsigned char)*c > 0x7e) {
        crypto_wipe(tok, sizeof tok); crypto_wipe(sealed, sizeof sealed); respond_sjson(fd, r, 400, "{\"error\":\"bad address\"}"); return;
    }
    tok[tl] = 0;                                        /* the page pads with zeros: a token has none */
    for (int i = 0; i < tl && tok[i]; i++) if (tok[i] < 0x21 || tok[i] > 0x7e || tok[i] == '"' || tok[i] == '\\') {
        crypto_wipe(tok, sizeof tok); crypto_wipe(sealed, sizeof sealed); respond_sjson(fd, r, 400, "{\"error\":\"not a token\"}"); return;
    }
    int rc = mass_set(url, !strcmp(th, "-") ? "-" : !strcmp(th, "=") ? "" : (const char *)tok);
    crypto_wipe(tok, sizeof tok); crypto_wipe(sealed, sizeof sealed);
    if (rc == -2) { respond_sjson(fd, r, 409, "{\"error\":\"Give Music Assistant's address\"}"); return; }
    if (rc == -4) { respond_sjson(fd, r, 400, "{\"error\":\"The Echo talks to Music Assistant's API without TLS: give its http address (port 8095)\"}"); return; }
    if (rc == -3) { respond_sjson(fd, r, 409, "{\"error\":\"A new address takes the token again: it only ever goes where it was given\"}"); return; }
    char o[4096]; size_t n = mass_status_json(o, sizeof o);
    respond_s(fd, r, rc ? 500 : 200, "application/json", NULL, o, n);
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

/* The camera's H.264 (camera.h, docs/camera.md), for the Echo Local integration: a signed GET, then records until the
 * stream ends or the viewer goes.  The body has no end, so no X-HM-Mac over it (respond_s): the answer's head is signed
 * over the server's nonce instead, and every record is sealed with a key of this stream's,
 *     SK = BLAKE2b-256 keyed with K over "hassmic camera 1\n" <counter> "\n" <nonce, 16 bytes>
 * as ChaCha20-Poly1305 (IETF, noise.c's nonce: 4 zero bytes, the record's number as 8 little-endian ones):
 *     length u32 LE (of what follows) | ciphertext | tag 16      the length as associated data
 *     plaintext: type u8 (camera.h CAM_*) | pts u64 LE (us) | H.264 (Annex B)
 * The video is K's alone on the LAN; the signed request (fresh counter) is what lets the camera on.  A failed send is
 * the viewer gone: the camera stops with it. */
static void camera_api(int fd, const struct req *r)
{
    uint8_t pub[32], k[32], sk[32], nonce[16], mac[16]; char nh[33], mh[33], h[600]; const char *why = "";
    crypto_blake2b_ctx ctx; int bad;
    if (!signed_ok(r, 0) || unhex(pub, r->pub, 32)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); return; }
    int id = camera_begin(&why);
    if (id < 0) { char o[64]; snprintf(o, sizeof o, "{\"error\":\"%s\"}", why); respond_sjson(fd, r, 409, o); return; }
    pthread_mutex_lock(&lk); bad = session_key(k, pub); pthread_mutex_unlock(&lk);
    if (bad) { camera_end(id); respond_json(fd, 401, "{\"error\":\"not logged in\"}"); return; }
    ws_random(nonce, sizeof nonce); hex(nh, nonce, 16);
    crypto_blake2b_keyed_init(&ctx, 32, k, 32);
    crypto_blake2b_update(&ctx, (const uint8_t *)"hassmic camera 1\n", 17);
    crypto_blake2b_update(&ctx, (const uint8_t *)r->ctr, strlen(r->ctr)); crypto_blake2b_update(&ctx, (const uint8_t *)"\n", 1);
    crypto_blake2b_update(&ctx, nonce, 16);
    crypto_blake2b_final(&ctx, sk);
    crypto_blake2b_keyed_init(&ctx, 16, k, 32);                 /* the head: respond_s's MAC, the nonce as its body */
    crypto_blake2b_update(&ctx, (const uint8_t *)"RESP\n", 5);
    crypto_blake2b_update(&ctx, (const uint8_t *)r->ctr, strlen(r->ctr)); crypto_blake2b_update(&ctx, (const uint8_t *)"\n", 1);
    crypto_blake2b_update(&ctx, (const uint8_t *)nh, 32);
    crypto_blake2b_final(&ctx, mac); crypto_wipe(k, sizeof k); hex(mh, mac, 16);
    int hn = snprintf(h, sizeof h, "HTTP/1.1 200 OK\r\nContent-Type: application/x-hassmic-camera\r\nConnection: close\r\nCache-Control: no-store\r\n"
                      "X-HM-Nonce: %s\r\nX-HM-Mac: %s\r\n\r\n", nh, mh);
    int t; long long pts; uint8_t *d = NULL; size_t len; unsigned long long seq = 0; int sent = 0;
    if (send_all(fd, h, (size_t)hn)) goto done;
    for (;;) {
        int g = camera_next(id, &t, &pts, &d, &len, 1000);
        if (g < 0) break;
        if (!g) continue;
        size_t pl = 9 + len, rl = 4 + pl + 16; uint8_t *rec = malloc(rl), *pt = malloc(pl);
        if (!rec || !pt) { free(rec); free(pt); free(d); break; }
        pt[0] = (uint8_t)t;
        for (int i = 0; i < 8; i++) pt[1 + i] = (uint8_t)((unsigned long long)pts >> (8 * i));
        memcpy(pt + 9, d, len); free(d); d = NULL;
        for (int i = 0; i < 4; i++) rec[i] = (uint8_t)((pl + 16) >> (8 * i));
        uint8_t n12[12] = { 0 }; crypto_aead_ctx a;
        for (int i = 0; i < 8; i++) n12[4 + i] = (uint8_t)(seq >> (8 * i));
        crypto_aead_init_ietf(&a, sk, n12);
        crypto_aead_write(&a, rec + 4, rec + 4 + pl, rec, 4, pt, pl);
        crypto_wipe(&a, sizeof a); seq++;
        int f = send_all(fd, rec, rl);
        free(rec); free(pt);
        if (f) break;
        sent++;
    }
done:
    crypto_wipe(sk, sizeof sk);
    camera_end(id);
    fprintf(stderr, "web: camera stream closed after %d records\n", sent);
}

/* A Drop In from Home Assistant (dropin.h dropin_ha_*, docs/video-dropin.md): a signed GET from the Echo Local
 * integration, then this connection is the call, both ways, as the camera stream's records: the head signed over the
 * server's nonce, each record sealed with ChaCha20-Poly1305 (noise.c's nonce layout, the record's number per
 * direction, its length as associated data) under a key of this call's and direction's,
 *     SK = BLAKE2b-256 keyed with K over "hassmic call 1 <down|up>\n" <counter> "\n" <nonce, 16 bytes>
 * down (Echo -> Home Assistant):  'S' <state: dropin_states[]>            on every change; "idle" last
 *                                  'A' <320 samples s16le, 16 kHz>          this side's mic, through Drop In's gate and gain
 *                                  'V' <type u8> <pts u64 LE, us> <H.264>   the camera (camera.h's records) while the call is
 *                                                                           connected, the setting "camera" is on and the lens
 *                                                                           open: one viewer, as a stream; the red dot shows
 * up (Home Assistant -> Echo):    'A' <320 samples s16le>                  to play; sent all along, silence too: the
 *                                                                           stream is the call's keepalive (LOST_MS)
 *                                  'V' <type u8> <pts u64 LE, us> <H.264>   the browser's camera, for this screen (video.c;
 *                                                                           ignored where there is no player); a record up
 *                                                                           to CALL_UP_MAX
 *                                  'H'                                      hung up
 * The connection's end hangs up as well. */
#define CALL_REC_MAX 4096
#define CALL_UP_MAX  (512 << 10)        /* a browser's key frame: WebCodecs at 640x360 makes 20-40 kB; room for more */
static int call_seal(int fd, const uint8_t sk[32], unsigned long long *seq, uint8_t type, const void *p, size_t n)
{
    uint8_t small[4 + 1 + CALL_REC_MAX + 16], n12[12] = { 0 }; crypto_aead_ctx a;
    size_t pl = 1 + n;
    uint8_t *rec = 4 + pl + 16 <= sizeof small ? small : malloc(4 + pl + 16);      /* a key frame: tens of kB */
    if (!rec) return -1;
    uint8_t *pt = rec + 4;                              /* sealed in place: crypto_aead_write takes the same buffer */
    pt[0] = type; if (n) memcpy(pt + 1, p, n);
    for (int i = 0; i < 4; i++) rec[i] = (uint8_t)((pl + 16) >> (8 * i));
    for (int i = 0; i < 8; i++) n12[4 + i] = (uint8_t)(*seq >> (8 * i));
    crypto_aead_init_ietf(&a, sk, n12);
    crypto_aead_write(&a, rec + 4, rec + 4 + pl, rec, 4, pt, pl);
    crypto_wipe(&a, sizeof a); (*seq)++;
    int r = send_all(fd, rec, 4 + pl + 16);
    if (rec != small) free(rec);
    return r;
}

static void call_key(uint8_t out[32], const uint8_t k[32], const char *dir, const char *ctr, const uint8_t nonce[16])
{
    crypto_blake2b_ctx ctx; char lb[32];
    int l = snprintf(lb, sizeof lb, "hassmic call 1 %s\n", dir);
    crypto_blake2b_keyed_init(&ctx, 32, k, 32);
    crypto_blake2b_update(&ctx, (const uint8_t *)lb, (size_t)l);
    crypto_blake2b_update(&ctx, (const uint8_t *)ctr, strlen(ctr)); crypto_blake2b_update(&ctx, (const uint8_t *)"\n", 1);
    crypto_blake2b_update(&ctx, nonce, 16);
    crypto_blake2b_final(&ctx, out);
}

static void call_api(int fd, const struct req *r)
{
    uint8_t pub[32], k[32], down[32], up[32], nonce[16], mac[16]; char nh[33], mh[33], h[600], why[96] = "";
    crypto_blake2b_ctx ctx; int bad;
    if (!signed_ok(r, 0) || unhex(pub, r->pub, 32)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); return; }
    pthread_mutex_lock(&lk); bad = session_key(k, pub); pthread_mutex_unlock(&lk);
    if (bad) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); return; }
    int gen = dropin_ha_begin(why, sizeof why);
    if (gen < 0) { char o[160], w[100]; jesc(w, sizeof w, why); snprintf(o, sizeof o, "{\"error\":\"%s\"}", w); respond_sjson(fd, r, 409, o); crypto_wipe(k, sizeof k); return; }
    ws_random(nonce, sizeof nonce); hex(nh, nonce, 16);
    call_key(down, k, "down", r->ctr, nonce); call_key(up, k, "up", r->ctr, nonce);
    crypto_blake2b_keyed_init(&ctx, 16, k, 32);
    crypto_blake2b_update(&ctx, (const uint8_t *)"RESP\n", 5);
    crypto_blake2b_update(&ctx, (const uint8_t *)r->ctr, strlen(r->ctr)); crypto_blake2b_update(&ctx, (const uint8_t *)"\n", 1);
    crypto_blake2b_update(&ctx, (const uint8_t *)nh, 32);
    crypto_blake2b_final(&ctx, mac); crypto_wipe(k, sizeof k); hex(mh, mac, 16);
    int hn = snprintf(h, sizeof h, "HTTP/1.1 200 OK\r\nContent-Type: application/x-hassmic-call\r\nConnection: close\r\nCache-Control: no-store\r\n"
                      "X-HM-Nonce: %s\r\nX-HM-Mac: %s\r\n\r\n", nh, mh);
    const char *end = "Home Assistant went";
    unsigned long long dseq = 0, useq = 0; int last = -1, frames_in = 0, frames_out = 0, cam = -1, pictures = 0; long long cam_try = 0;
    static uint8_t in[4 + 1 + CALL_UP_MAX + 16 + 8192]; size_t have = 0;   /* (one call at a time: dropin_ha_begin) */
    static uint8_t pt[1 + CALL_UP_MAX];
    int videos_in = 0;
    if (send_all(fd, h, (size_t)hn)) goto done;
    for (;;) {
        int st = dropin_ha_state(gen);
        if (st != last) { last = st; if (call_seal(fd, down, &dseq, 'S', dropin_states[st], strlen(dropin_states[st]))) break; }
        if (st == DROPIN_IDLE) { end = NULL; break; }             /* ended here: the Echo's button, "hang up", lost */
        int16_t pcm[320]; int fail = 0;
        while (dropin_ha_out(gen, pcm)) { if (call_seal(fd, down, &dseq, 'A', pcm, sizeof pcm)) { fail = 1; break; } frames_out++; }
        if (fail) break;
        /* the picture: while connected, tried again every 2 s (the setting switched on, the lens opened, a stream that
         * had the camera over) */
        if (st == DROPIN_LIVE && cam < 0 && now_ms() - cam_try > 2000) {
            const char *cw = ""; cam_try = now_ms();
            if (camera_present() && (cam = camera_begin(&cw)) < 0 && strcmp(cw, "off") && strcmp(cw, "covered")) fprintf(stderr, "web: no picture in the call: %s\n", cw);
        }
        while (cam > 0) {
            int t; long long pts; uint8_t *d = NULL; size_t len;
            int g = camera_next(cam, &t, &pts, &d, &len, 0);
            if (g < 0) { camera_end(cam); cam = -1; cam_try = now_ms(); video_self_stop(gen); break; }
            if (!g) break;
            video_self(gen, t, pts, d, len);                       /* what leaves, in this screen's corner */
            uint8_t *v = malloc(9 + len);
            if (v) {
                v[0] = (uint8_t)t; for (int i = 0; i < 8; i++) v[1 + i] = (uint8_t)((unsigned long long)pts >> (8 * i));
                memcpy(v + 9, d, len);
                if (call_seal(fd, down, &dseq, 'V', v, 9 + len)) fail = 1; else pictures++;
                free(v);
            }
            free(d);
            if (fail) break;
        }
        if (fail) break;
        struct pollfd q = { fd, POLLIN, 0 };
        int k2 = poll(&q, 1, 10);
        if (k2 < 0 && errno != EINTR) break;
        if (k2 <= 0) continue;
        ssize_t got = recv(fd, in + have, sizeof in - have, MSG_DONTWAIT);
        if (got == 0) break;
        if (got < 0) { if (errno == EAGAIN || errno == EINTR) continue; break; }
        have += (size_t)got;
        size_t at = 0;
        while (have - at >= 4) {
            uint32_t n = (uint32_t)in[at] | (uint32_t)in[at + 1] << 8 | (uint32_t)in[at + 2] << 16 | (uint32_t)in[at + 3] << 24;
            if (n < 1 + 16 || n > 1 + CALL_UP_MAX + 16) { fail = 1; break; }
            if (have - at < 4 + n) break;
            uint8_t n12[12] = { 0 }; crypto_aead_ctx a;
            for (int i = 0; i < 8; i++) n12[4 + i] = (uint8_t)(useq >> (8 * i));
            crypto_aead_init_ietf(&a, up, n12);
            int ok = !crypto_aead_read(&a, pt, in + at + 4 + n - 16, in + at, 4, in + at + 4, n - 16);
            crypto_wipe(&a, sizeof a);
            if (!ok) { fprintf(stderr, "web: a call record not sealed by Home Assistant\n"); fail = 1; break; }
            useq++;
            size_t pl = n - 16;
            if (pt[0] == 'A' && pl == 1 + 640) { int16_t f[320]; memcpy(f, pt + 1, 640); dropin_ha_in(gen, f); frames_in++; }
            else if (pt[0] == 'V' && pl > 1 + 9 && pt[1] >= 1 && pt[1] <= 3 && dropin_ha_state(gen) == DROPIN_LIVE) {
                long long pts = 0; for (int i = 0; i < 8; i++) pts |= (long long)pt[2 + i] << (8 * i);
                video_show(gen, pt[1], pts, pt + 10, pl - 10); videos_in++;
            }
            else if (pt[0] == 'H') { end = "Home Assistant hung up"; fail = 1; break; }
            at += 4 + n;
        }
        if (fail) break;
        memmove(in, in + at, have - at); have -= at;
    }
done:
    if (cam > 0) camera_end(cam);
    if (end) dropin_ha_end(gen, end);
    if (dropin_ha_state(gen) == DROPIN_IDLE && last != DROPIN_IDLE) call_seal(fd, down, &dseq, 'S', "idle", 4);   /* (it may hear it) */
    crypto_wipe(down, sizeof down); crypto_wipe(up, sizeof up);
    video_stop(gen);
    fprintf(stderr, "web: call from Home Assistant closed: %d frames in, %d out, %d pictures, %d of the browser's\n", frames_in, frames_out, pictures, videos_in);
}

/* Online updates come from GitHub: a line of BODY that switches them on (from off) counts only once this Echo reached
 * it (update_probe), else it is turned into a comment and WHY says what failed.  Imports go the same way.  Before
 * core_lock: the probe takes up to 15 s. */
static void updates_gate(char *body, char *why, size_t cap)
{
    why[0] = 0;
    for (char *p = body; *p; ) {
        size_t l = strcspn(p, "\n"); char *k = p + strspn(p, " \t"), v[16];
        if (!strncmp(k, "online_updates=", 15)) {
            snprintf(v, sizeof v, "%.*s", (int)strcspn(k + 15, "\r\n"), k + 15);
            int on = !strcmp(v, update_channels[UPDATE_BETA]) || !strcmp(v, update_channels[UPDATE_RELEASE]);
            if (on && update_channel(-1) == UPDATE_OFF && (why[0] || update_probe(why, cap))) *k = '#';
        }
        p += l + (p[l] == '\n');
    }
}

static void handle(int fd)
{
    struct req r; int e; uint8_t pub[32];
    struct timeval tv = { 10, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if ((e = read_request(fd, &r))) { if (e == -2) respond_json(fd, 413, "{\"error\":\"too large\"}"); return; }
    char *q = strchr(r.path, '?'); if (q) *q = 0;       /* no query strings in what we serve */

    if (!strcmp(r.method, "OPTIONS")) respond_c(fd, strncmp(r.path, "/kiosk/", 7) != 0, 204, "text/plain", NULL, NULL, 0);
    else if (!strcmp(r.method, "GET") && !strncmp(r.path, "/kiosk/state", 12)) {
        /* what the screen shows, for the home app that draws it itself: /kiosk/state/<v> waits up to 25 s for a
         * version other than v (a long poll: words appear at once), /kiosk/state answers now */
        if (!from_loopback(fd) || !kiosk_peer_ok(fd)) respond_c(fd, 0, 404, "application/json", NULL, "{}", 2);
        else {
            if (r.path[12] == '/') { char *e; unsigned long v = strtoul(r.path + 13, &e, 10); if (!*e) kiosk_state_wait((unsigned)v, 25000); }
            char o[8000]; size_t n = kiosk_state_json(o, sizeof o);
            respond_c(fd, 0, 200, "application/json", NULL, o, n < sizeof o ? n : sizeof o - 1);
        }
    }
    else if (!strcmp(r.method, "GET") && (!strcmp(r.path, "/kiosk/ring/snooze") || !strcmp(r.path, "/kiosk/ring/stop"))) {
        /* the home app draws the ring (its claim) and the finger is on it: a tap snoozes an alarm (stops a timer), a
         * hold stops (core_ring_tap).  Its uid only, as the config */
        if (!from_loopback(fd) || !kiosk_peer_ok(fd)) respond_c(fd, 0, 404, "application/json", NULL, "{}", 2);
        else { core_ring_tap(!strcmp(r.path + 12, "stop")); respond_c(fd, 0, 200, "application/json", NULL, "{}", 2); }
    }
    else if (!strcmp(r.method, "GET") && !strncmp(r.path, "/kiosk/config", 13)) {
        /* the token's way to the app: only from this Echo, and with no CORS headers: the pages the app shows run here
         * and must not read it */
        if (!from_loopback(fd) || !kiosk_peer_ok(fd)) respond_c(fd, 0, 404, "application/json", NULL, "{}", 2);
        else {
            const char *t = r.path + 13;
            if (!strncmp(t, "/front", 6)) kiosk_polled(1, kiosk_draws_parse(t + 6)); else if (!strncmp(t, "/back", 5)) kiosk_polled(0, 0);
            size_t n; char *j = kiosk_json(1, &n);
            if (j) { respond_c(fd, 0, 200, "application/json", NULL, j, n); free(j); } else respond_c(fd, 0, 503, "application/json", NULL, "{}", 2);
        }
    }
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
        char err[2048], o[2800], ej[2200], why[300], wj[340]; int applied;
        if (!signed_ok(&r, 1)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); free(r.body); return; }
        updates_gate(r.body, why, sizeof why);
        pthread_mutex_lock(&core_lock); applied = settings_apply_text(r.body, err, sizeof err); pthread_mutex_unlock(&core_lock);
        if (why[0]) { size_t n = strlen(err); snprintf(err + n, sizeof err - n, "online_updates: GitHub cannot be reached from this Echo (%s), so they stay off\n", why); }
        jesc(ej, sizeof ej, err); jesc(wj, sizeof wj, why);
        /* github_unreachable: the page's warning under "Online updates" */
        snprintf(o, sizeof o, "{\"applied\":%d,\"errors\":\"%s\",\"github_unreachable\":\"%s\"}", applied, ej, wj);
        fprintf(stderr, "web: %d setting%s changed%s%s", applied, applied == 1 ? "" : "s", err[0] ? ", refused: " : "\n", err);
        respond_sjson(fd, &r, 200, o);
    }
    else if (!strcmp(r.method, "POST") && !strcmp(r.path, "/api/identify")) {
        if (!signed_ok(&r, 1)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); free(r.body); return; }
        core_identify();
        respond_sjson(fd, &r, 200, "{\"identify\":true}");
    }
    else if (!strcmp(r.method, "POST") && !strcmp(r.path, "/api/dropin")) {       /* body: the node to drop in on, or "end" */
        char e[120], t[64], o[200], ej[160];
        if (!signed_ok(&r, 1)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); free(r.body); return; }
        snprintf(t, sizeof t, "%.*s", (int)(r.blen < sizeof t - 1 ? r.blen : sizeof t - 1), r.body ? r.body : "");
        t[strcspn(t, "\r\n")] = 0;
        if (!dropin_running()) respond_sjson(fd, &r, 409, "{\"error\":\"Drop In is not running on this Echo\"}");
        else if (!strcmp(t, "end")) { dropin_hangup("settings page"); respond_sjson(fd, &r, 200, "{\"dropin\":\"ended\"}"); }
        else if (dropin_call(t, e, sizeof e)) { jesc(ej, sizeof ej, e); snprintf(o, sizeof o, "{\"error\":\"%s\"}", ej); respond_sjson(fd, &r, 409, o); }
        else respond_sjson(fd, &r, 200, "{\"dropin\":\"calling\"}");
    }
    else if (!strcmp(r.method, "POST") && !strcmp(r.path, "/api/reset")) {       /* body "reset": not by a stray request */
        if (!signed_ok(&r, 1)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); free(r.body); return; }
        if (!r.body || r.blen != 5 || memcmp(r.body, "reset", 5)) respond_sjson(fd, &r, 400, "{\"error\":\"say reset\"}");
        else { respond_sjson(fd, &r, 200, "{\"reset\":true}"); core_reset("settings page"); }
    }
    else if (!strcmp(r.method, "POST") && !strcmp(r.path, "/api/reboot")) {      /* body "reboot", as reset's */
        if (!signed_ok(&r, 1)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); free(r.body); return; }
        if (!r.body || r.blen != 6 || memcmp(r.body, "reboot", 6)) respond_sjson(fd, &r, 400, "{\"error\":\"say reboot\"}");
        else { respond_sjson(fd, &r, 200, "{\"reboot\":true}"); core_reboot("settings page"); }
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
    else if (!strncmp(r.path, "/api/ring", 9)) ring_api(fd, &r);
    else if (!strncmp(r.path, "/api/wifi", 9)) wifi_api(fd, &r);
    else if (!strcmp(r.path, "/api/mass")) mass_api(fd, &r);
    else if (!strcmp(r.path, "/api/screen")) screen_api(fd, &r);
    else if (!strncmp(r.path, "/api/timer-sound", 16)) timer_sound_api(fd, &r);
    else if (!strncmp(r.path, "/api/sound/", 11)) sound_api(fd, &r);
    else if (!strncmp(r.path, "/api/sound-pack", 15)) sound_pack_api(fd, &r);
    else if (!strncmp(r.path, "/api/alarms", 11)) alarms_api(fd, &r);
    else if (!strcmp(r.path, "/api/kiosk")) kiosk_api(fd, &r);
    else if (!strcmp(r.path, "/api/apps") || !strncmp(r.path, "/api/apps/", 10)) apps_api(fd, &r);
    else if (!strcmp(r.method, "GET") && !strncmp(r.path, "/api/log/", 9)) log_api(fd, &r);
    else if (!strcmp(r.method, "GET") && !strcmp(r.path, "/api/camera/stream")) camera_api(fd, &r);
    else if (!strcmp(r.method, "GET") && !strcmp(r.path, "/api/call")) call_api(fd, &r);
    else if (!strcmp(r.method, "GET") && !strcmp(r.path, "/api/camera")) {     /* what the integration's entity shows */
        char o[128];
        if (!signed_ok(&r, 0)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); free(r.body); return; }
        camera_json(o, sizeof o); respond_sjson(fd, &r, 200, o);
    }
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

/* A login nobody asks about any more runs out here.  login_expire ran only when a browser asked or the button was
 * pressed: a page closed while it waited (an iPhone at 00:43 on an Echo 2, 2026-10-25) left the ring's
 * authenticated_setup_mode on until the next morning, as nothing asked again */
static void *expiry(void *arg)
{
    (void)arg;
    for (;;) {
        sleep(1);
        pthread_mutex_lock(&lk); login_expire(now_ms()); pthread_mutex_unlock(&lk);
    }
    return NULL;
}

int web_start(int p, const struct web_hooks *h)
{
    pthread_t t; uint8_t id[8];
    const char *e = getenv("HASSMIC_LOGIN_MS");
    if (e && atoi(e) > 0) LOGIN_MS = atoi(e);
    ws_random(id, sizeof id); hex(start_id, id, sizeof id);
    hooks = h; port = p;
    pthread_mutex_lock(&lk);
    int r = load_key();
    if (!r) load_clients();
    pthread_mutex_unlock(&lk);
    if (r) return -1;
    if (!pthread_create(&t, NULL, expiry, NULL)) pthread_detach(t);
    return pthread_create(&t, NULL, listener, NULL) ? -1 : (pthread_detach(t), 0);
}
