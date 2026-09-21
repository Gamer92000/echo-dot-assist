/* Wyoming satellite protocol: Home Assistant connects to us (wyoming integration). */
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include "core.h"
#include "wyoming.h"

#define TTS_RATE 22050      /* assumed when audio-start carries no rate */

static int client = -1;

static void send_event(const char *type, const char *data, const void *payload, size_t len)
{
    if (client >= 0 && wy_write(client, type, data, payload, len) < 0)
        shutdown(client, SHUT_RDWR);        /* reader thread notices and cleans up */
}

static void start(void)
{
    char data[256];
    snprintf(data, sizeof data,
             "{\"start_stage\":\"%s\",\"end_stage\":\"tts\",\"restart_on_end\":false%s}",
             core_local_wake ? "asr" : "wake", core_local_wake ? ",\"wake_word_name\":\"alexa\"" : "");
    send_event("run-pipeline", data, NULL, 0);
}

static void audio(const void *pcm, size_t len)
{
    char data[96]; struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    snprintf(data, sizeof data, "{\"rate\":16000,\"width\":2,\"channels\":1,\"timestamp\":%lld}",
             (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    send_event("audio-chunk", data, pcm, len);
}

static void played(void) { send_event("played", NULL, NULL, 0); }

static void send_info(void)
{
    char data[1024];
    snprintf(data, sizeof data,
             "{\"asr\":[],\"tts\":[],\"handle\":[],\"intent\":[],\"wake\":[],\"mic\":[],\"snd\":[],"
             "\"satellite\":{\"name\":\"%s\",\"attribution\":{\"name\":\"hassmic\",\"url\":\"\"},"
             "\"installed\":true,\"description\":\"Echo Dot 3 with Amazon audio front end\",\"version\":\"" VERSION "\","
             "\"area\":null,\"has_vad\":false,\"active_wake_words\":[%s],\"max_active_wake_words\":1,"
             "\"supports_trigger\":true}}",
             core_name, core_local_wake ? "\"alexa\"" : "");
    send_event("info", data, NULL, 0);
}

static void handle(const struct wy_event *ev)
{
    const char *t = ev->type;
    if (!strcmp(t, "audio-chunk")) { core_tts_data(ev->payload, ev->payload_len); return; }

    pthread_mutex_lock(&core_lock);
    if (!strcmp(t, "ping")) send_event("pong", NULL, NULL, 0);
    else if (!strcmp(t, "describe")) send_info();
    else if (!strcmp(t, "run-satellite")) { fprintf(stderr, "satellite: running\n"); core_link(1, 1); }
    else if (!strcmp(t, "pause-satellite")) core_link(1, 0);
    else if (!strcmp(t, "detection")) core_set_state(LISTENING);
    else if (!strcmp(t, "voice-stopped") || !strcmp(t, "transcript")) {
        if (!strcmp(t, "transcript")) {
            char text[256] = "";
            wy_json_str(strchr(ev->json, '\n') ? strchr(ev->json, '\n') : ev->json, "text", text, sizeof text);
            fprintf(stderr, "transcript: %s\n", text);
            core_mic_off();
        }
        if (core_state() == LISTENING) core_set_state(THINKING);
    } else if (!strcmp(t, "audio-start")) {
        long rate = TTS_RATE, ch = 1, width = 2;
        wy_json_int(ev->json, "rate", &rate); wy_json_int(ev->json, "channels", &ch); wy_json_int(ev->json, "width", &width);
        if (width != 2) fprintf(stderr, "play: unsupported sample width %ld, expect noise\n", width);
        core_tts_begin(rate, ch);
    } else if (!strcmp(t, "audio-stop")) core_tts_end();
    else if (!strcmp(t, "error")) { fprintf(stderr, "server error: %s\n", ev->json); core_error(); }
    pthread_mutex_unlock(&core_lock);
}

static void serve(int fd)
{
    struct wy_reader rd; static struct wy_event ev;
    if (wy_reader_init(&rd, fd) < 0) return;
    pthread_mutex_lock(&core_lock);
    client = fd; core_link(1, 0);
    pthread_mutex_unlock(&core_lock);
    fprintf(stderr, "client connected\n");

    while (wy_read(&rd, &ev) > 0) handle(&ev);

    pthread_mutex_lock(&core_lock);
    client = -1; core_link(0, 0);
    pthread_mutex_unlock(&core_lock);
    wy_reader_free(&rd);
    fprintf(stderr, "client disconnected\n");
}

static void print_mdns(void)
{
    printf("<?xml version=\"1.0\" standalone='no'?>\n<!DOCTYPE service-group SYSTEM \"avahi-service.dtd\">\n"
           "<service-group>\n  <name replace-wildcards=\"yes\">%s %%h</name>\n"
           "  <service><type>_wyoming._tcp</type><port>%d</port></service>\n</service-group>\n", core_name, core_port);
}

const struct proto proto_wyoming = { "wyoming", 16700, serve, start, audio, NULL, played, NULL, NULL, print_mdns };
