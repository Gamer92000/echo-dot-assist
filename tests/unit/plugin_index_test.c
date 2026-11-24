/* plugin_index.c: the plugin index as an Echo reads it (docs/plugins.md 5.3, 5.5, 5.6).  The lists are taken only
 * with the index key's signature, but are read as an untrusted text all the same: a list as the index's tool writes
 * it, then wrong in one way each, past every bound, and a real one cut and changed at random (under ASan/UBSan: no
 * read outside the text, and whatever comes out is within its fields' bounds) */
#define _GNU_SOURCE
#include "../../src/hassmic/plugin_index.h"
#include "../../src/third_party/monocypher.h"
#include "../../src/include/edkey.h"
#include "plugmsg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int fails, checks;
static uint8_t sk[64];                  /* the tests' index key */
static int fake_synced = 1;
static int fake_clock(void) { return fake_synced; }

/* malloc counted (the link wraps it): what a look at a list at the bounds holds at its peak */
#include <malloc.h>
void *__real_malloc(size_t); void *__real_calloc(size_t, size_t); void *__real_realloc(void *, size_t); void __real_free(void *);
static size_t mem_now, mem_peak;
static void mem_add(void *p) { if (p) { mem_now += malloc_usable_size(p); if (mem_now > mem_peak) mem_peak = mem_now; } }
static void mem_sub(void *p) { if (p) mem_now -= malloc_usable_size(p); }
void *__wrap_malloc(size_t n) { void *p = __real_malloc(n); mem_add(p); return p; }
void *__wrap_calloc(size_t a, size_t b) { void *p = __real_calloc(a, b); mem_add(p); return p; }
void *__wrap_realloc(void *o, size_t n) { size_t was = o ? malloc_usable_size(o) : 0; void *p = __real_realloc(o, n); if (p || !n) { mem_now -= was; mem_add(p); } return p; }
void __wrap_free(void *p) { mem_sub(p); __real_free(p); }

static void sign(const char *purpose, const char *t, size_t len, uint8_t sig[64])
{
    size_t hl = strlen(purpose); uint8_t *msg = malloc(hl + len);
    memcpy(msg, purpose, hl); memcpy(msg + hl, t, len); crypto_eddsa_sign(sig, sk, msg, hl + len); free(msg);
}
/* a revocation list of SERIAL naming NAME from FROM: taken? (pi_revoked_take's answer) */
static int take_from(long long serial, const char *name, int from, char *err, size_t cap)
{
    char t[300]; uint8_t sig[64];
    snprintf(t, sizeof t, "{\"revoked\": 1, \"serial\": %lld, \"entries\": [{\"id\": \"%s\", \"reason\": \"r\"}]}\n", serial, name);
    sign(PI_SIGN_REVOKED, t, strlen(t), sig);
    return pi_revoked_take(t, strlen(t), sig, 64, from, err, cap);
}
static int take(long long serial, const char *name, char *err, size_t cap) { return take_from(serial, name, PI_SITE, err, cap); }
/* the floor line of state/plugins/.revoked, -1 none */
static long long floor_line(const char *dir)
{
    char p[300], b[64] = ""; long long v = -1; FILE *f;
    snprintf(p, sizeof p, "%s/plugins/.revoked", dir);
    if ((f = fopen(p, "rb"))) { if (fgets(b, sizeof b, f)) sscanf(b, "floor %lld", &v); fclose(f); }
    return v;
}
static void put_file(const char *dir, const char *name, const char *text)
{
    char p[300]; FILE *f;
    snprintf(p, sizeof p, "%s/plugins/%s", dir, name);
    if ((f = fopen(p, "wb"))) { fputs(text, f); fclose(f); }
}
static ino_t inode(const char *dir, const char *name)
{
    char p[300]; struct stat st;
    snprintf(p, sizeof p, "%s/plugins/%s", dir, name);
    return stat(p, &st) ? 0 : st.st_ino;
}
static void ok(int c, const char *what) { checks++; if (!c) { fails++; printf("FAIL %s\n", what); } }

#define HEX64 "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
/* a version's "requires", as the index copies it from the manifest */
#define REQ "\"core\": 1, \"wake\": 1, \"files\": 1"
/* one plugin as tools/index.py writes it (json.dumps, indent 1), with VERSIONS for a version's fields */
static const char *PLUG =
    "{\n \"id\": \"%s\",\n \"name\": \"microWakeWord\",\n \"description\": \"The open wake word engine, with \\u00e9 and \\\"quotes\\\".\",\n \"author\": \"hassmic\",\n"
    " \"author_key\": \"" HEX64 "\",\n \"tier\": \"%s\",\n \"source\": \"https://github.com/Gamer92000/hassmic-plugin-microwakeword\",\n \"versions\": [%s]\n}";
static const char *VER =
    "{\"version\": %d, \"version_name\": \"%d.0\", \"requires\": {%s}, \"models\": [\"*\"], \"capabilities\": {%s}, \"commit\": \"" HEX64 "\","
    " \"manifest\": \"" HEX64 "\", \"package\": \"packages/x-%d.hmp\", \"size\": %d, \"sha256\": \"" HEX64 "\"}";

static char *index_of(int nplug, int nver, const char *req, const char *caps)
{
    size_t cap = 64 + (size_t)nplug * (900 + (size_t)nver * 520); char *o = malloc(cap), *vs = malloc((size_t)nver * 520 + 8), *p = malloc(900 + (size_t)nver * 520);
    size_t n = (size_t)snprintf(o, cap, "{\n \"index\": 1,\n \"serial\": 1795200000,\n \"revoked_serial\": 0,\n \"plugins\": [");
    size_t vn = 0; vs[0] = 0;
    for (int v = 1; v <= nver; v++) vn += (size_t)snprintf(vs + vn, (size_t)nver * 520 + 8 - vn, "%s", v > 1 ? "," : ""), vn += (size_t)snprintf(vs + vn, (size_t)nver * 520 + 8 - vn, VER, v, v, req, caps, v, 1000 + v);
    for (int i = 0; i < nplug; i++) {
        char id[32]; snprintf(id, sizeof id, i ? "plug%04d" : "microwakeword", i);
        snprintf(p, 900 + (size_t)nver * 520, PLUG, id, i % 3 ? "community" : "official", vs);
        n += (size_t)snprintf(o + n, cap - n, "%s%s", i ? "," : "", p);
    }
    n += (size_t)snprintf(o + n, cap - n, "]\n}\n");
    free(vs); free(p);
    return o;
}

/* TEXT with the first FROM replaced by TO (a fresh string) */
static char *with(const char *text, const char *from, const char *to)
{
    const char *at = strstr(text, from); size_t n = strlen(text) + strlen(to) + 1; char *o = malloc(n);
    if (!at) { snprintf(o, n, "%s", text); return o; }
    snprintf(o, n, "%.*s%s%s", (int)(at - text), text, to, at + strlen(from));
    return o;
}

static int parses(const char *t, char *err, size_t cap)
{
    struct pi_index ix; int r = pi_index_parse(t, strlen(t), &ix, err, cap);
    if (!r) pi_index_free(&ix);
    return !r;
}

static void sane(const struct pi_index *ix)
{
    for (int i = 0; i < ix->n; i++) {
        const struct pi_plugin *p = &ix->p[i];
        ok(ps_id_ok(p->id) && strlen(p->name) <= PI_NAME_MAX && strlen(p->description) <= PI_DESCRIPTION_MAX && strlen(p->author) <= PI_AUTHOR_MAX && strlen(p->source) <= PI_SOURCE_MAX && p->n >= 0 && p->n <= PI_VERSIONS
           && (p->tier == PS_OFFICIAL || p->tier == PS_COMMUNITY), "a plugin that came out is within its bounds");
        for (int k = 0; k < p->n; k++) ok(p->v[k].version >= 1 && p->v[k].size >= 1 && p->v[k].req[PM_SCOPE_CORE] >= 1 && (!k || p->v[k].version > p->v[k - 1].version), "and each version");
    }
}

/* 5.2's first step of a new index key: a build that takes either of two keys' signatures for every list (the index,
 * the revocation list; listings: plugin_store_test.c), and no third's.  The tests' second key, beside the first */
static void key_file(const char *path, const uint8_t pk[32]) { FILE *f = fopen(path, "wb"); fwrite(pk, 1, 32, f); fclose(f); }
static int signed_by(const uint8_t *k, const char *purpose, const char *t)
{
    uint8_t s[64], msg[300]; size_t hl = strlen(purpose), tl = strlen(t);
    memcpy(msg, purpose, hl); memcpy(msg + hl, t, tl); crypto_eddsa_sign(s, k, msg, hl + tl);
    return pi_signed(purpose, t, tl, s, 64);
}
static void second_key(const char *dir)
{
    uint8_t sk2[64], pk2[32], sk3[64], pk3[32], s2[32] = { 8 }, s3[32] = { 9 }, got[PI_KEYS][32], sig[64]; char p2[300], err[300];
    const char *t = "{\"index\": 1}", *first = getenv("HASSMIC_PLUGINS_PUB");
    crypto_eddsa_key_pair(sk2, pk2, s2); crypto_eddsa_key_pair(sk3, pk3, s3);
    snprintf(p2, sizeof p2, "%s/index2.pub", dir); key_file(p2, pk2);
    ok(!signed_by(sk2, PI_SIGN_INDEX, t), "no second key: an index signed by another key than the first is refused (only the first is read)");
    setenv("HASSMIC_PLUGINS_PUB2", p2, 1);
    ok(pi_keys(got) == 2 && !memcmp(got[1], pk2, 32), "HASSMIC_PLUGINS_PUB2: the tests' second key, beside the first");
    ok(signed_by(sk, PI_SIGN_INDEX, t) && signed_by(sk2, PI_SIGN_INDEX, t), "an index signed by either key: holds");
    ok(signed_by(sk2, PI_SIGN_REVOKED, t) && signed_by(sk2, PI_SIGN_LISTING, t), "the second key's signature for a revocation list and a listing too");
    ok(!signed_by(sk3, PI_SIGN_INDEX, t) && !signed_by(sk3, PI_SIGN_REVOKED, t) && !signed_by(sk3, PI_SIGN_LISTING, t), "a third key's: refused, for each list");
    { const char *r = "{\"revoked\": 1, \"serial\": 3, \"entries\": [{\"id\": \"bykey2\", \"reason\": \"r\"}]}\n"; uint8_t msg[400]; size_t hl = strlen(PI_SIGN_REVOKED), rl = strlen(r);
      memcpy(msg, PI_SIGN_REVOKED, hl); memcpy(msg + hl, r, rl);
      crypto_eddsa_sign(sig, sk3, msg, hl + rl);
      ok(pi_revoked_take(r, rl, sig, 64, PI_SITE, err, sizeof err) == PI_REFUSED && strstr(err, "signature"), "a revocation list by a third key: refused, said");
      ok(pi_revoked_take(r, rl, sig, 64, PI_MEMBER, err, sizeof err) == PI_REFUSED, "and from a member the same");
      crypto_eddsa_sign(sig, sk2, msg, hl + rl);
      ok(pi_revoked_take(r, rl, sig, 64, PI_MEMBER, err, sizeof err) == PI_TAKEN && pi_withdrawn("bykey2", 1, NULL, NULL, 0), "by the second key, from a member: taken, and what it names is withdrawn");
      pi_forget();
      ok(pi_revoked_serial() == 3, "kept on the flash: its signature by the second key holds at the next start");
      unsetenv("HASSMIC_PLUGINS_PUB2"); pi_forget();
      ok(pi_revoked_serial() == -1 && !pi_withdrawn("bykey2", 1, NULL, NULL, 0), "a build without the second key: the list it signed is no list (only the first is read)");
      ok(pi_revoked_unread() && pi_revoked_age() == -1 && !pi_revoked_current(), "... said as such (pi_revoked_unread), and no word: the week's warning at once (K6 of a review of a30d6ed)");
      setenv("HASSMIC_PLUGINS_PUB2", p2, 1); pi_forget();
      ok(!pi_revoked_unread() && pi_revoked_serial() == 3 && pi_revoked_age() >= 0, "the second key read again: the list and its word back");
      { char kp[400]; snprintf(kp, sizeof kp, "%s/plugins/.revoked", dir); unlink(kp); snprintf(kp, sizeof kp, "%s/plugins/.revoked-at", dir); unlink(kp); } pi_forget(); }
    /* 5.2's step (2): the site's list signed anew by the second key, its serial as before.  The Echo of both keys
     * keeps it as it came now (PI_SAME, the file rewritten, its floor line as it was), so that the step-(3) build,
     * the second key alone, still reads the list it kept (a review of 3c5d7ff, R1: the file stayed the first key's,
     * and what it withdrew ran under that build until its next look) */
    { const char *r = "{\"revoked\": 1, \"serial\": 1700000000, \"entries\": [{\"id\": \"rotated\", \"reason\": \"r\"}]}\n"; uint8_t msg[400]; size_t hl = strlen(PI_SIGN_REVOKED), rl = strlen(r);
      long long fl; ino_t was;
      memcpy(msg, PI_SIGN_REVOKED, hl); memcpy(msg + hl, r, rl);
      crypto_eddsa_sign(sig, sk, msg, hl + rl);
      ok(pi_revoked_take(r, rl, sig, 64, PI_SITE, err, sizeof err) == PI_TAKEN, "a two-key build: the first key's list taken");
      fl = floor_line(dir); was = inode(dir, ".revoked");
      ok(pi_revoked_take(r, rl, sig, 64, PI_SITE, err, sizeof err) == PI_SAME && inode(dir, ".revoked") == was, "the same list by the same key again: the same, the file left alone");
      crypto_eddsa_sign(sig, sk2, msg, hl + rl);
      ok(pi_revoked_take(r, rl, sig, 64, PI_SITE, err, sizeof err) == PI_SAME && inode(dir, ".revoked") != was && floor_line(dir) == fl,
         "the same list signed by the second key (step 2): the same, and the file written anew with that signature, its floor line kept");
      was = inode(dir, ".revoked");
      ok(pi_revoked_take(r, rl, sig, 64, PI_SITE, err, sizeof err) == PI_SAME && inode(dir, ".revoked") == was, "... and by it again: left alone");
      crypto_eddsa_sign(sig, sk, msg, hl + rl);
      ok(pi_revoked_take(r, rl, sig, 64, PI_MEMBER, err, sizeof err) == PI_SAME && inode(dir, ".revoked") == was,
         "the first key's copy of it again (a member's, or the site's before its switch): the same, the file left the second key's (a review of 3fcd319, S3)");
      pi_forget();
      ok(pi_revoked_serial() == 1700000000 && pi_withdrawn("rotated", 1, NULL, NULL, 0), "read again at a start: the list and what it withdraws");
      unsetenv("HASSMIC_PLUGINS_PUB2"); setenv("HASSMIC_PLUGINS_PUB", p2, 1); pi_forget();
      ok(pi_revoked_serial() == 1700000000 && pi_withdrawn("rotated", 1, NULL, NULL, 0) && !pi_revoked_unread(),
         "the step-(3) build, the second key alone: the list kept holds, what it withdraws stays withdrawn");
      setenv("HASSMIC_PLUGINS_PUB", first, 1); setenv("HASSMIC_PLUGINS_PUB2", p2, 1);
      { char kp[400]; snprintf(kp, sizeof kp, "%s/plugins/.revoked", dir); unlink(kp); snprintf(kp, sizeof kp, "%s/plugins/.revoked-at", dir); unlink(kp); } pi_forget(); }
    setenv("HASSMIC_PLUGINS_PUB2", "/nonexistent", 1);
    ok(pi_keys(got) == -1 && !signed_by(sk, PI_SIGN_INDEX, t), "a second key named and not to be had: nothing is taken as signed");
    setenv("HASSMIC_PLUGINS_PUB2", p2, 1); unsetenv("HASSMIC_PLUGINS_PUB");
    ok(pi_keys(got) == 2 && got[0][0] == 0xc2 && !memcmp(got[1], pk2, 32), "HASSMIC_PLUGINS_PUB2 alone: beside the key built in");
    unsetenv("HASSMIC_PLUGINS_PUB2"); setenv("HASSMIC_PLUGINS_PUB", first, 1);
}
/* Sources (issue #175): the owner's own index (pi_source).  A key of the owner's beside the project's, after it; no
 * check takes the two lists' signatures as the owner's but never a package's listing (purpose ""); the build's least
 * for a listing by a built-in key whatever the index, 0 for the owner's */
static void owner_source(void)
{
    uint8_t osk[64], opk[32], os[32] = { 11 }, got[PI_KEYS][32], zero[64] = { 0 }; const char *t = "{\"index\": 1}";
    crypto_eddsa_key_pair(osk, opk, os);
    setenv("HASSMIC_PLUGINS_LEAST", "500:400", 1);
    ok(!signed_by(osk, PI_SIGN_INDEX, t) && pi_least_index() == 500 && pi_least_listing(0) == 500, "no Sources: the owner's key is none, the build's least");
    pi_source("http://own/", opk, 0);
    ok(pi_keys(got) == 2 && !memcmp(got[1], opk, 32) && pi_keys_builtin() == 1, "the owner's key after the built-in one");
    ok(signed_by(osk, PI_SIGN_INDEX, t) && signed_by(sk, PI_SIGN_INDEX, t), "lists by either taken");
    ok(pi_signer(PI_SIGN_INDEX, t, strlen(t), zero, 64) == -1 && pi_signer("", t, strlen(t), zero, 64) == -1, "no signature: none");
    ok(pi_least_index() == 0 && pi_least_revoked() == 0 && pi_least_listing(0) == 500 && pi_least_listing(1) == 0,
       "the owner's serials: least 0 for its lists and its key's listings, the build's for the project key's listings");
    ok(!strcmp(pi_base(), "http://own/") && !strcmp(pi_key_name(1), "own (Sources)") && !strcmp(pi_key_name(0), "first"), "its base, its key named so");
    pi_source("http://own/", NULL, 1);
    ok(pi_signer(PI_SIGN_INDEX, t, strlen(t), zero, 64) == 1 && pi_signer(PI_SIGN_REVOKED, t, strlen(t), zero, 64) == 1, "no check: the lists' signatures taken, as the owner's place");
    ok(pi_signer("", t, strlen(t), zero, 64) == -1, "... never a package's listing (a file would pass for official)");
    ok(signed_by(sk, "", t) && signed_by(osk, "", t) == 0, "a listing signed by the project's key still holds, by no key none (with no check and no owner key)");
    pi_source("", NULL, 0);
    ok(!pi_source_custom() && !pi_source_trusts() && pi_least_index() == 500 && pi_keys(got) == 1, "back to the project's: as before");
    unsetenv("HASSMIC_PLUGINS_LEAST");
}

/* Keys under which any signature holds (edkey.h, K3 of a review of a30d6ed): the eight points of small order, and the
 * identity once more in a non-canonical encoding (y = p + 1; Monocypher takes those).  Each as the second key and as
 * the first: no key at all, and the null signature (R the identity, s = 0) taken for nothing */
static const uint8_t small_order[][32] = {
    { 1 },                                                              /* the identity */
    { 0 },                                                              /* order 4 */
    { 0xec, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f },     /* order 2 */
    { [31] = 0x80 },                                                    /* order 4, the other sign */
    { 0x26, 0xe8, 0x95, 0x8f, 0xc2, 0xb2, 0x27, 0xb0, 0x45, 0xc3, 0xf4, 0x89, 0xf2, 0xef, 0x98, 0xf0,
      0xd5, 0xdf, 0xac, 0x05, 0xd3, 0xc6, 0x33, 0x39, 0xb1, 0x38, 0x02, 0x88, 0x6d, 0x53, 0xfc, 0x05 },     /* order 8 */
    { 0xc7, 0x17, 0x6a, 0x70, 0x3d, 0x4d, 0xd8, 0x4f, 0xba, 0x3c, 0x0b, 0x76, 0x0d, 0x10, 0x67, 0x0f,
      0x2a, 0x20, 0x53, 0xfa, 0x2c, 0x39, 0xcc, 0xc6, 0x4e, 0xc7, 0xfd, 0x77, 0x92, 0xac, 0x03, 0x7a },
    { 0x26, 0xe8, 0x95, 0x8f, 0xc2, 0xb2, 0x27, 0xb0, 0x45, 0xc3, 0xf4, 0x89, 0xf2, 0xef, 0x98, 0xf0,
      0xd5, 0xdf, 0xac, 0x05, 0xd3, 0xc6, 0x33, 0x39, 0xb1, 0x38, 0x02, 0x88, 0x6d, 0x53, 0xfc, 0x85 },
    { 0xc7, 0x17, 0x6a, 0x70, 0x3d, 0x4d, 0xd8, 0x4f, 0xba, 0x3c, 0x0b, 0x76, 0x0d, 0x10, 0x67, 0x0f,
      0x2a, 0x20, 0x53, 0xfa, 0x2c, 0x39, 0xcc, 0xc6, 0x4e, 0xc7, 0xfd, 0x77, 0x92, 0xac, 0x03, 0xfa },
    { 0xee, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f },     /* the identity, y = p + 1 */
};
static void weak_keys(const char *dir)
{
    uint8_t got[PI_KEYS][32], null_sig[64] = { 1 }, sk2[64], pk2[32], s2[32] = { 11 }; char p2[300], err[300];
    const char *t = "{\"index\": 1}", *first = getenv("HASSMIC_PLUGINS_PUB");
    const char *r = "{\"revoked\": 1, \"serial\": 5, \"entries\": []}\n";
    int weak = 1, held = 1, refused = 1, strong = 1;
    snprintf(p2, sizeof p2, "%s/weak.pub", dir);
    for (size_t k = 0; k < sizeof small_order / 32; k++) {
        weak &= ed_key_weak(small_order[k]);
        /* (proof the test means it: the null signature verifies under each, as the review found) */
        held &= !crypto_eddsa_check(null_sig, small_order[k], (const uint8_t *)"hassmic plugin index 1\n{}", 25);
        key_file(p2, small_order[k]);
        setenv("HASSMIC_PLUGINS_PUB2", p2, 1);
        refused &= pi_keys(got) == -1 && !pi_signed(PI_SIGN_INDEX, t, strlen(t), null_sig, 64) && !pi_signed(PI_SIGN_LISTING, t, strlen(t), null_sig, 64)
                   && pi_revoked_take(r, strlen(r), null_sig, 64, PI_SITE, err, sizeof err) == PI_REFUSED && !signed_by(sk, PI_SIGN_INDEX, t);
        unsetenv("HASSMIC_PLUGINS_PUB2"); setenv("HASSMIC_PLUGINS_PUB", p2, 1);
        refused &= pi_keys(got) == -1 && !pi_signed(PI_SIGN_INDEX, t, strlen(t), null_sig, 64);
        setenv("HASSMIC_PLUGINS_PUB", first, 1);
    }
    ok(weak, "edkey.h: each of the eight points of small order, and the identity in a non-canonical encoding, is a key under which any signature holds");
    ok(held, "(the null signature verifies under each: what the rule is there for)");
    ok(refused, "such a key as the second or the first: no key (pi_keys -1), and the null signature taken for no list, no listing, no revocation list");
    for (int k = 0; k < 64; k++) { s2[0] = (uint8_t)k; crypto_eddsa_key_pair(sk2, pk2, s2); strong &= !ed_key_weak(pk2); }
    ok(strong && !ed_key_weak(sk + 32), "keys made by key_pair: never one (64 seeds, and the tests' own)");
}

#ifdef PI_TEST_BUILT_PUB2
/* built with -DPLUGINS_PUB2 (the Makefile's second build of this test): the public key of the seed { 42 } */
static void keys_built_in(const char *dir)
{
    uint8_t sk2[64], pk2[32], s2[32] = { 42 }, sk1[64], pk1[32], s1[32] = { 7 }, got[PI_KEYS][32]; char p1[300]; const char *t = "{\"index\": 1}";
    crypto_eddsa_key_pair(sk2, pk2, s2); crypto_eddsa_key_pair(sk1, pk1, s1);
    ok(pi_keys(got) == 2 && got[0][0] == 0xc2 && got[0][31] == 0xf5 && !memcmp(got[1], pk2, 32), "a second key built in: read beside the first");
    ok(signed_by(sk2, PI_SIGN_INDEX, t) && signed_by(sk2, PI_SIGN_REVOKED, t) && signed_by(sk2, PI_SIGN_LISTING, t), "the built-in second key's signature holds for each list");
    ok(!signed_by(sk1, PI_SIGN_INDEX, t), "another key's: refused");
    snprintf(p1, sizeof p1, "%s/index.pub", dir); key_file(p1, pk1); setenv("HASSMIC_PLUGINS_PUB", p1, 1);
    ok(pi_keys(got) == 1 && !memcmp(got[0], pk1, 32) && signed_by(sk1, PI_SIGN_INDEX, t) && !signed_by(sk2, PI_SIGN_INDEX, t),
       "the tests' own first key: in place of both built in (never the build's second beside it)");
    unsetenv("HASSMIC_PLUGINS_PUB");
}
#endif

int main(void)
{
    char err[300], dir[200], path[300]; struct pi_index ix; struct pi_revoked rv; uint8_t seed[32] = { 7 }, pk[32], sig[64], got[PI_KEYS][32];
    FILE *f;
    snprintf(dir, sizeof dir, "/tmp/plugin_index_test.%d", (int)getpid()); mkdir(dir, 0700); setenv("HASSMIC_STATE", dir, 1);
    unsetenv("HASSMIC_PLUGINS_LEAST"); unsetenv("HASSMIC_PLUGINS_BUILT");

    /* ---- the key, and what it signs */
    unsetenv("HASSMIC_PLUGINS_PUB"); unsetenv("HASSMIC_PLUGINS_PUB2");
#ifdef PI_TEST_BUILT_PUB2
    keys_built_in(dir);
    { char cmd[300]; snprintf(cmd, sizeof cmd, "rm -rf %s", dir); if (system(cmd)) {} }
    printf("plugin_index (a second key built in): %d checks, %s\n", checks, fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
#endif
    ok(pi_keys(got) == 1 && got[0][0] == 0xc2 && got[0][31] == 0xf5, "the key built in: keys/plugins.pub's, and no second (none is built in)");
    ok(!strncmp(pi_base(), "https://", 8) && pi_base()[strlen(pi_base()) - 1] == '/', "the site built in: https, ending in /");
    crypto_eddsa_key_pair(sk, pk, seed);
    snprintf(path, sizeof path, "%s/index.pub", dir); f = fopen(path, "wb"); fwrite(pk, 1, 32, f); fclose(f); setenv("HASSMIC_PLUGINS_PUB", path, 1);
    ok(pi_keys(got) == 1 && !memcmp(got[0], pk, 32), "HASSMIC_PLUGINS_PUB: the tests' key");
    second_key(dir);
    owner_source();
    weak_keys(dir);
    { const char *t = "{\"index\": 1}"; uint8_t msg[200]; size_t hl = strlen(PI_SIGN_INDEX);
      memcpy(msg, PI_SIGN_INDEX, hl); memcpy(msg + hl, t, strlen(t)); crypto_eddsa_sign(sig, sk, msg, hl + strlen(t));
      ok(pi_signed(PI_SIGN_INDEX, t, strlen(t), sig, 64), "an index signed by the key: holds");
      ok(!pi_signed(PI_SIGN_REVOKED, t, strlen(t), sig, 64) && !pi_signed(PI_SIGN_LISTING, t, strlen(t), sig, 64), "the same signature is none for a revocation list or a listing (each names its purpose)");
      ok(!pi_signed(PI_SIGN_INDEX, t, strlen(t) - 1, sig, 64) && !pi_signed(PI_SIGN_INDEX, t, strlen(t), sig, 63) && !pi_signed(PI_SIGN_INDEX, t, strlen(t), sig, 65), "nor for another text, nor as a signature of another length");
      setenv("HASSMIC_PLUGINS_PUB", "/nonexistent", 1);
      ok(!pi_signed(PI_SIGN_INDEX, t, strlen(t), sig, 64), "no key to be had: nothing is taken as signed");
      setenv("HASSMIC_PLUGINS_PUB", path, 1); }

    /* ---- an index as the tool writes it */
    char *good = index_of(3, 2, REQ, "\"wake\": true, \"files\": 2");
    ok(!pi_index_parse(good, strlen(good), &ix, err, sizeof err), "an index as tools/index.py writes it");
    ok(ix.serial == 1795200000 && ix.n == 3 && !ix.dropped && !strcmp(ix.p[0].id, "microwakeword") && ix.p[0].tier == PS_OFFICIAL && ix.p[1].tier == PS_COMMUNITY
       && !strcmp(ix.p[0].description, "The open wake word engine, with \xc3\xa9 and \"quotes\".") && ix.p[0].author_key[0] == 0x01 && ix.p[0].n == 2,
       "its serial, its plugins, their texts unescaped, their keys and tiers");
    ok(ix.p[0].v[1].version == 2 && ix.p[0].v[1].req[PM_SCOPE_CORE] == 1 && ix.p[0].v[1].req[PM_SCOPE_WAKE] == 1 && ix.p[0].v[1].req[PM_SCOPE_FILES] == 1 && !ix.p[0].v[1].req[PM_SCOPE_BUTTON] && !ix.p[0].v[1].req_unknown && ix.p[0].v[1].size == 1002 && ix.p[0].v[1].caps == (PM_CAPBIT(PM_CAP_WAKE) | PM_CAPBIT(PM_CAP_FILES)) && !ix.p[0].v[1].caps_unknown
       && ix.p[0].v[1].manifest[1] == 0x23 && ix.p[0].v[1].sha256[31] == 0xef && !strcmp(ix.p[0].v[1].version_name, "2.0"), "each version: number, what it requires, size, capabilities, digests");
    pi_index_free(&ix);
    free(good);
    good = index_of(1, 1, REQ, "\"wake\": true, \"teleport\": 1");
    ok(!pi_index_parse(good, strlen(good), &ix, err, sizeof err) && ix.p[0].v[0].caps == PM_CAPBIT(PM_CAP_WAKE) && ix.p[0].v[0].caps_unknown, "a capability this build does not know: said, so the version is not one it runs");
    pi_index_free(&ix); free(good);
    /* what a version requires (sdk/PROTOCOL.md "Versions"): a scope this build does not know is a later hassmic's,
     * said, never misread; a version is kept as written, whatever it is (one past what a byte holds was kept as 255,
     * which looked met on a build at 255: finding 6 of a review of 4e0a202) */
    good = index_of(1, 1, "\"core\": 1, \"wake\": 300, \"telepathy\": 2", "\"wake\": true");
    ok(!pi_index_parse(good, strlen(good), &ix, err, sizeof err) && ix.p[0].v[0].req[PM_SCOPE_CORE] == 1 && ix.p[0].v[0].req[PM_SCOPE_WAKE] == 300 && ix.p[0].v[0].req_unknown,
       "requires: a scope this build does not know said (req_unknown), wake 300 kept as 300");
    pi_index_free(&ix); free(good);
    good = index_of(1, 1, "\"core\": 1, \"wake\": 65535", "\"wake\": true");
    ok(!pi_index_parse(good, strlen(good), &ix, err, sizeof err) && ix.p[0].v[0].req[PM_SCOPE_WAKE] == 65535 && sizeof ix.p[0].v[0].req[0] * 8 >= 16,
       "requires: wake 65535, the bound, kept as it is (a u16: no version is cut to fit)");
    pi_index_free(&ix); free(good);
    /* a community version's "broad" (the index's flags, docs/plugins.md 5.3): read for the browse page; a name this
     * build does not know left out; anything but a list of names refuses the list, as any field of the wrong kind */
    good = index_of(2, 1, REQ, "\"net.internet\": [], \"call\": true, \"teleport\": 1");
    { char *b = with(good, "\"commit\"", "\"broad\": [\"net.internet\", \"call\", \"teleport\"], \"commit\""), *b2;
      ok(!pi_index_parse(b, strlen(b), &ix, err, sizeof err) && ix.p[0].v[0].broad == (PM_CAPBIT(PM_CAP_NET_INTERNET) | PM_CAPBIT(PM_CAP_CALL)) && !ix.p[1].v[0].broad,
         "a version's broad capabilities: read by name (one this build does not know left out); a version without: none");
      pi_index_free(&ix);
      static const char *const wrong[] = { "\"broad\": \"call\"", "\"broad\": [1]", "\"broad\": [\"call\", null]", "\"broad\": {\"call\": true}",
                                           "\"broad\": [\"a-name-of-thirty-two-characters-\"]" };
      for (size_t i = 0; i < sizeof wrong / sizeof wrong[0]; i++) {
          char to[200]; snprintf(to, sizeof to, "%s, \"commit\"", wrong[i]);
          b2 = with(good, "\"commit\"", to);
          ok(!parses(b2, err, sizeof err) && strstr(err, "broad capabilities"), "broad not a list of names: the whole list refused, said");
          free(b2);
      }
      { char many[1200] = "\"broad\": ["; size_t mn = strlen(many);
        for (int i = 0; i <= PI_BROAD_MAX; i++) mn += (size_t)snprintf(many + mn, sizeof many - mn, "%s\"call\"", i ? ", " : "");
        snprintf(many + mn, sizeof many - mn, "], \"commit\"");
        b2 = with(good, "\"commit\"", many);
        ok(!parses(b2, err, sizeof err) && strstr(err, "broad capabilities"), "more names than PI_BROAD_MAX: refused");
        free(b2); }
      free(b); }
    free(good);
    good = index_of(1, 20, REQ, "\"wake\": true");
    ok(!pi_index_parse(good, strlen(good), &ix, err, sizeof err) && ix.p[0].n == PI_VERSIONS && ix.p[0].v[0].version == 20 - PI_VERSIONS + 1 && ix.p[0].v[PI_VERSIONS - 1].version == 20 && ix.dropped == 20 - PI_VERSIONS,
       "more versions than kept: the highest, and how many were left out is said");
    pi_index_free(&ix); free(good);
    { /* plugins past PI_PLUGINS_MAX: the rest left out and counted, never silently */
      size_t bc = 64 + (size_t)(PI_PLUGINS_MAX + 5) * 260, bn; char *big = malloc(bc);
      bn = (size_t)snprintf(big, bc, "{\"index\": 1, \"serial\": 5, \"revoked_serial\": 0, \"plugins\": [");
      for (int i = 0; i < PI_PLUGINS_MAX + 5; i++)
          bn += (size_t)snprintf(big + bn, bc - bn, "%s{\"id\": \"p%05d\", \"name\": \"n\", \"description\": \"d\", \"author\": \"a\", \"author_key\": \"" HEX64 "\", \"tier\": \"official\", \"source\": \"s\", \"versions\": []}", i ? "," : "", i);
      snprintf(big + bn, bc - bn, "]}");
      int r = pi_index_parse(big, strlen(big), &ix, err, sizeof err);
      ok(strlen(big) <= PI_INDEX_MAX && !r && ix.n == PI_PLUGINS_MAX && ix.dropped == 5, "more plugins than kept: as many as are kept, the rest counted");
      if (!r) pi_index_free(&ix);
      free(big); }
    { char *huge = malloc(PI_INDEX_MAX + 2); memset(huge, ' ', PI_INDEX_MAX + 1); huge[0] = '{'; huge[PI_INDEX_MAX] = '}'; huge[PI_INDEX_MAX + 1] = 0;
      ok(!parses(huge, err, sizeof err), "a list over 1 MB: refused"); free(huge); }

    /* ---- wrong in one way each: the whole list is refused, and why is said */
    good = index_of(2, 2, REQ, "\"wake\": true");
    ok(parses(good, err, sizeof err), "(the list the changes start from)");
    static const char *const changes[][3] = {
        { "\"index\": 1", "\"index\": 2", "form" }, { "\"index\": 1", "\"index\": \"1\"", "form" }, { "\"serial\": 1795200000", "\"serial\": -1", "serial" },
        { "\"serial\": 1795200000", "\"serial\": 1.5", "serial" }, { "\"serial\": 1795200000", "\"serial\": 1795200000000000", "serial" }, { "\"serial\": 1795200000", "\"serial\": 01795200000", "serial" },
        { "\"plugins\": [", "\"plugins\": {\"a\": 1}, \"p\": [", "no list of plugins" }, { "\"id\": \"microwakeword\"", "\"id\": \"Micro\"", "id" }, { "\"id\": \"microwakeword\"", "\"id\": \"abc--d\"", "id" },
        { "\"id\": \"microwakeword\"", "\"id\": 7", "id" }, { "\"tier\": \"official\"", "\"tier\": \"sideload\"", "tier" }, { "\"tier\": \"official\"", "\"tier\": \"Official\"", "tier" },
        { "\"author_key\": \"0123", "\"author_key\": \"0A23", "author key" }, { "\"author_key\": \"0123", "\"author_key\": \"23", "author key" },
        { "\"name\": \"microWakeWord\"", "\"name\": \"micro\\nWakeWord\"", "name" }, { "\"name\": \"microWakeWord\"", "\"name\": \"micro\\u0007\"", "name" },
        { "\"name\": \"microWakeWord\"", "\"name\": \"0123456789012345678901234567890123456789012345678\"", "name" }, { "\"name\": \"microWakeWord\"", "\"name\": null", "name" },
        { "\"versions\": [", "\"versions\": {\"x\": 1}, \"v\": [", "versions" }, { "\"version\": 1,", "\"version\": 0,", "version" }, { "\"version\": 1,", "\"version\": \"1\",", "version" },
        { "\"version\": 2,", "\"version\": 1,", "order" }, { "\"requires\": {\"core\": 1", "\"requires\": {\"core\": 0", "requires" },
        { "\"requires\": {\"core\": 1", "\"requires\": {\"core\": 1.0", "requires" }, { "\"requires\": {\"core\": 1", "\"requires\": {\"core\": \"1\"", "requires" },
        { "\"requires\": {\"core\": 1", "\"requires\": {\"core\": 65536", "requires" }, { "\"requires\": {\"core\": 1, ", "\"requires\": {", "requires (no core)" },
        { "\"requires\": {", "\"requires\": [], \"r\": {", "requires" }, { "\"requires\": {", "\"r\": {", "requires" }, { "\"requires\": {\"core\": 1", "\"requires\": {\"Core\": 1, \"core\": 1", "requires" },
        { "\"requires\": {\"core\": 1", "\"requires\": {\"aaaaaaaaaaaaaaaaaaaaaaaaa\": 1, \"core\": 1", "requires" }, { "\"size\": 1001", "\"size\": 0", "size" },
        { "\"size\": 1001", "\"size\": \"1001\"", "size" }, { "\"manifest\": \"0123", "\"manifest\": \"x123", "digest" }, { "\"sha256\": \"0123", "\"sha256\": \"012", "SHA-256" },
        { "\"capabilities\": {", "\"capabilities\": [\"wake\"], \"c\": {", "capabilities" }, { "\"version_name\": \"1.0\"", "\"version_name\": \"1.0 is a name longer than 24\"", "version_name" },
        { "\"id\": \"plug0001\"", "\"id\": \"microwakeword\"", "twice" },
        { "\"revoked_serial\": 0", "\"revoked\": 0", "revoked_serial" }, { "\"revoked_serial\": 0", "\"revoked_serial\": -1", "revoked_serial" },
        { "\"description\": \"The", "\"description\": \"" "0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789"
          "0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789" "0123456789012345678901234567890123456789" "The", "description" },
    };
    for (size_t i = 0; i < sizeof changes / sizeof changes[0]; i++) {
        char *t = with(good, changes[i][0], changes[i][1]), what[200];
        snprintf(what, sizeof what, "%s -> %s: refused%s%s", changes[i][0], changes[i][1], changes[i][2][0] ? ", said: " : "", changes[i][2]);
        ok(strcmp(t, good) && !parses(t, err, sizeof err) && strstr(err, changes[i][2]), what);
        if (strstr(err, changes[i][2]) == NULL) printf("     said: %s\n", err);
        free(t);
    }
    ok(!parses("", err, sizeof err) && !parses("[]", err, sizeof err) && !parses(" {\"index\": 1, \"serial\": 1, \"revoked_serial\": 0, \"plugins\": []}", err, sizeof err), "no object, or one after white space: refused");
    { char t[] = "{\"index\": 1, \"serial\": 1, \"revoked_serial\": 3, \"plugins\": []}\0more"; struct pi_index e;
      ok(pi_index_parse(t, sizeof t - 1, &e, err, sizeof err) == -1, "a NUL inside: refused");
      ok(!pi_index_parse(t, strlen(t), &e, err, sizeof err) && e.n == 0 && e.serial == 1 && e.revoked_serial == 3, "an empty list: an index all the same, and the revocation list it was made with"); pi_index_free(&e); }
    { char *t = with(good, "\"models\": [\"*\"]", "\"models\": [\"*\"], \"later\": {\"field\": [1, 2]}");
      ok(parses(t, err, sizeof err), "a field this build does not use: left alone (the index can say more to later builds)"); free(t); }

    /* ---- the revocation list */
    static const char *const R = "{\n \"revoked\": 1,\n \"serial\": 7,\n \"entries\": [{\"id\": \"someplug\", \"upto\": 3, \"reason\": \"sends what it hears\"}, {\"id\": \"abc-\", \"reason\": \"gone\"},"
                                 " {\"author_key\": \"" HEX64 "\", \"reason\": \"the key was taken\"}]\n}\n";
    ok(!pi_revoked_parse(R, strlen(R), &rv, err, sizeof err) && rv.serial == 7 && rv.n == 3 && rv.r[0].upto == 3 && !rv.r[1].upto && !strcmp(rv.r[1].id, "abc-") && rv.r[2].by_key && rv.r[2].key[0] == 1
       && !strcmp(rv.r[0].reason, "sends what it hears"), "a revocation list: by id up to a version, by id whole (an id of the old form too), by author key");
    pi_revoked_free(&rv);
    static const char *const rbad[][2] = { { "\"revoked\": 1", "\"revoked\": 2" }, { "\"serial\": 7", "\"serial\": \"7\"" }, { ", \"reason\": \"gone\"", "" },
        { "\"upto\": 3", "\"upto\": 0" }, { "\"id\": \"someplug\"", "\"id\": \"-x\"" }, { "\"author_key\": \"0123", "\"author_key\": \"23" },
        { "{\"author_key\"", "{\"id\": \"x1y\", \"author_key\"" }, { "\"entries\": [", "\"entries\": {\"a\": 1}, \"e\": [" } };
    for (size_t i = 0; i < sizeof rbad / sizeof rbad[0]; i++) {
        char *t = with(R, rbad[i][0], rbad[i][1]), what[200];
        snprintf(what, sizeof what, "a revocation list with %s -> %s: refused", rbad[i][0], rbad[i][1]);
        ok(strcmp(t, R) && pi_revoked_parse(t, strlen(t), &rv, err, sizeof err) == -1, what);
        free(t);
    }

    /* ---- past a bound: the whole list refused, and said (it took the first 2048 and dropped the rest unseen: fail
     * open); at it, all of it */
    { size_t bc = 64 + (size_t)(PI_REVOCATIONS + 1) * 48, bn; char *big = malloc(bc);
      for (int extra = 0; extra < 2; extra++) {
          int r;
          bn = (size_t)snprintf(big, bc, "{\"revoked\": 1, \"serial\": 9, \"entries\": [");
          for (int i = 0; i < PI_REVOCATIONS + extra; i++) bn += (size_t)snprintf(big + bn, bc - bn, "%s{\"id\": \"p%05d\", \"reason\": \"r\"}", i ? "," : "", i);
          snprintf(big + bn, bc - bn, "]}");
          r = pi_revoked_parse(big, strlen(big), &rv, err, sizeof err);
          if (!extra) ok(!r && rv.n == PI_REVOCATIONS && !strcmp(rv.r[PI_REVOCATIONS - 1].id, "p02047"), "a revocation list of PI_REVOCATIONS entries: every one taken");
          else ok(r == -1 && strstr(err, "refused whole") && strstr(err, "2049"), "one entry more: the whole list refused, and said why (never the first ones taken and the rest dropped)");
          if (!r) pi_revoked_free(&rv);
      }
      free(big);
      big = malloc(PI_REVOKED_MAX + 64); bn = (size_t)snprintf(big, 64, "{\"revoked\": 1, \"serial\": 9, \"entries\": []");
      memset(big + bn, ' ', PI_REVOKED_MAX + 2 - bn); big[PI_REVOKED_MAX + 2] = '}'; big[PI_REVOKED_MAX + 3] = 0;
      ok(pi_revoked_parse(big, strlen(big), &rv, err, sizeof err) == -1 && strstr(err, "512 kB"), "a revocation list over 512 kB: refused whole, said");
      free(big); }

    /* ---- the one kept: taken only signed and newer, applied by id and version or by key */
    { uint8_t msg[2000]; size_t hl = strlen(PI_SIGN_REVOKED), rl = strlen(R);
      memcpy(msg, PI_SIGN_REVOKED, hl); memcpy(msg + hl, R, rl); crypto_eddsa_sign(sig, sk, msg, hl + rl);
      ok(pi_revoked_serial() == -1, "no list kept yet");
      ok(pi_revoked_take(R, rl, sig, 64, PI_SITE, err, sizeof err) == 1 && pi_revoked_serial() == 7, "a signed list: taken");
      ok(pi_revoked_take(R, rl, sig, 64, PI_SITE, err, sizeof err) == 0, "the same again: nothing new");
      ok(pi_withdrawn("someplug", 3, NULL, err, sizeof err) && !strcmp(err, "sends what it hears") && !pi_withdrawn("someplug", 4, NULL, NULL, 0)
         && pi_withdrawn("abc-", 99, NULL, NULL, 0) && !pi_withdrawn("other", 1, NULL, NULL, 0), "by id: up to its version, or every one");
      { uint8_t k[32]; for (int i = 0; i < 32; i++) k[i] = (uint8_t)(((i * 2) % 16) << 4 | ((i * 2 + 1) % 16));
        ok(pi_withdrawn("anyplug", 1, k, err, sizeof err) && !strcmp(err, "the key was taken"), "by author key: whatever the plugin"); k[0] ^= 1;
        ok(!pi_withdrawn("anyplug", 1, k, NULL, 0), "another key: not"); }
      pi_forget();
      ok(pi_revoked_serial() == 7 && pi_withdrawn("someplug", 1, NULL, NULL, 0), "kept on the flash: the next start has it");
      { char kp[400], *f = NULL; size_t fn = pi_revoked_file(&f); struct stat st;
        snprintf(kp, sizeof kp, "%s/plugins/.revoked.json", dir);
        ok(fn == 129 + rl && f && f[128] == '\n' && !memcmp(f + 129, R, rl) && stat(kp, &st), "one file, list and signature together: handed on as the signature's hex line and the list as it came");
        free(f);
        /* a power cut in the middle of a write leaves the old file or the new (statefile.c); a file cut by anything
         * else is no list, and none is taken from it */
        snprintf(kp, sizeof kp, "%s/plugins/.revoked", dir);
        { FILE *k = fopen(kp, "rb"); char buf[4000]; size_t kn = fread(buf, 1, sizeof buf, k); fclose(k);
          k = fopen(kp, "wb"); fwrite(buf, 1, kn - 20, k); fclose(k); pi_forget();
          ok(pi_revoked_serial() == -1 && !pi_withdrawn("someplug", 1, NULL, NULL, 0), "the file cut short: no list (taken at the next look)");
          k = fopen(kp, "wb"); fwrite(buf, 1, kn, k); fclose(k); pi_forget();
          ok(pi_revoked_serial() == 7, "(whole again: the list)"); } }

      /* the least serials of this build (5.5): a list under them is refused; one kept from before still withdraws
       * what it names (it was dropped at the start: an update that raised the least ran what it withdrew, for good
       * where the site is out of reach; a review, 2026-11-21), is not current, and any list from the least on
       * replaces it */
      setenv("HASSMIC_PLUGINS_LEAST", "0:8", 1); pi_forget();
      ok(pi_least_revoked() == 8 && pi_least_index() == 0 && pi_revoked_serial() == 7 && !pi_revoked_current() && pi_withdrawn("someplug", 1, NULL, NULL, 0),
         "a list kept under this build's least revocation serial: what it withdraws stays withdrawn, and it is not current");
      ok(pi_revoked_take(R, rl, sig, 64, PI_SITE, err, sizeof err) == -1 && strstr(err, "than this hassmic takes"), "and the same list handed again: refused, and said why");
      ok(take(8, "eight", err, sizeof err) == 1 && pi_revoked_current() && pi_withdrawn("eight", 1, NULL, NULL, 0), "a list of the least: taken over it");
      { struct stat st; char kp[400]; snprintf(kp, sizeof kp, "%s/plugins/.revoked", dir); unlink(kp); (void)st; }
      { FILE *k; char kp[400]; uint8_t s7[64]; size_t hl7 = strlen(PI_SIGN_REVOKED); uint8_t m7[2000];
        memcpy(m7, PI_SIGN_REVOKED, hl7); memcpy(m7 + hl7, R, rl); crypto_eddsa_sign(s7, sk, m7, hl7 + rl);
        snprintf(kp, sizeof kp, "%s/plugins/.revoked", dir); k = fopen(kp, "wb"); fprintf(k, "floor 7\n"); for (int i = 0; i < 64; i++) fprintf(k, "%02x", s7[i]); fprintf(k, "\n%s", R); fclose(k); pi_forget(); }
      setenv("HASSMIC_PLUGINS_LEAST", "1795200001:0", 1);
      ok(pi_least_index() == 1795200001 && pi_least_revoked() == 0, "(the index's least: HASSMIC_PLUGINS_LEAST's first number)");
      unsetenv("HASSMIC_PLUGINS_LEAST"); pi_forget();
      ok(pi_revoked_serial() == 7 && pi_least_revoked() == 0 && pi_revoked_current(), "(the build's own least: the list again)"); }

    /* ---- serials from the future (both are times).  The clock set: more than a day ahead refused.  Not set: taken
     * and used; the floor line kept beside it is no further than the build's time + a day, kept as it was made, and
     * it counts only once the clock is set and says the kept serial is from the future */
    { long long now = (long long)time(NULL), built = now - 10 * 86400; char bs[40]; int raise;
      pi_clock(fake_clock); fake_synced = 1;
      snprintf(bs, sizeof bs, "%lld", built); setenv("HASSMIC_PLUGINS_BUILT", bs, 1);
      ok(!pi_serial_ok(now + 3600, err, sizeof err) && pi_serial_ok(now + 2 * 86400, err, sizeof err) == -1 && strstr(err, "more than a day"), "the clock set: a serial an hour ahead is fine, two days ahead is not");
      ok(pi_floor(now + 3600, 0) == now + 3600 && pi_floor(5, 100) == 100, "the floor line: the serial itself, never under the least");
      ok(take(now + 2 * 86400, "far", err, sizeof err) == -1 && strstr(err, "more than a day") && pi_revoked_serial() == 7, "a revocation list two days ahead, the clock set: refused");
      ok(pi_held(now - 5, built, 0, 1, &raise) == now - 5 && raise && pi_held(now + 3 * 86400, built, 0, 1, &raise) == built && !raise, "pi_held, the clock set: the serial (and the line raised to it), or the line where the serial is from the future");
      fake_synced = 0;
      ok(pi_held(now - 5, built, 0, 1, &raise) == now - 5 && !raise && pi_held(now - 5, built, 0, 0, &raise) == built && !raise, "the clock not set: the serial for the revocation list, the line for the index");
      ok(!pi_serial_ok(now + 400LL * 86400, err, sizeof err) && pi_floor(now + 400LL * 86400, 0) == built + PI_AHEAD_S && pi_floor(built - 5, 0) == built - 5,
         "the clock not set: any serial can be taken, its floor line no further than the build's time + a day");
      ok(take(now + 400LL * 86400, "far", err, sizeof err) == 1 && pi_withdrawn("far", 1, NULL, NULL, 0) && floor_line(dir) == built + PI_AHEAD_S, "a list from a year ahead, the clock not set: taken and used (what it withdraws is withdrawn), its line the build's time + a day");
      ok(take(now, "honest", err, sizeof err) == PI_OLDER && pi_withdrawn("far", 1, NULL, NULL, 0),
         "the honest list of today, under the far one, the clock never set: refused (the kept list is the floor; a far serial only the index key can sign)");
      fake_synced = 1;
      ok(take(now, "honest", err, sizeof err) == 1 && pi_revoked_serial() == now && pi_withdrawn("honest", 1, NULL, NULL, 0) && !pi_withdrawn("far", 1, NULL, NULL, 0),
         "the clock set: the far one is from the future by it, and the honest list of today is taken from its line on (no freeze)");
      ok(take(now - 3 * 86400, "older", err, sizeof err) == PI_OLDER && pi_revoked_serial() == now, "an older list than the one kept, the clock set: refused");

      /* the review's roll-back (2026-11-21): a list taken while the clock was not set left the line at the build's
       * time + a day; with the clock set later an older signed list from between the line and the kept serial was
       * taken over it.  Now the kept serial is the floor wherever it can be believed, and the line is raised to it */
      { long long s1 = now - 86400, s0 = now - 5 * 86400;
        { char kp[400]; snprintf(kp, sizeof kp, "%s/plugins/.revoked", dir); unlink(kp); } pi_forget();
        setenv("HASSMIC_PLUGINS_BUILT", "100", 1);           /* (the build: long ago, so that the line is far under both) */
        fake_synced = 0;
        ok(take(s1, "newer", err, sizeof err) == 1 && floor_line(dir) == 100 + PI_AHEAD_S, "the clock not set: a list of yesterday taken, its line the build's time + a day");
        ok(take(s0, "older", err, sizeof err) == PI_OLDER && pi_withdrawn("newer", 1, NULL, NULL, 0), "the clock not set: an older one refused (it was taken from the line on)");
        pi_forget(); fake_synced = 1;
        ok(take(s0, "older", err, sizeof err) == PI_OLDER && pi_revoked_serial() == s1 && pi_withdrawn("newer", 1, NULL, NULL, 0) && floor_line(dir) == s1,
           "a restart, the clock set: the same older list refused, and the line raised to the kept serial on the flash");
        pi_forget(); fake_synced = 1; put_file(dir, ".revoked", "");    /* (the line raised at the first question, before any take) */
        { char kp[400]; snprintf(kp, sizeof kp, "%s/plugins/.revoked", dir); unlink(kp); }
        fake_synced = 0; ok(take(s1, "newer", err, sizeof err) == 1 && floor_line(dir) == 100 + PI_AHEAD_S, "(again the list of yesterday, the clock not set)");
        pi_forget(); fake_synced = 1;
        ok(take(s1, "newer", err, sizeof err) == 0 && floor_line(dir) == s1, "a restart, the clock set, the same list again (its r=0 path): the line raised to it");
        { char kp[400]; snprintf(kp, sizeof kp, "%s/plugins/.revoked", dir); unlink(kp); }
        fake_synced = 0; pi_forget(); ok(take(s1, "newer", err, sizeof err) == 1, "(once more, the clock not set)");
        pi_forget(); fake_synced = 1; pi_revoked_upkeep();
        ok(floor_line(dir) == s1, "and with nothing handed over at all: the line raised by the daily thread's upkeep (the first question)");
        fake_synced = 0; pi_forget();
        ok(take(s0, "older", err, sizeof err) == PI_OLDER, "then the clock not set again: still refused");
        /* the line never falls (a review, 2026-11-21): vouched for at s1, a newer list taken without the clock (three
         * days ahead of the clock that is set later: Home Assistant's, behind) left it at the build's time + a day,
         * and then an older list than s1 was taken from that line on */
        { long long s2 = now + 3 * 86400, sold = now - 20 * 86400;
          fake_synced = 0;
          ok(take(s2, "ahead", err, sizeof err) == 1 && floor_line(dir) == s1, "the clock not set: a newer list taken, the line kept where the clock vouched for it (it fell to the build's time + a day)");
          pi_forget(); fake_synced = 1;
          ok(take(sold, "older", err, sizeof err) == PI_OLDER && pi_revoked_serial() == s2 && pi_withdrawn("ahead", 1, NULL, NULL, 0),
             "then the clock set behind the kept serial: a list older than the one vouched for refused (it was taken)");
          ok(take(now - 3600, "between", err, sizeof err) == 1 && floor_line(dir) == now - 3600, "(one from the line on, the kept serial from the future by this clock: taken, as before)"); } }
      fake_synced = 1; unsetenv("HASSMIC_PLUGINS_BUILT"); }

    /* ---- the word (5.6's warning): when the list was last known to be the newest */
    { long long now = (long long)time(NULL); ino_t i1, i2; char kp[400];
      snprintf(kp, sizeof kp, "%s/plugins/.revoked", dir); unlink(kp); snprintf(kp, sizeof kp, "%s/plugins/.revoked-at", dir); unlink(kp); pi_forget();
      fake_synced = 1;
      ok(take(now - 100, "word", err, sizeof err) == 1 && pi_revoked_age() >= 0 && pi_revoked_age() < 5, "a list from the site: word, now");
      i1 = inode(dir, ".revoked-at");
      pi_test_skew(600);
      ok(take_from(now - 100, "word", PI_MEMBER, err, sizeof err) == 0 && pi_revoked_age() >= 600, "the same list from a member: no word (a member can hand it over as often as it likes)");
      ok(take(now - 100, "word", err, sizeof err) == 0 && pi_revoked_age() < 5, "the same from the site: word");
      i2 = inode(dir, ".revoked-at");
      ok(i1 && i1 == i2, ".revoked-at written at most once an hour: the second word not yet");
      pi_test_skew(3600); pi_revoked_upkeep();
      ok(inode(dir, ".revoked-at") != i2, "an hour later the upkeep writes it");
      pi_test_skew(30);
      ok(take_from(now - 50, "word2", PI_MEMBER, err, sizeof err) == 1 && pi_revoked_age() >= 50 && pi_revoked_age() < 60, "a newer list from a member: word, as of its serial");
      ok(take_from(now - 5 * 86400, "word3", PI_SITE, err, sizeof err) == PI_OLDER, "(an older one from the site: refused)");
      /* a member's archive (a review, 2026-11-21): each list newer than the last, all old; dated "now", one a week
       * kept the warning away */
      { char kp2[400]; snprintf(kp2, sizeof kp2, "%s/plugins/.revoked", dir); unlink(kp2); pi_forget(); }
      ok(take_from(now - 30 * 86400, "arch1", PI_MEMBER, err, sizeof err) == 1 && pi_revoked_age() >= 30 * 86400, "a member's newer but month-old list: word a month old, the warning stays");
      ok(take_from(now - 20 * 86400, "arch2", PI_MEMBER, err, sizeof err) == 1 && pi_revoked_age() >= 20 * 86400, "and the next from its archive: still old");
      { char bs2[40]; snprintf(bs2, sizeof bs2, "%lld", now - 10 * 86400); setenv("HASSMIC_PLUGINS_BUILT", bs2, 1); }
      fake_synced = 0;
      ok(take_from(now - 15 * 86400, "arch3", PI_MEMBER, err, sizeof err) == 1 && pi_revoked_age() >= 5 * 86400 && pi_revoked_age() < 5 * 86400 + 60,
         "the clock not set: as old as the build is younger than the list (all it can know)");
      ok(take_from(now - 86400, "arch4", PI_MEMBER, err, sizeof err) == 1 && pi_revoked_age() < 5, "(a list made after the build: nothing known, word now)");
      unsetenv("HASSMIC_PLUGINS_BUILT"); fake_synced = 1;
      /* over a restart: the age the file says, counted on */
      put_file(dir, ".revoked-at", "word 0 700000\n"); pi_forget(); fake_synced = 0;
      ok(pi_revoked_age() >= 700000, "the clock not set, after a restart: the age kept, counted on (it was -1: no warning)");
      put_file(dir, ".revoked-at", "word 0 100\n"); pi_forget(); fake_synced = 0;
      ok(pi_revoked_age() >= 100 && (pi_test_skew(7200), pi_revoked_age()) >= 7300, "and it grows with this run's own time");
      { long long a0 = pi_revoked_age(); pi_test_skew(PI_AT_EVERY_S); pi_revoked_upkeep(); pi_forget();
        ok(pi_revoked_age() >= a0 + PI_AT_EVERY_S, "written again within the hour while it grows: a restart keeps it"); }
      { char t[80]; snprintf(t, sizeof t, "word %lld 691200\n", now + 3 * 86400); put_file(dir, ".revoked-at", t); }
      pi_forget(); fake_synced = 1;
      ok(pi_revoked_age() >= 691200, "a clock behind the word's time: the age counted, not \"fresh\" (it was clipped to 0)");
      { char t[80]; snprintf(t, sizeof t, "word %lld 10\n", now - 8 * 86400); put_file(dir, ".revoked-at", t); }
      pi_forget(); fake_synced = 1;
      ok(pi_revoked_age() >= 8 * 86400, "the clock set: the clock's age where it says more than the count (the time off)");
      { char t[80]; snprintf(t, sizeof t, "%lld\n", now - 8 * 86400); put_file(dir, ".revoked-at", t); }
      pi_forget(); fake_synced = 1;
      ok(pi_revoked_age() >= 8 * 86400, "the older form (the wall time alone) still read");
      { char t[80]; snprintf(t, sizeof t, "%lld\n", now - 3600); put_file(dir, ".revoked-at", t); }
      pi_forget(); fake_synced = 0;
      ok(pi_revoked_age() == -1, "the older form, the clock not set: no word known (it read as an age of 0, the week's count begun anew at every start)");
      pi_forget(); fake_synced = 1;
      ok(pi_revoked_age() >= 3600 && pi_revoked_age() < 3660, "(and with the clock set: its age by the clock)");
      { char t[80]; snprintf(t, sizeof t, "%lld\n", now + 3 * 86400); put_file(dir, ".revoked-at", t); }
      pi_forget(); fake_synced = 1;
      ok(pi_revoked_age() == -1, "the older form ahead of the clock: no word known (it was clipped to an age of 0, \"fresh\": a review of a6c5a00)");
      unlink(kp); pi_forget();
      ok(pi_revoked_age() == -1 && pi_revoked_serial() >= 0, "a list and no word known: -1 (the page's warning says so)"); }

    /* ---- what a look at lists at the bounds holds at its peak (the parsed index's texts were fixed arrays of their
     * bounds: 2.6 kB a plugin, 10 MB for 4096; ~30 MB at a look's peak, on a Dot 2 with 512 MB in all).  Two lists
     * of 1 MB as the tool would write them: 4096 plugins (as many as it allows), and the longest texts with 16
     * versions each.  The text, the signature's copy of it, the tokens and the parsed form together */
    { size_t worst = 0; const char *what[2] = { "4096 plugins (no versions: 1 MB holds no more with any)", "fields at their bounds, 16 versions each" };
      for (int kind = 0; kind < 2; kind++) {
          size_t bc = PI_INDEX_MAX + 4096, bn; char *big = malloc(bc), pad[260]; int np = 0;
          memset(pad, 'x', sizeof pad); pad[PI_DESCRIPTION_MAX] = 0;
          bn = (size_t)snprintf(big, bc, "{\"index\": 1, \"serial\": 5, \"revoked_serial\": 0, \"plugins\": [");
          for (;;) {
              char one[16000]; size_t on = 0;
              on += (size_t)snprintf(one + on, sizeof one - on, "%s{\"id\": \"p%05d\", \"name\": \"%.*s\", \"description\": \"%.*s\", \"author\": \"%.*s\", \"author_key\": \"" HEX64 "\", \"tier\": \"official\", \"source\": \"%.*s\", \"versions\": [",
                                     np ? "," : "", np, kind ? PI_NAME_MAX : 1, pad, kind ? PI_DESCRIPTION_MAX : 1, pad, kind ? PI_AUTHOR_MAX : 1, pad, kind ? PI_SOURCE_MAX : 1, pad);
              for (int v = 1; v <= (kind ? PI_VERSIONS : 0); v++)
              { on += (size_t)snprintf(one + on, sizeof one - on, VER, v, v, REQ, "\"wake\": true, \"files\": 2", v, 1000 + v);
                if (v < PI_VERSIONS) one[on++] = ','; }
              on += (size_t)snprintf(one + on, sizeof one - on, "]}");
              if (bn + on + 4 > PI_INDEX_MAX || np == PI_PLUGINS_MAX) break;
              memcpy(big + bn, one, on); bn += on; np++;
          }
          bn += (size_t)snprintf(big + bn, bc - bn, "]}");
          { uint8_t s2[64]; size_t base; int r;
            sign(PI_SIGN_INDEX, big, bn, s2);
            base = mem_now; mem_peak = mem_now;
            r = !pi_signed(PI_SIGN_INDEX, big, bn, s2, 64) || pi_index_parse(big, bn, &ix, err, sizeof err);
            size_t peak = mem_peak - base + bn, held = r ? 0 : mem_now - base;
            ok(!r && ix.n == np && !ix.dropped, what[kind]);
            printf("     (%s, %d plugins, %zu kB of JSON: %zu kB at the peak of reading it, the text included; %zu kB kept)\n", what[kind], np, bn >> 10, peak >> 10, held >> 10);
            if (peak > worst) worst = peak;
            if (!r) pi_index_free(&ix); }
          free(big);
      }
      ok(worst < (6u << 20), "a look at a list at the bounds holds less than 6 MB at its peak"); }

    /* ---- a real list cut at every length and changed at random: whatever comes out keeps its bounds */
    srand(1);
    { size_t gl = strlen(good); char *t = malloc(gl + 1); int taken = 0;
      for (size_t cut = 0; cut <= gl; cut += 7) { struct pi_index e; memcpy(t, good, cut); t[cut] = 0; if (!pi_index_parse(t, cut, &e, err, sizeof err)) { sane(&e); pi_index_free(&e); taken++; } }
      for (int round = 0; round < 20000; round++) {
          struct pi_index e; memcpy(t, good, gl + 1);
          for (int k = 0, nk = 1 + rand() % 4; k < nk; k++) t[rand() % gl] = (char)(rand() % 3 ? "0123456789{}[]\",:\\ eu-.a"[rand() % 25] : rand() % 256);
          if (!pi_index_parse(t, gl, &e, err, sizeof err)) { sane(&e); pi_index_free(&e); taken++; }
          struct pi_revoked q; size_t rl = strlen(R); char rt[600]; memcpy(rt, R, rl + 1);
          for (int k = 0, nk = 1 + rand() % 3; k < nk; k++) rt[rand() % rl] = (char)(rand() % 256);
          if (!pi_revoked_parse(rt, rl, &q, err, sizeof err)) { ok(q.n <= 3, "a changed revocation list: no more entries than it had"); pi_revoked_free(&q); }
      }
      printf("     (%d of the changed lists read as lists)\n", taken);
      free(t); }
    free(good);

    { char cmd[300]; snprintf(cmd, sizeof cmd, "rm -rf %s", dir); if (system(cmd)) {} }
    printf("plugin_index: %d checks, %s\n", checks, fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
