/*
 * Bluetooth LE straight on the controller.  The Echo has no kernel Bluetooth stack (no BlueZ, no HCI sockets): MediaTek's
 * WMT driver exposes the combo chip's HCI as /dev/stpbt, H4 framing (packet type byte, then the HCI packet), and Amazon's
 * btmanagerd (Bluetooth speaker mode, pairing through the Alexa app) drives it from user space.  Without Alexa nothing can
 * pair with that any more, so hassmic takes the radio over (the speaker is a2dp.c now): alexa-off.sh stops btmanagerd.
 * The device node does not refuse a second opener, so this waits until btmanagerd is really gone; two users would steal
 * each other's events.
 *
 * Scanning: 30 ms every 320 ms, ESPHome's default for proxies on Wi-Fi.  The chip shares its antenna with Wi-Fi, and on
 * the Echo continuous scanning cut Wi-Fi throughput from 4.3 to about 1 MB/s, 30/320 to 3.8 MB/s.  The number of
 * advertisements received was the same for both.  Duplicate filtering is off: Home Assistant wants every RSSI.
 * What Wi-Fi brings while the scan has the antenna is lost unless the access point sends it again, and it never does
 * for broadcasts: from a wired PC, 42-54 of 60 broadcasts reached the Dot 2 with hassmic running, 59 of 60 with it
 * stopped, unicasts 60 of 60 either way (2026-10-07; a laptop's Wi-Fi on the same access points lost 7 of 120).  The
 * other Echos' wake word claims are broadcasts, so a round pauses the scan (ble_quiet) while it listens for them.
 *
 * Connections: a small GATT client, what ESPHome's proxy does with ESP-IDF's stack.  One thread owns the controller;
 * requests from the API threads are queued and answered through callbacks.  Per connection: ACL with L2CAP fragmentation
 * and the controller's flow control, ATT with one request on the air at a time (ATT's own rule), service discovery on
 * request, long reads (Read Blob), long writes (prepared writes), notifications and indications.  Home Assistant writes
 * the notification descriptors itself (with REMOTE_CACHING it does).  What the device asks of us: the MTU exchange and
 * connection parameter updates are granted; our GATT server is empty towards these devices.  Scanning pauses while a
 * connection is being set up, as ESPHome does.
 *
 * The peripheral role, for setting the Echo up from a phone (improv.c): legacy connectable advertising, and one link a
 * phone opens to us (conns[SRV], outside the proxy's slots), on which a small GATT server answers: Generic Access and the
 * one service ble_serve() gave, its characteristics read, written (long writes too) and notified.  No pairing on that
 * link: Improv has none, and a phone that asks is told so.
 *
 * Pairing (SMP, we are always the initiator): Just Works like an ESP32 proxy, which has no display or keyboard either.
 * LE Secure Connections when the controller does the P-256 part (LE Read Local P-256 Public Key, LE Generate DHKey),
 * legacy pairing otherwise and for devices that only know that.  AES, CMAC and the key functions are ble_crypto.c.
 * Home Assistant asks for it (pair), or the device does (Security Request), as ESP-IDF accepts those.  Bonds (the keys
 * the device gave us, its IRK to recognise it behind a private address) go to state/ble_bonds; on every later
 * connection the link is encrypted with them before GATT traffic starts.  Our own keys are never distributed: we only
 * ever connect, the device never needs to recognise or encrypt towards us.
 *
 * BR/EDR (the Bluetooth speaker) shares this thread and the controller: a2dp.c gets the events and ACL packets that
 * are not LE's, its own share of the controller's buffers, and sends its commands from upkeep() (hci.h).
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
#include "ble.h"
#include "board.h"
#include "ble_crypto.h"
#include "hci.h"

#define SCAN_INTERVAL 512                       /* 320 ms, units of 0.625 ms */
#define SCAN_WINDOW 48                          /* 30 ms */
#define ATT_MTU 517                             /* what we offer; ESPHome's too */
#define CONNECT_MS 20000                        /* then the attempt is cancelled */
#define ATT_MS 30000                            /* ATT transaction timeout: the connection is closed after it */
#define SMP_MS 30000                            /* SMP timeout: pairing failed */
#define MAX_BONDS 16

enum { H4_CMD = 1, H4_ACL = 2, H4_EVT = 4 };
enum { EV_DISCONNECT = 0x05, EV_ENC_CHANGE = 0x08, EV_ENC_REFRESH = 0x30, EV_CMD_COMPLETE = 0x0e, EV_CMD_STATUS = 0x0f, EV_NUM_COMPLETED = 0x13, EV_LE_META = 0x3e };
enum { LE_CONN_COMPLETE = 0x01, LE_ADV_REPORT = 0x02, LE_LTK_REQUEST = 0x05, LE_PK_COMPLETE = 0x08, LE_DHKEY_COMPLETE = 0x09 };
enum { OP_DISCONNECT = 0x0406, OP_EVENT_MASK = 0x0c01, OP_RESET = 0x0c03, OP_READ_LOCAL_CMDS = 0x1002, OP_READ_BUFFER = 0x1005, OP_LE_EVENT_MASK = 0x2001,
       OP_LE_READ_BUFFER = 0x2002, OP_LE_ADV_PARAMS = 0x2006, OP_LE_ADV_DATA = 0x2008, OP_LE_SCAN_RSP = 0x2009, OP_LE_ADV_ENABLE = 0x200a, OP_LE_SCAN_PARAMS = 0x200b, OP_LE_SCAN_ENABLE = 0x200c, OP_LE_CONNECT = 0x200d,
       OP_LE_CONNECT_CANCEL = 0x200e, OP_LE_CONN_UPDATE = 0x2013, OP_LE_START_ENC = 0x2019, OP_LE_LTK_NEG = 0x201b,
       OP_LE_READ_PK = 0x2025, OP_LE_DHKEY = 0x2026 };
enum { CID_ATT = 4, CID_SIG = 5, CID_SMP = 6 };
enum { ATT_ERROR = 0x01, ATT_MTU_REQ, ATT_MTU_RSP, ATT_FIND_INFO_REQ, ATT_FIND_INFO_RSP, ATT_READ_TYPE_REQ = 0x08,
       ATT_READ_TYPE_RSP, ATT_READ_REQ, ATT_READ_RSP, ATT_READ_BLOB_REQ, ATT_READ_BLOB_RSP, ATT_READ_GROUP_REQ = 0x10,
       ATT_READ_GROUP_RSP, ATT_WRITE_REQ, ATT_WRITE_RSP, ATT_PREP_WRITE_REQ = 0x16, ATT_PREP_WRITE_RSP, ATT_EXEC_WRITE_REQ,
       ATT_EXEC_WRITE_RSP, ATT_NOTIFY = 0x1b, ATT_INDICATE = 0x1d, ATT_CONFIRM = 0x1e, ATT_WRITE_CMD = 0x52 };
enum { ATT_E_INVALID_HANDLE = 0x01, ATT_E_READ_NOT_PERMITTED, ATT_E_WRITE_NOT_PERMITTED, ATT_E_INVALID_PDU, ATT_E_NOT_SUPPORTED = 0x06,
       ATT_E_INVALID_OFFSET, ATT_E_PREP_QUEUE_FULL = 0x09, ATT_E_NOT_FOUND = 0x0a, ATT_E_NOT_LONG = 0x0b, ATT_E_INVALID_LEN = 0x0d,
       ATT_E_UNSUPPORTED_GROUP = 0x10, GATT_E_FAILED = 0x85 };
enum { ATT_FIND_TYPE_REQ = 0x06, ATT_FIND_TYPE_RSP };
enum { HCI_E_UNKNOWN_CONN = 0x02, HCI_E_TIMEOUT = 0x08, HCI_E_LIMIT = 0x09, HCI_E_USER_ENDED = 0x13 };

static int fd = -1, wake[2] = { -1, -1 };
static const struct ble_handler *H;
static atomic_int want_on, want_active, scanning;
static atomic_llong quiet_until;                /* CLOCK_MONOTONIC ms: no scanning before then (ble_quiet) */
static char bdaddr[18];                         /* set once, before anything reads it */

static unsigned char in[2048]; static size_t have;             /* H4 stream: reads may split or join packets */
static int cc_op, cc_status;                                    /* last Command Complete / Status */
static unsigned char cc_ret[72];                                /* its return parameters after the status */
static struct ble_adv batch[BLE_BATCH]; static int nbatch; static struct timespec batch_t0;

static long ms_since(const struct timespec *t)
{
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - t->tv_sec) * 1000 + (now.tv_nsec - t->tv_nsec) / 1000000;
}
static void now(struct timespec *t) { clock_gettime(CLOCK_MONOTONIC, t); }
static long long mono_ms(void) { struct timespec t; now(&t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static unsigned u16(const unsigned char *p) { return p[0] | p[1] << 8; }
static void put16(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; }

/* ---------------------------------------------------------------- requests from other threads */

enum { R_CONNECT, R_DISCONNECT, R_SERVICES, R_READ, R_WRITE, R_PAIR, R_UNPAIR };
struct req { struct req *next; int kind; uint64_t addr; unsigned addr_type, handle; int response; size_t len; unsigned char data[]; };
static pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;
static struct req *qhead, **qtail = &qhead;                    /* qlock */
static uint64_t slot_addr[BLE_MAX_CONN];                       /* qlock: what ble_connections() reports, 0 = free */

static void poke(void) { if (wake[1] >= 0 && write(wake[1], "", 1) < 0) { /* pipe full: the thread wakes anyway */ } }

static void request(int kind, uint64_t addr, unsigned addr_type, unsigned handle, const void *data, size_t len, int response)
{
    struct req *r = calloc(1, sizeof *r + len);
    if (!r) return;
    r->kind = kind; r->addr = addr; r->addr_type = addr_type; r->handle = handle; r->response = response; r->len = len;
    if (len) memcpy(r->data, data, len);
    pthread_mutex_lock(&qlock); *qtail = r; qtail = &r->next; pthread_mutex_unlock(&qlock);
    poke();
}

/* ---------------------------------------------------------------- connections */

enum { C_FREE, C_WAIT, C_CONNECTING, C_MTU, C_UP, C_CLOSING };
enum { K_DISCOVER, K_READ, K_WRITE };
struct op { struct op *next; int kind; unsigned handle; int response; size_t len; unsigned char data[]; };

/* Pairing in progress.  Byte strings as the PDUs carry them (least significant octet first) */
enum { S_IDLE, S_RSP, S_CONFIRM, S_RANDOM, S_PK, S_CB, S_NB, S_DHKEY, S_EB, S_ENC, S_KEYS };
struct smp {
    int state, sc, asked, keysize, want, got;   /* asked: Home Assistant waits for the result; want/got: keys, see smp_rx */
    struct timespec t;
    unsigned char preq[7], pres[7], rnd[16], conf[16], peer_rnd[16];   /* ours (Mrand / Na), theirs (Sconfirm / Cb, Srand / Nb) */
    unsigned char pkb[64], mackey[16];          /* the device's public key; MacKey in spec order */
    unsigned char ltk[16], rand[8], irk[16], id[7]; unsigned ediv;     /* what the device distributes */
};

static struct conn {
    int state, handle, cancel, close_now, ltk_neg, upd; uint64_t addr; unsigned addr_type, mtu;
    int enc, enc_pending, need_enc, need_dhkey;  /* encrypted; being encrypted (GATT waits); Start Encryption / DHKey wanted */
    int heard, mtu_retry, peer_mtu;             /* the device has sent something; MTU exchange again once encrypted;
                                                   the device exchanged MTUs itself */
    unsigned char enc_ltk[16], enc_rand[8]; unsigned enc_ediv;
    struct smp smp;
    struct timespec t;                          /* connect started / current ATT request sent */
    struct op *ops, *cur;                       /* queue; cur: the request on the air */
    unsigned char val[512]; size_t vlen; size_t off;   /* long read / long write progress */
    int phase, idx; unsigned next;              /* discovery */
    struct ble_db *db;
    unsigned char rx[4 + ATT_MTU + 64]; size_t rxlen, rxwant;  /* L2CAP reassembly */
    unsigned char upd_par[8];                   /* LE Connection Update wanted by the device */
    int unacked;                                /* ACL packets sent, not yet reported complete */
    int srv;                                    /* conns[SRV]: a phone's link to our GATT server */
} conns[BLE_MAX_CONN + 1];
#define SRV BLE_MAX_CONN                        /* the peripheral link's place, after the proxy's slots */
#define NCONN (BLE_MAX_CONN + 1)

static unsigned acl_len = 27, acl_num = 1; static int credits;
struct frag { struct frag *next; int handle; size_t len; unsigned char b[]; };
static struct frag *fhead, **ftail = &fhead;

static void set_slot(struct conn *c, uint64_t addr)
{
    if (c - conns >= BLE_MAX_CONN) return;
    pthread_mutex_lock(&qlock); slot_addr[c - conns] = addr; pthread_mutex_unlock(&qlock);
    if (H->slots_changed) H->slots_changed();
}

static const struct ble_handler no_handler;

static struct conn *by_addr(uint64_t a) { for (int i = 0; i < BLE_MAX_CONN; i++) if (conns[i].state != C_FREE && conns[i].addr == a) return &conns[i]; return NULL; }
static struct conn *by_handle(int h) { for (int i = 0; i < NCONN; i++) if (conns[i].state >= C_MTU && conns[i].handle == h) return &conns[i]; return NULL; }
static struct conn *in_state(int s) { for (int i = 0; i < BLE_MAX_CONN; i++) if (conns[i].state == s) return &conns[i]; return NULL; }

static void acl_flush(void)
{
    while (fhead && credits > 0) {
        struct frag *f = fhead;
        if (!(fhead = f->next)) ftail = &fhead;
        struct conn *c = by_handle(f->handle);
        if (c) {
            if (write(fd, f->b, f->len) != (ssize_t)f->len) fprintf(stderr, "bluetooth: ACL write failed\n");
            else { credits--; c->unacked++; }
        }
        free(f);
    }
}

/* One L2CAP frame, cut into ACL packets the controller takes */
static void l2cap_send(struct conn *c, unsigned cid, const void *pdu, size_t n)
{
    unsigned char fr[4 + ATT_MTU + 16];
    if (n > sizeof fr - 4) return;
    put16(fr, n); put16(fr + 2, cid); memcpy(fr + 4, pdu, n); n += 4;
    for (size_t o = 0; o < n; o += acl_len) {
        size_t k = n - o < acl_len ? n - o : acl_len;
        struct frag *f = malloc(sizeof *f + 5 + k);
        if (!f) return;
        f->next = NULL; f->handle = c->handle; f->len = 5 + k;
        f->b[0] = H4_ACL; f->b[1] = c->handle; f->b[2] = (c->handle >> 8 & 0x0f) | (o ? 0x10 : 0x00);   /* continuing / first */
        put16(f->b + 3, k); memcpy(f->b + 5, fr + o, k);
        *ftail = f; ftail = &f->next;
    }
    acl_flush();
}

static void att_send(struct conn *c, const void *pdu, size_t n) { l2cap_send(c, CID_ATT, pdu, n); }
static int cmd(unsigned op, const void *par, unsigned n);

static struct conn *dh_owner;                   /* the one DHKey computation the controller does at a time */

static void free_conn(struct conn *c)
{
    if (c->smp.asked && H->paired) H->paired(c->addr, 0, HCI_E_UNKNOWN_CONN);
    if (dh_owner == c) dh_owner = NULL;
    for (struct op *o = c->ops, *n; o; o = n) { n = o->next; free(o); }
    free(c->cur); free(c->db);
    for (struct frag **p = &fhead; *p; ) {                      /* its queued packets die with it */
        struct frag *f = *p;
        if (f->handle == c->handle) { *p = f->next; free(f); } else p = &f->next;
    }
    ftail = &fhead; while (*ftail) ftail = &(*ftail)->next;
    credits += c->unacked;                                      /* the controller forgets them at disconnection */
    memset(c, 0, sizeof *c);
    set_slot(c, 0);
}

/* ---------------------------------------------------------------- GATT client */

static void uuid_from(const unsigned char *p, size_t n, uint64_t u[2])
{
    if (n == 2) { u[0] = (uint64_t)u16(p) << 32 | 0x1000; u[1] = 0x800000805f9b34fbULL; return; }     /* Bluetooth base UUID */
    u[0] = u[1] = 0;
    if (n == 16) for (int i = 15; i >= 0; i--) { if (i >= 8) u[0] = u[0] << 8 | p[i]; else u[1] = u[1] << 8 | p[i]; }
}

static void start_op(struct conn *c);

static void op_done(struct conn *c)
{
    free(c->cur); c->cur = NULL;
    start_op(c);
}

static void op_fail(struct conn *c, unsigned handle, int err)
{
    if (H->error) H->error(c->addr, handle, err);
    op_done(c);
}

static void send_read(struct conn *c)
{
    unsigned char p[5];
    if (!c->vlen) { p[0] = ATT_READ_REQ; put16(p + 1, c->cur->handle); att_send(c, p, 3); }
    else { p[0] = ATT_READ_BLOB_REQ; put16(p + 1, c->cur->handle); put16(p + 3, c->vlen); att_send(c, p, 5); }
}

static void send_prep(struct conn *c)
{
    unsigned char p[5 + ATT_MTU]; size_t k = c->cur->len - c->off;
    if (k > c->mtu - 5) k = c->mtu - 5;
    p[0] = ATT_PREP_WRITE_REQ; put16(p + 1, c->cur->handle); put16(p + 3, c->off); memcpy(p + 5, c->cur->data + c->off, k);
    att_send(c, p, 5 + k);
}

/* discovery requests: phase 0 services, 1 characteristics of service idx, 2 descriptors of characteristic idx */
static unsigned chr_end(struct conn *c, int i)
{
    struct ble_db *d = c->db; struct ble_chr *ch = &d->chr[i];
    return i + 1 < d->nchr && d->chr[i + 1].svc == ch->svc ? d->chr[i + 1].decl - 1 : d->svc[ch->svc].end;
}

static void disc_next(struct conn *c)
{
    struct ble_db *d = c->db; unsigned char p[7];
    for (;;) {
        if (c->phase == 0) {
            p[0] = ATT_READ_GROUP_REQ; put16(p + 1, c->next); put16(p + 3, 0xffff); put16(p + 5, 0x2800);
            att_send(c, p, 7); return;
        }
        if (c->phase == 1) {
            if (c->idx >= d->nsvc) { c->phase = 2; c->idx = 0; c->next = 0; continue; }
            if (!c->next) { c->next = d->svc[c->idx].start; d->svc[c->idx].first = d->nchr; }
            if (c->next > d->svc[c->idx].end) { c->idx++; c->next = 0; continue; }
            p[0] = ATT_READ_TYPE_REQ; put16(p + 1, c->next); put16(p + 3, d->svc[c->idx].end); put16(p + 5, 0x2803);
            att_send(c, p, 7); return;
        }
        if (c->idx >= d->nchr) {                                /* all found */
            fprintf(stderr, "bluetooth: %012llx has %d services, %d characteristics, %d descriptors\n",
                    (unsigned long long)c->addr, d->nsvc, d->nchr, d->ndsc);
            if (H->services) H->services(c->addr, d);
            op_done(c); return;
        }
        if (!c->next) { c->next = d->chr[c->idx].handle + 1; d->chr[c->idx].first = d->ndsc; }
        if (c->next > chr_end(c, c->idx)) { c->idx++; c->next = 0; continue; }
        p[0] = ATT_FIND_INFO_REQ; put16(p + 1, c->next); put16(p + 3, chr_end(c, c->idx));
        att_send(c, p, 5); return;
    }
}

static void disc_rx(struct conn *c, const unsigned char *p, size_t n)
{
    struct ble_db *d = c->db; unsigned last = 0;
    if (p[0] == ATT_ERROR) {
        if (n < 5 || p[4] != ATT_E_NOT_FOUND) { op_fail(c, 0, n >= 5 ? p[4] : GATT_E_FAILED); return; }
        if (c->phase == 0) { c->phase = 1; c->idx = 0; c->next = 0; }
        else { c->idx++; c->next = 0; }
        disc_next(c); return;
    }
    if (c->phase == 0 && p[0] == ATT_READ_GROUP_RSP && n >= 2 && p[1] >= 6) {
        for (size_t i = 2; i + p[1] <= n; i += p[1]) {
            last = u16(p + i + 2);
            if (d->nsvc == (int)(sizeof d->svc / sizeof *d->svc)) continue;
            struct ble_svc *s = &d->svc[d->nsvc++];
            s->start = u16(p + i); s->end = last; uuid_from(p + i + 4, p[1] - 4, s->uuid);
        }
        if (last >= 0xffff || last < c->next) { c->phase = 1; c->idx = 0; c->next = 0; } else c->next = last + 1;
    } else if (c->phase == 1 && p[0] == ATT_READ_TYPE_RSP && n >= 2 && p[1] >= 7) {
        for (size_t i = 2; i + p[1] <= n; i += p[1]) {
            last = u16(p + i);
            if (d->nchr == (int)(sizeof d->chr / sizeof *d->chr)) continue;
            struct ble_chr *ch = &d->chr[d->nchr++];
            ch->decl = last; ch->props = p[i + 2]; ch->handle = u16(p + i + 3); ch->svc = c->idx;
            uuid_from(p + i + 5, p[1] - 5, ch->uuid);
            d->svc[c->idx].n++;
        }
        if (last < c->next) { c->idx++; c->next = 0; } else c->next = last + 1;    /* nothing usable: next service */
    } else if (c->phase == 2 && p[0] == ATT_FIND_INFO_RSP && n >= 2) {
        size_t el = p[1] == 1 ? 4 : 18;
        for (size_t i = 2; i + el <= n; i += el) {
            last = u16(p + i);
            if (d->ndsc == (int)(sizeof d->dsc / sizeof *d->dsc)) continue;
            struct ble_dsc *ds = &d->dsc[d->ndsc++];
            ds->handle = last; uuid_from(p + i + 2, el - 2, ds->uuid);
            d->chr[c->idx].n++;
        }
        if (last < c->next) { c->idx++; c->next = 0; } else c->next = last + 1;
    } else { op_fail(c, 0, GATT_E_FAILED); return; }
    disc_next(c);
}

static void start_op(struct conn *c)
{
    while (!c->cur && c->ops && c->state == C_UP && !c->close_now && !c->enc_pending) {
        struct op *o = c->cur = c->ops; c->ops = o->next;
        unsigned char p[3 + ATT_MTU];
        now(&c->t); c->vlen = c->off = 0;
        switch (o->kind) {
        case K_READ: send_read(c); break;
        case K_WRITE:
            if (!o->response) {                             /* no answer: done once queued */
                size_t k = o->len > c->mtu - 3 ? c->mtu - 3 : o->len;
                p[0] = ATT_WRITE_CMD; put16(p + 1, o->handle); memcpy(p + 3, o->data, k); att_send(c, p, 3 + k);
                free(o); c->cur = NULL;
            } else if (o->len <= c->mtu - 3) {
                p[0] = ATT_WRITE_REQ; put16(p + 1, o->handle); memcpy(p + 3, o->data, o->len); att_send(c, p, 3 + o->len);
            } else send_prep(c);
            break;
        case K_DISCOVER:
            if (c->db) { if (H->services) H->services(c->addr, c->db); free(o); c->cur = NULL; break; }  /* known already */
            if (!(c->db = calloc(1, sizeof *c->db))) { op_fail(c, 0, GATT_E_FAILED); return; }
            c->phase = 0; c->idx = 0; c->next = 1;
            disc_next(c);
            break;
        }
    }
}

static void att_rx(struct conn *c, const unsigned char *p, size_t n)
{
    unsigned char r[5];
    if (!n) return;
    switch (p[0]) {
    case ATT_NOTIFY: case ATT_INDICATE:
        if (p[0] == ATT_INDICATE) { r[0] = ATT_CONFIRM; att_send(c, r, 1); }
        if (n >= 3 && H->notify) H->notify(c->addr, u16(p + 1), p + 3, n - 3);
        return;
    case ATT_MTU_REQ:                                       /* the device's side of the exchange: the smaller MTU counts */
        if (n >= 3) { unsigned m = u16(p + 1); c->mtu = m < 23 ? 23 : m < ATT_MTU ? m : ATT_MTU; c->peer_mtu = 1; }
        r[0] = ATT_MTU_RSP; put16(r + 1, ATT_MTU); att_send(c, r, 3);
        return;
    }
    if (!(p[0] & 1) && p[0] != ATT_CONFIRM) {               /* any other request to our (empty) GATT server */
        if (p[0] & 0x40) return;                            /* commands get no answer */
        r[0] = ATT_ERROR; r[1] = p[0]; put16(r + 2, n >= 3 ? u16(p + 1) : 0);
        r[4] = p[0] == ATT_FIND_INFO_REQ || p[0] == ATT_READ_TYPE_REQ || p[0] == ATT_READ_GROUP_REQ || p[0] == 0x06 ? ATT_E_NOT_FOUND : ATT_E_NOT_SUPPORTED;
        att_send(c, r, 5);
        return;
    }
    if (c->state == C_MTU && p[0] == ATT_ERROR && n >= 2 && p[1] == ATT_MTU_REQ && c->enc_pending) {
        c->mtu_retry = 1; return;                           /* a bonded device may answer only once encrypted */
    }
    if (c->state == C_MTU && (p[0] == ATT_MTU_RSP || (p[0] == ATT_ERROR && n >= 2 && p[1] == ATT_MTU_REQ))) {
        if (p[0] == ATT_MTU_RSP && n >= 3) { unsigned m = u16(p + 1); c->mtu = m < 23 ? 23 : m < ATT_MTU ? m : ATT_MTU; }
        else if (!c->peer_mtu) c->mtu = 23;                /* refused: the default, unless the device's own exchange set one */
        c->state = C_UP;
        fprintf(stderr, "bluetooth: connected to %012llx, MTU %u\n", (unsigned long long)c->addr, c->mtu);
        if (H->connection) H->connection(c->addr, 1, c->mtu, 0);
        start_op(c);
        return;
    }
    struct op *o = c->cur;
    if (!o) return;                                         /* late answer to something given up */
    if (o->kind == K_DISCOVER) { disc_rx(c, p, n); return; }
    if (p[0] == ATT_ERROR) {
        int err = n >= 5 ? p[4] : GATT_E_FAILED;
        if (o->kind == K_READ && c->vlen && err == ATT_E_NOT_LONG) { if (H->read) H->read(c->addr, o->handle, c->val, c->vlen); op_done(c); }
        else op_fail(c, o->handle, err);
        return;
    }
    switch (o->kind) {
    case K_READ:
        if (p[0] != ATT_READ_RSP && p[0] != ATT_READ_BLOB_RSP) return;
        { size_t k = n - 1 < sizeof c->val - c->vlen ? n - 1 : sizeof c->val - c->vlen; memcpy(c->val + c->vlen, p + 1, k); c->vlen += k; }
        if (n - 1 == c->mtu - 1 && c->vlen < sizeof c->val) { send_read(c); now(&c->t); return; }     /* full PDU: maybe more */
        if (H->read) H->read(c->addr, o->handle, c->val, c->vlen);
        op_done(c);
        break;
    case K_WRITE:
        if (p[0] == ATT_PREP_WRITE_RSP) {
            size_t k = o->len - c->off; if (k > c->mtu - 5) k = c->mtu - 5;
            c->off += k; now(&c->t);
            if (c->off < o->len) send_prep(c);
            else { r[0] = ATT_EXEC_WRITE_REQ; r[1] = 1; att_send(c, r, 2); }
            return;
        }
        if (p[0] != ATT_WRITE_RSP && p[0] != ATT_EXEC_WRITE_RSP) return;
        if (H->written) H->written(c->addr, o->handle);
        op_done(c);
        break;
    }
}

/* L2CAP signalling: grant connection parameter updates, refuse everything else that asks */
static void sig_rx(struct conn *c, const unsigned char *p, size_t n)
{
    unsigned char r[8];
    if (n < 4) return;
    if (p[0] == 0x12 && n >= 12) {                          /* Connection Parameter Update Request */
        memcpy(c->upd_par, p + 4, 8); c->upd = 1;
        r[0] = 0x13; r[1] = p[1]; put16(r + 2, 2); put16(r + 4, 0);
        l2cap_send(c, CID_SIG, r, 6);
    } else if (p[0] != 0x01 && p[0] != 0x13 && p[0] != 0x15 && p[0] != 0x07) {  /* not a response: Command Reject */
        r[0] = 0x01; r[1] = p[1]; put16(r + 2, 2); put16(r + 4, 0);
        l2cap_send(c, CID_SIG, r, 6);
    }
}

/* ---------------------------------------------------------------- GATT server (the peripheral link) */

/* The attribute table, built by ble_serve(): handle = index + 1.  Generic Access first (device name, appearance: what a
 * phone shows and iOS reads), then the server's service, each characteristic as declaration, value and, when it
 * notifies, its Client Characteristic Configuration descriptor. */
enum { A_SVC, A_CHR, A_VAL, A_CCCD };
#define GAP_NAME (-1)
#define GAP_LOOK (-2)
#define GAP_SVC  (-3)
struct attr { int kind, chr; };                 /* chr: the server's characteristic, or one of GAP_* */
static const struct ble_server *S;
static struct attr attrs[8 + 3 * BLE_SRV_CHR]; static unsigned nattr, svc_start;
static unsigned cccd_on;                        /* controller thread: bit per characteristic, the phone subscribed */
static atomic_uint notify_due;                  /* bit per characteristic: changed since last sent (ble_server_notify) */
static atomic_int drop_srv;                     /* ble_server_drop() */
static struct { unsigned char b[512]; size_t n; unsigned handle; } prep;     /* prepared writes: one value, as they come */

static void attr_add(int kind, int chr) { attrs[nattr].kind = kind; attrs[nattr].chr = chr; nattr++; }

/* the attribute's type; its length (2 or 16) */
static size_t attr_type(unsigned h, unsigned char *t)
{
    const struct attr *a = &attrs[h - 1]; unsigned u = 0;
    switch (a->kind) {
    case A_SVC: u = 0x2800; break;
    case A_CHR: u = 0x2803; break;
    case A_CCCD: u = 0x2902; break;
    case A_VAL:
        if (a->chr == GAP_NAME) u = 0x2a00;
        else if (a->chr == GAP_LOOK) u = 0x2a01;
        else { memcpy(t, S->chr[a->chr].uuid, 16); return 16; }
    }
    put16(t, u);
    return 2;
}

static unsigned attr_props(const struct attr *a) { return a->chr < 0 ? BLE_PROP_READ : S->chr[a->chr].props; }

/* its value, at most cap bytes */
static size_t attr_value(unsigned h, unsigned char *v, size_t cap)
{
    const struct attr *a = &attrs[h - 1]; unsigned char t[24]; size_t n = 0;
    switch (a->kind) {
    case A_SVC:
        if (a->chr == GAP_SVC) { put16(t, 0x1800); n = 2; } else { memcpy(t, S->uuid, 16); n = 16; }
        break;
    case A_CHR:                                 /* properties, value handle, value type */
        t[0] = attr_props(&attrs[h]); put16(t + 1, h + 1); n = 3 + attr_type(h + 1, t + 3);
        break;
    case A_CCCD: put16(t, cccd_on >> a->chr & 1); n = 2; break;
    case A_VAL:
        if (a->chr == GAP_LOOK) { put16(t, 0x0840); n = 2; break; }       /* appearance: generic speaker */
        return S->read(a->chr == GAP_NAME ? -1 : a->chr, v, cap);
    }
    if (n > cap) n = cap;
    memcpy(v, t, n);
    return n;
}

static unsigned svc_end(unsigned h) { return h < svc_start ? svc_start - 1 : nattr; }    /* h: a service declaration */

static void att_err(struct conn *c, unsigned op, unsigned h, unsigned e)
{
    unsigned char r[5] = { ATT_ERROR, (unsigned char)op }; put16(r + 2, h); r[4] = e;
    att_send(c, r, 5);
}

/* the handle range of a find or read-by request: 0 if it is valid, else answered with the error */
static int range(struct conn *c, const unsigned char *p, size_t n, size_t min, unsigned *s, unsigned *e)
{
    if (n < min) { att_err(c, p[0], 0, ATT_E_INVALID_PDU); return -1; }
    *s = u16(p + 1); *e = u16(p + 3);
    if (!*s || *s > *e) { att_err(c, p[0], *s, ATT_E_INVALID_HANDLE); return -1; }
    if (*s > nattr) { att_err(c, p[0], *s, ATT_E_NOT_FOUND); return -1; }
    if (*e > nattr) *e = nattr;
    return 0;
}

static void write_value(struct conn *c, const struct attr *a, const unsigned char *v, size_t n)
{
    (void)c;
    if (a->kind == A_CCCD) { if (n >= 1 && v[0] & 1) cccd_on |= 1u << a->chr; else cccd_on &= ~(1u << a->chr); }
    else S->write(a->chr, v, n);
}

/* 0 if the attribute takes a write of this kind, else the ATT error */
static int writable(unsigned h, int cmd)
{
    if (!h || h > nattr) return ATT_E_INVALID_HANDLE;
    const struct attr *a = &attrs[h - 1];
    if (a->kind == A_CCCD) return 0;
    if (a->kind != A_VAL || a->chr < 0) return ATT_E_WRITE_NOT_PERMITTED;
    return attr_props(a) & (cmd ? BLE_PROP_WRITE_NR | BLE_PROP_WRITE : BLE_PROP_WRITE) ? 0 : ATT_E_WRITE_NOT_PERMITTED;
}

static void gatts_rx(struct conn *c, const unsigned char *p, size_t n)
{
    unsigned char r[ATT_MTU], t[16], v[512]; size_t mx = c->mtu, o = 0, tl, vl, w = 0; unsigned s, e, h; int err;
    if (!n || !S) return;
    switch (p[0]) {
    case ATT_MTU_REQ:
        if (n >= 3) { unsigned m = u16(p + 1); c->mtu = m < 23 ? 23 : m < ATT_MTU ? m : ATT_MTU; }
        r[0] = ATT_MTU_RSP; put16(r + 1, ATT_MTU); att_send(c, r, 3);
        return;
    case ATT_FIND_INFO_REQ:                     /* handle and type of each attribute: one type length per answer */
        if (range(c, p, n, 5, &s, &e)) return;
        r[0] = ATT_FIND_INFO_RSP; o = 2;
        for (h = s; h <= e; h++) {
            tl = attr_type(h, t);
            if ((o > 2 && tl != w) || o + 2 + tl > mx) break;
            w = tl; put16(r + o, h); memcpy(r + o + 2, t, tl); o += 2 + tl;
        }
        r[1] = w == 2 ? 1 : 2;
        att_send(c, r, o);
        return;
    case ATT_FIND_TYPE_REQ:                     /* services by UUID: found handle, group end */
        if (range(c, p, n, 7, &s, &e)) return;
        r[0] = ATT_FIND_TYPE_RSP; o = 1;
        if (u16(p + 5) == 0x2800)
            for (h = s; h <= e && o + 4 <= mx; h++)
                if (attrs[h - 1].kind == A_SVC && (vl = attr_value(h, v, sizeof v)) == n - 7 && !memcmp(v, p + 7, vl)) {
                    put16(r + o, h); put16(r + o + 2, svc_end(h)); o += 4;
                }
        if (o == 1) att_err(c, p[0], s, ATT_E_NOT_FOUND); else att_send(c, r, o);
        return;
    case ATT_READ_TYPE_REQ:                     /* characteristic declarations, or values by type */
    case ATT_READ_GROUP_REQ:                    /* services */
        if (range(c, p, n, 7, &s, &e)) return;
        if (n != 7 && n != 21) { att_err(c, p[0], s, ATT_E_INVALID_PDU); return; }
        if (p[0] == ATT_READ_GROUP_REQ && (n != 7 || u16(p + 5) != 0x2800)) { att_err(c, p[0], s, ATT_E_UNSUPPORTED_GROUP); return; }
        r[0] = p[0] + 1; o = 2;
        for (h = s; h <= e; h++) {
            if ((tl = attr_type(h, t)) != n - 5 || memcmp(t, p + 5, tl)) continue;
            if (attrs[h - 1].kind == A_VAL && !(attr_props(&attrs[h - 1]) & BLE_PROP_READ)) {
                if (o == 2) { att_err(c, p[0], h, ATT_E_READ_NOT_PERMITTED); return; }
                break;
            }
            size_t hd = p[0] == ATT_READ_GROUP_REQ ? 4 : 2, room = mx - o - hd;
            vl = attr_value(h, v, sizeof v);
            if (vl > 253 - hd) vl = 253 - hd;
            if (o == 2) { if (vl > room) vl = room; w = hd + vl; r[1] = (unsigned char)w; }     /* the first sets the length */
            else if (hd + vl != w || o + w > mx) break;
            put16(r + o, h); if (hd == 4) put16(r + o + 2, svc_end(h));
            memcpy(r + o + hd, v, vl); o += w;
            if (h < e && p[0] == ATT_READ_GROUP_REQ) h = svc_end(h);        /* on to the next service */
        }
        if (o == 2) att_err(c, p[0], s, ATT_E_NOT_FOUND); else att_send(c, r, o);
        return;
    case ATT_READ_REQ: case ATT_READ_BLOB_REQ: {
        size_t off = p[0] == ATT_READ_BLOB_REQ && n >= 5 ? u16(p + 3) : 0;
        if (n < (p[0] == ATT_READ_REQ ? 3u : 5u)) { att_err(c, p[0], 0, ATT_E_INVALID_PDU); return; }
        h = u16(p + 1);
        if (!h || h > nattr) { att_err(c, p[0], h, ATT_E_INVALID_HANDLE); return; }
        if (attrs[h - 1].kind == A_VAL && !(attr_props(&attrs[h - 1]) & BLE_PROP_READ)) { att_err(c, p[0], h, ATT_E_READ_NOT_PERMITTED); return; }
        vl = attr_value(h, v, sizeof v);
        if (off > vl) { att_err(c, p[0], h, ATT_E_INVALID_OFFSET); return; }
        vl -= off; if (vl > mx - 1) vl = mx - 1;
        r[0] = p[0] + 1; memcpy(r + 1, v + off, vl);
        att_send(c, r, 1 + vl);
        return; }
    case ATT_WRITE_REQ: case ATT_WRITE_CMD:
        if (n < 3) { if (p[0] == ATT_WRITE_REQ) att_err(c, p[0], 0, ATT_E_INVALID_PDU); return; }
        h = u16(p + 1);
        if ((err = writable(h, p[0] == ATT_WRITE_CMD))) { if (p[0] == ATT_WRITE_REQ) att_err(c, p[0], h, err); return; }
        if (p[0] == ATT_WRITE_REQ) { r[0] = ATT_WRITE_RSP; att_send(c, r, 1); }     /* answered first: notifications follow it */
        write_value(c, &attrs[h - 1], p + 3, n - 3);
        return;
    case ATT_PREP_WRITE_REQ: {                  /* a long write: pieces queued, delivered whole on execute */
        if (n < 5) { att_err(c, p[0], 0, ATT_E_INVALID_PDU); return; }
        h = u16(p + 1); size_t off = u16(p + 3);
        if ((err = writable(h, 0))) { att_err(c, p[0], h, err); return; }
        if ((prep.n && prep.handle != h) || off + n - 5 > sizeof prep.b) { att_err(c, p[0], h, ATT_E_PREP_QUEUE_FULL); return; }
        prep.handle = h; memcpy(prep.b + off, p + 5, n - 5);
        if (off + n - 5 > prep.n) prep.n = off + n - 5;
        memcpy(r, p, n); r[0] = ATT_PREP_WRITE_RSP;
        att_send(c, r, n);
        return; }
    case ATT_EXEC_WRITE_REQ:
        r[0] = ATT_EXEC_WRITE_RSP; att_send(c, r, 1);
        if (n >= 2 && p[1] == 1 && prep.handle) write_value(c, &attrs[prep.handle - 1], prep.b, prep.n);
        prep.n = 0; prep.handle = 0;
        return;
    }
    if (!(p[0] & 1) && p[0] != ATT_CONFIRM && !(p[0] & 0x40)) att_err(c, p[0], n >= 3 ? u16(p + 1) : 0, ATT_E_NOT_SUPPORTED);
}

/* what changed and is subscribed to, as notifications (controller thread) */
static void gatts_notify(struct conn *c)
{
    unsigned due = atomic_exchange(&notify_due, 0) & cccd_on;
    for (unsigned h = 1; due && h <= nattr; h++) {
        const struct attr *a = &attrs[h - 1];
        if (a->kind != A_VAL || a->chr < 0 || !(due & 1u << a->chr)) continue;
        unsigned char r[ATT_MTU];
        r[0] = ATT_NOTIFY; put16(r + 1, h);
        size_t vl = S->read(a->chr, r + 3, c->mtu - 3);
        att_send(c, r, 3 + vl);
        due &= ~(1u << a->chr);
    }
}

/* the phone's link is gone: its state with it */
static void srv_gone(struct conn *c)
{
    free_conn(c);
    cccd_on = 0; prep.n = 0; prep.handle = 0; atomic_store(&notify_due, 0);
    if (S && S->connection) S->connection(0);
}

/* ---------------------------------------------------------------- advertising */

static pthread_mutex_t adv_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char adv_data[31], rsp_data[31]; static size_t adv_n, rsp_n;    /* adv_lock */
static atomic_int adv_gen;                      /* bumped by every ble_advertise() */
static int adv_applied = -1, advertising;       /* controller thread: the generation the controller has; it advertises */
static struct timespec adv_failed;              /* when the controller last refused: not again for 5 s */

/* Advertising as asked, while no phone is connected (the controller stops when one connects).  -1: controller gone */
static int adv_upkeep(void)
{
    int g = atomic_load(&adv_gen), want, st;
    unsigned char a[32] = { 0 }, r[32] = { 0 };
    pthread_mutex_lock(&adv_lock);
    want = adv_n > 0 && conns[SRV].state == C_FREE;
    a[0] = (unsigned char)adv_n; memcpy(a + 1, adv_data, adv_n); r[0] = (unsigned char)rsp_n; memcpy(r + 1, rsp_data, rsp_n);
    pthread_mutex_unlock(&adv_lock);
    if (g == adv_applied && want == advertising) return 0;
    if (want && !advertising && adv_failed.tv_sec && ms_since(&adv_failed) < 5000) return 0;
    if (advertising) {
        unsigned char off = 0;
        if (cmd(OP_LE_ADV_ENABLE, &off, 1) < 0) return -1;
        advertising = 0;
    }
    adv_applied = g;
    if (!want) return 0;
    /* ADV_IND every 100..150 ms on all three channels, public address, no filter: what ESPHome's Improv does */
    unsigned char par[15] = { 0xa0, 0, 0xf0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7, 0 }, on = 1;
    if ((st = cmd(OP_LE_ADV_PARAMS, par, 15)) == 0 && (st = cmd(OP_LE_ADV_DATA, a, 32)) == 0 &&
        (st = cmd(OP_LE_SCAN_RSP, r, 32)) == 0) st = cmd(OP_LE_ADV_ENABLE, &on, 1);
    if (st < 0) return -1;
    if (st) {
        if (!adv_failed.tv_sec) fprintf(stderr, "bluetooth: advertising refused, HCI status 0x%02x (command %04x); again every 5 s\n", st, cc_op);
        now(&adv_failed);
        return 0;
    }
    if (adv_failed.tv_sec) fprintf(stderr, "bluetooth: advertising\n");
    adv_failed.tv_sec = 0; advertising = 1;
    return 0;
}

/* ---------------------------------------------------------------- bonds and pairing */

struct bond { uint64_t addr; unsigned type, sc, keysize, ediv, has_irk; unsigned char ltk[16], rand[8], irk[16]; };
static struct bond bonds[MAX_BONDS]; static int nbonds;
/* Our P-256 key pair lives in the controller.  The Echo's MediaTek chip gives and takes P-256 values (public keys, the
 * DHKey) most significant octet first, against the spec, which has them least significant first like SMP: seen on the
 * device, its key was only on the curve read that way, and BlueZ refused it as sent.  Converted at the HCI boundary. */
static int have_sc, pk_ready; static unsigned char local_pk[64];   /* SMP order */
static void swap_coords(unsigned char *out, const unsigned char *in) { bytes_reverse(out, in, 32); bytes_reverse(out + 32, in + 32, 32); }
static unsigned char own_addr[6];                                  /* public, least significant octet first */

static const char *bonds_path(void)
{
    static char p[256]; const char *d = getenv("HASSMIC_STATE");
    snprintf(p, sizeof p, "%s/ble_bonds", d ? d : "/data/local/hassmic/state");
    return p;
}

static void hexout(FILE *f, const unsigned char *b, size_t n) { for (size_t i = 0; i < n; i++) fprintf(f, "%02x", b[i]); fputc(' ', f); }
static int hexin(const char *s, unsigned char *b, size_t n) { for (size_t i = 0; i < n; i++) if (sscanf(s + 2 * i, "%2hhx", &b[i]) != 1) return -1; return 0; }

/* one line per device: address type sc keysize ediv ltk rand irk|- (hex, keys as the controller takes them) */
static void bonds_load(void)
{
    char l[256], ltk[40], rnd[24], irk[40]; FILE *f = fopen(bonds_path(), "r"); unsigned long long a;
    if (!f) return;
    while (nbonds < MAX_BONDS && fgets(l, sizeof l, f)) {
        struct bond *b = &bonds[nbonds];
        if (sscanf(l, "%llx %u %u %u %u %39s %23s %39s", &a, &b->type, &b->sc, &b->keysize, &b->ediv, ltk, rnd, irk) != 8 ||
            hexin(ltk, b->ltk, 16) || hexin(rnd, b->rand, 8)) continue;
        b->addr = a; b->has_irk = hexin(irk, b->irk, 16) == 0;
        nbonds++;
    }
    fclose(f);
    if (nbonds) fprintf(stderr, "bluetooth: %d paired device%s\n", nbonds, nbonds == 1 ? "" : "s");
}

static void bonds_save(void)
{
    char tmp[300]; snprintf(tmp, sizeof tmp, "%s.tmp", bonds_path());
    int fdw = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600); FILE *f = fdw >= 0 ? fdopen(fdw, "w") : NULL;
    if (!f) { if (fdw >= 0) close(fdw); fprintf(stderr, "bluetooth: cannot write %s\n", tmp); return; }
    for (int i = 0; i < nbonds; i++) {
        struct bond *b = &bonds[i];
        fprintf(f, "%012llx %u %u %u %u ", (unsigned long long)b->addr, b->type, b->sc, b->keysize, b->ediv);
        hexout(f, b->ltk, 16); hexout(f, b->rand, 8);
        if (b->has_irk) hexout(f, b->irk, 16); else fputs("- ", f);
        fputc('\n', f);
    }
    if (fclose(f) || rename(tmp, bonds_path())) { unlink(tmp); fprintf(stderr, "bluetooth: cannot write %s\n", bonds_path()); }
}

static void addr_bytes(uint64_t a, unsigned char o[6]) { for (int i = 0; i < 6; i++) o[i] = a >> (8 * i); }

/* The bond for this address: the same address, or a private address made with the device's IRK */
static struct bond *bond_for(uint64_t addr, unsigned type)
{
    unsigned char ab[6]; addr_bytes(addr, ab);
    for (int i = 0; i < nbonds; i++) if (bonds[i].addr == addr && bonds[i].type == type) return &bonds[i];
    if (type == 1) for (int i = 0; i < nbonds; i++) if (bonds[i].has_irk && smp_ah_match(bonds[i].irk, ab)) return &bonds[i];
    return NULL;
}

static void bond_forget(struct bond *b)
{
    fprintf(stderr, "bluetooth: bond with %012llx removed\n", (unsigned long long)b->addr);
    *b = bonds[--nbonds]; bonds_save();
}

static void smp_send(struct conn *c, const void *p, size_t n) { l2cap_send(c, CID_SMP, p, n); }

static void pair_done(struct conn *c, int ok, int err)
{
    struct smp *s = &c->smp;
    fprintf(stderr, "bluetooth: pairing with %012llx %s (%s, 0x%02x)\n", (unsigned long long)c->addr, ok ? "done" : "failed",
            s->sc ? "LE Secure Connections" : "legacy", err);
    s->state = S_IDLE;
    if (s->asked && H->paired) H->paired(c->addr, ok, err);
    s->asked = 0;
}

static void smp_fail(struct conn *c, int reason)
{
    unsigned char p[2] = { 0x05, reason };
    smp_send(c, p, 2);
    pair_done(c, 0, reason);
}

static void encrypt_with(struct conn *c, const unsigned char ltk[16], const unsigned char rand[8], unsigned ediv)
{
    if (ltk != c->enc_ltk) memcpy(c->enc_ltk, ltk, 16);
    memcpy(c->enc_rand, rand, 8); c->enc_ediv = ediv;
    c->need_enc = c->enc_pending = 1;
}

static void smp_start(struct conn *c)
{
    struct smp *s = &c->smp;
    /* Pairing Request: NoInputNoOutput, no OOB, bonding + (Secure Connections), 16 byte keys, we distribute nothing,
     * the device its encryption key (legacy) and identity key */
    unsigned char p[7] = { 0x01, 0x03, 0x00, 0x01 | (have_sc ? 0x08 : 0), 16, 0x00, 0x03 };
    int asked = s->asked;
    memset(s, 0, sizeof *s); s->asked = asked;
    memcpy(s->preq, p, 7); s->state = S_RSP; now(&s->t);
    smp_send(c, p, 7);
}

/* addresses for f5/f6: type, then the address most significant octet first */
static void addr_a(unsigned type, const unsigned char ab[6], unsigned char a[7]) { a[0] = type; bytes_reverse(a + 1, ab, 6); }

static void random_bytes(unsigned char *b, size_t n)
{
    int r = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (r < 0 || read(r, b, n) != (ssize_t)n) fprintf(stderr, "bluetooth: no /dev/urandom\n");
    if (r >= 0) close(r);
}

static void smp_finish(struct conn *c)                      /* encrypted, keys received: remember them */
{
    struct smp *s = &c->smp; struct bond *b = bond_for(c->addr, c->addr_type);
    int legacy_key = !s->sc && (s->got & 3) == 3;
    if (s->sc || legacy_key) {
        if (!b) { if (nbonds == MAX_BONDS) bond_forget(&bonds[0]); b = &bonds[nbonds++]; }
        memset(b, 0, sizeof *b);
        b->addr = c->addr; b->type = c->addr_type; b->sc = s->sc; b->keysize = s->keysize;
        if (s->sc) memcpy(b->ltk, c->enc_ltk, 16);         /* LE Secure Connections: the key both sides derived */
        else { memcpy(b->ltk, s->ltk, 16); memcpy(b->rand, s->rand, 8); b->ediv = s->ediv; }
        if (s->got & 4) { memcpy(b->irk, s->irk, 16); b->has_irk = 1; }
        bonds_save();
    } else fprintf(stderr, "bluetooth: %012llx gave no key, paired for this connection only\n", (unsigned long long)c->addr);
    pair_done(c, 1, 0);
}

/* The device's side of the pairing (spec Vol 3 Part H 2.3-2.4, 3.5-3.6) */
static void smp_rx(struct conn *c, const unsigned char *p, size_t n)
{
    struct smp *s = &c->smp; unsigned char out[65], ra[6], o[16], x[16];
    if (!n) return;
    addr_bytes(c->addr, ra);
    if (p[0] == 0x0b) {                                     /* Security Request: accept, like ESP-IDF */
        struct bond *b = bond_for(c->addr, c->addr_type);
        if (s->state != S_IDLE || c->enc_pending) return;
        if (b) { if (!c->enc) encrypt_with(c, b->ltk, b->rand, b->ediv); }
        else smp_start(c);
        return;
    }
    if (p[0] == 0x05) { if (s->state != S_IDLE) pair_done(c, 0, n >= 2 ? p[1] : 0x08); return; }
    if (p[0] == 0x01) { out[0] = 0x05; out[1] = 0x05; smp_send(c, out, 2); return; }   /* we initiate, never respond */
    if (s->state == S_IDLE) return;
    now(&s->t);
    switch (s->state) {
    case S_RSP:
        if (p[0] != 0x02 || n < 7) break;
        memcpy(s->pres, p, 7);
        s->sc = have_sc && s->preq[3] & p[3] & 0x08;
        s->keysize = p[4] < 16 ? p[4] : 16;
        if (s->keysize < 7) { smp_fail(c, 0x06); return; }  /* encryption key size */
        { unsigned dist = s->preq[6] & p[6];                /* EncKey = LTK + EDIV/Rand (legacy only), IdKey = IRK + address */
          s->want = (!s->sc && dist & 1 ? 0x03 : 0) | (dist & 2 ? 0x0c : 0); }
        if (s->sc) { out[0] = 0x0c; memcpy(out + 1, local_pk, 64); smp_send(c, out, 65); s->state = S_PK; return; }
        random_bytes(s->rnd, 16); memset(x, 0, 16);         /* Just Works: TK = 0 */
        smp_c1(x, s->rnd, s->preq, s->pres, 0, own_addr, c->addr_type, ra, o);
        out[0] = 0x03; memcpy(out + 1, o, 16); smp_send(c, out, 17); s->state = S_CONFIRM;
        return;
    case S_CONFIRM:                                         /* legacy: their confirm, then our random */
        if (p[0] != 0x03 || n < 17) break;
        memcpy(s->conf, p + 1, 16);
        out[0] = 0x04; memcpy(out + 1, s->rnd, 16); smp_send(c, out, 17); s->state = S_RANDOM;
        return;
    case S_RANDOM:
        if (p[0] != 0x04 || n < 17) break;
        memcpy(s->peer_rnd, p + 1, 16); memset(x, 0, 16);
        smp_c1(x, s->peer_rnd, s->preq, s->pres, 0, own_addr, c->addr_type, ra, o);
        if (memcmp(o, s->conf, 16)) { smp_fail(c, 0x04); return; }         /* confirm value failed */
        smp_s1(x, s->peer_rnd, s->rnd, o);                  /* STK */
        memset(o + s->keysize, 0, 16 - s->keysize);
        { unsigned char r0[8] = { 0 }; encrypt_with(c, o, r0, 0); }
        s->state = S_ENC;
        return;
    case S_PK:                                              /* LE Secure Connections */
        if (p[0] != 0x0c || n < 65) break;
        memcpy(s->pkb, p + 1, 64);
        if (!memcmp(s->pkb, local_pk, 32)) { smp_fail(c, 0x08); return; }  /* our own key reflected */
        s->state = S_CB;
        return;
    case S_CB:
        if (p[0] != 0x03 || n < 17) break;
        memcpy(s->conf, p + 1, 16); random_bytes(s->rnd, 16);
        out[0] = 0x04; memcpy(out + 1, s->rnd, 16); smp_send(c, out, 17); s->state = S_NB;
        return;
    case S_NB: {
        if (p[0] != 0x04 || n < 17) break;
        unsigned char u[32], v[32], nb[16];
        memcpy(s->peer_rnd, p + 1, 16);
        bytes_reverse(u, s->pkb, 32); bytes_reverse(v, local_pk, 32); bytes_reverse(nb, s->peer_rnd, 16);
        smp_f4(u, v, nb, 0, o); bytes_reverse(o, o, 16);    /* Cb = f4(PKbx, PKax, Nb, 0) */
        if (memcmp(o, s->conf, 16)) { smp_fail(c, 0x04); return; }
        c->need_dhkey = 1; s->state = S_DHKEY;              /* upkeep() asks the controller */
        return; }
    case S_EB: {
        if (p[0] != 0x0d || n < 17) break;
        unsigned char na[16], nb[16], a1[7], a2[7], r0[16] = { 0 }, io[3] = { s->pres[3], s->pres[2], s->pres[1] };
        bytes_reverse(na, s->rnd, 16); bytes_reverse(nb, s->peer_rnd, 16);
        addr_a(0, own_addr, a1); addr_a(c->addr_type, ra, a2);
        smp_f6(s->mackey, nb, na, r0, io, a2, a1, o); bytes_reverse(o, o, 16);  /* Eb */
        if (memcmp(o, p + 1, 16)) { smp_fail(c, 0x0b); return; }          /* DHKey check failed */
        { unsigned char r8[8] = { 0 }; encrypt_with(c, c->enc_ltk, r8, 0); }
        s->state = S_ENC;
        return; }
    case S_KEYS:
        if (p[0] == 0x06 && n >= 17) { memcpy(s->ltk, p + 1, 16); s->got |= 1; }
        else if (p[0] == 0x07 && n >= 11) { s->ediv = u16(p + 1); memcpy(s->rand, p + 3, 8); s->got |= 2; }
        else if (p[0] == 0x08 && n >= 17) { memcpy(s->irk, p + 1, 16); s->got |= 4; }
        else if (p[0] == 0x09 && n >= 8) { memcpy(s->id, p + 1, 7); s->got |= 8; }
        else if (p[0] == 0x0a) return;                      /* signing key: not used */
        else break;
        if ((s->got & s->want) == s->want) smp_finish(c);
        return;
    }
    smp_fail(c, 0x08);                                      /* unexpected PDU: unspecified reason */
}

/* LE Generate DHKey Complete: finish the key calculation, send our DHKey check */
static void dhkey_done(int status, const unsigned char *dh)
{
    struct conn *c = dh_owner; struct smp *s;
    dh_owner = NULL;
    if (!c || (s = &c->smp)->state != S_DHKEY) return;
    if (status) { smp_fail(c, 0x0b); return; }
    unsigned char w[32], na[16], nb[16], a1[7], a2[7], ra[6], ltk[16], ea[16], r0[16] = { 0 }, out[17];
    unsigned char io[3] = { s->preq[3], s->preq[2], s->preq[1] };
    addr_bytes(c->addr, ra);
    memcpy(w, dh, 32); bytes_reverse(na, s->rnd, 16);      /* the chip's DHKey is in spec order already, see local_pk */ bytes_reverse(nb, s->peer_rnd, 16);
    addr_a(0, own_addr, a1); addr_a(c->addr_type, ra, a2);
    smp_f5(w, na, nb, a1, a2, s->mackey, ltk);
    bytes_reverse(c->enc_ltk, ltk, 16);                     /* kept until the link is encrypted with it */
    memset(c->enc_ltk + s->keysize, 0, 16 - s->keysize);
    smp_f6(s->mackey, na, nb, r0, io, a1, a2, ea);
    out[0] = 0x0d; bytes_reverse(out + 1, ea, 16); smp_send(c, out, 17);
    s->state = S_EB; now(&s->t);
}

/* Encryption Change / Key Refresh */
static void encrypted(struct conn *c, int status)
{
    struct smp *s = &c->smp;
    c->enc_pending = 0;
    if (c->mtu_retry && c->state == C_MTU) {
        unsigned char m[3] = { ATT_MTU_REQ }; put16(m + 1, ATT_MTU);
        c->mtu_retry = 0; now(&c->t); att_send(c, m, 3);
    }
    if (!status) {
        c->enc = 1;
        if (s->state == S_ENC) { if (!s->want) smp_finish(c); else { s->state = S_KEYS; now(&s->t); } }
        else if (s->asked && s->state == S_IDLE) pair_done(c, 1, 0);     /* asked to pair: the bond was enough */
    } else {
        fprintf(stderr, "bluetooth: encrypting the link to %012llx failed (0x%02x)\n", (unsigned long long)c->addr, status);
        if (s->state == S_ENC) pair_done(c, 0, status);
        else {
            struct bond *b = bond_for(c->addr, c->addr_type);
            if (b && status == 0x06) bond_forget(b);        /* PIN or key missing: the device forgot us */
            if (s->asked) pair_done(c, 0, status);
        }
    }
    start_op(c);
}

static void acl_rx(const unsigned char *p, size_t n)     /* p: after the H4 type byte */
{
    if (n < 4) return;
    struct conn *c = by_handle(u16(p) & 0x0fff); unsigned pb = p[1] >> 4 & 3; size_t len = u16(p + 2);
    if (!c) { a2dp_acl(p, n); return; }
    if (len > n - 4) return;
    p += 4;
    if (pb != 1) {                                          /* first fragment (not "continuing"): L2CAP header inside */
        if (len < 4) return;
        c->rxwant = 4 + u16(p); c->rxlen = 0;
    } else if (!c->rxwant) return;
    if (c->rxlen + len > sizeof c->rx) { c->rxwant = 0; return; }       /* bigger than we ever asked for: drop */
    memcpy(c->rx + c->rxlen, p, len); c->rxlen += len;
    if (c->rxlen < c->rxwant) return;
    c->rxwant = 0; c->heard = 1;
    unsigned cid = u16(c->rx + 2); size_t l = u16(c->rx);
    if (c->srv) {
        if (cid == CID_ATT) gatts_rx(c, c->rx + 4, l);
        else if (cid == CID_SIG) sig_rx(c, c->rx + 4, l);
        else if (cid == CID_SMP && l >= 1 && c->rx[4] == 0x01) { unsigned char f[2] = { 0x05, 0x05 }; smp_send(c, f, 2); }  /* Pairing Not Supported */
        return;
    }
    if (cid == CID_ATT) att_rx(c, c->rx + 4, l);
    else if (cid == CID_SIG) sig_rx(c, c->rx + 4, l);
    else if (cid == CID_SMP) smp_rx(c, c->rx + 4, l);
}

/* ---------------------------------------------------------------- HCI events */

static void flush_batch(void)
{
    if (nbatch && H->adverts) H->adverts(batch, nbatch);
    nbatch = 0;
}

/* LE Advertising Report: reports one after the other (what controllers send and BlueZ parses, whatever the spec's
 * array notation suggests); in practice there is one per event */
static void adv_report(const unsigned char *p, size_t n)
{
    unsigned count = n ? p[0] : 0; size_t i = 1;
    for (unsigned r = 0; r < count && i + 9 <= n; r++) {
        unsigned len = p[i + 8];
        if (i + 9 + len + 1 > n || len > 31) return;
        struct ble_adv *a = &batch[nbatch];
        a->addr_type = p[i + 1];
        a->addr = 0; for (int b = 5; b >= 0; b--) a->addr = a->addr << 8 | p[i + 2 + b];    /* little endian on the air */
        a->len = len; memcpy(a->data, p + i + 9, len);
        a->rssi = (signed char)p[i + 9 + len];
        if (!nbatch) now(&batch_t0);
        if (++nbatch == BLE_BATCH) flush_batch();
        i += 9 + len + 1;
    }
}

static void conn_complete(const unsigned char *q, size_t n)
{
    struct conn *c = in_state(C_CONNECTING);
    if (n >= 18 && !q[0] && q[3] == 1) {                    /* role peripheral: a phone connected to our advertising */
        advertising = 0;                                    /* the controller stopped it */
        c = &conns[SRV];
        if (c->state != C_FREE || !S) {                     /* cannot be: one advertising, one link */
            unsigned char d[3]; put16(d, u16(q + 1) & 0x0fff); d[2] = HCI_E_USER_ENDED;
            hci_write((unsigned char[]){ H4_CMD, OP_DISCONNECT & 0xff, OP_DISCONNECT >> 8, 3, d[0], d[1], d[2] }, 7);
            return;
        }
        memset(c, 0, sizeof *c);
        c->srv = 1; c->state = C_UP; c->handle = u16(q + 1) & 0x0fff; c->mtu = 23; c->addr_type = q[4]; now(&c->t);
        for (int b = 5; b >= 0; b--) c->addr = c->addr << 8 | q[5 + b];
        fprintf(stderr, "bluetooth: %012llx connected to us\n", (unsigned long long)c->addr);
        if (S->connection) S->connection(1);
        return;
    }
    if (!c || n < 18) return;
    if (q[0]) {                                             /* failed or cancelled */
        int err = c->cancel == 1 ? 0 : c->cancel == 2 ? HCI_E_TIMEOUT : q[0];
        fprintf(stderr, "bluetooth: connecting to %012llx failed (0x%02x)\n", (unsigned long long)c->addr, c->cancel ? err : q[0]);
        uint64_t a = c->addr; free_conn(c);
        if (H->connection) H->connection(a, 0, 0, err);
        return;
    }
    c->handle = u16(q + 1) & 0x0fff; c->state = C_MTU; c->mtu = ATT_MTU; now(&c->t);
    if (c->cancel) { c->close_now = 1; return; }            /* asked to go away while it came up */
    struct bond *b = bond_for(c->addr, c->addr_type);
    if (b) encrypt_with(c, b->ltk, b->rand, b->ediv);      /* paired before: encrypted before any GATT request */
    unsigned char p[3] = { ATT_MTU_REQ }; put16(p + 1, ATT_MTU);
    att_send(c, p, 3);
}

static void event(const unsigned char *p, size_t n)            /* p: event code, length, parameters */
{
    const unsigned char *q = p + 2; struct conn *c;
    if (n < 2 || a2dp_event(p, n)) return;
    n -= 2;
    switch (p[0]) {
    case EV_CMD_COMPLETE:
        if (n >= 4) { cc_op = u16(q + 1); cc_status = q[3]; memcpy(cc_ret, q + 4, n - 4 < sizeof cc_ret ? n - 4 : sizeof cc_ret); }
        break;
    case EV_CMD_STATUS: if (n >= 4) { cc_op = u16(q + 2); cc_status = q[0]; } break;
    case EV_DISCONNECT:
        if (n >= 4 && !q[0] && (c = by_handle(u16(q + 1) & 0x0fff))) {
            int err = c->state == C_CLOSING ? 0 : q[3]; uint64_t a = c->addr;
            fprintf(stderr, "bluetooth: %012llx disconnected (0x%02x)\n", (unsigned long long)a, q[3]);
            if (c->srv) { srv_gone(c); break; }
            free_conn(c);
            if (H->connection) H->connection(a, 0, 0, err);
        }
        break;
    case EV_ENC_CHANGE: case EV_ENC_REFRESH:
        if (n >= 3 && (c = by_handle(u16(q + 1) & 0x0fff))) encrypted(c, q[0] ? q[0] : p[0] == EV_ENC_CHANGE && n >= 4 && !q[3] ? 0x06 : 0);
        break;
    case EV_NUM_COMPLETED:
        for (unsigned i = 0; n >= 1 && i < q[0] && 1 + 4 * i + 4 <= n; i++) {
            unsigned k = u16(q + 3 + 4 * i);
            if (a2dp_completed(u16(q + 1 + 4 * i) & 0x0fff, k)) continue;      /* BR/EDR buffers: counted apart */
            credits += k; if (credits > (int)acl_num) credits = acl_num;
            if ((c = by_handle(u16(q + 1 + 4 * i) & 0x0fff))) { c->unacked -= k; if (c->unacked < 0) c->unacked = 0; }
        }
        acl_flush();
        break;
    case EV_LE_META:
        if (n < 1) break;
        if (q[0] == LE_ADV_REPORT) adv_report(q + 1, n - 1);
        else if (q[0] == LE_CONN_COMPLETE) conn_complete(q + 1, n - 1);
        else if (q[0] == LE_LTK_REQUEST && n >= 3 && (c = by_handle(u16(q + 1) & 0x0fff))) c->ltk_neg = 1;   /* never the peripheral */
        else if (q[0] == LE_PK_COMPLETE && n >= 66) { if (!q[1]) { swap_coords(local_pk, q + 2); pk_ready = 1; } else pk_ready = -1; }
        else if (q[0] == LE_DHKEY_COMPLETE && n >= 34) dhkey_done(q[1], q + 2);
        break;
    }
}

/* Reads what is there (waiting up to ms) and handles every whole packet.  -1: the device is gone. */
static int pump(int ms)
{
    struct pollfd p = { fd, POLLIN, 0 };
    if (poll(&p, 1, ms) < 0) return errno == EINTR ? 0 : -1;
    if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
    if (p.revents & POLLIN) {
        ssize_t r = read(fd, in + have, sizeof in - have);
        if (r <= 0) return -1;
        have += r;
    }
    for (;;) {
        size_t len; unsigned char pkt[sizeof in];
        if (have < 1) break;
        if (in[0] == H4_EVT) { if (have < 3) break; len = 3 + in[2]; }
        else if (in[0] == H4_ACL) { if (have < 5) break; len = 5 + u16(in + 3); }
        else { memmove(in, in + 1, --have); continue; }        /* not a packet start: resync (after a partial drain) */
        if (len > sizeof in) return -1;
        if (have < len) break;
        memcpy(pkt, in, len); memmove(in, in + len, have - len); have -= len;      /* handlers may pump again */
        if (pkt[0] == H4_EVT) event(pkt + 1, len - 1); else acl_rx(pkt + 1, len - 1);
    }
    if (nbatch && ms_since(&batch_t0) >= 100) flush_batch();
    return 0;
}

/* Sends a command and waits for its Command Complete / Status.  Returns the HCI status, -1 on timeout or I/O error.
 * Events arriving meanwhile are handled as usual. */
static int cmd(unsigned op, const void *par, unsigned n)
{
    unsigned char b[4 + 255] = { H4_CMD, op & 0xff, op >> 8, n };
    if (n > 255) return -1;
    if (n) memcpy(b + 4, par, n);
    cc_op = -1;
    if (write(fd, b, 4 + n) != (ssize_t)(4 + n)) return -1;
    struct timespec t0; now(&t0);
    while (cc_op != (int)op)
        if (ms_since(&t0) > 2000 || pump(100) < 0) { fprintf(stderr, "bluetooth: no answer to command %04x\n", op); return -1; }
    return cc_status;
}

/* ---------------------------------------------------------------- controller thread */

/* HASSMIC_BT_DEV: another controller, for tests (tests/fake_improv.py plays one on a pseudo terminal) */
static const char *bt_dev(void) { const char *e = getenv("HASSMIC_BT_DEV"); return e && *e ? e : board.bt_dev; }

static int stock_bt_running(void)
{
#ifdef __ANDROID__
    char p[PROP_NAME_MAX], v[PROP_VALUE_MAX] = "";
    snprintf(p, sizeof p, "init.svc.%s", board.bt_service);
    __system_property_get(p, v);
    return !strcmp(v, "running");
#else
    return 0;
#endif
}

/* The controller's address, from idme like btmac.sh does; the chip carries the same one. */
static void read_bdaddr(void)
{
    char h[16] = ""; FILE *f = fopen(board.bt_mac, "r");
    if (f) { if (fscanf(f, "%12s", h) != 1) h[0] = 0; fclose(f); }
    if (strlen(h) == 12)
        snprintf(bdaddr, sizeof bdaddr, "%.2s:%.2s:%.2s:%.2s:%.2s:%.2s", h, h + 2, h + 4, h + 6, h + 8, h + 10);
    for (char *c = bdaddr; *c; c++) if (*c >= 'a' && *c <= 'f') *c -= 32;
}

static int setup(void)
{
    /* LE events: connection, advertising report, connection update, remote features, LTK request, P-256 key, DHKey */
    static const unsigned char mask[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x3f }, le_mask[8] = { 0x9f, 0x01 };
    /* Bluetooth 4.0 LE events only: biscuit's MT8163 combo chip refuses the P-256 / DHKey bits with status 0x20
     * (unsupported parameter value).  Without those events LE Secure Connections cannot work; the supported-commands check
     * below decides it as before. */
    static const unsigned char le_mask40[8] = { 0x1f };
    int st;
    if ((st = cmd(OP_RESET, NULL, 0)) || (st = cmd(OP_EVENT_MASK, mask, 8)) ||
        ((st = cmd(OP_LE_EVENT_MASK, le_mask, 8)) && (st < 0 || (st = cmd(OP_LE_EVENT_MASK, le_mask40, 8))))) {
        if (st > 0) fprintf(stderr, "bluetooth: command %04x refused, HCI status 0x%02x\n", cc_op, st);    /* timeouts say so in cmd() */
        return -1;
    }
    if (cmd(OP_LE_READ_BUFFER, NULL, 0) == 0 && u16(cc_ret)) { acl_len = u16(cc_ret); acl_num = cc_ret[2]; }
    else if (cmd(OP_READ_BUFFER, NULL, 0) == 0) { acl_len = u16(cc_ret); acl_num = u16(cc_ret + 3); }    /* shared with BR/EDR */
    if (acl_len < 27) acl_len = 27;
    if (!acl_num) acl_num = 1;
    credits = acl_num;
    /* LE Secure Connections needs the controller's P-256: supported commands octet 34, bits 1 and 2 */
    have_sc = pk_ready = 0;
    if (cmd(OP_READ_LOCAL_CMDS, NULL, 0) == 0 && (cc_ret[34] & 0x06) == 0x06 && cmd(OP_LE_READ_PK, NULL, 0) == 0) {
        struct timespec t0; now(&t0);
        while (!pk_ready && ms_since(&t0) < 5000) if (pump(100) < 0) return -1;
        have_sc = pk_ready == 1;
    }
    fprintf(stderr, "bluetooth: controller ready, %s, ACL %u x %u, pairing: %s\n", bdaddr, acl_num, acl_len,
            have_sc ? "LE Secure Connections or legacy" : "legacy only");
    return a2dp_setup();
}

static int apply(int on, int active)
{
    unsigned char off[2] = { 0, 0 }, en[2] = { 1, 0 };          /* enable, no duplicate filter */
    unsigned char par[7] = { active, SCAN_INTERVAL & 0xff, SCAN_INTERVAL >> 8, SCAN_WINDOW & 0xff, SCAN_WINDOW >> 8, 0, 0 };
    if (atomic_load(&scanning) && cmd(OP_LE_SCAN_ENABLE, off, 2) < 0) return -1;     /* status 0x0c if not scanning: fine */
    atomic_store(&scanning, 0);
    if (on) {
        int st = cmd(OP_LE_SCAN_PARAMS, par, 7);
        if (st == 0) st = cmd(OP_LE_SCAN_ENABLE, en, 2);
        if (st < 0) return -1;
        if (st) fprintf(stderr, "bluetooth: scan refused, HCI status 0x%02x\n", st);
        else atomic_store(&scanning, 1);
    }
    return 0;
}

static void op_queue(struct conn *c, int kind, const struct req *r)
{
    struct op *o = calloc(1, sizeof *o + r->len), **t = &c->ops;
    if (!o) return;
    o->kind = kind; o->handle = r->handle; o->response = r->response; o->len = r->len; memcpy(o->data, r->data, r->len);
    while (*t) t = &(*t)->next;
    *t = o;
    start_op(c);
}

/* one request from the queue.  Returns -1 if the controller stopped answering. */
static int handle_request(struct req *r)
{
    struct conn *c = by_addr(r->addr);
    switch (r->kind) {
    case R_CONNECT:
        if (c) { if (c->state == C_UP && H->connection) H->connection(c->addr, 1, c->mtu, 0); break; }     /* already */
        if (!(c = in_state(C_FREE))) { if (H->connection) H->connection(r->addr, 0, 0, HCI_E_LIMIT); break; }
        c->state = C_WAIT; c->addr = r->addr; c->addr_type = r->addr_type; now(&c->t);
        set_slot(c, r->addr);
        break;
    case R_DISCONNECT:
        if (!c || c->state == C_WAIT) { if (c) free_conn(c); if (H->connection) H->connection(r->addr, 0, 0, 0); break; }
        if (c->state == C_CONNECTING) { if (!c->cancel) { c->cancel = 1; if (cmd(OP_LE_CONNECT_CANCEL, NULL, 0) < 0) return -1; } }
        else if (c->state != C_CLOSING) c->close_now = 1;
        break;
    case R_PAIR:
        if (!c || c->state < C_MTU || c->state == C_CLOSING) { if (H->paired) H->paired(r->addr, 0, HCI_E_UNKNOWN_CONN); break; }
        c->smp.asked = 1;
        if (c->smp.state != S_IDLE) break;                  /* under way: its result answers */
        { struct bond *b = bond_for(c->addr, c->addr_type);
          if (b && c->enc) pair_done(c, 1, 0);
          else if (b) { if (!c->enc_pending) encrypt_with(c, b->ltk, b->rand, b->ediv); }
          else smp_start(c); }
        break;
    case R_UNPAIR: {
        unsigned char ab[6]; addr_bytes(r->addr, ab);
        for (int i = nbonds - 1; i >= 0; i--)
            if (bonds[i].addr == r->addr || (bonds[i].has_irk && smp_ah_match(bonds[i].irk, ab))) bond_forget(&bonds[i]);
        if (c && c->state >= C_MTU) c->close_now = 1;        /* like ESP-IDF: removing the bond drops the link */
        if (H->unpaired) H->unpaired(r->addr, 1, 0);
        break; }
    case R_SERVICES: case R_READ: case R_WRITE:
        if (!c || c->state != C_UP) { if (H->error) H->error(r->addr, r->handle, GATT_E_FAILED); break; }
        op_queue(c, r->kind == R_SERVICES ? K_DISCOVER : r->kind == R_READ ? K_READ : K_WRITE, r);
        break;
    }
    return 0;
}

/* Connection upkeep that needs HCI commands (never sent from inside event handling) and timeouts.  -1: controller gone. */
static int upkeep(void)
{
    if (a2dp_upkeep() < 0) return -1;
    if (conns[SRV].state == C_UP) {
        if (atomic_exchange(&drop_srv, 0)) conns[SRV].close_now = 1;
        gatts_notify(&conns[SRV]);
    } else atomic_store(&drop_srv, 0);
    if (adv_upkeep() < 0) return -1;
    for (int i = 0; i < NCONN; i++) {
        struct conn *c = &conns[i]; unsigned char p[64];
        if (c->state == C_FREE) continue;
        if (c->need_dhkey && !dh_owner && c->state >= C_MTU) {         /* one at a time: the result names no connection */
            c->need_dhkey = 0; dh_owner = c;
            swap_coords(p, c->smp.pkb);
            int st = cmd(OP_LE_DHKEY, p, 64);
            if (st < 0) return -1;
            if (st) { dh_owner = NULL; smp_fail(c, 0x08); }
        }
        /* Not straight after the connection: started before the device's first packet, the link failed (0x3e) */
        if (c->need_enc && c->heard && c->state >= C_MTU && c->state != C_CLOSING) {
            c->need_enc = 0;
            put16(p, c->handle); memcpy(p + 2, c->enc_rand, 8); put16(p + 10, c->enc_ediv); memcpy(p + 12, c->enc_ltk, 16);
            int st = cmd(OP_LE_START_ENC, p, 28);
            if (st < 0) return -1;
            if (st) encrypted(c, st);
        }
        if (c->smp.state != S_IDLE && ms_since(&c->smp.t) > SMP_MS) {
            fprintf(stderr, "bluetooth: %012llx: pairing timed out\n", (unsigned long long)c->addr);
            smp_fail(c, 0x08);
        }
        if (c->ltk_neg) { c->ltk_neg = 0; put16(p, c->handle); if (cmd(OP_LE_LTK_NEG, p, 2) < 0) return -1; }
        if (c->upd && c->state >= C_MTU) {
            c->upd = 0; put16(p, c->handle); memcpy(p + 2, c->upd_par, 8); put16(p + 10, 0); put16(p + 12, 0);
            if (cmd(OP_LE_CONN_UPDATE, p, 14) < 0) return -1;
        }
        if (c->state == C_CONNECTING && !c->cancel && ms_since(&c->t) > CONNECT_MS) {
            c->cancel = 2; if (cmd(OP_LE_CONNECT_CANCEL, NULL, 0) < 0) return -1;
        }
        if (c->state == C_MTU && ms_since(&c->t) > 10000) {     /* no answer to the MTU exchange: default MTU */
            unsigned char e[5] = { ATT_ERROR, ATT_MTU_REQ }; att_rx(c, e, 5);
        }
        if (c->cur && c->state == C_UP && ms_since(&c->t) > ATT_MS) {
            fprintf(stderr, "bluetooth: %012llx: ATT timeout, disconnecting\n", (unsigned long long)c->addr);
            c->close_now = 1; op_fail(c, c->cur->handle, GATT_E_FAILED);
        }
        if (c->close_now && c->state >= C_MTU && c->state != C_CLOSING) {
            c->close_now = 0; c->state = C_CLOSING; now(&c->t);
            put16(p, c->handle); p[2] = HCI_E_USER_ENDED;
            if (cmd(OP_DISCONNECT, p, 3) < 0) return -1;
        }
        if (c->state == C_CLOSING && ms_since(&c->t) > 5000) {    /* never confirmed: forget it */
            if (c->srv) { srv_gone(c); continue; }
            uint64_t a = c->addr; free_conn(c);
            if (H->connection) H->connection(a, 0, 0, 0);
        }
    }
    return 0;
}

static int connect_next(void)                               /* scanning is off when this runs */
{
    struct conn *c = in_state(C_WAIT);
    if (!c || in_state(C_CONNECTING)) return 0;
    unsigned char p[25] = { 0x60, 0, 0x60, 0 };             /* scan 60 ms / 60 ms while connecting */
    p[4] = 0; p[5] = c->addr_type;                          /* no filter list; peer address type */
    for (int b = 0; b < 6; b++) p[6 + b] = c->addr >> (8 * b);
    p[12] = 0;                                              /* own address public */
    put16(p + 13, 24); put16(p + 15, 40);                   /* connection interval 30..50 ms */
    put16(p + 17, 0); put16(p + 19, 500);                   /* latency 0, supervision timeout 5 s */
    put16(p + 21, 0); put16(p + 23, 0);
    c->state = C_CONNECTING; now(&c->t);
    int st = cmd(OP_LE_CONNECT, p, 25);
    if (st < 0) return -1;
    if (st) {
        fprintf(stderr, "bluetooth: connect to %012llx refused (0x%02x)\n", (unsigned long long)c->addr, st);
        uint64_t a = c->addr; free_conn(c);
        if (H->connection) H->connection(a, 0, 0, st);
    }
    return 0;
}

static void drop_all(void)                                  /* controller lost: every connection with it */
{
    if (conns[SRV].state != C_FREE) srv_gone(&conns[SRV]);
    advertising = 0; adv_applied = -1;                      /* a new controller knows nothing of it */
    for (int i = 0; i < BLE_MAX_CONN; i++) if (conns[i].state != C_FREE) {
        uint64_t a = conns[i].addr; free_conn(&conns[i]);
        if (H->connection) H->connection(a, 0, 0, HCI_E_TIMEOUT);
    }
    for (struct frag *f = fhead, *n; f; f = n) { n = f->next; free(f); }
    fhead = NULL; ftail = &fhead;
    a2dp_lost();
}

static void *thread(void *arg)
{
    (void)arg;
    for (int said = 0;; sleep(5)) {
        if (stock_bt_running()) { if (!said++) fprintf(stderr, "bluetooth: waiting for %s to stop\n", board.bt_service); continue; }
        if ((fd = open(bt_dev(), O_RDWR | O_NOCTTY | O_CLOEXEC)) < 0) { if (!said++) fprintf(stderr, "bluetooth: %s: %s\n", bt_dev(), strerror(errno)); continue; }
        /* Left unread by the previous user.  A hassmic that was killed while scanning leaves the chip scanning, so this
         * does not run dry: stop after a moment, the reset stops the flow and pump() finds the packet boundaries again. */
        struct timespec t0; now(&t0);
        for (struct pollfd p = { fd, POLLIN, 0 }; ms_since(&t0) < 300 && poll(&p, 1, 100) > 0 && read(fd, in, sizeof in) > 0; ) ;
        have = 0; nbatch = 0;
        if (setup() < 0) { fprintf(stderr, "bluetooth: controller does not answer\n"); close(fd); fd = -1; continue; }
        said = 0;
        int on = 0, active = 0, was_quiet = 0;
        for (;;) {
            struct req *r;
            pthread_mutex_lock(&qlock); r = qhead; qhead = NULL; qtail = &qhead; pthread_mutex_unlock(&qlock);
            int bad = 0;
            while (r) { struct req *n = r->next; if (!bad && handle_request(r) < 0) bad = 1; free(r); r = n; }
            if (bad || upkeep() < 0) break;
            /* scanning pauses while a connection is being set up, and while a wake word round listens (ble_quiet): that pause
             * is neither logged nor reported, it is over within a second */
            long long qleft = atomic_load(&quiet_until) - mono_ms();
            int quiet = qleft > 0, brief = quiet || was_quiet;
            int won = atomic_load(&want_on) && !quiet && !in_state(C_WAIT) && !in_state(C_CONNECTING) && !a2dp_streaming(), wac = atomic_load(&want_active);
            was_quiet = quiet;
            if (won != on || (on && wac != active)) {
                if (apply(won, wac) < 0) break;
                if ((won != on || wac != active) && !brief)
                    fprintf(stderr, "bluetooth: %s\n", atomic_load(&scanning) ? (wac ? "scanning (active)" : "scanning (passive)") : "scan off");
                on = won; active = wac;
                if (H->scan_changed && !brief) H->scan_changed();
            }
            if (!on && connect_next() < 0) break;
            int busy = a2dp_busy();
            for (int i = 0; i < NCONN; i++) busy |= conns[i].state != C_FREE;
            busy |= adv_failed.tv_sec != 0;                 /* advertising refused: asked again in a while */
            struct pollfd p[2] = { { fd, POLLIN, 0 }, { wake[0], POLLIN, 0 } };
            int wait = busy ? 500 : nbatch ? 100 : -1;
            if (quiet && (wait < 0 || qleft < wait)) wait = (int)qleft + 1;          /* back to scanning when the pause ends */
            if (poll(p, 2, wait) < 0 && errno != EINTR) break;
            if (p[1].revents & POLLIN) { char c[16]; if (read(wake[0], c, sizeof c) < 0) break; }
            if (pump(0) < 0) break;
        }
        fprintf(stderr, "bluetooth: controller lost, reopening\n");
        drop_all();
        close(fd); fd = -1;
        if (atomic_exchange(&scanning, 0) && H->scan_changed) H->scan_changed();
    }
    return NULL;
}

/* ---------------------------------------------------------------- API */

int ble_present(void) { return access(bt_dev(), F_OK) == 0; }
const char *ble_mac(void) { return bdaddr; }
int ble_scanning(void) { return atomic_load(&scanning); }

int ble_connections(uint64_t *addrs)
{
    int n = 0;
    pthread_mutex_lock(&qlock);
    for (int i = 0; i < BLE_MAX_CONN; i++) if (slot_addr[i]) addrs[n++] = slot_addr[i];
    pthread_mutex_unlock(&qlock);
    return n;
}

void ble_scan(int on, int active) { atomic_store(&want_active, active != 0); atomic_store(&want_on, on != 0); poke(); }
void ble_quiet(int ms)
{
    long long until = mono_ms() + ms;
    if (until > atomic_load(&quiet_until)) atomic_store(&quiet_until, until);
    poke();
}
void ble_connect(uint64_t addr, unsigned addr_type) { request(R_CONNECT, addr, addr_type, 0, NULL, 0, 0); }
void ble_disconnect(uint64_t addr) { request(R_DISCONNECT, addr, 0, 0, NULL, 0, 0); }
void ble_services(uint64_t addr) { request(R_SERVICES, addr, 0, 0, NULL, 0, 0); }
void ble_read(uint64_t addr, unsigned handle) { request(R_READ, addr, 0, handle, NULL, 0, 0); }
void ble_pair(uint64_t addr) { request(R_PAIR, addr, 0, 0, NULL, 0, 0); }
void ble_unpair(uint64_t addr) { request(R_UNPAIR, addr, 0, 0, NULL, 0, 0); }
void ble_write(uint64_t addr, unsigned handle, const void *data, size_t len, int response)
{
    if (len > 512) len = 512;                               /* the most an attribute holds */
    request(R_WRITE, addr, 0, handle, data, len, response);
}

void ble_serve(const struct ble_server *s)
{
    if (S || !s || s->nchr > BLE_SRV_CHR) return;
    attr_add(A_SVC, GAP_SVC); attr_add(A_CHR, GAP_NAME); attr_add(A_VAL, GAP_NAME); attr_add(A_CHR, GAP_LOOK); attr_add(A_VAL, GAP_LOOK);
    svc_start = nattr + 1;
    attr_add(A_SVC, 0);
    for (int i = 0; i < s->nchr; i++) {
        attr_add(A_CHR, i); attr_add(A_VAL, i);
        if (s->chr[i].props & BLE_PROP_NOTIFY) attr_add(A_CCCD, i);
    }
    S = s;                                                  /* last: the controller thread looks at it */
}

void ble_advertise(const void *adv, size_t n, const void *rsp, size_t m)
{
    if (n > 31) n = 31;
    if (m > 31) m = 31;
    pthread_mutex_lock(&adv_lock);
    adv_n = n; if (n) memcpy(adv_data, adv, n);
    rsp_n = m; if (m) memcpy(rsp_data, rsp, m);
    pthread_mutex_unlock(&adv_lock);
    atomic_fetch_add(&adv_gen, 1);
    poke();
}

void ble_server_notify(int chr) { if (chr >= 0 && chr < BLE_SRV_CHR) { atomic_fetch_or(&notify_due, 1u << chr); poke(); } }
void ble_server_drop(void) { atomic_store(&drop_srv, 1); poke(); }

/* hci.h: for a2dp.c on this thread */
int hci_cmd(unsigned op, const void *par, unsigned n) { return cmd(op, par, n); }
const unsigned char *hci_ret(void) { return cc_ret; }
int hci_write(const void *b, size_t n) { return write(fd, b, n) == (ssize_t)n ? 0 : -1; }
void hci_poke(void) { poke(); }

void ble_start(const struct ble_handler *h)
{
    static int started; pthread_t t;
    H = h ? h : &no_handler;
    if (started || !ble_present() || pipe(wake)) return;
    started = 1;
    fcntl(wake[1], F_SETFL, O_NONBLOCK);
    read_bdaddr();
    for (int i = 0; i < 6 && bdaddr[0]; i++) own_addr[5 - i] = strtoul(bdaddr + 3 * i, NULL, 16);
    bonds_load();
    pthread_create(&t, NULL, thread, NULL); pthread_detach(t);
}
