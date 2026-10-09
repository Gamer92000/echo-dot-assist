/*
 * hassmic - Home Assistant voice satellite for Amazon Echo devices (first: Echo Dot 3, donut; see devices/).
 *
 * Replaces PuffinApp as the client of Amazon's `mixer` daemon: reads the post-AEC/beamformer stream,
 * runs the stock "Alexa" wake word locally, and speaks the ESPHome native API or Wyoming to Home Assistant.
 *
 *   hassmic [-P esphome|wyoming] [-p port] [-n name] [-w local|remote] [-m pryon.manifest] [-b input-device] [-L] [-E] [-V] [-S]
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
#include "davs.h"
#include "dropin.h"
#include "micdenoise.h"
#include "micgain.h"
#include "netio.h"
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
    char p[300], tmp[310]; FILE *f;
    state_file(p, sizeof p, file); snprintf(tmp, sizeof tmp, "%s.tmp", p);
    if (!(f = fopen(tmp, "w"))) return -1;
    fputs(text, f);
    if (fclose(f) || rename(tmp, p)) { unlink(tmp); return -1; }
    return 0;
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
static atomic_int flush_playback, alarm_on;   /* barge-in: drop queued TTS; UI sounds requested (bit per enum sound) */
static atomic_int busy;                             /* state != IDLE, for threads without the lock (the ring pauses) */
static atomic_int tts_on, music_on;                 /* something plays: the wake word model lowers its threshold then.
                                                    * music_on: MUSIC_* bits */
static atomic_int dump_toggle;                      /* SIGTTIN: start / stop writing the processed mic stream to a file */
static int soft_mute;                               /* under lock: mute switch from Home Assistant */
static int dnd;                                     /* under lock: do not disturb */
static int barge_in;                                /* under lock: start a new pipeline once the current one has ended
                                                      * (wake word during a reply, or the server asked to continue the conversation) */
static struct micgain mic_gain;                     /* under lock: gain of what the pipeline hears (micgain.h) */
static int mic_fresh;                               /* under lock: a pipeline started, the gain has not seen it yet */
static float keyword_db = 1;                        /* under lock: rms of the last wake word, dBFS */
static int mic_denoise;                             /* under lock: noise reduction ahead of the gain (micdenoise.h):
                                                      * 0 off, 1 low, 2 medium, 3 high */
static long long denoise_ns; static unsigned denoise_frames;   /* under lock: its cost in the running pipeline */
static atomic_int earcon_sounding;                  /* one of our sounds plays ... */
static atomic_llong earcon_heard_until;             /* ... and is still in the mic stream until then (mono_ms) */

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
 * shows them in the satellite's wake word select; its pick is kept in state/wake_word (state/mww_word for microWakeWord)
 * and loaded live by the capture thread.  -m names the Amazon model to use until Home Assistant has picked one (before
 * 2026-09-25 it was the only way, and Home Assistant was told "Alexa" whatever -m said). */
#define MAX_WAKE_WORDS 16
static struct core_wake_word wake_words[MAX_WAKE_WORDS];
static int n_wake_words, wake_active;               /* under core_lock once running */
static int wake_engine;                             /* WAKE_AMAZON / WAKE_MWW, under core_lock once running */
static const char *wake_m_arg;                      /* -m */
static int wake_cmdline_remote;                     /* -w remote: hassmic.conf decides, not the settings page */
static int wake_next = -1;                          /* asked for, takes effect with the restart */
static const char *wake_refused = "";
static atomic_int wake_switch;                      /* capture thread: load wake_words[wake_active] */

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
        size_t k = strlen(wake_words[i].name);
        if (shared[i] && dash) snprintf(wake_words[i].name + k, sizeof wake_words[i].name - k, " (%.20s)", dash + 1);
    }
    wake_active = def;
    if ((f = fopen(wake_word_path(), "r"))) {
        if (fscanf(f, "%63s", saved) == 1) { int i = wake_word_find(saved); if (i >= 0) wake_active = i; }
        fclose(f);
    }
    for (int i = 0; i < n_wake_words; i++)
        fprintf(stderr, "wake word: %s \"%s\" (%s%s)%s\n", wake_words[i].id, wake_words[i].name, wake_words[i].lang,
                wake_engine == WAKE_MWW ? ", microWakeWord" : "", i == wake_active ? ", active" : "");
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
        char active[64]; snprintf(active, sizeof active, "%s", n_wake_words ? wake_words[wake_active].id : "");
        wake_words_scan();
        if (wake_engine != WAKE_MWW) settings_save();       /* the last one went: Amazon's again, also after a restart */
        else if (strcmp(active, wake_words[wake_active].id)) fprintf(stderr, "wake word: %s gone, now %s\n", active, wake_words[wake_active].id);
        wake_list_changed();                                 /* also when only the active one's cutoff changed: reloaded */
        core_settings_changed();
    }
    pthread_mutex_unlock(&core_lock);
}

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

static void pipeline_start(void)
{
    quiet_abort = 0;
    mic_fresh = 1;
    proto->start();
    atomic_store(&streaming, 1);
    listening(core_local_wake);
    if (core_local_wake) core_set_state(LISTENING);
}

/* lock held.  Home Assistant heard the wake word in the stream (-w remote, wake word engine "homeassistant"): what the
 * Echo does on a wake word of its own, the sound, the ring and the front end's utterance state */
void core_remote_wake(void)
{
    if (core_local_wake || !atomic_load(&streaming)) return;
    sound_request(SND_WAKE);
    listening(1);
    core_set_state(LISTENING);
}

int core_wake_sound(int set) { if (set >= 0) use_earcon = set; return use_earcon; }

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
        if (st != DROPIN_LIVE) { listen_call = 0; listening_apply(); }       /* dropin.c turns it on while this side talks */
        if (connected && proto->dropin_changed) proto->dropin_changed();
    }
    pthread_mutex_unlock(&core_lock);
    /* A call is the newest source, as Alexa's paused what played: music's leftovers in micAsr would open the gate and go
     * to the other side.  No resume afterwards, as between the music sources; a phone without remote control was only
     * made silent and is heard again.  Outside core_lock: sendspin takes its own lock */
    if (st == DROPIN_LIVE && was != DROPIN_LIVE) { if (core_sendspin_port) sendspin_pause(); a2dp_pause(); }
    if (st == DROPIN_IDLE && was == DROPIN_LIVE) a2dp_unyield();
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
#define SOUND_WINDOW_MS 11000               /* a scoring window (9.98 s) and the decoder's lag behind it */
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
    long long now = mono_ms();
    if (atomic_load(&alarm_on) || atomic_load(&music_on) || atomic_load(&tts_on) || atomic_load(&earcon_sounding)
        || atomic_load(&sounds_pending) || now - atomic_load(&own_sound_ms) < SOUND_WINDOW_MS
        || now - atomic_load(&earcon_heard_until) < SOUND_WINDOW_MS) {
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

int core_mic_denoise(int set)
{
    if (set >= 0 && set != mic_denoise) { mic_denoise = set > 3 ? 3 : set; mic_fresh = 1; }
    return mic_denoise;
}

/* lock held.  The mic stream to the pipeline has stopped. */
static void mic_stopped(void)
{
    listening(0);
    afe_done();
    if (denoise_frames) fprintf(stderr, "denoise: %.1f s of audio took %.0f ms of CPU\n", denoise_frames / 100.0, denoise_ns / 1e6);
    denoise_frames = 0; denoise_ns = 0;
}

void core_mic_off(void) { atomic_store(&whisper_eou, 1); atomic_store(&streaming, 0); mic_stopped(); }

void core_restart_after(void) { barge_in = 1; }

void core_pipeline_finish(void)
{
    if (atomic_exchange(&streaming, 0) && connected && proto->stop) proto->stop();
    mic_stopped();
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
    if (satellite_running) adopted();
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
    /* A ringing timer stops only when told to: the button, "<wake word>, stop" (stop_word, core_transcript), Home
     * Assistant's media stop.  The wake word alone puts it in the background, as on a stock Echo: the ring pauses while
     * the pipeline runs ("Alexa, how long is left on the pasta timer" while another rings) and goes on after it */
    if (touch && atomic_load(&alarm_on)) {
        core_alarm_stop("button");
        wake_cut_ms = mono_ms();
    } else if ((state == THINKING || (touch && state == LISTENING)) && connected && proto->cancel) {
        /* As the center button of a Voice PE: a misheard command is stopped before its tool calls run, not only its
         * reply.  Same message as ESPHome's voice_assistant.stop; Home Assistant cancels the run's task (the LLM, and
         * the tool calls it has not made yet).  The wake word does the same and then listens again, as during a reply */
        fprintf(stderr, "%s: pipeline cancelled\n", touch ? "button" : "wake word");
        barge_in = 0;
        atomic_store(&streaming, 0);
        proto->cancel();
        core_pipeline_finish();
        quiet_abort = 1;
        if (!touch && state == IDLE && satellite_running && !core_muted()) {
            wake_cut_ms = mono_ms();
            sound_request(SND_WAKE);
            pipeline_start();
        }
    } else if (!connected || !satellite_running || core_muted()) {
        /* nothing to talk to, or privacy latch on */
    } else if (state == IDLE) {
        if (atomic_load(&alarm_on)) wake_cut_ms = mono_ms();    /* its "stop" drops this pipeline again */
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
    if (dropin_running() && dropin_status(NULL, 0) != DROPIN_IDLE) dropin_hangup("stop");     /* and the pipeline its wake word opened */
    pthread_mutex_lock(&core_lock);
    if (atomic_load(&alarm_on)) core_alarm_stop("stop");        /* and the pipeline its wake word opened, below */
    if (state == SPEAKING) {
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

/* ---------------------------------------------------------------- wake word arbitration (arb.c)
 * With other Echos in the arbitration network, a detection is scored and only acted on once the others' claims are in:
 * WINDOW_MS later, in the capture thread.  Until then nothing shows (no sound, no ring), so the Echos that lose stay
 * quiet.  The audio of the window is not lost: the winner sends it from the ring buffer ahead of the live stream. */

#define RING_SAMPLES (CAP_RATE * 4)
static int16_t ring[RING_SAMPLES];                  /* what the wake word engine was fed, by its sample index */
static uint64_t ring_n;                             /* capture thread: samples fed so far */
static atomic_int det_pending;                      /* a detection for the capture thread; det_begin/end under core_lock */
static uint64_t det_begin, det_end;
static long long arb_due;                           /* capture thread: a round runs, decide then */
static uint64_t arb_from;                           /* capture thread: first sample after the detection */

static void ring_put(const int16_t *s, size_t n)
{
    for (size_t i = 0; i < n; i++) ring[(ring_n + i) % RING_SAMPLES] = s[i];
    ring_n += n;
}

static double ring_power(uint64_t a, uint64_t b)    /* mean square over samples [a, b), as far as the ring still has them */
{
    double sum = 0; uint64_t n = 0;
    if (ring_n > RING_SAMPLES && a < ring_n - RING_SAMPLES) a = ring_n - RING_SAMPLES;
    if (b > ring_n) b = ring_n;
    for (uint64_t i = a; i < b; i++, n++) { double v = ring[i % RING_SAMPLES]; sum += v * v; }
    return n ? sum / n : 0;
}

/* Signal to noise of the wake word in dB x 100: the keyword against the half second before it (ending 100 ms ahead, so
 * that an early "begin" does not count the word as noise).  On the processed stream after beamforming, AEC and gain
 * control, the absolute level says less than how far the voice stands out of the room: the Echo the talker is close to
 * and facing hears it clearest.  HASSMIC_TEST_SCORE stands in for it on the PC, where SIGUSR1 plays the detection. */
/* Amazon's own measure of the wake word, which its cloud used to pick the Echo that answers ("ESP"): the energy of the
 * keyword and of the room before it as the front end measures them ("1-mic ESP" in its log).  As stock does it: hand the front end the keyword's place on its clock
 * (wake_afe_times), then read LASP_CMD_REQUEST_ARBITRATION_JSON: {"voiceEnergy":..,"ambientEnergy":..,..}.  Both
 * through lipc's tools: 150 ms (measured), inside the arbitration window of the others, who wait 200 ms and count
 * claims up to a second old.  Their ratio in dB x 100 is a signal to noise like our own score below, so Echos without
 * it (older builds, other front ends) still compare.  Reading it also puts the front end into its utterance state and
 * starts its diagnostics (FINDINGS.md "Listening mode"): afe_done() ends both.  0: not available. */
static int afe_score(int *score)                    /* capture thread */
{
    long ts, te; char buf[512]; const char *v, *a;
    if (!wake_afe_times(&ts, &te)) return 0;
    if (!afe_arbitration(ts, te, buf, sizeof buf)) return 0;
    if (!(v = strstr(buf, "\"voiceEnergy\":")) || !(a = strstr(buf, "\"ambientEnergy\":"))) return 0;
    atomic_store(&afe_asked, 1);
    double voice = atof(strchr(v, ':') + 1), ambient = atof(strchr(a, ':') + 1);
    *score = (int)lround(1000 * log10((voice + 1) / (ambient + 1)));
    fprintf(stderr, "wake: front end: voice energy %.0f, ambient %.0f\n", voice, ambient);
    return 1;
}

static int wake_score(uint64_t begin, uint64_t end, int simulated)
{
    const char *t = getenv("HASSMIC_TEST_SCORE");
    if (simulated && t) return atoi(t);
    uint64_t gap = CAP_RATE / 10, len = CAP_RATE / 2;
    uint64_t ne = begin > gap ? begin - gap : 0, nb = ne > len ? ne - len : 0;
    double w = ring_power(begin, end), n = ring_power(nb, ne), fs = 32768.0 * 32768.0;
    int own = (int)lround(1000 * log10((w + 1) / (n + 1))), afe;
    fprintf(stderr, "wake: level %.1f dBFS over noise %.1f dBFS\n", 10 * log10((w + 1) / fs), 10 * log10((n + 1) / fs));
    if (simulated || !afe_score(&afe)) return own;
    fprintf(stderr, "wake: score %d from the front end (%d from the mic stream)\n", afe, own);
    return afe;
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

/* lock held.  Mic audio to the pipeline: noise reduction if switched on, then brought to speech level.  at: the ring's
 * index of the first sample (0: not from the ring).
 * - The wake word just before it sets the gain to start with (from up to 3 s back), and RNNoise first hears the second
 *   of room and wake word ahead of the command, so that it does not start on the first word.
 * - Our own wake sound is still in the stream after the echo canceller: +21 dB over the floor in micRaw, +4 to +8 dB in
 *   micAsr (4 triggers, 2026-09-30), as loud as a quiet talker.  The gain took it for speech and came down for the
 *   command behind it (-32 instead of -26 dBFS in Home Assistant's recording), so it holds still while a sound plays. */
static void send_mic(const int16_t *pcm, size_t n, uint64_t at)
{
    static int16_t out[1024 + MICDENOISE_FRAME];
    if (mic_fresh) {
        micgain_start(&mic_gain, mono_ms() - last_wake_ms < 3000 ? keyword_db : 1); mic_fresh = 0;
        static const int denoise_db[] = { 0, 6, 9, 12 };
        fprintf(stderr, "mic gain: talker %.1f dBFS, gain %+.1f dB", micgain_talker_db(&mic_gain), mic_gain.gain_db);
        if (mic_denoise) fprintf(stderr, ", noise reduction %d dB", denoise_db[mic_denoise]);
        fprintf(stderr, "\n");
        if (mic_denoise) {
            micdenoise_reset(denoise_db[mic_denoise]);
            for (uint64_t i = at > CAP_RATE ? at - CAP_RATE : 0; at && i < at; ) {
                size_t o = i % RING_SAMPLES, k = at - i;
                if (k > RING_SAMPLES - o) k = RING_SAMPLES - o;
                micdenoise_run(ring + o, k, NULL);
                i += k;
            }
        }
    }
    mic_gain.hold = atomic_load(&sounds_pending) || atomic_load(&earcon_sounding) || mono_ms() < atomic_load(&earcon_heard_until);
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
        if (m) { micgain_run(&mic_gain, src, out, m); proto->audio(out, m * 2); }
        pcm += k; n -= k;
    }
}

static void stream_ring(uint64_t from)              /* lock held */
{
    while (from < ring_n) {
        uint64_t at = from % RING_SAMPLES, n = ring_n - from;
        if (n > RING_SAMPLES - at) n = RING_SAMPLES - at;
        send_mic(ring + at, n, from);
        from += n;
    }
}

static void answer(uint64_t from)                   /* capture thread: act on the wake word; audio after it from the ring */
{
    int was = atomic_load(&streaming);
    trigger(0);
    pthread_mutex_lock(&core_lock);
    if (from && !was && atomic_load(&streaming) && connected) stream_ring(from);
    pthread_mutex_unlock(&core_lock);
    if (!atomic_load(&streaming)) afe_done();          /* it stopped a ring, or cut a reply: the pipeline's end follows */
}

static void wake_heard(uint64_t begin, uint64_t end, int simulated)     /* capture thread */
{
    if (!arb_running()) { trigger(0); return; }
    if (arb_due) return;                            /* the same wake word once more while its round runs */
    pthread_mutex_lock(&core_lock);
    int alarm = atomic_load(&alarm_on), can = alarm || (connected && satellite_running && !core_muted()), prio = alarm || state != IDLE ? 2 : 0;
    pthread_mutex_unlock(&core_lock);
    if (!can) { trigger(0); return; }               /* could not answer: a claim would only silence the Echos that can */
    pthread_mutex_lock(&core_lock); char kw[64]; snprintf(kw, sizeof kw, "%s", wake_words[wake_active].name); pthread_mutex_unlock(&core_lock);
    /* Kiosk Satellite mode: their loudness, not the front end's energies (whose 150 ms only the window would pay for) */
    int score = arb_mode(-1) == ARB_KIOSK ? kiosk_score(end, simulated) : wake_score(begin, end, simulated);
    if (score == INT_MIN) { fprintf(stderr, "arbitration: less than 3 s heard, answers without a claim\n"); trigger(0); return; }
    /* The other Echos' claims are Wi-Fi broadcasts, and the BLE scan's share of the antenna loses them (ble.c): no
     * scanning while this round listens.  1 s covers the window (Kiosk Satellite's up to 500 ms), the claims of Echos
     * that heard it a little later, and the winner's "answers" */
    ble_quiet(1000);
    long long due = arb_claim(kw, score, prio);
    if (!due) { trigger(0); if (!atomic_load(&streaming)) afe_done(); return; }
    arb_due = due; arb_from = ring_n;
}

/* The keyword the loaded model answers to (capture thread).  Amazon's en-US sets for "Computer", "Amazon" and "Ziggy"
 * are one model (words.shrunk.txt: AMAZON COMPUTER HEY_DISNEY STOP ZIGGY, every one of them wakes from sleep in its
 * op.cfg.json, nothing in the set switches one off): with "Computer" picked, "Amazon" and "Ziggy" woke the Echo too
 * (GitHub issue 14).  Stock's client picks the one it wants out of the results; so do we.  The keyword is the set's
 * id up to the language ("computer-en-US" -> COMPUTER, the firmware's "alexa" -> ALEXA), but only if the set lists it:
 * a set installed by hand under another name answers to everything it knows, as before.  "" = no filter. */
static char wake_keyword[64];
static void on_wake(const char *keyword, uint64_t begin, uint64_t end);

static int words_list(const char *path, const char *kw)    /* words.shrunk.txt: "<word> <index>" per line */
{
    char line[96], w[64]; FILE *f = fopen(path, "r"); int hit = 0;
    if (!f) return 0;
    while (!hit && fgets(line, sizeof line, f)) hit = sscanf(line, "%63s", w) == 1 && !strcasecmp(w, kw);
    fclose(f);
    return hit;
}

static int wake_open_word(const struct core_wake_word *w)
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
    snprintf(wake_keyword, sizeof wake_keyword, "%s", hit ? kw : "");
    return wake_open(w->manifest, on_wake);
}

static void on_wake(const char *keyword, uint64_t begin, uint64_t end)
{
    if (!core_local_wake) return;
    if (wake_keyword[0] && strcasecmp(keyword, wake_keyword) && strcasecmp(keyword, "STOP")) {
        fprintf(stderr, "wake: %s is not the wake word picked (%s), ignored\n", keyword, wake_keyword);
        return;
    }
    /* In a Drop In the other side's voice plays here, and what the echo canceller leaves of its "Alexa" (meant for the
     * Echo over there) can be enough for ours.  While it talks, the wake word is left to that Echo */
    if (atomic_load(&dropin_live) && dropin_far_talking()) { fprintf(stderr, "wake: %s while the other side of the Drop In talks, ignored\n", keyword); return; }
    if (!strcasecmp(keyword, "STOP")) { stop_word(); return; }
    last_wake_ms = mono_ms();
    { float db = 10 * log10f((ring_power(begin, end) + 1) / (32768.0f * 32768.0f));     /* the talker's level, for the gain */
      pthread_mutex_lock(&core_lock); keyword_db = db; pthread_mutex_unlock(&core_lock); }
    if (!arb_running()) { trigger(0); return; }
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
    mic_stopped();
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

/* Finished timers ring until the button, the wake word, "stop", the media player's stop or the ring time (setting
 * timer_ring, 0 = until stopped).  Home Assistant keeps several timers per satellite and forgets each as it finishes
 * (intent/timers.py pops it before the finished event), so a cancel never names a ringing one: it is for a timer that
 * still runs, and must not silence the others.  One ring for all of them; each new one gives the ring its full time
 * again; one press stops them all (the Echo cannot ask which).  Home Assistant sees "Timer ringing" and their names. */
#define RINGING_MAX 4
static struct { char id[40], name[64]; } ringing[RINGING_MAX];  /* lock held: in the order they finished */
static int nringing, timer_ring_s = 60;                         /* lock held */
static atomic_llong alarm_until;                                /* mono_ms the ring ends; LLONG_MAX: until stopped */

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
 * finished), so the run is cancelled here, as the button does, before Home Assistant answers it */
int core_transcript(const char *text)
{
    static const char *const stops[] = { "stop", "stopp", "stoppen", "stoppa", "halt", "arrête", "arrete", "basta", "para", "pare" };
    /* a Drop In ends on these (and on the stops): Home Assistant has no intent for it, and the run is cancelled */
    static const char *const hangups[] = { "hangup", "endcall", "endthecall", "enddropin", "auflegen", "legauf", "beenden", "anrufbeenden",
                                           "dropinbeenden", "gesprächbeenden", "raccroche", "raccrocher", "cuelga", "colgar",
                                           "riattacca", "riaggancia", "ophangen", "hangop", "legop" };     /* the blueprint's languages */
    char w[32]; size_t n = 0;
    for (const char *p = text; *p && n < sizeof w - 1; p++)        /* lower case, without spaces and punctuation */
        if (!strchr(" \t.,!?¡¿\"'", *p)) w[n++] = (char)tolower((unsigned char)*p);
    w[n] = 0;
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
    if (!atomic_load(&alarm_on)) return 0;
    for (size_t i = 0; i < sizeof stops / sizeof stops[0]; i++) if (!strcmp(w, stops[i])) {
        core_alarm_stop("said stop");
        if (connected && proto->cancel) { barge_in = 0; atomic_store(&streaming, 0); proto->cancel(); quiet_abort = 1; }
        core_pipeline_finish();
        return 1;
    }
    return 0;
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

static int ring_sound;
static int ring_go(void) { return atomic_load(&dropin_ring) == ring_sound && !atomic_load(&sounds_pending); }

static void *earcon_thread(void *arg)
{
    enum { RATE = 48000, N = RATE * 12 / 100 };
    static short tone[N];
    static const char *const snd_names[SND_COUNT] = { "wake", "touch", "mics off", "mics on", "volume", "bluetooth connected",
                                                                "bluetooth disconnected", "identify", "drop in", "call connected",
                                                                "call ended", "incoming call", "outgoing call" };
    (void)arg;
    for (int i = 0; i < N; i++) {           /* 120 ms rising two-tone blip with 10 ms fades */
        double f = i < N / 2 ? 880.0 : 1320.0, env = fmin(1.0, fmin(i, N - i) / (RATE * 0.01));
        tone[i] = (short)(6000 * env * sin(2 * M_PI * f * i / RATE));
    }
    for (;;) {
        /* Amazon's own sounds where the image has them; the generated blip stands in for the wake and touch sounds otherwise */
        for (int p = atomic_exchange(&sounds_pending, 0), s = 0; p && s < SND_COUNT; s++) {
            const short *pcm; size_t n; unsigned rate;
            if (!(p & 1 << s)) continue;
            atomic_store(&earcon_sounding, 1);
            if (sound_get((enum sound)s, &pcm, &n, &rate)) { fprintf(stderr, "sound: %s\n", snd_names[s]); play_earcon(pcm, n, rate); }
            else if (s == SND_WAKE || s == SND_TOUCH || s == SND_IDENTIFY) play_earcon(tone, N, RATE);
            atomic_store(&earcon_heard_until, mono_ms() + 250);         /* speaker to mic stream: 85 ms, and the room's tail */
            atomic_store(&earcon_sounding, 0);
        }
        { int r = atomic_load(&dropin_ring); const short *pcm; size_t n; unsigned rate;     /* a Drop In rings: cut off when answered */
          if (r >= 0 && mono_ms() >= atomic_load(&dropin_ring_at) && sound_get((enum sound)r, &pcm, &n, &rate)) {
              ring_sound = r; play_earcon_while(pcm, n, rate, ring_go); usleep(300000); continue; } }
        if (atomic_load(&alarm_on)) {                   /* timer finished: triple blip every 1.2 s, until the ring time */
            if (mono_ms() > atomic_load(&alarm_until)) {
                pthread_mutex_lock(&core_lock);
                if (mono_ms() > atomic_load(&alarm_until)) core_alarm_stop("rang out");     /* not a timer that just came */
                pthread_mutex_unlock(&core_lock);
            } else if (!atomic_load(&busy)) { for (int k = 0; k < 3; k++) play_earcon(tone, N, RATE); usleep(800000); }
            /* in the background while a pipeline runs: speech to text must hear the user, not the ring */
        }
        usleep(20000);
    }
    return NULL;
}

/* Action button: cancel a running pipeline; else pause what plays (Bluetooth device first, then Sendspin), or resume
 * what the button paused; else talk */
static void on_action(void)
{
    if (web_approve()) return;                          /* a login of the settings page waited for this press */
    if (improv_authorize()) return;                     /* a phone setting up the network did */
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

/* Volume: 10 % per press like stock (3 of the ring's 30 steps on donut, board.volume_steps).  The volume_step-NN animations show 2 s and then loop
 * black forever, so the previous one has to be unset or they pile up in ledcontroller; a timer clears the last one. */
static int volume = -1;                      /* 0..100, read from the device on first use */
static char vol_pat[24];
static atomic_llong vol_clear_at;

static long long mono_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int read_volume(void) { return vol_read(VOL_MAIN, 40); }

/* The backend keeps one volume per kind of stream (audio.h).  VOL_MAIN is what the stock volume keys move and covers
 * music and sounds; the assistant's replies follow VOL_TTS alone.  One knob for the user: both. */
static void set_both(int v) { vol_write(VOL_MAIN, v); vol_write(VOL_TTS, v); }

/* Playing to a Bluetooth speaker (a2dp.c, btout.c) it has a volume of its own: what the buttons and Home Assistant move
 * meanwhile, starting from the speaker's when it tells (AVRCP absolute volume); the Echo's own comes back afterwards.
 * With absolute volume the speaker applies it and the mixer plays at full scale, so SBC gets the whole signal (at volume
 * 30 the mixer's curve, made for the Echo's small speaker, put it 38 dB down); without, the mixer applies it as before.
 * The Echo's own volume is kept in a file meanwhile, so that a hassmic that dies on the speaker does not take what the
 * mixer then says for it and play the Echo's speaker at the Bluetooth speaker's level, or at full scale. */
static int speaker_mode;                     /* under lock: SPEAKER_* */
static int echo_own_volume;                  /* under lock: the Echo's volume, set aside while speaker_mode */

static const char *own_volume_path(void)
{
    static char p[256]; const char *d = getenv("HASSMIC_STATE");
    snprintf(p, sizeof p, "%s/volume.speaker", d ? d : "/data/local/hassmic/state");
    return p;
}

int core_volume(void)
{
    if (volume < 0) {                        /* whatever it was left at: in line now */
        FILE *f = fopen(own_volume_path(), "r"); int v;
        if (f && fscanf(f, "%d", &v) == 1 && v >= 0 && v <= 100) {
            volume = v; vol_write(VOL_MAIN, volume);
            fprintf(stderr, "volume: %d (the Echo's own, set aside for a Bluetooth speaker)\n", volume);
        } else volume = read_volume();
        if (f) fclose(f);
        unlink(own_volume_path());
        vol_write(VOL_TTS, volume);
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
    if (mode == SPEAKER_NONE) volume = echo_own_volume;
    else if (mode == SPEAKER_ABSOLUTE) volume = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    speaker_mode = mode;
    int mix = mode == SPEAKER_ABSOLUTE ? 100 : volume;
    set_both(mix);
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

void core_set_volume(int v)
{
    char pat[24]; int step;
    volume = v < 0 ? 0 : v > 100 ? 100 : v;
    step = volume * board.volume_steps / 100 ? volume * board.volume_steps / 100 : 1;
    snprintf(pat, sizeof pat, "volume_step-%02d", step);
    if (speaker_mode != SPEAKER_ABSOLUTE) set_both(volume);
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

/* LED brightness.  Stock's auto brightness is ledcontroller's, not the Alexa client's, and it starts it by itself at boot
 * (Echo Dot 2 set to 50 with auto off and rebooted: 9 again, for 40 lux; 2026-10-01).  It polls the light sensor through
 * the same HAL file as core_lux() (1 Hz when settled, 20 Hz while moving), smooths it over 3 s and maps 0..400 lux on a
 * straight line to 0..100 (donut: 0.26 per lux - 4, at least 0), ramping there in 3 s.  So "auto" here is ledcontroller
 * left alone, and a fixed level is ledctrl -a off -b N in one call: two calls could land in either order, and the running
 * engine would overwrite a level that came first.  Neither ledcontroller nor anything stock keeps the auto flag: our
 * settings file does (proto_esphome.c), and it is applied again at every start.  ledcontroller writes each level it shows
 * to persist.ledbrightness.bootup (auto steps too) and restores it at boot, so that property is what the ring shows. */
static int led_auto = 1, led_level = 80;            /* 80: ledcontroller's first-boot level */

int core_led_auto(int set)
{
    if (set >= 0 && set != led_auto) {
        if (!set) led_level = core_led_brightness(-1);
        led_auto = set;
        if (use_led) run("/system/bin/ledctrl", "-a", set ? "on" : "off");     /* off: the level stays where auto left it */
        fprintf(stderr, "LED brightness: %s\n", set ? "auto" : "fixed");
    }
    return led_auto;
}

int core_led_brightness(int set)
{
    if (set >= 0) {
        char n[8]; snprintf(n, sizeof n, "%d", set > 100 ? 100 : set);
        char *argv[] = { "/system/bin/ledctrl", "-a", "off", "-b", n, NULL };
        if (use_led) run_argv(argv);
        led_auto = 0; led_level = atoi(n);
        fprintf(stderr, "LED brightness: fixed at %d\n", led_level);
    }
#ifdef __ANDROID__
    char v[PROP_VALUE_MAX] = "";                   /* fixed: ours, the property may not have it yet (ledctrl runs apart) */
    if (led_auto && use_led && __system_property_get("persist.ledbrightness.bootup", v) > 0) return atoi(v);
#endif
    return led_level;
}

/* The light sensor as stock's HAL (libacehal_ambientLightSensor.so, its per-model "facade") reads it: a sysfs file the
 * kernel driver fills with calibrated lux, parsed with atof.  0..400 is all stock uses of it. */
float core_lux(void)
{
    const char *e = getenv("HASSMIC_LUX");         /* tests */
    const char *const *p = e ? (const char *const[]){ e, NULL } : board.light_sensor;
    for (; p && *p; p++) {
        char buf[32]; int fd = open(*p, O_RDONLY); ssize_t n;
        if (fd < 0) continue;
        n = read(fd, buf, sizeof buf - 1); close(fd);
        if (n <= 0) continue;
        buf[n] = 0;
        return (float)atof(buf);
    }
    return NAN;
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
    if (vol_read(VOL_MUTE, 0) != 0) { fprintf(stderr, "speaker: global Mute was set, clearing it\n"); vol_write(VOL_MUTE, 0); }
    pthread_mutex_lock(&core_lock);
    int cur = core_volume();
    pthread_mutex_unlock(&core_lock);
    main_v = vol_read(VOL_MAIN, cur);
    tts_v = vol_read(VOL_TTS, cur);
    pthread_mutex_lock(&core_lock);
    if (speaker_mode == SPEAKER_ABSOLUTE) {                         /* 100 is ours: only a stray TTSVolume to mend */
        if (main_v == 100 && tts_v != 100) vol_write(VOL_TTS, 100);
    } else if (volume == cur) {                                     /* nobody set it meanwhile */
        if (main_v != cur) {
            volume = main_v;
            fprintf(stderr, "volume: %d (changed outside)\n", volume);
            if (connected && proto->volume_changed) proto->volume_changed(volume);
            if (core_sendspin_port) sendspin_volume_changed(volume);
            a2dp_volume_changed(volume);
        }
        if (tts_v != volume) vol_write(VOL_TTS, volume);
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
        at = atomic_load(&identify_until);
        if (at && mono_ms() >= at && atomic_compare_exchange_strong(&identify_until, &at, 0)) led("-u", "zzz_rainbow");
        usleep(200000);
    }
    return NULL;
}

/* ---------------------------------------------------------------- capture */

/* Self test of the version that runs: started, capture open, wake word engine loaded, ports bound (all before the thread
 * starts), and then a second of microphone audio through the capture loop.  That is as far as a broken build gets
 * before it is noticed at all; root then makes the update that runs the factory copy, the one the Echo falls back to
 * (ota_healthy, main.sh), so that the fallback is never older than the last version that worked. */
static atomic_long cap_bytes;

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
static void wake_load(void)
{
    char e[160];
    pthread_mutex_lock(&core_lock); struct core_wake_word w = wake_words[wake_active]; pthread_mutex_unlock(&core_lock);
    wake_close();
    if (wake_open_word(&w) == 0) { fprintf(stderr, "wake word: now \"%s\"\n", w.name); return; }
    /* a model replaced from the page is swapped in by two renames (mww_store.c): once more before giving it up */
    if (wake_is_mww(w.manifest)) { usleep(300000); if (wake_open_word(&w) == 0) { fprintf(stderr, "wake word: now \"%s\"\n", w.name); return; } }
    pthread_mutex_lock(&core_lock);
    if (wake_engine == WAKE_MWW) {          /* lists anew and raises wake_switch: the next round loads Amazon's pick */
        fprintf(stderr, "wake word: cannot load %s, back to Amazon's engine\n", w.manifest);
        settings_set("wake_engine", "amazon", e, sizeof e);
        pthread_mutex_unlock(&core_lock);
        return;
    }
    fprintf(stderr, "wake word: cannot load %s, back to Alexa\n", w.manifest);
    wake_active = 0; struct core_wake_word first = wake_words[0];
    pthread_mutex_unlock(&core_lock);
    wake_open_word(&first);
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
        { int t = atomic_exchange(&trigger_pending, 0);               /* 1: SIGUSR1 plays a wake word of the last 0.6 s */
          if (t == 2) trigger(1); else if (t == 1) wake_heard(ring_n > CAP_RATE * 6 / 10 ? ring_n - CAP_RATE * 6 / 10 : 0, ring_n, 1); }
        if (atomic_exchange(&stop_pending, 0)) stop_word();        /* SIGHUP: the "stop" keyword, for tests on the PC */
        if (n == 0) continue;
        atomic_fetch_add(&cap_bytes, n);

        /* What the wake word hears (post-AEC micAsr), for listening on the PC.  The mixer feeds the mic only to its one
         * micAsr client, so this is the only way to record it while hassmic runs: kill -TTIN <pid> starts, again stops. */
        if (atomic_exchange(&dump_toggle, 0)) {
            if (dump) { fprintf(stderr, "capture dump: off, %ld bytes\n", ftell(dump)); fclose(dump); dump = NULL; }
            else {
                char path[256]; const char *dir = getenv("HASSMIC_STATE");
                snprintf(path, sizeof path, "%s/capture.raw", dir ? dir : "/data/local/hassmic/state");
                dump = fopen(path, "wb");
                fprintf(stderr, "capture dump: %s %s (16 kHz mono s16le) from capture sample %ld\n", dump ? "on" : "cannot write", path,
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
                fprintf(stderr, "whisper: request from capture sample %ld\n", atomic_load(&cap_bytes) / 2 - n / 2);
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
            pthread_mutex_lock(&core_lock);
            if (atomic_load(&streaming) && connected) send_mic(pcm, n / 2, core_local_wake ? ring_n - n / 2 : 0);
            pthread_mutex_unlock(&core_lock);
        }
        if (atomic_exchange(&det_pending, 0)) {
            pthread_mutex_lock(&core_lock); uint64_t b = det_begin, e = det_end; pthread_mutex_unlock(&core_lock);
            wake_heard(b, e, 0);
        }
        if (arb_due && mono_ms() >= arb_due) { arb_due = 0; if (arb_decide()) answer(arb_from); else afe_done(); }    /* after this block went out live */

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
static void on_winch(int s) { (void)s; atomic_store(&pair_pending, 1); }

int main(int argc, char **argv)
{
    const char *manifest = NULL, *input = board.keypad; int port = 0, print_mdns = 0, o;
    micgain_init(&mic_gain, MICGAIN_LEVEL);            /* until the protocol has its saved settings (Wyoming: always) */
    while ((o = getopt(argc, argv, "P:p:n:w:m:b:z:o:a:i:W:LEVSTB")) != -1) switch (o) {
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
        default: fprintf(stderr, "usage: hassmic [-P esphome|wyoming] [-p port] [-n name] [-w local|remote] [-m manifest] [-b input-device] [-z port] [-o port] [-a port] [-i port] [-W port] [-L] [-E] [-V] [-S]\n"); return 2;
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
    signal(SIGPIPE, SIG_IGN); signal(SIGCHLD, SIG_IGN); signal(SIGUSR1, on_usr1); signal(SIGUSR2, on_usr2); signal(SIGHUP, on_hup); signal(SIGTTIN, on_ttin); signal(SIGWINCH, on_winch);
    if (access("/system/bin/ledctrl", X_OK)) use_led = 0;
    led("-u", "scone-setup");           /* a restart inside the pairing window: the window is gone, its chaser would loop on */
    led("-u", "factory-reset");         /* a reset asked for before this start is done (main.sh) */
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
        int ok = wake_open_word(&wake_words[wake_active]) == 0;
        if (!ok && wake_engine == WAKE_MWW) {       /* settings_load() switches back to it, and wake_load() off for good */
            fprintf(stderr, "cannot load microWakeWord model %s, Amazon's engine\n", wake_words[wake_active].manifest);
            wake_engine = WAKE_AMAZON; wake_words_scan();
            ok = wake_open_word(&wake_words[wake_active]) == 0;
        }
        if (!ok && (wake_active == 0 || (fprintf(stderr, "cannot load wake word model %s, trying Alexa\n", wake_words[wake_active].manifest), wake_active = 0,
                                         wake_open_word(&wake_words[0]) < 0))) {
            fprintf(stderr, "cannot load wake word model %s\n", wake_words[wake_active].manifest); return 1;
        }
    }
    if (whisper_open(on_whisper) == 0) { atomic_store(&whisper_last, -1); whisper_model = 1; }
    else fprintf(stderr, "whisper: no model, no whisper detection (scripts/artifacts.sh installs it)\n");
    static const struct arb_hooks arb_hooks = { arb_send_key, arb_notify, arb_request, arb_paired, arb_scan, dropin_message };
    if (arb_port && core_local_wake && proto->arb_send && arb_start(arb_port, core_node_name(), &arb_hooks)) fprintf(stderr, "arbitration: not available\n");
    if (dropin_port && arb_running() && dropin_start(dropin_port)) fprintf(stderr, "drop in: not available\n");

    pthread_t cap_t, play_t, ear_t;
    pthread_create(&cap_t, NULL, capture_thread, NULL);
    pthread_create(&play_t, NULL, playback_thread, NULL);
    pthread_create(&ear_t, NULL, earcon_thread, NULL);
    { pthread_t vol_t; pthread_create(&vol_t, NULL, volume_led_thread, NULL); pthread_detach(vol_t); }

    static const struct button_handler buttons = { on_action, on_mute, on_volume, on_pair, on_hold };
    if (buttons_start(input, &buttons) < 0) fprintf(stderr, "buttons: %s not available\n", input);
    else if (buttons_muted()) on_mute(1);

    if (core_sendspin_port) sendspin_start(core_sendspin_port);
    if (use_bt) a2dp_start(NULL);
    static const struct improv_hooks improv_hooks = { web_attention, improv_authorized };
    if (use_bt) improv_start(&improv_hooks);                 /* setup over Bluetooth, when there is no network (improv.c) */
    if (ota_port) ota_start(ota_port);
    pthread_mutex_lock(&core_lock); settings_load(); settings_preset(); pthread_mutex_unlock(&core_lock);    /* whatever the protocol */
    davs_start();                                             /* Amazon login for artifact downloads (settings page) */
    static const struct web_hooks web_hooks = { web_attention, web_approved };
    if (core_web_port && web_start(core_web_port, &web_hooks)) { fprintf(stderr, "web: not available\n"); core_web_port = 0; }

    int ls = net_listen(core_port);
    if (ls < 0) { perror("listen"); return 1; }
    fprintf(stderr, "hassmic " VERSION " (" BUILD ") %s on %d, wake=%s\n", proto->id, core_port, core_local_wake ? "local" : "remote");
    if (ota_port) { pthread_t st; if (!pthread_create(&st, NULL, selftest_thread, NULL)) pthread_detach(st); }
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
