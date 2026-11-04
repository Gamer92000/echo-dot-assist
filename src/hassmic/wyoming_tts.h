/* A Wyoming text-to-speech server on the Echo's own voice (speech.h), for Home Assistant: see wyoming_tts.c */
#ifndef WYOMING_TTS_H
#define WYOMING_TTS_H
#include <stddef.h>

#define WYTTS_PORT 16710            /* inbound TCP 16384-32767 only (lockdown.sh); 16700 is the Wyoming satellite's */

void wytts_start(int port);         /* where a voice is installed: listens in a thread of its own */
/* KEY's string in a JSON text as UTF-8 (escapes and \uXXXX undone, line breaks as spaces); 0, or -1 */
int  wytts_json_text(const char *json, const char *key, char *out, size_t cap);
#endif
