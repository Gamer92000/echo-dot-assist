/* CVSD and the filters around it: see cvsd.h.  The spec's coder: x^(k-1) = h y(k-1) the prediction, b(k) = the input at or
 * over it, the step grows by DMIN up to DMAX after J = 4 equal bits, else decays by beta down to DMIN; y(k) = x^(k-1) +-
 * the step, held to 16 bits.  Its constants: h = 1 - 1/32, beta = 1 - 1/1024, DMIN 10, DMAX 1280.  The decoder is the
 * same accumulator driven by the bits.  At 64 kHz the band is 0..32 kHz: the 16 kHz side is filtered to speech's band
 * both ways, flat to 3.4 kHz (cvsd.h), as a chip's CVSD path is (and a phone's again). */
#include "cvsd.h"
#include <math.h>
#include <string.h>

#define H     (1.0f - 1.0f / 32)
#define BETA  (1.0f - 1.0f / 1024)
#define DMIN  10.0f
#define DMAX  1280.0f

void fir_init(struct fir *f, int L, float rate_high, float fc, int taps)
{
    memset(f, 0, sizeof *f);
    f->L = L; f->taps = taps > FIR_TAPS ? FIR_TAPS : taps;
    float c = fc / rate_high, sum = 0;
    for (int i = 0; i < f->taps; i++) {
        float t = i - (f->taps - 1) / 2.0f, w = 0.54f - 0.46f * cosf(2 * (float)M_PI * i / (f->taps - 1));
        f->h[i] = (t == 0 ? 2 * c : sinf(2 * (float)M_PI * c * t) / ((float)M_PI * t)) * w;
        sum += f->h[i];
    }
    for (int i = 0; i < f->taps; i++) f->h[i] /= sum;           /* unity at DC */
}

static float fir_at(const struct fir *f)
{
    float a = 0;
    for (int k = 0; k < f->taps; k++) a += f->h[k] * f->x[(f->pos - k + f->taps) % f->taps];
    return a;
}

/* zeros between the samples, then the low-pass; times L for the energy the zeros took */
size_t fir_up(struct fir *f, const int16_t *in, size_t n, float *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++)
        for (int p = 0; p < f->L; p++) {
            f->pos = (f->pos + 1) % f->taps; f->x[f->pos] = p ? 0 : in[i];
            out[o++] = f->L * fir_at(f);
        }
    return o;
}

size_t fir_down(struct fir *f, const float *in, size_t n, int16_t *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        f->pos = (f->pos + 1) % f->taps; f->x[f->pos] = in[i];
        if ((i + 1) % f->L) continue;
        float v = fir_at(f);
        out[o++] = v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)lrintf(v);
    }
    return o;
}

void cvsd_init(struct cvsd *c) { c->y = 0; c->delta = DMIN; c->hist = 0; fir_init(&c->f, 4, 64000, 4400, 128); }

static float step(struct cvsd *c, unsigned b)
{
    c->hist = (c->hist << 1 | b) & 0xf;
    c->delta = c->hist == 0 || c->hist == 0xf ? fminf(c->delta + DMIN, DMAX) : fmaxf(c->delta * BETA, DMIN);
    float y = H * c->y + (b ? c->delta : -c->delta);
    return c->y = y > 32767 ? 32767 : y < -32768 ? -32768 : y;
}

size_t cvsd_encode(struct cvsd *c, const int16_t *pcm, size_t n, uint8_t *out)
{
    float up[64];
    size_t o = 0;
    for (size_t i = 0; i + 1 < n; i += 2) {
        fir_up(&c->f, pcm + i, 2, up);                              /* two samples: 8 bits */
        uint8_t byte = 0;
        for (int k = 0; k < 8; k++) {
            unsigned b = up[k] >= H * c->y;
            step(c, b);
            byte |= b << k;                                         /* least significant first */
        }
        out[o++] = byte;
    }
    return o;
}

size_t cvsd_decode(struct cvsd *c, const uint8_t *in, size_t n, int16_t *pcm)
{
    float s[8];
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        for (int k = 0; k < 8; k++) s[k] = step(c, in[i] >> k & 1);
        o += fir_down(&c->f, s, 8, pcm + o);
    }
    return o;
}
