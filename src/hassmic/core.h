/* What a protocol module (Wyoming, ESPHome native API) sees of the satellite core in main.c. */
#ifndef CORE_H
#define CORE_H
#include <pthread.h>
#include <stddef.h>

#define VERSION "0.2.0"

enum state { IDLE, LISTENING, THINKING, SPEAKING };

struct proto {
    const char *id;
    int port;                                   /* default; the stock firewall admits inbound TCP 16384-32767 only */
    void (*serve)(int fd);                      /* one client until it disconnects; no lock held */
    /* all below: core_lock held */
    void (*start)(void);                        /* ask the server to run a pipeline; mic audio follows */
    void (*audio)(const void *pcm, size_t len); /* 16 kHz mono s16le while streaming */
    void (*stop)(void);                         /* may be NULL: pipeline given up while the mic was streaming */
    void (*played)(void);                       /* may be NULL: queued playback finished (not called after a flush-less error) */
    void (*volume_changed)(int percent);        /* may be NULL */
    void (*mute_changed)(int muted);            /* may be NULL: effective mute (hardware latch or soft mute) changed */
    void (*print_mdns)(void);                   /* avahi service file on stdout */
};
extern const struct proto proto_wyoming, proto_esphome;

extern pthread_mutex_t core_lock;               /* guards state, the client socket (writes) and everything marked "lock held" */
extern const char *core_name;
extern int core_local_wake, core_port;

/* lock held */
enum state core_state(void);
void core_set_state(enum state s);
void core_link(int connected, int ready);       /* client connection / server wants pipelines.  (0,0) ends a running pipeline */
void core_mic_off(void);                        /* server has heard enough */
void core_pipeline_finish(void);
void core_restart_after(void);                  /* start another pipeline once the current one has finished */
void core_error(void);
/* Mute.  The mute button drives a hardware latch that cuts the mics and lights the button red; software can set that latch
 * but never clear it (privacy guarantee of the gpio-privacy driver, verified on device).  So Home Assistant's switch is a
 * soft mute that can be undone remotely, its state shows latch OR soft mute, and unmuting with the button clears both. */
int  core_soft_mute(int set);                    /* set: 0/1, or -1 to only read */
int  core_muted(void);                           /* effective */
int  core_wake_sound(int set);                   /* same convention */
int  core_volume(void);
void core_set_volume(int percent);

/* no lock needed */
void   core_tts_begin(unsigned rate, unsigned channels);   /* lock held for this one: switches to SPEAKING */
void   core_tts_data(const void *pcm, size_t len);
void   core_tts_end(void);                                  /* drains, then played() and core_pipeline_finish() */
int    core_tts_flushing(void);                             /* barge-in: drop audio until core_tts_end() */
void   core_tts_flush(void);
size_t core_tts_queued(void);                               /* bytes not yet played: for back pressure */
void   core_alarm(int on);                                  /* timer finished: ring until button, wake word or 60 s */
#endif
