/* Float32 reimplementation of the stock libpryon LFBE frontend, stage by stage
 * as measured on the device library (see tools/pryon-native/LEDGER.md, "Frontend").
 *
 *   pre-FFT   Kaldi ProcessWindow in float: int16 -> float, DC removal
 *             (sum / 400), preemphasis 0.96875 (descending, first sample
 *             scaled), povey window computed in double then rounded. Exact.
 *   FFT       Ne10 ne10_fft_r2c_1d_float32_neon (UNROLL_LEVEL 1) behind
 *             Kaldi's srfft slot; reimplemented below in the same order.
 *   power     Kaldi ComputePowerSpectrum (float). Exact.
 *   mel       Kaldi MelBanks, 64 bins 80..7200 Hz, weights in float with
 *             MelScale = 1127*logf(1+f/700) (msun logf), no HTK bin-0 quirk,
 *             VecVec = OpenBLAS ARMv7 sdot_vfp order, floor max(e, 1). Exact.
 *   log       FreeBSD msun logf (bionic libm). Exact.
 *
 * Build: cc -O2 -ffp-contract=off -o pryon_lfbe pryon_lfbe.c -lm
 * Usage: pryon_lfbe IN.raw(s16le 16 kHz) OUT.f32   (64 floats per frame)
 * Debug env: PRYON_FFT_FROM=f32 replays captured stock FFT outputs (512
 * floats per frame, frame order); PRYON_DUMP_PRE=f32 writes pre-FFT frames;
 * PRYON_FFT_ONLY=1 treats IN as f32 512-frames and writes FFT outputs.
 *
 * Full third-party license conditions and disclaimers: THIRD_PARTY_NOTICES.md and licenses/.
 * Third-party notices for ported algorithms:
 *   msun_logf is a port of FreeBSD msun e_logf.c:
 *     Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 *     Developed at SunPro, a Sun Microsystems, Inc. business.
 *     Permission to use, copy, modify, and distribute this software is freely
 *     granted, provided that this notice is preserved.
 *   The FFT follows Ne10's r2c NEON implementation (NE10_rfft_float32.neonintrinsic.c):
 *     Copyright 2014-16 ARM Limited and Contributors. BSD-3-Clause.
 *   The pre-FFT, power and mel stages follow Kaldi (Apache-2.0). */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FL 400
#define HOP 160
#define N 512
#define NB 64
#define PI 3.14159265358979323846264338327

/* OpenBLAS ARMv7 sdot_vfp (libpryon +0x51d9e0): two accumulators over blocks
 * of four, remainder into the first, then s0 + s1. VFP vmla is non-fused. */
static float sdot(const float *a, const float *b, int n) {
  float s0 = 0, s1 = 0; int i = 0;
  for (; i + 4 <= n; i += 4) { s0 += a[i] * b[i]; s1 += a[i + 1] * b[i + 1]; s0 += a[i + 2] * b[i + 2]; s1 += a[i + 3] * b[i + 3]; }
  for (; i < n; i++) s0 += a[i] * b[i];
  return s0 + s1;
}

/* FreeBSD msun __ieee754_logf (bionic libm); bit-exact against all 26,560
 * stock ApplyLog outputs. Finite x > 0 only. */
static float msun_logf(float x) {
  const float ln2_hi = 6.9313812256e-01f, ln2_lo = 9.0580006145e-06f;
  const float Lg1 = 0xaaaaaa.0p-24f, Lg2 = 0xccce13.0p-25f, Lg3 = 0x91e9ee.0p-25f, Lg4 = 0xf89e26.0p-26f;
  float hfsq, f, s, z, R, w, t1, t2, dk; int32_t k = 0, ix, i, j;
  memcpy(&ix, &x, 4);
  if (ix < 0x00800000 || ix >= 0x7f800000) return logf(x);  /* subnormal/inf: unused by LFBE */
  k += (ix >> 23) - 127; ix &= 0x007fffff;
  i = (ix + (0x95f64 << 3)) & 0x800000;
  { int32_t t = ix | (i ^ 0x3f800000); memcpy(&x, &t, 4); }
  k += (i >> 23); f = x - 1.0f;
  if ((0x007fffff & (0x8000 + ix)) < 0xc000) {
    if (f == 0.0f) { if (k == 0) return 0.0f; dk = (float)k; return dk * ln2_hi + dk * ln2_lo; }
    R = f * f * (0.5f - 0.33333333333333333f * f);
    if (k == 0) return f - R;
    dk = (float)k; return dk * ln2_hi - ((R - dk * ln2_lo) - f);
  }
  s = f / (2.0f + f); dk = (float)k; z = s * s; i = ix - (0x6147a << 3); w = z * z; j = (0x6b851 << 3) - ix;
  t1 = w * (Lg2 + w * Lg4); t2 = z * (Lg1 + w * Lg3); i |= j; R = t2 + t1;
  if (i > 0) {
    hfsq = 0.5f * f * f;
    if (k == 0) return f - (hfsq - s * (hfsq + R));
    return dk * ln2_hi - ((hfsq - (s * (hfsq + R) + dk * ln2_lo)) - f);
  }
  if (k == 0) return f - s * (f - R);
  return dk * ln2_hi - ((s * (f - R) - dk * ln2_lo) - f);
}

/* ---- FFT: Ne10 ne10_fft_r2c_1d_float32_neon, NE10_UNROLL_LEVEL 1, nfft 512 ----
 * Same arithmetic, in the same order, as the NEON intrinsics code, with GCC
 * vector types standing in for float32x4_t (lane-wise IEEE single; ARMv7 vmla
 * and vmls round the product, so -ffp-contract=off). Factors for nfft/4 = 128
 * are radix-8 first, then 4, 4 (stock r_factors_neon: 3,16,4,32,4,8,8,1).
 * Twiddle tables match the stock runtime's tables bit for bit. */
typedef float v4 __attribute__((vector_size(16)));
typedef struct { v4 r, i; } c4;
static float ne_tw[120][2], ne_su[12 + N / 32 * 12][2];
#define NE_PI ((double)(float)PI)                 /* NE10_PI is a float cast */
static const float TW81 = 0.70710678f, TW81N = -0.70710678f;
static void ne10_init(void) {
  int o = 0;
  for (int st = 0; st < 2; st++) {                /* (mstride, fstride) = (8, 4), (32, 1) */
    int ms = st ? 32 : 8, fs = st ? 1 : 4;
    for (int j = 0; j < ms; j++)
      for (int k = 1; k < 4; k++) {
        float ph = -2 * NE_PI * fs * k * j / 128;
        ne_tw[o + 3 * j + k - 1][0] = cos(ph); ne_tw[o + 3 * j + k - 1][1] = sin(ph);
      }
    o += ms * 3;
  }
  const float pi = (float)PI;
  for (int i = 1; i < 4; i++)
    for (int j = 0; j < 4; j++) { float ph = -2 * pi * ((float)(i * j) / N); ne_su[4 * i - 4 + j][0] = cos(ph); ne_su[4 * i - 4 + j][1] = sin(ph); }
  for (int k = 1; k < N / 32; k++)
    for (int s = 1; s < 4; s++)
      for (int j = 0; j < 4; j++) {
        float ph = -2 * pi * ((float)((k * 4 + j) * s) / N);
        ne_su[12 * k + j + 4 * (s - 1)][0] = cos(ph); ne_su[12 * k + j + 4 * (s - 1)][1] = sin(ph);
      }
}
static inline v4 ld(const float *p) { v4 v; memcpy(&v, p, 16); return v; }
static inline void st(float *p, v4 v) { memcpy(p, &v, 16); }
static inline v4 dup(float x) { return (v4){x, x, x, x}; }
static inline c4 cmul(c4 a, c4 b) {               /* NE10_CPX_MUL_NEON_F32 */
  v4 rr = a.r * b.r, ri = a.r * b.i, p = a.i * b.i, q = a.i * b.r; c4 z = {rr - p, ri + q}; return z;
}
static inline v4 rev4(v4 a) { return (v4){a[3], a[2], a[1], a[0]}; }

static void ne_radix8x4(float *out, const float *in) {  /* fstride 16, first stage */
  for (int f = 0; f < 16; f++) {
    v4 q[8], s[8];
    for (int k = 0; k < 8; k++) q[k] = ld(in + 4 * (f + 16 * k));
    s[0] = q[0] + q[4]; s[1] = q[0] - q[4]; s[2] = q[1] + q[5]; s[3] = q[1] - q[5];
    s[4] = q[2] + q[6]; s[5] = q[2] - q[6]; s[6] = q[3] + q[7]; s[7] = q[3] - q[7];
    s[3] = s[3] * dup(TW81); s[7] = s[7] * dup(TW81N);
    v4 S0 = s[0] + s[4], S1 = s[2] + s[6], S2 = s[7] - s[3], S3 = s[3] + s[7];
    v4 o[8] = {S0 + S1, s[1] + S3, S2 - s[5], s[0] - s[4], s[6] - s[2], s[1] - S3, s[5] + S2, S0 - S1};
    for (int k = 0; k < 8; k++) st(out + 4 * (8 * f + k), o[k]);
  }
}
static void ne_radix4x4_tw(float *out, const float *in, int fstride, int ms, const float (*tw)[2]) {
  const int is = 32;                               /* in_step = nfft/4 (float32x4 units) */
  const float *fin = in; float *fo = out;
  for (int f = fstride; f; f--) {
    {                                              /* first butterfly */
      v4 q0 = ld(fin), q1 = ld(fin + 4 * is), q2 = ld(fin + 8 * is), q3 = ld(fin + 12 * is);
      v4 s0 = q0 + q2, s1 = q1 + q3;
      st(fo, s0 + s1); st(fo + 4 * (2 * ms - 1), q0 - q2); st(fo + 4 * (2 * ms), q3 - q1); st(fo + 4 * (4 * ms - 1), s0 - s1);
    }
    fin += 4; fo += 4;
    {                                              /* other butterflies */
      const float *fi = fin, (*t)[2] = tw + 3; float *fr = fo, *fb = fo + 4 * (4 * ms - 4);
      for (int m = ms / 2 - 1; m > 0; m--) {
        c4 in_[4], o[4];
        for (int k = 0; k < 4; k++) { in_[k].r = ld(fi + 4 * k * is); in_[k].i = ld(fi + 4 * k * is + 4); }
        o[0] = in_[0];
        for (int k = 1; k < 4; k++) { c4 w = {dup(t[k - 1][0]), dup(t[k - 1][1])}; o[k] = cmul(in_[k], w); }
        c4 i0 = {o[0].r + o[2].r, o[0].i + o[2].i}, i1 = {o[0].r - o[2].r, o[0].i - o[2].i};
        c4 i2 = {o[1].r + o[3].r, o[1].i + o[3].i}, i3 = {o[1].r - o[3].r, o[1].i - o[3].i};
        c4 r0 = {i0.r + i2.r, i0.i + i2.i}, r2 = {i0.r - i2.r, i2.i - i0.i};
        c4 r1 = {i1.r + i3.i, i1.i - i3.r}, r3 = {i1.r - i3.i, -(i3.r + i1.i)};
        st(fr, r0.r); st(fr + 4, r0.i); st(fr + 4 * 2 * ms, r1.r); st(fr + 4 * (2 * ms + 1), r1.i);
        st(fb, r2.r); st(fb + 4, r2.i); st(fb - 4 * 2 * ms, r3.r); st(fb - 4 * (2 * ms - 1), r3.i);
        fi += 8; fr += 8; fb -= 8; t += 3;
      }
    }
    fin += 4 * 2 * (ms / 2 - 1); fo += 4 * 2 * (ms / 2 - 1);
    {                                              /* last butterfly */
      v4 q0 = ld(fin), q1 = ld(fin + 4 * is), q2 = ld(fin + 8 * is), q3 = ld(fin + 12 * is);
      q1 = q1 * dup(TW81); q3 = q3 * dup(TW81);
      v4 t = q1 - q3; q3 = q1 + q3; q1 = t;
      st(fo, q0 + q1); st(fo + 4, -(q2 + q3)); st(fo + 4 * 2 * ms, q0 - q1); st(fo + 4 * (2 * ms + 1), q2 - q3);
    }
    fin += 4; fo += 4;
    fo += 4 * 3 * ms;
  }
}
typedef struct { float r, i; } cpx;
static void ne_last_stage(cpx *dst, const float *src) {  /* ne10_radix4_r2c_with_twiddles_last_stage, nfft 512 */
  const float (*tw)[2] = ne_su;
  { const float *p = src;                          /* b0: NE10_FFT_R2C_4R_RCR */
    float s0 = p[0] + p[2], s1 = p[1] + p[3];
    dst[0].r = s0 + s1; dst[0].i = s0 - s1; dst[N / 4].r = p[0] - p[2]; dst[N / 4].i = p[3] - p[1]; }
  { const float *p = src + N - 4;                  /* b2: NE10_FFT_R2C_4R_CC */
    float s0 = (p[3] - p[1]) * TW81N, s1 = (p[3] + p[1]) * TW81N;
    dst[N / 8].r = p[0] + s0; dst[N / 8].i = s1 - p[2]; dst[N / 8 + N / 4].r = p[0] - s0; dst[N / 8 + N / 4].i = s1 + p[2]; }
  { cpx co[4], ci[4], t[4], o[4];                  /* b1 */
    for (int k = 0; k < 4; k++) { co[k].r = src[4 + k]; co[k].i = src[8 + k]; }
    ci[0] = co[0];
    for (int k = 1; k < 4; k++) {                  /* NE10_CPX_MUL_F32 */
      float arbr = co[k].r * tw[k][0], aibi = co[k].i * tw[k][1], arbi = co[k].r * tw[k][1], aibr = co[k].i * tw[k][0];
      ci[k].r = arbr - aibi; ci[k].i = aibr + arbi;
    }
    t[0].r = ci[0].r + ci[2].r; t[0].i = ci[0].i + ci[2].i; t[1].r = ci[0].r - ci[2].r; t[1].i = ci[0].i - ci[2].i;
    t[2].r = ci[1].r + ci[3].r; t[2].i = ci[1].i + ci[3].i; t[3].r = ci[1].r - ci[3].r; t[3].i = ci[1].i - ci[3].i;
    { float sw = t[3].i; t[3].i = -t[3].r; t[3].r = sw; }
    o[0].r = t[0].r + t[2].r; o[0].i = t[0].i + t[2].i; o[2].r = t[0].r - t[2].r; o[2].i = -(t[0].i - t[2].i);
    o[1].r = t[1].r + t[3].r; o[1].i = t[1].i + t[3].i; o[3].r = t[1].r - t[3].r; o[3].i = -(t[1].i - t[3].i);
    dst[1] = o[0]; dst[N / 4 + 1] = o[1]; dst[N / 4 - 1] = o[3]; dst[N / 2 - 1] = o[2]; }
  { const float *fin = src + 12, *w = &tw[4][0]; float *fo = (float *)dst;   /* second butterfly */
    float i0[4], i1[4], i2[4], i3[4], o0[4], o1[4], o2[4], o3[4], a0[4], a1[4], b0[4], b1[4];
    for (int k = 0; k < 4; k++) { i0[k] = fin[k]; i1[k] = fin[4 + k]; i2[k] = fin[8 + k]; i3[k] = fin[12 + k]; }
    for (int k = 0; k < 4; k++) { a0[k] = w[2 * k]; a1[k] = w[2 * k + 1]; b0[k] = w[8 + 2 * k]; b1[k] = w[8 + 2 * k + 1]; }
    o0[0] = i0[0]; o1[0] = i1[0]; o2[0] = i2[0]; o3[0] = i3[0];
    for (int k = 1; k < 4; k++) {
      float x, y;
      x = i0[k] * a0[k]; y = i1[k] * a1[k]; o0[k] = x - y; x = i0[k] * a1[k]; y = i1[k] * a0[k]; o1[k] = x + y;
      x = i2[k] * b0[k]; y = i3[k] * b1[k]; o2[k] = x - y; x = i2[k] * b1[k]; y = i3[k] * b0[k]; o3[k] = x + y;
    }
    i0[0] = o0[0] + o0[2]; i1[0] = o1[0] + o1[2]; i0[1] = o0[0] - o0[2]; i1[1] = o1[0] - o1[2];
    i0[2] = o0[1] + o0[3]; i1[2] = o1[1] + o1[3]; i0[3] = o0[1] - o0[3]; i1[3] = o1[1] - o1[3];
    i2[0] = o2[0] + o2[2]; i3[0] = o3[0] + o3[2]; i2[1] = o2[0] - o2[2]; i3[1] = o3[0] - o3[2];
    i2[2] = o2[1] + o2[3]; i3[2] = o3[1] + o3[3]; i2[3] = o2[1] - o2[3]; i3[3] = o3[1] - o3[3];
    o0[0] = i0[0] + i0[2]; o0[1] = i1[0] + i1[2]; o2[2] = i0[0] - i0[2]; o2[3] = -i1[0] + i1[2];
    o3[2] = i0[1] - i1[3]; o3[3] = -i1[1] - i0[3]; o1[0] = i0[1] + i1[3]; o1[1] = i1[1] - i0[3];
    o0[2] = i2[0] + i2[2]; o0[3] = i3[0] + i3[2]; o2[0] = i2[0] - i2[2]; o2[1] = -i3[0] + i3[2];
    o3[0] = i2[1] - i3[3]; o3[1] = -i3[1] - i2[3]; o1[2] = i2[1] + i3[3]; o1[3] = i3[1] - i2[3];
    fo += 4; memcpy(fo, o0, 16); fo += N / 2; memcpy(fo, o1, 16); fo -= 10; memcpy(fo, o3, 16); fo += N / 2; memcpy(fo, o2, 16); }
  { const float *fin = src + 28, *w = &tw[12][0]; float *fr = (float *)dst + 8, *fb = (float *)dst - 14;  /* other */
    for (int l = ((N >> 2) - 8) >> 3; l > 0; l--) {
      v4 a[8]; for (int k = 0; k < 8; k++) a[k] = ld(fin + 4 * k);
      fin += 32;
      /* NE10_RADIX4X4C_TRANSPOSE_NEON of (a0r,a0i),(a1r,a1i),(a2r,a2i),(a3r,a3i) */
      c4 in_[4];
      for (int k = 0; k < 4; k++) {
        in_[k].r = (v4){a[0][k], a[2][k], a[4][k], a[6][k]};
        in_[k].i = (v4){a[1][k], a[3][k], a[5][k], a[7][k]};
      }
      c4 o[4]; o[0] = in_[0];
      for (int k = 1; k < 4; k++) {
        c4 t; for (int j = 0; j < 4; j++) { t.r[j] = w[2 * j]; t.i[j] = w[2 * j + 1]; }
        w += 8; o[k] = cmul(in_[k], t);
      }
      c4 i0 = {o[0].r + o[2].r, o[0].i + o[2].i}, i1 = {o[0].r - o[2].r, o[0].i - o[2].i};
      c4 i2 = {o[1].r + o[3].r, o[1].i + o[3].i}, i3 = {o[1].r - o[3].r, o[1].i - o[3].i};
      c4 r2 = {i0.r - i2.r, -(i0.i - i2.i)}, r3 = {i1.r - i3.i, -(i1.i + i3.r)};
      c4 r0 = {i0.r + i2.r, i0.i + i2.i}, r1 = {i1.r + i3.i, i1.i - i3.r};
      r2.r = rev4(r2.r); r2.i = rev4(r2.i); r3.r = rev4(r3.r); r3.i = rev4(r3.i);
      c4 *outs[4] = {&r0, &r1, &r3, &r2}; float *dsts[4] = {fr, fr + N / 2, fb + N / 2, fb + N};
      for (int q = 0; q < 4; q++) for (int j = 0; j < 4; j++) { dsts[q][2 * j] = outs[q]->r[j]; dsts[q][2 * j + 1] = outs[q]->i[j]; }
      fr += 8; fb -= 8;
    }
  }
}
static void real_fft(float *data) {  /* in place, Kaldi packing [re0, reN/2, re1, im1, ...] */
  float a[N] __attribute__((aligned(16))), b[N + 2] __attribute__((aligned(16)));
  ne_radix8x4(a, data);                            /* Fout = tmpbuf (stage count is odd) */
  ne_radix4x4_tw(b, a, 4, 8, ne_tw);               /* Fout = user buffer */
  ne_radix4x4_tw(a, b, 1, 32, ne_tw + 24);         /* Fout = tmpbuf */
  cpx *out = (cpx *)b;
  ne_last_stage(out, a);
  out[N / 2].r = out[0].i; out[0].i = out[N / 2].i = 0.0f;
  out[0].i = out[N / 2].r;                         /* libpryon wrapper +0xa361ba */
  memcpy(data, b, sizeof(float) * N);
}

static float mel_scale(float f) { return 1127.0f * msun_logf(1.0f + f / 700.0f); }

int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: %s IN.raw OUT.f32\n", argv[0]); return 2; }
  ne10_init();
  FILE *fi = fopen(argv[1], "rb"), *o = fopen(argv[2], "wb");
  if (!fi || !o) { perror("open"); return 1; }
  if (getenv("PRYON_FFT_ONLY")) {
    float fr[N];
    while (fread(fr, sizeof(float), N, fi) == N) { real_fft(fr); fwrite(fr, sizeof(float), N, o); }
    return 0;
  }
  fseek(fi, 0, SEEK_END); long ns = ftell(fi) / 2; rewind(fi);
  short *pcm = malloc(ns * 2);
  if (!pcm || fread(pcm, 2, ns, fi) != (size_t)ns) { fprintf(stderr, "short read\n"); return 1; }

  float win[FL]; double a = 2 * PI / (FL - 1);
  for (int i = 0; i < FL; i++) win[i] = pow(0.5 - 0.5 * cos(a * (double)i), 0.85);

  int first[NB], size[NB]; float wts[NB][256];
  float fft_bin_width = 16000.0f / N, mlo = mel_scale(80), mhi = mel_scale(7200), delta = (mhi - mlo) / (NB + 1);
  for (int b = 0; b < NB; b++) {
    float l = mlo + b * delta, c = mlo + (b + 1) * delta, r = mlo + (b + 2) * delta;
    int fi0 = -1, li = -1;
    for (int i = 0; i < 256; i++) {
      float mel = mel_scale(fft_bin_width * i);
      if (mel > l && mel < r) {
        if (fi0 == -1) fi0 = i;
        wts[b][i - fi0] = mel <= c ? (mel - l) / (c - l) : (r - mel) / (r - c);
        li = i;
      }
    }
    first[b] = fi0; size[b] = li + 1 - fi0;
  }

  FILE *replay = getenv("PRYON_FFT_FROM") ? fopen(getenv("PRYON_FFT_FROM"), "rb") : NULL;
  FILE *dump = getenv("PRYON_DUMP_PRE") ? fopen(getenv("PRYON_DUMP_PRE"), "wb") : NULL;
  float one[FL];
  for (int i = 0; i < FL; i++) one[i] = 1;
  for (long s0 = 0, t = 0; s0 + FL <= ns; s0 += HOP, t++) {
    float w[N];
    for (int i = 0; i < FL; i++) w[i] = pcm[s0 + i];
    float mean = -sdot(w, one, FL) / FL;
    for (int i = 0; i < FL; i++) w[i] += mean;
    for (int i = FL - 1; i > 0; i--) w[i] -= 0.96875f * w[i - 1];
    w[0] -= 0.96875f * w[0];
    for (int i = 0; i < FL; i++) w[i] *= win[i];
    memset(w + FL, 0, sizeof(float) * (N - FL));
    if (dump) fwrite(w, sizeof(float), N, dump);
    if (replay) { fseek(replay, t * N * 4, SEEK_SET); if (fread(w, 4, N, replay) != N) memset(w, 0, sizeof w); }
    else real_fft(w);
    float first_e = w[0] * w[0], last_e = w[1] * w[1];
    for (int i = 1; i < N / 2; i++) { float re = w[i * 2], im = w[i * 2 + 1]; w[i] = re * re + im * im; }
    w[0] = first_e; w[N / 2] = last_e;
    float out[NB];
    for (int b = 0; b < NB; b++) {
      float e = sdot(wts[b], w + first[b], size[b]);
      out[b] = msun_logf(e < 1.0f ? 1.0f : e);
    }
    fwrite(out, sizeof(float), NB, o);
  }
  return 0;
}
