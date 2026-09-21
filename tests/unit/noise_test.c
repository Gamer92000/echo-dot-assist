/* Responder driven over stdin/stdout in hex lines by noise_ref.py (Python noiseprotocol = what aiosendspin uses). */
#include <stdio.h>
#include <string.h>
#include "noise.h"
static size_t unhex(const char *s, uint8_t *o) { size_t n = 0; unsigned v; while (sscanf(s + 2 * n, "%2x", &v) == 1) o[n++] = v; return n; }
static void puthex(const uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) printf("%02x", p[i]); printf("\n"); fflush(stdout); }
int main(void) {
    static char line[8192]; static uint8_t a[4096], b[4096], prologue[256], spriv[32], spub[32], ipub[32], psk[32];
    struct noise_hs hs; struct noise_cs tx, rx; size_t pl;
    fgets(line, sizeof line, stdin); pl = unhex(line, prologue);
    fgets(line, sizeof line, stdin); unhex(line, spriv);
    fgets(line, sizeof line, stdin); unhex(line, ipub);
    fgets(line, sizeof line, stdin); unhex(line, psk);
    { extern void crypto_x25519_public_key(uint8_t *, const uint8_t *); crypto_x25519_public_key(spub, spriv); }
    noise_kk_responder_init(&hs, prologue, pl, spriv, spub, ipub);
    fgets(line, sizeof line, stdin); size_t n = unhex(line, a);
    long p = noise_kk_read_msg1(&hs, a, n, b);
    if (p < 0) { puts("ERR msg1"); return 1; }
    puthex(b, p);                                           /* payload of message 1 */
    n = noise_kk_write_msg2(&hs, psk, "{}", 2, a, &tx, &rx); puthex(a, n);
    puthex(hs.hash_out, 32);
    for (int i = 0; i < 3; i++) {                           /* transport: decrypt, reply with the text reversed */
        fgets(line, sizeof line, stdin); n = unhex(line, a); p = noise_decrypt(&rx, a, n, b);
        if (p < 0) { puts("ERR transport"); return 1; }
        for (long j = 0; j < p / 2; j++) { uint8_t t = b[j]; b[j] = b[p - 1 - j]; b[p - 1 - j] = t; }
        n = noise_encrypt(&tx, b, p, a); puthex(a, n);
    }
    return 0;
}
