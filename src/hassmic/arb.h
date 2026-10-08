/* Wake word arbitration between Echos: of several that hear the wake word, only the one that heard it best answers.
 * See arb.c. */
#ifndef ARB_H
#define ARB_H
#include <stddef.h>

struct arb_hooks {
    /* Have Home Assistant run the action "esphome.<node>_arbitration_key" (node with '-' as '_', as HA names it) with
     * these two strings.  -1: no link to Home Assistant.  Called from arb's thread, never with its lock held. */
    int  (*send_key)(const char *node, const char *network, const char *key);
    void (*changed)(void);                      /* membership or peers changed: the entities follow */
    /* Ask Home Assistant once for the state of this entity (SubscribeHomeAssistantStateResponse with "once"); the answer
     * comes back as arb_ha_state().  -1: no link to Home Assistant.  Called from arb's thread, never with its lock held. */
    int  (*request)(const char *entity);
    void (*paired)(int result);                 /* button pairing: 1 started, 2 network handed or taken, -1 ended without */
    /* Report this tag as scanned to Home Assistant (the event esphome.tag_scanned, which needs no permission).  -1: no
     * link to Home Assistant.  Called from arb's thread, never with its lock held. */
    int  (*scan)(const char *tag_id);
    /* A member's message for us (arb_tell), checked: K's MAC, a fresh counter.  from: its id (the first 8 bytes of its
     * public key), node and IP as its beacons say.  May be NULL.  Called from arb's thread, never with its lock held. */
    void (*message)(const unsigned char from[8], const char *node, unsigned ip, const unsigned char *p, size_t n);
};

int  arb_start(int port, const char *node, const struct arb_hooks *h);           /* node: our ESPHome node name; 0 = running */
int  arb_running(void);
int  arb_arbitrate(int set);                    /* take part in rounds (the "arbitration" setting): 0/1, -1 only reads.  The
                                                 * network (keys, beacons: the settings pages' list of Echos) runs either way */
int  arb_peers(void);                           /* other members heard from lately that take part in rounds */
/* How rounds are settled (the "arbitration_mode" setting, kept in state/config by settings.c, read by lockdown.sh):
 * ARB_HASSMIC our own protocol between the Echos of the network; ARB_KIOSK Kiosk Satellite's (UDP 2330, loudness only),
 * with kiosks and with Echos in that mode.  -1 only reads.  Any lock may be held. */
enum { ARB_HASSMIC, ARB_KIOSK };
int  arb_mode(int set);
int  arb_window(int set);                       /* ARB_KIOSK's window in ms (100-500, as Kiosk Satellite's); -1 only reads */
/* ARB_KIOSK: dB added to our claims' loudness (-20..20), the owner's calibration against kiosks; INT_MIN only reads */
int  arb_offset(int set);
/* Home Assistant ran our "arbitration_key" action: a member hands us its network.  Any lock may be held. */
void arb_key(const char *network, const char *key);
/* Home Assistant's answer to hooks->request: the state of that entity.  Any lock may be held. */
void arb_ha_state(const char *entity, const char *state);
/* For the settings page: our network, the members (name, IP), the Echos outside it and why ("none": in no network,
 * "younger": in a younger one, should join ours, "older": we should join theirs), as JSON.  Any lock may be held. */
size_t arb_status_json(char *out, size_t cap);
/* For the settings pages (web.c vouchers): a key every member of our network derives from K, nobody else.  -1: in no
 * network.  And the members heard from lately, as JSON [{"node","ip"}] (unauthenticated: beacons say as much).  Any lock. */
int  arb_web_key(unsigned char out[32]);
size_t arb_members_json(char *out, size_t cap);
int  arb_pair(void);
/* Drop In (dropin.c) rides on the network.  A member by its node name: its id and IP (network byte order) from its
 * signed beacons; -1 if none counts.  arb_tell: a message for that member alone (n <= ARB_MSG_MAX), broadcast like
 * everything else here, under K's MAC and our counter; -1 outside a network.  arb_derive: a key every member derives
 * from K for that purpose, nobody else; -1 outside a network.  arb_self: our own id.  Any lock but arb's. */
#define ARB_MSG_MAX 160
int  arb_member(const char *node, unsigned char id[8], unsigned *ip);
int  arb_tell(const unsigned char to[8], const void *p, size_t n);
int  arb_derive(const char *label, unsigned char out[32]);
void arb_self(unsigned char id[8]);                            /* the pairing gesture: 2 min in which an Echo nearby may join; -1 not running */

/* The wake word was heard with this score (signal to noise, dB x 100); prio 2 = this Echo is in a conversation or
 * ringing and owns the next wake word.  Only Echos that heard the same keyword compete ("Echo" in German and in
 * English is the same one).  Returns the monotonic ms at which to call arb_decide(), or 0: nobody else to ask, answer now.
 * ARB_KIOSK: score is Kiosk Satellite's (main.c kiosk_score), prio is ignored, and there is always a round. */
long long arb_claim(const char *keyword, int score, int prio);
int       arb_decide(void);                     /* 1: this Echo answers */
#endif
