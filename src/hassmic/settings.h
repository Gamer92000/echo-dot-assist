/* The satellite's settings by name: one table for Home Assistant's entities, the web page (web.c) and exports.
 * Settings kept here persist in state/config ("name=value" lines); the others (arbitration, Sendspin, equalizer) stay
 * where their module keeps them and are only reached through this table.  All functions: core_lock held. */
#ifndef SETTINGS_H
#define SETTINGS_H
#include <stddef.h>

enum stype { S_BOOL, S_INT, S_CHOICE };

struct setting {
    const char *name, *label, *group, *unit;
    enum stype type;
    int min, max;                       /* S_INT */
    int export_;                        /* goes into an export: not tied to this one Echo (its name, keys, pairings) */
    int feature;                        /* its entities are in Home Assistant only while it is on */
};

/* The Bluetooth announcement words.  Home Assistant speaks them with the satellite's pipeline voice, but tells neither us
 * nor a template that pipeline's language, so the user picks it to match.  Four whole sentences each rather than one "a
 * device" to slot in: articles and cases differ between "connected to" and "disconnected from" in most languages. */
struct bt_lang { const char *code, *name, *on, *off, *on_any, *off_any; };
extern const struct bt_lang bt_langs[];
extern const int bt_lang_count;
extern const char *const denoise_names[4];      /* "Off", "Low", "Medium", "High": Home Assistant's select */

void settings_load(void);               /* state/config, or the older state/settings once; then applied.  Once only */
void settings_save(void);
void settings_preset(void);             /* state/preset (scripts/setup.sh --preset): applied once at start, then .applied */
int  settings_mic_level(void);
int  settings_on(const char *name);     /* a bool setting's value; 0 if this Echo has no such setting */
const struct bt_lang *settings_bt_lang(void);
int  settings_bt_lang_index(int set);   /* -1 reads */

/* The table: settings this Echo has (no Wi-Fi motion setting without the module, ...), in display order */
int  settings_count(void);
const struct setting *settings_at(int i);
const struct setting *settings_find(const char *name);
int  settings_get(const struct setting *s);                     /* S_CHOICE: the index */
int  settings_choices(const struct setting *s, const char *const **names);   /* S_CHOICE: count and the value names */
/* Set from text ("on"/"off"/"1"/"0", a number, a choice's value name).  0 ok, -1 unknown or out of range: err says why.
 * Saves, and has the protocol show it (proto->settings_changed). */
int  settings_set(const char *name, const char *value, char *err, size_t errsz);
/* "name=value" lines: every setting (or only the exportable ones); returns the length written (truncated to cap) */
size_t settings_text(char *out, size_t cap, int export_only);
/* Apply "name=value" lines (an export, a preset); # comments and blank lines skipped.  Errors are listed in err, one
 * per line; returns how many settings were applied */
int  settings_apply_text(const char *text, char *err, size_t errsz);
#endif
