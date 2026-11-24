/* EdDSA public keys that vouch for anything (a review of a30d6ed, K3).
 *
 * A public key of small order (the identity, the all-zero bytes, the other six points of order 2, 4 and 8, in any of
 * their encodings) takes a signature nobody made: Monocypher's crypto_eddsa_check is cofactored (it compares
 * [8]([s]B - [h]A - R) with the identity), and under such a key [8][h]A is the identity for every message, so the null
 * signature (R the identity, s = 0) verifies for every text.  Measured: a build with PLUGINS_PUB2 = 64 zeros took an
 * index, a revocation list and a listing whose signature was {1, 0, ...}.  A key made by keygen is [a]B for a secret
 * scalar and never one of these; a placeholder, a file of zeros or a typed-in key may be.
 *
 * So every key hassmic or its tools take (the index's, the release's, the update key, a plugin author's) is asked
 * this first: 1 when the null signature verifies under PK, which is exactly "PK has small order" (scripts/otatool.py
 * weak() is the rule in Python, and refuses besides every encoding it does not read as a point, six of which
 * Monocypher reads as small order).  Held to the points by tests/unit/plugin_index_test.c (weak_keys: the eight and
 * the identity's non-canonical form, here and as each key) and tests/otatool_test.sh (weak() and both otatools,
 * those six included).  A key that does not decode is not weak by this test: every check under it fails anyway */
#ifndef HASSMIC_EDKEY_H
#define HASSMIC_EDKEY_H
#include <stdint.h>
#include "../third_party/monocypher.h"

static inline int ed_key_weak(const uint8_t pk[32])
{
    static const uint8_t null_sig[64] = { 1 };      /* R = the identity (y = 1), s = 0 */
    return crypto_eddsa_check(null_sig, pk, (const uint8_t *)"", 0) == 0;
}
#endif
