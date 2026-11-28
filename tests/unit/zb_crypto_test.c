/* src/zigbee/crypto.c against references: CCM* (level 5) against what tests/unit/zb_crypto_ref.py computes with
 * Python's AES-CCM (tag 4, nonce 13: CCM* with M=4 is CCM), the install code of the Zigbee spec's example and its link
 * key, and HMAC-MMO against the same in Python (built on this file's own MMO hash, which the install code holds). */
#include "../../src/zigbee/zb.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails;
static void hex(const char *s, uint8_t *o, size_t n) { for (size_t i = 0; i < n; i++) sscanf(s + 2 * i, "%2hhx", &o[i]); }
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }

int main(int argc, char **argv)
{
    /* the install code example of Zigbee's Base Device Behavior spec: 83FED340 7A939723 A5C639B2 6916D505 C3B5 */
    uint8_t code[18], key[16], want[16];
    hex("83FED3407A939723A5C639B26916D505C3B5", code, 18);
    hex("66B6900981E1EE3CA4206B6B861C02BB", want, 16);
    check(!install_code_key(code, 18, key) && !memcmp(key, want, 16), "install code -> link key 66B6900981E1EE3CA4206B6B861C02BB");
    code[3] ^= 1;
    check(install_code_key(code, 18, key) != 0, "an install code with a wrong CRC: refused");
    /* the rest against the reference's lines on stdin: "ccm <key> <nonce> <a> <m> <ct+mic>", "hmac <key> <byte> <out>" */
    char line[2048], what[8], k[40], x[40], a[512], m[512], c[600];
    int n = 0;
    while (argc > 1 && fgets(line, sizeof line, stdin)) {
        if (sscanf(line, "%7s", what) != 1) continue;
        if (!strcmp(what, "ccm") && sscanf(line, "%*s %39s %39s %511s %511s %599s", k, x, a, m, c) == 5) {
            uint8_t kk[16], nn[13], aa[256], mm[256 + 4], cc[300]; size_t al = strlen(a) / 2, ml = strlen(m) / 2;
            if (!strcmp(a, "-")) al = 0;
            if (!strcmp(m, "-")) ml = 0;
            hex(k, kk, 16); hex(x, nn, 13); if (al) hex(a, aa, al); if (ml) hex(m, mm, ml); hex(c, cc, ml + 4);
            uint8_t enc[260]; memcpy(enc, mm, ml);
            ccm_encrypt(kk, nn, aa, al, enc, ml);
            int ok = !memcmp(enc, cc, ml + 4);
            uint8_t dec[260]; memcpy(dec, cc, ml + 4);
            ok &= !ccm_decrypt(kk, nn, aa, al, dec, ml + 4) && !memcmp(dec, mm, ml);
            dec[0] ^= 1; memcpy(dec, cc, ml + 4); dec[ml] ^= 0x80;
            ok &= ccm_decrypt(kk, nn, aa, al, dec, ml + 4) != 0;
            if (!ok) { printf("FAIL ccm %s\n", line); fails++; }
            n++;
        } else if (!strcmp(what, "hmac") && sscanf(line, "%*s %39s %39s %39s", k, x, c) == 3) {
            uint8_t kk[16], out[16], ww[16];
            hex(k, kk, 16); hex(c, ww, 16);
            keyed_hash(kk, (uint8_t)strtoul(x, NULL, 16), out);
            if (memcmp(out, ww, 16)) { printf("FAIL hmac %s\n", line); fails++; }
            n++;
        }
    }
    if (argc > 1) check(n > 0, "the reference's cases");
    printf("%d cases from the reference\n", n);
    return fails ? 1 : 0;
}
