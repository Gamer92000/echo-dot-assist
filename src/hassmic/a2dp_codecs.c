/*
 * Codecs of the Bluetooth speaker, one stream endpoint each; the source picks the one it likes best.  Packet formats as
 * the sources send them (Android, PipeWire, Apple):
 *
 *   SBC       RTP, a byte with the frame count, whole SBC frames (sbc.c)
 *   AAC       RTP, one LATM AudioMuxElement with its configuration in band (muxConfigPresent = 1).  Sources size the
 *             bitrate so that a frame fits one packet.  Decoded by the firmware's own FFmpeg 4 (libavcodec's aac_latm),
 *             loaded at run time: its decoder wants the LOAS framing, a 3-byte sync header we put in front.  The
 *             structures are FFmpeg 4's ABI; that the library is FFmpeg 4 is checked before AAC is offered
 *   aptX      no RTP header at all: 4-byte aptX samples back to back (libfreeaptx, src/third_party)
 *   aptX HD   RTP, 6-byte samples
 *   Opus      Google's A2DP Opus (Android 13+, PipeWire's "opus_g"): RTP, the frame count byte, one Opus packet
 *             (libopus, the firmware's).  Not offered, see the list at the end.
 */
#include "a2dp_codec.h"
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include "sbc.h"
#include "../third_party/freeaptx.h"

typedef struct OpusDecoder OpusDecoder;
OpusDecoder *opus_decoder_create(int32_t Fs, int channels, int *error);
int opus_decode(OpusDecoder *st, const unsigned char *data, int32_t len, int16_t *pcm, int frame_size, int decode_fec);
void opus_decoder_destroy(OpusDecoder *st);

static int16_t pcm[8192];                               /* decoded audio of one packet at most, any codec */

/* RTP header length (with CSRCs and extension), 0 if it is not one */
static size_t rtp_len(const unsigned char *p, size_t n)
{
    if (n < 13 || p[0] >> 6 != 2) return 0;
    size_t off = 12 + 4 * (p[0] & 15);
    if (p[0] & 0x10) { if (off + 4 > n) return 0; off += 4 + 4 * (p[off + 2] << 8 | p[off + 3]); }
    return off < n ? off : 0;
}

static int one_bit(unsigned v) { return v && !(v & (v - 1)); }

/* ---------------------------------------------------------------- SBC */

static const unsigned char sbc_caps[] = { 0x30 | 0x0f, 0xf0 | 0x0c | 0x03, 2, 53 };  /* 44.1/48 kHz, all modes; bitpool 2..53 */
static struct sbc sbc;

static int always(void) { return 1; }
static int never(void) { return 0; }                /* an endpoint we keep but do not offer */

static int sbc_check(const unsigned char *c, unsigned n)
{
    return n >= 4 && (c[0] >> 4 == 1 || c[0] >> 4 == 2) && one_bit(c[0] & 15) && one_bit(c[1] >> 4) && one_bit(c[1] >> 2 & 3) &&
           one_bit(c[1] & 3) && c[2] >= 2 && c[2] <= c[3];
}

static int sbc_open(const unsigned char *c, unsigned n) { (void)c; (void)n; sbc_init(&sbc); return 0; }

static void sbc_packet(const unsigned char *p, size_t n, a2dp_pcm_fn *out)
{
    size_t off = rtp_len(p, n);
    if (!off || p[off] & 0x80) return;                  /* fragmented SBC frame: never with our MTU */
    unsigned frames = p[off] & 15; off++;
    for (unsigned f = 0; f < frames && off < n; f++) {
        unsigned got = 0; size_t len = sbc_decode(&sbc, p + off, n - off, pcm, &got);
        if (!len) break;
        out(pcm, got, sbc.channels, sbc.rate);
        off += len;
    }
}

static void nothing(void) {}

/* ---------------------------------------------------------------- AAC through the firmware's FFmpeg */

/* MPEG-2 and MPEG-4 AAC LC; 44.1 and 48 kHz; mono or stereo; VBR, up to 320 kbit/s */
static const unsigned char aac_caps[] = { 0x80 | 0x40, 0x01, 0x80 | 0x08 | 0x04, 0x80 | 0x04, 0xe2, 0x00 };
enum { FMT_S16 = 1, FMT_S32, FMT_FLT, FMT_S16P = 6, FMT_S32P, FMT_FLTP };
/* FFmpeg 4 on 32-bit ARM: AVFrame { data[8], linesize[8], extended_data, width, height, nb_samples, format },
 * AVPacket { buf, pts, dts, data, size } */
#define FRAME_EXT 64
#define FRAME_SAMPLES 76
#define FRAME_FORMAT 80
#define PACKET_DATA 24
#define PACKET_SIZE 28

static struct {
    int tried, ok;
    unsigned (*version)(void);
    void *(*find)(const char *);
    void *(*alloc)(const void *);
    int  (*open)(void *, const void *, void **);
    int  (*send)(void *, const void *);
    int  (*receive)(void *, void *);
    void (*free_ctx)(void **);
    void *(*pkt_alloc)(void);
    int  (*pkt_new)(void *, int);
    void (*pkt_unref)(void *);
    void *(*frame_alloc)(void);
    void (*frame_unref)(void *);
    int  (*channels)(const void *);
    int  (*rate)(const void *);
    void (*log_level)(int);
    void *codec, *ctx, *pkt, *frame;
} ff;

static int aac_usable(void)
{
    if (ff.tried) return ff.ok;
    ff.tried = 1;
    void *c = dlopen("libavcodec.so", RTLD_NOW), *u = dlopen("libavutil.so", RTLD_NOW);
    if (!c || !u) { fprintf(stderr, "a2dp: no AAC (%s)\n", dlerror()); return 0; }
#define SYM(lib, f, name) if (!(*(void **)&ff.f = dlsym(lib, name))) { fprintf(stderr, "a2dp: no AAC (%s missing)\n", name); return 0; }
    SYM(c, version, "avcodec_version") SYM(c, find, "avcodec_find_decoder_by_name") SYM(c, alloc, "avcodec_alloc_context3")
    SYM(c, open, "avcodec_open2") SYM(c, send, "avcodec_send_packet") SYM(c, receive, "avcodec_receive_frame")
    SYM(c, free_ctx, "avcodec_free_context") SYM(c, pkt_alloc, "av_packet_alloc") SYM(c, pkt_new, "av_new_packet")
    SYM(c, pkt_unref, "av_packet_unref") SYM(u, frame_alloc, "av_frame_alloc") SYM(u, frame_unref, "av_frame_unref")
    SYM(u, channels, "av_frame_get_channels") SYM(u, rate, "av_frame_get_sample_rate") SYM(u, log_level, "av_log_set_level")
#undef SYM
    unsigned v = ff.version();
    if (v >> 16 != 58 || !(ff.codec = ff.find("aac_latm"))) { fprintf(stderr, "a2dp: no AAC (libavcodec %u, not FFmpeg 4)\n", v >> 16); return 0; }
    ff.pkt = ff.pkt_alloc(); ff.frame = ff.frame_alloc();
    if (!ff.pkt || !ff.frame || ff.pkt_new(ff.pkt, 1234) ||                  /* the packet layout is what we think */
        *(int *)((char *)ff.pkt + PACKET_SIZE) != 1234 || !*(void **)((char *)ff.pkt + PACKET_DATA)) {
        fprintf(stderr, "a2dp: no AAC (unexpected AVPacket layout)\n"); return 0;
    }
    ff.pkt_unref(ff.pkt);
    ff.log_level(8);                                    /* AV_LOG_FATAL: a corrupt frame is no news */
    return ff.ok = 1;
}

static int aac_check(const unsigned char *c, unsigned n)
{
    if (n < 6) return 0;
    unsigned f = c[1] << 4 | c[2] >> 4;
    return one_bit(c[0]) && c[0] & 0xc0 && (f == 0x010 || f == 0x008) && ((c[2] & 15) == 0x08 || (c[2] & 15) == 0x04);
}

static int aac_open(const unsigned char *c, unsigned n)
{
    (void)c; (void)n;
    if (!aac_usable()) return -1;
    if (!(ff.ctx = ff.alloc(ff.codec)) || ff.open(ff.ctx, ff.codec, NULL) < 0) { ff.free_ctx(&ff.ctx); return -1; }
    return 0;
}

static void aac_close(void) { if (ff.ctx) ff.free_ctx(&ff.ctx); }

static float sample(const void *f, int fmt, unsigned ch, unsigned nch, unsigned i)
{
    uint8_t **ext = *(uint8_t ***)((char *)f + FRAME_EXT);
    switch (fmt) {
    case FMT_FLTP: return ((const float *)ext[ch])[i];
    case FMT_FLT:  return ((const float *)ext[0])[i * nch + ch];
    case FMT_S16P: return ((const int16_t *)ext[ch])[i] / 32768.0f;
    case FMT_S16:  return ((const int16_t *)ext[0])[i * nch + ch] / 32768.0f;
    case FMT_S32P: return ((const int32_t *)ext[ch])[i] / 2147483648.0f;
    case FMT_S32:  return ((const int32_t *)ext[0])[i * nch + ch] / 2147483648.0f;
    }
    return 0;
}

static void aac_packet(const unsigned char *p, size_t n, a2dp_pcm_fn *out)
{
    size_t off = rtp_len(p, n), len = n - off;
    if (!off || !ff.ctx || len > 8191 || ff.pkt_new(ff.pkt, len + 3)) return;
    unsigned char *d = *(unsigned char **)((char *)ff.pkt + PACKET_DATA);
    d[0] = 0x56; d[1] = 0xe0 | len >> 8; d[2] = len;    /* LOAS: sync word 0x2b7, 13-bit length */
    memcpy(d + 3, p + off, len);
    int r = ff.send(ff.ctx, ff.pkt);
    ff.pkt_unref(ff.pkt);
    if (r < 0) return;
    while (ff.receive(ff.ctx, ff.frame) == 0) {
        unsigned ns = *(int *)((char *)ff.frame + FRAME_SAMPLES), nch = ff.channels(ff.frame), rate = ff.rate(ff.frame);
        int fmt = *(int *)((char *)ff.frame + FRAME_FORMAT);
        unsigned och = nch > 1 ? 2 : 1;
        if (ns > sizeof pcm / sizeof *pcm / och) ns = sizeof pcm / sizeof *pcm / och;
        for (unsigned i = 0; i < ns; i++) for (unsigned c = 0; c < och; c++) {
            float v = sample(ff.frame, fmt, c, nch, i) * 32768.0f;
            pcm[i * och + c] = v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
        }
        if (nch && rate) out(pcm, ns, och, rate);
        ff.frame_unref(ff.frame);
    }
}

/* ---------------------------------------------------------------- aptX, aptX HD */

/* vendor ID, codec ID (little endian), then frequency (high nibble: 44.1 kHz 0x2, 48 kHz 0x1) and channel mode (stereo) */
static const unsigned char aptx_caps[] = { 0x4f, 0, 0, 0, 0x01, 0x00, 0x30 | 0x02 };
static const unsigned char aptx_hd_caps[] = { 0xd7, 0, 0, 0, 0x24, 0x00, 0x30 | 0x02, 0, 0, 0, 0 };
static struct aptx_context *aptx; static unsigned aptx_rate; static int aptx_hd;

static int aptx_check(const unsigned char *c, unsigned n, const unsigned char *caps, unsigned ncaps)
{
    return n >= ncaps && !memcmp(c, caps, 6) && (c[6] >> 4 == 1 || c[6] >> 4 == 2) && (c[6] & 15) == 2;
}
static int aptx_check_sd(const unsigned char *c, unsigned n) { return aptx_check(c, n, aptx_caps, sizeof aptx_caps); }
static int aptx_check_hd(const unsigned char *c, unsigned n) { return aptx_check(c, n, aptx_hd_caps, sizeof aptx_hd_caps); }

static int aptx_open(const unsigned char *c, int hd)
{
    if (aptx) aptx_finish(aptx);
    aptx_hd = hd; aptx_rate = c[6] >> 4 == 1 ? 48000 : 44100;
    return (aptx = aptx_init(hd)) ? 0 : -1;
}
static int aptx_open_sd(const unsigned char *c, unsigned n) { (void)n; return aptx_open(c, 0); }
static int aptx_open_hd(const unsigned char *c, unsigned n) { (void)n; return aptx_open(c, 1); }
static void aptx_close(void) { if (aptx) aptx_finish(aptx); aptx = NULL; }

static void aptx_packet(const unsigned char *p, size_t n, a2dp_pcm_fn *out)
{
    static unsigned char raw[24 * 1024 + 24];
    size_t off = aptx_hd ? rtp_len(p, n) : 0, written = 0, dropped = 0; int synced = 0;
    if ((aptx_hd && !off) || !aptx) return;
    for (size_t o = off; o < n; ) {                     /* at most 1024 aptX samples (4096 frames) per round */
        size_t k = n - o, max = 1024 * (aptx_hd ? 6 : 4);
        if (k > max) k = max;
        size_t used = aptx_decode_sync(aptx, p + o, k, raw, sizeof raw, &written, &synced, &dropped);
        unsigned frames = written / 6;                  /* 24-bit little endian stereo: keep the top 16 bits */
        for (unsigned i = 0; i < 2 * frames; i++) pcm[i] = (int16_t)(raw[3 * i + 2] << 8 | raw[3 * i + 1]);
        if (frames) out(pcm, frames, 2, aptx_rate);
        if (!used) break;
        o += used;
    }
}

/* ---------------------------------------------------------------- Opus (Google) */

/* vendor ID, codec ID, then 48 kHz | 10 or 20 ms frames | mono or stereo */
static const unsigned char opus_caps[] = { 0xe0, 0, 0, 0, 0x01, 0x00, 0x80 | 0x18 | 0x03 };
static OpusDecoder *opus; static unsigned opus_ch;

static int opus_check(const unsigned char *c, unsigned n)
{
    return n >= sizeof opus_caps && !memcmp(c, opus_caps, 6) && c[6] & 0x80 && one_bit(c[6] & 0x18) && ((c[6] & 7) == 1 || (c[6] & 7) == 2);
}

static int opus_open(const unsigned char *c, unsigned n)
{
    int err = 0; (void)n;
    if (opus) opus_decoder_destroy(opus);
    opus_ch = (c[6] & 7) == 2 ? 2 : 1;
    return (opus = opus_decoder_create(48000, opus_ch, &err)) && !err ? 0 : -1;
}

static void opus_close(void) { if (opus) opus_decoder_destroy(opus); opus = NULL; }

static void opus_packet(const unsigned char *p, size_t n, a2dp_pcm_fn *out)
{
    size_t off = rtp_len(p, n);
    if (!off || !opus || p[off] & 0x80 || !(p[off] & 15) || off + 1 >= n) return;
    int got = opus_decode(opus, p + off + 1, n - off - 1, pcm, sizeof pcm / sizeof *pcm / opus_ch, 0);
    if (got > 0) out(pcm, got, opus_ch, 48000);
}

/* ---------------------------------------------------------------- the list
 * Opus is decoded fine (PipeWire's opus_g plays), but not offered.  Android uses A2DP Opus only as its low-latency
 * codec (btif_av_source_set_low_latency_codec: Opus priority highest when a stream starts in low-latency mode, -1
 * otherwise), and a Pixel 10 Pro XL (Android 17) alternated the two modes on every stream start: Opus, aptX HD, Opus,
 * ... each stream closed before its first packet, nothing ever played.  With Opus not offered it streams aptX HD. */

const struct a2dp_codec a2dp_codecs[] = {
    { "SBC", A2DP_SBC, sbc_caps, sizeof sbc_caps, always, sbc_check, sbc_open, sbc_packet, nothing },
    { "AAC", A2DP_AAC, aac_caps, sizeof aac_caps, aac_usable, aac_check, aac_open, aac_packet, aac_close },
    { "aptX HD", A2DP_VENDOR, aptx_hd_caps, sizeof aptx_hd_caps, always, aptx_check_hd, aptx_open_hd, aptx_packet, aptx_close },
    { "aptX", A2DP_VENDOR, aptx_caps, sizeof aptx_caps, always, aptx_check_sd, aptx_open_sd, aptx_packet, aptx_close },
    { "Opus", A2DP_VENDOR, opus_caps, sizeof opus_caps, never, opus_check, opus_open, opus_packet, opus_close },
};
const int a2dp_ncodecs = sizeof a2dp_codecs / sizeof *a2dp_codecs;
