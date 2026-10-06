/* A stand-in for the Echo's device attestation HAL (libacehal_dha.so), built as a .so and loaded through
 * HASSMIC_DHA_HAL: the fixed RSA key of dha_hal_key.h, its fields, and a signature that is just the input
 * repeated and flipped, so tests know the token byte for byte.  Not the real crypto — the point is the shape. */
#include <stdlib.h>
#include <string.h>
#include "dha_hal_key.h"

int aceDhaHal_open(void) { return 0; }
int aceDhaHal_close(void) { return 0; }

int aceDhaHal_getPublicKey(unsigned char **der, unsigned int *len)
{
    *der = malloc(sizeof dha_der);
    if (!*der) return -1;
    memcpy(*der, dha_der, sizeof dha_der);
    *len = (unsigned int)sizeof dha_der;
    return 0;
}

int aceDhaHal_getField(int id, unsigned char **val, unsigned int *len)
{
    const void *p; unsigned int n;
    if (id == 1) { p = dha_cpuid; n = (unsigned int)sizeof dha_cpuid; }
    else if (id == 0x101) { p = dha_dsn; n = (unsigned int)sizeof dha_dsn; }
    else if (id == 0x204) { p = dha_mac; n = (unsigned int)sizeof dha_mac; }
    else return -1;
    *val = malloc(n);
    if (!*val) return -1;
    memcpy(*val, p, n);
    *len = n;
    return 0;
}

int aceDhaHal_signData(const unsigned char *data, unsigned int len, unsigned char **sig, unsigned int *siglen)
{
    *sig = malloc(256);
    if (!*sig) return -1;
    for (int i = 0; i < 256; i++) (*sig)[i] = data[i % (len < 1 ? 1 : len)] ^ 0xA5;
    *siglen = 256;
    return 0;
}
