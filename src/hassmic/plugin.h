/* Plugins: programs of their own beside hassmic, each in a sandbox root's launcher makes (src/tools/plugrun.c), with
 * hassmic as their broker: it holds every resource and hands a plugin only what its manifest names and the owner
 * accepted.  docs/plugins.md; the wire format: sdk/plugmsg.h; packages and settings: plugin_store.h.
 *
 * Locks: core_lock may be held when calling in here; nothing in here takes it while holding one of its own. */
#ifndef PLUGIN_H
#define PLUGIN_H
#include <stddef.h>
#include "wake.h"

/* At start, before the wake word engine opens: the launcher's socket (HASSMIC_PLUGIN_FD, closed on exec from here on)
 * and its READY, the plugins installed, the broker thread.  ON: the feature setting `plugins`.  No launcher, a kernel
 * whose filter does not hold, the feature off: no plugin starts, and plugin_why() says which */
void plugin_start(int on);
void plugin_feature(int on);                /* the setting changed: off stops every plugin */
/* Plugins other than the wake engine's start once the self test passed, or 10 s after start where none runs
 * (EXPECTED 0), so that none can be why an update fell back */
void plugin_selftest(int expected);
void plugin_go(void);
/* hassmic ends: STOP to every plugin that runs, and up to MS for them to be gone (their stop callbacks write what they
 * keep).  Nothing starts after it.  Any thread, no lock held; the count of those still there */
int plugin_shutdown(int ms);

/* ---- a plugin as the wake word engine (wake.h; a model named "plugin:<id>/<word>") */
extern const struct wake_engine wake_plugin;
enum { PLUGIN_WAKE_MISSING,     /* no package of it here (or one for a newer hassmic, after a fall back to this one),
                                 * or nothing may run: Amazon's wake word stands in */
       PLUGIN_WAKE_DOWN,        /* installed and not listening: nothing listens, the page warns, tried again */
       PLUGIN_WAKE_UP };        /* it runs and has said which words it can listen for */
int  plugin_wake_state(const char *id);
/* the plugin the core wants as its engine ("": none): started first, a CPU group of its own, never switched off for
 * crashing.  Lock-free for the caller */
void plugin_wake_engine(const char *id);
int  plugin_wake_listening(const char *id);     /* its first slot's word is loaded and heard for */
/* main(), before the engine opens: the plugin started now, and up to MS for it to be up.  The state then */
int  plugin_wake_begin(const char *id, int ms);
/* main(): up to MS for the slot's WAKE_READY: 0 listening, -1 failed or no answer */
int  plugin_wake_wait(int slot, int ms);
struct plugin_word { char id[41], title[65]; };
/* the words of a plugin: what it listed, else (down) the checked items of its file list.  The count */
int  plugin_wake_words(const char *id, struct plugin_word *out, int max);
/* every plugin installed that has `wake`, for the engine setting's choices: ids, PS_ID_MAX apart, as last scanned
 * or (FRESH) as the store's folder has them now.  The count */
int  plugin_wake_engines(char (*ids)[25], int max, int fresh);
/* why the wake plugin does not listen, for the page and the log ("" when it does) */
void plugin_wake_why(const char *id, char *out, size_t cap);
/* the self test (main.c): 0 while a wake plugin that was ready under the version before is not ready under this one
 * (and the owner did not choose that it does not run; one for a newer hassmic, after a fall back, is not waited for).
 * LATE: asked again after the self test's 30 s for the plugin it was held for (its mark gone since is no answer) */
int  plugin_selftest_ok(const char *id, int late);

/* A thread at work on what plugin ID reported (main.c: its list of words read, plugin_words_scan; at start its
 * word opened and its answer waited for): between these two a crash of hassmic's is marked as one in the core on
 * that plugin's behalf, as the broker's thread marks its own calls into the core (plugin.c core_note: counted; the
 * wake engine's start then waits, another plugin is switched off at the fifth).  Only around what is the plugin's
 * own data: what else a start or a scan does (Amazon's models, the core's lists) is not held against a plugin.
 * Any thread, no lock; not nested (leave ends every bracket of its thread) */
void plugin_core_enter(const char *id);
void plugin_core_leave(void);

/* the core's side (main.c): the plugin's words or its state changed; any thread, no lock held */
void core_wake_plugin_changed(const char *id);

/* ---- the settings page (web.c): any thread, no lock held */
size_t plugin_json(char *out, size_t cap);                  /* everything the Plugins page shows */

/* ---- phase 4: music, the Echo's state, Home Assistant's side of plugins (docs/plugins.md 4.7, 4.10) */
/* the core's state for `state` and `audio.volume` plugins (STATE, VOLUME): core_lock held, called on every change */
void plugin_core_state(int state, int muted, int volume);
/* `button`: the button KEY (buttons.h BUTTON_*) pressed, or held a second (HOLD 1), to every plugin that runs
 * with the capability.  Called where Home Assistant's "Button" event is told (main.c button_event), so a press or a
 * hold that served a wait for the button never reaches it.  No lock held */
void plugin_button(int key, int hold);
/* Home Assistant's media player and the plugins' music (4.7): the newest plugin source's state (0 none,
 * PM_MUSIC_PLAYING, PM_MUSIC_PAUSED); a command of HA's to it (PM_MCMD_PAUSE, _RESUME, _STOP), 1 sent; whether a
 * listed plugin has audio.music (the media player offers pause and play then).  Any thread, core_lock or not */
int  plugin_music_state(void);
int  plugin_music_ha(int cmd);
int  plugin_has_music(void);
/* Entities and actions (ha.entities, ha.actions), listed while their plugin is switched on and may run, running or
 * not (4.10).  KIND: PE_* (plugin_store.h), -1 for an action; WORDS: a select's options, an event's types, an
 * action's arguments (IS_INT).  Any thread, core_lock or not; FN is called with the store's lock held */
struct plugin_entity {
    unsigned key; int kind, decimals, n;
    char object_id[64], name[120], icon[65], unit[13], words[12][33];
    double min, max, step; int is_int[4];
};
void plugin_entities_each(void (*fn)(const struct plugin_entity *e, void *arg), void *arg);
int  plugin_entity_value(unsigned key, int *kind, char *out, size_t cap);   /* 1 a state, 0 unavailable, -1 none such */
int  plugin_entity_command(unsigned key, const char *value);                /* Home Assistant set it: 0 told the plugin */
int  plugin_action(unsigned key, char (*args)[256], const int *ints, int nargs);   /* Home Assistant's call (each argument from ARGS or INTS by its type): 0 told */
int  plugin_action_page(const char *id, const char *name);                  /* the card's button: 0 told */
/* a proposal (PROPOSE) the owner takes (DROP 0: written as the row's value, the plugin started again) or drops */
int  plugin_propose_take(const char *id, const char *setting, const char *value, int drop, char *err, size_t errsz);
const char *plugin_why(void);                               /* "" when plugins can run, else why none does */
/* ---- phase 5: a plugin's call (`call`, docs/plugins.md 3.6, 4.8), what a person here does to it (main.c's
 * core_call_button, _answer, _hangup, while call.c's claim is a plugin's).  Any lock but the broker's: core_lock may
 * be held.  The button answers one that rings and ends any other; the screen's Answer answers one that rings; a
 * hang-up (WHY: PM_HANGUP_*) ends any.  1 if there was one */
int  plugin_call_button(void);
int  plugin_call_answer(void);
int  plugin_call_answer_by(const char *how);                /* the same, HOW for the log: "said answer" (main.c core_transcript) */
int  plugin_call_hangup(int why);
/* ---- phase 6: a request's copy (request.audio, request.text; docs/plugins.md 3.5, 3.6, 4.12).  The core's
 * side, core_lock held (main.c, proto_esphome.c, proto_wyoming.c); none of these waits for a plugin: the audio is a
 * send that never waits, the rest is queued for the broker's thread.
 * plugin_request_start: a request begins (its source, the wake word's slot, the phrase Home Assistant gets), as the
 * core opens listening mode for it: the local wake word, the button, a follow-up; with -w remote Home Assistant's wake
 * word (core_remote_wake), never the stream beside it.  plugin_request_audio: a block exactly as proto->audio got it
 * (16 kHz mono s16le), right after it.  plugin_request_end: the microphone off, and why.  plugin_request_text: what
 * speech to text understood (HEARD) or the reply (REPLY), once core_transcript let it through: a "stop" or a bare wake
 * phrase the Echo acted on itself never comes here */
enum { PLUGIN_REQ_WAKE_WORD = 1, PLUGIN_REQ_BUTTON, PLUGIN_REQ_FOLLOW_UP, PLUGIN_REQ_HA_WAKE };
enum { PLUGIN_REQEND_HA = 1, PLUGIN_REQEND_HEARD_HERE, PLUGIN_REQEND_CANCELLED, PLUGIN_REQEND_TIMEOUT, PLUGIN_REQEND_ERROR, PLUGIN_REQEND_LINK_LOST,
       PLUGIN_REQEND_MUTED };
enum { PLUGIN_TEXT_HEARD = 1, PLUGIN_TEXT_REPLY };
void plugin_request_start(int source, int slot, const char *phrase);
void plugin_request_audio(const void *pcm, size_t len);
void plugin_request_end(int why);
void plugin_request_text(int kind, const char *text);
/* a run starts that is no request yet (-w remote's stream, before Home Assistant's wake word): texts from here on are
 * none of the last request's, and are not passed on until the next one begins */
void plugin_request_none(void);
/* hassmic's own crashes with plugins started (2.6) hold plugins back: the one whose message it fell over for a run,
 * at three all but the wake engine's.  The owner switches them on again: the count to 0, what was held started */
void plugin_resume(void);
void plugin_installed(const char *id);                      /* a package went in: started (again) */
int  plugin_enable(const char *id, int on);
int  plugin_remove(const char *id);
/* the plugin index's revocation list changed, or the owner's "run it anyway" (web.c): what it now withdraws is
 * stopped, kept, and its card says why; what a list stopped and that may run again is started.  Any thread, no lock
 * held (plugin_fetch.c after a look, a daily look or a member's list) */
void plugin_withdrawn_changed(void);
int  plugin_feature_on(void);
/* the sandbox a plugin gets on this Echo, as /api/plugins "sandbox" says it: "complete" (the plugins' own SELinux
 * domain), "hassmic" (hassmic's), "su" (none of ours), "none" (no sandbox holds).  Any thread, no lock held */
const char *plugin_sandbox(void);
/* community plugins switched on or off (settings.c, the setting plugins_community, core_lock held; docs/plugins.md 2.7):
 * off, every plugin not listed as official is stopped and kept, and none is installed; on, they run again */
void plugin_community(int on);
/* ID is installed, not official, and community plugins are switched off: 1 a community listing, 2 none that holds
 * (a file's), 0 not so.  Any lock but bl */
int  plugin_community_off_now(const char *id);
/* ID is on the revocation list this Echo has and the owner did not say to run it anyway: 1 and the list's reason.
 * Any lock but bl (it takes ps_lock) */
int  plugin_withdrawn_now(const char *id, char *why, size_t cap);           /* plugins switched on, and a launcher to run them (plugin_fetch.c's daily look) */
void plugin_config_changed(const char *id);
/* no lock held: started again if it runs (WHY into the log): what it holds goes with its process (a server's row
 * changed, web.c) */
void plugin_restart(const char *id, const char *why);
/* the owner's development flag on its card (ps_develop): it takes effect at the plugin's next start, so a running
 * one is started again.  0, or -1 */
int  plugin_develop(const char *id, int on);
/* What it keeps (`storage`) replaced by another Echo's (the page's copy, 5.4: only when the owner ticked it): the
 * plugin stopped first, so that it does not write in between, started again after.  0, or -1 and why */
int  plugin_storage_replace(const char *id, const void *tar, size_t n, char *err, size_t errsz);
void plugin_files_changed(const char *id, const char *list);
/* An item just uploaded (still aside): the plugin's verdict.  1 good (kept, checked), 0 bad (dropped; ERR: the
 * plugin's reason, as text), 2 kept unchecked: the plugin is not running and looks at it when it next does.
 * plugin_item_checked_only: the same, but what the plugin did not look at is dropped (0, ERR says so), never kept
 * unchecked: for the move (web.c mww_migrate), whose answer says what the plugin took */
int  plugin_item_uploaded(const char *id, const char *list, const char *item, char *err, size_t errsz);
int  plugin_item_checked_only(const char *id, const char *list, const char *item, char *err, size_t errsz);
#endif
