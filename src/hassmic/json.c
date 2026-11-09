/* See json.h.  Recursive descent with an explicit depth bound; tokens in document order, so an object's keys and
 * values alternate after it and a container's first child is the token after it. */
#include "json.h"
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct p { struct json *j; const char *s; size_t i, n; };

#define JSON_MAX_TOKENS (256 << 10)                 /* (4 MB of them: a Music Assistant listing has some 100 an item) */

static void ws(struct p *p) { while (p->i < p->n && (p->s[p->i] == ' ' || p->s[p->i] == '\t' || p->s[p->i] == '\n' || p->s[p->i] == '\r')) p->i++; }

static int tok(struct p *p, int type, size_t start)
{
    struct json *j = p->j;
    if (j->n == j->cap) {
        if (j->cap >= JSON_MAX_TOKENS) return -1;              /* a document of millions of tiny values: refused */
        int cap = j->cap ? j->cap * 2 : 256; struct jtok *t = realloc(j->t, (size_t)cap * sizeof *t);
        if (!t) return -1;
        j->t = t; j->cap = cap;
    }
    j->t[j->n] = (struct jtok){ type, (int)start, (int)start, -1 };
    return j->n++;
}

static int hex4(const char *s) { for (int k = 0; k < 4; k++) { char c = s[k]; if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return 0; } return 1; }

static int string(struct p *p)
{
    size_t start = ++p->i;                                      /* past the quote */
    while (p->i < p->n && p->s[p->i] != '"') {
        unsigned char c = (unsigned char)p->s[p->i];
        if (c < 0x20) return -1;
        if (c == '\\') {
            if (p->i + 1 >= p->n) return -1;
            char e = p->s[p->i + 1];
            if (e == 'u') { if (p->i + 6 > p->n || !hex4(p->s + p->i + 2)) return -1; p->i += 6; continue; }
            if (!strchr("\"\\/bfnrt", e)) return -1;
            p->i += 2; continue;
        }
        p->i++;
    }
    if (p->i >= p->n) return -1;
    int t = tok(p, J_STR, start);
    if (t < 0) return -1;
    p->j->t[t].end = (int)p->i++;
    p->j->t[t].next = t + 1;
    return t;
}

static int value(struct p *p, int depth)
{
    ws(p);
    if (p->i >= p->n || depth > JSON_DEPTH) return -1;
    char c = p->s[p->i];
    if (c == '"') return string(p);
    if (c == '{' || c == '[') {
        int t = tok(p, c == '{' ? J_OBJ : J_ARR, p->i), first = 1;
        if (t < 0) return -1;
        p->i++;
        for (;;) {
            ws(p);
            if (p->i >= p->n) return -1;
            if (p->s[p->i] == (c == '{' ? '}' : ']')) { p->i++; break; }
            if (!first) { if (p->s[p->i] != ',') return -1; p->i++; ws(p); }
            first = 0;
            if (c == '{') {
                if (p->i >= p->n || p->s[p->i] != '"' || string(p) < 0) return -1;
                ws(p);
                if (p->i >= p->n || p->s[p->i] != ':') return -1;
                p->i++;
            }
            if (value(p, depth + 1) < 0) return -1;
        }
        p->j->t[t].end = (int)p->i; p->j->t[t].next = p->j->n;
        return t;
    }
    size_t start = p->i;
    if ((c >= '0' && c <= '9') || c == '-') {
        while (p->i < p->n && strchr("0123456789+-.eE", p->s[p->i])) p->i++;
        int t = tok(p, J_NUM, start);
        if (t < 0) return -1;
        p->j->t[t].end = (int)p->i; p->j->t[t].next = t + 1;
        return t;
    }
    static const struct { const char *w; int type; } words[] = { { "true", J_TRUE }, { "false", J_FALSE }, { "null", J_NULL } };
    for (int k = 0; k < 3; k++) {
        size_t l = strlen(words[k].w);
        if (p->n - p->i >= l && !memcmp(p->s + p->i, words[k].w, l)) {
            int t = tok(p, words[k].type, start);
            if (t < 0) return -1;
            p->i += l; p->j->t[t].end = (int)p->i; p->j->t[t].next = t + 1;
            return t;
        }
    }
    return -1;
}

int json_parse(struct json *j, const char *s, size_t len)
{
    j->s = s; j->n = 0;
    if (len > 0x3FFFFFFF) return -1;
    struct p p = { j, s, 0, len };
    if (value(&p, 0) < 0) return -1;
    ws(&p);
    return p.i == p.n || (p.i < p.n && !s[p.i]) ? 0 : -1;
}

void json_free(struct json *j) { free(j->t); j->t = NULL; j->n = j->cap = 0; }

int json_type(const struct json *j, int t) { return t >= 0 && t < j->n ? j->t[t].type : 0; }

int json_first(const struct json *j, int t)
{
    if (t < 0 || t >= j->n || (j->t[t].type != J_OBJ && j->t[t].type != J_ARR)) return -1;
    return t + 1 < j->t[t].next ? t + 1 : -1;
}

int json_next(const struct json *j, int in, int t)
{
    if (t < 0 || t >= j->n || in < 0 || in >= j->n) return -1;
    int nx = j->t[t].next;
    return nx < j->t[in].next ? nx : -1;
}

int json_len(const struct json *j, int t)
{
    int n = 0, obj = json_type(j, t) == J_OBJ;
    for (int k = json_first(j, t); k >= 0 && k < j->t[t].next; k = j->t[k].next) n++;
    return obj ? n / 2 : n;
}

int json_get(const struct json *j, int obj, const char *key)
{
    if (json_type(j, obj) != J_OBJ) return -1;
    size_t kl = strlen(key); int end = j->t[obj].next;
    for (int k = obj + 1; k < end; ) {
        int v = j->t[k].next;                                   /* the key's value */
        if (v >= end) break;
        const struct jtok *t = &j->t[k];
        if ((size_t)(t->end - t->start) == kl && !memcmp(j->s + t->start, key, kl) && !memchr(j->s + t->start, '\\', kl)) return v;
        k = j->t[v].next;
    }
    return -1;
}

static size_t put_utf8(char *o, unsigned v)
{
    if (v < 0x80) { o[0] = (char)v; return 1; }
    if (v < 0x800) { o[0] = (char)(0xC0 | v >> 6); o[1] = (char)(0x80 | (v & 63)); return 2; }
    if (v < 0x10000) { o[0] = (char)(0xE0 | v >> 12); o[1] = (char)(0x80 | (v >> 6 & 63)); o[2] = (char)(0x80 | (v & 63)); return 3; }
    o[0] = (char)(0xF0 | v >> 18); o[1] = (char)(0x80 | (v >> 12 & 63)); o[2] = (char)(0x80 | (v >> 6 & 63)); o[3] = (char)(0x80 | (v & 63));
    return 4;
}

int json_str(const struct json *j, int t, char *out, size_t n)
{
    if (!n) return 0;
    out[0] = 0;
    int ty = json_type(j, t);
    if (ty == J_NUM || ty == J_TRUE || ty == J_FALSE) {
        size_t l = (size_t)(j->t[t].end - j->t[t].start);
        if (l >= n) l = n - 1;
        memcpy(out, j->s + j->t[t].start, l); out[l] = 0;
        return 1;
    }
    if (ty != J_STR) return 0;
    size_t o = 0;
    for (int i = j->t[t].start; i < j->t[t].end; ) {
        char u[4]; size_t k = 1; unsigned char c = (unsigned char)j->s[i];
        if (c == '\\') {
            char e = j->s[i + 1]; i += 2;
            switch (e) {
            case 'n': case 'r': case 't': u[0] = ' '; break;   /* what is shown on one line */
            case 'b': case 'f': continue;
            case 'u': {
                unsigned v = (unsigned)strtoul((char[]){ j->s[i], j->s[i + 1], j->s[i + 2], j->s[i + 3], 0 }, NULL, 16); i += 4;
                if (v >= 0xD800 && v < 0xDC00 && i + 6 <= j->t[t].end && j->s[i] == '\\' && j->s[i + 1] == 'u' && hex4(j->s + i + 2)) {
                    unsigned lo = (unsigned)strtoul((char[]){ j->s[i + 2], j->s[i + 3], j->s[i + 4], j->s[i + 5], 0 }, NULL, 16);
                    if (lo >= 0xDC00 && lo < 0xE000) { v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00); i += 6; }
                }
                if (v >= 0xD800 && v < 0xE000) v = 0xFFFD;      /* a lone surrogate */
                if (!v) continue;
                if (v < 0x20) v = ' ';                         /* control characters: one line, as \n */
                k = put_utf8(u, v);
            } break;
            default: u[0] = e;
            }
        } else {                                                /* a character as written: its whole sequence */
            k = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
            if (i + (int)k > j->t[t].end) k = 1;
            for (size_t m = 1; m < k; m++)                      /* a lead byte without its sequence: one byte, or */
                if (((unsigned char)j->s[i + m] & 0xC0) != 0x80) { k = 1; break; }   /* it would eat the \ after it */
            memcpy(u, j->s + i, k); i += (int)k;
        }
        if (o + k >= n) break;                                  /* never half a character */
        memcpy(out + o, u, k); o += k;
    }
    out[o] = 0;
    return 1;
}

long long json_int(const struct json *j, int t, long long def)
{
    if (json_type(j, t) != J_NUM) return def;
    char b[40]; json_str(j, t, b, sizeof b);
    if (!strchr(b, '.') && !strchr(b, 'e') && !strchr(b, 'E')) return strtoll(b, NULL, 10);   /* (saturates) */
    double d = strtod(b, NULL);                                 /* 1e999: out of range, the cast undefined */
    return d != d ? def : d >= 9.2e18 ? LLONG_MAX : d <= -9.2e18 ? LLONG_MIN : (long long)d;
}

double json_num(const struct json *j, int t, double def)
{
    if (json_type(j, t) != J_NUM) return def;
    char b[40]; json_str(j, t, b, sizeof b);
    return strtod(b, NULL);
}

int json_bool(const struct json *j, int t, int def)
{
    int ty = json_type(j, t);
    return ty == J_TRUE ? 1 : ty == J_FALSE ? 0 : ty == J_NUM ? json_num(j, t, 0) != 0 : def;
}

int json_streq(const struct json *j, int t, const char *s)
{
    if (json_type(j, t) != J_STR) return 0;
    size_t l = strlen(s);
    if ((size_t)(j->t[t].end - j->t[t].start) == l && !memcmp(j->s + j->t[t].start, s, l)) return 1;
    char b[256]; if (l >= sizeof b) return 0;
    json_str(j, t, b, sizeof b);
    return !strcmp(b, s);
}

/* too long for out: out is "" (an empty JSON string where there is room for one) and -1 */
static int esc_fail(char *out, size_t n) { if (n >= 3) memcpy(out, "\"\"", 3); else if (n) out[0] = 0; return -1; }

int json_esc(char *out, size_t n, const char *s)
{
    size_t o = 0;
    #define PUT(c) do { if (o + 1 >= n) return esc_fail(out, n); out[o++] = (c); } while (0)
    PUT('"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { PUT('\\'); PUT((char)c); }
        else if (c < 0x20) { if (o + 7 >= n) return esc_fail(out, n); o += (size_t)snprintf(out + o, n - o, "\\u%04x", c); }
        else PUT((char)c);
    }
    PUT('"');
    out[o] = 0;
    return (int)o;
    #undef PUT
}
