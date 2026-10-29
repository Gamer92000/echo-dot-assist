/* Where the wake word ends: see wakealign.h.  The reference is tools/wakealign/walign.py (numpy), which this follows
 * step by step; tests/unit/wakealign_test.c holds the two to each other. */
#include "wakealign.h"
#include "statefile.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FFT_N 512
#define MAXCEP 16
#define MAXMEL 40
#define MAXDIM 64

struct wakealign {
    int ncep, nmel, flen, fshift, lctx, rctx;
    float low, high, preemph, lifter;
    int lda_rows, lda_cols; float *lda;                /* rows x cols, cols = ncep * (lctx + 1 + rctx) */
    int npdf, dim; int *goff; float *gconst, *mi, *iv; /* per pdf: gaussians goff[p] .. goff[p + 1] */
    int nstates; int *pdf, *aoff, *adest; float *aw;   /* arcs of state s: aoff[s] .. aoff[s + 1] */
    int *poff, *psrc; float *pw;                        /* the same arcs by destination (predecessors) */
    int nentry; int *estate; float *ew;
    float *final; int nlast; int *last; float ascale;
    uint64_t space;                                     /* FNV-1a of the LDA: the feature space (wakealign_space) */
    float banks[MAXMEL][FFT_N / 2 + 1], dct[MAXCEP][MAXMEL], lift[MAXCEP], win[1024];
    float cw[FFT_N / 2], sw[FFT_N / 2]; int rev[FFT_N];
};

static int rd(FILE *f, void *p, size_t n) { return fread(p, 1, n, f) == n; }

static float half(uint16_t h)                       /* IEEE 754 binary16 */
{
    int e = h >> 10 & 31, m = h & 1023; float v;
    if (e == 0) v = ldexpf((float)m, -24);
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = ldexpf((float)(m | 1024), e - 25);
    return h & 0x8000 ? -v : v;
}

static uint32_t fnv32(uint32_t h, const void *p, size_t n) { const unsigned char *b = p; while (n--) h = (h ^ *b++) * 16777619u; return h; }

static int rd_half(FILE *f, float *out, size_t n, uint32_t *h)   /* n float16s into floats, their bytes into h */
{
    uint16_t buf[256];
    while (n) {
        size_t k = n < 256 ? n : 256;
        if (!rd(f, buf, k * 2)) return 0;
        *h = fnv32(*h, buf, k * 2);
        for (size_t i = 0; i < k; i++) out[i] = half(buf[i]);
        out += k; n -= k;
    }
    return 1;
}
static int ru(FILE *f, int *v) { uint32_t u; if (!rd(f, &u, 4)) return 0; *v = (int)u; return 1; }

static float mel(float f) { return 1127.0f * logf(1.0f + f / 700.0f); }

static void tables(struct wakealign *w)
{
    /* Kaldi's MelBanks: bins spaced evenly in mel between low and high, weights on the FFT bins 0 .. N/2 - 1 */
    float ml = mel(w->low), mh = mel(w->high), d = (mh - ml) / (w->nmel + 1), bw = 16000.0f / FFT_N;
    memset(w->banks, 0, sizeof w->banks);
    for (int b = 0; b < w->nmel; b++) {
        float l = ml + b * d, c = ml + (b + 1) * d, r = ml + (b + 2) * d;
        for (int i = 0; i < FFT_N / 2; i++) {
            float m = mel(bw * i);
            if (m > l && m < r) w->banks[b][i] = m <= c ? (m - l) / (c - l) : (r - m) / (r - c);
        }
    }
    for (int k = 0; k < w->ncep; k++)
        for (int j = 0; j < w->nmel; j++)
            w->dct[k][j] = (float)(k ? sqrt(2.0 / w->nmel) * cos(M_PI / w->nmel * (j + 0.5) * k) : sqrt(1.0 / w->nmel));
    for (int k = 0; k < w->ncep; k++) w->lift[k] = 1.0f + 0.5f * w->lifter * sinf((float)M_PI * k / w->lifter);
    for (int i = 0; i < w->flen; i++) w->win[i] = (float)pow(0.5 - 0.5 * cos(2 * M_PI * i / (w->flen - 1)), 0.85);   /* Povey */
    for (int i = 0; i < FFT_N / 2; i++) { w->cw[i] = (float)cos(2 * M_PI * i / FFT_N); w->sw[i] = (float)-sin(2 * M_PI * i / FFT_N); }
    for (int i = 0; i < FFT_N; i++) {
        int r = 0;
        for (int b = 1, x = i; b < FFT_N; b <<= 1, x >>= 1) r = r << 1 | (x & 1);
        w->rev[i] = r;
    }
}

/* A word's file (version 3) names the shared file of its acoustic model next to it (walign.py write_shared: feature
 * settings, LDA, every GMM the words of that model use, one copy each) and the GMMs it takes from there by place, each
 * with a check of its bytes, so a shared file made anew for other words is refused rather than read wrong.  Only the
 * word's GMMs are read: as much memory as a file of its own took (~200 KB as floats) */
static int shared_name(const char *s)              /* a plain file name: no path, nothing hidden */
{
    size_t n = strlen(s);
    if (n < 5 || s[0] == '.' || strcmp(s + n - 4, ".was")) return 0;
    for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_' || *s == '-' || *s == '.')) return 0;
    return 1;
}

struct wakealign *wakealign_load(const char *path)
{
    FILE *f = fopen(path, "rb"), *s = NULL;
    if (!f) return NULL;
    struct wakealign *w = calloc(1, sizeof *w);
    char magic[4], name[64], sp[600] = ""; int ver = 0, nl = 0, nall = 0; uint32_t *idx = NULL; long size = 0;
    int ok = w && rd(f, magic, 4) && !memcmp(magic, "HMWA", 4) && ru(f, &ver) && ver == 3;
    ok = ok && ru(f, &nl) && nl > 0 && nl < (int)sizeof name && rd(f, name, (size_t)nl);
    if (ok) { name[nl] = 0; ok = shared_name(name); }
    if (ok) {
        const char *slash = strrchr(path, '/');
        snprintf(sp, sizeof sp, "%.*s%s", slash ? (int)(slash - path + 1) : 0, path, name);
        ok = (s = fopen(sp, "rb")) != NULL && !fseek(s, 0, SEEK_END) && (size = ftell(s)) > 0 && !fseek(s, 0, SEEK_SET);
    }
    ok = ok && rd(s, magic, 4) && !memcmp(magic, "HMWS", 4) && ru(s, &ver) && ver == 1;
    ok = ok && ru(s, &w->ncep) && ru(s, &w->nmel) && ru(s, &w->flen) && ru(s, &w->fshift) && ru(s, &w->lctx) && ru(s, &w->rctx)
         && rd(s, &w->low, 4) && rd(s, &w->high, 4) && rd(s, &w->preemph, 4) && rd(s, &w->lifter, 4);
    ok = ok && w->ncep > 0 && w->ncep <= MAXCEP && w->nmel > 0 && w->nmel <= MAXMEL && w->flen > 0 && w->flen <= FFT_N
         && w->fshift > 0 && w->lctx >= 0 && w->rctx >= 0 && w->lctx + w->rctx < 16;
    ok = ok && ru(s, &w->lda_rows) && ru(s, &w->lda_cols) && w->lda_rows > 0 && w->lda_rows <= MAXDIM
         && w->lda_cols == w->ncep * (w->lctx + 1 + w->rctx);
    if (ok) { w->lda = malloc(sizeof(float) * w->lda_rows * w->lda_cols); ok = w->lda && rd(s, w->lda, sizeof(float) * w->lda_rows * w->lda_cols); }
    ok = ok && ru(s, &nall) && ru(s, &w->dim) && nall > 0 && nall < 65536 && w->dim == w->lda_rows;
    if (ok) { idx = malloc(sizeof(uint32_t) * 2 * (size_t)nall); ok = idx && rd(s, idx, sizeof(uint32_t) * 2 * (size_t)nall); }
    ok = ok && ru(f, &w->npdf) && w->npdf > 0 && w->npdf < 4096;
    if (ok) {
        w->goff = calloc(w->npdf + 1, sizeof *w->goff);
        int cap = 0, n = 0; ok = w->goff != NULL;
        for (int p = 0; ok && p < w->npdf; p++) {
            int k, chk; ok = ru(f, &k) && ru(f, &chk) && k >= 0 && k < nall;
            if (!ok) break;
            int g = (int)idx[2 * k]; long at = (long)idx[2 * k + 1];
            ok = g > 0 && g <= 512 && at >= 0 && at <= size && (long)g * (4 + 4 * w->dim) <= size - at;  /* gmm_loglike sums at most 512 */
            if (ok && n + g > cap) {
                cap = (n + g) * 2;
                w->gconst = realloc(w->gconst, sizeof(float) * cap); w->mi = realloc(w->mi, sizeof(float) * cap * w->dim);
                w->iv = realloc(w->iv, sizeof(float) * cap * w->dim);
                ok = w->gconst && w->mi && w->iv;
            }
            uint32_t h = 2166136261u;
            ok = ok && !fseek(s, at, SEEK_SET) && rd(s, w->gconst + n, sizeof(float) * g);
            if (ok) h = fnv32(h, w->gconst + n, sizeof(float) * g);
            ok = ok && rd_half(s, w->mi + (size_t)n * w->dim, (size_t)g * w->dim, &h) && rd_half(s, w->iv + (size_t)n * w->dim, (size_t)g * w->dim, &h)
                 && h == (uint32_t)chk;
            n += g; w->goff[p + 1] = n;
        }
    }
    free(idx);
    if (s) fclose(s);
    ok = ok && ru(f, &w->nstates) && w->nstates > 0 && w->nstates < 4096;
    if (ok) {
        int cap = w->nstates * 4, n = 0;
        w->pdf = malloc(sizeof(int) * w->nstates); w->aoff = calloc(w->nstates + 1, sizeof(int));
        w->adest = malloc(sizeof(int) * cap); w->aw = malloc(sizeof(float) * cap);
        ok = w->pdf && w->aoff && w->adest && w->aw;
        for (int s = 0; ok && s < w->nstates; s++) {
            int na; ok = ru(f, &w->pdf[s]) && w->pdf[s] >= 0 && w->pdf[s] < w->npdf && ru(f, &na) && na >= 0 && na < 4096;
            if (ok && n + na > cap) { cap = (n + na) * 2; w->adest = realloc(w->adest, sizeof(int) * cap); w->aw = realloc(w->aw, sizeof(float) * cap); ok = w->adest && w->aw; }
            for (int a = 0; ok && a < na; a++, n++) ok = ru(f, &w->adest[n]) && w->adest[n] >= 0 && w->adest[n] < w->nstates && rd(f, &w->aw[n], 4);
            w->aoff[s + 1] = n;
        }
    }
    ok = ok && ru(f, &w->nentry) && w->nentry > 0 && w->nentry <= w->nstates;
    if (ok) {
        w->estate = malloc(sizeof(int) * w->nentry); w->ew = malloc(sizeof(float) * w->nentry);
        ok = w->estate && w->ew;
        for (int i = 0; ok && i < w->nentry; i++) ok = ru(f, &w->estate[i]) && w->estate[i] >= 0 && w->estate[i] < w->nstates && rd(f, &w->ew[i], 4);
    }
    if (ok) { w->final = malloc(sizeof(float) * w->nstates); ok = w->final && rd(f, w->final, sizeof(float) * w->nstates); }
    ok = ok && ru(f, &w->nlast) && w->nlast > 0 && w->nlast <= w->nstates;
    if (ok) {
        w->last = malloc(sizeof(int) * w->nlast); ok = w->last != NULL;
        for (int i = 0; ok && i < w->nlast; i++) ok = ru(f, &w->last[i]) && w->last[i] >= 0 && w->last[i] < w->nstates;
    }
    ok = ok && rd(f, &w->ascale, 4);
    fclose(f);
    if (ok) {                                       /* the arcs by destination, for the Viterbi */
        int na = w->aoff[w->nstates];
        w->poff = calloc(w->nstates + 1, sizeof(int)); w->psrc = malloc(sizeof(int) * (na + 1)); w->pw = malloc(sizeof(float) * (na + 1));
        ok = w->poff && w->psrc && w->pw;
        for (int a = 0; ok && a < na; a++) w->poff[w->adest[a] + 1]++;
        for (int s = 0; ok && s < w->nstates; s++) w->poff[s + 1] += w->poff[s];
        int *fill = ok ? calloc(w->nstates, sizeof(int)) : NULL; ok = ok && fill;
        for (int s = 0; ok && s < w->nstates; s++)
            for (int a = w->aoff[s]; a < w->aoff[s + 1]; a++) {
                int d = w->adest[a], k = w->poff[d] + fill[d]++;
                w->psrc[k] = s; w->pw[k] = w->aw[a];
            }
        free(fill);
    }
    if (!ok) { fprintf(stderr, "wakealign: %s: not an aligner model%s%s\n", path, sp[0] ? " with " : "", sp); wakealign_free(w); return NULL; }
    tables(w);
    w->space = 1469598103934665603ULL;
    for (size_t i = 0; i < sizeof(float) * w->lda_rows * w->lda_cols; i++) w->space = (w->space ^ ((unsigned char *)w->lda)[i]) * 1099511628211ULL;
    return w;
}

void wakealign_free(struct wakealign *w)
{
    if (!w) return;
    free(w->lda); free(w->goff); free(w->gconst); free(w->mi); free(w->iv); free(w->pdf); free(w->aoff); free(w->adest);
    free(w->aw); free(w->poff); free(w->psrc); free(w->pw); free(w->estate); free(w->ew); free(w->final); free(w->last); free(w);
}

static void fft(const struct wakealign *w, float *re, float *im)
{
    for (int i = 0; i < FFT_N; i++) if (i < w->rev[i]) { float t = re[i]; re[i] = re[w->rev[i]]; re[w->rev[i]] = t; t = im[i]; im[i] = im[w->rev[i]]; im[w->rev[i]] = t; }
    for (int len = 2; len <= FFT_N; len <<= 1) {
        int half = len / 2, step = FFT_N / len;
        for (int i = 0; i < FFT_N; i += len)
            for (int k = 0; k < half; k++) {
                float wr = w->cw[k * step], wi = w->sw[k * step];
                int a = i + k, b = a + half;
                float tr = re[b] * wr - im[b] * wi, ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr; im[b] = im[a] - ti; re[a] += tr; im[a] += ti;
            }
    }
}

/* Kaldi's MFCC with snip_edges false: frame t centred on sample t * shift + shift / 2, the signal reflected at its ends */
static void mfcc(const struct wakealign *w, const int16_t *pcm, long n, long t, float *out)
{
    float re[FFT_N], im[FFT_N], fb[MAXMEL]; double mean = 0;
    long start = t * w->fshift + w->fshift / 2 - w->flen / 2;
    for (int i = 0; i < w->flen; i++) {
        long j = start + i;
        if (j < 0) j = -j - 1;
        if (j >= n) j = 2 * n - 1 - j;
        if (j < 0) j = 0;
        re[i] = pcm[j]; mean += re[i];
    }
    mean /= w->flen;
    for (int i = 0; i < w->flen; i++) re[i] -= (float)mean;
    for (int i = w->flen - 1; i > 0; i--) re[i] -= w->preemph * re[i - 1];
    re[0] -= w->preemph * re[0];
    for (int i = 0; i < FFT_N; i++) { re[i] = i < w->flen ? re[i] * w->win[i] : 0; im[i] = 0; }
    fft(w, re, im);
    for (int b = 0; b < w->nmel; b++) {
        double e = 0;
        for (int i = 0; i < FFT_N / 2; i++) if (w->banks[b][i] > 0) e += w->banks[b][i] * ((double)re[i] * re[i] + (double)im[i] * im[i]);
        fb[b] = logf(e > 1.1920929e-07 ? (float)e : 1.1920929e-07f);
    }
    for (int k = 0; k < w->ncep; k++) {
        float c = 0;
        for (int j = 0; j < w->nmel; j++) c += w->dct[k][j] * fb[j];
        out[k] = c * w->lift[k];
    }
}

static float gmm_loglike(const struct wakealign *w, int p, const float *x)
{
    float best = -INFINITY, acc = 0;
    int a = w->goff[p], b = w->goff[p + 1];
    float ll[512]; int n = 0;
    for (int g = a; g < b && n < 512; g++, n++) {
        const float *m = w->mi + (size_t)g * w->dim, *v = w->iv + (size_t)g * w->dim;
        float s = w->gconst[g];
        for (int d = 0; d < w->dim; d++) s += x[d] * (m[d] - 0.5f * x[d] * v[d]);
        ll[n] = s; if (s > best) best = s;
    }
    for (int i = 0; i < n; i++) acc += expf(ll[i] - best);
    return best + logf(acc);
}

uint64_t wakealign_space(const struct wakealign *w) { return w->dim == WAKEALIGN_ADIM ? w->space : 0; }

/* the window's statistics into ad (walign.py Adapt.add): along the path, each frame's Gaussian posteriors in its pdf
 * (on the features as aligned, fx), summed per frame before they go in: K_i += (sum post mi_i) [x; 1], G_i +=
 * (sum post iv_i) [x; 1] [x; 1]', beta += sum post, x the features as they came (feat) */
static void accumulate(const struct wakealign *w, const float *feat, const float *fx, const short *path, long T, struct wakealign_adapt *ad)
{
    const int D = WAKEALIGN_ADIM;
    for (int i = 0; i < D; i++) {
        for (int j = 0; j <= D; j++) { ad->k[i][j] *= WAKEALIGN_FORGET; for (int l = j; l <= D; l++) ad->g[i][j][l] *= WAKEALIGN_FORGET; }
    }
    ad->beta *= WAKEALIGN_FORGET;
    for (long t = 0; t < T; t++) {
        int p = w->pdf[path[t]], a = w->goff[p], b = w->goff[p + 1];
        const float *x = fx + t * D; float ll[512], best = -INFINITY, sum = 0;
        for (int g = a; g < b; g++) {
            const float *m = w->mi + (size_t)g * D, *v = w->iv + (size_t)g * D; float s = w->gconst[g];
            for (int d = 0; d < D; d++) s += x[d] * (m[d] - 0.5f * x[d] * v[d]);
            ll[g - a] = s; if (s > best) best = s;
        }
        for (int g = 0; g < b - a; g++) { ll[g] = expf(ll[g] - best); sum += ll[g]; }
        double sk[WAKEALIGN_ADIM] = { 0 }, si[WAKEALIGN_ADIM] = { 0 }, sb = 0, xi[WAKEALIGN_ADIM + 1];
        for (int g = 0; g < b - a; g++) {
            double post = ll[g] / sum;
            if (post <= 1e-3) continue;
            const float *m = w->mi + (size_t)(a + g) * D, *v = w->iv + (size_t)(a + g) * D;
            for (int d = 0; d < D; d++) { sk[d] += post * m[d]; si[d] += post * v[d]; }
            sb += post;
        }
        for (int d = 0; d < D; d++) xi[d] = feat[t * D + d];
        xi[D] = 1;
        for (int i = 0; i < D; i++)
            for (int j = 0; j <= D; j++) {
                ad->k[i][j] += sk[i] * xi[j];
                double f = si[i] * xi[j];
                for (int l = j; l <= D; l++) ad->g[i][j][l] += f * xi[l];      /* G_i is symmetric: its upper half */
            }
        ad->beta += sb;
    }
    ad->requests++;
}

static int invert(const double *a, double *inv, int n)   /* Gauss-Jordan with partial pivoting; 0: singular */
{
    double m[(WAKEALIGN_ADIM + 1) * (WAKEALIGN_ADIM + 1)];
    memcpy(m, a, sizeof(double) * n * n);
    for (int i = 0; i < n * n; i++) inv[i] = i / n == i % n;
    for (int c = 0; c < n; c++) {
        int piv = c;
        for (int r = c + 1; r < n; r++) if (fabs(m[r * n + c]) > fabs(m[piv * n + c])) piv = r;
        if (fabs(m[piv * n + c]) < 1e-300) return 0;
        if (piv != c) for (int k = 0; k < n; k++) {
            double t = m[c * n + k]; m[c * n + k] = m[piv * n + k]; m[piv * n + k] = t;
            t = inv[c * n + k]; inv[c * n + k] = inv[piv * n + k]; inv[piv * n + k] = t;
        }
        double d = m[c * n + c];
        for (int k = 0; k < n; k++) { m[c * n + k] /= d; inv[c * n + k] /= d; }
        for (int r = 0; r < n; r++) {
            if (r == c || m[r * n + c] == 0) continue;
            double f = m[r * n + c];
            for (int k = 0; k < n; k++) { m[r * n + k] -= f * m[c * n + k]; inv[r * n + k] -= f * inv[c * n + k]; }
        }
    }
    return 1;
}

int wakealign_estimate(struct wakealign_adapt *ad)
{
    enum { D = WAKEALIGN_ADIM, E = WAKEALIGN_ADIM + 1 };
    if (ad->requests < WAKEALIGN_ADAPT_MIN) { ad->have = 0; return 0; }
    static double ginv[D][E * E], W[D][E], A[D * D], Ai[D * D], tmp[E * E];   /* one estimate at a time (main.c's worker) */
    for (int i = 0; i < D; i++) {
        for (int j = 0; j < E; j++) for (int l = 0; l < E; l++) tmp[j * E + l] = (j <= l ? ad->g[i][j][l] : ad->g[i][l][j]) + (j == l ? 1e-6 : 0);
        if (!invert(tmp, ginv[i], E)) return 0;
    }
    for (int i = 0; i < D; i++) for (int j = 0; j < E; j++) W[i][j] = ad->have ? ad->xform[i][j] : (double)(i == j);
    for (int it = 0; it < 10; it++) {
        /* A^-1 inverted afresh each round, between rows kept by Sherman-Morrison (row i of A changes by d: A^-1 -=
         * A^-1 e_i d' A^-1 / (1 + d' A^-1 e_i)): 10 inversions rather than 400 (0.4 s a request under qemu-arm) */
        for (int r = 0; r < D; r++) for (int q = 0; q < D; q++) A[r * D + q] = W[r][q];
        if (!invert(A, Ai, D)) return 0;
        for (int i = 0; i < D; i++) {
            double c[E], gc[E], gk[E], e1 = 0, e2 = 0, d[D], u[D], v[D];
            for (int j = 0; j < D; j++) c[j] = Ai[j * D + i];          /* the cofactor row, up to det(A) (it cancels) */
            c[D] = 0;
            for (int j = 0; j < E; j++) {
                gc[j] = gk[j] = 0;
                for (int l = 0; l < E; l++) { gc[j] += ginv[i][j * E + l] * c[l]; gk[j] += ginv[i][j * E + l] * ad->k[i][l]; }
            }
            for (int j = 0; j < E; j++) { e1 += c[j] * gc[j]; e2 += c[j] * gk[j]; }
            double disc = e2 * e2 + 4 * e1 * ad->beta;
            if (!(e1 > 0) || !(disc >= 0)) return 0;
            double alpha = (-e2 + sqrt(disc)) / (2 * e1);
            for (int j = 0; j < E; j++) {
                double nv = 0;
                for (int l = 0; l < E; l++) nv += (alpha * c[l] + ad->k[i][l]) * ginv[i][l * E + j];
                if (j < D) d[j] = nv - W[i][j];
                W[i][j] = nv;
            }
            double den = 1;
            for (int q = 0; q < D; q++) { u[q] = Ai[q * D + i]; v[q] = 0; }    /* u = A^-1 e_i, v' = d' A^-1 */
            for (int q = 0; q < D; q++) for (int r = 0; r < D; r++) v[q] += d[r] * Ai[r * D + q];
            for (int r = 0; r < D; r++) den += d[r] * u[r];
            if (fabs(den) < 1e-12) return 0;
            for (int r = 0; r < D; r++) for (int q = 0; q < D; q++) Ai[r * D + q] -= u[r] * v[q] / den;
        }
    }
    for (int i = 0; i < D; i++) for (int j = 0; j < E; j++) if (!isfinite(W[i][j])) return 0;
    for (int i = 0; i < D; i++) for (int j = 0; j < E; j++) ad->xform[i][j] = (float)W[i][j];
    ad->have = 1;
    return 1;
}

#define ADAPT_MAGIC "HMWF\1\0\0\0"
int wakealign_adapt_save(const struct wakealign_adapt *ad, const char *path)
{
    size_t n = 8 + sizeof *ad; unsigned char *b = malloc(n);
    if (!b) return -1;
    memcpy(b, ADAPT_MAGIC, 8); memcpy(b + 8, ad, sizeof *ad);   /* the struct as this build lays it out: the same build reads it */
    int r = statefile_write(path, 0600, b, n);
    free(b);
    return r;
}

int wakealign_adapt_load(struct wakealign_adapt *ad, const char *path)
{
    FILE *f = fopen(path, "rb"); char magic[8]; uint64_t space = ad->space;
    if (!f) return -1;
    int ok = fread(magic, 1, 8, f) == 8 && !memcmp(magic, ADAPT_MAGIC, 8) && fread(ad, sizeof *ad, 1, f) == 1 && fgetc(f) == EOF
             && ad->space == space && ad->requests >= 0 && isfinite(ad->beta);
    fclose(f);
    if (!ok) { memset(ad, 0, sizeof *ad); ad->space = space; return -1; }
    return 0;
}

long wakealign_end(const struct wakealign *w, const int16_t *pcm, size_t len) { return wakealign_end_adapt(w, pcm, len, NULL, NULL); }

long wakealign_end_adapt(const struct wakealign *w, const int16_t *pcm, size_t len, const struct wakealign_adapt *use, struct wakealign_adapt *add)
{
    long n = (long)len, T = (n + w->fshift / 2) / w->fshift;
    int C = w->ncep, ctx = w->lctx + 1 + w->rctx, S = w->nstates;
    if (T < ctx + 2) return -1;
    float *cep = malloc(sizeof(float) * T * C), *feat = malloc(sizeof(float) * T * w->dim), *delta = malloc(sizeof(float) * S * 2);
    float *ll = malloc(sizeof(float) * w->npdf), *orig = NULL; short *back = malloc(sizeof(short) * T * S), *path = NULL;
    long result = -1;
    if (!cep || !feat || !delta || !ll || !back) goto out;
    double mean[MAXCEP] = { 0 };
    for (long t = 0; t < T; t++) { mfcc(w, pcm, n, t, cep + t * C); for (int k = 0; k < C; k++) mean[k] += cep[t * C + k]; }
    for (long t = 0; t < T; t++) for (int k = 0; k < C; k++) cep[t * C + k] -= (float)(mean[k] / T);     /* CMN over the window */
    for (long t = 0; t < T; t++) {                  /* splice (edges repeated), LDA */
        float sp[MAXCEP * 16];
        for (int c = 0; c < ctx; c++) {
            long u = t + c - w->lctx; u = u < 0 ? 0 : u >= T ? T - 1 : u;
            memcpy(sp + c * C, cep + u * C, sizeof(float) * C);
        }
        for (int r = 0; r < w->dim; r++) {
            float s = 0; const float *row = w->lda + (size_t)r * w->lda_cols;
            for (int c = 0; c < w->lda_cols; c++) s += row[c] * sp[c];
            feat[t * w->dim + r] = s;
        }
    }
    int adapt = w->dim == WAKEALIGN_ADIM && ((use && use->have && use->space == w->space) || (add && add->space == w->space));
    /* orig: the features as they came, for add's statistics */
    if (adapt && add && add->space == w->space) {
        if (!(orig = malloc(sizeof(float) * T * w->dim))) goto out;
        memcpy(orig, feat, sizeof(float) * T * w->dim);
    }
    if (adapt && use && use->have && use->space == w->space)
        for (long t = 0; t < T; t++) {              /* fMLLR: x <- W [x; 1] */
            float x[WAKEALIGN_ADIM], *f = feat + t * w->dim;
            memcpy(x, f, sizeof x);
            for (int i = 0; i < WAKEALIGN_ADIM; i++) {
                float v = use->xform[i][WAKEALIGN_ADIM];
                for (int j = 0; j < WAKEALIGN_ADIM; j++) v += use->xform[i][j] * x[j];
                f[i] = v;
            }
        }
    float *cur = delta, *nxt = delta + S;
    for (int s = 0; s < S; s++) cur[s] = -INFINITY;
    for (int p = 0; p < w->npdf; p++) ll[p] = NAN;
    for (int i = 0; i < w->nentry; i++) {
        int s = w->estate[i];
        if (isnan(ll[w->pdf[s]])) ll[w->pdf[s]] = gmm_loglike(w, w->pdf[s], feat);
        float v = w->ew[i] + w->ascale * ll[w->pdf[s]];
        if (v > cur[s]) cur[s] = v;
    }
    for (long t = 1; t < T; t++) {
        for (int p = 0; p < w->npdf; p++) ll[p] = NAN;
        for (int d = 0; d < S; d++) {
            float best = -INFINITY; int arg = -1;
            for (int k = w->poff[d]; k < w->poff[d + 1]; k++) {
                float v = cur[w->psrc[k]] + w->pw[k];
                if (v > best) { best = v; arg = w->psrc[k]; }
            }
            back[t * S + d] = (short)arg;
            if (arg < 0) { nxt[d] = -INFINITY; continue; }
            int p = w->pdf[d];
            if (isnan(ll[p])) ll[p] = gmm_loglike(w, p, feat + t * w->dim);
            nxt[d] = best + w->ascale * ll[p];
        }
        float *tmp = cur; cur = nxt; nxt = tmp;
    }
    int s = -1; float best = -INFINITY;
    for (int d = 0; d < S; d++) if (cur[d] + w->final[d] > best) { best = cur[d] + w->final[d]; s = d; }
    if (s < 0) goto out;
    path = malloc(sizeof(short) * T);
    if (!path) goto out;
    for (long t = T - 1; t >= 0; t--) {             /* back through the path */
        path[t] = (short)s;
        if (t > 0) s = back[t * S + s];
        if (s < 0) goto out;
    }
    for (long t = T - 1; t >= 0 && result < 0; t--)  /* the last frame in the word's last phone */
        for (int i = 0; i < w->nlast; i++) if (w->last[i] == path[t]) { result = (t + 1) * w->fshift; break; }
    if (orig) accumulate(w, orig, feat, path, T, add);
out:
    free(cep); free(feat); free(delta); free(ll); free(back); free(orig); free(path);
    return result > n ? n : result;
}
