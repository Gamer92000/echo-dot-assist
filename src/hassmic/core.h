/* What a protocol module (Wyoming, ESPHome native API) sees of the satellite core in main.c. */
#ifndef CORE_H
#define CORE_H
#include <pthread.h>
#include <stddef.h>

#ifndef VERSION
#define VERSION "0"            /* the Makefile passes the commit's time in UTC, 2026.10.02.091530 */
#endif
#ifndef BUILD
#define BUILD "dev"            /* the Makefile passes git describe */
#endif
#ifndef BUILD_TIME
#define BUILD_TIME __DATE__ " " __TIME__   /* the Makefile passes the commit's time: builds repeat byte for byte */
#endif

enum state { IDLE, LISTENING, THINKING, SPEAKING };

struct proto {
    const char *id;
    int port;                                   /* default; the stock firewall admits inbound TCP 16384-32767 only */
    int threaded;                               /* serve() may run for several clients at once, one thread each */
    void (*serve)(int fd);                      /* one client until it disconnects; no lock held */
    /* all below: core_lock held */
    void (*start)(void);                        /* ask the server to run a pipeline; mic audio follows */
    void (*audio)(const void *pcm, size_t len); /* 16 kHz mono s16le while streaming */
    void (*stop)(void);                         /* may be NULL: pipeline given up while the mic was streaming */
    void (*cancel)(void);                       /* may be NULL: the user cancelled the run (action button); have the server
                                                 * abort it and drop what it still sends for it.  The mic is already off */
    void (*played)(void);                       /* may be NULL: queued playback finished (not called after a flush-less error) */
    void (*volume_changed)(int percent);        /* may be NULL */
    void (*mute_changed)(int muted);            /* may be NULL: effective mute (hardware latch or soft mute) changed */
    void (*print_mdns)(void);                   /* avahi service file on stdout */
    void (*bt_device)(const char *name, int on); /* may be NULL: announce a Bluetooth speaker connection (name may be "") */
    /* may be NULL (then no wake word arbitration): have Home Assistant run "esphome.<node>_arbitration_key" (arb.h).
     * -1: no client runs actions for us */
    int  (*arb_send)(const char *node, const char *network, const char *key);
    void (*arb_changed)(void);                  /* may be NULL: arbitration membership or peers changed */
    void (*sound)(const char *event);           /* may be NULL: sound detection heard one of core_sound_events */
    void (*whispered)(int on);                  /* may be NULL: the request just ended was whispered or not (core_whispered) */
    int  (*arb_request)(const char *entity);    /* may be NULL: ask Home Assistant once for an entity's state (arb.h) */
    void (*settings_changed)(void);             /* may be NULL: a setting changed elsewhere (web page): show them all again */
    void (*entities_changed)(void);             /* may be NULL: a feature went on or off: have the client list entities again */
    int  (*arb_scan)(const char *tag_id);       /* may be NULL: report a tag as scanned to Home Assistant (arb.h) */
    void (*timers_changed)(void);               /* may be NULL: a timer started or stopped ringing (core_timers_ringing) */
    void (*dropin_changed)(int state, const char *peer);   /* may be NULL: a Drop In's state (DROPIN_*), each in turn */
    void (*dropin_result)(const char *result);  /* may be NULL: what came of a call this Echo made (dropin_results) */
    void (*button)(const char *event);          /* may be NULL: a button was pressed or held (core_button_events) */
    void (*wake)(const char *event);            /* may be NULL: the wake word heard (core_wake_events) */
};
extern const struct proto proto_wyoming, proto_esphome;

extern pthread_mutex_t core_lock;               /* guards state, the client socket (writes) and everything marked "lock held" */
extern const char *core_name;                   /* "Echo Dot 2 5695c4": the model and the MAC's end (main.c name_make) */
const char *core_node_name(void);               /* "echo-dot-2-5695c4": the ESPHome device (host) name */
void core_node_of(const char *name, char node[64]);  /* any Echo's node from its name ("Echo Dot 2 5695c4"); a node stays */
void core_identify(void);                       /* rainbow ring for 10 s and a sound: which Echo is this */
/* Setup (OOBE): 1 from an install or a reset until Home Assistant took the Echo on (its voice assistant subscribed:
 * state/adopted); the ring shows stock's setup spinner meanwhile.  Any thread */
int  core_oobe(void);
/* Forget everything: root wipes state/ and the saved Wi-Fi networks and restarts the satellite, which then starts
 * the setup again (state/reset, main.sh).  why: for the log.  Any thread */
void core_reset(const char *why);
/* Restart the whole Echo, as a power cycle does: root reboots on state/reboot (main.sh).  why: for the log.  Any thread */
void core_reboot(const char *why);
extern int core_local_wake, core_port, core_sendspin_port, core_web_port;

/* lock held */
enum state core_state(void);
void core_set_state(enum state s);
void core_link(int connected, int ready);       /* client connection / server wants pipelines.  (0,0) ends a running pipeline */
void core_mic_off(void);                        /* server has heard enough */
int  core_ha_linked(void);                     /* lock held: a client (Home Assistant) is connected */
void core_settings_changed(void);
void core_entities_changed(void);             /* lock held: a feature went on or off: the protocol lists its entities anew */
int  core_whisper_model(void);                 /* a whisper model is loaded */
int  core_sound_failed(void);                  /* sound detection's last start failed (its model did not load); any thread */
void core_whisper_enable(int on);             /* whisper detection runs (with a model) */             /* lock held: settings.c changed one; the protocol shows it */
void core_mic_level(int dbfs);                  /* speech level the pipeline gets: micgain.h */
int  core_wake_sound_removal(int set);           /* our sounds taken out of what the pipeline gets (earsup.h), -1 reads */
int  core_mic_denoise(int set);                 /* noise reduction on what the pipeline gets (micdenoise.h): 0 off .. 3 high, -1 reads */
void core_pipeline_finish(void);
void core_restart_after(void);                  /* start another pipeline once the current one has finished */
void core_error(void);
/* Home Assistant heard no request (stt-no-text-recognized): the turn ends quietly, without core_error's ring */
void core_no_speech(void);
/* Mute.  The mute button drives a hardware latch that cuts the mics and lights the button red; software can set that latch
 * but never clear it (privacy guarantee of the gpio-privacy driver, verified on device).  So Home Assistant's switch is a
 * soft mute that can be undone remotely, its state shows latch OR soft mute, and unmuting with the button clears both. */
int  core_soft_mute(int set);                    /* set: 0/1, or -1 to only read */
int  core_muted(void);                           /* effective */
int  core_wake_sound(int set);                   /* same convention */
int  core_stop_ends_listening(int set);          /* "<wake word>, stop" also ends listening (main.c stop_word), -1 reads */
int  core_bt_announce(int set);                  /* same: chime and "Connected to <name>" when a phone connects */
int  core_bluetooth(int set);                   /* same; -B on the command line: leave the radio to the stock stack */
int  core_dnd(int set);                          /* same: do not disturb, announcements are dropped (the protocol checks) */
int  core_volume(void);
void core_set_volume(int percent);
/* Separate volumes (main.c says how; -1 reads): on/off, and per kind beside the music's (core_volume): its own volume
 * 0..100, whether it follows the music's instead, and by how many points above or below.  Off, all of it is core_volume */
enum { VOL_ASSISTANT, VOL_SOUNDS, VOL_TIMERS, VOL_CALLS, VOL_KINDS };
#define VOL_OFFSET_MAX 100
int  core_separate_volumes(int set);
int  core_kind_volume(int kind, int set);
int  core_kind_linked(int kind, int set);
int  core_kind_offset(int kind);
void core_set_kind_offset(int kind, int offset);    /* -VOL_OFFSET_MAX..VOL_OFFSET_MAX */
enum { SPEAKER_NONE, SPEAKER_MIXER, SPEAKER_ABSOLUTE };
void core_speaker_volume(int pct);              /* lock held: a speaker with absolute volume set by its own buttons */
void core_speaker(int mode, int pct);            /* not the lock: playing on the Echo's speaker, a Bluetooth speaker with a
                                                    volume of its own through the mixer, or one with absolute volume (pct:
                                                    its own; the mixer at full scale).  The Echo's own volume comes back */
/* Wake word models: with Amazon's engine the stock "Alexa" plus model sets in the models directory (README, "Another wake
 * word"), with microWakeWord its models (mww.h).  Listed anew when the engine or the models change; Home Assistant
 * picks the active one, and a second one to listen beside it if it likes (its "Wake word 2").  Lock held. */
#define CORE_WAKE_ACTIVE_MAX 2
struct core_wake_word { char id[64], name[64], lang[16], manifest[256]; };
int  core_wake_words(const struct core_wake_word **list);  /* count */
int  core_wake_word(int set);                    /* index of the (first) active one; set >= 0 makes it the only one and
                                                    keeps it, -1 reads */
int  core_wake_active(int *list);                /* the active ones' indexes, the first first: count (1 or 2) */
int  core_wake_set(const int *list, int n);      /* these active (duplicates and bad indexes dropped, at most 2): -1 if
                                                    none is left (nothing changes) */
int  core_wake_max(void);                        /* how many may listen at once */
const char *core_wake_phrase(void);              /* the name of the one that started the run (the button: the first) */
/* The engine (lock held): Amazon's (the model's own), microWakeWord (models in state/mww, mww.h), or Home Assistant's
 * (the mic streams, it listens: needs a restart either way, asked for here); the wake word list is the engine's.  -1
 * reads; microWakeWord without a model, or a change while hassmic.conf says -w remote, is refused (the engine stays) */
enum { WAKE_AMAZON, WAKE_MWW, WAKE_HA };       /* WAKE_HA: Home Assistant listens in the stream (-w remote) */
int  core_wake_engine(int set);
const char *core_wake_refused(void);             /* lock held: why the last core_wake_engine(set) did not take */
void core_remote_wake(void);                     /* lock held: Home Assistant heard the wake word (WAKE_HA) */
void core_wake_models_changed(void);         /* not the lock: a model was added, changed or deleted (mww_store.c) */
int  core_eq(int band);                          /* speaker equalizer, 0 bass / 1 mid / 2 treble: -6..+6 dB */
void core_set_eq(int band, int db);              /* the mixer keeps it across reboots */
int  core_led_auto(int set);                     /* LED ring follows the light sensor (stock's own engine, the default); -1 reads */
int  core_led_brightness(int set);               /* 0..100; set >= 0 fixes the level (auto off); -1 reads what the ring shows */
float core_lux(void);                            /* light sensor in lux as stock reads it (no lock needed); NAN: none */
/* Sound detection (sound.h): off by default.  The events Home Assistant may get, one per sound the model tells apart. */
int  core_sound(int set);                        /* set: 0/1, or -1 to only read */
int  core_whispered(void);                       /* the last request: 1 whispered, 0 not, -1 none scored yet, -2 no model
                                                  * (whisper.h; no lock needed) */
extern const char *const core_sound_events[];
extern const int core_sound_nevents;
/* The buttons for Home Assistant: a press and a hold (1 s) of each, in buttons.h's order (BUTTON_* * 2 + hold) */
extern const char *const core_button_events[];
extern const int core_button_nevents;
/* The wake word for Home Assistant's "Wake word" event entity (issue #48): accepted, almost (a near miss), answered by
 * another Echo, nothing said after it */
extern const char *const core_wake_events[];
extern const int core_wake_nevents;

/* no lock needed */
/* lock held for this one: switches to SPEAKING.  media: Home Assistant's media, which plays at the main volume (not a
 * reply or an announcement: the Assistant's, with separate volumes) */
void   core_tts_begin(unsigned rate, unsigned channels, int media);
void   core_tts_data(const void *pcm, size_t len);
void   core_tts_end(void);                                  /* drains, then played() and core_pipeline_finish() */
int    core_tts_flushing(void);                             /* barge-in: drop audio until core_tts_end() */
void   core_tts_flush(void);
size_t core_tts_queued(void);                               /* bytes not yet played: for back pressure */
/* Timers (lock held): Home Assistant's finished and cancelled events, by its timer id; a finished one rings until
 * stopped (button, "<wake word>, stop", media stop, HA's "Stop ringing", any request with core_timer_stop_any; not the
 * wake word alone) or the ring time (core_timer_ring) is up */
void   core_timer_finished(const char *id, const char *name, unsigned total);
void   core_timer_cancelled(const char *id);
void   core_alarm_stop(const char *why);                    /* every ringing timer; why: for the log */
int    core_transcript(const char *text);                   /* speech to text's result: 1 if it stopped a ring ("stop",
                                                               or anything with core_timer_stop_any), a Drop In or music
                                                               ("stop"); run cancelled: drop the rest of it */
int    core_timers_ringing(char *names, size_t cap);        /* count; names: "tea, 5 min", in the order they finished */
int    core_timer_ring(int set);                            /* seconds a ring lasts, 0 until stopped; -1 reads */
int    core_timer_stop_any(int set);                        /* 1: any recognised speech stops a ring; -1 reads */
enum { MUSIC_SENDSPIN = 1, MUSIC_BLUETOOTH = 2 };
void   core_music(int source, int on);                      /* a music stream runs: the wake word threshold follows */
void   core_bt_device(const char *name, int on);            /* a Bluetooth speaker source connected / went (not the lock) */
void   core_bt_pairing(int on);                             /* the Bluetooth speaker's pairing window opened / closed (not the lock) */
/* Drop In (dropin.c), not the lock.  core_dropin: the call's state changed (DROPIN_*; peer: the other Echo's node;
 * outgoing: this Echo called): ring, sounds, the front end's listening mode, Home Assistant.  core_dropin_result: what
 * came of a call this Echo made (DROPIN_R_*; here: refused before it started, nothing else told).  core_dropin_refusal:
 * why a call cannot run right now (muted; incoming: do not disturb too), NULL if it can */
void   core_dropin(int state, const char *peer, int outgoing);
void   core_dropin_result(int result, const char *peer, int here);
void   core_dropin_listen(int on);                          /* the front end's listening mode: this side of a call talks */
const char *core_dropin_refusal(int incoming);
void   core_spawn(char *const argv[]);                      /* the same, without waiting for it or its output */
void   core_run(char *const argv[], char *out, size_t n);   /* runs a stock tool the way the core does, until it closes its
                                                               stdout, which lands in out (any thread, not the lock) */
#endif
