/*
 * The phone: the Echo as one extension of a SIP phone system (Asterisk, a Fritz!Box, FreePBX), so a doorbell, a desk
 * phone or a softphone can ring it and it can call them.  A small user agent of our own over UDP (RFC 3261), for one
 * account on one server in the home's network: no TLS, no SRTP, no NAT traversal, no DTMF, no transfer or hold.
 *
 *   register   REGISTER to the server, the user's password by digest (RFC 2617, MD5, qop=auth or none), again at 80 %
 *              of what the server granted; after a refusal (a wrong password) only every REFUSED_MS, so a PBX's
 *              fail2ban never sees a flood; a new address of the Echo (another DHCP lease) registers at once
 *   incoming   an INVITE from the server's address only (anyone else on the LAN: 403), with an SDP offer: 100, 180,
 *              the Echo rings (Drop In's ringtone and ring) until the action button, Home Assistant, the page or the
 *              screen answers (200 with our answer, media at once), declines (603) or RING_MS pass (480); busy (in
 *              a call, or a Drop In: 486), do not disturb or the microphones off (480), no codec in common (488)
 *   outgoing   sip_call: INVITE sip:<number>@<server> with our offer, through any 401/407 once with the password; ring-
 *              back while it rings; 2xx: ACK and media; 3xx-6xx: ACK and a result; hang-up before the answer: CANCEL
 *              (after the first provisional, as RFC 3261 9.1 asks)
 *   in dialog  BYE either way, a re-INVITE answered with the same SDP, OPTIONS (a PBX's qualify) 200 anywhere
 * Every request and every response goes to the server's address (an outbound proxy, as phones on a PBX are set up),
 * with the Record-Route set as Route headers where the server put one; responses go where their request came from.
 * Retransmissions as RFC 3261 17 has them for UDP: T1 500 ms doubling to T2 4 s, given up after 64 T1.  A call ended
 * here still finishes its transactions (the ACK of a 487, a CANCEL, a late 200: ACK and BYE) in `z`, the call that ends.
 *
 * Media: RTP (RFC 3550) on the next port, symmetric (sent where the far end's packets come from, once they come; from
 * the address of its SDP only), no RTCP.  Codecs offered: Opus (RFC 7587: opus/48000/2, 16 kHz inside, FEC) first, then
 * G.711 A-law and mu-law; an offer is answered with the first of its own list we have.  The audio itself is call.c's,
 * Drop In's gate, echo suppressor and playout (call_begin with CALL_PHONE).  One call on the Echo at a time: the phone
 * claims it (call_claim) from ringing or calling on, so a Drop In then gets "busy" and the other way round.
 *
 * Account in state/phone (0600: "server=", "user=", "password=" hex; never exported, the page only says whether there
 * is a password), set from the settings page (sealed, web.c).  Notifications to the core (core_phone*) go through the
 * loop thread, in order, as dropin.c's: callers of sip_* may hold core_lock.
 */
#define _GNU_SOURCE
#include "sip.h"
#include "call.h"
#include "core.h"
#include "hash.h"
#include "statefile.h"
#include "ws.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define SIP_MAX     4000                /* a message, ours or theirs (UDP: a PBX keeps under the MTU without SIP over TCP) */
#define T1          500
#define T2          4000
#define TX_MS       (64 * T1)           /* a transaction given up */
#define RING_MS     60000               /* a call here rings this long at most (the PBX's own timeout is usually shorter) */
#define RINGBACK_MS 120000              /* a call out rings that long at most */
/* connected, no RTP for this long: the far end is gone.  Long: a phone with silence suppression sends nothing in its
 * pauses, and a PBX passes that on; the BYE is what ends a call, this only one whose far end vanished */
#define LOST_MS     120000
#define EXPIRES     600                 /* asked for; servers grant what they like */
#define RETRY_MS    60000               /* a registration that failed: again after a minute */
#define REFUSED_MS  300000              /* the password refused: every five minutes, never a flood of tries */
#define ADDR_MS     5000                /* the Echo's own address looked at */
#define PT_OPUS     111                 /* our dynamic payload type for Opus */

const char *const phone_states[4] = { "idle", "calling", "ringing", "connected" };
const char *const phone_lines[5] = { "off", "connecting", "registered", "refused", "unreachable" };
const char *const phone_results[PHONE_NRESULTS] = { "connected", "ringing", "busy", "declined", "not_answered", "not_found",
                                                   "unavailable", "no_line", "busy_here", "microphones_off_here", "cancelled", "failed" };

static pthread_mutex_t sl = PTHREAD_MUTEX_INITIALIZER;  /* everything below; taken after core_lock, before call.c's */
static atomic_int notify;
static int sock = -1, rsock = -1, running, enabled;
static uint16_t sip_port, rtp_port;

static struct { char server[128], user[64], pass[128]; } account;
static struct sockaddr_in srv;                           /* the server, resolved (sin_port 0: not yet) */
static long long srv_at;
static char local_ip[16] = "0.0.0.0";
static long long addr_at;

/* a request of ours, sent again until answered (REGISTER, INVITE, BYE, CANCEL) */
struct tx { int on; char buf[SIP_MAX]; size_t n; char branch[40]; long long next, until; int iv; int answered; };

static struct {
    int line; char text[128];
    long long next;                     /* the next REGISTER */
    char call_id[48], tag[20]; uint32_t cseq;
    int auth_tries, granted;
    struct tx t;
} reg;

/* A call: the one running (c), and the one ending (z), which only finishes its transactions */
struct call {
    int state, outgoing, rang, gen, acked, ok_sent;
    char peer[96];
    char call_id[96], ltag[24];
    char lhdr[300], rhdr[300];          /* From/To of each side as sent in this dialog, tags included */
    char target[256];                   /* the far end's Contact */
    char route[4][256]; int nroute;
    uint32_t lcseq;
    struct sockaddr_in to;              /* where the dialog's messages go */
    /* a call here: what each response repeats of its INVITE */
    char vias[1200], ifrom[300], ito[300], icseq[64], ibranch[80];
    char resp[SIP_MAX]; size_t resp_n; long long resp_next, resp_until; int resp_iv;     /* a final response until its ACK */
    /* a call out: its INVITE (the ruri and the To before a tag, for CANCEL and ACK) */
    struct tx inv; char ruri[200], ito0[300]; uint32_t icseq_n; int auth_tries, cancel, cancel_sent;
    char ack[SIP_MAX]; size_t ack_n;    /* the ACK of a 2xx, again for each 2xx again */
    /* media */
    int codec, pt, media; struct sockaddr_in rtp; uint16_t tx_seq; uint32_t tx_ts, ssrc; int first_tx;
    int rx_any; uint16_t rx_last; uint32_t rx_ext;
    char offer[1200];                   /* the far end's SDP, while it rings here */
    long long since, live_at, rx_at;
};
static struct call c, z;
static struct tx bye_tx, cancel_tx;    /* the BYE or CANCEL of a call that ended */

#define EVQ 16
static struct ev { int state, outgoing, result, here; char peer[96]; } evq[EVQ];
static unsigned ev_in, ev_out;
static atomic_int line_told = -1;
static int last_result = -1; static unsigned attempts;

static void cpy(char *d, size_t cap, const char *s)     /* bounded, cut where it does not fit */
{
    size_t n = strlen(s); if (n >= cap) n = cap - 1;
    memcpy(d, s, n); d[n] = 0;
}

static long long now_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void rand_hex(char *out, int bytes)
{
    uint8_t r[32]; ws_random(r, (size_t)bytes);
    for (int i = 0; i < bytes; i++) sprintf(out + 2 * i, "%02x", r[i]);
}

static void ev_push(int state, const char *peer, int outgoing, int result, int here)     /* sl held */
{
    struct ev *e = &evq[ev_in++ % EVQ];
    if (ev_in - ev_out > EVQ) ev_out = ev_in - EVQ;
    e->state = state; e->outgoing = outgoing; e->result = result; e->here = here;
    cpy(e->peer, sizeof e->peer, peer);
    if (result >= 0) last_result = result;
    atomic_store(&notify, 1);
}

static void set_line(int line, const char *fmt, ...)    /* sl held */
{
    va_list ap; char t[128];
    va_start(ap, fmt); vsnprintf(t, sizeof t, fmt, ap); va_end(ap);
    if (line != reg.line || strcmp(t, reg.text)) fprintf(stderr, "phone: line %s: %s\n", phone_lines[line], t);
    reg.line = line; cpy(reg.text, sizeof reg.text, t);
    atomic_store(&notify, 1);
}

/* ---------------------------------------------------------------- the account */

static const char *state_dir(void) { const char *e = getenv("HASSMIC_STATE"); return e ? e : "/data/local/hassmic/state"; }

static int server_ok(const char *s)
{
    const char *colon = strchr(s, ':');
    if (!*s || strlen(s) >= sizeof account.server) return 0;
    for (const char *p = s; *p && p != colon; p++) if (!isalnum((unsigned char)*p) && *p != '.' && *p != '-') return 0;
    if (colon) { char *e; long p = strtol(colon + 1, &e, 10); if (*e || p < 1 || p > 65535) return 0; }
    return 1;
}

static int user_ok(const char *s)               /* a SIP user part of the plain kind: what extensions are */
{
    if (!*s || strlen(s) >= sizeof account.user) return 0;
    for (const char *p = s; *p; p++) if (!isalnum((unsigned char)*p) && !strchr("._+-*#", *p)) return 0;
    return 1;
}

static int pass_ok(const char *s)
{
    if (strlen(s) >= sizeof account.pass) return 0;
    for (const char *p = s; *p; p++) if ((unsigned char)*p < 0x20 || *p == 0x7f) return 0;
    return 1;
}

static void load(void)
{
    char path[300], line[400]; FILE *f;
    snprintf(path, sizeof path, "%s/phone", state_dir());
    if (!(f = fopen(path, "r"))) return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "server=", 7) && server_ok(line + 7)) cpy(account.server, sizeof account.server, line + 7);
        else if (!strncmp(line, "user=", 5) && user_ok(line + 5)) cpy(account.user, sizeof account.user, line + 5);
        else if (!strncmp(line, "password=", 9)) {
            size_t n = strlen(line + 9) / 2; char p[sizeof account.pass] = "";
            if (n < sizeof p) { for (size_t i = 0; i < n; i++) { unsigned v; if (sscanf(line + 9 + 2 * i, "%2x", &v) != 1) { n = 0; break; } p[i] = (char)v; } p[n] = 0; }
            if (pass_ok(p)) memcpy(account.pass, p, sizeof p);
            memset(p, 0, sizeof p);
        }
    }
    memset(line, 0, sizeof line);
    fclose(f);
}

static int save(void)                           /* sl held */
{
    char path[300], t[600]; int n = 0;
    snprintf(path, sizeof path, "%s/phone", state_dir());
    n += snprintf(t + n, sizeof t - (size_t)n, "server=%s\nuser=%s\npassword=", account.server, account.user);
    for (const char *p = account.pass; *p && n < (int)sizeof t - 4; p++) n += snprintf(t + n, sizeof t - (size_t)n, "%02x", (unsigned char)*p);
    n += snprintf(t + n, sizeof t - (size_t)n, "\n");
    int r = statefile_write(path, 0600, t, (size_t)n);
    memset(t, 0, sizeof t);
    return r;
}

static int have_account(void) { return account.server[0] && account.user[0]; }

/* a bounded appender: what does not fit is cut (the message is then refused at the far end, never overrun here) */
struct out { char *p; size_t n, cap; };
static void put(struct out *o, const char *fmt, ...)
{
    va_list ap;
    if (o->n >= o->cap) return;
    va_start(ap, fmt); int k = vsnprintf(o->p + o->n, o->cap - o->n, fmt, ap); va_end(ap);
    if (k > 0) o->n = o->n + (size_t)k < o->cap ? o->n + (size_t)k : o->cap - 1;
}

/* ---------------------------------------------------------------- messages */

struct msg {
    int req, code; char method[16], uri[256], reason[64];
    int nh; struct { const char *name; char *val; } h[64];
    const char *body; size_t blen;
    char raw[SIP_MAX + 1];
};

static const char *canon(const char *n)         /* RFC 3261 7.3.3's compact forms */
{
    static const char *const map[][2] = { { "v", "Via" }, { "f", "From" }, { "t", "To" }, { "i", "Call-ID" }, { "m", "Contact" },
                                          { "l", "Content-Length" }, { "c", "Content-Type" }, { "k", "Supported" } };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) if (!strcasecmp(n, map[i][0])) return map[i][1];
    return n;
}

static int parse(struct msg *m, const uint8_t *p, size_t n)
{
    char *s, *e, *line;
    if (n > SIP_MAX) return -1;
    memcpy(m->raw, p, n); m->raw[n] = 0; m->nh = 0; m->body = ""; m->blen = 0;
    s = m->raw;
    if (!(e = strstr(s, "\r\n"))) return -1;
    *e = 0; line = s; s = e + 2;
    if (!strncmp(line, "SIP/2.0 ", 8)) {
        m->req = 0; m->code = atoi(line + 8);
        const char *r = strchr(line + 8, ' ');
        cpy(m->reason, sizeof m->reason, r ? r + 1 : "");
        if (m->code < 100 || m->code > 699) return -1;
    } else {
        char ver[16];
        m->req = 1;
        if (sscanf(line, "%15s %255s %15s", m->method, m->uri, ver) != 3 || strcmp(ver, "SIP/2.0")) return -1;
    }
    while ((e = strstr(s, "\r\n"))) {
        *e = 0;
        if (!*s) { s = e + 2; break; }                  /* the end of the headers */
        char *colon = strchr(s, ':');
        if (colon && m->nh < (int)(sizeof m->h / sizeof m->h[0])) {
            char *ne = colon; while (ne > s && (ne[-1] == ' ' || ne[-1] == '\t')) ne--;
            *ne = 0; char *v = colon + 1; while (*v == ' ' || *v == '\t') v++;
            m->h[m->nh].name = canon(s); m->h[m->nh].val = v; m->nh++;
        }
        s = e + 2;
    }
    m->body = s; m->blen = strlen(s);
    for (int i = 0; i < m->nh; i++) if (!strcasecmp(m->h[i].name, "Content-Length")) {
        long l = atol(m->h[i].val); if (l >= 0 && (size_t)l < m->blen) m->blen = (size_t)l;
    }
    return 0;
}

static const char *hget(const struct msg *m, const char *name)
{
    for (int i = 0; i < m->nh; i++) if (!strcasecmp(m->h[i].name, name)) return m->h[i].val;
    return NULL;
}

/* a parameter of a header value (";branch=", ";tag=", ";expires="): after the URI's '>' where there is one */
static int param(const char *v, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name); const char *p = v ? strchr(v, '>') : NULL;
    if (!v) return -1;
    if (!p) p = v;
    for (p = strchr(p, ';'); p; p = strchr(p + 1, ';')) {
        const char *q = p + 1; while (*q == ' ') q++;
        if (!strncasecmp(q, name, nl) && (q[nl] == '=' || q[nl] == ';' || q[nl] == ',' || !q[nl] || q[nl] == ' ')) {
            q += nl; size_t k = 0;
            if (*q == '=') { q++; if (*q == '"') q++; while (*q && !strchr(";,\" >", *q) && k + 1 < cap) out[k++] = *q++; }
            out[k] = 0; return 0;
        }
    }
    return -1;
}

static void uri_of(const char *v, char *out, size_t cap)        /* the URI of a name-addr or addr-spec */
{
    const char *a = strchr(v, '<'), *b; size_t n;
    if (a) { a++; b = strchr(a, '>'); n = b ? (size_t)(b - a) : strlen(a); }
    else { a = v; n = strcspn(a, "; ,"); }
    if (n >= cap) n = cap - 1;
    memcpy(out, a, n); out[n] = 0;
}

static void uri_user(const char *uri, char *out, size_t cap)
{
    const char *p = !strncasecmp(uri, "sips:", 5) ? uri + 5 : !strncasecmp(uri, "sip:", 4) ? uri + 4 : uri;
    const char *at = strchr(p, '@'); size_t n = at ? (size_t)(at - p) : 0;
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n); out[n] = 0;
}

static void display_of(const char *v, char *out, size_t cap)    /* '"Portero" <sip:60@...>' -> Portero */
{
    size_t k = 0;
    while (*v == ' ') v++;
    if (*v == '"') { v++; while (*v && *v != '"' && k + 1 < cap) { if (*v == '\\' && v[1]) v++; out[k++] = *v++; } }
    else { const char *a = strchr(v, '<'); if (a) while (v < a && k + 1 < cap) out[k++] = *v++; }
    while (k && out[k - 1] == ' ') k--;
    out[k] = 0;
}

static void clean(char *s)                      /* for Home Assistant, the page's JSON and the log */
{
    for (unsigned char *p = (unsigned char *)s; *p; p++) if (*p < 0x20 || *p == '"' || *p == '\\' || *p == 0x7f) *p = ' ';
}

static void peer_of(const char *hdr, char *out, size_t cap)     /* "Portero (60)", or "60" */
{
    char d[64], u[64], uri[200];
    display_of(hdr, d, sizeof d); uri_of(hdr, uri, sizeof uri); uri_user(uri, u, sizeof u);
    struct out o = { out, 0, cap };
    out[0] = 0;
    if (d[0] && strcmp(d, u)) put(&o, "%s%s%s%s", d, u[0] ? " (" : "", u, u[0] ? ")" : "");
    else snprintf(out, cap, "%s", u[0] ? u : d[0] ? d : "unknown");
    clean(out);
}

static void send_to(const struct sockaddr_in *to, const char *buf, size_t n)
{
    if (sendto(sock, buf, n, 0, (const struct sockaddr *)to, sizeof *to) < 0 && errno != EAGAIN) {
        static long long logged;
        if (now_ms() - logged > 5000) { logged = now_ms(); fprintf(stderr, "phone: send: %s\n", strerror(errno)); }
    }
}

/* ---------------------------------------------------------------- SDP */

static size_t sdp(char *out, size_t cap, int answer_pt, int codec)     /* answer_pt < 0: our offer */
{
    static unsigned ver;
    struct out o = { out, 0, cap };
    put(&o, "v=0\r\no=hassmic 1 %u IN IP4 %s\r\ns=hassmic\r\nc=IN IP4 %s\r\nt=0 0\r\n", ++ver, local_ip, local_ip);
    if (answer_pt < 0) put(&o, "m=audio %u RTP/AVP %d 8 0\r\n", rtp_port, PT_OPUS);
    else put(&o, "m=audio %u RTP/AVP %d\r\n", rtp_port, answer_pt);
    if (answer_pt < 0 || codec == CODEC_OPUS)
        put(&o, "a=rtpmap:%d opus/48000/2\r\na=fmtp:%d maxplaybackrate=16000;sprop-maxcapturerate=16000;useinbandfec=1\r\n",
            answer_pt < 0 ? PT_OPUS : answer_pt, answer_pt < 0 ? PT_OPUS : answer_pt);
    if (answer_pt < 0 || codec == CODEC_PCMA) put(&o, "a=rtpmap:8 PCMA/8000\r\n");
    if (answer_pt < 0 || codec == CODEC_PCMU) put(&o, "a=rtpmap:0 PCMU/8000\r\n");
    put(&o, "a=ptime:20\r\na=sendrecv\r\n");
    return o.n;
}

/* The first codec of an offer's (or answer's) list we have: its payload type, codec, and where its RTP goes.  -1: none */
static int sdp_pick(const char *body, int *codec, struct sockaddr_in *rtp)
{
    char ip[64] = "", mip[64] = "", line[300]; unsigned port = 0; int pts[32], npt = 0, in_audio = 0;
    struct { int pt; char name[32]; int rate; } map[32]; int nmap = 0;
    for (const char *p = body; *p; ) {
        size_t n = strcspn(p, "\r\n"); if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, p, n); line[n] = 0; p += n; while (*p == '\r' || *p == '\n') p++;
        if (!strncmp(line, "m=", 2)) {
            in_audio = !strncmp(line, "m=audio ", 8);
            if (in_audio && !npt) {
                char *q = line + 8, *e; port = (unsigned)strtoul(q, &e, 10); q = e;
                while (*q == ' ') q++;
                q += strcspn(q, " ");                   /* RTP/AVP */
                while (*q && npt < 32) { long v = strtol(q, &e, 10); if (e == q) break; pts[npt++] = (int)v; q = e; }
            }
        } else if (!strncmp(line, "c=IN IP4 ", 9)) snprintf(in_audio ? mip : ip, sizeof ip, "%.*s", (int)strcspn(line + 9, " /"), line + 9);
        else if (in_audio && !strncmp(line, "a=rtpmap:", 9) && nmap < 32) {
            char nm[32]; int pt, rate;
            if (sscanf(line + 9, "%d %31[^/]/%d", &pt, nm, &rate) == 3) { map[nmap].pt = pt; snprintf(map[nmap].name, sizeof map[0].name, "%s", nm); map[nmap].rate = rate; nmap++; }
        }
    }
    if (!port || !npt) return -1;
    memset(rtp, 0, sizeof *rtp); rtp->sin_family = AF_INET; rtp->sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, mip[0] ? mip : ip, &rtp->sin_addr) != 1) return -1;
    for (int i = 0; i < npt; i++) {
        int pt = pts[i], k = -1;
        for (int j = 0; j < nmap; j++) if (map[j].pt == pt) k = j;
        if (k >= 0 && !strcasecmp(map[k].name, "opus") && map[k].rate == 48000) { *codec = CODEC_OPUS; return pt; }
        if (pt == 8 && (k < 0 || !strcasecmp(map[k].name, "PCMA"))) { *codec = CODEC_PCMA; return pt; }
        if (pt == 0 && (k < 0 || !strcasecmp(map[k].name, "PCMU"))) { *codec = CODEC_PCMU; return pt; }
        if (k >= 0 && !strcasecmp(map[k].name, "PCMA") && map[k].rate == 8000) { *codec = CODEC_PCMA; return pt; }
        if (k >= 0 && !strcasecmp(map[k].name, "PCMU") && map[k].rate == 8000) { *codec = CODEC_PCMU; return pt; }
    }
    return -1;
}

/* ---------------------------------------------------------------- digest authentication */

/* The answer to a challenge (WWW-Authenticate or Proxy-Authenticate) as its header line, "" if it cannot be met */
static void digest(const char *chal, int proxy, const char *method, const char *uri, char *out, size_t cap)
{
    char realm[128] = "", nonce[160] = "", opaque[160] = "", algo[32] = "MD5", qop[64] = "", cnonce[17], ha1[33], ha2[33], resp[33], in[700];
    uint8_t d[16]; int n;
    out[0] = 0;
    if (!chal || strncasecmp(chal, "Digest", 6)) return;
    /* the challenge's parameters: name="value" or name=value, comma separated (param() reads ';', so by hand) */
    for (const char *p = chal + 6; *p; ) {
        while (*p == ' ' || *p == ',') p++;
        const char *eq = strchr(p, '='); if (!eq) break;
        char name[32]; size_t nl = (size_t)(eq - p); if (nl >= sizeof name) nl = sizeof name - 1;
        memcpy(name, p, nl); name[nl] = 0; while (nl && name[nl - 1] == ' ') name[--nl] = 0;
        p = eq + 1; while (*p == ' ') p++;
        char v[200]; size_t k = 0;
        if (*p == '"') { p++; while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; if (k + 1 < sizeof v) v[k++] = *p; p++; } if (*p) p++; }
        else while (*p && *p != ',') { if (k + 1 < sizeof v) v[k++] = *p; p++; }
        while (k && v[k - 1] == ' ') k--;
        v[k] = 0;
        if (!strcasecmp(name, "realm")) cpy(realm, sizeof realm, v);
        else if (!strcasecmp(name, "nonce")) cpy(nonce, sizeof nonce, v);
        else if (!strcasecmp(name, "opaque")) cpy(opaque, sizeof opaque, v);
        else if (!strcasecmp(name, "algorithm")) cpy(algo, sizeof algo, v);
        else if (!strcasecmp(name, "qop")) cpy(qop, sizeof qop, v);
    }
    if (strcasecmp(algo, "MD5") || !nonce[0]) return;           /* MD5-sess, SHA-256: not ours */
    int auth = 0;                                               /* qop="auth,auth-int": auth */
    { char *sv = NULL; for (char *t = strtok_r(qop, ", ", &sv); t; t = strtok_r(NULL, ", ", &sv)) auth |= !strcasecmp(t, "auth"); }
    n = snprintf(in, sizeof in, "%s:%s:%s", account.user, realm, account.pass);
    md5(in, (size_t)n, d); for (int i = 0; i < 16; i++) sprintf(ha1 + 2 * i, "%02x", d[i]);
    memset(in, 0, sizeof in);
    n = snprintf(in, sizeof in, "%s:%s", method, uri);
    md5(in, (size_t)n, d); for (int i = 0; i < 16; i++) sprintf(ha2 + 2 * i, "%02x", d[i]);
    rand_hex(cnonce, 8);
    if (auth) n = snprintf(in, sizeof in, "%s:%s:00000001:%s:auth:%s", ha1, nonce, cnonce, ha2);
    else n = snprintf(in, sizeof in, "%s:%s:%s", ha1, nonce, ha2);
    md5(in, (size_t)n, d); for (int i = 0; i < 16; i++) sprintf(resp + 2 * i, "%02x", d[i]);
    memset(ha1, 0, sizeof ha1);
    struct out o = { out, 0, cap };
    put(&o, "%s: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\", algorithm=MD5",
        proxy ? "Proxy-Authorization" : "Authorization", account.user, realm, nonce, uri, resp);
    if (opaque[0]) put(&o, ", opaque=\"%s\"", opaque);
    if (auth) put(&o, ", qop=auth, nc=00000001, cnonce=\"%s\"", cnonce);
    put(&o, "\r\n");
}

/* ---------------------------------------------------------------- building requests */

static void tx_start(struct tx *t, long long now)
{
    t->on = 1; t->answered = 0; t->iv = T1; t->next = now + T1; t->until = now + TX_MS;
    send_to(&srv, t->buf, t->n);
}

/* A request into t->buf: method, request URI, From/To/Call-ID/CSeq as given, a fresh branch; extra: whole header lines */
static void build(struct tx *t, const char *method, const char *ruri, const char *from, const char *to, const char *call_id,
                  uint32_t cseq, const char *cseq_method, char routes[][256], int nroute, const char *extra, const char *ctype, const char *body)
{
    struct out o = { t->buf, 0, sizeof t->buf };
    if (!cseq_method || strcmp(cseq_method, "ACK") || strcmp(method, "ACK") || !t->branch[0]) {   /* a 2xx's ACK: its own */
        strcpy(t->branch, "z9hG4bK"); rand_hex(t->branch + 7, 10);
    }
    put(&o, "%s %s SIP/2.0\r\nVia: SIP/2.0/UDP %s:%u;rport;branch=%s\r\nMax-Forwards: 70\r\n", method, ruri, local_ip, sip_port, t->branch);
    for (int i = 0; i < nroute; i++) put(&o, "Route: %s\r\n", routes[i]);
    put(&o, "From: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %u %s\r\n", from, to, call_id, cseq, cseq_method ? cseq_method : method);
    if (extra) put(&o, "%s", extra);
    put(&o, "User-Agent: hassmic/%s\r\n", VERSION);
    if (body && *body) put(&o, "Content-Type: %s\r\nContent-Length: %zu\r\n\r\n%s", ctype, strlen(body), body);
    else put(&o, "Content-Length: 0\r\n\r\n");
    t->n = o.n;
}

static void contact(char *out, size_t cap)
{
    snprintf(out, cap, "Contact: <sip:%s@%s:%u>\r\n", account.user, local_ip, sip_port);
}

static const char ALLOW[] = "Allow: INVITE, ACK, CANCEL, BYE, OPTIONS\r\n";

/* ---------------------------------------------------------------- registration (sl held) */

static void reg_send(long long now, const char *auth, int expires)
{
    char from[300], uri[160], extra[1200], ct[200];
    snprintf(uri, sizeof uri, "sip:%s", account.server);
    snprintf(from, sizeof from, "<sip:%s@%s>;tag=%s", account.user, account.server, reg.tag);
    contact(ct, sizeof ct);
    snprintf(extra, sizeof extra, "%s%sExpires: %d\r\n%s", ct, ALLOW, expires, auth ? auth : "");
    char to[300]; snprintf(to, sizeof to, "<sip:%s@%s>", account.user, account.server);
    build(&reg.t, "REGISTER", uri, from, to, reg.call_id, ++reg.cseq, NULL, NULL, 0, extra, NULL, NULL);
    tx_start(&reg.t, now);
}

static void unregister(void)                    /* sl held: once, best effort, as the binding would expire anyway */
{
    if (reg.line != LINE_UP || !srv.sin_port) return;
    reg_send(now_ms(), NULL, 0); reg.t.on = 0;
}

static void on_register(const struct msg *m, long long now)
{
    if (m->code < 200) return;
    reg.t.on = 0;
    if ((m->code == 401 || m->code == 407) && reg.auth_tries < 2 && account.pass[0]) {
        char auth[900], uri[160];
        snprintf(uri, sizeof uri, "sip:%s", account.server);
        digest(hget(m, m->code == 401 ? "WWW-Authenticate" : "Proxy-Authenticate"), m->code == 407, "REGISTER", uri, auth, sizeof auth);
        if (auth[0]) { reg.auth_tries++; reg_send(now, auth, EXPIRES); memset(auth, 0, sizeof auth); return; }
    }
    if (m->code == 423) {                       /* Interval Too Brief */
        const char *me = hget(m, "Min-Expires");
        int v = me ? atoi(me) : 0;
        if (v > 0 && v < 86400) { reg.granted = v; reg.next = now; return; }
    }
    if (m->code >= 200 && m->code < 300) {
        int exp = EXPIRES; char e[16];
        const char *h = hget(m, "Expires"); if (h) exp = atoi(h);
        for (int i = 0; i < m->nh; i++) if (!strcasecmp(m->h[i].name, "Contact") && strstr(m->h[i].val, local_ip) && !param(m->h[i].val, "expires", e, sizeof e)) exp = atoi(e);
        if (exp < 30) exp = 30;
        reg.next = now + (long long)exp * 800;                 /* at 80 % */
        reg.auth_tries = 0;
        set_line(LINE_UP, "registered at %s as %s (%d s)", account.server, account.user, exp);
        return;
    }
    if (m->code == 401 || m->code == 403 || m->code == 407) {
        reg.next = now + REFUSED_MS;
        set_line(LINE_REFUSED, "%s refused %s: %d %s", account.server, account.user, m->code, m->reason);
    } else {
        reg.next = now + RETRY_MS;
        set_line(LINE_UNREACHABLE, "%s answered %d %s", account.server, m->code, m->reason);
    }
    reg.auth_tries = 0;
}

/* ---------------------------------------------------------------- calls (sl held) */

static void rtp_send(const uint8_t *frame, size_t len, uint32_t seq, void *arg);

static void reset_call(struct call *k) { memset(k, 0, sizeof *k); k->media = -1; }

static void go_live(long long now)
{
    c.state = PHONE_LIVE; c.live_at = c.rx_at = now;
    c.tx_seq = (uint16_t)rand(); c.tx_ts = (uint32_t)rand(); c.first_tx = 1;
    { uint8_t r[4]; ws_random(r, 4); c.ssrc = (uint32_t)r[0] << 24 | r[1] << 16 | r[2] << 8 | r[3]; }
    c.rx_any = 0;
    c.media = call_begin(CALL_PHONE, c.codec, rtp_send, NULL);
    ev_push(PHONE_LIVE, c.peer, c.outgoing, c.outgoing ? PHONE_R_CONNECTED : -1, 0);
    fprintf(stderr, "phone: connected with %s (%s)\n", c.peer, c.codec == CODEC_OPUS ? "Opus" : c.codec == CODEC_PCMA ? "G.711 A-law" : "G.711 mu-law");
}

/* The call over here: the core told, its media gone; what its transactions still owe moves to z */
static void end_call(const char *why, int result)
{
    if (c.state == PHONE_IDLE) return;
    if (c.state == PHONE_LIVE) {
        struct call_stats st; call_stats(&st); call_end(c.media);
        fprintf(stderr, "phone: ended with %s after %lld s (%s); %u frames in, %u played, %u of them missing (%u came too late), "
                "longest gap %lld ms\n", c.peer, (now_ms() - c.live_at) / 1000, why, st.rx, st.played, st.lost, st.late, st.gap_max);
    } else fprintf(stderr, "phone: %s %s: %s\n", c.outgoing ? "call to" : "call from", c.peer, why);
    ev_push(PHONE_IDLE, "", c.outgoing, c.state == PHONE_CALLING && c.outgoing ? result : -1, 0);
    z = c; z.state = PHONE_IDLE; z.since = now_ms();
    reset_call(&c);
    call_release(CALL_PHONE);
}

static void dialog_request(struct tx *t, const char *method, const char *extra)    /* BYE in c's dialog */
{
    build(t, method, c.target[0] ? c.target : c.ruri, c.lhdr, c.rhdr, c.call_id, ++c.lcseq, NULL, c.route, c.nroute, extra, NULL, NULL);
}

static void send_bye(const char *why)
{
    char extra[160]; snprintf(extra, sizeof extra, "Reason: Q.850;cause=16;text=\"%.80s\"\r\n", why);
    dialog_request(&bye_tx, "BYE", extra);
    bye_tx.on = 1; bye_tx.iv = T1; bye_tx.next = now_ms() + T1; bye_tx.until = now_ms() + TX_MS;
    send_to(&c.to, bye_tx.buf, bye_tx.n);
}

/* ---- a call here: responses to its INVITE */

static size_t response(char *out, size_t cap, int code, const char *reason, const struct call *k, const char *ctype_body)
{
    struct out o = { out, 0, cap };
    put(&o, "SIP/2.0 %d %s\r\n%sFrom: %s\r\nTo: %s", code, reason, k->vias, k->ifrom, k->ito);
    if (code > 100 && !strstr(k->ito, "tag=")) put(&o, ";tag=%s", k->ltag);
    put(&o, "\r\nCall-ID: %s\r\nCSeq: %s\r\n", k->call_id, k->icseq);
    if (code >= 180 && code < 300) { char ct[200]; contact(ct, sizeof ct); put(&o, "%s%s", ct, ALLOW); }
    put(&o, "User-Agent: hassmic/%s\r\n", VERSION);
    if (ctype_body && *ctype_body) put(&o, "Content-Type: application/sdp\r\nContent-Length: %zu\r\n\r\n%s", strlen(ctype_body), ctype_body);
    else put(&o, "Content-Length: 0\r\n\r\n");
    return o.n;
}

static void respond_invite(int code, const char *reason, const char *body)     /* c's INVITE; a final one until its ACK */
{
    c.resp_n = response(c.resp, sizeof c.resp, code, reason, &c, body);
    send_to(&c.to, c.resp, c.resp_n);
    if (code >= 200) { long long now = now_ms(); c.resp_iv = T1; c.resp_next = now + T1; c.resp_until = now + TX_MS; c.acked = 0; }
}

/* The Vias of a request as its responses repeat them: the top one with where it came from (RFC 3581) */
static void vias_of(const struct msg *m, const struct sockaddr_in *from, char *out, size_t cap)
{
    struct out o = { out, 0, cap }; int top = 1;
    for (int i = 0; i < m->nh; i++) {
        if (strcasecmp(m->h[i].name, "Via")) continue;
        if (top) {
            char ip[16]; inet_ntop(AF_INET, &from->sin_addr, ip, sizeof ip);
            const char *rp = strcasestr(m->h[i].val, ";rport");
            if (rp && (rp[6] == ';' || !rp[6] || rp[6] == ',')) put(&o, "Via: %.*s;rport=%u;received=%s%s\r\n", (int)(rp - m->h[i].val), m->h[i].val, ntohs(from->sin_port), ip, rp + 6);
            else put(&o, "Via: %s;received=%s\r\n", m->h[i].val, ip);
            top = 0;
        } else put(&o, "Via: %s\r\n", m->h[i].val);
    }
}

static void reply(const struct msg *m, const struct sockaddr_in *from, int code, const char *reason, const char *extra)  /* outside a call */
{
    char buf[SIP_MAX], vias[1200], tag[24]; struct out o = { buf, 0, sizeof buf };
    const char *to = hget(m, "To");
    vias_of(m, from, vias, sizeof vias); rand_hex(tag, 6);
    put(&o, "SIP/2.0 %d %s\r\n%sFrom: %s\r\nTo: %s", code, reason, vias, hget(m, "From") ? hget(m, "From") : "", to ? to : "");
    if (to && !strstr(to, "tag=")) put(&o, ";tag=%s", tag);
    put(&o, "\r\nCall-ID: %s\r\nCSeq: %s\r\n%sUser-Agent: hassmic/%s\r\n%sContent-Length: 0\r\n\r\n", hget(m, "Call-ID") ? hget(m, "Call-ID") : "",
        hget(m, "CSeq") ? hget(m, "CSeq") : "", ALLOW, VERSION, extra ? extra : "");
    send_to(from, buf, o.n);
}

static void route_set(const struct msg *m, struct call *k, int reverse)        /* Record-Route, as Route headers */
{
    char all[8][256]; int n = 0;
    for (int i = 0; i < m->nh && n < 8; i++) {
        if (strcasecmp(m->h[i].name, "Record-Route")) continue;
        for (const char *p = m->h[i].val; *p && n < 8; ) {          /* "<a>, <b>" */
            while (*p == ' ' || *p == ',') p++;
            const char *a = strchr(p, '<'), *b = a ? strchr(a, '>') : NULL;
            if (!a || !b) break;
            snprintf(all[n++], sizeof all[0], "%.*s", (int)(b - a + 1), a);
            p = b + 1;
        }
    }
    k->nroute = n < 4 ? n : 4;
    for (int i = 0; i < k->nroute; i++) memcpy(k->route[i], all[reverse ? n - 1 - i : i], sizeof k->route[0]);
}

static void on_invite(const struct msg *m, const struct sockaddr_in *from, const char *no, long long now)
{
    char branch[80] = "", tag[64] = "";
    const char *cid = hget(m, "Call-ID"), *to = hget(m, "To"), *fr = hget(m, "From"), *cs = hget(m, "CSeq");
    param(hget(m, "Via"), "branch", branch, sizeof branch);
    if (!cid || !to || !fr || !cs) { reply(m, from, 400, "Bad Request", NULL); return; }
    param(to, "tag", tag, sizeof tag);
    if (c.state && !strcmp(c.call_id, cid)) {
        if (!strcmp(c.ibranch, branch)) { if (c.resp_n) send_to(&c.to, c.resp, c.resp_n); return; }    /* again */
        if (tag[0] && !strcmp(tag, c.ltag) && c.state == PHONE_LIVE) {           /* a re-INVITE: the same answer */
            char body[1200]; int codec; struct sockaddr_in rtp; char ans[1200] = "";
            if (m->blen && sdp_pick(m->body, &codec, &rtp) >= 0 && codec == c.codec) c.rtp = rtp;
            sdp(ans, sizeof ans, c.pt, c.codec);
            cpy(body, sizeof body, ans);
            struct call k = c; cpy(k.ibranch, sizeof k.ibranch, branch);
            vias_of(m, from, k.vias, sizeof k.vias); cpy(k.ifrom, sizeof k.ifrom, fr); cpy(k.ito, sizeof k.ito, to);
            cpy(k.icseq, sizeof k.icseq, cs);
            char buf[SIP_MAX]; size_t n = response(buf, sizeof buf, 200, "OK", &k, body);
            send_to(from, buf, n);
            return;
        }
    }
    if (z.call_id[0] && !strcmp(z.call_id, cid) && !strcmp(z.ibranch, branch)) { if (z.resp_n) send_to(from, z.resp, z.resp_n); return; }
    if (tag[0]) { reply(m, from, 481, "Call/Transaction Does Not Exist", NULL); return; }
    /* a new call */
    struct call k; reset_call(&k);
    cpy(k.call_id, sizeof k.call_id, cid); rand_hex(k.ltag, 8);
    cpy(k.ibranch, sizeof k.ibranch, branch);
    vias_of(m, from, k.vias, sizeof k.vias);
    cpy(k.ifrom, sizeof k.ifrom, fr); cpy(k.ito, sizeof k.ito, to); cpy(k.icseq, sizeof k.icseq, cs);
    k.to = *from;
    peer_of(fr, k.peer, sizeof k.peer);
    int code = 0; const char *reason = NULL;
    if (!enabled || !have_account()) { code = 480; reason = "Temporarily Unavailable"; }
    else if (no) { code = 480; reason = "Temporarily Unavailable"; }
    else if (c.state || call_claim(CALL_PHONE)) { code = 486; reason = "Busy Here"; }
    else if (!m->blen || sdp_pick(m->body, &k.codec, &k.rtp) < 0) { code = 488; reason = "Not Acceptable Here"; call_release(CALL_PHONE); }
    if (code) {
        fprintf(stderr, "phone: call from %s refused: %d %s%s%s\n", k.peer, code, reason, no ? ", " : "", no ? no : "");
        z = k; z.since = now;
        z.resp_n = response(z.resp, sizeof z.resp, code, reason, &z, NULL);
        send_to(from, z.resp, z.resp_n);
        z.resp_iv = T1; z.resp_next = now + T1; z.resp_until = now + TX_MS;
        return;
    }
    k.pt = sdp_pick(m->body, &k.codec, &k.rtp);
    snprintf(k.offer, sizeof k.offer, "%.*s", (int)m->blen, m->body);
    snprintf(k.lhdr, sizeof k.lhdr, "%s;tag=%s", to, k.ltag); cpy(k.rhdr, sizeof k.rhdr, fr);
    { const char *ct = hget(m, "Contact"); if (ct) uri_of(ct, k.target, sizeof k.target); }
    route_set(m, &k, 0);
    k.lcseq = 1; k.state = PHONE_RINGING; k.since = now; k.gen++;
    c = k;
    respond_invite(100, "Trying", NULL);
    respond_invite(180, "Ringing", NULL);
    fprintf(stderr, "phone: %s calls, ringing\n", c.peer);
    ev_push(PHONE_RINGING, c.peer, 0, -1, 0);
}

static void answer(void)
{
    char body[1200];
    sdp(body, sizeof body, c.pt, c.codec);
    respond_invite(200, "OK", body);
    c.ok_sent = 1;
    go_live(now_ms());
}

static void on_ack(const struct msg *m)
{
    const char *cid = hget(m, "Call-ID");
    if (!cid) return;
    if (c.call_id[0] && !strcmp(c.call_id, cid)) { c.acked = 1; c.resp_until = 0; }
    if (z.call_id[0] && !strcmp(z.call_id, cid)) { z.acked = 1; z.resp_until = 0; }
}

static void on_cancel(const struct msg *m, const struct sockaddr_in *from)
{
    char branch[80] = ""; const char *cid = hget(m, "Call-ID");
    param(hget(m, "Via"), "branch", branch, sizeof branch);
    if (!cid || strcmp(c.call_id, cid) || strcmp(c.ibranch, branch) || c.state != PHONE_RINGING) {
        reply(m, from, cid && z.call_id[0] && !strcmp(z.call_id, cid) ? 200 : 481, cid && z.call_id[0] && !strcmp(z.call_id, cid) ? "OK" : "Call/Transaction Does Not Exist", NULL);
        return;
    }
    reply(m, from, 200, "OK", NULL);
    respond_invite(487, "Request Terminated", NULL);
    end_call("the caller hung up", -1);
}

static void on_bye(const struct msg *m, const struct sockaddr_in *from)
{
    const char *cid = hget(m, "Call-ID");
    if (!cid || strcmp(c.call_id, cid) || c.state == PHONE_IDLE) { reply(m, from, cid && !strcmp(z.call_id, cid) ? 200 : 481, cid && !strcmp(z.call_id, cid) ? "OK" : "Call/Transaction Does Not Exist", NULL); return; }
    reply(m, from, 200, "OK", NULL);
    char why[160]; snprintf(why, sizeof why, "%s hung up", c.peer);
    end_call(why, PHONE_R_FAILED);
}

/* ---- a call out */

static void invite_send(long long now, const char *auth)
{
    char extra[1600], ct[200], body[1200];
    contact(ct, sizeof ct);
    snprintf(extra, sizeof extra, "%s%sSupported: replaces\r\n%s", ct, ALLOW, auth ? auth : "");
    sdp(body, sizeof body, -1, 0);
    c.icseq_n = ++c.lcseq;
    build(&c.inv, "INVITE", c.ruri, c.lhdr, c.ito0, c.call_id, c.icseq_n, NULL, NULL, 0, extra, "application/sdp", body);
    tx_start(&c.inv, now);
}

/* The ACK of a final non-2xx response: the INVITE's branch and request URI, the response's To (RFC 3261 17.1.1.3) */
static void ack_failure(const struct call *k, const struct msg *m)
{
    struct tx t; memset(&t, 0, sizeof t);
    cpy(t.branch, sizeof t.branch, k->inv.branch);
    struct out o = { t.buf, 0, sizeof t.buf };
    put(&o, "ACK %s SIP/2.0\r\nVia: SIP/2.0/UDP %s:%u;rport;branch=%s\r\nMax-Forwards: 70\r\nFrom: %s\r\nTo: %s\r\nCall-ID: %s\r\n"
        "CSeq: %u ACK\r\nContent-Length: 0\r\n\r\n", k->ruri, local_ip, sip_port, t.branch, k->lhdr, hget(m, "To") ? hget(m, "To") : k->ito0,
        k->call_id, k->icseq_n);
    send_to(&srv, t.buf, o.n);
}

static void cancel_send(const struct call *k)
{
    struct out o = { cancel_tx.buf, 0, sizeof cancel_tx.buf };
    cpy(cancel_tx.branch, sizeof cancel_tx.branch, k->inv.branch);
    put(&o, "CANCEL %s SIP/2.0\r\nVia: SIP/2.0/UDP %s:%u;rport;branch=%s\r\nMax-Forwards: 70\r\nFrom: %s\r\nTo: %s\r\nCall-ID: %s\r\n"
        "CSeq: %u CANCEL\r\nUser-Agent: hassmic/%s\r\nContent-Length: 0\r\n\r\n", k->ruri, local_ip, sip_port, cancel_tx.branch, k->lhdr, k->ito0,
        k->call_id, k->icseq_n, VERSION);
    cancel_tx.n = o.n;
    tx_start(&cancel_tx, now_ms());
}

static int result_of(int code)
{
    switch (code) {
    case 486: case 600: return PHONE_R_BUSY;
    case 603: case 403: return PHONE_R_DECLINED;
    case 404: case 484: case 604: return PHONE_R_NOT_FOUND;
    case 408: case 480: return PHONE_R_UNAVAILABLE;
    case 487: return PHONE_R_NOT_ANSWERED;
    default: return PHONE_R_FAILED;
    }
}

/* A response to the INVITE of k (c, or z: a call that ended here) */
static void on_invite_response(struct call *k, const struct msg *m, long long now)
{
    int live = k == &c;
    if (m->code < 200) {
        k->inv.answered = 1;
        if (k->cancel && !k->cancel_sent) { k->cancel_sent = 1; cancel_send(k); }
        if (live && m->code >= 180 && !k->rang) { k->rang = 1; k->since = now; fprintf(stderr, "phone: %s rings\n", k->peer); ev_push(-1, k->peer, 1, PHONE_R_RINGING, 0); }
        return;
    }
    if (m->code >= 300) {
        k->inv.on = 0;
        ack_failure(k, m);
        if (live && (m->code == 401 || m->code == 407) && k->auth_tries < 1 && account.pass[0]) {
            char auth[900];
            digest(hget(m, m->code == 401 ? "WWW-Authenticate" : "Proxy-Authenticate"), m->code == 407, "INVITE", k->ruri, auth, sizeof auth);
            if (auth[0]) { k->auth_tries++; invite_send(now, auth); memset(auth, 0, sizeof auth); return; }
        }
        if (live) { char why[100]; snprintf(why, sizeof why, "%d %s", m->code, m->reason); clean(why); end_call(why, result_of(m->code)); }
        return;
    }
    /* 2xx: the dialog */
    k->inv.on = 0;
    if (k->ack_n) { send_to(&k->to, k->ack, k->ack_n); return; }                /* a 2xx again: our ACK again */
    { const char *to = hget(m, "To"); if (to) cpy(k->rhdr, sizeof k->rhdr, to); }
    { const char *ct = hget(m, "Contact"); if (ct) uri_of(ct, k->target, sizeof k->target); }
    route_set(m, k, 1);
    struct tx a; memset(&a, 0, sizeof a);
    build(&a, "ACK", k->target[0] ? k->target : k->ruri, k->lhdr, k->rhdr, k->call_id, k->icseq_n, "ACK", k->route, k->nroute, NULL, NULL, NULL);
    memcpy(k->ack, a.buf, a.n); k->ack_n = a.n;
    send_to(&k->to, k->ack, k->ack_n);
    if (!live || k->cancel) {                       /* answered after we hung up: over at once */
        struct call keep = c; c = *k;
        send_bye("hung up before the answer came");
        *k = c; c = keep;
        return;
    }
    int codec; struct sockaddr_in rtp; int pt = m->blen ? sdp_pick(m->body, &codec, &rtp) : -1;
    if (pt < 0) { send_bye("no codec in common"); end_call("no codec in common in the answer", PHONE_R_FAILED); return; }
    k->pt = pt; k->codec = codec; k->rtp = rtp;
    go_live(now);
}

/* ---------------------------------------------------------------- the media (RTP) */

static void rtp_send(const uint8_t *frame, size_t len, uint32_t seq, void *arg)
{
    uint8_t pkt[12 + CALL_PKT]; struct sockaddr_in to;
    (void)seq; (void)arg;
    if (len > CALL_PKT) return;
    pthread_mutex_lock(&sl);
    if (c.state != PHONE_LIVE || !c.rtp.sin_port) { pthread_mutex_unlock(&sl); return; }
    pkt[0] = 0x80; pkt[1] = (uint8_t)(c.pt | (c.first_tx ? 0x80 : 0));
    pkt[2] = (uint8_t)(c.tx_seq >> 8); pkt[3] = (uint8_t)c.tx_seq;
    for (int i = 0; i < 4; i++) { pkt[4 + i] = (uint8_t)(c.tx_ts >> (24 - 8 * i)); pkt[8 + i] = (uint8_t)(c.ssrc >> (24 - 8 * i)); }
    c.tx_seq++; c.tx_ts += c.codec == CODEC_OPUS ? 960 : 160; c.first_tx = 0;
    to = c.rtp;
    pthread_mutex_unlock(&sl);
    memcpy(pkt + 12, frame, len);
    if (sendto(rsock, pkt, 12 + len, 0, (struct sockaddr *)&to, sizeof to) < 0 && errno != EAGAIN) {
        static long long logged;
        if (now_ms() - logged > 5000) { logged = now_ms(); fprintf(stderr, "phone: RTP send: %s\n", strerror(errno)); }
    }
}

static void on_rtp(const uint8_t *p, size_t n, const struct sockaddr_in *from)
{
    size_t off = 12; int media; uint32_t ext;
    if (n < 12 || (p[0] >> 6) != 2) return;
    off += (size_t)(p[0] & 0x0f) * 4;
    if (p[0] & 0x10) { if (n < off + 4) return; off += 4 + (size_t)(p[off + 2] << 8 | p[off + 3]) * 4; }
    if (p[0] & 0x20) { if (!n || p[n - 1] > n) return; n -= p[n - 1]; }
    if (n <= off) return;
    uint16_t seq = (uint16_t)(p[2] << 8 | p[3]);
    pthread_mutex_lock(&sl);
    if (c.state != PHONE_LIVE || (p[1] & 0x7f) != c.pt || from->sin_addr.s_addr != c.rtp.sin_addr.s_addr) { pthread_mutex_unlock(&sl); return; }
    c.rtp.sin_port = from->sin_port;                    /* symmetric: where its packets come from */
    /* RTP's 16 bits widened: call.c's jitter buffer counts on 32 */
    if (!c.rx_any) { c.rx_any = 1; c.rx_last = seq; c.rx_ext = ext = seq; }
    else { int16_t d = (int16_t)(seq - c.rx_last); ext = c.rx_ext + (uint32_t)(int32_t)d; if (d > 0) { c.rx_last = seq; c.rx_ext = ext; } }
    c.rx_at = now_ms(); media = c.media;
    pthread_mutex_unlock(&sl);
    call_rx(media, ext, p + off, n - off);
}

/* ---------------------------------------------------------------- the loop */

static void resolve(void)                       /* not sl: getaddrinfo may take a while */
{
    char host[128]; int port = 5060; struct addrinfo hints, *res = NULL; struct sockaddr_in a;
    pthread_mutex_lock(&sl);
    cpy(host, sizeof host, account.server);
    pthread_mutex_unlock(&sl);
    char *colon = strchr(host, ':'); if (colon) { *colon = 0; port = atoi(colon + 1); }
    memset(&a, 0, sizeof a);
    memset(&hints, 0, sizeof hints); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    if (host[0] && !getaddrinfo(host, NULL, &hints, &res) && res) { a = *(struct sockaddr_in *)res->ai_addr; a.sin_port = htons((uint16_t)port); }
    if (res) freeaddrinfo(res);
    pthread_mutex_lock(&sl);
    if (!strncmp(account.server, host, strlen(host))) {           /* the account did not change meanwhile */
        if (!a.sin_port && host[0] && reg.line != LINE_UNREACHABLE) set_line(LINE_UNREACHABLE, "cannot find %s", host);
        srv = a;
    }
    srv_at = now_ms();
    pthread_mutex_unlock(&sl);
}

static int local_addr(char ip[16])              /* sl held: the address the server sees us at */
{
    struct sockaddr_in me; socklen_t l = sizeof me; int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0), r = -1;
    if (s < 0 || !srv.sin_port) { if (s >= 0) close(s); return -1; }
    if (!connect(s, (struct sockaddr *)&srv, sizeof srv) && !getsockname(s, (struct sockaddr *)&me, &l)) { inet_ntop(AF_INET, &me.sin_addr, ip, 16); r = 0; }
    close(s);
    return r;
}

static void tx_tick(struct tx *t, const struct sockaddr_in *to, long long now)
{
    if (!t->on) return;
    if (now >= t->until) { t->on = 0; return; }
    if (!t->answered && now >= t->next) { send_to(to, t->buf, t->n); t->iv = t->iv * 2 < T2 ? t->iv * 2 : T2; t->next = now + t->iv; }
}

static void resp_tick(struct call *k, long long now)      /* a final response to an INVITE, until its ACK */
{
    if (!k->resp_until || k->acked) return;
    if (now >= k->resp_until) {
        k->resp_until = 0;
        if (k == &c && c.state == PHONE_LIVE && !c.outgoing) { send_bye("no ACK"); end_call("no ACK for our answer", -1); }
        return;
    }
    if (now >= k->resp_next) { send_to(&k->to, k->resp, k->resp_n); k->resp_iv = k->resp_iv * 2 < T2 ? k->resp_iv * 2 : T2; k->resp_next = now + k->resp_iv; }
}

static void tick(void)
{
    long long now = now_ms();
    int need_resolve = 0;
    pthread_mutex_lock(&sl);
    if (!enabled || !have_account()) {
        if (reg.line != LINE_OFF) { unregister(); set_line(LINE_OFF, !have_account() ? "no account" : "the phone is off"); }
        reg.t.on = 0; reg.next = 0;
    } else {
        /* the server's name looked up (not under sl): every 10 s until found, then every 10 min */
        need_resolve = now - srv_at > (srv.sin_port ? 600000 : 10000);
        if (srv.sin_port && now - addr_at >= ADDR_MS) {
            char ip[16];
            addr_at = now;
            if (!local_addr(ip) && strcmp(ip, local_ip)) {
                if (strcmp(local_ip, "0.0.0.0")) fprintf(stderr, "phone: this Echo is at %s now, registering again\n", ip);
                cpy(local_ip, sizeof local_ip, ip); reg.next = now; reg.t.on = 0;
            }
        }
        if (reg.line == LINE_OFF) set_line(LINE_CONNECTING, "registering at %s as %s", account.server, account.user);
        if (srv.sin_port && strcmp(local_ip, "0.0.0.0") && !reg.t.on && now >= reg.next) { reg.auth_tries = 0; reg_send(now, NULL, reg.granted > EXPIRES ? reg.granted : EXPIRES); }
        if (reg.t.on && now >= reg.t.until) {
            reg.t.on = 0; reg.next = now + RETRY_MS;
            set_line(LINE_UNREACHABLE, "no answer from %s", account.server);
        }
        tx_tick(&reg.t, &srv, now);
    }
    /* the call */
    if (c.state == PHONE_CALLING && c.outgoing) {
        tx_tick(&c.inv, &srv, now);
        if (!c.inv.on && !c.inv.answered && !c.ack_n && now >= c.inv.until) end_call("no answer from the server", PHONE_R_FAILED);
        else if (now - c.since > RINGBACK_MS) { c.cancel = 1; c.cancel_sent = 1; cancel_send(&c); end_call("not answered", PHONE_R_NOT_ANSWERED); }
    }
    if (c.state == PHONE_RINGING && now - c.since > RING_MS) { respond_invite(480, "Temporarily Unavailable", NULL); end_call("not answered", -1); }
    if (c.state == PHONE_LIVE && now - c.rx_at > LOST_MS) { send_bye("no audio"); end_call("lost: no audio for 2 min", -1); }
    resp_tick(&c, now);
    resp_tick(&z, now);
    if (z.outgoing && z.inv.on) tx_tick(&z.inv, &srv, now);
    if (z.call_id[0] && now - z.since > TX_MS + 1000) reset_call(&z);
    tx_tick(&bye_tx, &srv, now); tx_tick(&cancel_tx, &srv, now);
    pthread_mutex_unlock(&sl);
    if (need_resolve) resolve();
}

static void tell_core(void)
{
    for (;;) {
        struct ev e;
        pthread_mutex_lock(&sl);
        if (ev_out == ev_in) { pthread_mutex_unlock(&sl); break; }
        e = evq[ev_out++ % EVQ];
        pthread_mutex_unlock(&sl);
        if (e.state >= 0) core_phone(e.state, e.peer, e.outgoing);
        if (e.result >= 0) core_phone_result(e.result, e.peer, e.here);
    }
    pthread_mutex_lock(&sl);
    int line = reg.line;
    pthread_mutex_unlock(&sl);
    if (atomic_exchange(&line_told, line) != line) core_phone_line(line);
}

static void on_sip(const uint8_t *buf, size_t n, const struct sockaddr_in *from)
{
    static struct msg m;                        /* the loop's own: 9 kB off the stack */
    if (n < 20 || parse(&m, buf, n)) return;
    const char *no = m.req && !strcmp(m.method, "INVITE") ? core_dropin_refusal(1) : NULL;   /* before sl: core_lock comes first */
    long long now = now_ms();
    pthread_mutex_lock(&sl);
    if (!m.req) {
        char branch[80] = "", cs[64] = ""; const char *v = hget(&m, "Via"), *cseq = hget(&m, "CSeq");
        param(v, "branch", branch, sizeof branch);
        if (cseq) sscanf(cseq, "%*u %63s", cs);
        if (reg.t.on && !strcmp(branch, reg.t.branch) && !strcmp(cs, "REGISTER")) on_register(&m, now);
        else if (!strcmp(cs, "INVITE") && c.outgoing && !strcmp(branch, c.inv.branch)) on_invite_response(&c, &m, now);
        else if (!strcmp(cs, "INVITE") && z.outgoing && !strcmp(branch, z.inv.branch)) on_invite_response(&z, &m, now);
        else if (!strcmp(cs, "INVITE") && c.outgoing && c.ack_n && hget(&m, "Call-ID") && !strcmp(hget(&m, "Call-ID"), c.call_id)) send_to(&c.to, c.ack, c.ack_n);
        else if (bye_tx.on && !strcmp(branch, bye_tx.branch) && m.code >= 200) bye_tx.on = 0;
        else if (cancel_tx.on && !strcmp(branch, cancel_tx.branch) && m.code >= 200) cancel_tx.on = 0;
        else if (m.code < 200 && bye_tx.on && !strcmp(branch, bye_tx.branch)) bye_tx.answered = 1;
        pthread_mutex_unlock(&sl);
        return;
    }
    /* requests: from the server only.  Anyone else on the LAN could ring the Echo, or end its calls */
    if (!srv.sin_port || from->sin_addr.s_addr != srv.sin_addr.s_addr) {
        char ip[16]; inet_ntop(AF_INET, &from->sin_addr, ip, sizeof ip);
        static long long logged;
        if (now - logged > 10000) { logged = now; fprintf(stderr, "phone: %s from %s refused: not the server\n", m.method, ip); }
        if (strcmp(m.method, "ACK")) reply(&m, from, 403, "Forbidden", NULL);
        pthread_mutex_unlock(&sl);
        return;
    }
    if (!strcmp(m.method, "INVITE")) on_invite(&m, from, no, now);
    else if (!strcmp(m.method, "ACK")) on_ack(&m);
    else if (!strcmp(m.method, "CANCEL")) on_cancel(&m, from);
    else if (!strcmp(m.method, "BYE")) on_bye(&m, from);
    else if (!strcmp(m.method, "OPTIONS")) reply(&m, from, 200, "OK", "Accept: application/sdp\r\n");
    else if (!strcmp(m.method, "NOTIFY")) reply(&m, from, 200, "OK", NULL);
    else reply(&m, from, 405, "Method Not Allowed", NULL);
    pthread_mutex_unlock(&sl);
}

static void *loop(void *arg)
{
    static uint8_t buf[SIP_MAX + 1], rbuf[1500];
    (void)arg;
    for (;;) {
        struct pollfd pf[2] = { { sock, POLLIN, 0 }, { rsock, POLLIN, 0 } };
        if (poll(pf, 2, 20) > 0) {
            struct sockaddr_in from; socklen_t fl = sizeof from; ssize_t n;
            if ((pf[0].revents & POLLIN) && (n = recvfrom(sock, buf, SIP_MAX, 0, (struct sockaddr *)&from, &fl)) > 0) on_sip(buf, (size_t)n, &from);
            fl = sizeof from;
            while ((pf[1].revents & POLLIN) && (n = recvfrom(rsock, rbuf, sizeof rbuf, MSG_DONTWAIT, (struct sockaddr *)&from, &fl)) > 0) { on_rtp(rbuf, (size_t)n, &from); fl = sizeof from; }
        }
        tick();
        if (atomic_exchange(&notify, 0)) tell_core();
    }
    return NULL;
}

/* ---------------------------------------------------------------- interface */

int sip_running(void) { return running; }

int sip_enable(int set)
{
    pthread_mutex_lock(&sl);
    if (set >= 0 && (set != 0) != enabled) {
        enabled = set != 0;
        if (!enabled && c.state) {
            if (c.state == PHONE_LIVE) send_bye("the phone switched off");
            else if (c.state == PHONE_RINGING) respond_invite(480, "Temporarily Unavailable", NULL);
            else if (c.inv.answered) { c.cancel = c.cancel_sent = 1; cancel_send(&c); } else c.cancel = 1;
            end_call("the phone switched off", PHONE_R_CANCELLED);
        }
        reg.next = 0;
    }
    int r = enabled;
    pthread_mutex_unlock(&sl);
    return r;
}

int sip_call(const char *number, char *err, size_t errsz)
{
    int r = -1;
    if (!running) { snprintf(err, errsz, "the phone is not running on this Echo"); return -1; }
    const char *no = core_dropin_refusal(0);
    pthread_mutex_lock(&sl);
    attempts++;
    if (!user_ok(number)) { snprintf(err, errsz, "not a number: digits, letters, * # + . _ - only"); r = PHONE_R_NOT_FOUND; }
    else if (!enabled || !have_account()) { snprintf(err, errsz, !enabled ? "the phone is off on this Echo" : "no phone account on this Echo"); r = PHONE_R_NO_LINE; }
    else if (!srv.sin_port || !strcmp(local_ip, "0.0.0.0")) { snprintf(err, errsz, "the phone server %s is not found", account.server); r = PHONE_R_NO_LINE; }
    else if (no) { snprintf(err, errsz, "%s", no); r = PHONE_R_MUTED_HERE; }
    else if (c.state) { snprintf(err, errsz, "already in a phone call with %s", c.peer); r = PHONE_R_BUSY_HERE; }
    else if (call_claim(CALL_PHONE)) { snprintf(err, errsz, "already in a Drop In"); r = PHONE_R_BUSY_HERE; }
    if (r >= 0) { ev_push(-1, number, 1, r, 1); pthread_mutex_unlock(&sl); return -1; }
    reset_call(&c);
    c.outgoing = 1; c.state = PHONE_CALLING; c.since = now_ms(); c.to = srv; c.gen++;
    cpy(c.peer, sizeof c.peer, number);
    { char u[3 * sizeof account.user]; size_t k = 0;               /* '#' is not for a URI's user part as it is: %23 */
      for (const char *p = number; *p && k + 4 < sizeof u; p++) k += (size_t)(*p == '#' ? snprintf(u + k, sizeof u - k, "%%23") : snprintf(u + k, sizeof u - k, "%c", *p));
      struct out o = { c.ruri, 0, sizeof c.ruri }; put(&o, "sip:%s@%s", u, account.server); }
    rand_hex(c.call_id, 16); strcat(c.call_id, "@hassmic"); rand_hex(c.ltag, 8);
    snprintf(c.lhdr, sizeof c.lhdr, "\"%s\" <sip:%s@%s>;tag=%s", core_name, account.user, account.server, c.ltag);
    snprintf(c.ito0, sizeof c.ito0, "<%s>", c.ruri);
    cpy(c.rhdr, sizeof c.rhdr, c.ito0);
    c.lcseq = 0;
    fprintf(stderr, "phone: calling %s\n", number);
    ev_push(PHONE_CALLING, c.peer, 1, -1, 0);
    invite_send(c.since, NULL);
    pthread_mutex_unlock(&sl);
    return 0;
}

void sip_hangup(const char *why)
{
    pthread_mutex_lock(&sl);
    switch (c.state) {
    case PHONE_RINGING: respond_invite(603, "Decline", NULL); end_call(why, -1); break;
    case PHONE_LIVE: send_bye(why); end_call(why, -1); break;
    case PHONE_CALLING:
        c.cancel = 1;
        if (c.inv.answered) { c.cancel_sent = 1; cancel_send(&c); }   /* else on the first provisional (on_invite_response) */
        end_call(why, PHONE_R_CANCELLED);
        break;
    }
    pthread_mutex_unlock(&sl);
}

int sip_answer(void)
{
    pthread_mutex_lock(&sl);
    int st = c.state;
    if (st == PHONE_RINGING) answer();
    pthread_mutex_unlock(&sl);
    return st == PHONE_RINGING;
}

int sip_button(void)
{
    pthread_mutex_lock(&sl);
    int st = c.state;
    pthread_mutex_unlock(&sl);
    if (st == PHONE_RINGING) { sip_answer(); return 1; }
    if (st) sip_hangup("action button");
    return st != PHONE_IDLE;
}

int sip_status(char *peer, size_t cap)
{
    pthread_mutex_lock(&sl);
    int st = c.state;
    if (peer && cap) snprintf(peer, cap, "%s", st ? c.peer : "");
    pthread_mutex_unlock(&sl);
    return st;
}

int sip_line(void)
{
    pthread_mutex_lock(&sl);
    int l = reg.line;
    pthread_mutex_unlock(&sl);
    return l;
}

int sip_last(unsigned *attempt)
{
    pthread_mutex_lock(&sl);
    int r = last_result;
    if (attempt) *attempt = attempts;
    pthread_mutex_unlock(&sl);
    return r;
}

int sip_account(const char *server, const char *user, const char *password, char *err, size_t errsz)
{
    if (server && *server && !server_ok(server)) { snprintf(err, errsz, "not a server: a host name or IP address, and :port if it is not 5060"); return -1; }
    if (user && *user && !user_ok(user)) { snprintf(err, errsz, "not a user: letters, digits and . _ + - * # only"); return -1; }
    if (password && !pass_ok(password)) { snprintf(err, errsz, "the password has a character a SIP password cannot have"); return -1; }
    pthread_mutex_lock(&sl);
    unregister();
    if (server) cpy(account.server, sizeof account.server, server);
    if (user) cpy(account.user, sizeof account.user, user);
    if (password) cpy(account.pass, sizeof account.pass, password);
    memset(&srv, 0, sizeof srv); srv_at = 0; addr_at = 0; snprintf(local_ip, sizeof local_ip, "0.0.0.0");
    reg.t.on = 0; reg.next = 0; reg.granted = 0; reg.auth_tries = 0;
    if (reg.line != LINE_OFF) set_line(have_account() ? LINE_CONNECTING : LINE_OFF, have_account() ? "registering at %s as %s" : "no account", account.server, account.user);
    int r = save();
    pthread_mutex_unlock(&sl);
    if (r) snprintf(err, errsz, "cannot save the account");
    fprintf(stderr, "phone: account %s%s%s%s\n", account.user[0] ? account.user : "(none)", account.server[0] ? " at " : "", account.server, account.pass[0] ? ", with a password" : "");
    return r;
}

size_t sip_status_json(char *o, size_t cap)
{
    unsigned at; int res = sip_last(&at);
    pthread_mutex_lock(&sl);
    char text[128]; cpy(text, sizeof text, reg.text); clean(text);
    int n = snprintf(o, cap, "{\"on\":%s,\"line\":\"%s\",\"text\":\"%s\",\"state\":\"%s\",\"peer\":\"%s\",\"server\":\"%s\",\"user\":\"%s\","
                     "\"password\":%s,\"result\":\"%s\",\"attempt\":%u}", enabled ? "true" : "false", phone_lines[reg.line], text,
                     phone_states[c.state], c.state ? c.peer : "", account.server, account.user, account.pass[0] ? "true" : "false",
                     res >= 0 ? phone_results[res] : "", at);
    pthread_mutex_unlock(&sl);
    return n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap - 1;
}

int sip_start(int port)
{
    struct sockaddr_in a; pthread_t t; int tos = 0xb8;      /* DSCP EF: Wi-Fi's voice queue (WMM) where the AP honours it */
    if (port <= 0 || port > 65534) return -1;
    reset_call(&c); reset_call(&z);
    load();
    rand_hex(reg.call_id, 12); strcat(reg.call_id, "@hassmic"); rand_hex(reg.tag, 8); reg.cseq = 0;
    for (int i = 0; i < 2; i++) {
        int *s = i ? &rsock : &sock;
        if ((*s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)) < 0) { perror("phone: socket"); return -1; }
        memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons((uint16_t)(port + i)); a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(*s, (struct sockaddr *)&a, sizeof a)) { perror("phone: bind"); close(*s); *s = -1; if (i) { close(sock); sock = -1; } return -1; }
        setsockopt(*s, IPPROTO_IP, IP_TOS, &tos, sizeof tos);
    }
    sip_port = (uint16_t)port; rtp_port = (uint16_t)(port + 1);
    if (call_start()) { fprintf(stderr, "phone: no playout thread\n"); return -1; }
    running = 1;
    if (pthread_create(&t, NULL, loop, NULL)) { running = 0; return -1; }
    pthread_detach(t);
    fprintf(stderr, "phone: SIP on UDP %d, RTP on %d%s%s\n", port, port + 1, have_account() ? ", account " : ", no account yet", have_account() ? account.user : "");
    return 0;
}
