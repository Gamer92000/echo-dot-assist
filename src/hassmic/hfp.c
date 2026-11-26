/*
 * Phone calls over Bluetooth: the hands-free unit (HFP 1.7, HF role) on a2dp.c's BR/EDR links (hfp.h; docs/re-hfp.md).
 *
 *   SDP       a2dp.c lists a Hands-Free record (RFCOMM channel HFP_CHANNEL, features: EC/NR, CLI) while the feature
 *             is on; a phone paired before it was on keeps what it read then: pair it again.
 *   RFCOMM    the phone opens it (an audio gateway connects to a hands-free unit; we never page): the multiplexer on
 *             DLCI 0 and our channel's DLC, credit based flow control (PN), modem status (MSC).  We are the responder:
 *             our commands carry C/R 0, our responses C/R 1 (TS 07.10 5.2.1.2, as Linux's rfcomm core).
 *   AT        the service level connection: AT+BRSF, AT+BAC=1,2 (where both negotiate codecs and this model codes
 *             the link itself: sco_wideband()), AT+CIND=?, AT+CIND?, AT+CMER=3,0,0,1, AT+CHLD=? (both with three-way
 *             calling), then AT+CLIP=1 (who calls), AT+CCWA=1 (who waits), AT+CMEE=1, and AT+NREC=0 where the phone
 *             has echo cancelling of its own (ours runs: call.c, micHfp).  One command waits for its OK at a time.
 *   codec     the phone picks (+BCS, answered AT+BCS) before it opens the audio link: mSBC (wideband, 16 kHz) or CVSD;
 *             what the link carries is its air mode (Synchronous Connection Complete: transparent = mSBC).  Taking a
 *             call here asks the phone for the link (AT+BCC) where codecs are negotiated, else sets it up (CVSD).
 *   volume    remote volume control: the phone's +VGS (0..15) is the call's volume while its audio is here
 *             (core_call_volume: the volume, or Call volume with separate volumes), and the Echo's call volume goes to
 *             the phone as AT+VGS at the start of the audio and whenever it moves; a +VGS outside a call is not taken
 *             (it would move the music's volume): ours goes at the next call.  +VGM (the mic's gain) is left alone:
 *             the call's own gain sets the level (call.c).
 *   two calls a call waiting (+CCWA, callsetup 1 beside a call) or one on hold (callheld): "the other call", its
 *             caller from +CCWA, the indicators' moves, and +CLCC where the phone has enhanced call status (the
 *             indicators alone cannot say which call a swap made the active one).  AT+CHLD=2 takes it (holding this
 *             one: the action button, Home Assistant's "Switch phone call"); ending a call beside a held one is
 *             AT+CHLD=1 (the held one back), else AT+CHUP.
 *   the call  from the phone's indicators (call, callsetup) and whether the synchronous link is up: ringing here (the
 *             phone rings too), answered here (ATA; the phone then opens the link), talking (the link up: call.c's
 *             media, CALL_CODEC_PCM, as a plugin's call: the mic through the call's gate to scoaudio.c, what comes
 *             back into its jitter buffer and the Voip stream), on the phone (the call talks on the handset: nothing
 *             shown here; "take the call here" opens the link).  call.c's claim: one call on the Echo at a time,
 *             Drop In's or ours; a ring that finds it taken is not shown, a link asked for then is refused.
 *
 * Everything but the API at the end runs on the controller thread (ble.c's); the API only leaves requests and pokes it.
 */
#include "hfp.h"
#include "call.h"
#include "core.h"
#include "hci.h"
#include "logread.h"
#include "plugin_store.h"
#include "scoaudio.h"
#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define N1        127                   /* our largest RFCOMM information field */
#define CREDITS   7                     /* what we grant at a time */
#define AT_MS     8000                  /* a command's answer; none: the DLC is closed (a late one would answer the next) */
#define ANSWER_MS 6000                  /* answered here: the phone opens the audio link within that */
#define DIAL_MS   10000                 /* dialled here: the phone shows the call within that */
/* AT+BRSF: EC/NR (ours), three-way calling, calling line identification, remote volume, enhanced call status; codec
 * negotiation where this model codes the link (mSBC) */
static unsigned hf_features(void) { return 0x37 | (sco_wideband() ? 0x80 : 0); }

const char *const hfp_states[HFP_NSTATES] = { "no phone", "idle", "ringing", "calling", "connected", "on the phone" };

static long long ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

/* ---------------------------------------------------------------- state */

static atomic_int enabled;
static atomic_int req_answer, req_hangup, req_here, req_dial, req_switch;    /* from other threads, for upkeep */
static pthread_mutex_t smu = PTHREAD_MUTEX_INITIALIZER;             /* the snapshot below and the number to dial */
static struct { int state, second; char caller[64], phone[80], other[64]; } shown_st = { HFP_NONE, 0, "", "", "" };
static char dial_number[40];

static struct ag {
    int on, link; uint64_t addr; unsigned cod;                 /* cod: its class of device, 0 unknown */
    int mux, dlci, dlc, cfc, tx_cred, rx_cred, n1;
    char rx[512]; size_t rxn;
    char q[8][64]; int qh, qn, waiting; long long sent_at; char sent[64];
    int step, slc; unsigned feat;
    char ind[20][16]; int nind, i_call, i_setup, i_held;
    int call, setup, held; long long dialed; int dial_seen;    /* dialed: when we asked the phone to call, 0 none */
    char caller[64];
    char other[64];                                             /* the held call's caller (shown while ag.held) */
    char waiter[64]; int has_waiter;                            /* a call waiting beside this one: its caller */
    int chld, chld_sent;                                        /* +CHLD's: 1 "0", 2 "1", 4 "2"; the last we sent */
    int clcc_due, clcc_on, clcc_act, clcc_held, clcc_wait;      /* +CLCC: asked again, collecting, what it listed */
    char clcc_a[64], clcc_h[64], clcc_w[64];
    int codec;                                                  /* the phone's choice (+BCS): SCO_CVSD until it says */
    int msbc_off, msbc_fails;                                   /* mSBC links failed in a row (2: CVSD only on this connection) */
    int sco_req;                                                /* the link asked for last: what it was answered as
                                                                   (SCO_CVSD, SCO_MSBC; 0 refused or none), until its end.
                                                                   One at a time: a phone asks for one link and waits */
    int answer_rearmed;                                         /* the answer's window once more for a failed link */
    long long waiter_gone;                                      /* when the waiting call's callsetup went 0 */
    int vgs;                                                    /* the speaker gain the phone was told, -1 none */
    unsigned sco; int air;                                      /* the audio link and what it carries (SCO_*) */
    long long answer_until;
    int claimed, shown, gen;
} ag;

enum { S_BRSF = 1, S_BAC, S_CIND_TEST, S_CIND_READ, S_CMER, S_CHLD, S_UP };

static atomic_int call_vol = -1;        /* the Echo's call volume (0..100) as main.c last set it */
static atomic_int can_switch;           /* the phone takes AT+CHLD=2 (for the action button, any thread) */

/* what both sides have, from the phone's +BRSF */
static int wide(void) { return sco_wideband() && (ag.feat & 0x200) && !ag.msbc_off; }   /* codec negotiation (AG bit 9) */
static int three_way(void) { return ag.feat & 0x01; }
static int ecs(void) { return ag.feat & 0x40; }                             /* enhanced call status: +CLCC */
/* a call there: active or held (the call indicator says so for both; a phone that drops it to 0 with one held
 * still has that one) */
static int in_call(void) { return ag.call || ag.held; }

/* ---------------------------------------------------------------- RFCOMM frames */

static unsigned char crc_t[256];
static void crc_init(void)
{
    for (int i = 0; i < 256; i++) { unsigned v = i; for (int k = 0; k < 8; k++) v = v & 1 ? (v >> 1) ^ 0xe0 : v >> 1; crc_t[i] = v; }
}
static unsigned crc8(const unsigned char *p, size_t n) { unsigned f = 0xff; while (n--) f = crc_t[f ^ *p++]; return f; }

enum { SABM = 0x2f, UA = 0x63, DM = 0x0f, DISC = 0x43, UIH = 0xef, PF = 0x10 };

/* a frame on DLCI of LINK: CR the address's C/R bit, credits >= 0 rides along (UIH with P/F) */
static void rf_send_on(int link, int dlci, int cr, unsigned ctrl, const void *info, size_t n, int credits)
{
    unsigned char f[8 + N1]; size_t h;
    if (n > N1) n = N1;
    f[0] = dlci << 2 | cr << 1 | 1; f[1] = ctrl | (credits >= 0 ? PF : 0);
    if (n < 128) { f[2] = n << 1 | 1; h = 3; } else { f[2] = (n & 0x7f) << 1; f[3] = n >> 7; h = 4; }
    size_t fh = h;                                                  /* the FCS covers address, control (, length) */
    if (credits >= 0) f[h++] = credits;
    if (n) memcpy(f + h, info, n);
    f[h + n] = 0xff - crc8(f, (ctrl & ~PF) == UIH ? 2 : fh);
    a2dp_rfcomm_send(link, f, h + n + 1);
}
static void rf_send(int dlci, int cr, unsigned ctrl, const void *info, size_t n, int credits) { rf_send_on(ag.link, dlci, cr, ctrl, info, n, credits); }

static void mcc_send(unsigned type, int command, const unsigned char *v, size_t n)    /* a multiplexer command on DLCI 0 */
{
    unsigned char b[16] = { type << 2 | (command ? 2 : 0) | 1, n << 1 | 1 };
    memcpy(b + 2, v, n);
    rf_send(0, 0, UIH, b, 2 + n, -1);
}

static void msc_send(void) { unsigned char v[2] = { ag.dlci << 2 | 3, 0x8d }; mcc_send(0x38, 1, v, 2); }   /* RTC, RTR, DV */

/* ---------------------------------------------------------------- AT, our side */

/* 0 queued, -1 the queue full and the command dropped: what it stands for is set only once it is queued */
static int at_queue(const char *cmd)
{
    if (ag.qn == (int)(sizeof ag.q / sizeof *ag.q)) { fprintf(stderr, "phone: command dropped: %s\n", cmd); return -1; }
    snprintf(ag.q[(ag.qh + ag.qn) % 8], sizeof ag.q[0], "%s", cmd); ag.qn++;
    return 0;
}

static void at_pump(void)
{
    if (!ag.dlc || ag.waiting || !ag.qn || (ag.cfc && ag.tx_cred <= 0)) return;
    snprintf(ag.sent, sizeof ag.sent, "%s", ag.q[ag.qh]); ag.qh = (ag.qh + 1) % 8; ag.qn--;
    char line[72]; size_t n = (size_t)snprintf(line, sizeof line, "%s\r", ag.sent);
    rf_send(ag.dlci, 0, UIH, line, n, -1);
    if (ag.cfc) ag.tx_cred--;
    ag.waiting = 1; ag.sent_at = ms();
    if (!strncmp(ag.sent, "ATD", 3)) log_private("phone: > %s", ag.sent);
    else fprintf(stderr, "phone: > %s\n", ag.sent);
}

static void slc_next(void);
static void apply(void);
static void clcc_end(int ok);

/* the command that waited got its answer */
static void at_done(int ok)
{
    if (!ag.waiting) return;
    ag.waiting = 0;
    if (!ok) fprintf(stderr, "phone: %s refused\n", !strncmp(ag.sent, "ATD", 3) ? "the call" : ag.sent);
    if (!ok && !strncmp(ag.sent, "ATD", 3)) { ag.dialed = 0; ag.dial_seen = 0; apply(); }
    if (!ok && !strcmp(ag.sent, "ATA")) { ag.answer_until = 0; apply(); }
    if (!ok && !strncmp(ag.sent, "AT+BAC", 6)) ag.feat &= ~0x200u;          /* no codecs then: CVSD */
    if (!ok && !strcmp(ag.sent, "AT+CHLD=?")) ag.chld = 0;
    if (!strcmp(ag.sent, "AT+CLCC")) clcc_end(ok);
    if (!ok && !strncmp(ag.sent, "AT+CHLD=", 8)) ag.chld_sent = 0;
    if (!ag.slc) slc_next();
}

static void slc_next(void)
{
    char b[40];
    switch (ag.step) {
    case 0: ag.step = S_BRSF; snprintf(b, sizeof b, "AT+BRSF=%u", hf_features()); at_queue(b); break;
    case S_BRSF:
        if (wide()) { ag.step = S_BAC; at_queue("AT+BAC=1,2"); break; }
        /* fall through */
    case S_BAC: ag.step = S_CIND_TEST; at_queue("AT+CIND=?"); break;
    case S_CIND_TEST: ag.step = S_CIND_READ; at_queue("AT+CIND?"); break;
    case S_CIND_READ: ag.step = S_CMER; at_queue("AT+CMER=3,0,0,1"); break;
    case S_CMER:
        if (three_way()) { ag.step = S_CHLD; at_queue("AT+CHLD=?"); break; }
        /* fall through */
    case S_CHLD:
        ag.step = S_UP; ag.slc = 1;
        at_queue("AT+CLIP=1");
        if (three_way() && ag.chld) at_queue("AT+CCWA=1");
        at_queue("AT+CMEE=1");
        if (ag.feat & 0x02) at_queue("AT+NREC=0");                 /* the phone's EC/NR off: the Echo has its own */
        log_private("phone: %s connected for calls (features 0x%x%s%s)", a2dp_link_name(ag.link), ag.feat, wide() ? ", mSBC" : "",
                    ag.chld ? ", two calls" : "");
        apply();
        break;
    }
}

/* "+CIND: ("service",(0,1)),("call",(0,1)),...": the indicators' order */
static void cind_names(const char *p)
{
    ag.nind = 0; ag.i_call = ag.i_setup = ag.i_held = -1;
    while ((p = strchr(p, '"')) && ag.nind < 20) {
        const char *e = strchr(++p, '"');
        if (!e) break;
        size_t n = (size_t)(e - p) < sizeof ag.ind[0] - 1 ? (size_t)(e - p) : sizeof ag.ind[0] - 1;
        memcpy(ag.ind[ag.nind], p, n); ag.ind[ag.nind][n] = 0;
        if (!strcmp(ag.ind[ag.nind], "call")) ag.i_call = ag.nind;
        else if (!strcmp(ag.ind[ag.nind], "callsetup") || !strcmp(ag.ind[ag.nind], "call_setup")) ag.i_setup = ag.nind;
        else if (!strcmp(ag.ind[ag.nind], "callheld")) ag.i_held = ag.nind;
        ag.nind++; p = e + 1;
        const char *close = strchr(p, ')');                         /* past the range "(0,1)" */
        if (close) p = close + 1;
    }
}

static void ind_set(int i, int v)
{
    if (i == ag.i_call) ag.call = v;
    else if (i == ag.i_setup) ag.setup = v;
    else if (i == ag.i_held) ag.held = v;
}

/* "+CLIP: "<number>",<type>[,<subaddr>,<satype>,"<name>"[,<validity>]]" and "+CCWA: "<number>",<type>,<class>[,"<name>"]"
 * (the name after the third comma both times): the name where the phone gives one, into OUT */
static void who(const char *p, char *out, size_t cap)
{
    char num[64] = "", name[64] = ""; const char *q;
    if ((q = strchr(p, '"'))) { const char *e = strchr(++q, '"'); if (e) { size_t n = (size_t)(e - q) < 63 ? (size_t)(e - q) : 63; memcpy(num, q, n); num[n] = 0; p = e + 1; } }
    int commas = 0;
    for (q = p; *q && commas < 3; q++) if (*q == ',') commas++;
    if (commas == 3 && (q = strchr(q, '"'))) { const char *e = strchr(++q, '"'); if (e) { size_t n = (size_t)(e - q) < 63 ? (size_t)(e - q) : 63; memcpy(name, q, n); name[n] = 0; } }
    snprintf(out, cap, "%s", name[0] ? name : num);
}

/* "+CLCC: <idx>,<dir>,<stat>,<mode>,<mpty>[,"<number>",<type>[,"<name>"]]", one line a call: the active (or dialled,
 * alerting) one is the caller shown, a held one the held call, an incoming or waiting one the call that waits */
static void clcc_line(const char *p)
{
    int idx, dir, stat; char q[2][64] = { "", "" }; int k = 0; const char *s = p;
    if (!ag.clcc_on || sscanf(p, " %d,%d,%d", &idx, &dir, &stat) != 3) return;
    while (k < 2 && (s = strchr(s, '"'))) {
        const char *e = strchr(++s, '"');
        if (!e) break;
        size_t n = (size_t)(e - s) < 63 ? (size_t)(e - s) : 63; memcpy(q[k], s, n); q[k][n] = 0; k++; s = e + 1;
    }
    const char *name = q[1][0] ? q[1] : q[0];
    if (stat == 0 || stat == 2 || stat == 3) { snprintf(ag.clcc_a, sizeof ag.clcc_a, "%s", name); ag.clcc_act = 1; }
    else if (stat == 1) { snprintf(ag.clcc_h, sizeof ag.clcc_h, "%s", name); ag.clcc_held = 1; }
    else { snprintf(ag.clcc_w, sizeof ag.clcc_w, "%s", name); ag.clcc_wait = 1; }
}

static void clcc_end(int ok)
{
    if (!ag.clcc_on) return;
    ag.clcc_on = 0;
    if (!ok) return;
    if (ag.clcc_act) {
        snprintf(ag.caller, sizeof ag.caller, "%s", ag.clcc_a);
        snprintf(ag.other, sizeof ag.other, "%s", ag.clcc_held ? ag.clcc_h : "");
    } else if (ag.clcc_held) { snprintf(ag.caller, sizeof ag.caller, "%s", ag.clcc_h); ag.other[0] = 0; }   /* every call held */
    if (in_call()) { ag.has_waiter = ag.clcc_wait; snprintf(ag.waiter, sizeof ag.waiter, "%s", ag.clcc_wait ? ag.clcc_w : ""); }
    apply();
}

/* the calls' list asked again (two calls: the indicators do not say which is which) where the phone has it */
static void clcc_ask(void) { if (ecs() && ag.slc) ag.clcc_due = 1; }

static void swap_calls(void)                                    /* the active and the held one change places */
{
    char t[sizeof ag.caller]; memcpy(t, ag.caller, sizeof t);
    memcpy(ag.caller, ag.other, sizeof ag.caller); memcpy(ag.other, t, sizeof ag.other);
}

/* an indicator moved: the second call as far as the indicators tell (+CLCC, where there is one, says it exactly).
 * callheld 1 means two calls, one active and one held: from 0 or 2 the waiting (or a new) call taken, the one shown
 * held; from 1 again a swap.  2: every call held; from 1, the active one ended.  A waiting call's name stays in
 * ag.waiter after its callsetup 0, which some phones send before callheld 1 */
static void ind_moved(int i, int v)
{
    int setup = ag.setup, held = ag.held;
    ind_set(i, v);
    if (i == ag.i_held) {
        if (v == 1 && held == 1) swap_calls();
        else if (v == 1) {                                          /* (the waiter's name only while it waits or just went) */
            memcpy(ag.other, ag.caller, sizeof ag.other);
            if (ag.has_waiter || ms() - ag.waiter_gone < 3000) memcpy(ag.caller, ag.waiter, sizeof ag.caller); else ag.caller[0] = 0;
            ag.has_waiter = 0;
        }
        else if (v == 2 && held == 1) memcpy(ag.caller, ag.other, sizeof ag.caller);
        else if (v == 0 && held == 1 && ag.chld_sent == 1) memcpy(ag.caller, ag.other, sizeof ag.caller);    /* we ended this: the held one back */
        ag.chld_sent = 0; clcc_ask();
    } else if (i == ag.i_setup && setup == 1 && v == 0 && ag.has_waiter) { ag.has_waiter = 0; ag.waiter_gone = ms(); clcc_ask(); }   /* gone or taken */
    else if (i == ag.i_setup && v == 1 && in_call()) { ag.has_waiter = 1; clcc_ask(); }   /* a call waits (+CCWA names it) */
    if (!in_call() && !ag.setup) { ag.has_waiter = 0; ag.waiter[0] = 0; ag.other[0] = 0; }
}

/* the phone's speaker gain (+VGS): the call's volume while its audio is here */
static void vgs_from_phone(int g)
{
    if (g < 0 || g > 15) return;
    if (!ag.gen) { fprintf(stderr, "phone: its volume (%d of 15) not taken outside a call\n", g); return; }
    int v = core_call_volume((g * 100 + 7) / 15);
    /* what it became told as what the phone has: a linked Call volume that clamps lands elsewhere, and telling the
     * phone that at once moved its own slider back under its user's finger */
    ag.vgs = (v * 15 + 50) / 100;
    fprintf(stderr, "phone: volume %d of 15 from the phone: %d\n", g, v);
}

static void at_line(const char *l)
{
    if (!strncmp(l, "+CLIP", 5) || !strncmp(l, "+CCWA", 5) || !strncmp(l, "+CLCC", 5)) log_private("phone: < %s", l);
    else fprintf(stderr, "phone: < %s\n", l);
    if (!strcmp(l, "OK")) { at_done(1); return; }
    if (!strcmp(l, "ERROR") || !strncmp(l, "+CME ERROR", 10) || !strcmp(l, "NO CARRIER") || !strcmp(l, "BUSY")) { at_done(0); return; }
    if (!strncmp(l, "+BRSF:", 6)) { ag.feat = strtoul(l + 6, NULL, 10); return; }
    if (!strncmp(l, "+CIND:", 6)) {
        if (ag.step == S_CIND_TEST) { cind_names(l + 6); return; }
        const char *p = l + 6;
        for (int i = 0; i < ag.nind && *p; i++) { ind_set(i, atoi(p)); p = strchr(p, ','); if (!p) break; p++; }
        apply();
        return;
    }
    if (!strncmp(l, "+CIEV:", 6)) {
        int i = atoi(l + 6) - 1; const char *c = strchr(l, ',');
        if (c && i >= 0) { ind_moved(i, atoi(c + 1)); if (!ag.setup && !in_call()) ag.caller[0] = 0; apply(); }
        return;
    }
    if (!strncmp(l, "+CLIP:", 6)) { who(l + 6, ag.caller, sizeof ag.caller); apply(); return; }
    if (!strncmp(l, "+CCWA:", 6)) { who(l + 6, ag.waiter, sizeof ag.waiter); ag.has_waiter = 1; apply(); return; }
    if (!strncmp(l, "+CLCC:", 6)) { clcc_line(l + 6); return; }
    if (!strcmp(l, "RING")) { if (!ag.setup && !in_call()) ag.setup = 1; apply(); return; }   /* an AG without indicators */
    if (!strncmp(l, "+CHLD:", 6)) {                                 /* "(0,1,1x,2,2x,3,4)": the ones we use */
        ag.chld = 0;
        for (const char *p = l + 6; *p; p++)
            if (*p >= '0' && *p <= '2' && !isalnum((unsigned char)p[1]) && (p == l + 6 || !isalnum((unsigned char)p[-1]))) ag.chld |= 1 << (*p - '0');
        return;
    }
    if (!strncmp(l, "+BCS:", 5)) {                                  /* the phone's codec for the next audio link */
        int c = atoi(l + 5); char b[16];
        if (c == SCO_CVSD || (c == SCO_MSBC && wide())) {
            ag.codec = c; snprintf(b, sizeof b, "AT+BCS=%d", c); at_queue(b);
            fprintf(stderr, "phone: codec %s\n", c == SCO_MSBC ? "mSBC" : "CVSD");
        } else { fprintf(stderr, "phone: asks for codec %d, not ours\n", c); at_queue(wide() ? "AT+BAC=1,2" : "AT+BAC=1"); }
        return;
    }
    if (!strncmp(l, "+VGS", 4) && (l[4] == ':' || l[4] == '=')) { vgs_from_phone(atoi(l + 5)); return; }
}

static void at_rx(const unsigned char *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char c = (char)p[i];
        if (c == '\r' || c == '\n') {
            ag.rx[ag.rxn] = 0;
            if (ag.rxn) at_line(ag.rx);
            ag.rxn = 0;
        } else if (ag.rxn < sizeof ag.rx - 1 && (unsigned char)c >= 0x20) ag.rx[ag.rxn++] = c;
    }
}

/* ---------------------------------------------------------------- RFCOMM, the phone's frames */

static void dlc_close(void)
{
    ag.dlc = 0; ag.dlci = 0; ag.slc = 0; ag.step = 0; ag.waiting = 0; ag.qn = 0; ag.rxn = 0;
    ag.call = ag.setup = ag.held = 0; ag.dialed = 0; ag.dial_seen = 0; ag.caller[0] = 0; ag.answer_until = 0;
    ag.has_waiter = 0; ag.waiter[0] = ag.other[0] = 0; ag.chld = ag.chld_sent = 0; ag.clcc_due = ag.clcc_on = 0; ag.codec = SCO_CVSD;
    apply();
}

static void mcc_rx(const unsigned char *p, size_t n)
{
    while (n >= 2) {
        unsigned type = p[0] >> 2, cmd = p[0] >> 1 & 1; size_t len = p[1] >> 1;
        if (!(p[1] & 1) || len > n - 2) return;                    /* (lengths over 127: none we know) */
        const unsigned char *v = p + 2;
        if (cmd) switch (type) {
        case 0x20:                                                  /* PN: our channel's parameters */
            if (len >= 8) {
                unsigned char r[8]; memcpy(r, v, 8);
                unsigned n1 = v[4] | v[5] << 8;
                ag.cfc = (v[1] & 0xf0) == 0xf0;
                r[1] = ag.cfc ? 0xe0 : 0x00; r[3] = 0; if (n1 > N1 || !n1) n1 = N1;
                r[4] = n1 & 0xff; r[5] = n1 >> 8; r[6] = 0; r[7] = ag.cfc ? CREDITS : 0;
                ag.n1 = n1; if (ag.cfc) { ag.tx_cred = v[7] & 7; ag.rx_cred = CREDITS; }
                mcc_send(0x20, 0, r, 8);
            }
            break;
        case 0x38:                                                  /* MSC: answer it as it came */
            mcc_send(0x38, 0, v, len < 3 ? len : 3);
            break;
        case 0x24: {                                                /* RPN: what it asks, or 9600 8N1 */
            unsigned char r[8] = { len ? v[0] : 0, 0x07, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00 };
            if (len >= 8) memcpy(r, v, 8);
            mcc_send(0x24, 0, r, 8);
            break; }
        case 0x14: mcc_send(0x14, 0, v, len < 2 ? len : 2); break;  /* RLS */
        case 0x08: mcc_send(0x08, 0, v, len < 12 ? len : 12); break; /* Test */
        case 0x28: case 0x18: mcc_send(type, 0, NULL, 0); break;   /* FCon, FCoff */
        default: { unsigned char r = p[0]; mcc_send(0x04, 0, &r, 1); }   /* NSC */
        }
        p += 2 + len; n -= 2 + len;
    }
}

/* The major device class "Phone" (Bluetooth Assigned Numbers, class of device bits 8-12) */
static int is_phone(unsigned cod) { return (cod >> 8 & 0x1f) == 0x02; }

/* gateways refused while another had the place, their RFCOMM channel still open: the first phone among them (else the
 * first) gets the place when it is free again, so its next SABM is answered */
static struct { int on, link, sabm; long long sabm_at; uint64_t addr; unsigned cod; } waitq[4];   /* sabm: its SABM on DLCI 0
                                                                                                     unanswered, since sabm_at */
#define SABM_T1_MS 60000                /* an unanswered SABM is the sender's for this long: T1 is 20 s in Fluoride, longer
                                           in BlueZ; a UA after the sender gave up is ignored, so the longest is kept */
static int wait_sabm(int i) { return waitq[i].sabm && ms() - waitq[i].sabm_at < SABM_T1_MS; }
/* one that may take the place: its SABM still answerable, or none sent yet (it will come).  One whose SABM ran out of
 * time gave up on this channel: dropped, it would hold the place with no session */
static int wait_live(int i)
{
    if (!waitq[i].on) return 0;
    if (waitq[i].sabm && !wait_sabm(i)) { waitq[i].on = 0; return 0; }
    return 1;
}
static void wait_drop(int link) { for (int i = 0; i < 4; i++) if (waitq[i].on && waitq[i].link == link) waitq[i].on = 0; }
static void wait_add(int link, uint64_t addr, unsigned cod)        /* a phone or of no known class (no other waits) */
{
    wait_drop(link);
    int at = -1;
    for (int i = 0; i < 4 && at < 0; i++) if (!waitq[i].on) at = i;
    for (int i = 0; i < 4 && at < 0; i++) if (is_phone(cod) && !is_phone(waitq[i].cod)) at = i;  /* full: a phone first */
    if (at < 0) { fprintf(stderr, "phone: %012llx not kept waiting: four wait already\n", (unsigned long long)addr); return; }
    if (waitq[at].on) {                         /* told at once: its SABM is not to wait for a UA (sent only once) */
        fprintf(stderr, "phone: %012llx no longer waits: a phone does\n", (unsigned long long)waitq[at].addr);
        if (waitq[at].sabm) rf_send_on(waitq[at].link, 0, 1, DM | PF, NULL, 0, -1);
    }
    waitq[at].on = 1; waitq[at].link = link; waitq[at].addr = addr; waitq[at].cod = cod; waitq[at].sabm = 0;
}


void hfp_rfcomm_rx(int link, const unsigned char *p, size_t n)
{
    if (n < 4 || !ag.on) return;
    int other = link != ag.link;                        /* another gateway's frame: only its SABM and DISC count */
    if (other && (p[1] & ~PF) != SABM && (p[1] & ~PF) != DISC) return;     /* (its UIH: credits of its own, not ours) */
    unsigned dlci = p[0] >> 2, ctrl = p[1] & ~PF, pf = p[1] & PF; size_t len, h;
    if (p[2] & 1) { len = p[2] >> 1; h = 3; } else { if (n < 5) return; len = p[2] >> 1 | p[3] << 7; h = 4; }
    int credits = ctrl == UIH && pf && ag.cfc && dlci ? p[h] : -1;
    size_t at = h + (credits >= 0);
    if (at + len + 1 > n) return;
    {   /* the FCS over what it covers and itself leaves 0xcf (TS 07.10 5.2.1.6) */
        size_t c = ctrl == UIH ? 2 : h; unsigned char t[5];
        memcpy(t, p, c); t[c] = p[at + len];
        if (crc8(t, c + 1) != 0xcf) { fprintf(stderr, "phone: RFCOMM frame with a bad FCS\n"); return; }
    }
    if (other) {
        /* a waiter's SABM on DLCI 0 kept (answered UA when it gets the place, within T1), its DISC forgets it; one that
         * is no phone (a TV box refused, or the one a phone took the place from) is answered DM, so it stops asking
         * rather than waiting out its timers (TS 07.10: DM, a disconnected mode) */
        unsigned lc = a2dp_link_cod(link);
        for (int i = 0; i < 4; i++) if (waitq[i].on && waitq[i].link == link && !dlci) {
            if (ctrl == SABM) { waitq[i].sabm = 1; waitq[i].sabm_at = ms(); }
            if (ctrl == DISC) waitq[i].on = 0;     /* it gave up on the mux: no longer waits */
        }
        /* TS 07.10: a DISC in disconnected mode is answered DM (a waiter's mux was never opened: its SABM was held) */
        if (ctrl == DISC) { rf_send_on(link, dlci, 1, DM | PF, NULL, 0, -1); return; }
        int waits = 0;
        for (int i = 0; i < 4; i++) waits |= waitq[i].on && waitq[i].link == link;
        /* no phone, or one not kept waiting (evicted by a phone, or four wait already): DM, it is not to wait for UA */
        if (ctrl == SABM && ((lc && !is_phone(lc)) || !waits)) rf_send_on(link, dlci, 1, DM | PF, NULL, 0, -1);
        return;
    }
    const unsigned char *info = p + at;
    switch (ctrl) {
    case SABM:
        if (dlci == 0) { ag.mux = 1; rf_send(0, 1, UA | PF, NULL, 0, -1); break; }
        if (!ag.mux || dlci >> 1 != HFP_CHANNEL || (ag.dlc && dlci != (unsigned)ag.dlci)) { rf_send(dlci, 1, DM | PF, NULL, 0, -1); break; }
        rf_send(dlci, 1, UA | PF, NULL, 0, -1);
        if (!ag.dlc) {
            ag.dlc = 1; ag.dlci = dlci; ag.step = 0;
            if (!ag.cfc) ag.tx_cred = 1 << 30;
            msc_send();
            slc_next();
        }
        break;
    case DISC:
        rf_send(dlci, 1, UA | PF, NULL, 0, -1);
        if (dlci == 0) { ag.mux = 0; if (ag.dlc) dlc_close(); }
        else if ((int)dlci == ag.dlci) dlc_close();
        break;
    case UA: case DM:
        if (ctrl == DM && (int)dlci == ag.dlci) dlc_close();
        break;
    case UIH:
        if (dlci == 0) { mcc_rx(info, len); break; }
        if ((int)dlci != ag.dlci || !ag.dlc) break;
        if (credits > 0) ag.tx_cred += credits;
        if (len) {
            at_rx(info, len);
            if (ag.cfc && --ag.rx_cred <= 2) { rf_send(ag.dlci, 0, UIH, NULL, 0, CREDITS - ag.rx_cred); ag.rx_cred = CREDITS; }
        }
        break;
    }
    at_pump();
}

static void ag_gone(void);
static int call_on(void);


/* One audio gateway at a time, and a phone before anything else that is one.  An Android TV box (a Mi Box, Android 14)
 * paired as a music source connects for calls too, at once and with no way to turn that off in its settings, and held
 * the place: the phone that connected after it got music only (issue #233).  So a phone (its class of device says so)
 * takes the place from a device that is no phone and has no call; another phone (or one of no known class) waits for
 * the place to be free (waitq), and one known to be no phone is turned away (DM) until it connects again.
 * An unknown class (a phone whose Connection Request a2dp.c did not pair with its link) takes it from a holder known to
 * be no phone, as a phone does, and loses it to nobody */
void hfp_rfcomm_up(int link, uint64_t addr, unsigned cod)
{
    if (!atomic_load(&enabled)) return;
    if (ag.on && ag.link != link) {
        /* (an unknown class: a phone whose request a2dp.c did not pair with its link; against a holder known to be no
         * phone it counts as one) */
        /* a phone (or one of no known class) takes it only from a holder known to be no phone: a holder of no known
         * class may be a phone with its service level connection up (a link whose request a2dp.c did not keep), and
         * is never put out for another; never with a call, its audio link or the claim */
        int takes = (is_phone(cod) || !cod) && ag.cod && !is_phone(ag.cod);
        if (!takes || call_on() || ag.sco || ag.claimed) {
            fprintf(stderr, "phone: %012llx (class 0x%06x) opened RFCOMM, but %012llx (class 0x%06x) has the place: one at a time\n",
                    (unsigned long long)addr, cod, (unsigned long long)ag.addr, ag.cod);
            if (!cod || is_phone(cod)) wait_add(link, addr, cod);     /* (one that is no phone is answered DM) */
            return;
        }
        fprintf(stderr, "phone: %012llx is a phone or of no known class (0x%06x): it takes the place of %012llx (class 0x%06x, no phone, no call)\n",
                (unsigned long long)addr, cod, (unsigned long long)ag.addr, ag.cod);
        /* the other's DLC and multiplexer closed (DISC: a command, ours C/R 0); its frames after this are on a link
         * that is not ag.link and go unread, its SABMs answered DM (no phone).  Not put in waitq: such a device
         * connects for calls again by itself (the Mi Box at every connection), and the phone is what was wanted */
        if (ag.dlc) rf_send(ag.dlci, 0, DISC | PF, NULL, 0, -1);
        if (ag.mux) rf_send(0, 0, DISC | PF, NULL, 0, -1);
        ag_gone();
    }
    if (!crc_t[1]) crc_init();
    if (!ag.on) { memset(&ag, 0, sizeof ag); ag.on = 1; ag.link = link; ag.addr = addr; ag.cod = cod; ag.n1 = N1; ag.codec = SCO_CVSD; ag.vgs = -1; }
    fprintf(stderr, "phone: %012llx opened RFCOMM (class 0x%06x)\n", (unsigned long long)addr, cod);
}

static void ag_gone(void)
{
    if (!ag.on) return;
    ag.sco = 0;
    dlc_close();
    ag.on = 0;
    apply();
}

void hfp_rfcomm_down(int link)
{
    wait_drop(link);
    if (!ag.on || link != ag.link) return;
    fprintf(stderr, "phone: RFCOMM closed\n");
    ag_gone();
    int w = -1;
    for (int i = 0; i < 4; i++) if (wait_live(i) && (w < 0 || (is_phone(waitq[i].cod) && !is_phone(waitq[w].cod)))) w = i;
    if (w >= 0) {
        waitq[w].on = 0;
        fprintf(stderr, "phone: %012llx waited for the place: it has it now\n", (unsigned long long)waitq[w].addr);
        int sabm = wait_sabm(w);
        hfp_rfcomm_up(waitq[w].link, waitq[w].addr, waitq[w].cod);
        /* RFCOMM does not send a SABM twice (it waits for UA until T1, then gives up): the one it sent is answered */
        if (sabm && ag.on && ag.link == waitq[w].link) { ag.mux = 1; rf_send(0, 1, UA | PF, NULL, 0, -1); }
    }
}
void hfp_lost(void) { memset(waitq, 0, sizeof waitq); ag_gone(); }

/* ---------------------------------------------------------------- the call */

static uint32_t rx_seq; static int16_t rx_frame[CALL_FRAME]; static size_t rx_have; static atomic_int rx_gen;

/* what the phone sends (scoaudio.c's thread): call.c's frames of 20 ms.  A new call's media starts its frames anew
 * here, on the thread that fills them (the last call's may still be in here a moment after sco_stop) */
static void sco_rx(const int16_t *pcm, size_t n)
{
    static int seen_gen;
    int g = atomic_load(&rx_gen);
    if (g != seen_gen) { seen_gen = g; rx_have = 0; rx_seq = 0; }
    while (n) {
        size_t k = CALL_FRAME - rx_have < n ? CALL_FRAME - rx_have : n;
        memcpy(rx_frame + rx_have, pcm, k * 2); rx_have += k; pcm += k; n -= k;
        if (rx_have == CALL_FRAME) { call_rx(atomic_load(&rx_gen), rx_seq++, (const uint8_t *)rx_frame, CALL_PCM_BYTES); rx_have = 0; }
    }
}

/* the mic's frame, gated and gained (call.c's sender) */
static void mic_frame(const uint8_t *frame, size_t len, uint32_t seq, void *arg)
{
    (void)seq; (void)arg;
    if (len == CALL_PCM_BYTES) sco_tx((const int16_t *)frame, CALL_FRAME);
}

/* On the controller thread: sco_start/sco_stop only ask scoaudio.c's own thread, which opens and closes the devices
 * (and waits for the last call's audio threads) without holding this one up */
static void media(int on)
{
    if (on && !ag.gen) {
        int g = call_begin(CALL_HFP, CALL_CODEC_PCM, mic_frame, NULL);
        if (g < 0) return;
        atomic_store(&rx_gen, g); ag.gen = g;
        ag.vgs = -1;                                                /* the Echo's volume to the phone (upkeep) */
        sco_start(ag.sco, ag.air, sco_rx);
    } else if (!on && ag.gen) {
        sco_stop();
        call_end(ag.gen); ag.gen = 0;
        struct call_stats s; call_stats(&s);
        fprintf(stderr, "phone: call audio ended: %u frames in, %u played, %u lost, %u late\n", s.rx, s.played, s.lost, s.late);
    }
}

/* a call the phone has, or one we dialled or answered: only then does the Echo's microphone go to the phone */
static int call_on(void) { return in_call() || ag.setup || ag.dialed || ag.answer_until; }

static int status_now(void)
{
    if (!ag.on || !ag.slc) return HFP_NONE;
    if (ag.setup == 1 && !in_call()) return HFP_RINGING;
    if (in_call()) return ag.sco ? HFP_TALKING : HFP_ON_PHONE;
    if (ag.setup >= 2 || ag.dialed) return ag.sco ? HFP_TALKING : HFP_CALLING;      /* (an outgoing call's ringback) */
    return HFP_IDLE;
}

/* what the Echo shows (call.h's states), from the phone's indicators and the audio link */
static void apply(void)
{
    int incoming = ag.on && ag.setup == 1 && !in_call(), want = CALL_IDLE;
    if (ag.on && ag.answer_until && ms() > ag.answer_until) { ag.answer_until = 0; fprintf(stderr, "phone: answered here, the phone kept the audio\n"); }
    if (ag.dialed) {                                                /* our call: until the phone's call it made ends */
        if (ag.setup >= 2 || in_call()) ag.dial_seen = 1;
        else if (ag.dial_seen || ms() - ag.dialed > DIAL_MS) { if (!ag.dial_seen) fprintf(stderr, "phone: the phone did not call\n"); ag.dialed = 0; ag.dial_seen = 0; }
    }
    if (!ag.on || !atomic_load(&enabled)) want = CALL_IDLE;
    else if (ag.sco && !incoming && call_on()) want = CALL_LIVE;
    else if (incoming) want = ag.claimed || !core_call_refusal(1) ? CALL_RINGING : CALL_IDLE;     /* do not disturb, muted: the phone rings alone */
    else if (ag.answer_until && ag.call) want = CALL_ANSWERING;
    else if (ag.dialed && !ag.call) want = CALL_CALLING;
    if (want != CALL_IDLE && !ag.claimed) {
        if (call_claim(CALL_HFP)) { if (want != ag.shown) fprintf(stderr, "phone: the Echo is in another call: not shown here\n"); want = CALL_IDLE; }
        else ag.claimed = 1;
    }
    media(want == CALL_LIVE);
    if (want != ag.shown && ag.claimed) {
        const char *peer = ag.caller[0] ? ag.caller : a2dp_link_name(ag.link);
        core_call(CALL_HFP, want, peer, ag.dialed != 0 || ag.setup >= 2);
    }
    ag.shown = want;
    if (want == CALL_IDLE && ag.claimed) { call_release(CALL_HFP); ag.claimed = 0; }

    int st = status_now(), changed, second = st < HFP_TALKING ? 0 : ag.has_waiter ? HFP_WAITING : ag.held == 1 ? HFP_HELD : 0;
    char c[sizeof shown_st.caller], ph[sizeof shown_st.phone], o[sizeof shown_st.other];
    snprintf(c, sizeof c, "%s", st >= HFP_RINGING ? ag.caller : ""); ps_utf8_mend(c);
    snprintf(ph, sizeof ph, "%s", ag.on && ag.slc ? a2dp_link_name(ag.link) : ""); ps_utf8_mend(ph);
    snprintf(o, sizeof o, "%s", second == HFP_WAITING ? ag.waiter : second == HFP_HELD ? ag.other : ""); ps_utf8_mend(o);
    pthread_mutex_lock(&smu);
    changed = st != shown_st.state || second != shown_st.second || strcmp(shown_st.caller, c) || strcmp(shown_st.phone, ph) || strcmp(shown_st.other, o);
    shown_st.state = st; shown_st.second = second;
    /* what a phone says (Latin-1 names, a cut character) mended to UTF-8: Home Assistant's API drops a link over
     * invalid text */
    memcpy(shown_st.caller, c, sizeof c); memcpy(shown_st.phone, ph, sizeof ph); memcpy(shown_st.other, o, sizeof o);
    pthread_mutex_unlock(&smu);
    atomic_store(&can_switch, (ag.chld & 4) != 0);
    if (changed) { fprintf(stderr, "phone: %s%s\n", hfp_states[st], second == HFP_WAITING ? ", a call waiting" : second == HFP_HELD ? ", a call on hold" : ""); core_phone_changed(); }
}

void hfp_sco_unanswered(void) { ag.sco_req = 0; }

int hfp_sco_request(uint64_t addr, int esco)
{
    if (!atomic_load(&enabled) || !ag.on || !ag.slc || addr != ag.addr) return 0;
    /* no call, no microphone: a phone may open an audio link whenever it likes (its voice assistant, an app); the
     * Echo's microphone goes to it only for a call (a review of the first version: the link accepted any time).
     * For a call the phone decides where it talks, as with a car kit, also one answered on the phone itself: the
     * owner's decision (2026-11-25), over a review's wish for an Echo-side action first; docs/SECURITY.md says what a
     * paired phone can do with it */
    ag.sco_req = 0;
    if (!call_on()) { fprintf(stderr, "phone: audio link refused: no call\n"); return 0; }
    if (!ag.claimed && call_claimed() != CALL_NOBODY) { fprintf(stderr, "phone: audio link refused: the Echo is in another call\n"); return 0; }
    /* (mSBC only on eSCO: a plain SCO link is accepted, and carries, CVSD) */
    return ag.sco_req = ag.codec == SCO_MSBC && wide() && esco ? SCO_MSBC : SCO_CVSD;
}

void hfp_sco_up(int status, unsigned handle, uint64_t addr, int air)
{
    if (!ag.on || addr != ag.addr) return;
    int req = ag.sco_req; ag.sco_req = 0;                       /* (one end per request: a chip that refuses the
                                                                   command and also completes the link counts once) */
    if (status) {
        fprintf(stderr, "phone: the audio link failed (0x%02x)\n", status);
        /* answered here and the link failed: the phone's retry (as CVSD after the below) still belongs to that answer */
        if (ag.answer_until && ms() <= ag.answer_until && !ag.answer_rearmed) { ag.answer_rearmed = 1; ag.answer_until = ms() + ANSWER_MS; apply(); }
        /* an mSBC link that fails here (a chip that does not take it: models set by their siblings; also a2dp.c's
         * Enhanced Accept refused outright) leaves the call without audio unless the phone asks again.  Once may be
         * the air's; twice: CVSD only on this connection, offered at once (AT+BAC=1: HFP 1.7 lets the hands-free
         * unit update its codecs at any time, 4.11.3's "available codecs"), so the phone's next try is narrowband.
         * Counted only for a link answered as mSBC (not one refused for want of a call, nor a CVSD one), in a row:
         * any good link starts the count anew.  The connection turns CVSD only once the phone is told (the queue may
         * be full: the next failure tries again) */
        if (req == SCO_MSBC && !ag.msbc_off && ag.slc && ++ag.msbc_fails >= 2 && !at_queue("AT+BAC=1")) {
            ag.msbc_off = 1; ag.codec = SCO_CVSD;
            fprintf(stderr, "phone: mSBC failed: CVSD from now on with this phone\n");
        }
        return;
    }
    /* air mode 3 (transparent) carries mSBC: the codec the phone picked; 2 CVSD.  What the link is decides, not the
     * last +BCS (a phone that sets one up without asking) */
    ag.sco = handle; ag.answer_until = 0; ag.air = air == 3 && wide() ? SCO_MSBC : SCO_CVSD;
    ag.msbc_fails = 0; ag.answer_rearmed = 0;
    if (air == 3 && !wide()) fprintf(stderr, "phone: a transparent audio link without mSBC agreed: taken as CVSD\n");
    fprintf(stderr, "phone: audio link up (handle 0x%03x, %s)\n", handle, ag.air == SCO_MSBC ? "mSBC" : "CVSD");
    apply();
}

int hfp_sco_down(unsigned handle)
{
    if (!ag.sco || handle != ag.sco) return 0;
    ag.sco = 0;
    fprintf(stderr, "phone: audio link closed\n");
    apply();
    return 1;
}

int hfp_listed(void) { return atomic_load(&enabled) && sco_present(); }

/* SDP's SupportedFeatures of a hands-free unit: EC/NR, three-way calling, CLI, remote volume; wideband speech where
 * this model codes mSBC (bit 5; AT+BRSF's bits are others) */
unsigned hfp_sdp_features(void) { return 0x17 | (sco_wideband() ? 0x20 : 0); }

/* timers, and a call another holds the claim for (Drop In): looked at again until it is free, so a ring still going
 * shows then */
/* the call's volume (0..100) as the phone's 0..15; -1 not known yet */
static int vgs_now(void) { int v = atomic_load(&call_vol); return v < 0 ? -1 : (v * 15 + 50) / 100; }
static int vgs_due(void) { return ag.gen && ag.slc && vgs_now() >= 0 && vgs_now() != ag.vgs; }

/* (a phone waiting for the place: upkeep looks each round, so wait_retake hands it over once the holder is idle) */
static int waiting_any(void) { for (int i = 0; i < 4; i++) if (waitq[i].on) return 1; return 0; }
int hfp_busy(void) { return ag.waiting || ag.qn || ag.answer_until || ag.dialed || ag.clcc_due || vgs_due() || (ag.on && ag.slc && (ag.setup || in_call()) && !ag.claimed) || waiting_any(); }

/* A phone that waited because the holder (no phone) had a call, an audio link or the claim: the place once it is idle
 * again (controller thread, from upkeep: not inside apply/ag_gone) */
static void wait_retake(void)
{
    if (!ag.on || !ag.cod || is_phone(ag.cod) || call_on() || ag.sco || ag.claimed) return;
    int i = -1;                                         /* a known phone first, else one of no known class */
    for (int k = 0; k < 4; k++) if (wait_live(k) && (is_phone(waitq[k].cod) || !waitq[k].cod) && (i < 0 || (is_phone(waitq[k].cod) && !is_phone(waitq[i].cod)))) i = k;
    if (i >= 0) {
        int link = waitq[i].link, sabm = wait_sabm(i); uint64_t addr = waitq[i].addr; unsigned cod = waitq[i].cod;
        waitq[i].on = 0;
        fprintf(stderr, "phone: %012llx waited: the place is free of calls now\n", (unsigned long long)addr);
        hfp_rfcomm_up(link, addr, cod);                 /* (takes it from the holder: no phone, no call) */
        if (sabm && ag.on && ag.link == link) { ag.mux = 1; rf_send(0, 1, UA | PF, NULL, 0, -1); }
        return;
    }
}

int hfp_upkeep(void)
{
    if (atomic_load(&enabled)) wait_retake();
    if (!atomic_load(&enabled)) memset(waitq, 0, sizeof waitq);    /* off: nobody waits for the place */
    if (ag.on && !atomic_load(&enabled)) {                          /* switched off: the phone loses us */
        if (ag.dlc) rf_send(ag.dlci, 0, DISC | PF, NULL, 0, -1);
        if (ag.mux) rf_send(0, 0, DISC | PF, NULL, 0, -1);
        ag_gone();
    }
    if (ag.on && ag.slc) {
        if (atomic_exchange(&req_answer, 0) && ag.setup == 1 && !in_call() && !at_queue("ATA")) { ag.answer_until = ms() + ANSWER_MS; ag.answer_rearmed = 0; }
        if (atomic_exchange(&req_hangup, 0) && (in_call() || ag.setup)) {
            /* beside a held call: this one ended, the held one back (AT+CHUP leaves what happens to the held one to the
             * phone); beside a waiting one AT+CHUP, which leaves it ringing */
            if (ag.call && ag.held == 1 && !ag.has_waiter && (ag.chld & 2)) { if (!at_queue("AT+CHLD=1")) ag.chld_sent = 1; }
            else at_queue("AT+CHUP");
        }
        if (atomic_exchange(&req_switch, 0) && in_call() && (ag.has_waiter || ag.held) && (ag.chld & 4) && !at_queue("AT+CHLD=2")) ag.chld_sent = 2;
        if (atomic_exchange(&req_dial, 0)) {
            char b[48]; pthread_mutex_lock(&smu); snprintf(b, sizeof b, "ATD%s;", dial_number); pthread_mutex_unlock(&smu);
            if (!at_queue(b)) { ag.dialed = ms(); ag.dial_seen = 0; }
        }
        if (atomic_exchange(&req_here, 0) && in_call() && !ag.sco && call_claimed() == CALL_NOBODY) {
          /* where codecs are negotiated the phone sets the link up (AT+BCC: +BCS, then its link), else ours, CVSD */
          if (wide()) at_queue("AT+BCC");
          else {
            unsigned char s[17]; unsigned h = a2dp_link_handle(ag.link); uint32_t bw = 8000;
            s[0] = h & 0xff; s[1] = h >> 8; memcpy(s + 2, &bw, 4); memcpy(s + 6, &bw, 4);
            /* latency and retransmission: don't care; HV1-3, EV3-5 and the EDR types (bits 6-9 say "may not": none set),
             * as the accept path offers: the controllers agree on what both have */
            s[10] = 0xff; s[11] = 0xff; s[12] = 0x60; s[13] = 0x00; s[14] = 0xff; s[15] = 0x3f; s[16] = 0x00;
            a2dp_later(0x0428, s, 17);
          }
        }
        if (ag.clcc_due && !ag.clcc_on && !ag.waiting && !at_queue("AT+CLCC")) {   /* (asked once the queue is free: its lines are its) */
            ag.clcc_due = 0; ag.clcc_on = 1; ag.clcc_act = ag.clcc_held = ag.clcc_wait = 0; ag.clcc_a[0] = ag.clcc_h[0] = ag.clcc_w[0] = 0;
        }
        if (vgs_due()) { char b[16]; int g = vgs_now(); snprintf(b, sizeof b, "AT+VGS=%d", g); if (!at_queue(b)) ag.vgs = g; }   /* the call's volume to the phone */
        apply();
    } else { atomic_store(&req_answer, 0); atomic_store(&req_hangup, 0); atomic_store(&req_dial, 0); atomic_store(&req_here, 0); atomic_store(&req_switch, 0); }
    if (ag.waiting && ms() - ag.sent_at > AT_MS) {                 /* the phone stopped answering: start again from SABM */
        fprintf(stderr, "phone: no answer to %s: the channel closed\n", ag.sent);
        rf_send(ag.dlci, 0, DISC | PF, NULL, 0, -1);
        dlc_close();
    }
    if (ag.answer_until || ag.dialed) apply();
    at_pump();
    return 0;
}

/* ---------------------------------------------------------------- API (any thread) */

int hfp_present(void) { return sco_present(); }

int hfp_enable(int set)
{
    if (set >= 0 && atomic_exchange(&enabled, set != 0) != (set != 0)) { fprintf(stderr, "phone: calls over Bluetooth %s\n", set ? "on" : "off"); hci_poke(); }
    return atomic_load(&enabled);
}

int hfp_second(char *who, size_t n)
{
    pthread_mutex_lock(&smu);
    int s = shown_st.second;
    if (who && n) snprintf(who, n, "%s", shown_st.other);
    pthread_mutex_unlock(&smu);
    return s;
}

int hfp_status(char *caller, size_t ccap, char *phone, size_t pcap)
{
    pthread_mutex_lock(&smu);
    int st = shown_st.state;
    if (caller && ccap) snprintf(caller, ccap, "%s", shown_st.caller);
    if (phone && pcap) snprintf(phone, pcap, "%s", shown_st.phone);
    pthread_mutex_unlock(&smu);
    return st;
}

static int request(atomic_int *r) { atomic_store(r, 1); hci_poke(); return 1; }

int hfp_answer(void) { return hfp_status(NULL, 0, NULL, 0) == HFP_RINGING && request(&req_answer); }

int hfp_hangup(const char *why)
{
    int st = hfp_status(NULL, 0, NULL, 0);
    if (st < HFP_RINGING) return 0;
    fprintf(stderr, "phone: hanging up (%s)\n", why);
    return request(&req_hangup);
}

int hfp_button(void)
{
    int st = hfp_status(NULL, 0, NULL, 0), who = call_claimed();
    if (who != CALL_HFP) return 0;                                   /* (a call on the phone alone is not the button's) */
    if (st == HFP_RINGING) return request(&req_answer);
    if (hfp_second(NULL, 0) == HFP_WAITING && atomic_load(&can_switch)) return hfp_switch();   /* a call waits: answered, this one held (a car kit's way) */
    return hfp_hangup("the action button");
}

int hfp_switch(void) { return hfp_second(NULL, 0) && request(&req_switch); }

void hfp_call_volume(int pct) { if (atomic_exchange(&call_vol, pct) != pct) hci_poke(); }

int hfp_here(void) { return hfp_status(NULL, 0, NULL, 0) == HFP_ON_PHONE && call_claimed() == CALL_NOBODY && request(&req_here); }

int hfp_dial(const char *number, char *err, size_t n)
{
    size_t len = number ? strlen(number) : 0;
    for (size_t i = 0; i < len; i++)
        if (!isdigit((unsigned char)number[i]) && !strchr("+*#", number[i])) { snprintf(err, n, "not a phone number"); return -1; }
    if (!len || len >= sizeof dial_number) { snprintf(err, n, "not a phone number"); return -1; }
    int st = hfp_status(NULL, 0, NULL, 0);
    if (st == HFP_NONE) { snprintf(err, n, "no phone connected for calls"); return -1; }
    if (st != HFP_IDLE) { snprintf(err, n, "the phone is in a call"); return -1; }
    if (call_claimed() != CALL_NOBODY) { snprintf(err, n, "the Echo is in another call"); return -1; }
    pthread_mutex_lock(&smu); snprintf(dial_number, sizeof dial_number, "%s", number); pthread_mutex_unlock(&smu);
    return request(&req_dial) ? 0 : -1;
}
