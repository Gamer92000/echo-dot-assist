/* Replies on another speaker: the assistant's spoken replies (and, with the setting, announcements) play on another
 * Echo of the arbitration network or on a Home Assistant media player instead of this Echo's own speaker (a bathroom
 * Echo over running water, a better speaker in the room).  See replyspk.c. */
#ifndef REPLYSPK_H
#define REPLYSPK_H
#include <stddef.h>

/* ---------------------------------------------------------------- where replies play (any lock but replyspk's) */

/* here: this Echo.  room: the media player Home Assistant has in this Echo's area (proto_esphome.c asks it).  player:
 * a media player by its entity id.  echo: another Echo of the network, by its node */
enum { RS_HERE, RS_ROOM, RS_PLAYER, RS_ECHO, RS_MODES };
extern const char *const replyspk_modes[RS_MODES];             /* "here", "room", "player", "echo" */
#define RS_ENTITY_MAX 128
#define RS_NODE_MAX 64
#define RS_NAME_MAX 96
struct rs_target { int mode; char node[RS_NODE_MAX], player[RS_ENTITY_MAX]; };

void replyspk_load(void);                       /* state/reply_speaker, once at start */
void replyspk_get(struct rs_target *t);
/* "here", "room", "player" (the entity kept), "player media_player.x", "echo" (the node kept), "echo <node>".  0, or
 * -1 and why.  Saves; the protocol shows it (core_settings_changed).  core_lock held */
int  replyspk_set(const char *spec, char *err, size_t errsz);
int  replyspk_valid_entity(const char *e);      /* "media_player.<object id>": what may be named */

/* The settings (settings.c keeps them in state/config): announcements go too (off); this Echo plays other Echos'
 * replies (on); what a Home Assistant player takes to start, in ms (RS_DELAY_DEFAULT) */
#define RS_DELAY_DEFAULT 1500
#define RS_DELAY_MAX 5000
int  replyspk_announce(int set);
int  replyspk_accept(int set);
int  replyspk_delay(int set);

/* ---------------------------------------------------------------- what Home Assistant says (any lock) */

/* The state or friendly name (attr) of an entity, from the subscriptions proto_esphome.c keeps for the player in use;
 * 1 if it was one of ours */
int  replyspk_ha_state(const char *entity, const char *attr, const char *state);
/* The entity replies go to in RS_PLAYER or RS_ROOM: "" when none (room not found yet, or none there) */
void replyspk_player(char *out, size_t cap);
/* 1: Home Assistant has that player and it is not unavailable; 0 and why */
int  replyspk_player_ready(const char *entity, char *why, size_t cap);
/* Home Assistant runs this Echo's actions ("Allow the device to perform Home Assistant actions"): 1 yes, 0 no (its
 * probe went unanswered), -1 not known yet.  set: 0/1, -1 reads, -2 forgets (a new link) */
int  replyspk_actions(int set);
/* What Home Assistant found in this Echo's area: its name ("" none), the player (entity, its name; "" none), how many
 * players besides it, and up to 4 of their names joined by ", ".  found 0: the question failed (why in area) */
void replyspk_room(int found, const char *area, const char *entity, const char *name, int others, const char *names);
long long replyspk_room_age_ms(void);           /* since the room was last asked about; -1 never */
void replyspk_room_asked(void);
int  replyspk_room_wanted(void);                /* "room" chosen (again) since: ask now */

/* What came of the last reply sent elsewhere, for the page and the log: where it went (a node or entity), "played",
 * "here" (it played on this Echo instead) or "cut", and why ("" none) */
void replyspk_outcome(const char *where, const char *result, const char *why);

/* For the settings page (web.c state_json): target, the network's Echos, Home Assistant's player and room, actions,
 * the last outcome, as one JSON object.  Any lock but replyspk's */
size_t replyspk_json(char *out, size_t cap);

/* Home Assistant's select: the network's Echos this one knows (those that said they take replies, and any chosen),
 * sorted; count.  Learned in replyspk.c's loop (replyspk_learn: 1 if one was new) and kept in state/reply_speaker:
 * Home Assistant lists them from its next connection on (proto_esphome.c lists them again at once only when one is
 * chosen that it has not listed: that closes its link for a moment) */
int  replyspk_known(char (*nodes)[RS_NODE_MAX], int max);
int  replyspk_learn(void);

/* ---------------------------------------------------------------- between Echos (TCP, replyspk.c) */

/* arb's hook: a member's message to every member (arb.h); takes only its own kinds */
void replyspk_message(const unsigned char from[8], const char *node, unsigned ip, const unsigned char *p, size_t n);

int  replyspk_start(int port);                  /* the listener; arb must run.  0 ok */
int  replyspk_running(void);

/* The asking side, from the media thread (no lock).  rs_open: connect to the member, ask it to play chime (may be "")
 * then url, and wait for its yes (a few seconds at most).  0 and *out; else -1 and why.  rs_wait: the next thing it
 * says within ms: RS_EV_* or 0 (nothing yet); -1: it is gone.  rs_stop: ask it to stop (the session stays open for
 * its answer).  rs_close: done with it */
enum { RS_EV_STARTED = 'S', RS_EV_PLAYED = 'P', RS_EV_CUT = 'C', RS_EV_FAILED = 'F' };
struct rs_peer;
int  rs_open(const char *node, const char *chime, const char *url, int announce, struct rs_peer **out, char *why, size_t cap);
int  rs_wait(struct rs_peer *p, int ms);
void rs_stop(struct rs_peer *p);
void rs_close(struct rs_peer *p);

/* The playing side: what came of a reply another Echo sent (id: the session's, from core_remote_reply), RS_EV_*.
 * Any lock */
void replyspk_event(unsigned id, int ev);
#endif
