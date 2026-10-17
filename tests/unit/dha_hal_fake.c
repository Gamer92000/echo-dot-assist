/* A stand-in for the Echo's device attestation HAL (libacehal_dha.so), built as a .so and loaded through
 * HASSMIC_DHA_HAL: the fixed keys of dha_hal_key.h, their fields, and signatures tests know byte for byte.
 * Not the real crypto -- the point is the shape.  DHA_FAKE_V3 selects what donut's HAL answers (an EC key, the
 * certificate as field 0x203, DER ECDSA signatures); without it the RSA keymaster of biscuit and radar.  Read
 * per call, so one test can exercise both.  The refusals: DHA_FAKE_CURVE puts the EC key on a curve other than
 * P-256 (a model whose MAP nobody read), DHA_FAKE_NOCERT answers field 0x203 with an error (donut without the
 * keystore group). */
#include <stdlib.h>
#include <string.h>
#include "dha_hal_key.h"

static int v3(void) { return getenv("DHA_FAKE_V3") != NULL; }

int aceDhaHal_open(void) { return 0; }
int aceDhaHal_close(void) { return 0; }

int aceDhaHal_getPublicKey(unsigned char **der, unsigned int *len)
{
    const void *p;
    unsigned int n;
    if (v3()) { p = dha_ec_spki; n = (unsigned int)sizeof dha_ec_spki; }
    else { p = dha_der; n = (unsigned int)sizeof dha_der; }
    *der = malloc(n);
    if (!*der) return -1;
    memcpy(*der, p, n);
    if (v3() && getenv("DHA_FAKE_CURVE")) (*der)[22] = 0x22;     /* the curve OID no longer P-256 (1.2.840.10045.3.1.7) */
    *len = n;
    return 0;
}

int aceDhaHal_getField(int id, unsigned char **val, unsigned int *len)
{
    const void *p; unsigned int n; char *env;
    if (id == 1) { p = dha_cpuid; n = (unsigned int)sizeof dha_cpuid; }
    else if (id == 0x101) {
        if ((env = getenv("DHA_FAKE_DSN")) && *env) { p = env; n = (unsigned int)strlen(env) + 1; }  /* the serial of this Echo, NUL counted */
        else { p = dha_dsn; n = (unsigned int)sizeof dha_dsn; }
    }
    else if (id == 0x204) { p = dha_mac; n = (unsigned int)sizeof dha_mac; }
    else if (id == 0x203 && v3() && !getenv("DHA_FAKE_NOCERT")) { p = dha_cert_pem; n = (unsigned int)sizeof dha_cert_pem - 1; }
    else return -1;
    *val = malloc(n);
    if (!*val) return -1;
    memcpy(*val, p, n);
    *len = n;
    return 0;
}

/* the DER of R and S: each INTEGER gets DER's 0x00 when its top bit is set (mbedtls writes the same) */
static int integer(unsigned char **o, const unsigned char *v)
{
    unsigned char *w = *o;
    int n = 32;
    while (n > 1 && !*v) { v++; n--; }
    *w++ = 2;
    *w++ = (unsigned char)(n + (*v & 0x80 ? 1 : 0));
    if (*v & 0x80) *w++ = 0;
    memcpy(w, v, n);
    *o = w + n;
    return 0;
}

int aceDhaHal_signData(const unsigned char *data, unsigned int len, unsigned char **sig, unsigned int *siglen)
{
    if (v3())
    {
        unsigned char s[80], *o = s;                  /* 30 .. 02 <33> R 02 <33> S at most */
        unsigned char r2[32];
        for (int i = 0; i < 32; i++) r2[i] = data[len - 1 - i];     /* S: the input reversed */
        s[0] = 0x30;
        o = s + 2;                                    /* the length filled in below */
        integer(&o, data);
        integer(&o, r2);
        s[1] = (unsigned char)(o - s - 2);
        *sig = malloc(o - s);
        if (!*sig) return -1;
        memcpy(*sig, s, o - s);
        *siglen = (unsigned int)(o - s);
        return 0;
    }
    *sig = malloc(256);
    if (!*sig) return -1;
    for (int i = 0; i < 256; i++) (*sig)[i] = data[i % (len < 1 ? 1 : len)] ^ 0xA5;
    *siglen = 256;
    return 0;
}
