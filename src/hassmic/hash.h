/* SHA-1, SHA-256, HMAC-SHA-256, base64: what WebSocket and Noise need beyond the vendored Monocypher. */
#ifndef HASH_H
#define HASH_H
#include <stddef.h>
#include <stdint.h>

struct sha256 { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n; };
void sha256_init(struct sha256 *c);
void sha256_update(struct sha256 *c, const void *data, size_t len);
void sha256_final(struct sha256 *c, uint8_t out[32]);
void sha256(const void *data, size_t len, uint8_t out[32]);
/* HMAC over a || b (b may be empty): HKDF needs "previous output || counter byte" */
void hmac_sha256(const uint8_t *key, size_t keylen, const void *a, size_t alen, const void *b, size_t blen, uint8_t out[32]);
void sha1(const void *data, size_t len, uint8_t out[20]);

size_t b64_encode(const void *data, size_t len, char *out, int url, int pad);       /* out: 4*ceil(len/3)+1 bytes */
long   b64_decode(const char *in, size_t inlen, uint8_t *out, size_t cap);
#endif
