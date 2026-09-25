/*
 * Wake word arbitration, what Amazon calls ESP (Echo Spatial Perception): when several Echos hear the wake word, only
 * the one that heard it best answers.  Stock decides in the cloud; here the Echos decide among themselves on the LAN, so
 * only hassmic Echos take part.  Home Assistant's own rule still covers every other satellite: the first wake-up per
 * phrase in 2 s wins, later ones get "duplicate_wake_up_detected" (assist_pipeline, WAKE_WORD_COOLDOWN).
 *
 * Round (per keyword: an Echo listening for "Echo" and one for "Alexa" never compete): main.c scores a detection
 * (signal to noise of the wake word in the processed mic stream) and it is broadcast as a claim; WINDOW_MS later the
 * Echo answers only if no claim of the last LOOKBACK_MS beats it.  An Echo in a conversation or ringing claims with
 * priority: the next wake word is for it.  The winner broadcasts that it answers, so an Echo whose detection comes late
 * does not answer as well.  With no other member heard from lately there is no round and no delay.
 *
 * Network: the members share a key K; claims and beacons carry a MAC with it and a counter against replays (reserved in
 * blocks in the state file, so it keeps rising across restarts).  "Join arbitration network" (on by default) looks for
 * members first; with none after DISCOVER_MS the Echo makes up K and a random network id itself.  Two networks that
 * formed at the same time merge: the lower id wins.
 *
 * Who gets K: Home Assistant carries it.  A member that sees the beacon of an Echo outside its network (or in a younger
 * one) has Home Assistant run that Echo's own action, "esphome.<its node name>_arbitration_key", which Home Assistant
 * delivers only over the encrypted API link of the device it adopted under that node name.  The receiving Echo takes a
 * key from nowhere else, and only from a client holding the device's API key; the sending Echo needs "Allow the device
 * to perform Home Assistant actions" (Home Assistant raises a repair without it).  So the trust is the owner's: what
 * they adopted into Home Assistant and allowed to act.  K travels encrypted to the receiver's public key from its
 * beacon (X25519, BLAKE2b, XChaCha20-Poly1305), so neither Home Assistant's traces nor its logbook hold it readable; a
 * forged beacon (right node name, someone else's key) only gets the real Echo a key it cannot open.  The node name is
 * the ESPHome device name the Echo reports itself (from NAME in hassmic.conf), not the name given to it in Home
 * Assistant, so renaming the device or its entities there changes nothing.  Two Echos with the same NAME collide there.
 *   Limits: any Home Assistant admin or allowed ESPHome device can hand an Echo a key.  An Echo that leaves wipes K; K
 *   is not rotated when an Echo is removed.  Counters are only remembered in memory: right after a restart, one
 *   recorded packet per member can be replayed once (at worst one wake word lost).  The goal is that nobody on the
 *   network who is not in Home Assistant can silence an Echo or listen in on the rounds.
 *
 * UDP broadcast on one port (default 28930; the stock firewall admits inbound UDP 16384-32767), so everything reaches
 * every Echo in the subnet.  Wi-Fi drops broadcasts now and then (no retries on the air): claims go out twice, the
 * counter drops the copy.
 *
 *   beacon  "HMA1" 1  pub[32] net[8] nlen node [ctr[8] tag[16], in a network]
 *   claim   "HMA1" 4  net[8] id[8] ctr[8] kw[8] score[4] prio won tag[16]
 *   key (the action's "key" argument, base64)  sender pub[32] nonce[24] mac[16] enc(K)[32], "network": the id in hex
 * Little endian; id: first 8 bytes of the public key; kw: BLAKE2b of the keyword in lower case; tag: keyed BLAKE2b with
 * a key derived from K.
 */
#include "arb.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "hash.h"
#include "ws.h"
#include "../third_party/monocypher.h"

#define WINDOW_MS    200        /* wait for the others' claims; Wi-Fi broadcast on a quiet LAN arrives within a few ms */
#define LOOKBACK_MS  1000       /* claims this much older than our own detection still count: the engines report late by different amounts */
#define BEACON_MS    30000
#define LONER_MS     10000      /* beacon interval outside a network; members answer such a beacon at once */
#define PEER_TTL_MS  75000      /* a member missing two beacons in a row no longer counts */
#define DISCOVER_MS  5000       /* no member heard in this time: start a network */
#define PUSH_MS      30000      /* hand our key to the same Echo at most this often */
#define CTR_BLOCK    4096
#define NPEER  16
#define NCLAIM 16
#define NLEN   64

enum { T_BEACON = 1, T_CLAIM = 4 };

static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER; /* everything below; taken after core_lock, never before it */
static int sock = -1, running, join = 1;
static struct sockaddr_in dest;
static const struct arb_hooks *hooks;
static char node[NLEN];
static uint8_t sk[32], pk[32];
static int in_net;
static uint64_t net_id;
static uint8_t net_key[32], mac_key[32];
static uint64_t ctr, ctr_saved;
static long long join_since, beacon_at, answer_at;
static int reported_peers = -1;
static atomic_int notify;

static struct peer { uint8_t id[8]; uint64_t ctr; long long seen; } peers[NPEER];
static struct claim { uint8_t id[8], kw[8]; int score, prio, won; long long at; } claims[NCLAIM];
static unsigned claim_next;
static struct cand { uint8_t pub[32]; char node[NLEN]; uint64_t net; long long seen, pushed; } cands[NPEER];   /* Echos outside our network */
static struct push { char node[NLEN], net[20], key[160]; } pushes[NPEER];     /* for Home Assistant, sent outside the lock */
static int npush;
static struct { uint8_t kw[8]; int score, prio; long long at; } round_;

static long long now_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int ago(long long t, long long now, long long ms) { return t && now - t < ms; }     /* 0 = never */

/* ---------------------------------------------------------------- state files */

static const char *state_dir(void) { const char *e = getenv("HASSMIC_STATE"); return e ? e : "/data/local/hassmic/state"; }

static void save(void)
{
    char path[300], tmp[310], k64[48]; FILE *f; int fd;
    snprintf(path, sizeof path, "%s/arbitration", state_dir()); snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if ((fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600)) < 0 || !(f = fdopen(fd, "w"))) {
        if (fd >= 0) close(fd);
        fprintf(stderr, "arbitration: cannot write %s\n", tmp); return;
    }
    fprintf(f, "join %d\nctr %llu\n", join, (unsigned long long)ctr_saved);
    if (in_net) { b64_encode(net_key, 32, k64, 0, 1); fprintf(f, "net %016llx %s\n", (unsigned long long)net_id, k64); crypto_wipe(k64, sizeof k64); }
    if (fclose(f) || rename(tmp, path)) { unlink(tmp); fprintf(stderr, "arbitration: cannot write %s\n", path); }
}

static void derive(void) { crypto_blake2b_keyed(mac_key, 32, net_key, 32, (const uint8_t *)"hassmic arbitration mac", 23); }

static void load(void)
{
    char path[300], line[160], k64[64]; unsigned long long v; int j; FILE *f;
    snprintf(path, sizeof path, "%s/arbitration", state_dir());
    if (!(f = fopen(path, "r"))) return;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "join %d", &j) == 1) join = j != 0;
        else if (sscanf(line, "ctr %llu", &v) == 1) ctr_saved = v;
        else if (sscanf(line, "net %llx %63s", &v, k64) == 2 && v && b64_decode(k64, strlen(k64), net_key, 32) == 32) { net_id = v; in_net = 1; derive(); }
    }
    crypto_wipe(line, sizeof line); crypto_wipe(k64, sizeof k64);
    fclose(f);
}

static int identity(void)
{
    char path[300], t[64] = ""; FILE *f; int ok = 0;
    snprintf(path, sizeof path, "%s/arb_key", state_dir());
    if ((f = fopen(path, "r"))) { ok = fgets(t, sizeof t, f) && b64_decode(t, strcspn(t, "\r\n"), sk, 32) == 32; fclose(f); }
    if (!ok) {
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        ws_random(sk, 32); b64_encode(sk, 32, t, 0, 1); strcat(t, "\n");
        if (fd < 0 || write(fd, t, strlen(t)) != (ssize_t)strlen(t)) { if (fd >= 0) close(fd); fprintf(stderr, "arbitration: cannot write %s\n", path); return -1; }
        close(fd);
    }
    crypto_wipe(t, sizeof t);
    crypto_x25519_public_key(pk, sk);
    return 0;
}

static uint64_t next_ctr(void)
{
    if (++ctr >= ctr_saved) { ctr_saved = ctr + CTR_BLOCK; save(); }       /* on disk before it is used */
    return ctr;
}

/* ---------------------------------------------------------------- packets */

struct pkt { uint8_t p[256]; size_t n; };
static void put(struct pkt *b, const void *d, size_t n) { if (b->n + n <= sizeof b->p) { memcpy(b->p + b->n, d, n); b->n += n; } }
static void put64(struct pkt *b, uint64_t v) { uint8_t x[8]; for (int i = 0; i < 8; i++) x[i] = (uint8_t)(v >> 8 * i); put(b, x, 8); }
static void head(struct pkt *b, int type) { uint8_t t = (uint8_t)type; b->n = 0; put(b, "HMA1", 4); put(b, &t, 1); }
static void tag(struct pkt *b) { uint8_t t[16]; crypto_blake2b_keyed(t, 16, mac_key, 32, b->p, b->n); put(b, t, 16); }

static int tag_ok(const uint8_t *p, size_t n)
{
    uint8_t t[16];
    if (n < 16) return 0;
    crypto_blake2b_keyed(t, 16, mac_key, 32, p, n - 16);
    return !crypto_verify16(t, p + n - 16);
}

static void send_pkt(const struct pkt *b)
{
    static int last_err;
    if (sendto(sock, b->p, b->n, 0, (const struct sockaddr *)&dest, sizeof dest) >= 0) { last_err = 0; return; }
    if (errno != last_err) fprintf(stderr, "arbitration: send: %s\n", strerror(errno));      /* once, not every beacon while Wi-Fi is down */
    last_err = errno;
}

struct rd { const uint8_t *p, *end; int bad; };
static const uint8_t *take(struct rd *r, size_t n)
{
    const uint8_t *q = r->p;
    if (r->bad || (size_t)(r->end - r->p) < n) { r->bad = 1; return NULL; }
    r->p += n;
    return q;
}
static uint64_t get64(struct rd *r) { const uint8_t *q = take(r, 8); uint64_t v = 0; if (q) for (int i = 7; i >= 0; i--) v = v << 8 | q[i]; return v; }

/* An ESPHome node name as hassmic makes them (proto_esphome.c): lower case, digits, '-' */
static int valid_node(const char *s)
{
    if (!*s) return 0;
    for (; *s; s++) if (!(islower((unsigned char)*s) || isdigit((unsigned char)*s) || *s == '-')) return 0;
    return 1;
}

static void get_node(struct rd *r, char out[NLEN])
{
    const uint8_t *l = take(r, 1), *q;
    out[0] = 0;
    if (!l || *l >= NLEN) { r->bad = 1; return; }
    if ((q = take(r, *l))) { memcpy(out, q, *l); out[*l] = 0; }
    if (!r->bad && !valid_node(out)) r->bad = 1;
}

/* ---------------------------------------------------------------- peers, candidates (lk held) */

static struct peer *peer(const uint8_t id[8])
{
    struct peer *old = &peers[0];
    for (int i = 0; i < NPEER; i++) {
        if (peers[i].seen && !memcmp(peers[i].id, id, 8)) return &peers[i];
        if (peers[i].seen < old->seen) old = &peers[i];
    }
    memset(old, 0, sizeof *old); memcpy(old->id, id, 8);
    return old;
}

/* A packet that authenticated: 1 if its counter is new (not a replay, not the second copy) */
static int fresh(const uint8_t id[8], uint64_t c, long long now)
{
    struct peer *p = peer(id);
    if (c <= p->ctr) { if (!p->seen) memset(p, 0, sizeof *p); return 0; }
    p->ctr = c; p->seen = now;
    return 1;
}

static int count_peers(long long now)
{
    int n = 0;
    for (int i = 0; i < NPEER; i++) n += ago(peers[i].seen, now, PEER_TTL_MS);
    return n;
}

static struct cand *cand(const uint8_t pub[32])
{
    struct cand *old = &cands[0];
    for (int i = 0; i < NPEER; i++) {
        if (cands[i].seen && !memcmp(cands[i].pub, pub, 32)) return &cands[i];
        if (cands[i].seen < old->seen) old = &cands[i];
    }
    memset(old, 0, sizeof *old); memcpy(old->pub, pub, 32);
    return old;
}

/* ---------------------------------------------------------------- network */

static void beacon(void)
{
    struct pkt b; uint8_t l = (uint8_t)strlen(node);
    head(&b, T_BEACON); put(&b, pk, 32); put64(&b, in_net ? net_id : 0); put(&b, &l, 1); put(&b, node, l);
    if (in_net) { put64(&b, next_ctr()); tag(&b); }
    send_pkt(&b);
}

static void adopt(const uint8_t k[32], uint64_t id, const char *how)
{
    memcpy(net_key, k, 32); net_id = id; in_net = 1; derive();
    memset(peers, 0, sizeof peers); memset(claims, 0, sizeof claims);
    save();
    fprintf(stderr, "arbitration: %s network %016llx\n", how, (unsigned long long)id);
    beacon_at = 0; atomic_store(&notify, 1);
}

static void create(void)
{
    uint8_t k[32]; uint64_t id = 0;
    ws_random(k, 32);
    while (!id) ws_random(&id, sizeof id);
    adopt(k, id, "no other Echo found, started");
    crypto_wipe(k, sizeof k);
}

/* The key that wraps K between two Echos: X25519 between their identity keys, bound to both and to the network */
static int wrap_key(uint8_t key[32], const uint8_t their[32], const uint8_t from[32], const uint8_t to[32], uint64_t net)
{
    uint8_t shared[32], zero[32] = { 0 }, msg[16 + 32 + 32 + 8];
    crypto_x25519(shared, sk, their);
    if (!memcmp(shared, zero, 32)) return -1;       /* a low-order point: no secret at all */
    memcpy(msg, "hassmic arb key\0", 16); memcpy(msg + 16, from, 32); memcpy(msg + 48, to, 32);
    for (int i = 0; i < 8; i++) msg[80 + i] = (uint8_t)(net >> 8 * i);
    crypto_blake2b_keyed(key, 32, shared, 32, msg, sizeof msg);
    crypto_wipe(shared, sizeof shared);
    return 0;
}

/* Hand K to an Echo outside our network, or in a younger one, through Home Assistant */
static void push(struct cand *c, long long now)
{
    uint8_t key[32], blob[32 + 24 + 16 + 32];
    if (!in_net || !c->node[0] || (c->net && c->net <= net_id) || ago(c->pushed, now, PUSH_MS) || npush == NPEER) return;
    if (wrap_key(key, c->pub, pk, c->pub, net_id)) return;
    c->pushed = now;
    memcpy(blob, pk, 32); ws_random(blob + 32, 24);
    crypto_aead_lock(blob + 72, blob + 56, key, blob + 32, blob, 32, net_key, 32);    /* the sender's key as additional data */
    crypto_wipe(key, sizeof key);
    struct push *q = &pushes[npush++];
    snprintf(q->node, NLEN, "%s", c->node); snprintf(q->net, sizeof q->net, "%016llx", (unsigned long long)net_id);
    b64_encode(blob, sizeof blob, q->key, 0, 1);
}

static void on_packet(const uint8_t *p, size_t n, long long now)
{
    struct rd r = { p, p + n, 0 }; const uint8_t *m = take(&r, 5);
    if (!join || !m || memcmp(m, "HMA1", 4)) return;
    switch (m[4]) {
    case T_BEACON: {
        const uint8_t *pub = take(&r, 32); uint64_t net = get64(&r); char nd[NLEN];
        get_node(&r, nd);
        if (r.bad || !memcmp(pub, pk, 32)) return;
        if (in_net && net == net_id) {                  /* a member */
            uint64_t c = get64(&r); int known = 0;
            if (r.bad || r.end - r.p != 16 || !tag_ok(p, n)) return;
            for (int i = 0; i < NPEER; i++) {
                known |= !memcmp(peers[i].id, pub, 8) && ago(peers[i].seen, now, PEER_TTL_MS);
                if (cands[i].seen && !memcmp(cands[i].pub, pub, 32)) cands[i].seen = 0;    /* in our network now: nothing more to hand it */
            }
            /* one we did not count yet (it just joined, or we did): answer, or it would not count us until our next beacon.
             * Not rate limited: only a holder of K gets here, once per member */
            if (fresh(pub, c, now) && !known) beacon();
            return;
        }
        struct cand *c = cand(pub);
        snprintf(c->node, NLEN, "%s", nd); c->net = net; c->seen = now;
        if (in_net && !net && !ago(answer_at, now, 1000)) { answer_at = now; beacon(); }       /* someone looking: here we are */
        push(c, now);
    } break;
    case T_CLAIM: {
        uint64_t net = get64(&r); const uint8_t *id = take(&r, 8); uint64_t c = get64(&r); const uint8_t *kw = take(&r, 8), *s = take(&r, 4), *pw = take(&r, 2);
        if (r.bad || r.end - r.p != 16 || !in_net || net != net_id || !memcmp(id, pk, 8) || !tag_ok(p, n) || !fresh(id, c, now)) return;
        struct claim *k = &claims[claim_next++ % NCLAIM];
        memcpy(k->id, id, 8); memcpy(k->kw, kw, 8);
        k->score = (int32_t)((uint32_t)s[0] | (uint32_t)s[1] << 8 | (uint32_t)s[2] << 16 | (uint32_t)s[3] << 24);
        k->prio = pw[0]; k->won = pw[1]; k->at = now;
        fprintf(stderr, "arbitration: %02x%02x%02x%02x %s, score %d%s\n", id[0], id[1], id[2], id[3], k->won ? "answers" : "heard it",
                k->score, k->prio ? " (in a conversation)" : "");
    } break;
    }
}

static void *loop(void *arg)
{
    uint8_t buf[512]; struct pollfd pf = { sock, POLLIN, 0 };
    (void)arg;
    for (;;) {
        if (poll(&pf, 1, 100) > 0) {
            ssize_t n = recv(sock, buf, sizeof buf, 0);
            if (n > 0) { pthread_mutex_lock(&lk); on_packet(buf, (size_t)n, now_ms()); pthread_mutex_unlock(&lk); }
        }
        struct push q[NPEER]; int nq; long long now = now_ms();
        pthread_mutex_lock(&lk);
        if (join) {
            if (!in_net) {
                int members = 0;
                for (int i = 0; i < NPEER; i++) members |= cands[i].net && ago(cands[i].seen, now, PEER_TTL_MS);
                if (!members && now - join_since > DISCOVER_MS) create();
                static long long hinted;
                if (members && now - join_since > 60000 && !ago(hinted, now, 600000)) {
                    hinted = now;
                    for (int i = 0; i < NPEER; i++) if (cands[i].net && ago(cands[i].seen, now, PEER_TTL_MS))
                        fprintf(stderr, "arbitration: %s has not handed us its network yet: it needs \"Allow the device to perform Home "
                                "Assistant actions\" in its ESPHome options\n", cands[i].node);
                }
            }
            for (int i = 0; i < NPEER; i++) if (ago(cands[i].seen, now, PEER_TTL_MS)) push(&cands[i], now);
            if (!ago(beacon_at, now, in_net ? BEACON_MS : LONER_MS)) { beacon_at = now; beacon(); }
        }
        nq = npush; memcpy(q, pushes, sizeof q[0] * (size_t)nq); npush = 0;
        int np = count_peers(now);
        if (np != reported_peers) { reported_peers = np; atomic_store(&notify, 1); }
        pthread_mutex_unlock(&lk);
        for (int i = 0; i < nq; i++) {
            int r = hooks->send_key(q[i].node, q[i].net, q[i].key);
            fprintf(stderr, "arbitration: %s network %s to %s through Home Assistant\n", r < 0 ? "cannot hand (no link)" : "handing", q[i].net, q[i].node);
        }
        crypto_wipe(q, sizeof q);
        if (atomic_exchange(&notify, 0) && hooks->changed) hooks->changed();
    }
    return NULL;
}

/* ---------------------------------------------------------------- interface */

void arb_key(const char *network, const char *key)
{
    uint8_t blob[32 + 24 + 16 + 32 + 4], k[32], wrap[32]; unsigned long long id = 0; char *end;
    id = strtoull(network, &end, 16);
    if (*end || !id || b64_decode(key, strlen(key), blob, sizeof blob) != 32 + 24 + 16 + 32) {
        fprintf(stderr, "arbitration: malformed key from Home Assistant, ignored\n"); return;
    }
    pthread_mutex_lock(&lk);
    if (!running || !join) fprintf(stderr, "arbitration: key from Home Assistant ignored (not joining)\n");
    else if (in_net && id >= net_id)
        fprintf(stderr, "arbitration: key for network %016llx ignored (%s)\n", id, id == net_id ? "already in it" : "ours is older");
    else if (!memcmp(blob, pk, 32) || wrap_key(wrap, blob, blob, pk, id)
             || crypto_aead_unlock(k, blob + 56, wrap, blob + 32, blob, 32, blob + 72, 32))
        fprintf(stderr, "arbitration: key from Home Assistant does not open with ours, ignored\n");
    else adopt(k, id, in_net ? "moved to the older" : "joined");
    crypto_wipe(k, sizeof k); crypto_wipe(wrap, sizeof wrap);
    pthread_mutex_unlock(&lk);
}

static void claim_pkt(struct pkt *b, int score, int prio, int won)
{
    uint8_t s[4] = { (uint8_t)score, (uint8_t)(score >> 8), (uint8_t)(score >> 16), (uint8_t)(score >> 24) }, pw[2] = { (uint8_t)prio, (uint8_t)won };
    head(b, T_CLAIM); put64(b, net_id); put(b, pk, 8); put64(b, next_ctr()); put(b, round_.kw, 8); put(b, s, 4); put(b, pw, 2); tag(b);
}

long long arb_claim(const char *keyword, int score, int prio)
{
    long long now = now_ms(), due = 0; struct pkt b; uint8_t low[64]; size_t n = 0;
    for (; keyword[n] && n < sizeof low; n++) low[n] = (uint8_t)tolower((unsigned char)keyword[n]);
    pthread_mutex_lock(&lk);
    crypto_blake2b(round_.kw, 8, low, n);
    if (running && join && in_net && count_peers(now)) {
        round_.score = score; round_.prio = prio; round_.at = now;
        claim_pkt(&b, score, prio, 0); send_pkt(&b); send_pkt(&b);
        due = now + WINDOW_MS;
    }
    pthread_mutex_unlock(&lk);
    fprintf(stderr, "arbitration: heard it, score %d%s%s\n", score, prio ? " (in a conversation)" : "", due ? "" : ", no other Echo to ask");
    return due;
}

int arb_decide(void)
{
    int win = 1; struct pkt b;
    pthread_mutex_lock(&lk);
    for (int i = 0; i < NCLAIM; i++) {
        struct claim *k = &claims[i];
        if (!k->at || k->at < round_.at - LOOKBACK_MS || memcmp(k->kw, round_.kw, 8)) continue;     /* another round, another keyword */
        if (k->won || k->prio > round_.prio || (k->prio == round_.prio && (k->score > round_.score
                                                                          || (k->score == round_.score && memcmp(k->id, pk, 8) > 0)))) win = 0;
    }
    if (win && in_net) { claim_pkt(&b, round_.score, round_.prio, 1); send_pkt(&b); send_pkt(&b); }
    pthread_mutex_unlock(&lk);
    fprintf(stderr, "arbitration: %s\n", win ? "this Echo answers" : "another Echo answers");
    return win;
}

int arb_join(int set)
{
    pthread_mutex_lock(&lk);
    if (set >= 0 && set != join) {
        join = set;
        if (!join) {
            if (in_net) fprintf(stderr, "arbitration: left network %016llx\n", (unsigned long long)net_id);
            in_net = 0; net_id = 0; crypto_wipe(net_key, sizeof net_key); crypto_wipe(mac_key, sizeof mac_key);
            memset(peers, 0, sizeof peers); memset(claims, 0, sizeof claims);
        } else {                                    /* start over: earlier refusals and waits no longer apply */
            memset(cands, 0, sizeof cands); join_since = now_ms(); beacon_at = 0;
            fprintf(stderr, "arbitration: looking for a network\n");
        }
        save(); atomic_store(&notify, 1);
    }
    int r = join;
    pthread_mutex_unlock(&lk);
    return r;
}

int arb_running(void) { return running; }
int arb_peers(void) { pthread_mutex_lock(&lk); int n = running ? count_peers(now_ms()) : 0; pthread_mutex_unlock(&lk); return n; }

int arb_start(int port, const char *nd, const struct arb_hooks *h)
{
    int on = 1; struct sockaddr_in a; const char *addr = getenv("HASSMIC_ARB_ADDR");   /* tests: 127.255.255.255 */
    pthread_t t;
    snprintf(node, sizeof node, "%s", nd); hooks = h;
    if (identity()) return -1;
    load();
    ctr = ctr_saved; ctr_saved += CTR_BLOCK; save();
    if ((sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)) < 0) { perror("arbitration: socket"); return -1; }
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
    memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&a, sizeof a)) { perror("arbitration: bind"); close(sock); sock = -1; return -1; }
    dest = a; dest.sin_addr.s_addr = addr ? inet_addr(addr) : htonl(INADDR_BROADCAST);
    join_since = now_ms(); running = 1;
    if (pthread_create(&t, NULL, loop, NULL)) { running = 0; return -1; }
    pthread_detach(t);
    fprintf(stderr, "arbitration: port %d as %s, %s\n", port, node, !join ? "not joining" : in_net ? "member of a network" : "looking for a network");
    return 0;
}
