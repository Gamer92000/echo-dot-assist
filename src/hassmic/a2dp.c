/*
 * Bluetooth speaker: an A2DP sink on BR/EDR, next to the LE proxy on the same controller (ble.c owns /dev/stpbt and its
 * thread; this module gets the BR/EDR events and ACL links through hci.h).  What stock Alexa did with btmanagerd and
 * BTSinkPlayer, without the Alexa app: Home Assistant's "Bluetooth pairing" switch makes the Echo discoverable for
 * A2DP_PAIR_SECONDS, like "Alexa, pair".
 *
 *   GAP       page scan whenever a paired device exists (it connects to us, we never page), inquiry scan only while
 *             pairing.  Class of device: audio / loudspeaker.  One source streams at a time, MAX_LINKS may be connected.
 *   pairing   Secure Simple Pairing, NoInputNoOutput (Just Works: the phone asks its user, we accept), legacy PIN 0000
 *             for old devices; both only while pairing is on.  Link keys in state/bt_keys (0600).  Connections from
 *             unknown devices are refused outside the pairing window.
 *   L2CAP     basic mode, we accept channels (SDP, AVDTP, AVCTP) and open only AVCTP, when the device has not 2 s after
 *             AVDTP (BlueZ does not always).  AVDTP and AVCTP need an encrypted link: a device that asks before
 *             encrypting gets "pending" while we authenticate and encrypt.
 *   SDP       A2DP sink 1.3 (AVDTP 1.3), speaker; AVRCP 1.5 target (category 2: absolute volume) and controller.
 *   AVRCP     the Echo's volume is the device's volume slider, both ways; play / pause to the device (action button,
 *             another music source starting on the Echo).
 *   AVDTP     one sink endpoint per codec (a2dp_codecs.c: SBC, AAC, aptX HD, aptX; Opus not, see there), the source
 *             picks; delay reporting so video stays in sync.  One stream at a time: the others show as in use meanwhile.
 *   audio     decoded into a jitter buffer, a player thread feeds the mixer's music stream.  The phone's clock and the
 *             mixer's drift apart: a frame is dropped or repeated now and then to hold the buffer at its target.
 */
#include "a2dp.h"
#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "audio.h"
#include "ble.h"
#include "core.h"
#include "hci.h"
#include "a2dp_codec.h"

#define MAX_LINKS 2
#define MAX_CHANS 6                     /* per link: SDP, AVDTP signalling + media, and room for a retry */
#define MAX_KEYS 8
#define RX_MTU 1024                     /* what we receive per L2CAP frame; phones size their media packets to it */
#define PREBUF_MS 150                   /* jitter buffer while playing: Wi-Fi shares the antenna and delays packets */
#define MIXER_US 60000                  /* kept queued in the mixer beyond that */
#define OUTPUT_MS 70                    /* mixer, DAC, amplifier (measured for Sendspin, see sendspin.c) */
#define CHUNK_MS 10

enum { H4_CMD = 1, H4_ACL = 2 };
enum { EV_CONN_COMPLETE = 0x03, EV_CONN_REQUEST, EV_DISCONNECT, EV_AUTH_COMPLETE, EV_REMOTE_NAME = 0x07, EV_ENC_CHANGE, EV_PIN_REQUEST = 0x16,
       EV_LINK_KEY_REQUEST, EV_LINK_KEY_NOTIFY, EV_KEY_REFRESH = 0x30, EV_IO_CAP_REQUEST, EV_IO_CAP_RESPONSE, EV_USER_CONFIRM,
       EV_SSP_COMPLETE = 0x36 };
enum { OP_DISCONNECT = 0x0406, OP_ACCEPT = 0x0409, OP_REJECT, OP_LINK_KEY_REPLY, OP_LINK_KEY_NEG, OP_PIN_REPLY, OP_PIN_NEG,
       OP_AUTH = 0x0411, OP_ENCRYPT = 0x0413, OP_REMOTE_NAME = 0x0419, OP_IO_CAP_REPLY = 0x042b, OP_CONFIRM_REPLY, OP_CONFIRM_NEG, OP_IO_CAP_NEG = 0x0434,
       OP_LINK_POLICY = 0x080f, OP_LOCAL_NAME = 0x0c13, OP_SCAN_ENABLE = 0x0c1a, OP_CLASS = 0x0c24, OP_INQUIRY_MODE = 0x0c45,
       OP_EIR = 0x0c52, OP_SSP_MODE = 0x0c56, OP_READ_BUFFER = 0x1005 };
enum { PSM_SDP = 0x0001, PSM_AVCTP = 0x0017, PSM_AVDTP = 0x0019 };

static void (*notify)(void);
static int started;

static long long ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static unsigned u16(const unsigned char *p) { return p[0] | p[1] << 8; }
static void put16(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; }
static uint64_t addr_of(const unsigned char *p) { uint64_t a = 0; for (int i = 5; i >= 0; i--) a = a << 8 | p[i]; return a; }

/* ---------------------------------------------------------------- link keys */

static struct key { uint64_t addr; unsigned type; unsigned char k[16]; } keys[MAX_KEYS];
static int nkeys;

static const char *keys_path(void)
{
    static char p[256]; const char *d = getenv("HASSMIC_STATE");
    snprintf(p, sizeof p, "%s/bt_keys", d ? d : "/data/local/hassmic/state");
    return p;
}

static void keys_load(void)                     /* one line per device: address type key (hex) */
{
    char l[128], h[40]; unsigned long long a; FILE *f = fopen(keys_path(), "r");
    if (!f) return;
    while (nkeys < MAX_KEYS && fgets(l, sizeof l, f)) {
        struct key *k = &keys[nkeys]; int ok = sscanf(l, "%llx %u %39s", &a, &k->type, h) == 3 && strlen(h) == 32;
        for (int i = 0; ok && i < 16; i++) ok = sscanf(h + 2 * i, "%2hhx", &k->k[i]) == 1;
        if (ok) { k->addr = a; nkeys++; }
    }
    fclose(f);
    if (nkeys) fprintf(stderr, "a2dp: %d paired source%s\n", nkeys, nkeys == 1 ? "" : "s");
}

static void keys_save(void)
{
    char tmp[300]; snprintf(tmp, sizeof tmp, "%s.tmp", keys_path());
    int fdw = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600); FILE *f = fdw >= 0 ? fdopen(fdw, "w") : NULL;
    if (!f) { if (fdw >= 0) close(fdw); fprintf(stderr, "a2dp: cannot write %s\n", tmp); return; }
    for (int i = 0; i < nkeys; i++) {
        fprintf(f, "%012llx %u ", (unsigned long long)keys[i].addr, keys[i].type);
        for (int j = 0; j < 16; j++) fprintf(f, "%02x", keys[i].k[j]);
        fputc('\n', f);
    }
    if (fclose(f) || rename(tmp, keys_path())) { unlink(tmp); fprintf(stderr, "a2dp: cannot write %s\n", keys_path()); }
}

static struct key *key_for(uint64_t a) { for (int i = 0; i < nkeys; i++) if (keys[i].addr == a) return &keys[i]; return NULL; }

static void key_forget(uint64_t a)
{
    struct key *k = key_for(a);
    if (!k) return;
    fprintf(stderr, "a2dp: forgetting %012llx\n", (unsigned long long)a);
    *k = keys[--nkeys]; keys_save();
}

/* ---------------------------------------------------------------- pairing window, scan mode */

static atomic_int pair_req = -1, pairing_on;    /* request from other threads (-1 none); current state */
static long long pair_until;
static int scan_mode = -1;                      /* Write Scan Enable as sent: 0 off, 2 page, 3 inquiry + page */

static void set_pairing(int on)
{
    pair_until = on ? ms() + A2DP_PAIR_SECONDS * 1000LL : 0;
    if (atomic_exchange(&pairing_on, on) != on) {
        fprintf(stderr, "a2dp: pairing %s\n", on ? "on" : "off");
        if (notify) notify();
    }
}

/* ---------------------------------------------------------------- commands asked for by events (sent in upkeep) */

static struct { unsigned op, n; unsigned char p[24]; } pend[16];
static int npend;

static void later(unsigned op, const void *p, unsigned n)
{
    if (npend == (int)(sizeof pend / sizeof *pend) || n > sizeof pend[0].p) { fprintf(stderr, "a2dp: command %04x dropped\n", op); return; }
    pend[npend].op = op; pend[npend].n = n; memcpy(pend[npend].p, p, n); npend++;
}

static void later_addr(unsigned op, const unsigned char *addr, const void *extra, unsigned n)
{
    unsigned char b[24]; memcpy(b, addr, 6); if (n) memcpy(b + 6, extra, n);
    later(op, b, 6 + n);
}

/* ---------------------------------------------------------------- links, ACL, L2CAP */

struct chan { int used, psm, lcid, rcid, rmtu, cfg_in, cfg_out, pending, pend_id, out_id; };   /* out_id: ours, awaiting the answer */
static struct link {
    int used, handle, enc, auth_sent; uint64_t addr;
    long long auth_at, pend_until;              /* AVDTP waits for encryption: when we authenticate ourselves, give up */
    unsigned char rx[4 + 2048]; size_t rxlen, rxwant;
    struct chan ch[MAX_CHANS]; int next_id, av_sig, unacked;    /* av_sig: our CID of the AVDTP signalling channel, 0 = none */
    long long avctp_at;                         /* when we open AVRCP ourselves if the device has not (0: done / not due) */
    int avctp, av_label, vol_label, vol_pct;    /* AVRCP: our CID, 0 = none; next transaction label of ours; label of the
                                                   device's volume notification (-1: none registered), the volume it knows */
    char name[80]; int named, told;             /* the device's name (Remote Name Request), asked yet; connect announced */
} links[MAX_LINKS];

static unsigned acl_len = 27, acl_num = 1; static int credits;
struct frag { struct frag *next; int handle; size_t len; unsigned char b[]; };
static struct frag *fhead, **ftail = &fhead;

static struct link *link_by(int handle) { for (int i = 0; i < MAX_LINKS; i++) if (links[i].used && links[i].handle == handle) return &links[i]; return NULL; }
static struct chan *chan_by(struct link *l, int lcid) { for (int i = 0; i < MAX_CHANS; i++) if (l->ch[i].used && l->ch[i].lcid == lcid) return &l->ch[i]; return NULL; }

static void acl_flush(void)
{
    while (fhead && credits > 0) {
        struct frag *f = fhead; struct link *l = link_by(f->handle);
        if (!(fhead = f->next)) ftail = &fhead;
        if (l) { if (hci_write(f->b, f->len) < 0) fprintf(stderr, "a2dp: ACL write failed\n"); else { credits--; l->unacked++; } }
        free(f);
    }
}

static void l2_send(struct link *l, unsigned cid, const void *pdu, size_t n)
{
    unsigned char fr[4 + 1024];
    if (n > sizeof fr - 4) return;
    put16(fr, n); put16(fr + 2, cid); memcpy(fr + 4, pdu, n); n += 4;
    for (size_t o = 0; o < n; o += acl_len) {
        size_t k = n - o < acl_len ? n - o : acl_len;
        struct frag *f = malloc(sizeof *f + 5 + k);
        if (!f) return;
        f->next = NULL; f->handle = l->handle; f->len = 5 + k;
        f->b[0] = H4_ACL; f->b[1] = l->handle; f->b[2] = (l->handle >> 8 & 0x0f) | (o ? 0x10 : 0x20);   /* continuing / first */
        put16(f->b + 3, k); memcpy(f->b + 5, fr + o, k);
        *ftail = f; ftail = &f->next;
    }
    acl_flush();
}

static void sig_send(struct link *l, unsigned code, unsigned id, const void *data, size_t n)
{
    unsigned char b[64];
    if (n > sizeof b - 4) return;
    b[0] = code; b[1] = id; put16(b + 2, n); memcpy(b + 4, data, n);
    l2_send(l, 0x0001, b, 4 + n);
}

static void config_request(struct link *l, struct chan *c)
{
    unsigned char d[8]; put16(d, c->rcid); put16(d + 2, 0); d[4] = 0x01; d[5] = 2; put16(d + 6, RX_MTU);   /* option MTU */
    if (++l->next_id > 255) l->next_id = 1;
    sig_send(l, 0x04, l->next_id, d, 8);
}

static void conn_response(struct link *l, unsigned id, unsigned lcid, unsigned rcid, unsigned result, unsigned status)
{
    unsigned char d[8]; put16(d, lcid); put16(d + 2, rcid); put16(d + 4, result); put16(d + 6, status);
    sig_send(l, 0x03, id, d, 8);
}

static void chan_opened(struct link *l, struct chan *c);
static void chan_closed(struct link *l, struct chan *c);

/* pending AVDTP channels once security is settled: go on, or refuse (security block) */
static void pending_resolve(struct link *l, int ok)
{
    for (int i = 0; i < MAX_CHANS; i++) {
        struct chan *c = &l->ch[i];
        if (!c->used || !c->pending) continue;
        c->pending = 0;
        conn_response(l, c->pend_id, ok ? c->lcid : 0, c->rcid, ok ? 0 : 3, 0);
        if (ok) config_request(l, c); else memset(c, 0, sizeof *c);
    }
    l->auth_at = l->pend_until = 0; l->auth_sent = 0;
}

static void conn_request(struct link *l, unsigned id, const unsigned char *d, size_t n)
{
    if (n < 4) return;
    unsigned psm = u16(d), rcid = u16(d + 2); struct chan *c = NULL;
    if (psm != PSM_SDP && psm != PSM_AVDTP && psm != PSM_AVCTP) { conn_response(l, id, 0, rcid, 2, 0); return; }  /* not supported */
    for (int i = 0; i < MAX_CHANS && !c; i++) if (!l->ch[i].used) c = &l->ch[i];
    if (!c) { conn_response(l, id, 0, rcid, 4, 0); return; }                                           /* no resources */
    memset(c, 0, sizeof *c);
    c->used = 1; c->psm = psm; c->rcid = rcid; c->rmtu = 672; c->lcid = 0x40 + (int)(c - l->ch) + 8 * (int)(l - links);
    if (psm != PSM_SDP && !l->enc) {                       /* authenticated + encrypted first (security mode 4) */
        c->pending = 1; c->pend_id = id;
        if (!l->pend_until) { l->auth_at = ms() + 1000; l->pend_until = ms() + 15000; }   /* the phone may be at it already */
        conn_response(l, id, c->lcid, rcid, 1, 1);           /* pending, authentication pending */
        return;
    }
    conn_response(l, id, c->lcid, rcid, 0, 0);
    config_request(l, c);
}

static void config_req_rx(struct link *l, unsigned id, const unsigned char *d, size_t n)
{
    if (n < 4) return;
    struct chan *c = chan_by(l, u16(d)); unsigned flags = u16(d + 2); unsigned char r[48]; size_t rn = 6;
    if (!c) { unsigned char e[6] = { 0x02, 0x00 }; put16(e + 2, u16(d)); put16(e + 4, 0); sig_send(l, 0x01, id, e, 6); return; }  /* invalid CID */
    put16(r, c->rcid); put16(r + 2, flags & 1); put16(r + 4, 0);
    for (size_t i = 4; i + 2 <= n && i + 2 + d[i + 1] <= n; i += 2 + d[i + 1]) {
        unsigned type = d[i] & 0x7f, len = d[i + 1];
        if (type == 0x01 && len >= 2) c->rmtu = u16(d + i + 2);
        else if (type == 0x04 && len >= 1 && d[i + 2] != 0) {                      /* not basic mode: offer basic */
            static const unsigned char basic[11] = { 0x04, 9 };
            put16(r + 4, 1); memcpy(r + 6, basic, 11); rn = 17; break;
        } else if (type != 0x02 && type != 0x03 && type != 0x05 && !(d[i] & 0x80) && rn + 2 + len <= sizeof r) {   /* unknown, no hint */
            put16(r + 4, 3); memcpy(r + rn, d + i, 2 + len); rn += 2 + len;
        }
    }
    sig_send(l, 0x05, id, r, rn);
    if (!u16(r + 4) && !(flags & 1) && !c->cfg_in) { c->cfg_in = 1; if (c->cfg_out) chan_opened(l, c); }
}

static void chan_free(struct link *l, struct chan *c)
{
    if (c->cfg_in && c->cfg_out) chan_closed(l, c);
    memset(c, 0, sizeof *c);
}

static void sig_rx(struct link *l, const unsigned char *p, size_t n)
{
    while (n >= 4) {
        unsigned code = p[0], id = p[1]; size_t len = u16(p + 2);
        if (len > n - 4) return;
        const unsigned char *d = p + 4; unsigned char r[8]; struct chan *c;
        switch (code) {
        case 0x02: conn_request(l, id, d, len); break;
        case 0x04: config_req_rx(l, id, d, len); break;
        case 0x05:                                          /* Configuration Response to ours */
            if (len >= 6 && (c = chan_by(l, u16(d)))) {
                if (u16(d + 4) == 0) { if (!c->cfg_out) { c->cfg_out = 1; if (c->cfg_in) chan_opened(l, c); } }
                else if (!c->cfg_out) {                     /* our MTU refused: take the default */
                    unsigned char q[4]; put16(q, c->rcid); put16(q + 2, 0);
                    if (++l->next_id > 255) l->next_id = 1;
                    sig_send(l, 0x04, l->next_id, q, 4);
                }
            }
            break;
        case 0x06:                                          /* Disconnection Request */
            if (len >= 4) {
                memcpy(r, d, 4); sig_send(l, 0x07, id, r, 4);
                if ((c = chan_by(l, u16(d)))) chan_free(l, c);
            }
            break;
        case 0x08: sig_send(l, 0x09, id, d, len < 48 ? len : 48); break;           /* Echo */
        case 0x0a:                                          /* Information Request */
            if (len >= 2) {
                unsigned char q[8]; unsigned t = u16(d); put16(q, t);
                if (t == 2) { put16(q + 2, 0); memset(q + 4, 0, 4); sig_send(l, 0x0b, id, q, 8); }     /* no extended features */
                else { put16(q + 2, 1); sig_send(l, 0x0b, id, q, 4); }                                  /* not supported */
            }
            break;
        case 0x03:                                          /* Connection Response to ours: dcid, scid, result */
            for (int i = 0; len >= 8 && i < MAX_CHANS; i++) {
                c = &l->ch[i];
                if (!c->used || c->out_id != (int)id || c->lcid != (int)u16(d + 2)) continue;
                if (u16(d + 4) == 1) break;                 /* pending: the final answer follows */
                c->out_id = 0;
                if (u16(d + 4) == 0) { c->rcid = u16(d); config_request(l, c); }
                else { fprintf(stderr, "a2dp: %012llx refused our channel (%u)\n", (unsigned long long)l->addr, u16(d + 4)); memset(c, 0, sizeof *c); }
            }
            break;
        case 0x01:                                          /* Command Reject: of our connection request, maybe */
            for (int i = 0; i < MAX_CHANS; i++) if (l->ch[i].used && l->ch[i].out_id == (int)id) memset(&l->ch[i], 0, sizeof l->ch[i]);
            break;
        case 0x07: case 0x09: case 0x0b: break;             /* responses */
        default: r[0] = 0; r[1] = 0; sig_send(l, 0x01, id, r, 2); break;          /* Command Reject: not understood */
        }
        p += 4 + len; n -= 4 + len;
    }
}

/* ---------------------------------------------------------------- SDP server */

struct attr { unsigned id; const unsigned char *v; unsigned n; };
#define A(id, ...) { id, (const unsigned char[]){ __VA_ARGS__ }, sizeof (const unsigned char[]){ __VA_ARGS__ } }
static const unsigned char a_browse[] = { 0x35, 0x03, 0x19, 0x10, 0x02 },                  /* PublicBrowseRoot */
    a_avctp[] = { 0x35, 0x10, 0x35, 0x06, 0x19, 0x01, 0x00, 0x09, 0x00, 0x17,               /* L2CAP, PSM AVCTP */
                  0x35, 0x06, 0x19, 0x00, 0x17, 0x09, 0x01, 0x04 },                           /* AVCTP 1.4 */
    a_avrcp[] = { 0x35, 0x08, 0x35, 0x06, 0x19, 0x11, 0x0e, 0x09, 0x01, 0x05 };             /* AVRCP 1.5 */
static const struct attr r_sink[] = {                  /* A2DP sink 1.3, speaker */
    A(0x0000, 0x0a, 0x00, 0x01, 0x00, 0x01), A(0x0001, 0x35, 0x03, 0x19, 0x11, 0x0b),
    A(0x0004, 0x35, 0x10, 0x35, 0x06, 0x19, 0x01, 0x00, 0x09, 0x00, 0x19, 0x35, 0x06, 0x19, 0x00, 0x19, 0x09, 0x01, 0x03),
    { 0x0005, a_browse, sizeof a_browse }, A(0x0006, 0x35, 0x09, 0x09, 0x65, 0x6e, 0x09, 0x00, 0x6a, 0x09, 0x01, 0x00),
    A(0x0009, 0x35, 0x08, 0x35, 0x06, 0x19, 0x11, 0x0d, 0x09, 0x01, 0x03),
    A(0x0100, 0x25, 0x0a, 'A', 'u', 'd', 'i', 'o', ' ', 'S', 'i', 'n', 'k'), A(0x0311, 0x09, 0x00, 0x02),
}, r_target[] = {                                       /* AVRCP target, category 2 (amplifier): absolute volume */
    A(0x0000, 0x0a, 0x00, 0x01, 0x00, 0x02), A(0x0001, 0x35, 0x03, 0x19, 0x11, 0x0c), { 0x0004, a_avctp, sizeof a_avctp },
    { 0x0005, a_browse, sizeof a_browse }, { 0x0009, a_avrcp, sizeof a_avrcp }, A(0x0311, 0x09, 0x00, 0x02),
}, r_control[] = {                                      /* AVRCP controller, category 1: play / pause the phone */
    A(0x0000, 0x0a, 0x00, 0x01, 0x00, 0x03), A(0x0001, 0x35, 0x06, 0x19, 0x11, 0x0e, 0x19, 0x11, 0x0f),
    { 0x0004, a_avctp, sizeof a_avctp }, { 0x0005, a_browse, sizeof a_browse }, { 0x0009, a_avrcp, sizeof a_avrcp },
    A(0x0311, 0x09, 0x00, 0x01),
};
#undef A
static const struct { unsigned handle; const struct attr *a; int n; unsigned uuids[6]; } records[] = {
    { 0x00010001, r_sink, sizeof r_sink / sizeof *r_sink, { 0x110b, 0x0100, 0x0019, 0x1002, 0x110d } },
    { 0x00010002, r_target, sizeof r_target / sizeof *r_target, { 0x110c, 0x0100, 0x0017, 0x1002, 0x110e } },
    { 0x00010003, r_control, sizeof r_control / sizeof *r_control, { 0x110e, 0x110f, 0x0100, 0x0017, 0x1002 } },
};
#define NREC (int)(sizeof records / sizeof *records)

/* data element header: type, length of the data; returns the header's length, 0 if malformed */
static size_t de(const unsigned char *p, size_t n, unsigned *type, size_t *len)
{
    if (!n) return 0;
    unsigned s = p[0] & 7; *type = p[0] >> 3;
    if (*type == 0) { *len = 0; return 1; }
    if (s < 5) { *len = 1u << s; return *len + 1 <= n ? 1 : 0; }
    size_t h = s == 5 ? 2 : s == 6 ? 3 : 5;
    if (n < h) return 0;
    *len = s == 5 ? p[1] : s == 6 ? (size_t)(p[1] << 8 | p[2]) : (size_t)p[1] << 24 | p[2] << 16 | p[3] << 8 | p[4];
    return h + *len <= n ? h : 0;
}

/* a UUID element as a 16-bit value; -1 if it is not one of the Bluetooth base UUIDs */
static long uuid16(const unsigned char *p, size_t len)
{
    static const unsigned char base[12] = { 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb };
    if (len == 2) return p[0] << 8 | p[1];
    if (len == 4) return p[0] || p[1] ? -1 : p[2] << 8 | p[3];
    if (len == 16) return p[0] || p[1] || memcmp(p + 4, base, 12) ? -1 : p[2] << 8 | p[3];
    return -1;
}

/* the service search pattern: a bit per record that holds every UUID asked for; -1 = malformed */
static int sdp_match(const unsigned char **pp, size_t *np)
{
    unsigned t; size_t len, h = de(*pp, *np, &t, &len), i;
    if (!h || t != 6) return -1;
    const unsigned char *p = *pp + h; int all = (1 << NREC) - 1;
    for (i = 0; i < len; ) {
        unsigned ut; size_t ul, uh = de(p + i, len - i, &ut, &ul);
        if (!uh || ut != 3) return -1;
        long u = uuid16(p + i + uh, ul);
        for (int r = 0; r < NREC; r++) {
            int found = 0;
            for (int k = 0; k < 6; k++) found |= records[r].uuids[k] && u == (long)records[r].uuids[k];
            if (!found) all &= ~(1 << r);
        }
        i += uh + ul;
    }
    *pp += h + len; *np -= h + len;
    return all;
}

/* sequence header of n bytes of content at o: 2 or 3 bytes */
static size_t seq_head(unsigned char *o, size_t n)
{
    if (n < 256) { o[0] = 0x35; o[1] = n; return 2; }
    o[0] = 0x36; o[1] = n >> 8; o[2] = n; return 3;
}

/* record r's attributes the ID list (at p, len bytes, checked) asks for, as a data element sequence; returns its length */
static size_t sdp_record(int r, const unsigned char *p, size_t len, unsigned char *out)
{
    unsigned char body[400]; size_t o = 0;
    for (int k = 0; k < records[r].n; k++) {
        const struct attr *a = &records[r].a[k]; int want = 0;
        for (size_t i = 0; i < len; ) {
            unsigned it; size_t il, ih = de(p + i, len - i, &it, &il);
            const unsigned char *v = p + i + ih;
            if (il == 2) want |= a->id == (unsigned)(v[0] << 8 | v[1]);
            else want |= a->id >= (unsigned)(v[0] << 8 | v[1]) && a->id <= (unsigned)(v[2] << 8 | v[3]);
            i += ih + il;
        }
        if (!want || o + 3 + a->n > sizeof body) continue;
        body[o] = 0x09; body[o + 1] = a->id >> 8; body[o + 2] = a->id; memcpy(body + o + 3, a->v, a->n);
        o += 3 + a->n;
    }
    size_t h = seq_head(out, o); memcpy(out + h, body, o);
    return h + o;
}

/* the attribute ID list: checked, *p / *len its content.  0 = malformed */
static int sdp_idlist(const unsigned char **pp, size_t *np, const unsigned char **p, size_t *len)
{
    unsigned t; size_t h = de(*pp, *np, &t, len);
    if (!h || t != 6) return 0;
    *p = *pp + h;
    for (size_t i = 0; i < *len; ) {
        unsigned it; size_t il, ih = de(*p + i, *len - i, &it, &il);
        if (!ih || it != 1 || (il != 2 && il != 4)) return 0;
        i += ih + il;
    }
    *pp += h + *len; *np -= h + *len;
    return 1;
}

static void sdp_error(struct link *l, struct chan *c, const unsigned char *tid, unsigned code)
{
    unsigned char r[7] = { 0x01, tid[0], tid[1], 0, 2, code >> 8, code };
    l2_send(l, c->rcid, r, 7);
}

static void sdp_rx(struct link *l, struct chan *c, const unsigned char *p, size_t n)
{
    unsigned char full[1200], r[1024]; size_t fn = 0, rn;
    if (n < 5) return;
    unsigned pdu = p[0]; const unsigned char *tid = p + 1, *q = p + 5, *ids; size_t qn = (size_t)(p[3] << 8 | p[4]), idn;
    if (qn > n - 5) { sdp_error(l, c, tid, 0x0004); return; }                                          /* invalid PDU size */
    if (pdu == 0x02) {                                      /* ServiceSearch: the matching record handles */
        int m = sdp_match(&q, &qn);
        if (m < 0 || qn < 2) { sdp_error(l, c, tid, 0x0003); return; }
        unsigned max = q[0] << 8 | q[1], cnt = 0;
        rn = 9;
        for (int i = 0; i < NREC; i++) if (m >> i & 1 && cnt < max) {
            unsigned h = records[i].handle; r[rn++] = h >> 24; r[rn++] = h >> 16; r[rn++] = h >> 8; r[rn++] = h; cnt++;
        }
        r[rn++] = 0;                                        /* no continuation */
        r[0] = 0x03; r[1] = tid[0]; r[2] = tid[1]; r[3] = (rn - 5) >> 8; r[4] = rn - 5;
        r[5] = 0; r[6] = cnt; r[7] = 0; r[8] = cnt;
        l2_send(l, c->rcid, r, rn);
        return;
    }
    if (pdu != 0x04 && pdu != 0x06) { sdp_error(l, c, tid, 0x0003); return; }
    int m = 0;
    if (pdu == 0x04) {                                      /* ServiceAttribute: handle, max, IDs, continuation */
        if (qn < 4) { sdp_error(l, c, tid, 0x0003); return; }
        unsigned h = (unsigned)q[0] << 24 | q[1] << 16 | q[2] << 8 | q[3];
        for (int i = 0; i < NREC; i++) if (records[i].handle == h) m = 1 << i;
        if (!m) { sdp_error(l, c, tid, 0x0002); return; }
        q += 4; qn -= 4;
    } else if ((m = sdp_match(&q, &qn)) < 0) { sdp_error(l, c, tid, 0x0003); return; }
    if (qn < 2) { sdp_error(l, c, tid, 0x0003); return; }
    unsigned max = q[0] << 8 | q[1]; q += 2; qn -= 2;
    if (!sdp_idlist(&q, &qn, &ids, &idn) || qn < 1) { sdp_error(l, c, tid, 0x0003); return; }
    if (pdu == 0x04) fn = sdp_record(__builtin_ctz(m), ids, idn, full);
    else {                                                  /* a sequence of records */
        unsigned char recs[1100]; size_t rl = 0;
        for (int i = 0; i < NREC; i++) if (m >> i & 1) rl += sdp_record(i, ids, idn, recs + rl);
        fn = seq_head(full, rl); memcpy(full + fn, recs, rl); fn += rl;
    }
    size_t off = 0;
    if (q[0] == 2 && qn >= 3) off = q[1] << 8 | q[2];       /* our continuation state: the offset reached */
    if (off > fn) { sdp_error(l, c, tid, 0x0005); return; }  /* invalid continuation state */
    size_t k = fn - off, lim = c->rmtu > 16 ? c->rmtu - 10 : 16;
    if (max < 7) max = 7;
    if (k > max) k = max;
    if (k > lim) k = lim;
    if (k > sizeof r - 10) k = sizeof r - 10;
    rn = 0; r[rn++] = pdu + 1; r[rn++] = tid[0]; r[rn++] = tid[1]; rn += 2;
    r[rn++] = k >> 8; r[rn++] = k; memcpy(r + rn, full + off, k); rn += k;
    if (off + k < fn) { r[rn++] = 2; r[rn++] = (off + k) >> 8; r[rn++] = off + k; } else r[rn++] = 0;
    r[3] = (rn - 5) >> 8; r[4] = rn - 5;
    l2_send(l, c->rcid, r, rn);
}

/* ---------------------------------------------------------------- audio: jitter buffer and player */

#define RING_FRAMES 48000                               /* 1 s of stereo at 48 kHz */
static int16_t ring[RING_FRAMES * 2];
static size_t r_head, r_count; static unsigned r_rate;  /* r_lock */
static long r_dropped;
static pthread_mutex_t r_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t r_cond = PTHREAD_COND_INITIALIZER;
static atomic_int streaming;                            /* AVDTP says the source plays */
static atomic_int gap_max, gaps_long, dry_count;       /* per log interval: longest pause between media packets (ms),
                                                           pauses over 100 ms, times the buffer ran dry */

static atomic_int yielded;                              /* another source took over and the device cannot be paused */
static atomic_llong button_paused_at;                   /* the action button paused the device (AVRCP) */

static void ring_push(const int16_t *pcm, unsigned frames, unsigned ch, unsigned rate)
{
    pthread_mutex_lock(&r_lock);
    if (rate != r_rate) { r_rate = rate; r_count = 0; }
    for (unsigned i = 0; i < frames; i++) {
        if (r_count == RING_FRAMES) { r_dropped++; continue; }
        size_t at = (r_head + r_count) % RING_FRAMES * 2;
        ring[at] = pcm[i * ch]; ring[at + 1] = pcm[i * ch + ch - 1];      /* mono: both sides */
        r_count++;
    }
    pthread_cond_signal(&r_cond);
    pthread_mutex_unlock(&r_lock);
}

static void *player(void *arg)
{
    enum { MAXCHUNK = 48000 * CHUNK_MS / 1000 };
    int16_t buf[MAXCHUNK * 2], in[(MAXCHUNK + 8) * 2], prev[2] = { 0, 0 };
    unsigned rate = 0; int open = 0, primed = 0, peak = 0; long long quiet = 0, stat = 0;
    double avg = 0, base = 1, acc = 0;                  /* buffer level (ms); drift estimate; fractional input frames */
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&r_lock);
        long wait_ns = open || atomic_load(&streaming) ? 20000000 : 500000000;
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += wait_ns; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        /* start once the jitter buffer and the mixer's share are there: the mixer takes its share at once */
        unsigned rr = r_rate; size_t prime = (size_t)rr * (PREBUF_MS + MIXER_US / 1000) / 1000, want = primed ? 1 : prime;
        if (r_count < want || !rr) pthread_cond_timedwait(&r_cond, &r_lock, &ts);
        rr = r_rate;
        size_t chunk = (size_t)rr * CHUNK_MS / 1000, have = r_count;
        prime = (size_t)rr * (PREBUF_MS + MIXER_US / 1000) / 1000;
        if (!rr || (!primed && have < prime) || !have) {
            pthread_mutex_unlock(&r_lock);
            if (primed && !have) { primed = 0; if (atomic_load(&streaming)) { atomic_fetch_add(&dry_count, 1); fprintf(stderr, "a2dp: buffer ran dry\n"); } }
            if (open && !atomic_load(&streaming) && (quiet ? ms() - quiet > 3000 : (quiet = ms(), 0))) {
                bt_close(); open = 0; fprintf(stderr, "a2dp: output closed\n");
            }
            continue;
        }
        if (!primed) { primed = 1; avg = PREBUF_MS; acc = 0; prev[0] = ring[r_head * 2]; prev[1] = ring[r_head * 2 + 1]; }
        quiet = 0;
        /* The source's clock against the mixer's (a phone was 0.2 % fast): play the input slightly faster or slower,
         * resampled linearly, so the buffer stays near PREBUF_MS.  base follows the long-term drift, the error term the
         * rest; both stay far below audible pitch changes. */
        avg = 0.98 * avg + 0.02 * ((double)have * 1000 / rr);
        double err = avg - PREBUF_MS, ratio;
        base += err * 1e-7; base = base < 0.99 ? 0.99 : base > 1.01 ? 1.01 : base;
        ratio = base + (err < -100 ? -100 : err > 100 ? 100 : err) * 2e-5;
        acc += chunk * ratio;
        size_t n = (size_t)acc, out = chunk;
        if (n > MAXCHUNK + 7) n = MAXCHUNK + 7;
        if (n > have) { n = have; out = (size_t)(n / ratio); acc = 0; if (!out) out = 1; } else acc -= n;
        for (size_t i = 0; i < n; i++) { size_t at = (r_head + i) % RING_FRAMES * 2; in[2 * i] = ring[at]; in[2 * i + 1] = ring[at + 1]; }
        r_head = (r_head + n) % RING_FRAMES; r_count -= n;
        for (size_t j = 0; j < out; j++) {                  /* position in the input, prev being index -1 */
            double pos = -1 + (j + 1) * (double)n / out; long i0 = (long)floor(pos); double f = pos - i0;
            for (int c = 0; c < 2; c++) {
                double x0 = i0 < 0 ? prev[c] : in[2 * i0 + c], x1 = in[2 * (i0 + 1 < (long)n ? i0 + 1 : (long)n - 1) + c];
                buf[2 * j + c] = (int16_t)lrint(x0 + (x1 - x0) * f);
            }
        }
        if (n) { prev[0] = in[2 * n - 2]; prev[1] = in[2 * n - 1]; }
        long dropped = r_dropped; r_dropped = 0;
        pthread_mutex_unlock(&r_lock);
        if (dropped) fprintf(stderr, "a2dp: buffer full, %ld frames dropped\n", dropped);

        if (open && rate != rr) { bt_close(); open = 0; }
        if (!open) {
            if (bt_open(rr, 2)) { usleep(200000); continue; }
            open = 1; rate = rr; fprintf(stderr, "a2dp: output open, %u Hz\n", rr);
        }
        if (core_state() != IDLE) for (size_t i = 0; i < 2 * out; i++) buf[i] /= 6;     /* duck under the voice assistant */
        while (bt_queued_us() > MIXER_US) usleep(5000);
        for (size_t i = 0; i < 2 * out; i++) { int v = buf[i] < 0 ? -buf[i] : buf[i]; if (v > peak) peak = v; }
        if (bt_write(buf, out * 4) < 0) fprintf(stderr, "a2dp: mixer write failed\n");
        if (ms() - stat > 60000 && atomic_load(&streaming)) {     /* peak: whether the source sends more than silence */
            stat = ms();
            fprintf(stderr, "a2dp: buffer %.0f ms, source clock %+.0f ppm, peak %.0f dBFS; packets: longest gap %d ms, %d gaps > 100 ms; ran dry %d times\n",
                    avg, (base - 1) * 1e6, 20 * log10((peak + 1) / 32768.0), atomic_exchange(&gap_max, 0), atomic_exchange(&gaps_long, 0), atomic_exchange(&dry_count, 0));
            peak = 0;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------- AVDTP */

enum { AV_DISCOVER = 1, AV_GET_CAP, AV_SET_CONF, AV_GET_CONF, AV_RECONF, AV_OPEN, AV_START, AV_CLOSE, AV_SUSPEND, AV_ABORT,
       AV_SECURITY, AV_GET_ALL_CAP, AV_DELAY };
enum { ST_IDLE, ST_CONFIGURED, ST_OPEN, ST_STREAMING };
enum { E_BAD_ACP_SEID = 0x12, E_SEP_IN_USE, E_BAD_SERV_CATEGORY = 0x17, E_NOT_SUPPORTED = 0x19, E_UNSUPPORTED_CONF = 0x29,
       E_BAD_STATE = 0x31 };

/* stream endpoints: one per codec the device can decode, SEID = index + 1 */
static int seps[8], nseps;

static struct { struct link *l; int state, media, rseid, delay, label, sep; unsigned char cfg[16]; unsigned ncfg; } av;

static const struct a2dp_codec *sep_codec(int seid) { return seid >= 1 && seid <= nseps ? &a2dp_codecs[seps[seid - 1]] : NULL; }

static void set_streaming(int on)
{
    if (atomic_exchange(&streaming, on) == on) return;
    fprintf(stderr, "a2dp: %s\n", on ? "streaming" : "stream stopped");
    core_music(MUSIC_BLUETOOTH, on);
}

static void av_reset(void)
{
    set_streaming(0);
    if (av.l) sep_codec(av.sep)->close();
    av.l = NULL; av.state = ST_IDLE; av.media = 0; av.delay = 0; av.sep = 0;
}

static void av_send(struct link *l, const void *p, size_t n) { struct chan *c = chan_by(l, l->av_sig); if (c) l2_send(l, c->rcid, p, n); }

static void av_reply(struct link *l, unsigned label, unsigned sig, int accept, const unsigned char *d, size_t n)
{
    unsigned char r[48] = { label << 4 | (accept ? 2 : 3), sig };
    if (n > sizeof r - 2) n = sizeof r - 2;
    memcpy(r + 2, d, n); av_send(l, r, 2 + n);
}

static void delay_report(void)                          /* how long our audio takes, 1/10 ms: sources delay video by it */
{
    unsigned d = (PREBUF_MS + MIXER_US / 1000 + OUTPUT_MS) * 10;
    unsigned char p[5] = { (unsigned char)((++av.label & 15) << 4), AV_DELAY, av.rseid << 2, d >> 8, d };
    av_send(av.l, p, 5);
}

/* service capabilities: media transport, media codec (audio, the codec's type and information), delay reporting */
static size_t caps_of(const struct a2dp_codec *c, const unsigned char *info, unsigned ninfo, int delay, unsigned char *o)
{
    size_t n = 0;
    o[n++] = 1; o[n++] = 0;
    o[n++] = 7; o[n++] = 2 + ninfo; o[n++] = 0x00; o[n++] = c->type; memcpy(o + n, info, ninfo); n += ninfo;
    if (delay) { o[n++] = 8; o[n++] = 0; }
    return n;
}

/* Set Configuration / Reconfigure for endpoint seid: the categories asked for.  Returns 0 or the error, *cat the category
 * at fault; the codec configuration lands in av.cfg */
static int av_config(int seid, const unsigned char *d, size_t n, unsigned *cat, int reconf)
{
    const struct a2dp_codec *codec = sep_codec(seid); int have = 0, delay = 0;
    for (size_t i = 0; i + 2 <= n; i += 2 + d[i + 1]) {
        unsigned c = d[i], len = d[i + 1];
        *cat = c;
        if (i + 2 + len > n) return E_BAD_SERV_CATEGORY;
        if (c == 1 && !reconf) continue;                    /* media transport */
        if (c == 8 && !reconf) { delay = 1; continue; }     /* delay reporting */
        if (c != 7) return reconf ? 0x1e : E_BAD_SERV_CATEGORY;     /* invalid capabilities (reconfigure) / bad category */
        const unsigned char *s = d + i + 2;
        if (len < 2 || len - 2 > sizeof av.cfg || s[0] >> 4 != 0 || s[1] != codec->type || !codec->check(s + 2, len - 2))
            return E_UNSUPPORTED_CONF;
        memcpy(av.cfg, s + 2, len - 2); av.ncfg = len - 2; have = 1;
    }
    if (!have && !reconf) { *cat = 7; return E_UNSUPPORTED_CONF; }
    if (!reconf) av.delay = delay;
    return 0;
}

static void log_config(struct link *l)
{
    char hex[40] = ""; for (unsigned i = 0; i < av.ncfg && i < 16; i++) snprintf(hex + 2 * i, 3, "%02x", av.cfg[i]);
    fprintf(stderr, "a2dp: %012llx configured %s (%s)%s\n", (unsigned long long)l->addr, sep_codec(av.sep)->name, hex,
            av.delay ? ", delay reporting" : "");
}

static void av_rx(struct link *l, const unsigned char *p, size_t n)
{
    if (n < 2) return;
    unsigned label = p[0] >> 4, type = p[0] >> 2 & 3, msg = p[0] & 3, sig = p[1] & 0x3f;
    const unsigned char *d = p + 2; size_t dn = n - 2; unsigned char r[48]; unsigned cat = 0; int e;
    if (type != 0 || msg != 0) return;                     /* fragments (never this small) and answers to our delay reports */
    int mine = av.l == l, seid = dn >= 1 ? d[0] >> 2 : 0; const struct a2dp_codec *codec = sep_codec(seid);
    int ours = mine && seid == av.sep;                      /* the endpoint this link has configured */
    switch (sig) {
    case AV_DISCOVER:
        for (int i = 0; i < nseps; i++) {                   /* in use: taken by the stream, or by another source */
            r[2 * i] = (i + 1) << 2 | (av.l && (!mine || av.sep == i + 1) ? 2 : 0);
            r[2 * i + 1] = 0 << 4 | 1 << 3;                 /* audio, sink */
        }
        av_reply(l, label, sig, 1, r, 2 * nseps);
        break;
    case AV_GET_CAP: case AV_GET_ALL_CAP:
        if (!codec) { r[0] = E_BAD_ACP_SEID; av_reply(l, label, sig, 0, r, 1); break; }
        av_reply(l, label, sig, 1, r, caps_of(codec, codec->caps, codec->ncaps, sig == AV_GET_ALL_CAP, r));
        break;
    case AV_SET_CONF:
        if (dn < 2 || !codec) { r[0] = 0; r[1] = E_BAD_ACP_SEID; av_reply(l, label, sig, 0, r, 2); break; }
        if (av.l) { r[0] = 0; r[1] = E_SEP_IN_USE; av_reply(l, label, sig, 0, r, 2); break; }
        if ((e = av_config(seid, d + 2, dn - 2, &cat, 0)) || (codec->open(av.cfg, av.ncfg) && (cat = 7, e = E_UNSUPPORTED_CONF))) {
            r[0] = cat; r[1] = e; av_reply(l, label, sig, 0, r, 2); break;
        }
        av.l = l; av.sep = seid; av.state = ST_CONFIGURED; av.rseid = d[1] >> 2;
        log_config(l);
        av_reply(l, label, sig, 1, NULL, 0);
        if (av.delay) delay_report();
        break;
    case AV_RECONF:
        if (!ours) { r[0] = E_BAD_ACP_SEID; av_reply(l, label, sig, 0, r, 1); break; }
        if (av.state != ST_OPEN) { r[0] = 0; r[1] = E_BAD_STATE; av_reply(l, label, sig, 0, r, 2); break; }
        if ((e = av_config(seid, d + 1, dn - 1, &cat, 1))) { r[0] = cat; r[1] = e; av_reply(l, label, sig, 0, r, 2); break; }
        codec->close();
        if (codec->open(av.cfg, av.ncfg)) { r[0] = 7; r[1] = E_UNSUPPORTED_CONF; av_reply(l, label, sig, 0, r, 2); av_reset(); break; }
        log_config(l);
        av_reply(l, label, sig, 1, NULL, 0);
        break;
    case AV_GET_CONF:
        if (!ours) { r[0] = E_BAD_ACP_SEID; av_reply(l, label, sig, 0, r, 1); break; }
        av_reply(l, label, sig, 1, r, caps_of(codec, av.cfg, av.ncfg, av.delay, r));
        break;
    case AV_OPEN:
        if (!ours) { r[0] = E_BAD_ACP_SEID; av_reply(l, label, sig, 0, r, 1); break; }
        if (av.state != ST_CONFIGURED) { r[0] = E_BAD_STATE; av_reply(l, label, sig, 0, r, 1); break; }
        av.state = ST_OPEN; av_reply(l, label, sig, 1, NULL, 0);        /* the media channel comes next */
        break;
    case AV_START: case AV_SUSPEND:
        if (!ours) { r[0] = dn ? d[0] : 0; r[1] = E_BAD_ACP_SEID; av_reply(l, label, sig, 0, r, 2); break; }
        if (sig == AV_START ? av.state != ST_OPEN : av.state != ST_STREAMING) {
            if ((sig == AV_START && av.state == ST_STREAMING) || (sig == AV_SUSPEND && av.state == ST_OPEN)) { av_reply(l, label, sig, 1, NULL, 0); break; }
            r[0] = d[0]; r[1] = E_BAD_STATE; av_reply(l, label, sig, 0, r, 2); break;
        }
        av.state = sig == AV_START ? ST_STREAMING : ST_OPEN;
        if (sig == AV_START) { atomic_store(&yielded, 0); atomic_store(&button_paused_at, 0); }  /* started again: wanted */
        av_reply(l, label, sig, 1, NULL, 0);
        set_streaming(sig == AV_START);
        break;
    case AV_CLOSE: case AV_ABORT:
        if (sig == AV_CLOSE && !ours) { r[0] = E_BAD_ACP_SEID; av_reply(l, label, sig, 0, r, 1); break; }
        av_reply(l, label, sig, 1, NULL, 0);
        if (mine) av_reset();                               /* the source closes the media channel itself */
        break;
    case AV_DELAY: r[0] = E_NOT_SUPPORTED; av_reply(l, label, sig, 0, r, 1); break;   /* we are the sink */
    case AV_SECURITY: r[0] = E_NOT_SUPPORTED; av_reply(l, label, sig, 0, r, 1); break;
    default: { unsigned char g[2] = { label << 4 | 1, sig }; av_send(l, g, 2); break; }     /* general reject */
    }
}

static void media_rx(const unsigned char *p, size_t n)
{
    static long long last;
    long long t = ms();
    if (last && t - last < 5000) { int g = (int)(t - last); if (g > atomic_load(&gap_max)) atomic_store(&gap_max, g); if (g > 100) atomic_fetch_add(&gaps_long, 1); }
    last = t;
    if (atomic_load(&streaming) && !atomic_load(&yielded)) sep_codec(av.sep)->packet(p, n, ring_push);
}

/* ---------------------------------------------------------------- AVRCP
 * Both roles over one AVCTP channel, which the device opens.  Target (the device controls us): absolute volume, the
 * phone's volume slider is the Echo's volume and the other way round.  Controller (we control the device): play and
 * pause as pass-through commands, like the buttons of headphones. */

enum { AVC_CONTROL = 0x00, AVC_STATUS = 0x01, AVC_NOT_IMPLEMENTED = 0x08, AVC_ACCEPTED, AVC_REJECTED, AVC_STABLE = 0x0c,
       AVC_CHANGED, AVC_INTERIM = 0x0f };
enum { AVC_VENDOR = 0x00, AVC_UNIT_INFO = 0x30, AVC_SUBUNIT_INFO, AVC_PASS_THROUGH = 0x7c };
enum { PDU_GET_CAPS = 0x10, PDU_REGISTER = 0x31, PDU_SET_VOLUME = 0x50, EVENT_VOLUME = 0x0d, KEY_PLAY = 0x44, KEY_PAUSE = 0x46 };
#define PANEL 0x48                                      /* subunit type panel (9), ID 0 */

static atomic_int echo_volume = -1, vol_dirty, key_req, avrcp_links;

/* Setting the Echo's volume runs helper programs (mixer property, LED ring) and takes core_lock: far too slow for the
 * controller thread, whose stalls empty the jitter buffer.  A thread of its own does it, latest request wins. */
static pthread_mutex_t vol_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t vol_cond = PTHREAD_COND_INITIALIZER;
static int vol_want = -1;                               /* vol_lock */

static void *volume_thread(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&vol_lock);
        while (vol_want < 0) pthread_cond_wait(&vol_cond, &vol_lock);
        int v = vol_want; vol_want = -1;
        pthread_mutex_unlock(&vol_lock);
        pthread_mutex_lock(&core_lock); core_set_volume(v); pthread_mutex_unlock(&core_lock);
    }
    return NULL;
}

static void volume_set_later(int pct)
{
    pthread_mutex_lock(&vol_lock); vol_want = pct; pthread_cond_signal(&vol_cond); pthread_mutex_unlock(&vol_lock);
}

static int volume_now(void)                             /* 0..100; read at start, then kept up to date by the core */
{
    int v = atomic_load(&echo_volume);
    return v < 0 ? 50 : v;
}

static void avctp_send(struct link *l, unsigned label, int response, const unsigned char *avc, size_t n)
{
    unsigned char b[48]; struct chan *c = chan_by(l, l->avctp);
    if (!c || n > sizeof b - 3) return;
    b[0] = label << 4 | (response ? 2 : 0); b[1] = 0x11; b[2] = 0x0e;      /* profile: A/V remote control */
    memcpy(b + 3, avc, n); l2_send(l, c->rcid, b, 3 + n);
}

static void vendor_reply(struct link *l, unsigned label, unsigned code, unsigned pdu, const unsigned char *par, size_t n)
{
    unsigned char a[32] = { code, PANEL, AVC_VENDOR, 0x00, 0x19, 0x58, pdu, 0, n >> 8, n };      /* Bluetooth SIG company */
    memcpy(a + 10, par, n); avctp_send(l, label, 1, a, 10 + n);
}

static void avrcp_rx(struct link *l, const unsigned char *p, size_t n)
{
    unsigned char r[16];
    if (n < 6 || (p[0] >> 2 & 3)) return;                  /* fragments: never this small */
    unsigned label = p[0] >> 4;
    if (p[0] & 2) {                                         /* answers to our pass-through commands */
        if (n >= 7 && p[5] == AVC_PASS_THROUGH && !(p[6] & 0x80))
            fprintf(stderr, "a2dp: %012llx: %s %s\n", (unsigned long long)l->addr, (p[6] & 0x7f) == KEY_PLAY ? "play" : "pause",
                    p[3] == AVC_ACCEPTED ? "accepted" : p[3] == AVC_NOT_IMPLEMENTED ? "not implemented" : "rejected");
        return;
    }
    if (p[1] != 0x11 || p[2] != 0x0e) {                     /* another profile: "invalid profile identifier" */
        r[0] = label << 4 | 3; r[1] = p[1]; r[2] = p[2]; l2_send(l, chan_by(l, l->avctp)->rcid, r, 3); return;
    }
    const unsigned char *a = p + 3; size_t an = n - 3;
    if (a[2] == AVC_UNIT_INFO || a[2] == AVC_SUBUNIT_INFO) {
        unsigned char u[8] = { AVC_STABLE, 0xff, a[2], 0x07, PANEL, 0xff, 0xff, 0xff };
        if (a[2] == AVC_UNIT_INFO) { u[5] = 0x00; u[6] = 0x19; u[7] = 0x58; }
        avctp_send(l, label, 1, u, 8);
        return;
    }
    if (a[2] != AVC_VENDOR || an < 10 || a[3] || a[4] != 0x19 || a[5] != 0x58) {   /* pass-through to us and the rest */
        unsigned char e[16]; size_t k = an < sizeof e ? an : sizeof e;
        memcpy(e, a, k); e[0] = AVC_NOT_IMPLEMENTED; avctp_send(l, label, 1, e, k);
        return;
    }
    unsigned pdu = a[6]; size_t plen = a[8] << 8 | a[9]; const unsigned char *par = a + 10;
    if (10 + plen > an) plen = 0;
    switch (pdu) {
    case PDU_GET_CAPS:
        if (plen >= 1 && par[0] == 2) { unsigned char c[5] = { 2, 1, 0x00, 0x19, 0x58 }; vendor_reply(l, label, AVC_STABLE, pdu, c, 5); }
        else if (plen >= 1 && par[0] == 3) { unsigned char c[3] = { 3, 1, EVENT_VOLUME }; vendor_reply(l, label, AVC_STABLE, pdu, c, 3); }
        else { r[0] = 0x01; vendor_reply(l, label, AVC_REJECTED, pdu, r, 1); }       /* invalid parameter */
        break;
    case PDU_REGISTER:
        if (plen >= 1 && par[0] == EVENT_VOLUME) {
            l->vol_label = label; l->vol_pct = volume_now();
            r[0] = EVENT_VOLUME; r[1] = (l->vol_pct * 127 + 50) / 100; vendor_reply(l, label, AVC_INTERIM, pdu, r, 2);
        } else { r[0] = 0x01; vendor_reply(l, label, AVC_REJECTED, pdu, r, 1); }
        break;
    case PDU_SET_VOLUME:
        if (plen >= 1) {
            unsigned v = par[0] & 0x7f; int pct = (v * 100 + 63) / 127;
            l->vol_pct = pct;                               /* no notification back for its own change */
            atomic_store(&echo_volume, pct);
            volume_set_later(pct);
            r[0] = v; vendor_reply(l, label, AVC_ACCEPTED, pdu, r, 1);
        } else { r[0] = 0x01; vendor_reply(l, label, AVC_REJECTED, pdu, r, 1); }
        break;
    default: r[0] = 0x00; vendor_reply(l, label, AVC_REJECTED, pdu, r, 1); break;    /* invalid command */
    }
}

static struct link *avrcp_link(void)                    /* the streaming device if it has AVRCP, else any that has */
{
    if (av.l && av.l->avctp) return av.l;
    for (int i = 0; i < MAX_LINKS; i++) if (links[i].used && links[i].avctp) return &links[i];
    return NULL;
}

static void avctp_open(struct link *l)                  /* the device has not opened AVRCP: we do */
{
    struct chan *c = NULL;
    for (int i = 0; i < MAX_CHANS && !c; i++) if (!l->ch[i].used) c = &l->ch[i];
    if (!c) return;
    memset(c, 0, sizeof *c);
    c->used = 1; c->psm = PSM_AVCTP; c->rmtu = 672; c->lcid = 0x40 + (int)(c - l->ch) + 8 * (int)(l - links);
    if (++l->next_id > 255) l->next_id = 1;
    c->out_id = l->next_id;
    unsigned char d[4]; put16(d, PSM_AVCTP); put16(d + 2, c->lcid);
    sig_send(l, 0x02, c->out_id, d, 4);
}

static void avrcp_upkeep(void)
{
    for (int i = 0; i < MAX_LINKS; i++) {
        struct link *k = &links[i];
        if (!k->used || !k->avctp_at || ms() < k->avctp_at) continue;
        k->avctp_at = 0;
        int pending = 0; for (int j = 0; j < MAX_CHANS; j++) pending |= k->ch[j].used && k->ch[j].psm == PSM_AVCTP;
        if (!k->avctp && !pending && k->enc) avctp_open(k);
    }
    int key = atomic_exchange(&key_req, 0); struct link *l;
    if (key && (l = avrcp_link())) {                        /* press and release */
        for (int release = 0; release < 2; release++) {
            unsigned char a[5] = { AVC_CONTROL, PANEL, AVC_PASS_THROUGH, key | (release ? 0x80 : 0), 0 };
            avctp_send(l, l->av_label++ & 15, 0, a, 5);
        }
        fprintf(stderr, "a2dp: %012llx: %s\n", (unsigned long long)l->addr, key == KEY_PLAY ? "play" : "pause");
    }
    if (atomic_exchange(&vol_dirty, 0)) for (int i = 0; i < MAX_LINKS; i++) {
        l = &links[i]; int pct = volume_now();
        if (!l->used || !l->avctp || l->vol_label < 0 || pct == l->vol_pct) continue;
        unsigned char c[2] = { EVENT_VOLUME, (pct * 127 + 50) / 100 };
        vendor_reply(l, l->vol_label, AVC_CHANGED, PDU_REGISTER, c, 2);
        l->vol_label = -1; l->vol_pct = pct;                /* the device registers again */
    }
}

/* "Connected to <name>" like stock Alexa, once the device has opened AVDTP (a speaker connection, not just an ACL link)
 * and its name is in.  Phones ask for AVDTP a second or two after connecting; the name takes some 100 ms.  A Pixel
 * "disconnecting" in its Bluetooth settings closes AVDTP and AVRCP but keeps the ACL link up: the announcement follows
 * AVDTP (chan_closed), not the link. */
static void maybe_tell(struct link *l)
{
    if (l->told || !l->av_sig || !l->named) return;
    l->told = 1;
    core_bt_device(l->name, 1);
}

static void chan_opened(struct link *l, struct chan *c)
{
    if (c->psm == PSM_AVCTP && !l->avctp) { l->avctp = c->lcid; l->vol_label = -1; atomic_fetch_add(&avrcp_links, 1);
                                            fprintf(stderr, "a2dp: %012llx: remote control\n", (unsigned long long)l->addr); }
    if (c->psm != PSM_AVDTP) return;
    if (!l->av_sig) { l->av_sig = c->lcid; if (!l->avctp) l->avctp_at = ms() + 2000; maybe_tell(l); return; }
    if (av.l == l && av.state == ST_OPEN && !av.media) av.media = c->lcid;
}

static void chan_closed(struct link *l, struct chan *c)
{
    if (c->lcid == l->avctp) { l->avctp = 0; atomic_fetch_sub(&avrcp_links, 1); }
    if (c->lcid == l->av_sig) {
        l->av_sig = 0; if (av.l == l) av_reset();
        if (l->told) { l->told = 0; core_bt_device(l->name, 0); }  /* phones drop the profile, not always the link */
    }
    else if (av.l == l && c->lcid == av.media) { av.media = 0; if (av.state >= ST_OPEN) { av.state = ST_CONFIGURED; set_streaming(0); } }
}

static void l2_rx(struct link *l, const unsigned char *p, size_t n)
{
    int cid = u16(p + 2); struct chan *c;
    p += 4; n -= 4;
    if (cid == 0x0001) { sig_rx(l, p, n); return; }
    if (!(c = chan_by(l, cid)) || !c->cfg_in || !c->cfg_out) return;
    if (c->psm == PSM_SDP) sdp_rx(l, c, p, n);
    else if (cid == l->av_sig) av_rx(l, p, n);
    else if (cid == l->avctp) avrcp_rx(l, p, n);
    else if (av.l == l && cid == av.media) media_rx(p, n);
}

/* ---------------------------------------------------------------- hooks for ble.c */

static void link_gone(struct link *l)
{
    for (int i = 0; i < MAX_CHANS; i++) if (l->ch[i].used) chan_free(l, &l->ch[i]);
    if (av.l == l) av_reset();
    for (struct frag **p = &fhead; *p; ) {
        struct frag *f = *p;
        if (f->handle == l->handle) { *p = f->next; free(f); } else p = &f->next;
    }
    ftail = &fhead; while (*ftail) ftail = &(*ftail)->next;
    credits += l->unacked;
    memset(l, 0, sizeof *l);
}

int a2dp_acl(const unsigned char *p, size_t n)
{
    if (n < 4) return 0;
    struct link *l = link_by(u16(p) & 0x0fff); unsigned pb = p[1] >> 4 & 3; size_t len = u16(p + 2);
    if (!l) return 0;
    if (len > n - 4) return 1;
    p += 4;
    if (pb != 1) { if (len < 4) return 1; l->rxwant = 4 + u16(p); l->rxlen = 0; }
    else if (!l->rxwant) return 1;
    if (l->rxlen + len > sizeof l->rx) { l->rxwant = 0; return 1; }
    memcpy(l->rx + l->rxlen, p, len); l->rxlen += len;
    if (l->rxlen >= l->rxwant) { l->rxwant = 0; l2_rx(l, l->rx, l->rxlen); }
    return 1;
}

int a2dp_completed(unsigned handle, unsigned n)
{
    struct link *l = link_by(handle);
    if (!l) return 0;
    credits += n; if (credits > (int)acl_num) credits = acl_num;
    l->unacked -= n; if (l->unacked < 0) l->unacked = 0;
    acl_flush();
    return 1;
}

int a2dp_event(const unsigned char *e, size_t n)
{
    if (n < 2) return 0;
    const unsigned char *q = e + 2; size_t qn = n - 2; struct link *l;
    int pairing = atomic_load(&pairing_on);
    switch (e[0]) {
    case EV_CONN_REQUEST:                                   /* address, class of device, link type */
        if (qn < 10) return 1;
        {   uint64_t a = addr_of(q); int free_slot = 0;
            for (int i = 0; i < MAX_LINKS; i++) free_slot |= !links[i].used;
            if (q[9] == 1 && free_slot && (pairing || key_for(a))) { unsigned char role = 0x01; later_addr(OP_ACCEPT, q, &role, 1); }
            else {
                unsigned char why = q[9] != 1 || !free_slot ? 0x0d : 0x0f;        /* limited resources / unacceptable address */
                fprintf(stderr, "a2dp: connection from %012llx refused\n", (unsigned long long)a);
                later_addr(OP_REJECT, q, &why, 1);
            }
        }
        return 1;
    case EV_CONN_COMPLETE:                                  /* status, handle, address, link type, encryption */
        if (qn < 11) return 1;
        if (q[0] || q[9] != 1) return 1;
        for (int i = 0; i < MAX_LINKS; i++) if (!links[i].used) {
            l = &links[i]; memset(l, 0, sizeof *l);
            l->used = 1; l->handle = u16(q + 1) & 0x0fff; l->addr = addr_of(q + 3); l->enc = q[10];
            fprintf(stderr, "a2dp: %012llx connected\n", (unsigned long long)l->addr);
            { unsigned char r[4] = { 0x01, 0, 0, 0 }; later_addr(OP_REMOTE_NAME, q + 3, r, 4); }   /* page scan R1, clock offset unknown */
            return 1;
        }
        { unsigned char d[3]; put16(d, u16(q + 1)); d[2] = 0x14; later(OP_DISCONNECT, d, 3); }  /* no room after all */
        return 1;
    case EV_DISCONNECT:
        if (qn < 4 || q[0] || !(l = link_by(u16(q + 1) & 0x0fff))) return 0;
        fprintf(stderr, "a2dp: %012llx disconnected (0x%02x)\n", (unsigned long long)l->addr, q[3]);
        link_gone(l);
        return 1;
    case EV_REMOTE_NAME: {                                  /* status, address, name (UTF-8, NUL padded to 248) */
        if (qn < 7) return 1;
        uint64_t a = addr_of(q + 1); l = NULL;
        for (int i = 0; i < MAX_LINKS; i++) if (links[i].used && links[i].addr == a) l = &links[i];
        if (!l) return 0;                                   /* not ours (ble.c never asks, but let it see) */
        size_t nl = 0, max = qn - 7 < sizeof l->name - 1 ? qn - 7 : sizeof l->name - 1;
        if (!q[0]) while (nl < max && q[7 + nl]) nl++;
        if (nl == max && 7 + nl < qn) while (nl && (q[7 + nl] & 0xc0) == 0x80) nl--;      /* cut: not inside a UTF-8 character */
        memcpy(l->name, q + 7, nl); l->name[nl] = 0;
        for (size_t i = 0; i < nl; i++) if ((unsigned char)l->name[i] < 0x20) l->name[i] = ' ';
        if (q[0]) fprintf(stderr, "a2dp: %012llx: no name (0x%02x)\n", (unsigned long long)a, q[0]);
        else fprintf(stderr, "a2dp: %012llx is \"%s\"\n", (unsigned long long)a, l->name);
        l->named = 1;
        maybe_tell(l);
        return 1; }
    case EV_LINK_KEY_REQUEST: {
        if (qn < 6) return 1;
        struct key *k = key_for(addr_of(q));
        if (k) later_addr(OP_LINK_KEY_REPLY, q, k->k, 16); else later_addr(OP_LINK_KEY_NEG, q, NULL, 0);
        return 1; }
    case EV_LINK_KEY_NOTIFY: {                              /* address, key, type */
        if (qn < 23) return 1;
        uint64_t a = addr_of(q); struct key *k = key_for(a);
        if (!k) { if (nkeys == MAX_KEYS) key_forget(keys[0].addr); k = &keys[nkeys++]; }
        k->addr = a; memcpy(k->k, q + 6, 16); k->type = q[22];
        keys_save();
        fprintf(stderr, "a2dp: paired with %012llx (key type %u)\n", (unsigned long long)a, q[22]);
        if (pairing) set_pairing(0);                        /* like the stock speaker: one device per "pair" */
        return 1; }
    case EV_IO_CAP_REQUEST:
        if (qn < 6) return 1;
        if (pairing) { unsigned char io[3] = { 0x03, 0x00, 0x04 }; later_addr(OP_IO_CAP_REPLY, q, io, 3); }   /* NoInputNoOutput, no OOB, general bonding */
        else { unsigned char why = 0x18; later_addr(OP_IO_CAP_NEG, q, &why, 1);              /* pairing not allowed */
               fprintf(stderr, "a2dp: %012llx wants to pair, pairing is off\n", (unsigned long long)addr_of(q)); }
        return 1;
    case EV_USER_CONFIRM:
        if (qn < 6) return 1;
        later_addr(pairing ? OP_CONFIRM_REPLY : OP_CONFIRM_NEG, q, NULL, 0);
        return 1;
    case EV_PIN_REQUEST:
        if (qn < 6) return 1;
        if (pairing) { unsigned char pin[17] = { 4, '0', '0', '0', '0' }; later_addr(OP_PIN_REPLY, q, pin, 17); }
        else later_addr(OP_PIN_NEG, q, NULL, 0);
        return 1;
    case EV_IO_CAP_RESPONSE: return 1;
    case EV_SSP_COMPLETE:
        if (qn >= 7 && q[0]) fprintf(stderr, "a2dp: pairing with %012llx failed (0x%02x)\n", (unsigned long long)addr_of(q + 1), q[0]);
        return 1;
    case EV_AUTH_COMPLETE:                                  /* status, handle */
        if (qn < 3 || !(l = link_by(u16(q + 1) & 0x0fff))) return 0;
        if (q[0]) {
            fprintf(stderr, "a2dp: %012llx: authentication failed (0x%02x)\n", (unsigned long long)l->addr, q[0]);
            if (q[0] == 0x06) key_forget(l->addr);          /* key missing: the device forgot us */
            pending_resolve(l, 0);
        } else if (!l->enc) { unsigned char d[3]; put16(d, l->handle); d[2] = 1; later(OP_ENCRYPT, d, 3); }
        else pending_resolve(l, 1);
        return 1;
    case EV_ENC_CHANGE:                                     /* status, handle, on */
        if (qn < 4 || !(l = link_by(u16(q + 1) & 0x0fff))) return 0;
        l->enc = !q[0] && q[3];
        if (q[0] || l->enc) pending_resolve(l, l->enc);
        return 1;
    case EV_KEY_REFRESH:
        return qn >= 3 && link_by(u16(q + 1) & 0x0fff);
    }
    return 0;
}

int a2dp_upkeep(void)
{
    avrcp_upkeep();
    for (int i = 0; i < npend; i++) if (hci_cmd(pend[i].op, pend[i].p, pend[i].n) < 0) { npend = 0; return -1; }
    npend = 0;
    int r = atomic_exchange(&pair_req, -1);
    if (r >= 0) set_pairing(r);
    if (pair_until && ms() > pair_until) set_pairing(0);
    for (int i = 0; i < MAX_LINKS; i++) {
        struct link *l = &links[i];
        if (!l->used || !l->pend_until) continue;
        if (ms() > l->pend_until) { fprintf(stderr, "a2dp: %012llx: link never encrypted\n", (unsigned long long)l->addr); pending_resolve(l, 0); continue; }
        if (l->auth_sent || ms() < l->auth_at) continue;
        unsigned char d[2]; put16(d, l->handle); l->auth_sent = 1;
        if (hci_cmd(OP_AUTH, d, 2) < 0) return -1;        /* refused (the phone is at it): its result comes anyway */
    }
    int mode = atomic_load(&pairing_on) ? 3 : nkeys ? 2 : 0;
    if (mode != scan_mode) {
        unsigned char m = mode;
        int st = hci_cmd(OP_SCAN_ENABLE, &m, 1);
        if (st < 0) return -1;
        if (!st) scan_mode = mode;
        else { fprintf(stderr, "a2dp: scan mode refused (0x%02x)\n", st); scan_mode = mode; }
    }
    return 0;
}

int a2dp_streaming(void) { return atomic_load(&streaming); }

int a2dp_busy(void)
{
    int b = pair_until != 0 || npend > 0;
    for (int i = 0; i < MAX_LINKS; i++) b |= links[i].used && (links[i].pend_until || links[i].avctp_at);
    return b;
}

void a2dp_lost(void)
{
    for (int i = 0; i < MAX_LINKS; i++) if (links[i].used) link_gone(&links[i]);
    for (struct frag *f = fhead, *n; f; f = n) { n = f->next; free(f); }
    fhead = NULL; ftail = &fhead; npend = 0; scan_mode = -1;
}

int a2dp_setup(void)
{
    unsigned char b[248]; const char *name = core_name; size_t nl = strlen(name) < 232 ? strlen(name) : 232;
    a2dp_lost();
    if (hci_cmd(OP_READ_BUFFER, NULL, 0) == 0) { const unsigned char *r = hci_ret(); acl_len = u16(r); acl_num = u16(r + 3); }
    if (acl_len < 27) acl_len = 27;
    if (acl_len > 1021) acl_len = 1021;
    if (!acl_num) acl_num = 1;
    credits = acl_num;
    memset(b, 0, sizeof b); memcpy(b, name, nl);
    if (hci_cmd(OP_LOCAL_NAME, b, 248) < 0) return -1;
    { unsigned char cod[3] = { 0x14, 0x04, 0x24 };          /* audio + rendering; audio/video, loudspeaker */
      unsigned char one = 1, two = 2, pol[2] = { 0x05, 0x00 };   /* link policy: role switch, sniff */
      if (hci_cmd(OP_CLASS, cod, 3) < 0 || hci_cmd(OP_SSP_MODE, &one, 1) < 0 || hci_cmd(OP_INQUIRY_MODE, &two, 1) < 0 ||
          hci_cmd(OP_LINK_POLICY, pol, 2) < 0) return -1; }
    /* extended inquiry response: the name and the A2DP sink UUID, so phones show a speaker before they connect */
    memset(b, 0, 241); b[1] = nl + 1; b[2] = 0x09; memcpy(b + 3, name, nl);
    { unsigned char *u = b + 3 + nl; u[0] = 3; u[1] = 0x03; u[2] = 0x0b; u[3] = 0x11; }
    b[0] = 0;                                               /* FEC not required */
    if (hci_cmd(OP_EIR, b, 241) < 0) return -1;
    fprintf(stderr, "a2dp: ready as \"%s\", ACL %u x %u\n", name, acl_num, acl_len);
    return 0;
}

/* ---------------------------------------------------------------- API */

void a2dp_pair(int on) { atomic_store(&pair_req, on != 0); hci_poke(); }
void a2dp_volume_changed(int percent) { atomic_store(&echo_volume, percent); atomic_store(&vol_dirty, 1); hci_poke(); }

int a2dp_button(int resume)
{
    if (!atomic_load(&avrcp_links)) return 0;
    long long t = atomic_load(&button_paused_at);
    int paused = t && ms() - t < 30 * 60 * 1000LL;          /* by the button, and the device has not started since */
    if (!resume) {                                          /* while it streams: pause, or play if we just paused it (a
                                                               device may stream silence for a while after pausing) */
        if (!atomic_load(&streaming) || atomic_load(&yielded)) return 0;
        atomic_store(&button_paused_at, paused ? 0 : ms()); atomic_store(&key_req, paused ? KEY_PLAY : KEY_PAUSE); hci_poke();
        return 1;
    }
    if (!paused) return 0;
    atomic_store(&button_paused_at, 0); atomic_store(&key_req, KEY_PLAY); hci_poke();
    return 1;
}

void a2dp_pause(void)
{
    if (!atomic_load(&streaming)) return;
    atomic_store(&button_paused_at, 0);
    if (atomic_load(&avrcp_links)) { atomic_store(&key_req, KEY_PAUSE); hci_poke(); return; }
    if (!atomic_exchange(&yielded, 1)) fprintf(stderr, "a2dp: no remote control: silent while the other source plays\n");
}

void a2dp_unyield(void) { if (atomic_exchange(&yielded, 0)) fprintf(stderr, "a2dp: audible again\n"); }
int  a2dp_pairing(void) { return atomic_load(&pairing_on); }

void a2dp_start(void (*changed)(void))
{
    if (changed) notify = changed;
    if (started || !ble_present()) return;
    started = 1;
    pthread_mutex_lock(&core_lock); atomic_store(&echo_volume, core_volume()); pthread_mutex_unlock(&core_lock);
    keys_load();
    for (int i = 0; i < a2dp_ncodecs && nseps < (int)(sizeof seps / sizeof *seps); i++) if (a2dp_codecs[i].usable()) seps[nseps++] = i;
    { char names[80] = ""; for (int i = 0; i < nseps; i++) snprintf(names + strlen(names), sizeof names - strlen(names), "%s%s", i ? ", " : "", a2dp_codecs[seps[i]].name);
      fprintf(stderr, "a2dp: codecs %s\n", names); }
    pthread_t t; pthread_create(&t, NULL, player, NULL); pthread_detach(t);
    pthread_create(&t, NULL, volume_thread, NULL); pthread_detach(t);
    ble_start(NULL);
}
