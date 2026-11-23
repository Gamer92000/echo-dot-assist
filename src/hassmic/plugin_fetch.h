/* Plugins as the plugin index lists them (docs/plugins.md 5.3, 5.4): which there are, and one of them downloaded.
 * The network's side only: what comes of a package is the store's to say (plugin_store.c) and the install's to do
 * (web.c), exactly as for one a browser uploads; reading and checking the lists is plugin_index.c's.
 *
 * pf_look and pf_download are called when the owner asked on the settings page (its Plugins pages looking at the
 * list, "Install", "Update").  All of it blocks; no lock held by the caller.  By itself the Echo asks for the
 * revocation list alone, once a day while plugins are on and one is installed, and hands it to and takes it from the
 * other Echos of its network (pf_start; docs/plugins.md 5.6). */
#ifndef PLUGIN_FETCH_H
#define PLUGIN_FETCH_H
#include <stddef.h>
#include <stdint.h>
#include "plugin_store.h"

/* one version of a plugin as the index names it: what is downloaded, and what it has to be */
struct pf_asset { char id[PS_ID_MAX]; long version; long long size; uint8_t sha256[32], manifest[32]; int tier; };

/* Who asks sets how old what is known may be before the index is asked again; the Echo holds both, whatever a page
 * sends:
 * PF_AUTO   a page did, by itself, when it was opened: an hour, whether the last look found the list or failed
 * PF_ASKED  the owner pressed "Look again", "Install" or "Update" (and hassmic's own fetch, on update.c's pace): 30 s */
enum { PF_AUTO, PF_ASKED,
       PF_NOW,                          /* (pf_download's own: a package not the list's, a list from before a publish) */
       PF_DAILY };                      /* (the Echo's own, by itself: the daily look, a listing installed that does not
                                         * hold; now, as PF_NOW, and one that fails leaves what is known as it was: a
                                         * failure nobody asked for took the place of the page's list, and PF_AUTO
                                         * showed it for an hour without asking again, a review of 3fcd319, S5) */
/* Looks at the index unless what is known is young enough for PACE (a list that has WANT stands for the hour in any
 * case).  0: the list is there (it may be empty); -1 with why: the index not reached, or an answer that is none (a
 * signature that does not hold, a serial lower than one this Echo has seen, a list it cannot read), now or at the
 * look that is still young enough; "cancelled", and then what was known is as it was.  STOP as update_fetch's */
int  pf_look(const char *want, int pace, const volatile int *stop, char *err, size_t cap);
/* the same within UNTIL (CLOCK_MONOTONIC ms; 0: none): each request bounded by what is left, the wait for another
 * look's end too; out of time it fails as a cancelled look does, leaving what is known as it was (a push's, ota.c) */
int  pf_look_until(const char *want, int pace, const volatile int *stop, long long until, char *err, size_t cap);
void pf_look_async(int pace);                   /* the same in a thread of its own, one at a time: the page's */
/* After pf_look_async's look (any pace) the versions installed here that the index lists anew (5.2's move to a new key,
 * a higher tier: plugin_store.c ps_relist_wanted) have the index's listing taken, in a thread of its own (pf_json's
 * "relist" per plugin while it runs or after it failed).  pf_relist_async: the owner's "relist", which also asks again
 * for what failed a short while ago */
void pf_relist_async(void);
/* what the last look found, for the page, as JSON into *out (the caller's to free): the length, 0 out of memory.
 * {"state":"never|looking|ok|unreachable|failed","error":"...","note":"...","age":<s>,"serial":<n>,"revoked_serial":<n>,
 *  "revoked_age":<s>,"dropped":<n>,"scopes":{"<scope>":<this build's version>,...},"site":"<the index's address>",
 *  "plugins":[{"id","name","author","description","source","tier","key",
 *  "here":{"version","version_name","caps":[...],"broad":[...],"requires":{...},"needs":[]}|null,   the version this
 *                                                                  hassmic would install
 *  "relist":{"state","version","error"} (while the index's listing of the version installed is taken, or failed),
 *  "top":{"version","version_name","unknown","caps":[...],"requires":{"<scope>":<n>,...},"needs":["<scope> <n>",...],
 *         "broad":[...]}|null}]}                                   the highest the index has: what it requires, what
 *                                                                  of that this build lacks ("needs"), and whether it
 *                                                                  asks for a capability this build does not know
 * Withdrawn versions are not in it, nor a plugin all of whose versions are */
size_t pf_json(char **out);
/* The version of ID this Echo would install: the highest the last look found whose requires this build meets
 * (every scope it names, at its version or a later one: sdk/PROTOCOL.md "Versions"), not withdrawn, not under the
 * build's least.  1 and *a; 0 and why */
int  pf_best(const char *id, struct pf_asset *a, char *why, size_t cap);
/* the newest version of ID any look of this Echo ever found listed, kept over restarts (state/plugins/.listed);
 * 0: it has seen no list with it.  The index's word: good for asking for the button, nothing else (web.c sync) */
long pf_seen(const char *id);
/* A into *data (the caller's to free) from <index>/packages/<id>-<version>.hmp, within SECS: 0, or -1 with why.
 * Its size and SHA-256 are the index's or it is refused; where they are not the list's, the index is looked at
 * once more (it may have been published since: every publish lists every package anew) and *A is what that list
 * says of the same version, downloaded once more */
int  pf_download(struct pf_asset *a, long secs, const volatile int *stop, unsigned char **data, size_t *len, char *err, size_t cap);

/* The revocation list by itself (5.6): a thread that, while plugins are on and at least one is installed, asks the
 * site for revoked.json once a day at a random time (the index too where a listing installed may be one it lists
 * anew; within a minute of the start or of the network's return, then hourly, where one does not hold here), tells
 * the members of the arbitration network the serial of the
 * list this Echo has (hourly, and after a newer one), and fetches a newer one from a member that told it (from its
 * settings port WEB_PORT's GET /api/plugins/revoked).  Once, after web_start and arb_start */
void pf_start(int web_port);
/* hassmic ends (SIGTERM, main()'s end): a look of that thread's own under way is cancelled, and it asks nothing more */
void pf_shutdown(void);
/* main.c's arb hook, a message to every member: one of ours (M_REVOKED) is noted for the thread, one place per
 * member FROM (its id in the network), source address IP and the port it names; others are not ours */
void pf_message(const unsigned char from[8], const char *node, unsigned ip, const unsigned char *p, size_t n);
/* GET /api/plugins/revoked for a member of the network: TICKET, NONCE and MAC its X-HM-Ticket, X-HM-Ctr and X-HM-Mac
 * (plugin_fetch.c member_mac: a ticket this Echo told within two hours, a nonce not taken with it before).  The
 * list's length and *OUT (the caller's to free) as pi_revoked_file; 0 not a member, a request again, or no list,
 * which the page answers alike */
size_t pf_revoked_serve(const char *ticket, const char *nonce, const char *mac, char **out);
/* plugins run here and this Echo's revocation list is older than a week, or there is none: 1 and the page's words.
 * Any lock but ps_lock's and bl's own order (it takes ps_lock, then bl, then the list's) */
int  pf_revoked_stale(char *why, size_t cap);
#endif
