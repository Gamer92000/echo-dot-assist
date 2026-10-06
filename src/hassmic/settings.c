#include "settings.h"
#include "core.h"
#include "a2dp.h"
#include "arb.h"
#include "ble.h"
#include "micgain.h"
#include "sendspin.h"
#include "sound.h"
#include "update.h"
#include "wifimotion.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const denoise_names[4] = { "Off", "Low", "Medium", "High" };
static const char *const denoise_values[4] = { "off", "low", "medium", "high" };

const struct bt_lang bt_langs[] = {
    { "en", "English", "Connected to %s", "Disconnected from %s", "Connected to a Bluetooth device", "Disconnected from a Bluetooth device" },
    { "de", "Deutsch", "Verbunden mit %s", "Getrennt von %s", "Verbunden mit einem Bluetooth-Gerät", "Getrennt von einem Bluetooth-Gerät" },
    { "fr", "Français", "Connecté à %s", "Déconnecté de %s", "Connecté à un appareil Bluetooth", "Déconnecté d'un appareil Bluetooth" },
    { "es", "Español", "Conectado a %s", "Desconectado de %s", "Conectado a un dispositivo Bluetooth", "Desconectado de un dispositivo Bluetooth" },
    { "it", "Italiano", "Connesso a %s", "Disconnesso da %s", "Connesso a un dispositivo Bluetooth", "Disconnesso da un dispositivo Bluetooth" },
    { "pt", "Português", "Conectado a %s", "Desconectado de %s", "Conectado a um dispositivo Bluetooth", "Desconectado de um dispositivo Bluetooth" },
    { "nl", "Nederlands", "Verbonden met %s", "Verbinding met %s verbroken", "Verbonden met een Bluetooth-apparaat", "Verbinding met een Bluetooth-apparaat verbroken" },
    { "sv", "Svenska", "Ansluten till %s", "Frånkopplad från %s", "Ansluten till en Bluetooth-enhet", "Frånkopplad från en Bluetooth-enhet" },
    { "da", "Dansk", "Forbundet til %s", "Afbrudt fra %s", "Forbundet til en Bluetooth-enhed", "Afbrudt fra en Bluetooth-enhed" },
    { "nb", "Norsk", "Koblet til %s", "Koblet fra %s", "Koblet til en Bluetooth-enhet", "Koblet fra en Bluetooth-enhet" },
    { "fi", "Suomi", "Yhdistetty laitteeseen %s", "Yhteys laitteeseen %s katkaistu", "Yhdistetty Bluetooth-laitteeseen", "Yhteys Bluetooth-laitteeseen katkaistu" },
    { "pl", "Polski", "Połączono z %s", "Rozłączono z %s", "Połączono z urządzeniem Bluetooth", "Rozłączono z urządzeniem Bluetooth" },
};
const int bt_lang_count = (int)(sizeof bt_langs / sizeof bt_langs[0]);
static const char *bt_lang_codes[sizeof bt_langs / sizeof bt_langs[0]];

static int mic_level = MICGAIN_LEVEL, bt_lang;
static int bt_audio = 1, bt_speaker, whisper = 1;      /* features kept here; bt_speaker's default: settings_load */

static const char *state_dir(void) { const char *e = getenv("HASSMIC_STATE"); return e ? e : "/data/local/hassmic/state"; }
static void path(char *out, size_t cap, const char *file) { snprintf(out, cap, "%s/%s", state_dir(), file); }

/* ---------------------------------------------------------------- the table */

enum id { MIC_LEVEL, DENOISE, WAKE_SOUND, MUTE, DND, BT_ANNOUNCE, BT_LANG, LED_AUTO, LED_LEVEL, ARB, SOUND, WHISPER, WIFI_MOTION,
          WIFI_SENS, BT_AUDIO, BT_SPEAKER, UPDATES, SS_UNPAIRED, EQ_BASS, EQ_MID, EQ_TREBLE, NSET };

/* Features ("Features" group, feature = 1): Home Assistant lists their entities only while they are on (proto_esphome.c
 * listed()); everything else here is on the settings page only, except what Home Assistant always shows (wake sound,
 * mute, do not disturb, LEDs, equalizer). */
static const struct setting table[NSET] = {
    [MIC_LEVEL]   = { "mic_level", "Mic level", "Voice", "dBFS", S_INT, MICGAIN_LEVEL_MIN, MICGAIN_LEVEL_MAX, 1, 0 },
    [DENOISE]     = { "noise_reduction", "Noise reduction", "Voice", NULL, S_CHOICE, 0, 0, 1, 0 },
    [WAKE_SOUND]  = { "wake_sound", "Wake sound", "Voice", NULL, S_BOOL, 0, 1, 1, 0 },
    [MUTE]        = { "mute", "Mute", "Voice", NULL, S_BOOL, 0, 1, 0, 0 },            /* what the Echo is doing right now */
    [DND]         = { "do_not_disturb", "Do not disturb", "Voice", NULL, S_BOOL, 0, 1, 0, 0 },
    [BT_ANNOUNCE] = { "bluetooth_announcements", "Bluetooth announcements", "Bluetooth", NULL, S_BOOL, 0, 1, 1, 0 },
    [BT_LANG]     = { "bluetooth_announcement_language", "Announcement language", "Bluetooth", NULL, S_CHOICE, 0, 0, 1, 0 },
    [LED_AUTO]    = { "led_auto_brightness", "LED auto brightness", "Lights", NULL, S_BOOL, 0, 1, 1, 0 },
    [LED_LEVEL]   = { "led_brightness", "LED brightness", "Lights", "%", S_INT, 0, 100, 1, 0 },
    [ARB]         = { "arbitration", "Wake word arbitration with other Echos", "Features", NULL, S_BOOL, 0, 1, 1, 1 },
    [SOUND]       = { "sound_detection", "Sound detection", "Features", NULL, S_BOOL, 0, 1, 1, 1 },
    [WHISPER]     = { "whisper_detection", "Whisper detection", "Features", NULL, S_BOOL, 0, 1, 1, 1 },
    [WIFI_MOTION] = { "wifi_motion", "Wi-Fi motion (experimental)", "Features", NULL, S_BOOL, 0, 1, 1, 1 },
    [WIFI_SENS]   = { "wifi_motion_sensitivity", "Wi-Fi motion sensitivity", "Features", NULL, S_INT, WIFIMOTION_SENS_MIN, WIFIMOTION_SENS_MAX, 1, 0 },
    [BT_AUDIO]    = { "bluetooth_audio", "Bluetooth audio from phones", "Features", NULL, S_BOOL, 0, 1, 1, 1 },
    [BT_SPEAKER]  = { "bluetooth_speaker", "Play on a Bluetooth speaker", "Features", NULL, S_BOOL, 0, 1, 1, 1 },
    [UPDATES]     = { "online_updates", "Online updates", "System", NULL, S_CHOICE, 0, 0, 1, 0 },
    [SS_UNPAIRED] = { "sendspin_unpaired", "Music Assistant without pairing", "Music", NULL, S_BOOL, 0, 1, 1, 0 },
    [EQ_BASS]     = { "equalizer_bass", "Equalizer bass", "Sound", "dB", S_INT, -6, 6, 1, 0 },
    [EQ_MID]      = { "equalizer_mid", "Equalizer mid", "Sound", "dB", S_INT, -6, 6, 1, 0 },
    [EQ_TREBLE]   = { "equalizer_treble", "Equalizer treble", "Sound", "dB", S_INT, -6, 6, 1, 0 },
};

static int present(enum id i)
{
    switch (i) {
    case BT_ANNOUNCE: case BT_LANG: case BT_AUDIO: case BT_SPEAKER: return ble_present();
    case WHISPER: return core_whisper_model();
    case SOUND: return sound_model() != SOUND_NONE;               /* the firmware has one on every model so far */
    case LED_AUTO: return core_lux() == core_lux();               /* a light sensor: not NAN */
    case WIFI_MOTION: case WIFI_SENS: return wifimotion_present();
    case ARB: return arb_running();
    case SS_UNPAIRED: return core_sendspin_port != 0;
    default: return 1;
    }
}

static int ids[NSET], nids = -1;
static void index_table(void) { if (nids < 0) { nids = 0; for (int i = 0; i < NSET; i++) if (present(i)) ids[nids++] = i; } }
int settings_count(void) { index_table(); return nids; }
const struct setting *settings_at(int i) { index_table(); return i >= 0 && i < nids ? &table[ids[i]] : NULL; }
const struct setting *settings_find(const char *name)
{
    index_table();
    for (int i = 0; i < nids; i++) if (!strcmp(table[ids[i]].name, name)) return &table[ids[i]];
    return NULL;
}

int settings_choices(const struct setting *s, const char *const **names)
{
    switch (s - table) {
    case DENOISE: *names = denoise_values; return 4;
    case BT_LANG:
        for (int i = 0; i < bt_lang_count; i++) bt_lang_codes[i] = bt_langs[i].code;
        *names = bt_lang_codes; return bt_lang_count;
    case UPDATES: *names = update_channels; return 3;
    default: *names = NULL; return 0;
    }
}

int settings_get(const struct setting *s)
{
    switch (s - table) {
    case MIC_LEVEL: return mic_level;
    case DENOISE: return core_mic_denoise(-1);
    case WAKE_SOUND: return core_wake_sound(-1);
    case MUTE: return core_soft_mute(-1);
    case DND: return core_dnd(-1);
    case BT_ANNOUNCE: return core_bt_announce(-1);
    case BT_LANG: return bt_lang;
    case LED_AUTO: return core_led_auto(-1);
    case LED_LEVEL: return core_led_brightness(-1);
    case SOUND: return core_sound(-1);
    case WIFI_MOTION: return wifimotion_enable(-1);
    case WIFI_SENS: return wifimotion_sensitivity(-1);
    case UPDATES: return update_channel(-1);
    case ARB: return arb_arbitrate(-1);
    case SS_UNPAIRED: return sendspin_unpaired(-1);
    case EQ_BASS: case EQ_MID: case EQ_TREBLE: return core_eq((int)(s - table) - EQ_BASS);
    case BT_AUDIO: return bt_audio;
    case BT_SPEAKER: return bt_speaker;
    case WHISPER: return whisper;
    default: return 0;
    }
}

/* the module's own setter; saving and telling the protocol is the caller's */
static void put(enum id i, int v)
{
    switch (i) {
    case MIC_LEVEL: mic_level = v; core_mic_level(v); break;
    case DENOISE: core_mic_denoise(v); break;
    case WAKE_SOUND: core_wake_sound(v); break;
    case MUTE: core_soft_mute(v); break;
    case DND: core_dnd(v); break;
    case BT_ANNOUNCE: core_bt_announce(v); break;
    case BT_LANG: bt_lang = v; break;
    case LED_AUTO: core_led_auto(v); break;
    case LED_LEVEL: core_led_brightness(v); break;              /* a level of its own: auto off, or it moves */
    case SOUND: core_sound(v); break;
    case WIFI_MOTION: wifimotion_enable(v); break;
    case WIFI_SENS: wifimotion_sensitivity(v); break;
    case UPDATES: update_channel(v); break;
    case ARB: arb_arbitrate(v); break;                          /* arb.c keeps it */
    case SS_UNPAIRED: sendspin_unpaired(v); break;              /* sendspin.c keeps it */
    case EQ_BASS: case EQ_MID: case EQ_TREBLE: core_set_eq(i - EQ_BASS, v); break;     /* the mixer keeps it */
    case BT_AUDIO: bt_audio = v; if (!v) a2dp_pair(0); break;                        /* paired phones still connect */
    case BT_SPEAKER: bt_speaker = v; if (!v && a2dp_out_enabled()) a2dp_out_enable(0); break;   /* back on the Echo */
    case WHISPER: whisper = v; core_whisper_enable(v); break;
    default: break;
    }
}

/* text to a value: -1 if it is not one of s's */
static int parse(const struct setting *s, const char *v, int *out)
{
    const char *const *names; char *end; long n;
    switch (s->type) {
    case S_BOOL:
        if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true")) { *out = 1; return 0; }
        if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false")) { *out = 0; return 0; }
        return -1;
    case S_INT:
        n = strtol(v, &end, 10);
        if (!*v || *end || n < s->min || n > s->max) return -1;
        *out = (int)n; return 0;
    case S_CHOICE:
        for (int i = 0, k = settings_choices(s, &names); i < k; i++) if (!strcasecmp(v, names[i])) { *out = i; return 0; }
        if (s - table == DENOISE) for (int i = 0; i < 4; i++) if (!strcasecmp(v, denoise_names[i])) { *out = i; return 0; }
        if (s - table == BT_LANG) for (int i = 0; i < bt_lang_count; i++) if (!strcmp(v, bt_langs[i].name)) { *out = i; return 0; }
        return -1;
    }
    return -1;
}

static size_t format(const struct setting *s, char *out, size_t cap)
{
    const char *const *names; int v = settings_get(s);
    if (s->type == S_BOOL) return (size_t)snprintf(out, cap, "%s", v ? "on" : "off");
    if (s->type == S_CHOICE) { int k = settings_choices(s, &names); return (size_t)snprintf(out, cap, "%s", v >= 0 && v < k ? names[v] : ""); }
    return (size_t)snprintf(out, cap, "%d", v);
}

/* ---------------------------------------------------------------- file */

/* What state/config holds: the settings no other module keeps */
static int ours(enum id i) { return i != ARB && i != SS_UNPAIRED && i != EQ_BASS && i != EQ_MID && i != EQ_TREBLE; }

void settings_save(void)
{
    char p[300], tmp[310], v[48]; FILE *f;
    path(p, sizeof p, "config"); snprintf(tmp, sizeof tmp, "%s.tmp", p);
    if (!(f = fopen(tmp, "w"))) { fprintf(stderr, "settings: cannot write %s\n", tmp); return; }
    fprintf(f, "# hassmic settings: name=value; the web page and Home Assistant write it\n");
    for (int i = 0; i < NSET; i++) if (ours(i) && present(i)) { format(&table[i], v, sizeof v); fprintf(f, "%s=%s\n", table[i].name, v); }
    if (fclose(f) || rename(tmp, p)) { remove(tmp); fprintf(stderr, "settings: cannot write %s\n", p); }
}

/* The file before names (one line of numbers): 5 fields (before Bluetooth announcements), 6 (before do not disturb), 7
 * (before their language), 8 (before the mic level: the first three fields held noise suppression, auto gain and volume
 * multiplier for Home Assistant, which ignored them; unused since), 9 (before LED brightness: auto, as stock), 11 (before
 * sound detection: off), 12 (before Wi-Fi motion: off, default sensitivity), 14 (before online updates: off). */
static int load_old(void)
{
    int n, g, m, w, a = 1, d = 0, fmt = 0, la = 1, lb = -1, sd = 0, wm = 0, ws = WIFIMOTION_SENS_DEFAULT, uc = UPDATE_OFF; float v; char l[8] = "", p[300];
    FILE *f; path(p, sizeof p, "settings");
    if (getenv("HASSMIC_SETTINGS")) snprintf(p, sizeof p, "%s", getenv("HASSMIC_SETTINGS"));      /* tests */
    if (!(f = fopen(p, "r"))) return 0;
    if (fscanf(f, "%d %d %f %d %d %d %d %7s %d %d %d %d %d %d %d", &n, &g, &v, &m, &w, &a, &d, l, &fmt, &la, &lb, &sd, &wm, &ws, &uc) >= 5) {
        if (fmt == 2) { mic_level = g < MICGAIN_LEVEL_MIN ? MICGAIN_LEVEL_MIN : g > MICGAIN_LEVEL_MAX ? MICGAIN_LEVEL_MAX : g; core_mic_denoise(n < 0 ? 0 : n > 3 ? 3 : n); }
        core_soft_mute(m != 0); core_wake_sound(w != 0); core_bt_announce(a != 0); core_dnd(d != 0);
        for (int i = 0; i < bt_lang_count; i++) if (!strcmp(l, bt_langs[i].code)) bt_lang = i;     /* the code, not the index: the list may grow */
        if (!la) { if (lb >= 0) core_led_brightness(lb); else core_led_auto(0); }  /* ledcontroller started its auto at boot */
        core_sound(sd != 0);
        wifimotion_enable(wm != 0); wifimotion_sensitivity(ws);
        update_channel(uc);
    }
    fclose(f);
    return 1;
}

void settings_load(void)
{
    static int loaded; char p[300], line[160]; FILE *f; int led_auto = 1, led_level = -1;
    if (loaded) return;
    loaded = 1;
    path(p, sizeof p, "config");
    if (!(f = fopen(p, "r"))) {
        /* an Echo from before features: what it showed in Home Assistant stays (the speaker if one played) */
        bt_speaker = ble_present() && a2dp_out_enabled();
        if (load_old()) { settings_save(); fprintf(stderr, "settings: moved to %s\n", p); }
        core_mic_level(mic_level);
        return;
    }
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='), *v; int x;
        if (line[0] == '#' || !eq) continue;
        *eq = 0; v = eq + 1; v[strcspn(v, "\r\n")] = 0;
        for (int i = 0; i < NSET; i++) {
            if (strcmp(table[i].name, line) || !ours(i) || parse(&table[i], v, &x)) continue;
            /* the ring's level only counts with auto off: ledcontroller started its own auto at boot */
            if (i == LED_AUTO) led_auto = x; else if (i == LED_LEVEL) led_level = x; else put(i, x);
        }
    }
    fclose(f);
    if (!led_auto) { if (led_level >= 0) core_led_brightness(led_level); else core_led_auto(0); }
    core_mic_level(mic_level);
}

/* A preset (an export from another Echo's settings page) that scripts/setup.sh left in state/preset: applied once, at
 * start, as an import would be, then kept as state/preset.applied */
void settings_preset(void)
{
    char p[300], done[310], err[2048]; FILE *f; long n; char *t;
    path(p, sizeof p, "preset"); snprintf(done, sizeof done, "%s.applied", p);
    if (!(f = fopen(p, "r"))) return;
    fseek(f, 0, SEEK_END); n = ftell(f); rewind(f);
    if (n < 0 || n > 65536 || !(t = calloc(1, (size_t)n + 1))) { fclose(f); fprintf(stderr, "settings: preset %s unreadable, ignored\n", p); return; }
    if (fread(t, 1, (size_t)n, f) != (size_t)n) n = 0;
    fclose(f);
    int k = settings_apply_text(t, err, sizeof err);
    fprintf(stderr, "settings: preset applied, %d setting%s%s%s", k, k == 1 ? "" : "s", err[0] ? "; not taken:\n" : "\n", err);
    free(t);
    if (rename(p, done)) remove(p);                     /* once only, whatever happened */
}

int settings_mic_level(void) { return mic_level; }
int settings_on(const char *name) { const struct setting *s = settings_find(name); return s && s->type == S_BOOL && settings_get(s); }
const struct bt_lang *settings_bt_lang(void) { return &bt_langs[bt_lang]; }
int settings_bt_lang_index(int set) { if (set >= 0 && set < bt_lang_count) bt_lang = set; return bt_lang; }

/* ---------------------------------------------------------------- by name */

int settings_set(const char *name, const char *value, char *err, size_t errsz)
{
    const struct setting *s = settings_find(name); int v;
    if (!s) { snprintf(err, errsz, "%s: no such setting on this Echo", name); return -1; }
    if (parse(s, value, &v)) { snprintf(err, errsz, "%s: \"%s\" is not a value it takes", name, value); return -1; }
    int was = settings_get(s);
    put((enum id)(s - table), v);
    if (ours((enum id)(s - table))) settings_save();
    core_settings_changed();
    if (s->feature && was != v) core_entities_changed();          /* Home Assistant reads the list again */
    return 0;
}

size_t settings_text(char *out, size_t cap, int export_only)
{
    size_t n = 0; char v[48];
    index_table();
    for (int i = 0; i < nids && n < cap; i++) {
        const struct setting *s = &table[ids[i]];
        if (export_only && !s->export_) continue;
        /* a level fixes it (auto off): with auto on, the level is only what the room made it */
        if (export_only && ids[i] == LED_LEVEL && present(LED_AUTO) && core_led_auto(-1)) continue;
        format(s, v, sizeof v);
        n += (size_t)snprintf(out + n, cap - n, "%s=%s\n", s->name, v);
    }
    return n < cap ? n : cap;
}

int settings_apply_text(const char *text, char *err, size_t errsz)
{
    int applied = 0; size_t en = 0; const char *p = text;
    if (errsz) err[0] = 0;
    while (*p) {
        char line[200], *eq, *v; size_t l = strcspn(p, "\n");
        snprintf(line, sizeof line, "%.*s", (int)(l < sizeof line - 1 ? l : sizeof line - 1), p);
        p += l + (p[l] == '\n');
        line[strcspn(line, "\r")] = 0;
        char *k = line; while (isspace((unsigned char)*k)) k++;
        if (!*k || *k == '#' || !(eq = strchr(k, '='))) continue;
        *eq = 0; v = eq + 1;
        char e[160];
        if (!settings_set(k, v, e, sizeof e)) applied++;
        else if (en < errsz) en += (size_t)snprintf(err + en, errsz - en, "%s\n", e);
    }
    return applied;
}
