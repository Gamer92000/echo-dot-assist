/* The light ring beyond stock's animations (issue #26): Home Assistant's light, and the listening ring turned towards
 * the talker whatever else the ring shows.  ledcontroller only plays animation files it loaded at its start from
 * /system/etc/led-resources, and only names its layer config lists; root installs ours there (hassmic -G, main.sh
 * leds(), sysinstall.sh leds).  Without them: stock's animations only, and no light in Home Assistant. */
#ifndef RING_H
#define RING_H

struct ring_light {
    int on;
    float brightness, color_brightness;     /* 0..1, as Home Assistant sends them */
    float r, g, b;                          /* 0..1, the largest 1 */
    int effect;                             /* index in ring_effects */
};
extern const char *const ring_effects[];    /* "None" first, as ESPHome's own lights */
extern const int ring_neffects;

/* hassmic -G DIR: the animations, the layer config with their names added to stock's, and "digest" (what the running
 * hassmic compares with the installed set); -G - prints only the digest.  Root runs it; stock's files are read from
 * the system partition. */
int  ring_generate(const char *dir);
/* run: plays (op "-s") or stops ("-u") a pattern, ledctrl's options.  Looks whether the installed set is this build's */
void ring_start(void (*run)(const char *op, const char *name));
int  ring_custom(void);                     /* ours are in ledcontroller: Home Assistant gets the light */
/* Themes of the Echo's own rings (setting ring_theme): ids and names, "stock" first.  ring_themes(): their rings are in
 * the installed set (made where stock's rings are the Dot 2's blue); ring_theme(v) picks one (v < 0: only asks) and
 * gives the one picked; stock's rings play while the set has none */
extern const char *const ring_theme_ids[], *const ring_theme_names[];
extern const int ring_nthemes;
int  ring_themes(void);
int  ring_theme(int v);
/* The Echo's own patterns (stock's names): while one shows, Home Assistant's light is off.  "active-start" and
 * "active-end" are played turned towards the talker (ours) where we can read the beam */
void ring_set(const char *pattern);
void ring_unset(const char *pattern);
void ring_tick(void);                       /* every 200 ms: the beam while listening, the light back after the Echo's */
void ring_light_get(struct ring_light *l);
void ring_light_set(const struct ring_light *l, unsigned flash_ms);  /* flash_ms > 0: blinks that long, then as before */
#endif
