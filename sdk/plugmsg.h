/* The broker protocol's wire format (docs/plugins.md 3.1, sdk/PROTOCOL.md): made by tools/mkplugmsg.py from
 * sdk/messages.def, not to be edited.  Header-only, the same file for hassmic's broker and the C client.
 *
 * A packet (SOCK_SEQPACKET, one message each, PM_MAX_PACKET bytes at most): u16 type, u16 flags, u32 id, then the
 * type's fields, little-endian; a string is u16 length + bytes (no NUL), a bytes field the same with any byte.
 * A list is a u16 count where the field stands, and its elements after the last field of the struct it is in (of
 * several lists: in their order).
 *   reading:  pm_head() for the header, pm_get_<msg>() for the fields (0: well-formed; strings point into the
 *             packet and are not terminated: pm_cstr() copies one), pm_next_<msg>_<list>() per list element
 *   writing:  pm_winit(), pm_put_<msg>(), pm_add_<msg>_<list>() once per element counted in the list's n,
 *             pm_wlen(): the packet's length, -1 when something did not fit (never send a cut message)
 * Versions (sdk/PROTOCOL.md "Versions"): each message is part of one or more scopes (PM_SCOPE_*), from a
 * version of each; a message that grew (fields of a later version of its one scope) has readers and writers
 * that take the version spoken (v), the others one layout.  pm_spoken() says whether a type is part of what a
 * peer speaks (VER: the version of each scope spoken, 0 none; NULL: this build's own, PM_SCOPE_VERSION_*).
 * So does every value of an enum (pm_<enum>_ok: a field that holds one holds one the peer speaks, else the packet
 * is malformed; pm_check asks, and so does every sender before a packet goes) and every constant
 * (PM_CONST_SPOKEN).  A packet is exactly its layout: bytes after its last field make it malformed.
 * The manifest's keys have versions too (pm_key: the scope and version a key of a context came with).
 * pm_caps_t is a set of capabilities (PM_CAPBIT(PM_CAP_*)): this build's own, never on the wire. */
#ifndef PLUGMSG_H
#define PLUGMSG_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define PM_MAX_PACKET 32768
#define PM_WAKE_SAMPLES_MAX 4096
#define PM_REQ_SAMPLES_MAX 2048
/* each constant's scope and the version that brought it: PM_CONST_SPOKEN(NAME, VER), whether a peer that speaks
 * VER (NULL: this build) has it.  A constant of a later version applies only to a peer that speaks it */
#define PM_SCOPE_OF_MAX_PACKET PM_SCOPE_CORE
#define PM_SINCE_MAX_PACKET 1
#define PM_SCOPE_OF_WAKE_SAMPLES_MAX PM_SCOPE_WAKE
#define PM_SINCE_WAKE_SAMPLES_MAX 1
#define PM_SCOPE_OF_REQ_SAMPLES_MAX PM_SCOPE_REQUEST_AUDIO
#define PM_SINCE_REQ_SAMPLES_MAX 1
#define PM_CONST_SPOKEN(name, ver) (pm_ver((ver), PM_SCOPE_OF_##name) >= PM_SINCE_##name)
/* a version's words (pm_version_doc) are this many bytes at most; PM_WIDENING versions above 1 widen, of all scopes
 * (what an install dialog lists at most beside the capabilities' lines) */
#define PM_VERSION_DOC_MAX 300
#define PM_WIDENING 0
#define PM_HEADER 8
/* pm_<enum>_for's answer where a value's fallback is none (messages.def `>-`): the packet is not sent to that peer */
#define PM_NOT_SENT UINT64_MAX
#define PM_SCOPES_MAX 255
enum { PM_ERR_UNKNOWN = 1, PM_ERR_DENIED = 2, PM_ERR_BAD = 3, PM_ERR_BUSY = 4, PM_ERR_LIMIT = 5, PM_ERR_NOT_FOUND = 6, PM_ERR_FAILED = 7, PM_ERR_UNSET = 8 };
enum { PM_MUSIC_PLAYING = 1, PM_MUSIC_PAUSED = 2, PM_MUSIC_STOPPED = 3 };
enum { PM_MCMD_PAUSE = 1, PM_MCMD_RESUME = 2, PM_MCMD_STOP = 3, PM_MCMD_NEXT = 4, PM_MCMD_PREVIOUS = 5 };
enum { PM_MWHY_BUTTON = 1, PM_MWHY_STOP_WORD = 2, PM_MWHY_OTHER_SOURCE = 3, PM_MWHY_HOME_ASSISTANT = 4 };
enum { PM_STATE_IDLE = 0, PM_STATE_LISTENING = 1, PM_STATE_THINKING = 2, PM_STATE_SPEAKING = 3 };
enum { PM_FROM_HOME_ASSISTANT = 1, PM_FROM_PAGE = 2 };
enum { PM_HANGUP_BUTTON = 1, PM_HANGUP_SPOKEN = 2, PM_HANGUP_SCREEN = 3, PM_HANGUP_NOT_ANSWERED = 4, PM_HANGUP_NOT_LIVE = 5, PM_HANGUP_STOPPED = 6 };
enum { PM_RESULT_HUNG_UP = 1, PM_RESULT_BUSY = 2, PM_RESULT_DECLINED = 3, PM_RESULT_NO_ANSWER = 4, PM_RESULT_FAILED = 5 };
enum { PM_REFUSED_SCOPES = 1, PM_REFUSED_STATE = 2 };
enum { PM_REQSRC_WAKE_WORD = 1, PM_REQSRC_BUTTON = 2, PM_REQSRC_FOLLOW_UP = 3, PM_REQSRC_HA_WAKE = 4 };
enum { PM_REQEND_HA = 1, PM_REQEND_HEARD_HERE = 2, PM_REQEND_CANCELLED = 3, PM_REQEND_TIMEOUT = 4, PM_REQEND_ERROR = 5, PM_REQEND_LINK_LOST = 6, PM_REQEND_MUTED = 7 };
enum { PM_REQTEXT_HEARD = 1, PM_REQTEXT_REPLY = 2 };
enum { PM_WAKE_FAIL_NO_MODEL = 1, PM_WAKE_FAIL_BAD_MODEL = 2, PM_WAKE_FAIL_MEMORY = 3, PM_WAKE_FAIL_UNSUPPORTED = 4, PM_WAKE_FAIL_OTHER = 5 };
enum { PM_FILE_BAD_FORMAT = 1, PM_FILE_BAD_UNSUPPORTED = 2, PM_FILE_BAD_TOO_LARGE = 3, PM_FILE_BAD_MISSING = 4, PM_FILE_BAD_OTHER = 5 };
enum { PM_BUTTON_KEY_ACTION = 1, PM_BUTTON_KEY_VOLUME_UP = 2, PM_BUTTON_KEY_VOLUME_DOWN = 3 };
enum { PM_BUTTON_HOW_PRESS = 1, PM_BUTTON_HOW_HOLD = 2 };
enum { PM_CAP_STORAGE = 0, PM_CAP_FILES = 1, PM_CAP_STATE = 2, PM_CAP_LOG_READ = 3, PM_CAP_LOG_READ_FULL = 4, PM_CAP_NET_SERVER = 5, PM_CAP_NET_LAN = 6, PM_CAP_NET_INTERNET = 7, PM_CAP_NET_UDP = 8, PM_CAP_NET_DISCOVER = 9, PM_CAP_AUDIO_MUSIC = 10, PM_CAP_AUDIO_VOLUME = 11, PM_CAP_CALL = 12, PM_CAP_WAKE = 13, PM_CAP_REQUEST_AUDIO = 14, PM_CAP_REQUEST_TEXT = 15, PM_CAP_HA_ENTITIES = 16, PM_CAP_HA_ACTIONS = 17, PM_CAP_BUTTON = 18 };
/* a set of capabilities, 1 << PM_CAP_* each: 64 bits (hassmic's and the C client's own, never on the wire:
 * WELCOME names what was granted as the scopes spoken) */
typedef uint64_t pm_caps_t;
#define PM_CAPBIT(c) ((pm_caps_t)1 << (c))
enum { PM_HELLO = 1, PM_WELCOME = 2, PM_REFUSED = 3, PM_CONFIG = 4, PM_READY = 5, PM_PING = 6, PM_PONG = 7, PM_STOP = 8, PM_TIME = 9, PM_OK = 10, PM_ERR = 11, PM_FILES = 20, PM_FILE_OPEN = 21, PM_FILE_CHECK = 22, PM_FILE_OK = 23, PM_FILE_BAD = 24, PM_WAKE_WORDS = 30, PM_WAKE_OPEN = 31, PM_WAKE_READY = 32, PM_WAKE_FAILED = 33, PM_WAKE_CLOSE = 34, PM_WAKE_RESET = 35, PM_WAKE_HEARD = 36, PM_STORE_GET = 40, PM_STORE_VALUE = 41, PM_STORE_PUT = 42, PM_STORE_DEL = 43, PM_LOG_FOLLOW = 50, PM_LOG = 51, PM_CONNECT = 60, PM_UDP_OPEN = 61, PM_UDP_OPENED = 62, PM_UDP_PEER = 63, PM_SENDTO = 64, PM_BROADCAST = 65, PM_PROPOSE = 66, PM_NET = 67, PM_MUSIC_OPEN = 70, PM_MUSIC_STATE = 71, PM_MUSIC_CLOSE = 72, PM_MUSIC_POS = 73, PM_MUSIC_CMD = 74, PM_VOLUME_SET = 75, PM_VOLUME = 76, PM_STATE = 80, PM_ENTITY_STATE = 90, PM_EVENT = 91, PM_ENTITY_SET = 92, PM_ACTION = 93, PM_CALL_INCOMING = 100, PM_CALL_ANSWER = 101, PM_CALL_HANGUP = 102, PM_CALL_OUT = 103, PM_CALL_RINGING_THERE = 104, PM_CALL_LIVE = 105, PM_CALL_END = 106, PM_REQ_START = 110, PM_REQ_END = 111, PM_REQ_TEXT = 112, PM_BUTTON = 120 };
/* the scopes, and the version of each this build has: what it speaks at most */
#define PM_SCOPES 20
enum { PM_SCOPE_CORE = 0, PM_SCOPE_STORAGE = 1, PM_SCOPE_FILES = 2, PM_SCOPE_STATE = 3, PM_SCOPE_LOG_READ = 4, PM_SCOPE_LOG_READ_FULL = 5, PM_SCOPE_NET_SERVER = 6, PM_SCOPE_NET_LAN = 7, PM_SCOPE_NET_INTERNET = 8, PM_SCOPE_NET_UDP = 9, PM_SCOPE_NET_DISCOVER = 10, PM_SCOPE_AUDIO_MUSIC = 11, PM_SCOPE_AUDIO_VOLUME = 12, PM_SCOPE_CALL = 13, PM_SCOPE_WAKE = 14, PM_SCOPE_REQUEST_AUDIO = 15, PM_SCOPE_REQUEST_TEXT = 16, PM_SCOPE_HA_ENTITIES = 17, PM_SCOPE_HA_ACTIONS = 18, PM_SCOPE_BUTTON = 19 };
enum { PM_SCOPE_VERSION_CORE = 1, PM_SCOPE_VERSION_STORAGE = 1, PM_SCOPE_VERSION_FILES = 1, PM_SCOPE_VERSION_STATE = 1, PM_SCOPE_VERSION_LOG_READ = 1, PM_SCOPE_VERSION_LOG_READ_FULL = 1, PM_SCOPE_VERSION_NET_SERVER = 1, PM_SCOPE_VERSION_NET_LAN = 1, PM_SCOPE_VERSION_NET_INTERNET = 1, PM_SCOPE_VERSION_NET_UDP = 1, PM_SCOPE_VERSION_NET_DISCOVER = 1, PM_SCOPE_VERSION_AUDIO_MUSIC = 1, PM_SCOPE_VERSION_AUDIO_VOLUME = 1, PM_SCOPE_VERSION_CALL = 1, PM_SCOPE_VERSION_WAKE = 1, PM_SCOPE_VERSION_REQUEST_AUDIO = 1, PM_SCOPE_VERSION_REQUEST_TEXT = 1, PM_SCOPE_VERSION_HA_ENTITIES = 1, PM_SCOPE_VERSION_HA_ACTIONS = 1, PM_SCOPE_VERSION_BUTTON = 1 };

struct pm_hdr { uint16_t type, flags; uint32_t id; };
struct pm_str { const char *s; uint16_t n; };
struct pm_r { const uint8_t *p, *end; int bad; };
struct pm_list { struct pm_r r; uint16_t n; };
/* fitted: pm_fit made it what one peer speaks (hassmic's psend, the C client's put()): never sent to another */
struct pm_w { uint8_t *b, *p, *end; int over, fitted; };

static inline uint64_t pm_rn(struct pm_r *r, int n)
{
    uint64_t v = 0;
    if (r->bad || r->end - r->p < n) { r->bad = 1; return 0; }
    for (int i = 0; i < n; i++) v |= (uint64_t)r->p[i] << (8 * i);
    r->p += n;
    return v;
}
static inline struct pm_str pm_rstr(struct pm_r *r, unsigned max)
{
    struct pm_str s = { "", 0 }; unsigned n = (unsigned)pm_rn(r, 2);
    if (r->bad || n > max || (size_t)(r->end - r->p) < n || memchr(r->p, 0, n)) { r->bad = 1; return s; }
    s.s = (const char *)r->p; s.n = (uint16_t)n; r->p += n;
    return s;
}
/* bytes: the same without the rule about NUL (a value a plugin keeps; s is not text) */
static inline struct pm_str pm_rbytes(struct pm_r *r, unsigned max)
{
    struct pm_str s = { "", 0 }; unsigned n = (unsigned)pm_rn(r, 2);
    if (r->bad || n > max || (size_t)(r->end - r->p) < n) { r->bad = 1; return s; }
    s.s = (const char *)r->p; s.n = (uint16_t)n; r->p += n;
    return s;
}
/* N bytes of V at P, little-endian (pm_fit: a number rewritten where it stands) */
static inline void pm_wat(uint8_t *p, uint64_t v, int n) { for (int i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static inline void pm_wn(struct pm_w *w, uint64_t v, int n)
{
    if (w->over || w->end - w->p < n) { w->over = 1; return; }
    for (int i = 0; i < n; i++) *w->p++ = (uint8_t)(v >> (8 * i));
}
static inline void pm_wstr(struct pm_w *w, struct pm_str s, unsigned max)
{
    if (s.n > max || (s.n && memchr(s.s, 0, s.n))) { w->over = 1; return; }
    pm_wn(w, s.n, 2);
    if (w->over || (size_t)(w->end - w->p) < s.n) { w->over = 1; return; }
    if (s.n) memcpy(w->p, s.s, s.n);
    w->p += s.n;
}
static inline void pm_wbytes(struct pm_w *w, struct pm_str s, unsigned max)
{
    if (s.n > max) { w->over = 1; return; }
    pm_wn(w, s.n, 2);
    if (w->over || (size_t)(w->end - w->p) < s.n) { w->over = 1; return; }
    if (s.n) memcpy(w->p, s.s, s.n);
    w->p += s.n;
}
/* N bytes at P as a bytes field */
static inline struct pm_str pm_b(const void *p, size_t n) { struct pm_str r = { p ? (const char *)p : "", (uint16_t)(n > 0xffff ? 0xffff : n) }; return r; }
/* a C string as a field */
static inline struct pm_str pm_s(const char *s) { struct pm_str r = { s ? s : "", (uint16_t)(s ? strlen(s) : 0) }; if (s && strlen(s) > 0xffff) r.n = 0xffff; return r; }
/* a field as a C string: 0, or -1 when it does not fit (out is "" then) */
static inline int pm_cstr(char *out, size_t cap, struct pm_str s)
{
    if (!cap) return -1;
    if (s.n >= cap) { out[0] = 0; return -1; }
    if (s.n) memcpy(out, s.s, s.n);
    out[s.n] = 0;
    return 0;
}
static inline int pm_streq(struct pm_str s, const char *c) { size_t n = strlen(c); return s.n == n && !memcmp(s.s, c, n); }
static inline void pm_winit(struct pm_w *w, uint8_t *buf, size_t cap) { w->b = w->p = buf; w->end = buf + (cap > PM_MAX_PACKET ? PM_MAX_PACKET : cap); w->over = w->fitted = 0; }
static inline long pm_wlen(const struct pm_w *w) { return w->over ? -1 : (long)(w->p - w->b); }
static inline void pm_whead(struct pm_w *w, uint16_t type, uint32_t id) { pm_wn(w, type, 2); pm_wn(w, 0, 2); pm_wn(w, id, 4); }
/* the header of a packet: 0 and *body, *blen its fields; -1: too short or too long to be one */
static inline int pm_head(const uint8_t *pkt, size_t len, struct pm_hdr *h, const uint8_t **body, size_t *blen)
{
    struct pm_r r = { pkt, pkt + len, 0 };
    if (len < PM_HEADER || len > PM_MAX_PACKET) return -1;
    h->type = (uint16_t)pm_rn(&r, 2); h->flags = (uint16_t)pm_rn(&r, 2); h->id = (uint32_t)pm_rn(&r, 4);
    *body = r.p; *blen = len - PM_HEADER;
    return 0;
}

/* HELLO (>; core 1): The first message, and the plugin's: every scope its client speaks, each at the highest version it has (its sdk/messages.def's).  The greeting never grows. */
struct pm_hello_scopes { struct pm_str name; uint16_t version; };
struct pm_hello { uint32_t sdk; struct pm_list scopes; };
static inline void pm_skip_hello_scopes(struct pm_r *r) { struct pm_hello_scopes e_; (&e_)->name = pm_rstr(r, 24); (&e_)->version = (uint16_t)pm_rn(r, 2); (void)e_; }
static inline int pm_next_hello_scopes(struct pm_list *l, struct pm_hello_scopes *e) { if (!l->n) return 0; l->n--; e->name = pm_rstr((&l->r), 24); e->version = (uint16_t)pm_rn((&l->r), 2); return !l->r.bad; }
static inline void pm_add_hello_scopes(struct pm_w *w, const struct pm_hello_scopes *e) { pm_wstr(w, e->name, 24); pm_wn(w, e->version, 2); }
static inline int pm_get_hello(const uint8_t *body, size_t len, struct pm_hello *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->sdk = (uint32_t)pm_rn(r, 4); m->scopes.n = (uint16_t)pm_rn(r, 2); if (m->scopes.n > 255) (r)->bad = 1; m->scopes.r = *r; for (unsigned i_ = m->scopes.n; i_ && !(r)->bad; i_--) pm_skip_hello_scopes(r); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_hello(struct pm_w *w, uint32_t id, const struct pm_hello *m) { pm_whead(w, PM_HELLO, id); pm_wn(w, m->sdk, 4); if (m->scopes.n > 255) w->over = 1; pm_wn(w, m->scopes.n, 2); }
/* WELCOME (<; core 1): The answer to HELLO: where it runs, and the version of each scope spoken with it from here on: exactly its manifest's requires (hassmic sends it nothing of a later version, and takes nothing of one), which is also what it was granted (core and the scope of each capability the owner accepted).  The greeting never grows. */
struct pm_welcome_scopes { struct pm_str name; uint16_t version; };
struct pm_welcome { uint32_t clock_set; struct pm_str model; struct pm_str node; struct pm_str name; struct pm_str zone; struct pm_list scopes; };
static inline void pm_skip_welcome_scopes(struct pm_r *r) { struct pm_welcome_scopes e_; (&e_)->name = pm_rstr(r, 24); (&e_)->version = (uint16_t)pm_rn(r, 2); (void)e_; }
static inline int pm_next_welcome_scopes(struct pm_list *l, struct pm_welcome_scopes *e) { if (!l->n) return 0; l->n--; e->name = pm_rstr((&l->r), 24); e->version = (uint16_t)pm_rn((&l->r), 2); return !l->r.bad; }
static inline void pm_add_welcome_scopes(struct pm_w *w, const struct pm_welcome_scopes *e) { pm_wstr(w, e->name, 24); pm_wn(w, e->version, 2); }
static inline int pm_get_welcome(const uint8_t *body, size_t len, struct pm_welcome *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->clock_set = (uint32_t)pm_rn(r, 4); m->model = pm_rstr(r, 48); m->node = pm_rstr(r, 64); m->name = pm_rstr(r, 64); m->zone = pm_rstr(r, 64); m->scopes.n = (uint16_t)pm_rn(r, 2); if (m->scopes.n > 255) (r)->bad = 1; m->scopes.r = *r; for (unsigned i_ = m->scopes.n; i_ && !(r)->bad; i_--) pm_skip_welcome_scopes(r); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_welcome(struct pm_w *w, uint32_t id, const struct pm_welcome *m) { pm_whead(w, PM_WELCOME, id); pm_wn(w, m->clock_set, 4); pm_wstr(w, m->model, 48); pm_wstr(w, m->node, 64); pm_wstr(w, m->name, 64); pm_wstr(w, m->zone, 64); if (m->scopes.n > 255) w->over = 1; pm_wn(w, m->scopes.n, 2); }
/* REFUSED (<; core 1): The answer to a HELLO hassmic cannot serve; the plugin ends.  SCOPES: its client does not speak a scope at the version its manifest requires (HELLO named it lower, or not at all); STATE: a HELLO out of turn.  The greeting never grows. */
struct pm_refused { uint16_t why; };
static inline int pm_get_refused(const uint8_t *body, size_t len, struct pm_refused *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->why = (uint16_t)pm_rn(r, 2); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_refused(struct pm_w *w, uint32_t id, const struct pm_refused *m) { pm_whead(w, PM_REFUSED, id); pm_wn(w, m->why, 2); }
/* CONFIG (<; core 1): The plugin's settings, all of them: after WELCOME and whenever the owner changes one. */
struct pm_config_settings { struct pm_str name; struct pm_str value; };
struct pm_config { struct pm_list settings; };
static inline void pm_skip_config_settings(struct pm_r *r) { struct pm_config_settings e_; (&e_)->name = pm_rstr(r, 32); (&e_)->value = pm_rstr(r, 512); (void)e_; }
static inline int pm_next_config_settings(struct pm_list *l, struct pm_config_settings *e) { if (!l->n) return 0; l->n--; e->name = pm_rstr((&l->r), 32); e->value = pm_rstr((&l->r), 512); return !l->r.bad; }
static inline void pm_add_config_settings(struct pm_w *w, const struct pm_config_settings *e) { pm_wstr(w, e->name, 32); pm_wstr(w, e->value, 512); }
static inline int pm_get_config(const uint8_t *body, size_t len, struct pm_config *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->settings.n = (uint16_t)pm_rn(r, 2); if (m->settings.n > 32) (r)->bad = 1; m->settings.r = *r; for (unsigned i_ = m->settings.n; i_ && !(r)->bad; i_--) pm_skip_config_settings(r); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_config(struct pm_w *w, uint32_t id, const struct pm_config *m) { pm_whead(w, PM_CONFIG, id); if (m->settings.n > 32) w->over = 1; pm_wn(w, m->settings.n, 2); }
/* READY (>; core 1): The plugin has its settings and runs.  Until then hassmic sends it nothing but CONFIG and FILES. */
static inline void pm_put_ready(struct pm_w *w, uint32_t id) { pm_whead(w, PM_READY, id); }
/* PING (<; core 1): Every 10 s; three unanswered and the plugin is killed. */
static inline void pm_put_ping(struct pm_w *w, uint32_t id) { pm_whead(w, PM_PING, id); }
/* PONG (>; core 1): The answer, with PING's id. */
static inline void pm_put_pong(struct pm_w *w, uint32_t id) { pm_whead(w, PM_PONG, id); }
/* STOP (<; core 1): End now: 2 s, then SIGTERM. */
static inline void pm_put_stop(struct pm_w *w, uint32_t id) { pm_whead(w, PM_STOP, id); }
/* TIME (<; core 1): The clock was set, or the time zone changed. */
struct pm_time { uint32_t clock_set; struct pm_str zone; };
static inline int pm_get_time(const uint8_t *body, size_t len, struct pm_time *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->clock_set = (uint32_t)pm_rn(r, 4); m->zone = pm_rstr(r, 64); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_time(struct pm_w *w, uint32_t id, const struct pm_time *m) { pm_whead(w, PM_TIME, id); pm_wn(w, m->clock_set, 4); pm_wstr(w, m->zone, 64); }
/* OK (=; core 1): A request went through (the request's id).  A descriptor the request asked for comes with it.  From the plugin: a LOG handled (the LOG's id). */
static inline void pm_put_ok(struct pm_w *w, uint32_t id) { pm_whead(w, PM_OK, id); }
/* ERR (=; core 1): A request did not (the request's id). */
struct pm_err { uint16_t code; };
static inline int pm_get_err(const uint8_t *body, size_t len, struct pm_err *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->code = (uint16_t)pm_rn(r, 2); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_err(struct pm_w *w, uint32_t id, const struct pm_err *m) { pm_whead(w, PM_ERR, id); pm_wn(w, m->code, 2); }
/* FILES (<; files 1): One list of uploaded items: after CONFIG and whenever the owner adds, edits or deletes one. */
struct pm_files_items_fields { struct pm_str name; struct pm_str value; };
struct pm_files_items_files { struct pm_str name; uint32_t size; };
struct pm_files_items { struct pm_str id; uint8_t checked; struct pm_str title; struct pm_list fields; struct pm_list files; };
struct pm_files { struct pm_str list; struct pm_list items; };
static inline void pm_skip_files_items_fields(struct pm_r *r) { struct pm_files_items_fields e_; (&e_)->name = pm_rstr(r, 32); (&e_)->value = pm_rstr(r, 256); (void)e_; }
static inline int pm_next_files_items_fields(struct pm_list *l, struct pm_files_items_fields *e) { if (!l->n) return 0; l->n--; e->name = pm_rstr((&l->r), 32); e->value = pm_rstr((&l->r), 256); return !l->r.bad; }
static inline void pm_add_files_items_fields(struct pm_w *w, const struct pm_files_items_fields *e) { pm_wstr(w, e->name, 32); pm_wstr(w, e->value, 256); }
static inline void pm_skip_files_items_files(struct pm_r *r) { struct pm_files_items_files e_; (&e_)->name = pm_rstr(r, 64); (&e_)->size = (uint32_t)pm_rn(r, 4); (void)e_; }
static inline int pm_next_files_items_files(struct pm_list *l, struct pm_files_items_files *e) { if (!l->n) return 0; l->n--; e->name = pm_rstr((&l->r), 64); e->size = (uint32_t)pm_rn((&l->r), 4); return !l->r.bad; }
static inline void pm_add_files_items_files(struct pm_w *w, const struct pm_files_items_files *e) { pm_wstr(w, e->name, 64); pm_wn(w, e->size, 4); }
static inline void pm_skip_files_items(struct pm_r *r) { struct pm_files_items e_; (&e_)->id = pm_rstr(r, 40); (&e_)->checked = (uint8_t)pm_rn(r, 1); (&e_)->title = pm_rstr(r, 64); (&e_)->fields.n = (uint16_t)pm_rn(r, 2); if ((&e_)->fields.n > 16) (r)->bad = 1; (&e_)->files.n = (uint16_t)pm_rn(r, 2); if ((&e_)->files.n > 8) (r)->bad = 1; (&e_)->fields.r = *r; for (unsigned i_ = (&e_)->fields.n; i_ && !(r)->bad; i_--) pm_skip_files_items_fields(r); (&e_)->files.r = *r; for (unsigned i_ = (&e_)->files.n; i_ && !(r)->bad; i_--) pm_skip_files_items_files(r); (void)e_; }
static inline int pm_next_files_items(struct pm_list *l, struct pm_files_items *e) { if (!l->n) return 0; l->n--; e->id = pm_rstr((&l->r), 40); e->checked = (uint8_t)pm_rn((&l->r), 1); e->title = pm_rstr((&l->r), 64); e->fields.n = (uint16_t)pm_rn((&l->r), 2); if (e->fields.n > 16) ((&l->r))->bad = 1; e->files.n = (uint16_t)pm_rn((&l->r), 2); if (e->files.n > 8) ((&l->r))->bad = 1; e->fields.r = *(&l->r); for (unsigned i_ = e->fields.n; i_ && !((&l->r))->bad; i_--) pm_skip_files_items_fields((&l->r)); e->files.r = *(&l->r); for (unsigned i_ = e->files.n; i_ && !((&l->r))->bad; i_--) pm_skip_files_items_files((&l->r)); return !l->r.bad; }
static inline void pm_add_files_items(struct pm_w *w, const struct pm_files_items *e) { pm_wstr(w, e->id, 40); pm_wn(w, e->checked, 1); pm_wstr(w, e->title, 64); if (e->fields.n > 16) w->over = 1; pm_wn(w, e->fields.n, 2); if (e->files.n > 8) w->over = 1; pm_wn(w, e->files.n, 2); }
static inline int pm_get_files(const uint8_t *body, size_t len, struct pm_files *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->list = pm_rstr(r, 32); m->items.n = (uint16_t)pm_rn(r, 2); if (m->items.n > 64) (r)->bad = 1; m->items.r = *r; for (unsigned i_ = m->items.n; i_ && !(r)->bad; i_--) pm_skip_files_items(r); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_files(struct pm_w *w, uint32_t id, const struct pm_files *m) { pm_whead(w, PM_FILES, id); pm_wstr(w, m->list, 32); if (m->items.n > 64) w->over = 1; pm_wn(w, m->items.n, 2); }
/* FILE_OPEN (>; files 1): One file of an item: OK comes with a read-only descriptor of it. */
struct pm_file_open { struct pm_str list; struct pm_str item; struct pm_str file; };
static inline int pm_get_file_open(const uint8_t *body, size_t len, struct pm_file_open *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->list = pm_rstr(r, 32); m->item = pm_rstr(r, 40); m->file = pm_rstr(r, 64); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_file_open(struct pm_w *w, uint32_t id, const struct pm_file_open *m) { pm_whead(w, PM_FILE_OPEN, id); pm_wstr(w, m->list, 32); pm_wstr(w, m->item, 40); pm_wstr(w, m->file, 64); }
/* FILE_CHECK (<; files 1): The owner uploaded this item: can the plugin use it?  5 s for FILE_OK or FILE_BAD (this id). */
struct pm_file_check { struct pm_str list; struct pm_str item; };
static inline int pm_get_file_check(const uint8_t *body, size_t len, struct pm_file_check *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->list = pm_rstr(r, 32); m->item = pm_rstr(r, 40); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_file_check(struct pm_w *w, uint32_t id, const struct pm_file_check *m) { pm_whead(w, PM_FILE_CHECK, id); pm_wstr(w, m->list, 32); pm_wstr(w, m->item, 40); }
/* FILE_OK (>; files 1): The item is good. */
struct pm_file_ok_fields { struct pm_str name; struct pm_str value; };
struct pm_file_ok { struct pm_str title; struct pm_list fields; };
static inline void pm_skip_file_ok_fields(struct pm_r *r) { struct pm_file_ok_fields e_; (&e_)->name = pm_rstr(r, 32); (&e_)->value = pm_rstr(r, 256); (void)e_; }
static inline int pm_next_file_ok_fields(struct pm_list *l, struct pm_file_ok_fields *e) { if (!l->n) return 0; l->n--; e->name = pm_rstr((&l->r), 32); e->value = pm_rstr((&l->r), 256); return !l->r.bad; }
static inline void pm_add_file_ok_fields(struct pm_w *w, const struct pm_file_ok_fields *e) { pm_wstr(w, e->name, 32); pm_wstr(w, e->value, 256); }
static inline int pm_get_file_ok(const uint8_t *body, size_t len, struct pm_file_ok *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->title = pm_rstr(r, 64); m->fields.n = (uint16_t)pm_rn(r, 2); if (m->fields.n > 16) (r)->bad = 1; m->fields.r = *r; for (unsigned i_ = m->fields.n; i_ && !(r)->bad; i_--) pm_skip_file_ok_fields(r); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_file_ok(struct pm_w *w, uint32_t id, const struct pm_file_ok *m) { pm_whead(w, PM_FILE_OK, id); pm_wstr(w, m->title, 64); if (m->fields.n > 16) w->over = 1; pm_wn(w, m->fields.n, 2); }
/* FILE_BAD (>; files 1): The item cannot be used; it is not kept. */
struct pm_file_bad { uint16_t code; struct pm_str why; };
static inline int pm_get_file_bad(const uint8_t *body, size_t len, struct pm_file_bad *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->code = (uint16_t)pm_rn(r, 2); m->why = pm_rstr(r, 200); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_file_bad(struct pm_w *w, uint32_t id, const struct pm_file_bad *m) { pm_whead(w, PM_FILE_BAD, id); pm_wn(w, m->code, 2); pm_wstr(w, m->why, 200); }
/* WAKE_WORDS (>; wake 1): The items the plugin can listen for: at start and whenever they change.  Ids only: the phrases are hassmic's, fixed when the owner uploaded each item. */
struct pm_wake_words_words { struct pm_str id; };
struct pm_wake_words { struct pm_str list; struct pm_list words; };
static inline void pm_skip_wake_words_words(struct pm_r *r) { struct pm_wake_words_words e_; (&e_)->id = pm_rstr(r, 40); (void)e_; }
static inline int pm_next_wake_words_words(struct pm_list *l, struct pm_wake_words_words *e) { if (!l->n) return 0; l->n--; e->id = pm_rstr((&l->r), 40); return !l->r.bad; }
static inline void pm_add_wake_words_words(struct pm_w *w, const struct pm_wake_words_words *e) { pm_wstr(w, e->id, 40); }
static inline int pm_get_wake_words(const uint8_t *body, size_t len, struct pm_wake_words *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->list = pm_rstr(r, 32); m->words.n = (uint16_t)pm_rn(r, 2); if (m->words.n > 16) (r)->bad = 1; m->words.r = *r; for (unsigned i_ = m->words.n; i_ && !(r)->bad; i_--) pm_skip_wake_words_words(r); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_wake_words(struct pm_w *w, uint32_t id, const struct pm_wake_words *m) { pm_whead(w, PM_WAKE_WORDS, id); pm_wstr(w, m->list, 32); if (m->words.n > 16) w->over = 1; pm_wn(w, m->words.n, 2); }
/* WAKE_OPEN (<; wake 1): Listen for this word in this slot.  The first one comes with the stream's descriptor.  5 s for WAKE_READY or WAKE_FAILED. */
struct pm_wake_open { uint8_t slot; struct pm_str word; };
static inline int pm_get_wake_open(const uint8_t *body, size_t len, struct pm_wake_open *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->slot = (uint8_t)pm_rn(r, 1); m->word = pm_rstr(r, 40); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_wake_open(struct pm_w *w, uint32_t id, const struct pm_wake_open *m) { pm_whead(w, PM_WAKE_OPEN, id); pm_wn(w, m->slot, 1); pm_wstr(w, m->word, 40); }
/* WAKE_READY (>; wake 1): The slot's word is loaded and listening. */
struct pm_wake_ready { uint8_t slot; };
static inline int pm_get_wake_ready(const uint8_t *body, size_t len, struct pm_wake_ready *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->slot = (uint8_t)pm_rn(r, 1); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_wake_ready(struct pm_w *w, uint32_t id, const struct pm_wake_ready *m) { pm_whead(w, PM_WAKE_READY, id); pm_wn(w, m->slot, 1); }
/* WAKE_FAILED (>; wake 1): The slot's word could not be loaded. */
struct pm_wake_failed { uint8_t slot; uint16_t why; };
static inline int pm_get_wake_failed(const uint8_t *body, size_t len, struct pm_wake_failed *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->slot = (uint8_t)pm_rn(r, 1); m->why = (uint16_t)pm_rn(r, 2); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_wake_failed(struct pm_w *w, uint32_t id, const struct pm_wake_failed *m) { pm_whead(w, PM_WAKE_FAILED, id); pm_wn(w, m->slot, 1); pm_wn(w, m->why, 2); }
/* WAKE_CLOSE (<; wake 1): Stop listening in this slot. */
struct pm_wake_close { uint8_t slot; };
static inline int pm_get_wake_close(const uint8_t *body, size_t len, struct pm_wake_close *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->slot = (uint8_t)pm_rn(r, 1); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_wake_close(struct pm_w *w, uint32_t id, const struct pm_wake_close *m) { pm_whead(w, PM_WAKE_CLOSE, id); pm_wn(w, m->slot, 1); }
/* WAKE_RESET (<; wake 1): The audio has a gap (hassmic played something, the capture restarted): forget what was heard. */
static inline void pm_put_wake_reset(struct pm_w *w, uint32_t id) { pm_whead(w, PM_WAKE_RESET, id); }
/* WAKE_HEARD (>; wake 1): The slot's word, between two positions of the stream.  Checked by hassmic: begin < end, end not ahead of the stream and at most 300 ms behind it, 0.3 to 1.5 s long, one a second. */
struct pm_wake_heard { uint8_t slot; uint8_t score; uint64_t begin; uint64_t end; };
static inline int pm_get_wake_heard(const uint8_t *body, size_t len, struct pm_wake_heard *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->slot = (uint8_t)pm_rn(r, 1); m->score = (uint8_t)pm_rn(r, 1); m->begin = (uint64_t)pm_rn(r, 8); m->end = (uint64_t)pm_rn(r, 8); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_wake_heard(struct pm_w *w, uint32_t id, const struct pm_wake_heard *m) { pm_whead(w, PM_WAKE_HEARD, id); pm_wn(w, m->slot, 1); pm_wn(w, m->score, 1); pm_wn(w, m->begin, 8); pm_wn(w, m->end, 8); }
/* STORE_GET (>; storage 1): The value kept under a key: answered STORE_VALUE (this id), or ERR NOT_FOUND. */
struct pm_store_get { struct pm_str key; };
static inline int pm_get_store_get(const uint8_t *body, size_t len, struct pm_store_get *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->key = pm_rstr(r, 48); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_store_get(struct pm_w *w, uint32_t id, const struct pm_store_get *m) { pm_whead(w, PM_STORE_GET, id); pm_wstr(w, m->key, 48); }
/* STORE_VALUE (<; storage 1): The answer to STORE_GET. */
struct pm_store_value { struct pm_str value; };
static inline int pm_get_store_value(const uint8_t *body, size_t len, struct pm_store_value *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->value = pm_rbytes(r, 16384); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_store_value(struct pm_w *w, uint32_t id, const struct pm_store_value *m) { pm_whead(w, PM_STORE_VALUE, id); pm_wbytes(w, m->value, 16384); }
/* STORE_PUT (>; storage 1): Keep this value under this key, in place of what was there.  OK; ERR LIMIT: over the accepted kB (or 128 keys); ERR BUSY: written too often (10 writes, then one a minute: the Echo's flash), try again later. */
struct pm_store_put { struct pm_str key; struct pm_str value; };
static inline int pm_get_store_put(const uint8_t *body, size_t len, struct pm_store_put *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->key = pm_rstr(r, 48); m->value = pm_rbytes(r, 16384); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_store_put(struct pm_w *w, uint32_t id, const struct pm_store_put *m) { pm_whead(w, PM_STORE_PUT, id); pm_wstr(w, m->key, 48); pm_wbytes(w, m->value, 16384); }
/* STORE_DEL (>; storage 1): The key and its value away (a write as STORE_PUT is).  OK, or ERR NOT_FOUND. */
struct pm_store_del { struct pm_str key; };
static inline int pm_get_store_del(const uint8_t *body, size_t len, struct pm_store_del *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->key = pm_rstr(r, 48); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_store_del(struct pm_w *w, uint32_t id, const struct pm_store_del *m) { pm_whead(w, PM_STORE_DEL, id); pm_wstr(w, m->key, 48); }
/* LOG_FOLLOW (>; log.read 1, log.read.full 1): Send the log from here on (LOG events) until the plugin ends.  OK, or ERR BAD for a cursor that is none.  Asked again: from there instead. */
struct pm_log_follow { struct pm_str from; };
static inline int pm_get_log_follow(const uint8_t *body, size_t len, struct pm_log_follow *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->from = pm_rstr(r, 40); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_log_follow(struct pm_w *w, uint32_t id, const struct pm_log_follow *m) { pm_whead(w, PM_LOG_FOLLOW, id); pm_wstr(w, m->from, 40); }
/* LOG (<; log.read 1, log.read.full 1): Lines of hassmic's log, whole and in order: under log.read without what was said, names and addresses; under log.read.full (official plugins only) as the settings page shows them, what was said included, but for a wake word plugin's lines and the Sendspin pairing token (sdk/PROTOCOL.md).  It has an id: the plugin must answer OK (that id) once it has handled the lines, and the next LOG comes only after that OK (flow control; unanswered for 10 s, hassmic says so).  A plugin that falls behind loses nothing that is still kept. */
struct pm_log_lines { struct pm_str line; };
struct pm_log { struct pm_str cursor; uint8_t gap; struct pm_list lines; };
static inline void pm_skip_log_lines(struct pm_r *r) { struct pm_log_lines e_; (&e_)->line = pm_rstr(r, 1024); (void)e_; }
static inline int pm_next_log_lines(struct pm_list *l, struct pm_log_lines *e) { if (!l->n) return 0; l->n--; e->line = pm_rstr((&l->r), 1024); return !l->r.bad; }
static inline void pm_add_log_lines(struct pm_w *w, const struct pm_log_lines *e) { pm_wstr(w, e->line, 1024); }
static inline int pm_get_log(const uint8_t *body, size_t len, struct pm_log *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->cursor = pm_rstr(r, 40); m->gap = (uint8_t)pm_rn(r, 1); m->lines.n = (uint16_t)pm_rn(r, 2); if (m->lines.n > 64) (r)->bad = 1; m->lines.r = *r; for (unsigned i_ = m->lines.n; i_ && !(r)->bad; i_--) pm_skip_log_lines(r); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_log(struct pm_w *w, uint32_t id, const struct pm_log *m) { pm_whead(w, PM_LOG, id); pm_wstr(w, m->cursor, 40); pm_wn(w, m->gap, 1); if (m->lines.n > 64) w->over = 1; pm_wn(w, m->lines.n, 2); }
/* CONNECT (>; net.server 1, net.lan 1, net.internet 1): A socket by one of the capability's entries: OK comes with it, connected and non-blocking (TCP), or connected (UDP: send() reaches that address only).  The plugin's to keep and close; hassmic keeps no copy.  net.server: the server the owner entered; net.lan: an address on the local network the plugin names (private or link-local, a.b.c.d, never a name), never at DNS's ports 53 and 853; net.internet: an address or name on the internet the plugin names, never one on the local network (also not what a name resolves to).  Never loopback, the Echo's own addresses, multicast, broadcast, 0.0.0.0/8 or port 5555.  ERR DENIED: not that entry's host or port, not the kind's address, not the plugin's capability; UNSET: the owner has not entered the server; NOT_FOUND: its name has no address; FAILED: no connection (10 s for all of it); BUSY: two under way, or 30 in the last minute, or its name's lookup not begun (two of the plugin's own still under way, or the Echo's at its bound); BAD: before READY, no host for net.lan or net.internet, a host that is no name or address, port 0 where the entry has more than one. */
struct pm_connect { uint8_t kind; uint8_t entry; struct pm_str host; uint16_t port; };
static inline int pm_get_connect(const uint8_t *body, size_t len, struct pm_connect *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->kind = (uint8_t)pm_rn(r, 1); m->entry = (uint8_t)pm_rn(r, 1); m->host = pm_rstr(r, 253); m->port = (uint16_t)pm_rn(r, 2); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_connect(struct pm_w *w, uint32_t id, const struct pm_connect *m) { pm_whead(w, PM_CONNECT, id); pm_wn(w, m->kind, 1); pm_wn(w, m->entry, 1); pm_wstr(w, m->host, 253); pm_wn(w, m->port, 2); }
/* UDP_OPEN (>; net.udp 1, net.discover 1): A UDP socket bound to a port of the plugin's own (28940 to 28999, the same at every start): answered UDP_OPENED (this id) with its descriptor.  It hears only private and link-local senders.  kind CAP_NET_UDP: one of the manifest's net.udp count (entry from 0); CAP_NET_DISCOVER: the socket that searches (entry 0), BROADCAST's.  ERR DENIED: not the plugin's capability or entry; BUSY: that one is open; LIMIT: no port left; BAD: before READY. */
struct pm_udp_open { uint8_t kind; uint8_t entry; };
static inline int pm_get_udp_open(const uint8_t *body, size_t len, struct pm_udp_open *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->kind = (uint8_t)pm_rn(r, 1); m->entry = (uint8_t)pm_rn(r, 1); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_udp_open(struct pm_w *w, uint32_t id, const struct pm_udp_open *m) { pm_whead(w, PM_UDP_OPEN, id); pm_wn(w, m->kind, 1); pm_wn(w, m->entry, 1); }
/* UDP_OPENED (<; net.udp 1, net.discover 1): The answer to UDP_OPEN, with the socket (receive with recv or recvfrom; send with send once UDP_PEER fixed its peer, or by SENDTO). */
struct pm_udp_opened { uint8_t handle; uint16_t port; };
static inline int pm_get_udp_opened(const uint8_t *body, size_t len, struct pm_udp_opened *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->handle = (uint8_t)pm_rn(r, 1); m->port = (uint16_t)pm_rn(r, 2); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_udp_opened(struct pm_w *w, uint32_t id, const struct pm_udp_opened *m) { pm_whead(w, PM_UDP_OPENED, id); pm_wn(w, m->handle, 1); pm_wn(w, m->port, 2); }
/* UDP_PEER (>; net.udp 1): Fix the peer of a net.udp socket: send() reaches that address and port only, and only it is heard.  A private or link-local unicast address the Echo routes out of its network interface, never the Echo's own, loopback, port 5555 or DNS's 53 and 853.  A new peer once a second, two at once.  OK; ERR DENIED (the address), BAD (no such handle), BUSY (over the rate). */
struct pm_udp_peer { uint8_t handle; uint32_t address; uint16_t port; };
static inline int pm_get_udp_peer(const uint8_t *body, size_t len, struct pm_udp_peer *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->handle = (uint8_t)pm_rn(r, 1); m->address = (uint32_t)pm_rn(r, 4); m->port = (uint16_t)pm_rn(r, 2); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_udp_peer(struct pm_w *w, uint32_t id, const struct pm_udp_peer *m) { pm_whead(w, PM_UDP_PEER, id); pm_wn(w, m->handle, 1); pm_wn(w, m->address, 4); pm_wn(w, m->port, 2); }
/* SENDTO (>; net.udp 1): One datagram from a net.udp socket to an address as UDP_PEER allows them; sent without waiting (dropped when the socket's queue is full).  50 a second.  An answer only to an id that is not 0: OK, or ERR DENIED, BAD, BUSY (over the rate). */
struct pm_sendto { uint8_t handle; uint32_t address; uint16_t port; struct pm_str data; };
static inline int pm_get_sendto(const uint8_t *body, size_t len, struct pm_sendto *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->handle = (uint8_t)pm_rn(r, 1); m->address = (uint32_t)pm_rn(r, 4); m->port = (uint16_t)pm_rn(r, 2); m->data = pm_rbytes(r, 1472); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_sendto(struct pm_w *w, uint32_t id, const struct pm_sendto *m) { pm_whead(w, PM_SENDTO, id); pm_wn(w, m->handle, 1); pm_wn(w, m->address, 4); pm_wn(w, m->port, 2); pm_wbytes(w, m->data, 1472); }
/* BROADCAST (>; net.discover 1): The search: one datagram from the net.discover socket to the manifest's port, on every network the Echo is on (each one's broadcast address) or to the manifest's multicast group.  Once a second.  Answers come to that socket.  An answer only to an id that is not 0: OK, or ERR BAD, BUSY. */
struct pm_broadcast { uint8_t handle; struct pm_str data; };
static inline int pm_get_broadcast(const uint8_t *body, size_t len, struct pm_broadcast *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->handle = (uint8_t)pm_rn(r, 1); m->data = pm_rbytes(r, 1472); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_broadcast(struct pm_w *w, uint32_t id, const struct pm_broadcast *m) { pm_whead(w, PM_BROADCAST, id); pm_wn(w, m->handle, 1); pm_wbytes(w, m->data, 1472); }
/* PROPOSE (>; net.server 1): What the plugin found, offered to the owner for a host row its net.server names: shown on its card ('Found 192.168.1.20. Use it?'), written only by the owner's click.  A private or link-local IPv4 address as text; one per row at a time, a minute apart.  OK; ERR DENIED (not such a row, not such an address), BUSY (within the minute), BAD. */
struct pm_propose { struct pm_str setting; struct pm_str value; };
static inline int pm_get_propose(const uint8_t *body, size_t len, struct pm_propose *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->setting = pm_rstr(r, 32); m->value = pm_rstr(r, 15); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_propose(struct pm_w *w, uint32_t id, const struct pm_propose *m) { pm_whead(w, PM_PROPOSE, id); pm_wstr(w, m->setting, 32); pm_wstr(w, m->value, 15); }
/* NET (<; net.server 1, net.lan 1, net.internet 1, net.udp 1, net.discover 1): The Echo's address on its network, to a plugin with a net capability: after READY and whenever it changes. */
struct pm_net { uint32_t address; uint32_t mask; uint8_t up; };
static inline int pm_get_net(const uint8_t *body, size_t len, struct pm_net *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->address = (uint32_t)pm_rn(r, 4); m->mask = (uint32_t)pm_rn(r, 4); m->up = (uint8_t)pm_rn(r, 1); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_net(struct pm_w *w, uint32_t id, const struct pm_net *m) { pm_whead(w, PM_NET, id); pm_wn(w, m->address, 4); pm_wn(w, m->mask, 4); pm_wn(w, m->up, 1); }
/* MUSIC_OPEN (>; audio.music 1): Play: OK comes with a stream socket (SOCK_STREAM, non-blocking) into which the plugin writes s16le at this rate and these channels; what it writes plays on the Echo's speaker as music (ducked under the voice assistant, at the music's volume).  The newest music source wins: Sendspin and a phone over Bluetooth are paused.  Rates 8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000; 1 or 2 channels.  ERR BAD: a format it does not take, BUSY: open already (MUSIC_CLOSE first), LIMIT: ten in a minute. */
struct pm_music_open { uint32_t rate; uint8_t channels; };
static inline int pm_get_music_open(const uint8_t *body, size_t len, struct pm_music_open *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->rate = (uint32_t)pm_rn(r, 4); m->channels = (uint8_t)pm_rn(r, 1); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_music_open(struct pm_w *w, uint32_t id, const struct pm_music_open *m) { pm_whead(w, PM_MUSIC_OPEN, id); pm_wn(w, m->rate, 4); pm_wn(w, m->channels, 1); }
/* MUSIC_STATE (>; audio.music 1): What the plugin's player does now: the button, the stop word and Home Assistant's media player go by it.  PLAYING with a stream open makes it the newest source; title and artist go to its card.  An answer only to an id that is not 0. */
struct pm_music_state { uint8_t state; struct pm_str title; struct pm_str artist; };
static inline int pm_get_music_state(const uint8_t *body, size_t len, struct pm_music_state *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->state = (uint8_t)pm_rn(r, 1); m->title = pm_rstr(r, 128); m->artist = pm_rstr(r, 128); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_music_state(struct pm_w *w, uint32_t id, const struct pm_music_state *m) { pm_whead(w, PM_MUSIC_STATE, id); pm_wn(w, m->state, 1); pm_wstr(w, m->title, 128); pm_wstr(w, m->artist, 128); }
/* MUSIC_CLOSE (>; audio.music 1): The stream ends: what was written plays out, then the output closes.  OK. */
static inline void pm_put_music_close(struct pm_w *w, uint32_t id) { pm_whead(w, PM_MUSIC_CLOSE, id); }
/* MUSIC_POS (<; audio.music 1): Where the stream is, five times a second while it is open: what a player needs to say where it is (and to keep a group in step). */
struct pm_music_pos { uint64_t frames; uint64_t at; uint32_t queued; };
static inline int pm_get_music_pos(const uint8_t *body, size_t len, struct pm_music_pos *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->frames = (uint64_t)pm_rn(r, 8); m->at = (uint64_t)pm_rn(r, 8); m->queued = (uint32_t)pm_rn(r, 4); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_music_pos(struct pm_w *w, uint32_t id, const struct pm_music_pos *m) { pm_whead(w, PM_MUSIC_POS, id); pm_wn(w, m->frames, 8); pm_wn(w, m->at, 8); pm_wn(w, m->queued, 4); }
/* MUSIC_CMD (<; audio.music 1): Do this to what plays: from the action button, the stop word, another source that started or a Drop In call (OTHER_SOURCE), Home Assistant's media player. */
struct pm_music_cmd { uint8_t cmd; uint8_t why; };
static inline int pm_get_music_cmd(const uint8_t *body, size_t len, struct pm_music_cmd *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->cmd = (uint8_t)pm_rn(r, 1); m->why = (uint8_t)pm_rn(r, 1); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_music_cmd(struct pm_w *w, uint32_t id, const struct pm_music_cmd *m) { pm_whead(w, PM_MUSIC_CMD, id); pm_wn(w, m->cmd, 1); pm_wn(w, m->why, 1); }
/* VOLUME_SET (>; audio.volume 1): Set the Echo's volume (as the keys do; a burst sets it at once and then to its last value, 500 ms apart at most; the last value is dropped if the volume is set elsewhere before it, and VOLUME says what it is): OK; ERR BUSY over five a second or twenty a minute. */
struct pm_volume_set { uint8_t percent; };
static inline int pm_get_volume_set(const uint8_t *body, size_t len, struct pm_volume_set *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->percent = (uint8_t)pm_rn(r, 1); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_volume_set(struct pm_w *w, uint32_t id, const struct pm_volume_set *m) { pm_whead(w, PM_VOLUME_SET, id); pm_wn(w, m->percent, 1); }
/* VOLUME (<; audio.volume 1): The Echo's volume moved, from wherever: after READY and on every change. */
struct pm_volume { uint8_t percent; };
static inline int pm_get_volume(const uint8_t *body, size_t len, struct pm_volume *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->percent = (uint8_t)pm_rn(r, 1); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_volume(struct pm_w *w, uint32_t id, const struct pm_volume *m) { pm_whead(w, PM_VOLUME, id); pm_wn(w, m->percent, 1); }
/* STATE (<; state 1): What the Echo does: after READY and on every change of any of it.  Never what was said. */
struct pm_state { uint8_t state; uint8_t muted; uint8_t volume; };
static inline int pm_get_state(const uint8_t *body, size_t len, struct pm_state *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->state = (uint8_t)pm_rn(r, 1); m->muted = (uint8_t)pm_rn(r, 1); m->volume = (uint8_t)pm_rn(r, 1); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_state(struct pm_w *w, uint32_t id, const struct pm_state *m) { pm_whead(w, PM_STATE, id); pm_wn(w, m->state, 1); pm_wn(w, m->muted, 1); pm_wn(w, m->volume, 1); }
/* ENTITY_STATE (>; ha.entities 1): An entity's state, as text: a sensor's number, a binary sensor's or switch's on/off, a number within its limits, a select's option, a text sensor's text (255 bytes).  Ten a second for all of the plugin's.  An answer only to an id that is not 0: OK, or ERR BAD (no such entity, a value it cannot take), BUSY. */
struct pm_entity_state { struct pm_str name; struct pm_str value; };
static inline int pm_get_entity_state(const uint8_t *body, size_t len, struct pm_entity_state *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->name = pm_rstr(r, 32); m->value = pm_rstr(r, 255); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_entity_state(struct pm_w *w, uint32_t id, const struct pm_entity_state *m) { pm_whead(w, PM_ENTITY_STATE, id); pm_wstr(w, m->name, 32); pm_wstr(w, m->value, 255); }
/* EVENT (>; ha.entities 1): An event entity fires one of its types.  Counted with ENTITY_STATE. */
struct pm_event { struct pm_str name; struct pm_str type; };
static inline int pm_get_event(const uint8_t *body, size_t len, struct pm_event *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->name = pm_rstr(r, 32); m->type = pm_rstr(r, 32); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_event(struct pm_w *w, uint32_t id, const struct pm_event *m) { pm_whead(w, PM_EVENT, id); pm_wstr(w, m->name, 32); pm_wstr(w, m->type, 32); }
/* ENTITY_SET (<; ha.entities 1): Home Assistant set a switch, number or select of the plugin's, or pressed its button (value empty).  The plugin answers with ENTITY_STATE once it is so. */
struct pm_entity_set { struct pm_str name; struct pm_str value; };
static inline int pm_get_entity_set(const uint8_t *body, size_t len, struct pm_entity_set *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->name = pm_rstr(r, 32); m->value = pm_rstr(r, 255); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_entity_set(struct pm_w *w, uint32_t id, const struct pm_entity_set *m) { pm_whead(w, PM_ENTITY_SET, id); pm_wstr(w, m->name, 32); pm_wstr(w, m->value, 255); }
/* ACTION (<; ha.actions 1): One of the plugin's actions, called by Home Assistant (its keyed link) or by the owner's button on its card. */
struct pm_action_args { struct pm_str name; struct pm_str value; };
struct pm_action { struct pm_str name; uint8_t from; struct pm_str token; struct pm_list args; };
static inline void pm_skip_action_args(struct pm_r *r) { struct pm_action_args e_; (&e_)->name = pm_rstr(r, 32); (&e_)->value = pm_rstr(r, 255); (void)e_; }
static inline int pm_next_action_args(struct pm_list *l, struct pm_action_args *e) { if (!l->n) return 0; l->n--; e->name = pm_rstr((&l->r), 32); e->value = pm_rstr((&l->r), 255); return !l->r.bad; }
static inline void pm_add_action_args(struct pm_w *w, const struct pm_action_args *e) { pm_wstr(w, e->name, 32); pm_wstr(w, e->value, 255); }
static inline int pm_get_action(const uint8_t *body, size_t len, struct pm_action *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->name = pm_rstr(r, 32); m->from = (uint8_t)pm_rn(r, 1); m->token = pm_rstr(r, 32); m->args.n = (uint16_t)pm_rn(r, 2); if (m->args.n > 4) (r)->bad = 1; m->args.r = *r; for (unsigned i_ = m->args.n; i_ && !(r)->bad; i_--) pm_skip_action_args(r); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_action(struct pm_w *w, uint32_t id, const struct pm_action *m) { pm_whead(w, PM_ACTION, id); pm_wstr(w, m->name, 32); pm_wn(w, m->from, 1); pm_wstr(w, m->token, 32); if (m->args.n > 4) w->over = 1; pm_wn(w, m->args.n, 2); }
/* CALL_INCOMING (>; call 1): A call comes in: the Echo rings (its ringtone, its light, on a Show the plugin's name beside WHO) until a person answers here (CALL_ANSWER) or it ends.  OK: it rings; ERR BUSY: another call is on (a Drop In, another plugin's, this one's), or a ring asked faster than its looks at the line allow: each ring refused busy costs one, one a second come back and two at most kept, and with none left it is refused busy without a look; DENIED: do not disturb, or the microphones are off; LIMIT: three rings of this plugin unanswered in the last ten minutes; BAD: before READY. */
struct pm_call_incoming { struct pm_str who; };
static inline int pm_get_call_incoming(const uint8_t *body, size_t len, struct pm_call_incoming *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->who = pm_rstr(r, 64); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_call_incoming(struct pm_w *w, uint32_t id, const struct pm_call_incoming *m) { pm_whead(w, PM_CALL_INCOMING, id); pm_wstr(w, m->who, 64); }
/* CALL_ANSWER (<; call 1): A person here answered the ringing call (the action button, the Show's screen): answer it on the far side, then CALL_LIVE. */
static inline void pm_put_call_answer(struct pm_w *w, uint32_t id) { pm_whead(w, PM_CALL_ANSWER, id); }
/* CALL_HANGUP (<; call 1): hassmic ended the call (any state): end it on the far side.  No CALL_END is needed after it. */
struct pm_call_hangup { uint8_t why; };
static inline int pm_get_call_hangup(const uint8_t *body, size_t len, struct pm_call_hangup *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->why = (uint8_t)pm_rn(r, 1); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_call_hangup(struct pm_w *w, uint32_t id, const struct pm_call_hangup *m) { pm_whead(w, PM_CALL_HANGUP, id); pm_wn(w, m->why, 1); }
/* CALL_OUT (>; call 1): Place a call: only with a token an ACTION marked calls just brought (a person asked for it).  The Echo shows the call going out.  OK; ERR DENIED: no such token, used, older than 10 s, or the microphones are off; BUSY: another call is on; BAD. */
struct pm_call_out { struct pm_str token; struct pm_str who; };
static inline int pm_get_call_out(const uint8_t *body, size_t len, struct pm_call_out *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->token = pm_rstr(r, 32); m->who = pm_rstr(r, 64); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_call_out(struct pm_w *w, uint32_t id, const struct pm_call_out *m) { pm_whead(w, PM_CALL_OUT, id); pm_wstr(w, m->token, 32); pm_wstr(w, m->who, 64); }
/* CALL_RINGING_THERE (>; call 1): The far side rings (a call going out).  For the log.  An answer only to an id that is not 0. */
static inline void pm_put_call_ringing_there(struct pm_w *w, uint32_t id) { pm_whead(w, PM_CALL_RINGING_THERE, id); }
/* CALL_LIVE (>; call 1): The call is connected on the far side: OK comes with the call's audio socket (SOCK_SEQPACKET; each packet a u32 sequence number, then 20 ms of 16 kHz mono s16le, 644 bytes: the room's microphone from hassmic, the far side's voice to hassmic).  An incoming call only once a person answered it here, at the Echo (CALL_ANSWER); no token answers one.  ERR DENIED: not answered here; BAD: no call of this plugin's in that state. */
struct pm_call_live { struct pm_str token; };
static inline int pm_get_call_live(const uint8_t *body, size_t len, struct pm_call_live *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->token = pm_rstr(r, 32); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_call_live(struct pm_w *w, uint32_t id, const struct pm_call_live *m) { pm_whead(w, PM_CALL_LIVE, id); pm_wstr(w, m->token, 32); }
/* CALL_END (>; call 1): The call ended on the far side, or the plugin ends it: in any state.  OK. */
struct pm_call_end { uint8_t result; };
static inline int pm_get_call_end(const uint8_t *body, size_t len, struct pm_call_end *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->result = (uint8_t)pm_rn(r, 1); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_call_end(struct pm_w *w, uint32_t id, const struct pm_call_end *m) { pm_whead(w, PM_CALL_END, id); pm_wn(w, m->result, 1); }
/* REQ_START (<; request.audio 1): A request begins: a copy of what Home Assistant is sent comes on the socket that comes with this message, until REQ_END.  Only the Echo that streams the request has one; with the wake word in Home Assistant (-w remote) from its wake word on.  Nothing can be sent back: the plugin hears, it never takes part. */
struct pm_req_start { uint32_t run; uint8_t source; uint8_t slot; struct pm_str phrase; };
static inline int pm_get_req_start(const uint8_t *body, size_t len, struct pm_req_start *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->run = (uint32_t)pm_rn(r, 4); m->source = (uint8_t)pm_rn(r, 1); m->slot = (uint8_t)pm_rn(r, 1); m->phrase = pm_rstr(r, 64); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_req_start(struct pm_w *w, uint32_t id, const struct pm_req_start *m) { pm_whead(w, PM_REQ_START, id); pm_wn(w, m->run, 4); pm_wn(w, m->source, 1); pm_wn(w, m->slot, 1); pm_wstr(w, m->phrase, 64); }
/* REQ_END (<; request.audio 1): The request's audio ended (the microphone off): the socket has its last block and then its end. */
struct pm_req_end { uint32_t run; uint8_t why; uint32_t samples; uint32_t dropped; };
static inline int pm_get_req_end(const uint8_t *body, size_t len, struct pm_req_end *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->run = (uint32_t)pm_rn(r, 4); m->why = (uint8_t)pm_rn(r, 1); m->samples = (uint32_t)pm_rn(r, 4); m->dropped = (uint32_t)pm_rn(r, 4); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_req_end(struct pm_w *w, uint32_t id, const struct pm_req_end *m) { pm_whead(w, PM_REQ_END, id); pm_wn(w, m->run, 4); pm_wn(w, m->why, 1); pm_wn(w, m->samples, 4); pm_wn(w, m->dropped, 4); }
/* REQ_TEXT (<; request.text 1): What speech to text understood of a request, or the assistant's answer, as Home Assistant reports them (Wyoming: the transcript only).  A bare stop or wake word that the Echo acts on itself is not passed on. */
struct pm_req_text { uint32_t run; uint8_t kind; struct pm_str text; };
static inline int pm_get_req_text(const uint8_t *body, size_t len, struct pm_req_text *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->run = (uint32_t)pm_rn(r, 4); m->kind = (uint8_t)pm_rn(r, 1); m->text = pm_rstr(r, 1024); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_req_text(struct pm_w *w, uint32_t id, const struct pm_req_text *m) { pm_whead(w, PM_REQ_TEXT, id); pm_wn(w, m->run, 4); pm_wn(w, m->kind, 1); pm_wstr(w, m->text, 1024); }
/* BUTTON (<; button 1): A button of the Echo was pressed or held a second: exactly what Home Assistant's \"Button\" event entity hears (the same moment, the same exclusions): never a press that served a wait for the button (a login of the settings page, adb, a plugin's install, a withdrawn plugin run anyway, a phone's setup over Bluetooth), never a hold that served the settings page's note, never a skip of a double or triple press.  The plugin only hears: what the button does on the Echo happens all the same, and nothing it answers changes that.  Five a second, ten at once: more are dropped, and the log counts them. */
struct pm_button { uint8_t key; uint8_t how; };
static inline int pm_get_button(const uint8_t *body, size_t len, struct pm_button *m) { struct pm_r r_ = { body, body + len, 0 }, *r = &r_; m->key = (uint8_t)pm_rn(r, 1); m->how = (uint8_t)pm_rn(r, 1); return r->bad || r->p != r->end ? -1 : 0; }
static inline void pm_put_button(struct pm_w *w, uint32_t id, const struct pm_button *m) { pm_whead(w, PM_BUTTON, id); pm_wn(w, m->key, 1); pm_wn(w, m->how, 1); }

/* the version of scope S that VER speaks (NULL: this build's own) */
static inline unsigned pm_ver(const uint16_t *ver, int s)
{
    static const uint16_t own[PM_SCOPES] = { PM_SCOPE_VERSION_CORE, PM_SCOPE_VERSION_STORAGE, PM_SCOPE_VERSION_FILES, PM_SCOPE_VERSION_STATE, PM_SCOPE_VERSION_LOG_READ, PM_SCOPE_VERSION_LOG_READ_FULL, PM_SCOPE_VERSION_NET_SERVER, PM_SCOPE_VERSION_NET_LAN, PM_SCOPE_VERSION_NET_INTERNET, PM_SCOPE_VERSION_NET_UDP, PM_SCOPE_VERSION_NET_DISCOVER, PM_SCOPE_VERSION_AUDIO_MUSIC, PM_SCOPE_VERSION_AUDIO_VOLUME, PM_SCOPE_VERSION_CALL, PM_SCOPE_VERSION_WAKE, PM_SCOPE_VERSION_REQUEST_AUDIO, PM_SCOPE_VERSION_REQUEST_TEXT, PM_SCOPE_VERSION_HA_ENTITIES, PM_SCOPE_VERSION_HA_ACTIONS, PM_SCOPE_VERSION_BUTTON };
    return s < 0 || s >= PM_SCOPES ? 0 : ver ? ver[s] : own[s];
}
/* the version of scope S this build has (0: none such) */
static inline unsigned pm_scope_version(int s) { return pm_ver(NULL, s); }
/* 1 when TYPE is part of what VER speaks: one of its scopes spoken at a version that has it */
static inline int pm_spoken(uint16_t type, const uint16_t *ver)
{
    (void)ver;
    switch (type) {
    case PM_HELLO: return pm_ver(ver, PM_SCOPE_CORE) >= 1;
    case PM_WELCOME: return pm_ver(ver, PM_SCOPE_CORE) >= 1;
    case PM_REFUSED: return pm_ver(ver, PM_SCOPE_CORE) >= 1;
    case PM_CONFIG: return pm_ver(ver, PM_SCOPE_CORE) >= 1;
    case PM_READY: return pm_ver(ver, PM_SCOPE_CORE) >= 1;
    case PM_PING: return pm_ver(ver, PM_SCOPE_CORE) >= 1;
    case PM_PONG: return pm_ver(ver, PM_SCOPE_CORE) >= 1;
    case PM_STOP: return pm_ver(ver, PM_SCOPE_CORE) >= 1;
    case PM_TIME: return pm_ver(ver, PM_SCOPE_CORE) >= 1;
    case PM_OK: return pm_ver(ver, PM_SCOPE_CORE) >= 1;
    case PM_ERR: return pm_ver(ver, PM_SCOPE_CORE) >= 1;
    case PM_FILES: return pm_ver(ver, PM_SCOPE_FILES) >= 1;
    case PM_FILE_OPEN: return pm_ver(ver, PM_SCOPE_FILES) >= 1;
    case PM_FILE_CHECK: return pm_ver(ver, PM_SCOPE_FILES) >= 1;
    case PM_FILE_OK: return pm_ver(ver, PM_SCOPE_FILES) >= 1;
    case PM_FILE_BAD: return pm_ver(ver, PM_SCOPE_FILES) >= 1;
    case PM_WAKE_WORDS: return pm_ver(ver, PM_SCOPE_WAKE) >= 1;
    case PM_WAKE_OPEN: return pm_ver(ver, PM_SCOPE_WAKE) >= 1;
    case PM_WAKE_READY: return pm_ver(ver, PM_SCOPE_WAKE) >= 1;
    case PM_WAKE_FAILED: return pm_ver(ver, PM_SCOPE_WAKE) >= 1;
    case PM_WAKE_CLOSE: return pm_ver(ver, PM_SCOPE_WAKE) >= 1;
    case PM_WAKE_RESET: return pm_ver(ver, PM_SCOPE_WAKE) >= 1;
    case PM_WAKE_HEARD: return pm_ver(ver, PM_SCOPE_WAKE) >= 1;
    case PM_STORE_GET: return pm_ver(ver, PM_SCOPE_STORAGE) >= 1;
    case PM_STORE_VALUE: return pm_ver(ver, PM_SCOPE_STORAGE) >= 1;
    case PM_STORE_PUT: return pm_ver(ver, PM_SCOPE_STORAGE) >= 1;
    case PM_STORE_DEL: return pm_ver(ver, PM_SCOPE_STORAGE) >= 1;
    case PM_LOG_FOLLOW: return pm_ver(ver, PM_SCOPE_LOG_READ) >= 1 || pm_ver(ver, PM_SCOPE_LOG_READ_FULL) >= 1;
    case PM_LOG: return pm_ver(ver, PM_SCOPE_LOG_READ) >= 1 || pm_ver(ver, PM_SCOPE_LOG_READ_FULL) >= 1;
    case PM_CONNECT: return pm_ver(ver, PM_SCOPE_NET_SERVER) >= 1 || pm_ver(ver, PM_SCOPE_NET_LAN) >= 1 || pm_ver(ver, PM_SCOPE_NET_INTERNET) >= 1;
    case PM_UDP_OPEN: return pm_ver(ver, PM_SCOPE_NET_UDP) >= 1 || pm_ver(ver, PM_SCOPE_NET_DISCOVER) >= 1;
    case PM_UDP_OPENED: return pm_ver(ver, PM_SCOPE_NET_UDP) >= 1 || pm_ver(ver, PM_SCOPE_NET_DISCOVER) >= 1;
    case PM_UDP_PEER: return pm_ver(ver, PM_SCOPE_NET_UDP) >= 1;
    case PM_SENDTO: return pm_ver(ver, PM_SCOPE_NET_UDP) >= 1;
    case PM_BROADCAST: return pm_ver(ver, PM_SCOPE_NET_DISCOVER) >= 1;
    case PM_PROPOSE: return pm_ver(ver, PM_SCOPE_NET_SERVER) >= 1;
    case PM_NET: return pm_ver(ver, PM_SCOPE_NET_SERVER) >= 1 || pm_ver(ver, PM_SCOPE_NET_LAN) >= 1 || pm_ver(ver, PM_SCOPE_NET_INTERNET) >= 1 || pm_ver(ver, PM_SCOPE_NET_UDP) >= 1 || pm_ver(ver, PM_SCOPE_NET_DISCOVER) >= 1;
    case PM_MUSIC_OPEN: return pm_ver(ver, PM_SCOPE_AUDIO_MUSIC) >= 1;
    case PM_MUSIC_STATE: return pm_ver(ver, PM_SCOPE_AUDIO_MUSIC) >= 1;
    case PM_MUSIC_CLOSE: return pm_ver(ver, PM_SCOPE_AUDIO_MUSIC) >= 1;
    case PM_MUSIC_POS: return pm_ver(ver, PM_SCOPE_AUDIO_MUSIC) >= 1;
    case PM_MUSIC_CMD: return pm_ver(ver, PM_SCOPE_AUDIO_MUSIC) >= 1;
    case PM_VOLUME_SET: return pm_ver(ver, PM_SCOPE_AUDIO_VOLUME) >= 1;
    case PM_VOLUME: return pm_ver(ver, PM_SCOPE_AUDIO_VOLUME) >= 1;
    case PM_STATE: return pm_ver(ver, PM_SCOPE_STATE) >= 1;
    case PM_ENTITY_STATE: return pm_ver(ver, PM_SCOPE_HA_ENTITIES) >= 1;
    case PM_EVENT: return pm_ver(ver, PM_SCOPE_HA_ENTITIES) >= 1;
    case PM_ENTITY_SET: return pm_ver(ver, PM_SCOPE_HA_ENTITIES) >= 1;
    case PM_ACTION: return pm_ver(ver, PM_SCOPE_HA_ACTIONS) >= 1;
    case PM_CALL_INCOMING: return pm_ver(ver, PM_SCOPE_CALL) >= 1;
    case PM_CALL_ANSWER: return pm_ver(ver, PM_SCOPE_CALL) >= 1;
    case PM_CALL_HANGUP: return pm_ver(ver, PM_SCOPE_CALL) >= 1;
    case PM_CALL_OUT: return pm_ver(ver, PM_SCOPE_CALL) >= 1;
    case PM_CALL_RINGING_THERE: return pm_ver(ver, PM_SCOPE_CALL) >= 1;
    case PM_CALL_LIVE: return pm_ver(ver, PM_SCOPE_CALL) >= 1;
    case PM_CALL_END: return pm_ver(ver, PM_SCOPE_CALL) >= 1;
    case PM_REQ_START: return pm_ver(ver, PM_SCOPE_REQUEST_AUDIO) >= 1;
    case PM_REQ_END: return pm_ver(ver, PM_SCOPE_REQUEST_AUDIO) >= 1;
    case PM_REQ_TEXT: return pm_ver(ver, PM_SCOPE_REQUEST_TEXT) >= 1;
    case PM_BUTTON: return pm_ver(ver, PM_SCOPE_BUTTON) >= 1;
    }
    return 0;
}
/* V is a value of ERR_* that a peer speaking VER has (NULL: this build) */
static inline int pm_err_ok(uint64_t v, const uint16_t *ver)
{
    (void)ver;
    switch (v) {
    case PM_ERR_UNKNOWN: return pm_ver(ver, PM_SCOPE_CORE) >= 1;
    case PM_ERR_DENIED: return pm_ver(ver, PM_SCOPE_CORE) >= 1;
    case PM_ERR_BAD: return pm_ver(ver, PM_SCOPE_CORE) >= 1;
    case PM_ERR_BUSY: return pm_ver(ver, PM_SCOPE_CORE) >= 1;
    case PM_ERR_LIMIT: return pm_ver(ver, PM_SCOPE_CORE) >= 1;
    case PM_ERR_NOT_FOUND: return pm_ver(ver, PM_SCOPE_CORE) >= 1;
    case PM_ERR_FAILED: return pm_ver(ver, PM_SCOPE_CORE) >= 1;
    case PM_ERR_UNSET: return pm_ver(ver, PM_SCOPE_CORE) >= 1;
    }
    return 0;
}
/* the value of ERR_* a peer speaking VER gets for V: V where it has it, else the fallback of the version before it,
 * PM_NOT_SENT where that is none (messages.def `>-`: the packet is not sent to that peer) (a value no version has: V, which
 * pm_check refuses) */
static inline uint64_t pm_err_for(uint64_t v, const uint16_t *ver)
{
    (void)ver;
    return v;
}
/* V is a value of MUSIC_* that a peer speaking VER has (NULL: this build) */
static inline int pm_music_ok(uint64_t v, const uint16_t *ver)
{
    (void)ver;
    switch (v) {
    case PM_MUSIC_PLAYING: return pm_ver(ver, PM_SCOPE_AUDIO_MUSIC) >= 1;
    case PM_MUSIC_PAUSED: return pm_ver(ver, PM_SCOPE_AUDIO_MUSIC) >= 1;
    case PM_MUSIC_STOPPED: return pm_ver(ver, PM_SCOPE_AUDIO_MUSIC) >= 1;
    }
    return 0;
}
/* the value of MUSIC_* a peer speaking VER gets for V: V where it has it, else the fallback of the version before it,
 * PM_NOT_SENT where that is none (messages.def `>-`: the packet is not sent to that peer) (a value no version has: V, which
 * pm_check refuses) */
static inline uint64_t pm_music_for(uint64_t v, const uint16_t *ver)
{
    (void)ver;
    return v;
}
/* V is a value of MCMD_* that a peer speaking VER has (NULL: this build) */
static inline int pm_mcmd_ok(uint64_t v, const uint16_t *ver)
{
    (void)ver;
    switch (v) {
    case PM_MCMD_PAUSE: return pm_ver(ver, PM_SCOPE_AUDIO_MUSIC) >= 1;
    case PM_MCMD_RESUME: return pm_ver(ver, PM_SCOPE_AUDIO_MUSIC) >= 1;
    case PM_MCMD_STOP: return pm_ver(ver, PM_SCOPE_AUDIO_MUSIC) >= 1;
    case PM_MCMD_NEXT: return pm_ver(ver, PM_SCOPE_AUDIO_MUSIC) >= 1;
    case PM_MCMD_PREVIOUS: return pm_ver(ver, PM_SCOPE_AUDIO_MUSIC) >= 1;
    }
    return 0;
}
/* the value of MCMD_* a peer speaking VER gets for V: V where it has it, else the fallback of the version before it,
 * PM_NOT_SENT where that is none (messages.def `>-`: the packet is not sent to that peer) (a value no version has: V, which
 * pm_check refuses) */
static inline uint64_t pm_mcmd_for(uint64_t v, const uint16_t *ver)
{
    (void)ver;
    return v;
}
/* V is a value of MWHY_* that a peer speaking VER has (NULL: this build) */
static inline int pm_mwhy_ok(uint64_t v, const uint16_t *ver)
{
    (void)ver;
    switch (v) {
    case PM_MWHY_BUTTON: return pm_ver(ver, PM_SCOPE_AUDIO_MUSIC) >= 1;
    case PM_MWHY_STOP_WORD: return pm_ver(ver, PM_SCOPE_AUDIO_MUSIC) >= 1;
    case PM_MWHY_OTHER_SOURCE: return pm_ver(ver, PM_SCOPE_AUDIO_MUSIC) >= 1;
    case PM_MWHY_HOME_ASSISTANT: return pm_ver(ver, PM_SCOPE_AUDIO_MUSIC) >= 1;
    }
    return 0;
}
/* the value of MWHY_* a peer speaking VER gets for V: V where it has it, else the fallback of the version before it,
 * PM_NOT_SENT where that is none (messages.def `>-`: the packet is not sent to that peer) (a value no version has: V, which
 * pm_check refuses) */
static inline uint64_t pm_mwhy_for(uint64_t v, const uint16_t *ver)
{
    (void)ver;
    return v;
}
/* V is a value of STATE_* that a peer speaking VER has (NULL: this build) */
static inline int pm_state_ok(uint64_t v, const uint16_t *ver)
{
    (void)ver;
    switch (v) {
    case PM_STATE_IDLE: return pm_ver(ver, PM_SCOPE_STATE) >= 1;
    case PM_STATE_LISTENING: return pm_ver(ver, PM_SCOPE_STATE) >= 1;
    case PM_STATE_THINKING: return pm_ver(ver, PM_SCOPE_STATE) >= 1;
    case PM_STATE_SPEAKING: return pm_ver(ver, PM_SCOPE_STATE) >= 1;
    }
    return 0;
}
/* the value of STATE_* a peer speaking VER gets for V: V where it has it, else the fallback of the version before it,
 * PM_NOT_SENT where that is none (messages.def `>-`: the packet is not sent to that peer) (a value no version has: V, which
 * pm_check refuses) */
static inline uint64_t pm_state_for(uint64_t v, const uint16_t *ver)
{
    (void)ver;
    return v;
}
/* V is a value of FROM_* that a peer speaking VER has (NULL: this build) */
static inline int pm_from_ok(uint64_t v, const uint16_t *ver)
{
    (void)ver;
    switch (v) {
    case PM_FROM_HOME_ASSISTANT: return pm_ver(ver, PM_SCOPE_HA_ACTIONS) >= 1;
    case PM_FROM_PAGE: return pm_ver(ver, PM_SCOPE_HA_ACTIONS) >= 1;
    }
    return 0;
}
/* the value of FROM_* a peer speaking VER gets for V: V where it has it, else the fallback of the version before it,
 * PM_NOT_SENT where that is none (messages.def `>-`: the packet is not sent to that peer) (a value no version has: V, which
 * pm_check refuses) */
static inline uint64_t pm_from_for(uint64_t v, const uint16_t *ver)
{
    (void)ver;
    return v;
}
/* V is a value of HANGUP_* that a peer speaking VER has (NULL: this build) */
static inline int pm_hangup_ok(uint64_t v, const uint16_t *ver)
{
    (void)ver;
    switch (v) {
    case PM_HANGUP_BUTTON: return pm_ver(ver, PM_SCOPE_CALL) >= 1;
    case PM_HANGUP_SPOKEN: return pm_ver(ver, PM_SCOPE_CALL) >= 1;
    case PM_HANGUP_SCREEN: return pm_ver(ver, PM_SCOPE_CALL) >= 1;
    case PM_HANGUP_NOT_ANSWERED: return pm_ver(ver, PM_SCOPE_CALL) >= 1;
    case PM_HANGUP_NOT_LIVE: return pm_ver(ver, PM_SCOPE_CALL) >= 1;
    case PM_HANGUP_STOPPED: return pm_ver(ver, PM_SCOPE_CALL) >= 1;
    }
    return 0;
}
/* the value of HANGUP_* a peer speaking VER gets for V: V where it has it, else the fallback of the version before it,
 * PM_NOT_SENT where that is none (messages.def `>-`: the packet is not sent to that peer) (a value no version has: V, which
 * pm_check refuses) */
static inline uint64_t pm_hangup_for(uint64_t v, const uint16_t *ver)
{
    (void)ver;
    return v;
}
/* V is a value of RESULT_* that a peer speaking VER has (NULL: this build) */
static inline int pm_result_ok(uint64_t v, const uint16_t *ver)
{
    (void)ver;
    switch (v) {
    case PM_RESULT_HUNG_UP: return pm_ver(ver, PM_SCOPE_CALL) >= 1;
    case PM_RESULT_BUSY: return pm_ver(ver, PM_SCOPE_CALL) >= 1;
    case PM_RESULT_DECLINED: return pm_ver(ver, PM_SCOPE_CALL) >= 1;
    case PM_RESULT_NO_ANSWER: return pm_ver(ver, PM_SCOPE_CALL) >= 1;
    case PM_RESULT_FAILED: return pm_ver(ver, PM_SCOPE_CALL) >= 1;
    }
    return 0;
}
/* the value of RESULT_* a peer speaking VER gets for V: V where it has it, else the fallback of the version before it,
 * PM_NOT_SENT where that is none (messages.def `>-`: the packet is not sent to that peer) (a value no version has: V, which
 * pm_check refuses) */
static inline uint64_t pm_result_for(uint64_t v, const uint16_t *ver)
{
    (void)ver;
    return v;
}
/* V is a value of REFUSED_* that a peer speaking VER has (NULL: this build) */
static inline int pm_refused_ok(uint64_t v, const uint16_t *ver)
{
    (void)ver;
    switch (v) {
    case PM_REFUSED_SCOPES: return pm_ver(ver, PM_SCOPE_CORE) >= 1;
    case PM_REFUSED_STATE: return pm_ver(ver, PM_SCOPE_CORE) >= 1;
    }
    return 0;
}
/* the value of REFUSED_* a peer speaking VER gets for V: V where it has it, else the fallback of the version before it,
 * PM_NOT_SENT where that is none (messages.def `>-`: the packet is not sent to that peer) (a value no version has: V, which
 * pm_check refuses) */
static inline uint64_t pm_refused_for(uint64_t v, const uint16_t *ver)
{
    (void)ver;
    return v;
}
/* V is a value of REQSRC_* that a peer speaking VER has (NULL: this build) */
static inline int pm_reqsrc_ok(uint64_t v, const uint16_t *ver)
{
    (void)ver;
    switch (v) {
    case PM_REQSRC_WAKE_WORD: return pm_ver(ver, PM_SCOPE_REQUEST_AUDIO) >= 1;
    case PM_REQSRC_BUTTON: return pm_ver(ver, PM_SCOPE_REQUEST_AUDIO) >= 1;
    case PM_REQSRC_FOLLOW_UP: return pm_ver(ver, PM_SCOPE_REQUEST_AUDIO) >= 1;
    case PM_REQSRC_HA_WAKE: return pm_ver(ver, PM_SCOPE_REQUEST_AUDIO) >= 1;
    }
    return 0;
}
/* the value of REQSRC_* a peer speaking VER gets for V: V where it has it, else the fallback of the version before it,
 * PM_NOT_SENT where that is none (messages.def `>-`: the packet is not sent to that peer) (a value no version has: V, which
 * pm_check refuses) */
static inline uint64_t pm_reqsrc_for(uint64_t v, const uint16_t *ver)
{
    (void)ver;
    return v;
}
/* V is a value of REQEND_* that a peer speaking VER has (NULL: this build) */
static inline int pm_reqend_ok(uint64_t v, const uint16_t *ver)
{
    (void)ver;
    switch (v) {
    case PM_REQEND_HA: return pm_ver(ver, PM_SCOPE_REQUEST_AUDIO) >= 1;
    case PM_REQEND_HEARD_HERE: return pm_ver(ver, PM_SCOPE_REQUEST_AUDIO) >= 1;
    case PM_REQEND_CANCELLED: return pm_ver(ver, PM_SCOPE_REQUEST_AUDIO) >= 1;
    case PM_REQEND_TIMEOUT: return pm_ver(ver, PM_SCOPE_REQUEST_AUDIO) >= 1;
    case PM_REQEND_ERROR: return pm_ver(ver, PM_SCOPE_REQUEST_AUDIO) >= 1;
    case PM_REQEND_LINK_LOST: return pm_ver(ver, PM_SCOPE_REQUEST_AUDIO) >= 1;
    case PM_REQEND_MUTED: return pm_ver(ver, PM_SCOPE_REQUEST_AUDIO) >= 1;
    }
    return 0;
}
/* the value of REQEND_* a peer speaking VER gets for V: V where it has it, else the fallback of the version before it,
 * PM_NOT_SENT where that is none (messages.def `>-`: the packet is not sent to that peer) (a value no version has: V, which
 * pm_check refuses) */
static inline uint64_t pm_reqend_for(uint64_t v, const uint16_t *ver)
{
    (void)ver;
    return v;
}
/* V is a value of REQTEXT_* that a peer speaking VER has (NULL: this build) */
static inline int pm_reqtext_ok(uint64_t v, const uint16_t *ver)
{
    (void)ver;
    switch (v) {
    case PM_REQTEXT_HEARD: return pm_ver(ver, PM_SCOPE_REQUEST_TEXT) >= 1;
    case PM_REQTEXT_REPLY: return pm_ver(ver, PM_SCOPE_REQUEST_TEXT) >= 1;
    }
    return 0;
}
/* the value of REQTEXT_* a peer speaking VER gets for V: V where it has it, else the fallback of the version before it,
 * PM_NOT_SENT where that is none (messages.def `>-`: the packet is not sent to that peer) (a value no version has: V, which
 * pm_check refuses) */
static inline uint64_t pm_reqtext_for(uint64_t v, const uint16_t *ver)
{
    (void)ver;
    return v;
}
/* V is a value of WAKE_FAIL_* that a peer speaking VER has (NULL: this build) */
static inline int pm_wake_fail_ok(uint64_t v, const uint16_t *ver)
{
    (void)ver;
    switch (v) {
    case PM_WAKE_FAIL_NO_MODEL: return pm_ver(ver, PM_SCOPE_WAKE) >= 1;
    case PM_WAKE_FAIL_BAD_MODEL: return pm_ver(ver, PM_SCOPE_WAKE) >= 1;
    case PM_WAKE_FAIL_MEMORY: return pm_ver(ver, PM_SCOPE_WAKE) >= 1;
    case PM_WAKE_FAIL_UNSUPPORTED: return pm_ver(ver, PM_SCOPE_WAKE) >= 1;
    case PM_WAKE_FAIL_OTHER: return pm_ver(ver, PM_SCOPE_WAKE) >= 1;
    }
    return 0;
}
/* the value of WAKE_FAIL_* a peer speaking VER gets for V: V where it has it, else the fallback of the version before it,
 * PM_NOT_SENT where that is none (messages.def `>-`: the packet is not sent to that peer) (a value no version has: V, which
 * pm_check refuses) */
static inline uint64_t pm_wake_fail_for(uint64_t v, const uint16_t *ver)
{
    (void)ver;
    return v;
}
/* V is a value of FILE_BAD_* that a peer speaking VER has (NULL: this build) */
static inline int pm_file_bad_ok(uint64_t v, const uint16_t *ver)
{
    (void)ver;
    switch (v) {
    case PM_FILE_BAD_FORMAT: return pm_ver(ver, PM_SCOPE_FILES) >= 1;
    case PM_FILE_BAD_UNSUPPORTED: return pm_ver(ver, PM_SCOPE_FILES) >= 1;
    case PM_FILE_BAD_TOO_LARGE: return pm_ver(ver, PM_SCOPE_FILES) >= 1;
    case PM_FILE_BAD_MISSING: return pm_ver(ver, PM_SCOPE_FILES) >= 1;
    case PM_FILE_BAD_OTHER: return pm_ver(ver, PM_SCOPE_FILES) >= 1;
    }
    return 0;
}
/* the value of FILE_BAD_* a peer speaking VER gets for V: V where it has it, else the fallback of the version before it,
 * PM_NOT_SENT where that is none (messages.def `>-`: the packet is not sent to that peer) (a value no version has: V, which
 * pm_check refuses) */
static inline uint64_t pm_file_bad_for(uint64_t v, const uint16_t *ver)
{
    (void)ver;
    return v;
}
/* V is a value of BUTTON_KEY_* that a peer speaking VER has (NULL: this build) */
static inline int pm_button_key_ok(uint64_t v, const uint16_t *ver)
{
    (void)ver;
    switch (v) {
    case PM_BUTTON_KEY_ACTION: return pm_ver(ver, PM_SCOPE_BUTTON) >= 1;
    case PM_BUTTON_KEY_VOLUME_UP: return pm_ver(ver, PM_SCOPE_BUTTON) >= 1;
    case PM_BUTTON_KEY_VOLUME_DOWN: return pm_ver(ver, PM_SCOPE_BUTTON) >= 1;
    }
    return 0;
}
/* the value of BUTTON_KEY_* a peer speaking VER gets for V: V where it has it, else the fallback of the version before it,
 * PM_NOT_SENT where that is none (messages.def `>-`: the packet is not sent to that peer) (a value no version has: V, which
 * pm_check refuses) */
static inline uint64_t pm_button_key_for(uint64_t v, const uint16_t *ver)
{
    (void)ver;
    return v;
}
/* V is a value of BUTTON_HOW_* that a peer speaking VER has (NULL: this build) */
static inline int pm_button_how_ok(uint64_t v, const uint16_t *ver)
{
    (void)ver;
    switch (v) {
    case PM_BUTTON_HOW_PRESS: return pm_ver(ver, PM_SCOPE_BUTTON) >= 1;
    case PM_BUTTON_HOW_HOLD: return pm_ver(ver, PM_SCOPE_BUTTON) >= 1;
    }
    return 0;
}
/* the value of BUTTON_HOW_* a peer speaking VER gets for V: V where it has it, else the fallback of the version before it,
 * PM_NOT_SENT where that is none (messages.def `>-`: the packet is not sent to that peer) (a value no version has: V, which
 * pm_check refuses) */
static inline uint64_t pm_button_how_for(uint64_t v, const uint16_t *ver)
{
    (void)ver;
    return v;
}
/* V is a capability of this build's (a CAP_* value names a capability: any client that knows it may name it, and
 * the broker answers DENIED where it is not the plugin's) */
static inline int pm_cap_ok(uint64_t v, const uint16_t *ver)
{
    (void)ver;
    switch (v) {
    case PM_CAP_STORAGE: return 1;
    case PM_CAP_FILES: return 1;
    case PM_CAP_STATE: return 1;
    case PM_CAP_LOG_READ: return 1;
    case PM_CAP_LOG_READ_FULL: return 1;
    case PM_CAP_NET_SERVER: return 1;
    case PM_CAP_NET_LAN: return 1;
    case PM_CAP_NET_INTERNET: return 1;
    case PM_CAP_NET_UDP: return 1;
    case PM_CAP_NET_DISCOVER: return 1;
    case PM_CAP_AUDIO_MUSIC: return 1;
    case PM_CAP_AUDIO_VOLUME: return 1;
    case PM_CAP_CALL: return 1;
    case PM_CAP_WAKE: return 1;
    case PM_CAP_REQUEST_AUDIO: return 1;
    case PM_CAP_REQUEST_TEXT: return 1;
    case PM_CAP_HA_ENTITIES: return 1;
    case PM_CAP_HA_ACTIONS: return 1;
    case PM_CAP_BUTTON: return 1;
    }
    return 0;
}
/* A packet's fields against its type, every list walked to its end and nothing after it, and every value of an
 * enum one VER has: 0 well-formed, 1 not,
 * -1 a type this build does not know or VER does not speak (answered ERR_UNKNOWN, never a reason to end) */
static inline int pm_check(uint16_t type, const uint8_t *body, size_t len, const uint16_t *ver)
{
    if (!pm_spoken(type, ver)) return -1;
    switch (type) {
    case PM_HELLO: { struct pm_hello m; if (pm_get_hello(body, len, &m)) return 1;  { struct pm_hello_scopes e_scopes; while (pm_next_hello_scopes(&m.scopes, &e_scopes)) {   } } return 0; }
    case PM_WELCOME: { struct pm_welcome m; if (pm_get_welcome(body, len, &m)) return 1;  { struct pm_welcome_scopes e_scopes; while (pm_next_welcome_scopes(&m.scopes, &e_scopes)) {   } } return 0; }
    case PM_REFUSED: { struct pm_refused m; if (pm_get_refused(body, len, &m)) return 1; if (!pm_refused_ok(m.why, ver)) return 1;  return 0; }
    case PM_CONFIG: { struct pm_config m; if (pm_get_config(body, len, &m)) return 1;  { struct pm_config_settings e_settings; while (pm_next_config_settings(&m.settings, &e_settings)) {   } } return 0; }
    case PM_READY: (void)body; return len ? 1 : 0;
    case PM_PING: (void)body; return len ? 1 : 0;
    case PM_PONG: (void)body; return len ? 1 : 0;
    case PM_STOP: (void)body; return len ? 1 : 0;
    case PM_TIME: { struct pm_time m; if (pm_get_time(body, len, &m)) return 1;   return 0; }
    case PM_OK: (void)body; return len ? 1 : 0;
    case PM_ERR: { struct pm_err m; if (pm_get_err(body, len, &m)) return 1; if (!pm_err_ok(m.code, ver)) return 1;  return 0; }
    case PM_FILES: { struct pm_files m; if (pm_get_files(body, len, &m)) return 1;  { struct pm_files_items e_items; while (pm_next_files_items(&m.items, &e_items)) {  { struct pm_files_items_fields e_fields; while (pm_next_files_items_fields(&e_items.fields, &e_fields)) {   } } { struct pm_files_items_files e_files; while (pm_next_files_items_files(&e_items.files, &e_files)) {   } } } } return 0; }
    case PM_FILE_OPEN: { struct pm_file_open m; if (pm_get_file_open(body, len, &m)) return 1;   return 0; }
    case PM_FILE_CHECK: { struct pm_file_check m; if (pm_get_file_check(body, len, &m)) return 1;   return 0; }
    case PM_FILE_OK: { struct pm_file_ok m; if (pm_get_file_ok(body, len, &m)) return 1;  { struct pm_file_ok_fields e_fields; while (pm_next_file_ok_fields(&m.fields, &e_fields)) {   } } return 0; }
    case PM_FILE_BAD: { struct pm_file_bad m; if (pm_get_file_bad(body, len, &m)) return 1; if (!pm_file_bad_ok(m.code, ver)) return 1;  return 0; }
    case PM_WAKE_WORDS: { struct pm_wake_words m; if (pm_get_wake_words(body, len, &m)) return 1;  { struct pm_wake_words_words e_words; while (pm_next_wake_words_words(&m.words, &e_words)) {   } } return 0; }
    case PM_WAKE_OPEN: { struct pm_wake_open m; if (pm_get_wake_open(body, len, &m)) return 1;   return 0; }
    case PM_WAKE_READY: { struct pm_wake_ready m; if (pm_get_wake_ready(body, len, &m)) return 1;   return 0; }
    case PM_WAKE_FAILED: { struct pm_wake_failed m; if (pm_get_wake_failed(body, len, &m)) return 1; if (!pm_wake_fail_ok(m.why, ver)) return 1;  return 0; }
    case PM_WAKE_CLOSE: { struct pm_wake_close m; if (pm_get_wake_close(body, len, &m)) return 1;   return 0; }
    case PM_WAKE_RESET: (void)body; return len ? 1 : 0;
    case PM_WAKE_HEARD: { struct pm_wake_heard m; if (pm_get_wake_heard(body, len, &m)) return 1;   return 0; }
    case PM_STORE_GET: { struct pm_store_get m; if (pm_get_store_get(body, len, &m)) return 1;   return 0; }
    case PM_STORE_VALUE: { struct pm_store_value m; if (pm_get_store_value(body, len, &m)) return 1;   return 0; }
    case PM_STORE_PUT: { struct pm_store_put m; if (pm_get_store_put(body, len, &m)) return 1;   return 0; }
    case PM_STORE_DEL: { struct pm_store_del m; if (pm_get_store_del(body, len, &m)) return 1;   return 0; }
    case PM_LOG_FOLLOW: { struct pm_log_follow m; if (pm_get_log_follow(body, len, &m)) return 1;   return 0; }
    case PM_LOG: { struct pm_log m; if (pm_get_log(body, len, &m)) return 1;  { struct pm_log_lines e_lines; while (pm_next_log_lines(&m.lines, &e_lines)) {   } } return 0; }
    case PM_CONNECT: { struct pm_connect m; if (pm_get_connect(body, len, &m)) return 1; if (!pm_cap_ok(m.kind, ver)) return 1;  return 0; }
    case PM_UDP_OPEN: { struct pm_udp_open m; if (pm_get_udp_open(body, len, &m)) return 1; if (!pm_cap_ok(m.kind, ver)) return 1;  return 0; }
    case PM_UDP_OPENED: { struct pm_udp_opened m; if (pm_get_udp_opened(body, len, &m)) return 1;   return 0; }
    case PM_UDP_PEER: { struct pm_udp_peer m; if (pm_get_udp_peer(body, len, &m)) return 1;   return 0; }
    case PM_SENDTO: { struct pm_sendto m; if (pm_get_sendto(body, len, &m)) return 1;   return 0; }
    case PM_BROADCAST: { struct pm_broadcast m; if (pm_get_broadcast(body, len, &m)) return 1;   return 0; }
    case PM_PROPOSE: { struct pm_propose m; if (pm_get_propose(body, len, &m)) return 1;   return 0; }
    case PM_NET: { struct pm_net m; if (pm_get_net(body, len, &m)) return 1;   return 0; }
    case PM_MUSIC_OPEN: { struct pm_music_open m; if (pm_get_music_open(body, len, &m)) return 1;   return 0; }
    case PM_MUSIC_STATE: { struct pm_music_state m; if (pm_get_music_state(body, len, &m)) return 1; if (!pm_music_ok(m.state, ver)) return 1;  return 0; }
    case PM_MUSIC_CLOSE: (void)body; return len ? 1 : 0;
    case PM_MUSIC_POS: { struct pm_music_pos m; if (pm_get_music_pos(body, len, &m)) return 1;   return 0; }
    case PM_MUSIC_CMD: { struct pm_music_cmd m; if (pm_get_music_cmd(body, len, &m)) return 1; if (!pm_mcmd_ok(m.cmd, ver)) return 1; if (!pm_mwhy_ok(m.why, ver)) return 1;  return 0; }
    case PM_VOLUME_SET: { struct pm_volume_set m; if (pm_get_volume_set(body, len, &m)) return 1;   return 0; }
    case PM_VOLUME: { struct pm_volume m; if (pm_get_volume(body, len, &m)) return 1;   return 0; }
    case PM_STATE: { struct pm_state m; if (pm_get_state(body, len, &m)) return 1; if (!pm_state_ok(m.state, ver)) return 1;  return 0; }
    case PM_ENTITY_STATE: { struct pm_entity_state m; if (pm_get_entity_state(body, len, &m)) return 1;   return 0; }
    case PM_EVENT: { struct pm_event m; if (pm_get_event(body, len, &m)) return 1;   return 0; }
    case PM_ENTITY_SET: { struct pm_entity_set m; if (pm_get_entity_set(body, len, &m)) return 1;   return 0; }
    case PM_ACTION: { struct pm_action m; if (pm_get_action(body, len, &m)) return 1; if (!pm_from_ok(m.from, ver)) return 1; { struct pm_action_args e_args; while (pm_next_action_args(&m.args, &e_args)) {   } } return 0; }
    case PM_CALL_INCOMING: { struct pm_call_incoming m; if (pm_get_call_incoming(body, len, &m)) return 1;   return 0; }
    case PM_CALL_ANSWER: (void)body; return len ? 1 : 0;
    case PM_CALL_HANGUP: { struct pm_call_hangup m; if (pm_get_call_hangup(body, len, &m)) return 1; if (!pm_hangup_ok(m.why, ver)) return 1;  return 0; }
    case PM_CALL_OUT: { struct pm_call_out m; if (pm_get_call_out(body, len, &m)) return 1;   return 0; }
    case PM_CALL_RINGING_THERE: (void)body; return len ? 1 : 0;
    case PM_CALL_LIVE: { struct pm_call_live m; if (pm_get_call_live(body, len, &m)) return 1;   return 0; }
    case PM_CALL_END: { struct pm_call_end m; if (pm_get_call_end(body, len, &m)) return 1; if (!pm_result_ok(m.result, ver)) return 1;  return 0; }
    case PM_REQ_START: { struct pm_req_start m; if (pm_get_req_start(body, len, &m)) return 1; if (!pm_reqsrc_ok(m.source, ver)) return 1;  return 0; }
    case PM_REQ_END: { struct pm_req_end m; if (pm_get_req_end(body, len, &m)) return 1; if (!pm_reqend_ok(m.why, ver)) return 1;  return 0; }
    case PM_REQ_TEXT: { struct pm_req_text m; if (pm_get_req_text(body, len, &m)) return 1; if (!pm_reqtext_ok(m.kind, ver)) return 1;  return 0; }
    case PM_BUTTON: { struct pm_button m; if (pm_get_button(body, len, &m)) return 1; if (!pm_button_key_ok(m.key, ver)) return 1; if (!pm_button_how_ok(m.how, ver)) return 1;  return 0; }
    }
    return -1;
}

/* The packet's enum values made what VER speaks, before it goes (sdk/PROTOCOL.md "Values, constants and keys"): a
 * value of a later version than VER has is rewritten in place to its fallback (pm_<enum>_for).  hassmic's psend and
 * the C client's put() run it on every packet, so no sender drops a message for a value a later version brought,
 * but for one whose fallback is none (messages.def `>-`): that packet is not for this peer at all, and is dropped
 * without a word, as what an older peer cannot be told.  Only a message marked `drop` (pm_droppable) is ever such a
 * one: mkplugmsg.py refuses `>-` on an enum any other message holds, so an answer, an end or a state always goes.
 * In place: BODY is the packet as it goes to this one peer
 * (encode a packet per peer, never send one buffer to two).
 * 0; 1 not to be sent to this peer (`>-`); -1 a packet that does not read (left as it is: pm_check refuses it) */
static inline int pm_fit(uint16_t type, uint8_t *body, size_t len, const uint16_t *ver)
{
    struct pm_r r_ = { body, body + len, 0 }, *r = &r_; int un_ = 0, *unsent = &un_; (void)r; (void)ver; (void)unsent;
    switch (type) {
    }
    return 0;
}
/* 1 when TYPE may be left unsent whole to a peer whose version has no word for a value it holds (messages.def `drop`):
 * the only messages pm_fit ever answers 1 for.  A sender whose caller learns "it went" from its answer sends none of these */
static inline int pm_droppable(uint16_t type)
{
    switch (type) {
    case PM_BUTTON: return 1;
    }
    return 0;
}

static inline const char *pm_name(uint16_t type)
{
    switch (type) {
    case PM_HELLO: return "HELLO";
    case PM_WELCOME: return "WELCOME";
    case PM_REFUSED: return "REFUSED";
    case PM_CONFIG: return "CONFIG";
    case PM_READY: return "READY";
    case PM_PING: return "PING";
    case PM_PONG: return "PONG";
    case PM_STOP: return "STOP";
    case PM_TIME: return "TIME";
    case PM_OK: return "OK";
    case PM_ERR: return "ERR";
    case PM_FILES: return "FILES";
    case PM_FILE_OPEN: return "FILE_OPEN";
    case PM_FILE_CHECK: return "FILE_CHECK";
    case PM_FILE_OK: return "FILE_OK";
    case PM_FILE_BAD: return "FILE_BAD";
    case PM_WAKE_WORDS: return "WAKE_WORDS";
    case PM_WAKE_OPEN: return "WAKE_OPEN";
    case PM_WAKE_READY: return "WAKE_READY";
    case PM_WAKE_FAILED: return "WAKE_FAILED";
    case PM_WAKE_CLOSE: return "WAKE_CLOSE";
    case PM_WAKE_RESET: return "WAKE_RESET";
    case PM_WAKE_HEARD: return "WAKE_HEARD";
    case PM_STORE_GET: return "STORE_GET";
    case PM_STORE_VALUE: return "STORE_VALUE";
    case PM_STORE_PUT: return "STORE_PUT";
    case PM_STORE_DEL: return "STORE_DEL";
    case PM_LOG_FOLLOW: return "LOG_FOLLOW";
    case PM_LOG: return "LOG";
    case PM_CONNECT: return "CONNECT";
    case PM_UDP_OPEN: return "UDP_OPEN";
    case PM_UDP_OPENED: return "UDP_OPENED";
    case PM_UDP_PEER: return "UDP_PEER";
    case PM_SENDTO: return "SENDTO";
    case PM_BROADCAST: return "BROADCAST";
    case PM_PROPOSE: return "PROPOSE";
    case PM_NET: return "NET";
    case PM_MUSIC_OPEN: return "MUSIC_OPEN";
    case PM_MUSIC_STATE: return "MUSIC_STATE";
    case PM_MUSIC_CLOSE: return "MUSIC_CLOSE";
    case PM_MUSIC_POS: return "MUSIC_POS";
    case PM_MUSIC_CMD: return "MUSIC_CMD";
    case PM_VOLUME_SET: return "VOLUME_SET";
    case PM_VOLUME: return "VOLUME";
    case PM_STATE: return "STATE";
    case PM_ENTITY_STATE: return "ENTITY_STATE";
    case PM_EVENT: return "EVENT";
    case PM_ENTITY_SET: return "ENTITY_SET";
    case PM_ACTION: return "ACTION";
    case PM_CALL_INCOMING: return "CALL_INCOMING";
    case PM_CALL_ANSWER: return "CALL_ANSWER";
    case PM_CALL_HANGUP: return "CALL_HANGUP";
    case PM_CALL_OUT: return "CALL_OUT";
    case PM_CALL_RINGING_THERE: return "CALL_RINGING_THERE";
    case PM_CALL_LIVE: return "CALL_LIVE";
    case PM_CALL_END: return "CALL_END";
    case PM_REQ_START: return "REQ_START";
    case PM_REQ_END: return "REQ_END";
    case PM_REQ_TEXT: return "REQ_TEXT";
    case PM_BUTTON: return "BUTTON";
    }
    return "?";
}
/* who sends a type: '>' the plugin, '<' the broker, '=' either, 0 unknown */
static inline char pm_dir(uint16_t type)
{
    switch (type) {
    case PM_HELLO: return '>';
    case PM_WELCOME: return '<';
    case PM_REFUSED: return '<';
    case PM_CONFIG: return '<';
    case PM_READY: return '>';
    case PM_PING: return '<';
    case PM_PONG: return '>';
    case PM_STOP: return '<';
    case PM_TIME: return '<';
    case PM_OK: return '=';
    case PM_ERR: return '=';
    case PM_FILES: return '<';
    case PM_FILE_OPEN: return '>';
    case PM_FILE_CHECK: return '<';
    case PM_FILE_OK: return '>';
    case PM_FILE_BAD: return '>';
    case PM_WAKE_WORDS: return '>';
    case PM_WAKE_OPEN: return '<';
    case PM_WAKE_READY: return '>';
    case PM_WAKE_FAILED: return '>';
    case PM_WAKE_CLOSE: return '<';
    case PM_WAKE_RESET: return '<';
    case PM_WAKE_HEARD: return '>';
    case PM_STORE_GET: return '>';
    case PM_STORE_VALUE: return '<';
    case PM_STORE_PUT: return '>';
    case PM_STORE_DEL: return '>';
    case PM_LOG_FOLLOW: return '>';
    case PM_LOG: return '<';
    case PM_CONNECT: return '>';
    case PM_UDP_OPEN: return '>';
    case PM_UDP_OPENED: return '<';
    case PM_UDP_PEER: return '>';
    case PM_SENDTO: return '>';
    case PM_BROADCAST: return '>';
    case PM_PROPOSE: return '>';
    case PM_NET: return '<';
    case PM_MUSIC_OPEN: return '>';
    case PM_MUSIC_STATE: return '>';
    case PM_MUSIC_CLOSE: return '>';
    case PM_MUSIC_POS: return '<';
    case PM_MUSIC_CMD: return '<';
    case PM_VOLUME_SET: return '>';
    case PM_VOLUME: return '<';
    case PM_STATE: return '<';
    case PM_ENTITY_STATE: return '>';
    case PM_EVENT: return '>';
    case PM_ENTITY_SET: return '<';
    case PM_ACTION: return '<';
    case PM_CALL_INCOMING: return '>';
    case PM_CALL_ANSWER: return '<';
    case PM_CALL_HANGUP: return '<';
    case PM_CALL_OUT: return '>';
    case PM_CALL_RINGING_THERE: return '>';
    case PM_CALL_LIVE: return '>';
    case PM_CALL_END: return '>';
    case PM_REQ_START: return '<';
    case PM_REQ_END: return '<';
    case PM_REQ_TEXT: return '<';
    case PM_BUTTON: return '<';
    }
    return 0;
}
/* the capabilities a type needs, any one of them (PM_CAPBIT(PM_CAP_*)), 0 none */
static inline pm_caps_t pm_caps(uint16_t type)
{
    switch (type) {
    case PM_FILES: return PM_CAPBIT(PM_CAP_FILES);
    case PM_FILE_OPEN: return PM_CAPBIT(PM_CAP_FILES);
    case PM_FILE_CHECK: return PM_CAPBIT(PM_CAP_FILES);
    case PM_FILE_OK: return PM_CAPBIT(PM_CAP_FILES);
    case PM_FILE_BAD: return PM_CAPBIT(PM_CAP_FILES);
    case PM_WAKE_WORDS: return PM_CAPBIT(PM_CAP_WAKE);
    case PM_WAKE_OPEN: return PM_CAPBIT(PM_CAP_WAKE);
    case PM_WAKE_READY: return PM_CAPBIT(PM_CAP_WAKE);
    case PM_WAKE_FAILED: return PM_CAPBIT(PM_CAP_WAKE);
    case PM_WAKE_CLOSE: return PM_CAPBIT(PM_CAP_WAKE);
    case PM_WAKE_RESET: return PM_CAPBIT(PM_CAP_WAKE);
    case PM_WAKE_HEARD: return PM_CAPBIT(PM_CAP_WAKE);
    case PM_STORE_GET: return PM_CAPBIT(PM_CAP_STORAGE);
    case PM_STORE_VALUE: return PM_CAPBIT(PM_CAP_STORAGE);
    case PM_STORE_PUT: return PM_CAPBIT(PM_CAP_STORAGE);
    case PM_STORE_DEL: return PM_CAPBIT(PM_CAP_STORAGE);
    case PM_LOG_FOLLOW: return PM_CAPBIT(PM_CAP_LOG_READ) | PM_CAPBIT(PM_CAP_LOG_READ_FULL);
    case PM_LOG: return PM_CAPBIT(PM_CAP_LOG_READ) | PM_CAPBIT(PM_CAP_LOG_READ_FULL);
    case PM_CONNECT: return PM_CAPBIT(PM_CAP_NET_SERVER) | PM_CAPBIT(PM_CAP_NET_LAN) | PM_CAPBIT(PM_CAP_NET_INTERNET);
    case PM_UDP_OPEN: return PM_CAPBIT(PM_CAP_NET_UDP) | PM_CAPBIT(PM_CAP_NET_DISCOVER);
    case PM_UDP_OPENED: return PM_CAPBIT(PM_CAP_NET_UDP) | PM_CAPBIT(PM_CAP_NET_DISCOVER);
    case PM_UDP_PEER: return PM_CAPBIT(PM_CAP_NET_UDP);
    case PM_SENDTO: return PM_CAPBIT(PM_CAP_NET_UDP);
    case PM_BROADCAST: return PM_CAPBIT(PM_CAP_NET_DISCOVER);
    case PM_PROPOSE: return PM_CAPBIT(PM_CAP_NET_SERVER);
    case PM_NET: return PM_CAPBIT(PM_CAP_NET_SERVER) | PM_CAPBIT(PM_CAP_NET_LAN) | PM_CAPBIT(PM_CAP_NET_INTERNET) | PM_CAPBIT(PM_CAP_NET_UDP) | PM_CAPBIT(PM_CAP_NET_DISCOVER);
    case PM_MUSIC_OPEN: return PM_CAPBIT(PM_CAP_AUDIO_MUSIC);
    case PM_MUSIC_STATE: return PM_CAPBIT(PM_CAP_AUDIO_MUSIC);
    case PM_MUSIC_CLOSE: return PM_CAPBIT(PM_CAP_AUDIO_MUSIC);
    case PM_MUSIC_POS: return PM_CAPBIT(PM_CAP_AUDIO_MUSIC);
    case PM_MUSIC_CMD: return PM_CAPBIT(PM_CAP_AUDIO_MUSIC);
    case PM_VOLUME_SET: return PM_CAPBIT(PM_CAP_AUDIO_VOLUME);
    case PM_VOLUME: return PM_CAPBIT(PM_CAP_AUDIO_VOLUME);
    case PM_STATE: return PM_CAPBIT(PM_CAP_STATE);
    case PM_ENTITY_STATE: return PM_CAPBIT(PM_CAP_HA_ENTITIES);
    case PM_EVENT: return PM_CAPBIT(PM_CAP_HA_ENTITIES);
    case PM_ENTITY_SET: return PM_CAPBIT(PM_CAP_HA_ENTITIES);
    case PM_ACTION: return PM_CAPBIT(PM_CAP_HA_ACTIONS);
    case PM_CALL_INCOMING: return PM_CAPBIT(PM_CAP_CALL);
    case PM_CALL_ANSWER: return PM_CAPBIT(PM_CAP_CALL);
    case PM_CALL_HANGUP: return PM_CAPBIT(PM_CAP_CALL);
    case PM_CALL_OUT: return PM_CAPBIT(PM_CAP_CALL);
    case PM_CALL_RINGING_THERE: return PM_CAPBIT(PM_CAP_CALL);
    case PM_CALL_LIVE: return PM_CAPBIT(PM_CAP_CALL);
    case PM_CALL_END: return PM_CAPBIT(PM_CAP_CALL);
    case PM_REQ_START: return PM_CAPBIT(PM_CAP_REQUEST_AUDIO);
    case PM_REQ_END: return PM_CAPBIT(PM_CAP_REQUEST_AUDIO);
    case PM_REQ_TEXT: return PM_CAPBIT(PM_CAP_REQUEST_TEXT);
    case PM_BUTTON: return PM_CAPBIT(PM_CAP_BUTTON);
    }
    return 0;
}
/* scope S's name, "" past the last */
static inline const char *pm_scope_name(int s)
{
    switch (s) {
    case PM_SCOPE_CORE: return "core";
    case PM_SCOPE_STORAGE: return "storage";
    case PM_SCOPE_FILES: return "files";
    case PM_SCOPE_STATE: return "state";
    case PM_SCOPE_LOG_READ: return "log.read";
    case PM_SCOPE_LOG_READ_FULL: return "log.read.full";
    case PM_SCOPE_NET_SERVER: return "net.server";
    case PM_SCOPE_NET_LAN: return "net.lan";
    case PM_SCOPE_NET_INTERNET: return "net.internet";
    case PM_SCOPE_NET_UDP: return "net.udp";
    case PM_SCOPE_NET_DISCOVER: return "net.discover";
    case PM_SCOPE_AUDIO_MUSIC: return "audio.music";
    case PM_SCOPE_AUDIO_VOLUME: return "audio.volume";
    case PM_SCOPE_CALL: return "call";
    case PM_SCOPE_WAKE: return "wake";
    case PM_SCOPE_REQUEST_AUDIO: return "request.audio";
    case PM_SCOPE_REQUEST_TEXT: return "request.text";
    case PM_SCOPE_HA_ENTITIES: return "ha.entities";
    case PM_SCOPE_HA_ACTIONS: return "ha.actions";
    case PM_SCOPE_BUTTON: return "button";
    }
    return "";
}
/* the scope named by the N bytes at NAME, -1 none of this build's */
static inline int pm_scope_of(const char *name, size_t n)
{
    for (int s = 0; s < PM_SCOPES; s++) { const char *c = pm_scope_name(s); if (strlen(c) == n && !memcmp(c, name, n)) return s; }
    return -1;
}
/* the capability bit (PM_CAP_*) scope S is of, -1 for core */
static inline int pm_scope_cap(int s)
{
    switch (s) {
    case PM_SCOPE_STORAGE: return PM_CAP_STORAGE;
    case PM_SCOPE_FILES: return PM_CAP_FILES;
    case PM_SCOPE_STATE: return PM_CAP_STATE;
    case PM_SCOPE_LOG_READ: return PM_CAP_LOG_READ;
    case PM_SCOPE_LOG_READ_FULL: return PM_CAP_LOG_READ_FULL;
    case PM_SCOPE_NET_SERVER: return PM_CAP_NET_SERVER;
    case PM_SCOPE_NET_LAN: return PM_CAP_NET_LAN;
    case PM_SCOPE_NET_INTERNET: return PM_CAP_NET_INTERNET;
    case PM_SCOPE_NET_UDP: return PM_CAP_NET_UDP;
    case PM_SCOPE_NET_DISCOVER: return PM_CAP_NET_DISCOVER;
    case PM_SCOPE_AUDIO_MUSIC: return PM_CAP_AUDIO_MUSIC;
    case PM_SCOPE_AUDIO_VOLUME: return PM_CAP_AUDIO_VOLUME;
    case PM_SCOPE_CALL: return PM_CAP_CALL;
    case PM_SCOPE_WAKE: return PM_CAP_WAKE;
    case PM_SCOPE_REQUEST_AUDIO: return PM_CAP_REQUEST_AUDIO;
    case PM_SCOPE_REQUEST_TEXT: return PM_CAP_REQUEST_TEXT;
    case PM_SCOPE_HA_ENTITIES: return PM_CAP_HA_ENTITIES;
    case PM_SCOPE_HA_ACTIONS: return PM_CAP_HA_ACTIONS;
    case PM_SCOPE_BUTTON: return PM_CAP_BUTTON;
    }
    return -1;
}
/* 1 when a version of scope S above FROM and up to TO widens what its capability lets a plugin do (docs/plugins.md 3.3:
 * an update that comes to require it is wider than what was accepted) */
static inline int pm_widens(int s, unsigned from, unsigned to)
{
    (void)s; (void)from; (void)to;
    return 0;
}
/* what version V of scope S brought, in its definition's words ("" for version 1 and for none such) */
static inline const char *pm_version_doc(int s, unsigned v)
{
    (void)s; (void)v;
    return "";
}
/* KEY of the manifest's context CTX (sdk/messages.def's keys lines: "manifest", "setting", "net.lan" ...): the version of
 * its scope that brought it, and *SCOPE that scope; 0 when this build knows no such key there */
static inline unsigned pm_key(const char *ctx, const char *key, int *scope)
{
    static const struct { const char *ctx, *key; int scope; unsigned since; } t[] = {
        { "manifest", "id", PM_SCOPE_CORE, 1 },
        { "manifest", "name", PM_SCOPE_CORE, 1 },
        { "manifest", "author", PM_SCOPE_CORE, 1 },
        { "manifest", "version", PM_SCOPE_CORE, 1 },
        { "manifest", "version_name", PM_SCOPE_CORE, 1 },
        { "manifest", "source", PM_SCOPE_CORE, 1 },
        { "manifest", "license", PM_SCOPE_CORE, 1 },
        { "manifest", "description", PM_SCOPE_CORE, 1 },
        { "manifest", "requires", PM_SCOPE_CORE, 1 },
        { "manifest", "author_key", PM_SCOPE_CORE, 1 },
        { "manifest", "digest", PM_SCOPE_CORE, 1 },
        { "manifest", "memory_mb", PM_SCOPE_CORE, 1 },
        { "manifest", "threads", PM_SCOPE_CORE, 1 },
        { "manifest", "models", PM_SCOPE_CORE, 1 },
        { "manifest", "capabilities", PM_SCOPE_CORE, 1 },
        { "manifest", "settings", PM_SCOPE_CORE, 1 },
        { "setting", "name", PM_SCOPE_CORE, 1 },
        { "setting", "label", PM_SCOPE_CORE, 1 },
        { "setting", "type", PM_SCOPE_CORE, 1 },
        { "setting", "help", PM_SCOPE_CORE, 1 },
        { "setting", "unit", PM_SCOPE_CORE, 1 },
        { "setting", "min", PM_SCOPE_CORE, 1 },
        { "setting", "max", PM_SCOPE_CORE, 1 },
        { "setting", "options", PM_SCOPE_CORE, 1 },
        { "setting", "default", PM_SCOPE_CORE, 1 },
        { "setting", "item", PM_SCOPE_CORE, 1 },
        { "setting", "max_items", PM_SCOPE_CORE, 1 },
        { "setting", "fields", PM_SCOPE_CORE, 1 },
        { "setting", "catalog", PM_SCOPE_CORE, 1 },
        { "setting.item", "by", PM_SCOPE_FILES, 1 },
        { "setting.item", "files", PM_SCOPE_FILES, 1 },
        { "setting.item.file", "suffix", PM_SCOPE_FILES, 1 },
        { "setting.item.file", "as", PM_SCOPE_FILES, 1 },
        { "setting.item.file", "max_kb", PM_SCOPE_FILES, 1 },
        { "setting.item.file", "optional", PM_SCOPE_FILES, 1 },
        { "setting.field", "name", PM_SCOPE_FILES, 1 },
        { "setting.field", "label", PM_SCOPE_FILES, 1 },
        { "setting.field", "type", PM_SCOPE_FILES, 1 },
        { "setting.field", "min", PM_SCOPE_FILES, 1 },
        { "setting.field", "max", PM_SCOPE_FILES, 1 },
        { "setting.field", "step", PM_SCOPE_FILES, 1 },
        { "net.server", "setting", PM_SCOPE_NET_SERVER, 1 },
        { "net.server", "proto", PM_SCOPE_NET_SERVER, 1 },
        { "net.server", "ports", PM_SCOPE_NET_SERVER, 1 },
        { "net.discover", "udp", PM_SCOPE_NET_DISCOVER, 1 },
        { "net.discover", "to", PM_SCOPE_NET_DISCOVER, 1 },
        { "net.lan", "proto", PM_SCOPE_NET_LAN, 1 },
        { "net.lan", "ports", PM_SCOPE_NET_LAN, 1 },
        { "net.internet", "proto", PM_SCOPE_NET_INTERNET, 1 },
        { "net.internet", "ports", PM_SCOPE_NET_INTERNET, 1 },
        { "ha.entities.binary_sensor", "name", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.binary_sensor", "kind", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.binary_sensor", "label", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.binary_sensor", "icon", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.binary_sensor", "page", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.sensor", "name", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.sensor", "kind", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.sensor", "label", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.sensor", "icon", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.sensor", "page", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.sensor", "unit", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.sensor", "decimals", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.text_sensor", "name", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.text_sensor", "kind", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.text_sensor", "label", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.text_sensor", "icon", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.text_sensor", "page", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.switch", "name", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.switch", "kind", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.switch", "label", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.switch", "icon", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.switch", "page", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.button", "name", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.button", "kind", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.button", "label", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.button", "icon", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.button", "page", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.number", "name", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.number", "kind", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.number", "label", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.number", "icon", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.number", "page", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.number", "unit", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.number", "min", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.number", "max", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.number", "step", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.select", "name", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.select", "kind", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.select", "label", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.select", "icon", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.select", "page", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.select", "options", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.event", "name", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.event", "kind", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.event", "label", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.event", "icon", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.event", "page", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.entities.event", "types", PM_SCOPE_HA_ENTITIES, 1 },
        { "ha.actions", "name", PM_SCOPE_HA_ACTIONS, 1 },
        { "ha.actions", "label", PM_SCOPE_HA_ACTIONS, 1 },
        { "ha.actions", "args", PM_SCOPE_HA_ACTIONS, 1 },
        { "ha.actions", "page", PM_SCOPE_HA_ACTIONS, 1 },
        { "ha.actions", "calls", PM_SCOPE_HA_ACTIONS, 1 },
        { "ha.actions.arg", "name", PM_SCOPE_HA_ACTIONS, 1 },
        { "ha.actions.arg", "type", PM_SCOPE_HA_ACTIONS, 1 },
        { 0, 0, 0, 0 } };
    for (size_t i = 0; t[i].ctx; i++) if (!strcmp(t[i].ctx, ctx) && !strcmp(t[i].key, key)) { if (scope) *scope = t[i].scope; return t[i].since; }
    return 0;
}
#endif
