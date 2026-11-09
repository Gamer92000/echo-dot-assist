/* Protocol Buffers' wire format, as much of it as the ESPHome native API needs (proto_esphome.c): fields written into
 * a fixed buffer, and read one at a time out of a received message.  Header-only, so tests/unit/pb_test.c reads with it
 * under ASan/UBSan. */
#ifndef PB_H
#define PB_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct pb { unsigned char *p; size_t n, cap; };

static inline void pb_raw(struct pb *b, const void *d, size_t n) { if (b->n + n <= b->cap) { memcpy(b->p + b->n, d, n); b->n += n; } }
static inline void pb_varint(struct pb *b, uint64_t v) { unsigned char c; do { c = v & 0x7f; v >>= 7; if (v) c |= 0x80; pb_raw(b, &c, 1); } while (v); }
static inline void pb_uint(struct pb *b, int f, uint64_t v) { if (v) { pb_varint(b, f << 3); pb_varint(b, v); } }
static inline void pb_bytes(struct pb *b, int f, const void *d, size_t n) { if (n) { pb_varint(b, f << 3 | 2); pb_varint(b, n); pb_raw(b, d, n); } }
static inline void pb_str(struct pb *b, int f, const char *s) { pb_bytes(b, f, s, strlen(s)); }
static inline void pb_sint(struct pb *b, int f, int32_t v) { pb_uint(b, f, (uint32_t)v << 1 ^ (uint32_t)(v >> 31)); }   /* zigzag */
static inline void pb_fixed32(struct pb *b, int f, uint32_t v) { pb_varint(b, f << 3 | 5); pb_raw(b, &v, 4); }      /* little endian host */
static inline void pb_float(struct pb *b, int f, float v) { uint32_t u; memcpy(&u, &v, 4); pb_fixed32(b, f, u); }

struct pbf { unsigned field, wire; uint64_t v; const unsigned char *data; size_t len; };

/* 10 bytes at most (64 bits), as protobuf's own readers take them.  An 11th, or a varint cut off at the end, is
 * malformed: what comes in before a key is set comes from anyone on the network, and the shift went on past 63 bits
 * (undefined) and took a cut varint as read */
static inline int pb_uvarint(const unsigned char **p, const unsigned char *end, uint64_t *out)
{
    uint64_t v = 0;
    for (int sh = 0; sh < 64 && *p < end; sh += 7) {
        unsigned char c = *(*p)++;
        v |= (uint64_t)(c & 0x7f) << sh;
        if (!(c & 0x80)) { *out = v; return 1; }
    }
    return 0;
}

/* Field numbers are 1 to 2^29 - 1, protobuf's own limit: 0 or a larger one is malformed, as its readers take it (a key
 * past 32 bits would also have wrapped in field onto a small number).  Malformed ends the message there, as every
 * other malformed field does; such a key used to be skipped as an unknown field */
#define PB_FIELD_MAX 0x1fffffffu

static inline int pb_next(const unsigned char **p, const unsigned char *end, struct pbf *f)
{
    uint64_t k, v;
    if (!pb_uvarint(p, end, &k) || !(k >> 3) || k >> 3 > PB_FIELD_MAX) return 0;      /* also the end of the message */
    f->field = k >> 3; f->wire = k & 7; f->v = 0; f->data = NULL; f->len = 0;
    switch (f->wire) {
    case 0: if (!pb_uvarint(p, end, &v)) return 0; f->v = v; break;
    case 1: if (end - *p < 8) return 0; *p += 8; break;
    case 2:
        if (!pb_uvarint(p, end, &v) || (uint64_t)(end - *p) < v) return 0;
        f->data = *p; f->len = v; *p += v;
        break;
    case 5: if (end - *p < 4) return 0; { uint32_t u; memcpy(&u, *p, 4); f->v = u; } *p += 4; break;
    default: return 0;
    }
    return 1;
}

static inline void pbf_str(const struct pbf *f, char *out, size_t outsz)
{
    size_t n = f->len < outsz - 1 ? f->len : outsz - 1;
    memcpy(out, f->data, n); out[n] = 0;
}
#endif
