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
 *   Limits: someone who can change traffic (not only read it) can change the page itself, as with any plain HTTP
 *   page; and the Echo's public key comes from GET /api/hello unsigned.
 *
 * The page may come from another Echo: answers allow any origin (CORS), as the signature, not a cookie, is what counts.
 */
#include "web.h"
#include "core.h"
#include "board.h"
#include "hash.h"
#include "netio.h"
#include "settings.h"
#include "ws.h"
#include "../third_party/monocypher.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
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
#define MAX_CLIENTS 16
#define MAX_BODY    16384
#define LOGIN_MS    60000           /* a login waits this long for the button */

static const struct web_hooks *hooks;
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;  /* everything below; never held while taking core_lock */
static uint8_t sk[32], pk[32];
static struct client { uint8_t pub[32]; unsigned long long ctr; char label[64]; } clients[MAX_CLIENTS];
static int nclients;
static struct { uint8_t pub[32]; char label[64]; long long at; int state; } login;      /* state: 0 none, 1 waits, 2 approved, 3 refused */
static atomic_int nconn;

static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static const char *state_dir(void) { const char *e = getenv("HASSMIC_STATE"); return e ? e : "/data/local/hassmic/state"; }

static void hex(char *out, const uint8_t *b, size_t n) { for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]); }
static int unhex(uint8_t *out, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) { unsigned v; if (!isxdigit((unsigned char)s[2 * i]) || !isxdigit((unsigned char)s[2 * i + 1]) || sscanf(s + 2 * i, "%2x", &v) != 1) return -1; out[i] = (uint8_t)v; }
    return s[2 * n] ? -1 : 0;
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

static void login_expire(long long now)     /* lk held */
{
    if (login.state == 1 && now - login.at > LOGIN_MS) {
        login.state = 3; fprintf(stderr, "web: login ran out without the button\n");
        if (hooks->attention) hooks->attention(0);
        if (hooks->approved) hooks->approved(0);
    }
}

int web_approve(void)
{
    int r = 0;
    pthread_mutex_lock(&lk);
    login_expire(now_ms());
    if (login.state == 1) {
        if (nclients == MAX_CLIENTS) { memmove(clients, clients + 1, sizeof clients[0] * (MAX_CLIENTS - 1)); nclients--; }   /* the oldest goes */
        struct client *k = &clients[nclients++];
        memcpy(k->pub, login.pub, 32); k->ctr = 0; snprintf(k->label, sizeof k->label, "%s", login.label);
        save_clients();
        login.state = 2; r = 1;
        fprintf(stderr, "web: login approved (%s)\n", login.label);
        if (hooks->attention) hooks->attention(0);
        if (hooks->approved) hooks->approved(1);
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
    else if (login.state && !memcmp(login.pub, pub, 32)) r = login.state == 1 ? "waiting" : login.state == 2 ? "approved" : "refused";
    else if (login.state == 1) {                /* two at once: neither */
        login.state = 3; r = "refused";
        fprintf(stderr, "web: two logins at once, both refused\n");
        if (hooks->attention) hooks->attention(0);
        if (hooks->approved) hooks->approved(0);
    } else {
        memcpy(login.pub, pub, 32); snprintf(login.label, sizeof login.label, "%s", label); login.at = now; login.state = 1;
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
    char h[512];
    const char *msg = code == 200 ? "OK" : code == 204 ? "No Content" : code == 400 ? "Bad Request" : code == 401 ? "Unauthorized"
                    : code == 404 ? "Not Found" : code == 413 ? "Payload Too Large" : code == 503 ? "Service Unavailable" : "Error";
    int n = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\n"
                     "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Headers: X-HM-Pub, X-HM-Ctr, X-HM-Mac, Content-Type\r\n"
                     "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\nAccess-Control-Allow-Private-Network: true\r\n%s\r\n",
                     code, msg, type, len, extra ? extra : "");
    if (send_all(fd, h, (size_t)n) == 0 && len) send_all(fd, body, len);
}

static void respond_json(int fd, int code, const char *json) { respond(fd, code, "application/json", NULL, json, strlen(json)); }

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
    if (cl < 0 || cl > MAX_BODY) return -2;
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

static size_t hello_json(char *o, size_t cap)
{
    char p[65], name[160], node[160];
    hex(p, pk, 32); jesc(name, sizeof name, core_name); jesc(node, sizeof node, core_node_name());
    return (size_t)snprintf(o, cap, "{\"name\":\"%s\",\"node\":\"%s\",\"model\":\"%s\",\"codename\":\"%s\",\"version\":\"%s\",\"pub\":\"%s\"}",
                            name, node, board.model, board.codename, VERSION, p);
}

/* lock order: core_lock first (settings), lk only inside the helpers above */
static void state_json(int fd, const uint8_t me[32])
{
    size_t cap = 32768, n = 0; char *o = malloc(cap), v[300];
    if (!o) { respond_json(fd, 503, "{}"); return; }
    n += (size_t)snprintf(o + n, cap - n, "{\"device\":");
    n += hello_json(o + n, cap - n);
    pthread_mutex_lock(&core_lock);
    n += (size_t)snprintf(o + n, cap - n, ",\"ha\":%s,\"settings\":[", core_ha_linked() ? "true" : "false");
    for (int i = 0, k = settings_count(); i < k && n < cap - 600; i++) {
        const struct setting *s = settings_at(i); const char *const *names; int nc = settings_choices(s, &names);
        n += (size_t)snprintf(o + n, cap - n, "%s{\"name\":\"%s\",\"label\":\"%s\",\"group\":\"%s\",\"type\":\"%s\",\"value\":%d,\"min\":%d,\"max\":%d,\"export\":%s,\"unit\":\"%s\",\"choices\":[",
                              i ? "," : "", s->name, s->label, s->group, s->type == S_BOOL ? "bool" : s->type == S_INT ? "int" : "choice",
                              settings_get(s), s->min, s->max, s->export_ ? "true" : "false", s->unit ? s->unit : "");
        for (int c = 0; c < nc && n < cap - 200; c++) { jesc(v, sizeof v, names[c]); n += (size_t)snprintf(o + n, cap - n, "%s\"%s\"", c ? "," : "", v); }
        n += (size_t)snprintf(o + n, cap - n, "]}");
    }
    /* what does not work, and why */
    n += (size_t)snprintf(o + n, cap - n, "],\"warnings\":[");
    int w = 0;
    if (!core_ha_linked()) n += (size_t)snprintf(o + n, cap - n, "%s\"Home Assistant is not connected\"", w++ ? "," : "");
    if (core_local_wake && core_whispered() == -2)
        n += (size_t)snprintf(o + n, cap - n, "%s\"Whisper detection: no model on this Echo (scripts/artifacts.sh installs it)\"", w++ ? "," : "");
    { const struct core_wake_word *l; if (core_local_wake && core_wake_words(&l) <= 1)
        n += (size_t)snprintf(o + n, cap - n, "%s\"Wake words: only Alexa, Amazon's others are not installed (scripts/artifacts.sh)\"", w++ ? "," : ""); }
    pthread_mutex_unlock(&core_lock);
    n += (size_t)snprintf(o + n, cap - n, "],\"clients\":[");
    pthread_mutex_lock(&lk);
    for (int i = 0; i < nclients && n < cap - 300; i++) {
        char h[65]; hex(h, clients[i].pub, 32); jesc(v, sizeof v, clients[i].label);
        n += (size_t)snprintf(o + n, cap - n, "%s{\"pub\":\"%s\",\"label\":\"%s\",\"me\":%s}", i ? "," : "", h, v, memcmp(clients[i].pub, me, 32) ? "false" : "true");
    }
    pthread_mutex_unlock(&lk);
    n += (size_t)snprintf(o + n, cap - n, "]}");
    respond(fd, 200, "application/json", NULL, o, n < cap ? n : cap);
    free(o);
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
    else if (!strcmp(r.path, "/api/hello")) { char o[600]; hello_json(o, sizeof o); respond_json(fd, 200, o); }
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
        else state_json(fd, pub);
    }
    else if (!strcmp(r.method, "GET") && !strcmp(r.path, "/api/export")) {
        char t[4096], head[200]; size_t n;
        if (!signed_ok(&r, 0)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); free(r.body); return; }
        n = (size_t)snprintf(t, sizeof t, "# hassmic settings, exported from %s (%s %s)\n# not in it: the Echo's name, keys, pairings\n", core_name, board.model, VERSION);
        pthread_mutex_lock(&core_lock); n += settings_text(t + n, sizeof t - n, 1); pthread_mutex_unlock(&core_lock);
        snprintf(head, sizeof head, "Content-Disposition: attachment; filename=\"hassmic-settings.conf\"\r\n");
        respond(fd, 200, "text/plain; charset=utf-8", head, t, n);
    }
    else if (!strcmp(r.method, "POST") && !strcmp(r.path, "/api/set")) {
        /* body: "name=value" lines, one setting or a whole export */
        char err[2048], o[2400], ej[2200]; int applied;
        if (!signed_ok(&r, 1)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); free(r.body); return; }
        pthread_mutex_lock(&core_lock); applied = settings_apply_text(r.body, err, sizeof err); pthread_mutex_unlock(&core_lock);
        jesc(ej, sizeof ej, err);
        snprintf(o, sizeof o, "{\"applied\":%d,\"errors\":\"%s\"}", applied, ej);
        fprintf(stderr, "web: %d setting%s changed%s%s", applied, applied == 1 ? "" : "s", err[0] ? ", refused: " : "\n", err);
        respond_json(fd, 200, o);
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
        respond_json(fd, 200, found ? "{\"revoked\":true}" : "{\"revoked\":false}");
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
