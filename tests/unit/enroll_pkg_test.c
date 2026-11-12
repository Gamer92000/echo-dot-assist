/* enroll_pkg.c: a word's name, STOP cut out of Amazon's configuration without disturbing the rest, and the folder made
 * from a set (a stand-in with Amazon's layout: no firmware needed). */
#include "enroll_pkg.h"
#include "json.h"
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void strip(const char *in, const char *want, int removed)
{
    char t[2048]; size_t len = strlen(in); snprintf(t, sizeof t, "%s", in);
    int n = enroll_strip_named(t, &len, sizeof t, "STOP");
    if (n != removed || strcmp(t, want)) { fprintf(stderr, "strip: %s\n got %d %s\nwant %d %s\n", in, n, t, removed, want); exit(1); }
    struct json j = { 0 }; assert(!json_parse(&j, t, len)); json_free(&j);      /* what is left is still JSON */
}

static void put(const char *dir, const char *name, const char *text)
{
    char p[512]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "w"); assert(f); fputs(text, f); fclose(f);
}

static char *slurp(const char *dir, const char *name)
{
    static char b[8192]; char p[512]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "r"); assert(f); size_t n = fread(b, 1, sizeof b - 1, f); b[n] = 0; fclose(f); return b;
}

int main(void)
{
    char key[64], id[80], err[200];
    assert(!enroll_name("Hey Computer!", "en-US", key, sizeof key, id, sizeof id, err, sizeof err));
    assert(!strcmp(key, "HEY_COMPUTER") && !strcmp(id, "hey_computer-en-US"));
    assert(!enroll_name("  nova ", "en-US", key, sizeof key, id, sizeof id, err, sizeof err) && !strcmp(key, "NOVA") && !strcmp(id, "nova-en-US"));
    assert(enroll_name("Alexa", "en-US", key, sizeof key, id, sizeof id, err, sizeof err) < 0);          /* Amazon's */
    assert(enroll_name("stop", "en-US", key, sizeof key, id, sizeof id, err, sizeof err) < 0);
    assert(enroll_name("ab", "en-US", key, sizeof key, id, sizeof id, err, sizeof err) < 0);             /* too short */
    assert(enroll_name("9lives", "en-US", key, sizeof key, id, sizeof id, err, sizeof err) < 0);         /* a letter first */
    assert(enroll_name("caf\xc3\xa9", "en-US", key, sizeof key, id, sizeof id, err, sizeof err) < 0);    /* no accents */
    assert(enroll_name("../../x", "en-US", key, sizeof key, id, sizeof id, err, sizeof err) < 0);                  /* too short once the dots go */
    assert(!enroll_name("../../xyz", "en-US", key, sizeof key, id, sizeof id, err, sizeof err) && !strcmp(id, "xyz-en-US"));   /* no path gets through */
    assert(enroll_name("nova", "../US", key, sizeof key, id, sizeof id, err, sizeof err) < 0);

    strip("{\"s\":[{\"name\":\"ALEXA\",\"a\":1},{\"name\":\"STOP\",\"a\":2}]}", "{\"s\":[{\"name\":\"ALEXA\",\"a\":1}]}", 1);
    strip("{\"s\":[{\"name\":\"STOP\"},{\"name\":\"ALEXA\"}]}", "{\"s\":[{\"name\":\"ALEXA\"}]}", 1);
    strip("{\"s\": [ {\"name\": \"A\"},\n  {\"name\": \"STOP\"},\n  {\"name\": \"B\"} ]}", "{\"s\": [ {\"name\": \"A\"},\n  {\"name\": \"B\"} ]}", 1);
    strip("[{\"name\":\"STOP\"},{\"name\":\"STOP\"},{\"name\":\"A\"}]", "[{\"name\":\"A\"}]", 2);
    strip("[{\"name\":\"A\"},{\"name\":\"STOP\"},{\"name\":\"STOP\"}]", "[{\"name\":\"A\"}]", 2);
    strip("{\"x\":{\"rules\":[{\"name\":\"ALEXA\",\"next\":\"awake\"},{\"name\":\"STOP\",\"next\":\"sleep\"}],\"t\":{\"d\":1}},"
          "\"y\":{\"rules\":[{\"name\":\"STOP\"}]}}",
          "{\"x\":{\"rules\":[{\"name\":\"ALEXA\",\"next\":\"awake\"}],\"t\":{\"d\":1}},\"y\":{\"rules\":[]}}", 2);
    strip("{\"name\":\"STOP\",\"k\":[\"STOP\",{\"n\":\"STOP\"}]}", "{\"name\":\"STOP\",\"k\":[\"STOP\",{\"n\":\"STOP\"}]}", 0);   /* only objects named so, in arrays */
    { char bad[] = "{\"s\":["; size_t l = strlen(bad); assert(enroll_strip_named(bad, &l, sizeof bad, "STOP") < 0); }

    /* a stand-in for Amazon's set: the files that matter, one folder deeper */
    char root[] = "/tmp/enroll_pkg_XXXXXX"; assert(mkdtemp(root));
    char base[300], dest[300], cand[300]; snprintf(base, sizeof base, "%s/base", root); snprintf(dest, sizeof dest, "%s/out", root); snprintf(cand, sizeof cand, "%s/cand.onnx", root);
    mkdir(base, 0755);
    put(base, "kw.cfg.json", "{\"keyword-orchestra\":{\"keyword-spotters\":[{\"name\":\"ALEXA\",\"t\":0.923},{\"name\":\"STOP\",\"t\":0.55}]}}");
    put(base, "op.cfg.json", "{\"awake\":{\"rules\":[{\"name\":\"ALEXA\",\"next\":\"awake\"},{\"name\":\"STOP\",\"next\":\"sleep\"}]},\"sleep\":{\"rules\":[{\"name\":\"ALEXA\",\"next\":\"awake\"}]}}");
    put(base, "watermark.cfg.json", "{\"enabled_keys\":[{\"keyword_name\":\"ALEXA\"}]}");
    put(base, "words.shrunk.txt", "<eps> 0\nALEXA 1\nSTOP 2\n");
    put(base, "pryon.manifest", "{\"checksum\":\"checksum.txt\"}");
    put(base, "checksum.txt", "74a26372ecb79fc6e5954d9f9fff29a3");
    put(base, ENROLL_MODEL, "ORIGINAL-MODEL");
    { char sub[320]; snprintf(sub, sizeof sub, "%s/nttfusionconfig", base); mkdir(sub, 0755); put(sub, "x.json", "{}"); }
    FILE *c = fopen(cand, "w"); fputs("NEW-HEAD", c); fclose(c);

    assert(!enroll_make_set(base, cand, dest, "HEY_COMPUTER", "phrase=Hey Computer\n", err, sizeof err));
    assert(!strcmp(slurp(dest, ENROLL_MODEL), "NEW-HEAD"));                        /* the retrained model */
    assert(!strcmp(slurp(base, ENROLL_MODEL), "ORIGINAL-MODEL"));                  /* Amazon's own set untouched */
    assert(!strcmp(slurp(dest, "words.shrunk.txt"), "<eps> 0\nHEY_COMPUTER 1\nSTOP 2\n"));
    assert(!strcmp(slurp(dest, "kw.cfg.json"), "{\"keyword-orchestra\":{\"keyword-spotters\":[{\"name\":\"HEY_COMPUTER\",\"t\":0.923}]}}"));
    assert(!strcmp(slurp(dest, "op.cfg.json"), "{\"awake\":{\"rules\":[{\"name\":\"HEY_COMPUTER\",\"next\":\"awake\"}]},\"sleep\":{\"rules\":[{\"name\":\"HEY_COMPUTER\",\"next\":\"awake\"}]}}"));
    assert(!strcmp(slurp(dest, "watermark.cfg.json"), "{\"enabled_keys\":[{\"keyword_name\":\"HEY_COMPUTER\"}]}"));
    assert(!strcmp(slurp(dest, "nttfusionconfig/x.json"), "{}"));
    assert(strlen(slurp(dest, "checksum.txt")) == 32 && strcmp(slurp(dest, "checksum.txt"), "74a26372ecb79fc6e5954d9f9fff29a3"));
    assert(!strcmp(slurp(dest, ENROLL_MARK), "phrase=Hey Computer\n"));
    assert(enroll_make_set(base, cand, dest, "X", "n", err, sizeof err) < 0);      /* not over an existing folder */
    assert(!access(dest, F_OK));                                                   /* (and that one is left alone) */

    /* a set that is not Alexa and Stop is refused, and nothing is left behind */
    put(base, "words.shrunk.txt", "<eps> 0\nECHO 1\nSTOP 2\n");
    char dest2[300]; snprintf(dest2, sizeof dest2, "%s/out2", root);
    assert(enroll_make_set(base, cand, dest2, "NOVA", "n", err, sizeof err) < 0 && access(dest2, F_OK));
    char cmd[400]; snprintf(cmd, sizeof cmd, "rm -rf %s", root); assert(!system(cmd));
    printf("enroll_pkg: ok\n");
    return 0;
}
