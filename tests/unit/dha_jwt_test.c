/* dha.c's attestation token against what MAP builds (libace_map.so of biscuit and radar, read out of the firmware):
 * the two format strings, the b64url of every part (URL-safe, unpadded), the fields from the HAL, the date as MAP
 * writes it, and the signature over SHA-256 of the first two parts joined.  The HAL is the fake of
 * dha_hal_fake.c through HASSMIC_DHA_HAL; the key and fields are dha_hal_key.h, shared with it, so this test pins
 * dha.c's format, not the fake's data.  Both shapes: drvV1 (the RSA keymaster) and donut's drvV3 (DHA_FAKE_V3: the
 * certificate of field 0x203, the fixed cpuid, the DER signature split into R and S).  With --service[2], checkers'
 * way: no HAL, the key behind the binder service amazon.security.dha, asked through a fake of the service tool (core_run
 * below, printing replies as Android 7's Parcel does), and the token Java MAP builds from it. */
#include "dha.h"
#include "hash.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "dha_hal_key.h"

static int fails;
static uint8_t sig_input[32];      /* SHA-256 of "hb.pb", set once the parts are known */

static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) fails++;
}

/* the expected token, built the way the disassembly says MAP builds it */
static void expected(char *out, size_t cap, time_t when)
{
    char h[600], p[300], hb[900], pb[400], date[32], mod_b64[400], mac_b64[64], sig[256], sig_b64[400];
    struct tm tm;
    size_t hl;

    for (int i = 0; i < 256; i++) sig[i] = (uint8_t)(sig_input[i % 32] ^ 0xA5);    /* the fake's signature */
    b64_encode(dha_mod, 256, mod_b64, 1, 0);
    b64_encode(dha_mac, sizeof dha_mac, mac_b64, 1, 0);
    b64_encode(sig, 256, sig_b64, 1, 0);
    localtime_r(&when, &tm);
    strftime(date, sizeof date, "%Y-%m-%dT%H:%MZ", &tm);
    hl = (size_t)snprintf(h, sizeof h, "{\"typ\":\"drvV1\",\"alg\":\"PS256\",\"jwk\":{\"kty\":\"RSA\",\"e\":\"65537\",\"n\":\"%s\","
                              "\"mac\":\"%s\"}}", mod_b64, mac_b64);
    (void)hl;
    snprintf(p, sizeof p, "{\"dev\":{\"dt\":\"%s\",\"cpuid\":\"%s\",\"dsn\":\"%s\",\"typ\":\"v1\"},\"dat\":\"%s\",\"cust\":{\"typ\":\"v1\"}}",
             dha_dt, dha_cpuid, dha_dsn, date);
    b64_encode(h, strlen(h), hb, 1, 0);
    b64_encode(p, strlen(p), pb, 1, 0);
    snprintf(out, cap, "%s.%s.%s", hb, pb, sig_b64);
}

/* donut's: the PEM body between the markers (CR and LF dropped) in x5c, the cpuid a MAP literal, R || S from the
 * fake's DER signature over the digest */
static void expected_v3(char *out, size_t cap, time_t when)
{
    static const char begin[] = "-----BEGIN CERTIFICATE-----", endm[] = "-----END CERTIFICATE-----";
    char h[600], p[300], hb[900], pb[400], date[32], rs[64], rs_b64[90];
    const char *b = strstr(dha_cert_pem, begin) + sizeof begin - 1, *e = strstr(dha_cert_pem, endm);
    struct tm tm;
    size_t hl = (size_t)snprintf(h, sizeof h, "%s", "{\"typ\":\"drvV3\",\"alg\":\"ES256\",\"x5c\":[\"");

    for (const char *c = b; c < e; c++)
        if (*c != '\r' && *c != '\n') h[hl++] = *c;
    hl += (size_t)snprintf(h + hl, sizeof h - hl, "%s", "\"]}");
    localtime_r(&when, &tm);
    strftime(date, sizeof date, "%Y-%m-%dT%H:%MZ", &tm);
    snprintf(p, sizeof p, "{\"dev\":{\"dt\":\"%s\",\"cpuid\":\"dfae219fe47947c7\",\"dsn\":\"%s\",\"typ\":\"v1\"},\"dat\":\"%s\"}",
             dha_dt, dha_dsn, date);
    b64_encode(h, hl, hb, 1, 0);
    b64_encode(p, strlen(p), pb, 1, 0);
    for (int i = 0; i < 2; i++)                      /* R: the digest, S: it reversed, each left-padded to 32 */
    {
        const uint8_t *v = sig_input;
        uint8_t rr[32];
        int n = 32;
        if (i) { for (int j = 0; j < 32; j++) rr[j] = sig_input[31 - j]; v = rr; }
        while (n > 1 && !*v) { v++; n--; }
        memset(rs + 32 * i, 0, 32 - n);
        memcpy(rs + 32 * i + (32 - n), v, n);
    }
    b64_encode(rs, 64, rs_b64, 1, 0);
    snprintf(out, cap, "%s.%s.%s", hb, pb, rs_b64);
}

/* ---------------------------------------------------------------- checkers: the fake service */

static int svc_calls[8];

/* the tool's print of a reply: exception 0, then the words, as "0x<offset>: " lines of four with an ASCII column */
static void parcel_print(const uint32_t *w, size_t nw, char *out, size_t n)
{
    size_t k = (size_t)snprintf(out, n, "Result: Parcel(\n");
    for (size_t i = 0; i < nw && k < n; i += 4)
    {
        k += (size_t)snprintf(out + k, n - k, "  0x%08zx: ", i * 4);
        for (size_t j = i; j < i + 4 && j < nw && k < n; j++)
            k += (size_t)snprintf(out + k, n - k, "%08x ", w[j]);
        if (k < n)
            k += (size_t)snprintf(out + k, n - k, "'....'\n");
    }
    if (k < n)
        snprintf(out + k, n - k, ")\n");
}

static size_t put_bytes(uint32_t *w, const uint8_t *p, size_t len)
{
    w[0] = 0;
    w[1] = (uint32_t)len;
    for (size_t i = 0; i < len; i++)
        w[2 + i / 4] = (i % 4 ? w[2 + i / 4] : 0) | (uint32_t)p[i] << (8 * (i % 4));
    return 2 + (len + 3) / 4;
}

/* the DER ECDSA signature the fake EC key makes: R the digest, S the digest reversed, an INTEGER's 0x00 where the top bit is set */
static void der_sig(const uint8_t dg[32], uint8_t *out, int *len)
{
    uint8_t rr[32], *q = out + 2;
    for (int j = 0; j < 32; j++) rr[j] = dg[31 - j];
    for (int i = 0; i < 2; i++)
    {
        const uint8_t *v = i ? rr : dg;
        int pad = v[0] & 0x80;
        *q++ = 2;
        *q++ = (uint8_t)(32 + pad);
        if (pad) *q++ = 0;
        memcpy(q, v, 32);
        q += 32;
    }
    out[0] = 0x30;
    out[1] = (uint8_t)(q - out - 2);
    *len = (int)(q - out);
}

/* "/system/bin/service call amazon.security.dha <code> [i32 ...]": what fireos-dha answers on the tester's Echo, with
 * the fixed key.  sign's signature: byte i is digest[i % 32] ^ 0x5A, so the test can check what was signed. */
void core_run(char *const argv[], char *out, size_t n)
{
    uint32_t w[400];
    size_t nw = 0;
    int code;
    out[0] = 0;
    if (strcmp(argv[0], "/system/bin/service") || strcmp(argv[1], "call") || strcmp(argv[2], getenv("DHA_FAKE_SVC_NAME") ? getenv("DHA_FAKE_SVC_NAME") : "amazon.security.dha"))
        return;
    code = atoi(argv[3]);
    if (code >= 0 && code < 8)
        svc_calls[code]++;
    if (code == 6)
    {
        w[0] = 0;
        w[1] = (uint32_t)atoi(getenv("DHA_FAKE_SVC_VERSION") ? getenv("DHA_FAKE_SVC_VERSION") : "1");
        nw = 2;
    }
    else if (code == 3)
        nw = put_bytes(w, dha_der, sizeof dha_der);
    else if (code == 4)
        nw = put_bytes(w, dha_mac, sizeof dha_mac);
    else if (code == 5 && getenv("DHA_FAKE_SVC_VERSION"))
    {
        /* crown: the leaf, then another certificate; LF line ends as the service prints them */
        static char chain[sizeof dha_cert_pem * 2];
        size_t k = 0;
        for (const char *c = dha_cert_pem; *c; c++)
            if (*c != '\r')
                chain[k++] = *c;
        memcpy(chain + k, chain, k);
        nw = put_bytes(w, (const uint8_t *)chain, 2 * k);
    }
    else if (code == 1 && getenv("DHA_FAKE_SVC_VERSION"))
    {
        uint8_t dg[32], der[80];
        int k = 4, n = 2;
        if (!argv[k] || strcmp(argv[k], "i32") || atoi(argv[k + 1]) != 32)
            return;
        for (int i = 0; i < 8; i++)
        {
            uint32_t v;
            k += 2;
            if (!argv[k] || strcmp(argv[k], "i32"))
                return;
            v = (uint32_t)strtol(argv[k + 1], NULL, 10);
            for (int b = 0; b < 4; b++)
                dg[i * 4 + b] = (uint8_t)(v >> (8 * b));
        }
        der_sig(dg, der, &n);
        nw = put_bytes(w, der, (size_t)n);
    }
    else if (code == 1)
    {
        uint8_t dg[32], sig[256];
        int k = 4;
        if (!argv[k] || strcmp(argv[k], "i32") || atoi(argv[k + 1]) != 32)
            return;                                 /* the byte[]'s length first */
        for (int i = 0; i < 8; i++)
        {
            uint32_t v;
            k += 2;
            if (!argv[k] || strcmp(argv[k], "i32"))
                return;
            v = (uint32_t)strtol(argv[k + 1], NULL, 10);
            for (int b = 0; b < 4; b++)
                dg[i * 4 + b] = (uint8_t)(v >> (8 * b));
        }
        for (int i = 0; i < 256; i++)
            sig[i] = dg[i % 32] ^ 0x5A;
        nw = put_bytes(w, sig, sizeof sig);
    }
    else
    {
        w[0] = 0xfffffff9u;                          /* getDhaCertificateChain on version 1: an exception */
        w[1] = w[2] = 0;
        nw = 3;
    }
    parcel_print(w, nw, out, n);
}

/* Java MAP's drvV1 (kcpsdk.auth.w/r/l0 of com.amazon.imp.apk), built from the decompiled code's order of puts */
static void expected_java(char *out, size_t cap, time_t when, const char *dsn, const char *cpuid)
{
    char h[800], p[400], hb[1100], pb[600], date[32], mod_b64[400], mac_b64[64], sig[256], sig_b64[400], cpu[96] = "";
    uint8_t dg[32];
    struct tm tm;

    b64_encode(dha_mod, 256, mod_b64, 1, 1);                   /* Base64 flags 10: URL-safe, padded */
    b64_encode(dha_mac, sizeof dha_mac, mac_b64, 1, 1);
    gmtime_r(&when, &tm);                                      /* SimpleDateFormat in UTC */
    strftime(date, sizeof date, "%Y-%m-%dT%H:%MZ", &tm);
    snprintf(h, sizeof h, "{\"typ\":\"drvV1\",\"alg\":\"PS256\",\"jwk\":{\"kty\":\"RSA\",\"n\":\"%s\",\"e\":\"65537\","
             "\"mac\":\"%s\"}}", mod_b64, mac_b64);
    if (cpuid)
        snprintf(cpu, sizeof cpu, ",\"cpuid\":\"%s\"", cpuid);
    snprintf(p, sizeof p, "{\"dat\":\"%s\",\"dev\":{\"typ\":\"v1\",\"dt\":\"%s\",\"dsn\":\"%s\"%s},\"cust\":{\"typ\":\"v1\"}}",
             date, dha_dt, dsn, cpu);
    b64_encode(h, strlen(h), hb, 1, 0);                        /* flags 11: unpadded */
    b64_encode(p, strlen(p), pb, 1, 0);
    snprintf(out, cap, "%s.%s", hb, pb);
    sha256(out, strlen(out), dg);
    for (int i = 0; i < 256; i++)
        sig[i] = (char)(dg[i % 32] ^ 0x5A);
    b64_encode(sig, 256, sig_b64, 1, 0);
    snprintf(out + strlen(out), cap - strlen(out), ".%s", sig_b64);
}

/* Java MAP's drvV3 (crown): x5c = the first certificate's body in one line with "/" written "\\/" (org.json), the payload
 * as drvV1's, R || S from the DER signature */
static void expected_java_v3(char *out, size_t cap, time_t when, const char *dsn, const char *cpuid)
{
    static const char begin[] = "-----BEGIN CERTIFICATE-----", endm[] = "-----END CERTIFICATE-----";
    char h[900], p[400], hb[1300], pb[600], date[32], rs_b64[90], cpu[96] = "";
    const char *b = strstr(dha_cert_pem, begin) + sizeof begin - 1, *e = strstr(dha_cert_pem, endm);
    uint8_t dg[32], der[80], rs[64];
    struct tm tm;
    size_t hl = (size_t)snprintf(h, sizeof h, "%s", "{\"typ\":\"drvV3\",\"alg\":\"ES256\",\"x5c\":[\"");
    int dl;

    for (const char *c = b; c < e; c++)
        if (*c != '\r' && *c != '\n')
        {
            if (*c == '/') h[hl++] = '\\';
            h[hl++] = *c;
        }
    hl += (size_t)snprintf(h + hl, sizeof h - hl, "%s", "\"]}");
    gmtime_r(&when, &tm);
    strftime(date, sizeof date, "%Y-%m-%dT%H:%MZ", &tm);
    if (cpuid)
        snprintf(cpu, sizeof cpu, ",\"cpuid\":\"%s\"", cpuid);
    snprintf(p, sizeof p, "{\"dat\":\"%s\",\"dev\":{\"typ\":\"v1\",\"dt\":\"%s\",\"dsn\":\"%s\"%s},\"cust\":{\"typ\":\"v1\"}}",
             date, dha_dt, dsn, cpu);
    b64_encode(h, hl, hb, 1, 0);
    b64_encode(p, strlen(p), pb, 1, 0);
    snprintf(out, cap, "%s.%s", hb, pb);
    sha256(out, strlen(out), dg);
    der_sig(dg, der, &dl);
    memset(rs, 0, sizeof rs);                         /* both halves are 32 bytes whole, the 0x00 dropped */
    memcpy(rs, dg, 32);
    for (int j = 0; j < 32; j++) rs[32 + j] = dg[31 - j];
    b64_encode(rs, 64, rs_b64, 1, 0);
    snprintf(out + strlen(out), cap - strlen(out), ".%s", rs_b64);
}

static int service_tests(int version2)
{
    char got[2048], want[2048], err[160], cpuinfo[] = "/tmp/dha_jwt_test_cpuinfo.XXXXXX";
    const time_t when = 1760000000;
    int fd = mkstemp(cpuinfo);
    FILE *f = fd < 0 ? NULL : fdopen(fd, "w");

    if (!f) { perror("cpuinfo"); return 1; }
    /* the tester's Echo's lines (tabs as the kernel prints them) */
    fprintf(f, "processor\t: 0\nmodel name\t: ARMv7 Processor rev 4 (v7l)\n\nHardware\t: MT8163\nRevision\t: 0000\n"
               "Serial\t\t: 8b4c188d83c47b66\n");
    fclose(f);
    setenv("HASSMIC_DHA_HAL", "/nonexistent/libacehal_dha.so", 1);
    setenv("HASSMIC_CPUINFO", cpuinfo, 1);
    setenv("TZ", "CET-1CEST", 1);            /* Java MAP dates in UTC whatever the zone: the test runs in another */
    tzset();
    if (version2)
    {
        setenv("DHA_FAKE_SVC_VERSION", "2", 1);
        setenv("DHA_FAKE_SVC_NAME", "fireosdhaservice", 1);      /* athena's name: tried after fireos-dha's */
        check(dha_jwt(dha_dt, "G0911B0593750S3V", when, got, sizeof got, err, sizeof err) == 0, "a drvV3 token from the fake service");
        if (fails) { fprintf(stderr, "%s\n", err); unlink(cpuinfo); return 1; }
        check(svc_calls[6] == 1 && svc_calls[5] == 1 && svc_calls[1] == 1 && !svc_calls[3] && !svc_calls[4],
              "version once, then the chain and one signature; no key, no HMAC");
        expected_java_v3(want, sizeof want, when, "G0911B0593750S3V", "8b4c188d83c47b66");
        check(!strcmp(got, want), "byte for byte what Java MAP builds (first certificate, \\/ escapes, R || S)");
        if (strcmp(got, want)) { printf("     got  %.140s...\n     want %.140s...\n", got, want); }
        check(dha_jwt(dha_dt, "", when, got, sizeof got, err, sizeof err) == -1, "no serial: refused");
        check(dha_jwt(dha_dt, "G0911B0593750S3V", when, got, 600, err, sizeof err) == -1, "a caller's buffer too small: refused");
        unlink(cpuinfo);
        return fails ? 1 : 0;
    }
    check(dha_jwt(dha_dt, "G0911B0593750S3V", when, got, sizeof got, err, sizeof err) == 0, "a token from the fake service");
    if (fails) { fprintf(stderr, "%s\n", err); unlink(cpuinfo); return 1; }
    check(svc_calls[6] == 1 && svc_calls[3] == 1 && svc_calls[4] == 1 && svc_calls[1] == 1,
          "version once, then key, HMAC and one signature");
    expected_java(want, sizeof want, when, "G0911B0593750S3V", "8b4c188d83c47b66");
    check(!strcmp(got, want), "byte for byte what Java MAP builds (cpuid from /proc/cpuinfo, the caller's serial)");
    if (strcmp(got, want)) { printf("     got  %.120s...\n     want %.120s...\n", got, want); }
    check(dha_serial(want, sizeof want) == -1, "no serial from the service (the caller has idme's)");

    f = fopen(cpuinfo, "w");                 /* no Serial line: MAP puts null, which drops the key */
    if (f) { fprintf(f, "processor\t: 0\n"); fclose(f); }
    check(dha_jwt(dha_dt, "G0911B0593750S3V", when, got, sizeof got, err, sizeof err) == 0, "a token without a CPU serial");
    expected_java(want, sizeof want, when, "G0911B0593750S3V", NULL);
    check(!strcmp(got, want), "cpuid left out, as JSONObject.put(name, null) does");

    check(dha_jwt(dha_dt, "", when, got, sizeof got, err, sizeof err) == -1, "no serial: refused");
    check(dha_jwt(dha_dt, "G0911B0593750S3V", when, got, 600, err, sizeof err) == -1, "a caller's buffer too small: refused");
    unlink(cpuinfo);
    return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
    char got[2048], want[2048], err[160];
    const time_t when = 1760000000;        /* any fixed moment: 2025-10-09 08:53 UTC */

    if (argc < 2) { fprintf(stderr, "usage: dha_jwt_test <path to dha_hal_fake.so> | --service | --service2\n"); return 2; }
    if (!strcmp(argv[1], "--service") || !strcmp(argv[1], "--service2"))
        return service_tests(!strcmp(argv[1], "--service2"));
    setenv("HASSMIC_DHA_HAL", argv[1], 1);
    unsetenv("DHA_FAKE_V3");
    setenv("TZ", "UTC0", 1);               /* the Echos run on GMT; the date comes out the same everywhere */
    tzset();

    check(dha_jwt(dha_dt, "not-this-one", when, got, sizeof got, err, sizeof err) == 0, "a token from the fake key");
    if (fails) { fprintf(stderr, "%s\n", err); return 1; }
    check(strchr(got, '.') == strrchr(got, '.') ? 0 : 1, "three parts");
    check(dha_serial(want, sizeof want) == 0 && !strcmp(want, "G000TEST00000000"), "the serial is field 0x101, NUL off");

    /* the signature input: SHA-256 over the first two parts (that, the fake signs) */
    {
        char hp[1200];
        snprintf(hp, sizeof hp, "%.*s", (int)(strrchr(got, '.') - got), got);
        sha256(hp, strlen(hp), sig_input);
    }
    expected(want, sizeof want, when);
    check(!strcmp(got, want), "byte for byte what MAP builds");
    if (strcmp(got, want)) { printf("     got  %.90s...\n     want %.90s...\n", got, want); }

    char again[2048];
    dha_jwt(dha_dt, "not-this-one", when, again, sizeof again, err, sizeof err);
    check(!strcmp(got, again), "the same moment signs the same token");

    check(dha_jwt(dha_dt, "not-this-one", when, got, 64, err, sizeof err) == -1, "a caller's buffer too small: refused, not overrun");

    /* donut's drvV3, the same way */
    setenv("DHA_FAKE_V3", "1", 1);
    check(dha_jwt(dha_dt, "not-this-one", when, got, sizeof got, err, sizeof err) == 0, "a drvV3 token from the fake EC key");
    if (fails) { fprintf(stderr, "%s\n", err); return 1; }
    check(strchr(got, '.') == strrchr(got, '.') ? 0 : 1, "three parts");
    check(dha_serial(want, sizeof want) == 0 && !strcmp(want, "G000TEST00000000"), "the serial is field 0x101 there too");
    {
        char hp[1200];
        snprintf(hp, sizeof hp, "%.*s", (int)(strrchr(got, '.') - got), got);
        sha256(hp, strlen(hp), sig_input);
    }
    expected_v3(want, sizeof want, when);
    check(!strcmp(got, want), "byte for byte what donut's MAP builds");
    if (strcmp(got, want)) { printf("     got  %.90s...\n     want %.90s...\n", got, want); }

    char again3[2048];
    dha_jwt(dha_dt, "not-this-one", when, again3, sizeof again3, err, sizeof err);
    check(!strcmp(got, again3), "the same moment signs the same token");

    check(dha_jwt(dha_dt, "not-this-one", when, got, 64, err, sizeof err) == -1, "a caller's buffer too small: refused, not overrun");

    /* the refusals say why: a key of neither shape is not tried as one of them, a missing certificate names the group */
    setenv("DHA_FAKE_CURVE", "1", 1);
    check(dha_jwt(dha_dt, "not-this-one", when, got, sizeof got, err, sizeof err) == -1 && strstr(err, "neither"),
          "an EC key on another curve: refused as no known shape");
    unsetenv("DHA_FAKE_CURVE");
    setenv("DHA_FAKE_NOCERT", "1", 1);
    check(dha_jwt(dha_dt, "not-this-one", when, got, sizeof got, err, sizeof err) == -1 && strstr(err, "keystore"),
          "no certificate: the keystore group named");
    unsetenv("DHA_FAKE_NOCERT");
    return fails ? 1 : 0;
}
