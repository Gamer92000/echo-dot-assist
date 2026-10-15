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
 *
 * checkers (Echo Show 5, Fire OS on Android) has no libacehal_dha.so: its key sits behind fireos-dha, the binder service
 * "amazon.security.dha" (fireos.dha.IFireOsDha, libdha-aidl.so; init runs it as keystore with drmrpc), which Amazon's
 * Java MAP (com.amazon.imp, kcpsdk.auth.r "FireOsDhaSigner") reaches through amazon.security.FireOsDha.  Transactions
 * (BpFireOsDha): 1 sign(byte[] digest), 3 getDhaPublicKey, 4 getDhaHmac, 5 getDhaCertificateChain, 6 getDhaVersion;
 * each reply is the exception, then the value.  Asked through the firmware's /system/bin/service as hassmic's user
 * system on the tester's Echo (2026-10-13): version 1, an RSA-2048 SubjectPublicKeyInfo, a 32-byte HMAC, no chain (-7),
 * and sign answers RSASSA-PSS with SHA-256 and a 32-byte salt over the digest it is given, as the Dots' HAL does.  For
 * version 1 Java MAP builds drvV1 too, from the same values, but its JSON is its own (org.json keeps the order of the
 * puts; kcpsdk.auth.w/r/l0/h0, read out of com.amazon.imp.apk):
 *   header   {"typ":"drvV1","alg":"PS256","jwk":{"kty":"RSA","n":"<modulus>","e":"65537","mac":"<getDhaHmac>"}}
 *            (n and mac in Base64 flags 10: URL-safe, *padded*; e the key's exponent in decimal)
 *   payload  {"dat":"<UTC, yyyy-MM-dd'T'HH:mm'Z'>","dev":{"typ":"v1","dt":"<device type>","dsn":"<the registration's
 *            serial>","cpuid":"<the Serial line of /proc/cpuinfo>"},"cust":{"typ":"v1"}}
 *            ("ds", the device secret, only in a registration by device secret: not the code pair's)
 *   token    as above, header and payload in flags 11 (unpadded), the signature over SHA-256 of the first two parts.
 */
#include "dha.h"
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "afe_parcel.h"
#include "core.h"
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

/* ---------------------------------------------------------------- checkers: the binder service */

#define DHA_SVC "amazon.security.dha"
static int svc;                     /* the key is the service's, not a HAL's: its version, 1 (checkers) or 2 (crown) */

/* one transaction through the service tool; the reply's byte[] into *val (malloc'd) and its length; 0, or -1 */
static int svc_get(const char *code, const void *in, size_t inlen, uint8_t **val, uint32_t *len)
{
    struct afe_call c;
    enum { TEXT = 48 * 1024, WORDS = 2048 };
    char *text = malloc(TEXT);      /* a 294-byte key prints as ~1.5 kB of hex dump, crown's 5.4 kB chain as ~24 kB */
    uint32_t *w = malloc(WORDS * sizeof *w);
    int nw;
    *val = NULL;
    if (!text || !w)
    {
        free(text);
        free(w);
        return -1;
    }
    svc_call_start(&c, DHA_SVC, code);
    if (in && svc_blob(&c, in, inlen))
    {
        free(text);
        free(w);
        return -1;
    }
    core_run(c.argv, text, TEXT);
    nw = svc_reply_words(text, w, WORDS);
    free(text);
    if (nw < 2 || w[0] != 0 || w[1] == 0xffffffffu || w[1] > (uint32_t)(nw - 2) * 4 || !(*val = malloc(w[1] + 1)))
    {
        free(w);
        return -1;                  /* an exception (no chain: -7), a null array, a reply cut short */
    }
    for (uint32_t i = 0; i < w[1]; i++)
        (*val)[i] = (uint8_t)(w[2 + i / 4] >> (8 * (i % 4)));
    (*val)[w[1]] = 0;               /* the chain is text: callers may strstr it */
    *len = w[1];
    free(w);
    return 0;
}

/* getDhaVersion: Java MAP's switch between drvV1 (1) and drvV3 (2); 0 when the service does not answer (no Android) */
static int svc_version(void)
{
    struct afe_call c;
    char text[256];
    uint32_t w[4];
    svc_call_start(&c, DHA_SVC, "6");
    core_run(c.argv, text, sizeof text);
    return svc_reply_words(text, w, 4) == 2 && w[0] == 0 ? (int)w[1] : 0;
}

static int load(char *err, size_t errsz)
{
    static int done;
    const char *so;
    if (done)
        return hal.sign || svc ? 0 : say(err, errsz, "no attestation HAL");
    done = 1;
    /* HASSMIC_DHA_HAL points tests at a stand-in; on an Echo nobody sets it */
    so = getenv("HASSMIC_DHA_HAL");
    void *h = dlopen(so ? so : "libacehal_dha.so", RTLD_NOW);
    if (!h)
    {
        int v = svc_version();
        if (v == 1 || v == 2)
            return svc = v, 0;
        if (v)
            return say(err, errsz, "the attestation service is DHA version %d, which hassmic does not build", v);
        return say(err, errsz, "no attestation HAL (%s)", dlerror());
    }
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

/* what kind of key a SubjectPublicKeyInfo holds, by its AlgorithmIdentifier: rsaEncryption (biscuit and radar, read
 * from biscuit's HAL 2026-10-09) or id-ecPublicKey on P-256 (donut; ES256 is P-256 by definition, and MAP's 32-byte
 * R and S assume it) */
enum { KEY_OTHER, KEY_RSA, KEY_P256 };
static int key_kind(const uint8_t *k, size_t klen)
{
    static const uint8_t rsa[] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01};        /* 1.2.840.113549.1.1.1 */
    static const uint8_t ec[] = {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01};                    /* 1.2.840.10045.2.1 */
    static const uint8_t p256[] = {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07};            /* 1.2.840.10045.3.1.7 */
    const uint8_t *p = k, *end = k + klen, *oid;
    size_t len;
    if (der(&p, end, 0x30, &len))
        return KEY_OTHER; /* SubjectPublicKeyInfo */
    end = p + len;
    if (der(&p, end, 0x30, &len))
        return KEY_OTHER; /* AlgorithmIdentifier */
    end = p + len;
    if (der(&p, end, 0x06, &len))
        return KEY_OTHER; /* algorithm */
    oid = p;
    p += len;
    if (len == sizeof rsa && !memcmp(oid, rsa, len))
        return KEY_RSA;
    if (len != sizeof ec || memcmp(oid, ec, len) || der(&p, end, 0x06, &len))
        return KEY_OTHER; /* parameters: the named curve */
    return len == sizeof p256 && !memcmp(p, p256, len) ? KEY_P256 : KEY_OTHER;
}

/* the RSA modulus of a SubjectPublicKeyInfo, without the sign byte; with f4, whether the exponent is 65537 */
static int modulus(const uint8_t *k, size_t klen, const uint8_t **n, size_t *nlen, int *f4)
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
    *n = p;
    *nlen = len;
    while (*nlen > 1 && !**n)
        (*n)++, (*nlen)--;
    if (f4)
    {
        p += len;
        *f4 = !der(&p, end, 0x02, &len) && len == 3 && p[0] == 1 && p[1] == 0 && p[2] == 1;
    }
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

    if (hal.pubkey(&pub, &publen) || !pub || modulus(pub, publen, &mod, &modlen, NULL) || modlen != 256
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
    size_t hl, pl, n;

    if (hal.field(F_CERT, &cert, &certlen) || !cert)          /* the file is keystore's: the group it needs */
        return say(err, errsz, "no attestation certificate (hassmic needs the keystore group)");
    if (certlen > 8192 || !(b = findb(cert, certlen, PEM_BEGIN)) || !(e = findb(cert, certlen, PEM_END))
        || e < b + PEM_BEGIN_LEN)
    {
        free(cert);
        return say(err, errsz, "the attestation certificate is no PEM certificate");
    }
    if (text_field(F_DSN, dsn, sizeof dsn)
        || !localtime_r(&when, &tm) || !strftime(date, sizeof date, "%Y-%m-%dT%H:%MZ", &tm))
        goto bad;
    b += PEM_BEGIN_LEN;
    if (!(h = malloc(strlen(head) + certlen + strlen(tail) + 1)))
        goto bad;
    memcpy(h, head, strlen(head));
    hl = strlen(head);
    for (const uint8_t *q = b; q < e; q++)           /* the body between the markers, CR and LF dropped (MAP's loop) */
        if (*q != '\r' && *q != '\n')
            h[hl++] = (char)*q;
    memcpy(h + hl, tail, sizeof tail);               /* the NUL too */
    hl += strlen(tail);
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

/* the Serial line of /proc/cpuinfo, as Java MAP's AmazonJWTHelper reads it (l0.c: the text after the colon, trimmed);
 * HASSMIC_CPUINFO points tests at a stand-in.  0, or -1: then MAP leaves cpuid out (JSONObject.put of null) */
static int cpu_serial(char *out, size_t cap)
{
    char line[256];
    FILE *f = fopen(getenv("HASSMIC_CPUINFO") ? getenv("HASSMIC_CPUINFO") : "/proc/cpuinfo", "r");
    int ok = -1;
    while (f && ok && fgets(line, sizeof line, f))
    {
        char *v = strchr(line, ':'), *e;
        if (strncmp(line, "Serial", 6) || !v)
            continue;
        for (v++; *v == ' ' || *v == '\t'; v++)
            ;
        for (e = v + strlen(v); e > v && (e[-1] == '\n' || e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'); e--)
            ;
        *e = 0;
        ok = (size_t)(e - v) < cap && !strpbrk(v, "\"\\") ? (memcpy(out, v, (size_t)(e - v) + 1), 0) : -1;
        break;                      /* MAP takes the first Serial line */
    }
    if (f)
        fclose(f);
    return ok;
}

/* Java MAP's drvV3 (crown, service version 2; kcpsdk.auth.q a/m/j/h and v.i): the header is typ, alg ES256, x5c with the
 * chain's first certificate as the DER re-encoded in Base64 flags 2 (standard, padded, one line), written by org.json,
 * which escapes every "/" as "\/"; the payload is drvV1's (dat, dev, cust: l0.d/c); the signature is the service's DER
 * ECDSA over SHA-256 of the first two parts, turned into R || S (l0.o).  Neither the key nor the HMAC is asked for. */
static int jwt_java_v3(const char *dt, const char *dsn, time_t when, char *out, size_t cap, char *err, size_t errsz)
{
    static const char head[] = "{\"typ\":\"drvV3\",\"alg\":\"ES256\",\"x5c\":[\"";
    char cpuid[64], cpu_part[96] = "", date[32], p[400], *h = NULL, *hb = NULL, pb[600];
    uint8_t *chain = NULL, *sig = NULL, rs[64], hash[32];
    uint32_t chainlen = 0, siglen = 0;
    const uint8_t *b, *e;
    size_t hl, n;
    struct tm tm;

    if (!dsn || !*dsn || strpbrk(dsn, "\"\\") || strlen(dsn) > 64)
        return say(err, errsz, "no device serial for the attestation token");
    if (svc_get("5", NULL, 0, &chain, &chainlen) || !(b = findb(chain, chainlen, PEM_BEGIN)))
        goto bad;
    b += PEM_BEGIN_LEN;
    if (!(e = findb(b, chainlen - (size_t)(b - chain), PEM_END)) || e - b > 4096)
        goto bad;
    if (!gmtime_r(&when, &tm) || !strftime(date, sizeof date, "%Y-%m-%dT%H:%MZ", &tm))
        goto bad;
    if (!cpu_serial(cpuid, sizeof cpuid))
        snprintf(cpu_part, sizeof cpu_part, ",\"cpuid\":\"%s\"", cpuid);

    if (!(h = malloc(sizeof head + 2 * (size_t)(e - b) + 8)))
        goto bad;
    hl = (size_t)snprintf(h, sizeof head + 2 * (size_t)(e - b) + 8, "%s", head);
    for (const uint8_t *q = b; q < e; q++)           /* the body, one line; '/' as org.json writes it */
        if (*q != '\r' && *q != '\n')
        {
            if (*q == '/')
                h[hl++] = '\\';
            h[hl++] = (char)*q;
        }
    hl += (size_t)snprintf(h + hl, 8, "\"]}");
    snprintf(p, sizeof p, "{\"dat\":\"%s\",\"dev\":{\"typ\":\"v1\",\"dt\":\"%s\",\"dsn\":\"%s\"%s},\"cust\":{\"typ\":\"v1\"}}",
             date, dt, dsn, cpu_part);
    if (!(hb = malloc(4 * ((hl + 2) / 3) + 1)))
        goto bad;
    b64_encode(h, hl, hb, 1, 0);
    b64_encode(p, strlen(p), pb, 1, 0);

    n = strlen(hb) + 1 + strlen(pb);
    if (n + 1 + (64 + 2) / 3 * 4 + 1 > cap)
    {
        free(chain); free(h); free(hb);
        return say(err, errsz, "token longer than the caller's buffer");
    }
    snprintf(out, cap, "%s.%s", hb, pb);
    sha256(out, n, hash);
    if (svc_get("1", hash, sizeof hash, &sig, &siglen) || siglen > 72 || der_rs(sig, siglen, rs))
        goto bad;
    out[n] = '.';
    b64_encode(rs, sizeof rs, out + n + 1, 1, 0);
    free(chain); free(sig); free(h); free(hb);
    return 0;
bad:
    free(chain); free(sig); free(h); free(hb);
    return say(err, errsz, "the attestation service did not answer as MAP expects it");
}

/* Java MAP's drvV1 (checkers): the values from the service, the JSON as kcpsdk.auth puts it together */
static int jwt_java(const char *dt, const char *dsn, time_t when, char *out, size_t cap, char *err, size_t errsz)
{
    char cpuid[64], cpu_part[96] = "", date[32], h[700], p[400], hb[1000], pb[600];
    uint8_t *pub = NULL, *mac = NULL, *sig = NULL, hash[32];
    uint32_t publen = 0, maclen = 0, siglen = 0;
    const uint8_t *mod;
    size_t modlen, hl, n;
    struct tm tm;
    int f4 = 0;

    if (!dsn || !*dsn || strpbrk(dsn, "\"\\") || strlen(dsn) > 64)
        return say(err, errsz, "no device serial for the attestation token");
    if (svc_get("3", NULL, 0, &pub, &publen) || modulus(pub, publen, &mod, &modlen, &f4) || modlen != 256 || !f4
        || svc_get("4", NULL, 0, &mac, &maclen) || !maclen || maclen > 74
        || !gmtime_r(&when, &tm) || !strftime(date, sizeof date, "%Y-%m-%dT%H:%MZ", &tm))
        goto bad;
    if (!cpu_serial(cpuid, sizeof cpuid))
        snprintf(cpu_part, sizeof cpu_part, ",\"cpuid\":\"%s\"", cpuid);

    hl = (size_t)snprintf(h, sizeof h, "%s", "{\"typ\":\"drvV1\",\"alg\":\"PS256\",\"jwk\":{\"kty\":\"RSA\",\"n\":\"");
    hl += b64_encode(mod, modlen, h + hl, 1, 1);
    hl += (size_t)snprintf(h + hl, sizeof h - hl, "%s", "\",\"e\":\"65537\",\"mac\":\"");
    hl += b64_encode(mac, maclen, h + hl, 1, 1);
    hl += (size_t)snprintf(h + hl, sizeof h - hl, "%s", "\"}}");
    snprintf(p, sizeof p, "{\"dat\":\"%s\",\"dev\":{\"typ\":\"v1\",\"dt\":\"%s\",\"dsn\":\"%s\"%s},\"cust\":{\"typ\":\"v1\"}}",
             date, dt, dsn, cpu_part);
    b64_encode(h, hl, hb, 1, 0);
    b64_encode(p, strlen(p), pb, 1, 0);

    n = strlen(hb) + 1 + strlen(pb);
    if (n + 1 + (256 + 2) / 3 * 4 + 1 > cap)         /* a 2048-bit key's signature, plus its dot */
    {
        free(pub); free(mac);
        return say(err, errsz, "token longer than the caller's buffer");
    }
    snprintf(out, cap, "%s.%s", hb, pb);
    sha256(out, n, hash);                            /* FireOsDhaSigner.h: the service signs the digest */
    if (svc_get("1", hash, sizeof hash, &sig, &siglen) || siglen != 256)
        goto bad;
    out[n] = '.';
    b64_encode(sig, siglen, out + n + 1, 1, 0);
    free(pub); free(mac); free(sig);
    return 0;
bad:
    free(pub); free(mac); free(sig);
    return say(err, errsz, "the attestation service did not answer as MAP expects it");
}

int dha_jwt(const char *dt, const char *dsn, time_t when, char *out, size_t cap, char *err, size_t errsz)
{
    static int opened;
    uint8_t *pub = NULL;
    uint32_t publen = 0;
    int kind;
    if (load(err, errsz))
        return -1;
    if (svc == 2)
        return jwt_java_v3(dt, dsn, when, out, cap, err, errsz);
    if (svc)
        return jwt_java(dt, dsn, when, out, cap, err, errsz);
    if (!opened && hal.open())
        return say(err, errsz, "the attestation key would not open (hassmic needs the drmrpc group)");
    opened = 1;                     /* one session for the process; the PC build has none and fails at load() */

    /* which token this Echo's MAP builds, asked of the key: the RSA keymaster of biscuit and radar makes drvV1, the
     * EC dhav2 key of donut drvV3 (biscuit's and radar's HALs have the certificate too, so its presence would not tell
     * them apart -- their MAPs do not use it).  Anything else is a model nobody has read the MAP of: refused by name,
     * not tried as one of the two */
    if (hal.pubkey(&pub, &publen) || !pub)
        return say(err, errsz, "the attestation key gave no public key");
    kind = key_kind(pub, publen);
    free(pub);
    if (kind == KEY_RSA)
        return jwt_v1(dt, when, out, cap, err, errsz);
    if (kind == KEY_P256)
        return jwt_v3(dt, when, out, cap, err, errsz);
    return say(err, errsz, "the attestation key is neither MAP's RSA key (drvV1) nor its P-256 key (drvV3)");
}

int dha_serial(char *out, size_t cap)
{
    char err[80];
    if (load(err, sizeof err) || svc || text_field(F_DSN, out, cap))      /* the service has no field call */
        return -1;
    return 0;
}
