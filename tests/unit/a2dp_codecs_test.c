/* The Bluetooth speaker's codec glue (a2dp_codecs.c): a 1 kHz stereo tone encoded the way sources send it (aptX raw,
 * aptX HD and Opus in RTP, packets as PipeWire builds them), decoded through a2dp_codecs[], must come out as the same
 * tone at the same level.  AAC needs the firmware's FFmpeg 4 and is tested on the device. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "a2dp_codec.h"
#include "../third_party/freeaptx.h"

typedef struct OpusEncoder OpusEncoder;
OpusEncoder *opus_encoder_create(int32_t Fs, int channels, int application, int *error);
int opus_encode(OpusEncoder *st, const int16_t *pcm, int frame_size, unsigned char *data, int32_t max_data_bytes);

#define RATE 48000
#define SECONDS 2
static int16_t out[RATE * SECONDS * 2 + 8192]; static size_t nout; static unsigned out_rate, out_ch;

static void sink(const int16_t *pcm, unsigned frames, unsigned ch, unsigned rate)
{
    out_rate = rate; out_ch = ch;
    for (unsigned i = 0; i < frames && nout < RATE * SECONDS + 4096; i++, nout++) { out[2 * nout] = pcm[i * ch]; out[2 * nout + 1] = pcm[i * ch + ch - 1]; }
}

static const struct a2dp_codec *find(const char *name) { for (int i = 0; i < a2dp_ncodecs; i++) if (!strcmp(a2dp_codecs[i].name, name)) return &a2dp_codecs[i]; return NULL; }
static double tone(int i) { return 12000 * sin(2 * M_PI * 1000 * i / RATE); }

static int verdict(const char *name)
{
    double e = 0, p; size_t skip = 4800;                    /* codec delay and start-up */
    for (size_t i = skip; i < nout; i++) e += (double)out[2 * i] * out[2 * i];
    double rms = nout > skip ? sqrt(e / (nout - skip)) : 0, want = 12000 / sqrt(2);
    /* 1 kHz: correlate with sine and cosine */
    double si = 0, co = 0; for (size_t i = skip; i < nout; i++) { si += out[2 * i] * sin(2 * M_PI * 1000 * i / RATE); co += out[2 * i] * cos(2 * M_PI * 1000 * i / RATE); }
    p = nout > skip ? sqrt(si * si + co * co) * 2 / (nout - skip) : 0;
    int ok = out_rate == RATE && nout >= RATE * SECONDS * 9 / 10 && fabs(20 * log10(rms / want)) < 1 && p / 12000 > 0.9;
    printf("%s %-8s %zu frames at %u Hz, level %+.2f dB, 1 kHz part %.3f\n", ok ? "ok  " : "FAIL", name, nout, out_rate, 20 * log10(rms / want), p / 12000);
    return !ok;
}

static void rtp(unsigned char *p, int seq) { memset(p, 0, 12); p[0] = 0x80; p[1] = 96; p[2] = seq >> 8; p[3] = seq; }

static int test_aptx(int hd)
{
    const struct a2dp_codec *c = find(hd ? "aptX HD" : "aptX"); unsigned char cfg[11];
    memcpy(cfg, c->caps, c->ncaps); cfg[6] = 0x12;                     /* 48 kHz stereo */
    if (!c->check(cfg, c->ncaps) || c->open(cfg, c->ncaps)) { printf("FAIL %s config\n", c->name); return 1; }
    struct aptx_context *enc = aptx_init(hd); nout = 0;
    for (int i = 0, seq = 0; i < RATE * SECONDS; i += 4 * 64, seq++) {     /* 64 aptX samples (256 frames) per packet */
        unsigned char in[256 * 6], pkt[12 + 64 * 6]; size_t w = 0, h = hd ? 12 : 0;
        for (int k = 0; k < 256; k++) for (int ch = 0; ch < 2; ch++) {
            int v = (int)tone(i + k) * 256; unsigned char *b = in + 6 * k + 3 * ch; b[0] = v; b[1] = v >> 8; b[2] = v >> 16;
        }
        aptx_encode(enc, in, sizeof in, pkt + h, sizeof pkt - h, &w);
        if (hd) rtp(pkt, seq);
        c->packet(pkt, h + w, sink);
    }
    aptx_finish(enc); c->close();
    return verdict(c->name);
}

static int test_opus(void)
{
    const struct a2dp_codec *c = find("Opus"); unsigned char cfg[7]; int err = 0;
    memcpy(cfg, c->caps, 7); cfg[6] = 0x80 | 0x10 | 0x02;             /* 48 kHz, 20 ms, stereo */
    if (!c->check(cfg, 7) || c->open(cfg, 7)) { printf("FAIL Opus config\n"); return 1; }
    OpusEncoder *enc = opus_encoder_create(RATE, 2, 2049, &err); nout = 0;     /* OPUS_APPLICATION_AUDIO */
    for (int i = 0, seq = 0; i < RATE * SECONDS; i += 960, seq++) {
        int16_t in[960 * 2]; unsigned char pkt[13 + 1500];
        for (int k = 0; k < 960; k++) in[2 * k] = in[2 * k + 1] = (int16_t)tone(i + k);
        int n = opus_encode(enc, in, 960, pkt + 13, 1500);
        rtp(pkt, seq); pkt[12] = 1;                                     /* one frame */
        if (n > 0) c->packet(pkt, 13 + n, sink);
    }
    c->close();
    return verdict(c->name);
}

int main(void) { return test_aptx(0) | test_aptx(1) | test_opus(); }
