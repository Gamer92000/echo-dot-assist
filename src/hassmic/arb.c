/*
 * Wake word arbitration, what Amazon calls ESP (Echo Spatial Perception): when several Echos hear the wake word, only
 * the one that heard it best answers.  Stock decides in the cloud; here the Echos decide among themselves on the LAN, so
 * only hassmic Echos take part.  Home Assistant's own rule still covers every other satellite: the first wake-up per
 * phrase in 2 s wins, later ones get "duplicate_wake_up_detected" (assist_pipeline, WAKE_WORD_COOLDOWN).
 *
 * Round (per keyword: an Echo listening for "Echo" and one for "Alexa" never compete): main.c scores a detection
 * (signal to noise of the wake word: the front end's own energies where it gives them, else taken from the processed mic
 * stream) and it is broadcast as a claim; WINDOW_MS later the
 * Echo answers only if no claim of the last LOOKBACK_MS beats it.  An Echo in a conversation or ringing claims with
 * priority: the next wake word is for it.  The winner broadcasts that it answers, so an Echo whose detection comes late
 * does not answer as well.  With no other member heard from lately there is no round and no delay.
 *
 * Network: the members share a key K; claims and beacons carry a MAC with it and a counter against replays (reserved in
 * blocks in the state file, so it keeps rising across restarts).  "Join arbitration network" (on by default) looks for
 * members first; with none after DISCOVER_MS the Echo makes up K and a random network id itself.  Two networks that
 * formed at the same time merge: the lower id wins.
 *
 * Who gets K, three ways; the first two go through Home Assistant, so the trust is the owner's: what they adopted.
 *   Handoff entity: every Echo shows its public key on a diagnostic text sensor, "Arbitration handoff".  HA does not
 *   tell a device its entity ids (they follow the device name in HA, which the user may have changed), so the Echo
 *   asks HA for the likely ones (sensor.<node>_arbitration_handoff, _2, _3, and further as long as HA answers for
 *   the last: another device holds it, up to as many as the subnet has hosts; once-requests, which need no
 *   permission) and the one showing its own key is its own; from then on it broadcasts that id (T_ENTITY).  A member asks HA for
 *   the entity a newcomer named, and only if HA shows the newcomer's beacon key there does it put K on its own entity,
 *   sealed to that key; the newcomer reads it there.  A forged beacon (or T_ENTITY) gets no offer: HA would have to
 *   show the forger's key on an adopted device's entity.  Without permission to act, and nothing to configure.
 *   Action: with no confirmed entity after ATTEST_WAIT_MS (older Echo, entity disabled, renamed device), the member
 *   has HA run the newcomer's own action, "esphome.<its node name>_arbitration_key", which HA delivers only over the
 *   encrypted API link of the device it adopted under that node name; the receiving Echo takes it only from a client
 *   holding the device's API key.  Needs "Allow the device to perform Home Assistant actions" on the member (HA raises
 *   a repair without it).
 *   Pairing (no Home Assistant at all): Volume up + Volume down held on both Echos.  For 2 min each broadcasts a pair
 *   request; a member hands K (T_GIVE) PAIR_SETTLE_MS after its own press, and only if exactly one Echo outside its
 *   network asked since 2 min before that press, and no Echo of an older network is pairing (that one gives).  Taking
 *   a key, by any way, ends the window.  An attacker who asks too only blocks it; one who asks alone gets K only if
 *   the owner pressed the member without pressing the newcomer first, or (one who keeps asking) pressed an Echo that
 *   is alone in a network of its own: that one hands over its own lone K, and can be silenced until it joins another.
 * K travels sealed to the receiver's public key from its beacon (X25519, BLAKE2b, XChaCha20-Poly1305), so neither HA's
 * states, traces nor logbook hold it readable.  The node name is the ESPHome device name the Echo reports itself (from
 * NAME in hassmic.conf), not the name given to it in HA.  Two Echos with the same NAME collide there.
 *   Limits: any HA admin (or, for the action, any allowed ESPHome device) can hand an Echo a key.  An Echo that leaves
 *   wipes K; K is not rotated when an Echo is removed.  Counters are only remembered in memory: right after a restart,
 *   one recorded packet per member can be replayed once (at worst one wake word lost).  The goal is that nobody on the
 *   network who is not in Home Assistant (and has not pressed the buttons) can silence an Echo or listen in on the rounds.
 *
 * UDP broadcast on one port (default 28930; the stock firewall admits inbound UDP 16384-32767), so everything reaches
 * every Echo in the subnet.  Wi-Fi drops broadcasts now and then (no retries on the air): claims go out twice, the
 * counter drops the copy.
 *
 *   beacon  "HMA1" 1  pub[32] net[8] nlen node [ctr[8] tag[16], in a network]
 *   claim   "HMA1" 4  net[8] id[8] ctr[8] kw[8] score[4] prio won tag[16]
 *   entity  "HMA1" 2  pub[32] elen entity       (unauthenticated: only names where HA shows pub)
 *   pair    "HMA1" 5  pub[32] net[8] nlen node
 *   give    "HMA1" 6  to[32] net[8] key[104]
 *   key (the action's "key" argument, base64)  sender pub[32] nonce[24] mac[16] enc(K)[32], "network": the id in hex
 *   handoff entity  "HMA1 <pub base64>[ <network hex> <to: first 8 bytes of its key, hex> <key base64>]"
 * Little endian; id: first 8 bytes of the public key; kw: BLAKE2b of the keyword in lower case; tag: keyed BLAKE2b with
 * a key derived from K.
 */
#include "arb.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
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
#define POLL_MS      3000       /* ask Home Assistant again for a peer's handoff entity */
#define SELF_POLL_MS 3000       /* ...and for the ids ours may have, until one shows our key; every minute after 20 tries */
#define ATTEST_WAIT_MS 20000    /* no entity confirmed for a newcomer by then: hand K through its action instead.  The
                                 * chain takes three polls once HA is linked; the action would raise a repair in HA for
                                 * every owner who did not allow it */
#define PAIR_MS      120000     /* the pairing gesture's window, and how far back a member looks for requests */
#define PAIR_SETTLE_MS 2000     /* a member waits this long after its press for a second requester */
#define NPEER  16
#define NCLAIM 16
#define NLEN   64
#define ELEN   96               /* "sensor." + a slug of the HA device name + "_arbitration_handoff_2" */
#define BLOB   (32 + 24 + 16 + 32)

enum { T_BEACON = 1, T_ENTITY = 2, T_CLAIM = 4, T_PAIR = 5, T_GIVE = 6 };

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

static struct peer { uint8_t id[8]; uint64_t ctr; long long seen; char node[NLEN]; uint32_t ip; } peers[NPEER];   /* node, ip: from its beacons */
static struct claim { uint8_t id[8], kw[8]; int score, prio, won; long long at; } claims[NCLAIM];
static unsigned claim_next;
static struct cand {                                    /* Echos outside our network */
    uint8_t pub[32]; char node[NLEN], ent[ELEN]; uint64_t net; int attested, offers;  /* HA shows pub on ent; offers made there */
    uint32_t ip;                                        /* where its beacons come from (network byte order), for the settings page */
    long long seen, first, pushed, polled;
} cands[NPEER];
static struct push { char node[NLEN], net[20], key[160]; } pushes[NPEER];     /* for Home Assistant, sent outside the lock */
static int npush;
static char polls[NPEER + 4][ELEN];              /* entities to ask Home Assistant for, outside the lock */
static int npoll;
static char self_ent[ELEN], offer[200];                 /* our handoff entity once HA showed our key there; what we offer on it */
static long long self_polled;
static int self_polls, self_n = 3, self_next = -1;     /* ids of ours to try: 3, or one past the last taken; one to ask now */
static struct preq { uint8_t pub[32]; char node[NLEN]; uint64_t net; long long seen; } preqs[NPEER];   /* pair requests heard */
static long long pair_at, pair_until, pair_sent, give_at;
static int pair_gave, give_left;
static atomic_int pair_event;
static uint8_t give_pkt[5 + 32 + 8 + BLOB];
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
    if (self_ent[0]) {                                  /* where Home Assistant shows our key: peers need not guess */
        uint8_t e = (uint8_t)strlen(self_ent);
        head(&b, T_ENTITY); put(&b, pk, 32); put(&b, &e, 1); put(&b, self_ent, e); send_pkt(&b);
    }
}

static void adopt(const uint8_t k[32], uint64_t id, const char *how, const char *from)
{
    memcpy(net_key, k, 32); net_id = id; in_net = 1; derive();
    memset(peers, 0, sizeof peers); memset(claims, 0, sizeof claims);
    offer[0] = 0;                                       /* sealed under the old network */
    save();
    fprintf(stderr, "arbitration: %s network %016llx%s%s%s\n", how, (unsigned long long)id, from ? " (key " : "", from ? from : "", from ? ")" : "");
    beacon_at = 0; atomic_store(&notify, 1);
}

static void create(void)
{
    uint8_t k[32]; uint64_t id = 0;
    ws_random(k, 32);
    while (!id) ws_random(&id, sizeof id);
    adopt(k, id, "no other Echo found, started", NULL);
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

/* K sealed to another Echo's public key: our key, nonce, MAC, enc(K) */
static int seal(uint8_t blob[BLOB], const uint8_t to[32])
{
    uint8_t key[32];
    if (wrap_key(key, to, pk, to, net_id)) return -1;
    memcpy(blob, pk, 32); ws_random(blob + 32, 24);
    crypto_aead_lock(blob + 72, blob + 56, key, blob + 32, blob, 32, net_key, 32);    /* the sender's key as additional data */
    crypto_wipe(key, sizeof key);
    return 0;
}

/* A sealed K for network id: 1 if it opened and we took it.  from: where it came from, for the log */
static int take_key(uint64_t id, const uint8_t blob[BLOB], const char *from)
{
    uint8_t k[32], wrap[32]; int ok = 0;
    if (!running || !join) fprintf(stderr, "arbitration: key %s ignored (not joining)\n", from);
    else if (in_net && id >= net_id)
        fprintf(stderr, "arbitration: key for network %016llx ignored (%s)\n", (unsigned long long)id, id == net_id ? "already in it" : "ours is older");
    else if (!memcmp(blob, pk, 32) || wrap_key(wrap, blob, blob, pk, id)
             || crypto_aead_unlock(k, blob + 56, wrap, blob + 32, blob, 32, blob + 72, 32))
        fprintf(stderr, "arbitration: key %s does not open with ours, ignored\n", from);
    else {
        adopt(k, id, in_net ? "moved to the older" : "joined", from); ok = 1;
        /* in, whichever way: the gesture is done.  Left running, an Echo that just joined would be a member pairing,
         * and hand K to whoever asked during its window (a forger's request, recorded while it was the one asking) */
        if (pair_until) { pair_until = 0; atomic_store(&pair_event, 2); }
    }
    crypto_wipe(k, sizeof k); crypto_wipe(wrap, sizeof wrap);
    return ok;
}

static void hex8(char out[17], const uint8_t *id) { for (int i = 0; i < 8; i++) snprintf(out + 2 * i, 3, "%02x", id[i]); }

/* Hand K to an Echo outside our network, or in a younger one: on our handoff entity if Home Assistant showed its key on
 * the entity it named, else (after ATTEST_WAIT_MS) through its action.  An offer it has not taken by the next push (it
 * reads every 3 s, so HA no longer shows it our entity) goes out through the action as well */
static void push(struct cand *c, long long now)
{
    uint8_t blob[BLOB]; char k64[160], to[17];
    if (!in_net || (c->net && c->net <= net_id) || ago(c->pushed, now, PUSH_MS)) return;
    if (!c->attested && (!c->node[0] || now - c->first < ATTEST_WAIT_MS || npush == NPEER)) return;
    if (seal(blob, c->pub)) return;
    c->pushed = now;
    b64_encode(blob, sizeof blob, k64, 0, 1);
    if (c->attested) {
        hex8(to, c->pub);
        snprintf(offer, sizeof offer, "%016llx %s %s", (unsigned long long)net_id, to, k64);
        fprintf(stderr, "arbitration: offering network %016llx to %s on our handoff entity\n", (unsigned long long)net_id, c->node);
        atomic_store(&notify, 1);
    }
    if ((!c->attested || c->offers++) && c->node[0] && npush < NPEER) {
        struct push *q = &pushes[npush++];
        snprintf(q->node, NLEN, "%s", c->node); snprintf(q->net, sizeof q->net, "%016llx", (unsigned long long)net_id);
        snprintf(q->key, sizeof q->key, "%s", k64);
    }
    crypto_wipe(k64, sizeof k64);
}

/* sensor.<node>_arbitration_handoff with '-' as '_', as HA names it while the device keeps the name we report; i > 0:
 * HA's suffix for a second entity of that id */
static void self_candidate(char out[ELEN], int i)
{
    int n = snprintf(out, ELEN, "sensor.%s_arbitration_handoff", node);
    if (i) snprintf(out + n, ELEN - (size_t)n, "_%d", i + 1);
    for (char *c = out; *c; c++) if (*c == '-') *c = '_';
}

/* i for one of our candidate ids, else -1 */
static int self_index(const char *entity)
{
    char base[ELEN]; size_t n; int i;
    self_candidate(base, 0); n = strlen(base);
    if (strncmp(entity, base, n)) return -1;
    if (!entity[n]) return 0;
    if (entity[n] != '_' || !isdigit((unsigned char)entity[n + 1]) || entity[n + 1] == '0') return -1;
    for (const char *c = entity + n + 1; *c; c++) if (!isdigit((unsigned char)*c)) return -1;
    i = atoi(entity + n + 1);
    return i >= 2 && i < 1 << 24 ? i - 1 : -1;
}

/* Hosts in our IPv4 subnet (the first interface up that is not loopback): 254 for a /24 */
static int subnet_hosts(void)
{
    struct ifaddrs *ifs, *a; int n = 254;
    if (getifaddrs(&ifs)) return n;
    for (a = ifs; a; a = a->ifa_next)
        if (a->ifa_addr && a->ifa_netmask && a->ifa_addr->sa_family == AF_INET && (a->ifa_flags & IFF_UP) && !(a->ifa_flags & IFF_LOOPBACK)) {
            uint32_t m = ntohl(((struct sockaddr_in *)a->ifa_netmask)->sin_addr.s_addr);
            n = ~m > 2 ? (~m > 1u << 24 ? 1 << 24 : (int)(~m - 1)) : 1;
            break;
        }
    freeifaddrs(ifs);
    return n;
}

static int valid_entity(const char *s)
{
    if (strncmp(s, "sensor.", 7) || !s[7]) return 0;
    for (s += 7; *s; s++) if (!(islower((unsigned char)*s) || isdigit((unsigned char)*s) || *s == '_')) return 0;
    return 1;
}

static void poll_entity(const char *e) { if (npoll < (int)(sizeof polls / sizeof polls[0])) snprintf(polls[npoll++], ELEN, "%s", e); }

static void on_packet(const uint8_t *p, size_t n, long long now, uint32_t ip)
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
            if (offer[0]) {                             /* the one we offered K to has it: take the offer down */
                char to[17]; hex8(to, pub);
                if (!strncmp(offer + 17, to, 16)) { offer[0] = 0; atomic_store(&notify, 1); }
            }
            /* one we did not count yet (it just joined, or we did): answer, or it would not count us until our next beacon.
             * Not rate limited: only a holder of K gets here, once per member */
            int f = fresh(pub, c, now);
            if (f) { struct peer *pp = peer(pub); snprintf(pp->node, NLEN, "%s", nd); pp->ip = ip; }
            if (f && !known) beacon();
            return;
        }
        struct cand *c = cand(pub);
        if (!c->first) c->first = now;
        c->ip = ip;
        snprintf(c->node, NLEN, "%s", nd); c->net = net; c->seen = now;
        if (in_net && !net && !ago(answer_at, now, 1000)) { answer_at = now; beacon(); }       /* someone looking: here we are */
        push(c, now);
    } break;
    case T_ENTITY: {                                    /* only for an Echo we know from its beacon; checked with HA before use */
        const uint8_t *pub = take(&r, 32), *l = take(&r, 1), *q; char e[ELEN];
        if (r.bad || *l >= ELEN || !(q = take(&r, *l))) return;
        memcpy(e, q, *l); e[*l] = 0;
        if (!valid_entity(e)) return;
        for (int i = 0; i < NPEER; i++) if (cands[i].seen && !memcmp(cands[i].pub, pub, 32) && strcmp(cands[i].ent, e)) {
            snprintf(cands[i].ent, ELEN, "%s", e); cands[i].attested = 0; cands[i].polled = 0;
        }
    } break;
    case T_PAIR: {
        const uint8_t *pub = take(&r, 32); uint64_t net = get64(&r); char nd[NLEN];
        get_node(&r, nd);
        if (r.bad || !memcmp(pub, pk, 32)) return;
        struct preq *o = &preqs[0];
        for (int i = 0; i < NPEER; i++) {
            if (preqs[i].seen && !memcmp(preqs[i].pub, pub, 32)) { o = &preqs[i]; break; }
            if (preqs[i].seen < o->seen) o = &preqs[i];
        }
        if (pair_until && (!o->seen || memcmp(o->pub, pub, 32))) fprintf(stderr, "arbitration: pair request from %s\n", nd);   /* anyone can send them */
        memcpy(o->pub, pub, 32); snprintf(o->node, NLEN, "%s", nd); o->net = net; o->seen = now;
    } break;
    case T_GIVE: {      /* a recorded one replayed in a later pairing can only put us back into the network we were handed */
        const uint8_t *to = take(&r, 32); uint64_t net = get64(&r); const uint8_t *blob = take(&r, BLOB);
        if (r.bad || r.p != r.end || memcmp(to, pk, 32) || !pair_until || now >= pair_until) return;
        take_key(net, blob, "from pairing");
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
            struct sockaddr_in from; socklen_t fl = sizeof from;
            ssize_t n = recvfrom(sock, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
            if (n > 0) { pthread_mutex_lock(&lk); on_packet(buf, (size_t)n, now_ms(), from.sin_addr.s_addr); pthread_mutex_unlock(&lk); }
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
                        fprintf(stderr, "arbitration: %s has not handed us its network yet: Home Assistant shows %s, and %s may not perform "
                                "Home Assistant actions; hold Volume up and Volume down on both Echos to pair them\n", cands[i].node,
                                self_ent[0] ? "our handoff entity but not its own" : "no handoff entity under the name we expect", cands[i].node);
                }
            }
            for (int i = 0; i < NPEER; i++) if (ago(cands[i].seen, now, PEER_TTL_MS)) push(&cands[i], now);
            if (!ago(beacon_at, now, in_net ? BEACON_MS : LONER_MS)) { beacon_at = now; beacon(); }
            /* Home Assistant: the ids our handoff entity may have, until one shows our key; the entities newcomers named
             * (we may hand them K once HA shows their key there) and those of older networks (they may offer us theirs) */
            /* every id between the first three and the last one tried is another device's (we only go one further when
             * HA answers for the last), so a round asks the first three and the last; a taken one, the next at once */
            if (!self_ent[0] && !ago(self_polled, now, self_polls < 20 ? SELF_POLL_MS : 60000)) {
                char e[ELEN]; self_polled = now; self_polls++; self_next = -1;
                for (int i = 0; i < 3; i++) { self_candidate(e, i); poll_entity(e); }
                if (self_n > 3) { self_candidate(e, self_n - 1); poll_entity(e); }
            } else if (!self_ent[0] && self_next >= 0) {
                char e[ELEN]; self_candidate(e, self_next); poll_entity(e); self_next = -1;
            }
            for (int i = 0; i < NPEER; i++) {
                struct cand *c = &cands[i];
                int give = in_net && (!c->net || c->net > net_id) && !c->attested, get = !in_net || (c->net && c->net < net_id);
                if (c->ent[0] && ago(c->seen, now, PEER_TTL_MS) && (give || get) && !ago(c->polled, now, POLL_MS)) { c->polled = now; poll_entity(c->ent); }
            }
        }
        if (pair_until && now >= pair_until) {
            pair_until = 0;
            if (!pair_gave) { fprintf(stderr, "arbitration: pairing ended, no Echo to pair with\n"); atomic_store(&pair_event, -1); }
        }
        if (pair_until && join) {
            if (!ago(pair_sent, now, 1000)) {
                struct pkt b; uint8_t l = (uint8_t)strlen(node);
                pair_sent = now; head(&b, T_PAIR); put(&b, pk, 32); put64(&b, in_net ? net_id : 0); put(&b, &l, 1); put(&b, node, l); send_pkt(&b);
            }
            /* a member: hand K to the one Echo outside our network that asked, from 2 min before our press until now.
             * One from an older network pairing too: it is the member here, we take its key instead */
            if (in_net && !pair_gave && now - pair_at >= PAIR_SETTLE_MS) {
                struct preq *one = NULL; int n = 0, older = 0;
                for (int i = 0; i < NPEER; i++) {
                    struct preq *p = &preqs[i];
                    if (!p->seen || p->seen < pair_at - PAIR_MS) continue;
                    if (!p->net || p->net > net_id) { one = p; n++; }
                    else if (p->net < net_id) older = 1;
                }
                if (older) ;
                else if (n > 1) {
                    fprintf(stderr, "arbitration: pairing refused: %d Echos asked at once, the network goes to neither\n", n);
                    pair_until = 0; atomic_store(&pair_event, -1);
                } else if (n == 1) {
                    struct pkt b; uint8_t blob[BLOB];
                    if (!seal(blob, one->pub)) {
                        head(&b, T_GIVE); put(&b, one->pub, 32); put64(&b, net_id); put(&b, blob, BLOB);
                        memcpy(give_pkt, b.p, b.n); give_left = 3; give_at = 0; pair_gave = 1;
                        fprintf(stderr, "arbitration: pairing: handing network %016llx to %s\n", (unsigned long long)net_id, one->node);
                    }
                    crypto_wipe(blob, sizeof blob);
                }
            }
        }
        if (give_left && !ago(give_at, now, 500)) {     /* three times: Wi-Fi drops broadcasts */
            struct pkt b; memcpy(b.p, give_pkt, sizeof give_pkt); b.n = sizeof give_pkt;
            send_pkt(&b); give_at = now;
            if (!--give_left) { crypto_wipe(give_pkt, sizeof give_pkt); pair_until = 0; atomic_store(&pair_event, 2); }
        }
        nq = npush; memcpy(q, pushes, sizeof q[0] * (size_t)nq); npush = 0;
        char pq[NPEER + 4][ELEN]; int npq = npoll; memcpy(pq, polls, sizeof pq[0] * (size_t)npq); npoll = 0;
        int np = count_peers(now);
        if (np != reported_peers) { reported_peers = np; atomic_store(&notify, 1); }
        pthread_mutex_unlock(&lk);
        for (int i = 0; i < nq; i++) {
            int r = hooks->send_key(q[i].node, q[i].net, q[i].key);
            fprintf(stderr, "arbitration: %s network %s to %s through Home Assistant\n", r < 0 ? "cannot hand (no link)" : "handing", q[i].net, q[i].node);
        }
        crypto_wipe(q, sizeof q);
        for (int i = 0; i < npq; i++) if (hooks->request) hooks->request(pq[i]);
        int pe = atomic_exchange(&pair_event, 0);
        if (pe && hooks->paired) hooks->paired(pe);
        if (atomic_exchange(&notify, 0) && hooks->changed) hooks->changed();
    }
    return NULL;
}

/* ---------------------------------------------------------------- interface */

void arb_key(const char *network, const char *key)
{
    uint8_t blob[BLOB + 4]; unsigned long long id = 0; char *end;
    id = strtoull(network, &end, 16);
    if (*end || !id || b64_decode(key, strlen(key), blob, sizeof blob) != BLOB) {
        fprintf(stderr, "arbitration: malformed key from Home Assistant, ignored\n"); return;
    }
    pthread_mutex_lock(&lk);
    take_key(id, blob, "from Home Assistant");
    pthread_mutex_unlock(&lk);
}

/* "HMA1 <pub>[ <net> <to> <key>]": 0 if it parses; *has_offer: the second part is there */
static int parse_handoff(const char *s, uint8_t pub[32], uint64_t *net, uint8_t to[8], uint8_t blob[BLOB], int *has_offer)
{
    char p64[64], n[20], t[20], k64[200]; uint8_t tmp[BLOB + 4]; int f;
    *has_offer = 0;
    if (strncmp(s, "HMA1 ", 5)) return -1;
    f = sscanf(s + 5, "%63s %19s %19s %199s", p64, n, t, k64);
    if (f < 1 || b64_decode(p64, strlen(p64), tmp, sizeof tmp) != 32) return -1;
    memcpy(pub, tmp, 32);
    if (f < 4) return f == 1 ? 0 : -1;
    char *end; *net = strtoull(n, &end, 16);
    if (*end || strlen(n) != 16 || strlen(t) != 16 || b64_decode(k64, strlen(k64), tmp, sizeof tmp) != BLOB) return -1;
    for (int i = 0; i < 8; i++) { unsigned v; if (sscanf(t + 2 * i, "%2x", &v) != 1) return -1; to[i] = (uint8_t)v; }
    memcpy(blob, tmp, BLOB); *has_offer = 1;
    return 0;
}

void arb_ha_state(const char *entity, const char *state)
{
    uint8_t pub[32], to[8], blob[BLOB]; uint64_t net = 0; int has_offer, ok;
    ok = !parse_handoff(state, pub, &net, to, blob, &has_offer);           /* not: "unavailable", "unknown", an older Echo */
    pthread_mutex_lock(&lk);
    int i = running && join && !self_ent[0] ? self_index(entity) : -1;
    if (i >= 0 && ok && !memcmp(pub, pk, 32)) {
        snprintf(self_ent, ELEN, "%s", entity); beacon_at = 0;
        fprintf(stderr, "arbitration: Home Assistant shows our key on %s\n", entity);
    } else if (i >= 0 && i + 2 > self_n && i + 1 < subnet_hosts()) {
        /* another device's entity holds that id (any state at all, "unavailable" from one that is off): HA gave ours
         * a higher suffix.  No more Echos of that name than hosts in the subnet can be in one group (broadcast) */
        self_n = i + 2; self_next = i + 1;
    }
    if (!ok) { pthread_mutex_unlock(&lk); return; }
    for (int i = 0; running && join && i < NPEER; i++) {
        struct cand *c = &cands[i];
        if (!c->seen || strcmp(c->ent, entity)) continue;
        if (memcmp(pub, c->pub, 32)) {                  /* the name it gave is someone else's entity (or a forger's) */
            if (c->attested) fprintf(stderr, "arbitration: %s no longer shows %s's key\n", entity, c->node);
            c->attested = 0; continue;
        }
        if (!c->attested) { c->attested = 1; c->pushed = 0; fprintf(stderr, "arbitration: Home Assistant shows %s's key on %s\n", c->node, entity); }
        if (has_offer && !memcmp(to, pk, 8) && !memcmp(blob, pub, 32)) {
            char from[NLEN + 40]; snprintf(from, sizeof from, "offered by %s", c->node);
            take_key(net, blob, from);
        }
    }
    pthread_mutex_unlock(&lk);
    crypto_wipe(blob, sizeof blob);
}

void arb_handoff(char *out, size_t cap)
{
    char p64[48];
    b64_encode(pk, 32, p64, 0, 1);
    pthread_mutex_lock(&lk);
    if (!running) snprintf(out, cap, "%s", "");
    else snprintf(out, cap, "HMA1 %s%s%s", p64, offer[0] ? " " : "", offer);
    pthread_mutex_unlock(&lk);
}

int arb_pair(void)
{
    long long now = now_ms();
    pthread_mutex_lock(&lk);
    if (!running) { pthread_mutex_unlock(&lk); return -1; }
    if (!join) {                                        /* the gesture says "join": as the switch does */
        join = 1; memset(cands, 0, sizeof cands); join_since = now; beacon_at = 0; save(); atomic_store(&notify, 1);
    }
    pair_at = now; pair_until = now + PAIR_MS; pair_sent = 0; pair_gave = 0;
    fprintf(stderr, "arbitration: pairing for %d s (%s)\n", PAIR_MS / 1000, in_net ? "a member: hands its network to the one Echo that asks"
                                                                                    : "looking for a member to take a network from");
    atomic_store(&pair_event, 1);
    pthread_mutex_unlock(&lk);
    return 0;
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

static size_t jstr(char *o, size_t cap, const char *s)          /* node names and entity ids: [a-z0-9_.-] only */
{
    size_t n = 0;
    for (; *s && n + 1 < cap; s++) if (isalnum((unsigned char)*s) || *s == '-' || *s == '_' || *s == '.') o[n++] = *s;
    o[n] = 0;
    return n;
}

size_t arb_status_json(char *o, size_t cap)
{
    size_t n = 0; long long now = now_ms(); char t[ELEN], ip[INET_ADDRSTRLEN]; int k = 0;
#define J(...) do { if (n < cap) n += (size_t)snprintf(o + n, cap - n, __VA_ARGS__); } while (0)
    pthread_mutex_lock(&lk);
    jstr(t, sizeof t, self_ent);
    J("{\"running\":%s,\"joining\":%s,\"node\":\"%s\",", running ? "true" : "false", join ? "true" : "false", node);
    if (in_net) J("\"network\":\"%016llx\",", (unsigned long long)net_id); else J("\"network\":null,");
    J("\"handoff_entity\":%s%s%s,\"pairing\":%s,\"members\":[", t[0] ? "\"" : "", t[0] ? t : "null", t[0] ? "\"" : "",
      pair_until && now < pair_until ? "true" : "false");
    for (int i = 0; i < NPEER; i++) if (ago(peers[i].seen, now, PEER_TTL_MS)) {
        struct in_addr a = { peers[i].ip }; inet_ntop(AF_INET, &a, ip, sizeof ip); jstr(t, sizeof t, peers[i].node);
        J("%s{\"node\":\"%s\",\"ip\":\"%s\",\"seen_s\":%lld}", k++ ? "," : "", t, peers[i].ip ? ip : "", (now - peers[i].seen) / 1000);
    }
    J("],\"others\":["); k = 0;
    for (int i = 0; i < NPEER; i++) if (ago(cands[i].seen, now, PEER_TTL_MS)) {
        struct cand *c = &cands[i]; const char *st;
        struct in_addr a = { c->ip }; inet_ntop(AF_INET, &a, ip, sizeof ip); jstr(t, sizeof t, c->node);
        st = !c->net ? "none" : in_net && c->net > net_id ? "younger" : "older";      /* none: not in a network (yet) */
        J("%s{\"node\":\"%s\",\"ip\":\"%s\",\"network\":", k++ ? "," : "", t, c->ip ? ip : "");
        if (c->net) J("\"%016llx\"", (unsigned long long)c->net); else J("null");
        J(",\"state\":\"%s\",\"for_s\":%lld,\"key_confirmed\":%s}", st, (now - c->first) / 1000, c->attested ? "true" : "false");
    }
    J("]}");
    pthread_mutex_unlock(&lk);
#undef J
    return n < cap ? n : cap - 1;
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
