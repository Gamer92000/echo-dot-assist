/* dha_test: what the Echo's device attestation HAL (libacehal_dha.so -> /system/lib/hw/amzn_dha.<soc>.so, Amazon's
 * keymaster) gives: the public key, the fields MAP puts into its registration token, and a signature over a test hash.
 * MAP (libace_map.so map_registration_create_dha_jwt) signs /auth/register's device_authentication_token with it; see
 * src/hassmic/davs.c.  The private key never leaves the HAL.
 *   dha_test            public key (base64 DER), fields 1 / 0x101 / 0x201..0x204, test signature
 * Calls (from libacehal_dha.so and its caller in libace_map.so):
 *   int aceDhaHal_open(void)                                    0 = ok, reference counted
 *   int aceDhaHal_getPublicKey(uint8_t **der, uint32_t *len)    malloc'd SubjectPublicKeyInfo
 *   int aceDhaHal_getField(int id, char **val, uint32_t *len)   malloc'd; 1 = cpuid, 0x204 = "mac" of the JWT header
 *   int aceDhaHal_signData(const uint8_t *data, uint32_t len, uint8_t **sig, uint32_t *siglen)
 *   int aceDhaHal_close(void) */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void dump(const char *what, const uint8_t *p, uint32_t n)
{
    printf("%s (%u):", what, n);
    for (uint32_t i = 0; i < n && i < 600; i++) printf(i % 32 ? "%02x" : "\n  %02x", p[i]);
    int text = n > 0;
    for (uint32_t i = 0; i < n; i++) if ((p[i] < 32 || p[i] > 126) && !(i == n - 1 && !p[i])) text = 0;
    if (text) printf("\n  \"%.*s\"", (int)n, (const char *)p);
    printf("\n");
}

int main(void)
{
    void *h = dlopen("libacehal_dha.so", RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    int (*op)(void) = dlsym(h, "aceDhaHal_open");
    int (*pk)(uint8_t **, uint32_t *) = dlsym(h, "aceDhaHal_getPublicKey");
    int (*gf)(int, uint8_t **, uint32_t *) = dlsym(h, "aceDhaHal_getField");
    int (*sg)(const uint8_t *, uint32_t, uint8_t **, uint32_t *) = dlsym(h, "aceDhaHal_signData");
    if (!op || !pk || !gf || !sg) { fprintf(stderr, "dlsym failed\n"); return 1; }
    int r = op();
    printf("open: %d\n", r);
    if (r) return 1;
    uint8_t *p = NULL; uint32_t n = 0;
    r = pk(&p, &n);
    printf("getPublicKey: %d\n", r);
    if (!r && p) { dump("public key DER", p, n); free(p); }
    static const int ids[] = { 1, 0x101, 0x201, 0x202, 0x203, 0x204 };
    for (unsigned i = 0; i < sizeof ids / sizeof *ids; i++) {
        p = NULL; n = 0;
        r = gf(ids[i], &p, &n);
        char what[32]; snprintf(what, sizeof what, "field %#x", ids[i]);
        if (r || !p) printf("%s: %d\n", what, r); else { dump(what, p, n); free(p); }
    }
    uint8_t hash[32];
    for (int i = 0; i < 32; i++) hash[i] = (uint8_t)i;
    p = NULL; n = 0;
    r = sg(hash, sizeof hash, &p, &n);
    printf("signData: %d\n", r);
    if (!r && p) { dump("signature", p, n); free(p); }
    return 0;
}
