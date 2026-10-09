/* Audio backend: Amazon mixer daemon on the device, files on the PC (for protocol tests). */
#ifndef AUDIO_H
#define AUDIO_H
#include <stddef.h>

#define CAP_RATE 16000

/* Blocks until the next block of 16 kHz mono s16le is available.  Returns bytes, 0 on timeout, -1 on fatal error.
 * *data stays valid until the next call. */
int  cap_open(void);
int  cap_read(const void **data);
void cap_close(void);

int  play_open(unsigned rate, unsigned channels);
int  play_write(const void *data, size_t len);      /* blocks at playback speed */
void play_close(int drain);

/* Second, independent playback stream for music (Sendspin); mixes with the voice stream.  One at a time.
 * music_write blocks while the device buffer is full; music_queued_us is how much written audio has not played yet. */
int       music_open(unsigned rate, unsigned channels);
int       music_write(const void *data, size_t len);
long long music_queued_us(void);
void      music_close(void);

/* Bluetooth speaker stream: like the music stream, beside it */
int       bt_open(unsigned rate, unsigned channels);
int       bt_write(const void *data, size_t len);
long long bt_queued_us(void);
void      bt_close(void);

/* Drop In (dropin.c): the mixer's "Voip" stream.  Opening one puts Amazon's front end into its call mode (libasp "VoIP
 * mode 1": the canceller tuned for calls, noise dependent volume), which takes 10 dB more of the far end out of micAsr
 * than the TTS stream gets (2026-10-08, biscuit: the same speech at -48 dBFS in micRaw left -60..-66 dBFS in micAsr on
 * Voip, -45..-58 on TTS, over a -72 floor).  Like the music stream otherwise; voip_queued_us is what has not played. */
int       voip_open(unsigned rate, unsigned channels);
int       voip_write(const void *data, size_t len);
long long voip_queued_us(void);
void      voip_close(void);

/* Volume, 0..100.  VOL_MAIN: music, sounds and calls (what the keys move); VOL_TTS: the assistant's replies; VOL_MUTE:
 * a global mute the backend keeps (1 = set; nothing of ours sets it, main.c clears it).  vol_read gives fallback when
 * the backend cannot tell.  The Dots: the mixer's MainVolume, TTSVolume and Mute (audio_manager_*_prop, kept by the
 * mixer across reboots); Android: a gain on each player, kept in state/volume; the PC: in memory. */
enum vol { VOL_MAIN, VOL_TTS, VOL_MUTE };
int  vol_read(enum vol which, int fallback);
void vol_write(enum vol which, int v);

/* Amazon's front end (the mixer's libasp on the Dots, the audio HAL's on Android; main.c listening() and afe_score()).
 * afe_listening: its utterance flag, which holds the cancellers still while a command is spoken (FINDINGS.md "Listening
 * mode"); afe_arbitration: the wake word's place on its clock (ms), then its energies as JSON into json (1 = got them);
 * afe_stream_stopped: the end of a command, which also ends the diagnostics that reading the energies starts.  Nothing
 * on the PC. */
void afe_listening(int on);
int  afe_arbitration(long ts, long te, char *json, size_t n);
void afe_stream_stopped(void);
/* Its user equalizer, {"bands":[{"name":"BASS","level":n},..]} both ways (main.c core_eq): get = 1 when json got one */
int  afe_eq_get(char *json, size_t n);
void afe_eq_set(const char *json);

/* Short UI sound on its own stream; mixes with whatever else plays.  Blocks for the length of the sound. */
void play_earcon(const short *pcm, size_t samples, unsigned rate);
/* The same, cut off (flushed) as soon as go() returns 0: ringtones, 4 to 6 s long, that stop when the call is answered */
void play_earcon_while(const short *pcm, size_t samples, unsigned rate, int (*go)(void));
#endif
