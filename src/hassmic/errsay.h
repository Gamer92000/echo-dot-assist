/* What went wrong, as a sentence the Echo says (main.c core_error_say, with the Echo's own voice: speech.h).  Pure:
 * tests/unit/errsay_test.c.  English: the voice is. */
#ifndef ERRSAY_H
#define ERRSAY_H
#include <stddef.h>

/* CODE: Home Assistant's pipeline error code ("intent-failed", "stt-stream-failed", ...) or one of hassmic's own:
 * "timeout" (no answer within the pipeline timeout), "refused" (Home Assistant would not start a run), "no-link" (no
 * Home Assistant), "no-assistant" (linked, but no voice assistant subscribed).  MESSAGE: Home Assistant's text with it
 * ("Error talking to Ollama: [Errno 111] Connect call failed ..."), or NULL.  The sentence in OUT; "" for codes that
 * are not failures to tell (nothing said, another Echo answered). */
void errsay_sentence(const char *code, const char *message, char *out, size_t n);
#endif
