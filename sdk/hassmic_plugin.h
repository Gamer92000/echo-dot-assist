/* The reference client of the hassmic plugin protocol in C (sdk/PROTOCOL.md): what a plugin links to speak to the
 * broker.  A plain C header: fixed-width types, no macros that carry logic, no inline functions, so that C++
 * includes it and Rust, Zig or Go bind it.  MIT.
 *
 * A plugin is a static program in a sandbox: no file system, no sockets of its own making, three descriptors (its
 * stderr is hassmic's log, 50 lines a minute; 3 is the control socket).  It calls hm_run() with its callbacks and
 * lives in them: hm_run() says HELLO, hands over the settings and files, and returns when hassmic says STOP.
 * One thread: every callback runs on hm_run()'s.  The calls below may be made from callbacks only.
 *
 * The client needs nothing but the system calls of sdk/filter.def: read, write, poll, recvmsg, send, rt_sigaction.
 * It installs a handler for SIGSYS: with the development flag on (the plugin's card) a call the sandbox refuses
 * raises it, and the handler writes the call's number into the log before the plugin ends (sdk/PROTOCOL.md). */
#ifndef HASSMIC_PLUGIN_H
#define HASSMIC_PLUGIN_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Versions (sdk/PROTOCOL.md "Versions"): the protocol is scopes, core and one per capability, each at a version that
 * only ever adds.  What a plugin speaks is what its manifest's "requires" names, and nothing in its code: the client
 * offers hassmic every scope of its sdk/messages.def at its version (HELLO), hassmic answers with the versions
 * spoken (WELCOME: the manifest's), and the client keeps to those.  hm_scope() says which */

/* capabilities, for hm_granted(), in the manifest's names: storage (0), files, state, log.read,
 * log.read.full, net.server, net.lan, net.internet, net.udp, net.discover, audio.music, audio.volume, call, wake
 * (13), request.audio, request.text, ha.entities, ha.actions, button (18) */
#define HM_CAP_STORAGE    0
#define HM_CAP_FILES      1
#define HM_CAP_STATE      2
#define HM_CAP_LOG_READ   3
#define HM_CAP_LOG_READ_FULL 4
#define HM_CAP_NET_SERVER 5
#define HM_CAP_NET_LAN    6
#define HM_CAP_NET_INTERNET 7
#define HM_CAP_NET_UDP    8
#define HM_CAP_NET_DISCOVER 9
#define HM_CAP_AUDIO_MUSIC 10
#define HM_CAP_AUDIO_VOLUME 11
#define HM_CAP_CALL       12
#define HM_CAP_WAKE       13
#define HM_CAP_REQUEST_AUDIO 14
#define HM_CAP_REQUEST_TEXT 15
#define HM_CAP_HA_ENTITIES 16
#define HM_CAP_HA_ACTIONS 17
#define HM_CAP_BUTTON     18

/* what a request was refused with (hm_connect's *why; ERR_* of the protocol).  HM_ERR_NOT_SPOKEN is the client's own,
 * never hassmic's (0xffff, which messages.def keeps out of ERR_*): the request was not sent, because its message is
 * of a scope, or of a version of one, not spoken with hassmic (WELCOME's versions: its manifest's requires), a
 * request of a capability it does not ask for among them.  Not for a value: one of a later version goes as its
 * fallback, and one of no version is HM_ERR_BAD, as any request that is wrong */
enum { HM_ERR_UNKNOWN = 1, HM_ERR_DENIED, HM_ERR_BAD, HM_ERR_BUSY, HM_ERR_LIMIT, HM_ERR_NOT_FOUND, HM_ERR_FAILED, HM_ERR_UNSET, HM_ERR_NOT_SPOKEN = 0xffff };

struct hm_info {
    int clock_set;                      /* the wall clock is set (it comes from Home Assistant) */
    char model[49], node[65], name[65], zone[65];
};

struct hm_setting { const char *name, *value; };        /* a bool is "0" or "1" */

struct hm_field { const char *name, *value; };
struct hm_file { const char *suffix; uint32_t size; };
struct hm_item {
    const char *id, *title;
    int checked;                        /* the plugin said hm_file_check's 0 to it */
    const struct hm_field *fields; int nfields;     /* the owner's values of the item's fields */
    const struct hm_file *files; int nfiles;
};

/* what file_check answers: a title for the item's row (a wake word model: the phrase; letters, digits and spaces,
 * 40 are kept) and start values of its fields, read out of the file */
struct hm_check {
    char title[65];
    struct { char name[33], value[257]; } fields[8]; int nfields;
    char why[201];                      /* when it returns a code: shown to the owner as text */
};
enum { HM_FILE_BAD_FORMAT = 1, HM_FILE_BAD_UNSUPPORTED, HM_FILE_BAD_TOO_LARGE, HM_FILE_BAD_MISSING, HM_FILE_BAD_OTHER };
enum { HM_WAKE_FAIL_NO_MODEL = 1, HM_WAKE_FAIL_BAD_MODEL, HM_WAKE_FAIL_MEMORY, HM_WAKE_FAIL_UNSUPPORTED, HM_WAKE_FAIL_OTHER };
/* audio.music, state, ha.actions: what a music player does (hm_music_state), what it is told to do (music_cmd) and why, what the Echo does
 * (state), where an action came from */
enum { HM_MUSIC_PLAYING = 1, HM_MUSIC_PAUSED, HM_MUSIC_STOPPED };
enum { HM_MCMD_PAUSE = 1, HM_MCMD_RESUME, HM_MCMD_STOP, HM_MCMD_NEXT, HM_MCMD_PREVIOUS };
enum { HM_MWHY_BUTTON = 1, HM_MWHY_STOP_WORD, HM_MWHY_OTHER_SOURCE, HM_MWHY_HOME_ASSISTANT };
enum { HM_STATE_IDLE = 0, HM_STATE_LISTENING, HM_STATE_THINKING, HM_STATE_SPEAKING };
enum { HM_FROM_HOME_ASSISTANT = 1, HM_FROM_PAGE };
struct hm_arg { const char *name, *value; };            /* an action's argument: an int as its digits */
/* call: why hassmic ended a call (call_hangup), what came of it on the far side (hm_call_end) */
enum { HM_HANGUP_BUTTON = 1, HM_HANGUP_SPOKEN, HM_HANGUP_SCREEN, HM_HANGUP_NOT_ANSWERED, HM_HANGUP_NOT_LIVE, HM_HANGUP_STOPPED };
enum { HM_RESULT_HUNG_UP = 1, HM_RESULT_BUSY, HM_RESULT_DECLINED, HM_RESULT_NO_ANSWER, HM_RESULT_FAILED };
/* a call's frame: 20 ms of 16 kHz mono */
#define HM_CALL_FRAME 320
/* request.audio and request.text: what began a request (request_start), why its audio ended (request_end),
 * which text (request_text) */
enum { HM_REQ_WAKE_WORD = 1, HM_REQ_BUTTON, HM_REQ_FOLLOW_UP, HM_REQ_HA_WAKE };
enum { HM_REQEND_HA = 1, HM_REQEND_HEARD_HERE, HM_REQEND_CANCELLED, HM_REQEND_TIMEOUT, HM_REQEND_ERROR, HM_REQEND_LINK_LOST, HM_REQEND_MUTED };
enum { HM_TEXT_HEARD = 1, HM_TEXT_REPLY };
/* button: which button (button), and how */
enum { HM_BUTTON_ACTION = 1, HM_BUTTON_VOLUME_UP, HM_BUTTON_VOLUME_DOWN };
enum { HM_BUTTON_PRESS = 1, HM_BUTTON_HOLD };

/* Any callback may be NULL.  Strings and arrays are good until the callback returns. */
struct hm_callbacks {
    /* all settings: before ready, and whenever the owner changes one */
    void (*config)(void *ctx, const struct hm_setting *settings, int n);
    /* one list of uploaded items: before ready, and whenever the owner adds, edits or deletes one */
    void (*files)(void *ctx, const char *list, const struct hm_item *items, int n);
    /* the owner uploaded this item: 0 when the plugin can use it (out filled), else HM_FILE_BAD_* and out->why.
     * Its files open with hm_file_open as any item's.  5 s. */
    int (*file_check)(void *ctx, const char *list, const char *item, struct hm_check *out);
    /* settings and files are in: the plugin runs.  hassmic hears READY after this returns */
    void (*ready)(void *ctx);
    /* wake: listen for this word in this slot (0 or 1): 0 when it is loaded, else HM_WAKE_FAIL_*.  5 s. */
    int (*wake_open)(void *ctx, int slot, const char *word);
    void (*wake_close)(void *ctx, int slot);
    /* the audio has a gap (also: a block was lost, see wake_audio's position): forget what was heard */
    void (*wake_reset)(void *ctx);
    /* one block of the microphone stream: 16 kHz mono, position = samples since hassmic's start.  A position that
     * is not the last one plus its count: blocks were lost (the plugin fell behind) or the Echo was muted */
    void (*wake_audio)(void *ctx, uint64_t position, const int16_t *samples, int n);
    /* the clock was set or the zone changed: hm_info() has the new values */
    void (*time)(void *ctx);
    /* hassmic says to end: hm_run returns after this.  2 s, then it is killed.  Also when STOP comes while a request
     * of a callback waits: that request fails (HM_ERR_FAILED) and this runs as soon as that callback returned.  From
     * STOP on only storage writes go through (at once or not at all: nothing is kept for later), and what was kept for
     * later is written as hm_run ends.  STOP comes for an update, the owner's switch, a removal and as hassmic itself
     * ends in order (SIGTERM, an update of hassmic); a crash, a power cut or a SIGKILL end the plugin without it, and
     * this never runs: what must not be lost is written as it goes too (hm_store_put every few seconds), not only
     * here */
    void (*stop)(void *ctx);
    /* log.read: lines of the log, from where hm_log_follow said, whole and in order.  CURSOR: where the next
     * ones begin (keep it with hm_store_put to go on from there after a restart); GAP 1: lines between the cursor
     * asked for and these are gone.  Each line has its time in front and no line end.  The next lines come once this
     * returned (the client answers hassmic then: flow control), so a slow callback holds the log back, never loses it */
    void (*log)(void *ctx, const char *cursor, int gap, const char *const *lines, int n);
    /* about once a second while it runs (from READY on), from hm_run's loop: for what a plugin does by the clock (the
     * first connect and every one after it, send what it gathered).  A tick that takes long delays what hassmic sends
     * (the loop reads it between two ticks), never loses it: keep it short all the same, hassmic ends a plugin that
     * leaves its PINGs unanswered for 30 s */
    void (*tick)(void *ctx);
    /* ---- audio.music: the stream hm_music_open gave takes more now (the loop polls it for writing while
     * this is set, the stream is open and the last hm_music_state said playing): write as much as it takes (it is
     * non-blocking: EAGAIN is "full").  A player that has nothing to write says so (hm_music_state paused or
     * stopped), or the loop calls this again at once */
    void (*music_writable)(void *ctx, int fd);
    /* audio.music: where the stream is, five times a second: FRAMES reached the speaker at AT_US (CLOCK_MONOTONIC,
     * microseconds); QUEUED_US are written and not played yet */
    void (*music_pos)(void *ctx, uint64_t frames, uint64_t at_us, uint32_t queued_us);
    /* audio.music: do this (HM_MCMD_*) to what plays, because of WHY (HM_MWHY_*: the button, the stop word, another
     * source or a call, Home Assistant's media player); say what the player does after it with hm_music_state */
    void (*music_cmd)(void *ctx, int cmd, int why);
    /* audio.volume: the Echo's volume, after ready and whenever it moves */
    void (*volume)(void *ctx, int percent);
    /* state: what the Echo does (HM_STATE_*), whether its microphones are off, its volume: after ready and on change */
    void (*state)(void *ctx, int state, int muted, int volume);
    /* a net capability: the Echo's address and mask (a.b.c.d as (a << 24) | ...; 0 while it has none) */
    void (*net)(void *ctx, uint32_t address, uint32_t mask, int up);
    /* ha.entities: Home Assistant set a switch ("on"/"off"), a number, a select's option, or pressed a button ("");
     * answer with hm_entity_state once it is so */
    void (*entity_set)(void *ctx, const char *name, const char *value);
    /* ha.actions: one of its actions, from Home Assistant or its card's button (HM_FROM_*), with its arguments.  An
     * action the manifest marks "calls" brings a call token: hm_action_token() in here */
    void (*action)(void *ctx, const char *name, int from, const struct hm_arg *args, int n);
    /* ---- call: a person here answered the call that rings (the action button, the Show's screen): answer
     * it on the far side, then hm_call_live */
    void (*call_answer)(void *ctx);
    /* call: hassmic ended the call, in whatever state (HM_HANGUP_*: the button, "stop" or "hang up", the screen, a
     * ring nobody answered, no hm_call_live in time after an answer, the plugin stopped): end it on the far side.  The
     * audio socket is closed by then; no hm_call_end is needed */
    void (*call_hangup)(void *ctx, int why);
    /* call: one frame of the room's microphone while the call is live (20 ms, HM_CALL_FRAME samples, after the
     * Echo's gate, echo suppressor and gain), with its sequence number (from 0; a gap: frames dropped, the plugin
     * fell behind) */
    void (*call_audio)(void *ctx, uint32_t seq, const int16_t *pcm, int n);
    /* ---- request.audio: a request begins (HM_REQ_*: the wake word heard here, the action button, a
     * follow-up Home Assistant asked for, the wake word heard in Home Assistant; SLOT the wake word's, PHRASE what goes
     * to Home Assistant with it).  RUN numbers the request: request_audio and request_end carry it, request_text too.
     * Only the Echo that streams a request has one.  A plugin hears; it never takes part in the request (there is no
     * call that sends anything into it) */
    void (*request_start)(void *ctx, uint32_t run, int source, int slot, const char *phrase);
    /* request.audio: one block of it exactly as Home Assistant got it, 16 kHz mono, the lead-in's 800 ms of zeros at
     * its start included.  OFFSET: samples since the run's first; one that is not the last one plus its count is a gap
     * (blocks dropped: the plugin did not read in time, and Home Assistant lost nothing) */
    void (*request_audio)(void *ctx, uint32_t run, uint32_t offset, const int16_t *pcm, int n);
    /* request.audio: the request's audio ended (HM_REQEND_*); every block of it was handed to request_audio before.
     * SAMPLES what Home Assistant got, DROPPED of them what this plugin did not */
    void (*request_end)(void *ctx, uint32_t run, int why, uint32_t samples, uint32_t dropped);
    /* request.text: what speech to text understood of request RUN (HM_TEXT_HEARD) or the assistant's answer
     * (HM_TEXT_REPLY; Wyoming has none), UTF-8, 1024 bytes at most.  A "stop" the Echo acted on itself is not passed */
    void (*request_text)(void *ctx, uint32_t run, int kind, const char *text);
    /* ---- button: KEY (HM_BUTTON_ACTION, _VOLUME_UP, _VOLUME_DOWN) was pressed (HM_BUTTON_PRESS: let go
     * within a second) or held a second (HM_BUTTON_HOLD: still down), as Home Assistant's "Button" event entity hears
     * it: never a press that served a wait for the button (a login, a plugin's install, a phone's setup ...) nor a
     * hold that served the settings page's note.  The plugin only hears: the button does on the Echo what it does
     * anyway.  Five a second, ten at once; more are dropped */
    void (*button)(void *ctx, int key, int how);
};

/* The plugin's life.  0 when hassmic said to stop, -1 when it refused the plugin or went away. */
int hm_run(const struct hm_callbacks *cb, void *ctx);
const struct hm_info *hm_info(void);
/* the version of scope NAME ("core", a capability's name: "ha.entities") spoken with hassmic: the manifest's
 * requires, from WELCOME on; 0 where it is not spoken */
int hm_scope(const char *name);
/* 1 when the owner accepted capability CAP (HM_CAP_*): its scope is spoken.  (No set of bits: capabilities have no
 * bound, and WELCOME names them as the scopes spoken) */
int hm_granted(int cap);

/* a line into hassmic's log, as "plugin <id>: ..." (printf's format); 50 a minute are kept */
void hm_log(const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

/* files: one file of an item, read-only: its descriptor (read, lseek; it cannot be mapped), -1 */
int hm_file_open(const char *list, const char *item, const char *suffix);
/* the whole file into memory the plugin frees: 0, -1 (missing, larger than max, no memory) */
int hm_file_read(const char *list, const char *item, const char *suffix, size_t max, void **data, size_t *len);

/* wake: the items of LIST the plugin can listen for: after its files arrive, and whenever that changes */
int hm_wake_words(const char *list, const char *const *ids, int n);
/* wake: the slot's word, between two positions of the stream (begin < end, end at most 300 ms behind the stream,
 * 0.3 to 1.5 s long, one a second at most); score 0..255 */
int hm_wake_heard(int slot, uint64_t begin, uint64_t end, int score);

/* storage (capability storage): a value per key ([a-z0-9._-], 1 to 48, not beginning with a dot), up to 16 kB
 * each, within the kB the owner accepted.  The Echo's flash takes ten writes, then one a minute.
 * hm_store_get: 0 and the value (cut to CAP; *len its size), 1 no such key, -1.
 * hm_store_put: 0 written; 1 kept here and written by the client when the Echo takes writes again (a minute at most;
 * only the latest value of a key is kept, 8 keys; one the Echo then refuses for good is said in the plugin's log);
 * -1 not written and not kept: HM_ERR_BUSY in *why when the Echo takes no write now and the client has no room for
 * another key (or no memory), HM_ERR_LIMIT over the kB, else what was wrong.
 * hm_store_del: 0, 1 no such key, -1 */
int hm_store_get(const char *key, void *buf, size_t cap, size_t *len);
int hm_store_put(const char *key, const void *data, size_t len, int *why);
int hm_store_del(const char *key);

/* log.read, log.read.full (official plugins only: the lines as the settings page shows them, what
 * was said included): from now on, the log comes to the log callback.  FROM: a cursor the callback gave before, "start"
 * (the oldest line kept) or "" (from now).  0, or -1 (and the log goes on from where it was) */
int hm_log_follow(const char *from);

/* net.server: a socket to the server the owner entered in the ENTRY'th entry of net.server (from 0), at PORT
 * (0: the entry's own, its only port or the one the owner entered).  Only once the plugin runs: from tick or a
 * callback after ready, never in ready itself (-1, HM_ERR_BAD: hassmic refuses a CONNECT before READY, which is due
 * 5 s after the start and a connect may take 10).  Waits for hassmic: 10 s at most for the whole of it (the name's
 * lookup and every address it has); meanwhile hassmic sends no log lines, and the client answers PING and keeps what
 * else comes for afterwards.  The descriptor, connected and non-blocking (poll before read and write; F_SETFL is not allowed), the
 * plugin's to close; or -1 and *why (HM_ERR_UNSET: the owner has not entered the server; HM_ERR_NOT_FOUND: no address;
 * HM_ERR_FAILED: it did not answer, or hassmic said STOP meanwhile; HM_ERR_DENIED; HM_ERR_BUSY: two under way or thirty in the last minute; HM_ERR_BAD:
 * before READY).  When the owner changes or clears the server, hassmic starts the plugin again: a socket it holds is
 * never to a server the owner no longer means.  For a UDP entry
 * the socket is connected too: send() reaches that server and no other */
int hm_connect(int entry, int port, int *why);

/* ---- net.lan, net.internet */

/* net.lan: a socket to ADDRESS (an IPv4 address on the local network, "a.b.c.d": private or link-local, never a name)
 * by the ENTRY'th entry of net.lan, at PORT (0: the entry's only port).  net.internet: the same to HOST (a name or an
 * IPv4 address, anywhere) by net.internet's entry.  Never loopback, the Echo's own addresses, multicast, broadcast or
 * port 5555.  As hm_connect otherwise: only once the plugin runs, 10 s at most for all of it, the descriptor
 * connected and non-blocking (UDP: send() reaches that address only), or -1 and *why (HM_ERR_DENIED: not an address
 * the entry allows, or a port it does not list; HM_ERR_BAD: no address, a host that is no name or address, port 0
 * where the entry lists more than one, or before READY; HM_ERR_NOT_FOUND, HM_ERR_FAILED, HM_ERR_BUSY as there) */
int hm_connect_lan(int entry, const char *address, int port, int *why);
int hm_connect_internet(int entry, const char *host, int port, int *why);

/* ---- net.udp, net.discover, net.server's proposals, audio.music, audio.volume, ha.entities */

/* audio.music: a stream to the Echo's speaker, s16le at RATE (8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000)
 * and CHANNELS (1, 2): its descriptor (non-blocking; write what it takes, the music_writable callback says when it
 * takes more), or -1 and *why (HM_ERR_BUSY: open already; HM_ERR_LIMIT: ten in a minute; HM_ERR_BAD).  Playing it
 * makes the plugin the newest music source: Sendspin and a phone are paused.  hm_music_state says what the player
 * does (the button, the stop word and Home Assistant's media player go by it); hm_music_close ends the stream (what
 * was written plays out).  A plugin that opens one after another plugin's takes the speaker from it: its stream ends
 * (writes fail with EPIPE) */
int hm_music_open(unsigned rate, int channels, int *why);
int hm_music_state(int state, const char *title, const char *artist);
int hm_music_close(void);
/* audio.volume: set the Echo's volume, 0..100 (five a second) */
int hm_volume_set(int percent);
/* net.udp (KIND HM_CAP_NET_UDP, ENTRY 0 to its count - 1) or net.discover (HM_CAP_NET_DISCOVER, 0): a UDP socket bound
 * to a port of the plugin's own (*port, the same at every start), which only private and link-local senders reach;
 * *handle names it below.  Its descriptor (non-blocking: recv, recvfrom; send once hm_udp_peer fixed a peer), -1 and
 * *why.  hm_udp_peer: send() reaches that address and port only (a private one, not the Echo's own, not 5555).
 * hm_udp_sendto: one datagram to such an address (50 a second; not waited for).  hm_broadcast: the search, to the
 * manifest's port on every network or to its group (once a second; the answers come to the discover socket) */
int hm_udp_open(int kind, int entry, int *handle, int *port, int *why);
int hm_udp_peer(int handle, uint32_t address, int port);
int hm_udp_sendto(int handle, uint32_t address, int port, const void *data, size_t len);
int hm_broadcast(int handle, const void *data, size_t len);
/* what it found, for a host row its net.server names: shown on its card, written only by the owner's click.  A
 * private address as text, one a row a minute */
int hm_propose(const char *setting, const char *value);
/* ha.entities: an entity's state as text ("on"/"off", a number, an option, a text of 255 bytes) and an event entity's
 * event; ten a second for all of them.  Not waited for: a value it cannot take is said in hassmic's log */
int hm_entity_state(const char *name, const char *value);
int hm_event(const char *name, const char *type);
/* ---- calls (capability call; docs/plugins.md 3.6, 4.8).  One call on the Echo at a time.
 * hm_action_token: in the action callback, the call token an action marked "calls" brought (32 hex digits), else "";
 * good for one hm_call_out within 10 s (a call going out, and nothing else): copy it.
 * hm_call_incoming: a call comes in: the Echo rings, with the plugin's name beside WHO (a number, a name: shown as
 * text).  0, or -1 and *why: HM_ERR_BUSY another call is on; HM_ERR_DENIED do not disturb, or the microphones are off;
 * HM_ERR_LIMIT three rings of this plugin unanswered in ten minutes; HM_ERR_BUSY also for a ring asked faster than
 * its looks at the line allow (each ring refused busy costs one, one a second come back, two at most kept; with none
 * left it is refused busy without a look).  Answered only by a person at the Echo (the call_answer callback): never by itself,
 * not from Home Assistant or the page, by no token.
 * hm_call_out: place a call with a token: 0, or -1 and *why (HM_ERR_DENIED: no such token, spent, too old, or the
 * microphones are off; HM_ERR_BUSY).
 * hm_call_ringing_there: the far side rings (for hassmic's log).
 * hm_call_live: the call is connected on the far side (one going out was taken, one coming in was answered here):
 * the room's microphone comes to the call_audio callback, hm_call_audio sends the far side's voice.  TOKEN is not
 * looked at (pass ""). 0, or -1 and *why (HM_ERR_DENIED: a ringing call nobody answered here).
 * hm_call_end: the call is over on the far side, or the plugin ends it (HM_RESULT_*).
 * hm_call_audio: one frame of the far side's voice (HM_CALL_FRAME samples) with its sequence number, as it plays on
 * the Echo's Voip stream (jitter buffer, a missing frame faded): 0, -1 when the socket is full (the frame is dropped:
 * send at the call's pace) or no call is live */
const char *hm_action_token(void);
int hm_call_incoming(const char *who, int *why);
int hm_call_out(const char *token, const char *who, int *why);
int hm_call_ringing_there(void);
int hm_call_live(const char *token, int *why);
int hm_call_end(int result);
int hm_call_audio(uint32_t seq, const int16_t *pcm);

/* Descriptors the loop polls for reading (up to 8: a socket from hm_connect or hm_udp_open): READABLE runs when one
 * has something.  hm_unwatch before closing it */
int hm_watch(int fd, void (*readable)(void *ctx, int fd));
int hm_unwatch(int fd);

/* 16 bytes at a time from the kernel (getrandom): /dev/urandom is not there */
int hm_random(void *out, size_t n);

#ifdef __cplusplus
}
#endif
#endif
