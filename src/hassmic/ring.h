/* The light ring beyond stock's animations (issue #26): Home Assistant's light, and the listening ring turned towards
 * the talker whatever else the ring shows.  ledcontroller only plays animation files it loaded at its start from
 * /system/etc/led-resources, and only names its layer config lists; root installs ours there (hassmic -G, main.sh
 * leds(), sysinstall.sh leds).  Without them: stock's animations only, and no light in Home Assistant. */
#ifndef RING_H
#define RING_H
#include <stddef.h>

#define RING_LEDS_MAX 24

struct ring_light {
    int on;
    float brightness, color_brightness;     /* 0..1, as Home Assistant sends them */
    float r, g, b;                          /* 0..1, the largest 1 */
    int effect;                             /* index in ring_effects */
    int fill;                               /* 0..100 %: that share of the LEDs lit, the rest dark (issue #57) */
    int nleds;                              /* > 0: LED by LED instead (Home Assistant's action), from the fill's start */
    unsigned char led_rgb[RING_LEDS_MAX][3];        /* as seen, 0..255; black: that LED off */
};
extern const char *const ring_effects[];    /* "None" first, as ESPHome's own lights */
extern const int ring_neffects;

/* hassmic -G DIR: the animations, the layer config with their names added to stock's, and "digest" (what the running
 * hassmic compares with the installed set); -G - prints only the digest.  Root runs it; stock's files are read from
 * the system partition. */
int  ring_generate(const char *dir);
/* run: plays (op "-s") or stops ("-u") a pattern, ledctrl's options.  Looks whether the installed set is this build's.
 * reloaded: after ring_reload, without the module's lock (custom_changed: ring_custom() or ring_per_led() is not
 * what it was) */
void ring_start(void (*run)(const char *op, const char *name), void (*reloaded)(int custom_changed));
int  ring_custom(void);                     /* ours are in ledcontroller: Home Assistant gets the light */
int  ring_assistant_steps(void);            /* and "hassmic_assist_NN": stock's volume_step-NN in cyan */
int  ring_per_led(void);                    /* and each LED's own layer: the fill, rotation and LED by LED (issue #57) */
/* The Echo's own patterns (stock's names): while one shows, Home Assistant's light is off.  "active-start" and
 * "active-end" are played turned towards the talker (ours) where we can read the beam */
void ring_set(const char *pattern);
/* Where a design (ringdesign.h) can take the place of stock's: one slot per pattern the core plays (RING_VOLUME: each
 * "volume_step-NN", RING_ASSIST: each "hassmic_assist_NN"; both take the colours of the design's first frame through
 * stock's steps).  RING_LISTEN and RING_END are turned towards the talker as stock's are: listening's copies held at
 * its loop when the talker moves */
enum { RING_PLAIN, RING_LISTEN, RING_END, RING_VOLUME, RING_ASSIST };
struct ring_slot { const char *id, *stock; int kind; };
#define RING_SLOTS_MAX 32
extern const struct ring_slot ring_slots[];
extern const int ring_nslots;
int  ring_slot_available(int i);            /* stock's config lists its pattern, so a design can go there */
int  ring_slot_live(int i);                 /* a design shows there now */
int  ring_leds(void);
/* Whether APPLIED (state/ring-applied's text) fits the system partition's share for designs.  0, or -1 and why */
int  ring_check(const char *applied, size_t n, char *err, size_t cap);
/* Root installed a set (ledcontroller started again with it): what plays where from now on, and what showed set
 * again, since the new ledcontroller shows nothing */
void ring_reload(void);
void ring_unset(const char *pattern);
void ring_tick(void);                       /* every 200 ms: the beam while listening, the light back after the Echo's */
void ring_light_get(struct ring_light *l);
void ring_light_set(const struct ring_light *l, unsigned flash_ms);  /* flash_ms > 0: blinks that long, then as before */
int  ring_rotation(int set);                /* LEDs the fill's start is turned by, 0..ring_leds()-1; -1 reads */
#endif
