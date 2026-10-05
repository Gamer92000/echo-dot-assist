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
};
extern const struct proto proto_wyoming, proto_esphome;

extern pthread_mutex_t core_lock;               /* guards state, the client socket (writes) and everything marked "lock held" */
extern const char *core_name;
const char *core_node_name(void);               /* "Echo Dot" -> "echo-dot": the ESPHome device (host) name */
extern int core_local_wake, core_port, core_sendspin_port;

/* lock held */
enum state core_state(void);
void core_set_state(enum state s);
void core_link(int connected, int ready);       /* client connection / server wants pipelines.  (0,0) ends a running pipeline */
void core_mic_off(void);                        /* server has heard enough */
int  core_ha_linked(void);                     /* lock held: a client (Home Assistant) is connected */
void core_settings_changed(void);             /* lock held: settings.c changed one; the protocol shows it */
void core_mic_level(int dbfs);                  /* speech level the pipeline gets: micgain.h */
int  core_mic_denoise(int set);                 /* noise reduction on what the pipeline gets (micdenoise.h): 0 off .. 3 high, -1 reads */
void core_pipeline_finish(void);
void core_restart_after(void);                  /* start another pipeline once the current one has finished */
void core_error(void);
/* Mute.  The mute button drives a hardware latch that cuts the mics and lights the button red; software can set that latch
 * but never clear it (privacy guarantee of the gpio-privacy driver, verified on device).  So Home Assistant's switch is a
 * soft mute that can be undone remotely, its state shows latch OR soft mute, and unmuting with the button clears both. */
int  core_soft_mute(int set);                    /* set: 0/1, or -1 to only read */
int  core_muted(void);                           /* effective */
int  core_wake_sound(int set);                   /* same convention */
int  core_bt_announce(int set);                  /* same: chime and "Connected to <name>" when a phone connects */
int  core_bluetooth(int set);                   /* same; -B on the command line: leave the radio to the stock stack */
int  core_dnd(int set);                          /* same: do not disturb, announcements are dropped (the protocol checks) */
int  core_volume(void);
void core_set_volume(int percent);
enum { SPEAKER_NONE, SPEAKER_MIXER, SPEAKER_ABSOLUTE };
void core_speaker(int mode, int pct);            /* not the lock: playing on the Echo's speaker, a Bluetooth speaker with a
                                                    volume of its own through the mixer, or one with absolute volume (pct:
                                                    its own; the mixer at full scale).  The Echo's own volume comes back */
/* Wake word models: the stock "Alexa" plus model sets in the models directory (README, "Another wake word").  The list
 * is fixed after start; Home Assistant picks the active one. */
struct core_wake_word { char id[64], name[64], lang[16], manifest[256]; };
int  core_wake_words(const struct core_wake_word **list);  /* count */
int  core_wake_word(int set);                    /* index of the active one; set >= 0 switches to it and keeps it, -1 reads */
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

/* no lock needed */
void   core_tts_begin(unsigned rate, unsigned channels);   /* lock held for this one: switches to SPEAKING */
void   core_tts_data(const void *pcm, size_t len);
void   core_tts_end(void);                                  /* drains, then played() and core_pipeline_finish() */
int    core_tts_flushing(void);                             /* barge-in: drop audio until core_tts_end() */
void   core_tts_flush(void);
size_t core_tts_queued(void);                               /* bytes not yet played: for back pressure */
void   core_alarm(int on);                                  /* timer finished: ring until button, wake word or 60 s */
enum { MUSIC_SENDSPIN = 1, MUSIC_BLUETOOTH = 2 };
void   core_music(int source, int on);                      /* a music stream runs: the wake word threshold follows */
void   core_bt_device(const char *name, int on);            /* a Bluetooth speaker source connected / went (not the lock) */
void   core_bt_pairing(int on);                             /* the Bluetooth speaker's pairing window opened / closed (not the lock) */
void   core_run(char *const argv[], char *out, size_t n);   /* runs a stock tool the way the core does, until it closes its
                                                               stdout, which lands in out (any thread, not the lock) */
#endif
