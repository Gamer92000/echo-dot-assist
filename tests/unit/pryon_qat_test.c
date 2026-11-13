/* pryon_qat.c's exp/log/log1p must equal the Echo's own libm (bionic, Android 7.1.2) bit for bit: the wake word trainer
 * on a PC and on the Echo then train the same model.  A fixed sequence of inputs, a hash of the results.  The expected
 * 64-bit FNV-1a hash was taken from the Echo's libm (and, identically, from the firmware's libm.so under qemu):
 *   cc -DUSE_LIBM ... then run it on the Echo, or with qemu-arm -L firmware/donut/rootfs
 * The two agree on every one of 240,006 inputs of the longer sequence the first measurement used (docs/re-pryon-training.md); glibc's differ on 7% of exp results. */
#include "../../src/tools/pryon_qat.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define N 200000
static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t next(void) { rng ^= rng >> 12; rng ^= rng << 25; rng ^= rng >> 27; return rng * 0x2545F4914F6CDD1Dull; }
static double unit(void) { return (double)(next() >> 11) / 9007199254740992.0; }   /* [0, 1) */

static uint64_t h = 0xcbf29ce484222325ull;                 /* FNV-1a over the result bits, NaNs made one NaN */
static void put(double v)
{
    uint64_t b; if (v != v) b = 0x7ff8000000000000ull; else memcpy(&b, &v, 8);
    for (int i = 0; i < 8; i++) { h ^= (b >> (8 * i)) & 0xff; h *= 0x100000001b3ull; }
}

int main(void)
{
    for (int i = 0; i < N; i++) {
        double x;
        switch (i % 5) {
        case 0: x = -60 * unit(); break;                   /* sigmoid, softmax */
        case 1: x = -700 + 1400 * unit(); break;
        case 2: x = unit(); break;
        case 3: x = pq_exp(-40 + 80 * unit()); break;      /* log's whole range; not the platform's exp: the inputs must not depend on it */
        default: x = -1 + 1e-12 + (1 - 1e-12) * unit(); break;  /* log1p(-p) */
        }
#ifdef USE_LIBM
        put(exp(x)); put(log(x)); put(log1p(x));
#else
        put(pq_exp(x)); put(pq_log(x)); put(pq_log1p(x));
#endif
    }
    printf("%016llx\n", (unsigned long long)h);
#ifdef USE_LIBM
    return 0;
#else
    if (h != 0x96594667583f1fd3ull) { fprintf(stderr, "pryon_qat: exp/log/log1p differ from the Echo's libm\n"); return 1; }
    printf("pryon_qat: exp/log/log1p equal the Echo's libm\n");
    return 0;
#endif
}
