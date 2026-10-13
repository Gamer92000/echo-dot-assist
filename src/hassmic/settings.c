#include "settings.h"
#include "core.h"
#include "a2dp.h"
#include "arb.h"
#include "dropin.h"
#include "https.h"
#include "ble.h"
#include "btout.h"
#include "micgain.h"
#include "ring.h"
#include "sendspin.h"
#include "sound.h"
#include "spkgain.h"
#include "update.h"
#include "wifimotion.h"
#include "statefile.h"
#include "ring.h"
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const denoise_names[4] = { "Off", "Low", "Medium", "High" };
static const char *const denoise_values[4] = { "off", "low", "medium", "high" };
static const char *const arb_modes[2] = { "hassmic", "kiosk" };            /* ARB_HASSMIC, ARB_KIOSK */
static const char *const wake_engines[3] = { "amazon", "microwakeword", "homeassistant" };  /* WAKE_AMAZON, WAKE_MWW, WAKE_HA */
static const char *const dropin_answers[2] = { "auto", "ask" };          /* dropin_ask() 0, 1 */

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
static int light_sensor;                                /* off: auto brightness moved the level in HA's history all day (forum, 2026-10-15) */
static int bt_stay;                                     /* a2dp.c gets it only while the feature is on: off drops the link */
static int wake_events;                                 /* off: an event per near miss is HA history most homes never read */
static int request_text;                                /* off: what people say kept in HA's history is theirs to choose */

static const char *state_dir(void) { const char *e = getenv("HASSMIC_STATE"); return e ? e : "/data/local/hassmic/state"; }
static void path(char *out, size_t cap, const char *file) { snprintf(out, cap, "%s/%s", state_dir(), file); }

/* ---------------------------------------------------------------- the table */

enum id { WAKE_ENGINE, MIC_LEVEL, DENOISE, WAKE_SOUND, WAKE_SOUND_REMOVAL, MUTE, DND, TIMER_RING, TIMER_STOP_ANY, STOP_ENDS, BT_ANNOUNCE, BT_LANG, LED_AUTO, LED_LEVEL, RING_ROT, BAND, ARB, ARB_MODE, ARB_AUTO, ARB_WINDOW, ARB_OFFSET, SOUND, WHISPER, WAKE_EVENTS, REQUEST_TEXT, WIFI_MOTION,
          WIFI_SENS, LIGHT_SENSOR, BT_AUDIO, BT_SPEAKER, BT_OUT_DELAY, BT_OUT_AWAKE, BT_OUT_STAY, DROPIN, DROPIN_ANSWER, UPDATES, HTTPS_VERIFY, SS_UNPAIRED, EQ_BASS, EQ_MID, EQ_TREBLE, SPK_GAIN, VOLUME, VOL_SPLIT,
          ASSIST_LINK, ASSIST_LEVEL, ASSIST_OFFSET, SOUNDS_LINK, SOUNDS_LEVEL, SOUNDS_OFFSET, TIMER_LINK, TIMER_LEVEL, TIMER_OFFSET,
          CALL_LINK, CALL_LEVEL, CALL_OFFSET, NSET };

/* Features ("Features" group, feature = 1): Home Assistant lists their entities only while they are on (proto_esphome.c
 * listed()); everything else here is on the settings page only, except what Home Assistant always shows (wake sound,
 * mute, do not disturb, LEDs, equalizer). */
static const struct setting table[NSET] = {
    /* main.c keeps it; microWakeWord's models are in state/mww (mww_store.c), not in an export */
    [WAKE_ENGINE] = { "wake_engine", "Wake word engine", "Wake word", NULL, S_CHOICE, 0, 0, 1, 0 },
    [MIC_LEVEL]   = { "mic_level", "Mic level", "Voice", "dBFS", S_INT, MICGAIN_LEVEL_MIN, MICGAIN_LEVEL_MAX, 1, 0 },
    [DENOISE]     = { "noise_reduction", "Noise reduction", "Voice", NULL, S_CHOICE, 0, 0, 1, 0 },
    [WAKE_SOUND]  = { "wake_sound", "Wake sound", "Voice", NULL, S_BOOL, 0, 1, 1, 0 },
    /* earsup.c; experimental: a card of the page's Features, but no feature: it has no entities, and switching a
     * feature makes Home Assistant reconnect */
    [WAKE_SOUND_REMOVAL] = { "wake_sound_removal", "Wake sound removal (experimental)", "Features", NULL, S_BOOL, 0, 1, 1, 0 },
    [MUTE]        = { "mute", "Mute", "Voice", NULL, S_BOOL, 0, 1, 0, 0 },            /* what the Echo is doing right now */
    [DND]         = { "do_not_disturb", "Do not disturb", "Voice", NULL, S_BOOL, 0, 1, 0, 0 },
    [TIMER_RING]  = { "timer_ring", "Timers ring for", "Voice", "s", S_INT, 0, 600, 1, 0 },     /* 0: until stopped */
    [TIMER_STOP_ANY] = { "timer_stop_any", "Any request stops a ringing timer", "Voice", NULL, S_BOOL, 0, 1, 1, 0 },
    [STOP_ENDS]   = { "stop_ends_listening", "Stop after the wake word ends listening", "Voice", NULL, S_BOOL, 0, 1, 1, 0 },
    [BT_ANNOUNCE] = { "bluetooth_announcements", "Bluetooth announcements", "Bluetooth", NULL, S_BOOL, 0, 1, 1, 0 },
    [BT_LANG]     = { "bluetooth_announcement_language", "Announcement language", "Bluetooth", NULL, S_CHOICE, 0, 0, 1, 0 },
    [LED_AUTO]    = { "led_auto_brightness", "LED auto brightness", "Lights", NULL, S_BOOL, 0, 1, 1, 0 },
    [LED_LEVEL]   = { "led_brightness", "LED brightness", "Lights", "%", S_INT, 0, 100, 1, 0 },
    /* ring.c keeps it: where the light's fill starts.  Not exported: it is for how this Echo stands */
    [RING_ROT]    = { "ring_rotation", "Light ring rotation", "Lights", "LEDs", S_INT, 0, 23, 0, 0 },   /* settings_max() */
    /* ring_band.c: the screen's rows kept for the light band (stock's attention line is 10) */
    [BAND]        = { "light_band", "Light band", "Lights", "px", S_INT, 0, 40, 1, 0 },
    [ARB]         = { "arbitration", "Wake word arbitration with other Echos", "Features", NULL, S_BOOL, 0, 1, 1, 1 },
    /* arb.c keeps them in memory, state/config on disk: lockdown.sh admits Kiosk Satellite's port by it */
    [ARB_MODE]    = { "arbitration_mode", "Arbitration mode", "Features", NULL, S_CHOICE, 0, 0, 1, 0 },
    [ARB_AUTO]    = { "arbitration_window_auto", "Learn the wait from the network", "Features", NULL, S_BOOL, 0, 1, 1, 0 },
    [ARB_WINDOW]  = { "arbitration_window", "Arbitration window", "Features", "ms", S_INT, 100, 1000, 1, 0 },
    [ARB_OFFSET]  = { "arbitration_offset", "Kiosk Satellite loudness offset", "Features", "dB", S_INT, -20, 20, 1, 0 },
    [SOUND]       = { "sound_detection", "Sound detection", "Features", NULL, S_BOOL, 0, 1, 1, 1 },
    [WHISPER]     = { "whisper_detection", "Whisper detection", "Features", NULL, S_BOOL, 0, 1, 1, 1 },
    /* proto_esphome.c's "Wake word" event entity (core_wake_events, issue #48) */
    [WAKE_EVENTS] = { "wake_events", "Wake word events", "Features", NULL, S_BOOL, 0, 1, 1, 1 },
    /* proto_esphome.c's "Last heard" and "Last reply" text sensors (issue #53) */
    [REQUEST_TEXT] = { "request_text", "Request and reply text", "Features", NULL, S_BOOL, 0, 1, 1, 1 },
    [WIFI_MOTION] = { "wifi_motion", "Wi-Fi motion (experimental)", "Features", NULL, S_BOOL, 0, 1, 1, 1 },
    [WIFI_SENS]   = { "wifi_motion_sensitivity", "Wi-Fi motion sensitivity", "Features", NULL, S_INT, WIFIMOTION_SENS_MIN, WIFIMOTION_SENS_MAX, 1, 0 },
    [LIGHT_SENSOR] = { "light_sensor", "Light sensor", "Features", NULL, S_BOOL, 0, 1, 1, 1 },
    [BT_AUDIO]    = { "bluetooth_audio", "Bluetooth audio from phones", "Features", NULL, S_BOOL, 0, 1, 1, 1 },
    [BT_SPEAKER]  = { "bluetooth_speaker", "Play on a Bluetooth speaker", "Features", NULL, S_BOOL, 0, 1, 1, 1 },
    /* what the speaker adds to the Echo's latency, for Sendspin's sync: a2dp.c keeps it (state/bt_speaker), per speaker
     * and Echo, so not exported */
    [BT_OUT_DELAY] = { "bluetooth_speaker_delay", "Bluetooth speaker delay", "Features", "ms", S_INT, 0, 1000, 0, 0 },
    /* btout.c keeps it; a fix for one speaker, so not exported either */
    [BT_OUT_AWAKE] = { "bluetooth_speaker_keep_awake", "Keep the speaker awake", "Features", NULL, S_BOOL, 0, 1, 0, 0 },
    /* a2dp.c keeps it; a battery speaker or a mains one, so per speaker as well */
    [BT_OUT_STAY] = { "bluetooth_speaker_stay_connected", "Stay connected when off", "Features", NULL, S_BOOL, 0, 1, 0, 0 },
    /* dropin.c keeps them in memory, state/config on disk */
    [DROPIN]      = { "drop_in", "Drop In", "Features", NULL, S_BOOL, 0, 1, 1, 1 },
    [DROPIN_ANSWER] = { "drop_in_answer", "Drop In answers", "Features", NULL, S_CHOICE, 0, 0, 1, 0 },
    [UPDATES]     = { "online_updates", "Online updates", "System", NULL, S_CHOICE, 0, 0, 1, 0 },
    /* https.c keeps it: media from a Home Assistant that serves HTTPS itself (issue #30) */
    [HTTPS_VERIFY] = { "https_verify", "Check HTTPS certificates", "System", NULL, S_BOOL, 0, 1, 1, 0 },
    [SS_UNPAIRED] = { "sendspin_unpaired", "Music Assistant without pairing", "Music", NULL, S_BOOL, 0, 1, 1, 0 },
    [EQ_BASS]     = { "equalizer_bass", "Equalizer bass", "Sound", "dB", S_INT, -6, 6, 1, 0 },
    [EQ_MID]      = { "equalizer_mid", "Equalizer mid", "Sound", "dB", S_INT, -6, 6, 1, 0 },
    [EQ_TREBLE]   = { "equalizer_treble", "Equalizer treble", "Sound", "dB", S_INT, -6, 6, 1, 0 },
    /* over stock (spkgain.c); not exported: speakers of one model differ (one Dot 2 is clean where another is too loud) */
    [SPK_GAIN]    = { "speaker_gain", "Speaker gain", "Sound", "dB", S_INT, 0, SPKGAIN_MAX, 0, 0 },  /* settings_max() */
    /* the page draws these as one card of its own (web/app.js buildVolumes).  The volume itself (music's, with separate
     * volumes) is the mixer's or main.c's, not state/config's, and no export's: a volume is for the room it is in */
    [VOLUME]      = { "volume", "Volume", "Sound", "%", S_INT, 0, 100, 0, 0 },
    /* main.c keeps them (issue #32), three per kind in VOL_* order.  Each after what switches it: separate volumes on
     * starts the levels at the volume, following the music starts the offset where the level was and leaving it the
     * level where it played; saved or imported values then win.  Following the music is a feature: the kind's own
     * number leaves Home Assistant, the media player's volume moves it.  Exported: the switches and the offsets, how
     * the volumes relate; not a kind's own volume, which like the volume is for the room it is in */
    [VOL_SPLIT]   = { "separate_volumes", "Separate volumes", "Sound", NULL, S_BOOL, 0, 1, 1, 1 },
    [ASSIST_LINK] = { "assistant_volume_linked", "Voice replies follow the music volume", "Sound", NULL, S_BOOL, 0, 1, 1, 1 },
    [ASSIST_LEVEL] = { "assistant_volume", "Voice replies volume", "Sound", "%", S_INT, 0, 100, 0, 0 },
    [ASSIST_OFFSET] = { "assistant_volume_offset", "Voice replies above the music", "Sound", NULL, S_INT, -VOL_OFFSET_MAX, VOL_OFFSET_MAX, 1, 0 },
    [SOUNDS_LINK] = { "sounds_volume_linked", "Echo sounds follow the music volume", "Sound", NULL, S_BOOL, 0, 1, 1, 1 },
    [SOUNDS_LEVEL] = { "sounds_volume", "Echo sounds volume", "Sound", "%", S_INT, 0, 100, 0, 0 },
    [SOUNDS_OFFSET] = { "sounds_volume_offset", "Echo sounds above the music", "Sound", NULL, S_INT, -VOL_OFFSET_MAX, VOL_OFFSET_MAX, 1, 0 },
    [TIMER_LINK]  = { "timer_volume_linked", "Timers follow the music volume", "Sound", NULL, S_BOOL, 0, 1, 1, 1 },
    [TIMER_LEVEL] = { "timer_volume", "Timer volume", "Sound", "%", S_INT, 0, 100, 0, 0 },
    [TIMER_OFFSET] = { "timer_volume_offset", "Timers above the music", "Sound", NULL, S_INT, -VOL_OFFSET_MAX, VOL_OFFSET_MAX, 1, 0 },
    [CALL_LINK]   = { "call_volume_linked", "Calls follow the music volume", "Sound", NULL, S_BOOL, 0, 1, 1, 1 },
    [CALL_LEVEL]  = { "call_volume", "Call volume", "Sound", "%", S_INT, 0, 100, 0, 0 },
    [CALL_OFFSET] = { "call_volume_offset", "Calls above the music", "Sound", NULL, S_INT, -VOL_OFFSET_MAX, VOL_OFFSET_MAX, 1, 0 },
};

static int present(enum id i)
{
    switch (i) {
    case BT_ANNOUNCE: case BT_LANG: case BT_AUDIO: case BT_SPEAKER: case BT_OUT_DELAY: case BT_OUT_AWAKE: case BT_OUT_STAY: return ble_present();
    case WHISPER: return core_whisper_model();
    case SOUND: return sound_model() != SOUND_NONE;               /* the firmware has one on every model so far */
    case LIGHT_SENSOR: return core_lux() == core_lux();          /* a light sensor: not NAN */
    case LED_AUTO: return core_led_has_auto();
    case WIFI_MOTION: case WIFI_SENS: return wifimotion_present();
    case ARB: case ARB_MODE: case ARB_AUTO: case ARB_WINDOW: case ARB_OFFSET: return arb_running();
    case DROPIN: case DROPIN_ANSWER: return dropin_running();
    case SS_UNPAIRED: return core_sendspin_port != 0;
    case SPK_GAIN: return spkgain_present();
    case BAND: return ring_band_rows(-1) >= 0;
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

/* the speaker gain's top is the control's: +12 dB on an Echo Dot 2, +2 on an Echo Dot 3; the rotation's, the ring's last LED */
int settings_max(const struct setting *s)
{
    return s - table == SPK_GAIN ? spkgain_max() : s - table == RING_ROT ? ring_leds() - 1 : s->max;
}

int settings_choices(const struct setting *s, const char *const **names)
{
    switch (s - table) {
    case DENOISE: *names = denoise_values; return 4;
    case BT_LANG:
        for (int i = 0; i < bt_lang_count; i++) bt_lang_codes[i] = bt_langs[i].code;
        *names = bt_lang_codes; return bt_lang_count;
    case UPDATES: *names = update_channels; return 3;
    case ARB_MODE: *names = arb_modes; return 2;
    case WAKE_ENGINE: *names = wake_engines; return 3;
    case DROPIN_ANSWER: *names = dropin_answers; return 2;
    default: *names = NULL; return 0;
    }
}

int settings_get(const struct setting *s)
{
    switch (s - table) {
    case WAKE_ENGINE: return core_wake_engine(-1);
    case MIC_LEVEL: return mic_level;
    case DENOISE: return core_mic_denoise(-1);
    case WAKE_SOUND: return core_wake_sound(-1);
    case WAKE_SOUND_REMOVAL: return core_wake_sound_removal(-1);
    case MUTE: return core_soft_mute(-1);
    case DND: return core_dnd(-1);
    case TIMER_RING: return core_timer_ring(-1);
    case TIMER_STOP_ANY: return core_timer_stop_any(-1);
    case STOP_ENDS: return core_stop_ends_listening(-1);
    case BT_ANNOUNCE: return core_bt_announce(-1);
    case BT_LANG: return bt_lang;
    case LED_AUTO: return core_led_auto(-1);
    case LED_LEVEL: return core_led_brightness(-1);
    case RING_ROT: return ring_rotation(-1);
    case SOUND: return core_sound(-1);
    case WIFI_MOTION: return wifimotion_enable(-1);
    case WIFI_SENS: return wifimotion_sensitivity(-1);
    case UPDATES: return update_channel(-1);
    case HTTPS_VERIFY: return https_verify(-1);
    case ARB: return arb_arbitrate(-1);
    case ARB_MODE: return arb_mode(-1);
    case ARB_AUTO: return arb_window_auto(-1);
    case ARB_WINDOW: return arb_window(-1);
    case ARB_OFFSET: return arb_offset(INT_MIN);
    case SS_UNPAIRED: return sendspin_unpaired(-1);
    case EQ_BASS: case EQ_MID: case EQ_TREBLE: return core_eq((int)(s - table) - EQ_BASS);
    case BT_AUDIO: return bt_audio;
    case BT_SPEAKER: return bt_speaker;
    case BT_OUT_DELAY: return a2dp_out_delay(-1);
    case BT_OUT_AWAKE: return btout_keep_awake(-1);
    case BT_OUT_STAY: return bt_stay;
    case WHISPER: return whisper;
    case LIGHT_SENSOR: return light_sensor;
    case WAKE_EVENTS: return wake_events;
    case REQUEST_TEXT: return request_text;
    case DROPIN: return dropin_enable(-1);
    case DROPIN_ANSWER: return dropin_ask(-1);
    case SPK_GAIN: return spkgain_get();
    case VOLUME: return core_volume();
    case VOL_SPLIT: return core_separate_volumes(-1);
    case ASSIST_LINK: case SOUNDS_LINK: case TIMER_LINK: case CALL_LINK: return core_kind_linked((int)(s - table - ASSIST_LINK) / 3, -1);
    case ASSIST_LEVEL: case SOUNDS_LEVEL: case TIMER_LEVEL: case CALL_LEVEL: return core_kind_volume((int)(s - table - ASSIST_LINK) / 3, -1);
    case ASSIST_OFFSET: case SOUNDS_OFFSET: case TIMER_OFFSET: case CALL_OFFSET: return core_kind_offset((int)(s - table - ASSIST_LINK) / 3);
    case BAND: return ring_band_rows(-1);
    default: return 0;
    }
}

/* the module's own setter; saving and telling the protocol is the caller's */
static void put(enum id i, int v)
{
    switch (i) {
    case WAKE_ENGINE: core_wake_engine(v); break;
    case MIC_LEVEL: mic_level = v; core_mic_level(v); break;
    case DENOISE: core_mic_denoise(v); break;
    case WAKE_SOUND: core_wake_sound(v); break;
    case WAKE_SOUND_REMOVAL: core_wake_sound_removal(v); break;
    case MUTE: core_soft_mute(v); break;
    case DND: core_dnd(v); break;
    case TIMER_RING: core_timer_ring(v); break;
    case TIMER_STOP_ANY: core_timer_stop_any(v); break;
    case STOP_ENDS: core_stop_ends_listening(v); break;
    case BT_ANNOUNCE: core_bt_announce(v); break;
    case BT_LANG: bt_lang = v; break;
    case LED_AUTO: core_led_auto(v); break;
    case LED_LEVEL: core_led_brightness(v); break;              /* a level of its own: auto off, or it moves */
    case RING_ROT: ring_rotation(v); break;
    case SOUND: core_sound(v); break;
    case WIFI_MOTION: wifimotion_enable(v); break;
    case WIFI_SENS: wifimotion_sensitivity(v); break;
    case UPDATES: update_channel(v); break;
    case HTTPS_VERIFY: https_verify(v); break;
    case ARB: arb_arbitrate(v); break;                          /* arb.c keeps it */
    case ARB_MODE: arb_mode(v); break;
    case ARB_AUTO: arb_window_auto(v); break;
    case ARB_WINDOW: arb_window(v); break;
    case ARB_OFFSET: arb_offset(v); break;
    case SS_UNPAIRED: sendspin_unpaired(v); break;              /* sendspin.c keeps it */
    case EQ_BASS: case EQ_MID: case EQ_TREBLE: core_set_eq(i - EQ_BASS, v); break;     /* the mixer keeps it */
    case BT_AUDIO: bt_audio = v; if (!v) a2dp_pair(0); break;                        /* paired phones still connect */
    case BT_SPEAKER:                                            /* back on the Echo, and the link goes */
        bt_speaker = v; a2dp_out_stay(bt_stay && v); if (!v && a2dp_out_enabled()) a2dp_out_enable(0); break;
    case WHISPER: whisper = v; core_whisper_enable(v); break;
    case LIGHT_SENSOR: light_sensor = v; break;
    case WAKE_EVENTS: wake_events = v; break;
    case REQUEST_TEXT: request_text = v; break;
    case BT_OUT_DELAY: a2dp_out_delay(v); break;                    /* a2dp.c saves it */
    case BT_OUT_AWAKE: btout_keep_awake(v); break;
    case BT_OUT_STAY: bt_stay = v; a2dp_out_stay(v && bt_speaker); break;
    case DROPIN: dropin_enable(v); break;
    case DROPIN_ANSWER: dropin_ask(v); break;
    case SPK_GAIN: spkgain_set(v); break;
    case VOLUME: core_set_volume(v); break;
    case VOL_SPLIT: core_separate_volumes(v); break;
    case ASSIST_LINK: case SOUNDS_LINK: case TIMER_LINK: case CALL_LINK: core_kind_linked((i - ASSIST_LINK) / 3, v); break;
    case ASSIST_LEVEL: case SOUNDS_LEVEL: case TIMER_LEVEL: case CALL_LEVEL: core_kind_volume((i - ASSIST_LINK) / 3, v); break;
    case ASSIST_OFFSET: case SOUNDS_OFFSET: case TIMER_OFFSET: case CALL_OFFSET: core_set_kind_offset((i - ASSIST_LINK) / 3, v); break;
    case BAND: ring_band_rows(v); break;
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
        if (!*v || *end || n < s->min || n > settings_max(s)) return -1;
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

/* Applied before the rest, whatever order a file has them in (state/config by hand, an import): switching seeds what
 * comes after it (separate volumes the levels, following the music the offset), and the saved values must then win */
static int switches(int i) { return i == VOL_SPLIT || i == ASSIST_LINK || i == SOUNDS_LINK || i == TIMER_LINK || i == CALL_LINK; }

/* What state/config holds: the settings no other module keeps */
static int ours(enum id i) { return i != ARB && i != SS_UNPAIRED && i != EQ_BASS && i != EQ_MID && i != EQ_TREBLE && i != BT_OUT_DELAY && i != VOLUME; }

void settings_save(void)
{
    char p[300], v[48]; struct statefile s;
    path(p, sizeof p, "config");
    if (statefile_open(&s, p, 0666)) { fprintf(stderr, "settings: cannot write %s.tmp\n", p); return; }
    fprintf(s.f, "# hassmic settings: name=value; the web page and Home Assistant write it\n");
    for (int i = 0; i < NSET; i++) if (ours(i) && present(i)) { format(&table[i], v, sizeof v); fprintf(s.f, "%s=%s\n", table[i].name, v); }
    if (statefile_commit(&s)) fprintf(stderr, "settings: cannot write %s\n", p);
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
    for (int pass = 0; pass < 2; pass++, rewind(f)) while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='), *v; int x;
        if (line[0] == '#' || !eq) continue;
        *eq = 0; v = eq + 1; v[strcspn(v, "\r\n")] = 0;
        for (int i = 0; i < NSET; i++) {
            if (strcmp(table[i].name, line) || !ours(i) || switches(i) != !pass || parse(&table[i], v, &x)) continue;
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

int settings_peek(const char *name, char *out, size_t cap)
{
    char p[300], line[160]; FILE *f; size_t k = strlen(name); int r = -1, x;
    const struct setting *s = NULL;
    for (int i = 0; i < NSET; i++) if (!strcmp(table[i].name, name)) s = &table[i];
    path(p, sizeof p, "config");
    if (!(f = fopen(p, "r"))) return -1;
    /* the last one it takes, as settings_load ends up with (a line added by hand; one it refuses changes nothing) */
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, name, k) && line[k] == '=') {
            line[strcspn(line, "\r\n")] = 0;
            if (!s || !parse(s, line + k + 1, &x)) { snprintf(out, cap, "%s", line + k + 1); r = 0; }
        }
    fclose(f);
    return r;
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
    if (s - table == WAKE_ENGINE && settings_get(s) != v) { snprintf(err, errsz, "%s: %s", name, core_wake_refused()); return -1; }
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
    int applied = 0; size_t en = 0;
    if (errsz) err[0] = 0;
    for (int pass = 0; pass < 2; pass++) for (const char *p = text; *p; ) {
        char line[200], *eq, *v; size_t l = strcspn(p, "\n");
        snprintf(line, sizeof line, "%.*s", (int)(l < sizeof line - 1 ? l : sizeof line - 1), p);
        p += l + (p[l] == '\n');
        line[strcspn(line, "\r")] = 0;
        char *k = line; while (isspace((unsigned char)*k)) k++;
        if (!*k || *k == '#' || !(eq = strchr(k, '='))) continue;
        *eq = 0; v = eq + 1;
        const struct setting *s = settings_find(k);
        if ((s && switches((int)(s - table))) != !pass) continue;      /* switches first */
        char e[160];
        if (!settings_set(k, v, e, sizeof e)) applied++;
        else if (en < errsz) en += (size_t)snprintf(err + en, errsz - en, "%s\n", e);
    }
    return applied;
}
