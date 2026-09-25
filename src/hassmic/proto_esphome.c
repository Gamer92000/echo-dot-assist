/*
 * ESPHome native API, server side: Home Assistant's esphome integration connects to us and sees a voice assistant device
 * with a media player.  Message numbers and fields follow esphome/components/api/api.proto.
 *
 *   encryption       Noise like ESPHome's `api: encryption:` without a key in the YAML.  The device starts without a key;
 *                    Home Assistant generates one and sets it (NoiseEncryptionSetKeyRequest over a Noise connection with
 *                    the all-zero PSK, so it cannot be sniffed), then connects with it.  From then on only that key gets
 *                    in.  Deleting the device in Home Assistant sends an empty key, which starts over; so does deleting
 *                    state/api_key.
 *
 *   voice pipeline   VoiceAssistantRequest -> mic audio as VoiceAssistantAudio (API_AUDIO) -> events.  The reply is fetched
 *                    over http like an announcement: without the SPEAKER flag Home Assistant renders TTS in the media
 *                    player's announcement format (48 kHz) instead of streaming 16 kHz over the API connection.
 *   announcements    VoiceAssistantAnnounceRequest with http URLs; Home Assistant transcodes to the WAV format the media
 *                    player entity advertises, we stream it.  Same path for media_player.play_media.
 *   timers           finished timer rings (core_alarm)
 *   do not disturb   a switch; announcements are dropped while it is on
 *   bluetooth proxy  LE scanning with raw advertisements, GATT connections to up to 3 devices at a time, pairing (ble.c)
 *   bluetooth speaker  a switch opens the pairing window (a2dp.c).  A phone connecting is announced by asking Home
 *                    Assistant to run assist_satellite.announce on us (HomeassistantActionRequest, what an ESPHome YAML
 *                    `homeassistant.action` sends).  Home Assistant only runs it with "Allow the device to perform Home
 *                    Assistant actions" ticked in the device's options; otherwise it raises a repair saying so.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdatomic.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "a2dp.h"
#include "ble.h"
#include "core.h"
#include "hash.h"
#include "noise.h"
#include "sendspin.h"
#include "net.h"
#include "netio.h"
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD                 /* plain C: same code on the PC and on the armv7 build */
#include "../third_party/minimp3.h"

enum {
    HELLO_REQ = 1, HELLO_RESP, CONNECT_REQ, CONNECT_RESP, DISCONNECT_REQ, DISCONNECT_RESP, PING_REQ, PING_RESP,
    DEVICE_INFO_REQ, DEVICE_INFO_RESP, LIST_ENTITIES_REQ, LIST_ENTITIES_DONE = 19, SUBSCRIBE_STATES = 20,
    SUBSCRIBE_HA_ACTIONS = 34, HA_ACTION,
    LIST_SENSOR = 16, LIST_SWITCH = 17, LIST_TEXT_SENSOR = 18, SENSOR_STATE = 25, SWITCH_STATE = 26, TEXT_SENSOR_STATE = 27, SWITCH_COMMAND = 33, LIST_NUMBER = 49, NUMBER_STATE, NUMBER_COMMAND,
    LIST_SELECT = 52, SELECT_STATE, SELECT_COMMAND,
    LIST_MEDIA_PLAYER = 63, MEDIA_PLAYER_STATE, MEDIA_PLAYER_COMMAND,
    SUBSCRIBE_VA = 89, VA_REQUEST, VA_RESPONSE, VA_EVENT, VA_AUDIO = 106, VA_TIMER_EVENT = 115,
    VA_ANNOUNCE = 119, VA_ANNOUNCE_FINISHED, VA_CONFIG_REQ, VA_CONFIG_RESP, VA_SET_CONFIG, SET_KEY_REQ = 124, SET_KEY_RESP,
    BLE_SUBSCRIBE = 66, BLE_DEVICE_REQ = 68, BLE_CONNECTION, BLE_SERVICES_REQ, BLE_SERVICES, BLE_SERVICES_DONE, BLE_READ_REQ,
    BLE_READ, BLE_WRITE_REQ, BLE_READ_DESC_REQ, BLE_WRITE_DESC_REQ, BLE_NOTIFY_REQ, BLE_NOTIFY_DATA, BLE_CONN_FREE_REQ,
    BLE_CONN_FREE, BLE_GATT_ERROR, BLE_WRITTEN, BLE_NOTIFY, BLE_PAIRED, BLE_UNPAIRED, BLE_UNSUBSCRIBE, BLE_CACHE_CLEARED,
    BLE_RAW_ADV = 93, BLE_SCANNER_STATE = 126, BLE_SCANNER_SET_MODE = 127,
};
/* Proxy features: passive scan, active connections, remote caching (Home Assistant keeps the GATT database and writes the
 * notification descriptors itself), pairing, raw advertisements, scanner state and mode.  Not: cache clearing (there is
 * no cache on the Echo), connection parameters.  Scanner state: idle / running; mode: passive / active.
 * Device requests: connect (0, and the v3 variants 4 and 5, all the same here), disconnect, pair, unpair, clear cache. */
enum { BLE_FEATURES = 1 | 2 | 4 | 8 | 32 | 64, BLE_IDLE = 0, BLE_RUNNING = 2, BLE_PASSIVE = 0, BLE_ACTIVE = 1 };
enum { BLE_REQ_CONNECT = 0, BLE_REQ_DISCONNECT, BLE_REQ_PAIR, BLE_REQ_UNPAIR, BLE_REQ_CLEAR_CACHE = 6 };
enum { EV_ERROR = 0, EV_RUN_START, EV_RUN_END, EV_STT_START, EV_STT_END, EV_INTENT_START, EV_INTENT_END, EV_TTS_START,
       EV_TTS_END, EV_WAKE_START, EV_WAKE_END, EV_VAD_START, EV_VAD_END, EV_TTS_STREAM_START = 98, EV_TTS_STREAM_END = 99, EV_INTENT_PROGRESS = 100 };
enum { FEAT_VOICE = 1, FEAT_SPEAKER = 2, FEAT_API_AUDIO = 4, FEAT_TIMERS = 8, FEAT_ANNOUNCE = 16, FEAT_START_CONVERSATION = 32 };
enum { KEY_NOISE = 2, KEY_GAIN, KEY_MULT, KEY_MUTE, KEY_WAKE_SOUND, KEY_SENDSPIN_TOKEN, KEY_SOC_TEMP, KEY_CPU_USAGE, KEY_BT_PAIRING,
       KEY_BT_ANNOUNCE, KEY_DND, KEY_EQ_BASS, KEY_EQ_MID, KEY_EQ_TREBLE, KEY_BT_LANG };
enum { MP_KEY = 1, MP_IDLE = 1, MP_PLAYING = 2, MP_CMD_STOP = 2, MP_CMD_MUTE = 3, MP_CMD_UNMUTE = 4 };
#define MEDIA_RATE 48000        /* what we ask Home Assistant to transcode announcements and media to: WAV mono s16 */

/* Several API clients at a time, like ESPHome firmware: Home Assistant plus e.g. a debugging client.  Replies go to the
 * client that asked, state changes to every client that subscribed to states, voice assistant traffic to the one
 * client that subscribed to the voice assistant (first come, first served - also like the firmware). */
#define MAX_CLIENTS 4
/* lock held.  enc: Noise frames (tx is used under the lock, rx by the client's reader only); keyed: with the device key;
 * actions: runs Home Assistant actions for us */
static struct { int fd, states, enc, keyed, ble, ble_free, ble_user, actions; struct noise_cs tx, rx; } clients[MAX_CLIENTS] = { { .fd = -1 }, { .fd = -1 }, { .fd = -1 }, { .fd = -1 } };
static int va_fd = -1;                  /* lock held: voice assistant subscriber */
static __thread int reply_fd = -1;      /* the client whose request this thread is handling */
static int tts_expected;                /* lock held: the reply is being fetched or played; RUN_END must not end the pipeline */
static char tts_url[1024];              /* lock held: reply URL of the running pipeline (known from RUN_START with streaming TTS) */
static int announcing, media_playing;   /* lock held */
static void announce_done(void);
static atomic_int media_busy;

/* ---------------------------------------------------------------- protobuf */

struct pb { unsigned char *p; size_t n, cap; };

static void pb_raw(struct pb *b, const void *d, size_t n) { if (b->n + n <= b->cap) { memcpy(b->p + b->n, d, n); b->n += n; } }
static void pb_varint(struct pb *b, uint64_t v) { unsigned char c; do { c = v & 0x7f; v >>= 7; if (v) c |= 0x80; pb_raw(b, &c, 1); } while (v); }
static void pb_uint(struct pb *b, int f, uint64_t v) { if (v) { pb_varint(b, f << 3); pb_varint(b, v); } }
static void pb_bytes(struct pb *b, int f, const void *d, size_t n) { if (n) { pb_varint(b, f << 3 | 2); pb_varint(b, n); pb_raw(b, d, n); } }
static void pb_str(struct pb *b, int f, const char *s) { pb_bytes(b, f, s, strlen(s)); }
static void pb_sint(struct pb *b, int f, int32_t v) { pb_uint(b, f, (uint32_t)v << 1 ^ (uint32_t)(v >> 31)); }   /* zigzag */
static void pb_fixed32(struct pb *b, int f, uint32_t v) { pb_varint(b, f << 3 | 5); pb_raw(b, &v, 4); }      /* little endian host */
static void pb_float(struct pb *b, int f, float v) { uint32_t u; memcpy(&u, &v, 4); pb_fixed32(b, f, u); }

struct pbf { unsigned field, wire; uint64_t v; const unsigned char *data; size_t len; };

static int pb_next(const unsigned char **p, const unsigned char *end, struct pbf *f)
{
    uint64_t k = 0, v = 0; int sh = 0;
    if (*p >= end) return 0;
    while (*p < end) { unsigned char c = *(*p)++; k |= (uint64_t)(c & 0x7f) << sh; sh += 7; if (!(c & 0x80)) break; }
    f->field = k >> 3; f->wire = k & 7; f->v = 0; f->data = NULL; f->len = 0;
    switch (f->wire) {
    case 0: for (sh = 0; *p < end;) { unsigned char c = *(*p)++; v |= (uint64_t)(c & 0x7f) << sh; sh += 7; if (!(c & 0x80)) break; } f->v = v; break;
    case 1: if (end - *p < 8) return 0; *p += 8; break;
    case 2:
        for (sh = 0; *p < end;) { unsigned char c = *(*p)++; v |= (uint64_t)(c & 0x7f) << sh; sh += 7; if (!(c & 0x80)) break; }
        if ((uint64_t)(end - *p) < v) return 0;
        f->data = *p; f->len = v; *p += v;
        break;
    case 5: if (end - *p < 4) return 0; { uint32_t u; memcpy(&u, *p, 4); f->v = u; } *p += 4; break;
    default: return 0;
    }
    return 1;
}

static void pbf_str(const struct pbf *f, char *out, size_t outsz)
{
    size_t n = f->len < outsz - 1 ? f->len : outsz - 1;
    memcpy(out, f->data, n); out[n] = 0;
}

static int client_of(int fd)            /* lock held */
{
    for (int i = 0; i < MAX_CLIENTS; i++) if (fd >= 0 && clients[i].fd == fd) return i;
    return -1;
}

/* Noise frame: 0x01, big-endian length, then a handshake message or ciphertext */
static int write_frame(int fd, const void *data, size_t len)
{
    unsigned char h[3] = { 1, len >> 8, len & 0xff };
    return len > 0xffff || write_all(fd, h, 3) < 0 || (len && write_all(fd, data, len) < 0) ? -1 : 0;
}

/* lock held */
static void send_to(int fd, unsigned type, const struct pb *b)
{
    unsigned char h[12]; struct pb hb = { h, 0, sizeof h }; size_t n = b ? b->n : 0; int c = client_of(fd), rc;
    if (fd < 0) return;
    if (c >= 0 && clients[c].enc) {                     /* inside: big-endian type and length, then the message */
        static unsigned char pt[4 + 8192], ct[sizeof pt + NOISE_TAG];
        if (4 + n > sizeof pt) { fprintf(stderr, "esphome: message %u too long (%zu)\n", type, n); return; }
        pt[0] = type >> 8; pt[1] = type; pt[2] = n >> 8; pt[3] = n;
        if (n) memcpy(pt + 4, b->p, n);
        rc = write_frame(fd, ct, noise_encrypt(&clients[c].tx, pt, 4 + n, ct));
    } else {
        pb_raw(&hb, "", 1); pb_varint(&hb, n); pb_varint(&hb, type);
        rc = write_all(fd, h, hb.n) < 0 || (n && write_all(fd, b->p, n) < 0) ? -1 : 0;
    }
    if (rc < 0) shutdown(fd, SHUT_RDWR);                /* its reader cleans up */
}

static void send_msg(unsigned type, const struct pb *b) { send_to(reply_fd, type, b); }          /* answer to a request */
static void send_va(unsigned type, const struct pb *b) { send_to(va_fd, type, b); }              /* voice assistant traffic */
static void send_state(unsigned type, const struct pb *b)                                        /* entity state: all subscribers */
{
    for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i].fd >= 0 && clients[i].states) send_to(clients[i].fd, type, b);
}

#define PB(name, size) unsigned char name##_buf[size]; struct pb name = { name##_buf, 0, size }

/* ---------------------------------------------------------------- identity */

static const char *node_name(void)      /* "Echo Dot" -> "echo-dot": ESPHome device names are host names */
{
    static char n[64]; size_t j = 0;
    for (const char *s = core_name; *s && j < sizeof n - 1; s++)
        n[j++] = isalnum((unsigned char)*s) ? tolower((unsigned char)*s) : '-';
    n[j] = 0;
    return n;
}

static const char *mac(void)
{
    static char m[24] = "02:00:00:00:00:01";
    FILE *f = fopen("/sys/class/net/wlan0/address", "r");
    if (f) { if (fscanf(f, "%17s", m) == 1) for (char *c = m; *c; c++) *c = toupper((unsigned char)*c); fclose(f); }
    return m;
}

static const char *mac_plain(void)      /* "aabbccddeeff": mDNS and the Noise server hello */
{
    static char m[24]; size_t j = 0;
    for (const char *c = mac(); *c && j < sizeof m - 1; c++) if (*c != ':') m[j++] = tolower((unsigned char)*c);
    m[j] = 0;
    return m;
}

/* ---------------------------------------------------------------- encryption key (see top) */

#define NOISE_NAME "Noise_NNpsk0_25519_ChaChaPoly_SHA256"
static uint8_t api_key[32]; static int have_key;        /* lock held */

static const char *key_path(void)
{
    static char p[256]; const char *d = getenv("HASSMIC_STATE");
    snprintf(p, sizeof p, "%s/api_key", d ? d : "/data/local/hassmic/state");
    return p;
}

static void key_load(void)
{
    char t[128] = ""; FILE *f = fopen(key_path(), "r");
    have_key = 0;
    if (!f) return;
    if (fgets(t, sizeof t, f) && b64_decode(t, strcspn(t, "\r\n"), api_key, sizeof api_key) == 32) have_key = 1;
    else fprintf(stderr, "encryption: %s is not a key, ignored\n", key_path());
    fclose(f);
}

static void print_mdns_to(FILE *f)
{
    char ss[512] = "";
    if (core_sendspin_port) sendspin_mdns(core_sendspin_port, ss, sizeof ss);
    fprintf(f, "<?xml version=\"1.0\" standalone='no'?>\n<!DOCTYPE service-group SYSTEM \"avahi-service.dtd\">\n"
            "<service-group>\n  <name>%s</name>\n  <service><type>_esphomelib._tcp</type><port>%d</port>\n"
            "    <txt-record>mac=%s</txt-record><txt-record>friendly_name=%s</txt-record><txt-record>version=2025.5.0</txt-record>\n"
            "    <txt-record>platform=hassmic</txt-record><txt-record>network=wifi</txt-record>\n"
            /* with a key Home Assistant asks for it when the device is added; without, it adds the device and sets one */
            "    <txt-record>%s=" NOISE_NAME "</txt-record>\n  </service>\n%s</service-group>\n",
            node_name(), core_port, mac_plain(), core_name, have_key ? "api_encryption" : "api_encryption_supported", ss);
}

static void print_mdns(void) { key_load(); print_mdns_to(stdout); }

/* The service file main.sh wrote at boot names the encryption state: rewrite it when that changes (avahi notices). */
static void mdns_refresh(void)
{
    const char *path = getenv("HASSMIC_MDNS_FILE"); char tmp[300]; FILE *f;
    if (!path) path = "/data/misc/avahi/services/hassmic.service";
    if (access(path, F_OK)) return;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (!(f = fopen(tmp, "w"))) { fprintf(stderr, "encryption: cannot update %s\n", path); return; }
    print_mdns_to(f);
    if (fclose(f) || rename(tmp, path)) { fprintf(stderr, "encryption: cannot update %s\n", path); unlink(tmp); return; }
    chmod(path, 0644);
}

/* lock held.  k: base64 as Home Assistant sends it; empty clears.  Returns 1 on success. */
static int key_set(const unsigned char *k, size_t len)
{
    uint8_t raw[32]; char tmp[300]; FILE *f;
    if (!len) {
        if (unlink(key_path()) && errno != ENOENT) { fprintf(stderr, "encryption: cannot remove %s\n", key_path()); return 0; }
        memset(api_key, 0, sizeof api_key); have_key = 0; fprintf(stderr, "encryption: key cleared, plaintext connections accepted again\n");
        mdns_refresh(); return 1;
    }
    char t[64]; if (len >= sizeof t) return 0;
    memcpy(t, k, len); t[len] = 0;
    if (b64_decode(t, len, raw, sizeof raw) != 32) { fprintf(stderr, "encryption: rejected a key that is not 32 bytes\n"); return 0; }
    snprintf(tmp, sizeof tmp, "%s.tmp", key_path());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || !(f = fdopen(fd, "w"))) { if (fd >= 0) close(fd); fprintf(stderr, "encryption: cannot write %s\n", tmp); return 0; }
    fprintf(f, "%s\n", t);
    if (fclose(f) || rename(tmp, key_path())) { unlink(tmp); fprintf(stderr, "encryption: cannot write %s\n", key_path()); return 0; }
    memcpy(api_key, raw, 32); have_key = 1;
    fprintf(stderr, "encryption: key set, only encrypted connections with it from now on\n");
    mdns_refresh();
    return 1;
}

/* ---------------------------------------------------------------- settings entities
 * Home Assistant applies these to the mic stream on its side (they travel in every VoiceAssistantRequest), like the
 * controls of its Wyoming integration.  Kept in a small file so they survive restarts. */

static const char *const noise_names[] = { "Off", "Low", "Medium", "High", "Max" };
static int noise_level, auto_gain; static float vol_mult = 1.0f;
/* The words of the Bluetooth announcements.  Home Assistant speaks them with the satellite's pipeline voice, but tells
 * neither us nor a template that pipeline's language (not in any ESPHome event, not on the pipeline select, no action
 * returns it), so the user picks the language here to match.  Four whole sentences each rather than one "a device" to
 * slot in: articles and cases differ between "connected to" and "disconnected from" in most of these languages. */
static const struct bt_lang { const char *code, *name, *on, *off, *on_any, *off_any; } bt_langs[] = {
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
#define BT_LANGS (int)(sizeof bt_langs / sizeof bt_langs[0])
static int bt_lang;                     /* lock held: index into bt_langs */

static const char *settings_path(void) { const char *p = getenv("HASSMIC_SETTINGS"); return p ? p : "/data/local/hassmic/state/settings"; }

static void settings_load(void)
{
    int n, g, m, w, a = 1, d = 0; float v; char l[8] = ""; FILE *f = fopen(settings_path(), "r");
    if (!f) return;
    /* older files: 5 fields (before Bluetooth announcements), 6 (before do not disturb), 7 (before their language) */
    if (fscanf(f, "%d %d %f %d %d %d %d %7s", &n, &g, &v, &m, &w, &a, &d, l) >= 5) {
        noise_level = n < 0 ? 0 : n > 4 ? 4 : n; auto_gain = g < 0 ? 0 : g > 31 ? 31 : g; vol_mult = v < 0.1f ? 0.1f : v > 10 ? 10 : v;
        core_soft_mute(m != 0); core_wake_sound(w != 0); core_bt_announce(a != 0); core_dnd(d != 0);
        for (int i = 0; i < BT_LANGS; i++) if (!strcmp(l, bt_langs[i].code)) bt_lang = i;     /* the code, not the index: the list may grow */
    }
    fclose(f);
}

static void settings_save(void)
{
    FILE *f = fopen(settings_path(), "w");
    if (!f) { fprintf(stderr, "settings: cannot write %s\n", settings_path()); return; }
    fprintf(f, "%d %d %.2f %d %d %d %d %s\n", noise_level, auto_gain, vol_mult, core_soft_mute(-1), core_wake_sound(-1), core_bt_announce(-1), core_dnd(-1),
            bt_langs[bt_lang].code);
    fclose(f);
}

static void send_setting(int key)       /* lock held */
{
    PB(b, 48);
    pb_fixed32(&b, 1, key);
    switch (key) {
    case KEY_NOISE: pb_str(&b, 2, noise_names[noise_level]); send_state(SELECT_STATE, &b); break;
    case KEY_GAIN:  pb_float(&b, 2, auto_gain); send_state(NUMBER_STATE, &b); break;
    case KEY_MULT:  pb_float(&b, 2, vol_mult); send_state(NUMBER_STATE, &b); break;
    case KEY_MUTE:  pb_uint(&b, 2, core_muted()); send_state(SWITCH_STATE, &b); break;
    case KEY_WAKE_SOUND: pb_uint(&b, 2, core_wake_sound(-1)); send_state(SWITCH_STATE, &b); break;
    case KEY_BT_PAIRING: if (ble_present()) { pb_uint(&b, 2, a2dp_pairing()); send_state(SWITCH_STATE, &b); } break;
    case KEY_BT_ANNOUNCE: if (ble_present()) { pb_uint(&b, 2, core_bt_announce(-1)); send_state(SWITCH_STATE, &b); } break;
    case KEY_DND:   pb_uint(&b, 2, core_dnd(-1)); send_state(SWITCH_STATE, &b); break;
    case KEY_BT_LANG: if (ble_present()) { pb_str(&b, 2, bt_langs[bt_lang].name); send_state(SELECT_STATE, &b); } break;
    case KEY_EQ_BASS: case KEY_EQ_MID: case KEY_EQ_TREBLE: pb_float(&b, 2, core_eq(key - KEY_EQ_BASS)); send_state(NUMBER_STATE, &b); break;
    }
}

/* The Sendspin pairing token, so that it can be copied from Home Assistant into Music Assistant.  It is a secret of
 * sorts (whoever has it can pair a server with this player), so the entity is diagnostic and disabled by default:
 * Home Assistant only records it once the user enables it. */
static void send_token_state(void)      /* lock held */
{
    PB(b, 256); char tok[160];
    if (!core_sendspin_port) return;
    sendspin_pairing_token(tok, sizeof tok);
    pb_fixed32(&b, 1, KEY_SENDSPIN_TOKEN); pb_str(&b, 2, tok);
    send_state(TEXT_SENSOR_STATE, &b);
}

/* ---------------------------------------------------------------- diagnostics
 * SoC temperature (thermal zone "mtktscpu") and CPU usage, as sensors that are diagnostic and disabled by default: Home
 * Assistant records them only once the user switches them on.  Pushed every 30 s to whoever subscribed to states. */

static float read_soc_temp(void)
{
    char path[80], type[32]; float t = NAN;
    for (int z = 0; z < 16; z++) {
        snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/type", z);
        FILE *f = fopen(path, "r"); if (!f) break;
        int ok = fscanf(f, "%31s", type) == 1; fclose(f);
        if (!ok || strcmp(type, "mtktscpu")) continue;
        snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/temp", z);
        if ((f = fopen(path, "r"))) { int mc; if (fscanf(f, "%d", &mc) == 1) t = mc / 1000.0f; fclose(f); }
        break;
    }
    return t;
}

static float cpu_usage(void)             /* percent busy since the previous call; NAN the first time */
{
    static unsigned long long last_busy, last_total;
    unsigned long long v[8] = { 0 }, total = 0, busy; float pct = NAN;
    FILE *f = fopen("/proc/stat", "r"); if (!f) return NAN;
    int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    fclose(f);
    if (n < 4) return NAN;
    for (int i = 0; i < 8; i++) total += v[i];
    busy = total - v[3] - v[4];                                 /* idle, iowait */
    if (last_total && total > last_total) pct = 100.0f * (float)(busy - last_busy) / (float)(total - last_total);
    last_busy = busy; last_total = total;
    return pct;
}

static void send_sensor(int key, float v)   /* lock held */
{
    PB(b, 32);
    if (isnan(v)) return;
    pb_fixed32(&b, 1, key); pb_float(&b, 2, v);
    send_state(SENSOR_STATE, &b);
}

static void send_diag_states(void) { send_sensor(KEY_SOC_TEMP, read_soc_temp()); send_sensor(KEY_CPU_USAGE, cpu_usage()); }

static void *diag_thread(void *arg)
{
    (void)arg;
    cpu_usage();
    for (;;) {
        sleep(30);
        pthread_mutex_lock(&core_lock);
        int any = 0;
        for (int i = 0; i < MAX_CLIENTS; i++) any |= clients[i].fd >= 0 && clients[i].states;
        if (any) send_diag_states();
        pthread_mutex_unlock(&core_lock);
    }
    return NULL;
}

static void send_diag_entities(void)
{
    { PB(b, 192); pb_str(&b, 1, "soc_temperature"); pb_fixed32(&b, 2, KEY_SOC_TEMP); pb_str(&b, 3, "SoC temperature");
      pb_str(&b, 5, "mdi:thermometer"); pb_str(&b, 6, "\xc2\xb0" "C"); pb_uint(&b, 7, 1); pb_str(&b, 9, "temperature"); pb_uint(&b, 10, 1);
      pb_uint(&b, 12, 1); pb_uint(&b, 13, 2); send_msg(LIST_SENSOR, &b); }
    { PB(b, 192); pb_str(&b, 1, "cpu_usage"); pb_fixed32(&b, 2, KEY_CPU_USAGE); pb_str(&b, 3, "CPU usage");
      pb_str(&b, 5, "mdi:chip"); pb_str(&b, 6, "%"); pb_uint(&b, 10, 1); pb_uint(&b, 12, 1); pb_uint(&b, 13, 2); send_msg(LIST_SENSOR, &b); }
}

static void send_setting_entities(void)
{
    { PB(b, 256); pb_str(&b, 1, "noise_suppression_level"); pb_fixed32(&b, 2, KEY_NOISE); pb_str(&b, 3, "Noise suppression level");
      pb_str(&b, 5, "mdi:volume-off"); for (int i = 0; i < 5; i++) pb_str(&b, 6, noise_names[i]); pb_uint(&b, 8, 1); send_msg(LIST_SELECT, &b); }
    { PB(b, 256); pb_str(&b, 1, "auto_gain"); pb_fixed32(&b, 2, KEY_GAIN); pb_str(&b, 3, "Auto gain"); pb_str(&b, 5, "mdi:microphone-plus");
      pb_float(&b, 6, 0); pb_float(&b, 7, 31); pb_float(&b, 8, 1); pb_uint(&b, 10, 1); pb_str(&b, 11, "dBFS"); pb_uint(&b, 12, 2); send_msg(LIST_NUMBER, &b); }
    { PB(b, 256); pb_str(&b, 1, "mic_volume_multiplier"); pb_fixed32(&b, 2, KEY_MULT); pb_str(&b, 3, "Mic volume multiplier"); pb_str(&b, 5, "mdi:volume-vibrate");
      pb_float(&b, 6, 0.1f); pb_float(&b, 7, 10); pb_float(&b, 8, 0.1f); pb_uint(&b, 10, 1); pb_uint(&b, 12, 1); send_msg(LIST_NUMBER, &b); }
    static const char *const eq_ids[] = { "equalizer_bass", "equalizer_mid", "equalizer_treble" };
    static const char *const eq_labels[] = { "Equalizer bass", "Equalizer mid", "Equalizer treble" };   /* sort together in HA */
    for (int i = 0; i < 3; i++) {       /* like the Alexa app's sliders */
        PB(b, 192); pb_str(&b, 1, eq_ids[i]); pb_fixed32(&b, 2, KEY_EQ_BASS + i); pb_str(&b, 3, eq_labels[i]); pb_str(&b, 5, "mdi:tune-vertical");
        pb_float(&b, 6, -6); pb_float(&b, 7, 6); pb_float(&b, 8, 1); pb_str(&b, 11, "dB"); pb_uint(&b, 12, 2); send_msg(LIST_NUMBER, &b);
    }
    { PB(b, 128); pb_str(&b, 1, "mute"); pb_fixed32(&b, 2, KEY_MUTE); pb_str(&b, 3, "Mute"); pb_str(&b, 5, "mdi:microphone-off"); send_msg(LIST_SWITCH, &b); }
    { PB(b, 128); pb_str(&b, 1, "do_not_disturb"); pb_fixed32(&b, 2, KEY_DND); pb_str(&b, 3, "Do not disturb"); pb_str(&b, 5, "mdi:minus-circle");
      send_msg(LIST_SWITCH, &b); }
    if (core_sendspin_port) { PB(b, 192); pb_str(&b, 1, "sendspin_pairing_token"); pb_fixed32(&b, 2, KEY_SENDSPIN_TOKEN); pb_str(&b, 3, "Sendspin pairing token");
      pb_str(&b, 5, "mdi:key-link"); pb_uint(&b, 6, 1); pb_uint(&b, 7, 2); send_msg(LIST_TEXT_SENSOR, &b); }
    { PB(b, 128); pb_str(&b, 1, "wake_sound"); pb_fixed32(&b, 2, KEY_WAKE_SOUND); pb_str(&b, 3, "Wake sound"); pb_str(&b, 5, "mdi:bell-ring");
      pb_uint(&b, 8, 1); send_msg(LIST_SWITCH, &b); }
    if (ble_present()) { PB(b, 128); pb_str(&b, 1, "bluetooth_pairing"); pb_fixed32(&b, 2, KEY_BT_PAIRING); pb_str(&b, 3, "Bluetooth pairing");
      pb_str(&b, 5, "mdi:bluetooth-connect"); send_msg(LIST_SWITCH, &b); }
    if (ble_present()) { PB(b, 128); pb_str(&b, 1, "bluetooth_announcements"); pb_fixed32(&b, 2, KEY_BT_ANNOUNCE); pb_str(&b, 3, "Bluetooth announcements");
      pb_str(&b, 5, "mdi:bluetooth-audio"); pb_uint(&b, 8, 1); send_msg(LIST_SWITCH, &b); }
    if (ble_present()) { PB(b, 512); pb_str(&b, 1, "bluetooth_announcement_language"); pb_fixed32(&b, 2, KEY_BT_LANG);
      pb_str(&b, 3, "Bluetooth announcement language"); pb_str(&b, 5, "mdi:translate");
      for (int i = 0; i < BT_LANGS; i++) pb_str(&b, 6, bt_langs[i].name);
      pb_uint(&b, 8, 1); send_msg(LIST_SELECT, &b); }
    send_diag_entities();
}

static void on_setting(unsigned type, const unsigned char *p, const unsigned char *end)
{
    struct pbf f; unsigned key = 0; float num = 0; int on = 0; char opt[32] = "";
    while (pb_next(&p, end, &f)) {
        if (f.field == 1) key = f.v;
        else if (f.field == 2 && f.wire == 5) { uint32_t u = f.v; memcpy(&num, &u, 4); }
        else if (f.field == 2 && f.wire == 0) on = f.v != 0;
        else if (f.field == 2 && f.data) pbf_str(&f, opt, sizeof opt);
    }
    if (type == SELECT_COMMAND && key == KEY_NOISE) { for (int i = 0; i < 5; i++) if (!strcmp(opt, noise_names[i])) noise_level = i; }
    else if (type == SELECT_COMMAND && key == KEY_BT_LANG) { for (int i = 0; i < BT_LANGS; i++) if (!strcmp(opt, bt_langs[i].name)) bt_lang = i; }
    else if (type == NUMBER_COMMAND && key == KEY_GAIN) auto_gain = num < 0 ? 0 : num > 31 ? 31 : (int)(num + 0.5f);
    else if (type == NUMBER_COMMAND && key == KEY_MULT) vol_mult = num < 0.1f ? 0.1f : num > 10 ? 10 : num;
    else if (type == SWITCH_COMMAND && key == KEY_MUTE) { core_soft_mute(on); settings_save(); send_setting(KEY_MUTE); return; }
    else if (type == SWITCH_COMMAND && key == KEY_WAKE_SOUND) core_wake_sound(on);
    else if (type == SWITCH_COMMAND && key == KEY_BT_ANNOUNCE) core_bt_announce(on);
    else if (type == SWITCH_COMMAND && key == KEY_DND) core_dnd(on);
    else if (type == SWITCH_COMMAND && key == KEY_BT_PAIRING) { a2dp_pair(on); return; }     /* its state follows through bt_changed */
    else if (type == NUMBER_COMMAND && key >= KEY_EQ_BASS && key <= KEY_EQ_TREBLE) {           /* the mixer keeps it, not our file */
        core_set_eq(key - KEY_EQ_BASS, (int)lroundf(num)); send_setting(key); return;
    }
    else return;
    fprintf(stderr, "settings: noise=%s gain=%d mult=%.1f mute=%d wake_sound=%d bt_announce=%d dnd=%d bt_lang=%s\n", noise_names[noise_level], auto_gain,
            vol_mult, core_soft_mute(-1), core_wake_sound(-1), core_bt_announce(-1), core_dnd(-1), bt_langs[bt_lang].code);
    settings_save(); send_setting(key);
}

/* ---------------------------------------------------------------- media over http (announcements, play_media) */

struct media_job { char url[2][1024]; int announce, start_conversation; };

static void send_mp_state(void)         /* lock held */
{
    PB(b, 32);
    pb_fixed32(&b, 1, MP_KEY); pb_uint(&b, 2, media_playing ? MP_PLAYING : MP_IDLE); pb_float(&b, 3, core_volume() / 100.0f);
    send_state(MEDIA_PLAYER_STATE, &b);
}

static int http_get_host(const char *host, const char *port, const char *path)
{
    char req[1400], line[1024]; int v6 = strchr(host, ':') != NULL;
    int fd = net_connect(host, port, 4);
    if (fd < 0) return -1;
    struct timeval tv = { 15, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);     /* TTS may still be rendering */
    /* HTTP/1.0: no chunked transfer encoding, the body ends with the connection */
    int n = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s%s%s:%s\r\nUser-Agent: hassmic/" VERSION "\r\n\r\n",
                     path, v6 ? "[" : "", host, v6 ? "]" : "", port);
    if (write_all(fd, req, n) < 0) { close(fd); return -1; }
    for (int first = 1;; first = 0) {                   /* header lines, byte by byte: they are short */
        size_t i = 0; char c;
        while (i < sizeof line - 1 && read(fd, &c, 1) == 1 && c != '\n') line[i++] = c;
        line[i] = 0;
        if (first && !strstr(line, " 200")) { fprintf(stderr, "media: http://%s:%s%s -> %s\n", host, port, path, line); close(fd); return -1; }
        if (i <= 1) return fd;                          /* "\r": end of headers */
    }
}

static int http_get(const char *url)    /* returns a socket positioned at the body, or -1 */
{
    char host[256], port[8] = "80"; const char *p = url, *path;
    if (strncmp(p, "http://", 7)) { fprintf(stderr, "media: only http:// is supported: %s\n", url); return -1; }
    p += 7; path = strchr(p, '/');
    snprintf(host, sizeof host, "%.*s", (int)(path ? path - p : (long)strlen(p)), p);
    char *colon = strrchr(host, ':'), *close_br = strrchr(host, ']');
    if (colon && close_br > colon) colon = NULL;                            /* "[fe80::1]": the colons are the address */
    if (colon) { *colon = 0; snprintf(port, sizeof port, "%s", colon + 1); }
    if (host[0] == '[' && (close_br = strchr(host, ']'))) { *close_br = 0; memmove(host, host + 1, strlen(host)); }
    return http_get_host(host, port, path ? path : "/");
}

static void feed(const void *pcm, size_t len)       /* queue with back pressure */
{
    core_tts_data(pcm, len);
    while (core_tts_queued() > 256 * 1024 && !core_tts_flushing()) usleep(50000);
}

static int play_wav(int fd, const unsigned char *head, size_t have)
{
    unsigned char ck[8], fmt[16]; unsigned rate = 0, ch = 0, bits = 0; static unsigned char buf[8192];
    (void)head; (void)have;                             /* the 12 byte RIFF/WAVE header, already consumed */
    for (;;) {
        if (read_full(fd, ck, 8) != 8) return -1;
        uint32_t len; memcpy(&len, ck + 4, 4);
        if (!memcmp(ck, "data", 4)) break;
        if (!memcmp(ck, "fmt ", 4) && len >= 16) {
            if (read_full(fd, fmt, 16) != 16) return -1;
            ch = fmt[2] | fmt[3] << 8; memcpy(&rate, fmt + 4, 4); bits = fmt[14] | fmt[15] << 8; len -= 16;
        }
        for (len += len & 1; len; ) { ssize_t r = read(fd, buf, len < sizeof buf ? len : sizeof buf); if (r <= 0) return -1; len -= r; }
    }
    if (bits != 16 || !rate || !ch) { fprintf(stderr, "media: unsupported WAV (%u Hz, %u ch, %u bit)\n", rate, ch, bits); return -1; }
    fprintf(stderr, "media: WAV %u Hz x%u\n", rate, ch);
    pthread_mutex_lock(&core_lock); core_tts_begin(rate, ch); pthread_mutex_unlock(&core_lock);
    size_t odd = 0;
    for (ssize_t r; !core_tts_flushing() && (r = read(fd, buf + odd, sizeof buf - odd)) > 0; ) {
        size_t n = odd + r, use = n & ~(size_t)(2 * ch - 1);        /* whole frames only */
        feed(buf, use);
        odd = n - use; memmove(buf, buf + use, odd);
    }
    return 0;
}

/* Home Assistant sends TTS announcements as they come from the TTS engine, usually MP3 (it only asks for WAV once a
 * voice pipeline has run), so that has to play too. */
static int play_mp3(int fd, const unsigned char *head, size_t have)
{
    static unsigned char in[32768]; static mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    static mp3dec_t dec; mp3dec_frame_info_t info; size_t n = have; int began = 0, eof = 0;
    memcpy(in, head, have);
    mp3dec_init(&dec);
    while (!core_tts_flushing()) {
        while (!eof && n < sizeof in) { ssize_t r = read(fd, in + n, sizeof in - n); if (r <= 0) eof = 1; else n += r; }
        if (!n) break;
        int samples = mp3dec_decode_frame(&dec, in, n, pcm, &info);
        if (!info.frame_bytes) { if (eof) break; n = 0; continue; }     /* no sync in the whole buffer */
        if (samples > 0) {
            if (!began) {
                fprintf(stderr, "media: MP3 %d Hz x%d\n", info.hz, info.channels);
                pthread_mutex_lock(&core_lock); core_tts_begin(info.hz, info.channels); pthread_mutex_unlock(&core_lock);
                began = 1;
            }
            feed(pcm, (size_t)samples * info.channels * 2);
        }
        n -= info.frame_bytes; memmove(in, in + info.frame_bytes, n);
    }
    if (!began) fprintf(stderr, "media: no MP3 audio found\n");
    return began ? 0 : -1;
}

static int play_url(int fd)
{
    unsigned char h[12];
    if (read_full(fd, h, 12) != 12) { fprintf(stderr, "media: empty response\n"); return -1; }
    if (!memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4)) return play_wav(fd, h, 12);
    if (!memcmp(h, "fLaC", 4) || !memcmp(h, "OggS", 4)) { fprintf(stderr, "media: FLAC/Ogg is not supported, only WAV and MP3\n"); return -1; }
    return play_mp3(fd, h, 12);
}

static void *media_thread(void *arg)
{
    struct media_job *job = arg; int ok = 0, began = 0;
    for (int i = 0; i < 2; i++) {
        if (!job->url[i][0] || core_tts_flushing()) continue;
        int fd = http_get(job->url[i]);
        if (fd < 0) continue;
        if (play_url(fd) == 0) { began = 1; if (i == 1) ok = 1; }
        close(fd);
    }
    pthread_mutex_lock(&core_lock);
    if (job->announce) {
        if (job->start_conversation && ok) core_restart_after();
        if (!began) { PB(b, 8); pb_uint(&b, 1, 0); send_va(VA_ANNOUNCE_FINISHED, &b); announcing = 0; announce_done(); }
    }
    if (!began) { media_playing = 0; send_mp_state(); core_pipeline_finish(); atomic_store(&media_busy, 0); }
    pthread_mutex_unlock(&core_lock);
    if (began) core_tts_end();          /* played() below finishes the job once the audio is out */
    free(job);
    return NULL;
}

/* lock held.  url0 = optional chime, url1 = the media */
static void media_start(const char *url0, const char *url1, int announce, int start_conversation, int is_reply)
{
    struct media_job *job = calloc(1, sizeof *job); pthread_t t;
    if (!job) return;
    if (atomic_exchange(&media_busy, 1)) {              /* one at a time: refuse, the running one finishes first */
        fprintf(stderr, "media: busy, dropped %s\n", url1);
        if (announce) { PB(b, 8); pb_uint(&b, 1, 0); send_va(VA_ANNOUNCE_FINISHED, &b); }
        free(job); return;
    }
    if (!is_reply && (core_state() == LISTENING || core_state() == THINKING)) core_pipeline_finish();
    snprintf(job->url[0], sizeof job->url[0], "%s", url0); snprintf(job->url[1], sizeof job->url[1], "%s", url1);
    job->announce = announce; job->start_conversation = start_conversation;
    announcing = announce; media_playing = 1; send_mp_state();
    fprintf(stderr, "media: %s%s\n", announce ? "announce " : "", url1);
    pthread_create(&t, NULL, media_thread, job); pthread_detach(t);
}

/* ---------------------------------------------------------------- bluetooth proxy
 * Home Assistant subscribes to raw advertisements and parses them itself.  The radio scans only while someone is
 * subscribed.  Active scanning (scan requests, so names from scan responses) is the default, like ESPHome's; Home
 * Assistant can switch the mode (not kept across restarts, it sets it again).
 * Connections: Home Assistant asks for a free slot (connections free), connects, fetches the services once and caches
 * them, then reads, writes, subscribes and pairs.  Answers go to every client that uses the proxy (subscribed, or sent a
 * request).  When the last one leaves,
 * its connections are closed, as ESPHome does. */

static int ble_mode = BLE_ACTIVE;       /* lock held */

static int ble_subscribers(void)        /* lock held */
{
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) n += clients[i].fd >= 0 && clients[i].ble;
    return n;
}

static void ble_update(void) { ble_scan(ble_subscribers() > 0, ble_mode == BLE_ACTIVE); }      /* lock held */

static void send_ble(unsigned type, const struct pb *b)                                        /* lock held */
{
    for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i].fd >= 0 && clients[i].ble) send_to(clients[i].fd, type, b);
}

static void send_scanner_state(void)    /* lock held */
{
    PB(b, 16);
    pb_uint(&b, 1, ble_scanning() ? BLE_RUNNING : BLE_IDLE); pb_uint(&b, 2, ble_mode); pb_uint(&b, 3, ble_mode);
    send_ble(BLE_SCANNER_STATE, &b);
}

static void ble_changed(void) { pthread_mutex_lock(&core_lock); send_scanner_state(); pthread_mutex_unlock(&core_lock); }

static void send_gatt(unsigned type, const struct pb *b)                                      /* lock held */
{
    for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i].fd >= 0 && (clients[i].ble || clients[i].ble_free || clients[i].ble_user)) send_to(clients[i].fd, type, b);
}

static void send_slots(int to_asker)    /* lock held */
{
    PB(b, 64); uint64_t a[BLE_MAX_CONN]; int n = ble_connections(a);
    pb_uint(&b, 1, BLE_MAX_CONN - n); pb_uint(&b, 2, BLE_MAX_CONN);
    if (n) { PB(al, 40); for (int i = 0; i < n; i++) pb_varint(&al, a[i]); pb_bytes(&b, 3, al.p, al.n); }    /* packed */
    if (to_asker) { send_msg(BLE_CONN_FREE, &b); return; }
    for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i].fd >= 0 && clients[i].ble_free) send_to(clients[i].fd, BLE_CONN_FREE, &b);
}

static void ble_slots(void) { pthread_mutex_lock(&core_lock); send_slots(0); pthread_mutex_unlock(&core_lock); }

static void ble_connection(uint64_t addr, int connected, unsigned mtu, int error)
{
    PB(b, 32);
    pb_uint(&b, 1, addr); pb_uint(&b, 2, connected); pb_uint(&b, 3, mtu); pb_uint(&b, 4, error);
    pthread_mutex_lock(&core_lock); send_gatt(BLE_CONNECTION, &b); pthread_mutex_unlock(&core_lock);
}

static void pb_uuid(struct pb *b, int f, const uint64_t u[2])  /* repeated uint64, packed */
{
    PB(v, 20); pb_varint(&v, u[0]); pb_varint(&v, u[1]); pb_bytes(b, f, v.p, v.n);
}

/* one message per service, like ESPHome: a big database would not fit into one */
static void ble_db(uint64_t addr, const struct ble_db *db)
{
    static unsigned char buf[8000], sb[7900], cb[1200], db_[64];
    pthread_mutex_lock(&core_lock);
    for (int s = 0; s < db->nsvc; s++) {
        const struct ble_svc *sv = &db->svc[s]; struct pb m = { buf, 0, sizeof buf }, sm = { sb, 0, sizeof sb };
        pb_uuid(&sm, 1, sv->uuid); pb_uint(&sm, 2, sv->start);
        for (int c = sv->first; c < sv->first + sv->n; c++) {
            const struct ble_chr *ch = &db->chr[c]; struct pb cm = { cb, 0, sizeof cb };
            pb_uuid(&cm, 1, ch->uuid); pb_uint(&cm, 2, ch->handle); pb_uint(&cm, 3, ch->props);
            for (int d = ch->first; d < ch->first + ch->n; d++) {
                struct pb dm = { db_, 0, sizeof db_ };
                pb_uuid(&dm, 1, db->dsc[d].uuid); pb_uint(&dm, 2, db->dsc[d].handle);
                pb_bytes(&cm, 4, dm.p, dm.n);
            }
            pb_bytes(&sm, 3, cm.p, cm.n);
        }
        if (sm.n >= sm.cap - 64) fprintf(stderr, "bluetooth: service %d of %012llx too big, truncated\n", s, (unsigned long long)addr);
        pb_uint(&m, 1, addr); pb_bytes(&m, 2, sm.p, sm.n);
        send_gatt(BLE_SERVICES, &m);
    }
    { PB(b, 16); pb_uint(&b, 1, addr); send_gatt(BLE_SERVICES_DONE, &b); }
    pthread_mutex_unlock(&core_lock);
}

static void ble_data(unsigned type, uint64_t addr, unsigned handle, const void *data, size_t len, int with_data)
{
    PB(b, 600);
    pb_uint(&b, 1, addr); pb_uint(&b, 2, handle); if (with_data) pb_bytes(&b, 3, data, len);
    pthread_mutex_lock(&core_lock); send_gatt(type, &b); pthread_mutex_unlock(&core_lock);
}

static void ble_read_cb(uint64_t a, unsigned h, const void *d, size_t n) { ble_data(BLE_READ, a, h, d, n, 1); }
static void ble_notify_cb(uint64_t a, unsigned h, const void *d, size_t n) { ble_data(BLE_NOTIFY_DATA, a, h, d, n, 1); }
static void ble_written_cb(uint64_t a, unsigned h) { ble_data(BLE_WRITTEN, a, h, NULL, 0, 0); }
static void ble_error_cb(uint64_t addr, unsigned handle, int error)
{
    PB(b, 32);
    pb_uint(&b, 1, addr); pb_uint(&b, 2, handle); pb_uint(&b, 3, error);
    pthread_mutex_lock(&core_lock); send_gatt(BLE_GATT_ERROR, &b); pthread_mutex_unlock(&core_lock);
}

static void on_ble_request(unsigned type, const unsigned char *p, const unsigned char *end)   /* lock held */
{
    struct pbf f; uint64_t addr = 0; unsigned kind = 0, handle = 0, addr_type = 0, flag = 0; const unsigned char *data = NULL; size_t len = 0;
    while (pb_next(&p, end, &f)) switch (f.field) {
        case 1: addr = f.v; break;
        case 2: if (type == BLE_DEVICE_REQ) kind = f.v; else handle = f.v; break;
        case 3: if (type == BLE_WRITE_DESC_REQ && f.data) { data = f.data; len = f.len; } else flag = f.v != 0; break;
        case 4: if (type == BLE_DEVICE_REQ) addr_type = f.v; else if (f.data) { data = f.data; len = f.len; } break;
    }
    switch (type) {
    case BLE_DEVICE_REQ:
        if (kind == BLE_REQ_DISCONNECT) ble_disconnect(addr);
        else if (kind == BLE_REQ_PAIR) ble_pair(addr);
        else if (kind == BLE_REQ_UNPAIR) ble_unpair(addr);
        else if (kind == BLE_REQ_CLEAR_CACHE) { PB(b, 24); pb_uint(&b, 1, addr); pb_uint(&b, 2, 1); send_msg(BLE_CACHE_CLEARED, &b); }
        else ble_connect(addr, addr_type);
        break;
    case BLE_SERVICES_REQ:   ble_services(addr); break;
    case BLE_READ_REQ: case BLE_READ_DESC_REQ: ble_read(addr, handle); break;
    case BLE_WRITE_REQ:      ble_write(addr, handle, data, len, flag); break;
    case BLE_WRITE_DESC_REQ: ble_write(addr, handle, data, len, 1); break;
    case BLE_NOTIFY_REQ: { PB(b, 24); pb_uint(&b, 1, addr); pb_uint(&b, 2, handle); send_msg(BLE_NOTIFY, &b); } break;     /* see BLE_FEATURES */
    }
}


static void ble_adverts(const struct ble_adv *a, int n)
{
    PB(b, BLE_BATCH * 64); PB(one, 64);
    for (int i = 0; i < n; i++) {
        one.n = 0;
        pb_uint(&one, 1, a[i].addr); pb_sint(&one, 2, a[i].rssi); pb_uint(&one, 3, a[i].addr_type); pb_bytes(&one, 4, a[i].data, a[i].len);
        pb_bytes(&b, 1, one.p, one.n);
    }
    pthread_mutex_lock(&core_lock);
    send_ble(BLE_RAW_ADV, &b);
    pthread_mutex_unlock(&core_lock);
}

static void ble_pair_result(unsigned type, uint64_t addr, int ok, int error)
{
    PB(b, 32);
    pb_uint(&b, 1, addr); pb_uint(&b, 2, ok); pb_uint(&b, 3, error);
    pthread_mutex_lock(&core_lock); send_gatt(type, &b); pthread_mutex_unlock(&core_lock);
}
static void ble_paired_cb(uint64_t a, int ok, int e) { ble_pair_result(BLE_PAIRED, a, ok, e); }
static void ble_unpaired_cb(uint64_t a, int ok, int e) { ble_pair_result(BLE_UNPAIRED, a, ok, e); }

/* Bluetooth speaker: the pairing window opened, ran out or ended with a paired device */
static void bt_changed(void) { pthread_mutex_lock(&core_lock); send_setting(KEY_BT_PAIRING); pthread_mutex_unlock(&core_lock); }

static const struct ble_handler ble_handler = {
    .adverts = ble_adverts, .scan_changed = ble_changed, .slots_changed = ble_slots, .connection = ble_connection, .services = ble_db,
    .read = ble_read_cb, .written = ble_written_cb, .notify = ble_notify_cb, .error = ble_error_cb,
    .paired = ble_paired_cb, .unpaired = ble_unpaired_cb,
};

/* ---------------------------------------------------------------- core callbacks (lock held) */

static void pb_map(struct pb *b, int f, const char *k, const char *v)      /* HomeassistantServiceMap */
{
    PB(e, 400); pb_str(&e, 1, k); pb_str(&e, 2, v);
    pb_bytes(b, f, e.p, e.n);
}

/* The satellite entity is ours by MAC: Home Assistant keys the device on it, entity ids can be renamed.  The name
 * the phone chose goes in plain data, never into the template.
 * Home Assistant refuses an announcement while the satellite still plays one (SatelliteBusyError: a phone
 * reconnecting during "Disconnected from ..."), and still for a moment after our "finished", until its announce call
 * has unwound.  So one request at a time, latest wins: the next goes 1 s after the previous announcement ended, or
 * 15 s after its request if none came (Home Assistant dropped it), and not at all once 30 s old. */
static char bt_next[160];               /* lock held: waiting to be asked */
static long long bt_next_ms, bt_asked_ms, bt_free_ms;   /* lock held: queued at; last request out, no end yet (0: none); not before */

static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

static void bt_ask(const char *msg)
{
    PB(b, 1024); char tpl[400], m[24]; size_t j = 0; int n = 0;
    for (const char *c = mac(); *c && j < sizeof m - 1; c++) m[j++] = tolower((unsigned char)*c);
    m[j] = 0;
    snprintf(tpl, sizeof tpl, "{%% for e in integration_entities('esphome') if e.startswith('assist_satellite.') and "
             "('mac', '%s') in (device_attr(e, 'connections') or []) %%}{{ e }}{%% endfor %%}", m);
    pb_str(&b, 1, "assist_satellite.announce");
    pb_map(&b, 2, "message", msg);
    pb_map(&b, 3, "entity_id", tpl);
    pb_map(&b, 3, "preannounce", "{{ false }}");    /* the chime already played here.  Plain data arrives as strings, and
                                                       the announce schema takes a real bool only: a template renders one */
    for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i].fd >= 0 && clients[i].actions) { send_to(clients[i].fd, HA_ACTION, &b); n++; }
    if (n) bt_asked_ms = now_ms();
    fprintf(stderr, "bluetooth: \"%s\" %s\n", msg, n ? "asked of Home Assistant" : "not spoken: no client takes actions");
}

static void bt_try(void)                /* lock held */
{
    long long now = now_ms();
    if (!bt_next[0] || announcing || now < bt_free_ms || (bt_asked_ms && now - bt_asked_ms < 15000)) return;
    if (now - bt_next_ms < 30000) bt_ask(bt_next);
    bt_next[0] = 0;
}

static void *bt_thread(void *arg)
{
    (void)arg;
    for (;;) { usleep(200000); pthread_mutex_lock(&core_lock); bt_try(); pthread_mutex_unlock(&core_lock); }
    return NULL;
}

static void bt_device(const char *name, int on)
{
    static int started;
    if (!started) { pthread_t t; started = !pthread_create(&t, NULL, bt_thread, NULL); if (started) pthread_detach(t); }
    const struct bt_lang *l = &bt_langs[bt_lang];
    if (*name) snprintf(bt_next, sizeof bt_next, on ? l->on : l->off, name);
    else snprintf(bt_next, sizeof bt_next, "%s", on ? l->on_any : l->off_any);
    bt_next_ms = now_ms();
    bt_try();
    if (bt_next[0]) fprintf(stderr, "bluetooth: \"%s\" waits for the announcement before\n", bt_next);
}

static void announce_done(void) { bt_asked_ms = 0; bt_free_ms = now_ms() + 1000; }

static void start(void)
{
    PB(b, 96);
    tts_expected = 0; tts_url[0] = 0;
    pb_uint(&b, 1, 1);                                  /* start */
    pb_uint(&b, 3, core_local_wake ? 1 : 3);            /* flags: USE_VAD, plus USE_WAKE_WORD when detection is remote */
    { PB(as, 24); pb_uint(&as, 1, noise_level); pb_uint(&as, 2, auto_gain); pb_float(&as, 3, vol_mult);
      pb_varint(&b, 4 << 3 | 2); pb_varint(&b, as.n); pb_raw(&b, as.p, as.n); }
    if (core_local_wake) { const struct core_wake_word *w; if (core_wake_words(&w)) pb_str(&b, 5, w[core_wake_word(-1)].name); }  /* HA's duplicate check keys on it */
    send_va(VA_REQUEST, &b);
}

static void audio(const void *pcm, size_t len)
{
    static unsigned char buf[4096 + 16];
    for (size_t off = 0; off < len; off += 4096) {
        struct pb b = { buf, 0, sizeof buf };
        pb_bytes(&b, 1, (const char *)pcm + off, len - off < 4096 ? len - off : 4096);
        send_va(VA_AUDIO, &b);
    }
}

static void stop(void)
{
    PB(a, 8); pb_uint(&a, 2, 1); send_va(VA_AUDIO, &a);        /* end of audio */
    send_va(VA_REQUEST, NULL);                                 /* start = false */
}

static void played(void)
{
    if (announcing) { PB(b, 8); pb_uint(&b, 1, 1); send_va(VA_ANNOUNCE_FINISHED, &b); announcing = 0; announce_done(); }
    if (media_playing) { media_playing = 0; send_mp_state(); }
    atomic_store(&media_busy, 0);
    tts_expected = 0;
}

static void volume_changed(int percent) { (void)percent; send_mp_state(); }
static void mute_changed(int muted) { (void)muted; send_setting(KEY_MUTE); settings_save(); }

/* ---------------------------------------------------------------- requests from Home Assistant */

static void on_event(const unsigned char *p, const unsigned char *end)
{
    struct pbf f, g; unsigned type = 0; char key[48], val[1024], text[512] = ""; int cont = 0, stream_now = 0;
    while (pb_next(&p, end, &f)) {
        if (f.field == 1) type = f.v;
        else if (f.field == 2 && f.wire == 2) {
            const unsigned char *q = f.data; key[0] = val[0] = 0;
            while (pb_next(&q, f.data + f.len, &g)) { if (g.field == 1 && g.data) pbf_str(&g, key, sizeof key); else if (g.field == 2 && g.data) pbf_str(&g, val, sizeof val); }
            if (!strcmp(key, "text") || !strcmp(key, "message") || !strcmp(key, "code")) snprintf(text + strlen(text), sizeof text - strlen(text), "%s ", val);
            if (!strcmp(key, "continue_conversation") && !strcmp(val, "1")) cont = 1;
            if (!strcmp(key, "tts_start_streaming") && !strcmp(val, "1")) stream_now = 1;
            if (!strcmp(key, "url")) snprintf(tts_url, sizeof tts_url, "%s", val);
        }
    }
    switch (type) {
    case EV_STT_START:  core_set_state(LISTENING); break;
    case EV_VAD_END:    core_mic_off(); if (core_state() == LISTENING) core_set_state(THINKING); break;
    case EV_STT_END:    fprintf(stderr, "transcript: %s\n", text); core_mic_off(); if (core_state() == LISTENING) core_set_state(THINKING); break;
    case EV_INTENT_END: if (cont) core_restart_after(); break;
    case EV_TTS_START:  fprintf(stderr, "reply: %s\n", text); break;
    case EV_INTENT_PROGRESS: if (!stream_now) break;    /* streaming TTS: the URL from RUN_START can be fetched already */
        /* fall through */
    case EV_TTS_END:
        if (!tts_expected && tts_url[0]) { tts_expected = 1; core_mic_off(); media_start("", tts_url, 1, 0, 1); }
        break;
    case EV_RUN_END:    if (!tts_expected && core_state() != SPEAKING) core_pipeline_finish(); break;
    case EV_ERROR:      fprintf(stderr, "pipeline error: %s\n", text); tts_expected = 0; core_error(); break;
    }
}

static void on_timer(const unsigned char *p, const unsigned char *end)
{
    struct pbf f; unsigned type = 0, total = 0; char tname[64] = "";
    while (pb_next(&p, end, &f)) {
        if (f.field == 1) type = f.v; else if (f.field == 3 && f.data) pbf_str(&f, tname, sizeof tname); else if (f.field == 4) total = f.v;
    }
    static const char *const names[] = { "started", "updated", "cancelled", "finished" };
    fprintf(stderr, "timer %s: %s (%u s)\n", type < 4 ? names[type] : "?", tname, total);
    if (type == 3) core_alarm(1); else if (type == 2) core_alarm(0);
}

static void on_announce(const unsigned char *p, const unsigned char *end)
{
    struct pbf f; static char media[1024], pre[1024]; int conv = 0;
    media[0] = pre[0] = 0;
    while (pb_next(&p, end, &f)) {
        if (f.field == 1 && f.data) pbf_str(&f, media, sizeof media);
        else if (f.field == 3 && f.data) pbf_str(&f, pre, sizeof pre);
        else if (f.field == 4) conv = f.v != 0;
    }
    /* Do not disturb drops announcements (reported as not played), but not the one we asked for ourselves: a Bluetooth
     * connection message answers something the user just did, as stock's does. */
    if (core_dnd(-1) && !bt_asked_ms) { fprintf(stderr, "media: do not disturb, dropped announcement %s\n", media); PB(b, 8); pb_uint(&b, 1, 0); send_va(VA_ANNOUNCE_FINISHED, &b); return; }
    media_start(pre, media, 1, conv, 0);
}

static void on_mp_command(const unsigned char *p, const unsigned char *end)
{
    struct pbf f; static char url[1024]; int has_cmd = 0, cmd = 0, has_vol = 0, has_url = 0; float vol = 0;
    url[0] = 0;
    while (pb_next(&p, end, &f)) switch (f.field) {
        case 2: has_cmd = f.v != 0; break;
        case 3: cmd = f.v; break;
        case 4: has_vol = f.v != 0; break;
        case 5: { uint32_t u = f.v; memcpy(&vol, &u, 4); } break;
        case 6: has_url = f.v != 0; break;
        case 7: if (f.data) pbf_str(&f, url, sizeof url); break;
    }
    if (has_vol) core_set_volume((int)(vol * 100 + 0.5f));
    if (has_cmd && cmd == MP_CMD_STOP && media_playing) core_tts_flush();
    if (has_url && url[0]) media_start("", url, 0, 0, 0);
    if (has_cmd && !has_url) send_mp_state();
}

static void send_device_info(void)
{
    PB(b, 512);
    pb_str(&b, 2, node_name()); pb_str(&b, 3, mac()); pb_str(&b, 4, "2025.5.0"); pb_str(&b, 5, __DATE__ " " __TIME__);
    pb_str(&b, 6, "Echo Dot 3 (donut)"); pb_str(&b, 8, "hassmic.echo-dot-3"); pb_str(&b, 9, VERSION);
    pb_str(&b, 12, "Amazon"); pb_str(&b, 13, core_name);
    if (ble_present()) { pb_uint(&b, 11, 5); pb_uint(&b, 15, BLE_FEATURES); pb_str(&b, 18, ble_mac()); }    /* 11: legacy "active connections" */
    pb_uint(&b, 17, FEAT_VOICE | FEAT_API_AUDIO | FEAT_TIMERS | FEAT_ANNOUNCE | FEAT_START_CONVERSATION);      /* no SPEAKER: see top */
    pb_uint(&b, 19, 1);                                 /* api_encryption_supported */
    pb_uint(&b, 26, !have_key);                         /* api_encryption_provisionable: Home Assistant sets the key */
    send_msg(DEVICE_INFO_RESP, &b);
}

static void send_entities(void)
{
    PB(b, 256); PB(fmt, 48);
    pb_str(&b, 1, "media_player"); pb_fixed32(&b, 2, MP_KEY); pb_str(&b, 3, "Media Player");
    for (int purpose = 0; purpose < 2; purpose++) {     /* 0 = default, 1 = announcement: Home Assistant transcodes to this */
        fmt.n = 0;
        pb_str(&fmt, 1, "wav"); pb_uint(&fmt, 2, MEDIA_RATE); pb_uint(&fmt, 3, 1); pb_uint(&fmt, 4, purpose); pb_uint(&fmt, 5, 2);
        pb_varint(&b, 9 << 3 | 2); pb_varint(&b, fmt.n); pb_raw(&b, fmt.p, fmt.n);      /* purpose 0 encodes as absent: fine */
    }
    send_msg(LIST_MEDIA_PLAYER, &b);
    send_setting_entities();
    send_msg(LIST_ENTITIES_DONE, NULL);
}

/* Every installed model (core.h); Home Assistant shows them in the satellite's wake word select, one active at a time */
static void send_va_config(void)
{
    PB(b, 3072);
    if (core_local_wake) {
        const struct core_wake_word *w; int n = core_wake_words(&w);
        for (int i = 0; i < n; i++) {
            PB(e, 192); pb_str(&e, 1, w[i].id); pb_str(&e, 2, w[i].name); pb_str(&e, 3, w[i].lang);
            pb_varint(&b, 1 << 3 | 2); pb_varint(&b, e.n); pb_raw(&b, e.p, e.n);
        }
        if (n) pb_str(&b, 2, w[core_wake_word(-1)].id);
        pb_uint(&b, 3, 1);
    }
    send_msg(VA_CONFIG_RESP, &b);
}

static void on_va_set_config(const unsigned char *p, const unsigned char *end)
{
    struct pbf f; char id[64]; const struct core_wake_word *w; int n = core_wake_words(&w);
    while (pb_next(&p, end, &f)) {
        if (f.field != 1 || !f.data) continue;
        pbf_str(&f, id, sizeof id);
        for (int i = 0; i < n; i++) if (!strcmp(w[i].id, id)) { core_wake_word(i); return; }
        fprintf(stderr, "wake word: Home Assistant picked unknown \"%s\", ignored\n", id);
    }
}

/* returns 0 to close the connection */
static int handle(unsigned type, const unsigned char *p, size_t len)
{
    const unsigned char *end = p + len; struct pbf f; int keep = 1;
    if (type == VA_AUDIO) {                             /* TTS stream: no lock, just queue */
        while (pb_next(&p, end, &f)) if (f.field == 1 && f.data) core_tts_data(f.data, f.len);
        return 1;
    }
    pthread_mutex_lock(&core_lock);
    int c = client_of(reply_fd);
    if (have_key && c >= 0 && !clients[c].keyed && type != DISCONNECT_REQ) {
        /* got in before the key was set (the one that set it has had its answer): the key counts from here on */
        fprintf(stderr, "encryption: closing a connection without the key\n");
        pthread_mutex_unlock(&core_lock);
        return 0;
    }
    switch (type) {
    case HELLO_REQ: { PB(b, 128); pb_uint(&b, 1, 1); pb_uint(&b, 2, 10); pb_str(&b, 3, "hassmic " VERSION); pb_str(&b, 4, node_name()); send_msg(HELLO_RESP, &b); } break;
    case CONNECT_REQ:      send_msg(CONNECT_RESP, NULL); break;                 /* no password */
    case DISCONNECT_REQ:   send_msg(DISCONNECT_RESP, NULL); keep = 0; break;
    case PING_REQ:         send_msg(PING_RESP, NULL); break;
    case DEVICE_INFO_REQ:  send_device_info(); break;
    case LIST_ENTITIES_REQ: send_entities(); break;
    case SUBSCRIBE_STATES:
        for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i].fd == reply_fd) clients[i].states = 1;
        send_mp_state(); for (int k = KEY_NOISE; k <= KEY_WAKE_SOUND; k++) send_setting(k); send_setting(KEY_BT_PAIRING); send_setting(KEY_BT_ANNOUNCE); send_setting(KEY_DND);
        send_setting(KEY_BT_LANG);
        for (int k = KEY_EQ_BASS; k <= KEY_EQ_TREBLE; k++) send_setting(k);
        send_token_state(); send_diag_states(); break;
    case SELECT_COMMAND: case NUMBER_COMMAND: case SWITCH_COMMAND: on_setting(type, p, end); break;
    case SUBSCRIBE_HA_ACTIONS: if (c >= 0) clients[c].actions = 1; break;
    case SUBSCRIBE_VA: {
        int sub = 0;
        while (pb_next(&p, end, &f)) if (f.field == 1) sub = f.v != 0;
        if (sub && va_fd >= 0 && va_fd != reply_fd) { fprintf(stderr, "voice assistant: a second client tried to subscribe, ignored\n"); break; }
        if (!sub && va_fd != reply_fd) break;
        fprintf(stderr, "voice assistant: %s\n", sub ? "subscribed" : "unsubscribed");
        va_fd = sub ? reply_fd : -1;
        if (!sub) tts_expected = 0;
        core_link(1, sub);
    } break;
    case VA_RESPONSE:
        while (pb_next(&p, end, &f)) if (f.field == 2 && f.v) { fprintf(stderr, "pipeline refused\n"); core_error(); }
        break;
    case VA_EVENT:         on_event(p, end); break;
    case VA_TIMER_EVENT:   on_timer(p, end); break;
    case VA_ANNOUNCE:      on_announce(p, end); break;
    case VA_CONFIG_REQ:    send_va_config(); break;
    case VA_SET_CONFIG:    if (core_local_wake) on_va_set_config(p, end); break;
    case MEDIA_PLAYER_COMMAND: on_mp_command(p, end); break;
    case BLE_SUBSCRIBE: case BLE_UNSUBSCRIBE:
        if (c < 0 || !ble_present()) break;
        clients[c].ble = type == BLE_SUBSCRIBE;         /* flags ignored: raw advertisements are all we send */
        ble_update(); if (type == BLE_SUBSCRIBE) send_scanner_state();
        break;
    case BLE_SCANNER_SET_MODE:
        ble_mode = BLE_PASSIVE;                         /* the zero value: an empty message */
        while (pb_next(&p, end, &f)) if (f.field == 1 && f.v == BLE_ACTIVE) ble_mode = BLE_ACTIVE;
        ble_update(); send_scanner_state();
        break;
    case BLE_CONN_FREE_REQ: if (c >= 0 && ble_present()) { clients[c].ble_free = 1; send_slots(1); } break;
    case BLE_DEVICE_REQ: case BLE_SERVICES_REQ: case BLE_READ_REQ: case BLE_READ_DESC_REQ: case BLE_WRITE_REQ:
    case BLE_WRITE_DESC_REQ: case BLE_NOTIFY_REQ:
        if (ble_present() && c >= 0) { clients[c].ble_user = 1; on_ble_request(type, p, end); }
        break;
    case SET_KEY_REQ: {
        /* Without a key: from an encrypted (zero-PSK) connection only, so the key never crosses the network readable.
         * With one: only from a connection that has it (Home Assistant rotating or clearing it). */
        const unsigned char *k = NULL; size_t kl = 0; int ok = 0;
        while (pb_next(&p, end, &f)) if (f.field == 1 && f.data) { k = f.data; kl = f.len; }
        if (c >= 0 && (have_key ? clients[c].keyed : clients[c].enc)) { ok = key_set(k, kl); if (ok && have_key) clients[c].keyed = 1; }
        else fprintf(stderr, "encryption: key change refused (%s connection)\n", have_key ? "unkeyed" : "plaintext");
        PB(b, 8); pb_uint(&b, 1, ok); send_msg(SET_KEY_RESP, &b);
    } break;
    }
    pthread_mutex_unlock(&core_lock);
    return keep;
}

static int read_varint(int fd, uint32_t *out)
{
    uint32_t v = 0; unsigned char c;
    for (int sh = 0; sh < 35; sh += 7) { if (read_full(fd, &c, 1) != 1) return -1; v |= (uint32_t)(c & 0x7f) << sh; if (!(c & 0x80)) { *out = v; return 0; } }
    return -1;
}

/* Body of a Noise frame whose 0x01 has been read.  buf holds 64 KiB, the most a frame can carry.  -1 on error. */
static long read_frame(int fd, unsigned char *buf)
{
    unsigned char h[2];
    if (read_full(fd, h, 2) != 2) return -1;
    size_t len = (size_t)h[0] << 8 | h[1];
    return len && read_full(fd, buf, len) != (ssize_t)len ? -1 : (long)len;
}

/* Client hello (ignored), handshake message 1, our server hello and message 2.  With a key only that key gets through;
 * without one only the all-zero PSK, which is what Home Assistant uses to set the key.  0 = encrypted from here on. */
static int noise_handshake(int fd, int slot, unsigned char *buf)
{
    static const uint8_t zero[32]; uint8_t psk[32], payload[64], out[32 + NOISE_TAG + 1]; unsigned char pre;
    struct noise_hs hs; struct noise_cs tx, rx; char hello[160]; int keyed; long n;
    if (read_frame(fd, buf) < 0 || read_full(fd, &pre, 1) != 1 || pre != 1 || (n = read_frame(fd, buf)) < 1 || buf[0] != 0) return -1;
    pthread_mutex_lock(&core_lock);
    keyed = have_key; memcpy(psk, have_key ? api_key : zero, 32);
    pthread_mutex_unlock(&core_lock);
    size_t hl = 1 + snprintf(hello + 1, sizeof hello - 1, "%s%c%s", node_name(), 0, mac_plain()) + 1;
    hello[0] = 1;                                       /* chosen protocol: Noise */
    if (write_frame(fd, hello, hl) < 0) return -1;
    noise_nn_responder_init(&hs, "NoiseAPIInit\0\0", 14);
    if (n - 1 > 32 + NOISE_TAG + (long)sizeof payload || noise_nn_read_msg1(&hs, psk, buf + 1, n - 1, payload) < 0) {
        static const char fail[] = "\x01Handshake MAC failure";           /* what the client turns into "invalid key" */
        write_frame(fd, fail, sizeof fail - 1);
        fprintf(stderr, "encryption: handshake with the wrong key (%s)\n", keyed ? "not the device key" : "no key set yet, expected the zero PSK");
        memset(psk, 0, sizeof psk);
        return -1;
    }
    memset(psk, 0, sizeof psk);
    out[0] = 0;                                         /* handshake OK */
    size_t ml = noise_nn_write_msg2(&hs, NULL, 0, out + 1, &tx, &rx);
    pthread_mutex_lock(&core_lock);
    clients[slot].tx = tx; clients[slot].rx = rx; clients[slot].enc = 1; clients[slot].keyed = keyed;
    int rc = write_frame(fd, out, ml + 1);              /* under the lock: no state may overtake it */
    pthread_mutex_unlock(&core_lock);
    memset(&tx, 0, sizeof tx); memset(&rx, 0, sizeof rx);
    return rc;
}

static void serve(int fd)
{
    unsigned char *buf = malloc(1 << 16), *pt = malloc(1 << 16), pre; uint32_t len, type, cap = 1 << 16; int slot = -1, n = 0, enc = 0;
    if (!buf || !pt) { free(buf); free(pt); return; }
    pthread_mutex_lock(&core_lock);
    { static int loaded; if (!loaded) { loaded = 1; settings_load(); key_load(); pthread_t t; pthread_create(&t, NULL, diag_thread, NULL); pthread_detach(t);
                                       ble_start(&ble_handler); a2dp_start(bt_changed); } }
    for (int i = 0; i < MAX_CLIENTS; i++) { if (clients[i].fd < 0 && slot < 0) slot = i; n += clients[i].fd >= 0; }
    if (slot >= 0) { clients[slot].fd = fd; clients[slot].states = clients[slot].enc = clients[slot].keyed = clients[slot].ble = clients[slot].ble_free = clients[slot].ble_user = clients[slot].actions = 0; if (!n) core_link(1, 0); }
    int keyed = have_key;
    pthread_mutex_unlock(&core_lock);
    if (slot < 0) { fprintf(stderr, "client refused: %d connections already\n", MAX_CLIENTS); free(buf); free(pt); return; }
    reply_fd = fd;

    if (read_full(fd, &pre, 1) != 1) goto done;
    if (pre == 1) {
        if (noise_handshake(fd, slot, buf)) goto done;
        enc = 1;
    } else if (keyed) {                                 /* the client sees 0x01 and reports "requires encryption" */
        fprintf(stderr, "encryption: plaintext connection refused, the device has a key\n");
        write_frame(fd, "", 0);
        goto done;
    }
    fprintf(stderr, "client connected (%d, %s)\n", n + 1, enc ? (keyed ? "encrypted" : "zero-PSK, to set the key") : "plaintext");

    for (int first = 1;; first = 0) {
        const unsigned char *msg = buf;
        if (enc) {
            long fl, pl;
            if (read_full(fd, &pre, 1) != 1 || pre != 1 || (fl = read_frame(fd, buf)) < 0) break;
            if ((pl = noise_decrypt(&clients[slot].rx, buf, fl, pt)) < 4) { fprintf(stderr, "encryption: bad frame, closing\n"); break; }
            type = pt[0] << 8 | pt[1]; len = pt[2] << 8 | pt[3];
            if (len != (uint32_t)pl - 4) break;
            msg = pt + 4;
        } else {
            if (!first && read_full(fd, &pre, 1) != 1) break;
            if (pre != 0) break;
            if (read_varint(fd, &len) || read_varint(fd, &type) || len > (1 << 20)) break;
            if (len > cap) { unsigned char *nb = realloc(buf, len); if (!nb) break; buf = nb; cap = len; msg = buf; }
            if (len && read_full(fd, buf, len) != (ssize_t)len) break;
        }
        if (!handle(type, msg, len)) break;
    }

done:

    pthread_mutex_lock(&core_lock);
    clients[slot].fd = -1; clients[slot].states = clients[slot].enc = clients[slot].keyed = clients[slot].ble = clients[slot].ble_free = clients[slot].ble_user = clients[slot].actions = 0; n = 0;
    ble_update();
    if (!ble_subscribers()) {                           /* nobody left to use them */
        int users = 0; uint64_t a[BLE_MAX_CONN];
        for (int i = 0; i < MAX_CLIENTS; i++) users += clients[i].fd >= 0 && (clients[i].ble_free || clients[i].ble_user);
        if (!users) for (int i = 0, k = ble_connections(a); i < k; i++) ble_disconnect(a[i]);
    }
    memset(&clients[slot].tx, 0, sizeof clients[slot].tx); memset(&clients[slot].rx, 0, sizeof clients[slot].rx);
    for (int i = 0; i < MAX_CLIENTS; i++) n += clients[i].fd >= 0;
    if (va_fd == fd) { va_fd = -1; tts_expected = 0; core_link(n > 0, 0); }      /* the voice assistant's client left: pipelines end */
    else if (!n) core_link(0, 0);
    pthread_mutex_unlock(&core_lock);
    free(buf); free(pt);
    fprintf(stderr, "client disconnected (%d left)\n", n);
}

const struct proto proto_esphome = { "esphome", 26053, 1, serve, start, audio, stop, played, volume_changed, mute_changed, print_mdns, bt_device };
