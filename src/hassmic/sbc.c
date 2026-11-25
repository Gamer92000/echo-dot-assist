/*
 * SBC decoder, straight from the A2DP specification: frame header, bit allocation (12.6.3), dequantisation, joint stereo
 * and the synthesis filter bank (12.6.4) in floating point.  Checked against BlueZ's libsbc (sbcdec) in tests/unit.
 * A2DP guarantees whole frames per media packet, so there is no resynchronisation and the CRC is not checked (L2CAP is
 * reliable); mSBC's frames come over the air as they are, and their CRC is checked.
 */
#include "sbc.h"
#include <math.h>
#include <string.h>

/* A2DP specification, section 12.8: prototype filter (the synthesis window with its signs as the spec lists it) */
static const float proto4[40] = {
    0.00000000e+00, 5.36548976e-04, 1.49188357e-03, 2.73370904e-03, 3.83720193e-03,
    3.89205149e-03, 1.86581691e-03, -3.06012286e-03, 1.09137620e-02, 2.04385087e-02,
    2.88757392e-02, 3.21939290e-02, 2.58767811e-02, 6.13245186e-03, -2.88217274e-02,
    -7.76463494e-02, 1.35593274e-01, 1.94987841e-01, 2.46636662e-01, 2.81828203e-01,
    2.94315332e-01, 2.81828203e-01, 2.46636662e-01, 1.94987841e-01, -1.35593274e-01,
    -7.76463494e-02, -2.88217274e-02, 6.13245186e-03, 2.58767811e-02, 3.21939290e-02,
    2.88757392e-02, 2.04385087e-02, -1.09137620e-02, -3.06012286e-03, 1.86581691e-03,
    3.89205149e-03, 3.83720193e-03, 2.73370904e-03, 1.49188357e-03, 5.36548976e-04,
};
static const float proto8[80] = {
    0.00000000e+00, 1.56575398e-04, 3.43256425e-04, 5.54620202e-04, 8.23919506e-04,
    1.13992507e-03, 1.47640169e-03, 1.78371725e-03, 2.01182542e-03, 2.10371989e-03,
    1.99454554e-03, 1.61656283e-03, 9.02154502e-04, -1.78805361e-04, -1.64973098e-03,
    -3.49717454e-03, 5.65949473e-03, 8.02941163e-03, 1.04584443e-02, 1.27472335e-02,
    1.46525263e-02, 1.59045603e-02, 1.62208471e-02, 1.53184106e-02, 1.29371806e-02,
    8.85757540e-03, 2.92408442e-03, -4.91578024e-03, -1.46404076e-02, -2.61098752e-02,
    -3.90751381e-02, -5.31873032e-02, 6.79989431e-02, 8.29847578e-02, 9.75753918e-02,
    1.11196689e-01, 1.23264548e-01, 1.33264415e-01, 1.40753505e-01, 1.45389847e-01,
    1.46955068e-01, 1.45389847e-01, 1.40753505e-01, 1.33264415e-01, 1.23264548e-01,
    1.11196689e-01, 9.75753918e-02, 8.29847578e-02, -6.79989431e-02, -5.31873032e-02,
    -3.90751381e-02, -2.61098752e-02, -1.46404076e-02, -4.91578024e-03, 2.92408442e-03,
    8.85757540e-03, 1.29371806e-02, 1.53184106e-02, 1.62208471e-02, 1.59045603e-02,
    1.46525263e-02, 1.27472335e-02, 1.04584443e-02, 8.02941163e-03, -5.65949473e-03,
    -3.49717454e-03, -1.64973098e-03, -1.78805361e-04, 9.02154502e-04, 1.61656283e-03,
    1.99454554e-03, 2.10371989e-03, 2.01182542e-03, 1.78371725e-03, 1.47640169e-03,
    1.13992507e-03, 8.23919506e-04, 5.54620202e-04, 3.43256425e-04, 1.56575398e-04,
};
/* loudness allocation offsets, appendix B: [sampling frequency][subband] */
static const int offset4[4][4] = { { -1, 0, 0, 0 }, { -2, 0, 0, 1 }, { -2, 0, 0, 1 }, { -2, 0, 0, 1 } };
static const int offset8[4][8] = { { -2, 0, 0, 0, 0, 0, 0, 1 }, { -3, 0, 0, 0, 0, 0, 1, 2 }, { -4, 0, 0, 0, 0, 0, 1, 2 }, { -4, 0, 0, 0, 0, 0, 1, 2 } };
static const unsigned rates[4] = { 16000, 32000, 44100, 48000 };

static float cos4[8][4], cos8[16][8], win4[40], win8[80];     /* matrixing N[k][i]; window D = proto * -M */
static int tables_ready;

static void tables(void)
{
    for (int k = 0; k < 8; k++) for (int i = 0; i < 4; i++) cos4[k][i] = cos((i + 0.5) * (k + 2) * M_PI / 4);
    for (int k = 0; k < 16; k++) for (int i = 0; i < 8; i++) cos8[k][i] = cos((i + 0.5) * (k + 4) * M_PI / 8);
    for (int i = 0; i < 40; i++) win4[i] = proto4[i] * -4;
    for (int i = 0; i < 80; i++) win8[i] = proto8[i] * -8;
    tables_ready = 1;
}

void sbc_init(struct sbc *s) { memset(s, 0, sizeof *s); if (!tables_ready) tables(); }

struct bits { const uint8_t *p; size_t pos; };
static unsigned get(struct bits *b, int n)
{
    unsigned v = 0;
    while (n--) { v = v << 1 | (b->p[b->pos >> 3] >> (7 - (b->pos & 7)) & 1); b->pos++; }
    return v;
}

/* Bit allocation for one channel (mono, dual channel: nch 1) or both (stereo, joint stereo: nch 2), section 12.6.3 */
static void allocate(int nch, int nsb, int snr, int fs, int bitpool, const int sf[2][8], int bits[2][8])
{
    int need[2][8], max = 0, count = 0, slices = 0, slice;
    for (int ch = 0; ch < nch; ch++) for (int sb = 0; sb < nsb; sb++) {
        int n;
        if (snr) n = sf[ch][sb];
        else if (!sf[ch][sb]) n = -5;
        else { int l = sf[ch][sb] - (nsb == 4 ? offset4[fs][sb] : offset8[fs][sb]); n = l > 0 ? l / 2 : l; }
        need[ch][sb] = n; if (n > max) max = n;
    }
    slice = max + 1;
    do {
        slice--; count += slices; slices = 0;
        for (int ch = 0; ch < nch; ch++) for (int sb = 0; sb < nsb; sb++) {
            if (need[ch][sb] > slice + 1 && need[ch][sb] < slice + 16) slices++;
            else if (need[ch][sb] == slice + 1) slices += 2;
        }
    } while (count + slices < bitpool);
    if (count + slices == bitpool) { count += slices; slice--; }
    for (int ch = 0; ch < nch; ch++) for (int sb = 0; sb < nsb; sb++) {
        int b = need[ch][sb] < slice + 2 ? 0 : need[ch][sb] - slice;
        bits[ch][sb] = b > 16 ? 16 : b;
    }
    /* the rest of the bitpool: subband by subband, alternating channels */
    for (int sb = 0, ch = 0; count < bitpool && sb < nsb; ) {
        if (bits[ch][sb] >= 2 && bits[ch][sb] < 16) { bits[ch][sb]++; count++; }
        else if (need[ch][sb] == slice + 1 && bitpool > count + 1) { bits[ch][sb] = 2; count += 2; }
        if (nch == 1 || ch == 1) { ch = 0; sb++; } else ch = 1;
    }
    for (int sb = 0, ch = 0; count < bitpool && sb < nsb; ) {
        if (bits[ch][sb] < 16) { bits[ch][sb]++; count++; }
        if (nch == 1 || ch == 1) { ch = 0; sb++; } else ch = 1;
    }
}

/* One block of one channel through the synthesis filter bank: nsb subband samples in, nsb PCM samples out */
static void synthesis(float *v, int nsb, const float *s, int16_t *out, int stride)
{
    int m2 = 2 * nsb; float u[80];
    memmove(v + m2, v, (10 * m2 - m2) * sizeof *v);
    for (int k = 0; k < m2; k++) {
        float a = 0; const float *n = nsb == 4 ? cos4[k] : cos8[k];
        for (int i = 0; i < nsb; i++) a += n[i] * s[i];
        v[k] = a;
    }
    for (int i = 0; i < 5; i++) for (int j = 0; j < nsb; j++) {
        u[i * m2 + j] = v[i * 2 * m2 + j];
        u[i * m2 + nsb + j] = v[i * 2 * m2 + 3 * nsb + j];
    }
    const float *w = nsb == 4 ? win4 : win8;
    for (int j = 0; j < nsb; j++) {
        float x = 0;
        for (int i = 0; i < 10; i++) x += u[j + nsb * i] * w[j + nsb * i];
        long r = lrintf(x);
        out[j * stride] = r > 32767 ? 32767 : r < -32768 ? -32768 : r;
    }
}

/* CRC-8, polynomial x^8 + x^4 + x^3 + x^2 + 1, initial 0x0f, over bytes 1-2 and the first `bits` bits from byte 4 */
static uint8_t crc8(const uint8_t *p, size_t bits)
{
    unsigned c = 0x0f;
    for (size_t i = 8; i < 32 + bits; i++) {
        if (i >= 24 && i < 32) continue;                /* the CRC itself */
        unsigned b = p[i >> 3] >> (7 - (i & 7)) & 1;
        c = (c << 1 ^ ((c >> 7 ^ b) ? 0x1d : 0)) & 0xff;
    }
    return c;
}
/* One frame of the given parameters, its header (and the CRC where CHECK) already read: section 12.6 */
static size_t decode(struct sbc *s, const uint8_t *p, size_t n, int fs, int blocks, int mode, int snr, int nsb, int bitpool,
                     int check, int16_t *out, unsigned *samples)
{
    int nch = mode ? 2 : 1;
    size_t len = 4 + (size_t)(4 * nsb * nch) / 8;
    if (mode <= 1) len += (blocks * nch * bitpool + 7) / 8;
    else len += ((mode == 3 ? nsb : 0) + blocks * bitpool + 7) / 8;
    if (len > n || bitpool > (mode <= 1 ? 16 : 32) * nsb) return 0;     /* the spec's bitpool limits keep reads in the frame */

    struct bits b = { p, 32 }; int join = 0, sf[2][8], bits[2][8];
    if (mode == 3) { for (int sb = 0; sb < nsb - 1; sb++) join |= get(&b, 1) << sb; get(&b, 1); }
    for (int ch = 0; ch < nch; ch++) for (int sb = 0; sb < nsb; sb++) sf[ch][sb] = get(&b, 4);
    if (check && crc8(p, b.pos - 32) != p[3]) return 0;
    if (mode <= 1) for (int ch = 0; ch < nch; ch++) {            /* each channel on its own */
        int one[2][8], ob[2][8];
        memcpy(one[0], sf[ch], sizeof one[0]);
        allocate(1, nsb, snr, fs, bitpool, one, ob);
        memcpy(bits[ch], ob[0], sizeof bits[ch]);
    } else allocate(2, nsb, snr, fs, bitpool, sf, bits);

    if (s->nsb != nsb) { memset(s->v, 0, sizeof s->v); s->nsb = nsb; }
    for (int blk = 0; blk < blocks; blk++) {
        float sb_s[2][8];
        for (int ch = 0; ch < nch; ch++) for (int sb = 0; sb < nsb; sb++) {
            int nb = bits[ch][sb];
            if (!nb) { sb_s[ch][sb] = 0; continue; }
            unsigned q = get(&b, nb);
            float levels = (float)((1u << nb) - 1), scale = (float)(2u << sf[ch][sb]);
            sb_s[ch][sb] = scale * ((2.0f * q + 1) / levels - 1);
        }
        for (int sb = 0; sb < nsb; sb++) if (join >> sb & 1) {
            float l = sb_s[0][sb] + sb_s[1][sb], r = sb_s[0][sb] - sb_s[1][sb];
            sb_s[0][sb] = l; sb_s[1][sb] = r;
        }
        for (int ch = 0; ch < nch; ch++) synthesis(s->v[ch], nsb, sb_s[ch], out + blk * nsb * nch + ch, nch);
    }
    s->rate = rates[fs]; s->channels = nch;
    *samples = blocks * nsb;
    return len;
}

size_t sbc_decode(struct sbc *s, const uint8_t *p, size_t n, int16_t *out, unsigned *samples)
{
    if (n < 4 || p[0] != 0x9c) return 0;
    return decode(s, p, n, p[1] >> 6, 4 * ((p[1] >> 4 & 3) + 1), p[1] >> 2 & 3, p[1] >> 1 & 1, p[1] & 1 ? 8 : 4, p[2], 0, out, samples);
}

/* mSBC's fixed parameters (HFP 1.7, 5.7.4): 16 kHz, 15 blocks, mono, loudness, 8 subbands, bitpool 26.  The air is
 * not reliable as L2CAP is: the CRC is checked, a frame that fails it is a lost one */
size_t msbc_decode(struct sbc *s, const uint8_t *p, size_t n, int16_t *out)
{
    unsigned samples;
    if (n < MSBC_FRAME || p[0] != 0xad || p[1] || p[2]) return 0;
    return decode(s, p, n, 0, 15, 0, 0, 8, 26, 1, out, &samples) == MSBC_FRAME ? MSBC_FRAME : 0;
}

/* ---------------------------------------------------------------- encoder
 * The analysis filter bank (12.5.1 of the specification, in floating point like the decoder), scale factors, joint
 * stereo per subband where mid/side needs smaller scale factors than left/right, the same bit allocation as decoding,
 * and the CRC sinks check (libsbc refuses a frame without it). */

static float acos8[8][16];                      /* analysis matrixing M[k][i] = cos((k + 0.5)(i - 4) pi / 8) */

int sbc_enc_init(struct sbc_enc *e, unsigned rate, int bitpool)
{
    memset(e, 0, sizeof *e);
    if (!tables_ready) tables();
    if (!acos8[0][0]) for (int k = 0; k < 8; k++) for (int i = 0; i < 16; i++) acos8[k][i] = cos((k + 0.5) * (i - 4) * M_PI / 8);
    e->fs = -1;
    for (int i = 0; i < 4; i++) if (rates[i] == rate) e->fs = i;
    e->bitpool = bitpool < 2 ? 2 : bitpool > 250 ? 250 : bitpool;
    return e->fs < 0 ? -1 : 0;
}

size_t sbc_enc_len(int bitpool) { return 4 + 4 * 8 * 2 / 8 + (8 + 16 * bitpool + 7) / 8; }

/* 8 new samples of one channel (stride apart, oldest first) through the analysis filter: 8 subband samples */
static void analysis(float *x, const int16_t *in, int stride, float *s)
{
    float y[16];
    memmove(x + 8, x, 72 * sizeof *x);
    for (int i = 0; i < 8; i++) x[7 - i] = in[i * stride];
    for (int i = 0; i < 16; i++) {
        float a = 0;
        for (int j = 0; j < 5; j++) a += proto8[i + 16 * j] * x[i + 16 * j];
        y[i] = a;
    }
    for (int k = 0; k < 8; k++) {
        float a = 0;
        for (int i = 0; i < 16; i++) a += acos8[k][i] * y[i];
        s[k] = a;
    }
}

static int scalefactor(float m) { int sf = 0; while (sf < 15 && m >= (float)(2 << sf)) sf++; return sf; }

static void put(uint8_t *p, size_t *pos, unsigned v, int n)
{
    while (n--) { if (v >> n & 1) p[*pos >> 3] |= 0x80 >> (*pos & 7); (*pos)++; }
}

/* BLOCKS blocks of NCH channels (interleaved) into one frame with the header bytes h0..h2: joint stereo where it pays */
static size_t encode(struct sbc_enc *e, const int16_t *pcm, int nch, int blocks, const uint8_t h[3], uint8_t *out)
{
    float sb[16][2][8];
    for (int blk = 0; blk < blocks; blk++) for (int ch = 0; ch < nch; ch++) analysis(e->x[ch], pcm + blk * 8 * nch + ch, nch, sb[blk][ch]);

    int sf[2][8], bits[2][8], join = 0;
    for (int k = 0; k < 8; k++) {
        float m[2] = { 0, 0 }, ms[2] = { 0, 0 };
        for (int blk = 0; blk < blocks; blk++) for (int ch = 0; ch < nch; ch++) if (fabsf(sb[blk][ch][k]) > m[ch]) m[ch] = fabsf(sb[blk][ch][k]);
        if (nch == 1) { sf[0][k] = scalefactor(m[0]); continue; }
        for (int blk = 0; blk < blocks; blk++) {
            float l = sb[blk][0][k], r = sb[blk][1][k], mid = (l + r) / 2, side = (l - r) / 2;
            if (fabsf(mid) > ms[0]) ms[0] = fabsf(mid);
            if (fabsf(side) > ms[1]) ms[1] = fabsf(side);
        }
        int a = scalefactor(m[0]), b = scalefactor(m[1]), c = scalefactor(ms[0]), d = scalefactor(ms[1]);
        if (k < 7 && c + d < a + b) {                   /* the last subband never joins (12.5.2) */
            join |= 1 << k; sf[0][k] = c; sf[1][k] = d;
            for (int blk = 0; blk < blocks; blk++) {
                float l = sb[blk][0][k], r = sb[blk][1][k];
                sb[blk][0][k] = (l + r) / 2; sb[blk][1][k] = (l - r) / 2;
            }
        } else { sf[0][k] = a; sf[1][k] = b; }
    }
    allocate(nch, 8, 0, e->fs, e->bitpool, sf, bits);

    size_t len = 4 + 4 * nch + ((nch == 2 ? 8 : 0) + blocks * e->bitpool + 7) / 8, pos = 32;
    memset(out, 0, len);
    out[0] = h[0]; out[1] = h[1]; out[2] = h[2];
    if (nch == 2) for (int k = 0; k < 8; k++) put(out, &pos, join >> k & 1, 1);
    for (int ch = 0; ch < nch; ch++) for (int k = 0; k < 8; k++) put(out, &pos, sf[ch][k], 4);
    out[3] = crc8(out, pos - 32);
    for (int blk = 0; blk < blocks; blk++) for (int ch = 0; ch < nch; ch++) for (int k = 0; k < 8; k++) {
        int nb = bits[ch][k];
        if (!nb) continue;
        /* the decoder rebuilds scale * ((2q + 1) / levels - 1): q is the level the sample falls in */
        float levels = (float)((1u << nb) - 1), scale = (float)(2u << sf[ch][k]);
        long q = (long)floorf((sb[blk][ch][k] / scale + 1) * levels / 2);
        put(out, &pos, q < 0 ? 0 : q > (long)levels - 1 ? (unsigned)levels - 1 : (unsigned)q, nb);
    }
    return len;
}

size_t sbc_encode(struct sbc_enc *e, const int16_t *pcm, uint8_t *out)
{
    const uint8_t h[3] = { 0x9c, (uint8_t)(e->fs << 6 | 3 << 4 | 3 << 2 | 0 << 1 | 1), (uint8_t)e->bitpool };
    return encode(e, pcm, 2, 16, h, out);
}

size_t msbc_encode(struct sbc_enc *e, const int16_t *pcm, uint8_t *out)
{
    static const uint8_t h[3] = { 0xad, 0, 0 };
    e->fs = 0; e->bitpool = 26;
    return encode(e, pcm, 1, 15, h, out);
}
