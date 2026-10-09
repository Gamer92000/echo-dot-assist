/* dha.c's attestation token against what MAP builds (libace_map.so of biscuit and radar, read out of the firmware):
 * the two format strings, the b64url of every part (URL-safe, unpadded), the fields from the HAL, the date as MAP
 * writes it, and the signature over SHA-256 of the first two parts joined.  The HAL is the fake of
 * dha_hal_fake.c through HASSMIC_DHA_HAL; the key and fields are dha_hal_key.h, shared with it, so this test pins
 * dha.c's format, not the fake's data.  Both shapes: drvV1 (the RSA keymaster) and donut's drvV3 (DHA_FAKE_V3: the
 * certificate of field 0x203, the fixed cpuid, the DER signature split into R and S). */
#include "dha.h"
#include "hash.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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

int main(int argc, char **argv)
{
    char got[2048], want[2048], err[160];
    const time_t when = 1760000000;        /* any fixed moment: 2025-10-09 08:53 UTC */

    if (argc < 2) { fprintf(stderr, "usage: dha_jwt_test <path to dha_hal_fake.so>\n"); return 2; }
    setenv("HASSMIC_DHA_HAL", argv[1], 1);
    unsetenv("DHA_FAKE_V3");
    setenv("TZ", "UTC0", 1);               /* the Echos run on GMT; the date comes out the same everywhere */
    tzset();

    check(dha_jwt(dha_dt, when, got, sizeof got, err, sizeof err) == 0, "a token from the fake key");
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
    dha_jwt(dha_dt, when, again, sizeof again, err, sizeof err);
    check(!strcmp(got, again), "the same moment signs the same token");

    check(dha_jwt(dha_dt, when, got, 64, err, sizeof err) == -1, "a caller's buffer too small: refused, not overrun");

    /* donut's drvV3, the same way */
    setenv("DHA_FAKE_V3", "1", 1);
    check(dha_jwt(dha_dt, when, got, sizeof got, err, sizeof err) == 0, "a drvV3 token from the fake EC key");
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
    dha_jwt(dha_dt, when, again3, sizeof again3, err, sizeof err);
    check(!strcmp(got, again3), "the same moment signs the same token");

    check(dha_jwt(dha_dt, when, got, 64, err, sizeof err) == -1, "a caller's buffer too small: refused, not overrun");
    return fails ? 1 : 0;
}
