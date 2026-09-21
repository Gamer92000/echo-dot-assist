/*
 * ESPHome native API, plaintext, server side: Home Assistant's esphome integration connects to us and sees a voice
 * assistant device with a media player.  Message numbers and fields follow esphome/components/api/api.proto.
 *
 *   voice pipeline   VoiceAssistantRequest -> mic audio as VoiceAssistantAudio (API_AUDIO) -> events.  The reply is fetched
 *                    over http like an announcement: without the SPEAKER flag Home Assistant renders TTS in the media
 *                    player's announcement format (48 kHz) instead of streaming 16 kHz over the API connection.
 *   announcements    VoiceAssistantAnnounceRequest with http URLs; Home Assistant transcodes to the WAV format the media
 *                    player entity advertises, we stream it.  Same path for media_player.play_media.
 *   timers           finished timer rings (core_alarm)
 */
#include <ctype.h>
#include <netdb.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "core.h"
#include "netio.h"
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD                 /* plain C: same code on the PC and on the armv7 build */
#include "../third_party/minimp3.h"

enum {
    HELLO_REQ = 1, HELLO_RESP, CONNECT_REQ, CONNECT_RESP, DISCONNECT_REQ, DISCONNECT_RESP, PING_REQ, PING_RESP,
    DEVICE_INFO_REQ, DEVICE_INFO_RESP, LIST_ENTITIES_REQ, LIST_ENTITIES_DONE = 19, SUBSCRIBE_STATES = 20,
    LIST_SWITCH = 17, SWITCH_STATE = 26, SWITCH_COMMAND = 33, LIST_NUMBER = 49, NUMBER_STATE, NUMBER_COMMAND,
    LIST_SELECT = 52, SELECT_STATE, SELECT_COMMAND,
    LIST_MEDIA_PLAYER = 63, MEDIA_PLAYER_STATE, MEDIA_PLAYER_COMMAND,
    SUBSCRIBE_VA = 89, VA_REQUEST, VA_RESPONSE, VA_EVENT, VA_AUDIO = 106, VA_TIMER_EVENT = 115,
    VA_ANNOUNCE = 119, VA_ANNOUNCE_FINISHED, VA_CONFIG_REQ, VA_CONFIG_RESP, VA_SET_CONFIG,
};
enum { EV_ERROR = 0, EV_RUN_START, EV_RUN_END, EV_STT_START, EV_STT_END, EV_INTENT_START, EV_INTENT_END, EV_TTS_START,
       EV_TTS_END, EV_WAKE_START, EV_WAKE_END, EV_VAD_START, EV_VAD_END, EV_TTS_STREAM_START = 98, EV_TTS_STREAM_END = 99, EV_INTENT_PROGRESS = 100 };
enum { FEAT_VOICE = 1, FEAT_SPEAKER = 2, FEAT_API_AUDIO = 4, FEAT_TIMERS = 8, FEAT_ANNOUNCE = 16, FEAT_START_CONVERSATION = 32 };
enum { KEY_NOISE = 2, KEY_GAIN, KEY_MULT, KEY_MUTE, KEY_WAKE_SOUND };
enum { MP_KEY = 1, MP_IDLE = 1, MP_PLAYING = 2, MP_CMD_STOP = 2, MP_CMD_MUTE = 3, MP_CMD_UNMUTE = 4 };
#define MEDIA_RATE 48000        /* what we ask Home Assistant to transcode announcements and media to: WAV mono s16 */

static int client = -1;
static int tts_expected;                /* lock held: the reply is being fetched or played; RUN_END must not end the pipeline */
static char tts_url[1024];              /* lock held: reply URL of the running pipeline (known from RUN_START with streaming TTS) */
static int announcing, media_playing;   /* lock held */
static atomic_int media_busy;

/* ---------------------------------------------------------------- protobuf */

struct pb { unsigned char *p; size_t n, cap; };

static void pb_raw(struct pb *b, const void *d, size_t n) { if (b->n + n <= b->cap) { memcpy(b->p + b->n, d, n); b->n += n; } }
static void pb_varint(struct pb *b, uint64_t v) { unsigned char c; do { c = v & 0x7f; v >>= 7; if (v) c |= 0x80; pb_raw(b, &c, 1); } while (v); }
static void pb_uint(struct pb *b, int f, uint64_t v) { if (v) { pb_varint(b, f << 3); pb_varint(b, v); } }
static void pb_bytes(struct pb *b, int f, const void *d, size_t n) { if (n) { pb_varint(b, f << 3 | 2); pb_varint(b, n); pb_raw(b, d, n); } }
static void pb_str(struct pb *b, int f, const char *s) { pb_bytes(b, f, s, strlen(s)); }
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

/* lock held */
static void send_msg(unsigned type, const struct pb *b)
{
    unsigned char h[12]; struct pb hb = { h, 0, sizeof h };
    if (client < 0) return;
    pb_raw(&hb, "", 1); pb_varint(&hb, b ? b->n : 0); pb_varint(&hb, type);
    if (write_all(client, h, hb.n) < 0 || (b && b->n && write_all(client, b->p, b->n) < 0)) shutdown(client, SHUT_RDWR);
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

static void print_mdns(void)
{
    char m[24]; size_t j = 0;
    for (const char *c = mac(); *c; c++) if (*c != ':') m[j++] = tolower((unsigned char)*c);
    m[j] = 0;
    printf("<?xml version=\"1.0\" standalone='no'?>\n<!DOCTYPE service-group SYSTEM \"avahi-service.dtd\">\n"
           "<service-group>\n  <name>%s</name>\n  <service><type>_esphomelib._tcp</type><port>%d</port>\n"
           "    <txt-record>mac=%s</txt-record><txt-record>friendly_name=%s</txt-record><txt-record>version=2025.5.0</txt-record>\n"
           "    <txt-record>platform=hassmic</txt-record><txt-record>network=wifi</txt-record>\n  </service>\n</service-group>\n",
           node_name(), core_port, m, core_name);
}

/* ---------------------------------------------------------------- settings entities
 * Home Assistant applies these to the mic stream on its side (they travel in every VoiceAssistantRequest), like the
 * controls of its Wyoming integration.  Kept in a small file so they survive restarts. */

static const char *const noise_names[] = { "Off", "Low", "Medium", "High", "Max" };
static int noise_level, auto_gain; static float vol_mult = 1.0f;
static const char *settings_path(void) { const char *p = getenv("HASSMIC_SETTINGS"); return p ? p : "/data/local/hassmic/state/settings"; }

static void settings_load(void)
{
    int n, g, m, w; float v; FILE *f = fopen(settings_path(), "r");
    if (!f) return;
    if (fscanf(f, "%d %d %f %d %d", &n, &g, &v, &m, &w) == 5) {
        noise_level = n < 0 ? 0 : n > 4 ? 4 : n; auto_gain = g < 0 ? 0 : g > 31 ? 31 : g; vol_mult = v < 0.1f ? 0.1f : v > 10 ? 10 : v;
        core_soft_mute(m != 0); core_wake_sound(w != 0);
    }
    fclose(f);
}

static void settings_save(void)
{
    FILE *f = fopen(settings_path(), "w");
    if (!f) { fprintf(stderr, "settings: cannot write %s\n", settings_path()); return; }
    fprintf(f, "%d %d %.2f %d %d\n", noise_level, auto_gain, vol_mult, core_soft_mute(-1), core_wake_sound(-1));
    fclose(f);
}

static void send_setting(int key)       /* lock held */
{
    PB(b, 48);
    pb_fixed32(&b, 1, key);
    switch (key) {
    case KEY_NOISE: pb_str(&b, 2, noise_names[noise_level]); send_msg(SELECT_STATE, &b); break;
    case KEY_GAIN:  pb_float(&b, 2, auto_gain); send_msg(NUMBER_STATE, &b); break;
    case KEY_MULT:  pb_float(&b, 2, vol_mult); send_msg(NUMBER_STATE, &b); break;
    case KEY_MUTE:  pb_uint(&b, 2, core_muted()); send_msg(SWITCH_STATE, &b); break;
    case KEY_WAKE_SOUND: pb_uint(&b, 2, core_wake_sound(-1)); send_msg(SWITCH_STATE, &b); break;
    }
}

static void send_setting_entities(void)
{
    { PB(b, 256); pb_str(&b, 1, "noise_suppression_level"); pb_fixed32(&b, 2, KEY_NOISE); pb_str(&b, 3, "Noise suppression level");
      pb_str(&b, 5, "mdi:volume-off"); for (int i = 0; i < 5; i++) pb_str(&b, 6, noise_names[i]); pb_uint(&b, 8, 1); send_msg(LIST_SELECT, &b); }
    { PB(b, 256); pb_str(&b, 1, "auto_gain"); pb_fixed32(&b, 2, KEY_GAIN); pb_str(&b, 3, "Auto gain"); pb_str(&b, 5, "mdi:microphone-plus");
      pb_float(&b, 6, 0); pb_float(&b, 7, 31); pb_float(&b, 8, 1); pb_uint(&b, 10, 1); pb_str(&b, 11, "dBFS"); pb_uint(&b, 12, 2); send_msg(LIST_NUMBER, &b); }
    { PB(b, 256); pb_str(&b, 1, "mic_volume_multiplier"); pb_fixed32(&b, 2, KEY_MULT); pb_str(&b, 3, "Mic volume multiplier"); pb_str(&b, 5, "mdi:volume-vibrate");
      pb_float(&b, 6, 0.1f); pb_float(&b, 7, 10); pb_float(&b, 8, 0.1f); pb_uint(&b, 10, 1); pb_uint(&b, 12, 1); send_msg(LIST_NUMBER, &b); }
    { PB(b, 128); pb_str(&b, 1, "mute"); pb_fixed32(&b, 2, KEY_MUTE); pb_str(&b, 3, "Mute"); pb_str(&b, 5, "mdi:microphone-off"); send_msg(LIST_SWITCH, &b); }
    { PB(b, 128); pb_str(&b, 1, "wake_sound"); pb_fixed32(&b, 2, KEY_WAKE_SOUND); pb_str(&b, 3, "Wake sound"); pb_str(&b, 5, "mdi:bell-ring");
      pb_uint(&b, 8, 1); send_msg(LIST_SWITCH, &b); }
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
    else if (type == NUMBER_COMMAND && key == KEY_GAIN) auto_gain = num < 0 ? 0 : num > 31 ? 31 : (int)(num + 0.5f);
    else if (type == NUMBER_COMMAND && key == KEY_MULT) vol_mult = num < 0.1f ? 0.1f : num > 10 ? 10 : num;
    else if (type == SWITCH_COMMAND && key == KEY_MUTE) { core_soft_mute(on); settings_save(); send_setting(KEY_MUTE); return; }
    else if (type == SWITCH_COMMAND && key == KEY_WAKE_SOUND) core_wake_sound(on);
    else return;
    fprintf(stderr, "settings: noise=%s gain=%d mult=%.1f mute=%d wake_sound=%d\n", noise_names[noise_level], auto_gain, vol_mult, core_soft_mute(-1), core_wake_sound(-1));
    settings_save(); send_setting(key);
}

/* ---------------------------------------------------------------- media over http (announcements, play_media) */

struct media_job { char url[2][1024]; int announce, start_conversation; };

static void send_mp_state(void)         /* lock held */
{
    PB(b, 32);
    pb_fixed32(&b, 1, MP_KEY); pb_uint(&b, 2, media_playing ? MP_PLAYING : MP_IDLE); pb_float(&b, 3, core_volume() / 100.0f);
    send_msg(MEDIA_PLAYER_STATE, &b);
}

static int http_get_host(const char *host, const char *port, const char *path)
{
    char req[1400], line[1024]; struct addrinfo hints = { 0 }, *ai;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &ai)) { fprintf(stderr, "media: cannot resolve %s\n", host); return -1; }
    int fd = socket(ai->ai_family, SOCK_STREAM, 0);
    struct timeval tv = { 4, 0 };                       /* connect() obeys the send timeout */
    if (fd >= 0) { setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv); }
    if (fd < 0 || connect(fd, ai->ai_addr, ai->ai_addrlen)) {
        fprintf(stderr, "media: cannot connect to %s:%s (not a local address? see lockdown.sh)\n", host, port);
        if (fd >= 0) close(fd);
        freeaddrinfo(ai); return -1;
    }
    freeaddrinfo(ai);
    tv.tv_sec = 15; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);     /* TTS may still be rendering */
    /* HTTP/1.0: no chunked transfer encoding, the body ends with the connection */
    int n = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s:%s\r\nUser-Agent: hassmic/" VERSION "\r\n\r\n", path, host, port);
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
    char *colon = strrchr(host, ':'); if (colon) { *colon = 0; snprintf(port, sizeof port, "%s", colon + 1); }
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
        if (!began) { PB(b, 8); pb_uint(&b, 1, 0); send_msg(VA_ANNOUNCE_FINISHED, &b); announcing = 0; }
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
        if (announce) { PB(b, 8); pb_uint(&b, 1, 0); send_msg(VA_ANNOUNCE_FINISHED, &b); }
        free(job); return;
    }
    if (!is_reply && (core_state() == LISTENING || core_state() == THINKING)) core_pipeline_finish();
    snprintf(job->url[0], sizeof job->url[0], "%s", url0); snprintf(job->url[1], sizeof job->url[1], "%s", url1);
    job->announce = announce; job->start_conversation = start_conversation;
    announcing = announce; media_playing = 1; send_mp_state();
    fprintf(stderr, "media: %s%s\n", announce ? "announce " : "", url1);
    pthread_create(&t, NULL, media_thread, job); pthread_detach(t);
}

/* ---------------------------------------------------------------- core callbacks (lock held) */

static void start(void)
{
    PB(b, 96);
    tts_expected = 0; tts_url[0] = 0;
    pb_uint(&b, 1, 1);                                  /* start */
    pb_uint(&b, 3, core_local_wake ? 1 : 3);            /* flags: USE_VAD, plus USE_WAKE_WORD when detection is remote */
    { PB(as, 24); pb_uint(&as, 1, noise_level); pb_uint(&as, 2, auto_gain); pb_float(&as, 3, vol_mult);
      pb_varint(&b, 4 << 3 | 2); pb_varint(&b, as.n); pb_raw(&b, as.p, as.n); }
    if (core_local_wake) pb_str(&b, 5, "Alexa");
    send_msg(VA_REQUEST, &b);
}

static void audio(const void *pcm, size_t len)
{
    static unsigned char buf[4096 + 16];
    for (size_t off = 0; off < len; off += 4096) {
        struct pb b = { buf, 0, sizeof buf };
        pb_bytes(&b, 1, (const char *)pcm + off, len - off < 4096 ? len - off : 4096);
        send_msg(VA_AUDIO, &b);
    }
}

static void stop(void)
{
    PB(a, 8); pb_uint(&a, 2, 1); send_msg(VA_AUDIO, &a);        /* end of audio */
    send_msg(VA_REQUEST, NULL);                                 /* start = false */
}

static void played(void)
{
    if (announcing) { PB(b, 8); pb_uint(&b, 1, 1); send_msg(VA_ANNOUNCE_FINISHED, &b); announcing = 0; }
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
    pb_uint(&b, 17, FEAT_VOICE | FEAT_API_AUDIO | FEAT_TIMERS | FEAT_ANNOUNCE | FEAT_START_CONVERSATION);      /* no SPEAKER: see top */
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

static void send_va_config(void)
{
    PB(b, 128); PB(w, 64);
    if (core_local_wake) {
        pb_str(&w, 1, "alexa"); pb_str(&w, 2, "Alexa"); pb_str(&w, 3, "en");
        pb_varint(&b, 1 << 3 | 2); pb_varint(&b, w.n); pb_raw(&b, w.p, w.n);
        pb_str(&b, 2, "alexa"); pb_uint(&b, 3, 1);
    }
    send_msg(VA_CONFIG_RESP, &b);
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
    switch (type) {
    case HELLO_REQ: { PB(b, 128); pb_uint(&b, 1, 1); pb_uint(&b, 2, 10); pb_str(&b, 3, "hassmic " VERSION); pb_str(&b, 4, node_name()); send_msg(HELLO_RESP, &b); } break;
    case CONNECT_REQ:      send_msg(CONNECT_RESP, NULL); break;                 /* no password */
    case DISCONNECT_REQ:   send_msg(DISCONNECT_RESP, NULL); keep = 0; break;
    case PING_REQ:         send_msg(PING_RESP, NULL); break;
    case DEVICE_INFO_REQ:  send_device_info(); break;
    case LIST_ENTITIES_REQ: send_entities(); break;
    case SUBSCRIBE_STATES: send_mp_state(); for (int k = KEY_NOISE; k <= KEY_WAKE_SOUND; k++) send_setting(k); break;
    case SELECT_COMMAND: case NUMBER_COMMAND: case SWITCH_COMMAND: on_setting(type, p, end); break;
    case SUBSCRIBE_VA: {
        int sub = 0;
        while (pb_next(&p, end, &f)) if (f.field == 1) sub = f.v != 0;
        fprintf(stderr, "voice assistant: %s\n", sub ? "subscribed" : "unsubscribed");
        core_link(1, sub);
    } break;
    case VA_RESPONSE:
        while (pb_next(&p, end, &f)) if (f.field == 2 && f.v) { fprintf(stderr, "pipeline refused\n"); core_error(); }
        break;
    case VA_EVENT:         on_event(p, end); break;
    case VA_TIMER_EVENT:   on_timer(p, end); break;
    case VA_ANNOUNCE:      on_announce(p, end); break;
    case VA_CONFIG_REQ:    send_va_config(); break;
    case VA_SET_CONFIG:    break;                                               /* one wake word, nothing to choose */
    case MEDIA_PLAYER_COMMAND: on_mp_command(p, end); break;
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

static void serve(int fd)
{
    unsigned char *buf = malloc(1 << 20), pre; uint32_t len, type;
    if (!buf) return;
    pthread_mutex_lock(&core_lock);
    { static int loaded; if (!loaded) { loaded = 1; settings_load(); } }
    client = fd; core_link(1, 0);
    pthread_mutex_unlock(&core_lock);
    fprintf(stderr, "client connected\n");

    for (;;) {
        if (read_full(fd, &pre, 1) != 1) break;
        if (pre != 0) { fprintf(stderr, "client wants encryption (preamble %u): remove the key in Home Assistant\n", pre); break; }
        if (read_varint(fd, &len) || read_varint(fd, &type) || len > (1 << 20)) break;
        if (len && read_full(fd, buf, len) != (ssize_t)len) break;
        if (!handle(type, buf, len)) break;
    }

    pthread_mutex_lock(&core_lock);
    client = -1; tts_expected = 0; core_link(0, 0);
    pthread_mutex_unlock(&core_lock);
    free(buf);
    fprintf(stderr, "client disconnected\n");
}

const struct proto proto_esphome = { "esphome", 26053, serve, start, audio, stop, played, volume_changed, mute_changed, print_mdns };
