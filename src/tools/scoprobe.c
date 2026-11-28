/* SCO and eSCO over HCI on MediaTek's /dev/stpbt (issue #233: can an Echo be a phone's hands-free speakerphone?).
 * The sound cards wire no PCM link to the Bluetooth chip (cannoli's device tree: ETDM2 out to the amplifier, ETDM1 in
 * from the microphones' ADC, nothing else), so call audio can only come and go as HCI synchronous data (H4 type 3).
 * Two Echos stand in for Echo and phone: one pages the other, sets up a synchronous link and both send a known signal
 * over it; each counts what arrives.  Transparent air mode (mSBC's) carries bytes as sent: a counting pattern shows
 * loss and damage byte for byte.  CVSD (the chip codes it) carries a 1 kHz tone: what arrives is measured for it.
 * Run with hassmic stopped (`stop hassmic`: init's service, main.sh and hassmic with it; `start hassmic` after): the
 * device opens beside another user and both would lose packets.
 *   scoprobe info                                   what the controller says it can do, and tries the commands
 *   scoprobe listen [secs] [air]                     page scan; accepts the ACL and the synchronous link
 *   scoprobe connect <bdaddr> [secs] [air] [link]    ACL to bdaddr, then the synchronous link
 *   air: cvsd (default) | transp | msbc     link: d1 (SCO HV3) | s1 | s3 (eSCO, CVSD's) | t1 | t2 (eSCO, mSBC's)
 *   msbc: the controller codes mSBC on the air from 16 kHz linear audio (Android's T2 parameters, esco_parameters.cc):
 *   with -E <path> only, the audio on that path (1: the PCM pins); nothing is sent or counted over HCI then
 *   options first: -o <file> (what arrives), -l <bytes> (HCI packet), -n (no synchronous flow control), -q (quiet),
 *   -R (no HCI reset: btmtksdio sends its PCM settings, 0xfc72 and the pin mux, when it sees one complete),
 *   -E <path> (Enhanced Setup/Accept Synchronous Connection, whose data path fields name where the audio goes:
 *   0 = HCI, 1-0xfe a vendor's, e.g. the PCM pins), -U <n> (its transport unit size), -x <opcode>:<hex bytes> (a vendor command after the reset, in order; repeatable) */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int fd, quiet, no_reset, enhanced = -1, tunit = -1;   /* -U <n>: the transport unit size (enhanced) */   /* -E <path>: enhanced setup/accept with that data path, 0 = HCI */
static FILE *out;
static uint8_t in[4096]; static size_t have;
static const char *xcmd[8]; static int nx;

static long long now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000000 + t.tv_nsec / 1000; }
static unsigned u16(const uint8_t *p) { return p[0] | p[1] << 8; }
static void dump(const char *tag, const uint8_t *b, int n)
{
    printf("%s", tag);
    for (int i = 0; i < n && i < 48; i++) printf(" %02x", b[i]);
    printf("%s\n", n > 48 ? " ..." : "");
}
static void put(const uint8_t *b, int n) { if (write(fd, b, n) != n) printf("write: %s\n", strerror(errno)); }
static void send_cmd(unsigned op, const void *p, int n)
{
    uint8_t b[4 + 255] = { 1, op & 0xff, op >> 8, n };
    memcpy(b + 4, p, n); put(b, 4 + n);
}

/* --- the link's state and counters --- */
static int acl = -1, sco = -1, air_transp, air_msbc, pktlen = 48, flow = 1, sco_buf_len, sco_buf_num, credits;
static uint8_t peer[6];
static long long rx_pkts, rx_bytes, rx_good, rx_bad, rx_flag[4], rx_gap, tx_pkts, tx_done;
static int rx_next = -1; static uint8_t tx_ctr;
static double tone_re, tone_im, tone_cos = 1, tone_sin, pw_all, pw_n; static long tone_k;
static uint8_t cc_ret[300]; static int cc_len; static unsigned cc_op; static int cc_status = -2;

static void sync_complete(const uint8_t *p)
{
    printf("sync connection complete: status 0x%02x handle 0x%03x link %s interval %u retx window %u rx len %u tx len %u air %u\n",
           p[0], u16(p + 1) & 0xfff, p[9] == 2 ? "eSCO" : p[9] == 0 ? "SCO" : "?", p[10], p[11], u16(p + 12), u16(p + 14), p[16]);
    if (!p[0]) sco = u16(p + 1) & 0xfff;
}

/* the 57 bytes after the handle (setup) or the address (accept): Linux's hci_enhanced_setup_sync for HCI's data path */
static int enh_params(uint8_t *p, unsigned lat, unsigned pkt, uint8_t retx)
{
    uint8_t *q = p;
#define U16(v) do { unsigned _v = (v); *q++ = _v; *q++ = _v >> 8; } while (0)
#define U32(v) do { uint32_t _v = (v); memcpy(q, &_v, 4); q += 4; } while (0)
#define FMT(id) do { *q++ = (id); U16(0); U16(0); } while (0)
    U32(8000); U32(8000);                                                  /* air: 64 kb/s each way */
    int af = air_msbc ? 0x05 : air_transp ? 0x03 : 0x02;                     /* mSBC | transparent | CVSD on the air */
    FMT(af); FMT(af);
    U16(60); U16(60);                                                        /* codec frame */
    unsigned hb = air_msbc ? 32000 : air_transp ? 8000 : 16000;              /* host side: 16 kHz s16 | 64 kb/s | 8 kHz s16 */
    U32(hb); U32(hb);
    FMT(air_transp ? 0x03 : 0x04); FMT(air_transp ? 0x03 : 0x04);           /* transparent | linear PCM */
    U16(air_transp ? 8 : 16); U16(air_transp ? 8 : 16);
    *q++ = air_transp ? 0 : 2; *q++ = air_transp ? 0 : 2;                   /* 2's complement */
    *q++ = 0; *q++ = 0;                                                      /* MSB position */
    *q++ = enhanced; *q++ = enhanced;                                        /* data path */
    int tu = tunit >= 0 ? tunit : enhanced ? 0 : air_transp ? 1 : 16;        /* (a path that is not HCI: not packet based) */
    *q++ = tu; *q++ = tu;                                                    /* transport unit */
    U16(lat); U16(pkt); *q++ = retx;
#undef U16
#undef U32
#undef FMT
    return q - p;
}

static void accept_sync(const uint8_t *addr)
{
    if (enhanced >= 0) {
        uint8_t a[6 + 57]; memcpy(a, addr, 6);
        int n = air_msbc ? enh_params(a + 6, 13, 0x0380, 2) : enh_params(a + 6, 0xffff, 0x003f, 0xff);   /* mSBC: T2's own */
        send_cmd(0x043e, a, 6 + n);
        return;
    }
    uint8_t a[21]; memcpy(a, addr, 6);
    uint32_t bw = 8000; memcpy(a + 6, &bw, 4); memcpy(a + 10, &bw, 4);
    a[14] = 0xff; a[15] = 0xff;                                           /* max latency: don't care */
    unsigned vs = air_transp ? 0x0003 : 0x0060; a[16] = vs; a[17] = vs >> 8;
    a[18] = 0xff;                                                          /* retransmission: don't care */
    a[19] = 0x3f; a[20] = 0x00;                                            /* HV1-3, EV3-5, every EDR type */
    send_cmd(0x0429, a, sizeof a);
}

static void event(const uint8_t *p, int n)
{
    uint8_t ev = p[0]; const uint8_t *d = p + 2;
    switch (ev) {
    case 0x0e: if (u16(d + 1) == cc_op) { cc_len = n - 5; memcpy(cc_ret, d + 3, cc_len); cc_status = d[3]; } return;
    case 0x0f: if (u16(d + 2) == cc_op) { cc_len = 0; cc_status = d[0]; }
               else if (d[0]) printf("command 0x%04x: status 0x%02x\n", u16(d + 2), d[0]);
               return;
    case 0x13:                                                               /* Number Of Completed Packets */
        for (int i = 0; i < d[0]; i++) if ((int)(u16(d + 1 + 4 * i) & 0xfff) == sco) { tx_done += u16(d + 3 + 4 * i); credits += u16(d + 3 + 4 * i); }
        return;
    case 0x03:
        printf("connection complete: status 0x%02x handle 0x%03x link %u\n", d[0], u16(d + 1) & 0xfff, d[9]);
        if (!d[0]) acl = u16(d + 1) & 0xfff;
        return;
    case 0x04:
        printf("connection request from %02x:%02x:%02x:%02x:%02x:%02x link %u\n", d[5], d[4], d[3], d[2], d[1], d[0], d[9]);
        memcpy(peer, d, 6);
        if (d[9] == 1) { uint8_t a[7]; memcpy(a, d, 6); a[6] = 0x01; send_cmd(0x0409, a, 7); }
        else accept_sync(d);
        return;
    case 0x2c: sync_complete(d); return;
    case 0x2d: printf("sync connection changed: status 0x%02x handle 0x%03x interval %u window %u\n", d[0], u16(d + 1) & 0xfff, d[3], d[4]); return;
    case 0x05:
        printf("disconnected: handle 0x%03x reason 0x%02x\n", u16(d + 1) & 0xfff, d[3]);
        if ((int)(u16(d + 1) & 0xfff) == sco) sco = -1;
        if ((int)(u16(d + 1) & 0xfff) == acl) acl = -1;
        return;
    case 0x10: printf("HARDWARE ERROR 0x%02x\n", d[0]); return;
    case 0x1b: return;                                                       /* Max Slots Change */
    }
    if (!quiet) dump("event", p, n);
}

static void sco_rx(const uint8_t *p, int n)
{
    unsigned h = u16(p), len = p[2]; const uint8_t *b = p + 3;
    if ((int)len > n - 3) return;
    rx_pkts++; rx_bytes += len; rx_flag[h >> 12 & 3]++;
    if (out) fwrite(b, 1, len, out);
    if (air_transp) {                                                       /* counting bytes: whole, or where it broke */
        int ok = 1;
        for (unsigned i = 1; i < len; i++) if ((uint8_t)(b[i - 1] + 1) != b[i]) { ok = 0; break; }
        if (ok) rx_good++; else rx_bad++;
        if (len && rx_next >= 0 && b[0] != rx_next) rx_gap++;
        if (len) rx_next = (uint8_t)(b[len - 1] + 1);
    } else {                                                                 /* s16le at 8 kHz: the 1 kHz tone's share */
        for (unsigned i = 0; i + 1 < len; i += 2) {
            double x = (int16_t)(b[i] | b[i + 1] << 8) / 32768.0;
            double ph = 2 * M_PI * 1000 * tone_k++ / 8000.0;
            tone_re += x * cos(ph); tone_im += x * sin(ph); pw_all += x * x; pw_n++;
        }
    }
}

/* reads what is there (or waits up to ms for it) and hands out whole packets; H4 reads may split or join them */
static int pump(int ms)
{
    struct pollfd pf = { fd, POLLIN, 0 };
    if (poll(&pf, 1, ms) > 0) {
        ssize_t r = read(fd, in + have, sizeof in - have);
        if (r <= 0) { printf("read: %s\n", r ? strerror(errno) : "EOF"); return -1; }
        have += r;
    }
    for (;;) {
        size_t len;
        if (have < 1) break;
        if (in[0] == 4) { if (have < 3) break; len = 3 + in[2]; }
        else if (in[0] == 2) { if (have < 5) break; len = 5 + u16(in + 3); }
        else if (in[0] == 3) { if (have < 4) break; len = 4 + in[3]; }
        else { printf("stray byte 0x%02x\n", in[0]); memmove(in, in + 1, --have); continue; }
        if (len > sizeof in) { printf("packet of %zu bytes\n", len); have = 0; break; }
        if (have < len) break;
        uint8_t pkt[sizeof in]; memcpy(pkt, in, len); memmove(in, in + len, have - len); have -= len;
        if (pkt[0] == 4) event(pkt + 1, len - 1);
        else if (pkt[0] == 3) sco_rx(pkt + 1, len - 1);
        else if (!quiet) dump("acl", pkt + 1, len - 1);
    }
    return 0;
}

/* a command and its Command Complete (or failing Command Status); -1 on timeout */
static int cmd(unsigned op, const void *p, int n)
{
    cc_op = op; cc_status = -2; cc_len = 0;
    send_cmd(op, p, n);
    long long end = now_us() + 3000000;
    while (cc_status == -2 && now_us() < end) if (pump(100) < 0) return -1;
    cc_op = 0;
    if (cc_status == -2) { printf("command 0x%04x: no answer\n", op); return -1; }
    return cc_status;
}

static int reset(void)
{
    uint8_t b[1100];
    for (int i = 0; i < 50; i++) {                                         /* what an earlier user left */
        struct pollfd pf = { fd, POLLIN, 0 };
        if (poll(&pf, 1, 200) <= 0 || read(fd, b, sizeof b) <= 0) break;
    }
    have = 0;
    if (no_reset) printf("no HCI reset: the driver sets no audio\n");
    else {                                                                   /* (the Dot 2's core comes back slowly after hassmic) */
        int st = -1;
        for (int i = 0; i < 8 && (st = cmd(0x0c03, NULL, 0)); i++) usleep(1000000);
        if (st) { printf("reset failed\n"); return -1; }
    }
    if (cmd(0x1005, NULL, 0) == 0 && cc_len >= 8) { sco_buf_len = cc_ret[3]; sco_buf_num = u16(cc_ret + 6); }
    printf("buffers: ACL %u x %u, SCO %u x %u\n", u16(cc_ret + 4), u16(cc_ret + 1), sco_buf_num, sco_buf_len);
    if (cmd(0x1009, NULL, 0) == 0) printf("bdaddr %02x:%02x:%02x:%02x:%02x:%02x\n", cc_ret[6], cc_ret[5], cc_ret[4], cc_ret[3], cc_ret[2], cc_ret[1]);
    uint8_t mask[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x3f };
    cmd(0x0c01, mask, 8);
    if (flow) { uint8_t on = 1; int st = cmd(0x0c2f, &on, 1); printf("synchronous flow control on: status %d\n", st); if (st) flow = 0; }
    for (int i = 0; i < nx; i++) {
        unsigned op = strtoul(xcmd[i], NULL, 16); const char *h = strchr(xcmd[i], ':'); uint8_t b[255]; int n = 0;
        for (h = h ? h + 1 : ""; h[0] && h[1] && n < 255; h += 2) { unsigned x; if (sscanf(h, "%2x", &x) != 1) break; b[n++] = x; }
        int st = cmd(op, b, n);
        printf("vendor 0x%04x (%d bytes): status %d", op, n, st);
        for (int k = 1; st == 0 && k < cc_len; k++) printf(" %02x", cc_ret[k]);
        printf("\n");
    }
    unsigned vs = air_transp ? 0x0003 : 0x0060; uint8_t v[2] = { vs, vs >> 8 };
    printf("write voice setting 0x%04x: status %d\n", vs, cmd(0x0c26, v, 2));
    return 0;
}

static int info(void)
{
    if (reset()) return 1;
    if (cmd(0x1001, NULL, 0) == 0)
        printf("version: HCI %u rev 0x%04x, LMP %u subversion 0x%04x, manufacturer %u\n", cc_ret[1], u16(cc_ret + 2), cc_ret[4], u16(cc_ret + 7), u16(cc_ret + 5));
    if (cmd(0x1003, NULL, 0) == 0) {
        const uint8_t *f = cc_ret + 1;
        dump("features:", f, 8);
        static const struct { int byte, bit; const char *name; } fs[] = {
            { 1, 3, "SCO link" }, { 1, 4, "HV2" }, { 1, 5, "HV3" }, { 1, 6, "u-law" }, { 1, 7, "A-law" }, { 2, 0, "CVSD" },
            { 2, 3, "transparent synchronous data" }, { 3, 7, "eSCO (EV3)" }, { 4, 0, "EV4" }, { 4, 1, "EV5" },
            { 5, 5, "EDR eSCO 2 Mb/s" }, { 5, 6, "EDR eSCO 3 Mb/s" }, { 5, 7, "3-slot EDR eSCO" }, { 6, 0, "extended inquiry" },
            { 6, 3, "secure simple pairing" }, { 7, 2, "erroneous data reporting" } };
        for (size_t i = 0; i < sizeof fs / sizeof *fs; i++) printf("  %-32s %s\n", fs[i].name, f[fs[i].byte] >> fs[i].bit & 1 ? "yes" : "no");
    }
    if (cmd(0x1002, NULL, 0) == 0) {
        const uint8_t *c = cc_ret + 1;
        dump("commands:", c, 64);
        static const struct { int oct, bit; const char *name; } cs[] = {
            { 16, 3, "Setup Synchronous Connection" }, { 16, 4, "Accept Synchronous Connection" },
            { 29, 3, "Enhanced Setup Synchronous Connection" }, { 29, 4, "Enhanced Accept Synchronous Connection" },
            { 29, 5, "Read Local Supported Codecs" }, { 45, 2, "Read Local Supported Codecs v2" } };
        for (size_t i = 0; i < sizeof cs / sizeof *cs; i++) printf("  %-40s %s\n", cs[i].name, c[cs[i].oct] >> cs[i].bit & 1 ? "yes" : "no");
    }
    if (cmd(0x0c25, NULL, 0) == 0) printf("voice setting now 0x%04x\n", u16(cc_ret + 1));
    int st = cmd(0x0c2e, NULL, 0); printf("read synchronous flow control: status %d value %d\n", st, st ? -1 : cc_ret[1]);
    st = cmd(0x0c5a, NULL, 0); printf("read erroneous data reporting: status %d value %d\n", st, st ? -1 : cc_ret[1]);
    st = cmd(0x100b, NULL, 0); printf("read local supported codecs: status %d\n", st);
    if (!st) dump("  codecs:", cc_ret + 1, cc_len - 1);              /* 0x02 CVSD, 0x03 transparent, 0x05 mSBC */
    st = cmd(0x100d, NULL, 0); printf("read local supported codecs v2: status %d\n", st);
    if (!st) dump("  codecs:", cc_ret + 1, cc_len - 1);
    return 0;
}

static void tx_one(void)
{
    uint8_t b[4 + 255] = { 3, sco & 0xff, sco >> 8 & 0x0f, pktlen };
    if (air_transp) for (int i = 0; i < pktlen; i++) b[4 + i] = tx_ctr++;
    else {
        static long k;
        for (int i = 0; i + 1 < pktlen; i += 2) {
            int16_t s = (int16_t)(8192 * sin(2 * M_PI * 1000 * k++ / 8000.0));
            b[4 + i] = s & 0xff; b[5 + i] = (uint16_t)s >> 8;
        }
    }
    put(b, 4 + pktlen); tx_pkts++; credits--;
}

static void report(int sec)
{
    printf("%3ds rx %lld pkts %lld B", sec, rx_pkts, rx_bytes);
    if (rx_flag[1] | rx_flag[2] | rx_flag[3]) printf(" (flags: ok %lld, maybe %lld, none %lld, lost %lld)", rx_flag[0], rx_flag[1], rx_flag[2], rx_flag[3]);
    if (air_transp) printf(" pattern whole %lld broken %lld gaps %lld", rx_good, rx_bad, rx_gap);
    else if (pw_n) {
        double tone = 2 * (tone_re * tone_re + tone_im * tone_im) / pw_n / pw_n, all = pw_all / pw_n;
        printf(" level %.1f dBFS, 1 kHz %.0f %% of it", 10 * log10(all + 1e-12), 100 * tone / (all + 1e-12));
    }
    printf(" | tx %lld done %lld\n", tx_pkts, tx_done);
    tone_re = tone_im = pw_all = pw_n = 0;
    (void)tone_cos; (void)tone_sin;
}

/* the synchronous link is up (or is to come): send at the air's rate, count, report every second */
static void run(int secs)
{
    long long t0 = now_us(), end = t0 + secs * 1000000LL, start = -1, sent = 0, next = t0 + 1000000;
    int rate = air_transp ? 8000 : 16000, sec = 0;                 /* host bytes a second: 64 kb/s, or 8 kHz s16 */
    credits = sco_buf_num ? sco_buf_num : 8;
    while (now_us() < end) {
        if (pump(1) < 0) return;
        long long t = now_us();
        if (sco >= 0 && !air_msbc) {
            if (start < 0) { start = t; printf("sending %d-byte packets, %d B/s\n", pktlen, rate); }
            while (sent * 1000000 < (t - start) * (long long)rate && (!flow || credits > 0)) { tx_one(); sent += pktlen; }
            if (sent * 1000000 < (t - start - 200000) * (long long)rate) sent = (t - start) * (long long)rate / 1000000;  /* no backlog */
        }
        if (t >= next) { report(++sec); next += 1000000; }
        if (acl < 0 && start >= 0) break;
    }
}

static void hang_up(void)
{
    if (sco >= 0) { uint8_t d[3] = { sco & 0xff, sco >> 8, 0x13 }; send_cmd(0x0406, d, 3); }
    if (acl >= 0) { uint8_t d[3] = { acl & 0xff, acl >> 8, 0x13 }; send_cmd(0x0406, d, 3); }
    for (int i = 0; i < 20 && (acl >= 0 || sco >= 0); i++) pump(100);
}

static int parse_addr(const char *s, uint8_t *a)
{
    unsigned b[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) return -1;
    for (int i = 0; i < 6; i++) a[i] = b[5 - i];
    return 0;
}

int main(int argc, char **argv)
{
    int o;
    while ((o = getopt(argc, argv, "o:l:nqRx:E:U:")) != -1)
        switch (o) {
        case 'o': out = fopen(optarg, "wb"); break;
        case 'l': pktlen = atoi(optarg); break;
        case 'n': flow = 0; break;
        case 'R': no_reset = 1; break;
        case 'E': enhanced = strtoul(optarg, NULL, 0); break;
        case 'U': tunit = atoi(optarg); break;
        case 'q': quiet = 1; break;
        case 'x': if (nx < 8) xcmd[nx++] = optarg; break;
        default: return 2;
        }
    argc -= optind; argv += optind;
    if (argc < 1) { fprintf(stderr, "usage: scoprobe [-o file] [-l bytes] [-n] [-q] info | listen [secs] [air] | connect <bdaddr> [secs] [air] [link]\n"); return 2; }
    setvbuf(stdout, NULL, _IOLBF, 0);
    fd = open("/dev/stpbt", O_RDWR | O_NOCTTY);
    if (fd < 0) { printf("open: %s\n", strerror(errno)); return 1; }
    if (!strcmp(argv[0], "info")) return info();

    int con = !strcmp(argv[0], "connect"), a = con ? 2 : 1;
    if (con && (argc < 2 || parse_addr(argv[1], peer))) { printf("connect: which bdaddr?\n"); return 2; }
    int secs = argc > a ? atoi(argv[a]) : 20;
    air_transp = argc > a + 1 && !strcmp(argv[a + 1], "transp");
    air_msbc = argc > a + 1 && !strcmp(argv[a + 1], "msbc");
    if (air_msbc && enhanced < 0) { printf("msbc: only with -E <path> (the controller codes it)\n"); return 2; }
    const char *link = argc > a + 2 ? argv[a + 2] : air_transp || air_msbc ? "t2" : "s3";
    if (reset()) return 1;
    if (sco_buf_len && pktlen > sco_buf_len) pktlen = sco_buf_len;
    if (air_transp && pktlen == 48) pktlen = sco_buf_len && sco_buf_len < 60 ? sco_buf_len : 60;   /* mSBC's frame */

    if (!con) {
        uint8_t scan = 0x02; printf("page scan: status %d\n", cmd(0x0c1a, &scan, 1));
        run(secs); hang_up(); report(-1); return 0;
    }
    uint8_t c[13]; memcpy(c, peer, 6);
    c[6] = 0x18; c[7] = 0xcc; c[8] = 0x01; c[9] = 0; c[10] = 0; c[11] = 0; c[12] = 0x01;   /* DM/DH 1-5, R1, may switch roles */
    cmd(0x0405, c, 13);
    for (long long end = now_us() + 10000000; acl < 0 && now_us() < end;) if (pump(100) < 0) return 1;
    if (acl < 0) { printf("no ACL link\n"); return 1; }
    static const struct { const char *name; unsigned pkt, lat; uint8_t retx; } ls[] = {   /* Linux's sco_param tables */
        { "d1", 0x03c4, 0xffff, 0xff }, { "s1", 0x03c8, 7, 1 }, { "s3", 0x0380, 10, 1 }, { "t1", 0x03c8, 8, 2 }, { "t2", 0x0380, 13, 2 } };
    size_t i = 0;
    while (i < sizeof ls / sizeof *ls && strcmp(ls[i].name, link)) i++;
    if (i == sizeof ls / sizeof *ls) { printf("link %s?\n", link); hang_up(); return 2; }
    uint8_t s[17] = { acl & 0xff, acl >> 8 };
    uint32_t bw = 8000; memcpy(s + 2, &bw, 4); memcpy(s + 6, &bw, 4);
    s[10] = ls[i].lat; s[11] = ls[i].lat >> 8;
    unsigned vs = air_transp ? 0x0003 : 0x0060; s[12] = vs; s[13] = vs >> 8;
    s[14] = ls[i].retx; s[15] = ls[i].pkt; s[16] = ls[i].pkt >> 8;
    printf("setup synchronous connection %s (packets 0x%04x latency %u retx %u voice 0x%04x)\n", link, ls[i].pkt, ls[i].lat, ls[i].retx, vs);
    usleep(300000); pump(10);
    if (enhanced >= 0) {
        uint8_t e[2 + 57] = { acl & 0xff, acl >> 8 };
        int en = enh_params(e + 2, ls[i].lat, ls[i].pkt, ls[i].retx);
        printf("enhanced setup, data path %d: status %d\n", enhanced, cmd(0x043d, e, 2 + en));
    } else cmd(0x0428, s, 17);
    run(secs); hang_up(); report(-1);
    return 0;
}
