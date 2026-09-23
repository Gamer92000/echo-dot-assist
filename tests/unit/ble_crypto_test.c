/* AES, AES-CMAC and the SMP functions against FIPS-197, RFC 4493 and the sample data of the Bluetooth Core spec
 * (Vol 3 Part H, Appendix D). Values written as the spec prints them: most significant octet first. */
#include <stdio.h>
#include <string.h>
#include "ble_crypto.h"

static int bad;
static void unhex(const char *s, uint8_t *o) { for (size_t i = 0; s[2 * i]; i++) sscanf(s + 2 * i, "%2hhx", &o[i]); }
static void le(const char *s, uint8_t *o) { size_t n = strlen(s) / 2; unhex(s, o); bytes_reverse(o, o, n); }  /* PDU order */
static void check(const char *what, const uint8_t *got, const char *want)
{
    char h[80]; size_t n = strlen(want) / 2;
    for (size_t i = 0; i < n; i++) sprintf(h + 2 * i, "%02x", got[i]);
    if (strcmp(h, want)) { printf("FAIL %s: %s, want %s\n", what, h, want); bad = 1; }
}

int main(void)
{
    uint8_t k[32], m[64], a[32], b[32], c[16], d[16], o[16], o2[16], io[3], a1[7], a2[7];

    unhex("000102030405060708090a0b0c0d0e0f", k); unhex("00112233445566778899aabbccddeeff", m);
    aes128(k, m, o); check("AES-128", o, "69c4e0d86a7b0430d8cdb78070b4c55a");

    unhex("2b7e151628aed2a6abf7158809cf4f3c", k);
    aes_cmac(k, m, 0, o); check("CMAC empty", o, "bb1d6929e95937287fa37d129b756746");
    unhex("6bc1bee22e409f96e93d7e117393172a", m);
    aes_cmac(k, m, 16, o); check("CMAC 16", o, "070a16b46b4d4144f79bdd9dd04a287c");
    unhex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411", m);
    aes_cmac(k, m, 40, o); check("CMAC 40", o, "dfa66747de9ae63030ca32611497c827");

    unhex("20b003d2f297be2c5e2c83a7e9f9a5b9eff49111acf4fddbcc0301480e359de6", a);
    unhex("55188b3d32f6bb9a900afcfbeed4e72a59cb9ac2f19d7cfb6b4fdd49f47fc5fd", b);
    unhex("d5cb8454d177733effffb2ec712baeab", c);
    smp_f4(a, b, c, 0, o); check("f4", o, "f2c916f107a9bd1cf1eda1bea974872d");

    unhex("ec0234a357c8ad05341010a60a397d9b99796b13b4f866f1868d34f373bfa698", a);
    unhex("d5cb8454d177733effffb2ec712baeab", c); unhex("a6e8e7cc25a75f6e216583f7ff3dc4cf", d);
    unhex("0056123737bfce", a1); unhex("00a713702dcfc1", a2);
    smp_f5(a, c, d, a1, a2, o, o2); check("f5 MacKey", o, "2965f176a1084a02fd3f6a20ce636e20"); check("f5 LTK", o2, "6986791169d7cd23980522b594750a38");

    unhex("12a3343bb453bb5408da42d20c2d0fc8", b); unhex("010102", io);
    smp_f6(o, c, d, b, io, a1, a2, o2); check("f6", o2, "e3c473989cd0e8c5d26c0b09da958f61");

    /* legacy: preq/pres/addresses as PDUs carry them */
    uint8_t r[16], preq[7], pres[7], ia[6], ra[6], r1[16], r2[16];
    memset(k, 0, 16);
    le("5783d52156ad6f0e6388274ec6702ee0", r); le("07071000000101", preq); le("05000800000302", pres);
    le("a1a2a3a4a5a6", ia); le("b1b2b3b4b5b6", ra);
    smp_c1(k, r, preq, pres, 1, ia, 0, ra, o); bytes_reverse(o, o, 16); check("c1", o, "1e1e3fef878988ead2a74dc5bef13b86");
    le("000f0e0d0c0b0a091122334455667788", r1); le("010203040506070899aabbccddeeff00", r2);
    smp_s1(k, r1, r2, o); bytes_reverse(o, o, 16); check("s1", o, "9a1fe1f0e8b0f49b5b4216ae796da062");

    uint8_t irk[16], addr[6];
    le("ec0234a357c8ad05341010a60a397d9b", irk); le("7081940dfbaa", addr);        /* prand 708194, hash 0dfbaa */
    if (!smp_ah_match(irk, addr)) { printf("FAIL ah: address not resolved\n"); bad = 1; }
    addr[0] ^= 1;
    if (smp_ah_match(irk, addr)) { printf("FAIL ah: wrong address resolved\n"); bad = 1; }

    if (!bad) printf("AES, CMAC and SMP f4/f5/f6/c1/s1/ah vectors ok\n");
    return bad;
}
