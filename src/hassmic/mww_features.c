/* microWakeWord's features: TFLite Micro's audio front end (tensorflow/lite/experimental/microfrontend/lib) with the
 * settings microWakeWord trains with (ESPHome's preprocessor_settings.h): 30 ms Hann windows every 10 ms, a 512 point
 * fixed point FFT (kissfft, 16 bit), 40 mel channels 125-7500 Hz, noise reduction (smoothing bits 10, even 0.025, odd
 * 0.06, min signal 0.05), PCAN gain control (strength 0.95, offset 80, gain bits 21) and the log scale (shift 6).
 * All integer per window, as the original; its tables in single precision floats as its current version makes them, so
 * the features come out bit for bit as the models were trained on them (tests/unit/mww_ref.py holds it to
 * pymicro-features, which microWakeWord trains with). */
#include "mww.h"
#include <math.h>
#include <string.h>

#define WINDOW_BITS       12
#define FILTERBANK_BITS   12
#define NR_BITS           14            /* noise reduction's fixed point */
#define NR_SMOOTHING_BITS 10
#define PCAN_SNR_BITS     12
#define PCAN_OUTPUT_BITS  6
#define LOG_SCALE_LOG2    16
#define LOG_SEGMENTS_LOG2 7
#define LOG_COEFF         45426         /* ln(2) << 16 */
#define LOG_SCALE_SHIFT   6
#define FFT_N             512           /* real FFT: a 256 point complex one, radix 4 throughout */
#define FFT_C             (FFT_N / 2)

static int msb32(uint32_t x) { return x ? 32 - __builtin_clz(x) : 0; }
static int msb64(uint64_t x) { return x ? 64 - __builtin_clzll(x) : 0; }

/* ---------------------------------------------------------------- kissfft, FIXED_POINT 16 */

struct cpx { int16_t r, i; };
static struct cpx twiddles[FFT_C], super_tw[FFT_C / 2];
static int tables_made;

#define SROUND(x) ((int16_t)(((x) + (1 << 14)) >> 15))
static inline void fixdiv(struct cpx *c, int k) { c->r = SROUND((int32_t)c->r * (32767 / k)); c->i = SROUND((int32_t)c->i * (32767 / k)); }
static inline struct cpx cmul(struct cpx a, struct cpx b)
{
    struct cpx m;
    m.r = SROUND((int32_t)a.r * b.r - (int32_t)a.i * b.i);
    m.i = SROUND((int32_t)a.r * b.i + (int32_t)a.i * b.r);
    return m;
}
static struct cpx cexp_(double phase) { struct cpx c; c.r = (int16_t)floor(.5 + 32767 * cos(phase)); c.i = (int16_t)floor(.5 + 32767 * sin(phase)); return c; }

static void make_tables(void)
{
    const double pi = 3.141592653589793238462643383279502884197169399375105820974944;
    if (tables_made) return;
    for (int i = 0; i < FFT_C; i++) twiddles[i] = cexp_(-2 * pi * i / FFT_C);
    for (int i = 0; i < FFT_C / 2; i++) super_tw[i] = cexp_(-3.14159265358979323846264338327 * ((double)(i + 1) / FFT_C + .5));
    tables_made = 1;
}

static void bfly4(struct cpx *out, size_t fstride, size_t m)
{
    const struct cpx *tw1 = twiddles, *tw2 = twiddles, *tw3 = twiddles;
    struct cpx s[6];
    for (size_t k = 0; k < m; k++, out++) {
        fixdiv(&out[0], 4); fixdiv(&out[m], 4); fixdiv(&out[2 * m], 4); fixdiv(&out[3 * m], 4);
        s[0] = cmul(out[m], *tw1); s[1] = cmul(out[2 * m], *tw2); s[2] = cmul(out[3 * m], *tw3);
        s[5].r = out[0].r - s[1].r; s[5].i = out[0].i - s[1].i;
        out[0].r += s[1].r; out[0].i += s[1].i;
        s[3].r = s[0].r + s[2].r; s[3].i = s[0].i + s[2].i;
        s[4].r = s[0].r - s[2].r; s[4].i = s[0].i - s[2].i;
        out[2 * m].r = out[0].r - s[3].r; out[2 * m].i = out[0].i - s[3].i;
        tw1 += fstride; tw2 += fstride * 2; tw3 += fstride * 3;
        out[0].r += s[3].r; out[0].i += s[3].i;
        out[m].r = s[5].r + s[4].i; out[m].i = s[5].i - s[4].r;
        out[3 * m].r = s[5].r - s[4].i; out[3 * m].i = s[5].i + s[4].r;
    }
}

/* kf_work for 256 = 4 x 4 x 4 x 4: m is the stage's length / 4 */
static void work(struct cpx *out, const struct cpx *f, size_t fstride, size_t m)
{
    struct cpx *beg = out, *end = out + 4 * m;
    if (m == 1) { do { *out = *f; f += fstride; } while (++out != end); }
    else { do { work(out, f, fstride * 4, m / 4); f += fstride; } while ((out += m) != end); }
    bfly4(beg, fstride, m);
}

static void fftr(const int16_t *in, struct cpx *freq)        /* kiss_fftr: FFT_N real samples -> FFT_C + 1 bins */
{
    struct cpx tmp[FFT_C], tdc, fpk, fpnk, f1k, f2k, tw;
    work(tmp, (const struct cpx *)in, 1, FFT_C / 4);
    tdc = tmp[0]; fixdiv(&tdc, 2);
    freq[0].r = (int16_t)(tdc.r + tdc.i); freq[FFT_C].r = (int16_t)(tdc.r - tdc.i);
    freq[FFT_C].i = freq[0].i = 0;
    for (int k = 1; k <= FFT_C / 2; k++) {
        fpk = tmp[k]; fpnk.r = tmp[FFT_C - k].r; fpnk.i = (int16_t)-tmp[FFT_C - k].i;
        fixdiv(&fpk, 2); fixdiv(&fpnk, 2);
        f1k.r = (int16_t)(fpk.r + fpnk.r); f1k.i = (int16_t)(fpk.i + fpnk.i);
        f2k.r = (int16_t)(fpk.r - fpnk.r); f2k.i = (int16_t)(fpk.i - fpnk.i);
        tw = cmul(f2k, super_tw[k - 1]);
        freq[k].r = (int16_t)((f1k.r + tw.r) >> 1); freq[k].i = (int16_t)((f1k.i + tw.i) >> 1);
        freq[FFT_C - k].r = (int16_t)((f1k.r - tw.r) >> 1); freq[FFT_C - k].i = (int16_t)((tw.i - f1k.i) >> 1);
    }
}

/* ---------------------------------------------------------------- set up */

/* The tables are single precision, as TFLite Micro makes them, but from correctly rounded float functions: glibc's are
 * (and with them pymicro-features and microWakeWord's training), bionic's log1pf is not (two filterbank weights came
 * out one step apart, 2026-10-07).  A double function rounded to float is the same on both. */
static float log1p_f(float x) { return (float)log1p((double)x); }
static float cos_f(float x) { return (float)cos((double)x); }
static float pow_f(float x, float y) { return (float)pow((double)x, (double)y); }

static float mel(float hz) { return 1127.0f * log1p_f(hz / 700.0f); }

static int filterbank_init(struct mww_features *f)
{
    enum { N1 = MWW_CHANNELS + 1, ALIGN = 2, BLOCK = 4 };
    const float lower = 125.0f, upper = 7500.0f;
    float center[N1]; int16_t starts[N1], widths[N1];
    const float mel_low = mel(lower), mel_spacing = (mel(upper) - mel_low) / (float)N1;
    for (int i = 0; i < N1; i++) center[i] = mel_low + mel_spacing * (i + 1);
    const float hz_per_bin = 0.5f * 16000 / ((float)(FFT_C + 1) - 1);
    f->start_index = 1.5 + lower / hz_per_bin;
    f->end_index = 0;
    int freq_start = f->start_index, weight_start = 0, zeros = 0;
    for (int c = 0; c < N1; c++) {
        int fi = freq_start;
        while (mel(fi * hz_per_bin) <= center[c]) fi++;
        int width = fi - freq_start;
        starts[c] = (int16_t)freq_start; widths[c] = (int16_t)width;
        if (width == 0) {               /* gets no bins: points at a block of zero weights at the front */
            f->chan_freq_start[c] = 0; f->chan_weight_start[c] = 0; f->chan_width[c] = BLOCK;
            if (!zeros) { zeros = 1; for (int j = 0; j < c; j++) f->chan_weight_start[j] += BLOCK; weight_start += BLOCK; }
        } else {
            int aligned = freq_start / ALIGN * ALIGN, aw = freq_start - aligned + width, padded = ((aw - 1) / BLOCK + 1) * BLOCK;
            f->chan_freq_start[c] = (int16_t)aligned; f->chan_weight_start[c] = (int16_t)weight_start; f->chan_width[c] = (int16_t)padded;
            weight_start += padded;
        }
        freq_start = fi;
    }
    if (weight_start > (int)(sizeof f->weights / sizeof f->weights[0])) return -1;
    memset(f->weights, 0, sizeof f->weights); memset(f->unweights, 0, sizeof f->unweights);
    for (int c = 0; c < N1; c++) {
        int freq = starts[c], off = freq - f->chan_freq_start[c], ws = f->chan_weight_start[c];
        float denom = c == 0 ? mel_low : center[c - 1];
        for (int j = 0; j < widths[c]; j++, freq++) {
            float w = (center[c] - mel(freq * hz_per_bin)) / (center[c] - denom);
            f->weights[ws + off + j] = (int16_t)floorf(w * (1 << FILTERBANK_BITS) + 0.5f);
            f->unweights[ws + off + j] = (int16_t)floorf((1.0f - w) * (1 << FILTERBANK_BITS) + 0.5f);
        }
        if (freq > f->end_index) f->end_index = freq;
    }
    return f->end_index < FFT_C + 1 ? 0 : -1;
}

static int16_t pcan_gain(int input_bits, uint32_t x)
{
    const float strength = 0.95f, offset = 80.0f; const int gain_bits = 21;
    float xf = (float)x / (float)((uint32_t)1 << input_bits);
    float g = (float)((uint32_t)1 << gain_bits) * pow_f(xf + offset, -strength);
    return g > 0x7fff ? 0x7fff : (int16_t)(g + 0.5f);
}

static void pcan_init(struct mww_features *f)
{
    int correction = msb32(FFT_N) - 1 - FILTERBANK_BITS / 2, input_bits = NR_SMOOTHING_BITS - correction;
    int16_t *lut = f->gain_lut;
    f->snr_shift = 21 - correction - PCAN_SNR_BITS;
    lut[0] = pcan_gain(input_bits, 0); lut[1] = pcan_gain(input_bits, 1);
    lut -= 6;
    for (int iv = 2; iv <= 32; iv++) {
        uint32_t x0 = (uint32_t)1 << (iv - 1), x1 = x0 + (x0 >> 1), x2 = iv == 32 ? x0 + (x0 - 1) : 2 * x0;
        int16_t y0 = pcan_gain(input_bits, x0), y1 = pcan_gain(input_bits, x1), y2 = pcan_gain(input_bits, x2);
        int32_t d1 = (int32_t)y1 - y0, d2 = (int32_t)y2 - y0, a1 = 4 * d1 - d2, a2 = d2 - a1;
        lut[4 * iv] = y0; lut[4 * iv + 1] = (int16_t)a1; lut[4 * iv + 2] = (int16_t)a2;
    }
}

int mww_features_init(struct mww_features *f)
{
    memset(f, 0, sizeof *f);
    make_tables();
    const float arg = (float)M_PI * 2.0f / (float)MWW_WINDOW;
    for (int i = 0; i < MWW_WINDOW; i++) f->coef[i] = (int16_t)floorf((0.5f - 0.5f * cos_f(arg * (i + 0.5f))) * (1 << WINDOW_BITS) + 0.5f);
    if (filterbank_init(f)) return -1;
    pcan_init(f);
    return 0;
}

void mww_features_reset(struct mww_features *f) { f->used = 0; memset(f->estimate, 0, sizeof f->estimate); }
void mww_features_free(struct mww_features *f) { (void)f; }

/* ---------------------------------------------------------------- per window */

static uint16_t sqrt32(uint32_t num)
{
    if (!num) return 0;
    uint32_t res = 0; int maxb = (32 - msb32(num)) | 1;
    uint32_t bit = 1U << (31 - maxb); int it = (31 - maxb) / 2 + 1;
    while (it--) { if (num >= res + bit) { num -= res + bit; res = (res >> 1) + bit; } else res >>= 1; bit >>= 2; }
    if (num > res && res != 0xFFFF) ++res;
    return (uint16_t)res;
}

static uint32_t sqrt64(uint64_t num)
{
    if (!(num >> 32)) return sqrt32((uint32_t)num);
    uint64_t res = 0; int maxb = (64 - msb64(num)) | 1;
    uint64_t bit = 1ULL << (63 - maxb); int it = (63 - maxb) / 2 + 1;
    while (it--) { if (num >= res + bit) { num -= res + bit; res = (res >> 1) + bit; } else res >>= 1; bit >>= 2; }
    if (num > res && res != 0xFFFFFFFFLL) ++res;
    return (uint32_t)res;
}

/* log_lut.h: (log2(1 + k/128) - k/128) << 16 */
static const uint16_t log_lut[] = {
    0,    224,  442,  654,  861,  1063, 1259, 1450, 1636, 1817, 1992, 2163, 2329, 2490, 2646, 2797, 2944, 3087, 3224,
    3358, 3487, 3611, 3732, 3848, 3960, 4068, 4172, 4272, 4368, 4460, 4549, 4633, 4714, 4791, 4864, 4934, 5001, 5063,
    5123, 5178, 5231, 5280, 5326, 5368, 5408, 5444, 5477, 5507, 5533, 5557, 5578, 5595, 5610, 5622, 5631, 5637, 5640,
    5641, 5638, 5633, 5626, 5615, 5602, 5586, 5568, 5547, 5524, 5498, 5470, 5439, 5406, 5370, 5332, 5291, 5249, 5203,
    5156, 5106, 5054, 5000, 4944, 4885, 4825, 4762, 4697, 4630, 4561, 4490, 4416, 4341, 4264, 4184, 4103, 4020, 3935,
    3848, 3759, 3668, 3575, 3481, 3384, 3286, 3186, 3084, 2981, 2875, 2768, 2659, 2549, 2437, 2323, 2207, 2090, 1971,
    1851, 1729, 1605, 1480, 1353, 1224, 1094, 963,  830,  695,  559,  421,  282,  142,  0 };

static uint32_t log_scaled(uint32_t x)
{
    uint32_t integer = (uint32_t)msb32(x) - 1;
    int32_t frac = (int32_t)(x - (1LL << integer));
    if (integer < LOG_SCALE_LOG2) frac <<= LOG_SCALE_LOG2 - integer; else frac >>= integer - LOG_SCALE_LOG2;
    uint32_t base = (uint32_t)frac >> (LOG_SCALE_LOG2 - LOG_SEGMENTS_LOG2), unit = (1U << LOG_SCALE_LOG2) >> LOG_SEGMENTS_LOG2;
    int32_t c0 = log_lut[base], c1 = log_lut[base + 1], seg = (int32_t)(unit * base), rel = ((c1 - c0) * (frac - seg)) >> LOG_SCALE_LOG2;
    uint32_t fraction = (uint32_t)(frac + c0 + rel), log2 = (integer << LOG_SCALE_LOG2) + fraction, round = 1U << (LOG_SCALE_LOG2 - 1);
    uint32_t loge = (uint32_t)(((uint64_t)LOG_COEFF * log2 + round) >> LOG_SCALE_LOG2);
    return ((loge << LOG_SCALE_SHIFT) + round) >> LOG_SCALE_LOG2;
}

static int16_t wide_dynamic(uint32_t x, const int16_t *lut)
{
    if (x <= 2) return lut[x];
    int16_t iv = (int16_t)msb32(x);
    lut += 4 * iv - 6;
    int16_t frac = (int16_t)(((iv < 11) ? (x << (11 - iv)) : (x >> (iv - 11))) & 0x3FF);
    int32_t r = ((int32_t)lut[2] * frac) >> 5;
    r += (int32_t)((uint32_t)lut[1] << 5);
    r *= frac;
    r = (r + (1 << 14)) >> 15;
    r += lut[0];
    return (int16_t)r;
}

size_t mww_features_run(struct mww_features *f, const int16_t *s, size_t n, uint16_t out[MWW_CHANNELS], int *done)
{
    int16_t win[FFT_N]; struct cpx freq[FFT_C + 1]; int32_t energy[FFT_C + 1]; uint64_t work[MWW_CHANNELS + 1]; uint32_t sig[MWW_CHANNELS];
    size_t take = (size_t)(MWW_WINDOW - f->used);
    if (take > n) take = n;
    memcpy(f->window + f->used, s, take * sizeof *s);
    f->used += (int)take;
    *done = 0;
    if (f->used < MWW_WINDOW) return take;

    /* window, scaled up for the FFT's resolution as far as the loudest sample allows */
    int16_t maxabs = 0;
    for (int i = 0; i < MWW_WINDOW; i++) {
        int16_t v = (int16_t)(((int32_t)f->window[i] * f->coef[i]) >> WINDOW_BITS);
        win[i] = v;
        if (v < 0) v = (int16_t)-v;
        if (v > maxabs) maxabs = v;
    }
    memmove(f->window, f->window + MWW_STEP, sizeof f->window[0] * (MWW_WINDOW - MWW_STEP));
    f->used -= MWW_STEP;
    int shift = 15 - msb32((uint32_t)maxabs);
    for (int i = 0; i < MWW_WINDOW; i++) win[i] = (int16_t)((uint16_t)win[i] << shift);
    for (int i = MWW_WINDOW; i < FFT_N; i++) win[i] = 0;
    fftr(win, freq);

    /* filterbank: energies, weighted into channels; the bins below start_index have zero weights */
    memset(energy, 0, sizeof energy);
    for (int i = f->start_index; i < f->end_index; i++) {
        int32_t re = freq[i].r, im = freq[i].i;
        energy[i] = (int32_t)(uint32_t)(re * re + im * im);
    }
    uint64_t wacc = 0, uacc = 0;
    for (int c = 0; c < MWW_CHANNELS + 1; c++) {
        const int32_t *mag = energy + f->chan_freq_start[c];
        const int16_t *w = f->weights + f->chan_weight_start[c], *u = f->unweights + f->chan_weight_start[c];
        for (int j = 0; j < f->chan_width[c]; j++) { wacc += (uint64_t)w[j] * (uint64_t)(int64_t)mag[j]; uacc += (uint64_t)u[j] * (uint64_t)(int64_t)mag[j]; }
        work[c] = wacc; wacc = uacc; uacc = 0;
    }
    for (int c = 0; c < MWW_CHANNELS; c++) sig[c] = sqrt64(work[c + 1]) >> shift;

    /* noise reduction */
    const uint32_t even = (uint32_t)(0.025 * (1 << NR_BITS)), odd = (uint32_t)(0.06 * (1 << NR_BITS)), minrem = (uint32_t)(0.05 * (1 << NR_BITS));
    for (int c = 0; c < MWW_CHANNELS; c++) {
        uint32_t sm = (c & 1) ? odd : even, up = sig[c] << NR_SMOOTHING_BITS;
        uint32_t est = (uint32_t)(((uint64_t)up * sm + (uint64_t)f->estimate[c] * ((1 << NR_BITS) - sm)) >> NR_BITS);
        f->estimate[c] = est;
        if (est > up) est = up;
        uint32_t floor_ = (uint32_t)(((uint64_t)sig[c] * minrem) >> NR_BITS), sub = (up - est) >> NR_SMOOTHING_BITS;
        sig[c] = sub > floor_ ? sub : floor_;
    }
    /* PCAN */
    for (int c = 0; c < MWW_CHANNELS; c++) {
        uint32_t gain = (uint32_t)wide_dynamic(f->estimate[c], f->gain_lut);
        uint32_t snr = (uint32_t)(((uint64_t)sig[c] * gain) >> f->snr_shift);
        sig[c] = snr < (2 << PCAN_SNR_BITS) ? (snr * snr) >> (2 + 2 * PCAN_SNR_BITS - PCAN_OUTPUT_BITS)
                                            : (snr >> (PCAN_SNR_BITS - PCAN_OUTPUT_BITS)) - (1 << PCAN_OUTPUT_BITS);
    }
    /* log */
    int correction = msb32(FFT_N) - 1 - FILTERBANK_BITS / 2;
    for (int c = 0; c < MWW_CHANNELS; c++) {
        uint32_t v = correction < 0 ? sig[c] >> -correction : sig[c] << correction;
        v = v > 1 ? log_scaled(v) : 0;
        out[c] = (uint16_t)(v < 0xFFFF ? v : 0xFFFF);
    }
    *done = 1;
    return take;
}
