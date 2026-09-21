/*
 * hassmic - Home Assistant voice satellite for the Echo Dot 3 (donut).
 *
 * Replaces PuffinApp as the client of Amazon's `mixer` daemon: reads the post-AEC/beamformer stream,
 * runs the stock "Alexa" wake word locally, and speaks Wyoming to Home Assistant.
 *
 *   hassmic [-p port] [-n name] [-w local|remote] [-m pryon.manifest] [-b input-device] [-L] [-E] [-V]
 *     -L no LED ring   -E no earcon on wake   -V leave the volume buttons alone
 *
 * Default port 16700: the stock firewall only admits inbound TCP 16384-32767.
 */
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "audio.h"
#include "buttons.h"
#include "netio.h"
#include "wake.h"
#include "wyoming.h"

#define VERSION          "0.1.0"
#define DEFAULT_MANIFEST "/system/local/models/keyword/en-US/ALEXA/pryon.manifest"
#define PIPELINE_TIMEOUT 30         /* seconds in LISTENING or THINKING before giving up */
#define TTS_RATE         22050      /* assumed when audio-start carries no rate */

enum state { IDLE, LISTENING, THINKING, SPEAKING };
static const char *const state_names[] = { "idle", "listening", "thinking", "speaking" };

static const char *name = "Echo Dot";
static int local_wake = 1, use_led = 1, use_earcon = 1, use_volume = 1;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;   /* guards client, state, state_since and socket writes */
static int client = -1;
static int satellite_running;
static enum state state;
static time_t state_since;
static atomic_int streaming, trigger_pending, quit;
static atomic_int flush_playback, earcon_pending;   /* barge-in: drop queued TTS; wake sound requested */
static int barge_in;                                /* under lock: start a new pipeline once the old one has ended */

/* ---------------------------------------------------------------- LED ring */

static void run(const char *path, const char *a1, const char *a2)
{
    char *argv[] = { (char *)path, (char *)a1, (char *)a2, NULL };
    if (fork() == 0) {                  /* bionic API 24 has no posix_spawn; SIGCHLD is ignored: no zombie */
        execv(path, argv);
        _exit(127);
    }
}

static void led(const char *op, const char *pattern)
{
    if (use_led && pattern) run("/system/bin/ledctrl", op, pattern);
}

static void led_for(enum state from, enum state to)
{
    static const char *const pattern[] = { NULL, "ca-active-start", "active-thinking", "active-talking" };
    if (pattern[from]) led("-u", pattern[from]);
    if (pattern[to]) led("-s", pattern[to]);
    else if (from != IDLE) led("-s", "ca-active-end");
}

/* ---------------------------------------------------------------- state (call with lock held) */

static void set_state(enum state s)
{
    if (s == state) return;
    fprintf(stderr, "state: %s -> %s\n", state_names[state], state_names[s]);
    led_for(state, s);
    state = s; state_since = time(NULL);
}

static void send_event(const char *type, const char *data, const void *payload, size_t len)
{
    if (client >= 0 && wy_write(client, type, data, payload, len) < 0) {
        shutdown(client, SHUT_RDWR);        /* reader thread notices and cleans up */
    }
}

static void pipeline_start(void)
{
    char data[256];
    snprintf(data, sizeof data,
             "{\"start_stage\":\"%s\",\"end_stage\":\"tts\",\"restart_on_end\":false%s}",
             local_wake ? "asr" : "wake", local_wake ? ",\"wake_word_name\":\"alexa\"" : "");
    send_event("run-pipeline", data, NULL, 0);
    atomic_store(&streaming, 1);
    if (local_wake) set_state(LISTENING);
}

static void pipeline_finish(void)
{
    atomic_store(&streaming, 0);
    set_state(IDLE);
    wake_reset();
    if (client < 0) barge_in = 0;
    if ((barge_in || !local_wake) && satellite_running && client >= 0) {
        if (barge_in) atomic_store(&earcon_pending, 1);
        barge_in = 0;
        pipeline_start();
    }
}

static void trigger(void)
{
    pthread_mutex_lock(&lock);
    if (client < 0 || !satellite_running || buttons_muted()) {
        /* nothing to talk to, or privacy latch on */
    } else if (state == IDLE) {
        atomic_store(&earcon_pending, 1);
        pipeline_start();
    } else if (state == SPEAKING && !barge_in) {
        fprintf(stderr, "barge-in\n");
        barge_in = 1;
        atomic_store(&flush_playback, 1);   /* playback thread drops TTS up to audio-stop, then we restart */
    }
    pthread_mutex_unlock(&lock);
}

static void on_wake(const char *keyword) { (void)keyword; if (local_wake) trigger(); }

/* ---------------------------------------------------------------- playback queue */

struct item { struct item *next; int kind; unsigned rate, ch; size_t len; unsigned char data[]; };
enum { Q_START, Q_DATA, Q_STOP };

static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cond = PTHREAD_COND_INITIALIZER;
static struct item *q_head, *q_tail;

static void q_push(int kind, unsigned rate, unsigned ch, const void *data, size_t len)
{
    struct item *it = malloc(sizeof *it + len);
    if (!it) return;
    it->next = NULL; it->kind = kind; it->rate = rate; it->ch = ch; it->len = len;
    if (len) memcpy(it->data, data, len);
    pthread_mutex_lock(&q_lock);
    if (q_tail) q_tail->next = it; else q_head = it;
    q_tail = it;
    pthread_cond_signal(&q_cond);
    pthread_mutex_unlock(&q_lock);
}

static void *playback_thread(void *arg)
{
    int open = 0;
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&q_lock);
        while (!q_head) pthread_cond_wait(&q_cond, &q_lock);
        struct item *it = q_head;
        q_head = it->next; if (!q_head) q_tail = NULL;
        pthread_mutex_unlock(&q_lock);

        if (atomic_load(&flush_playback) && it->kind != Q_STOP) {
            if (open) { play_close(0); open = 0; }
            free(it);
            continue;
        }
        switch (it->kind) {
        case Q_START:
            if (open) play_close(0);
            open = play_open(it->rate, it->ch) == 0;
            if (!open) fprintf(stderr, "play: open %u Hz x%u failed\n", it->rate, it->ch);
            break;
        case Q_DATA:
            if (open && play_write(it->data, it->len) < 0) { play_close(0); open = 0; }
            break;
        case Q_STOP:
            if (open) { play_close(1); open = 0; }
            atomic_store(&flush_playback, 0);
            pthread_mutex_lock(&lock);
            send_event("played", NULL, NULL, 0);
            pipeline_finish();
            pthread_mutex_unlock(&lock);
            break;
        }
        free(it);
    }
    return NULL;
}

/* ---------------------------------------------------------------- earcon, buttons */

static void *earcon_thread(void *arg)
{
    enum { RATE = 48000, N = RATE * 12 / 100 };
    static short tone[N];
    (void)arg;
    for (int i = 0; i < N; i++) {           /* 120 ms rising two-tone blip with 10 ms fades */
        double f = i < N / 2 ? 880.0 : 1320.0, env = fmin(1.0, fmin(i, N - i) / (RATE * 0.01));
        tone[i] = (short)(6000 * env * sin(2 * M_PI * f * i / RATE));
    }
    for (;;) {
        if (atomic_exchange(&earcon_pending, 0) && use_earcon) play_earcon(tone, N, RATE);
        usleep(20000);
    }
    return NULL;
}

static void on_action(void) { atomic_store(&trigger_pending, 1); }

static void on_mute(int muted)
{
    fprintf(stderr, "mic mute: %d\n", muted);
    pthread_mutex_lock(&lock);
    if (muted) { if (state == LISTENING) pipeline_finish(); led("-s", "mics-off_on"); }
    else { led("-u", "mics-off_on"); led("-s", "mics-off_end"); }
    pthread_mutex_unlock(&lock);
}

/* Volume: 10 % per press like stock (3 of the ring's 30 steps).  The volume_step-NN animations show 2 s and then loop
 * black forever, so the previous one has to be unset or they pile up in ledcontroller; a timer clears the last one. */
static int volume = -1;                      /* 0..100, read from the device on first use */
static char vol_pat[24];
static atomic_llong vol_clear_at;

static long long mono_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int read_volume(void)
{
    char line[128]; int v = 40, x;
    FILE *f = popen("/system/bin/audio_manager_get_prop MainVolume 2>/dev/null", "r");
    if (!f) return v;
    while (fgets(line, sizeof line, f)) if (sscanf(line, "%d", &x) == 1 && x >= 0 && x <= 100) v = x;
    pclose(f);
    return v;
}

static void on_volume(int dir)
{
    char val[8], pat[24]; int step;
    if (!use_volume) return;
    pthread_mutex_lock(&lock);
    if (volume < 0) volume = read_volume();
    volume = (volume + 5) / 10 * 10 + dir * 10;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    step = volume * 30 / 100 ? volume * 30 / 100 : 1;
    snprintf(val, sizeof val, "%d", volume);
    snprintf(pat, sizeof pat, "volume_step-%02d", step);
    run("/system/bin/audio_manager_set_prop", "MainVolume", val);
    if (vol_pat[0] && strcmp(vol_pat, pat)) led("-u", vol_pat);
    led("-s", pat);
    strcpy(vol_pat, pat);
    atomic_store(&vol_clear_at, mono_ms() + 2500);
    fprintf(stderr, "volume: %d\n", volume);
    pthread_mutex_unlock(&lock);
}

static void *volume_led_thread(void *arg)
{
    (void)arg;
    while (!atomic_load(&quit)) {
        long long at = atomic_load(&vol_clear_at);
        if (at && mono_ms() >= at) {
            pthread_mutex_lock(&lock);
            if (atomic_load(&vol_clear_at) == at) { led("-u", vol_pat); vol_pat[0] = 0; atomic_store(&vol_clear_at, 0); }
            pthread_mutex_unlock(&lock);
        }
        usleep(200000);
    }
    return NULL;
}

/* ---------------------------------------------------------------- capture */

static void *capture_thread(void *arg)
{
    (void)arg;
    while (!atomic_load(&quit)) {
        const void *pcm; int n = cap_read(&pcm);
        if (n < 0) { fprintf(stderr, "capture: fatal\n"); atomic_store(&quit, 1); break; }
        if (atomic_exchange(&trigger_pending, 0)) trigger();
        if (n == 0) continue;

        if (local_wake) wake_feed(pcm, n / 2);
        if (atomic_load(&streaming)) {
            char data[96]; struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            snprintf(data, sizeof data, "{\"rate\":%d,\"width\":2,\"channels\":1,\"timestamp\":%lld}",
                     CAP_RATE, (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
            pthread_mutex_lock(&lock);
            if (atomic_load(&streaming)) send_event("audio-chunk", data, pcm, n);
            pthread_mutex_unlock(&lock);
        }

        pthread_mutex_lock(&lock);
        if (local_wake && (state == LISTENING || state == THINKING) && time(NULL) - state_since > PIPELINE_TIMEOUT) {
            fprintf(stderr, "pipeline timeout\n");
            pipeline_finish();
        }
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

/* ---------------------------------------------------------------- Wyoming server side */

static void send_info(void)
{
    char data[1024];
    snprintf(data, sizeof data,
             "{\"asr\":[],\"tts\":[],\"handle\":[],\"intent\":[],\"wake\":[],\"mic\":[],\"snd\":[],"
             "\"satellite\":{\"name\":\"%s\",\"attribution\":{\"name\":\"hassmic\",\"url\":\"\"},"
             "\"installed\":true,\"description\":\"Echo Dot 3 with Amazon audio front end\",\"version\":\"" VERSION "\","
             "\"area\":null,\"has_vad\":false,\"active_wake_words\":[%s],\"max_active_wake_words\":1,"
             "\"supports_trigger\":true}}",
             name, local_wake ? "\"alexa\"" : "");
    send_event("info", data, NULL, 0);
}

static void handle(const struct wy_event *ev)
{
    const char *t = ev->type;
    if (!strcmp(t, "audio-chunk")) {
        if (!atomic_load(&flush_playback)) q_push(Q_DATA, 0, 0, ev->payload, ev->payload_len);
        return;
    }

    pthread_mutex_lock(&lock);
    if (!strcmp(t, "ping")) send_event("pong", NULL, NULL, 0);
    else if (!strcmp(t, "describe")) send_info();
    else if (!strcmp(t, "run-satellite")) {
        satellite_running = 1;
        fprintf(stderr, "satellite: running\n");
        if (!local_wake && state == IDLE) pipeline_start();
    } else if (!strcmp(t, "pause-satellite")) {
        satellite_running = 0;
        pipeline_finish();
    } else if (!strcmp(t, "detection")) set_state(LISTENING);
    else if (!strcmp(t, "voice-stopped") || !strcmp(t, "transcript")) {
        if (!strcmp(t, "transcript")) {
            char text[256] = "";
            wy_json_str(strchr(ev->json, '\n') ? strchr(ev->json, '\n') : ev->json, "text", text, sizeof text);
            fprintf(stderr, "transcript: %s\n", text);
            atomic_store(&streaming, 0);
        }
        if (state == LISTENING) set_state(THINKING);
    } else if (!strcmp(t, "audio-start")) {
        long rate = TTS_RATE, ch = 1, width = 2;
        wy_json_int(ev->json, "rate", &rate); wy_json_int(ev->json, "channels", &ch); wy_json_int(ev->json, "width", &width);
        if (width != 2) fprintf(stderr, "play: unsupported sample width %ld, expect noise\n", width);
        atomic_store(&streaming, 0);
        set_state(SPEAKING);
        q_push(Q_START, rate, ch, NULL, 0);
    } else if (!strcmp(t, "audio-stop")) q_push(Q_STOP, 0, 0, NULL, 0);
    else if (!strcmp(t, "error")) {
        fprintf(stderr, "server error: %s\n", ev->json);
        if (state != SPEAKING) { led("-s", "anim_start_error_short"); pipeline_finish(); }
    }
    pthread_mutex_unlock(&lock);
}

static void serve(int fd)
{
    struct wy_reader rd; static struct wy_event ev;
    if (wy_reader_init(&rd, fd) < 0) return;
    pthread_mutex_lock(&lock);
    client = fd;
    pthread_mutex_unlock(&lock);
    fprintf(stderr, "client connected\n");

    while (wy_read(&rd, &ev) > 0) handle(&ev);

    pthread_mutex_lock(&lock);
    client = -1; satellite_running = 0;
    pipeline_finish();
    pthread_mutex_unlock(&lock);
    wy_reader_free(&rd);
    fprintf(stderr, "client disconnected\n");
}

static void on_usr1(int s) { (void)s; atomic_store(&trigger_pending, 1); }

int main(int argc, char **argv)
{
    const char *manifest = DEFAULT_MANIFEST, *input = "/dev/input/event3"; int port = 16700, o;
    while ((o = getopt(argc, argv, "p:n:w:m:b:LEV")) != -1) switch (o) {
        case 'p': port = atoi(optarg); break;
        case 'n': name = optarg; break;
        case 'w': local_wake = strcmp(optarg, "remote") != 0; break;
        case 'm': manifest = optarg; break;
        case 'b': input = optarg; break;
        case 'L': use_led = 0; break;
        case 'E': use_earcon = 0; break;
        case 'V': use_volume = 0; break;
        default: fprintf(stderr, "usage: hassmic [-p port] [-n name] [-w local|remote] [-m manifest] [-b input-device] [-L] [-E] [-V]\n"); return 2;
    }
    signal(SIGPIPE, SIG_IGN); signal(SIGCHLD, SIG_IGN); signal(SIGUSR1, on_usr1);
    if (access("/system/bin/ledctrl", X_OK)) use_led = 0;

    if (cap_open() < 0) { fprintf(stderr, "cannot open capture (is PuffinApp still running?)\n"); return 1; }
    if (local_wake && wake_open(manifest, on_wake) < 0) { fprintf(stderr, "cannot load wake word model %s\n", manifest); return 1; }

    pthread_t cap_t, play_t, ear_t;
    pthread_create(&cap_t, NULL, capture_thread, NULL);
    pthread_create(&play_t, NULL, playback_thread, NULL);
    pthread_create(&ear_t, NULL, earcon_thread, NULL);
    { pthread_t vol_t; pthread_create(&vol_t, NULL, volume_led_thread, NULL); pthread_detach(vol_t); }

    static const struct button_handler buttons = { on_action, on_mute, on_volume };
    if (buttons_start(input, &buttons) < 0) fprintf(stderr, "buttons: %s not available\n", input);
    else if (buttons_muted()) on_mute(1);

    int ls = net_listen(port);
    if (ls < 0) { perror("listen"); return 1; }
    fprintf(stderr, "hassmic " VERSION " listening on %d, wake=%s\n", port, local_wake ? "local" : "remote");
    while (!atomic_load(&quit)) {
        int c = net_accept(ls);
        if (c < 0) break;
        serve(c);
        close(c);
    }
    cap_close();
    if (local_wake) wake_close();
    return 1;
}
