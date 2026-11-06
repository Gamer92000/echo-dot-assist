/* wyoming_tts.c: a streamed text cut into sentences (wytts_sentence) as Home Assistant's chunks bring it, and the JSON
 * text of a chunk.  The server itself runs in tests/fake_ha_own_voice.py */
#include "wyoming_tts.h"
#include <stdio.h>
#include <string.h>

/* what wyoming_tts.c calls elsewhere: never reached here */
int core_reply_claim(void) { return 0; }
int core_reply_part(const char *t) { (void)t; return -1; }
void core_reply_final(void) {}

static int fails;

/* TEXT fed in pieces of STEP bytes (0: at once), then its end: the sentences, joined by '|' */
static void check(const char *text, size_t step, const char *want)
{
    char buf[1024] = "", got[1024] = ""; size_t n = 0, len = strlen(text), fed = 0;
    while (fed < len || fed == len) {
        size_t k = step ? step : len; if (k > len - fed) k = len - fed;
        memcpy(buf + n, text + fed, k); n += k; fed += k;
        int final = fed == len;
        for (size_t e; n && (e = wytts_sentence(buf, n, final)); ) {
            if (*got) strcat(got, "|");
            strncat(got, buf, e);
            memmove(buf, buf + e, n - e); n -= e;
        }
        if (final) break;
    }
    if (strcmp(got, want)) { printf("FAIL [%s] step %zu\n  got  \"%s\"\n  want \"%s\"\n", text, step, got, want); fails++; }
}

int main(void)
{
    for (size_t step = 0; step < 5; step++) {               /* the same, however the chunks fall */
        check("Hello there. How are you?", step, "Hello there.| How are you?");
        check("Dr. Smith went to the U.S. on 3. November. Then left", step, "Dr. Smith went to the U.S. on 3. November.| Then left");
        check("It costs 3.5 euros! Really?\" Yes.", step, "It costs 3.5 euros!| Really?\"| Yes.");
        check("Line one\nLine two", step, "Line one\n|Line two");
        check("He said \xe2\x80\x9cGo.\xe2\x80\x9d Then", step, "He said \xe2\x80\x9cGo.\xe2\x80\x9d| Then");
        check("It was founded in 1998. It grew.", step, "It was founded in 1998.| It grew.");
        check("See example.com. Done", step, "See example.com.| Done");
        check("Wait... what?! Fine", step, "Wait...| what?!| Fine");
        check("e.g. this one", step, "e.g. this one");
    }
    /* a sentence must not end where the chunk does: "3." may be "3.5" */
    char b[] = "It is 3.";
    if (wytts_sentence(b, strlen(b), 0) != 0) { printf("FAIL a sentence ended at the chunk's end\n"); fails++; }
    /* no end for SENTENCE_MAX bytes: cut at a space */
    char longs[600]; memset(longs, 0, sizeof longs);
    for (int i = 0; i < 100; i++) strcat(longs, "word ");
    size_t e = wytts_sentence(longs, strlen(longs), 0);
    if (!e || e > 400 || longs[e - 1] != ' ') { printf("FAIL a long text was not cut at a space: %zu\n", e); fails++; }

    char out[64];
    if (wytts_json_text("{\"type\":\"synthesize-chunk\"}\n{\"text\": \"Gr\\u00fc\\u00dfe.\\nJa\"}", "text", out, sizeof out) ||
        strcmp(out, "Gr\xc3\xbc\xc3\x9f" "e.\nJa")) { printf("FAIL json text: \"%s\"\n", out); fails++; }
    printf(fails ? "wytts: %d failed\n" : "wytts: all good\n", fails);
    return fails != 0;
}
