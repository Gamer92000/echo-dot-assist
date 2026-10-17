/* A small interpreter for microWakeWord's .tflite models (mww.h): int8 throughout, the thirteen operators its streaming
 * models are built from (every model of github.com/esphome/micro-wake-word-models, v2 and experiments, 2026-10-07):
 * CONV_2D, DEPTHWISE_CONV_2D, FULLY_CONNECTED, LOGISTIC, QUANTIZE, CONCATENATION, STRIDED_SLICE, SPLIT_V, RESHAPE and
 * the resource variables that hold the stream's past (VAR_HANDLE, READ_VARIABLE, ASSIGN_VARIABLE, CALL_ONCE for their
 * start values).  Arithmetic as TFLite's reference kernels (per-channel requantization with its rounding), so a model
 * gives what it gives on ESPHome; LOGISTIC through a table of the float function, as TFLite's optimized kernel (within
 * one step of 1/256 of TFLite Micro's fixed point one).  tests/unit/mww_ref.py holds it to TFLite.
 *
 * Models come from the settings page: the file is untrusted.  Every offset and size of the flatbuffer is checked before
 * use, every tensor an operator touches must have the shape and type that operator needs, and a model with anything
 * else is refused at load.  Each tensor gets its own buffer (models are ~60 KB with ~40 KB of activations). */
#include "mww.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- flatbuffer reading, bounds-checked */

struct fb { const uint8_t *p; size_t n; int bad; };

static uint32_t rd32(struct fb *b, size_t at) { if (at > b->n || b->n - at < 4) { b->bad = 1; return 0; } uint32_t v; memcpy(&v, b->p + at, 4); return v; }
static uint16_t rd16(struct fb *b, size_t at) { if (at > b->n || b->n - at < 2) { b->bad = 1; return 0; } uint16_t v; memcpy(&v, b->p + at, 2); return v; }
static uint8_t  rd8(struct fb *b, size_t at) { if (at >= b->n) { b->bad = 1; return 0; } return b->p[at]; }

/* where field f of the table at t lies, 0: absent */
static size_t field(struct fb *b, size_t t, int f)
{
    if (!t) return 0;
    int32_t so = (int32_t)rd32(b, t);
    int64_t vt = (int64_t)t - so;
    if (vt < 0 || (uint64_t)vt >= b->n) { b->bad = 1; return 0; }
    uint16_t vlen = rd16(b, (size_t)vt);
    if (4 + 2 * f + 2 > vlen) return 0;
    uint16_t o = rd16(b, (size_t)vt + 4 + 2 * f);
    return o ? t + o : 0;
}
static size_t deref(struct fb *b, size_t at) { if (!at) return 0; uint32_t o = rd32(b, at); size_t r = at + o; if (r < at || r >= b->n) { b->bad = 1; return 0; } return r; }
static size_t table(struct fb *b, size_t t, int f) { return deref(b, field(b, t, f)); }
static int64_t i32f(struct fb *b, size_t t, int f, int64_t dflt) { size_t a = field(b, t, f); return a ? (int32_t)rd32(b, a) : dflt; }
static int u8f(struct fb *b, size_t t, int f, int dflt) { size_t a = field(b, t, f); return a ? rd8(b, a) : dflt; }
/* a vector: its length, *at its first element */
static uint32_t vec(struct fb *b, size_t t, int f, size_t *at)
{
    size_t v = table(b, t, f);
    *at = 0;
    if (!v) return 0;
    uint32_t n = rd32(b, v);
    if (n > b->n) { b->bad = 1; return 0; }
    *at = v + 4;
    return n;
}
static size_t vtab(struct fb *b, size_t at, uint32_t i) { return deref(b, at + 4 * (size_t)i); }   /* vector of tables */
static int str_get(struct fb *b, size_t t, int f, char *out, size_t cap)
{
    size_t at; uint32_t n = vec(b, t, f, &at);
    if (!at || n >= cap || at + n > b->n) { if (cap) out[0] = 0; return at ? -1 : 0; }
    memcpy(out, b->p + at, n); out[n] = 0;
    return 0;
}

/* ---------------------------------------------------------------- the model */

enum { T_FLOAT32 = 0, T_INT32 = 2, T_UINT8 = 3, T_INT64 = 4, T_INT8 = 9, T_RESOURCE = 13 };
enum { OP_CONCAT = 2, OP_CONV = 3, OP_DWCONV = 4, OP_FC = 9, OP_LOGISTIC = 14, OP_RESHAPE = 22, OP_STRIDED_SLICE = 45,
       OP_SPLIT_V = 102, OP_QUANTIZE = 114, OP_CALL_ONCE = 129, OP_VAR_HANDLE = 142, OP_READ_VAR = 143, OP_ASSIGN_VAR = 144 };
enum { ACT_NONE, ACT_RELU, ACT_RELU_N1_TO_1, ACT_RELU6 };
#define MAX_DIMS 5
#define MAX_IO   12
#define MAX_VARS 32
#define MAX_TENSORS 512
#define MAX_OPS 256
/* the int32 accumulator of a convolution can hold 255 x 128 per product: at most this many products, and a bias of at
 * most 2^30, keep any model from overflowing it */
#define MAX_DOT 32768
#define MAX_BIAS (1 << 30)

struct tensor {
    int type, ndim, dims[MAX_DIMS], is_const;
    size_t count, bytes;
    uint8_t *data;
    float scale; int32_t zp;
    int nscales; float *scales;     /* per channel (filters), else NULL */
};

struct op {
    int code, nin, nout, in[MAX_IO], out[MAX_IO];
    int padding, stride_w, stride_h, dil_w, dil_h, act, depth_mult, axis, begin_mask, end_mask, shrink_mask, offset, sub;
    char var[64];                   /* VAR_HANDLE's shared name */
    int32_t *mult, *shift; int nmult; int32_t act_min, act_max, pad_h, pad_w;
    int8_t lut[256];                /* LOGISTIC / QUANTIZE: the whole int8 input range */
};

struct graph { int nt, nops; struct tensor *t; struct op *ops; int in, out; };

struct mww_model {
    struct graph g[2];              /* main, and the start values of the variables (CALL_ONCE) */
    int ng, initialized;
    struct { char name[64]; uint8_t *data; size_t bytes; } var[MAX_VARS];
    int nvar;
};

static size_t type_size(int t) { return t == T_INT32 || t == T_FLOAT32 || t == T_RESOURCE ? 4 : t == T_INT64 ? 8 : t == T_INT8 || t == T_UINT8 ? 1 : 0; }

#define FAIL(...) do { snprintf(err, errsz, __VA_ARGS__); goto fail; } while (0)

/* QuantizeMultiplier: m = q * 2^shift with q in [0.5, 1) as Q31 */
static void quantize_multiplier(double m, int32_t *q, int32_t *shift)
{
    if (m == 0) { *q = 0; *shift = 0; return; }
    int s; double f = frexp(m, &s);
    int64_t qf = (int64_t)llround(f * (double)(1LL << 31));
    if (qf == (1LL << 31)) { qf /= 2; s++; }
    if (s < -31) { s = 0; qf = 0; }
    if (s > 30) { s = 30; qf = (1LL << 31) - 1; }
    *q = (int32_t)qf; *shift = s;
}

static int32_t srdhm(int32_t a, int32_t b)          /* SaturatingRoundingDoublingHighMul */
{
    if (a == b && a == INT32_MIN) return INT32_MAX;
    int64_t ab = (int64_t)a * b, nudge = ab >= 0 ? (1 << 30) : (1 - (1 << 30));
    return (int32_t)((ab + nudge) / (1LL << 31));
}
static int32_t rdbpot(int32_t x, int e)             /* RoundingDivideByPOT */
{
    int32_t mask = (int32_t)((1LL << e) - 1), rem = x & mask, thr = (mask >> 1) + (x < 0 ? 1 : 0);
    return (x >> e) + (rem > thr ? 1 : 0);
}
static inline int32_t mbqm(int32_t x, int32_t q, int32_t shift)     /* MultiplyByQuantizedMultiplier */
{
    int left = shift > 0 ? shift : 0, right = shift > 0 ? 0 : -shift;
    return rdbpot(srdhm((int32_t)((uint32_t)x << left), q), right);
}

/* a real value quantized with scale and zero point, rounded half away from zero, clamped to [lo, hi] */
static int32_t quant(double x, float scale, int32_t zp, int32_t lo, int32_t hi)
{
    double q = round(x / scale) + zp;
    return q < lo ? lo : q > hi ? hi : (int32_t)q;
}

static void act_range(int act, float scale, int32_t zp, int32_t qmin, int32_t qmax, int32_t *lo, int32_t *hi)
{
    int32_t z = quant(0, scale, zp, qmin, qmax), six = quant(6, scale, zp, qmin, qmax), m1 = quant(-1, scale, zp, qmin, qmax), p1 = quant(1, scale, zp, qmin, qmax);
    *lo = qmin; *hi = qmax;
    if (act == ACT_RELU) *lo = z > qmin ? z : qmin;
    else if (act == ACT_RELU6) { *lo = z > qmin ? z : qmin; *hi = six < qmax ? six : qmax; }
    else if (act == ACT_RELU_N1_TO_1) { *lo = m1 > qmin ? m1 : qmin; *hi = p1 < qmax ? p1 : qmax; }
}

static void graph_free(struct graph *g)
{
    for (int i = 0; i < g->nt; i++) { free(g->t[i].data); free(g->t[i].scales); }
    for (int i = 0; i < g->nops; i++) { free(g->ops[i].mult); free(g->ops[i].shift); }
    free(g->t); free(g->ops);
    memset(g, 0, sizeof *g);
}

void mww_model_free(struct mww_model *m)
{
    if (!m) return;
    for (int i = 0; i < m->ng; i++) graph_free(&m->g[i]);
    for (int i = 0; i < m->nvar; i++) free(m->var[i].data);
    free(m);
}

/* quantized with a scale arithmetic can use (per channel: each of them) */
static int qok(const struct tensor *t)
{
    for (int s = 0; s < (t->nscales ? t->nscales : 1); s++) { float v = t->nscales ? t->scales[s] : t->scale; if (!(v > 1e-20f && v < 1e20f)) return 0; }
    return 1;
}

/* the bias values: none so large that the accumulator could overflow (MAX_BIAS) */
static int bias_ok(const struct tensor *b)
{
    for (size_t i = 0; b && i < b->count; i++) { int32_t v; memcpy(&v, b->data + 4 * i, 4); if (v > MAX_BIAS || v < -MAX_BIAS) return 0; }
    return 1;
}

/* tensor i of graph g as an operator's operand: in range, of that type (-1: any) and rank (0: any) */
static struct tensor *operand(struct graph *g, int i, int type, int ndim)
{
    if (i < 0 || i >= g->nt) return NULL;
    struct tensor *t = &g->t[i];
    if ((type >= 0 && t->type != type) || (ndim && t->ndim != ndim)) return NULL;
    return t;
}

static int load_tensors(struct fb *b, size_t sg, struct graph *g, size_t buffers, uint32_t nbuf, char *err, size_t errsz)
{
    size_t at; uint32_t n = vec(b, sg, 0, &at);
    if (!n || n > MAX_TENSORS) FAIL("%u tensors", n);
    if (!(g->t = calloc(n, sizeof *g->t))) FAIL("no memory");
    g->nt = (int)n;
    for (uint32_t i = 0; i < n; i++) {
        struct tensor *t = &g->t[i]; size_t tt = vtab(b, at, i), sh, q;
        uint32_t nd = vec(b, tt, 0, &sh);
        if (nd > MAX_DIMS) FAIL("tensor %u: %u dimensions", i, nd);
        t->ndim = (int)nd; t->count = 1;
        for (uint32_t d = 0; d < nd; d++) {
            int32_t v = (int32_t)rd32(b, sh + 4 * d);
            if (v < 1 || v > 65536) FAIL("tensor %u: dimension %d", i, v);
            t->dims[d] = v; t->count *= (size_t)v;
            if (t->count > (1 << 20)) FAIL("tensor %u too large", i);
        }
        t->type = u8f(b, tt, 1, 0);
        size_t es = type_size(t->type);
        if (!es) FAIL("tensor %u: type %d", i, t->type);
        t->bytes = t->count * es;
        if ((q = table(b, tt, 4))) {
            size_t sa, za; uint32_t ns = vec(b, q, 2, &sa), nz = vec(b, q, 3, &za);
            if (ns > 4096) FAIL("tensor %u: %u scales", i, ns);
            if (ns) { uint32_t v = rd32(b, sa); memcpy(&t->scale, &v, 4); }
            if (nz) t->zp = (int32_t)(int64_t)((uint64_t)rd32(b, za) | (uint64_t)rd32(b, za + 4) << 32);
            if (ns > 1) {
                if (!(t->scales = malloc(ns * sizeof *t->scales))) FAIL("no memory");
                for (uint32_t s = 0; s < ns; s++) { uint32_t v = rd32(b, sa + 4 * s); memcpy(&t->scales[s], &v, 4); }
                t->nscales = (int)ns;
            }
            for (int s = 0; s < (t->nscales ? t->nscales : 1); s++) {
                float v = t->nscales ? t->scales[s] : t->scale;
                if (!(v >= 0 && v < 1e20f)) FAIL("tensor %u: scale", i);
            }
        }
        size_t bf = field(b, tt, 2);
        uint32_t bi = bf ? rd32(b, bf) : 0;
        size_t da = 0; uint32_t dl = 0;
        if (bi && bi < nbuf) dl = vec(b, vtab(b, buffers, bi), 0, &da);
        else if (bi) FAIL("tensor %u: buffer %u", i, bi);
        if (!(t->data = calloc(1, t->bytes ? t->bytes : 1))) FAIL("no memory");
        if (dl) {
            if (dl != t->bytes || da + dl > b->n) FAIL("tensor %u: %u bytes of data for %zu", i, dl, t->bytes);
            memcpy(t->data, b->p + da, dl); t->is_const = 1;
        }
        if (t->type == T_INT8 ? t->zp < -128 || t->zp > 127 : t->type == T_UINT8 ? t->zp < 0 || t->zp > 255 : t->zp != 0) FAIL("tensor %u: zero point %d", i, (int)t->zp);
        if (b->bad) FAIL("tensor %u: broken", i);
    }
    return 0;
fail:
    return -1;
}

static size_t slice_plan(struct graph *g, const struct op *o, int *start, int *stop, int *step, int *dim);

/* everything an operator needs that does not change from run to run, and the checks that make running it safe */
static int prepare(struct mww_model *m, struct graph *g, struct op *o, char *err, size_t errsz)
{
    struct tensor *in, *out, *f, *bias;
    switch (o->code) {
    case OP_CONV: case OP_DWCONV: {
        if (o->nin < 2 || o->nout != 1 || !(in = operand(g, o->in[0], T_INT8, 4)) || !(f = operand(g, o->in[1], T_INT8, 4)) || !f->is_const
            || !(out = operand(g, o->out[0], T_INT8, 4))) FAIL("convolution: operands");
        bias = o->nin > 2 && o->in[2] >= 0 ? operand(g, o->in[2], T_INT32, 1) : NULL;
        if (o->nin > 2 && o->in[2] >= 0 && (!bias || !bias->is_const)) FAIL("convolution: bias");
        int oc = out->dims[3], ic = in->dims[3];
        if (in->dims[0] != out->dims[0]) FAIL("convolution: batch");
        if (o->code == OP_CONV ? (f->dims[0] != oc || f->dims[3] != ic) : (f->dims[0] != 1 || f->dims[3] != oc || o->depth_mult < 1 || (long long)ic * o->depth_mult != oc)) FAIL("convolution: filter shape");
        if (bias && bias->dims[0] != oc) FAIL("convolution: bias shape");
        if (f->nscales && f->nscales != oc) FAIL("convolution: %d filter scales", f->nscales);
        if (!qok(in) || !qok(f) || !qok(out) || !bias_ok(bias)) FAIL("convolution: quantization");
        if ((long long)f->dims[1] * f->dims[2] * (o->code == OP_CONV ? ic : 1) > MAX_DOT) FAIL("convolution: kernel too large");
        if (o->stride_w < 1 || o->stride_h < 1 || o->dil_w < 1 || o->dil_h < 1 || o->stride_w > 1024 || o->stride_h > 1024 || o->dil_w > 1024 || o->dil_h > 1024)
            FAIL("convolution: stride");
        int eh = (f->dims[1] - 1) * o->dil_h + 1, ew = (f->dims[2] - 1) * o->dil_w + 1;
        int th = (out->dims[1] - 1) * o->stride_h + eh - in->dims[1], tw = (out->dims[2] - 1) * o->stride_w + ew - in->dims[2];
        o->pad_h = o->padding == 0 && th > 0 ? th / 2 : 0; o->pad_w = o->padding == 0 && tw > 0 ? tw / 2 : 0;
        if (!(o->mult = malloc(oc * sizeof *o->mult)) || !(o->shift = malloc(oc * sizeof *o->shift))) FAIL("no memory");
        for (int c = 0; c < oc; c++) {
            double em = (double)in->scale * (double)(f->nscales ? f->scales[c] : f->scale) / (double)out->scale;
            if (!isfinite(em)) FAIL("convolution: scales");
            quantize_multiplier(em, &o->mult[c], &o->shift[c]);
        }
        o->nmult = oc;
        act_range(o->act, out->scale, out->zp, -128, 127, &o->act_min, &o->act_max);
        return 0;
    }
    case OP_FC: {
        if (o->nin < 2 || o->nout != 1 || !(in = operand(g, o->in[0], T_INT8, 0)) || !(f = operand(g, o->in[1], T_INT8, 2)) || !f->is_const
            || !(out = operand(g, o->out[0], T_INT8, 0))) FAIL("fully connected: operands");
        bias = o->nin > 2 && o->in[2] >= 0 ? operand(g, o->in[2], T_INT32, 1) : NULL;
        if (o->nin > 2 && o->in[2] >= 0 && (!bias || !bias->is_const)) FAIL("fully connected: bias");
        int depth = f->dims[1], units = f->dims[0];
        if (in->count % (size_t)depth || out->count != in->count / (size_t)depth * (size_t)units || (bias && bias->dims[0] != units)) FAIL("fully connected: shapes");
        if (f->nscales && f->nscales != units) FAIL("fully connected: scales");
        if (!qok(in) || !qok(f) || !qok(out) || !bias_ok(bias) || depth > MAX_DOT || f->zp) FAIL("fully connected: quantization");
        if (!(o->mult = malloc(units * sizeof *o->mult)) || !(o->shift = malloc(units * sizeof *o->shift))) FAIL("no memory");
        for (int c = 0; c < units; c++) {
            double em = (double)in->scale * (double)(f->nscales ? f->scales[c] : f->scale) / (double)out->scale;
            if (!isfinite(em)) FAIL("fully connected: scales");
            quantize_multiplier(em, &o->mult[c], &o->shift[c]);
        }
        o->nmult = units;
        act_range(o->act, out->scale, out->zp, -128, 127, &o->act_min, &o->act_max);
        return 0;
    }
    case OP_LOGISTIC: case OP_QUANTIZE: {
        if (o->nin != 1 || o->nout != 1 || !(in = operand(g, o->in[0], T_INT8, 0)) || !(out = operand(g, o->out[0], -1, 0))
            || (out->type != T_INT8 && out->type != T_UINT8) || in->count != out->count || !qok(in) || !qok(out) || in->nscales || out->nscales)
            FAIL("%s: operands", o->code == OP_QUANTIZE ? "quantize" : "logistic");
        int32_t lo = out->type == T_INT8 ? -128 : 0, hi = out->type == T_INT8 ? 127 : 255;
        if (o->code == OP_LOGISTIC) {
            for (int v = -128; v < 128; v++) {
                float x = in->scale * (float)(v - in->zp), y = 1.0f / (1.0f + expf(-x));
                o->lut[v + 128] = (int8_t)quant(y, out->scale, out->zp, lo, hi);
            }
        } else {
            int32_t mq, ms; double em = (double)in->scale / (double)out->scale;
            if (!isfinite(em)) FAIL("quantize: scales");
            quantize_multiplier(em, &mq, &ms);
            for (int v = -128; v < 128; v++) {
                int64_t q = (int64_t)mbqm(v - in->zp, mq, ms) + out->zp;
                o->lut[v + 128] = (int8_t)(q < lo ? lo : q > hi ? hi : q);
            }
        }
        return 0;
    }
    case OP_RESHAPE:
        if (o->nin < 1 || o->nout != 1 || !(in = operand(g, o->in[0], -1, 0)) || !(out = operand(g, o->out[0], in->type, 0)) || in->bytes != out->bytes) FAIL("reshape: operands");
        return 0;
    case OP_CONCAT: {
        if (o->nin < 1 || o->nout != 1 || !(out = operand(g, o->out[0], -1, 0))) FAIL("concatenation: operands");
        int ax = o->axis < 0 ? o->axis + out->ndim : o->axis; size_t sum = 0;
        if (ax < 0 || ax >= out->ndim) FAIL("concatenation: axis");
        o->axis = ax;
        for (int i = 0; i < o->nin; i++) {
            if (!(in = operand(g, o->in[i], out->type, out->ndim))) FAIL("concatenation: input %d", i);
            for (int d = 0; d < out->ndim; d++) if (d != ax && in->dims[d] != out->dims[d]) FAIL("concatenation: shapes");
            sum += (size_t)in->dims[ax];
        }
        if (sum != (size_t)out->dims[ax] || (out->type != T_INT8 && out->type != T_INT32)) FAIL("concatenation: shapes");
        for (int i = 0; out->type == T_INT8 && i < o->nin; i++) {
            in = &g->t[o->in[i]];
            if ((in->scale != out->scale || in->zp != out->zp) && (!qok(in) || !qok(out) || !isfinite((double)in->scale / out->scale))) FAIL("concatenation: scales");
        }
        return 0;
    }
    case OP_SPLIT_V: {
        struct tensor *ax;
        if (o->nin != 3 || o->nout < 1 || !(in = operand(g, o->in[0], -1, 0)) || !(ax = operand(g, o->in[2], T_INT32, 0)) || !ax->is_const || ax->count != 1) FAIL("split: operands");
        int a; memcpy(&a, ax->data, 4);
        if (a < 0) a += in->ndim;
        if (a < 0 || a >= in->ndim) FAIL("split: axis");
        o->axis = a; size_t sum = 0;
        for (int i = 0; i < o->nout; i++) {
            if (!(out = operand(g, o->out[i], in->type, in->ndim))) FAIL("split: output %d", i);
            for (int d = 0; d < in->ndim; d++) if (d != a && out->dims[d] != in->dims[d]) FAIL("split: shapes");
            sum += (size_t)out->dims[a];
        }
        if (sum != (size_t)in->dims[a]) FAIL("split: sizes");
        return 0;
    }
    case OP_STRIDED_SLICE: {
        struct tensor *bt, *et, *st;
        if (o->nin != 4 || o->nout != 1 || !(in = operand(g, o->in[0], -1, 0)) || !(out = operand(g, o->out[0], in->type, 0))
            || !(bt = operand(g, o->in[1], T_INT32, 1)) || !(et = operand(g, o->in[2], T_INT32, 1)) || !(st = operand(g, o->in[3], T_INT32, 1))
            || !bt->is_const || !et->is_const || !st->is_const || bt->dims[0] != in->ndim || et->dims[0] != in->ndim || st->dims[0] != in->ndim) FAIL("strided slice: operands");
        for (int d = 0; d < in->ndim; d++) { int32_t s0; memcpy(&s0, st->data + 4 * d, 4); if (s0 == 0 || s0 > 65536 || s0 < -65536) FAIL("strided slice: stride %d", (int)s0); }
        int a[MAX_DIMS], z[MAX_DIMS], s[MAX_DIMS], n[MAX_DIMS];
        if (slice_plan(g, o, a, z, s, n) != out->count) FAIL("strided slice: not the output's size");
        return 0;
    }
    case OP_VAR_HANDLE: {
        if (o->nout != 1 || !operand(g, o->out[0], T_RESOURCE, 0)) FAIL("variable: operands");
        int i;
        for (i = 0; i < m->nvar && strcmp(m->var[i].name, o->var); i++) ;
        if (i == m->nvar) { if (m->nvar == MAX_VARS) FAIL("too many variables"); snprintf(m->var[m->nvar++].name, sizeof m->var[0].name, "%s", o->var); }
        o->sub = i;
        return 0;
    }
    case OP_READ_VAR:
        if (o->nin != 1 || o->nout != 1 || !operand(g, o->in[0], T_RESOURCE, 0) || !operand(g, o->out[0], -1, 0)) FAIL("read variable: operands");
        return 0;
    case OP_ASSIGN_VAR:
        if (o->nin != 2 || !operand(g, o->in[0], T_RESOURCE, 0) || !operand(g, o->in[1], -1, 0)) FAIL("assign variable: operands");
        return 0;
    case OP_CALL_ONCE:
        if (g != &m->g[0] || o->sub != 1 || m->ng != 2) FAIL("call once: subgraph %d", o->sub);
        return 0;
    }
    FAIL("operator %d is not one microWakeWord models use", o->code);
fail:
    return -1;
}

static int load_ops(struct fb *b, size_t sg, struct graph *g, size_t codes, uint32_t ncodes, char *err, size_t errsz)
{
    size_t at; uint32_t n = vec(b, sg, 3, &at);
    if (n > MAX_OPS) FAIL("%u operators", n);
    if (!(g->ops = calloc(n ? n : 1, sizeof *g->ops))) FAIL("no memory");
    g->nops = (int)n;
    for (uint32_t i = 0; i < n; i++) {
        struct op *o = &g->ops[i]; size_t ot = vtab(b, at, i), ia, oa, opt;
        uint32_t ci = (uint32_t)i32f(b, ot, 0, 0);
        if (ci >= ncodes) FAIL("operator %u: code %u", i, ci);
        size_t ct = vtab(b, codes, ci);
        int dep = (int8_t)u8f(b, ct, 0, 0), code = (int)i32f(b, ct, 3, 0);
        o->code = code > dep ? code : dep;
        uint32_t ni = vec(b, ot, 1, &ia), no = vec(b, ot, 2, &oa);
        if (ni > MAX_IO || no > MAX_IO) FAIL("operator %u: %u inputs", i, ni);
        o->nin = (int)ni; o->nout = (int)no;
        for (uint32_t k = 0; k < ni; k++) o->in[k] = (int32_t)rd32(b, ia + 4 * k);
        for (uint32_t k = 0; k < no; k++) o->out[k] = (int32_t)rd32(b, oa + 4 * k);
        opt = table(b, ot, 4);
        o->stride_w = o->stride_h = o->dil_w = o->dil_h = o->depth_mult = 1;
        switch (o->code) {
        case OP_CONV:
            o->padding = u8f(b, opt, 0, 0); o->stride_w = (int)i32f(b, opt, 1, 0); o->stride_h = (int)i32f(b, opt, 2, 0);
            o->act = u8f(b, opt, 3, 0); o->dil_w = (int)i32f(b, opt, 4, 1); o->dil_h = (int)i32f(b, opt, 5, 1); break;
        case OP_DWCONV:
            o->padding = u8f(b, opt, 0, 0); o->stride_w = (int)i32f(b, opt, 1, 0); o->stride_h = (int)i32f(b, opt, 2, 0);
            o->depth_mult = (int)i32f(b, opt, 3, 0); o->act = u8f(b, opt, 4, 0); o->dil_w = (int)i32f(b, opt, 5, 1); o->dil_h = (int)i32f(b, opt, 6, 1);
            break;
        case OP_FC:
            o->act = u8f(b, opt, 0, 0);
            if (u8f(b, opt, 1, 0)) FAIL("fully connected: shuffled weights");
            break;
        case OP_CONCAT: o->axis = (int)i32f(b, opt, 0, 0); o->act = u8f(b, opt, 1, 0); if (o->act) FAIL("concatenation: activation"); break;
        case OP_STRIDED_SLICE:
            o->begin_mask = (int)i32f(b, opt, 0, 0); o->end_mask = (int)i32f(b, opt, 1, 0);
            if (i32f(b, opt, 2, 0) || i32f(b, opt, 3, 0)) FAIL("strided slice: ellipsis or new axis");
            o->shrink_mask = (int)i32f(b, opt, 4, 0); o->offset = u8f(b, opt, 5, 0); break;
        case OP_CALL_ONCE: o->sub = (int)i32f(b, opt, 0, 0); break;
        case OP_VAR_HANDLE: {
            char c[64];
            if (str_get(b, opt, 0, c, sizeof c) || str_get(b, opt, 1, o->var, sizeof o->var)) FAIL("variable: name");
            if (c[0]) { size_t l = strlen(o->var); if (l + strlen(c) + 2 > sizeof o->var) FAIL("variable: name"); memmove(o->var + strlen(c) + 1, o->var, l + 1); memcpy(o->var, c, strlen(c)); o->var[strlen(c)] = '/'; }
            break;
        }
        }
        if (o->act > ACT_RELU6 || o->padding > 1) FAIL("operator %u: options", i);
        if (b->bad) FAIL("operator %u: broken", i);
    }
    return 0;
fail:
    return -1;
}

struct mww_model *mww_model_load(const void *data, size_t len, char *err, size_t errsz)
{
    struct fb b = { data, len, 0 };
    struct mww_model *m = calloc(1, sizeof *m);
    size_t codes, subs, bufs;
    if (!m) { snprintf(err, errsz, "no memory"); return NULL; }
    if (len < 16 || memcmp((const uint8_t *)data + 4, "TFL3", 4)) FAIL("not a TensorFlow Lite model");
    size_t root = rd32(&b, 0);                       /* the root table: offset 0 is the one place deref() would refuse */
    if (root < 8 || root >= len) FAIL("not a TensorFlow Lite model");
    uint32_t ncodes = vec(&b, root, 1, &codes), nsub = vec(&b, root, 2, &subs), nbuf = vec(&b, root, 4, &bufs);
    if (b.bad || !nsub || nsub > 2) FAIL("not a microWakeWord model (%u subgraphs)", nsub);
    m->ng = (int)nsub;
    for (int s = 0; s < m->ng; s++) {
        size_t sg = vtab(&b, subs, (uint32_t)s), ia, oa;
        if (load_tensors(&b, sg, &m->g[s], bufs, nbuf, err, errsz) || load_ops(&b, sg, &m->g[s], codes, ncodes, err, errsz)) goto fail;
        uint32_t ni = vec(&b, sg, 1, &ia), no = vec(&b, sg, 2, &oa);
        m->g[s].in = ni ? (int32_t)rd32(&b, ia) : -1; m->g[s].out = no ? (int32_t)rd32(&b, oa) : -1;
        if (s == 0 && (ni != 1 || no != 1)) FAIL("%u inputs, %u outputs", ni, no);
    }
    for (int s = 0; s < m->ng; s++)
        for (int i = 0; i < m->g[s].nops; i++) if (prepare(m, &m->g[s], &m->g[s].ops[i], err, errsz)) goto fail;
    struct tensor *in = operand(&m->g[0], m->g[0].in, T_INT8, 3), *out = operand(&m->g[0], m->g[0].out, -1, 0);
    if (!in || in->dims[0] != 1 || in->dims[2] != MWW_CHANNELS || in->dims[1] > 16 || !qok(in) || in->nscales) FAIL("input: not 1 x n x 40 int8");
    if (!out || out->count != 1 || (out->type != T_UINT8 && out->type != T_INT8) || !qok(out) || out->nscales) FAIL("output: not one probability");
    if (b.bad) FAIL("broken model file");
    mww_model_reset(m);
    return m;
fail:
    mww_model_free(m);
    return NULL;
}

/* ---------------------------------------------------------------- running */

static void conv(struct graph *g, const struct op *o)
{
    const struct tensor *in = &g->t[o->in[0]], *f = &g->t[o->in[1]], *out = &g->t[o->out[0]];
    const int32_t *bias = o->nin > 2 && o->in[2] >= 0 ? (const int32_t *)g->t[o->in[2]].data : NULL;
    const int8_t *x = (const int8_t *)in->data, *w = (const int8_t *)f->data; int8_t *y = (int8_t *)out->data;
    int N = in->dims[0], H = in->dims[1], W = in->dims[2], C = in->dims[3], KH = f->dims[1], KW = f->dims[2];
    int OH = out->dims[1], OW = out->dims[2], OC = out->dims[3], dw = o->code == OP_DWCONV, dm = o->depth_mult;
    int32_t ioff = -in->zp;
    for (int n = 0; n < N; n++)
        for (int oy = 0; oy < OH; oy++)
            for (int ox = 0; ox < OW; ox++)
                for (int oc = 0; oc < OC; oc++) {
                    int32_t acc = 0; int iy0 = oy * o->stride_h - o->pad_h, ix0 = ox * o->stride_w - o->pad_w;
                    for (int ky = 0; ky < KH; ky++) {
                        int iy = iy0 + ky * o->dil_h;
                        if (iy < 0 || iy >= H) continue;
                        for (int kx = 0; kx < KW; kx++) {
                            int ix = ix0 + kx * o->dil_w;
                            if (ix < 0 || ix >= W) continue;
                            const int8_t *xp = x + (((size_t)n * H + iy) * W + ix) * C;
                            if (dw) acc += (xp[oc / dm] + ioff) * w[((size_t)ky * KW + kx) * OC + oc];
                            else {
                                const int8_t *wp = w + (((size_t)oc * KH + ky) * KW + kx) * C;
                                for (int c = 0; c < C; c++) acc += (xp[c] + ioff) * wp[c];
                            }
                        }
                    }
                    if (bias) acc += bias[oc];
                    int64_t v = (int64_t)mbqm(acc, o->mult[oc], o->shift[oc]) + out->zp;
                    y[(((size_t)n * OH + oy) * OW + ox) * OC + oc] = (int8_t)(v < o->act_min ? o->act_min : v > o->act_max ? o->act_max : v);
                }
}

static void fully_connected(struct graph *g, const struct op *o)
{
    const struct tensor *in = &g->t[o->in[0]], *f = &g->t[o->in[1]], *out = &g->t[o->out[0]];
    const int32_t *bias = o->nin > 2 && o->in[2] >= 0 ? (const int32_t *)g->t[o->in[2]].data : NULL;
    const int8_t *x = (const int8_t *)in->data, *w = (const int8_t *)f->data; int8_t *y = (int8_t *)out->data;
    int depth = f->dims[1], units = f->dims[0], batches = (int)(in->count / (size_t)depth);
    int32_t ioff = -in->zp, foff = -f->zp;
    for (int b = 0; b < batches; b++)
        for (int u = 0; u < units; u++) {
            int32_t acc = 0;
            for (int d = 0; d < depth; d++) acc += (x[(size_t)b * depth + d] + ioff) * (w[(size_t)u * depth + d] + foff);
            if (bias) acc += bias[u];
            int64_t v = (int64_t)mbqm(acc, o->mult[u], o->shift[u]) + out->zp;
            y[(size_t)b * units + u] = (int8_t)(v < o->act_min ? o->act_min : v > o->act_max ? o->act_max : v);
        }
}

static void concat(struct graph *g, const struct op *o)
{
    struct tensor *out = &g->t[o->out[0]];
    size_t es = type_size(out->type), outer = 1, inner = es, at = 0;
    for (int d = 0; d < o->axis; d++) outer *= (size_t)out->dims[d];
    for (int d = o->axis + 1; d < out->ndim; d++) inner *= (size_t)out->dims[d];
    for (size_t k = 0; k < outer; k++)
        for (int i = 0; i < o->nin; i++) {
            const struct tensor *in = &g->t[o->in[i]];
            size_t n = (size_t)in->dims[o->axis] * inner;
            const uint8_t *src = in->data + k * n;
            if (out->type == T_INT8 && (in->scale != out->scale || in->zp != out->zp)) {   /* rescaled, as TFLite does it */
                float s = in->scale / out->scale, bias = -in->zp * s;
                for (size_t j = 0; j < n; j++) {
                    double v = round((double)((int8_t)src[j] * s + bias)) + out->zp;
                    out->data[at + j] = (uint8_t)(int8_t)(v < -128 ? -128 : v > 127 ? 127 : v);
                }
            } else memcpy(out->data + at, src, n);
            at += n;
        }
}

static void split(struct graph *g, const struct op *o)
{
    const struct tensor *in = &g->t[o->in[0]];
    size_t es = type_size(in->type), outer = 1, inner = es, at = 0;
    for (int d = 0; d < o->axis; d++) outer *= (size_t)in->dims[d];
    for (int d = o->axis + 1; d < in->ndim; d++) inner *= (size_t)in->dims[d];
    for (size_t k = 0; k < outer; k++)
        for (int i = 0; i < o->nout; i++) {
            struct tensor *out = &g->t[o->out[i]];
            size_t n = (size_t)out->dims[o->axis] * inner;
            memcpy(out->data + k * n, in->data + at, n);
            at += n;
        }
}

/* TFLite's strided slice (no ellipsis, no new axes) over 5 dimensions (the input's last ones); how many elements */
static size_t slice_plan(struct graph *g, const struct op *o, int *start, int *stop, int *step, int *dim)
{
    const struct tensor *in = &g->t[o->in[0]];
    const int32_t *bv = (const int32_t *)g->t[o->in[1]].data, *ev = (const int32_t *)g->t[o->in[2]].data, *sv = (const int32_t *)g->t[o->in[3]].data;
    int nd = in->ndim; size_t cnt = 1;
    for (int k = 0; k < MAX_DIMS; k++) { start[k] = 0; stop[k] = 1; step[k] = 1; dim[k] = 1; }
    for (int i = 0; i < nd; i++) {
        int k = MAX_DIMS - nd + i, d = in->dims[i], s = sv[i];
        int64_t b = bv[i], e = o->offset ? (int64_t)bv[i] + ev[i] : ev[i];     /* the model's numbers: any int32 */
        if (o->begin_mask & 1 << i) b = s > 0 ? 0 : d - 1;
        else { if (b < 0) b += d; b = s > 0 ? (b < 0 ? 0 : b > d ? d : b) : (b < -1 ? -1 : b > d - 1 ? d - 1 : b); }
        if (o->shrink_mask & 1 << i) e = b + 1;
        else if (o->end_mask & 1 << i) e = s > 0 ? d : -1;
        else { if (e < 0) e += d; e = s > 0 ? (e < 0 ? 0 : e > d ? d : e) : (e < -1 ? -1 : e > d - 1 ? d - 1 : e); }
        start[k] = (int)b; stop[k] = (int)e; step[k] = s; dim[k] = d;
        int64_t len = s > 0 ? (e > b ? (e - b + s - 1) / s : 0) : (b > e ? (b - e - s - 1) / -s : 0);
        cnt *= (size_t)len;
    }
    return cnt;
}

static void strided_slice(struct graph *g, const struct op *o)
{
    const struct tensor *in = &g->t[o->in[0]]; struct tensor *out = &g->t[o->out[0]];
    int start[MAX_DIMS], stop[MAX_DIMS], step[MAX_DIMS], dim[MAX_DIMS];
    size_t es = type_size(in->type), at = 0, st[MAX_DIMS];
    slice_plan(g, o, start, stop, step, dim);
    st[MAX_DIMS - 1] = 1;
    for (int k = MAX_DIMS - 2; k >= 0; k--) st[k] = st[k + 1] * (size_t)dim[k + 1];
    for (int a = start[0]; step[0] > 0 ? a < stop[0] : a > stop[0]; a += step[0])
     for (int b = start[1]; step[1] > 0 ? b < stop[1] : b > stop[1]; b += step[1])
      for (int c = start[2]; step[2] > 0 ? c < stop[2] : c > stop[2]; c += step[2])
       for (int d = start[3]; step[3] > 0 ? d < stop[3] : d > stop[3]; d += step[3])
        for (int e = start[4]; step[4] > 0 ? e < stop[4] : e > stop[4]; e += step[4]) {
            size_t src = a * st[0] + b * st[1] + c * st[2] + d * st[3] + e * st[4];
            memcpy(out->data + at * es, in->data + src * es, es); at++;
        }
}

static int run_graph(struct mww_model *m, int gi)
{
    struct graph *g = &m->g[gi];
    for (int i = 0; i < g->nops; i++) {
        struct op *o = &g->ops[i];
        switch (o->code) {
        case OP_CONV: case OP_DWCONV: conv(g, o); break;
        case OP_FC: fully_connected(g, o); break;
        case OP_LOGISTIC: case OP_QUANTIZE: {
            const struct tensor *in = &g->t[o->in[0]]; struct tensor *out = &g->t[o->out[0]];
            for (size_t k = 0; k < in->count; k++) out->data[k] = (uint8_t)o->lut[(int8_t)in->data[k] + 128];
            break;
        }
        case OP_RESHAPE: memcpy(g->t[o->out[0]].data, g->t[o->in[0]].data, g->t[o->out[0]].bytes); break;
        case OP_CONCAT: concat(g, o); break;
        case OP_SPLIT_V: split(g, o); break;
        case OP_STRIDED_SLICE: strided_slice(g, o); break;
        case OP_VAR_HANDLE: { int32_t id = o->sub; memcpy(g->t[o->out[0]].data, &id, 4); break; }
        case OP_READ_VAR: case OP_ASSIGN_VAR: {
            int32_t id; memcpy(&id, g->t[o->in[0]].data, 4);
            if (id < 0 || id >= m->nvar) return -1;
            if (o->code == OP_ASSIGN_VAR) {
                const struct tensor *v = &g->t[o->in[1]];
                if (!m->var[id].data) { if (!(m->var[id].data = malloc(v->bytes))) return -1; m->var[id].bytes = v->bytes; }
                if (m->var[id].bytes != v->bytes) return -1;
                memcpy(m->var[id].data, v->data, v->bytes);
            } else {
                struct tensor *out = &g->t[o->out[0]];
                if (m->var[id].bytes != out->bytes) return -1;
                memcpy(out->data, m->var[id].data, out->bytes);
            }
            break;
        }
        case OP_CALL_ONCE: if (!m->initialized) { m->initialized = 1; if (run_graph(m, o->sub)) return -1; } break;
        }
    }
    return 0;
}

void mww_model_reset(struct mww_model *m)
{
    for (int i = 0; i < m->nvar; i++) { free(m->var[i].data); m->var[i].data = NULL; m->var[i].bytes = 0; }
    m->initialized = 0;
}

int mww_model_stride(const struct mww_model *m) { return m->g[0].t[m->g[0].in].dims[1]; }

void mww_model_quantize(const struct mww_model *m, const uint16_t *features, int8_t *out)
{
    /* the front end's 0..~670 are the training's floats x 25.6 (pymicro-features hands those floats out) */
    const struct tensor *in = &m->g[0].t[m->g[0].in];
    for (size_t i = 0; i < in->count; i++) {
        double q = nearbyint((double)features[i] * 0.0390625 / (double)in->scale) + in->zp;
        out[i] = (int8_t)(q < -128 ? -128 : q > 127 ? 127 : q);
    }
}

int mww_model_run(struct mww_model *m, const uint16_t *features)
{
    struct tensor *in = &m->g[0].t[m->g[0].in], *out = &m->g[0].t[m->g[0].out];
    mww_model_quantize(m, features, (int8_t *)in->data);
    if (run_graph(m, 0)) return -1;
    /* in 256ths, as the uint8 output of microWakeWord's models is (scale 1/256): ESPHome compares those with cutoff x 255 */
    int32_t v = out->type == T_UINT8 ? out->data[0] : (int8_t)out->data[0];
    float p = (v - out->zp) * out->scale * 256.0f;
    return p < 0 ? 0 : p > 255 ? 255 : (int)lroundf(p);
}
