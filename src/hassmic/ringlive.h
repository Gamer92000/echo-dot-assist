/* The light ring live (feature/led-live): any colour on any LED from Home Assistant, frame by frame, written by hassmic
 * itself into the LED driver's frame file, which ledcontroller writes only when its own picture changes (PLAN.md "Light
 * ring live", measured on a Dot 3).  Only where root gave hassmic that one file (main.sh) and the board names it.
 *
 * What it never does: write led_current (the chip's current, ledcontroller's alone), the driver's calibration, the boot
 * animation or the mute button's PWM; write a channel above what ledcontroller itself writes at the brightness the ring
 * runs at (its table's pwm x 255); write while one of the Echo's own patterns shows (ring.c) or the microphones are off;
 * write more than RINGLIVE_FPS frames a second.  When nothing of ours is left it writes the dark frame once and hands
 * the ring back: ledcontroller, idle, shows dark too.
 *
 * LEDs are counted from where the ring's fill starts (the volume steps' first LED, turned by "Light ring rotation"), as
 * the light_ring_leds action does; a design plays as drawn (ringdesign.h), as installed it would. */
#ifndef RINGLIVE_H
#define RINGLIVE_H
#include "ringdesign.h"
#include "ring.h"

#define RINGLIVE_FPS    30              /* the driver takes ~65 a second (15 ms a write, mostly the I2C bus) */
#define RINGLIVE_ANIM_FRAMES RD_FRAMES

struct live_led { int on; float brightness, r, g, b; };     /* as Home Assistant's light: 0..1, the colour's largest 1 */

/* hold: 1 while the Echo's own patterns or the microphones' state own the ring (called from the writer thread, no lock
 * of the caller's held).  level: the ring's brightness 0..100 (auto included).  changed: the LEDs' states or what plays
 * changed by itself (a timeout, an animation's end), from the writer thread without locks.  0: live (the frame is ours
 * to write), -1: not on this Echo (the reason logged) */
int  ringlive_start(int (*hold)(void), int (*level)(void), void (*changed)(void));
int  ringlive_available(void);
/* "Light ring live" on or off: on, the live ring also draws Home Assistant's "Light ring" (ring.c's state, its exact
 * colour, the live effects, its fill) under the LEDs' own lights; off, all of it off and the palette back */
void ringlive_enable(int on);
int  ringlive_enabled(void);
void ringlive_fade(int ms);             /* the next change fades in over ms (a light's transition) */
void ringlive_shutdown(void);           /* the dark frame if ours shows, the writer ended (at most 1 s) */
/* A design's text (ringdesign.h), as drawn: the settings page's "Show on the Echo".  0, or -1 with why */
int  ringlive_play_text(const char *text, size_t n, char *err, size_t cap);
int  ringlive_leds(void);

void ringlive_led_get(int i, struct live_led *l);
void ringlive_led_set(int i, const struct live_led *l);
/* Every LED at once: "#rrggbb"/"rrggbb" each ("" or "off": dark), brightness 0..1 over them all; timeout_s > 0: dark
 * again after that.  0, or -1 with why */
int  ringlive_frame(char colors[][16], int n, float brightness, int timeout_s, char *err, size_t cap);
/* An animation from Home Assistant: "<ms>:RRGGBB,RRGGBB,..." per frame, as a design's lines, LEDs from the fill's
 * start, the frames one per line in LINES; repeat 0: until stopped.  0, or -1 with why */
int  ringlive_animate(const char *lines, size_t n, int repeat, float brightness, char *err, size_t cap);
/* A design of the library by its name (case does not matter), as drawn; repeat 0: as the design says (its loop for
 * ever, or once).  0, or -1 with why */
int  ringlive_design(const char *name, int repeat, float brightness, char *err, size_t cap);
void ringlive_stop(void);               /* the animation; the LEDs' own colours show again */
void ringlive_clear(void);              /* everything: the LEDs off, the animation stopped */
int  ringlive_active(void);             /* any LED on or an animation playing */
void ringlive_playing(char *out, size_t cap);   /* the design's name, "animation" for Home Assistant's, "" */
int  ringlive_writes(void);             /* frames written since the start (the tests' measure of the rate) */

/* Pure parts, for the unit test: a colour as seen (0..1 a channel) to what the driver is written at pwm (the
 * brightness table's for the ring's level): gamma 2.2 to 0..255 as ring.c maps a design, then pwm as ledcontroller's
 * HAL does (scaleDutyCycle: multiplied and truncated).  Never above 255 x pwm */
void ringlive_duty(const float seen[3], float pwm, unsigned char out[3]);
/* The brightness table: pwm per level 0..100 of led_mute_ring_config.json's rgbTable for the cap colour (idme alsparams
 * capcolor; -1: unknown, the lower of each level).  0, or -1 */
int  ringlive_table(const char *json, size_t n, int capcolor, float pwm[101]);
/* The frame file's text: 12 x "RRGGBB" and a newline, out at least leds * 6 + 2 */
void ringlive_text(const unsigned char (*px)[3], int leds, char *out);
/* An effect of ring.h's list over n LEDs in the fill's order at time t (ms): as seen, colour rgb (largest 1) at level */
void ringlive_effect(int effect, const float rgb[3], float level, int n, long long t, float out[][3]);
#endif
