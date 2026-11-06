/* What was said and the reply, drawn as a picture for the Echo Show's screen (ring_band.c's text layer): the words in a
 * font of the firmware (Amazon Ember, as stock's screens; Roboto where it has none), wrapped and fitted into the space,
 * over a shade that rises from the bottom.  Pixels only: ring_band.c puts them on the screen through ringbar.dex. */
#ifndef TEXTDRAW_H
#define TEXTDRAW_H
#include <stdint.h>

/* The fonts, once: 0, or -1 when none of them opens (then no text is drawn).  The PC: HASSMIC_FONTS a folder in place of
 * /system/fonts (a firmware's), HASSMIC_FONT one file for both faces */
int  textdraw_init(void);
/* heard and reply (UTF-8, either may be empty) into px, w x h pixels of 0xAARRGGBB, not premultiplied, top row first;
 * the bottom `keep` rows stay clear (the band is there).  The heard text small and grey at the top, the reply large
 * and white below it, both as large as fits: lines wrap at spaces, a word longer than a line breaks, and what does not
 * fit at the smallest size ends in "…" */
void textdraw_render(uint32_t *px, int w, int h, int keep, const char *heard, const char *reply);

/* Timers, as textdraw_render draws words: n of them stacked in the middle, each its name (grey) over its time left, large
 * with its unit ("4:59 min", "1:02:03 h", "45 s"), over a bar of what is left of it.  A paused one grey; ringing: the times orange and the bars
 * full (t: what rings) */
struct textdraw_timer { const char *name; int left, total, active; };
void textdraw_timers(uint32_t *px, int w, int h, int keep, const struct textdraw_timer *t, int n, int ringing);
#endif
