/*
 * hassmic - Home Assistant voice satellite for Amazon Echo devices (first: Echo Dot 3, donut; see devices/).
 *
 * Replaces PuffinApp as the client of Amazon's `mixer` daemon: reads the post-AEC/beamformer stream,
 * runs the stock "Alexa" wake word locally, and speaks the ESPHome native API or Wyoming to Home Assistant.
 *
 *   hassmic [-P esphome|wyoming] [-p port] [-n name] [-w local|remote] [-m pryon.manifest] [-b input-device] [-L] [-E] [-V] [-S]
 *   hassmic -G dir     the light ring's own animations into dir (ring.h), for root to install
 *     -o port  push update port (default 28929, 0 = off; see scripts/ota-push.sh)
 *     -a port  wake word arbitration between Echos, UDP (default 28930, 0 = off; see arb.c)
 *     -i port  Drop In audio between Echos, UDP (default 28932, 0 = off; needs -a; see dropin.c)
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
#include <limits.h>
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
#include <sys/stat.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
#include "audio.h"
#include "a2dp.h"
#include "arb.h"
#include "ble.h"
#include "improv.h"
#include "buttons.h"
#include "ring.h"
#include "ringdesign.h"
#include "ledq.h"
#include "davs.h"
#include "dropin.h"
#include "micdenoise.h"
#include "earsup.h"
#include "afemeta.h"
#include "micend.h"
#include "micgain.h"
#include "wordtail.h"
#include "wakealign.h"
#include "netaddr.h"
#include "netio.h"
#include "lux.h"
#include "wake.h"
#include "mww.h"
#include "core.h"
#include "ota.h"
#include "sendspin.h"
#include "sounds.h"
#include "sound.h"
#include "whisper.h"
#include "settings.h"
#include "web.h"
#include "board.h"
#include "clock.h"
#include "statefile.h"

#define PIPELINE_TIMEOUT 30         /* seconds in LISTENING or THINKING before giving up */
#define TTS_RATE         22050      /* assumed when audio-start carries no rate */

static const char *const state_names[] = { "idle", "listening", "thinking", "speaking" };
static int use_led = 1, use_earcon = 1, use_volume = 1, use_bt_announce = 1, use_bt = 1;
static atomic_int sounds_pending;
static void sound_queue(enum sound s) { atomic_fetch_or(&sounds_pending, 1 << s); }                          /* played by the earcon thread */
static void sound_request(enum sound s) { if (use_earcon) sound_queue(s); }

const char *core_name;                  /* name_make(), or -n (the PC) */

/* The name is UTF-8.  Latin-1 letters (U+00C0..U+00FF, lead byte 0xC3) are spelled out, the German way for the umlauts
 * ("Küchen Echo" -> "kuechen-echo"); anything else that is not a letter or digit separates words.  Each byte of "ü"
 * used to become a dash ("k--chen-echo"), which Home Assistant showed as the host name next to the friendly name. */
static const char *latin1_ascii(unsigned char c)        /* second byte after 0xC3, upper and lower case alike */
{
    static const char *const t[32] = {
        "a", "a", "a", "a", "ae", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",        /* À..Ï */
        "d", "n", "o", "o", "o", "o", "oe", NULL, "o", "u", "u", "u", "ue", "y", "th", "ss" };    /* Ð..ß (× is no letter) */
    if (c == 0xBF) return "y";                                                                  /* ÿ */
    if (c == 0xB7) return NULL;                                                                 /* ÷ */
    return c >= 0x80 && c <= 0xBF ? t[(c - 0x80) & 0x1F] : NULL;
}

static void node_of(const char *name, char n[64])
{
    size_t j = 0; int dash = 0;
    for (const unsigned char *s = (const unsigned char *)name; *s; s++) {
        const char *add = NULL; char one[2] = { 0, 0 };
        if (isalnum(*s)) { one[0] = (char)tolower(*s); add = one; }
        else if (*s == 0xC3 && s[1]) add = latin1_ascii(*++s);
        else while ((s[1] & 0xC0) == 0x80) s++;          /* other UTF-8 characters: skip their continuation bytes */
        if (!add) { dash = j > 0; continue; }            /* separators collapse to one dash, none at the start */
        if (dash && j < 63) n[j++] = '-';
        dash = 0;
        for (; *add && j < 63; add++) n[j++] = *add;
    }
    n[j] = 0;
    if (!j) snprintf(n, 64, "echo");                    /* a name with no Latin letter at all ("日本") */
}

void core_node_of(const char *name, char n[64]) { node_of(name, n); }

const char *core_node_name(void)
{
    static char n[64];
    if (!n[0]) node_of(core_name, n);
    return n;
}

/* ---------------------------------------------------------------- the name */

/* One name, made at start and kept nowhere: the model's (board.default_name: "Echo Dot 3") and the last three bytes of
 * the Wi-Fi MAC address, as ESPHome's name_add_mac_suffix and the Voice PE do ("Echo Dot 2 5695c4", node
 * "echo-dot-2-5695c4"): two Echos of a model never share it, and a reset never changes it.  What people call it is
 * Home Assistant's business (the name given when adding the device), not the Echo's.  -n sets another: for the PC,
 * which has no Echo's MAC address.  radar brings wlan0 up late in the boot: the address is waited for. */
static void name_make(void)
{
    static char name[64];
    char mac[24] = "";
    for (int i = 0; i < 120; i++) {
        FILE *f = fopen("/sys/class/net/wlan0/address", "r");
        if (f) { if (fscanf(f, "%23s", mac) != 1) mac[0] = 0; fclose(f); }
        if (strlen(mac) == 17 && strcmp(mac, "00:00:00:00:00:00")) break;
        mac[0] = 0;
#ifndef __ANDROID__
        break;                                          /* the PC: no Echo's address to wait for */
#endif
        if (!i) fprintf(stderr, "name: waiting for the Wi-Fi MAC address\n");
        sleep(1);
    }
    if (mac[0]) snprintf(name, sizeof name, "%s %.2s%.2s%.2s", board.default_name, mac + 9, mac + 12, mac + 15);
    else snprintf(name, sizeof name, "%s", board.default_name);
    core_name = name;
}

static void state_file(char *out, size_t cap, const char *file)
{
    const char *d = getenv("HASSMIC_STATE");
    snprintf(out, cap, "%s/%s", d ? d : "/data/local/hassmic/state", file);
}

static int write_state(const char *file, const char *text)
{
    char p[300];
    state_file(p, sizeof p, file);
    return statefile_write(p, 0666, text, strlen(text));
}

/* ---------------------------------------------------------------- setup (OOBE) and reset */

/* From an install or a reset until Home Assistant took the Echo on: stock's setup spinner (setup-mode: orange, what
 * uxconfig.json shows for oobe-setup-mode-on and state-boot-up-oobe, on every model), and Improv advertises while there is no
 * network (improv.c).  "Took it on" = its voice assistant subscribed (core_link ready): Home Assistant does that once the
 * device is added, not while its discovery merely looks.  An Echo that has Home Assistant's encryption key was added
 * before this existed. */
static void led(const char *op, const char *pattern);
static atomic_int oobe;
int core_oobe(void) { return atomic_load(&oobe); }

static void oobe_load(void)
{
    char p[300];
    state_file(p, sizeof p, "adopted");
    if (!access(p, F_OK)) return;
    state_file(p, sizeof p, "api_key");
    if (!access(p, F_OK)) { write_state("adopted", "key\n"); return; }
    atomic_store(&oobe, 1);
}

static void adopted(void)
{
    if (!atomic_load(&oobe)) return;
    if (write_state("adopted", "ha\n")) { fprintf(stderr, "setup: cannot write state/adopted\n"); return; }
    atomic_store(&oobe, 0);
    led("-u", "setup-mode");
    fprintf(stderr, "setup: Home Assistant took this Echo on, setup done\n");
}

void core_reset(const char *why)
{
    char line[48];
    snprintf(line, sizeof line, "%s\n", why);
    if (write_state("reset", line)) { fprintf(stderr, "reset: cannot ask root for it\n"); return; }
    led("-s", "factory-reset");
    fprintf(stderr, "reset (%s): root forgets the settings and Wi-Fi networks, then the setup starts again\n", why);
}

/* What a power cycle does, for an Echo that is stuck where only that helps (issue #17: Home Assistant lost it after
 * the router handed out new addresses, and the settings page was all that still reached it).  Root reboots on
 * state/reboot (main.sh ota_watch); hassmic itself may not. */
void core_reboot(const char *why)
{
    char line[48];
    snprintf(line, sizeof line, "%s\n", why);
    if (write_state("reboot", line)) { fprintf(stderr, "reboot: cannot ask root for it\n"); return; }
    fprintf(stderr, "reboot (%s): root restarts the Echo\n", why);
}
static int ota_port = 28929;                        /* 0 = no push updates */
static int arb_port = 28930;                        /* 0 = no wake word arbitration */
static int dropin_port = 28932;                     /* 0 = no Drop In (needs the arbitration network) */
int core_web_port = 28931;                           /* 0 = no settings page */
int core_local_wake = 1, core_port, core_sendspin_port = 28928;       /* 0 = Sendspin off */
static const struct proto *proto = &proto_esphome;

pthread_mutex_t core_lock = PTHREAD_MUTEX_INITIALIZER;
static int connected, satellite_running;
static enum state state;
static time_t state_since;
static atomic_int streaming, trigger_pending, button_pending, pair_pending, stop_pending, quit;
static atomic_int flush_playback, alarm_on;   /* barge-in: drop queued TTS; a finished timer rings */
static atomic_int busy;                             /* state != IDLE, for threads without the lock (the ring pauses) */
static atomic_int tts_on, music_on;                 /* something plays: the wake word model lowers its threshold then.
                                                    * music_on: MUSIC_* bits */
/* 64 bits: a long is 32 on the Echo, and 32 000 bytes/s pass 2^31 after 18.6 h.  It went negative, cap_clock() made
 * ~9.2e18 of it, and the first wake word after that streamed the ring for ever under core_lock (issue #31) */
static atomic_llong cap_bytes;                      /* capture thread adds: micAsr bytes read since the start */
static uint64_t cap_clock(void) { return (uint64_t)atomic_load(&cap_bytes) / 2; }   /* the ring's sample index, -w remote too */
static atomic_int dump_toggle;                      /* SIGTTIN: start / stop writing the processed mic stream to a file */
static int soft_mute;                               /* under lock: mute switch from Home Assistant */
static int dnd;                                     /* under lock: do not disturb */
static int barge_in;                                /* under lock: start a new pipeline once the current one has ended
                                                      * (wake word during a reply, or the server asked to continue the conversation) */
static uint64_t barge_from;                         /* under lock: where that wake word ended (ring samples), 0: no wake word */
static uint64_t replay_from;                        /* under lock: the pipeline just started from a wake word that ended
                                                      * here; the capture thread sends the ring from the end of the word's
                                                      * tail (word_tail_end) ahead of the live stream, which waits until
                                                      * that is known (stream_ring).  0: nothing to send */
static struct micgain mic_gain;                     /* under lock: gain of what the pipeline hears (micgain.h) */
static int mic_fresh;                               /* under lock: a pipeline started, the gain has not seen it yet */
static float keyword_db = 1;                        /* under lock: rms of the last wake word, dBFS */
/* Setting wake_word_in_request: the request's audio starts at the wake word, not after it.  Off by default: Home
 * Assistant's own sentences ("turn on the light") do not take "Alexa, turn on the light"; an LLM agent does, and
 * speech-to-text gets the whole utterance (nothing of the command cut, no stray end of the word: issue #72) */
static int wake_in_request;                         /* under lock */
static uint64_t kw_begin, kw_end;                   /* under lock: the last wake word heard, ring samples (wake engine's) */
static int kw_slot;                                 /* under lock: its slot (wake_align) */
static int replay_whole;                            /* under lock: this pipeline's replay starts at kw_begin */
static uint64_t talker_from;                        /* under lock: where the command starts (the word's tail end), micend */
static struct micend mic_end;                       /* under lock: the end of the request heard here (micend.h) */
static int end_mode;                                /* under lock: setting end_of_request, MICEND_* */
static int mic_denoise;                             /* under lock: noise reduction ahead of the gain (micdenoise.h):
                                                      * 0 off, 1 low, 2 medium, 3 high */
static long long denoise_ns; static unsigned denoise_frames;   /* under lock: its cost in the running pipeline */
static atomic_int earcon_sounding;                  /* one of our sounds plays ... */
static atomic_llong earcon_heard_until;             /* ... and is still in the mic stream until then (mono_ms) */
/* the last ones as spans of the capture clock in the mic stream (to: LLONG_MAX while it plays), one sound starting
 * inside the last one's tail going on in its span: a held volume key takes one */
static struct { atomic_llong from, to; } sound_spans[8];
static int sound_span, sound_taken;                 /* earcon thread: the newest, the mixer took it */
static atomic_int sounds_q16 = 65536, timers_q16 = 65536;  /* our sounds' and timers' gain, 1 = 65536 (separate volumes) */
/* the volume keys' tone: at the volume they just set, as stock's (on_volume).  At the sounds' own it said nothing of the
 * step: with nothing playing the keys seemed to do nothing */
static atomic_int click_q16 = 65536;

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

/* Its stdout into buf (NUL-terminated, cut at n - 1); empty when it cannot run (PC build) */
static void run_output(char *const argv[], char *buf, size_t n);
void core_run(char *const argv[], char *out, size_t n) { run_output(argv, out, n); }
void core_spawn(char *const argv[]) { run_argv(argv); }
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

/* What ring.c plays: through ledctrl one at a time, in order (ledq.c says why), or into HASSMIC_FAKE_LED's file for
 * the tests on the PC, through the same queue (ledq_start in main) */
static void led_run(const char *op, const char *pattern) { ledq_run(op, pattern); }

/* The Echo's own patterns go through ring.c: Home Assistant's light steps aside for them, and the listening ring is
 * turned towards the talker there */
static void led(const char *op, const char *pattern)
{
    if (!pattern) return;
    if (!strcmp(op, "-s")) ring_set(pattern); else ring_unset(pattern);
}

/* "active-start"/"active-end", not the "ca-" ones of uxconfig.json (same look): ledcontroller turns only these two
 * towards the talker (issue #22).  The mixer's front end sends its beam on LIPC (com.doppler.lasp beam_index);
 * ledcontroller's EchoAnimationStore::getBeamIndexTask (libled_resources.so) stores it in every frame of exactly
 * "active-start" and "active-end", and LedDevice rotates a frame by it when writing the ring, but only a frame of a
 * single active animation: with two or more (mics-off_on, a ringing timer) it composes a fresh, unturned one.
 * Checked on a Dot 2 with lipc-set-prop -i com.amazon.ledd BeamDirection N (the same path): the cyan pair moves by N
 * LEDs, the "ca-" patterns never move. */
static void led_for(enum state from, enum state to)
{
    static const char *const pattern[] = { NULL, "active-start", "active-thinking", "active-talking" };
    if (pattern[from]) led("-u", pattern[from]);
    if (pattern[to]) led("-s", pattern[to]);
    else if (from != IDLE) led("-s", "active-end");
}

/* ---------------------------------------------------------------- state (call with lock held) */

enum state core_state(void) { return state; }

void core_set_state(enum state s)
{
    if (s == state) return;
    fprintf(stderr, "state: %s -> %s\n", state_names[state], state_names[s]);
    /* a ringing timer goes to the background (an announcement, a conversation Home Assistant starts): stock flashes
     * "ready-timer-short" once on FOCUS_ENTERED_BACKGROUND, then the pipeline's own patterns cover "ready-timer" */
    if (state == IDLE && atomic_load(&alarm_on)) led("-s", "ready-timer-short");
    led_for(state, s);
    state = s; state_since = time(NULL);
    atomic_store(&busy, s != IDLE);
}

static long long mono_ms(void);

/* Amazon's keyword models accept the wake word at a lower score while the device itself makes noise (kw.cfg.json:
 * "AlarmState" 1 cuts the ECHO threshold from 0.75 to 0.45, "AudioPlayerState" / "audio_playback" 1 to 0.70), because
 * that is when the user shouts over it and a false accept costs little. */
static atomic_llong own_sound_ms;                   /* last time something of ours started or stopped playing */

static void playback_hint(void)
{
    int alarm = atomic_load(&alarm_on), music = atomic_load(&music_on) != 0, tts = atomic_load(&tts_on);
    atomic_store(&own_sound_ms, mono_ms());
    wake_property("AlarmState", alarm);
    wake_property("AudioPlayerState", music);
    wake_property("audio_playback", alarm || music || tts);
}

/* ---------------------------------------------------------------- wake word models
 * Amazon's engine: the firmware only has "Alexa"; other keywords are model sets fetched from Amazon once (README,
 * "Another wake word") and kept in <models>/<keyword>-<language>/pryon.manifest.  microWakeWord (the settings page's
 * wake word engine, off by default): the models in state/mww (mww.h).  The engine's are offered to Home Assistant, which
 * shows them in the satellite's wake word selects; its pick is kept in state/wake_word (state/mww_word for microWakeWord)
 * and loaded live by the capture thread.  -m names the Amazon model to use until Home Assistant has picked one (before
 * 2026-09-25 it was the only way, and Home Assistant was told "Alexa" whatever -m said).
 * Home Assistant has a second select, "Wake word 2" (as for Voice PE, GitHub issue 18): a second wake word listens
 * beside the first, in a decoder (or microWakeWord model) of its own, and the run it starts names it, so Home
 * Assistant takes "Assistant 2" for it.  The file then has its id on a second line (older builds read the first). */
#define MAX_WAKE_WORDS 16
_Static_assert(CORE_WAKE_ACTIVE_MAX == WAKE_SLOTS, "one slot per active wake word");
static struct core_wake_word wake_words[MAX_WAKE_WORDS];
static size_t wake_bare_len[MAX_WAKE_WORDS];        /* each name's length before " (de-DE)", if wake_words_scan added one */
static int n_wake_words;                            /* under core_lock once running */
static int wake_act[WAKE_SLOTS], n_act = 1;         /* the active ones (indexes into wake_words), in their slots; same lock */
static int wake_engine;                             /* WAKE_AMAZON / WAKE_MWW, under core_lock once running */
static const char *wake_m_arg;                      /* -m */
static int wake_cmdline_remote;                     /* -w remote: hassmic.conf decides, not the settings page */
static int wake_next = -1;                          /* asked for, takes effect with the restart */
static const char *wake_refused = "";
static atomic_int wake_switch;                      /* capture thread: load the active ones */
static char wake_loaded[WAKE_SLOTS][64];            /* under core_lock: the names of the ones loaded, by slot ("": none) */
static char wake_phrase[64];                        /* under core_lock: the one that started the run ("": the first) */

static const char *models_dir(void) { const char *e = getenv("HASSMIC_MODELS"); return e ? e : "/data/local/hassmic/models"; }
static const char *wake_word_path(void)
{
    static char p[256]; const char *d = getenv("HASSMIC_STATE");
    snprintf(p, sizeof p, "%s/%s", d ? d : "/data/local/hassmic/state", wake_engine == WAKE_MWW ? "mww_word" : "wake_word");
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

/* An id as -m or state/wake_word name it: itself, else the one set it was renamed to.  artifact-install.sh migrate gives
 * sets installed by hand under the short name the region (models/echo-de -> echo-de-DE) and moves state/wake_word
 * along, but hassmic.conf's -m kept the old path (2026-10-06, donut: "-m .../models/echo-de/pryon.manifest" became a
 * second "Echo" in HA's select that loads nothing; picked there, the Echo fell back to Alexa at every start).  -1: none */
static int wake_word_find(const char *id)
{
    int hit = -1; size_t n = strlen(id);
    for (int i = 0; i < n_wake_words; i++) if (!strcmp(wake_words[i].id, id)) return i;
    for (int i = 0; i < n_wake_words; i++)
        if (!strncmp(wake_words[i].id, id, n) && wake_words[i].id[n] == '-') { if (hit >= 0) return -1; hit = i; }   /* only if unique */
    return hit;
}

/* microWakeWord's models: named by their manifests */
static void mww_words_scan(void)
{
    static struct mww_info l[MWW_MODELS_MAX]; char path[512];
    for (int i = 0, k = mww_list(l, MWW_MODELS_MAX); i < k && n_wake_words < MAX_WAKE_WORDS; i++) {
        struct core_wake_word *w = &wake_words[n_wake_words++];
        mww_paths(l[i].id, path, NULL, sizeof path);
        snprintf(w->id, sizeof w->id, "%.47s", l[i].id); snprintf(w->name, sizeof w->name, "%.63s", l[i].name);
        snprintf(w->manifest, sizeof w->manifest, "%.255s", path);
        const char *lang = l[i].langs[0] ? l[i].langs : "en";               /* Home Assistant's select wants one */
        snprintf(w->lang, sizeof w->lang, "%.*s", (int)strcspn(lang, ","), lang);
    }
}

/* lock held once running: the engine's wake words, the saved pick active (else -m's, else the first) */
static void wake_words_scan(void)
{
    char path[512], saved[64] = ""; DIR *d; struct dirent *e; FILE *f; int def = 0; const char *m_arg = wake_m_arg;
    n_wake_words = 0;
    if (wake_engine == WAKE_MWW) {
        mww_words_scan();
        if (!n_wake_words) { fprintf(stderr, "wake word: no microWakeWord model, back to Amazon's engine\n"); wake_engine = WAKE_AMAZON; }
        else m_arg = NULL;
    }
    if (wake_engine == WAKE_AMAZON) wake_word_add(board.wake_id, board.wake_manifest);      /* the firmware's own: always there */
    if (wake_engine == WAKE_AMAZON && (d = opendir(models_dir()))) {
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
        const char *id = slash ? slash + 1 : dir;
        int i = !access(m_arg, R_OK) ? wake_word_add(id, m_arg) : wake_word_find(id);
        if (i >= 0) def = i;
        else fprintf(stderr, "wake word: -m %s: no such model, Alexa until Home Assistant picks one\n", m_arg);
    }
    /* Home Assistant's wake word select keys its options by name: of two "Computer" (de-DE and en-US) it showed one, and
     * picking it got the one listed last.  A name two sets share gets the set's language and region ("Computer
     * (en-US)"); the firmware's "Alexa" keeps its plain name */
    char shared[MAX_WAKE_WORDS] = { 0 };
    for (int i = 0; wake_engine == WAKE_AMAZON && i < n_wake_words; i++)
        for (int j = 0; j < n_wake_words; j++) if (j != i && !strcmp(wake_words[i].name, wake_words[j].name)) shared[i] = 1;
    for (int i = 0; i < n_wake_words; i++) {
        const char *dash = strchr(wake_words[i].id, '-');
        size_t k = wake_bare_len[i] = strlen(wake_words[i].name);
        if (shared[i] && dash) snprintf(wake_words[i].name + k, sizeof wake_words[i].name - k, " (%.20s)", dash + 1);
    }
    wake_act[0] = def; n_act = 1;
    if ((f = fopen(wake_word_path(), "r"))) {       /* the first, then the second if there is one */
        for (int k = 0; k < WAKE_SLOTS && fscanf(f, "%63s", saved) == 1; k++) {
            int i = wake_word_find(saved);
            if (i < 0) continue;
            if (!k) wake_act[0] = i; else if (i != wake_act[0]) wake_act[n_act++] = i;
        }
        fclose(f);
    }
    for (int i = 0; i < n_wake_words; i++)
        fprintf(stderr, "wake word: %s \"%s\" (%s%s)%s\n", wake_words[i].id, wake_words[i].name, wake_words[i].lang,
                wake_engine == WAKE_MWW ? ", microWakeWord" : "", i == wake_act[0] ? ", active" : n_act > 1 && i == wake_act[1] ? ", active (second)" : "");
}

int core_wake_words(const struct core_wake_word **list) { *list = wake_words; return n_wake_words; }
const char *core_wake_refused(void) { return wake_refused; }

/* lock held.  The list is new: Home Assistant reads it on its next connection, so the link is closed (as when a
 * feature's entities change) */
static void wake_list_changed(void)
{
    atomic_store(&wake_switch, 1);
    core_entities_changed();
}

/* The wake word in Home Assistant (WAKE_HA) or here is decided at start (arbitration, the protocol's flags, what HA
 * lists all follow it): switching between the two writes the setting and has root restart the satellite, as a rename
 * does (state/restart, main.sh); until then the setting reads what was asked for. */
int core_wake_engine(int set)
{
    static struct mww_info one;
    int cur = wake_next >= 0 ? wake_next : core_local_wake ? wake_engine : WAKE_HA;
    if (set < 0 || set == cur) return cur;
    wake_refused = "";
    if (wake_cmdline_remote) {
        wake_refused = "hassmic.conf starts the satellite with -w remote: take that out of ARGS first";
        fprintf(stderr, "wake word: %s\n", wake_refused); return cur;
    }
    if (set == WAKE_MWW && !mww_list(&one, 1)) {
        wake_refused = "no microWakeWord model on this Echo yet: add one first";
        fprintf(stderr, "wake word: no microWakeWord model yet, the engine stays\n"); return cur;
    }
    if (set == WAKE_HA || !core_local_wake || wake_next >= 0) {     /* (a restart already asked for: another one) */
        wake_next = set;
        settings_save();                                    /* before root can act on the restart */
        if (write_state("restart", "wake word engine\n")) { wake_next = -1; settings_save(); wake_refused = "cannot ask for the restart"; return cur; }
        fprintf(stderr, "wake word: engine %s from the next start, restart asked\n", set == WAKE_HA ? "Home Assistant's" : set == WAKE_MWW ? "microWakeWord" : "Amazon's");
        return set;
    }
    wake_engine = set == WAKE_MWW ? WAKE_MWW : WAKE_AMAZON;
    fprintf(stderr, "wake word: engine now %s\n", wake_engine == WAKE_MWW ? "microWakeWord" : "Amazon's");
    wake_words_scan();
    wake_list_changed();
    return wake_engine;
}

void core_wake_models_changed(void)
{
    pthread_mutex_lock(&core_lock);
    if (wake_engine == WAKE_MWW) {
        char active[64]; snprintf(active, sizeof active, "%s", n_wake_words ? wake_words[wake_act[0]].id : "");
        wake_words_scan();                                   /* (a second one that went is no longer read back) */
        if (wake_engine != WAKE_MWW) settings_save();       /* the last one went: Amazon's again, also after a restart */
        else if (strcmp(active, wake_words[wake_act[0]].id)) fprintf(stderr, "wake word: %s gone, now %s\n", active, wake_words[wake_act[0]].id);
        wake_list_changed();                                 /* also when only the active one's cutoff changed: reloaded */
        core_settings_changed();
    }
    pthread_mutex_unlock(&core_lock);
}

static void wake_save(void)
{
    FILE *f = fopen(wake_word_path(), "w");
    if (!f) { fprintf(stderr, "wake word: cannot write %s\n", wake_word_path()); return; }
    for (int i = 0; i < n_act; i++) fprintf(f, "%s\n", wake_words[wake_act[i]].id);
    fclose(f);
}

int core_wake_set(const int *list, int n)
{
    int a[WAKE_SLOTS], k = 0;
    for (int i = 0; i < n && k < WAKE_SLOTS; i++) {
        int dup = list[i] < 0 || list[i] >= n_wake_words;
        for (int j = 0; j < k; j++) dup |= a[j] == list[i];
        if (!dup) a[k++] = list[i];
    }
    if (!k) return -1;
    /* Home Assistant sends the ids in the order its selects were last set, not by select: the first stays first */
    for (int i = 1; i < k; i++) if (a[i] == wake_act[0]) { a[i] = a[0]; a[0] = wake_act[0]; }
    if (k == n_act && !memcmp(a, wake_act, sizeof a[0] * (size_t)k)) return 0;
    memcpy(wake_act, a, sizeof a[0] * (size_t)k); n_act = k;
    wake_phrase[0] = 0;
    wake_save();
    fprintf(stderr, "wake word: \"%s\"%s%s%s\n", wake_words[a[0]].name, k > 1 ? " and \"" : "", k > 1 ? wake_words[a[1]].name : "", k > 1 ? "\"" : "");
    atomic_store(&wake_switch, 1);
    return 0;
}

int core_wake_word(int set)
{
    if (set >= 0 && set < n_wake_words && (set != wake_act[0] || n_act > 1)) core_wake_set(&set, 1);
    return wake_act[0];
}

int core_wake_active(int *list)
{
    for (int i = 0; i < n_act; i++) list[i] = wake_act[i];
    return n_act;
}

int core_wake_max(void) { return n_wake_words < WAKE_SLOTS ? n_wake_words : WAKE_SLOTS; }

const char *core_wake_phrase(void)
{
    return wake_phrase[0] ? wake_phrase : n_wake_words ? wake_words[wake_act[0]].name : "";
}

/* Lock held: the run's wake word as said, for arbitration: its name without the language and region it gets only when
 * this Echo has another set of that name.  With the name, Echos with different sets installed never competed:
 * 2026-10-12, the Echo 2 (only echo-de-DE) claimed "Echo", the Dot 2 (echo-de-DE and echo-en-US) "Echo (de-DE)", each
 * passed over the other's claim and both answered the same "Echo" */
static void wake_phrase_bare(char *out, size_t cap)
{
    const char *p = core_wake_phrase();
    for (int i = 0; i < n_wake_words; i++)
        if (!strcmp(wake_words[i].name, p)) { snprintf(out, cap, "%.*s", (int)wake_bare_len[i], p); return; }
    snprintf(out, cap, "%s", p);
}

static long long wake_cut_ms;    /* when the wake word last cut a reply or an alarm: a "stop" right behind it belongs to that */
static long long last_wake_ms;   /* Amazon's models know "stop" only in the ~2 s after the wake word (op.cfg.json: awake state) */
static int quiet_abort;
static int run_wake;             /* under lock: the wake word started this run (ours or Home Assistant's), not the button or a follow-up */
/* Lock held.  heard_ms: when the engine last reported the wake word (on_wake; SIGUSR1 on the PC), stop_ms its last
 * "stop": a "stop" after heard_ms is that wake word's.  stop_cut: that "stop" ended what its wake word had cut (a
 * ring, a call, a reply, a THINKING run).  stop_ends: setting stop_ends_listening (stop_word) */
static long long heard_ms, stop_ms;
static int stop_cut, stop_ends;

static atomic_int afe_asked;                        /* the front end was asked for the wake word's energies (afe_score) */

static void afe_done(void)                          /* any thread: no command follows, or it is over */
{
    if (!atomic_exchange(&afe_asked, 0)) return;
    afe_stream_stopped();
}

/* lock held.  Tells Amazon's front end that a command is being spoken, as stock does after the wake word.  While its
 * "utterance" flag is set (libasp.so, FINDINGS.md "Listening mode") the echo canceller (AEC_V2) and the interference
 * canceller (ARA_V2) stop adapting and the beam merger keeps its beam group; without it they adapt to the talker and take
 * the voice for interference after ~1.5 s: in micAsr a quiet sentence then sinks to 0-3 dB over the floor while micRaw
 * still has it at 8-10 dB (6 captures, 2026-09-30); with it micAsr stays within 1 dB of micRaw for a 4 s sentence.
 * Stock PuffinApp sets the flag by reading LASP_CMD_REQUEST_ARBITRATION_JSON and clears it with
 * LASP_CMD_NOTIFY_ASR_STREAM_STOPPED; those also start and stop the front end's diagnostics with their metrics, so this
 * uses the plain switch.  No timeout in the front end, and the mixer keeps the state: cleared at start in case hassmic
 * died while listening.  With the wake word at the server (-w remote) the mic streams all the time, and the cancellers
 * must keep adapting while nobody talks: set only from Home Assistant's "wake word heard" (core_remote_wake) to the end
 * of the command, as stock does around its own wake word. */
static int listen_pipe, listen_call;                /* lock held: a command is spoken; a Drop In runs (core_dropin) */
static void listening_apply(void)
{
    static int is = -1;
    int on = listen_pipe || listen_call;
    if (on == is) return;
    is = on;
    afe_listening(on);
}
static void listening(int on) { listen_pipe = on; listening_apply(); }

/* Lock held.  Silence ahead of every request Home Assistant starts at speech to text (issue #80; PR #71 had it as a
 * setting).  Its VAD is a new one per run (assist_pipeline's MicroVadSpeexEnhancer; pymicro-vad 1.0.1 in HA 2026.10.1):
 * -1 for the first 76 chunks of 10 ms, which the segmenter counts as silence, and speech starts it after 0.3 s over 0.2.
 * Our stream opens on the command (the ring's replay from the word's tail), so a short one fell into that blindness:
 * espeak's "on" (0.2 s, -26 dBFS in a -55 dBFS room) started it only with 160 ms or more in front, and a Dot 2 user's
 * "Alexa lights off" waited for the 15 s timeout (home-assistant/core#175735 has the same with ESPHome satellites:
 * 800 ms of silence cured it).  Some speech to text wants a lead-in too: Parakeet TDT v2 gave "" for two of that user's
 * requests, "The lights on." for the same audio with 50-300 ms of silence in front.  The button and follow-ups start
 * runs as cold, so all of them; not the wake word at the server, whose run streams all the time.  800 ms (the VAD's
 * 760 and a little), sent at once: nothing waits for it, and STT gets 0.8 s more to listen to */
#define LEAD_IN_MS 800
static void lead_in(void)
{
    static const int16_t zero[1024];
    for (int left = LEAD_IN_MS * CAP_RATE / 1000; left > 0; left -= 1024)
        proto->audio(zero, (size_t)(left < 1024 ? left : 1024) * sizeof zero[0]);
}

static void pipeline_start(uint64_t from)    /* from: where the wake word that starts it ended, 0: not a wake word */
{
    quiet_abort = 0;
    mic_fresh = 1;
    replay_from = from;
    replay_whole = wake_in_request && from && from == kw_end && kw_begin < kw_end;
    talker_from = 0;
    /* until the first audio (send_mic: up to 0.5 s while the word's tail is looked for), not the last request's: its
     * end ended this one before any audio went out (heard_end in the capture thread) */
    micend_start(&mic_end, MICEND_OFF, 0, 0, NULL);
    run_wake = from != 0;
    proto->start();
    atomic_store(&streaming, 1);
    if (core_local_wake) lead_in();                 /* ahead of the capture thread's first block: it needs the lock */
    listening(core_local_wake);
    if (core_local_wake) core_set_state(LISTENING);
}

/* Home Assistant's "Wake word" event entity (issue #48), listed while the feature is on (proto_esphome.c): every wake
 * word this Echo took, its near misses, the ones another device answered and the ones nothing was said after, so an
 * automation can count wakes per room and spot false ones (a "detected" followed by "no_speech").  Types stay as they
 * are: automations use them, a new kind is a new type.  None while muted: the Echo does not act on the wake word then */
const char *const core_wake_events[] = { "detected", "near_miss", "answered_elsewhere", "no_speech" };
const int core_wake_nevents = sizeof core_wake_events / sizeof core_wake_events[0];

static void wake_told(const char *event)       /* lock held */
{
    if (connected && proto->wake && !core_muted()) proto->wake(event);
}

static void wake_tell(const char *event)       /* any thread but the lock's holder */
{
    pthread_mutex_lock(&core_lock); wake_told(event); pthread_mutex_unlock(&core_lock);
}

/* lock held.  Home Assistant heard the wake word in the stream (-w remote, wake word engine "homeassistant"): what the
 * Echo does on a wake word of its own, the sound, the ring and the front end's utterance state */
void core_remote_wake(void)
{
    if (core_local_wake || !atomic_load(&streaming)) return;
    run_wake = 1;
    wake_told("detected");
    sound_request(SND_WAKE);
    listening(1);
    core_set_state(LISTENING);
}

int core_wake_sound(int set) { if (set >= 0) use_earcon = set; return use_earcon; }

int core_wake_in_request(int set) { if (set >= 0) wake_in_request = set != 0; return wake_in_request; }
int core_end_of_request(int set) { if (set >= 0 && set < MICEND_MODES) end_mode = set; return end_mode; }
int core_stop_ends_listening(int set) { if (set >= 0) stop_ends = set != 0; return stop_ends; }

int core_bt_announce(int set) { if (set >= 0) use_bt_announce = set; return use_bt_announce; }

/* Models whose controller does not answer to our bring-up yet (biscuit's MT8163 combo) run with -B: no A2DP sink, no
 * Bluetooth proxy; the stock stack may keep the radio. */
int core_bluetooth(int set) { if (set >= 0) use_bt = set; return use_bt; }

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

/* While the Echo is discoverable the ring runs Amazon's blue device search chaser: `scone-setup` (stock's
 * "discovery-in-progress"), frame for frame the same as `btpair-setup` but listed in layer_config_common.json (layer 4,
 * below listening/thinking/talking, so a voice command still shows on top).  It loops until unset. */
void core_bt_pairing(int on)
{
    pthread_mutex_lock(&core_lock);
    led(on ? "-s" : "-u", "scone-setup");
    pthread_mutex_unlock(&core_lock);
}

/* Do not disturb, like stock: announcements from Home Assistant are dropped, while the wake word, replies, timers, music
 * and Bluetooth connection messages carry on.  Switching it on shows Amazon's single purple pulse (do_not_disturb: 2 s
 * fade in and out, layer 2, nothing after its `loop` marker); switching it off shows nothing. */
static atomic_llong dnd_clear_at;

/* Identify (settings page, Home Assistant's button): which of several Echos is this one.  Amazon's rainbow (zzz_rainbow,
 * a loop, the same file on donut, biscuit and radar) for IDENTIFY_MS and stock's setup beacon sound, whatever the wake
 * sound setting says: it was asked for.  Again while it runs: longer, and the sound once more.  Lock held or not. */
#define IDENTIFY_MS 10000
static atomic_llong identify_until;
void core_identify(void)
{
    if (!atomic_exchange(&identify_until, mono_ms() + IDENTIFY_MS)) led("-s", "zzz_rainbow");
    sound_queue(SND_IDENTIFY);
    fprintf(stderr, "identify: rainbow for %d s\n", IDENTIFY_MS / 1000);
}
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

/* ---------------------------------------------------------------- Drop In (dropin.c)
 * As stock shows a call: the ring's call animations (layer 2, under listening and replies; every model has them) and the
 * comms sounds of the image, whatever the wake sound setting says (a call that opens the microphone is never silent).
 * The front end's listening mode follows who talks (core_dropin_listen). */
static atomic_int dropin_ring = -1;                 /* the earcon thread loops this ringtone (SND_RING_*), -1: none */
static atomic_llong dropin_ring_at;                 /* ... from then on (mono_ms) */
static atomic_int dropin_live;
static int dropin_was;                              /* lock held */
static const char *const dropin_led[4] = { NULL, "call_outbound_ringing", "call_incoming", "call_connected" };
static void mix_apply(void);

void core_dropin(int st, const char *peer, int outgoing)
{
    pthread_mutex_lock(&core_lock);
    int was = dropin_was;
    dropin_was = st;
    if (st != was) {
        fprintf(stderr, "drop in: %s%s%s\n", dropin_states[st], peer[0] ? " " : "", peer);
        if (dropin_led[was]) led("-u", dropin_led[was]);
        if (dropin_led[st]) led("-s", dropin_led[st]);
        else if (was == DROPIN_LIVE) led("-s", "call_connected_end");
        /* the caller's ringtone only after a second: an Echo that connects at once (answer: auto) is in before that */
        atomic_store(&dropin_ring_at, mono_ms() + (st == DROPIN_CALLING ? 1000 : 0));
        atomic_store(&dropin_ring, st == DROPIN_RINGING ? SND_RING_IN : st == DROPIN_CALLING ? SND_RING_OUT : -1);
        if (st == DROPIN_LIVE) sound_queue(outgoing || was == DROPIN_RINGING ? SND_CALL_ON : SND_DROPIN);
        else if (st == DROPIN_IDLE) sound_queue(SND_CALL_OFF);
        atomic_store(&dropin_live, st == DROPIN_LIVE);
        mix_apply();                                /* a loud call's volume counts now, or no longer (mix_target) */
        if (st != DROPIN_LIVE) { listen_call = 0; listening_apply(); }       /* dropin.c turns it on while this side talks */
        if (connected && proto->dropin_changed) proto->dropin_changed(st, peer);
    }
    pthread_mutex_unlock(&core_lock);
    /* A call is the newest source, as Alexa's paused what played: music's leftovers in micAsr would open the gate and go
     * to the other side.  No resume afterwards, as between the music sources; a phone without remote control was only
     * made silent and is heard again.  Outside core_lock: sendspin takes its own lock */
    if (st == DROPIN_LIVE && was != DROPIN_LIVE) { if (core_sendspin_port) sendspin_pause(); a2dp_pause(); }
    if (st == DROPIN_IDLE && was == DROPIN_LIVE) a2dp_unyield();
}

/* What came of a call this Echo made, for Home Assistant ("Drop In result").  One refused here (this Echo in a call or
 * muted, the other not in its network) never left idle, so nothing above made a sound: the end of a call's, as for a
 * call the other Echo refuses, which goes from calling back to idle.  Said to the Echo, the voice blueprint's reply
 * follows it */
void core_dropin_result(int r, const char *peer, int here)
{
    pthread_mutex_lock(&core_lock);
    fprintf(stderr, "drop in: result %s%s%s%s\n", dropin_results[r], peer[0] ? " " : "", peer, here ? " (refused here: end of call sound)" : "");
    if (here) sound_queue(SND_CALL_OFF);
    if (connected && proto->dropin_result) proto->dropin_result(dropin_results[r]);
    pthread_mutex_unlock(&core_lock);
}

/* Listening mode in a call, from dropin.c's capture side (not the lock).  On for the whole call, as for a command, the
 * echo canceller never adapted to the Voip path: it stops adapting in that mode, and what it left of the far end in
 * micAsr was -46..-62 dBFS; off, -63..-69, the room's floor about -70 (2026-10-08, biscuit, the same speech at the same
 * volume).  Off for the whole call, a talker would be taken for interference after 1.5 s.  So on only while this side
 * talks (dropin.c: its level over the room's and over the echo it can expect, with a hangover) */
void core_dropin_listen(int on)
{
    pthread_mutex_lock(&core_lock);
    if (on != listen_call && (!on || atomic_load(&dropin_live))) { listen_call = on; listening_apply(); }
    pthread_mutex_unlock(&core_lock);
}

/* Any lock or none (dropin_call runs under core_lock from Home Assistant): plain reads of flags */
const char *core_dropin_refusal(int incoming)
{
    if (buttons_muted() || soft_mute) return "the microphones are off";
    if (incoming && dnd) return "do not disturb is on";
    return NULL;
}

/* ---------------------------------------------------------------- sound detection
 * The stock detector (sound.h, docs/re-aed.md), off unless Home Assistant switches it on: then a second decoder runs on
 * the mic stream beside the wake word.  Home Assistant gets the types the model can tell apart.  On every test window
 * smokeAlarm, smokeSiren and carbonMonoxideSiren scored the same, and so did cough and runningWater: one event each.
 * humanPresence is left out: it fires on any talk, TV, knock or alarm clock, every window while someone is about.
 * Stock checks each hit in Amazon's cloud before anyone is told; nothing here can, so a window in which the Echo itself
 * made sound is dropped: echo cancellation leaves enough of a timer ringing to pass for a beeping appliance. */
static const struct { const char *amazon, *event; } sound_map[] = {
    { "smokeAlarm", "smoke_or_co_alarm" }, { "smokeSiren", "smoke_or_co_alarm" }, { "carbonMonoxideSiren", "smoke_or_co_alarm" },
    { "glassBreak", "glass_break" }, { "dogBark", "dog_bark" }, { "babyCry", "baby_cry" }, { "snore", "snoring" },
    { "cough", "cough" }, { "waterSounds", "water" }, { "beepingAppliance", "beeping_appliance" },
};
#define SOUND_MAP (int)(sizeof sound_map / sizeof sound_map[0])
const char *const core_sound_events[] = { "smoke_or_co_alarm", "glass_break", "dog_bark", "baby_cry", "snoring", "cough",
                                          "water", "beeping_appliance" };
const int core_sound_nevents = sizeof core_sound_events / sizeof core_sound_events[0];
static atomic_int sound_want;               /* the switch; the capture thread opens and closes the decoder to match */
static atomic_int sound_failed;             /* the last start failed (the model did not load): the settings page says so */
int core_sound_failed(void) { return atomic_load(&sound_failed); }

int core_sound(int set)
{
    if (set >= 0 && set != atomic_load(&sound_want)) {
        atomic_store(&sound_want, set);
        fprintf(stderr, "sound detection: switched %s\n", set ? "on" : "off");
    }
    return atomic_load(&sound_want);
}

static void on_sound(const char *const *types, int n)    /* detector thread */
{
    long long now = mono_ms(), window = sound_window_ms();
    if (atomic_load(&alarm_on) || atomic_load(&music_on) || atomic_load(&tts_on) || atomic_load(&earcon_sounding)
        || atomic_load(&sounds_pending) || now - atomic_load(&own_sound_ms) < window
        || now - atomic_load(&earcon_heard_until) < window) {
        fprintf(stderr, "sound: dropped, the Echo played something in that window\n");
        return;
    }
    const char *sent[SOUND_MAP]; int ns = 0;
    pthread_mutex_lock(&core_lock);
    if (atomic_load(&sound_want) && !core_muted() && connected && proto->sound)
        for (int i = 0; i < n; i++)
            for (int j = 0; j < SOUND_MAP; j++) {
                if (strcmp(types[i], sound_map[j].amazon)) continue;
                int dup = 0;
                for (int k = 0; k < ns; k++) dup |= sent[k] == sound_map[j].event;
                if (!dup) { sent[ns++] = sound_map[j].event; proto->sound(sound_map[j].event); }
            }
    pthread_mutex_unlock(&core_lock);
}

/* Whisper detection (whisper.h, docs/re-whisper.md): each request's mic audio, from the start of streaming to Home
 * Assistant's VAD end, is scored once; the binary sensor says whether the last one was whispered, for the conversation
 * agent's prompt template.  The result comes within milliseconds of the end of speech, while speech to text still
 * runs, so it is in Home Assistant before the agent's prompt is rendered.  Without the DAVS model there is no sensor. */
static atomic_int whisper_eou;              /* core_mic_off: the end of speech came (not a cancel or timeout) */
static atomic_int whisper_last = -2;        /* core_whispered */

int core_whispered(void) { return atomic_load(&whisper_last); }
static int whisper_model;
static atomic_int whisper_enabled = 1;               /* the settings page's "Whisper detection" */
int  core_whisper_model(void) { return whisper_model; }
void core_whisper_enable(int on) { atomic_store(&whisper_enabled, on != 0); }

static void on_whisper(int whispered, int confidence, int threshold)    /* detector thread */
{
    fprintf(stderr, "whisper: %s (confidence %d, threshold %d)\n", whispered ? "whispered" : "not whispered", confidence, threshold);
    atomic_store(&whisper_last, whispered);
    pthread_mutex_lock(&core_lock);
    if (connected && proto->whispered) proto->whispered(whispered);
    pthread_mutex_unlock(&core_lock);
}

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

void core_mic_level(int dbfs)
{
    if (dbfs != mic_gain.level) { micgain_init(&mic_gain, dbfs); mic_fresh = 1; }
}

int core_wake_sound_removal(int set) { if (set >= 0) earsup_enable(set); return earsup_enabled(); }

int core_mic_denoise(int set)
{
    if (set >= 0 && set != mic_denoise) { mic_denoise = set > 3 ? 3 : set; mic_fresh = 1; }
    return mic_denoise;
}

/* lock held.  The mic stream to the pipeline has stopped. */
static void mic_stopped(void)
{
    if (mic_end.mode && !mic_end.ended && mic_end.heard) {     /* for tuning (issue #66) */
        fprintf(stderr, "end of request: not heard here first (talker heard %d ms, quiet for %d ms", mic_end.heard * 10, mic_end.quiet * 10);
        if (mic_end.vad_frames) fprintf(stderr, ", Amazon's VAD: no speech in %d of %d loud frames", mic_end.vad_vetoed, mic_end.vad_frames);
        fprintf(stderr, ")\n");
        mic_end.mode = MICEND_OFF;
    }
    listening(0);
    afe_done();
    if (denoise_frames) fprintf(stderr, "denoise: %.1f s of audio took %.0f ms of CPU\n", denoise_frames / 100.0, denoise_ns / 1e6);
    denoise_frames = 0; denoise_ns = 0;
}

void core_mic_off(void) { atomic_store(&whisper_eou, 1); atomic_store(&streaming, 0); mic_stopped(); }

/* lock held, capture thread: the talker finished (micend.h).  As Home Assistant's own VAD end, from here */
static void heard_end(void)
{
    fprintf(stderr, "end of request: heard here (talker %.1f dBFS, nothing within %.0f dB of it for %d ms; ",
            mic_end.talker_db, micend_margin(mic_end.mode), micend_hold_ms(mic_end.mode));
    if (mic_end.vad_frames) fprintf(stderr, "Amazon's VAD heard no speech in %d of %d loud frames)\n", mic_end.vad_vetoed, mic_end.vad_frames);
    else fprintf(stderr, "no VAD in the stream)\n");
    proto->end();
    core_mic_off();
    if (state == LISTENING) core_set_state(THINKING);
}

void core_restart_after(void) { barge_in = 1; barge_from = 0; }

void core_pipeline_finish(void)
{
    if (atomic_exchange(&streaming, 0) && connected && proto->stop) proto->stop();
    mic_stopped();
    core_set_state(IDLE);
    if (mono_ms() - last_wake_ms > 3000) wake_reset();      /* a reset puts the engine back to sleep: "<wake word>, stop" would lose its "stop" */
    if (!connected) barge_in = 0;
    if ((barge_in || !core_local_wake) && satellite_running && connected) {
        if (barge_in) sound_request(SND_WAKE);
        uint64_t from = barge_in ? barge_from : 0;      /* what was said after the wake word while the reply was cut */
        barge_in = 0; barge_from = 0;
        pipeline_start(from);
    }
}

void core_link(int up, int ready)
{
    int was = satellite_running;
    connected = up; satellite_running = up && ready;
    if (satellite_running) adopted();
    if (!satellite_running) core_pipeline_finish();
    else if (!was && !core_local_wake && state == IDLE) pipeline_start(0);
}

void core_error(void)
{
    if (quiet_abort) { quiet_abort = 0; return; }      /* Home Assistant's complaint about a pipeline "stop" ended: not news */
    if (state != SPEAKING) { led("-s", "anim_start_error_short"); core_pipeline_finish(); }
}

/* Nothing said after the wake word (or the button, or a follow-up question): the wake word was meant for another
 * device, or the Echo woke by mistake (issue #48: 4 in 5 days on two Dots, each with the error ring).  Nothing failed
 * that the ring could help with; a stock Echo ends such a turn quietly too */
void core_no_speech(void)
{
    if (quiet_abort) { quiet_abort = 0; return; }
    if (run_wake) wake_told("no_speech");
    if (state != SPEAKING) core_pipeline_finish();
}

/* Lock held.  Ends the run without anything of it carried out, as the action button does: Home Assistant aborts it
 * (VoiceAssistantRequest start=false alone, its _abort_pipeline), proto_esphome.c drops what it still reports of it
 * until the next run's RUN_START, and its complaint about that is not shown (quiet_abort).  Needs proto->cancel */
static void quiet_cancel(void)
{
    barge_in = 0;
    atomic_store(&streaming, 0);
    proto->cancel();
    core_pipeline_finish();
    quiet_abort = 1;
}

/* Lock held.  The wake word just heard has had its "stop".  4 s: the engine reports "stop" only in the ~2 s after the
 * wake word, and a round adds at most 1 s */
static int stop_heard(void) { return heard_ms && stop_ms >= heard_ms && stop_ms - heard_ms < 4000; }

/* Lock held.  That wake word starts nothing (trigger: its "stop" came during the arbitration round, before any run).
 * What stop_word's "pipeline dropped" does to a run that started: a "stop" that ended what the wake word cut drops
 * that wake word's run, started or not.  Without this a ring stopped during the round was followed by a run that
 * listened, with the "stop" in its replayed audio (the round's window is up to 1 s, all of it when the other Echos
 * did not hear the wake word).  With stop_ends_listening any "stop" does */
static int stopped_now(void) { return stop_heard() && (stop_cut || stop_ends); }

/* touch: the action button rather than the wake word.  from: where the wake word ended on the ring's clock (0: unknown) */
static void trigger(int touch, uint64_t from)
{
    pthread_mutex_lock(&core_lock);
    /* A ringing timer stops only when told to: the button, "<wake word>, stop" (stop_word, core_transcript), Home
     * Assistant's media stop or "Stop ringing", any request with timer_stop_any.  The wake word alone puts it in the background, as on a stock Echo: the ring pauses while
     * the pipeline runs ("Alexa, how long is left on the pasta timer" while another rings) and goes on after it */
    if (touch && atomic_load(&alarm_on)) {
        core_alarm_stop("button");
        wake_cut_ms = mono_ms();
    } else if ((state == THINKING || (touch && state == LISTENING)) && connected && proto->cancel) {
        /* As the center button of a Voice PE: a misheard command is stopped before its tool calls run, not only its
         * reply.  Same message as ESPHome's voice_assistant.stop; Home Assistant cancels the run's task (the LLM, and
         * the tool calls it has not made yet).  The wake word does the same and then listens again, as during a reply,
         * unless its "stop" came already (during the round) */
        fprintf(stderr, "%s: pipeline cancelled\n", touch ? "button" : "wake word");
        quiet_cancel();
        if (!touch && stopped_now()) fprintf(stderr, "stop: came during the round, nothing started\n");
        else if (!touch && state == IDLE && satellite_running && !core_muted()) {
            wake_cut_ms = mono_ms();
            sound_request(SND_WAKE);
            pipeline_start(from);
        }
    } else if (!connected || !satellite_running || core_muted()) {
        /* nothing to talk to, or privacy latch on */
    } else if (state == IDLE && !touch && stopped_now()) {
        fprintf(stderr, "stop: came during the round, nothing started\n");
    } else if (state == IDLE) {
        if (atomic_load(&alarm_on) || dropin_was != DROPIN_IDLE) wake_cut_ms = mono_ms();     /* its "stop" drops this pipeline again */
        if (touch) wake_phrase[0] = 0;                          /* the button: the first wake word's assistant */
        sound_request(touch ? SND_TOUCH : SND_WAKE);
        pipeline_start(touch ? 0 : from);
    } else if (state == SPEAKING && !touch && stopped_now()) {
        /* its "stop" came during the round to a THINKING run whose reply began before the round ended: the reply is
         * cut as stop_word would have cut it, and no run follows */
        fprintf(stderr, "stop: came during the round, reply cut\n");
        barge_in = 0;
        atomic_store(&flush_playback, 1);
    } else if (state == SPEAKING && !atomic_load(&flush_playback)) {       /* not already being cut.  barge_in alone does not
                                                                             * say that: continue-conversation sets it too */
        fprintf(stderr, "barge-in\n");
        wake_cut_ms = mono_ms();
        barge_in = 1; barge_from = from;
        atomic_store(&flush_playback, 1);   /* playback thread drops TTS up to audio-stop, then we restart */
    }
    pthread_mutex_unlock(&core_lock);
}

/* "<wake word>, stop" to music (Sendspin, a phone over Bluetooth): paused as the action button does, which resumes it.
 * Lock held (Sendspin's adm_lock nests inside core_lock, as for the volume).  1 = something played and was paused */
static long long music_stopped_ms;                  /* core_lock: when a stop last paused music */
static int music_stop(void)
{
    int paused = (core_sendspin_port && sendspin_stop()) | a2dp_stop();     /* both: one may not have heard it yet */
    if (paused) music_stopped_ms = mono_ms();
    return paused;
}

/* "Stop", the second keyword of Amazon's wake word models.  The engine reports it only right behind the wake word
 * ("<wake word>, stop"; on its own it is never detected, also not with a changed op.cfg.json: tried).
 * It ends what is making noise and never starts anything:
 * a ringing alarm, a reply being spoken (also one that would listen again afterwards), a Drop In call, else music.  Said
 * as "<wake word>, stop", the wake word has already cut the reply and opened a new pipeline by the time "stop" is
 * recognised: that pipeline is dropped again.  Only then: "<wake word>, stop the music" out of silence is a command for
 * Home Assistant, not for us.
 * Music (issue #51) is paused here rather than left to speech to text, which often misses the one short word over it
 * (three tries in a row on a Dot 2).  Its pipeline goes on, so "<wake word>, stop the timer" still reaches Home
 * Assistant; nothing said after it ends quietly (core_no_speech), and a bare "stop" is cancelled (core_transcript).
 * With stop_ends_listening (off by default) "<wake word>, stop" means "be quiet": after all of the above it also ends
 * the run that is listening, quietly, as the button does (stop_heard: also one this wake word just opened out of
 * silence), and keeps the round's winner from opening one (trigger).  An Echo that woke by mistake listens for up to
 * 15 s (Home Assistant's VAD limit) and has whatever is said meanwhile carried out; this ends it at once.  The cost:
 * "<wake word>, stop the timer" no longer reaches Home Assistant, and one Echo's log (7 days) had "stop" in 24 of 700
 * wake words, 2 of them false in 354 requests it answered: such a command is lost and has to be said again.
 * A "stop" during the arbitration round finds no run of its wake word yet: trigger opens none then when the "stop"
 * ended something (stop_cut, stopped_now), as the drop above would, or with stop_ends_listening. */
static void stop_word(void)
{
    int ended = 0;
    if (dropin_running() && dropin_status(NULL, 0) != DROPIN_IDLE) { dropin_hangup("stop"); ended = 1; }   /* and the pipeline its wake word opened */
    pthread_mutex_lock(&core_lock);
    stop_ms = mono_ms();
    if (atomic_load(&alarm_on)) { core_alarm_stop("stop"); ended = 1; }     /* and the pipeline its wake word opened, below */
    stop_cut = ended || state == SPEAKING || state == THINKING;               /* THINKING: the round's winner cancels it (trigger) */
    if (state == SPEAKING) {
        fprintf(stderr, "stop: reply cut\n");
        barge_in = 0;
        atomic_fetch_and(&sounds_pending, ~(1 << SND_WAKE | 1 << SND_TOUCH));
        atomic_store(&flush_playback, 1);
    } else if (state == LISTENING && mono_ms() - wake_cut_ms < 4000) {
        fprintf(stderr, "stop: pipeline dropped\n");
        stop_cut = 1;                       /* a wake word whose round still runs (heard while this listened) opens none */
        atomic_fetch_and(&sounds_pending, ~(1 << SND_WAKE | 1 << SND_TOUCH));
        /* Cancelled as the button does, not ended.  Ended (end of audio, then start=false), nothing here marked the
         * run cancelled (after a THINKING cancel the new run's RUN_START clears the mark): Home Assistant's STT_START
         * a moment later put the Echo back to LISTENING with the mic closed.  Once the "stop" it had been sent was
         * carried out too (a media player elsewhere paused): the end of audio reaches Home Assistant before the abort,
         * and its STT may finish on what it has.  Without the end of audio only the abort goes out.  Wyoming has
         * no cancel: ended there, as before */
        if (connected && proto->cancel) quiet_cancel();
        else { barge_in = 0; core_pipeline_finish(); quiet_abort = 1; }
    } else {
        int quiet = state == LISTENING && stop_ends && stop_heard() && connected && proto->cancel;
        if (!ended && atomic_load(&music_on) && music_stop()) fprintf(stderr, "stop: music paused%s\n", quiet ? "" : ", the pipeline goes on");
        if (quiet) {                                            /* as the button: nothing of the run is carried out */
            fprintf(stderr, "stop: listening ended\n");
            atomic_fetch_and(&sounds_pending, ~(1 << SND_WAKE | 1 << SND_TOUCH));
            quiet_cancel();
        }
    }
    pthread_mutex_unlock(&core_lock);
}

/* ---------------------------------------------------------------- wake word arbitration (arb.c)
 * With other Echos in the arbitration network, a detection is scored and only acted on once the others' claims are in:
 * when every member's is (arb_settled), at the latest after the window (arb.c: learned, 150-1000 ms), in the capture
 * thread.  Until then nothing shows (no sound, no ring), so the Echos that lose stay
 * quiet.  The audio of the window is not lost: the winner sends it from the ring buffer ahead of the live stream.
 * Nor is it without arbitration: Pryon accepts a keyword 116-128 ms after its end (6 accepts in device logs, "samples
 * <begin>-<end> (fed <n>)"), and the live stream starts with the next 50 ms block, so what the talker says right after
 * the wake word would be cut.  Every pipeline a wake word starts sends the ring from the end of the word's tail, which
 * comes after the keyword's end (word_tail_end, wordtail.h). */

#define RING_SAMPLES (CAP_RATE * 4)
static int16_t ring[RING_SAMPLES];                  /* what the wake word engine was fed, by its sample index */
static uint64_t ring_n;                             /* capture thread: samples fed so far */
static atomic_int det_pending;                      /* a detection for the capture thread; det_begin/end under core_lock */
static uint64_t det_begin, det_end;
static long long arb_due;                           /* capture thread: a round runs, decide then */
static uint64_t arb_from;                           /* capture thread: where the wake word of the round ended */

static void ring_put(const int16_t *s, size_t n)
{
    for (size_t i = 0; i < n; i++) ring[(ring_n + i) % RING_SAMPLES] = s[i];
    ring_n += n;
}

/* Amazon's front end heard speech in samples [from, to) of the ring (micend.h): its DNN VAD >= 1 in a record of a
 * 128-sample frame overlapping them (afemeta.h), 0 when none (a veto, so only where the network is sure: inside
 * commands it dips under 2 for up to 0.44 s, mostly to 1, and aggressive ends after 0.45 s; sounds without words are 0
 * in 91-100 % of frames, docs/re-vad.md), -1 when the ring has no records there (the PC, an audio
 * HAL that drops the lowest bits, -w remote: no ring).  Records start every 128 samples from the mixer stream's
 * start, the ring's 0, until a read loses samples (a stalled micAsr reopened): then the phase is looked for again in
 * the 128 samples before from.  Capture thread */
static int meta_phase = -1;

static int ring_meta(uint64_t at, struct afemeta *m)
{
    int16_t s[AFEMETA_FRAME];
    if (at + AFEMETA_FRAME > ring_n || (ring_n > RING_SAMPLES && at < ring_n - RING_SAMPLES)) return 0;
    for (int i = 0; i < AFEMETA_FRAME; i++) s[i] = ring[(at + i) % RING_SAMPLES];
    return afemeta_frame(s, m);
}

static int ring_speech(uint64_t from, uint64_t to)
{
    struct afemeta m;
    for (int tries = 0; tries < 2; tries++) {
        if (meta_phase >= 0) {
            int v = -1, lost = 0;
            for (uint64_t a = from - (from + AFEMETA_FRAME - (uint64_t)meta_phase) % AFEMETA_FRAME; a < to && a + AFEMETA_FRAME <= ring_n; a += AFEMETA_FRAME) {
                if (!ring_meta(a, &m)) { lost = 1; break; }
                v = m.vad >= 1 ? 1 : v < 0 ? 0 : v;
            }
            if (!lost) return v;
        }
        meta_phase = -1;
        for (uint64_t k = 0; k < AFEMETA_FRAME && k <= from; k++)
            if (ring_meta(from - k, &m)) { meta_phase = (int)((from - k) % AFEMETA_FRAME); break; }
        if (meta_phase < 0) return -1;
    }
    return -1;
}

static double ring_power(uint64_t a, uint64_t b)    /* mean square over samples [a, b), as far as the ring still has them */
{
    double sum = 0; uint64_t n = 0;
    if (ring_n > RING_SAMPLES && a < ring_n - RING_SAMPLES) a = ring_n - RING_SAMPLES;
    if (b > ring_n) b = ring_n;
    for (uint64_t i = a; i < b; i++, n++) { double v = ring[i % RING_SAMPLES]; sum += v * v; }
    return n ? sum / n : 0;
}

/* The room's level as this Echo hears it, at any time, for the media log (media_mark): the processed mic stream (what
 * the wake word engine gets, also with -w remote) in 100 ms frames, the mean square of the last 10 frames (1 s) and,
 * as the floor, the frame at the 20th percentile of the last 50 (5 s), so that a voice does not make the room loud;
 * both in dBFS as wake_score's lines.  Measured in the capture thread, published as two atomics in dBFS x 100: any
 * thread reads them without a lock.  room_cdb INT_MIN: less than 5 s heard yet */
#define ROOM_FRAME (CAP_RATE / 10)
#define ROOM_FRAMES 50
static atomic_int room_cdb = INT_MIN, floor_cdb = INT_MIN;

static int cmp_int(const void *a, const void *b) { int x = *(const int *)a, y = *(const int *)b; return (x > y) - (x < y); }

static void room_put(const int16_t *s, size_t n)    /* capture thread */
{
    static double sum, ms[ROOM_FRAMES]; static int cdb[ROOM_FRAMES]; static unsigned k, frames;
    for (size_t i = 0; i < n; i++) {
        sum += (double)s[i] * s[i];
        if (++k < ROOM_FRAME) continue;
        unsigned at = frames++ % ROOM_FRAMES; int sorted[ROOM_FRAMES]; double last = 0;
        ms[at] = sum / ROOM_FRAME; cdb[at] = (int)lround(1000 * log10((ms[at] + 1) / (32768.0 * 32768.0)));
        sum = 0; k = 0;
        if (frames < ROOM_FRAMES) continue;
        for (int j = 0; j < 10; j++) last += ms[(at + ROOM_FRAMES - j) % ROOM_FRAMES] / 10;
        memcpy(sorted, cdb, sizeof sorted); qsort(sorted, ROOM_FRAMES, sizeof sorted[0], cmp_int);
        atomic_store(&floor_cdb, sorted[ROOM_FRAMES / 5]);
        atomic_store(&room_cdb, (int)lround(1000 * log10((last + 1) / (32768.0 * 32768.0))));
    }
}

static void room_str(char *o, size_t cap)           /* any thread: "room -61.2 dBFS (floor -66.0)" */
{
    int r = atomic_load(&room_cdb), f = atomic_load(&floor_cdb);
    if (r == INT_MIN || f == INT_MIN) snprintf(o, cap, "room not measured yet");
    else snprintf(o, cap, "room %.1f dBFS (floor %.1f)", r / 100.0, f / 100.0);
}

/* Signal to noise of the wake word in dB x 100: the keyword against the half second before it (ending 100 ms ahead, so
 * that an early "begin" does not count the word as noise).  On the processed stream after beamforming, AEC and gain
 * control, the absolute level says less than how far the voice stands out of the room: the Echo the talker is close to
 * and facing hears it clearest.  HASSMIC_TEST_SCORE stands in for it on the PC, where SIGUSR1 plays the detection. */
/* Amazon's own measure of the wake word, which its cloud used to pick the Echo that answers ("ESP"): the energy of the
 * keyword and of the room before it as the front end measures them ("1-mic ESP" in its log).  As stock does it: hand the front end the keyword's place on its clock
 * (wake_afe_times), then read LASP_CMD_REQUEST_ARBITRATION_JSON: {"voiceEnergy":..,"ambientEnergy":..,..}.  Both
 * through the firmware's tools: 150 ms on the Dots (lipc), 240 and 380 ms on an Echo Show 5 (two service calls; issue
 * #77), so on a thread of its own (afe_thread).  Their ratio in dB x 100 is a signal to noise like our own score below,
 * so Echos without it (older builds, other front ends) still compare.  Reading it also puts the front end into its
 * utterance state and starts its diagnostics (FINDINGS.md "Listening mode"): afe_done() ends both.  0: not available. */
static int afe_score(long ts, long te, int *score)  /* afe_thread */
{
    char buf[512]; const char *v, *a; long long t0 = mono_ms();
    if (!afe_arbitration(ts, te, buf, sizeof buf)) return 0;
    if (!(v = strstr(buf, "\"voiceEnergy\":")) || !(a = strstr(buf, "\"ambientEnergy\":"))) return 0;
    atomic_store(&afe_asked, 1);
    double voice = atof(strchr(v, ':') + 1), ambient = atof(strchr(a, ':') + 1);
    *score = (int)lround(1000 * log10((voice + 1) / (ambient + 1)));
    fprintf(stderr, "wake: front end: voice energy %.0f, ambient %.0f (%lld ms)\n", voice, ambient, mono_ms() - t0);
    return 1;
}

static int wake_score(uint64_t begin, uint64_t end, int simulated)
{
    const char *t = getenv("HASSMIC_TEST_SCORE");
    if (simulated && t) return atoi(t);
    uint64_t gap = CAP_RATE / 10, len = CAP_RATE / 2;
    uint64_t ne = begin > gap ? begin - gap : 0, nb = ne > len ? ne - len : 0;
    double w = ring_power(begin, end), n = ring_power(nb, ne), fs = 32768.0 * 32768.0;
    fprintf(stderr, "wake: level %.1f dBFS over noise %.1f dBFS\n", 10 * log10((w + 1) / fs), 10 * log10((n + 1) / fs));
    return (int)lround(1000 * log10((w + 1) / (n + 1)));
}

/* The front end's reading, off the capture thread (issue #77).  On it, the capture stood still for as long as the
 * reading took, and the Echo Show 5's recorder had 160 ms of blocks to fill meanwhile (audio_android.c CAP_NBUF, then
 * 8): the rest was lost, a splice 100-250 ms long in the command right after the wake word, which speech-to-text
 * misheard.  Now the capture goes on into the ring, and the claim goes out once the score is in (score_wait), as late
 * as before; without it after AFE_WAIT_MS, with the mic stream's score (the others' window until learned is 500 ms,
 * arb.c).  A reading left behind still puts the front end into its utterance state: afe_late ends that again when no
 * command came of the wake word.  One at a time: a wake word while one runs goes without (a service call that hangs
 * costs the score, no longer the capture). */
#define AFE_WAIT_MS 500
static atomic_int afe_busy;                         /* a reading runs (afe_thread) */
static atomic_int afe_got;                          /* its score; INT_MIN: none */
static long afe_ts, afe_te;                         /* the keyword's place on the front end's clock, set before it starts */
static long long score_due;                         /* capture thread: the claim waits for the reading until then */
static struct { uint64_t end; int own, prio; char kw[64]; } pend;   /* capture thread: the wake word that waits */
static int afe_late;                                /* capture thread: a reading was left behind */

static void *afe_thread(void *arg)
{
    int score;
    (void)arg;
    atomic_store(&afe_got, afe_score(afe_ts, afe_te, &score) ? score : INT_MIN);
    atomic_store(&afe_busy, 0);                     /* last: once it is 0, afe_asked says what the reading did */
    return NULL;
}

static int afe_start(int simulated)                 /* capture thread: 1 = a reading runs */
{
    pthread_t t;
    if (atomic_load(&afe_busy)) { fprintf(stderr, "wake: the front end's last reading still runs, no score from it\n"); return 0; }
    /* SIGUSR1's wake word has no place on the front end's clock; on the PC HASSMIC_FAKE_AFE plays one (audio_file.c) */
    if ((simulated && !getenv("HASSMIC_FAKE_AFE")) || !wake_afe_times(&afe_ts, &afe_te)) return 0;
    atomic_store(&afe_busy, 1);
    if (pthread_create(&t, NULL, afe_thread, NULL)) { atomic_store(&afe_busy, 0); return 0; }
    pthread_detach(t);
    return 1;
}

static void afe_late_check(void)                    /* capture thread */
{
    if (!afe_late || atomic_load(&afe_busy)) return;
    afe_late = 0;
    /* a round or a command still to come ends it itself; under the lock, so that a command starting meanwhile
     * (pipeline_start, Home Assistant's thread) does not get its utterance state ended */
    pthread_mutex_lock(&core_lock);
    if (!arb_due && !atomic_load(&streaming)) afe_done();
    pthread_mutex_unlock(&core_lock);
}

/* Kiosk Satellite's measure of a wake word, for arb.c's Kiosk Satellite mode (docs/kiosk-arbitration.md, "Score"): of the
 * last 3 s the wake word engine heard, in 20 ms frames of dBFS, the mean of the 10 loudest frames of the last 1.5 s
 * (the wake word) over the frame at the 20th percentile of all 3 s (the room), that floor never under -75 dBFS; dB x 100.
 * Loudness, nothing else: the Echo the talker is closest to and facing wins, as with the front end's energies.
 * micAsr runs about 30 dB under the level speech-to-text expects (micgain.c): a quiet room's floor would sit on their
 * -75 dBFS clamp and shorten our margin against a kiosk's, whose Android mic runs at speech level.  So the frames are
 * lifted by KIOSK_LIFT_DB first, which leaves a floor above the clamp untouched and the margin a plain signal to noise.
 * A guess from that 30 dB: not yet measured next to a kiosk.  INT_MIN: less than 3 s heard (a kiosk then answers
 * without a claim).  HASSMIC_TEST_SCORE stands in for it on the PC, as for wake_score. */
#define KIOSK_LIFT_DB 30

static int cmp_desc(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x < y) - (x > y); }

static int kiosk_score(uint64_t end, int simulated)
{
    enum { FRAME = CAP_RATE / 50, NF = 150, RECENT = 75, LOUD = 10 };
    const char *t = getenv("HASSMIC_TEST_SCORE");
    double db[NF], s[NF], speech = 0, floor_;
    if (simulated && t) return atoi(t);
    if (end > ring_n) end = ring_n;
    if (end < (uint64_t)FRAME * NF || ring_n - end + (uint64_t)FRAME * NF > RING_SAMPLES) return INT_MIN;
    for (int i = 0; i < NF; i++) {
        uint64_t a = end - (uint64_t)(NF - i) * FRAME;
        db[i] = 10 * log10(ring_power(a, a + FRAME) / (32768.0 * 32768.0) + 1e-10) + KIOSK_LIFT_DB;
    }
    memcpy(s, db + NF - RECENT, sizeof s[0] * RECENT);
    qsort(s, RECENT, sizeof s[0], cmp_desc);
    for (int i = 0; i < LOUD; i++) speech += s[i] / LOUD;
    memcpy(s, db, sizeof s);
    qsort(s, NF, sizeof s[0], cmp_desc);
    floor_ = s[NF - 1 - NF / 5];                        /* sorted loudest first: a fifth of the frames are quieter */
    if (floor_ < -75) floor_ = -75;
    fprintf(stderr, "wake: kiosk score: wake word %.1f dBFS over room %.1f dBFS (both +%d dB)\n", speech, floor_, KIOSK_LIFT_DB);
    return (int)lround((speech - floor_) * 100);
}

/* lock held.  Mic audio to the pipeline: our own sounds taken out if switched on (earsup.h), noise reduction if
 * switched on, then brought to speech level.  at: the capture clock of the first sample (the ring's index).
 * - The wake word just before it sets the gain to start with (from up to 3 s back), and RNNoise first hears the second
 *   of room and wake word ahead of the command, so that it does not start on the first word.
 * - Our own wake sound is still in the stream after the echo canceller: +21 dB over the floor in micRaw, +4 to +8 dB in
 *   micAsr (4 triggers, 2026-09-30), as loud as a quiet talker.  The gain took it for speech and came down for the
 *   command behind it (-32 instead of -26 dBFS in Home Assistant's recording), so it holds still while a sound plays,
 *   and over audio earsup held back across one.  sound: this audio has one in it (sound_in, earsup). */
static void send_out(const int16_t *pcm, size_t n, int sound)
{
    static int16_t out[1024 + MICDENOISE_FRAME];
    mic_gain.hold = sound;
    micend_run(&mic_end, pcm, n, mic_gain.hold);        /* the level as heard, before the gain; heard_end() acts on it */
    while (n) {
        size_t k = n < 1024 ? n : 1024, m = k;
        const int16_t *src = pcm;
        if (mic_denoise) {
            struct timespec t0, t1;
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t0);
            m = micdenoise_run(pcm, k, out); src = out;
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t1);
            denoise_ns += (t1.tv_sec - t0.tv_sec) * 1000000000LL + t1.tv_nsec - t0.tv_nsec; denoise_frames += m / MICDENOISE_FRAME;
        }
        if (m) {
            micgain_run(&mic_gain, src, out, m); proto->audio(out, m * 2);
            double e = 0; for (size_t i = 0; i < m; i++) e += (double)out[i] * out[i];   /* the band's listening bar */
            ring_level(e > 0 ? (int)lround(100 * log10(e / m / (32768.0 * 32768.0))) : -1000);
        }
        pcm += k; n -= k;
    }
}

/* Whether audio has our sound in it, by when it was captured: the replay goes out later than that, so what plays now
 * says nothing of it.  Held from ahead of a sound, a command said in one breath kept the gain of the quieter wake word
 * (issue #82, Dot 2: +17..+31 dB), each frame on the peak limit; not held after one that is over (a slow aligner), the
 * sound fed the talker's level.  A Drop In's ring and a timer's tone are not in it, as before: both stop for a pipeline */
static int sound_in(uint64_t at, size_t n)
{
    for (size_t i = 0; i < sizeof sound_spans / sizeof *sound_spans; i++)
        if (at < (uint64_t)atomic_load(&sound_spans[i].to) && at + n > (uint64_t)atomic_load(&sound_spans[i].from)) return 1;  /* to first: sound_start */
    return 0;
}

static void send_mic(const int16_t *pcm, size_t n, uint64_t at)
{
    if (mic_fresh) {
        micgain_start(&mic_gain, mono_ms() - last_wake_ms < 3000 ? keyword_db : 1); mic_fresh = 0;
        mic_gain.limit_stays = 1;                       /* people speak on over the wake sound */
        static const int denoise_db[] = { 0, 6, 9, 12 };
        fprintf(stderr, "mic gain: talker %.1f dBFS, gain %+.1f dB", micgain_talker_db(&mic_gain), mic_gain.gain_db);
        if (mic_denoise) fprintf(stderr, ", noise reduction %d dB", denoise_db[mic_denoise]);
        micend_start(&mic_end, proto->end ? end_mode : MICEND_OFF, micgain_talker_db(&mic_gain), at, ring_speech);
        if (talker_from > at) mic_end.from = talker_from;   /* the wake word sent too: not the request's speech yet */
        mic_gain.talker_min_db = mic_end.mode ? mic_end.talker_db - micend_margin(mic_end.mode) : -1000;   /* nor the gain's */
        if (mic_end.mode) fprintf(stderr, ", end heard here: %s", micend_names[mic_end.mode]);
        fprintf(stderr, "\n");
        if (mic_denoise) {
            micdenoise_reset(denoise_db[mic_denoise]);
            for (uint64_t i = at > CAP_RATE ? at - CAP_RATE : 0; core_local_wake && i < at; ) {   /* the ring: local wake word only */
                size_t o = i % RING_SAMPLES, k = at - i;
                if (k > RING_SAMPLES - o) k = RING_SAMPLES - o;
                micdenoise_run(ring + o, k, NULL);
                i += k;
            }
        }
        earsup_reset();
    }
    while (n) {
        size_t k = n < 1024 ? n : 1024, m;
        const int16_t *p; int held_sound;
        m = earsup_run(pcm, k, at, &p, &held_sound);
        if (m) send_out(p, m, held_sound || sound_in(at, m));
        pcm += k; n -= k; at += k;
    }
}

static void stream_ring(uint64_t from, uint64_t to) /* capture thread, lock held: samples [from, to) as far as the ring has them */
{
    if (ring_n > RING_SAMPLES && from < ring_n - RING_SAMPLES) from = ring_n - RING_SAMPLES;
    if (to > ring_n) to = ring_n;
    while (from < to) {
        uint64_t at = from % RING_SAMPLES, n = to - from;
        if (n > RING_SAMPLES - at) n = RING_SAMPLES - at;
        send_mic(ring + at, n, from);
        from += n;
    }
}

/* Each slot's wake word aligner (wakealign.h): the file for the word the slot holds in the language spoken here
 * (align_resolve), next to our binary (scripts/bundle.sh) or in $HASSMIC_WAKEALIGN.  Capture thread: looked up at
 * every detection, loaded when the file to use changed (the time zone comes from Home Assistant after the start, the
 * language from the page).  NULL: none for that word, the replay starts by wordtail.h's rule */
static struct wakealign *wake_align[WAKE_SLOTS];
static struct core_wake_word align_word[WAKE_SLOTS];        /* capture thread: what each slot holds */
static char align_file[WAKE_SLOTS][600];                    /* and the file its aligner came from, "" none */
static int align_stale[WAKE_SLOTS];                         /* capture thread: wake_align[slot] is a word's the slot no
                                                             * longer holds, kept while a run may use it */

static atomic_int align_busy;                       /* align_thread runs on one of them */

/* wake_open_word (capture thread): the slot holds w now.  The old word's aligner goes at once, unless a run uses it:
 * then align_resolve frees it when the run is over (waiting here would hold up the capture, issue #77); until then it
 * is never run (align_ahead resolves first) and word_tail_end goes by the level */
static void wake_align_load(int slot, const struct core_wake_word *w)
{
    align_word[slot] = *w; align_file[slot][0] = 0; align_stale[slot] = 0;
    if (atomic_load(&align_busy)) { align_stale[slot] = 1; return; }
    wakealign_free(wake_align[slot]); wake_align[slot] = NULL;
}

/* The language spoken here, as a locale of tools/wakealign/build.sh's: the page's speech_language, else the time zone
 * Home Assistant gave (clock.c state/timezone; a German speaker's "Alexa" with the English wake word is found far better
 * with German sounds, settings.c SPEECH_LANG), else the word's own ("computer-en-US"); "" when none of them says.  A
 * language picked on the page in the zone's variety of it where the zone has one (en in London: en-GB) */
static void speech_locale(const struct core_wake_word *w, char *out, size_t cap)
{
    static const char *const langs[][2] = { { "de", "de-DE" }, { "en", "en-US" }, { "fr", "fr-FR" }, { "es", "es-ES" }, { "ja", "ja-JP" } };
    static const char *const zones[][2] = {      /* by prefix, the first that matches */
        { "Europe/Berlin", "de-DE" }, { "Europe/Vienna", "de-DE" }, { "Europe/Zurich", "de-DE" }, { "Europe/Busingen", "de-DE" },
        { "Europe/Vaduz", "de-DE" }, { "Europe/Paris", "fr-FR" }, { "Europe/Monaco", "fr-FR" }, { "Europe/Madrid", "es-ES" },
        { "Atlantic/Canary", "es-ES" }, { "America/Mexico_City", "es-MX" }, { "America/Monterrey", "es-MX" }, { "America/Tijuana", "es-MX" },
        { "America/Cancun", "es-MX" }, { "America/Merida", "es-MX" }, { "America/Chihuahua", "es-MX" }, { "Asia/Tokyo", "ja-JP" },
        { "Europe/London", "en-GB" }, { "Europe/Dublin", "en-GB" }, { "Australia/", "en-AU" }, { "Asia/Kolkata", "en-IN" },
        { "Asia/Calcutta", "en-IN" }, { "Canada/", "en-CA" }, { "America/", "en-US" }, { "US/", "en-US" } };
    const char *l = settings_speech_language(), *zl = NULL; char p[300], zone[96] = ""; FILE *f; size_t ln = strlen(l);
    out[0] = 0;
    state_file(p, sizeof p, "timezone");
    if ((f = fopen(p, "r"))) { if (fscanf(f, "%95s", zone) != 1) zone[0] = 0; fclose(f); }
    for (size_t i = 0; zone[0] && !zl && i < sizeof zones / sizeof *zones; i++) if (!strncmp(zone, zones[i][0], strlen(zones[i][0]))) zl = zones[i][1];
    /* a language picked: the zone's variety of it where the zone has one (English in London: en-GB's dictionary) */
    if (ln && zl && !strncmp(zl, l, ln) && zl[ln] == '-') { snprintf(out, cap, "%s", zl); return; }
    for (size_t i = 0; ln && i < sizeof langs / sizeof *langs; i++) if (!strcmp(l, langs[i][0])) { snprintf(out, cap, "%s", langs[i][1]); return; }
    if (zl) { snprintf(out, cap, "%s", zl); return; }
    const char *dash = strchr(w->id, '-');
    if (dash && !wake_is_mww(w->manifest)) snprintf(out, cap, "%s", dash + 1);     /* a microWakeWord id is a name, no locale */
}

static void align_resolve(int slot)                 /* capture thread */
{
    /* locales whose words are made of the same models and dictionary as another's (tools/wakealign/build.sh): its files */
    static const char *const same[][2] = { { "en-CA", "en-US" }, { "en-AU", "en-GB" }, { "fr-CA", "fr-FR" } };
    char dir[300], loc[16], word[64], cand[2][600]; const char *env = getenv("HASSMIC_WAKEALIGN"); int n = 0;
    const struct core_wake_word *w = &align_word[slot];
    if (!w->id[0]) return;
    if (env) snprintf(dir, sizeof dir, "%s", env);
    else {
        char *slash; ssize_t l = readlink("/proc/self/exe", dir, sizeof dir - 1);
        if (l <= 0) return;
        dir[l] = 0; if ((slash = strrchr(dir, '/'))) *slash = 0;
    }
    speech_locale(w, loc, sizeof loc);
    if (wake_is_mww(w->manifest)) {                 /* a microWakeWord model by its name: "Okay Nabu" -> okay_nabu */
        size_t k = 0;
        for (const char *c = w->name; *c && k < sizeof word - 1; c++) word[k++] = isalnum((unsigned char)*c) ? (char)tolower((unsigned char)*c) : '_';
        word[k] = 0;
    } else snprintf(word, sizeof word, "%.*s", (int)strcspn(w->id, "-"), w->id);      /* Amazon's: "computer-en-US" -> computer */
    /* the language spoken, else the word's own (the firmware's "alexa" and microWakeWord's are English) */
    const char *dash = strchr(w->id, '-'), *own = !wake_is_mww(w->manifest) && dash ? dash + 1 : "en-US", *locs[2] = { loc, own };
    for (int i = 0; i < 2; i++) {
        const char *l = locs[i];
        for (size_t k = 0; k < sizeof same / sizeof *same; k++) if (!strcmp(l, same[k][0])) l = same[k][1];
        if (l[0]) snprintf(cand[n++], sizeof cand[0], "%s/%s-%s.wa", dir, word, l);
    }
    const char *pick = "";
    for (int i = 0; i < n && !*pick; i++) if (!access(cand[i], R_OK)) pick = cand[i];
    if (!align_stale[slot] && !strcmp(pick, align_file[slot])) return;
    wakealign_free(wake_align[slot]); wake_align[slot] = NULL; align_stale[slot] = 0;
    snprintf(align_file[slot], sizeof align_file[slot], "%.599s", pick);
    if (*pick && (wake_align[slot] = wakealign_load(pick))) fprintf(stderr, "wake: %s aligned by %s (%s spoken)\n", w->id, pick, loc[0] ? loc : "?");
}

/* The aligner's run, off the capture thread as the front end's reading is (issue #77: on it the capture stood still,
 * and the Echo Show 5's recorder lost what came meanwhile): 4 ms for the window on the PC, 0.2 s under qemu-arm, and
 * the speaker adaptation's update after it more again.  The capture goes on into the ring; the replay waits for the
 * answer (align_done) as it waits for the window.  One run at a time: a wake word while one runs goes by the level.
 * Speaker adaptation (wakealign.h, setting wake_word_adaptation): one household's statistics per feature space (a
 * language's models share one), state/wakealign-<space>.adapt, written at most every 10 minutes; the run aligns with
 * the transform where there is one and falls back to the plain alignment when that finds nothing near Pryon's end, and
 * what it settled on is added.  Online on the 220 German requests (each with what the ones before taught): 158 ->
 * 163; on 75 English ones in German voices: 48 -> 53 */
static uint64_t align_key;                          /* capture thread: the wake word a run was started (or given up) for */
static uint64_t align_skipped;                      /* capture thread: one that got none (a run busy, no thread): by the level */
static _Atomic uint64_t align_done;                 /* the one whose answer is in align_at, align_ms (set before it) */
static int64_t align_at; static long long align_ms;
static struct { const struct wakealign *wa; int16_t win[WAKEALIGN_BEFORE + WAKEALIGN_AFTER]; size_t n; uint64_t from, end, stop; int adapt; } job;
/* align_thread's: a household's statistics per feature space in memory (557 KB each), one per acoustic model of
 * tools/wakealign/build.sh; past that the one used longest ago makes room, saved first */
#define ADAPT_SPACES 5
static struct { struct wakealign_adapt *ad; long long saved_ms, used_ms; int saved_at; } adapts[ADAPT_SPACES];

static void adapt_save(int i)                       /* align_thread: adapts[i] to state/, if it learnt since */
{
    char name[64], p[300]; struct wakealign_adapt *ad = adapts[i].ad;
    if (!ad || ad->requests == adapts[i].saved_at) return;
    snprintf(name, sizeof name, "wakealign-%016llx.adapt", (unsigned long long)ad->space); state_file(p, sizeof p, name);
    if (!wakealign_adapt_save(ad, p)) { adapts[i].saved_ms = mono_ms(); adapts[i].saved_at = ad->requests; }
}

static int adapt_for(uint64_t space)                /* align_thread: the slot of the household's statistics in space, -1 */
{
    int i, at = -1; char name[64], p[300];
    if (!space) return -1;
    for (i = 0; i < ADAPT_SPACES; i++) if (adapts[i].ad && adapts[i].ad->space == space) { adapts[i].used_ms = mono_ms(); return i; }
    for (i = 0; i < ADAPT_SPACES; i++) {            /* a free slot, else the one used longest ago */
        if (!adapts[i].ad) { at = i; break; }
        if (at < 0 || adapts[i].used_ms < adapts[at].used_ms) at = i;
    }
    if (adapts[at].ad) { adapt_save(at); free(adapts[at].ad); adapts[at].ad = NULL; }
    if (!(adapts[at].ad = calloc(1, sizeof *adapts[at].ad))) return -1;
    struct wakealign_adapt *ad = adapts[at].ad;
    ad->space = space;
    snprintf(name, sizeof name, "wakealign-%016llx.adapt", (unsigned long long)space); state_file(p, sizeof p, name);
    if (!wakealign_adapt_load(ad, p)) fprintf(stderr, "wake: speaker adaptation from %d requests (%s)\n", ad->requests, name);
    adapts[at].saved_ms = adapts[at].used_ms = mono_ms(); adapts[at].saved_at = ad->requests;
    return at;
}

static int align_near(int64_t at) { return at >= (int64_t)job.end - CAP_RATE * 2 / 5 && at <= (int64_t)job.stop; }

static void align_said(char *s, size_t n, long e)   /* e: in the window, -1 none */
{
    if (e < 0) snprintf(s, n, "none");
    else snprintf(s, n, "%+lld ms", ((long long)job.from + e - (long long)job.end) * 1000 / CAP_RATE);
}

static void *align_thread(void *arg)
{
    (void)arg;
    long long t0 = mono_ms();
    int slot = job.adapt ? adapt_for(wakealign_space(job.wa)) : -1;
    struct wakealign_adapt *ad = slot >= 0 ? adapts[slot].ad : NULL;
    int adapted = 0; long e = -1, by_adapt = -1;
    /* the word's end against Pryon's in the 220 requests of wakealign.h: 250 ms before to 450 ms after; further off is
     * a misalignment (the PC's SIGUSR1 has no word there: 1 s before).  Tighter than -400 ms loses words: over 200 ms
     * of the command cut in 67 of 4586 -> 115 at -250 ms (docs/wake-word-end.md) */
    if (ad && ad->have) { e = by_adapt = wakealign_end_adapt(job.wa, job.win, job.n, ad, NULL); adapted = e >= 0 && align_near((int64_t)(job.from + (uint64_t)e)); }
    if (!adapted) e = wakealign_end(job.wa, job.win, job.n);
    int64_t at = e >= 0 ? (int64_t)(job.from + (uint64_t)e) : -1;
    align_at = at >= 0 && align_near(at) ? at : -1;
    if (align_at < 0) {                             /* what it said: issue #79's two on a Dot 2 left no trace of it */
        char plain[32], adapt[32];
        align_said(plain, sizeof plain, e); align_said(adapt, sizeof adapt, by_adapt);
        fprintf(stderr, "wake: the aligner's end, from Pryon's end: %s, with the speaker adaptation %s\n", plain, ad && ad->have ? adapt : "not in use");
    }
    /* tests: a CPU too busy for it (word_tail_end's ALIGN_LATE): the ms in that file, read each run */
    const char *slow = getenv("HASSMIC_FAKE_ALIGN_SLOW"); FILE *sf; int ms = 0;
    if (slow && (sf = fopen(slow, "r"))) { if (fscanf(sf, "%d", &ms) != 1) ms = 0; fclose(sf); }
    if (ms > 0) usleep((useconds_t)ms * 1000);
    align_ms = mono_ms() - t0;
    atomic_store(&align_done, job.end);             /* the answer is out: the capture thread may go on */
    if (ad && align_at >= 0) {
        wakealign_end_adapt(job.wa, job.win, job.n, adapted ? ad : NULL, ad);
        int had = ad->have; wakealign_estimate(ad);
        if (ad->have && !had) fprintf(stderr, "wake: speaker adaptation in use (%d requests)\n", ad->requests);
        if (mono_ms() - adapts[slot].saved_ms > 600000 || ad->requests == WAKEALIGN_ADAPT_MIN) adapt_save(slot);
    }
    atomic_store(&align_busy, 0);
    return NULL;
}

static void align_ahead(void)                       /* capture thread, before it takes core_lock for the block */
{
    pthread_mutex_lock(&core_lock); uint64_t end = replay_from; int slot = kw_slot; float kdb = keyword_db; pthread_mutex_unlock(&core_lock);
    if (!end || end == align_key || kdb > 0 || slot < 0 || slot >= WAKE_SLOTS) return;
    if (!atomic_load(&align_busy)) align_resolve(slot);         /* the model for it (never swapped under a run) */
    if (ring_n < end + WAKEALIGN_AFTER) return;                 /* while the window fills */
    align_key = end;
    if (!wake_align[slot]) return;
    if (atomic_load(&align_busy)) { align_skipped = end; return; }
    uint64_t from = end > WAKEALIGN_BEFORE ? end - WAKEALIGN_BEFORE : 0, stop = end + WAKEALIGN_AFTER;
    if (ring_n > RING_SAMPLES && from < ring_n - RING_SAMPLES) from = ring_n - RING_SAMPLES;
    for (uint64_t i = from; i < stop; i++) job.win[i - from] = ring[i % RING_SAMPLES];
    job.wa = wake_align[slot]; job.n = (size_t)(stop - from); job.from = from; job.end = end; job.stop = stop;
    job.adapt = settings_on("wake_word_adaptation");
    pthread_t t;
    atomic_store(&align_busy, 1);
    if (pthread_create(&t, NULL, align_thread, NULL)) { atomic_store(&align_busy, 0); align_skipped = end; return; }
    pthread_detach(t);
}

/* Where the replay after a wake word that ended at end starts, from the ring up to to (capture thread, lock held); -1:
 * not known yet, the stream waits for more.  With an aligner the word's end as aligned in [end - 1.5 s, end + 0.5 s]
 * (align_ahead), before Pryon's end too (Echo, Ziggy: it fires after the word); else, or when it finds nothing near,
 * wordtail.h's rule from Pryon's end on.  Both wait for 0.5 s after the end, then the stream catches up from the ring */
#define ALIGN_LATE CAP_RATE                         /* samples past the aligner's window it is waited for: 1 s */
static int64_t word_tail_end(uint64_t end, uint64_t to)
{
    float db[WORDTAIL_FRAMES]; int n = 0, cut, pause_ok = 1; const char *why = "";
    if (keyword_db > 0) return (int64_t)end;        /* no level known (the PC's SIGUSR1) */
    if (kw_slot >= 0 && kw_slot < WAKE_SLOTS && wake_align[kw_slot]) {
        static uint64_t told;                       /* the wake word the level's reason was logged for */
        int in = atomic_load(&align_done) == end;
        /* align_thread's answer not in yet: waited for until ALIGN_LATE past the window (0.2 s under qemu-arm; a CPU
         * that busy would hold the whole request), then the level, and its answer is not taken any more */
        if (align_skipped != end && !in && to < end + WAKEALIGN_AFTER + ALIGN_LATE) return -1;
        if (align_skipped != end && in && align_at >= 0) {
            if (!replay_whole) fprintf(stderr, "wake: the word ends %+lld ms from Pryon's end (aligned in %lld ms)\n",
                                       (long long)(align_at - (int64_t)end) * 1000 / CAP_RATE, align_ms);
            return align_at;
        }
        if (told != end) {
            told = end;
            fprintf(stderr, "wake: %s, by the level instead\n", align_skipped == end ? "no aligner run for it (one was busy)" :
                    in ? "the aligner found no end near Pryon's" : "the aligner took over 1 s");
        }
        align_skipped = end;
        pause_ok = 0;
    }
    for (; n < WORDTAIL_FRAMES && end + (uint64_t)(n + 1) * WORDTAIL_FRAME <= to; n++)
        db[n] = 10 * log10(ring_power(end + (uint64_t)n * WORDTAIL_FRAME, end + (uint64_t)(n + 1) * WORDTAIL_FRAME) / (32768.0 * 32768.0) + 1e-10);
    if ((cut = wordtail_cut(db, n, keyword_db, pause_ok, &why)) < 0) return -1;
    if (!replay_whole) fprintf(stderr, "wake: %d ms of the word's tail left out (%s)\n", cut * 1000 * WORDTAIL_FRAME / CAP_RATE, why);
    return (int64_t)(end + (uint64_t)cut * WORDTAIL_FRAME);
}

/* ---------------------------------------------------------------- media in the room (wake_heard)
 * The room's players come from the setting arbitration_media_players; Home Assistant sends their states over the keyed
 * link (proto->media_subscribe, core_media_state).  All of it: lock held, except own_ended */
struct media_player { char id[96], st[16]; int active, known, vol, vol_known; long long ended, since; };   /* ended: mono_ms it stopped playing;
                                                    since: it was subscribed on a keyed link (0: not on any); st: HA's last
                                                    state, vol: its volume_level x 1000, both for the media log only */
static struct media_player room[MEDIA_MAX];
static int nroom, media_ambient = 50, media_linked;  /* media_linked: a keyed link follows the room's players */
static char media_ids[SETTINGS_TEXT_MAX + 1];
static atomic_llong own_ended;                      /* mono_ms our own music (Sendspin, Bluetooth) last stopped; 0: never */

/* How long media counts after it stopped: the talk that follows the music (turn it down, the next title), and a pause
 * that is only a pause.  HASSMIC_MEDIA_HOLD_MS for tests */
static long long media_hold_ms(void)
{
    static long long ms = -1;
    if (ms < 0) { const char *e = getenv("HASSMIC_MEDIA_HOLD_MS"); ms = e && atoll(e) > 0 ? atoll(e) : 120000; }
    return ms;
}

/* How long Home Assistant may take to send a player's state: it sends it at once when the entity exists, and nothing
 * when it does not (a typo in the id).  HASSMIC_MEDIA_MISSING_MS for tests */
static long long media_missing_ms(void)
{
    static long long ms = -1;
    if (ms < 0) { const char *e = getenv("HASSMIC_MEDIA_MISSING_MS"); ms = e && atoll(e) > 0 ? atoll(e) : 10000; }
    return ms;
}

/* The media log (lines "media: ..."): how much a room's music adds to this Echo's background, measured, so that the fixed
 * offset can give way to one that follows the music (PLAN.md, the Echo of the room where media plays: 2026-10-17 a quiet
 * Sonos got the 3 dB, its Echo's background lower than the other's).  On each change of a listed player (playing or
 * buffering against anything else), of its volume, and of our own playback: the room's level then (room_str: the
 * second before the change reached us, and the floor of the 5 s before), and again media_after_ms later; meanwhile
 * any other noise (people talking) is in both.  While something plays, a line every media_every_ms.  Logging only:
 * nothing here feeds a score.  mlog_lock: a leaf, taken with core_lock held or without (core_music) */
#define MLOG_MAX 8
static pthread_mutex_t mlog_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { char id[96], what[32]; int volume; long long due; } mlog[MLOG_MAX];     /* due 0: free */
static atomic_int mlog_pending;

static long long media_after_ms(void)               /* HASSMIC_MEDIA_AFTER_MS for tests */
{
    static long long ms = -1;
    if (ms < 0) { const char *e = getenv("HASSMIC_MEDIA_AFTER_MS"); ms = e && atoll(e) > 0 ? atoll(e) : 5000; }
    return ms;
}

static long long media_every_ms(void)               /* HASSMIC_MEDIA_EVERY_MS for tests */
{
    static long long ms = -1;
    if (ms < 0) { const char *e = getenv("HASSMIC_MEDIA_EVERY_MS"); ms = e && atoll(e) > 0 ? atoll(e) : 60000; }
    return ms;
}

/* any thread: the line for now, and the one media_log_tick writes later.  A state's later line is never replaced, so a
 * player that flips back still gets it; a volume's is (a slider moved in steps: one line after the last) */
static void media_mark(const char *id, const char *what, int volume, const char *line)
{
    char r[64]; int k = -1; room_str(r, sizeof r);
    fprintf(stderr, "media: %s, %s\n", line, r);
    pthread_mutex_lock(&mlog_lock);
    for (int i = 0; volume && i < MLOG_MAX && k < 0; i++) if (mlog[i].due && mlog[i].volume && !strcmp(mlog[i].id, id)) k = i;
    for (int i = 0; i < MLOG_MAX && k < 0; i++) if (!mlog[i].due) k = i;
    if (k >= 0) {
        if (!mlog[k].due) atomic_fetch_add(&mlog_pending, 1);
        snprintf(mlog[k].id, sizeof mlog[k].id, "%s", id); snprintf(mlog[k].what, sizeof mlog[k].what, "%s", what);
        mlog[k].volume = volume; mlog[k].due = mono_ms() + media_after_ms();
    }
    pthread_mutex_unlock(&mlog_lock);
}

/* volume_led_thread, no lock held: the later lines that are due, and the line every media_every_ms while a listed player
 * plays or our own music does (not during the hold: that is the talk after the music) */
static void media_log_tick(void)
{
    static long long every;
    char lines[MLOG_MAX + 1][360], r[64]; int nl = 0; long long now = mono_ms();
    if (!atomic_load(&mlog_pending) && now - every < media_every_ms()) return;
    room_str(r, sizeof r);
    pthread_mutex_lock(&core_lock);
    pthread_mutex_lock(&mlog_lock);
    for (int i = 0; i < MLOG_MAX; i++) {
        if (!mlog[i].due || now < mlog[i].due) continue;
        const char *st = "?"; int active = 0, own = !strcmp(mlog[i].id, "own playback");
        if (own) { active = atomic_load(&music_on) != 0; st = active ? "on" : "off"; }
        for (int j = 0; !own && j < nroom; j++) if (!strcmp(room[j].id, mlog[i].id)) { st = room[j].known ? room[j].st : "unknown"; active = room[j].active; }
        int now_too = mlog[i].volume ? !active : strcmp(st, mlog[i].what) != 0;     /* what changed meanwhile */
        snprintf(lines[nl++], sizeof lines[0], "media: %.95s %.31s %lld s, %.63s%s%.15s", mlog[i].id, mlog[i].what, media_after_ms() / 1000, r,
                 now_too ? ", now " : "", now_too ? st : "");
        mlog[i].due = 0; atomic_fetch_sub(&mlog_pending, 1);
    }
    pthread_mutex_unlock(&mlog_lock);
    if (now - every >= media_every_ms()) {
        char ids[MEDIA_MAX * 98 + 16] = ""; size_t n = 0;
        if (atomic_load(&music_on)) n += (size_t)snprintf(ids, sizeof ids, "own playback");
        for (int j = 0; j < nroom && n < sizeof ids; j++)
            if (room[j].active) n += (size_t)snprintf(ids + n, sizeof ids - n, "%s%s", n ? ", " : "", room[j].id);
        if (n) snprintf(lines[nl++], sizeof lines[0], "media: %s, playing: %.250s", r, ids);
        every = now;                                /* nothing plays: the first line a full period after it starts */
    }
    pthread_mutex_unlock(&core_lock);
    for (int i = 0; i < nl; i++) fprintf(stderr, "%s\n", lines[i]);
}

/* Home Assistant's entity ids as it makes them (slugs: lower case letters, digits, _), so that the list holds only
 * what can match; at most MEDIA_MAX: Home Assistant keeps a tracker per subscription for as long as the link lasts */
int core_media_players(const char *list, char *err, size_t errsz)
{
    char ids[MEDIA_MAX][96], log[MEDIA_MAX * 98] = ""; int n = 0; size_t ln = 0; const char *p = list;
    struct media_player old[MEDIA_MAX]; int nold = nroom;
    while (*p) {
        size_t l = strcspn(p, ","), k; const char *a = p, *e = p + l; int ok, dup = 0;
        while (a < e && isspace((unsigned char)*a)) a++;
        while (e > a && isspace((unsigned char)e[-1])) e--;
        p += l + (p[l] == ',');
        if (a == e) continue;
        k = (size_t)(e - a); ok = k > 13 && k <= 13 + 80 && !strncmp(a, "media_player.", 13);
        for (size_t j = 13; ok && j < k; j++) ok = (a[j] >= 'a' && a[j] <= 'z') || (a[j] >= '0' && a[j] <= '9') || a[j] == '_';
        if (!ok) { snprintf(err, errsz, "\"%.*s\" is not a media player's entity id (media_player.<lower case letters, digits, _>)", (int)(k > 100 ? 100 : k), a); return -1; }
        for (int j = 0; j < n; j++) dup |= strlen(ids[j]) == k && !strncmp(ids[j], a, k);
        if (dup) continue;
        if (n == MEDIA_MAX) { snprintf(err, errsz, "at most %d media players", MEDIA_MAX); return -1; }
        memcpy(ids[n], a, k); ids[n++][k] = 0;
    }
    /* the ones still listed keep what Home Assistant said of them; new ones are followed at once (removed ones stay
     * followed until the link closes: ESPHome's API has no unsubscribe, and their states are ignored; one listed again
     * gets the last state Home Assistant sent of it, proto_esphome.c media_sub).  The list first, then the
     * subscriptions: a state known already comes back at once (core_media_state) */
    int fresh[MEDIA_MAX] = { 0 };
    memcpy(old, room, sizeof old);
    memset(room, 0, sizeof room); media_ids[0] = 0;
    for (int i = 0; i < n; i++) {
        int j = 0; size_t m = strlen(media_ids);
        while (j < nold && strcmp(old[j].id, ids[i])) j++;
        if (j < nold) room[i] = old[j];
        else { memcpy(room[i].id, ids[i], sizeof room[i].id); fresh[i] = 1; }
        snprintf(media_ids + m, sizeof media_ids - m, "%s%s", i ? "," : "", ids[i]);
    }
    nroom = n;
    for (int i = 0; i < n; i++)
        if (fresh[i] && connected && proto->media_subscribe && proto->media_subscribe(ids[i]) > 0) {
            room[i].since = mono_ms();
            if (ln < sizeof log) ln += (size_t)snprintf(log + ln, sizeof log - ln, "%s%s", ln ? ", " : "", ids[i]);
        }
    if (ln) fprintf(stderr, "arbitration: following %s in Home Assistant\n", log);
    return 0;
}

const char *core_media_list(void) { return media_ids; }
const char *core_media_player(int i) { return i >= 0 && i < nroom ? room[i].id : NULL; }
int core_media_ambient(int set) { if (set >= 10 && set <= 100) media_ambient = set; return media_ambient; }

/* playing and buffering count; anything else (paused, idle, on, off, standby, unavailable, unknown) is over, and counts
 * MEDIA_HOLD_MS more only if it played before: a TV that is merely on says nothing about the room */
int core_media_state(const char *entity, const char *state)
{
    for (int i = 0; i < nroom; i++) {
        struct media_player *m = &room[i]; int on = !strcmp(state, "playing") || !strcmp(state, "buffering");
        if (strcmp(m->id, entity)) continue;
        if (m->known && on != m->active) {              /* a change, not the state at subscribe */
            char line[160]; snprintf(line, sizeof line, "%s %s -> %s", entity, m->st, state);
            media_mark(entity, state, 0, line);
        }
        snprintf(m->st, sizeof m->st, "%s", state);
        if (on != m->active && !on) {
            m->ended = mono_ms();
            fprintf(stderr, "arbitration: %s %s (counts %lld s more)\n", entity, state, media_hold_ms() / 1000);
        } else if (on != m->active || !m->known) fprintf(stderr, "arbitration: %s %s\n", entity, state);
        m->active = on; m->known = 1;
        return 1;
    }
    return 0;
}

/* a keyed link subscribed to states (proto_esphome.c media_follow): it follows every listed player from now on */
void core_media_followed(void)
{
    long long now = mono_ms();
    media_linked = 1;
    for (int i = 0; i < nroom; i++) room[i].since = now;
}

/* Home Assistant gone (restart, network): no more states come, so what played counts as stopped now */
void core_media_link_lost(void)
{
    long long now = mono_ms();
    media_linked = 0;
    for (int i = 0; i < nroom; i++) {
        if (room[i].active) { room[i].ended = now; fprintf(stderr, "arbitration: %s counts as stopped, Home Assistant is gone (counts %lld s more)\n", room[i].id, media_hold_ms() / 1000); }
        room[i].active = room[i].known = room[i].vol_known = 0; room[i].since = 0;
    }
}

/* Home Assistant's volume_level of a listed player ("0.3"; nothing while it has none, e.g. off), for the media log
 * only: the first one is where it stands, a change is logged */
int core_media_volume(const char *entity, const char *value)
{
    char *e; double v = strtod(value, &e);
    if (e == value || !(v >= 0 && v <= 1.001)) return 0;
    for (int i = 0; i < nroom; i++) {
        struct media_player *m = &room[i]; int milli = (int)lround(v * 1000);
        if (strcmp(m->id, entity)) continue;
        if (m->vol_known && m->vol != milli) {
            char line[160], what[32];
            snprintf(what, sizeof what, "volume %.2f", milli / 1000.0);
            snprintf(line, sizeof line, "%s volume %.2f -> %.2f", entity, m->vol / 1000.0, milli / 1000.0);
            media_mark(entity, what, 1, line);
        }
        m->vol = milli; m->vol_known = 1;
        return 1;
    }
    return 0;
}

/* Is media playing in this room, and why (for the log and the page): own playback first, then a player that plays,
 * then what stopped last within MEDIA_HOLD_MS */
static int media_here(char *why, size_t cap)
{
    long long now = mono_ms(), hold = media_hold_ms(), own = atomic_load(&own_ended), last = 0; int k = -1;
    if (atomic_load(&music_on)) { snprintf(why, cap, "own playback"); return 1; }
    for (int i = 0; i < nroom; i++) if (room[i].active) { snprintf(why, cap, "%s playing", room[i].id); return 1; }
    for (int i = 0; i < nroom; i++) if (room[i].ended > last) { last = room[i].ended; k = i; }
    if (own && now - own < hold && own >= last) { snprintf(why, cap, "own playback ended %lld s ago", (now - own) / 1000); return 1; }
    if (k >= 0 && now - last < hold) { snprintf(why, cap, "%s stopped %lld s ago", room[k].id, (now - last) / 1000); return 1; }
    return 0;
}

/* linked: a keyed link follows the players.  A player's state: playing, stopped, unknown (no keyed link, or its state
 * not here yet), missing (subscribed media_missing_ms ago on a keyed link and still nothing: Home Assistant does not
 * know the entity, HA's tracker sends nothing for one it does not have) */
size_t core_media_json(char *o, size_t cap)
{
    char why[160]; int here = media_here(why, sizeof why); size_t n; long long now = mono_ms();
    n = (size_t)snprintf(o, cap, "{\"pct\":%d,\"here\":%s%s%s,\"linked\":%s,\"players\":[", media_ambient, here ? "\"" : "", here ? why : "null",
                         here ? "\"" : "", media_linked ? "true" : "false");
    for (int i = 0; i < nroom && n < cap; i++)     /* ids: [a-z0-9_.] only, nothing to escape */
        n += (size_t)snprintf(o + n, cap - n, "%s{\"id\":\"%s\",\"state\":\"%s\"}", i ? "," : "", room[i].id,
                              room[i].active ? "playing" : room[i].known ? "stopped"
                              : media_linked && room[i].since && now - room[i].since >= media_missing_ms() ? "missing" : "unknown");
    if (n < cap) n += (size_t)snprintf(o + n, cap - n, "]}");
    return n < cap ? n : cap - 1;
}

static void answer(uint64_t from)                   /* capture thread: act on the wake word that ended at from */
{
    trigger(0, from);
    if (!atomic_load(&streaming)) afe_done();          /* it stopped a ring, or cut a reply: the pipeline's end follows */
}

/* Media in the room.  The front end's score is the wake word's energy over the room's, and music beside an Echo raises
 * its room's: 2026-10-16, a Sonos playing beside the kitchen Echo and the talker in the kitchen, the living room Echo
 * won by 0.09 dB (17:55:05: kitchen voice 935183 / ambient 20909 = 1651, living room 529266 / 11588 = 1660; at 17:51:18
 * 1032 against 1114, the music no help either way).  So while media plays in its room (its own Sendspin or Bluetooth
 * music, a media player of arbitration_media_players playing or buffering, or MEDIA_HOLD_MS after either stopped), an
 * Echo counts the background only arbitration_media_ambient % (50 by default, 100 = off): 1000 * log10(100 / pct)
 * added to the score, +301 (3 dB) at 50 %.  The same as that factor on afe_score's ambient, but it holds for every
 * score: the mic stream's own, Kiosk Satellite mode's loudness (e = score / 10, arb.c), HASSMIC_TEST_SCORE.  At 50 %
 * the kitchen wins both rounds above (1952; 1333, there as a preference for the room).  The case it can turn wrong,
 * music in the kitchen and the talker in the living room, is not measured yet: hence 3 dB by default, 10 dB at most
 * (10 %).  On the wire it is only a higher score: claims keep their format, older Echos compare the number, and a
 * member could always claim any score.  An Echo in a conversation or ringing still claims priority 2, which no score
 * beats; so does one Home Assistant plays media on (play_media: state not IDLE), as before. */
static void claim(const char *kw, int score, int prio, uint64_t end)   /* capture thread */
{
    { char why[160], fl[48]; pthread_mutex_lock(&core_lock); int pct = media_ambient, here = media_here(why, sizeof why); pthread_mutex_unlock(&core_lock);
      int f = atomic_load(&floor_cdb);            /* the media log's: the room of the last 5 s, the wake word mostly above it */
      if (f == INT_MIN) fl[0] = 0; else snprintf(fl, sizeof fl, "; room floor %.1f dBFS", f / 100.0);
      if (!here) fprintf(stderr, "arbitration: no media of this room: score %d%s\n", score, fl);
      else if (pct >= 100) fprintf(stderr, "arbitration: media of this room (%s), setting off: score %d%s\n", why, score, fl);
      else { int bonus = (int)lround(1000 * log10(100.0 / pct));
             fprintf(stderr, "arbitration: media of this room (%s): ambient counted at %d %%, score %d -> %d%s\n", why, pct, score, score + bonus, fl);
             score += bonus; } }
    /* The other Echos' claims are Wi-Fi broadcasts, and the BLE scan's share of the antenna loses them (ble.c): no
     * scanning while this round listens: the wait it took (none when nobody was worth waiting for, arb.c "Rivals"),
     * then the claims of Echos that heard it a little later and the winner's "answers", 1 s at least.  Right after the
     * claim: its first copy is out before the scan stops anyway, the other two 30 and 80 ms on */
    long long due = arb_claim(kw, score, prio), w = due ? due - mono_ms() : 0;
    ble_quiet(w + 500 > 1000 ? (int)w + 500 : 1000);
    if (!due) { answer(end); return; }
    arb_due = due; arb_from = end;
}

static void wake_heard(uint64_t begin, uint64_t end, int simulated)     /* capture thread */
{
    if (!arb_running()) { trigger(0, end); return; }
    if (arb_due || score_due) return;               /* the same wake word once more while its round runs */
    pthread_mutex_lock(&core_lock);
    int alarm = atomic_load(&alarm_on), can = alarm || (connected && satellite_running && !core_muted()), prio = alarm || state != IDLE ? 2 : 0;
    pthread_mutex_unlock(&core_lock);
    if (!can) { trigger(0, end); return; }               /* could not answer: a claim would only silence the Echos that can */
    /* the one heard: with two wake words, Echos compete per wake word as they do with one each */
    pthread_mutex_lock(&core_lock); char kw[64]; wake_phrase_bare(kw, sizeof kw); pthread_mutex_unlock(&core_lock);
    /* Kiosk Satellite mode: their loudness, not the front end's energies (whose time only the window would pay for) */
    if (arb_mode(-1) == ARB_KIOSK) {
        int score = kiosk_score(end, simulated);
        if (score == INT_MIN) { fprintf(stderr, "arbitration: less than 3 s heard, answers without a claim\n"); trigger(0, end); return; }
        claim(kw, score, prio, end);
        return;
    }
    int own = wake_score(begin, end, simulated);
    /* the front end's score only for a round: arbitration off, or nobody to ask, and the claim is only a log line */
    if (!arb_rounds() || !afe_start(simulated)) { claim(kw, own, prio, end); return; }
    pend.end = end; pend.own = own; pend.prio = prio; snprintf(pend.kw, sizeof pend.kw, "%s", kw);
    score_due = mono_ms() + AFE_WAIT_MS;
}

static void score_wait(void)                        /* capture thread, while score_due: the claim once the reading is in */
{
    int busy = atomic_load(&afe_busy), afe = busy ? INT_MIN : atomic_load(&afe_got), score = pend.own;
    if (busy && mono_ms() < score_due) return;
    score_due = 0;
    if (afe != INT_MIN) { fprintf(stderr, "wake: score %d from the front end (%d from the mic stream)\n", afe, pend.own); score = afe; }
    else if (busy) { fprintf(stderr, "wake: no score from the front end within %d ms, the mic stream's goes\n", AFE_WAIT_MS); afe_late = 1; }
    claim(pend.kw, score, pend.prio, pend.end);
}

/* The keyword the loaded model answers to (capture thread).  Amazon's en-US sets for "Computer", "Amazon" and "Ziggy"
 * are one model (words.shrunk.txt: AMAZON COMPUTER HEY_DISNEY STOP ZIGGY, every one of them wakes from sleep in its
 * op.cfg.json, nothing in the set switches one off): with "Computer" picked, "Amazon" and "Ziggy" woke the Echo too
 * (GitHub issue 14).  Stock's client picks the one it wants out of the results; so do we.  The keyword is the set's
 * id up to the language ("computer-en-US" -> COMPUTER, the firmware's "alexa" -> ALEXA), but only if the set lists it:
 * a set installed by hand under another name answers to everything it knows, as before.  "" = no filter.  Per slot:
 * with "Computer" and "Amazon" both picked, both decoders hear both words, and each takes only its own. */
static char wake_keyword[WAKE_SLOTS][64];
static atomic_int wake_slot_heard;                  /* the slot whose wake word was heard last: its "stop" counts */
static void on_wake(int slot, const char *keyword, uint64_t begin, uint64_t end);

/* Pryon's near misses (its detector thread): only of the wake word picked, as on_wake takes them */
static void on_near_miss(int slot, const char *keyword)
{
    if (!core_local_wake || !strcasecmp(keyword, "STOP") || (wake_keyword[slot][0] && strcasecmp(keyword, wake_keyword[slot]))) return;
    if (atomic_load(&dropin_live) && dropin_far_talking()) return;
    wake_tell("near_miss");
}

static int words_list(const char *path, const char *kw)    /* words.shrunk.txt: "<word> <index>" per line */
{
    char line[96], w[64]; FILE *f = fopen(path, "r"); int hit = 0;
    if (!f) return 0;
    while (!hit && fgets(line, sizeof line, f)) hit = sscanf(line, "%63s", w) == 1 && !strcasecmp(w, kw);
    fclose(f);
    return hit;
}

static int wake_open_word(int slot, const struct core_wake_word *w)
{
    char dir[256], p[560], kw[64], *slash; DIR *d; struct dirent *e; int hit = 0;
    snprintf(kw, sizeof kw, "%.*s", (int)strcspn(w->id, "-"), w->id);
    snprintf(dir, sizeof dir, "%s", w->manifest);
    if ((slash = strrchr(dir, '/'))) *slash = 0;
    if (!wake_is_mww(w->manifest)) {
        snprintf(p, sizeof p, "%s/words.shrunk.txt", dir); hit = words_list(p, kw);
        if (!hit && (d = opendir(dir))) {                  /* the newer sets keep it in BDPGeneratedFiles/ */
            while (!hit && (e = readdir(d))) {
                if (e->d_name[0] == '.') continue;
                snprintf(p, sizeof p, "%s/%s/words.shrunk.txt", dir, e->d_name); hit = words_list(p, kw);
            }
            closedir(d);
        }
    }
    snprintf(wake_keyword[slot], sizeof wake_keyword[slot], "%s", hit ? kw : "");
    wake_on_miss(on_near_miss);
    int r = wake_open(slot, w->manifest, on_wake);
    wake_align_load(slot, w);
    return r;
}

static void on_wake(int slot, const char *keyword, uint64_t begin, uint64_t end)
{
    if (!core_local_wake) return;
    if (wake_keyword[slot][0] && strcasecmp(keyword, wake_keyword[slot]) && strcasecmp(keyword, "STOP")) {
        fprintf(stderr, "wake: %s is not the wake word picked (%s), ignored\n", keyword, wake_keyword[slot]);
        return;
    }
    /* In a Drop In the other side's voice plays here, and what the echo canceller leaves of its "Alexa" (meant for the
     * Echo over there) can be enough for ours.  While it talks, the wake word is left to that Echo */
    if (atomic_load(&dropin_live) && dropin_far_talking()) {
        fprintf(stderr, "wake: %s while the other side of the Drop In talks (%.1f dBFS here), ignored\n", keyword, dropin_far_level());
        return;
    }
    /* "stop" comes from the decoder that heard its wake word just before; another one on the same model set (one
     * multi-keyword en-US set in both slots) is awake too, and its "stop" would stop twice */
    if (!strcasecmp(keyword, "STOP")) { if (slot == atomic_load(&wake_slot_heard)) stop_word(); return; }
    wake_tell("detected");
    last_wake_ms = mono_ms();
    atomic_store(&wake_slot_heard, slot);
    { float db = 10 * log10f((ring_power(begin, end) + 1) / (32768.0f * 32768.0f));     /* the talker's level, for the gain */
      pthread_mutex_lock(&core_lock); keyword_db = db; heard_ms = last_wake_ms; kw_begin = begin; kw_end = end; kw_slot = slot; snprintf(wake_phrase, sizeof wake_phrase, "%s", wake_loaded[slot]); pthread_mutex_unlock(&core_lock); }
    if (!arb_running()) { trigger(0, end); return; }
    pthread_mutex_lock(&core_lock); det_begin = begin; det_end = end; pthread_mutex_unlock(&core_lock);
    atomic_store(&det_pending, 1);                  /* the ring is the capture thread's */
}

static int arb_send_key(const char *node, const char *network, const char *key)
{
    pthread_mutex_lock(&core_lock);
    int r = connected && proto->arb_send ? proto->arb_send(node, network, key) : -1;
    pthread_mutex_unlock(&core_lock);
    return r;
}

static void arb_notify(void) { pthread_mutex_lock(&core_lock); if (proto->arb_changed) proto->arb_changed(); pthread_mutex_unlock(&core_lock); }

void core_settings_changed(void) { if (connected && proto->settings_changed) proto->settings_changed(); }
int  core_ha_linked(void) { return connected; }
void core_entities_changed(void) { if (connected && proto->entities_changed) proto->entities_changed(); }

/* The settings page's login waits for the action button: the ring says so (an animation all models have) */
static void web_attention(int on) { led(on ? "-s" : "-u", "authenticated_setup_mode"); }
static void web_approved(int ok) { sound_queue(ok ? SND_BT_ON : SND_BT_OFF); }
static void improv_authorized(void) { sound_queue(SND_BT_ON); }      /* a phone's setup may go ahead: as a login */

static int arb_request(const char *entity)
{
    pthread_mutex_lock(&core_lock);
    int r = connected && proto->arb_request ? proto->arb_request(entity) : -1;
    pthread_mutex_unlock(&core_lock);
    return r;
}

static int arb_scan(const char *tag_id)
{
    pthread_mutex_lock(&core_lock);
    int r = connected && proto->arb_scan ? proto->arb_scan(tag_id) : -1;
    pthread_mutex_unlock(&core_lock);
    return r;
}

/* Arbitration pairing (Volume up + Volume down): a tap when it starts, the Bluetooth sounds for joined or not */
static void arb_paired(int result) { sound_queue(result == 1 ? SND_TOUCH : result == 2 ? SND_BT_ON : SND_BT_OFF); }
static void on_pair(void)
{
    /* the network only: whether this Echo then takes part in rounds stays the "arbitration" setting's */
    if (arb_pair() < 0) fprintf(stderr, "arbitration: not running, no pairing\n");
}

/* ---------------------------------------------------------------- playback queue */

struct item { struct item *next; int kind, media; unsigned rate, ch; size_t len; unsigned char data[]; };   /* media: Q_START */
enum { Q_START, Q_DATA, Q_STOP };
static void tts_kind(int media);

static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cond = PTHREAD_COND_INITIALIZER;
static struct item *q_head, *q_tail;
static size_t q_bytes;

static void q_push(int kind, int media, unsigned rate, unsigned ch, const void *data, size_t len)
{
    struct item *it = malloc(sizeof *it + len);
    if (!it) return;
    it->next = NULL; it->kind = kind; it->media = media; it->rate = rate; it->ch = ch; it->len = len;
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
            tts_kind(it->media);            /* the TTS stream's volume for what comes now */
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

void core_tts_begin(unsigned rate, unsigned channels, int media)
{
    atomic_store(&streaming, 0);
    mic_stopped();
    core_set_state(SPEAKING);
    q_push(Q_START, media, rate, channels, NULL, 0);
}

void core_tts_data(const void *pcm, size_t len) { if (!atomic_load(&flush_playback)) q_push(Q_DATA, 0, 0, 0, pcm, len); }
void core_tts_end(void) { q_push(Q_STOP, 0, 0, 0, NULL, 0); }
int  core_tts_flushing(void) { return atomic_load(&flush_playback); }
void core_tts_flush(void) { atomic_store(&flush_playback, 1); }

size_t core_tts_queued(void)
{
    pthread_mutex_lock(&q_lock); size_t n = q_bytes; pthread_mutex_unlock(&q_lock);
    return n;
}

/* ---------------------------------------------------------------- earcon, buttons */

static long long mono_ms(void);

/* Finished timers ring until the button, "<wake word>, stop", the media player's stop, Home Assistant's "Stop ringing" or
 * the ring time (setting timer_ring, 0 = until stopped).  Home Assistant keeps several timers per satellite and forgets each as it finishes
 * (intent/timers.py pops it before the finished event), so a cancel never names a ringing one: it is for a timer that
 * still runs, and must not silence the others.  One ring for all of them; each new one gives the ring its full time
 * again; one press stops them all (the Echo cannot ask which).  Home Assistant sees "Timer ringing" and their names. */
#define RINGING_MAX 4
static struct { char id[40], name[64]; } ringing[RINGING_MAX];  /* lock held: in the order they finished */
static int nringing, timer_ring_s = 60, timer_stop_any;         /* lock held */
static atomic_llong alarm_until;                                /* mono_ms the ring ends; LLONG_MAX: until stopped */
/* What rings (issue #63): stock's timer tone (the default, where the firmware has it: core_timer_sound_default), its
 * alarm tone, the built-in blip, or one uploaded on the page; set with the lock, read by the earcon thread.  The
 * page's "Play" (preview) rings it once, PREVIEW_MS at most */
static atomic_int timer_sound = TIMER_SOUND_BEEP, preview;
static atomic_llong preview_until;
#define PREVIEW_MS 10000
static const enum sound timer_sounds[] = { [TIMER_SOUND_TIMER] = SND_TIMER, [TIMER_SOUND_ALARM] = SND_ALARM, [TIMER_SOUND_CUSTOM] = SND_CUSTOM };

static void alarm_set(int on)          /* lock held */
{
    if (atomic_exchange(&alarm_on, on) == on) return;
    /* Stock's own (PuffinApp UXEventArbitrator::onAlertStateChange): "ready-timer" for timers and reminders from the
     * alert's start until it stops, layer 3, so listening and replies show over it and it comes back after them.
     * "active_timer" has a file but no entry in layer_config_common.json on any model: ledcontroller refuses it
     * ("not in map"), and the ring stayed dark */
    led(on ? "-s" : "-u", "ready-timer");
    playback_hint();
}

void core_timer_finished(const char *id, const char *name, unsigned total)
{
    int i = 0; unsigned h = total / 3600, m = total % 3600 / 60;
    while (i < nringing && strcmp(ringing[i].id, id)) i++;
    if (i == RINGING_MAX) { memmove(ringing, ringing + 1, sizeof ringing[0] * --i); nringing = i; }    /* the oldest goes */
    if (i == nringing) nringing++;
    snprintf(ringing[i].id, sizeof ringing[i].id, "%s", id);
    /* "set a timer for 5 minutes" has no name: its length stands in, so the sensor is never blank while it rings */
    if (name[0]) snprintf(ringing[i].name, sizeof ringing[i].name, "%s", name);
    else if (total % 60 || !total) snprintf(ringing[i].name, sizeof ringing[i].name, "%u s", total);
    else if (h && m) snprintf(ringing[i].name, sizeof ringing[i].name, "%u h %u min", h, m);
    else if (h) snprintf(ringing[i].name, sizeof ringing[i].name, "%u h", h);
    else snprintf(ringing[i].name, sizeof ringing[i].name, "%u min", m);
    atomic_store(&alarm_until, timer_ring_s ? mono_ms() + timer_ring_s * 1000LL : LLONG_MAX);
    fprintf(stderr, "alarm: %s rings (%d ringing)\n", ringing[i].name, nringing);
    alarm_set(1);
    if (connected && proto->timers_changed) proto->timers_changed();
}

void core_timer_cancelled(const char *id)
{
    for (int i = 0; i < nringing; i++) if (!strcmp(ringing[i].id, id)) {
        memmove(ringing + i, ringing + i + 1, sizeof ringing[0] * (size_t)(nringing - i - 1));
        if (!--nringing) alarm_set(0);
        if (connected && proto->timers_changed) proto->timers_changed();
        return;
    }
}

void core_alarm_stop(const char *why)
{
    if (!atomic_load(&alarm_on)) return;
    fprintf(stderr, "alarm: off (%s)\n", why);
    nringing = 0;
    alarm_set(0);
    if (connected && proto->timers_changed) proto->timers_changed();
}

/* "<wake word>, stop" with an engine that has no "stop" keyword (microWakeWord, Home Assistant's): the transcript is the
 * word alone.  Home Assistant has nothing to stop a ringing satellite timer with (it forgot the timer when it
 * finished), so the run is cancelled here, as the button does, before Home Assistant answers it.  Music plays on
 * without a ring: paused here as stop_word does (issue #51); after the keyword paused it, the transcript is that same
 * "stop" again, and Home Assistant would answer it (nothing plays any more, or it pauses another player).  "Stopp den Timer"
 * went to Home Assistant, which answered "no timer is running" while it rang on (issue #21): with timer_stop_any any
 * recognised speech stops the ring, as EchoMuse does, whatever the language and however the ring garbled it; then a
 * question during a ring is not answered */
int core_transcript(const char *text)
{
    static const char *const stops[] = { "stop", "stopp", "stoppen", "stoppa", "halt", "arrête", "arrete", "basta", "para", "pare" };
    /* a Drop In ends on these (and on the stops): Home Assistant has no intent for it, and the run is cancelled */
    static const char *const hangups[] = { "hangup", "endcall", "endthecall", "enddropin", "auflegen", "legauf", "beenden", "anrufbeenden",
                                           "dropinbeenden", "gesprächbeenden", "raccroche", "raccrocher", "cuelga", "colgar",
                                           "riattacca", "riaggancia", "ophangen", "hangop", "legop" };     /* the blueprint's languages */
    char w[32], k[32], kw[64]; size_t n = 0, m = 0;
    for (const char *p = text; *p && n < sizeof w - 1; p++)        /* lower case, without spaces and punctuation */
        if (!strchr(" \t.,!?¡¿\"'", *p)) w[n++] = (char)tolower((unsigned char)*p);
    w[n] = 0;
    wake_phrase_bare(kw, sizeof kw);                                /* "Echo", not "Echo (de-DE)" */
    for (const char *p = kw; *p && m < sizeof k - 1; p++)          /* "Alexa, stop" with the wake word sent too */
        if (!strchr(" \t.,!?¡¿\"'", *p)) k[m++] = (char)tolower((unsigned char)*p);
    k[m] = 0;
    if (m && n > m && !strncmp(w, k, m)) memmove(w, w + m, n - m + 1);
    /* The wake word sent too and nothing after it: without the word Home Assistant would have heard nothing and ended
     * quietly (core_no_speech).  So here: not speech that stops a ring (timer_stop_any), nor a request for the agent */
    if (replay_whole && m && n == m && !strcmp(w, k)) {
        fprintf(stderr, "transcript: the wake word alone, as nothing said\n");
        if (run_wake) wake_told("no_speech");
        if (connected && proto->cancel) quiet_cancel(); else core_pipeline_finish();
        return 1;
    }
    if (dropin_running() && dropin_status(NULL, 0) != DROPIN_IDLE) {
        int hit = 0;
        for (size_t i = 0; i < sizeof stops / sizeof stops[0]; i++) hit |= !strcmp(w, stops[i]);
        for (size_t i = 0; i < sizeof hangups / sizeof hangups[0]; i++) hit |= !strcmp(w, hangups[i]);
        if (hit) {
            dropin_hangup("said hang up");
            if (connected && proto->cancel) { barge_in = 0; atomic_store(&streaming, 0); proto->cancel(); quiet_abort = 1; }
            core_pipeline_finish();
            return 1;
        }
    }
    int hit = 0;
    for (size_t i = 0; i < sizeof stops / sizeof stops[0]; i++) hit |= !strcmp(w, stops[i]);
    if (atomic_load(&alarm_on)) {
        if (!hit && !(timer_stop_any && w[0])) return 0;
        core_alarm_stop(hit ? "said stop" : "a request during the ring");
    } else {
        /* 10 s: the keyword's pipeline ends at Home Assistant's end of speech, a few seconds on */
        if (!hit || !(music_stop() || (music_stopped_ms && mono_ms() - music_stopped_ms < 10000))) return 0;
        fprintf(stderr, "said stop: music paused, run cancelled\n");
    }
    if (connected && proto->cancel) { barge_in = 0; atomic_store(&streaming, 0); proto->cancel(); quiet_abort = 1; }
    core_pipeline_finish();
    return 1;
}

int core_timers_ringing(char *names, size_t cap)
{
    size_t n = 0;
    if (cap) names[0] = 0;
    for (int i = 0; i < nringing && n < cap; i++) n += (size_t)snprintf(names + n, cap - n, "%s%s", i ? ", " : "", ringing[i].name);
    return nringing;
}

int core_timer_ring(int set)
{
    if (set >= 0) timer_ring_s = set;
    return timer_ring_s;
}

int core_timer_sound(int set)
{
    if (set >= 0 && set <= TIMER_SOUND_CUSTOM && (set == TIMER_SOUND_BEEP || sound_present(timer_sounds[set]))) atomic_store(&timer_sound, set);
    return atomic_load(&timer_sound);
}

void core_timer_sound_default(void) { if (core_timer_sound(TIMER_SOUND_TIMER) != TIMER_SOUND_TIMER) core_timer_sound(TIMER_SOUND_BEEP); }

void core_timer_sound_preview(void) { atomic_store(&preview, 1); }

int core_timer_stop_any(int set)
{
    if (set >= 0) timer_stop_any = set != 0;
    return timer_stop_any;
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
    if (!on && was & source && !atomic_load(&music_on)) atomic_store(&own_ended, mono_ms());   /* media in the room: the hold */
    int is = atomic_load(&music_on);
    if (!was != !is) {
        media_mark("own playback", is ? "on" : "off", 0, is ? "own playback off -> on" : "own playback on -> off");
        playback_hint();
    }
}

#define SOUND_TO_MIC (CAP_RATE * 85 / 1000)         /* speaker to mic stream (2026-09-30) */
static void sound_start(void)                       /* earcon thread: a sound goes to the mixer */
{
    long long from = (long long)(cap_clock() + SOUND_TO_MIC);
    sound_taken = from <= atomic_load(&sound_spans[sound_span].to);     /* its span goes on: from stays */
    if (!sound_taken) {
        sound_span = (sound_span + 1) % (int)(sizeof sound_spans / sizeof *sound_spans);
        atomic_store(&sound_spans[sound_span].to, 0);   /* empty first: sound_in never pairs the old from with to below */
        atomic_store(&sound_spans[sound_span].from, from);
    }
    atomic_store(&sound_spans[sound_span].to, LLONG_MAX);
}
static void sound_over(void)                        /* earcon thread: it played; the room's tail goes on for a while */
{
    atomic_store(&sound_spans[sound_span].to, (long long)(cap_clock() + CAP_RATE / 4));
}

/* the earcon thread: the mixer took a sound, which plays after lead_ms of silence (audio.h) */
static void earcon_started(unsigned lead_ms)
{
    uint64_t now = cap_clock() + (uint64_t)lead_ms * CAP_RATE / 1000;
    /* a ring's or a timer's tone comes here too: no span; the preview's three blips one span: from the first */
    if (atomic_load(&sound_spans[sound_span].to) == LLONG_MAX && !sound_taken) {
        sound_taken = 1;
        atomic_store(&sound_spans[sound_span].from, (long long)(now + SOUND_TO_MIC));
    }
    earsup_started(now);
}

/* Our sounds and timers at their own volume (separate volumes): the Earcon stream plays at MainVolume, the loudest
 * of music, sounds and timers, so the quieter ones are turned down here (mix_apply).  A copy to free, or NULL to play
 * them as they are */
static short *scaled(const short *pcm, size_t n, int q16)
{
    short *out;
    if (q16 >= 65536 || !(out = malloc(n * sizeof *out))) return NULL;      /* no memory: it plays unscaled, loud */
    for (size_t i = 0; i < n; i++) out[i] = (short)((pcm[i] * q16 + 32768) >> 16);      /* rounded, as audio_mixer.c */
    return out;
}

static int ring_sound;
static int ring_go(void) { return atomic_load(&dropin_ring) == ring_sound && !atomic_load(&sounds_pending); }
/* a timer's sound plays on until it is stopped or rings out, and gives way to a pipeline (speech to text must hear the
 * user, not the ring), our other sounds and a Drop In's ring; then it starts again from the top */
static int alarm_go(void)
{
    return atomic_load(&alarm_on) && mono_ms() <= atomic_load(&alarm_until) && !atomic_load(&busy) && !atomic_load(&sounds_pending)
           && atomic_load(&dropin_ring) < 0;
}
static int preview_go(void)
{
    return mono_ms() < atomic_load(&preview_until) && !atomic_load(&alarm_on) && !atomic_load(&busy) && !atomic_load(&sounds_pending)
           && atomic_load(&dropin_ring) < 0;
}

/* The ring's blip as loud as Alexa's timer tone: BS.1770 loudness of its cycle (three blips, 0.8 s of silence) at a peak
 * of 6000 is -20.8 LUFS, the tone's -16.6 (donut; -16.2 on the Shows), so the ring takes 4.3 dB more (issue #63).  The
 * same blip standing in for the wake and touch sounds keeps 6000 */
#define BEEP_PEAK 9850

static void *earcon_thread(void *arg)
{
    enum { RATE = 48000, N = RATE * 12 / 100 };
    static short tone[N], beep[N];
    static const char *const snd_names[SND_COUNT] = { "wake", "touch", "mics off", "mics on", "volume", "bluetooth connected",
                                                                "bluetooth disconnected", "identify", "drop in", "call connected",
                                                                "call ended", "incoming call", "outgoing call", "timer", "alarm",
                                                                "own timer sound" };
    (void)arg;
    for (int i = 0; i < N; i++) {           /* 120 ms rising two-tone blip with 10 ms fades */
        double f = i < N / 2 ? 880.0 : 1320.0, env = fmin(1.0, fmin(i, N - i) / (RATE * 0.01));
        tone[i] = (short)(6000 * env * sin(2 * M_PI * f * i / RATE));
        beep[i] = (short)(BEEP_PEAK * env * sin(2 * M_PI * f * i / RATE));
    }
    for (;;) {
        /* Amazon's own sounds where the image has them; the generated blip stands in for the wake and touch sounds otherwise */
        for (int p = atomic_exchange(&sounds_pending, 0), s = 0; p && s < SND_COUNT; s++) {
            const short *pcm; size_t n; unsigned rate;
            if (!(p & 1 << s)) continue;
            atomic_store(&earcon_sounding, 1);
            if (sound_get((enum sound)s, &pcm, &n, &rate)) fprintf(stderr, "sound: %s\n", snd_names[s]);
            else if (s == SND_WAKE || s == SND_TOUCH || s == SND_IDENTIFY) { pcm = tone; n = N; rate = RATE; }
            else pcm = NULL;
            if (pcm) {
                short *quiet = scaled(pcm, n, atomic_load(s == SND_VOLUME ? &click_q16 : &sounds_q16));
                if (quiet) pcm = quiet;
                sound_start();
                earsup_play(pcm, n, rate, cap_clock()); play_earcon(pcm, n, rate);
                sound_over();
                free(quiet);
            }
            atomic_store(&earcon_heard_until, mono_ms() + 250);         /* speaker to mic stream: 85 ms, and the room's tail */
            atomic_store(&earcon_sounding, 0);
        }
        { int r = atomic_load(&dropin_ring); const short *pcm; size_t n; unsigned rate;     /* a Drop In rings: cut off when answered */
          if (r >= 0 && mono_ms() >= atomic_load(&dropin_ring_at) && sound_get((enum sound)r, &pcm, &n, &rate)) {
              short *quiet = scaled(pcm, n, atomic_load(&sounds_q16));     /* one of our sounds: at their volume */
              ring_sound = r; play_earcon_while(quiet ? quiet : pcm, n, rate, ring_go); free(quiet); usleep(300000); continue; } }
        int pv = atomic_exchange(&preview, 0) && !atomic_load(&alarm_on);   /* none during a ring, nor after it */
        if (atomic_load(&alarm_on) || pv) {             /* timer finished: its sound, or a triple blip every 1.2 s, until the ring time */
            if (!pv && mono_ms() > atomic_load(&alarm_until)) {
                pthread_mutex_lock(&core_lock);
                if (mono_ms() > atomic_load(&alarm_until)) core_alarm_stop("rang out");     /* not a timer that just came */
                pthread_mutex_unlock(&core_lock);
            } else if (!atomic_load(&busy)) {
                int ts = atomic_load(&timer_sound); const short *pcm = beep; size_t n = N; unsigned rate = RATE;
                if (ts != TIMER_SOUND_BEEP && !sound_get(timer_sounds[ts], &pcm, &n, &rate)) { pcm = beep; n = N; rate = RATE; }  /* gone: the blip */
                short *quiet = scaled(pcm, n, atomic_load(&timers_q16));
                if (pv) { atomic_store(&preview_until, mono_ms() + PREVIEW_MS); atomic_store(&earcon_sounding, 1); sound_start(); }
                if (pcm != beep) play_earcon_while(quiet ? quiet : pcm, n, rate, pv ? preview_go : alarm_go);
                else for (int k = 0; k < 3; k++) play_earcon(quiet ? quiet : beep, N, RATE);
                free(quiet);
                if (pv) { sound_over(); atomic_store(&earcon_heard_until, mono_ms() + 250); atomic_store(&earcon_sounding, 0); }
                else if (pcm == beep) usleep(800000);
            }
            /* in the background while a pipeline runs: speech to text must hear the user, not the ring */
        }
        usleep(20000);
    }
    return NULL;
}

/* The buttons in Home Assistant (issue #26): one event entity, "Button", told of every press and hold beside what the
 * button does on the Echo, so an automation can stop its own alarm clock with the action button or skip a track with
 * a volume key held.  Holds at 1 s, well before acebuttond's 5 s and the reset at 10 s (buttons.c) */
const char *const core_button_events[] = { "action", "action_hold", "volume_up", "volume_up_hold", "volume_down", "volume_down_hold" };
const int core_button_nevents = sizeof core_button_events / sizeof core_button_events[0];

static void button_event(int key, int hold)     /* any thread but the lock's holder */
{
    const char *e = core_button_events[key * 2 + !!hold];
    fprintf(stderr, "button: %s\n", e);
    pthread_mutex_lock(&core_lock);
    if (connected && proto->button) proto->button(e);
    pthread_mutex_unlock(&core_lock);
}

/* Action button: cancel a running pipeline; else pause what plays (Bluetooth device first, then Sendspin), or resume
 * what the button paused; else talk.  Home Assistant hears of it, but not of a press that approved a login or a phone:
 * an automation on the button must not run whenever someone lets in a browser */
static void on_action(void)
{
    if (web_approve()) return;                          /* a login of the settings page waited for this press */
    if (improv_authorize()) return;                     /* a phone setting up the network did */
    button_event(BUTTON_ACTION, 0);
    if (dropin_running() && dropin_button()) return;    /* a Drop In rang (answered) or ran (ended) */
    pthread_mutex_lock(&core_lock);
    int busy = state == LISTENING || state == THINKING;
    pthread_mutex_unlock(&core_lock);
    if (!busy && (a2dp_button(0) || (core_sendspin_port && sendspin_button()) || a2dp_button(1))) return;
    atomic_store(&trigger_pending, 2);                  /* 2: touch */
}

/* Action button held: at 5 s the ring warns, at 10 s everything is forgotten (core_reset).  acebuttond still sees the
 * button: its 5 s hold starts Amazon's setup, whose services are stopped (nothing happens, checked: PLAN.md), and its
 * 21 s hold is Amazon's factory reset, which the reset here comes long before. */
static void on_hold(int stage)
{
    if (stage == 1) { led("-s", "factory-reset"); fprintf(stderr, "action button held 5 s: reset at 10 s\n"); }
    else if (stage == 0) { led("-u", "factory-reset"); fprintf(stderr, "action button let go: no reset\n"); }
    else core_reset("action button held 10 s");
}

static void mute_latch(int muted, int sound)
{
    static int last = 0;
    fprintf(stderr, "mic mute button: %d\n", muted);
    pthread_mutex_lock(&core_lock);
    int was = last || soft_mute; last = muted;
    if (!muted && soft_mute) { soft_mute = 0; fprintf(stderr, "soft mute: 0 (released with the button)\n"); }
    mute_update(was, sound);
    pthread_mutex_unlock(&core_lock);
}

static void on_mute(int muted) { mute_latch(muted, 1); }       /* hardware latch changed (button) */

/* Volume: 10 % per press like stock (3 of the ring's 30 steps on donut, board.volume_steps).  The volume_step-NN animations show 2 s and then loop
 * black forever, so the previous one has to be unset or they pile up in ledcontroller; a timer clears the last one. */
static int volume = -1;                      /* 0..100, read from the device on first use */
static char vol_pat[32];
static atomic_llong vol_clear_at;

static long long mono_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int read_volume(void) { return vol_read(VOL_MAIN, 40); }

/* The backend keeps one volume per kind of stream (audio.h).  VOL_MAIN is what the stock volume keys move and covers
 * music and sounds; the assistant's replies follow VOL_TTS alone.  One knob for the user: both, the loudest of the
 * four while the volumes are separate (mix_target, mix_apply).
 *
 * Separate volumes (setting separate_volumes, issue #32): off, one volume for everything, as stock.  On, four of
 * their own, 0..100 each: music (`volume`: what the media player, Sendspin and the keys show), the assistant's replies
 * and announcements, our sounds (wake, buttons, Bluetooth, Drop In's chimes), timers and Drop In calls (the far end).  What the Echo does with a
 * volume (Echo Dot 2, docs/re-platform.md): the mixer has MainVolume for the Music, Earcon and Voip streams, TTSVolume
 * for TTS and AlarmVolume for Alarm, and Amazon's speaker processing (libasp) adds up to 14.5 dB to everything once its
 * own volume is 90 or more, which is whichever of MainVolume and TTSVolume was set last (AlarmVolume never moves it:
 * the Alarm stream topped out 12 dB under the others).  So both mixer volumes stay at the loudest of the four, where
 * libasp agrees with them, and each kind is turned down in our own PCM by what the Echo's curve puts between its volume
 * and that one (vol_db): 30 sounds the same on every slider, and as 30 on a stock Echo.  Music, the phone's Bluetooth
 * audio and Drop In through the backend (audio_music_gain), replies too (audio_tts_gain), sounds and timers in the
 * earcon thread.  Home Assistant's media (media_player.play_media) plays on the TTS stream, but is music (tts_kind).
 * A Bluetooth speaker with absolute volume takes the loudest's place: the mixer plays at full scale, the speaker is set
 * to the loudest (a2dp_speaker_volume), the rest turned down as on the Echo's speaker (the Echo's curve standing in for
 * the speaker's, which no one measured), and its own buttons move the music (core_speaker_volume).
 * Replies, sounds, timers and calls each have a volume of their own or follow the music's, some points above or below
 * it (music + offset, clamped to 0..100: a volume like the others, so at offset 0 the music's own loudness).  Calls
 * follow it by default, as they did before they had a volume (the Voip stream follows MainVolume; its curve is taken
 * to be the Music stream's, docs/re-platform.md). */
static int vol_split;                        /* under lock */
static struct { int level, linked, offset; } kinds[VOL_KINDS] = { { 40, 0, 0 }, { 40, 0, 0 }, { 40, 0, 0 }, { 40, 1, 0 } };   /* under lock */
static int tts_media;                        /* under lock: the TTS stream plays Home Assistant's media */
static int tts_written = -1, main_written = -1;     /* under lock: TTSVolume and MainVolume as we last set them */
static int speaker_mode;                     /* under lock: SPEAKER_* */
static int echo_own_volume;                  /* under lock: the Echo's volume, set aside while speaker_mode */

static void volume_led(int v, int assistant);
static int clamp100(int v) { return v < 0 ? 0 : v > 100 ? 100 : v; }
static int clamp_offset(int v) { return v < -VOL_OFFSET_MAX ? -VOL_OFFSET_MAX : v > VOL_OFFSET_MAX ? VOL_OFFSET_MAX : v; }

static float vol_db(int v) { return board_vol_db(v); }      /* the Echo's volume curve (volcurve.c) */

/* lock held: what a kind (VOL_*) plays at now, its own volume or the music's moved by its offset */
static int kind_volume(int k) { return kinds[k].linked ? clamp100(core_volume() + kinds[k].offset) : kinds[k].level; }

/* lock held: what everything is turned down from: the volume, or the loudest kind */
static int loudest(void)
{
    int v = core_volume();
    if (!vol_split) return v;
    for (int k = 0; k < VOL_KINDS; k++)       /* a call's volume only while one is on: no hidden loud one moves the rest */
        if ((k != VOL_CALLS || atomic_load(&dropin_live)) && kind_volume(k) > v) v = kind_volume(k);
    return v;
}

/* lock held: what MainVolume and TTSVolume both are now: the loudest, or full scale for a speaker that sets the volume */
static int mix_target(void) { return speaker_mode == SPEAKER_ABSOLUTE ? 100 : loudest(); }

/* lock held: what audio for volume v is multiplied by: 1 at the mixer's own, 0 at volume 0 */
static float mix_gain(int v)
{
    if (!vol_split) return 1;
    return v <= 0 ? 0 : powf(10.0f, (vol_db(v) - vol_db(loudest())) / 20.0f);
}

static void mix_apply(void)                  /* lock held: the mixer's volumes and our gains, as they should be now */
{
    int v = mix_target();
    if (v != main_written) { vol_write(VOL_MAIN, v); main_written = v; }
    if (v != tts_written) { vol_write(VOL_TTS, v); tts_written = v; }
    static int spk_was = -1;
    int spk = speaker_mode == SPEAKER_ABSOLUTE ? loudest() : -1;
    if (spk >= 0 && spk != spk_was) fprintf(stderr, "volume: the speaker at %d\n", spk);
    spk_was = spk;
    a2dp_speaker_volume(loudest());
    audio_music_gain(mix_gain(core_volume()));
    audio_tts_gain(mix_gain(tts_media ? core_volume() : kind_volume(VOL_ASSISTANT)));
    atomic_store(&sounds_q16, (int)lroundf(mix_gain(kind_volume(VOL_SOUNDS)) * 65536));
    atomic_store(&timers_q16, (int)lroundf(mix_gain(kind_volume(VOL_TIMERS)) * 65536));
    audio_call_gain(mix_gain(kind_volume(VOL_CALLS)));
}

static void tts_kind(int media)              /* the playback thread, at each start of the TTS stream */
{
    pthread_mutex_lock(&core_lock);
    tts_media = media;
    mix_apply();
    pthread_mutex_unlock(&core_lock);
}

/* The music volume while the volumes are separate: MainVolume is then the loudest of three and says nothing about it,
 * so it is kept here (and read at start instead of MainVolume) */
static const char *music_volume_path(void)
{
    static char p[256]; const char *d = getenv("HASSMIC_STATE");
    snprintf(p, sizeof p, "%s/volume.music", d ? d : "/data/local/hassmic/state");
    return p;
}
static int music_dirty;                      /* under lock: volume.music to be written (music_volume_write) */
static void music_volume_keep(void)          /* lock held */
{
    music_dirty = vol_split;
    if (!vol_split) unlink(music_volume_path());
}

/* volume_sync's thread, outside the lock: a write with its fsyncs takes tens of ms on the flash, and the volume moves
 * in bursts (keys held, Home Assistant's slider, Sendspin); so at most once a second, the last value */
static void music_volume_write(void)
{
    char t[8];
    pthread_mutex_lock(&core_lock);
    int v = speaker_mode ? echo_own_volume : volume, go = music_dirty;      /* a speaker that came meanwhile */
    music_dirty = 0;
    pthread_mutex_unlock(&core_lock);
    if (!go) return;
    snprintf(t, sizeof t, "%d\n", v);
    statefile_write(music_volume_path(), 0644, t, strlen(t));
    pthread_mutex_lock(&core_lock);
    if (!vol_split) unlink(music_volume_path());        /* switched off meanwhile */
    pthread_mutex_unlock(&core_lock);
}

int core_separate_volumes(int set)
{
    if (set >= 0 && (set != 0) != vol_split) {
        /* on: each starts where everything was on the Echo's own speaker, so nothing jumps (levels after it, from
         * state/config or an import, then win: settings.c lists them later) */
        if (set) for (int k = 0; k < VOL_KINDS; k++) kinds[k].level = speaker_mode ? echo_own_volume : core_volume();
        vol_split = set != 0;
        if (!vol_split || speaker_mode == SPEAKER_NONE) music_volume_keep();    /* not a Bluetooth speaker's: core_speaker */
        mix_apply();
    }
    return vol_split;
}
int core_kind_volume(int k, int set) { if (set >= 0) { kinds[k].level = clamp100(set); mix_apply(); } return kinds[k].level; }
int core_kind_linked(int k, int set)
{
    if (set >= 0 && (set != 0) != kinds[k].linked) {
        /* nothing jumps: linked, the offset puts it where it was (the offset spans -100..100, any level from any
         * music volume); apart, its own volume is where it played (a saved or imported offset or volume after it then
         * wins: settings.c lists them later) */
        int music = speaker_mode ? echo_own_volume : core_volume();         /* never a Bluetooth speaker's */
        if (set) kinds[k].offset = clamp_offset(kinds[k].level - music);
        else kinds[k].level = clamp100(music + kinds[k].offset);
        kinds[k].linked = set != 0;
        mix_apply();
    }
    return kinds[k].linked;
}
int core_kind_offset(int k) { return kinds[k].offset; }
void core_set_kind_offset(int k, int offset) { kinds[k].offset = clamp_offset(offset); mix_apply(); }

/* A speaker with absolute volume turned by its own buttons: it is at the loudest kind, so the music moves by as much
 * (separate volumes; else that is the volume) */
void core_speaker_volume(int pct)
{
    int v = vol_split ? core_volume() + pct - loudest() : pct;
    fprintf(stderr, "volume: the speaker's own buttons, %d\n", pct);
    core_set_volume(v);
    if (vol_split && loudest() != pct) fprintf(stderr, "volume: the speaker back at %d, the loudest\n", loudest());
}

/* lock held: a kind's own volume from the keys, a step as the main one's; the ring shows it (replies in cyan) */
static void level_step(int k, int dir)
{
    static const char *const what[VOL_KINDS] = { "assistant", "sounds", "timers", "calls" };
    int v = kinds[k].level = clamp100((kinds[k].level + 5) / 10 * 10 + dir * 10);
    mix_apply();
    volume_led(v, k == VOL_ASSISTANT);
    fprintf(stderr, "volume: %s %d\n", what[k], v);
    settings_save();
    core_settings_changed();
}

/* Playing to a Bluetooth speaker (a2dp.c, btout.c) it has a volume of its own: what the buttons and Home Assistant move
 * meanwhile, starting from the speaker's when it tells (AVRCP absolute volume); the Echo's own comes back afterwards.
 * With absolute volume the speaker applies it and the mixer plays at full scale, so SBC gets the whole signal (at volume
 * 30 the mixer's curve, made for the Echo's small speaker, put it 38 dB down); without, the mixer applies it as before.
 * The Echo's own volume is kept in a file meanwhile, so that a hassmic that dies on the speaker does not take what the
 * mixer then says for it and play the Echo's speaker at the Bluetooth speaker's level, or at full scale. */
static const char *own_volume_path(void)
{
    static char p[256]; const char *d = getenv("HASSMIC_STATE");
    snprintf(p, sizeof p, "%s/volume.speaker", d ? d : "/data/local/hassmic/state");
    return p;
}

int core_volume(void)
{
    if (volume < 0) {                        /* whatever it was left at: in line now */
        /* volume.music only while state/config says the volumes are separate (this may run before settings_load): a
         * hand edit or an older build (a fallback after an update) leaves the file behind with the setting gone, and
         * the volume moved since is MainVolume's */
        char sv[8]; int split = !settings_peek("separate_volumes", sv, sizeof sv) && !strcmp(sv, "on");
        FILE *f = fopen(own_volume_path(), "r"), *m = split ? fopen(music_volume_path(), "r") : NULL; int v;
        if (!split && !unlink(music_volume_path())) fprintf(stderr, "volume: state/volume.music left over, the volumes are not separate\n");
        if (f && fscanf(f, "%d", &v) == 1 && v >= 0 && v <= 100) {
            volume = v;
            fprintf(stderr, "volume: %d (the Echo's own, set aside for a Bluetooth speaker)\n", volume);
        } else if (m && fscanf(m, "%d", &v) == 1 && v >= 0 && v <= 100) volume = v;     /* separate volumes */
        else volume = read_volume();
        if (f) fclose(f);
        if (m) fclose(m);
        unlink(own_volume_path());
        mix_apply();
    }
    return volume;
}

void core_speaker(int mode, int pct)
{
    pthread_mutex_lock(&core_lock);
    int was = core_volume();
    if (mode == speaker_mode) { pthread_mutex_unlock(&core_lock); return; }
    if (!speaker_mode) {
        echo_own_volume = was;
        FILE *f = fopen(own_volume_path(), "w"); if (f) { fprintf(f, "%d\n", was); fclose(f); }
    }
    if (mode == SPEAKER_NONE) { volume = echo_own_volume; if (vol_split) music_volume_keep(); }
    else if (mode == SPEAKER_ABSOLUTE) volume = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    speaker_mode = mode;
    mix_apply();
    int mix = mix_target();
    if (mode == SPEAKER_NONE) unlink(own_volume_path());
    fprintf(stderr, "volume: %d, mixer at %d (%s)\n", volume, mix, mode == SPEAKER_NONE ? "the Echo's speaker" :
            mode == SPEAKER_ABSOLUTE ? "a Bluetooth speaker sets the volume" : "a Bluetooth speaker, the mixer sets the volume");
    if (volume != was) {
        if (connected && proto->volume_changed) proto->volume_changed(volume);
        if (core_sendspin_port) sendspin_volume_changed(volume);
        a2dp_volume_changed(volume);
    }
    pthread_mutex_unlock(&core_lock);
}

/* The ring shows the level for 2 s: stock's white steps, the Assistant's own volume in cyan where root installed ours
 * (ring.c; stock's otherwise, the two then look alike) */
static void volume_led(int v, int assistant)
{
    char pat[32]; int step = v * board.volume_steps / 100 ? v * board.volume_steps / 100 : 1;
    if (assistant && ring_assistant_steps()) snprintf(pat, sizeof pat, "hassmic_assist_%02d", step);
    else snprintf(pat, sizeof pat, "volume_step-%02d", step);
    if (vol_pat[0] && strcmp(vol_pat, pat)) led("-u", vol_pat);
    led("-s", pat);
    strcpy(vol_pat, pat);
    atomic_store(&vol_clear_at, mono_ms() + 2500);
}

void core_set_volume(int v)
{
    volume = clamp100(v);
    if (vol_split && speaker_mode == SPEAKER_NONE) music_volume_keep();
    mix_apply();
    volume_led(volume, 0);
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
        char out[1024] = "", key[32], *s; int x;
        afe_eq_get(out, sizeof out);                /* {"bands":[{"name":"BASS","level":0},{"name":"MIDRANGE",... */
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
    afe_eq_set(json);
    fprintf(stderr, "equalizer: bass %d, mid %d, treble %d\n", eq[0], eq[1], eq[2]);
}

static atomic_int led_auto = 1, led_level = 80;     /* 80: ledcontroller's first-boot level; read without core_lock
                                                     * (core_led_brightness(-1): the band's thread, the protocols) */

/* Root installed the ring's set again (designs applied on the settings page) and restarted ledcontroller, which starts
 * with auto brightness on: a fixed level once more, and Home Assistant's light listed anew if it came or went */
static void ring_reloaded(int custom_changed)
{
    pthread_mutex_lock(&core_lock);
    if (!led_auto) core_led_brightness(led_level);
    if (custom_changed) core_entities_changed();
    pthread_mutex_unlock(&core_lock);
}

/* LED brightness.  Stock's auto brightness is ledcontroller's, not the Alexa client's, and it starts it by itself at boot
 * (Echo Dot 2 set to 50 with auto off and rebooted: 9 again, for 40 lux; 2026-10-01).  It polls the light sensor through
 * the same HAL file as core_lux() (1 Hz when settled, 20 Hz while moving), smooths it over 3 s and maps 0..400 lux on a
 * straight line to 0..100 (donut: 0.26 per lux - 4, at least 0), ramping there in 3 s.  So "auto" here is ledcontroller
 * left alone, and a fixed level is ledctrl -a off -b N in one call: two calls could land in either order, and the running
 * engine would overwrite a level that came first.  Neither ledcontroller nor anything stock keeps the auto flag: our
 * settings file does (proto_esphome.c), and it is applied again at every start.  ledcontroller writes each level it shows
 * to persist.ledbrightness.bootup (auto steps too) and restores it at boot, so that property is what the ring shows. */

int core_led_auto(int set)
{
    if (set >= 0 && set != led_auto) {
        if (!set) led_level = core_led_brightness(-1);
        led_auto = set;
        if (use_led) ledq_run("-a", set ? "on" : "off");        /* off: the level stays where auto left it */
        fprintf(stderr, "LED brightness: %s\n", set ? "auto" : "fixed");
    }
    return led_auto;
}

int core_led_brightness(int set)
{
    if (set >= 0) {
        char n[8]; snprintf(n, sizeof n, "%d", set > 100 ? 100 : set);
        const char *args[] = { "-a", "off", "-b", n };
        if (use_led) ledq_args(4, args);            /* queued with the ring's: levels ms apart land in the order sent */
        led_auto = 0; led_level = atoi(n);
        fprintf(stderr, "LED brightness: fixed at %d\n", led_level);
    }
#ifdef __ANDROID__
    char v[PROP_VALUE_MAX] = "";                   /* fixed: ours, the property may not have it yet (ledctrl queued) */
    if (led_auto && use_led && __system_property_get("persist.ledbrightness.bootup", v) > 0) return atoi(v);
#endif
    return led_level;
}

/* A TCS3400 whose driver gives raw channels only (athena: its own `als_lux` stays 0, the table behind it unset; stock's
 * virtual_als turns red/green/blue/clear into a level itself, lux.h does the same).  DIR holds `input*` directories; the
 * one named tcs3400_als has als_red/green/blue/clear, als_itime (ms) and als_gain. */
static float lux_tcs3400(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    float lux = NAN;
    while (d && (e = readdir(d))) {
        char p[320], n[32] = ""; FILE *f;
        if (strncmp(e->d_name, "input", 5)) continue;
        snprintf(p, sizeof p, "%s/%s/name", dir, e->d_name);
        if ((f = fopen(p, "r"))) { if (fgets(n, sizeof n, f)) n[strcspn(n, "\n")] = 0; fclose(f); }
        if (strcmp(n, "tcs3400_als")) continue;
        static const char *const ch[] = { "als_red", "als_green", "als_blue", "als_clear", "als_itime", "als_gain" };
        double v[6]; int ok = 1;
        for (int i = 0; i < 6 && ok; i++) {
            snprintf(p, sizeof p, "%s/%s/%s", dir, e->d_name, ch[i]);
            ok = (f = fopen(p, "r")) && fscanf(f, "%lf", &v[i]) == 1;       /* "64 (auto)": the number first */
            if (f) fclose(f);
        }
        if (!ok) break;
        lux = lux_tcs3400_calc(v[0], v[1], v[2], v[3], v[4], v[5]);
        if (lux < 0) lux = NAN;
        break;
    }
    if (d) closedir(d);
    return lux;
}

/* The light sensor as stock's HAL (libacehal_ambientLightSensor.so, its per-model "facade") reads it: a sysfs file the
 * kernel driver fills with calibrated lux, parsed with atof.  0..400 is all stock uses of it. */
float core_lux(void)
{
    const char *e = getenv("HASSMIC_LUX");         /* tests */
    const char *const *p = e ? (const char *const[]){ e, NULL } : board.light_sensor;
    for (; p && *p; p++) {
        char buf[32]; int fd; ssize_t n;
        if (!strncmp(*p, "tcs3400:", 8)) { float l = lux_tcs3400(*p + 8); if (!isnan(l)) return l; continue; }
        fd = open(*p, O_RDONLY);
        if (fd < 0) continue;
        n = read(fd, buf, sizeof buf - 1); close(fd);
        if (n <= 0) continue;
        buf[n] = 0;
        return (float)atof(buf);
    }
    return NAN;
}

/* Auto brightness is stock's engine (ledcontroller, above), which only the LED ring has: the Echo Show's band
 * (ring_band.c) has a light sensor but nothing that follows it, so the switch is not offered there. */
int core_led_has_auto(void)
{
    return ring_band_rows(-1) < 0 && !isnan(core_lux());
}

/* Anything may move MainVolume behind our back (audio_manager_set_prop, a stock daemon, the stock keys when -V), and
 * TTSVolume does not follow by itself: replies would then play at the old volume.  Poll both every 2 s: a changed
 * MainVolume is the user's wish and is adopted (Home Assistant and Music Assistant are told, no LED), a strayed TTSVolume
 * is pulled back to what it should be (mix_target); while the volumes are separate MainVolume is not the volume and is
 * pulled back too.
 * The mixer's global Mute silences every stream whatever the volumes say, and it persists across reboots: stock Alexa
 * ("Alexa, mute") can leave it set, and then nothing plays.  Nothing of ours uses it (the mic button is a hardware latch,
 * a player mute from Music Assistant is ours in software), so a set Mute is cleared. */
static void volume_sync(void)
{
    int main_v, tts_v;
    if (vol_read(VOL_MUTE, 0) != 0) { fprintf(stderr, "speaker: global Mute was set, clearing it\n"); vol_write(VOL_MUTE, 0); }
    pthread_mutex_lock(&core_lock);
    int cur = core_volume(), want = mix_target();
    pthread_mutex_unlock(&core_lock);
    main_v = vol_read(VOL_MAIN, want);
    tts_v = vol_read(VOL_TTS, want);
    pthread_mutex_lock(&core_lock);
    if (speaker_mode == SPEAKER_ABSOLUTE) {                         /* 100 is ours: only a stray TTSVolume to mend */
        if (main_v == 100 && tts_v != 100) { tts_written = -1; mix_apply(); }
    } else if (volume == cur && mix_target() == want) {           /* nobody set it meanwhile */
        if (main_v != want && !vol_split) {                         /* MainVolume is the volume: the user's wish */
            volume = main_written = main_v;
            fprintf(stderr, "volume: %d (changed outside)\n", volume);
            if (connected && proto->volume_changed) proto->volume_changed(volume);
            if (core_sendspin_port) sendspin_volume_changed(volume);
            a2dp_volume_changed(volume);
        } else if (main_v != want) {                                /* the loudest of three: ours again */
            fprintf(stderr, "volume: MainVolume %d changed outside, %d again (separate volumes)\n", main_v, want);
            main_written = -1;
        }
        if (tts_v != mix_target()) tts_written = -1;               /* set by someone else: ours again */
        mix_apply();
    }
    pthread_mutex_unlock(&core_lock);
}

/* HASSMIC_FAKE_SPEAKER=<file> (tests; the PC has no Bluetooth speaker): "none", "mixer" or "absolute <pct>" in it is
 * a speaker coming and going, as btout.c would tell it; another <pct> while absolute, its own buttons (a2dp.c) */
static void fake_speaker(void)
{
    static const char *path; static int init; char t[32]; int pct = -1; FILE *f;
    if (!init) { init = 1; path = getenv("HASSMIC_FAKE_SPEAKER"); }
    if (!path || !(f = fopen(path, "r"))) return;
    if (fgets(t, sizeof t, f)) {
        static int last = -1;
        int mode = !strncmp(t, "mixer", 5) ? SPEAKER_MIXER : sscanf(t, "absolute %d", &pct) == 1 ? SPEAKER_ABSOLUTE : SPEAKER_NONE;
        pthread_mutex_lock(&core_lock);
        int same = mode == speaker_mode;
        if (same && mode == SPEAKER_ABSOLUTE && pct != last) core_speaker_volume(pct);     /* its own buttons */
        pthread_mutex_unlock(&core_lock);
        if (!same) core_speaker(mode, pct);
        last = pct;
    }
    fclose(f);
}

static void on_volume(int dir)
{
    if (!use_volume) return;
    pthread_mutex_lock(&core_lock);
    /* separate volumes: what plays now, a Drop In call, a reply (not Home Assistant's media) or a ringing timer, else
     * the music; one that follows the music moves with it */
    int k = !vol_split ? -1 : atomic_load(&dropin_live) ? VOL_CALLS : atomic_load(&tts_on) && !tts_media ? VOL_ASSISTANT
          : atomic_load(&alarm_on) ? VOL_TIMERS : -1;
    if (k >= 0 && !kinds[k].linked) level_step(k, dir);
    else core_set_volume((core_volume() + 5) / 10 * 10 + dir * 10);
    int now = k >= 0 ? kind_volume(k) : core_volume();         /* linked: the music moved, the kind with it */
    /* at 0 the lowest step's, so a press is still heard where another kind keeps the speaker up (separate volumes;
     * above the loudest, scaled() leaves it at full) */
    atomic_store(&click_q16, (int)lroundf(mix_gain(now > 0 ? now : 10) * 65536));
    sound_request(SND_VOLUME);
    pthread_mutex_unlock(&core_lock);
}

static void *volume_led_thread(void *arg)
{
    int tick = 0;
    (void)arg;
    while (!atomic_load(&quit)) {
        if (++tick % 10 == 0) { volume_sync(); music_volume_write(); }
        fake_speaker();                                 /* tests only: at once, as btout.c tells it on the Echo */
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
        at = atomic_load(&identify_until);
        if (at && mono_ms() >= at && atomic_compare_exchange_strong(&identify_until, &at, 0)) led("-u", "zzz_rainbow");
        ring_tick();
        if (tick % 5 == 0) rd_tick();               /* root's answer to designs applied on the settings page */
        media_log_tick();
        usleep(200000);
    }
    return NULL;
}

/* ---------------------------------------------------------------- capture */

/* Self test of the version that runs: started, capture open, wake word engine loaded, ports bound (all before the thread
 * starts), and then a second of microphone audio through the capture loop.  That is as far as a broken build gets
 * before it is noticed at all; root then makes the update that runs the factory copy, the one the Echo falls back to
 * (ota_healthy, main.sh), so that the fallback is never older than the last version that worked. */

static void *selftest_thread(void *arg)
{
    (void)arg;
    for (int i = 0; i < 300 && !atomic_load(&quit); i++) {          /* 30 s */
        if (atomic_load(&cap_bytes) >= CAP_RATE * 2) { fprintf(stderr, "self test: passed\n"); ota_healthy(); return NULL; }
        usleep(100000);
    }
    fprintf(stderr, "self test: no audio from the mixer within 30 s: this version does not become the fallback\n");
    return NULL;
}

/* Capture thread: the active wake word in place of the one loaded.  One that does not load: Amazon's first ("Alexa"),
 * and the list says so (Home Assistant's select asks again on reconnect, and must not show the one that failed); a
 * microWakeWord model that does not load switches the engine back to Amazon's, setting included */
/* lock held: the names of what the slots hold now, for the runs they start */
static void wake_loaded_set(void)
{
    for (int s = 0; s < WAKE_SLOTS; s++) snprintf(wake_loaded[s], sizeof wake_loaded[s], "%s", s < n_act ? wake_words[wake_act[s]].name : "");
}

/* a model replaced from the page is swapped in by two renames (mww_store.c): once more before giving it up */
static int wake_open_retry(int slot, const struct core_wake_word *w)
{
    if (wake_open_word(slot, w) == 0) return 0;
    if (!wake_is_mww(w->manifest)) return -1;
    usleep(300000);
    return wake_open_word(slot, w);
}

/* Capture thread (main() before it runs): the second wake word, if one is picked, into the second slot.  One that does
 * not load leaves the first listening alone, as if none was picked (Home Assistant's "Wake word 2" reads so) until the
 * next start or pick */
static void wake_load_second(void)
{
    pthread_mutex_lock(&core_lock);
    int have = n_act > 1; struct core_wake_word w; if (have) w = wake_words[wake_act[1]];
    pthread_mutex_unlock(&core_lock);
    if (!have) return;
    if (wake_open_retry(1, &w) == 0) { fprintf(stderr, "wake word: second \"%s\"\n", w.name); return; }
    fprintf(stderr, "wake word: cannot load %s, the first one listens alone\n", w.manifest);
    pthread_mutex_lock(&core_lock);
    if (n_act > 1 && !strcmp(wake_words[wake_act[1]].manifest, w.manifest)) n_act = 1;
    pthread_mutex_unlock(&core_lock);
}

/* Capture thread: the active wake words in place of the ones loaded.  A first one that does not load: Amazon's first
 * ("Alexa"), and the list says so (Home Assistant's select asks again on reconnect, and must not show the one that
 * failed); a microWakeWord model that does not load switches the engine back to Amazon's, setting included */
static void wake_load(void)
{
    char e[160];
    pthread_mutex_lock(&core_lock); struct core_wake_word w = wake_words[wake_act[0]]; pthread_mutex_unlock(&core_lock);
    wake_close(-1);
    if (wake_open_retry(0, &w) == 0) fprintf(stderr, "wake word: now \"%s\"\n", w.name);
    else {
        pthread_mutex_lock(&core_lock);
        if (wake_engine == WAKE_MWW) {          /* lists anew and raises wake_switch: the next round loads Amazon's pick */
            fprintf(stderr, "wake word: cannot load %s, back to Amazon's engine\n", w.manifest);
            settings_set("wake_engine", "amazon", e, sizeof e);
            pthread_mutex_unlock(&core_lock);
            return;
        }
        fprintf(stderr, "wake word: cannot load %s, back to Alexa\n", w.manifest);
        wake_act[0] = 0; n_act = 1; struct core_wake_word first = wake_words[0];
        pthread_mutex_unlock(&core_lock);
        wake_open_word(0, &first);
    }
    wake_load_second();
    pthread_mutex_lock(&core_lock); wake_loaded_set(); pthread_mutex_unlock(&core_lock);
}

static void *capture_thread(void *arg)
{
    FILE *dump = NULL;
    int sound_running = 0;                  /* the decoder is open: only this thread opens, feeds and closes it */
    int whisper_on = 0;                     /* a whisper detector takes this request's audio */
    (void)arg;
    while (!atomic_load(&quit)) {
        const void *pcm; int n = cap_read(&pcm);
        if (n < 0) { fprintf(stderr, "capture: fatal\n"); atomic_store(&quit, 1); break; }
        if (atomic_exchange(&button_pending, 0)) on_action();      /* SIGUSR2: action button, for tests on the PC */
        if (atomic_exchange(&pair_pending, 0)) on_pair();          /* SIGWINCH: both volume keys held, for tests on the PC */
        { int t = atomic_exchange(&trigger_pending, 0);               /* 1: SIGUSR1 plays a wake word of the last 0.6 s that */
          uint64_t lag = CAP_RATE * 128 / 1000;                       /* ended 128 ms ago, as late as Pryon accepts one */
          if (t == 1) { pthread_mutex_lock(&core_lock); wake_phrase[0] = 0; heard_ms = mono_ms(); wake_told("detected"); pthread_mutex_unlock(&core_lock); }   /* the first one's */
          if (t == 3) wake_tell("near_miss");                         /* SIGTTOU: Pryon's near miss */
          if (t == 2) trigger(1, 0);
          else if (t == 1) {
              uint64_t b = ring_n > CAP_RATE ? ring_n - CAP_RATE * 6 / 10 : 0, e = ring_n > CAP_RATE ? ring_n - lag : ring_n;
              pthread_mutex_lock(&core_lock); kw_begin = b; kw_end = e; kw_slot = 0;
              if (getenv("HASSMIC_FAKE_KEYWORD")) keyword_db = 10 * log10f((ring_power(b, e) + 1) / (32768.0f * 32768.0f));   /* as on_wake: the tail is looked for */
              pthread_mutex_unlock(&core_lock);
              wake_heard(b, e, 1);
          } }
        if (atomic_exchange(&stop_pending, 0)) stop_word();        /* SIGHUP: the "stop" keyword, for tests on the PC */
        if (n == 0) continue;
        atomic_fetch_add(&cap_bytes, n);
        room_put(pcm, n / 2);                                      /* the room's level, for the media log */

        /* What the wake word hears (post-AEC micAsr), for listening on the PC.  The mixer feeds the mic only to its one
         * micAsr client, so this is the only way to record it while hassmic runs: kill -TTIN <pid> starts, again stops. */
        if (atomic_exchange(&dump_toggle, 0)) {
            if (dump) { fprintf(stderr, "capture dump: off, %ld bytes\n", ftell(dump)); fclose(dump); dump = NULL; }
            else {
                char path[256]; const char *dir = getenv("HASSMIC_STATE");
                snprintf(path, sizeof path, "%s/capture.raw", dir ? dir : "/data/local/hassmic/state");
                dump = fopen(path, "wb");
                fprintf(stderr, "capture dump: %s %s (16 kHz mono s16le) from capture sample %lld\n", dump ? "on" : "cannot write", path,
                        atomic_load(&cap_bytes) / 2 - n / 2);
            }
        }
        if (dump) fwrite(pcm, 1, n, dump);

        if (core_local_wake && atomic_exchange(&wake_switch, 0)) wake_load();     /* another wake word, engine or model */
        if (core_local_wake) { ring_put(pcm, n / 2); wake_feed(pcm, n / 2); }
        if (atomic_load(&sound_want) != sound_running) {           /* Home Assistant switched sound detection */
            if (!sound_running) {
                const char *types[SOUND_MAP]; for (int i = 0; i < SOUND_MAP; i++) types[i] = sound_map[i].amazon;
                if (sound_open(types, SOUND_MAP, on_sound) == 0) { sound_running = 1; atomic_store(&sound_failed, 0); }
                else {
                    /* off through the settings: the file, Home Assistant's list ("Sound" goes) and the page agree */
                    char e[80];
                    fprintf(stderr, "sound detection: cannot start (model did not load), switched off\n");
                    pthread_mutex_lock(&core_lock);             /* with the switch, so the page sees both or neither */
                    atomic_store(&sound_failed, 1);
                    if (settings_set("sound_detection", "off", e, sizeof e)) atomic_store(&sound_want, 0);
                    pthread_mutex_unlock(&core_lock);
                }
            } else { sound_close(); sound_running = 0; }
        }
        if (sound_running) sound_feed(pcm, n / 2);
        if (atomic_load(&whisper_last) != -2) {     /* a model: what streams to the pipeline is one request */
            int s = atomic_load(&streaming);
            if (s && !whisper_on && atomic_load(&whisper_enabled)) {             /* the position lines it up with the capture dump's */
                atomic_store(&whisper_eou, 0); whisper_begin(); whisper_on = 1;
                fprintf(stderr, "whisper: request from capture sample %lld\n", atomic_load(&cap_bytes) / 2 - n / 2);
            }
            if (whisper_on && s) whisper_feed(pcm, n / 2);
            if (whisper_on && !s) { whisper_end(atomic_exchange(&whisper_eou, 0)); whisper_on = 0; }
        }
        if (atomic_load(&dropin_live)) {               /* a Drop In: the call gets the mic as the pipeline does; muted, silence */
            static int16_t zeros[1024];
            int own = atomic_load(&earcon_sounding) || atomic_load(&sounds_pending) || mono_ms() < atomic_load(&earcon_heard_until)
                      || atomic_load(&tts_on);       /* our own sounds and replies: not for the other side */
            if (!core_muted()) dropin_mic(pcm, n / 2, own);
            else for (size_t k = n / 2; k; ) { size_t m = k < 1024 ? k : 1024; dropin_mic(zeros, m, 0); k -= m; }
        }
        if (atomic_load(&streaming)) {
            align_ahead();                              /* outside the lock: it takes up to ~0.1 s on an Echo */
            pthread_mutex_lock(&core_lock);
            uint64_t at = cap_clock() - n / 2;
            if (atomic_load(&streaming) && connected) {
                if (!replay_from) send_mic(pcm, n / 2, at);
                else {                                      /* after the wake word: from its tail's end, this block included */
                    int64_t from = word_tail_end(replay_from, at + n / 2);
                    if (from >= 0) {
                        talker_from = (uint64_t)from;
                        if (replay_whole) {                 /* or the word itself too, from a little ahead of it */
                            uint64_t lead = CAP_RATE / 10;
                            from = (int64_t)(kw_begin > lead ? kw_begin - lead : 0);
                            /* the command's start can lie before Pryon's end (the aligner's answer, taken from 400 ms
                             * ahead of it): signed, or a Dot 2's log read "the command from 1152921504606726 ms after it" */
                            fprintf(stderr, "wake: the wake word sent too, from %llu ms before its end (the command from %+lld ms after it)\n",
                                    (unsigned long long)((replay_from - (uint64_t)from) * 1000 / CAP_RATE),
                                    ((long long)talker_from - (long long)replay_from) * 1000 / CAP_RATE);
                        }
                        stream_ring((uint64_t)from, at + n / 2); replay_from = 0;
                    }
                }
                if (mic_end.ended && atomic_load(&streaming)) heard_end();
            } else replay_from = 0;
            pthread_mutex_unlock(&core_lock);
        }
        if (atomic_exchange(&det_pending, 0)) {
            pthread_mutex_lock(&core_lock); uint64_t b = det_begin, e = det_end; pthread_mutex_unlock(&core_lock);
            wake_heard(b, e, 0);
        }
        if (score_due) score_wait();
        afe_late_check();
        if (arb_due && (mono_ms() >= arb_due || arb_settled())) { arb_due = 0; if (arb_decide()) answer(arb_from); else { afe_done(); wake_tell("answered_elsewhere"); } }    /* after this block went out live */

        pthread_mutex_lock(&core_lock);
        if (core_local_wake && (state == LISTENING || state == THINKING) && time(NULL) - state_since > PIPELINE_TIMEOUT) {
            fprintf(stderr, "pipeline timeout\n");
            core_pipeline_finish();
        }
        pthread_mutex_unlock(&core_lock);
    }
    return NULL;
}

static void *serve_thread(void *arg) { int c = (int)(long)arg; proto->serve(c); netaddr_link(c, 0); close(c); return NULL; }

/* netaddr.c: Home Assistant or a Sendspin server ought to be connected and is not, or the address changed: root
 * announces mDNS again */
static int na_linked(void) { pthread_mutex_lock(&core_lock); int up = connected; pthread_mutex_unlock(&core_lock); return up; }
static int na_expected(void) { return !atomic_load(&oobe); }
static int na_ss_expected(void) { return core_sendspin_port && sendspin_expected(); }
static const struct netaddr_peer na_peers[] = {
    { "Home Assistant", 100, na_linked, na_expected },              /* > 90 s: it has given up the old link by then */
    { "Sendspin server", 30, sendspin_linked, na_ss_expected },
};
static void na_announce(const char *why)
{
    char line[80];
    snprintf(line, sizeof line, "%s\n", why);
    if (write_state("mdns", line)) fprintf(stderr, "mDNS: cannot ask root to announce again\n");
    else fprintf(stderr, "mDNS: announced again (%s)\n", why);
}

static void on_usr1(int s) { (void)s; atomic_store(&trigger_pending, 1); }
static void on_usr2(int s) { (void)s; atomic_store(&button_pending, 1); }
static void on_hup(int s) { (void)s; atomic_store(&stop_pending, 1); }
static void on_ttin(int s) { (void)s; atomic_store(&dump_toggle, 1); }
static void on_ttou(int s) { (void)s; atomic_store(&trigger_pending, 3); }
static void on_winch(int s) { (void)s; atomic_store(&pair_pending, 1); }

int main(int argc, char **argv)
{
    const char *manifest = NULL, *input = board.keypad; int port = 0, print_mdns = 0, o;
    micgain_init(&mic_gain, MICGAIN_LEVEL);            /* until the protocol has its saved settings (Wyoming: always) */
    while ((o = getopt(argc, argv, "P:p:n:w:m:b:z:o:a:i:W:G:LEVSTB")) != -1) switch (o) {
        case 'G': return ring_generate(optarg);     /* root, at the satellite's start (main.sh leds) */
        case 'P': proto = !strcmp(optarg, "wyoming") ? &proto_wyoming : &proto_esphome; break;
        case 'p': port = atoi(optarg); break;
        case 'n': core_name = optarg; break;
        case 'w': core_local_wake = strcmp(optarg, "remote") != 0; wake_cmdline_remote = !core_local_wake; break;
        case 'm': manifest = optarg; break;
        case 'b': input = optarg; break;
        case 'z': core_sendspin_port = atoi(optarg); break;
        case 'o': ota_port = atoi(optarg); break;
        case 'a': arb_port = atoi(optarg); break;
        case 'i': dropin_port = atoi(optarg); break;
        case 'W': core_web_port = atoi(optarg); break;
        case 'L': use_led = 0; break;
        case 'E': use_earcon = 0; break;
        case 'V': use_volume = 0; break;
        case 'S': print_mdns = 1; break;
        case 'B': use_bt = 0; break;
        case 'T': { char tok[160]; sendspin_init(); sendspin_pairing_token(tok, sizeof tok); puts(tok); return 0; }
        default: fprintf(stderr, "usage: hassmic [-P esphome|wyoming] [-p port] [-n name] [-w local|remote] [-m manifest] [-b input-device] [-z port] [-o port] [-a port] [-i port] [-W port] [-G dir] [-L] [-E] [-V] [-S]\n"); return 2;
    }
    core_port = port ? port : proto->port;
    if (!core_name) name_make();
    if (!print_mdns) { char p[300];                 /* names set on the settings page or by NAME before there was one name */
      state_file(p, sizeof p, "name"); if (!unlink(p)) fprintf(stderr, "name: the one set before is gone, this Echo is \"%s\" now\n", core_name);
      state_file(p, sizeof p, "node"); unlink(p); }
    if (!print_mdns) fprintf(stderr, "name: %s (node %s)\n", core_name, core_node_name());
    { char eng[24];                                 /* the settings page's "Home Assistant" engine: as -w remote */
      if (core_local_wake && !settings_peek("wake_engine", eng, sizeof eng) && !strcmp(eng, "homeassistant")) core_local_wake = 0; }
    if (print_mdns) { proto->print_mdns(); return 0; }
    clock_log_start();                              /* before any thread: it forks */
    signal(SIGPIPE, SIG_IGN); signal(SIGCHLD, SIG_IGN); signal(SIGUSR1, on_usr1); signal(SIGUSR2, on_usr2); signal(SIGHUP, on_hup); signal(SIGTTIN, on_ttin); signal(SIGTTOU, on_ttou); signal(SIGWINCH, on_winch);
    if (access("/system/bin/ledctrl", X_OK)) use_led = 0;
    ledq_start(use_led ? "/system/bin/ledctrl" : NULL, getenv("HASSMIC_FAKE_LED"), child_ids, LEDQ_TIMEOUT_MS);
    { char rows[8];                                 /* the band's height before it starts (a preset's too): else the
                                                     * default 10 px came up first, and with "Light band" 0 the apps were
                                                     * laid out twice */
      if (!settings_peek("light_band", rows, sizeof rows)) ring_band_rows(atoi(rows)); }
    ring_start(led_run, ring_reloaded);
    rd_start();                         /* designs asked for before this start: still installing, or back */
    audio_earcon_started = earcon_started;
    led("-u", "scone-setup");           /* a restart inside the pairing window: the window is gone, its chaser would loop on */
    led("-u", "factory-reset");         /* a reset asked for before this start is done (main.sh) */
    led("-u", "authenticated_setup_mode");  /* a login or adb request the last run left waiting is gone with it */
    oobe_load();
    if (atomic_load(&oobe)) { led("-s", "setup-mode"); fprintf(stderr, "setup: not set up yet (no Home Assistant), the ring shows the setup spinner until it is\n"); }
    else led("-u", "setup-mode");

    if (cap_open() < 0) { fprintf(stderr, "cannot open capture (is PuffinApp still running?)\n"); return 1; }
    pthread_mutex_lock(&core_lock); listening(0); pthread_mutex_unlock(&core_lock);
    if (core_local_wake) {
        char eng[24];                               /* the engine before the rest of the settings: no Amazon model loaded in vain */
        wake_m_arg = manifest;
        if (!settings_peek("wake_engine", eng, sizeof eng) && !strcmp(eng, "microwakeword")) wake_engine = WAKE_MWW;
        wake_words_scan();
        int ok = wake_open_word(0, &wake_words[wake_act[0]]) == 0;
        if (!ok && wake_engine == WAKE_MWW) {       /* settings_load() switches back to it, and wake_load() off for good */
            fprintf(stderr, "cannot load microWakeWord model %s, Amazon's engine\n", wake_words[wake_act[0]].manifest);
            wake_engine = WAKE_AMAZON; wake_words_scan();
            ok = wake_open_word(0, &wake_words[wake_act[0]]) == 0;
        }
        if (!ok && (wake_act[0] == 0 || (fprintf(stderr, "cannot load wake word model %s, trying Alexa\n", wake_words[wake_act[0]].manifest), wake_act[0] = 0, n_act = 1,
                                         wake_open_word(0, &wake_words[0]) < 0))) {
            fprintf(stderr, "cannot load wake word model %s\n", wake_words[wake_act[0]].manifest); return 1;
        }
        wake_load_second();
        wake_loaded_set();
    }
    if (whisper_open(on_whisper) == 0) { atomic_store(&whisper_last, -1); whisper_model = 1; }
    else fprintf(stderr, "whisper: no model, no whisper detection (the settings page's Download from Amazon gets it)\n");
    static const struct arb_hooks arb_hooks = { arb_send_key, arb_notify, arb_request, arb_paired, arb_scan, dropin_message };
    if (arb_port && core_local_wake && proto->arb_send && arb_start(arb_port, core_node_name(), &arb_hooks)) fprintf(stderr, "arbitration: not available\n");
    if (dropin_port && arb_running() && dropin_start(dropin_port)) fprintf(stderr, "drop in: not available\n");

    pthread_t cap_t, play_t, ear_t;
    pthread_create(&cap_t, NULL, capture_thread, NULL);
    pthread_create(&play_t, NULL, playback_thread, NULL);
    pthread_create(&ear_t, NULL, earcon_thread, NULL);
    { pthread_t vol_t; pthread_create(&vol_t, NULL, volume_led_thread, NULL); pthread_detach(vol_t); }

    static const struct button_handler buttons = { on_action, on_mute, on_volume, on_pair, on_hold, button_event };
    if (buttons_start(input, &buttons) < 0) fprintf(stderr, "buttons: %s not available\n", input);
    /* The latch is no press at start: on the Echo Show 8 it comes up on (the mics are cut, stock's system server plays
     * its privacy sound) and it lets go by itself 20-80 s after power on (nothing in userspace writes it).  hassmic,
     * up 40-64 s after the kernel started, found it on in two boots of three and said "mics off" to a user who had
     * touched nothing (2026-10-13).  Shown, not announced; the release is announced as any other. */
    else if (buttons_muted()) mute_latch(1, 0);

    if (core_sendspin_port) sendspin_start(core_sendspin_port);
    if (use_bt) a2dp_start(NULL);
    static const struct improv_hooks improv_hooks = { web_attention, improv_authorized };
    if (use_bt) improv_start(&improv_hooks);                 /* setup over Bluetooth, when there is no network (improv.c) */
    if (ota_port) ota_start(ota_port);
    pthread_mutex_lock(&core_lock); core_timer_sound_default(); settings_load(); settings_preset(); pthread_mutex_unlock(&core_lock);    /* whatever the protocol */
    davs_start();                                             /* Amazon login for artifact downloads (settings page) */
    static const struct web_hooks web_hooks = { web_attention, web_approved };
    if (core_web_port && web_start(core_web_port, &web_hooks)) { fprintf(stderr, "web: not available\n"); core_web_port = 0; }

    int ls = net_listen(core_port);
    if (ls < 0) { perror("listen"); return 1; }
    static const struct netaddr_hooks na_hooks = { na_peers, sizeof na_peers / sizeof na_peers[0], na_announce };
    netaddr_start(&na_hooks);
    fprintf(stderr, "hassmic " VERSION " (" BUILD ") %s on %d, wake=%s\n", proto->id, core_port, core_local_wake ? "local" : "remote");
    if (ota_port) { pthread_t st; if (!pthread_create(&st, NULL, selftest_thread, NULL)) pthread_detach(st); }
    while (!atomic_load(&quit)) {
        int c = net_accept(ls);
        if (c < 0) break;
        /* A link that went away must not hold a client slot (or core_lock, in a blocked write) for ever:
         * writes give up after 5 s, keepalive notices a dead peer within ~25 s.  Both make serve() return.  Keepalive
         * only probes while nothing waits for an acknowledgement, though; with a state update unacknowledged the
         * kernel retransmits for some 15 min (tcp_retries2): the user timeout ends that after 60 s.  A link over an
         * address the Echo no longer has is closed at once (netaddr.c). */
        { struct timeval tv = { 5, 0 }; int on = 1, idle = 10, intvl = 5, cnt = 3; unsigned user_ms = 60000;
          setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv); setsockopt(c, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on);
          setsockopt(c, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle); setsockopt(c, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
          setsockopt(c, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt); setsockopt(c, IPPROTO_TCP, TCP_USER_TIMEOUT, &user_ms, sizeof user_ms); }
        netaddr_link(c, 1);
        if (proto->threaded) {
            pthread_t t;
            if (pthread_create(&t, NULL, serve_thread, (void *)(long)c)) { netaddr_link(c, 0); close(c); } else pthread_detach(t);
        } else { proto->serve(c); netaddr_link(c, 0); close(c); }
    }
    cap_close();
    if (core_local_wake) wake_close(-1);
    return 1;
}
