/*
 * Device attestation (DHA): the token an Echo signs with its own key so that Amazon's /auth/register takes the Echo's
 * device type.  Without it the registration answers 400 InvalidDevice even once the code was entered (Echo Dot 2,
 * 2026-10-06; docs/re-davs-login.md).  Two shapes, and the HAL's key says which one this Echo's MAP builds: an RSA
 * keymaster key means drvV1 (biscuit, radar), the EC dhav2 key means drvV3 around a certificate (donut).
 *
 * What MAP builds (libace_map.so map_registration_create_dha_jwt, biscuit and radar NS65741), byte for byte:
 *   header   {"typ":"drvV1","alg":"PS256","jwk":{"kty":"RSA","e":"65537","n":"<modulus>","mac":"<field 0x204>"}}
 *            (e as written, in decimal; n the 256-byte modulus; both base64url without padding, as all parts)
 *   payload  {"dev":{"dt":"<device type>","cpuid":"<field 1>","dsn":"<field 0x101>","typ":"v1"},
 *            "dat":"%Y-%m-%dT%H:%MZ","cust":{"typ":"v1"}}
 *   token    b64url(header) "." b64url(payload) "." b64url(RSASSA-PSS over SHA-256 of the first two, salt 32)
 *
 * donut's MAP (donut_puffin NS65741, read the same way) builds a drvV3 token instead, around the attestation
 * certificate the Echo fetched from Amazon when it was provisioned (/persist/dha_certificate.pem, HAL field 0x203):
 *   header   {"typ":"drvV3","alg":"ES256","x5c":["<the PEM's body, CR and LF dropped>"]}
 *   payload  {"dev":{"dt":"<device type>","cpuid":"dfae219fe47947c7","dsn":"<field 0x101>","typ":"v1"},
 *            "dat":"%Y-%m-%dT%H:%MZ"}   (no "cust"; the cpuid a literal of donut's MAP, every Echo Dot 3 the same)
 *   token    b64url(header) "." b64url(payload) "." b64url(R || S, each left-padded to 32 bytes big-endian)
 *            from the DER ECDSA signature the HAL returns over SHA-256 of the first two parts (MAP splits it with
 *            mbedtls_asn1_get_mpi; the JWS raw form).  The certificate rides in the token, so it runs ~1.9 kB.
 *
 * The key is Amazon's keymaster in the TEE (libacehal_dha.so -> /system/lib/hw/amzn_dha.<soc>.so -> /dev/trustzone);
 * the private half never leaves it, the HAL signs a hash we give it.  Its session needs group drmrpc: as puffin
 * without it, "Failed to open DHA session: Non-specific cause" (root, system, keystore, puffin+drmrpc all work).
 * donut's TEE and certificate sit behind two more groups its DAEMON_GROUPS carry (as puffinmrmd's do):
 * drmrpc (1026) for /dev/trustzone, keystore (1017) for /persist/dha_certificate.pem (0660 keystore:keystore;
 * without it the HAL answers field 0x203 with an error, measured on a Dot 3).
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
    F_CERT = 0x203,
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
 * comes out as one unpadded encode of the whole string (map_base64_url_encode: A-Za-z0-9-_, never '='; donut's is the
 * same).  What it signs is SHA-256 over b64url(header) "." b64url(payload) (_registration_sign_token), the HAL does
 * the PSS (or, on donut, the ECDSA) itself.
 */

/* the needle MAP's strstr uses, and how far it skips: the body is what stands between the markers */
#define PEM_BEGIN "-----BEGIN CERTIFICATE-----"        /* 27 characters */
#define PEM_BEGIN_LEN 27
#define PEM_END "-----END CERTIFICATE-----"

static const uint8_t *findb(const uint8_t *hay, size_t n, const char *needle)
{
    size_t l = strlen(needle);
    if (n < l)
        return NULL;
    for (size_t i = 0; i <= n - l; i++)
        if (!memcmp(hay + i, needle, l))
            return hay + i;
    return NULL;
}

/* MAP splits the DER signature the HAL returns into its two INTEGERs (mbedtls_asn1_get_mpi) and writes each
 * left-padded big-endian into 32 bytes: the JWS raw form R || S. */
static int der_rs(const uint8_t *sig, size_t n, uint8_t rs[64])
{
    size_t len;
    const uint8_t *p = sig, *end = sig + n, *q;
    if (der(&p, end, 0x30, &len) || p + len != end)             /* one SEQUENCE over the whole thing */
        return -1;
    end = p + len;
    for (int i = 0; i < 2; i++)
    {
        if (der(&p, end, 0x02, &len) || !len || len > 33)
            return -1;
        q = p;
        p += len;
        if (len == 33)                                          /* DER's sign byte: R and S stay below 2^256 */
        {
            if (*q)
                return -1;
            q++;
            len--;
        }
        while (len > 1 && !*q)                                  /* what mbedtls_mpi_size would not count */
        {
            q++;
            len--;
        }
        if (len > 32)
            return -1;
        memset(rs + 32 * i, 0, 32 - len);
        memcpy(rs + 32 * i + (32 - len), q, len);
    }
    return p == end ? 0 : -1;
}

static int jwt_v1(const char *dt, time_t when, char *out, size_t cap, char *err, size_t errsz)
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

static int jwt_v3(const char *dt, time_t when, char *out, size_t cap, char *err, size_t errsz)
{
    static const char head[] = "{\"typ\":\"drvV3\",\"alg\":\"ES256\",\"x5c\":[\"";
    static const char tail[] = "\"]}";
    static const char pfmt[] = "{\"dev\":{\"dt\":\"%s\",\"cpuid\":\"dfae219fe47947c7\",\"dsn\":\"%s\",\"typ\":\"v1\"},"
                               "\"dat\":\"%s\"}";
    char dsn[96] = "", date[32], p[300];
    uint8_t *cert = NULL, *sig = NULL, rs[64], hash[32];
    char *h = NULL, *hb = NULL, *pb = NULL;
    uint32_t certlen = 0, siglen = 0;
    const uint8_t *b, *e;
    struct tm tm;
    size_t hl, bl = 0, pl, n;

    if (hal.field(F_CERT, &cert, &certlen) || !cert || certlen > 8192
        || !(b = findb(cert, certlen, PEM_BEGIN)) || !(e = findb(cert, certlen, PEM_END)) || e < b + PEM_BEGIN_LEN
        || text_field(F_DSN, dsn, sizeof dsn)
        || !localtime_r(&when, &tm) || !strftime(date, sizeof date, "%Y-%m-%dT%H:%MZ", &tm))
        goto bad;
    b += PEM_BEGIN_LEN;
    if (!(h = malloc(strlen(head) + certlen + strlen(tail) + 1)))
        goto bad;
    hl = (size_t)snprintf(h, strlen(head) + certlen + strlen(tail) + 1, "%s", head);
    for (const uint8_t *q = b; q < e; q++)           /* the body between the markers, CR and LF dropped (MAP's loop) */
        if (*q != '\r' && *q != '\n')
            h[hl + bl++] = (char)*q;
    hl += (size_t)snprintf(h + hl + bl, strlen(tail) + 1, "%s", tail) + bl;
    pl = (size_t)snprintf(p, sizeof p, pfmt, dt, dsn, date);
    hb = malloc(4 * ((hl + 2) / 3) + 1);
    pb = malloc(4 * ((pl + 2) / 3) + 1);
    if (!hb || !pb)
        goto bad;
    b64_encode(h, hl, hb, 1, 0);
    b64_encode(p, pl, pb, 1, 0);

    n = strlen(hb) + 1 + strlen(pb);                 /* the signing input, as in drvV1 */
    if (n + 1 + (64 + 2) / 3 * 4 + 1 > cap)          /* the raw R || S, 64 bytes, plus its dot */
    {
        free(cert); free(h); free(hb); free(pb);
        return say(err, errsz, "token longer than the caller's buffer");
    }
    snprintf(out, cap, "%s.%s", hb, pb);
    sha256(out, n, hash);
    if (hal.sign(hash, sizeof hash, &sig, &siglen) || !sig || siglen > 72 || der_rs(sig, siglen, rs))
        goto bad;
    out[n] = '.';
    b64_encode(rs, sizeof rs, out + n + 1, 1, 0);
    free(cert); free(sig); free(h); free(hb); free(pb);
    return 0;
bad:
    free(cert); free(sig); free(h); free(hb); free(pb);
    return say(err, errsz, "the attestation key did not answer as MAP expects it");
}

int dha_jwt(const char *dt, time_t when, char *out, size_t cap, char *err, size_t errsz)
{
    static int opened;
    uint8_t *pub = NULL;
    uint32_t publen = 0;
    const uint8_t *mod;
    size_t modlen;
    int v1;
    if (load(err, errsz))
        return -1;
    if (!opened && hal.open())
        return say(err, errsz, "the attestation key would not open (hassmic needs the drmrpc group)");
    opened = 1;                     /* one session for the process; the PC build has none and fails at load() */

    /* which token this Echo's MAP builds, asked of the key: the RSA keymaster of biscuit and radar (a 256-byte
     * modulus to parse) makes drvV1; the EC dhav2 key of donut makes drvV3 (biscuit's and radar's HALs have the
     * certificate too, so its presence would not tell them apart -- their MAPs do not use it) */
    v1 = !hal.pubkey(&pub, &publen) && pub && !modulus(pub, publen, &mod, &modlen) && modlen == 256;
    free(pub);
    return v1 ? jwt_v1(dt, when, out, cap, err, errsz)
              : jwt_v3(dt, when, out, cap, err, errsz);
}

int dha_serial(char *out, size_t cap)
{
    char err[80];
    if (load(err, sizeof err) || text_field(F_DSN, out, cap))
        return -1;
    return 0;
}
