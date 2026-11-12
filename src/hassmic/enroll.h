/* A wake word of your own, trained on the Echo (the settings page, Wake word: "Train your own"): the Echo asks for the
 * word eight times and for eight other things, records them through its own microphones (the audio the wake word
 * hears: after the front end), has the stock engine turn each recording into the activations that feed its wake word
 * head (pryon_extract), fits a new head to them (pryon_train, about four minutes on a Dot 3, at the lowest priority so
 * the wake word keeps working), checks it on the recordings it never saw, and, if every one is right, installs the set
 * next to the others (artifacts.c) and, if asked, makes it the wake word.  Nothing leaves the Echo, and the
 * recordings are deleted when it is done.  See enroll.c and docs/GUIDE.md. */
#ifndef ENROLL_H
#define ENROLL_H
#include <stddef.h>
#include <stdint.h>

/* 1 if this Echo can: Amazon's engine's tools beside the binary and the stock set to start from */
int  enroll_available(const char *base_manifest);
/* Begins a session on PHRASE (SECONDS: each recording, 2..6).  USE_NOW: the word is the wake word once installed.
 * BASE_MANIFEST: the stock set's pryon.manifest (board.wake_manifest).  0, or -1 with the reason in ERR. */
int  enroll_start(const char *phrase, int seconds, int use_now, const char *base_manifest, char *err, size_t errsz);
/* Cancellation is accepted through training; packaging and installation are non-cancellable. */
void enroll_cancel(void);
/* Clear a terminal result; unresolved work is retracted only while root is idle. 0 or -1 with ERR. */
int enroll_dismiss(char *err, size_t errsz);
/* {"state","phrase","prompt":{...},"progress","eta","message","error","result":{...},"install"} */
size_t enroll_json(char *out, size_t cap);

/* the capture thread: 1 while the prompts run (the wake word is not fed meanwhile), and the mic's 16 kHz samples */
int  enroll_recording(void);
void enroll_feed(const int16_t *pcm, size_t n);
/* a wake word set made here (its folder has the mark file): the only kind the page may remove */
int  enroll_ours(const char *id);
/* asks root to remove it (state/artifacts/delete; main.sh's watcher runs artifact-install.sh delete, which restarts hassmic):
 * 0, or -1 with the reason.  Not for Amazon's sets, and the caller must not pass the word in use. */
int  enroll_delete(const char *id, char *err, size_t errsz);
/* at start: what a restart left (recordings of a session that never ended) goes */
void enroll_init(void);
#endif
