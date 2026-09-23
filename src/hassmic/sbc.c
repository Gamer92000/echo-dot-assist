/*
 * SBC decoder, straight from the A2DP specification: frame header, bit allocation (12.6.3), dequantisation, joint stereo
 * and the synthesis filter bank (12.6.4) in floating point.  Checked against BlueZ's libsbc (sbcdec) in tests/unit.
 * A2DP guarantees whole frames per media packet, so there is no resynchronisation and the CRC is not checked (L2CAP is
 * reliable).
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

size_t sbc_decode(struct sbc *s, const uint8_t *p, size_t n, int16_t *out, unsigned *samples)
{
    if (n < 4 || p[0] != 0x9c) return 0;
    int fs = p[1] >> 6, blocks = 4 * ((p[1] >> 4 & 3) + 1), mode = p[1] >> 2 & 3, snr = p[1] >> 1 & 1, nsb = p[1] & 1 ? 8 : 4;
    int nch = mode ? 2 : 1, bitpool = p[2];
    size_t len = 4 + (size_t)(4 * nsb * nch) / 8;
    if (mode <= 1) len += (blocks * nch * bitpool + 7) / 8;
    else len += ((mode == 3 ? nsb : 0) + blocks * bitpool + 7) / 8;
    if (len > n || bitpool > (mode <= 1 ? 16 : 32) * nsb) return 0;     /* the spec's bitpool limits keep reads in the frame */

    struct bits b = { p, 32 }; int join = 0, sf[2][8], bits[2][8];
    if (mode == 3) { for (int sb = 0; sb < nsb - 1; sb++) join |= get(&b, 1) << sb; get(&b, 1); }
    for (int ch = 0; ch < nch; ch++) for (int sb = 0; sb < nsb; sb++) sf[ch][sb] = get(&b, 4);
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
