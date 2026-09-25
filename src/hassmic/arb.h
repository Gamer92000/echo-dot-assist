/* Wake word arbitration between Echos: of several that hear the wake word, only the one that heard it best answers.
 * See arb.c. */
#ifndef ARB_H
#define ARB_H

struct arb_hooks {
    /* Have Home Assistant run the action "esphome.<node>_arbitration_key" (node with '-' as '_', as HA names it) with
     * these two strings.  -1: no link to Home Assistant.  Called from arb's thread, never with its lock held. */
    int  (*send_key)(const char *node, const char *network, const char *key);
    void (*changed)(void);                      /* membership or number of peers changed: the entities follow */
};

int  arb_start(int port, const char *node, const struct arb_hooks *h);           /* node: our ESPHome node name; 0 = running */
int  arb_running(void);
int  arb_join(int set);                         /* "Join arbitration network": 0/1, or -1 to only read */
int  arb_peers(void);                           /* other members heard from lately */
/* Home Assistant ran our "arbitration_key" action: a member hands us its network.  Any lock may be held. */
void arb_key(const char *network, const char *key);

/* The wake word was heard with this score (signal to noise, dB x 100); prio 2 = this Echo is in a conversation or
 * ringing and owns the next wake word.  Only Echos that heard the same keyword compete ("Echo" in German and in
 * English is the same one).  Returns the monotonic ms at which to call arb_decide(), or 0: nobody else to ask, answer now. */
long long arb_claim(const char *keyword, int score, int prio);
int       arb_decide(void);                     /* 1: this Echo answers */
#endif
