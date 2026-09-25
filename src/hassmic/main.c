/*
 * hassmic - Home Assistant voice satellite for the Echo Dot 3 (donut).
 *
 * Replaces PuffinApp as the client of Amazon's `mixer` daemon: reads the post-AEC/beamformer stream,
 * runs the stock "Alexa" wake word locally, and speaks the ESPHome native API or Wyoming to Home Assistant.
 *
 *   hassmic [-P esphome|wyoming] [-p port] [-n name] [-w local|remote] [-m pryon.manifest] [-b input-device] [-L] [-E] [-V] [-S]
 *     -o port  push update port (default 28929, 0 = off; see scripts/ota-push.sh)
 *     -z port  Sendspin player port (default 28928, 0 = off)
 *     -T       print the Sendspin pairing token (paste it into Music Assistant to pair) and exit
 *     -L no LED ring   -E no earcon on wake   -V leave the volume buttons alone   -S print the avahi service file and exit
 *
 * Default ports 26053 (ESPHome) and 16700 (Wyoming): the stock firewall only admits inbound TCP 16384-32767.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include "audio.h"
#include "a2dp.h"
#include "buttons.h"
#include "netio.h"
#include "wake.h"
#include "core.h"
#include "ota.h"
#include "sendspin.h"
#include "sounds.h"

#define DEFAULT_MANIFEST "/system/local/models/keyword/en-US/ALEXA/pryon.manifest"
#define PIPELINE_TIMEOUT 30         /* seconds in LISTENING or THINKING before giving up */
#define TTS_RATE         22050      /* assumed when audio-start carries no rate */

static const char *const state_names[] = { "idle", "listening", "thinking", "speaking" };
static int use_led = 1, use_earcon = 1, use_volume = 1, use_bt_announce = 1;
static atomic_int sounds_pending;
static void sound_queue(enum sound s) { atomic_fetch_or(&sounds_pending, 1 << s); }                          /* played by the earcon thread */
static void sound_request(enum sound s) { if (use_earcon) sound_queue(s); }

const char *core_name = "Echo Dot";
static int ota_port = 28929;                        /* 0 = no push updates */
int core_local_wake = 1, core_port, core_sendspin_port = 28928;       /* 0 = Sendspin off */
static const struct proto *proto = &proto_esphome;

pthread_mutex_t core_lock = PTHREAD_MUTEX_INITIALIZER;
static int connected, satellite_running;
static enum state state;
static time_t state_since;
static atomic_int streaming, trigger_pending, button_pending, stop_pending, quit;
static atomic_int flush_playback, alarm_on;   /* barge-in: drop queued TTS; UI sounds requested (bit per enum sound) */
static atomic_int tts_on, music_on;                 /* something plays: the wake word model lowers its threshold then.
                                                    * music_on: MUSIC_* bits */
static atomic_int dump_toggle;                      /* SIGTTIN: start / stop writing the processed mic stream to a file */
static int soft_mute;                               /* under lock: mute switch from Home Assistant */
static int dnd;                                     /* under lock: do not disturb */
static int barge_in;                                /* under lock: start a new pipeline once the current one has ended
                                                      * (wake word during a reply, or the server asked to continue the conversation) */

/* ---------------------------------------------------------------- LED ring */

/* In a child before exec: plain group aipc.  The real group 3990 that runas -r gives us is only for hassmic's own sockets
 * (net.c); a shell would make it the effective group, and AIPC and the mixer refuse that. */
static void child_ids(void) { gid_t e = getegid(); setregid(e, e); }

static void run_argv(char *const argv[])
{
    if (fork() == 0) {                  /* bionic API 24 has no posix_spawn; SIGCHLD is ignored: no zombie */
        child_ids();
        int nul = open("/dev/null", O_WRONLY);          /* ledctrl and audio_manager_set_prop chat on stdout: two log lines */
        if (nul >= 0) dup2(nul, 1);                     /* per LED change otherwise.  Errors (stderr) still reach the log */
        execv(argv[0], argv);
        _exit(127);
    }
}

static void run(const char *path, const char *a1, const char *a2)
{
    char *argv[] = { (char *)path, (char *)a1, (char *)a2, NULL };
    run_argv(argv);
}

/* Its stdout into buf (NUL-terminated, cut at n - 1); empty when it cannot run (PC build) */
static void run_output(char *const argv[], char *buf, size_t n)
{
    int p[2]; size_t len = 0; ssize_t r;
    buf[0] = 0;
    if (pipe(p)) return;
    if (fork() == 0) {                  /* not popen: its shell would undo child_ids() */
        child_ids();
        int nul = open("/dev/null", O_WRONLY);
        dup2(p[1], 1); if (nul >= 0) dup2(nul, 2);
        close(p[0]); close(p[1]);
        execv(argv[0], argv);
        _exit(127);
    }
    close(p[1]);
    while (len + 1 < n && ((r = read(p[0], buf + len, n - 1 - len)) > 0 || (r < 0 && errno == EINTR))) if (r > 0) len += (size_t)r;
    buf[len] = 0;
    close(p[0]);                        /* SIGCHLD is ignored: nothing to wait for */
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

enum state core_state(void) { return state; }

void core_set_state(enum state s)
{
    if (s == state) return;
    fprintf(stderr, "state: %s -> %s\n", state_names[state], state_names[s]);
    led_for(state, s);
    state = s; state_since = time(NULL);
}

static long long mono_ms(void);

/* Amazon's keyword models accept the wake word at a lower score while the device itself makes noise (kw.cfg.json:
 * "AlarmState" 1 cuts the ECHO threshold from 0.75 to 0.45, "AudioPlayerState" / "audio_playback" 1 to 0.70), because
 * that is when the user shouts over it and a false accept costs little. */
static void playback_hint(void)
{
    int alarm = atomic_load(&alarm_on), music = atomic_load(&music_on) != 0, tts = atomic_load(&tts_on);
    wake_property("AlarmState", alarm);
    wake_property("AudioPlayerState", music);
    wake_property("audio_playback", alarm || music || tts);
}

/* ---------------------------------------------------------------- wake word models
 * The firmware only has "Alexa"; other keywords are model sets fetched from Amazon once (README, "Another wake word")
 * and kept in <models>/<keyword>-<language>/pryon.manifest.  All are offered to Home Assistant, which shows them in the
 * satellite's wake word select; its pick is kept in state/wake_word and loaded live by the capture thread.  -m names
 * the model to use until Home Assistant has picked one (before 2026-09-25 it was the only way, and Home Assistant was
 * told "Alexa" whatever -m said). */
#define MAX_WAKE_WORDS 16
static struct core_wake_word wake_words[MAX_WAKE_WORDS];
static int n_wake_words, wake_active;               /* under core_lock once running */
static atomic_int wake_switch;                      /* capture thread: load wake_words[wake_active] */

static const char *models_dir(void) { const char *e = getenv("HASSMIC_MODELS"); return e ? e : "/data/local/hassmic/models"; }
static const char *wake_word_path(void)
{
    static char p[256]; const char *d = getenv("HASSMIC_STATE");
    snprintf(p, sizeof p, "%s/wake_word", d ? d : "/data/local/hassmic/state");
    return p;
}

/* id "echo-de" -> name "Echo", language "de"; "hey_disney-en-US" -> "Hey Disney", "en" */
static int wake_word_add(const char *id, const char *manifest)
{
    for (int i = 0; i < n_wake_words; i++) if (!strcmp(wake_words[i].manifest, manifest)) return i;
    if (n_wake_words == MAX_WAKE_WORDS) return -1;
    struct core_wake_word *w = &wake_words[n_wake_words];
    const char *dash = strchr(id, '-');
    int k = dash ? (int)(dash - id) : (int)strlen(id);
    snprintf(w->id, sizeof w->id, "%s", id); snprintf(w->manifest, sizeof w->manifest, "%s", manifest);
    snprintf(w->name, sizeof w->name, "%.*s", k, id);
    for (char *c = w->name; *c; c++) {
        if (*c == '_') *c = ' ';
        *c = (char)(c == w->name || c[-1] == ' ' ? toupper((unsigned char)*c) : tolower((unsigned char)*c));
    }
    snprintf(w->lang, sizeof w->lang, "%s", dash ? dash + 1 : "en");
    w->lang[strcspn(w->lang, "-_")] = 0;
    return n_wake_words++;
}

static void wake_words_scan(const char *m_arg)
{
    char path[512], saved[64] = ""; DIR *d; struct dirent *e; FILE *f; int def = 0;
    wake_word_add("alexa", DEFAULT_MANIFEST);        /* the firmware's own: always there */
    if ((d = opendir(models_dir()))) {
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            snprintf(path, sizeof path, "%s/%s/pryon.manifest", models_dir(), e->d_name);
            if (!access(path, R_OK)) wake_word_add(e->d_name, path);
        }
        closedir(d);
    }
    if (m_arg) {                                    /* named after its directory, like the ones found above */
        char dir[256], *slash; snprintf(dir, sizeof dir, "%s", m_arg);
        if ((slash = strrchr(dir, '/'))) *slash = 0;
        slash = strrchr(dir, '/');
        int i = wake_word_add(slash ? slash + 1 : dir, m_arg);
        if (i >= 0) def = i;
    }
    wake_active = def;
    if ((f = fopen(wake_word_path(), "r"))) {
        if (fscanf(f, "%63s", saved) == 1) for (int i = 0; i < n_wake_words; i++) if (!strcmp(wake_words[i].id, saved)) wake_active = i;
        fclose(f);
    }
    for (int i = 0; i < n_wake_words; i++)
        fprintf(stderr, "wake word: %s \"%s\" (%s)%s\n", wake_words[i].id, wake_words[i].name, wake_words[i].lang, i == wake_active ? ", active" : "");
}

int core_wake_words(const struct core_wake_word **list) { *list = wake_words; return n_wake_words; }

int core_wake_word(int set)
{
    if (set >= 0 && set < n_wake_words && set != wake_active) {
        FILE *f = fopen(wake_word_path(), "w");
        wake_active = set;
        if (f) { fprintf(f, "%s\n", wake_words[set].id); fclose(f); } else fprintf(stderr, "wake word: cannot write %s\n", wake_word_path());
        atomic_store(&wake_switch, 1);
    }
    return wake_active;
}

static long long wake_cut_ms;    /* when the wake word last cut a reply or an alarm: a "stop" right behind it belongs to that */
static long long last_wake_ms;   /* Amazon's models know "stop" only in the ~2 s after the wake word (op.cfg.json: awake state) */
static int quiet_abort;

static void pipeline_start(void)
{
    quiet_abort = 0;
    proto->start();
    atomic_store(&streaming, 1);
    if (core_local_wake) core_set_state(LISTENING);
}

int core_wake_sound(int set) { if (set >= 0) use_earcon = set; return use_earcon; }

int core_bt_announce(int set) { if (set >= 0) use_bt_announce = set; return use_bt_announce; }

/* Stock Alexa played its Bluetooth chime and said "Now connected to <name>".  The chime is on the image; there is no TTS
 * engine on it, so the words come from Home Assistant (the protocol asks it) and only while it is connected. */
void core_bt_device(const char *name, int on)
{
    pthread_mutex_lock(&core_lock);
    if (use_bt_announce) {
        sound_queue(on ? SND_BT_ON : SND_BT_OFF);
        if (connected && proto->bt_device) proto->bt_device(name, on);
    }
    pthread_mutex_unlock(&core_lock);
}

/* Do not disturb, like stock: announcements from Home Assistant are dropped, while the wake word, replies, timers, music
 * and Bluetooth connection messages carry on.  Switching it on shows Amazon's single purple pulse (do_not_disturb: 2 s
 * fade in and out, layer 2, nothing after its `loop` marker); switching it off shows nothing. */
static atomic_llong dnd_clear_at;
int core_dnd(int set)
{
    if (set >= 0 && set != dnd) {
        dnd = set;
        fprintf(stderr, "do not disturb: %d\n", set);
        if (set && satellite_running) { led("-s", "do_not_disturb"); atomic_store(&dnd_clear_at, mono_ms() + 2500); }   /* quiet when restored at start */
    }
    return dnd;
}

int core_muted(void) { return soft_mute || buttons_muted(); }

static void mute_update(int was, int sound)   /* lock held: ring, sound + Home Assistant follow the effective state */
{
    int now = core_muted();
    if (now == was) return;
    if (sound) sound_request(now ? SND_MICS_OFF : SND_MICS_ON);
    if (now) { if (state == LISTENING) core_pipeline_finish(); led("-s", "mics-off_on"); }
    else { led("-u", "mics-off_on"); led("-s", "mics-off_end"); }
    if (connected && proto->mute_changed) proto->mute_changed(now);
}

int core_soft_mute(int set)
{
    if (set >= 0 && set != soft_mute) {
        int was = core_muted();
        soft_mute = set;
        fprintf(stderr, "soft mute: %d%s\n", set, !set && buttons_muted() ? " (hardware latch still on: only the button releases it)" : "");
        mute_update(was, satellite_running);            /* quiet when the saved setting is restored at start */
        if (!set && buttons_muted() && connected && proto->mute_changed) proto->mute_changed(1);   /* switch bounces back */
    }
    return soft_mute;
}

void core_mic_off(void) { atomic_store(&streaming, 0); }
void core_restart_after(void) { barge_in = 1; }

void core_pipeline_finish(void)
{
    if (atomic_exchange(&streaming, 0) && connected && proto->stop) proto->stop();
    core_set_state(IDLE);
    if (mono_ms() - last_wake_ms > 3000) wake_reset();      /* a reset puts the engine back to sleep: "<wake word>, stop" would lose its "stop" */
    if (!connected) barge_in = 0;
    if ((barge_in || !core_local_wake) && satellite_running && connected) {
        if (barge_in) sound_request(SND_WAKE);
        barge_in = 0;
        pipeline_start();
    }
}

void core_link(int up, int ready)
{
    int was = satellite_running;
    connected = up; satellite_running = up && ready;
    if (!satellite_running) core_pipeline_finish();
    else if (!was && !core_local_wake && state == IDLE) pipeline_start();
}

void core_error(void)
{
    if (quiet_abort) { quiet_abort = 0; return; }      /* Home Assistant's complaint about a pipeline "stop" ended: not news */
    if (state != SPEAKING) { led("-s", "anim_start_error_short"); core_pipeline_finish(); }
}

static void trigger(int touch)          /* touch: the action button rather than the wake word */
{
    pthread_mutex_lock(&core_lock);
    if (atomic_load(&alarm_on)) {
        core_alarm(0);
        wake_cut_ms = mono_ms();
    } else if (!connected || !satellite_running || core_muted()) {
        /* nothing to talk to, or privacy latch on */
    } else if (state == IDLE) {
        sound_request(touch ? SND_TOUCH : SND_WAKE);
        pipeline_start();
    } else if (state == SPEAKING && !atomic_load(&flush_playback)) {       /* not already being cut.  barge_in alone does not
                                                                             * say that: continue-conversation sets it too */
        fprintf(stderr, "barge-in\n");
        wake_cut_ms = mono_ms();
        barge_in = 1;
        atomic_store(&flush_playback, 1);   /* playback thread drops TTS up to audio-stop, then we restart */
    }
    pthread_mutex_unlock(&core_lock);
}

/* "Stop", the second keyword of Amazon's wake word models.  The engine reports it only right behind the wake word
 * ("<wake word>, stop"; on its own it is never detected, also not with a changed op.cfg.json: tried).
 * It ends what is making noise and never starts anything:
 * a ringing alarm, a reply being spoken (also one that would listen again afterwards).  Said as "<wake word>, stop", the wake
 * word has already cut the reply and opened a new pipeline by the time "stop" is recognised: that pipeline is dropped again.
 * Only then: "<wake word>, stop the music" out of silence is a command for Home Assistant, not for us. */
static void stop_word(void)
{
    pthread_mutex_lock(&core_lock);
    if (atomic_load(&alarm_on)) {
        core_alarm(0);
    } else if (state == SPEAKING) {
        fprintf(stderr, "stop: reply cut\n");
        barge_in = 0;
        atomic_fetch_and(&sounds_pending, ~(1 << SND_WAKE | 1 << SND_TOUCH));
        atomic_store(&flush_playback, 1);
    } else if (state == LISTENING && mono_ms() - wake_cut_ms < 4000) {
        fprintf(stderr, "stop: pipeline dropped\n");
        atomic_fetch_and(&sounds_pending, ~(1 << SND_WAKE | 1 << SND_TOUCH));
        barge_in = 0;
        core_pipeline_finish();
        quiet_abort = 1;
    }
    pthread_mutex_unlock(&core_lock);
}

static void on_wake(const char *keyword)
{
    if (!core_local_wake) return;
    if (!strcasecmp(keyword, "STOP")) stop_word(); else { last_wake_ms = mono_ms(); trigger(0); }
}

/* ---------------------------------------------------------------- playback queue */

struct item { struct item *next; int kind; unsigned rate, ch; size_t len; unsigned char data[]; };
enum { Q_START, Q_DATA, Q_STOP };

static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cond = PTHREAD_COND_INITIALIZER;
static struct item *q_head, *q_tail;
static size_t q_bytes;

static void q_push(int kind, unsigned rate, unsigned ch, const void *data, size_t len)
{
    struct item *it = malloc(sizeof *it + len);
    if (!it) return;
    it->next = NULL; it->kind = kind; it->rate = rate; it->ch = ch; it->len = len;
    if (len) memcpy(it->data, data, len);
    pthread_mutex_lock(&q_lock);
    if (q_tail) q_tail->next = it; else q_head = it;
    q_tail = it; q_bytes += len;
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
        q_bytes -= it->len;
        pthread_mutex_unlock(&q_lock);

        if (atomic_load(&flush_playback) && it->kind != Q_STOP) {
            if (open) { play_close(0); open = 0; }
            free(it);
            continue;
        }
        switch (it->kind) {
        case Q_START:
            if (open) play_close(1);        /* e.g. announcement chime followed by the message */
            open = play_open(it->rate, it->ch) == 0;
            if (open) { atomic_store(&tts_on, 1); playback_hint(); }
            if (!open) fprintf(stderr, "play: open %u Hz x%u failed\n", it->rate, it->ch);
            break;
        case Q_DATA:
            if (open && play_write(it->data, it->len) < 0) { play_close(0); open = 0; }
            break;
        case Q_STOP:
            if (open) { play_close(1); open = 0; }
            atomic_store(&tts_on, 0); playback_hint();
            atomic_store(&flush_playback, 0);
            pthread_mutex_lock(&core_lock);
            if (proto->played) proto->played();      /* also without a client: modules reset their state here */
            core_pipeline_finish();
            pthread_mutex_unlock(&core_lock);
            break;
        }
        free(it);
    }
    return NULL;
}

void core_tts_begin(unsigned rate, unsigned channels)
{
    atomic_store(&streaming, 0);
    core_set_state(SPEAKING);
    q_push(Q_START, rate, channels, NULL, 0);
}

void core_tts_data(const void *pcm, size_t len) { if (!atomic_load(&flush_playback)) q_push(Q_DATA, 0, 0, pcm, len); }
void core_tts_end(void) { q_push(Q_STOP, 0, 0, NULL, 0); }
int  core_tts_flushing(void) { return atomic_load(&flush_playback); }
void core_tts_flush(void) { atomic_store(&flush_playback, 1); }

size_t core_tts_queued(void)
{
    pthread_mutex_lock(&q_lock); size_t n = q_bytes; pthread_mutex_unlock(&q_lock);
    return n;
}

/* ---------------------------------------------------------------- earcon, buttons */

static long long mono_ms(void);

void core_alarm(int on)
{
    if (atomic_exchange(&alarm_on, on) == on) return;
    fprintf(stderr, "alarm: %s\n", on ? "ringing" : "off");
    led(on ? "-s" : "-u", "active_timer");
    playback_hint();
}

/* The newest music source wins: a Bluetooth device that starts pauses the Sendspin group (the controller role; the
 * whole group, since a player cannot tell whether it has the group to itself), a Sendspin stream that starts pauses the
 * Bluetooth device (AVRCP; without it the device only goes unheard until Sendspin stops).  No automatic resume. */
void core_music(int source, int on)
{
    int was = on ? atomic_fetch_or(&music_on, source) : atomic_fetch_and(&music_on, ~source);
    if (on && !(was & source)) {
        if (source == MUSIC_BLUETOOTH && was & MUSIC_SENDSPIN && core_sendspin_port) sendspin_pause();
        if (source == MUSIC_SENDSPIN && was & MUSIC_BLUETOOTH) a2dp_pause();
    }
    if (!on && source == MUSIC_SENDSPIN && was & MUSIC_SENDSPIN) a2dp_unyield();
    if (!was != !atomic_load(&music_on)) playback_hint();
}

static void *earcon_thread(void *arg)
{
    enum { RATE = 48000, N = RATE * 12 / 100 };
    static short tone[N];
    static const char *const snd_names[SND_COUNT] = { "wake", "touch", "mics off", "mics on", "volume", "bluetooth connected",
                                                                "bluetooth disconnected" };
    (void)arg;
    for (int i = 0; i < N; i++) {           /* 120 ms rising two-tone blip with 10 ms fades */
        double f = i < N / 2 ? 880.0 : 1320.0, env = fmin(1.0, fmin(i, N - i) / (RATE * 0.01));
        tone[i] = (short)(6000 * env * sin(2 * M_PI * f * i / RATE));
    }
    for (long long alarm_end = 0;;) {
        /* Amazon's own sounds where the image has them; the generated blip stands in for the wake and touch sounds otherwise */
        for (int p = atomic_exchange(&sounds_pending, 0), s = 0; p && s < SND_COUNT; s++) {
            const short *pcm; size_t n; unsigned rate;
            if (!(p & 1 << s)) continue;
            if (sound_get((enum sound)s, &pcm, &n, &rate)) { fprintf(stderr, "sound: %s\n", snd_names[s]); play_earcon(pcm, n, rate); }
            else if (s == SND_WAKE || s == SND_TOUCH) play_earcon(tone, N, RATE);
        }
        if (atomic_load(&alarm_on)) {                   /* timer finished: triple blip every 1.2 s, at most a minute */
            if (!alarm_end) alarm_end = mono_ms() + 60000;
            if (mono_ms() > alarm_end) core_alarm(0);
            else { for (int k = 0; k < 3; k++) play_earcon(tone, N, RATE); usleep(800000); }
        } else alarm_end = 0;
        usleep(20000);
    }
    return NULL;
}

/* While music plays the action button pauses it (and resumes it again); otherwise it wakes the assistant. */
/* Action button: pause what plays (Bluetooth device first, then Sendspin), or resume what the button paused, else talk */
static void on_action(void)
{
    if (a2dp_button(0) || (core_sendspin_port && sendspin_button()) || a2dp_button(1)) return;
    atomic_store(&trigger_pending, 2);                  /* 2: touch */
}

static void on_mute(int muted)          /* hardware latch changed (button) */
{
    static int last = 0;
    fprintf(stderr, "mic mute button: %d\n", muted);
    pthread_mutex_lock(&core_lock);
    int was = last || soft_mute; last = muted;
    if (!muted && soft_mute) { soft_mute = 0; fprintf(stderr, "soft mute: 0 (released with the button)\n"); }
    mute_update(was, 1);
    pthread_mutex_unlock(&core_lock);
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

static int read_prop_volume(const char *prop, int fallback)
{
    char out[1024], *line, *save; int v = fallback, x;
    char *argv[] = { "/system/bin/audio_manager_get_prop", (char *)prop, NULL };
    run_output(argv, out, sizeof out);
    for (line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
        if (sscanf(line, "%d", &x) == 1 && x >= 0 && x <= 100) v = x;
    return v;
}

static int read_volume(void) { return read_prop_volume("MainVolume", 40); }

/* The mixer keeps one volume per stream type.  MainVolume is what the stock volume keys move and covers the Music and Earcon
 * streams; the TTS stream, which carries the assistant's replies, follows TTSVolume alone.  One knob for the user: both. */
static void set_prop_volume(const char *prop, int v)
{
    char val[8];
    snprintf(val, sizeof val, "%d", v);
    run("/system/bin/audio_manager_set_prop", prop, val);
}

int core_volume(void)
{
    if (volume < 0) { volume = read_volume(); set_prop_volume("TTSVolume", volume); }      /* whatever it was left at: in line now */
    return volume;
}

void core_set_volume(int v)
{
    char pat[24]; int step;
    volume = v < 0 ? 0 : v > 100 ? 100 : v;
    step = volume * 30 / 100 ? volume * 30 / 100 : 1;
    snprintf(pat, sizeof pat, "volume_step-%02d", step);
    set_prop_volume("MainVolume", volume);
    set_prop_volume("TTSVolume", volume);
    if (vol_pat[0] && strcmp(vol_pat, pat)) led("-u", vol_pat);
    led("-s", pat);
    strcpy(vol_pat, pat);
    atomic_store(&vol_clear_at, mono_ms() + 2500);
    fprintf(stderr, "volume: %d\n", volume);
    if (connected && proto->volume_changed) proto->volume_changed(volume);
    if (core_sendspin_port) sendspin_volume_changed(volume);
    a2dp_volume_changed(volume);
}

/* Speaker equalizer: the mixer's own user EQ (libasp "ASP/UserEq"), which stock set from the Alexa app through PuffinApp.
 * LIPC com.doppler.lasp takes the three bands as JSON, clamps each to -6..+6 (dB steps), applies them to everything the
 * mixer plays (music, replies and sounds alike, on the 3.5 mm jack too) and keeps them across reboots in
 * /data/misc/audio/audioCtrl.cfg.  So no state file of ours: read from the mixer once, then cached. */
static const char *const eq_names[3] = { "BASS", "MIDRANGE", "TREBLE" };
static int eq[3], eq_read;

int core_eq(int band)
{
    if (!eq_read) {
        char out[256], key[32], *s; int x;
        char *argv[] = { "/system/bin/lipc-get-prop", "-s", "com.doppler.lasp", "LASP_CMD_GET_USER_EQ_INFO", NULL };
        run_output(argv, out, sizeof out);          /* {"bands":[{"name":"BASS","level":0},{"name":"MIDRANGE",... */
        for (int i = 0; i < 3; i++) {
            snprintf(key, sizeof key, "\"%s\",\"level\":", eq_names[i]);
            if ((s = strstr(out, key)) && sscanf(s + strlen(key), "%d", &x) == 1) eq[i] = x < -6 ? -6 : x > 6 ? 6 : x;
        }
        eq_read = 1;
    }
    return eq[band];
}

void core_set_eq(int band, int db)
{
    char json[160];
    core_eq(band);                                  /* the other two bands as the mixer has them */
    eq[band] = db < -6 ? -6 : db > 6 ? 6 : db;
    snprintf(json, sizeof json, "{\"bands\":[{\"name\":\"%s\",\"level\":%d},{\"name\":\"%s\",\"level\":%d},{\"name\":\"%s\",\"level\":%d}]}",
             eq_names[0], eq[0], eq_names[1], eq[1], eq_names[2], eq[2]);
    char *argv[] = { "/system/bin/lipc-set-prop", "-s", "com.doppler.lasp", "LASP_CMD_SET_USER_EQ_INFO", json, NULL };
    run_argv(argv);
    fprintf(stderr, "equalizer: bass %d, mid %d, treble %d\n", eq[0], eq[1], eq[2]);
}

/* Anything may move MainVolume behind our back (audio_manager_set_prop, a stock daemon, the stock keys when -V), and
 * TTSVolume does not follow by itself: replies would then play at the old volume.  Poll both every 2 s: a changed
 * MainVolume is the user's wish and is adopted (Home Assistant and Music Assistant are told, no LED), a strayed TTSVolume
 * is pulled back in line.
 * The mixer's global Mute silences every stream whatever the volumes say, and it persists across reboots: stock Alexa
 * ("Alexa, mute") can leave it set, and then nothing plays.  Nothing of ours uses it (the mic button is a hardware latch,
 * a player mute from Music Assistant is ours in software), so a set Mute is cleared. */
static void volume_sync(void)
{
    int main_v, tts_v;
    if (read_prop_volume("Mute", 0) != 0) { fprintf(stderr, "speaker: global Mute was set, clearing it\n"); set_prop_volume("Mute", 0); }
    pthread_mutex_lock(&core_lock);
    int cur = core_volume();
    pthread_mutex_unlock(&core_lock);
    main_v = read_prop_volume("MainVolume", cur);
    tts_v = read_prop_volume("TTSVolume", cur);
    pthread_mutex_lock(&core_lock);
    if (volume == cur) {                                            /* nobody set it meanwhile */
        if (main_v != cur) {
            volume = main_v;
            fprintf(stderr, "volume: %d (changed outside)\n", volume);
            if (connected && proto->volume_changed) proto->volume_changed(volume);
            if (core_sendspin_port) sendspin_volume_changed(volume);
            a2dp_volume_changed(volume);
        }
        if (tts_v != volume) set_prop_volume("TTSVolume", volume);
    }
    pthread_mutex_unlock(&core_lock);
}

static void on_volume(int dir)
{
    if (!use_volume) return;
    pthread_mutex_lock(&core_lock);
    core_set_volume((core_volume() + 5) / 10 * 10 + dir * 10);
    sound_request(SND_VOLUME);
    pthread_mutex_unlock(&core_lock);
}

static void *volume_led_thread(void *arg)
{
    int tick = 0;
    (void)arg;
    while (!atomic_load(&quit)) {
        if (++tick % 10 == 0) volume_sync();
        long long at = atomic_load(&vol_clear_at);
        if (at && mono_ms() >= at) {
            pthread_mutex_lock(&core_lock);
            if (atomic_load(&vol_clear_at) == at) { led("-u", vol_pat); vol_pat[0] = 0; atomic_store(&vol_clear_at, 0); }
            pthread_mutex_unlock(&core_lock);
        }
        at = atomic_load(&dnd_clear_at);                /* played out; unset it like the volume steps, so the next pulse starts clean */
        if (at && mono_ms() >= at && atomic_compare_exchange_strong(&dnd_clear_at, &at, 0)) {
            pthread_mutex_lock(&core_lock); led("-u", "do_not_disturb"); pthread_mutex_unlock(&core_lock);
        }
        usleep(200000);
    }
    return NULL;
}

/* ---------------------------------------------------------------- capture */

static void *capture_thread(void *arg)
{
    FILE *dump = NULL;
    (void)arg;
    while (!atomic_load(&quit)) {
        const void *pcm; int n = cap_read(&pcm);
        if (n < 0) { fprintf(stderr, "capture: fatal\n"); atomic_store(&quit, 1); break; }
        if (atomic_exchange(&button_pending, 0)) on_action();      /* SIGUSR2: action button, for tests on the PC */
        { int t = atomic_exchange(&trigger_pending, 0); if (t) trigger(t == 2); }
        if (atomic_exchange(&stop_pending, 0)) stop_word();        /* SIGHUP: the "stop" keyword, for tests on the PC */
        if (n == 0) continue;

        /* What the wake word hears (post-AEC micAsr), for listening on the PC.  The mixer feeds the mic only to its one
         * micAsr client, so this is the only way to record it while hassmic runs: kill -TTIN <pid> starts, again stops. */
        if (atomic_exchange(&dump_toggle, 0)) {
            if (dump) { fprintf(stderr, "capture dump: off, %ld bytes\n", ftell(dump)); fclose(dump); dump = NULL; }
            else {
                char path[256]; const char *dir = getenv("HASSMIC_STATE");
                snprintf(path, sizeof path, "%s/capture.raw", dir ? dir : "/data/local/hassmic/state");
                dump = fopen(path, "wb");
                fprintf(stderr, "capture dump: %s %s (16 kHz mono s16le)\n", dump ? "on" : "cannot write", path);
            }
        }
        if (dump) fwrite(pcm, 1, n, dump);

        if (core_local_wake && atomic_exchange(&wake_switch, 0)) {        /* Home Assistant picked another wake word */
            pthread_mutex_lock(&core_lock); struct core_wake_word w = wake_words[wake_active]; pthread_mutex_unlock(&core_lock);
            wake_close();
            if (wake_open(w.manifest, on_wake) == 0) fprintf(stderr, "wake word: now \"%s\"\n", w.name);
            else { fprintf(stderr, "wake word: cannot load %s, back to Alexa\n", w.manifest); wake_open(wake_words[0].manifest, on_wake); }
        }
        if (core_local_wake) wake_feed(pcm, n / 2);
        if (atomic_load(&streaming)) {
            pthread_mutex_lock(&core_lock);
            if (atomic_load(&streaming) && connected) proto->audio(pcm, n);
            pthread_mutex_unlock(&core_lock);
        }

        pthread_mutex_lock(&core_lock);
        if (core_local_wake && (state == LISTENING || state == THINKING) && time(NULL) - state_since > PIPELINE_TIMEOUT) {
            fprintf(stderr, "pipeline timeout\n");
            core_pipeline_finish();
        }
        pthread_mutex_unlock(&core_lock);
    }
    return NULL;
}

static void *serve_thread(void *arg) { int c = (int)(long)arg; proto->serve(c); close(c); return NULL; }

static void on_usr1(int s) { (void)s; atomic_store(&trigger_pending, 1); }
static void on_usr2(int s) { (void)s; atomic_store(&button_pending, 1); }
static void on_hup(int s) { (void)s; atomic_store(&stop_pending, 1); }
static void on_ttin(int s) { (void)s; atomic_store(&dump_toggle, 1); }

int main(int argc, char **argv)
{
    const char *manifest = NULL, *input = "/dev/input/event3"; int port = 0, print_mdns = 0, o;
    while ((o = getopt(argc, argv, "P:p:n:w:m:b:z:o:LEVST")) != -1) switch (o) {
        case 'P': proto = !strcmp(optarg, "wyoming") ? &proto_wyoming : &proto_esphome; break;
        case 'p': port = atoi(optarg); break;
        case 'n': core_name = optarg; break;
        case 'w': core_local_wake = strcmp(optarg, "remote") != 0; break;
        case 'm': manifest = optarg; break;
        case 'b': input = optarg; break;
        case 'z': core_sendspin_port = atoi(optarg); break;
        case 'o': ota_port = atoi(optarg); break;
        case 'L': use_led = 0; break;
        case 'E': use_earcon = 0; break;
        case 'V': use_volume = 0; break;
        case 'S': print_mdns = 1; break;
        case 'T': { char tok[160]; sendspin_init(); sendspin_pairing_token(tok, sizeof tok); puts(tok); return 0; }
        default: fprintf(stderr, "usage: hassmic [-P esphome|wyoming] [-p port] [-n name] [-w local|remote] [-m manifest] [-b input-device] [-L] [-E] [-V] [-S]\n"); return 2;
    }
    core_port = port ? port : proto->port;
    if (print_mdns) { proto->print_mdns(); return 0; }
    signal(SIGPIPE, SIG_IGN); signal(SIGCHLD, SIG_IGN); signal(SIGUSR1, on_usr1); signal(SIGUSR2, on_usr2); signal(SIGHUP, on_hup); signal(SIGTTIN, on_ttin);
    if (access("/system/bin/ledctrl", X_OK)) use_led = 0;

    if (cap_open() < 0) { fprintf(stderr, "cannot open capture (is PuffinApp still running?)\n"); return 1; }
    if (core_local_wake) {
        wake_words_scan(manifest);
        const char *m = wake_words[wake_active].manifest;
        if (wake_open(m, on_wake) < 0 && (wake_active == 0 || (fprintf(stderr, "cannot load wake word model %s, trying Alexa\n", m), wake_active = 0,
                                                                  wake_open(wake_words[0].manifest, on_wake) < 0))) {
            fprintf(stderr, "cannot load wake word model %s\n", wake_words[wake_active].manifest); return 1;
        }
    }

    pthread_t cap_t, play_t, ear_t;
    pthread_create(&cap_t, NULL, capture_thread, NULL);
    pthread_create(&play_t, NULL, playback_thread, NULL);
    pthread_create(&ear_t, NULL, earcon_thread, NULL);
    { pthread_t vol_t; pthread_create(&vol_t, NULL, volume_led_thread, NULL); pthread_detach(vol_t); }

    static const struct button_handler buttons = { on_action, on_mute, on_volume };
    if (buttons_start(input, &buttons) < 0) fprintf(stderr, "buttons: %s not available\n", input);
    else if (buttons_muted()) on_mute(1);

    if (core_sendspin_port) sendspin_start(core_sendspin_port);
    a2dp_start(NULL);
    if (ota_port) ota_start(ota_port);

    int ls = net_listen(core_port);
    if (ls < 0) { perror("listen"); return 1; }
    fprintf(stderr, "hassmic " VERSION " (" BUILD ") %s on %d, wake=%s\n", proto->id, core_port, core_local_wake ? "local" : "remote");
    while (!atomic_load(&quit)) {
        int c = net_accept(ls);
        if (c < 0) break;
        /* A link that went away must not hold a client slot (or core_lock, in a blocked write) for ever:
         * writes give up after 5 s, keepalive notices a dead peer within ~25 s.  Both make serve() return. */
        { struct timeval tv = { 5, 0 }; int on = 1, idle = 10, intvl = 5, cnt = 3;
          setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv); setsockopt(c, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on);
          setsockopt(c, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle); setsockopt(c, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
          setsockopt(c, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt); }
        if (proto->threaded) {
            pthread_t t;
            if (pthread_create(&t, NULL, serve_thread, (void *)(long)c)) close(c); else pthread_detach(t);
        } else { proto->serve(c); close(c); }
    }
    cap_close();
    if (core_local_wake) wake_close();
    return 1;
}
