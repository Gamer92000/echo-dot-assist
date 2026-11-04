/* The Echo's own voice (text to speech on the device, no Home Assistant): Fire OS's offline Ivona engine with one of
 * Amazon's voices (docs/re-ivona.md: Nina, Evelyn, ...), where the firmware has the engine's app and a voice is
 * installed (`speech_ivona.c`); nothing elsewhere (`speech_none.c`).  The voice loads in a thread of its own at
 * start (4.4 s on the Show 5) and stays loaded; sentences are spoken one after the other, in the order given. */
#ifndef SPEECH_H
#define SPEECH_H

/* What the speech thread hands the core: a sentence begins (RATE Hz mono s16), its audio in pieces, its end.  The
 * core's (main.c), called from the speech thread without the lock (end also from speech_cancel's caller).  TAG is
 * speech_say's.  end comes for every sentence taken, also one that failed, was cancelled or was dropped from the queue
 * (begin then never came): a Home Assistant reply's run ends with it. */
struct speech_out {
    void (*begin)(unsigned rate, int tag);
    void (*data)(const void *pcm, unsigned len);
    void (*end)(int tag);
};

/* Audio for someone else (wyoming_tts.c: Home Assistant's text to speech): the same, with the caller's ARG, and end's
 * OK (0: nothing came, or not all of it).  Called from the speech thread; end comes for every sentence taken. */
struct speech_sink {
    void (*begin)(void *arg, unsigned rate);
    void (*data)(void *arg, const void *pcm, unsigned len);
    void (*end)(void *arg, int ok);
    void *arg;
};

void speech_start(const struct speech_out *out);  /* looks for the engine and a voice and loads it, in the background */
int  speech_installed(void);                 /* 1: a voice and the engine are on the Echo (it may still be loading) */
int  speech_ready(void);                     /* 1: a voice is loaded and speaks; 0: none (yet) */
const char *speech_voice(void);              /* its name ("Nina"), "" without one */
const char *speech_language(void);           /* its language as a BCP 47 tag ("en-US"), "" without one */
/* The voices installed (their folders' names: "evelyn", "nina"), for the setting "voice": count, NAMES set.  Which is
 * wanted (Nina by default where she is, else the first; -1 without any) and the pick: the speech thread loads it
 * (~4.6 s; meanwhile speech_ready() is 0) and keeps the one before when it fails.  Known before speech_start */
int  speech_voices(const char *const **names);
int  speech_voice_index(void);
void speech_voice_select(int i);
int  speech_say(const char *text, int tag);  /* queued: 0, -1 without a voice (or the queue is full) */
int  speech_render(const char *text, const struct speech_sink *sink);  /* queued for SINK: 0, -1 as speech_say */
void speech_cancel(void);                    /* drops what is queued to be played here and ends such a sentence being
                                              * made (barge-in, "stop"); renders for others go on */
#endif
