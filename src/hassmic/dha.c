/*
 * Device attestation (DHA): the token an Echo signs with its own key so that Amazon's /auth/register takes the Echo's
 * device type.  Without it the registration answers 400 InvalidDevice even once the code was entered (Echo Dot 2,
 * 2026-10-06; docs/re-davs-login.md).  What MAP builds (libace_map.so map_registration_create_dha_jwt, biscuit and
 * radar NS65741), byte for byte:
 *   header   {"typ":"drvV1","alg":"PS256","jwk":{"kty":"RSA","e":"65537","n":"<modulus>","mac":"<field 0x204>"}}
 *            (e as written, in decimal; n the 256-byte modulus; both base64url without padding, as all parts)
 *   payload  {"dev":{"dt":"<device type>","cpuid":"<field 1>","dsn":"<field 0x101>","typ":"v1"},
 *            "dat":"%Y-%m-%dT%H:%MZ","cust":{"typ":"v1"}}
 *   token    b64url(header) "." b64url(payload) "." b64url(RSASSA-PSS over SHA-256 of the first two, salt 32)
 * The key is Amazon's keymaster in the TEE (libacehal_dha.so -> /system/lib/hw/amzn_dha.<soc>.so -> /dev/trustzone);
 * the private half never leaves it, the HAL signs a hash we give it.  Its session needs group drmrpc: as puffin
 * without it, "Failed to open DHA session: Non-specific cause" (root, system, keystore, puffin+drmrpc all work).
 * MAP dates the token with local time and an appended Z; the Echos run on GMT.  The date is the caller's, not the
 * clock's: an Echo whose clock never synced (biscuit on 2026-10-06 said a day earlier) would sign a stale one.
 * Loaded with dlopen: donut, biscuit and radar each have their own HAL build, and the PC build has none.
 */
#include "dha.h"
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hash.h"

enum
{
    F_CPUID = 1,
    F_DSN = 0x101,
    F_MAC = 0x204
};

static struct
{
    int (*open)(void);
    int (*close)(void);
    int (*pubkey)(uint8_t **der, uint32_t *len);
    int (*field)(int id, uint8_t **val, uint32_t *len);
    int (*sign)(const uint8_t *data, uint32_t len, uint8_t **sig, uint32_t *siglen);
} hal;

static int say(char *err, size_t errsz, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (err && errsz)
        vsnprintf(err, errsz, fmt, ap);
    va_end(ap);
    return -1;
}

static int load(char *err, size_t errsz)
{
    static int done;
    const char *so;
    if (done)
        return hal.sign ? 0 : say(err, errsz, "no attestation HAL");
    done = 1;
    /* HASSMIC_DHA_HAL points tests at a stand-in; on an Echo nobody sets it */
    so = getenv("HASSMIC_DHA_HAL");
    void *h = dlopen(so ? so : "libacehal_dha.so", RTLD_NOW);
    if (!h)
        return say(err, errsz, "no attestation HAL (%s)", dlerror());
    hal.open = (int (*)(void))dlsym(h, "aceDhaHal_open");
    hal.close = (int (*)(void))dlsym(h, "aceDhaHal_close");
    hal.pubkey = (int (*)(uint8_t **, uint32_t *))dlsym(h, "aceDhaHal_getPublicKey");
    hal.field = (int (*)(int, uint8_t **, uint32_t *))dlsym(h, "aceDhaHal_getField");
    hal.sign = (int (*)(const uint8_t *, uint32_t, uint8_t **, uint32_t *))dlsym(h, "aceDhaHal_signData");
    if (!hal.open || !hal.close || !hal.pubkey || !hal.field || !hal.sign)
    {
        hal.sign = NULL;
        return say(err, errsz, "attestation HAL incomplete");
    }
    return 0;
}

/* a text field without the NUL the HAL counts in its length (field 1: 17 bytes for 16 digits) */
static int text_field(int id, char *out, size_t cap)
{
    uint8_t *p = NULL;
    uint32_t n = 0;
    if (hal.field(id, &p, &n) || !p)
        return -1;
    while (n && !p[n - 1])
        n--;
    int ok = n < cap;
    if (ok)
    {
        memcpy(out, p, n);
        out[n] = 0;
    }
    for (uint32_t i = 0; ok && i < n; i++)
        if (p[i] < 0x21 || p[i] > 0x7e || p[i] == '"' || p[i] == '\\')
            ok = 0;
    free(p);
    return ok ? 0 : -1;
}

/* DER: the tag's contents at *p (advanced past them), length in *len; 0, or -1 */
static int der(const uint8_t **p, const uint8_t *end, int tag, size_t *len)
{
    const uint8_t *q = *p;
    if (end - q < 2 || *q++ != tag)
        return -1;
    size_t n = *q++;
    if (n & 0x80)
    {
        int k = n & 0x7f;
        if (k < 1 || k > 2 || end - q < k)
            return -1;
        for (n = 0; k--;)
            n = n << 8 | *q++;
    }
    if ((size_t)(end - q) < n)
        return -1;
    *p = q;
    *len = n;
    return 0;
}

/* the RSA modulus of a SubjectPublicKeyInfo, without the sign byte */
static int modulus(const uint8_t *k, size_t klen, const uint8_t **n, size_t *nlen)
{
    const uint8_t *p = k, *end = k + klen;
    size_t len;
    if (der(&p, end, 0x30, &len))
        return -1; /* SubjectPublicKeyInfo */
    end = p + len;
    if (der(&p, end, 0x30, &len))
        return -1; /* AlgorithmIdentifier: skipped */
    p += len;
    if (der(&p, end, 0x03, &len) || !len || *p++)
        return -1; /* BIT STRING, no unused bits */
    if (der(&p, end, 0x30, &len))
        return -1; /* RSAPublicKey */
    if (der(&p, end, 0x02, &len))
        return -1; /* modulus */
    while (len > 1 && !*p)
        p++, len--;
    *n = p;
    *nlen = len;
    return 0;
}

/* MAP b64url-encodes the header piecewise, each piece cut to a multiple of 3 bytes with the rest carried over, which
 * comes out as one unpadded encode of the whole string (map_base64_url_encode: A-Za-z0-9-_, never '=').  What it signs
 * is SHA-256 over b64url(header) "." b64url(payload) (_registration_sign_token), the HAL does the PSS itself.
 * donut's MAP builds none of this: it makes a drvV3/ES256 token around a certificate from HAL field 0x203 (payload
 * without "cust"), not reversed.  There dha_jwt fails, and davs.c says this Echo cannot log in to Amazon.
 */

int dha_jwt(const char *dt, time_t when, char *out, size_t cap, char *err, size_t errsz)
{
    static const char head[] = "{\"typ\":\"drvV1\",\"alg\":\"PS256\",\"jwk\":{\"kty\":\"RSA\",\"e\":\"65537\",\"n\":\"";
    static const char mid[] = "\",\"mac\":\"";
    static const char tail[] = "\"}}";
    static const char pfmt[] = "{\"dev\":{\"dt\":\"%s\",\"cpuid\":\"%s\",\"dsn\":\"%s\",\"typ\":\"v1\"},"
                               "\"dat\":\"%s\",\"cust\":{\"typ\":\"v1\"}}";
    char cpuid[48] = "", dsn[96] = "", date[32], h[600], p[300], hb[900], pb[400];
    uint8_t *pub = NULL, *mac = NULL, *sig = NULL;
    uint32_t publen = 0, maclen = 0, siglen = 0;
    struct tm tm;
    size_t hl, pl, n;
    const uint8_t *mod;
    size_t modlen;
    uint8_t hash[32];
    static int opened;

    if (load(err, errsz))
        return -1;
    if (!opened && hal.open())
        return say(err, errsz, "the attestation key would not open (hassmic needs the drmrpc group)");
    opened = 1;                     /* one session for the process; the PC build has none and fails at load() */

    if (hal.pubkey(&pub, &publen) || !pub || modulus(pub, publen, &mod, &modlen) || modlen != 256
        || hal.field(F_MAC, &mac, &maclen) || !mac || !maclen || maclen > 74        /* MAP's b64 of it holds 100 bytes */
        || text_field(F_CPUID, cpuid, sizeof cpuid) || text_field(F_DSN, dsn, sizeof dsn)
        || !localtime_r(&when, &tm) || !strftime(date, sizeof date, "%Y-%m-%dT%H:%MZ", &tm))
        goto bad;

    hl = (size_t)snprintf(h, sizeof h, "%s", head);
    hl += b64_encode(mod, modlen, h + hl, 1, 0);
    hl += (size_t)snprintf(h + hl, sizeof h - hl, "%s", mid);
    hl += b64_encode(mac, maclen, h + hl, 1, 0);
    hl += (size_t)snprintf(h + hl, sizeof h - hl, "%s", tail);
    pl = (size_t)snprintf(p, sizeof p, pfmt, dt, cpuid, dsn, date);
    b64_encode(h, hl, hb, 1, 0);
    b64_encode(p, pl, pb, 1, 0);

    n = strlen(hb) + 1 + strlen(pb);                 /* the signing input, MAP's _registration_sign_token */
    if (n + 1 + (384 + 2) / 3 * 4 + 1 > cap)         /* a 384-byte signature at most, plus its dot */
    {
        free(pub); free(mac);
        return say(err, errsz, "token longer than the caller's buffer");
    }
    snprintf(out, cap, "%s.%s", hb, pb);
    sha256(out, n, hash);
    if (hal.sign(hash, sizeof hash, &sig, &siglen) || !sig || !siglen || siglen > 384)
        goto bad;
    if (n + 1 + (siglen + 2) / 3 * 4 + 1 > cap)
    {
        free(pub); free(mac); free(sig);
        return say(err, errsz, "token longer than the caller's buffer");
    }
    out[n] = '.';
    b64_encode(sig, siglen, out + n + 1, 1, 0);
    free(pub); free(mac); free(sig);
    return 0;
bad:
    free(pub); free(mac); free(sig);
    return say(err, errsz, "the attestation key did not answer as MAP expects it");
}

int dha_serial(char *out, size_t cap)
{
    char err[80];
    if (load(err, sizeof err) || text_field(F_DSN, out, cap))
        return -1;
    return 0;
}
