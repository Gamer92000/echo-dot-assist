/* Zigbee's cryptography: AES-128 (mbedTLS, the same build hassmic's TLS uses), CCM* with a 4-byte MIC (security level
 * 5, ENC-MIC-32, the only level Zigbee PRO uses), the Matyas-Meyer-Oseas hash (Zigbee spec B.6), its HMAC (B.1.4: the
 * key-transport key is HMAC(link key, 0x00), the key-load key HMAC(link key, 0x02), a verify key's hash HMAC(key,
 * 0x03)) and the link key of an install code (its MMO hash, CRC included).  tests/unit/zb_crypto_test.c holds them to
 * the spec's vectors. */
#include "zb.h"
#include <mbedtls/aes.h>
#include <string.h>

void aes128(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    mbedtls_aes_context c;
    mbedtls_aes_init(&c);
    mbedtls_aes_setkey_enc(&c, key, 128);
    mbedtls_aes_crypt_ecb(&c, MBEDTLS_AES_ENCRYPT, in, out);
    mbedtls_aes_free(&c);
}

/* CBC-MAC over B0, the header's length and bytes, the payload: T (16 bytes, the first 4 count) */
static void ccm_mac(const uint8_t key[16], const uint8_t nonce[13], const uint8_t *a, size_t al, const uint8_t *m, size_t ml, uint8_t t[16])
{
    uint8_t b[16], x[16] = { 0 };
    size_t i, k;
    b[0] = (uint8_t)((al ? 0x40 : 0) | ((4 - 2) / 2) << 3 | (2 - 1));
    memcpy(b + 1, nonce, 13);
    b[14] = (uint8_t)(ml >> 8); b[15] = (uint8_t)ml;
    for (i = 0; i < 16; i++) x[i] ^= b[i];
    aes128(key, x, x);
    if (al) {
        /* the header's length (2 bytes, big-endian: Zigbee's headers are short), then the header, padded */
        uint8_t blk[16]; size_t p = 2;
        memset(blk, 0, 16); blk[0] = (uint8_t)(al >> 8); blk[1] = (uint8_t)al;
        for (k = 0; k < al; k++) {
            blk[p++] = a[k];
            if (p == 16) { for (i = 0; i < 16; i++) x[i] ^= blk[i]; aes128(key, x, x); memset(blk, 0, 16); p = 0; }
        }
        if (p) { for (i = 0; i < 16; i++) x[i] ^= blk[i]; aes128(key, x, x); }
    }
    for (k = 0; k < ml; k += 16) {
        uint8_t blk[16] = { 0 };
        memcpy(blk, m + k, ml - k < 16 ? ml - k : 16);
        for (i = 0; i < 16; i++) x[i] ^= blk[i];
        aes128(key, x, x);
    }
    memcpy(t, x, 16);
}

/* the key stream's block i (A_i = flags 1, nonce, counter i big-endian) */
static void ccm_ctr(const uint8_t key[16], const uint8_t nonce[13], unsigned i, uint8_t s[16])
{
    uint8_t a[16];
    a[0] = 2 - 1;
    memcpy(a + 1, nonce, 13);
    a[14] = (uint8_t)(i >> 8); a[15] = (uint8_t)i;
    aes128(key, a, s);
}

void ccm_encrypt(const uint8_t key[16], const uint8_t nonce[13], const uint8_t *a, size_t al, uint8_t *m, size_t ml)
{
    uint8_t t[16], s[16];
    ccm_mac(key, nonce, a, al, m, ml, t);
    for (size_t k = 0; k < ml; k += 16) {
        ccm_ctr(key, nonce, (unsigned)(k / 16 + 1), s);
        for (size_t i = 0; i < 16 && k + i < ml; i++) m[k + i] ^= s[i];
    }
    ccm_ctr(key, nonce, 0, s);
    for (int i = 0; i < 4; i++) m[ml + i] = t[i] ^ s[i];
}

int ccm_decrypt(const uint8_t key[16], const uint8_t nonce[13], const uint8_t *a, size_t al, uint8_t *m, size_t n)
{
    uint8_t t[16], s[16], mic[4];
    int bad = 0;
    if (n < 4) return -1;
    size_t ml = n - 4;
    ccm_ctr(key, nonce, 0, s);
    for (int i = 0; i < 4; i++) mic[i] = m[ml + i] ^ s[i];
    for (size_t k = 0; k < ml; k += 16) {
        ccm_ctr(key, nonce, (unsigned)(k / 16 + 1), s);
        for (size_t i = 0; i < 16 && k + i < ml; i++) m[k + i] ^= s[i];
    }
    ccm_mac(key, nonce, a, al, m, ml, t);
    for (int i = 0; i < 4; i++) bad |= t[i] ^ mic[i];
#ifdef ZB_FUZZ
    (void)bad; return 0;                /* tests/unit/zb_fuzz.c: past the MIC to what lies behind it */
#endif
    return bad ? -1 : 0;
}

/* B.6: M || 1 || 0...0 || l (16 bits, l in bits) to a whole number of 16-byte blocks; H_j = E(H_{j-1}, M_j) ^ M_j */
void mmo_hash(const uint8_t *in, size_t n, uint8_t out[16])
{
    uint8_t h[16] = { 0 }, blk[16], e[16];
    size_t bits = n * 8, total = n + 1 + 2, k, i;
    while (total % 16) total++;
    for (k = 0; k < total; k += 16) {
        for (i = 0; i < 16; i++) {
            size_t p = k + i;
            blk[i] = p < n ? in[p] : p == n ? 0x80 : p == total - 2 ? (uint8_t)(bits >> 8) : p == total - 1 ? (uint8_t)bits : 0;
        }
        aes128(h, blk, e);
        for (i = 0; i < 16; i++) h[i] = e[i] ^ blk[i];
    }
    memcpy(out, h, 16);
}

void keyed_hash(const uint8_t key[16], uint8_t in, uint8_t out[16])
{
    uint8_t a[17], b[32], inner[16];
    for (int i = 0; i < 16; i++) a[i] = key[i] ^ 0x36;
    a[16] = in;
    mmo_hash(a, 17, inner);
    for (int i = 0; i < 16; i++) b[i] = key[i] ^ 0x5c;
    memcpy(b + 16, inner, 16);
    mmo_hash(b, 32, out);
}

/* the CRC an install code carries: CRC-16/X-25 (reflected 0x1021, init and final xor 0xFFFF), little-endian */
static uint16_t x25(const uint8_t *p, size_t n)
{
    uint16_t c = 0xFFFF;
    while (n--) { c ^= *p++; for (int i = 0; i < 8; i++) c = c & 1 ? (uint16_t)((c >> 1) ^ 0x8408) : (uint16_t)(c >> 1); }
    return (uint16_t)~c;
}

int install_code_key(const uint8_t *code, size_t n, uint8_t key[16])
{
    if (n != 8 && n != 10 && n != 14 && n != 18) return -1;   /* 6, 8, 12 or 16 bytes and the CRC */
    if (x25(code, n - 2) != rd16(code + n - 2)) return -1;
    mmo_hash(code, n, key);
    return 0;
}
