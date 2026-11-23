/* The plugin index as an Echo reads it (docs/plugins.md 5.3, 5.5, 5.6; the repository hassmic-plugins makes it):
 * its key, the signed list of plugins (index.json) and the signed list of what was withdrawn (revoked.json).  The
 * reading and the checks only, the serials this build takes, and the revocation list this Echo keeps; asking the
 * site, and the other Echos, is plugin_fetch.c's.
 *
 * The key is built in (keys/plugins.pub, named in keys/plugins.conf; the Makefile passes it as PLUGINS_PUB):
 * whatever it signs is the index, and a package's listing by it says the plugin's tier (plugin_store.c).  So it is
 * also what decides which plugins are official on every Echo of this build (docs/SECURITY.md "Plugins").  A second
 * key may be built in beside it (keys/plugins.conf PLUGINS_PUB2, the Makefile's PLUGINS_PUB2; none by default): the
 * first step of a new index key (docs/plugins.md 5.2 "A new index key"), a build that takes either key's signature
 * for every list and listing, so that the index can sign with the new key once this build has reached the Echos.
 * Whoever holds either key holds the index then.  HASSMIC_PLUGINS_PUB, HASSMIC_PLUGINS_PUB2 (files of the 32 bytes;
 * the first in place of the built-in keys, both of them, the second beside it) and HASSMIC_PLUGIN_INDEX (the site)
 * for the tests. */
#ifndef PLUGIN_INDEX_H
#define PLUGIN_INDEX_H
#include <stddef.h>
#include <stdint.h>
#include "plugin_store.h"

/* each signature is over a text that starts with its purpose, so that none passes for another */
#define PI_SIGN_INDEX   "hassmic plugin index 1\n"
#define PI_SIGN_REVOKED "hassmic plugin revoked 1\n"
#define PI_SIGN_LISTING "hassmic plugin listing 2\n"

/* The bounds, in one place.  The index's tool has the same numbers (hassmic-plugins tools/index.py BOUNDS) and
 * refuses to publish past any of them; its tests/test_index.py reads them from this file where this tree is beside
 * it and fails where they differ.  A list past a bound is refused whole and said why (the index's dropping of
 * plugins and versions past PI_PLUGINS_MAX and PI_VERSIONS is counted and shown on the page instead: a list of
 * plugins that says less offers less, a list of withdrawals that says less would let run what it withdrew) */
#define PI_INDEX_MAX        (1u << 20)  /* index.json: some thousand plugins with a few versions each */
#define PI_REVOKED_MAX      (512u << 10)/* revoked.json, as 5.6 bounds it */
#define PI_PLUGINS_MAX      4096        /* plugins kept of one index: more than 1 MB of JSON holds */
#define PI_VERSIONS         16          /* versions kept per plugin, the highest: an Echo installs the newest it runs */
#define PI_REVOCATIONS      2048        /* entries of one revocation list */
#define PI_NAME_MAX         48          /* a plugin's name, in bytes (the manifest's, plugin_store.c) */
#define PI_AUTHOR_MAX       48
#define PI_DESCRIPTION_MAX  240
#define PI_SOURCE_MAX       200
#define PI_VERSION_NAME_MAX 24
#define PI_REASON_MAX       200         /* a revocation's reason */
#define PI_AHEAD_S          86400       /* how far past now a serial may be (both lists' serials are times) */

#define PI_BROAD_MAX        32          /* names in a version's "broad" */
/* a capability's name in a version's "capabilities" and "broad", in bytes: one longer is cut short and unknown in the
 * first, and refuses the whole list in the second (the index's BOUNDS CAP_NAME_MAX holds every name it lists under it) */
#define PI_CAP_NAME_MAX     31

struct pi_version {
    long version; pm_caps_t caps;       /* PM_CAPBIT(PM_CAP_*) of what it asks for, by the names this build knows */
    pm_caps_t broad;                    /* of those, the broad ones the index flags (a community version's "broad") */
    uint8_t caps_unknown;               /* it asks for a capability whose name this build does not know */
    /* its manifest's "requires": the least version of each scope (PM_SCOPE_*; 0 none), as written: a version is a
     * u16 (1 to 65535, the manifest's bound), and none is cut to fit (one above 255 read as 255 looked met on a build
     * at 255: finding 6 of a review of 4e0a202); and whether it names a scope this build does not know (one for a
     * newer hassmic) */
    uint16_t req[PM_SCOPES]; uint8_t req_unknown;
    char version_name[PI_VERSION_NAME_MAX + 1];
    uint8_t manifest[32], sha256[32];   /* its manifest's BLAKE2b-256 (what the listing names), the package's SHA-256 */
    long long size;                     /* the package's bytes */
};
/* The texts point into the index's own pool, each as long as it is (they were fixed arrays of their bounds, 2.6 kB a
 * plugin, 10 MB for a list at 4096 plugins: plugin_index_test measures the peak of a look at the bounds) */
struct pi_plugin {
    char id[PS_ID_MAX];
    const char *name, *author, *description, *source;
    uint8_t author_key[32];
    int tier;                           /* PS_OFFICIAL, PS_COMMUNITY */
    int n; struct pi_version *v;        /* lowest first, as the index has them; the highest PI_VERSIONS */
};
struct pi_index {
    long long serial;
    long long revoked_serial;           /* the revocation list it was built with: taken only with one at least that new */
    int n, dropped;                     /* dropped: plugins past PI_PLUGINS_MAX, versions past PI_VERSIONS (counted, shown) */
    struct pi_plugin *p;
    struct pi_version *vs;              /* every plugin's versions, one allocation */
    char *pool;                         /* every text, one allocation */
};
struct pi_revocation { char id[PS_ID_MAX]; long upto; uint8_t key[32]; int by_key; char reason[PI_REASON_MAX + 1]; };    /* upto 0: every version */
struct pi_revoked { long long serial; int n; struct pi_revocation *r; };

/* the index's base address, "https://.../" (PLUGIN_INDEX, HASSMIC_PLUGIN_INDEX for the tests) */
const char *pi_base(void);
/* the index's public keys, the first and, where one is built in or the tests name one, the second: how many (1, 2),
 * or -1 where a test's file cannot be read or one of them is a key under which any signature holds (edkey.h: then
 * nothing is taken as signed) */
#define PI_KEYS 2
int  pi_keys(uint8_t pk[PI_KEYS][32]);
/* SIG (64 bytes) by an index key (either of pi_keys) over PURPOSE + DATA: 1 */
int  pi_signed(const char *purpose, const void *data, size_t len, const uint8_t *sig, size_t siglen);
/* the same, and which key: 0 the first, 1 the second, -1 neither (a listing's key against the index's, for 5.2's
 * move of listings to a new key: plugin_store.c ps_relist) */
int  pi_signer(const char *purpose, const void *data, size_t len, const uint8_t *sig, size_t siglen);

/* ---- the serials (docs/plugins.md 5.5).  Both lists' serials are times.  This build takes neither list under its
 * least serial (keys/plugins.conf PLUGINS_LEAST_INDEX, PLUGINS_LEAST_REVOKED: what the index had published when it
 * was released, which no factory reset lowers; HASSMIC_PLUGINS_LEAST=<index>:<revoked> for the tests), nor, with the
 * clock set, one more than PI_AHEAD_S past now.  With the clock not set yet a serial further ahead is taken and used.
 * What a list taken holds later lists to (the floor) is the rule of each list's keeper: plugin_index.c "the
 * revocation list this Echo keeps" for that list, plugin_fetch.c pf_look for the index */
long long pi_least_index(void);
long long pi_least_revoked(void);
void pi_clock(int (*synced)(void));     /* main.c: clock_synced; without it (the unit tests) the PC's own time counts as set */
int  pi_clock_set(void);                /* the clock is set (by Home Assistant) */
/* 0: SERIAL may be taken now; -1 and why: it is past what this Echo's clock says can be (set clock only) */
int  pi_serial_ok(long long serial, char *err, size_t cap);
/* the floor line kept beside a list of SERIAL taken now: at least the build's least LEAST, at most what the clock
 * allows now (the build's own time + PI_AHEAD_S while it is not set).  It holds only where the list's serial
 * cannot be believed: past now + PI_AHEAD_S by a clock set later (pi_held) */
long long pi_floor(long long serial, long long least);
/* What a kept list of SERIAL, kept with FLOOR (pi_floor's when it was taken), holds the next list to now, at least
 * LEAST.  The clock set and SERIAL not past now + PI_AHEAD_S: SERIAL, and *RAISE 1 where that is above FLOOR (the
 * caller keeps it as the new floor line).  The clock set and SERIAL past that (a list taken before the clock was set,
 * from the future by it): FLOOR.  The clock not set: SERIAL where CLOCKLESS_KEPT, else FLOOR */
long long pi_held(long long serial, long long floor, long long least, int clockless_kept, int *raise);

/* index.json, whose signature held: 0 and *IX (pi_index_free's to free), or -1 and why.  Read as written: every
 * field this build uses of the kind it is meant to be and within its bounds, or the whole list is refused (a list
 * the key signed that does not read is the tool's fault, not something to take half of); fields it does not use
 * are left alone, so that the index can say more to later builds */
int  pi_index_parse(const char *text, size_t len, struct pi_index *ix, char *err, size_t cap);
void pi_index_free(struct pi_index *ix);
/* the same for revoked.json: more than PI_REVOCATIONS entries, or any entry wrong, and none of it is taken */
int  pi_revoked_parse(const char *text, size_t len, struct pi_revoked *rv, char *err, size_t cap);
void pi_revoked_free(struct pi_revoked *rv);

/* ---- the revocation list this Echo has: state/plugins/.revoked, the newest it was given, whole and with its
 * signature in one file ("floor <n>", the signature's hex, the list: written through statefile.c, so that a power
 * cut leaves the old file or the new one), read and checked again at the first question after every start.
 * A list kept under this build's least serial still withdraws what it names (it can only withdraw too much); it is
 * not current (pi_revoked_current), and any list from the least on replaces it.
 * Its own lock; the store asks with ps_lock held (the order: ps_lock, then this one) */
long long pi_revoked_serial(void);      /* -1: none */
int  pi_revoked_current(void);          /* a list is kept, at least this build's least */
/* a list was kept and does not hold under this build's keys (5.2: a step back to a build of other keys): no list and
 * no word, the week's warning at once (pf_revoked_stale says why) */
int  pi_revoked_unread(void);
enum { PI_TAKEN = 1, PI_SAME = 0, PI_REFUSED = -1, PI_OLDER = -2 };
enum { PI_SITE, PI_MEMBER };            /* where a list came from */
/* a list and its signature from FROM: PI_TAKEN a newer one than kept (taken and kept), PI_SAME the same serial,
 * PI_OLDER an older one than this Echo holds the next to (refused: the one kept stays), PI_REFUSED and why (the
 * signature, the form, a bound, a serial under this build's least or from the future).  Word that the list kept is
 * the newest (pi_revoked_age) is the site's answer (PI_TAKEN or PI_SAME from PI_SITE), now, and a newer list from a
 * member, dated no later than its serial (the time it was made: all a member's list says); a member's same list is no
 * word (any member can hand it over as often as it likes).  The floor line kept never falls below the one before */
int  pi_revoked_take(const char *text, size_t len, const uint8_t *sig, size_t siglen, int from, char *err, size_t cap);
/* the list kept, as the site gave it, for another Echo (GET /api/plugins/revoked): the signature's 128 hex digits, a
 * newline and the list, into *OUT (the caller's to free), its length; 0 when there is none */
size_t pi_revoked_file(char **out);
/* How long ago this Echo last had word that its list is the newest, in seconds: never less than the time it can
 * count (this Echo's running time since the word, kept over restarts), the clock's word where that says more (a clock
 * behind the word's time says nothing: never "fresh" from it); -1 when there was no word at all.  Kept in
 * state/plugins/.revoked-at ("word <wall time or 0> <age>"), written at most once an hour (PI_AT_EVERY_S) */
long long pi_revoked_age(void);
#define PI_AT_EVERY_S 3600
#define PI_STALE_S    (7 * 24 * 3600LL) /* 5.6's warning: no word in a week */
/* the floor line and .revoked-at, when owed: from the daily thread (at least once a minute), no lock held */
void pi_revoked_upkeep(void);
/* 1 when the list withdraws version VERSION of ID or the author key KEY (KEY may be NULL), WHY its reason.  Every
 * tier: since the index key decides what is official, it may withdraw what it listed so (plugins.md 5.6 as built) */
int  pi_withdrawn(const char *id, long version, const uint8_t key[32], char *why, size_t cap);
void pi_forget(void);                   /* tests: what a restart forgets */
void pi_test_skew(long long s);         /* tests: this file's monotonic clock S seconds later */
#endif
