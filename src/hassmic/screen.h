/* The screen UI of a model with a screen (the Echo Show: screen.c, which runs src/ui/ as a process of its own;
 * screen_none.c elsewhere).  Every call returns at once, whatever the UI process does: the state is kept here and
 * a thread of screen.c writes what changed (src/ui/scene.h has the protocol). */
#ifndef SCREEN_H
#define SCREEN_H
#include <stddef.h>

int  screen_present(void);                               /* this model has one: Sendspin asks for metadata and artwork */
void screen_start(void);
void screen_led(const char *op, const char *pattern);    /* main.c led(): the core's patterns, "-s" on, "-u" off */
void screen_level(int dbfs10);                           /* the talker (listening) or the reply (speaking), dBFS x 10 */
void screen_tz(const char *posix_tz);                    /* Home Assistant's time zone (GetTimeResponse) */
/* Sendspin (Music Assistant): what the group plays, and what its controller allows.  state: 0 stopped, 1 playing,
 * 2 paused; pos_ms where it is now; speed x1000 */
void screen_music_meta(const char *title, const char *artist, const char *album);
void screen_music_progress(int state, long long pos_ms, long long dur_ms, int speed);
/* repeat "off"/"one"/"all" (NULL: not told), shuffle -1 not told */
void screen_music_controller(int volume, int muted, const char *commands, const char *repeat, int shuffle);
void screen_music_art(const void *image, size_t n);      /* JPEG; n 0: none */
void screen_music_gone(void);                            /* no Music Assistant any more */
/* Music Assistant's API (mass.c): a line for the UI as it is (scene.h), and one followed by bytes (an image); queued,
 * the UI's restart drops what it had not read (mass_replay sends the state again) */
void screen_line(const char *line);
size_t screen_status_json(char *o, size_t cap);         /* the settings page's Screen page: {"apps","home","kiosk"} */
int  screen_set_home(const char *pkg);                   /* a package, "" the face, NULL none chosen: 0 set */
void screen_bin(const char *line, const void *data, size_t n);
/* The light band (ring_band.c) drawn by the UI process as one more layer: whether that process can run here (no more
 * once it was given up), its width once it is up (0 before, and again while it is started anew), and a row of it (w
 * pixels, 0xAARRGGBB little-endian; every row of the band the same; -1: the UI was given up, the band draws itself) */
int  screen_ui(void);
int  screen_width(void);
int  screen_band(const void *argb, int w);

/* Words and timers over the screen (the UI's words.c; issue #104): what was said and Home Assistant's reply, and the
 * running and ringing timers, on a see-through layer over the face and any app (never over the drawer or the band).
 * SCREEN_WORDS_RUN: a request starts (the last one's words go), _HEARD: what was said (a follow-up's: the reply goes
 * too), _REPLY: the reply, _IDLE: the satellite is idle again (the words stay screen_words_hold seconds, then fade).
 * Any thread, core_lock or not.  Nothing is kept while "Speech on screen" is off.
 * screen_words_on, _hold, screen_timers_on: the settings "Speech on screen", "Speech stays for" (s, 0 until the next
 * request), "Timers on screen"; -1 where there is no screen; set < 0 reads.
 * screen_timers_changed (lock held): the core's running or ringing timers changed (core_timers_running); told now, not
 * at the next once-a-second look.  screen_keep_on: a timer runs (not paused) or rings and timers show: the screen
 * stays on (ring_band.c's timeout) */
enum { SCREEN_WORDS_RUN, SCREEN_WORDS_HEARD, SCREEN_WORDS_REPLY, SCREEN_WORDS_IDLE };
void screen_words(int what, const char *text);
int  screen_words_on(int set);
int  screen_words_hold(int set);
int  screen_timers_on(int set);
void screen_timers_changed(void);
int  screen_keep_on(void);
#endif
