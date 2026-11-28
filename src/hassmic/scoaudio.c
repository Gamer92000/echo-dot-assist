/* A phone call's audio: see scoaudio.h and docs/re-hfp.md.  Every chip here CVSD-codes the link itself except the Dot
 * 2's integrated core, which hands out the coded bitstream; none moves it over HCI (measured: Number Of Completed
 * Packets never comes for SCO data written to /dev/stpbt, whatever the setup).
 *
 * pcm: what stock's HAL routes, found in its mixer_paths.xml (donut's "bt-sco" paths) or the kernel's DAPM routes
 *      (cannoli's PCM1), opened with ALSA's own ioctls (as spkgain.c sets controls: no libtinyalsa, whose struct
 *      pcm_config differs between firmwares) at 8 kHz s16.  The playback side runs from the start with silence when
 *      nothing is queued: cannoli's capture (UL5) carries nothing while its playback (DL6) stands still, and the BT
 *      chip's PCM clock is the SoC's.  Cannoli's UL5 gives two channels whatever is asked: the first is the call.
 * ebc: AudDrv_BTCVSD's character device (misc 10:59, media:media 0660: device.conf has the daemon in group media).
 *      ioctl magic 'C': 0xe0 a buffer (0 TX, 2 RX; 1, 3 free them), 0xe1 the state (TX 0..4, RX 0x10..0x14: idle,
 *      init, ready, running, ending), in the HAL's order (AudioBTCVSDControl BT_SCO_RX_Begin/_End, TX_Begin/_End:
 *      the buffer, then running; ending, idle, then free).  read() gives 32-byte units, 30 bytes of the air and 2
 *      bytes 01 00 when the packet came (anything else: lost), and fails after the driver's timeouts while nothing
 *      comes; write() takes the bitstream and blocks at its rate (64 kbit/s).  The driver frees what a read or write
 *      in another thread is using: freed under a blocked write, its next look at the buffer was a NULL dereference
 *      and the kernel rebooted the Echo (2026-11-25).  So the device is closed by the last of its two threads to
 *      leave it, never under them, and a new call's audio waits for them.
 * hci: SCO data packets of s16le at 8 kHz (voice setting 0x0060), 48 bytes (3 ms) each, paced by the clock.
 * mSBC (wideband, HFP's codec 2): the link is transparent and every 60 bytes on it one H2-framed frame (msbc.c), 7.5 ms
 *      at 16 kHz: no resampling.  pcm (board.sco_msbc_chip): the chip codes it, from and to 16 kHz linear audio on the
 *      same PCM devices (opened at 16 kHz then); the link accepted with Enhanced Accept, data path 1.  ebc: the same 64 kbit/s as CVSD, read in the same 30-byte pieces (a lost one as 30
 *      zeros, so the frame it cut is a lost frame), written a packet at a time.  hci: one 60-byte packet each 7.5 ms. */
#include "scoaudio.h"
#include "audio.h"
#include "board.h"
#include "cvsd.h"
#include "msbc.h"
#include "hci.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sound/asound.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define RATE16   16000
#define QMAX     (RATE16 * SCO_QUEUE_MS / 1000)
#define PERIOD8  160                    /* 20 ms at 8 kHz */
#define PERIOD16 320                    /* 20 ms at 16 kHz (mSBC the chip codes) */

static int kind = -1;                   /* SCO_* as this run has it */
static void (*rx_cb)(const int16_t *, size_t);
static atomic_int running, threads;     /* the media runs; how many of its threads have not ended yet */
static unsigned hci_handle;
static int codec = SCO_CVSD;            /* this call's */

static int kind_now(void)
{
    if (kind < 0) {
        const char *e = getenv("HASSMIC_SCO");
        if (e && !strcmp(e, "hci")) kind = SCO_HCI;
        else if (e && !strcmp(e, "none")) kind = SCO_NONE;
        else kind = audio_sco_native() ? board.sco : SCO_NONE;     /* the PC's stand-in has none but over HCI */
    }
    return kind;
}

int sco_present(void) { return kind_now() != SCO_NONE; }
const char *sco_kind(void) { static const char *const n[] = { "none", "pcm", "ebc", "hci" }; return n[kind_now()]; }
int sco_wideband(void) { int k = kind_now(); return k == SCO_EBC || k == SCO_HCI || (k == SCO_PCM && board.sco_msbc_chip); }

/* Accepting the phone's link.  CVSD: what the chip has (voice setting 0x0060, any packet type, the phone's latency and
 * retransmission).  mSBC coded here: transparent data, eSCO EV3 or 2-EV3 (T1, T2).  mSBC the chip codes: Enhanced
 * Accept with mSBC on the air, 16 kHz s16 on data path 1 (the PCM pins), transport unit 16, T2's latency, packets and
 * retransmission: byte for byte what scoprobe's "msbc -U 16" set up and accepted on the Echo Dot 3, Dot 5 and Show 5
 * (2026-11-25).  The MT7668 takes the command with transport unit 0 and then sets up no link; 16 works on both chips. */
unsigned sco_accept_params(int c, int esco, unsigned char *p, int *n)
{
    unsigned char *q = p;
#define U16(v) do { unsigned _v = (v); *q++ = _v & 0xff; *q++ = _v >> 8; } while (0)
#define U32(v) do { uint32_t _v = (v); *q++ = _v; *q++ = _v >> 8; *q++ = _v >> 16; *q++ = _v >> 24; } while (0)
#define FMT(id) do { *q++ = (id); U16(0); U16(0); } while (0)
    /* (HASSMIC_SCO_CHIP_MSBC: the PC's tests push this accept through a2dp.c's queue on the HCI path) */
    if (c == SCO_MSBC && esco && ((kind_now() == SCO_PCM && board.sco_msbc_chip) || getenv("HASSMIC_SCO_CHIP_MSBC"))) {
        U32(8000); U32(8000);                                       /* the air: 64 kbit/s each way */
        FMT(0x05); FMT(0x05);                                       /* mSBC on the air */
        U16(60); U16(60);                                           /* its frame on the air: 60 bytes */
        U32(32000); U32(32000);                                     /* the PCM side: 16 kHz s16 */
        FMT(0x04); FMT(0x04);                                       /* linear PCM */
        U16(16); U16(16);
        *q++ = 2; *q++ = 2;                                         /* 2's complement */
        *q++ = 0; *q++ = 0;                                         /* MSB position */
        *q++ = 1; *q++ = 1;                                         /* data path: the PCM pins */
        *q++ = 16; *q++ = 16;                                       /* transport unit: 16 bits (0: no link on the MT7668) */
        U16(13); U16(0x0380); *q++ = 2;                             /* T2: latency, 2-EV3 only (as measured), retransmission */
        *n = (int)(q - p);
        return 0x043e;
    }
    U32(8000); U32(8000);
    U16(0xffff);                                                    /* latency: the phone's */
    if (c == SCO_MSBC && esco) { U16(0x0063); *q++ = 0xff; U16(0x0388); }    /* transparent; EV3, 2-EV3 */
    else { U16(0x0060); *q++ = 0xff; U16(0x003f); }                 /* CVSD, 16-bit linear; HV1-3, EV3-5, EDR too */
#undef U16
#undef U32
#undef FMT
    *n = (int)(q - p);
    return 0x0429;
}

int sco_chip_setup(void)
{
    const unsigned char *c = board.bt_sco_cmd;
    if (!c || kind_now() == SCO_HCI) return 0;
    int st = hci_cmd(c[0] | c[1] << 8, c + 3, c[2]);
    if (st < 0) return -1;
    if (st) fprintf(stderr, "phone: the chip refused its PCM setup (0x%02x)\n", st);
    return 0;
}

/* ---------------------------------------------------------------- the queue to the phone (16 kHz) */

static pthread_mutex_t qmu = PTHREAD_MUTEX_INITIALIZER;
static int16_t q[QMAX]; static size_t qn;

void sco_tx(const int16_t *pcm, size_t n)
{
    if (!atomic_load(&running)) return;
    pthread_mutex_lock(&qmu);
    if (n > QMAX) { pcm += n - QMAX; n = QMAX; }
    if (qn + n > QMAX) { size_t drop = qn + n - QMAX; memmove(q, q + drop, (qn - drop) * 2); qn -= drop; }
    memcpy(q + qn, pcm, n * 2); qn += n;
    pthread_mutex_unlock(&qmu);
}

/* n samples out of the queue, silence for what it lacks */
static void q_take(int16_t *out, size_t n)
{
    pthread_mutex_lock(&qmu);
    size_t k = qn < n ? qn : n;
    memcpy(out, q, k * 2); memset(out + k, 0, (n - k) * 2);
    memmove(q, q + k, (qn - k) * 2); qn -= k;
    pthread_mutex_unlock(&qmu);
}

static void give(const int16_t *pcm, size_t n) { void (*cb)(const int16_t *, size_t) = rx_cb; if (atomic_load(&running) && cb) cb(pcm, n); }

/* 8 kHz in -> 16 kHz to rx */
static void give8(struct fir *up, const int16_t *s8, size_t n)
{
    float f[2 * PERIOD8]; int16_t s16[2 * PERIOD8];
    for (size_t o = 0; o < n; o += PERIOD8) {
        size_t k = n - o < PERIOD8 ? n - o : PERIOD8, m = fir_up(up, s8 + o, k, f);
        for (size_t i = 0; i < m; i++) s16[i] = f[i] > 32767 ? 32767 : f[i] < -32768 ? -32768 : (int16_t)f[i];
        give(s16, m);
    }
}

/* n 8 kHz samples to the phone, out of the queue */
static void take8(struct fir *dn, int16_t *s8, size_t n)
{
    int16_t s16[2 * PERIOD8]; float f[2 * PERIOD8];
    q_take(s16, 2 * n);
    for (size_t i = 0; i < 2 * n; i++) f[i] = s16[i];
    fir_down(dn, f, 2 * n, s8);
}

static long long now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000000LL + t.tv_nsec; }

/* ---------------------------------------------------------------- pcm: ALSA */

static int ctl_set(const char *name, int v)
{
    int fd = open("/dev/snd/controlC0", O_RDWR | O_CLOEXEC), r = -1;
    if (fd < 0) return -1;
    struct snd_ctl_elem_list l; memset(&l, 0, sizeof l);
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &l) == 0 && l.count) {
        struct snd_ctl_elem_id *ids = calloc(l.count, sizeof *ids);
        if (ids) {
            l.space = l.count; l.pids = ids;
            if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &l) == 0)
                for (unsigned i = 0; i < l.used; i++) {
                    if (strcmp((const char *)ids[i].name, name)) continue;
                    struct snd_ctl_elem_info in; memset(&in, 0, sizeof in); in.id.numid = ids[i].numid;
                    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &in) < 0) break;
                    struct snd_ctl_elem_value val; memset(&val, 0, sizeof val); val.id.numid = ids[i].numid;
                    for (unsigned k = 0; k < in.count && k < 128; k++) val.value.integer.value[k] = v;
                    r = ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &val) < 0 ? -1 : 0;
                    break;
                }
            free(ids);
        }
    }
    close(fd);
    return r;
}

static void route(int on)
{
    for (const char *const *c = board.sco_route; c && *c; c++)
        if (ctl_set(*c, on)) fprintf(stderr, "phone: control \"%s\" not set\n", *c);
}

static void hw_mask(struct snd_pcm_hw_params *p, int param, unsigned bit)
{
    struct snd_mask *m = &p->masks[param - SNDRV_PCM_HW_PARAM_FIRST_MASK];
    memset(m, 0, sizeof *m); m->bits[bit >> 5] |= 1u << (bit & 31);
}

static void hw_int(struct snd_pcm_hw_params *p, int param, unsigned v)
{
    struct snd_interval *i = &p->intervals[param - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    i->min = i->max = v; i->integer = 1;
}

/* card 0's PCM device of that name ("00-10: VOIP_Call_BT_Playback ..." in /proc/asound/pcm), -1 none.  By name, as
 * stock's MediaTek HAL finds its devices: the numbers follow the machine driver's links, the names are the models'
 * own (checked against each kernel's link table and the four Echos measured) */
static int pcm_find(const char *name)
{
    FILE *f = fopen(getenv("HASSMIC_ASOUND_PCM") ? getenv("HASSMIC_ASOUND_PCM") : "/proc/asound/pcm", "r");
    char l[256]; int dev = -1; size_t n = strlen(name);
    while (f && dev < 0 && fgets(l, sizeof l, f)) {
        /* "CC-DD: <id> : <name> : ...", ASoC's id the stream's name and " (*)" (a front end) or " <codec dai>-<n>" */
        int c, d, at = 0;
        if (sscanf(l, "%d-%d: %n", &c, &d, &at) != 2 || !at || c != 0) continue;
        char *id = l + at, *end = strstr(id, " : ");
        if (end) *end = 0; else id[strcspn(id, "\n")] = 0;
        if (strncmp(id, name, n)) continue;
        const char *rest = id + n;
        if (!*rest || !strcmp(rest, " (*)") || (rest[0] == ' ' && rest[1] && !strchr(rest + 1, ' '))) dev = d;
    }
    if (f) fclose(f);
    if (dev < 0) fprintf(stderr, "phone: no PCM device \"%s\"\n", name);
    return dev;
}

/* card 0's device NAME, s16 at RATE, CH channels, 20 ms periods: the descriptor, or -1 */
static int pcm_open(const char *name, int capture, unsigned ch, unsigned rate)
{
    int dev = name ? pcm_find(name) : -1;
    if (dev < 0) return -1;
    char path[32]; snprintf(path, sizeof path, "/dev/snd/pcmC0D%d%c", dev, capture ? 'c' : 'p');
    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { fprintf(stderr, "phone: %s: %s\n", path, strerror(errno)); return -1; }
    struct snd_pcm_hw_params p; memset(&p, 0, sizeof p);
    for (int i = 0; i <= SNDRV_PCM_HW_PARAM_LAST_MASK - SNDRV_PCM_HW_PARAM_FIRST_MASK; i++) memset(&p.masks[i], 0xff, sizeof p.masks[i]);
    for (int i = 0; i <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; i++) { p.intervals[i].min = 0; p.intervals[i].max = UINT_MAX; }
    p.rmask = ~0u; p.info = ~0u;
    hw_mask(&p, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    hw_mask(&p, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
    hw_mask(&p, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
    hw_int(&p, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, 16); hw_int(&p, SNDRV_PCM_HW_PARAM_FRAME_BITS, 16 * ch);
    hw_int(&p, SNDRV_PCM_HW_PARAM_CHANNELS, ch); hw_int(&p, SNDRV_PCM_HW_PARAM_RATE, rate);
    hw_int(&p, SNDRV_PCM_HW_PARAM_PERIODS, 4);
    p.intervals[SNDRV_PCM_HW_PARAM_PERIOD_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min = rate / 50;
    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &p) < 0) { fprintf(stderr, "phone: %s: hw params: %s\n", path, strerror(errno)); close(fd); return -1; }
    unsigned period = p.intervals[SNDRV_PCM_HW_PARAM_PERIOD_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min,
             buffer = p.intervals[SNDRV_PCM_HW_PARAM_BUFFER_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min;
    struct snd_pcm_sw_params s; memset(&s, 0, sizeof s);
    s.tstamp_mode = SNDRV_PCM_TSTAMP_NONE; s.period_step = 1; s.avail_min = period;
    s.start_threshold = capture ? 1 : period * 2; s.stop_threshold = buffer;
    s.boundary = buffer; while (s.boundary * 2 <= INT_MAX - buffer) s.boundary *= 2;
    if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &s) < 0 || ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
        fprintf(stderr, "phone: %s: sw params: %s\n", path, strerror(errno)); close(fd); return -1;
    }
    if (capture) ioctl(fd, SNDRV_PCM_IOCTL_START);
    fprintf(stderr, "phone: %s open, %u ch, %u Hz, period %u, buffer %u\n", path, ch, rate, period, buffer);
    return fd;
}

/* frames of CH channels through the descriptor, waiting for it at most 100 ms a time while the media runs; -1 gone */
static int pcm_xfer(int fd, int capture, int16_t *buf, unsigned frames)
{
    while (atomic_load(&running)) {
        struct pollfd pf = { fd, capture ? POLLIN : POLLOUT, 0 };
        if (poll(&pf, 1, 100) <= 0) continue;
        struct snd_xferi x = { 0, buf, frames };
        if (ioctl(fd, capture ? SNDRV_PCM_IOCTL_READI_FRAMES : SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) == 0) return 0;
        if (errno == EAGAIN) continue;
        if (errno == EPIPE || errno == ESTRPIPE) {                  /* an xrun: on again */
            if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0) return -1;
            if (capture) ioctl(fd, SNDRV_PCM_IOCTL_START);
            continue;
        }
        fprintf(stderr, "phone: pcm %s: %s\n", capture ? "read" : "write", strerror(errno));
        return -1;
    }
    return -1;
}

/* 8 kHz (CVSD, the chip's) or, for mSBC the chip codes, 16 kHz as it is */
static void *pcm_play(void *arg)
{
    int fd = (int)(intptr_t)arg; unsigned ch = board.sco_play_ch > 0 ? board.sco_play_ch : 1;
    int wide = codec == SCO_MSBC; unsigned per = wide ? PERIOD16 : PERIOD8;
    struct fir dn; fir_init(&dn, 2, RATE16, 3600, 96);
    int16_t s[PERIOD16], fr[PERIOD16 * 2];
    while (atomic_load(&running)) {
        if (wide) q_take(s, per); else take8(&dn, s, per);
        for (unsigned i = 0; i < per; i++) for (unsigned c = 0; c < ch; c++) fr[i * ch + c] = s[i];
        if (pcm_xfer(fd, 0, fr, per) < 0) break;
    }
    ioctl(fd, SNDRV_PCM_IOCTL_DROP); close(fd);
    atomic_fetch_sub(&threads, 1);
    return NULL;
}

static void *pcm_rec(void *arg)
{
    int fd = (int)(intptr_t)arg; unsigned ch = board.sco_cap_ch > 0 ? board.sco_cap_ch : 1;
    int wide = codec == SCO_MSBC; unsigned per = wide ? PERIOD16 : PERIOD8;
    struct fir up; fir_init(&up, 2, RATE16, 3600, 96);
    int16_t fr[PERIOD16 * 2], s[PERIOD16];
    while (atomic_load(&running)) {
        if (pcm_xfer(fd, 1, fr, per) < 0) break;
        for (unsigned i = 0; i < per; i++) s[i] = fr[i * ch];
        if (wide) give(s, per); else give8(&up, s, per);
    }
    ioctl(fd, SNDRV_PCM_IOCTL_DROP); close(fd);
    atomic_fetch_sub(&threads, 1);
    return NULL;
}

/* ---------------------------------------------------------------- ebc: BTCVSD */

#define EBC_ALLOC _IOWR('C', 0xe0, unsigned int)
#define EBC_STATE _IOWR('C', 0xe1, unsigned int)
#define EBC_UNIT  32                    /* what read() hands out per packet: 30 bytes of the air, 2 bytes "valid" */
#define EBC_AIR   30
#define EBC_CHUNK 180                   /* what we write at a time: 22.5 ms, six packets */

static int ebc_fd = -1;
static atomic_int ebc_users;                    /* its threads still in it: the last one out closes it */
static void ebc_close(void);
static void ebc_leave(void) { if (atomic_fetch_sub(&ebc_users, 1) == 1) ebc_close(); }

static void *ebc_rec(void *arg)
{
    (void)arg;
    struct cvsd d; cvsd_init(&d);
    struct msbc_rx m; msbc_rx_init(&m);
    static const unsigned char none[EBC_AIR];
    unsigned char b[EBC_UNIT * 16]; int16_t pcm[2 * EBC_AIR] = { 0 }, wide[3 * MSBC_SAMPLES]; long lost = 0, got = 0;
    while (atomic_load(&running)) {
        ssize_t n = read(ebc_fd, b, sizeof b);
        if (n <= 0) { usleep(5000); continue; }                     /* the driver's timeout: nothing came (yet) */
        for (ssize_t o = 0; o + EBC_UNIT <= n; o += EBC_UNIT) {
            int ok = b[o + EBC_AIR] == 1;
            got += ok; lost += !ok;
            if (codec == SCO_MSBC) { size_t w = msbc_rx(&m, ok ? b + o : none, EBC_AIR, wide); if (w) give(wide, w); continue; }
            if (ok) cvsd_decode(&d, b + o, EBC_AIR, pcm);
            else for (int i = 0; i < 2 * EBC_AIR; i++) pcm[i] = pcm[i] / 2;      /* lost: the last, fading */
            give(pcm, 2 * EBC_AIR);
        }
    }
    if (lost) fprintf(stderr, "phone: %ld of %ld packets from the phone lost\n", lost, lost + got);
    if (codec == SCO_MSBC) fprintf(stderr, "phone: mSBC from the phone: %u frames, %u lost or broken\n", m.frames, m.lost);
    ebc_leave();
    atomic_fetch_sub(&threads, 1);
    return NULL;
}

static void *ebc_play(void *arg)
{
    (void)arg;
    struct cvsd e; cvsd_init(&e);
    struct msbc_tx m; msbc_tx_init(&m);
    int16_t pcm[2 * EBC_CHUNK]; unsigned char b[EBC_CHUNK];
    while (atomic_load(&running)) {
        size_t n = sizeof b;
        if (codec == SCO_MSBC) {                                    /* three packets, 22.5 ms, as CVSD's chunk */
            for (int i = 0; i < 3; i++) { q_take(pcm, MSBC_SAMPLES); msbc_tx(&m, pcm, b + i * MSBC_PACKET); }
        } else { q_take(pcm, 2 * EBC_CHUNK); cvsd_encode(&e, pcm, 2 * EBC_CHUNK, b); }
        if (write(ebc_fd, b, n) <= 0) usleep(5000);                 /* (the driver's timeout: the link went) */
    }
    ebc_leave();
    atomic_fetch_sub(&threads, 1);
    return NULL;
}

static int ebc_open(void)
{
    if ((ebc_fd = open("/dev/ebc", O_RDWR | O_CLOEXEC)) < 0) { fprintf(stderr, "phone: /dev/ebc: %s\n", strerror(errno)); return -1; }
    int rx = 0, tx = 0;
    if (ioctl(ebc_fd, EBC_ALLOC, 2) < 0 || !(rx = 1) || ioctl(ebc_fd, EBC_STATE, 0x13) < 0 ||
        ioctl(ebc_fd, EBC_ALLOC, 0) < 0 || !(tx = 1) || ioctl(ebc_fd, EBC_STATE, 0x03) < 0) goto fail;
    return 0;
fail:
    fprintf(stderr, "phone: /dev/ebc refused: %s\n", strerror(errno));
    if (rx) { ioctl(ebc_fd, EBC_STATE, 0x10); ioctl(ebc_fd, EBC_ALLOC, 3); }   /* what it took, given back: no thread uses it */
    if (tx) { ioctl(ebc_fd, EBC_STATE, 0x00); ioctl(ebc_fd, EBC_ALLOC, 1); }
    close(ebc_fd); ebc_fd = -1;
    return -1;
}

static void ebc_close(void)
{
    if (ebc_fd < 0) return;
    ioctl(ebc_fd, EBC_STATE, 0x14); ioctl(ebc_fd, EBC_STATE, 0x10); ioctl(ebc_fd, EBC_ALLOC, 3);
    ioctl(ebc_fd, EBC_STATE, 0x04); ioctl(ebc_fd, EBC_STATE, 0x00); ioctl(ebc_fd, EBC_ALLOC, 1);
    close(ebc_fd); ebc_fd = -1;
}

/* ---------------------------------------------------------------- hci */

#define HCI_SAMPLES 24                  /* 48 bytes, 3 ms */

static struct fir hci_up;
static struct msbc_rx hci_m;

void sco_hci_rx(const uint8_t *p, size_t n)
{
    if (n < 3 || kind_now() != SCO_HCI || !atomic_load(&running) || (unsigned)(p[0] | (p[1] & 0x0f) << 8) != hci_handle) return;
    size_t len = p[2] < n - 3 ? p[2] : n - 3;
    if (codec == SCO_MSBC) { int16_t w[(255 / MSBC_PACKET + 2) * MSBC_SAMPLES]; size_t k = msbc_rx(&hci_m, p + 3, len, w); if (k) give(w, k); return; }
    int16_t s8[128]; size_t k = len / 2 > 128 ? 128 : len / 2;
    for (size_t i = 0; i < k; i++) s8[i] = (int16_t)(p[3 + 2 * i] | p[4 + 2 * i] << 8);
    give8(&hci_up, s8, k);
}

static void *hci_play(void *arg)
{
    (void)arg;
    struct fir dn; fir_init(&dn, 2, RATE16, 3600, 96);
    struct msbc_tx m; msbc_tx_init(&m);
    long long next = now_ns();
    while (atomic_load(&running)) {
        int16_t s8[HCI_SAMPLES], s16[MSBC_SAMPLES]; unsigned char b[4 + MSBC_PACKET];
        size_t len;
        if (codec == SCO_MSBC) { q_take(s16, MSBC_SAMPLES); msbc_tx(&m, s16, b + 4); len = MSBC_PACKET; }
        else {
            take8(&dn, s8, HCI_SAMPLES); len = 2 * HCI_SAMPLES;
            for (int i = 0; i < HCI_SAMPLES; i++) { b[4 + 2 * i] = s8[i] & 0xff; b[5 + 2 * i] = (uint16_t)s8[i] >> 8; }
        }
        b[0] = 3; b[1] = hci_handle & 0xff; b[2] = hci_handle >> 8 & 0x0f; b[3] = (unsigned char)len;
        hci_write(b, 4 + len);
        next += codec == SCO_MSBC ? 7500000LL : HCI_SAMPLES * 125000LL;     /* 7.5 ms a frame, 3 ms of CVSD's */
        long long w = next - now_ns();
        if (w > 0) { struct timespec t = { w / 1000000000, w % 1000000000 }; nanosleep(&t, NULL); }
        else if (w < -100000000) next = now_ns();                   /* far behind (a stopped process): no burst */
    }
    atomic_fetch_sub(&threads, 1);
    return NULL;
}

/* ---------------------------------------------------------------- start, stop */

static int spawn(void *(*fn)(void *), void *arg)
{
    pthread_t t;
    atomic_fetch_add(&threads, 1);
    if (pthread_create(&t, NULL, fn, arg)) { atomic_fetch_sub(&threads, 1); return -1; }
    pthread_detach(t);
    return 0;
}

static void do_stop(void);
static int do_start(unsigned handle, int c, void (*rx)(const int16_t *, size_t))
{
    do_stop();
    for (int i = 0; i < 40 && atomic_load(&threads); i++) usleep(50000);   /* the last call's threads see it end */
    if (atomic_load(&threads)) { fprintf(stderr, "phone: the last call's audio threads are still running: no audio\n"); return -1; }
    rx_cb = rx; hci_handle = handle;
    if (c == SCO_MSBC && !sco_wideband()) { fprintf(stderr, "phone: mSBC asked for on a path that carries CVSD only: CVSD\n"); c = SCO_CVSD; }
    codec = c;
    pthread_mutex_lock(&qmu); qn = 0; pthread_mutex_unlock(&qmu);
    /* hci's receive state, before running: sco_hci_rx (the controller thread) reads it from then on */
    fir_init(&hci_up, 2, RATE16, 3600, 96); msbc_rx_init(&hci_m);
    atomic_store(&running, 1);
    int k = kind_now(), ok = 0;
    if (k == SCO_PCM) {
        route(1);
        unsigned rate = codec == SCO_MSBC ? 16000 : 8000;            /* mSBC: the chip codes it from 16 kHz */
        int p = pcm_open(board.sco_play, 0, board.sco_play_ch > 0 ? board.sco_play_ch : 1, rate);
        int c = p < 0 ? -1 : pcm_open(board.sco_cap, 1, board.sco_cap_ch > 0 ? board.sco_cap_ch : 1, rate);
        if (p >= 0 && c >= 0) {                                     /* a descriptor whose thread did not start: closed */
            int tp = !spawn(pcm_play, (void *)(intptr_t)p), tc = !spawn(pcm_rec, (void *)(intptr_t)c);
            if (!tp) close(p);
            if (!tc) close(c);
            ok = tp && tc;
        } else { if (p >= 0) close(p); if (c >= 0) close(c); }
    } else if (k == SCO_EBC && !ebc_open()) {
        atomic_store(&ebc_users, 2);                                /* the two threads; one not started leaves at once */
        if (spawn(ebc_rec, NULL)) ebc_leave(); else ok = 1;
        if (spawn(ebc_play, NULL)) { ebc_leave(); ok = 0; }
    } else if (k == SCO_HCI) {
        ok = !spawn(hci_play, NULL);
    }
    if (!ok) { fprintf(stderr, "phone: the call's audio did not start (%s)\n", sco_kind()); do_stop(); return -1; }
    fprintf(stderr, "phone: call audio runs (%s, %s)\n", sco_kind(), codec == SCO_MSBC ? "mSBC" : "CVSD");
    return 0;
}

static void do_stop(void)
{
    if (!atomic_exchange(&running, 0)) return;
    int k = kind_now();
    /* each thread ends at its next read or write (pcm: 100 ms at most, ebc: a packet, 3.75 ms, while the link is up) */
    for (int i = 0; i < 40 && atomic_load(&threads); i++) usleep(25000);
    if (atomic_load(&threads)) fprintf(stderr, "phone: the call's audio threads did not end within a second (ebc: the last closes it)\n");
    if (k == SCO_PCM) route(0);
    fprintf(stderr, "phone: call audio stopped\n");
}

/* Start and stop on a thread of their own: opening the devices and waiting for the last call's audio threads (a
 * second at the most, /dev/ebc's read in its timeout) held the Bluetooth controller's thread up, and with it every
 * link.  The newest wish wins: a start and a stop asked quickly after one another leave it stopped */
static pthread_mutex_t wm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wc = PTHREAD_COND_INITIALIZER;
static int w_req, w_on, w_codec; static unsigned w_handle; static void (*w_rx)(const int16_t *, size_t);

static void *worker(void *arg)
{
    (void)arg;
    int seen = 0;
    for (;;) {
        pthread_mutex_lock(&wm);
        while (w_req == seen) pthread_cond_wait(&wc, &wm);
        seen = w_req; int on = w_on, c = w_codec; unsigned h = w_handle; void (*rx)(const int16_t *, size_t) = w_rx;
        pthread_mutex_unlock(&wm);
        if (on) do_start(h, c, rx); else do_stop();
    }
    return NULL;
}

static void worker_start(void) { pthread_t t; if (!pthread_create(&t, NULL, worker, NULL)) pthread_detach(t); }

static void wish(int on, unsigned handle, int c, void (*rx)(const int16_t *, size_t))
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, worker_start);
    pthread_mutex_lock(&wm);
    w_on = on; w_handle = handle; w_codec = c; w_rx = rx; w_req++;
    pthread_cond_signal(&wc);
    pthread_mutex_unlock(&wm);
}

int  sco_start(unsigned handle, int c, void (*rx)(const int16_t *, size_t)) { wish(1, handle, c, rx); return 0; }
void sco_stop(void) { wish(0, 0, SCO_CVSD, NULL); }
