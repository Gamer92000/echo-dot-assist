/* Noise_KKpsk2_25519_ChaChaPoly_SHA256, responder side, plus the transport cipher states.  See noise.c. */
#ifndef NOISE_H
#define NOISE_H
#include <stddef.h>
#include <stdint.h>

#define NOISE_TAG 16

struct noise_hs {
    uint8_t h[32], ck[32], k[32]; int has_k; uint64_t n;
    uint8_t s_priv[32], s_pub[32], rs[32], e_priv[32], e_pub[32], re[32]; int e_set;
    uint8_t hash_out[32];                       /* handshake hash, valid after noise_kk_write_msg2() */
};
struct noise_cs { uint8_t k[32]; uint64_t n; };

void   noise_keypair(uint8_t priv[32], uint8_t pub[32]);
void   noise_kk_responder_init(struct noise_hs *s, const void *prologue, size_t plen,
                               const uint8_t s_priv[32], const uint8_t s_pub[32], const uint8_t initiator_pub[32]);
/* payload needs room for len - 48 bytes.  Returns the payload length, -1 if the message does not authenticate. */
long   noise_kk_read_msg1(struct noise_hs *s, const uint8_t *msg, size_t len, uint8_t *payload);
/* out needs 32 + plen + 16 bytes.  The PSK may be chosen after message 1 was read (its payload names it). */
size_t noise_kk_write_msg2(struct noise_hs *s, const uint8_t psk[32], const void *payload, size_t plen, uint8_t *out,
                           struct noise_cs *send, struct noise_cs *recv);
size_t noise_encrypt(struct noise_cs *c, const void *pt, size_t len, uint8_t *out);         /* out: len + 16 */
long   noise_decrypt(struct noise_cs *c, const uint8_t *ct, size_t len, uint8_t *out);      /* -1 = forged / out of order */
#endif
