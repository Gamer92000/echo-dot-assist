/* Wake word clips (setting wake_clips, off by default; the settings page's "Wake word clips" card): the last
 * WAKECLIP_KEEP wake words and the last WAKECLIP_KEEP near misses as the engine heard them, kept in RAM only, to be
 * listened to on the page.  What woke the Echo (the television, a word in a conversation, its own reply) or what it
 * nearly took is heard there, not guessed from the log; a clip marked "wake word" or "not the wake word" is a sample
 * for training one's own (microWakeWord).  Nothing of it reaches Home Assistant, the log or the flash: a restart, an
 * update or switching it off drops every clip.
 *
 * A clip runs from WAKECLIP_PRE_MS ahead of the word to the setting wake_clips_after past its end (0.3 to 3 s: past
 * that it would hold the request itself, which "Last heard" says better and more privately).  The ring holds 4 s, so
 * the part after the word is copied as it comes (wakeclip_feed), not when the clip is asked for.
 *
 * The audio is the ring's: post-AEC micAsr as the wake word engine was fed it, before any gain or noise reduction.
 * Locking: one mutex of its own; every function any thread, none blocks for longer than a copy. */
#ifndef WAKECLIP_H
#define WAKECLIP_H
#include <stddef.h>
#include <stdint.h>

#define WAKECLIP_KEEP 10            /* per kind */
#define WAKECLIP_RATE 16000
#define WAKECLIP_PRE_MS 1000        /* the room before the word: its floor, and whether someone was already talking */
#define WAKECLIP_WORD_MAX_MS 2000   /* a word reported longer is cut to its last 2 s (Pryon's begin can lie far back) */
#define WAKECLIP_AFTER_MAX_MS 3000
#define WAKECLIP_ENV 96             /* the waveform the list carries (wakeclip_json "env") */
#define WAKECLIP_OUTCOME_MS 30000   /* what came of a wake word is told within this, or not at all */

enum wakeclip_kind { WAKECLIP_HIT, WAKECLIP_MISS, WAKECLIP_KINDS };
enum wakeclip_label { WAKECLIP_UNLABELLED, WAKECLIP_WAKE, WAKECLIP_NOT_WAKE };
/* what the Echo itself played when it heard it: its own sound is the commonest false wake */
enum { WAKECLIP_MUSIC = 1, WAKECLIP_REPLY = 2, WAKECLIP_RINGING = 4, WAKECLIP_SOUND = 8 };

struct wakeclip_info {
    unsigned id;                    /* new with every clip, never 0 */
    int kind, label, playing, done; /* done: every sample is in */
    char word[32];                  /* the engine's keyword ("ALEXA", "STOP", a microWakeWord id) */
    char outcome[24];               /* "request", "no_speech", "answered_elsewhere", or "" */
    long long wall;                 /* when, seconds since 1970; 0: the clock was not set */
    long long mono_ms;
    uint64_t from;                  /* the clip's first sample, in the engine's sample index */
    uint32_t len, word_at, word_len;/* samples: the clip's, and where the word lies in it */
    float word_db, floor_db;        /* dBFS: the word's rms, the room's before it (once done) */
    int peak;                       /* the largest |sample|: 32767 means clipped */
};

void wakeclip_set(int on, int after_ms);    /* off drops every clip */
int  wakeclip_enabled(void);
int  wakeclip_after_ms(void);
/* A wake word (WAKECLIP_HIT) or near miss whose word lies in samples [begin, end) of the engine's index */
void wakeclip_mark(int kind, const char *word, uint64_t begin, uint64_t end, int playing, long long wall, long long mono_ms);
/* The capture thread, each block once it is in the ring: ring holds cap samples, ring_n were put in so far */
void wakeclip_feed(const int16_t *ring, size_t cap, uint64_t ring_n);
/* What came of the newest wake word, if it was heard within WAKECLIP_OUTCOME_MS of now and has none yet */
void wakeclip_outcome(const char *what, long long now_ms);
int  wakeclip_get(int i, struct wakeclip_info *out);     /* 0..: newest first; -1 past the last */
int16_t *wakeclip_audio(unsigned id, size_t *n);        /* a malloc'd copy of a done clip; NULL: none, or not done */
int  wakeclip_label(unsigned id, int label);            /* -1: no such clip or label */
int  wakeclip_delete(unsigned id);                      /* 0: all; -1: no such clip */
size_t wakeclip_json(char *out, size_t cap, long long now_ms);   /* the list, for the page (web.c); now: "ago" */
#endif
