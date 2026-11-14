/* SHA-1 (WebSocket accept key only), SHA-256, HMAC-SHA-256, base64 / base64url.  Small and plain; not constant time where
 * it does not matter (hashes are, base64 of public data is not an issue). */
#include "hash.h"
#include <string.h>

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

/* ---------------------------------------------------------------- SHA-256 */

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };

static void sha256_block(struct sha256 *c, const uint8_t *p)
{
    uint32_t w[64], s[8];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(s, c->h, sizeof s);
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR(s[4], 6) ^ ROR(s[4], 11) ^ ROR(s[4], 25), ch = (s[4] & s[5]) ^ (~s[4] & s[6]);
        uint32_t t1 = s[7] + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ROR(s[0], 2) ^ ROR(s[0], 13) ^ ROR(s[0], 22), maj = (s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]);
        s[7] = s[6]; s[6] = s[5]; s[5] = s[4]; s[4] = s[3] + t1; s[3] = s[2]; s[2] = s[1]; s[1] = s[0]; s[0] = t1 + S0 + maj;
    }
    for (int i = 0; i < 8; i++) c->h[i] += s[i];
}

void sha256_init(struct sha256 *c)
{
    static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(c->h, iv, sizeof iv); c->len = 0; c->n = 0;
}

void sha256_update(struct sha256 *c, const void *data, size_t len)
{
    const uint8_t *p = data;
    c->len += len;
    while (len) {
        size_t k = 64 - c->n < len ? 64 - c->n : len;
        memcpy(c->buf + c->n, p, k); c->n += k; p += k; len -= k;
        if (c->n == 64) { sha256_block(c, c->buf); c->n = 0; }
    }
}

void sha256_final(struct sha256 *c, uint8_t out[32])
{
    uint64_t bits = c->len * 8; uint8_t pad[72] = { 0x80 }, lenb[8];
    size_t padlen = (c->n < 56 ? 56 : 120) - c->n;
    for (int i = 0; i < 8; i++) lenb[i] = bits >> (56 - 8 * i);
    sha256_update(c, pad, padlen); sha256_update(c, lenb, 8);
    for (int i = 0; i < 8; i++) { out[4 * i] = c->h[i] >> 24; out[4 * i + 1] = c->h[i] >> 16; out[4 * i + 2] = c->h[i] >> 8; out[4 * i + 3] = c->h[i]; }
}

void sha256(const void *data, size_t len, uint8_t out[32])
{
    struct sha256 c; sha256_init(&c); sha256_update(&c, data, len); sha256_final(&c, out);
}

void hmac_sha256(const uint8_t *key, size_t keylen, const void *a, size_t alen, const void *b, size_t blen, uint8_t out[32])
{
    uint8_t k[64] = { 0 }, pad[64], inner[32]; struct sha256 c;
    if (keylen > 64) sha256(key, keylen, k); else memcpy(k, key, keylen);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha256_init(&c); sha256_update(&c, pad, 64); sha256_update(&c, a, alen); if (blen) sha256_update(&c, b, blen); sha256_final(&c, inner);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sha256_init(&c); sha256_update(&c, pad, 64); sha256_update(&c, inner, 32); sha256_final(&c, out);
}

/* ---------------------------------------------------------------- SHA-1 */

void sha1(const void *data, size_t len, uint8_t out[20])
{
    uint32_t h[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
    const uint8_t *p = data; uint8_t tail[128] = { 0 }; size_t full = len / 64 * 64, rest = len - full, tl;
    memcpy(tail, p + full, rest); tail[rest] = 0x80;
    tl = rest < 56 ? 64 : 128;
    for (int i = 0; i < 8; i++) tail[tl - 1 - i] = ((uint64_t)len * 8) >> (8 * i);
    for (size_t off = 0; off < full + tl; off += 64) {
        const uint8_t *b = off < full ? p + off : tail + (off - full); uint32_t w[80], a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)b[4 * i] << 24 | b[4 * i + 1] << 16 | b[4 * i + 2] << 8 | b[4 * i + 3];
        for (int i = 16; i < 80; i++) w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (bb & c) | (~bb & d); k = 0x5a827999; } else if (i < 40) { f = bb ^ c ^ d; k = 0x6ed9eba1; }
            else if (i < 60) { f = (bb & c) | (bb & d) | (c & d); k = 0x8f1bbcdc; } else { f = bb ^ c ^ d; k = 0xca62c1d6; }
            uint32_t t = ROL(a, 5) + f + e + k + w[i]; e = d; d = c; c = ROL(bb, 30); bb = a; a = t;
        }
        h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e;
    }
    for (int i = 0; i < 5; i++) { out[4 * i] = h[i] >> 24; out[4 * i + 1] = h[i] >> 16; out[4 * i + 2] = h[i] >> 8; out[4 * i + 3] = h[i]; }
}

/* MD5 (RFC 1321): SIP's digest authentication (sip.c, RFC 2617) asks for it, nothing else */
void md5(const void *data, size_t len, uint8_t out[16])
{
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af,
        0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
        0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
        0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97,
        0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
    static const uint8_t R[64] = { 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };
    uint32_t h[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    const uint8_t *p = data; uint8_t tail[128] = { 0 }; size_t full = len / 64 * 64, rest = len - full, tl;
    memcpy(tail, p + full, rest); tail[rest] = 0x80;
    tl = rest < 56 ? 64 : 128;
    for (int i = 0; i < 8; i++) tail[tl - 8 + i] = ((uint64_t)len * 8) >> (8 * i);     /* little endian, unlike SHA */
    for (size_t off = 0; off < full + tl; off += 64) {
        const uint8_t *b = off < full ? p + off : tail + (off - full); uint32_t w[16], a = h[0], bb = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 16; i++) w[i] = b[4 * i] | b[4 * i + 1] << 8 | b[4 * i + 2] << 16 | (uint32_t)b[4 * i + 3] << 24;
        for (int i = 0; i < 64; i++) {
            uint32_t f; int g;
            if (i < 16) { f = (bb & c) | (~bb & d); g = i; } else if (i < 32) { f = (d & bb) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = bb ^ c ^ d; g = (3 * i + 5) % 16; } else { f = c ^ (bb | ~d); g = 7 * i % 16; }
            uint32_t t = d; d = c; c = bb; bb = bb + ROL(a + f + K[i] + w[g], R[i]); a = t;
        }
        h[0] += a; h[1] += bb; h[2] += c; h[3] += d;
    }
    for (int i = 0; i < 4; i++) { out[4 * i] = h[i]; out[4 * i + 1] = h[i] >> 8; out[4 * i + 2] = h[i] >> 16; out[4 * i + 3] = h[i] >> 24; }
}

/* ---------------------------------------------------------------- base64 */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char B64URL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

size_t b64_encode(const void *data, size_t len, char *out, int url, int pad)
{
    const uint8_t *p = data; const char *abc = url ? B64URL : B64; size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = p[i] << 16 | (i + 1 < len ? p[i + 1] << 8 : 0) | (i + 2 < len ? p[i + 2] : 0);
        out[o++] = abc[v >> 18]; out[o++] = abc[v >> 12 & 63];
        if (i + 1 < len) out[o++] = abc[v >> 6 & 63]; else if (pad) out[o++] = '=';
        if (i + 2 < len) out[o++] = abc[v & 63]; else if (pad) out[o++] = '=';
    }
    out[o] = 0;
    return o;
}

/* Accepts both alphabets, with or without padding.  Returns decoded length, or -1 on a bad character / too small buffer. */
long b64_decode(const char *in, size_t inlen, uint8_t *out, size_t cap)
{
    uint32_t acc = 0; int bits = 0; size_t o = 0;
    for (size_t i = 0; i < inlen; i++) {
        char ch = in[i]; int v;
        if (ch == '=') break;
        if (ch >= 'A' && ch <= 'Z') v = ch - 'A'; else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26; else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
        else if (ch == '+' || ch == '-') v = 62; else if (ch == '/' || ch == '_') v = 63; else return -1;
        acc = acc << 6 | v; bits += 6;
        if (bits >= 8) { bits -= 8; if (o >= cap) return -1; out[o++] = acc >> bits & 0xff; }
    }
    return (long)o;
}
