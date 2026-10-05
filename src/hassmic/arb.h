/* Wake word arbitration between Echos: of several that hear the wake word, only the one that heard it best answers.
 * See arb.c. */
#ifndef ARB_H
#define ARB_H
#include <stddef.h>

struct arb_hooks {
    /* Have Home Assistant run the action "esphome.<node>_arbitration_key" (node with '-' as '_', as HA names it) with
     * these two strings.  -1: no link to Home Assistant.  Called from arb's thread, never with its lock held. */
    int  (*send_key)(const char *node, const char *network, const char *key);
    void (*changed)(void);                      /* membership, peers or our handoff text changed: the entities follow */
    /* Ask Home Assistant once for the state of this entity (SubscribeHomeAssistantStateResponse with "once"); the answer
     * comes back as arb_ha_state().  -1: no link to Home Assistant.  Called from arb's thread, never with its lock held. */
    int  (*request)(const char *entity);
    void (*paired)(int result);                 /* button pairing: 1 started, 2 network handed or taken, -1 ended without */
};

int  arb_start(int port, const char *node, const struct arb_hooks *h);           /* node: our ESPHome node name; 0 = running */
int  arb_running(void);
int  arb_join(int set);                         /* "Join arbitration network": 0/1, or -1 to only read */
int  arb_peers(void);                           /* other members heard from lately */
/* Home Assistant ran our "arbitration_key" action: a member hands us its network.  Any lock may be held. */
void arb_key(const char *network, const char *key);
/* Home Assistant's answer to hooks->request: the state of that entity.  Any lock may be held. */
void arb_ha_state(const char *entity, const char *state);
/* The text of our "Arbitration handoff" entity: our public key, and a sealed network key while we offer one */
void arb_handoff(char *out, size_t cap);
int  arb_pair(void);                            /* the pairing gesture: 2 min in which an Echo nearby may join; -1 not running */

/* The wake word was heard with this score (signal to noise, dB x 100); prio 2 = this Echo is in a conversation or
 * ringing and owns the next wake word.  Only Echos that heard the same keyword compete ("Echo" in German and in
 * English is the same one).  Returns the monotonic ms at which to call arb_decide(), or 0: nobody else to ask, answer now. */
long long arb_claim(const char *keyword, int score, int prio);
int       arb_decide(void);                     /* 1: this Echo answers */
#endif
