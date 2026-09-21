/* Noise_KKpsk2_25519_ChaChaPoly_SHA256, responder side (Sendspin: the server initiates), per the Noise spec rev 34.
 *
 *   -> s          pre-message: initiator static
 *   <- s          pre-message: responder static
 *   ...
 *   -> e, es, ss
 *   <- e, ee, se, psk
 *
 * PSK handshakes also MixKey() every ephemeral public key.  X25519 and ChaCha20-Poly1305 come from Monocypher. */
#include "noise.h"
#include <string.h>
#include "../third_party/monocypher.h"
#include "hash.h"
#include "ws.h"

static const char PROTOCOL[] = "Noise_KKpsk2_25519_ChaChaPoly_SHA256";

static void mix_hash(struct noise_hs *s, const void *data, size_t len)
{
    struct sha256 c; sha256_init(&c); sha256_update(&c, s->h, 32); sha256_update(&c, data, len); sha256_final(&c, s->h);
}

/* HKDF as Noise defines it: up to three 32 byte outputs */
static void hkdf(const uint8_t ck[32], const void *in, size_t inlen, uint8_t *o1, uint8_t *o2, uint8_t *o3)
{
    uint8_t temp[32], one = 1, two = 2, three = 3;
    hmac_sha256(ck, 32, in, inlen, NULL, 0, temp);
    hmac_sha256(temp, 32, &one, 1, NULL, 0, o1);
    hmac_sha256(temp, 32, o1, 32, &two, 1, o2);
    if (o3) hmac_sha256(temp, 32, o2, 32, &three, 1, o3);
    crypto_wipe(temp, sizeof temp);
}

static void mix_key(struct noise_hs *s, const void *in, size_t inlen)
{
    uint8_t ck[32];
    hkdf(s->ck, in, inlen, ck, s->k, NULL);
    memcpy(s->ck, ck, 32); s->has_k = 1; s->n = 0;
}

static void mix_key_and_hash(struct noise_hs *s, const void *in, size_t inlen)
{
    uint8_t ck[32], th[32];
    hkdf(s->ck, in, inlen, ck, th, s->k);
    memcpy(s->ck, ck, 32); mix_hash(s, th, 32); s->has_k = 1; s->n = 0;
}

static void nonce12(uint64_t n, uint8_t out[12])
{
    memset(out, 0, 4);
    for (int i = 0; i < 8; i++) out[4 + i] = n >> (8 * i);
}

static void aead_encrypt(const uint8_t k[32], uint64_t n, const void *ad, size_t adlen, const void *pt, size_t len, uint8_t *out)
{
    crypto_aead_ctx ctx; uint8_t nonce[12];
    nonce12(n, nonce);
    crypto_aead_init_ietf(&ctx, k, nonce);
    crypto_aead_write(&ctx, out, out + len, ad, adlen, pt, len);       /* ciphertext || 16 byte tag */
    crypto_wipe(&ctx, sizeof ctx);
}

static int aead_decrypt(const uint8_t k[32], uint64_t n, const void *ad, size_t adlen, const uint8_t *ct, size_t len, uint8_t *out)
{
    crypto_aead_ctx ctx; uint8_t nonce[12]; int rc;
    if (len < NOISE_TAG) return -1;
    nonce12(n, nonce);
    crypto_aead_init_ietf(&ctx, k, nonce);
    rc = crypto_aead_read(&ctx, out, ct + len - NOISE_TAG, ad, adlen, ct, len - NOISE_TAG);
    crypto_wipe(&ctx, sizeof ctx);
    return rc;
}

void noise_keypair(uint8_t priv[32], uint8_t pub[32])
{
    ws_random(priv, 32);
    crypto_x25519_public_key(pub, priv);
}

void noise_kk_responder_init(struct noise_hs *s, const void *prologue, size_t plen,
                             const uint8_t s_priv[32], const uint8_t s_pub[32], const uint8_t initiator_pub[32])
{
    memset(s, 0, sizeof *s);
    sha256(PROTOCOL, sizeof PROTOCOL - 1, s->h);
    memcpy(s->ck, s->h, 32);
    mix_hash(s, prologue, plen);
    memcpy(s->s_priv, s_priv, 32); memcpy(s->s_pub, s_pub, 32); memcpy(s->rs, initiator_pub, 32);
    mix_hash(s, s->rs, 32);             /* -> s */
    mix_hash(s, s->s_pub, 32);          /* <- s */
}

long noise_kk_read_msg1(struct noise_hs *s, const uint8_t *msg, size_t len, uint8_t *payload)
{
    uint8_t dh[32];
    if (len < 32 + NOISE_TAG) return -1;
    memcpy(s->re, msg, 32);
    mix_hash(s, s->re, 32); mix_key(s, s->re, 32);                          /* e (psk mode) */
    crypto_x25519(dh, s->s_priv, s->re); mix_key(s, dh, 32);                /* es */
    crypto_x25519(dh, s->s_priv, s->rs); mix_key(s, dh, 32);                /* ss */
    crypto_wipe(dh, sizeof dh);
    if (aead_decrypt(s->k, s->n++, s->h, 32, msg + 32, len - 32, payload)) return -1;
    mix_hash(s, msg + 32, len - 32);
    return (long)(len - 32 - NOISE_TAG);
}

size_t noise_kk_write_msg2(struct noise_hs *s, const uint8_t psk[32], const void *payload, size_t plen, uint8_t *out,
                           struct noise_cs *send, struct noise_cs *recv)
{
    uint8_t dh[32], k1[32], k2[32];
    if (!s->e_set) noise_keypair(s->e_priv, s->e_pub);                      /* tests inject a fixed ephemeral */
    memcpy(out, s->e_pub, 32);
    mix_hash(s, s->e_pub, 32); mix_key(s, s->e_pub, 32);                    /* e (psk mode) */
    crypto_x25519(dh, s->e_priv, s->re); mix_key(s, dh, 32);                /* ee */
    crypto_x25519(dh, s->e_priv, s->rs); mix_key(s, dh, 32);                /* se: initiator static x responder ephemeral */
    mix_key_and_hash(s, psk, 32);                                           /* psk2 */
    aead_encrypt(s->k, s->n++, s->h, 32, payload, plen, out + 32);
    mix_hash(s, out + 32, plen + NOISE_TAG);

    hkdf(s->ck, NULL, 0, k1, k2, NULL);                                     /* Split(): initiator sends with k1 */
    memcpy(recv->k, k1, 32); recv->n = 0;
    memcpy(send->k, k2, 32); send->n = 0;
    memcpy(s->hash_out, s->h, 32);                                          /* handshake hash: channel binding for pairing */
    crypto_wipe(dh, sizeof dh); crypto_wipe(k1, 32); crypto_wipe(k2, 32);
    crypto_wipe(s->e_priv, 32); crypto_wipe(s->k, 32); crypto_wipe(s->ck, 32);
    return 32 + plen + NOISE_TAG;
}

size_t noise_encrypt(struct noise_cs *c, const void *pt, size_t len, uint8_t *out)
{
    aead_encrypt(c->k, c->n++, NULL, 0, pt, len, out);
    return len + NOISE_TAG;
}

long noise_decrypt(struct noise_cs *c, const uint8_t *ct, size_t len, uint8_t *out)
{
    if (aead_decrypt(c->k, c->n, NULL, 0, ct, len, out)) return -1;        /* nonce only advances on success */
    c->n++;
    return (long)(len - NOISE_TAG);
}
