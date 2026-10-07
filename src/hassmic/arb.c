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
 * blocks in the state file, so it keeps rising across restarts).  Every Echo is in one, always: it looks for members
 * first; with none after DISCOVER_MS it makes up K and a random network id itself.  Two networks that formed at the
 * same time merge: the lower id wins.  The network is also how the settings pages find each other (members' signed
 * beacons give name and address), so it does not depend on the "arbitration" setting, which only says whether this
 * Echo takes part in rounds.  One that does not says so in its beacons (a flags byte after the counter) and the others
 * leave it out: they do not wait for its claims, it answers every wake word itself.  Older Echos reject that longer
 * beacon, so they do not count it either.
 *
 * Who gets K, three ways; the first two go through Home Assistant, so the trust is the owner's: what they adopted.
 *   Tag: HA names entities after area, device and (since 2026.9) parent device, in an order users can configure, and
 *   never tells a device its entity ids, so no Echo can know where HA shows anything of another.  Tags are the
 *   exception: an ESPHome device may report one as scanned without "perform actions" (HA fires events, tag_scanned
 *   included, for any adopted device), and HA keeps it as tag.<tag id>, with no area or device in the id.  So every
 *   Echo that needs one (outside a network, or an Echo outside ours in sight) scans "hassmic_<its public key, hex>"
 *   every SCAN_MS, and an Echo reads another's tag (once-requests, which need no permission either): HA shows the last
 *   scan's time, and once that changed after our first read, a device HA adopted presented that key since (a stale tag
 *   of an Echo since removed from HA never changes).  Both sides check: a member hands K (T_GIVE, sealed to the
 *   newcomer's key) only to an Echo whose tag HA confirmed, and the newcomer takes it only from an Echo whose tag HA
 *   confirmed, so a forger can neither get K nor push a network of its own.  No clock needed, nothing to configure.
 *   Action: with no tag confirmed after ATTEST_WAIT_MS (HA without the tag integration, an older Echo), the member
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
 *   Limits: any HA user (a tag scanned in the companion app) or adopted ESPHome device can vouch for a key, and for the
 *   action any allowed ESPHome device can hand an Echo one.  An Echo that leaves
 *   wipes K; K is not rotated when an Echo is removed.  Counters are only remembered in memory: right after a restart,
 *   one recorded packet per member can be replayed once (at worst one wake word lost).  The goal is that nobody on the
 *   network who is not in Home Assistant (and has not pressed the buttons) can silence an Echo or listen in on the rounds.
 *
 * UDP broadcast on one port (default 28930; the stock firewall admits inbound UDP 16384-32767), so everything reaches
 * every Echo in the subnet.  Wi-Fi drops broadcasts now and then (no retries on the air): claims go out three times,
 * spread over 80 ms (COPY_MS), the counter drops the copies.
 *
 *   beacon  "HMA1" 1  pub[32] net[8] nlen node [ctr[8] tag[16], in a network]
 *   claim   "HMA1" 4  net[8] id[8] ctr[8] kw[8] score[4] prio won tag[16]
 *   pair    "HMA1" 5  pub[32] net[8] nlen node
 *   give    "HMA1" 6  to[32] net[8] key[104]     (taken while pairing, or from an Echo whose tag HA confirmed)
 *   key (the action's "key" argument, base64)  sender pub[32] nonce[24] mac[16] enc(K)[32], "network": the id in hex
 *   tag id  "hassmic_<pub, 64 hex>"; HA's entity tag.hassmic_<pub hex>, state the last scan's time
 * Little endian; id: first 8 bytes of the public key; kw: BLAKE2b of the keyword in lower case; tag: keyed BLAKE2b with
 * a key derived from K.
 *
 * Kiosk Satellite mode (the "arbitration_mode" setting; docs/kiosk-arbitration.md): rounds the way Kiosk Satellite
 * (2026.10.1) settles them, so that Echos and kiosks listening for the same phrase answer once between them.  Written
 * from the description of its wire format, no code of theirs (CC BY-NC-ND).  As they designed it, and nothing more:
 *   claim  {"ks":"wake","v":1,"id":"<16 hex>","n":<random 0..2^30>,"p":"<phrase>","e":<dB, 0.1>}  UDP broadcast to
 *          port 2330, three copies at 0, 15 and 30 ms; id:n tells copies apart.  p: the wake word in lower case,
 *          whitespace collapsed ("alexa", "hey jarvis"); e: main.c's kiosk_score, their loudness over the noise floor.
 *   round  wait the window (setting, 100-500 ms, 400 as theirs), then every claim for our phrase that arrived within
 *          +-window of ours competes: the highest e answers, an exact tie goes to the lower id.
 * So: no key (anyone on the LAN can claim and silence this Echo), no priority for an Echo in a conversation or
 * ringing, no "answers" message (a claim Wi-Fi drops leaves both answering; Home Assistant's 2 s cooldown then lets the
 * first one through), and a wait of the window on every wake word, as nothing tells whether anyone else listens.  Our
 * id is the hex of our public key's first 8 bytes: stable across restarts.  The Echo network runs on as before (keys,
 * beacons, the settings pages); our beacons say "no rounds" (flag 1), so Echos in our own mode do not wait for claims
 * we never send, and "kiosk mode" (flag 2, with arbitration on), so the pages can show who is in which.  2330 lies
 * outside the firewall's inbound 16384-32767: lockdown.sh admits it while state/config says arbitration_mode=kiosk.
 * The socket is the loop's own: opened and closed there as the mode changes, so no other thread ever polls a closed one.
 */
#include "arb.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
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
/* A claim and an "answers" go out three times, these ms after the first: Wi-Fi broadcasts have no retries, and two copies
 * sent in the same ms were lost together (2026-10-06, biscuit missed radar's claim in 2 of 3 rounds while donut got it;
 * it got radar's "answers" 200 ms later).  The last copy still reaches a member whose detection came 60 ms before ours
 * (the spread seen, -17..+59 ms) before its decision.  Copies carry the same counter: receivers drop all but the first */
static const int COPY_MS[] = { 30, 80 };
#define LOOKBACK_MS  1000       /* claims this much older than our own detection still count: the engines report late by different amounts */
/* Beacons are broadcasts too, and an Echo misses 10-30 % of those (ble.c: the BLE scan's share of the antenna, plus what
 * Wi-Fi loses anyway).  At a fixed 30 s one Echo's beacons fell into the gaps two times in three, at both others, for
 * as long as we watched: it was out of their networks (their rounds, the settings page's list) for 15 s every 90 s
 * (2026-10-07, while a wired PC got every one).  A random interval keeps a sender from staying in such a phase, and a
 * member counts until it has been silent for 5 min, about ten beacons: an Echo that is really gone (unplugged) stays
 * in the list that long, and the others wait for its claim (WINDOW_MS) in that time */
#define BEACON_MIN_MS 20000
#define BEACON_MAX_MS 40000
#define LONER_MS     10000      /* beacon interval outside a network; members answer such a beacon at once */
/* ...but at most one a second (answer_at), and broadcasts get lost: in its first DISCOVER_MS an Echo outside a network
 * asks every LOOK_MS, or one missed answer had it start a network of its own beside the existing one */
#define LOOK_MS      1200
#define PEER_TTL_MS  300000     /* a member not heard of for this long no longer counts */
#define DISCOVER_MS  5000       /* no member heard in this time: start a network */
#define PUSH_MS      30000      /* hand our key to the same Echo at most this often */
#define CTR_BLOCK    4096
#define POLL_MS      3000       /* ask Home Assistant again for an Echo's tag, until it confirms the key */
#define SCAN_MS      10000      /* scan our tag this often while another Echo may be waiting for it to change... */
#define SCAN_N       12         /* ...this many times in a row, then every SCAN_SLOW_MS (an Echo HA never confirms, a
                                 * forger's beacons: no endless stream of scans in HA's logbook) */
#define SCAN_SLOW_MS 300000
#define GIVE_MS      5000       /* hand K on the LAN to an Echo HA confirmed this often, until it is in */
#define ATTEST_WAIT_MS 30000    /* no tag confirmed for a newcomer by then: hand K through its action instead.  A tag
                                 * takes a read, a scan (within SCAN_MS) and a read again; the action would raise a
                                 * repair in HA for every owner who did not allow it */
#define PAIR_MS      120000     /* the pairing gesture's window, and how far back a member looks for requests */
#define PAIR_SETTLE_MS 2000     /* a member waits this long after its press for a second requester */
#define NPEER  16
#define NCLAIM 16
#define NLEN   64
#define TLEN   80               /* "tag.hassmic_" + 64 hex */
#define BLOB   (32 + 24 + 16 + 32)

enum { T_BEACON = 1, T_CLAIM = 4, T_PAIR = 5, T_GIVE = 6 };    /* 2 was the handoff entity's id (older Echos), ignored */
/* the beacon's flags byte.  F_HELLO: sent in the first beacons after a start or a join, when the sender counts nobody
 * yet; every member answers at once.  Without it a restarted Echo, which the others still count for minutes
 * (PEER_TTL_MS), would see none of them until their next beacon, up to BEACON_MAX_MS later.  Older builds ignore it */
enum { F_QUIET = 1, F_KIOSK = 2, F_HELLO = 4 };

#define KIOSK_PORT    2330      /* Kiosk Satellite's, fixed (HASSMIC_KIOSK_PORT: tests) */
#define KIOSK_KEEP_MS 2000      /* their claims kept this long: copies within it are the same claim */
#define KIOSK_GAP_MS  15        /* between the three copies */
#define KIOSK_PHRASE  64

static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER; /* everything below; taken after core_lock, never before it */
static int sock = -1, running, arbitrate = 1;    /* arbitrate: take part in rounds (the network runs regardless) */
static struct sockaddr_in dest;
static const struct arb_hooks *hooks;
static char node[NLEN];
static uint8_t sk[32], pk[32];
static int in_net;
static uint64_t net_id;
static uint8_t net_key[32], mac_key[32];
static uint64_t ctr, ctr_saved;
static long long started, beacon_at, answer_at, beacon_gap = BEACON_MIN_MS, hello_until;
static int reported_peers = -1;
static atomic_int notify;

static struct peer { uint8_t id[8]; uint64_t ctr; long long seen; char node[NLEN]; uint32_t ip; int quiet, kiosk; } peers[NPEER];   /* node, ip, quiet (no rounds), kiosk: from its beacons */
static struct claim { uint8_t id[8], kw[8]; int score, prio, won; long long at; } claims[NCLAIM];
static unsigned claim_next;
static struct cand {                                    /* Echos outside our network */
    uint8_t pub[32]; char node[NLEN]; uint64_t net;
    int attested;                                       /* HA showed a scan of its tag after our first read */
    char tag0[48];                                      /* its tag's state at our first read ("": no answer yet) */
    uint32_t ip;                                        /* where its beacons come from (network byte order), for the settings page */
    long long seen, first, pushed, gave, polled;        /* pushed: through the action; gave: on the LAN */
} cands[NPEER];
static struct push { char node[NLEN], net[20], key[160]; } pushes[NPEER];     /* for Home Assistant, sent outside the lock */
static int npush;
static char polls[NPEER][TLEN];                  /* tags to ask Home Assistant for, outside the lock */
static int npoll;
static char self_tag[TLEN - 4];                         /* our tag id, "hassmic_<pub hex>" */
static long long scan_at;
static int scans, scan_due;                             /* scans in a row while someone may wait; one to send outside the lock */
static struct preq { uint8_t pub[32]; char node[NLEN]; uint64_t net; long long seen; } preqs[NPEER];   /* pair requests heard */
static long long pair_at, pair_until, pair_sent, give_at;
static int pair_gave, give_left;
static atomic_int pair_event;
static uint8_t give_pkt[5 + 32 + 8 + BLOB];
static struct { uint8_t kw[8]; int score, prio, kiosk; long long at; } round_;

/* Kiosk Satellite mode.  kround.e and kclaims[].e: dB x 10, as on the wire */
static int mode = ARB_HASSMIC, kwindow = 400, koffset;     /* koffset: dB */
static int ksock = -1;                                  /* the loop's: opened and closed there only */
static long long kfailed;
static char kid[17];
static struct kclaim { char id[65], p[KIOSK_PHRASE]; long long n, at; int e; } kclaims[NCLAIM];
static unsigned kclaim_next;
static long long kheard;                                /* the last claim from another device, for the settings page */
static long long klogged;                               /* claims are logged once a second at most, the rest counted: */
static int kunlogged;                                   /* anyone on the LAN can send them, and boot.log is on flash */
static struct { char p[KIOSK_PHRASE]; int e; } kround;
static struct { uint8_t p[256]; size_t n; long long at; int left; } copies[2];    /* claims and "answers": copies still due */
static int wake_fd[2] = { -1, -1 };                     /* a byte wakes the loop: copies are due before its next poll ends */
static char kpkt[300];
static size_t kpkt_n;
static int kcopies;                                     /* copies of our claim still to send, the next at kcopy_at */
static long long kcopy_at;

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
    fprintf(f, "arbitrate %d\nctr %llu\n", arbitrate, (unsigned long long)ctr_saved);
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
        /* "join 0": the switch of older versions, which also left the network; now only the rounds stay off */
        if (sscanf(line, "arbitrate %d", &j) == 1 || sscanf(line, "join %d", &j) == 1) arbitrate = j != 0;
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

/* Send now, and have the loop send the same packet again COPY_MS later (lk held) */
static void send_spread(const struct pkt *b, long long now)
{
    int i = !copies[0].left ? 0 : !copies[1].left ? 1 : copies[0].at > copies[1].at;   /* a free slot, else the older one */
    send_pkt(b);
    memcpy(copies[i].p, b->p, b->n); copies[i].n = b->n; copies[i].at = now; copies[i].left = 2;
    if (wake_fd[1] >= 0) { char c = 1; if (write(wake_fd[1], &c, 1) < 0) {} }
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

static int count_peers(long long now)            /* members we settle wake words with */
{
    int n = 0;
    for (int i = 0; i < NPEER; i++) n += ago(peers[i].seen, now, PEER_TTL_MS) && !peers[i].quiet;
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

static void beacon(int answer)                          /* answer: to another's beacon, never F_HELLO (two would answer for ever) */
{
    struct pkt b; uint8_t l = (uint8_t)strlen(node);
    head(&b, T_BEACON); put(&b, pk, 32); put64(&b, in_net ? net_id : 0); put(&b, &l, 1); put(&b, node, l);
    if (in_net) {
        /* kiosk: takes part, the other way (an Echo with arbitration off is just quiet, whatever its mode) */
        uint8_t f = (uint8_t)((!arbitrate || mode == ARB_KIOSK ? F_QUIET : 0) | (arbitrate && mode == ARB_KIOSK ? F_KIOSK : 0)
                              | (!answer && now_ms() < hello_until ? F_HELLO : 0));
        put64(&b, next_ctr()); if (f) put(&b, &f, 1);
        tag(&b);
    }
    send_pkt(&b);
}

static void adopt(const uint8_t k[32], uint64_t id, const char *how, const char *from)
{
    memcpy(net_key, k, 32); net_id = id; in_net = 1; derive();
    memset(peers, 0, sizeof peers); memset(claims, 0, sizeof claims);
    save();
    fprintf(stderr, "arbitration: %s network %016llx%s%s%s\n", how, (unsigned long long)id, from ? " (key " : "", from ? from : "", from ? ")" : "");
    beacon_at = 0; hello_until = now_ms() + BEACON_MAX_MS; atomic_store(&notify, 1);
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
    if (!running) fprintf(stderr, "arbitration: key %s ignored (not running)\n", from);
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

/* Hand K to an Echo outside our network, or in a younger one: on the LAN once HA confirmed its tag (it takes it once HA
 * confirmed ours), else (after ATTEST_WAIT_MS) through its action */
static void push(struct cand *c, long long now)
{
    uint8_t blob[BLOB]; char k64[160];
    if (!in_net || (c->net && c->net <= net_id)) return;
    if (c->attested) {
        struct pkt b;
        if (ago(c->gave, now, GIVE_MS) || seal(blob, c->pub)) return;
        if (!c->gave) fprintf(stderr, "arbitration: handing network %016llx to %s on the LAN\n", (unsigned long long)net_id, c->node);
        c->gave = now;
        head(&b, T_GIVE); put(&b, c->pub, 32); put64(&b, net_id); put(&b, blob, BLOB);
        send_pkt(&b); send_pkt(&b);                     /* Wi-Fi drops broadcasts */
        crypto_wipe(&b, sizeof b); crypto_wipe(blob, sizeof blob);
        return;
    }
    if (ago(c->pushed, now, PUSH_MS) || !c->node[0] || now - c->first < ATTEST_WAIT_MS || npush == NPEER) return;
    if (seal(blob, c->pub)) return;
    c->pushed = now;
    b64_encode(blob, sizeof blob, k64, 0, 1);
    struct push *q = &pushes[npush++];
    snprintf(q->node, NLEN, "%s", c->node); snprintf(q->net, sizeof q->net, "%016llx", (unsigned long long)net_id);
    snprintf(q->key, sizeof q->key, "%s", k64);
    crypto_wipe(k64, sizeof k64); crypto_wipe(blob, sizeof blob);
}

static void tag_entity(char out[TLEN], const uint8_t pub[32])
{
    memcpy(out, "tag.hassmic_", 12);
    for (int i = 0; i < 32; i++) snprintf(out + 12 + 2 * i, 3, "%02x", pub[i]);
}

static void poll_tag(const uint8_t pub[32]) { if (npoll < NPEER) tag_entity(polls[npoll++], pub); }

static void on_packet(const uint8_t *p, size_t n, long long now, uint32_t ip)
{
    struct rd r = { p, p + n, 0 }; const uint8_t *m = take(&r, 5);
    if (!m || memcmp(m, "HMA1", 4)) return;
    switch (m[4]) {
    case T_BEACON: {
        const uint8_t *pub = take(&r, 32); uint64_t net = get64(&r); char nd[NLEN];
        get_node(&r, nd);
        if (r.bad || !memcmp(pub, pk, 32)) return;
        if (in_net && net == net_id) {                  /* a member */
            uint64_t c = get64(&r); int known = 0, flags = r.end - r.p == 17 ? r.p[0] : 0;
            if (r.bad || (r.end - r.p != 16 && r.end - r.p != 17) || !tag_ok(p, n)) return;
            for (int i = 0; i < NPEER; i++) {
                known |= !memcmp(peers[i].id, pub, 8) && ago(peers[i].seen, now, PEER_TTL_MS);
                if (cands[i].seen && !memcmp(cands[i].pub, pub, 32)) cands[i].seen = 0;    /* in our network now: nothing more to hand it */
            }
            /* one we did not count yet (it just joined, or we did), or one that just started and counts nobody (F_HELLO):
             * answer, or it would not count us until our next beacon.  Not rate limited: only a holder of K gets here, once
             * per fresh counter */
            int f = fresh(pub, c, now);
            if (f) { struct peer *pp = peer(pub); snprintf(pp->node, NLEN, "%s", nd); pp->ip = ip; pp->quiet = flags & F_QUIET; pp->kiosk = !!(flags & F_KIOSK); }
            if (f && (!known || (flags & F_HELLO))) beacon(1);
            return;
        }
        struct cand *c = cand(pub);
        /* someone new may wait for our tag: the fast rate again, at most once per slow period (anyone on the LAN can
         * make up new keys, and each scan is a logbook entry in HA) */
        static long long sped;
        if (!c->first) { c->first = now; if (!ago(sped, now, SCAN_SLOW_MS)) { sped = now; scans = 0; } }
        c->ip = ip;
        snprintf(c->node, NLEN, "%s", nd); c->net = net; c->seen = now;
        if (in_net && !net && !ago(answer_at, now, 1000)) { answer_at = now; beacon(1); }       /* someone looking: here we are */
        push(c, now);
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
        if (r.bad || r.p != r.end || memcmp(to, pk, 32)) return;
        if (pair_until && now < pair_until) { take_key(net, blob, "from pairing"); return; }
        /* outside pairing only from an Echo HA confirmed, for the network it beacons */
        for (int i = 0; i < NPEER; i++) {
            struct cand *c = &cands[i];
            if (!c->seen || !c->attested || c->net != net || memcmp(c->pub, blob, 32)) continue;
            if (!in_net || net < net_id) {
                char from[NLEN + 40]; snprintf(from, sizeof from, "from %s, confirmed by Home Assistant", c->node);
                take_key(net, blob, from);
            }
            break;
        }
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

/* ---------------------------------------------------------------- Kiosk Satellite mode (lk held) */

/* Their phrase: lower case, whitespace trimmed and collapsed */
static void kiosk_phrase(char out[KIOSK_PHRASE], const char *s)
{
    size_t n = 0; int gap = 0;
    for (; *s; s++) {
        if (isspace((unsigned char)*s)) { gap = n > 0; continue; }
        if (gap && n + 1 < KIOSK_PHRASE) out[n++] = ' ';
        gap = 0;
        if (n + 1 < KIOSK_PHRASE) out[n++] = (char)tolower((unsigned char)*s);
    }
    out[n] = 0;
}

static const char *jws(const char *p) { while (isspace((unsigned char)*p)) p++; return p; }

/* A JSON string, p just past its opening quote, into out (UTF-8): past the closing quote, NULL if malformed or too long */
static const char *jstring(const char *p, char *out, size_t cap)
{
    size_t n = 0;
    while (*p != '"') {
        unsigned c = (unsigned char)*p++; int u = 0;
        if (c < 0x20) return NULL;                      /* the end of the datagram among them */
        if (c == '\\') {
            switch (c = (unsigned char)*p++) {
            case '"': case '\\': case '/': break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u':
                c = 0;
                for (int i = 0; i < 4; i++, p++) {
                    if (!isxdigit((unsigned char)*p)) return NULL;
                    c = c << 4 | (unsigned)(isdigit((unsigned char)*p) ? *p - '0' : (tolower((unsigned char)*p) - 'a' + 10));
                }
                u = 1; break;
            default: return NULL;
            }
        }
        uint8_t b[3]; size_t k = 1;                     /* \u escapes as UTF-8; surrogate pairs stay apart (no phrase of ours has them) */
        if (!u || c < 0x80) b[0] = (uint8_t)c;
        else if (c < 0x800) { b[0] = (uint8_t)(0xc0 | c >> 6); b[1] = (uint8_t)(0x80 | (c & 0x3f)); k = 2; }
        else { b[0] = (uint8_t)(0xe0 | c >> 12); b[1] = (uint8_t)(0x80 | (c >> 6 & 0x3f)); b[2] = (uint8_t)(0x80 | (c & 0x3f)); k = 3; }
        if (n + k >= cap) return NULL;
        memcpy(out + n, b, k); n += k;
    }
    out[n] = 0;
    return p + 1;
}

/* A claim, checked as their receivers check it: ks "wake", v 1 (integers), id a string of at most 64, n an integer, p a
 * string, e a finite number.  Other keys with plain values are passed over (a later version may add some); objects and
 * arrays are not theirs, so the datagram is.  t: a NUL-terminated datagram.  0 ok */
static int kiosk_parse(const char *t, struct kclaim *k)
{
    char key[16], ks[8] = "", p[KIOSK_PHRASE * 2]; int have = 0; double e = 0;
    enum { KS = 1, V = 2, ID = 4, N = 8, P = 16, E = 32 };
    const char *q = jws(t);
    if (*q++ != '{') return -1;
    for (q = jws(q); *q != '}'; ) {
        if (*q++ != '"' || !(q = jstring(q, key, sizeof key))) return -1;
        q = jws(q);
        if (*q++ != ':') return -1;
        q = jws(q);
        int bit = !strcmp(key, "ks") ? KS : !strcmp(key, "v") ? V : !strcmp(key, "id") ? ID : !strcmp(key, "n") ? N
                : !strcmp(key, "p") ? P : !strcmp(key, "e") ? E : 0;
        if (have & bit) return -1;
        have |= bit;
        if (*q == '"') {
            char skip[KIOSK_PHRASE * 2];
            char *dst = bit == KS ? ks : bit == ID ? k->id : bit == P ? p : skip;
            size_t cap = bit == KS ? sizeof ks : bit == ID ? sizeof k->id : bit == P ? sizeof p : sizeof skip;
            if ((bit & (V | N | E)) || !(q = jstring(q + 1, dst, cap))) return -1;
        } else {
            char *end; double d = strtod(q, &end); int whole = 1;
            if (end == q) {                             /* true, false, null: only under keys of no interest */
                size_t l = !strncmp(q, "true", 4) || !strncmp(q, "null", 4) ? 4 : !strncmp(q, "false", 5) ? 5 : 0;
                if (!l || bit) return -1;
                q += l;
            } else {
                if (bit & (KS | ID | P)) return -1;
                for (const char *c = q; c < end; c++) whole &= *c != '.' && *c != 'e' && *c != 'E';
                if ((bit & (V | N)) && (!whole || fabs(d) > 9e15)) return -1;
                if (bit == V && d != 1) return -1;
                if (bit == N) k->n = (long long)d;
                if (bit == E) e = d;
                q = end;
            }
        }
        q = jws(q);
        if (*q == ',') q = jws(q + 1);
        else if (*q != '}') return -1;
    }
    if (have != (KS | V | ID | N | P | E) || strcmp(ks, "wake") || !k->id[0] || !isfinite(e) || fabs(e) > 1e6) return -1;
    kiosk_phrase(k->p, p);
    k->e = (int)lround(e * 10);
    return 0;
}

static void kiosk_packet(const char *t, long long now)
{
    struct kclaim c;
    memset(&c, 0, sizeof c);
    if (kiosk_parse(t, &c) || !strcmp(c.id, kid)) return;     /* our own broadcast comes back */
    for (int i = 0; i < NCLAIM; i++)
        if (ago(kclaims[i].at, now, KIOSK_KEEP_MS) && kclaims[i].n == c.n && !strcmp(kclaims[i].id, c.id)) return;   /* a copy */
    c.at = now; kheard = now;
    kclaims[kclaim_next++ % NCLAIM] = c;
    if (ago(klogged, now, 1000)) { kunlogged++; return; }
    fprintf(stderr, "arbitration: kiosk claim from %.16s: \"%s\", %.1f dB", c.id, c.p, c.e / 10.0);
    if (kunlogged) fprintf(stderr, " (%d more since the last one logged)", kunlogged);
    fprintf(stderr, "\n");
    klogged = now; kunlogged = 0;
}

static void kiosk_send(void)
{
    static int last_err;
    if (ksock < 0) return;
    struct sockaddr_in to = dest; const char *pe = getenv("HASSMIC_KIOSK_PORT");
    to.sin_port = htons((uint16_t)(pe ? atoi(pe) : KIOSK_PORT));
    if (sendto(ksock, kpkt, kpkt_n, 0, (const struct sockaddr *)&to, sizeof to) >= 0) { last_err = 0; return; }
    if (errno != last_err) fprintf(stderr, "arbitration: kiosk send: %s\n", strerror(errno));
    last_err = errno;
}

/* The loop's: the socket open while this Echo settles rounds the kiosk way, closed otherwise */
static void kiosk_sync(long long now)
{
    int want = running && arbitrate && mode == ARB_KIOSK, on = 1; const char *pe = getenv("HASSMIC_KIOSK_PORT");
    struct sockaddr_in a;
    if (!want && ksock >= 0) { close(ksock); ksock = -1; kcopies = 0; fprintf(stderr, "arbitration: kiosk port closed\n"); }
    if (!want || ksock >= 0 || ago(kfailed, now, 10000)) return;
    memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons((uint16_t)(pe ? atoi(pe) : KIOSK_PORT)); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if ((ksock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)) < 0
        || setsockopt(ksock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) || setsockopt(ksock, SOL_SOCKET, SO_BROADCAST, &on, sizeof on)
        || bind(ksock, (struct sockaddr *)&a, sizeof a)) {
        if (!kfailed) fprintf(stderr, "arbitration: kiosk port %d: %s; answering every wake word until it opens\n", ntohs(a.sin_port), strerror(errno));
        if (ksock >= 0) close(ksock);
        ksock = -1; kfailed = now; return;
    }
    kfailed = 0;
    fprintf(stderr, "arbitration: Kiosk Satellite mode, port %d, window %d ms, offset %+d dB, id %s\n", ntohs(a.sin_port), kwindow, koffset, kid);
}

/* Our claim, first copy now (the loop sends the other two): the time to decide, 0 if there is no round */
static long long kiosk_claim(const char *keyword, int score, long long now)
{
    char ph[KIOSK_PHRASE], esc[KIOSK_PHRASE * 6]; size_t n = 0; uint32_t r;
    kiosk_phrase(ph, keyword);
    if (!running || !arbitrate || ksock < 0 || !ph[0]) return 0;
    for (const char *c = ph; *c && n + 7 < sizeof esc; c++) {
        if (*c == '"' || *c == '\\') { esc[n++] = '\\'; esc[n++] = *c; }
        else if ((unsigned char)*c < 0x20) n += (size_t)snprintf(esc + n, sizeof esc - n, "\\u%04x", *c);
        else esc[n++] = *c;
    }
    esc[n] = 0;
    ws_random(&r, sizeof r);
    snprintf(kround.p, sizeof kround.p, "%s", ph);
    /* the owner's offset: main.c's lift is a guess, so how an Echo's loudness compares with a tablet's is set by ear.
     * Rounded as sent: both sides compare the numbers on the wire */
    kround.e = (int)lround(score / 10.0) + koffset * 10;
    int l = snprintf(kpkt, sizeof kpkt, "{\"ks\":\"wake\",\"v\":1,\"id\":\"%s\",\"n\":%u,\"p\":\"%s\",\"e\":%s%d.%d}", kid, r & 0x3fffffff, esc,
                     kround.e < 0 ? "-" : "", abs(kround.e) / 10, abs(kround.e) % 10);
    if (l < 0 || (size_t)l >= sizeof kpkt) return 0;
    kpkt_n = (size_t)l;
    kiosk_send(); kcopies = 2; kcopy_at = now + KIOSK_GAP_MS;
    if (wake_fd[1] >= 0) { char c = 1; if (write(wake_fd[1], &c, 1) < 0) {} }    /* the loop may sit in a 100 ms poll */
    return now + kwindow;
}

static int kiosk_decide(void)
{
    int win = 1;
    for (int i = 0; i < NCLAIM; i++) {
        struct kclaim *k = &kclaims[i];
        if (!k->at || llabs(k->at - round_.at) > kwindow || strcmp(k->p, kround.p)) continue;
        if (k->e > kround.e || (k->e == kround.e && strcmp(k->id, kid) < 0)) {
            if (win) fprintf(stderr, "arbitration: %.16s heard it better (%.1f dB against our %.1f)\n", k->id, k->e / 10.0, kround.e / 10.0);
            win = 0;
        }
    }
    return win;
}

static long long copy_at(int i) { return copies[i].at + COPY_MS[2 - copies[i].left]; }     /* lk held, left > 0 */

static void *loop(void *arg)
{
    uint8_t buf[512]; struct pollfd pf[3] = { { sock, POLLIN, 0 }, { -1, POLLIN, 0 }, { wake_fd[0], POLLIN, 0 } };
    (void)arg;
    for (;;) {
        long long due = now_ms() + 100;
        int wait;
        pthread_mutex_lock(&lk);
        kiosk_sync(now_ms());
        pf[1].fd = ksock;                               /* -1: poll leaves it out */
        if (kcopies && kcopy_at < due) due = kcopy_at;
        for (int i = 0; i < 2; i++) if (copies[i].left && copy_at(i) < due) due = copy_at(i);
        pthread_mutex_unlock(&lk);
        wait = due - now_ms() < 0 ? 0 : (int)(due - now_ms());
        if (poll(pf, 3, wait) > 0) {
            if (pf[2].revents & POLLIN) { char c[16]; if (read(wake_fd[0], c, sizeof c) < 0) {} }    /* only to plan again */
            struct sockaddr_in from; socklen_t fl = sizeof from; ssize_t n;
            if ((pf[0].revents & POLLIN) && (n = recvfrom(sock, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl)) > 0) {
                pthread_mutex_lock(&lk); on_packet(buf, (size_t)n, now_ms(), from.sin_addr.s_addr); pthread_mutex_unlock(&lk);
            }
            if ((pf[1].revents & POLLIN) && (n = recv(pf[1].fd, buf, sizeof buf - 1, 0)) > 0) {
                buf[n] = 0;
                pthread_mutex_lock(&lk); kiosk_packet((const char *)buf, now_ms()); pthread_mutex_unlock(&lk);
            }
        }
        struct push q[NPEER]; int nq; long long now = now_ms();
        pthread_mutex_lock(&lk);
        if (kcopies && now >= kcopy_at) { kiosk_send(); kcopy_at += KIOSK_GAP_MS; kcopies--; }      /* Wi-Fi drops broadcasts */
        for (int i = 0; i < 2; i++) if (copies[i].left && now >= copy_at(i)) {
            struct pkt b; memcpy(b.p, copies[i].p, copies[i].n); b.n = copies[i].n; send_pkt(&b); copies[i].left--;
        }
        {
            if (!in_net) {
                int members = 0;
                for (int i = 0; i < NPEER; i++) members |= cands[i].net && ago(cands[i].seen, now, PEER_TTL_MS);
                if (!members && now - started > DISCOVER_MS) create();
                static long long hinted;
                if (members && now - started > 60000 && !ago(hinted, now, 600000)) {
                    hinted = now;
                    for (int i = 0; i < NPEER; i++) if (cands[i].net && ago(cands[i].seen, now, PEER_TTL_MS))
                        fprintf(stderr, "arbitration: %s has not handed us its network yet: Home Assistant %s, and %s may not perform "
                                "Home Assistant actions; hold Volume up and Volume down on both Echos to pair them\n", cands[i].node,
                                cands[i].attested ? "confirmed its tag, maybe not ours" : "has not confirmed its tag (no tag integration?)", cands[i].node);
                }
            }
            for (int i = 0; i < NPEER; i++) if (ago(cands[i].seen, now, PEER_TTL_MS)) push(&cands[i], now);
            if (!ago(beacon_at, now, in_net ? beacon_gap : now - started < DISCOVER_MS ? LOOK_MS : LONER_MS)) {
                uint32_t r; ws_random(&r, sizeof r);
                beacon_at = now; beacon_gap = BEACON_MIN_MS + r % (BEACON_MAX_MS - BEACON_MIN_MS); beacon(0);
            }
            /* Home Assistant: our tag scanned while another Echo may wait for it (we are outside a network, or one is
             * outside ours: it gives or takes); the tags of those not confirmed yet */
            int need = !in_net;
            for (int i = 0; i < NPEER; i++) need |= ago(cands[i].seen, now, PEER_TTL_MS);
            if (!need) scans = 0;
            else if (!ago(scan_at, now, scans < SCAN_N ? SCAN_MS : SCAN_SLOW_MS)) { scan_at = now; scans++; scan_due = 1; }
            for (int i = 0; i < NPEER; i++) {
                struct cand *c = &cands[i];
                int give = in_net && (!c->net || c->net > net_id), get = c->net && (!in_net || c->net < net_id);
                if (!c->attested && ago(c->seen, now, PEER_TTL_MS) && (give || get) && !ago(c->polled, now, POLL_MS)) { c->polled = now; poll_tag(c->pub); }
            }
        }
        if (pair_until && now >= pair_until) {
            pair_until = 0;
            if (!pair_gave) { fprintf(stderr, "arbitration: pairing ended, no Echo to pair with\n"); atomic_store(&pair_event, -1); }
        }
        if (pair_until) {
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
        char pq[NPEER][TLEN]; int npq = npoll; memcpy(pq, polls, sizeof pq[0] * (size_t)npq); npoll = 0;
        int sd = scan_due; scan_due = 0;
        int np = count_peers(now);
        if (np != reported_peers) { reported_peers = np; atomic_store(&notify, 1); }
        pthread_mutex_unlock(&lk);
        for (int i = 0; i < nq; i++) {
            int r = hooks->send_key(q[i].node, q[i].net, q[i].key);
            fprintf(stderr, "arbitration: %s network %s to %s through Home Assistant\n", r < 0 ? "cannot hand (no link)" : "handing", q[i].net, q[i].node);
        }
        crypto_wipe(q, sizeof q);
        for (int i = 0; i < npq; i++) if (hooks->request) hooks->request(pq[i]);
        if (sd && hooks->scan) hooks->scan(self_tag);
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

void arb_ha_state(const char *entity, const char *state)
{
    char t[TLEN];
    /* "unknown" (made by hand, never scanned) and "unavailable" are no scans; HA sends nothing for a missing entity */
    if (!*state || !strcmp(state, "unknown") || !strcmp(state, "unavailable")) return;
    pthread_mutex_lock(&lk);
    for (int i = 0; running && i < NPEER; i++) {
        struct cand *c = &cands[i];
        if (!c->seen || c->attested) continue;
        tag_entity(t, c->pub);
        if (strcmp(t, entity)) continue;
        if (!c->tag0[0]) snprintf(c->tag0, sizeof c->tag0, "%s", state);
        else if (strncmp(c->tag0, state, sizeof c->tag0 - 1)) {
            c->attested = 1; c->pushed = 0;
            fprintf(stderr, "arbitration: Home Assistant confirms %s's key (its tag was scanned at %s)\n", c->node, state);
        }
    }
    pthread_mutex_unlock(&lk);
}

int arb_pair(void)
{
    long long now = now_ms();
    pthread_mutex_lock(&lk);
    if (!running) { pthread_mutex_unlock(&lk); return -1; }
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
    const char *why;
    pthread_mutex_lock(&lk);
    crypto_blake2b(round_.kw, 8, low, n);
    round_.kiosk = mode == ARB_KIOSK; round_.at = now;
    if (round_.kiosk) {
        due = kiosk_claim(keyword, score, now);
        why = due ? " (Kiosk Satellite mode)" : !arbitrate ? ", arbitration off" : ", kiosk port not open";
    } else if (running && arbitrate && in_net && count_peers(now)) {
        round_.score = score; round_.prio = prio;
        claim_pkt(&b, score, prio, 0); send_spread(&b, now);
        due = now + WINDOW_MS; why = "";
    } else why = !arbitrate ? ", arbitration off" : ", no other Echo to ask";
    pthread_mutex_unlock(&lk);
    fprintf(stderr, "arbitration: heard it, score %d%s%s\n", score, prio && !round_.kiosk ? " (in a conversation)" : "", why);
    return due;
}

int arb_decide(void)
{
    int win = 1; struct pkt b;
    pthread_mutex_lock(&lk);
    if (round_.kiosk) {
        win = kiosk_decide();
        pthread_mutex_unlock(&lk);
        fprintf(stderr, "arbitration: %s\n", win ? "this Echo answers" : "another device answers");
        return win;
    }
    for (int i = 0; i < NCLAIM; i++) {
        struct claim *k = &claims[i];
        if (!k->at || k->at < round_.at - LOOKBACK_MS || memcmp(k->kw, round_.kw, 8)) continue;     /* another round, another keyword */
        if (k->won || k->prio > round_.prio || (k->prio == round_.prio && (k->score > round_.score
                                                                          || (k->score == round_.score && memcmp(k->id, pk, 8) > 0)))) win = 0;
    }
    if (win && in_net) { claim_pkt(&b, round_.score, round_.prio, 1); send_spread(&b, now_ms()); }
    pthread_mutex_unlock(&lk);
    fprintf(stderr, "arbitration: %s\n", win ? "this Echo answers" : "another Echo answers");
    return win;
}

int arb_arbitrate(int set)
{
    pthread_mutex_lock(&lk);
    if (set >= 0 && set != arbitrate) {
        arbitrate = set;
        fprintf(stderr, "arbitration: %s\n", arbitrate ? "takes part in rounds again" : "off: answers every wake word, stays in the network");
        memset(claims, 0, sizeof claims);
        beacon_at = 0;                              /* the members learn it now, not in 30 s */
        save(); atomic_store(&notify, 1);
    }
    int r = arbitrate;
    pthread_mutex_unlock(&lk);
    return r;
}

int arb_mode(int set)
{
    pthread_mutex_lock(&lk);
    if ((set == ARB_HASSMIC || set == ARB_KIOSK) && set != mode) {
        mode = set;
        if (running) fprintf(stderr, "arbitration: %s\n", mode == ARB_KIOSK ? "Kiosk Satellite mode: loudness only, with kiosks too"
                                                                           : "our own mode: between the Echos of the network");
        memset(claims, 0, sizeof claims); memset(kclaims, 0, sizeof kclaims);
        beacon_at = 0; atomic_store(&notify, 1);        /* the members learn it now, not in 30 s */
    }
    int r = mode;
    pthread_mutex_unlock(&lk);
    return r;
}

int arb_offset(int set)
{
    pthread_mutex_lock(&lk);
    if (set >= -20 && set <= 20) koffset = set;
    int r = koffset;
    pthread_mutex_unlock(&lk);
    return r;
}

int arb_window(int set)
{
    pthread_mutex_lock(&lk);
    if (set >= 100 && set <= 500) kwindow = set;
    int r = kwindow;
    pthread_mutex_unlock(&lk);
    return r;
}

static size_t jstr(char *o, size_t cap, const char *s)          /* node names: [a-z0-9_.-] only */
{
    size_t n = 0;
    for (; *s && n + 1 < cap; s++) if (isalnum((unsigned char)*s) || *s == '-' || *s == '_' || *s == '.') o[n++] = *s;
    o[n] = 0;
    return n;
}

size_t arb_status_json(char *o, size_t cap)
{
    size_t n = 0; long long now = now_ms(); char t[NLEN], ip[INET_ADDRSTRLEN]; int k = 0;
#define J(...) do { if (n < cap) n += (size_t)snprintf(o + n, cap - n, __VA_ARGS__); } while (0)
    pthread_mutex_lock(&lk);
    J("{\"running\":%s,\"arbitrates\":%s,\"node\":\"%s\",\"mode\":\"%s\",\"kiosk_port\":%s,", running ? "true" : "false",
      arbitrate ? "true" : "false", node, mode == ARB_KIOSK ? "kiosk" : "hassmic", ksock >= 0 ? "true" : "false");
    if (kheard) J("\"kiosk_heard_s\":%lld,", (now - kheard) / 1000); else J("\"kiosk_heard_s\":null,");
    if (in_net) J("\"network\":\"%016llx\",", (unsigned long long)net_id); else J("\"network\":null,");
    J("\"tag\":\"tag.%s\",\"pairing\":%s,\"members\":[", self_tag, pair_until && now < pair_until ? "true" : "false");
    for (int i = 0; i < NPEER; i++) if (ago(peers[i].seen, now, PEER_TTL_MS)) {
        struct in_addr a = { peers[i].ip }; inet_ntop(AF_INET, &a, ip, sizeof ip); jstr(t, sizeof t, peers[i].node);
        /* quiet with the kiosk flag: it takes part, the kiosk way */
        J("%s{\"node\":\"%s\",\"ip\":\"%s\",\"seen_s\":%lld,\"arbitrates\":%s,\"mode\":\"%s\"}", k++ ? "," : "", t, peers[i].ip ? ip : "",
          (now - peers[i].seen) / 1000, peers[i].quiet && !peers[i].kiosk ? "false" : "true", peers[i].kiosk ? "kiosk" : "hassmic");
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

int arb_web_key(unsigned char out[32])
{
    int r = -1;
    pthread_mutex_lock(&lk);
    if (running && in_net) { crypto_blake2b_keyed(out, 32, net_key, 32, (const uint8_t *)"hassmic web voucher 1", 21); r = 0; }
    pthread_mutex_unlock(&lk);
    return r;
}

size_t arb_members_json(char *o, size_t cap)
{
    size_t n = 0; long long now = now_ms(); char t[NLEN], ip[INET_ADDRSTRLEN]; int k = 0;
    pthread_mutex_lock(&lk);
    n += (size_t)snprintf(o + n, cap - n, "[");
    for (int i = 0; running && in_net && i < NPEER && n < cap; i++) if (ago(peers[i].seen, now, PEER_TTL_MS) && peers[i].ip) {
        struct in_addr a = { peers[i].ip }; inet_ntop(AF_INET, &a, ip, sizeof ip); jstr(t, sizeof t, peers[i].node);
        n += (size_t)snprintf(o + n, cap - n, "%s{\"node\":\"%s\",\"ip\":\"%s\"}", k++ ? "," : "", t, ip);
    }
    if (n < cap) n += (size_t)snprintf(o + n, cap - n, "]");
    pthread_mutex_unlock(&lk);
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
    hex8(kid, pk);
    memcpy(self_tag, "hassmic_", 8);
    for (int i = 0; i < 32; i++) snprintf(self_tag + 8 + 2 * i, 3, "%02x", pk[i]);
    load();
    ctr = ctr_saved; ctr_saved += CTR_BLOCK; save();
    if ((sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)) < 0) { perror("arbitration: socket"); return -1; }
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
    memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&a, sizeof a)) { perror("arbitration: bind"); close(sock); sock = -1; return -1; }
    dest = a; dest.sin_addr.s_addr = addr ? inet_addr(addr) : htonl(INADDR_BROADCAST);
    if (pipe(wake_fd) == 0) for (int i = 0; i < 2; i++) { fcntl(wake_fd[i], F_SETFL, O_NONBLOCK); fcntl(wake_fd[i], F_SETFD, FD_CLOEXEC); }
    else wake_fd[0] = wake_fd[1] = -1;                  /* copies then go out within the loop's 100 ms */
    started = now_ms(); hello_until = started + BEACON_MAX_MS; running = 1;
    if (pthread_create(&t, NULL, loop, NULL)) { running = 0; return -1; }
    pthread_detach(t);
    fprintf(stderr, "arbitration: port %d as %s, %s%s\n", port, node, in_net ? "member of a network" : "looking for a network",
            arbitrate ? "" : ", arbitration off");
    return 0;
}
