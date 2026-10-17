/* Whisper detection backend: Amazon's whisper detector (libpryon WhisperApi_*, docs/re-whisper.md) on the mic audio of
 * one request, with the model scripts/artifacts.sh installs (DAVS only; without it there is no detection).  A request
 * is scored once its end of speech is known: Home Assistant's VAD end.  The callback runs on the detector's thread. */
#ifndef WHISPER_H
#define WHISPER_H
#include <stddef.h>
#include <stdint.h>

/* whispered: 1 or 0; confidence 0..1000 against the model's threshold */
typedef void (*whisper_cb)(int whispered, int confidence, int threshold);

int  whisper_open(whisper_cb cb);           /* loads the model; -1: none installed (or it does not load) */
void whisper_begin(void);                   /* a new request: a fresh detector */
void whisper_feed(const int16_t *samples, size_t count);
void whisper_end(int score);                /* score: the end of speech came, score what was fed; 0: drop it */
#endif
