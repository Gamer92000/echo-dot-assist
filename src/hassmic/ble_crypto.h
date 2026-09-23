/* Crypto for Bluetooth LE pairing (Core spec Vol 3 Part H 2.2): AES-128, AES-CMAC and the SMP functions built on them.
 * Plain C, no controller involved, so it is tested on the PC (tests/unit/ble_crypto_test.c). */
#ifndef BLE_CRYPTO_H
#define BLE_CRYPTO_H
#include <stddef.h>
#include <stdint.h>

void aes128(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);             /* FIPS-197 byte order */
void aes_cmac(const uint8_t key[16], const uint8_t *m, size_t n, uint8_t mac[16]);   /* RFC 4493 */
void bytes_reverse(uint8_t *out, const uint8_t *in, size_t n);                        /* out may be in */

/* LE Secure Connections, in the spec's byte order: most significant octet first (PDUs carry the reverse) */
void smp_f4(const uint8_t u[32], const uint8_t v[32], const uint8_t x[16], uint8_t z, uint8_t out[16]);
void smp_f5(const uint8_t w[32], const uint8_t n1[16], const uint8_t n2[16], const uint8_t a1[7], const uint8_t a2[7],
            uint8_t mackey[16], uint8_t ltk[16]);
void smp_f6(const uint8_t w[16], const uint8_t n1[16], const uint8_t n2[16], const uint8_t r[16], const uint8_t io[3],
            const uint8_t a1[7], const uint8_t a2[7], uint8_t out[16]);

/* Legacy pairing and private address resolution, in over-the-air order: least significant octet first */
void smp_c1(const uint8_t k[16], const uint8_t r[16], const uint8_t preq[7], const uint8_t pres[7],
            uint8_t iat, const uint8_t ia[6], uint8_t rat, const uint8_t ra[6], uint8_t out[16]);
void smp_s1(const uint8_t k[16], const uint8_t r1[16], const uint8_t r2[16], uint8_t out[16]);
int  smp_ah_match(const uint8_t irk[16], const uint8_t addr[6]);     /* resolvable private address made from this IRK */
#endif
