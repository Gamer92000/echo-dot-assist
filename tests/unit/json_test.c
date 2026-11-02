/* json.c: lookups, walks, escapes (UTF-16 pairs, cut strings), what is refused, and a fuzz run of a Music Assistant
 * answer cut and changed at random (under ASan/UBSan: no read outside the text, no crash) */
#include "../../src/hassmic/json.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *DOC =
    "{\"message_id\":\"7\",\"result\":[{\"item_id\":\"12\",\"provider\":\"library\",\"name\":\"Caf\\u00e9 \\\"Tacvba\\\"\","
    "\"uri\":\"library://album/12\",\"media_type\":\"album\",\"year\":1994,\"favorite\":true,\"version\":null,"
    "\"artists\":[{\"name\":\"Bj\\u00f6rk\"},{\"name\":\"\\ud83c\\udfb5 Notes\"}],"
    "\"metadata\":{\"images\":[{\"type\":\"thumb\",\"path\":\"https://x/y.jpg\",\"provider\":\"spotify\",\"proxy_id\":\"abc\"}]},"
    "\"duration\":2.5e2,\"empty\":{},\"none\":[]},"
    "{\"name\":\"Zwei\",\"raw\":\"\xc3\xa4\xc3\xb6\xc3\xbc\"}]}";

int main(void)
{
    struct json j = { 0 }; char b[64];
    assert(!json_parse(&j, DOC, strlen(DOC)));
    int r = json_get(&j, 0, "result");
    assert(json_type(&j, r) == J_ARR && json_len(&j, r) == 2);
    int a = json_first(&j, r);
    assert(json_type(&j, a) == J_OBJ);
    assert(json_str(&j, json_get(&j, a, "name"), b, sizeof b) && !strcmp(b, "Caf\xc3\xa9 \"Tacvba\""));
    assert(json_int(&j, json_get(&j, a, "year"), 0) == 1994 && json_int(&j, json_get(&j, a, "duration"), 0) == 250);
    assert(json_bool(&j, json_get(&j, a, "favorite"), 0) == 1);
    assert(!json_str(&j, json_get(&j, a, "version"), b, sizeof b) && !b[0] && json_type(&j, json_get(&j, a, "version")) == J_NULL);
    assert(json_get(&j, a, "nope") < 0 && json_streq(&j, json_get(&j, a, "media_type"), "album"));
    int ar = json_get(&j, a, "artists"), n = 0;
    JSON_EACH(&j, ar, k) {
        json_str(&j, json_get(&j, k, "name"), b, sizeof b);
        assert(n == 0 ? !strcmp(b, "Bj\xc3\xb6rk") : !strcmp(b, "\xf0\x9f\x8e\xb5 Notes"));
        n++;
    }
    assert(n == 2);
    int im = json_first(&j, json_get(&j, json_get(&j, a, "metadata"), "images"));
    assert(json_str(&j, json_get(&j, im, "proxy_id"), b, sizeof b) && !strcmp(b, "abc"));
    assert(json_first(&j, json_get(&j, a, "empty")) < 0 && json_len(&j, json_get(&j, a, "none")) == 0);
    int z = json_next(&j, r, a);
    assert(z > a && json_next(&j, r, z) < 0);
    assert(json_str(&j, json_get(&j, z, "raw"), b, sizeof b) && !strcmp(b, "\xc3\xa4\xc3\xb6\xc3\xbc"));
    /* cut at a character, never inside one: 4 bytes for "äö" + the NUL does not fit the third */
    char c[6]; json_str(&j, json_get(&j, z, "raw"), c, sizeof c); assert(!strcmp(c, "\xc3\xa4\xc3\xb6"));
    char e[6]; json_str(&j, json_get(&j, a, "name"), e, sizeof e); assert(!strcmp(e, "Caf\xc3\xa9"));
    json_free(&j);

    /* refused */
    const char *bad[] = { "", "{", "[1,]", "{\"a\" 1}", "{\"a\":1,}", "\"\\x\"", "\"\\u12g4\"", "[tru]", "\"a\nb\"", "[1] 2", "{1:2}" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) assert(json_parse(&j, bad[i], strlen(bad[i])) != 0);
    char deep[300]; memset(deep, '[', 100); memset(deep + 100, ']', 100); deep[200] = 0;
    assert(json_parse(&j, deep, 200) != 0);                       /* deeper than JSON_DEPTH */
    json_free(&j);

    /* escaping, and back */
    char q[64]; assert(json_esc(q, sizeof q, "a\"b\\c\n\x01") > 0 && !strcmp(q, "\"a\\\"b\\\\c\\u000a\\u0001\""));
    assert(!json_parse(&j, q, strlen(q)) && json_str(&j, 0, b, sizeof b) && !strcmp(b, "a\"b\\c  "));
    assert(json_esc(q, 4, "abcdef") < 0);
    json_free(&j);

    /* fuzz: the document cut short and its bytes changed */
    size_t L = strlen(DOC); char *m = malloc(L + 1); unsigned seed = 1;
    for (int it = 0; it < 200000; it++) {
        memcpy(m, DOC, L + 1);
        size_t cut = (seed = seed * 1103515245 + 12345) % (L + 1);
        for (int f = 0; f < 3; f++) { seed = seed * 1103515245 + 12345; m[(seed >> 8) % L] = (char)(seed >> 16); }
        char *e = malloc(cut ? cut : 1); memcpy(e, m, cut);  /* exactly cut bytes: a read past them is ASan's */
        if (!json_parse(&j, e, cut)) {
            for (int t = 0; t < j.n; t++) { json_str(&j, t, b, sizeof b); json_int(&j, t, 0); json_get(&j, t, "name"); json_len(&j, t); }
        }
        json_free(&j); free(e);
    }
    json_free(&j); free(m);

    /* strings of escapes and UTF-8 lead bytes without their sequence, each in exactly its own bytes: a lead byte that
     * took the \ after it made the next \\u read four bytes past the end */
    const char *x = "\"\xc3\\\\u\"";
    char *xs = malloc(strlen(x)); memcpy(xs, x, strlen(x));
    assert(!json_parse(&j, xs, strlen(x)) && json_str(&j, 0, b, sizeof b) && !strcmp(b, "\xc3\\u"));
    json_free(&j); free(xs);
    static const char ab[] = { '\\', 'u', '0', 'a', '"', (char)0xc3, (char)0xe2, (char)0xf0, (char)0x80 };
    for (int it = 0; it < 200000; it++) {
        char s[12]; size_t l = 2 + (seed = seed * 1103515245 + 12345) % 9;
        s[0] = '"'; s[l - 1] = '"';
        for (size_t i = 1; i + 1 < l; i++) { seed = seed * 1103515245 + 12345; s[i] = ab[(seed >> 8) % sizeof ab]; }
        char *e = malloc(l); memcpy(e, s, l);
        if (!json_parse(&j, e, l)) json_str(&j, 0, b, sizeof b);
        json_free(&j); free(e);
    }
    printf("json: ok\n");
    return 0;
}
