/* The phone (sip.c): the Echo as an extension of a SIP phone system (Asterisk, a Fritz!Box, FreePBX), one account,
 * one call at a time, the call's audio through call.c as Drop In's. */
#ifndef SIP_H
#define SIP_H
#include <stddef.h>

/* A call's state, the same numbers as Drop In's (dropin.h DROPIN_*): main.c shows both alike */
enum { PHONE_IDLE, PHONE_CALLING, PHONE_RINGING, PHONE_LIVE };
extern const char *const phone_states[4];       /* "idle", "calling", "ringing", "connected": Home Assistant's text */
/* The line: no account (or the feature off), registering, registered, the password refused, the server not answering */
enum { LINE_OFF, LINE_CONNECTING, LINE_UP, LINE_REFUSED, LINE_UNREACHABLE };
extern const char *const phone_lines[5];        /* "off", "connecting", "registered", "refused", "unreachable" */
/* What came of a call this Echo made (Home Assistant's event entity "Phone result"; types stable, a new one is new):
 * answered, rings there, busy, declined, nobody answered, no such number, not reachable now, no line (no account, not
 * registered), this Echo in a call already, its microphones off, cancelled here, anything else */
enum { PHONE_R_CONNECTED, PHONE_R_RINGING, PHONE_R_BUSY, PHONE_R_DECLINED, PHONE_R_NOT_ANSWERED, PHONE_R_NOT_FOUND,
       PHONE_R_UNAVAILABLE, PHONE_R_NO_LINE, PHONE_R_BUSY_HERE, PHONE_R_MUTED_HERE, PHONE_R_CANCELLED, PHONE_R_FAILED,
       PHONE_NRESULTS };
extern const char *const phone_results[PHONE_NRESULTS];

int  sip_start(int port);                       /* SIP on UDP port, RTP on port + 1, the account from state/phone.  0 ok */
int  sip_running(void);
/* Any lock but sip.c's own (core_lock may be held): */
int  sip_enable(int set);                       /* the "phone" setting: registered, calls in and out; -1 reads */
/* Call a number (an extension, digits, * # + allowed) at the account's server.  0: calling; -1 and why in err (the
 * result is reported all the same, as Drop In's) */
int  sip_call(const char *number, char *err, size_t errsz);
void sip_hangup(const char *why);               /* ends whatever runs: a call, one ringing here (declined), one going out */
int  sip_answer(void);                          /* a call ringing here: 1 if it was */
int  sip_button(void);                          /* the action button: answers a ringing call or ends one; 1 if it did */
int  sip_status(char *peer, size_t cap);        /* PHONE_*; peer: who ("Portero (60)", the number dialled; "" when idle) */
int  sip_line(void);                            /* LINE_* */
int  sip_last(unsigned *attempt);               /* the last call's PHONE_R_* (-1: none yet); attempt counts calls made */
/* The account: server ("host" or "host:port"), user (the extension), password.  NULL keeps what is there; "" clears.
 * Saved in state/phone (0600, never exported).  0, or -1 and why in err */
int  sip_account(const char *server, const char *user, const char *password, char *err, size_t errsz);
size_t sip_status_json(char *out, size_t cap);  /* the settings page's: line, state, account (never the password) */
#endif
