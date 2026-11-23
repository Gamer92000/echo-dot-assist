/*
 * Plugins from the plugin index (docs/plugins.md 5.3, 5.4): the repository hassmic-plugins publishes on GitHub
 * Pages a signed list of plugins (index.json, .sig), a signed list of what was withdrawn (revoked.json, .sig) and
 * every package it lists (packages/<id>-<version>.hmp), each with a listing inside signed by the index key.  The
 * Echo reads them the way update.c fetches its own bundles: update_fetch, i.e. the firmware's libcurl, net_socket,
 * HTTPS against the system's CAs, only the address built in (keys/plugins.conf: PLUGIN_INDEX).  A browser could read
 * the site itself, but it would then have to hand the Echo what it read, and the Echo checks everything anyway: one
 * reader, the one that decides.
 *
 * Trust.  In the key, not in the site: both lists are taken only with their signatures (plugin_index.c), each only
 * with a serial at least as high as the one this Echo holds it to (plugin_index.c pi_held: the newest taken wherever
 * the clock can vouch for it, the revocation list's also without a clock; state/plugins/.serial for the index, the
 * kept list for the revocations), and at least the build's least (keys/plugins.conf); and an index only beside a
 * revocation list at least as new as the one it was made with (its revoked_serial), so whoever can serve the site's
 * files cannot hand an Echo an older list that still offers a version since taken out, nor a new index with an old
 * revocation list (a review, 2026-11-21: the pair
 * was not held together, and a version the newer list withdrew was offered and installed).  A package is downloaded
 * from the address made here of the base, the checked id and the version (never the path the list names, which is
 * not even read), at most the size the list says, and refused unless its size and SHA-256 are the list's.  The
 * store then checks it as an upload: the author's signature, the listing by the index key (which says official or
 * community), the manifest's rules.
 *
 * Which version.  Per plugin the highest the list has whose requires this build meets (every scope at its version or
 * a later one: sdk/PROTOCOL.md "Versions"), not withdrawn (5.6: every tier), not under the build's least for an
 * official one (5.2).  The list says what every version requires, so nothing is downloaded to find out.
 *
 * Privacy.  A look is four requests to GitHub Pages (index.json, revoked.json and their signatures), a download
 * one, naming nothing but the files and saying "hassmic" as what asks, no version (update.c fetch's UA): no
 * version of hassmic's, no plugin installed here.  They are made only when the owner asked (plugin_fetch.h).  How
 * often is the Echo's to hold, not the page's (a page is whatever a browser runs): what was looked at is kept with
 * its time, a look that failed like one that found the list, and pf_look asks again only when that is older than
 * the asker's pace: an hour for what a page does by itself when it is opened (PF_AUTO), 30 s for a press of "Look
 * again" or of "Install" (PF_ASKED).  The Echo's own looks (PF_DAILY) leave that as it was where they fail.
 *
 * The revocation list by itself (5.6, pf_start's thread): once a day at a random time, while plugins are on and at
 * least one is installed, revoked.json and its signature and nothing else (the index too where a listing installed
 * may be one it lists anew: daily_fetch, and sooner where one does not hold here: relist_tick); and from the other Echos of the
 * arbitration network, which tell each other the serial of the list they have (arb's T_MSG, M_REVOKED) and hand it
 * over from their settings port (GET /api/plugins/revoked: the file as kept, to members only, a request once:
 * pf_revoked_serve).  Each place a member's word makes is asked once in ten minutes, and all of them a few times a
 * minute (MEMBERS_*): who names a member is the member, and a member with the key names as many as it likes.
 * Neither is asked for by the owner: the page's Plugins card says so, and neither runs with plugins off.
 * Why arb's message and not the settings page's member calls: the announcement is one small message on a channel
 * that already authenticates the members and is heard by every Echo whatever page is open; the page's calls run
 * only in a browser that shows the other Echos, and would make the browser a courier of what the Echos can carry
 * themselves.  New: one message kind and one GET for members that serves a public, signed file.
 */
#include "plugin_fetch.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "arb.h"
#include "hash.h"
#include <stdint.h>
#include "json.h"
#include "net.h"
#include "plugin.h"
#include "plugin_index.h"
#include "plugin_net.h"
#include "statefile.h"
#include "update.h"
#include "../third_party/monocypher.h"

#define FRESH_S     3600                /* PF_AUTO: a page opened, again and again */
#define FORCED_S    30                  /* PF_ASKED: "Look again" pressed over and over */
#define SIG_MAX     1024                /* a signature file is 64 bytes: anything longer is no signature */

enum { PF_NEVER, PF_OK, PF_UNREACHABLE, PF_FAILED,
       PF_CANCELLED };                  /* a look's own end, never what is known: a cancelled look leaves the list as it was */
static pthread_mutex_t fl = PTHREAD_MUTEX_INITIALIZER;          /* the list */
static pthread_mutex_t looking_mu = PTHREAD_MUTEX_INITIALIZER;  /* one look at a time; never taken under fl */
static struct { int state, looking; time_t at; char err[240], note[240]; struct pi_index ix; int key; } known;  /* fl; note: what the look found beside its answer; key: the index key that signed it (pi_signer) */

/* 5.2's move to a new index key (a review of a30d6ed, K2): after a look (the page's, or the daily one where a listing
 * installed may want it: daily_fetch), the index's listing of each version installed
 * here that it lists anew (another key, a higher tier, or one under the build's least: plugin_store.c ps_relist_wanted)
 * is downloaded and taken in place of the installed one's (ps_relist), in a thread of its own, one package at a time.
 * What became of each, by id, for the page ("Browse plugins" says it on the row, and offers to try again): pending,
 * taken, nothing to take (the package's own answer, ps_relist 0 or -1: that package is not asked for again), failed
 * (the download, or ps_relist's -2, memory or the flash: asked again at the next look, at most every RELIST_AGAIN_S,
 * the daily one included, or at once by the owner's "relist") */
enum { R_NONE, R_PENDING, R_TAKEN, R_NOTHING, R_FAILED };
#define RELIST_AGAIN_S 600
static struct relist { char id[PS_ID_MAX]; long version; uint8_t sha256[32]; int state; time_t at; char err[160]; } relists[PS_MAX];      /* fl */
static int relisting;                                                                                                       /* fl */
/* fl: the versions installed here the last look that had the list found not listed for this build (none of that
 * number, API and manifest, or withdrawn, or over the bound): while the index stays so, no look moves their
 * listings, so they are no reason for relist_tick to read it (plugin_store.c ps_relist_reason).  The daily look reads
 * it all the same while such a listing does not hold here (daily_fetch, T4 of a review of 9cd7a77: the index may list
 * the version again, a revocation be lifted).  Made anew by every look that has the list; an entry of relists[]
 * whose version is no longer installed is dropped (relist_upkeep).  Both kept the daily look reading index.json for
 * good: a failure for a plugin removed or updated since, a listing of a version the index had dropped (a review of
 * 3fcd319, S4) */
static struct ps_idv unlisted[PS_MAX]; static int nunlisted;                                                                 /* fl */
/* hassmic ends (pf_shutdown): the thread's own looks and downloads cancelled, nothing more asked */
static volatile int pf_stop;

/* The newest version of each plugin any look ever found listed: what a copy from another Echo is held against
 * (web.c sync_judge: never a version older than one this Echo has seen offered; pf_seen).  Kept apart from the
 * list, which is one look's and gone with a restart or a look that failed: state/plugins/.listed (a dot: no
 * plugin's id), a line "<id> <version>" each, only ever raised, written when a look raises one (a new version of
 * a plugin: rare).  A restart must not forget it: the settings page restarts an Echo for any approved browser, and
 * the rule is there for the one that is not the owner's.
 * What it is worth: the index's signed word, but a version that is there is not one an Echo has to take; it only
 * ever makes a copy ask for this Echo's button, never lets one in.  PF_SEEN ids: far more than an index of 1 MB
 * lists; an id beyond them is one this Echo has seen no list for (pf_seen 0) */
#define PF_SEEN PI_PLUGINS_MAX
struct seen { char id[PS_ID_MAX]; long version; };
static struct seen *seen; static int nseen, cseen, seen_read;           /* fl */
/* fl: the index's floor line (pi_floor of the newest taken) and that index's serial, state/plugins/.serial "index <floor>
 * <serial>" ("index <floor>" before 2026-11-21's second round: the serial is then the floor).  What the next has to
 * reach: pi_held (the serial wherever the clock set vouches for it, the line raised to it then) */
static long long serial_seen = -1, serial_kept = -1; static int serial_read;

static void seen_path(char *p, size_t cap) { snprintf(p, cap, "%s/.listed", ps_dir()); }
static void serial_path(char *p, size_t cap) { snprintf(p, cap, "%s/.serial", ps_dir()); }

static int seen_add(const char *id, long v)      /* fl held: its index, -1 no room */
{
    if (nseen == PF_SEEN) return -1;
    if (nseen == cseen) {
        int nc = cseen ? cseen * 2 : 32; struct seen *ns = realloc(seen, sizeof *ns * (size_t)nc);
        if (!ns) return -1;
        seen = ns; cseen = nc;
    }
    snprintf(seen[nseen].id, sizeof seen[0].id, "%s", id); seen[nseen].version = v;
    return nseen++;
}

static void seen_load(void)     /* fl held; once */
{
    char p[400], line[80], id[PS_ID_MAX]; long v; FILE *f;
    if (seen_read) return;
    seen_read = 1; seen_path(p, sizeof p);
    if ((f = ps_fopen(p))) {
        while (nseen < PF_SEEN && fgets(line, sizeof line, f))
            if (sscanf(line, "%24s %ld", id, &v) == 2 && ps_id_ok(id) && v > 0) seen_add(id, v);
        fclose(f);
    }
}

static void serial_load(void)   /* fl held; once */
{
    char p[400]; long long v; FILE *f;
    if (serial_read) return;
    serial_read = 1; serial_path(p, sizeof p);
    if ((f = ps_fopen(p))) {
        long long k = -1; int got = fscanf(f, "index %lld %lld", &v, &k);
        if (got >= 1 && v >= 0) { serial_seen = v; serial_kept = got == 2 && k >= v ? k : v; }
        fclose(f);
    }
}

static int unmet(const struct pi_version *v, char *need, size_t cap);
/* fl held: what a look found.  The text to write into *T (the caller's to write and free, without fl: the flash)
 * when something rose, else NULL.  Only by the versions this build runs (their requires met): a version made for a
 * newer hassmic is no version a copy here could be, and held as the floor it refused every copy of the versions this
 * hassmic runs, button or not (a review, 2026-11-21) */
static void seen_raise(const struct pi_index *ix, char **t, size_t *tn)
{
    int rose = 0; size_t cap, o = 0;
    *t = NULL; *tn = 0;
    seen_load();
    for (int i = 0; i < ix->n; i++) {
        const struct pi_plugin *p = &ix->p[i]; long top = 0; int k;
        for (int v = 0; v < p->n; v++) if (!unmet(&p->v[v], NULL, 0) && p->v[v].version > top) top = p->v[v].version;
        if (!top) continue;
        for (k = 0; k < nseen && strcmp(seen[k].id, p->id); k++) ;
        if (k == nseen && (k = seen_add(p->id, 0)) < 0) continue;
        if (top > seen[k].version) { seen[k].version = top; rose = 1; }
    }
    cap = (size_t)nseen * 40 + 1;
    if (!rose || !(*t = malloc(cap))) return;
    for (int k = 0; k < nseen; k++) o += (size_t)snprintf(*t + o, cap - o, "%s %ld\n", seen[k].id, seen[k].version);
    *tn = o;
}

long pf_seen(const char *id)
{
    long v = 0;
    pthread_mutex_lock(&fl);
    seen_load();
    for (int k = 0; k < nseen; k++) if (!strcmp(seen[k].id, id)) v = seen[k].version;
    pthread_mutex_unlock(&fl);
    return v;
}

/* HASSMIC_PLUGIN_FRESH=<s>[:<s when forced>] for the tests, which change what the stand-in site has from one line
 * to the next */
static long fresh_s(int force)
{
    const char *e = getenv("HASSMIC_PLUGIN_FRESH"), *c = e ? strchr(e, ':') : NULL;
    if (!e) return force ? FORCED_S : FRESH_S;
    return force && c ? atol(c + 1) : atol(e);
}

static time_t now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + 1; }    /* (+1: 0 is "never") */

/* libcurl's words about URL without the URL in front: what the page shows */
static const char *bare(const char *why, const char *url)
{
    size_t ul = strlen(url);
    return strncmp(why, url, ul) || strlen(why) < ul + 2 ? why : why + ul + 2;
}

static long long mono_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000; }

/* one file of the site into *D: 0, or -1 with *state and err.  UNTIL (monotonic ms, 0 none): what is left of the
 * asker's time bounds the request, 15 s at most as always; none left: PF_CANCELLED, "no time" (nothing found out
 * about the site, so nothing of what is known changes) */
static int get(const char *name, size_t max, const volatile int *stop, long long until, unsigned char **d, size_t *n, int *state, char *err, size_t cap)
{
    char url[600], why[300]; long secs = 15;
    if (until) {
        long long left = until - mono_ms();
        if (left < 1000) { *state = PF_CANCELLED; snprintf(err, cap, "no time was left to read the plugin index"); return -1; }
        if (left / 1000 < secs) secs = (long)(left / 1000);
    }
    snprintf(url, sizeof url, "%s%s", pi_base(), name);
    if (!update_fetch(url, max, secs, stop, d, n, why, sizeof why)) return 0;
    if (!strcmp(why, "cancelled") || (stop && *stop)) { *state = PF_CANCELLED; snprintf(err, cap, "cancelled"); }
    else if (until && mono_ms() >= until - 1000) { *state = PF_CANCELLED; snprintf(err, cap, "no time was left to read the plugin index"); }
    /* any answer over HTTP is the site reached; what fails otherwise is the name lookup, the connection or TLS, on
     * an Echo kept off the internet */
    else if (!strncmp(why, "HTTP ", 5)) { *state = PF_FAILED; snprintf(err, cap, "the plugin index answered %.8s for %s", why, name); }
    else if (strstr(why, ": more than ")) { *state = PF_FAILED; snprintf(err, cap, "the plugin index's %s is larger than this hassmic reads (%zu kB)", name, max >> 10); }
    else { *state = PF_UNREACHABLE; snprintf(err, cap, "%s", bare(why, url)); }
    return -1;
}

/* what is known, as a look's answer: 0, or -1 and the reason kept from the look that failed (fl held) */
static int known_answer(char *err, size_t cap)
{
    if (known.state == PF_OK) return 0;
    if (known.state == PF_UNREACHABLE) snprintf(err, cap, "this Echo cannot reach the plugin index (%.150s)", known.err); else snprintf(err, cap, "%s", known.err);
    return -1;
}

static const struct pi_plugin *find(const struct pi_index *ix, const char *id)   /* fl held */
{
    for (int i = 0; i < ix->n; i++) if (!strcmp(ix->p[i].id, id)) return &ix->p[i];
    return NULL;
}

static void took(void);

int pf_look(const char *want, int pace, const volatile int *stop, char *err, size_t cap) { return pf_look_until(want, pace, stop, 0, err, cap); }

int pf_look_until(const char *want, int pace, const volatile int *stop, long long until, char *err, size_t cap)
{
    unsigned char *ij = NULL, *is = NULL, *rj = NULL, *rs = NULL; size_t ijn = 0, isn = 0, rjn = 0, rsn = 0;
    char why[240] = "", note[240] = "", *st = NULL; size_t stn = 0; int state = PF_OK, fresh, rc, withdrawn = 0, raise = 0, wrote = 0, by = -1; long long floor;
    struct pi_index ix = { 0 };
    /* (not blocked in the lock behind a look whose thread sits in a name lookup: a stop is heard here too) */
    while (pthread_mutex_trylock(&looking_mu)) {
        struct timespec ts = { 0, 50000000 };
        if (stop && *stop) { snprintf(err, cap, "cancelled"); return -1; }
        if (until && mono_ms() >= until) { snprintf(err, cap, "no time was left to read the plugin index: another look at it was under way"); return -1; }
        nanosleep(&ts, NULL);
    }
    pthread_mutex_lock(&fl);
    /* Whoever waited for another's look has its answer.  What is known stands for as long as the asker's pace
     * says, whatever it is: the list, an empty list, or why there is none (a look that failed is one the site was
     * asked for too).  And for the hour in any case where a plugin is asked for and the list has it: then nothing
     * more is to be found out by asking */
    { long age = known.state == PF_NEVER ? -1 : (long)(now_s() - known.at);
      fresh = pace != PF_NOW && pace != PF_DAILY && age >= 0 && age < fresh_s(pace == PF_ASKED);
      if (!fresh && pace != PF_NOW && pace != PF_DAILY && age >= 0 && age < fresh_s(0) && known.state == PF_OK && want && find(&known.ix, want)) fresh = 1; }
    known.looking = !fresh;             /* (pf_look_async marked it before it knew) */
    rc = fresh ? known_answer(err, cap) : 0;
    pthread_mutex_unlock(&fl);
    if (fresh) { pthread_mutex_unlock(&looking_mu); return rc; }

    /* The revocation list first, and taken whatever becomes of the index: it proves itself, and a look that fails
     * for the index (an older one, one that does not read) still brings the newest word on what was withdrawn.
     * Not taken (its signature, a bound, a serial from the future or under the build's least): the look fails, a
     * list of plugins without the newest word on which were withdrawn would offer them again.  Older than the one
     * this Echo has: that one stays, said beside the answer, and the index is held to it by the pair's rule below
     * (the whole look failed, and with it every install, while the site served an older list than a member had
     * handed over; a review, 2026-11-21) */
    if (get("revoked.json", PI_REVOKED_MAX, stop, until, &rj, &rjn, &state, why, sizeof why) || get("revoked.json.sig", SIG_MAX, stop, until, &rs, &rsn, &state, why, sizeof why)) goto done;
    withdrawn = pi_revoked_take((const char *)rj, rjn, rs, rsn, PI_SITE, why, sizeof why);
    if (withdrawn == PI_OLDER) {
        snprintf(note, sizeof note, "the plugin index's list of withdrawn plugins is older than the one this Echo has (from another Echo of its network, or from an earlier look): this Echo keeps its own");
        fprintf(stderr, "plugins: %s\n", why);
        withdrawn = 0; why[0] = 0;
    } else if (withdrawn < 0) { state = PF_FAILED; withdrawn = 0; goto done; }
    if (get("index.json", PI_INDEX_MAX, stop, until, &ij, &ijn, &state, why, sizeof why) || get("index.json.sig", SIG_MAX, stop, until, &is, &isn, &state, why, sizeof why)) goto done;
    /* the signature first: nothing of a list is read before its signer is known */
    if ((by = pi_signer(PI_SIGN_INDEX, ij, ijn, is, isn)) < 0) { state = PF_FAILED; snprintf(why, sizeof why, "the plugin index's signature does not hold: this is not a list its key signed, and nothing of it was taken"); goto done; }
    if (pi_index_parse((const char *)ij, ijn, &ix, why, sizeof why)) { state = PF_FAILED; goto done; }
    if (ix.serial < pi_least_index()) { state = PF_FAILED; snprintf(why, sizeof why, "the plugin index answered with an older list (serial %lld) than this hassmic takes (%lld): not taken", ix.serial, pi_least_index()); goto done; }
    { char e2[200]; if (pi_serial_ok(ix.serial, e2, sizeof e2)) { state = PF_FAILED; snprintf(why, sizeof why, "the plugin index: %s", e2); goto done; } }
    pthread_mutex_lock(&fl); serial_load(); floor = pi_held(serial_kept, serial_seen, pi_least_index(), 0, &raise); if (raise) { serial_seen = serial_kept; wrote = 1; } pthread_mutex_unlock(&fl);
    if (ix.serial < floor) { state = PF_FAILED; snprintf(why, sizeof why, "the plugin index answered with an older list (serial %lld) than this Echo has seen (%lld): not taken", ix.serial, serial_kept); goto done; }
    /* the pair: an index is made with a revocation list (its build leaves out what that list withdrew, and names its
     * serial); beside an older one than that, the site's files are of two times, and the newer withdrawals are not
     * here to hold its offers to */
    if (pi_revoked_serial() < ix.revoked_serial) {
        state = PF_FAILED;
        snprintf(why, sizeof why, "the plugin index was made with a newer revocation list (serial %lld) than the one this Echo has (%lld): its files are not of one time, nothing of it was taken", ix.revoked_serial, pi_revoked_serial());
        goto done;
    }
done:
    pthread_mutex_lock(&fl);
    known.looking = 0;
    /* a look someone cancelled found nothing out: the list every browser reads, its age and its reason stay as
     * they were (before, one browser's cancelled fetch left every page an empty list that "failed: cancelled").
     * Nor does one the Echo made by itself that failed (PF_DAILY): the page's list, and the hour PF_AUTO holds it,
     * are what a page or the owner asked for; the failure is in the log */
    if (state != PF_CANCELLED && !(pace == PF_DAILY && state != PF_OK)) {
        known.state = state; known.at = now_s(); snprintf(known.err, sizeof known.err, "%s", why); snprintf(known.note, sizeof known.note, "%s", note);
        pi_index_free(&known.ix);
        if (state == PF_OK) {
            known.ix = ix; memset(&ix, 0, sizeof ix); known.key = by;
            seen_raise(&known.ix, &st, &stn);
            /* the floor line this index sets (plugin_index.c pi_floor: its serial, or the most the clock allowed),
             * and the newest serial taken: an older one than that replaces it only where the clock set says that
             * one is from the future (without a clock an older index is taken from the floor line on, and the
             * newest stays what the floor rises to once the clock vouches for it) */
            { long long f = pi_floor(known.ix.serial, pi_least_index()), s2 = known.ix.serial;
              if (serial_kept > s2 && !(pi_clock_set() && serial_kept > (long long)time(NULL) + PI_AHEAD_S)) s2 = serial_kept;
              if (f < serial_seen) f = serial_seen;
              if (f != serial_seen || s2 != serial_kept) { serial_seen = f; serial_kept = s2; wrote = 1; } }
        }
    }
    { long long a = serial_seen, k = serial_kept; pthread_mutex_unlock(&fl);
      /* (under looking_mu: one look writes at a time, in the order they ended) */
      if (wrote) {
          char p[400], t[60]; int n = snprintf(t, sizeof t, "index %lld %lld\n", a, k);
          mkdir(ps_dir(), 0700); serial_path(p, sizeof p);
          if (statefile_write(p, 0600, t, (size_t)n)) fprintf(stderr, "plugins: cannot write %s: the index's serial is held until the next start only\n", p);
      } }
    pi_index_free(&ix);
    free(ij); free(is); free(rj); free(rs);
    if (st) { char p[400]; mkdir(ps_dir(), 0700); seen_path(p, sizeof p); if (statefile_write(p, 0600, st, stn)) fprintf(stderr, "plugins: cannot write %s: the versions seen listed are known until the next start only\n", p); free(st); }
    if (state == PF_CANCELLED) { snprintf(err, cap, "%s", why[0] ? why : "cancelled"); if (strcmp(err, "cancelled")) fprintf(stderr, "plugins: the plugin index not looked at: %s\n", err); }
    else if (state == PF_OK) {
        pthread_mutex_lock(&fl);
        fprintf(stderr, "plugins: the plugin index looked at: serial %lld, %d plugin%s%s; revocations: serial %lld%s\n", known.ix.serial, known.ix.n, known.ix.n == 1 ? "" : "s",
                known.ix.dropped ? " (some left out: more than this hassmic keeps)" : "", pi_revoked_serial(), withdrawn ? ", new" : "");
        pthread_mutex_unlock(&fl);
    } else {
        fprintf(stderr, "plugins: the plugin index not looked at: %s%s\n", state == PF_UNREACHABLE ? "not reachable: " : "", why);
        if (state == PF_UNREACHABLE) snprintf(err, cap, "this Echo cannot reach the plugin index (%.150s)", why); else snprintf(err, cap, "%s", why);
    }
    pthread_mutex_unlock(&looking_mu);
    /* a newer revocation list: what it names that runs here is stopped now, not at the next start, and the other
     * Echos are told (no lock held) */
    if (withdrawn) took();
    return state == PF_OK ? 0 : -1;
}

/* ---------------------------------------------------------------- the index's listings for what is installed (K2) */

static struct relist *relist_of(const char *id)         /* fl held: ID's place, a free one, NULL none */
{
    struct relist *free_ = NULL;
    for (int i = 0; i < PS_MAX; i++) {
        if (!strcmp(relists[i].id, id)) return &relists[i];
        if (!free_ && !relists[i].id[0]) free_ = &relists[i];
    }
    if (!free_) for (int i = 0; i < PS_MAX; i++) if (relists[i].state != R_PENDING) { free_ = &relists[i]; break; }
    if (free_) { memset(free_, 0, sizeof *free_); snprintf(free_->id, sizeof free_->id, "%s", id); }
    return free_;
}

static int withdrawn_v(const struct pi_plugin *p, const struct pi_version *v);
struct relist_job { int n; struct pf_asset a[PS_MAX]; };
static void *relist_thread(void *arg)
{
    struct relist_job *j = arg; int took = 0;
    for (int i = 0; i < j->n; i++) {
        unsigned char *d = NULL; size_t len = 0; char err[240] = ""; int r; struct relist *q;
        /* (a minute a package: a few hundred kB from GitHub Pages; pf_download looks again where the package is not the
         * list's, which runs no second relisting: relisting is set) */
        r = pf_download(&j->a[i], 60, &pf_stop, &d, &len, err, sizeof err);
        if (!r) { r = ps_relist(d, len, err, sizeof err); if (r == 0) snprintf(err, sizeof err, "nothing to take"); }
        else r = -2;
        free(d);
        pthread_mutex_lock(&fl);
        if ((q = relist_of(j->a[i].id))) {
            q->state = r > 0 ? R_TAKEN : r == 0 || r == -1 ? R_NOTHING : R_FAILED; q->at = now_s(); snprintf(q->err, sizeof q->err, "%s", r > 0 ? "" : err);
        }
        pthread_mutex_unlock(&fl);
        if (r > 0) took = 1;
        else fprintf(stderr, "plugins: %s version %ld: the plugin index's listing not taken: %s\n", j->a[i].id, j->a[i].version, err);
    }
    pthread_mutex_lock(&fl); relisting = 0; pthread_mutex_unlock(&fl);
    free(j);
    /* what it changed: a plugin a file's until now (community plugins off) starts, the wake word engine's comes back,
     * and the cards say the new tier (no lock held) */
    if (took) plugin_withdrawn_changed();
    return NULL;
}

/* After a look that has the list: what of it is a new listing of a version installed here.  ASKED: the owner's
 * "relist" (also what failed within RELIST_AGAIN_S) */
static void relist_due(int asked)
{
    const struct ps_plugin *l; int n, k = 0; pthread_t t;
    struct mine { char id[PS_ID_MAX]; long version; uint8_t digest[32]; } *mine = calloc(PS_MAX, sizeof *mine);
    struct relist_job *j = calloc(1, sizeof *j);
    if (!mine || !j) { free(mine); free(j); return; }
    ps_lock(); n = ps_list(&l);
    for (int i = 0; i < n && k < PS_MAX; i++) if (l[i].here && !l[i].unread) { snprintf(mine[k].id, sizeof mine[k].id, "%s", l[i].m.id); mine[k].version = l[i].m.version; memcpy(mine[k].digest, l[i].m.manifest_digest, 32); k++; }
    ps_unlock();
    /* (the index's versions of what is installed, under fl; whether the store wants each is asked without it: the
     * store's lock is never taken under fl) */
    pthread_mutex_lock(&fl);
    if (known.state != PF_OK) { pthread_mutex_unlock(&fl); free(mine); free(j); return; }
    nunlisted = 0;
    { long long serial = known.ix.serial; int key = known.key;
      for (int i = 0; i < k; i++) {
          const struct pi_plugin *p = find(&known.ix, mine[i].id); const struct pi_version *v = NULL;
          for (int x = 0; p && x < p->n; x++) if (p->v[x].version == mine[i].version && !memcmp(p->v[x].manifest, mine[i].digest, 32)) v = &p->v[x];
          if (!v || withdrawn_v(p, v) || v->size > PS_PACKAGE_MAX) {
              snprintf(unlisted[nunlisted].id, sizeof unlisted[0].id, "%s", mine[i].id); unlisted[nunlisted++].version = mine[i].version;
              mine[i].id[0] = 0; continue;
          }
          if (relisting) continue;      /* (what is listed is known now; a relisting under way takes its own) */
          { struct relist *q = relist_of(p->id);
            /* that package already: nothing to take from it, or failed a short while ago */
            if (q && q->version == v->version && !memcmp(q->sha256, v->sha256, 32)
                && (q->state == R_NOTHING || q->state == R_TAKEN || (q->state == R_FAILED && !asked && now_s() - q->at < RELIST_AGAIN_S))) { mine[i].id[0] = 0; continue; } }
          memset(&j->a[j->n], 0, sizeof j->a[0]);
          snprintf(j->a[j->n].id, sizeof j->a[0].id, "%s", p->id); j->a[j->n].version = v->version; j->a[j->n].size = v->size; j->a[j->n].tier = p->tier;
          memcpy(j->a[j->n].sha256, v->sha256, 32); memcpy(j->a[j->n].manifest, v->manifest, 32);
          j->n++;
      }
      pthread_mutex_unlock(&fl);
      { int w = 0; for (int i = 0; i < j->n; i++) if (ps_relist_wanted(j->a[i].id, j->a[i].version, j->a[i].manifest, j->a[i].tier, key, serial)) j->a[w++] = j->a[i]; j->n = w; }
    }
    free(mine);
    pthread_mutex_lock(&fl);
    if (!j->n || relisting) { pthread_mutex_unlock(&fl); free(j); return; }
    relisting = 1;
    for (int i = 0; i < j->n; i++) {
        struct relist *q = relist_of(j->a[i].id);
        if (q) { snprintf(q->id, sizeof q->id, "%s", j->a[i].id); q->version = j->a[i].version; memcpy(q->sha256, j->a[i].sha256, 32); q->state = R_PENDING; q->at = now_s(); q->err[0] = 0; }
    }
    pthread_mutex_unlock(&fl);
    if (pthread_create(&t, NULL, relist_thread, j)) { pthread_mutex_lock(&fl); relisting = 0; pthread_mutex_unlock(&fl); free(j); }
    else pthread_detach(t);
}

static void *look_thread(void *arg)
{
    char err[240]; int pace = (int)(intptr_t)arg;
    if (!pf_look(NULL, pace == PF_NOW ? PF_ASKED : pace, NULL, err, sizeof err)) relist_due(pace == PF_NOW);
    return NULL;
}

void pf_look_async(int pace)
{
    pthread_t t; int go;
    /* marked here already, so that the page's next question is answered "looking" and a second press starts none */
    pthread_mutex_lock(&fl); go = !known.looking; if (go) known.looking = 1; pthread_mutex_unlock(&fl);
    if (!go) return;
    if (pthread_create(&t, NULL, look_thread, (void *)(intptr_t)pace)) { pthread_mutex_lock(&fl); known.looking = 0; pthread_mutex_unlock(&fl); }
    else pthread_detach(t);
}

void pf_relist_async(void) { pf_look_async(PF_NOW); }

/* ---------------------------------------------------------------- which version, for the page and for an install */

/* (every tier: the index key that lists a plugin as official may withdraw it, 5.6 as built) */
static int withdrawn_v(const struct pi_plugin *p, const struct pi_version *v)
{
    return pi_withdrawn(p->id, v->version, p->author_key, NULL, 0);
}
/* V's requires against this build's scopes (ps_scope_have): 0 met; else how many it names that this build lacks (a
 * scope it has not, or a later version), and in NEED (may be NULL) what they are, "button 2, ...", as the page says
 * them (a scope this build does not know by name: "a scope this hassmic does not know") */
static int unmet(const struct pi_version *v, char *need, size_t cap)
{
    int n = 0; size_t o = 0;
    if (need && cap) need[0] = 0;
    for (int s = 0; s < PM_SCOPES; s++)
        if (v->req[s] > ps_scope_have(s)) {
            if (need && o < cap) { int k = snprintf(need + o, cap - o, "%s%s %u", n ? ", " : "", pm_scope_name(s), (unsigned)v->req[s]); o = k < 0 ? o : o + (size_t)k; }
            n++;
        }
    if (v->req_unknown) { if (need && o < cap) snprintf(need + o, cap - o, "%sa scope this hassmic does not know", n ? ", " : ""); n++; }
    return n;
}
static int runs_here(const struct pi_plugin *p, const struct pi_version *v)
{
    return !unmet(v, NULL, 0) && !v->caps_unknown && v->size <= PS_PACKAGE_MAX && (p->tier != PS_OFFICIAL || v->version >= ps_official_least(p->id));
}
/* fl held: the highest version not withdrawn (TOP), and the highest of those this build installs (HERE); NULL none */
static void pick(const struct pi_plugin *p, const struct pi_version **top, const struct pi_version **here)
{
    *top = *here = NULL;
    for (int k = p->n - 1; k >= 0; k--) {
        if (withdrawn_v(p, &p->v[k])) continue;
        if (!*top) *top = &p->v[k];
        if (runs_here(p, &p->v[k])) { *here = &p->v[k]; return; }
    }
}

int pf_best(const char *id, struct pf_asset *a, char *why, size_t cap)
{
    const struct pi_plugin *p; const struct pi_version *top, *here; char need[160]; int r = 0;
    pthread_mutex_lock(&fl);
    if (!(p = find(&known.ix, id))) snprintf(why, cap, "the plugin index does not list %s", id);
    else if (pick(p, &top, &here), here) {
        memset(a, 0, sizeof *a);
        snprintf(a->id, sizeof a->id, "%s", p->id); a->version = here->version; a->size = here->size; a->tier = p->tier;
        memcpy(a->sha256, here->sha256, 32); memcpy(a->manifest, here->manifest, 32);
        r = 1;
    }
    else if (!p->n) snprintf(why, cap, "%s has no released version yet", p->name);
    else if (!top) snprintf(why, cap, "the plugin index withdrew every version of %s", p->name);
    else if (unmet(top, need, sizeof need)) snprintf(why, cap, "the newest version of %s needs a newer hassmic (%s), and none this one has the scopes for is listed", p->name, need);
    else if (p->tier == PS_OFFICIAL && top->version < ps_official_least(p->id)) snprintf(why, cap, "version %ld of %s was withdrawn: this hassmic runs it from version %ld", top->version, p->name, ps_official_least(p->id));
    else if (top->size > PS_PACKAGE_MAX) snprintf(why, cap, "the package of %s is over %d MB, which no plugin package is", p->name, PS_PACKAGE_MAX >> 20);
    else snprintf(why, cap, "%s asks for something this hassmic does not have yet", p->name);
    pthread_mutex_unlock(&fl);
    return r;
}

/* ---------------------------------------------------------------- the page's list */

struct buf { char *p; size_t n, cap; int oom; };
static void put(struct buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(struct buf *b, const char *fmt, ...)
{
    va_list ap; int k;
    if (b->oom) return;
    for (;;) {
        va_start(ap, fmt); k = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap); va_end(ap);
        if (k < 0) { b->oom = 1; return; }
        if ((size_t)k < b->cap - b->n) { b->n += (size_t)k; return; }
        { size_t nc = b->cap * 2 + (size_t)k; char *np = realloc(b->p, nc); if (!np) { b->oom = 1; return; } b->p = np; b->cap = nc; }
    }
}
static void putj(struct buf *b, const char *s)      /* a JSON string */
{
    char e[1600];
    if (json_esc(e, sizeof e, s) < 0) json_esc(e, sizeof e, "");
    put(b, "%s", e);
}
static void caps(struct buf *b, pm_caps_t c)    /* names, as the manifest has them */
{
    int first = 1;
    put(b, "[");
    for (int bit = 0; ps_cap_name(bit); bit++)
        if (c & PM_CAPBIT(bit)) { put(b, "%s\"%s\"", first ? "" : ",", ps_cap_name(bit)); first = 0; }
    put(b, "]");
}
/* V's requires as the manifest has them ({"core":1,...}, the scopes this build knows by name), and what of them this
 * build lacks ("needs": ["button 2", ...], unmet's words, one each) */
static void requires(struct buf *b, const struct pi_version *v)
{
    int k = 0;
    put(b, "\"requires\":{");
    for (int s = 0; s < PM_SCOPES; s++) if (v->req[s]) put(b, "%s\"%s\":%u", k++ ? "," : "", pm_scope_name(s), (unsigned)v->req[s]);
    put(b, "},\"needs\":[");
    k = 0;
    for (int s = 0; s < PM_SCOPES; s++) if (v->req[s] > ps_scope_have(s)) put(b, "%s\"%s %u\"", k++ ? "," : "", pm_scope_name(s), (unsigned)v->req[s]);
    if (v->req_unknown) put(b, "%s\"a scope this hassmic does not know\"", k ? "," : "");
    put(b, "]");
}

size_t pf_json(char **out)
{
    static const char *const names[] = { "never", "ok", "unreachable", "failed" };
    struct buf b = { malloc(65536), 0, 65536, 0 }; char fp[20];
    *out = NULL;
    if (!b.p) return 0;
    pthread_mutex_lock(&fl);
    put(&b, "{\"state\":\"%s\",\"error\":", known.looking ? "looking" : names[known.state]); putj(&b, known.err);
    put(&b, ",\"note\":"); putj(&b, known.note);
    /* site: where the index is, for the page to name to an owner whose Echo cannot reach it (the base built in) */
    /* revoked_age: how long ago this Echo last had word that its revocation list is the newest (any source), -1 not
     * known; the page says it beside the list's own age (docs/SECURITY.md: what the network can do is keep it away) */
    put(&b, ",\"age\":%ld,\"serial\":%lld,\"revoked_serial\":%lld,\"revoked_age\":%lld,\"dropped\":%d,\"scopes\":{", known.at ? (long)(now_s() - known.at) : -1L, known.ix.serial,
        pi_revoked_serial(), pi_revoked_age(), known.ix.dropped);
    /* scopes: this build's versions, which a version's requires are held to (the page names what one needs) */
    for (int s = 0; s < PM_SCOPES; s++) put(&b, "%s\"%s\":%u", s ? "," : "", pm_scope_name(s), ps_scope_have(s));
    put(&b, "},\"site\":");
    putj(&b, pi_base());
    put(&b, ",\"plugins\":[");
    for (int i = 0, first = 1; i < known.ix.n; i++) {
        const struct pi_plugin *p = &known.ix.p[i]; const struct pi_version *top, *here;
        pick(p, &top, &here);
        if (p->n && !top) continue;                 /* every version withdrawn: not offered at all */
        ps_fingerprint(p->author_key, fp);
        put(&b, "%s{\"id\":\"%s\",\"name\":", first ? "" : ",", p->id); putj(&b, p->name);
        put(&b, ",\"author\":"); putj(&b, p->author);
        put(&b, ",\"description\":"); putj(&b, p->description);
        put(&b, ",\"source\":"); putj(&b, p->source);
        put(&b, ",\"tier\":\"%s\",\"key\":\"%s\",\"here\":", p->tier == PS_OFFICIAL ? "official" : "community", fp);
        /* broad: the capabilities the index flags as broad for that version (a community one's; 5.3), for the page */
        if (here) { put(&b, "{\"version\":%ld,\"version_name\":", here->version); putj(&b, here->version_name); put(&b, ",\"caps\":"); caps(&b, here->caps); put(&b, ",\"broad\":"); caps(&b, here->broad); put(&b, ","); requires(&b, here); put(&b, "}"); }
        else put(&b, "null");
        /* relist: the index's listing of the version installed here, being taken or not taken (K2): for the row */
        { const struct relist *q = NULL; static const char *const rs[] = { "", "pending", "taken", "nothing", "failed" };
          for (int k = 0; k < PS_MAX && !q; k++) if (!strcmp(relists[k].id, p->id) && (relists[k].state == R_PENDING || relists[k].state == R_FAILED)) q = &relists[k];
          if (q) { put(&b, ",\"relist\":{\"state\":\"%s\",\"version\":%ld,\"error\":", rs[q->state], q->version); putj(&b, q->err); put(&b, "}"); } }
        put(&b, ",\"top\":");
        if (top) {
            put(&b, "{\"version\":%ld,\"version_name\":", top->version); putj(&b, top->version_name);
            put(&b, ",\"unknown\":%s,\"caps\":", top->caps_unknown ? "true" : "false"); caps(&b, top->caps);
            put(&b, ","); requires(&b, top); put(&b, ",\"broad\":"); caps(&b, top->broad); put(&b, "}");
        } else put(&b, "null");
        put(&b, "}");
        first = 0;
    }
    pthread_mutex_unlock(&fl);
    put(&b, "]}");
    if (b.oom) { free(b.p); return 0; }
    *out = b.p;
    return b.n;
}

/* ---------------------------------------------------------------- one package */

/* one download of A: 0, -1 with why, -2 its size or SHA-256 not the list's */
static int download(const struct pf_asset *a, long secs, const volatile int *stop, unsigned char **data, size_t *len, char *err, size_t cap)
{
    char url[600], why[300], name[60]; uint8_t sha[32];
    *data = NULL; *len = 0;
    /* made from what was checked, and checked once more here: this is the one place a name from the network
     * becomes part of an address */
    if (!ps_id_ok(a->id) || a->version < 1 || a->version > 1000000000) { snprintf(err, cap, "not a package's name"); return -1; }
    snprintf(name, sizeof name, "%s-%ld.hmp", a->id, a->version);
    if (a->size < 1 || a->size > PS_PACKAGE_MAX) { snprintf(err, cap, "the index's %s is over %d MB, which no plugin package is", name, PS_PACKAGE_MAX >> 20); return -1; }
    snprintf(url, sizeof url, "%spackages/%s", pi_base(), name);
    fprintf(stderr, "plugins: downloading %s from the plugin index\n", name);
    /* (at most the size the index names: a larger file is no package of its) */
    if (update_fetch(url, (size_t)a->size, secs, stop, data, len, why, sizeof why)) {
        if (strstr(why, ": more than ")) { snprintf(err, cap, "the index's %s is larger than the index says: not the package it lists", name); return -2; }
        if (!strncmp(why, "HTTP ", 5)) snprintf(err, cap, "the index's %s could not be downloaded (%.8s)", name, why);
        else if (!strcmp(why, "cancelled")) snprintf(err, cap, "cancelled");
        else snprintf(err, cap, "this Echo cannot reach the plugin index (%.150s)", bare(why, url));
        return -1;
    }
    sha256(*data, *len, sha);
    if ((long long)*len != a->size || memcmp(sha, a->sha256, 32)) {
        snprintf(err, cap, "the index's %s is not the package the index lists (its size or its SHA-256 differs): not installed", name);
        free(*data); *data = NULL; *len = 0;
        return -2;
    }
    return 0;
}

/* Every publish of the index lists every package anew, under its new serial: the listing inside changes, and with
 * it each package's size and SHA-256.  The list kept here stands for up to an hour (pf_look), so an install right
 * after a publish found the new package against the old list's SHA-256 and failed, for up to that hour (a review,
 * 2026-11-21).  So a package that is not the list's makes one fresh look, and the version the fresh list names is
 * downloaded once more: as that list says it is, and held to it as strictly.  Not re-listing unchanged packages
 * instead would keep a package's listing at the index serial it first had, and an old listing under the build's
 * least index serial counts as a file's (5.5): every plugin listed before a release that raised the least would
 * need the button again */
int pf_download(struct pf_asset *a, long secs, const volatile int *stop, unsigned char **data, size_t *len, char *err, size_t cap)
{
    struct timespec t0, t1; char e2[240]; int r; long left;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if ((r = download(a, secs, stop, data, len, err, cap)) != -2) return r;
    fprintf(stderr, "plugins: %s; the plugin index looked at again, it may have been published since the list this Echo has\n", err);
    if (pf_look(NULL, PF_NOW, stop, e2, sizeof e2)) { char e1[130]; snprintf(e1, sizeof e1, "%s", err); snprintf(err, cap, "%s; and the plugin index could not be read again: %.100s", e1, e2); return -1; }
    pthread_mutex_lock(&fl);
    { const struct pi_plugin *p = find(&known.ix, a->id); const struct pi_version *v = NULL;
      for (int k = 0; p && k < p->n; k++) if (p->v[k].version == a->version) v = &p->v[k];
      if (!v || withdrawn_v(p, v)) r = -3;
      else if (v->size == a->size && !memcmp(v->sha256, a->sha256, 32)) r = -4;
      else { a->size = v->size; memcpy(a->sha256, v->sha256, 32); memcpy(a->manifest, v->manifest, 32); a->tier = p->tier; r = 0; } }
    pthread_mutex_unlock(&fl);
    if (r == -3) { snprintf(err, cap, "the plugin index no longer lists version %ld of %s: look again", a->version, a->id); return -1; }
    if (r == -4) return -1;             /* (the list says the same: the site's file is not the one it lists) */
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if ((left = secs - (long)(t1.tv_sec - t0.tv_sec)) < 1) { snprintf(err, cap, "no time was left to download it again"); return -1; }
    return download(a, left, stop, data, len, err, cap) ? -1 : 0;
}

/* ---------------------------------------------------------------- the revocation list by itself (5.6) */

/* arb's T_MSG to every member: { M_REVOKED, serial (8 bytes, big-endian), this Echo's settings port (2, big-endian),
 * ticket (8) }.  The next kind after duck.c's 0x40 and ringstop.c's 0x41, 0x42; builds before this pass it over.
 * arb.c authenticates it with the network's key but does not hide it: any host of the LAN reads that this Echo keeps
 * a revocation list (it has plugins), its serial and its settings port.  The ticket is what a member's request for the
 * list has to name (below): it makes a request seen on the way worth nothing later */
#define M_REVOKED   0x43
#define M_LEN       19
#define DAY_MS      (24LL * 3600 * 1000)
#define TELL_MS     (3600LL * 1000)     /* the serial told to the members: once an hour, and after a newer list */
/* One fetch from a place in this time, whatever serial it names: what one member's word costs this Echo.  It was one
 * per member, port and serial, and a member naming a new serial each time had a fetch (and a write) each time: 249
 * in 5 s (a review, 2026-11-21).  An honest member tells hourly, and at once after a newer list: the most that waits
 * is a newer list it had within ten minutes of the last fetch from it */
#define MEMBER_MS   (10LL * 60 * 1000)
/* And fetches from members in all: four at once, then one every 20 s (three a minute).  A place is per id, address
 * and port, all of which a member that holds the network's key makes up at will (arb's ids are its own word, signed
 * only with the shared key): the ten minutes above held per id, and one member naming a new id with each message had
 * 496 fetches and as many lines in the log in 5 s (a review of 6f39dd8).  What honest members need: a newer list
 * makes each Echo fetch once, from the member that told it first, a second time where that one failed; four at once
 * cover a house where several members told within the same minute.  At three a minute a member that mints ids has 180
 * fetches an hour of at most MEMBER_S and 512 kB each, of this Echo's time and Wi-Fi.  This bounds what one member's
 * mistake costs, and what anyone without the key can do (nothing: arb drops what it did not sign); it does not hold
 * against a member that holds the key and means it (below, SENDERS) */
#define MEMBERS_BURST 4
#define MEMBERS_EVERY (20LL * 1000)
/* The log's lines on members' handovers: five at once, then one every ten minutes; what is left out is counted and
 * said with the next line that goes out (or by the thread once one may).  Each fetch said a line (the same 496), and
 * boot.log is what a reset of the log keeps of the Echo's history.  A list taken is always said (only a newer list the
 * index key signed is taken: no member makes those) */
#define MLOG_BURST  5
#define MLOG_EVERY  (10LL * 60 * 1000)
/* A member's fetch, all of it: its list is a few kB and at most 512 kB, from an Echo of the same network, whose
 * settings port answers within milliseconds; 512 kB at the 1 to 2 MB/s an Echo's Wi-Fi does is half a second.  Ten
 * times that.  It was 15 s, as for GitHub, and one member that held every request that long had the thread to
 * itself: no other member's list came through (the same review) */
#define MEMBER_S    5
/* Members waiting to be asked, one place each, keyed by who told (the member's id in the network), from where (the
 * message's source address) and the settings port it named: as many as arb.c holds peers (NPEER).  A message under a
 * known id from another address or port is a place of its own, never the known one's: it rewrote the place's address,
 * and a member sending under an honest member's id had the honest member's newer list never asked for (the same
 * review).  A place keeps its highest serial, when it began to wait, its time, and the ticket it last named.  A
 * newcomer when all are taken goes into a place that waits for nothing, else one that waits out its ten minutes (it
 * was asked lately), else the one that began to wait last; the one that began first is asked first.  What a member
 * with the key can still do: make places at will (an id, an address and a port are its own to name), keep them full
 * and put a place of its own over an honest member's as that one arrives, so another member's newer list waits for
 * as long as it keeps at it, unbounded (a review of a6c5a00 did it for good with one message after each honest word);
 * and arb drops every message of an honest member once one under that member's id came with a counter ahead of its
 * own (arb.c fresh: arbitration's own older limit, until a restart).  The maintainer's decision: the handover is not
 * held against a member with the key, which already disturbs arbitration, Drop In and ducking at will; it only helps
 * Echos without the internet.  What holds: the list itself it cannot change (only what the index key signed is
 * taken); an Echo that reaches the site still gets it with the daily look, within a day, which does not go through
 * any member; one that does not gets it only through members, and warns after a week without word (pf_revoked_stale) */
#define SENDERS     16
/* A request for the list names one of the two tickets this Echo told last, a nonce of the asker's, and a MAC over
 * both with the key derived from the network's.  A ticket is told hourly (with the serial) and taken for two such
 * hours; the last TICKET_RING nonces taken with it are kept, the oldest dropped, and a request whose nonce is among
 * them is refused: a request seen on the way and sent again (it was answered for ever, with the list as it is now: a
 * review of 6f39dd8).  That is against hosts without the network's key only, which cannot make a request of their
 * own: a member can make as many as it likes.  It was a cap, 64 nonces a ticket and then a new ticket told at most a
 * minute later, said to be more than all members' fetches in two hours, which at 16 places, one each in ten minutes,
 * are 192; and a member that took the 64 as each ticket was told had every honest request refused, for as long as it
 * kept at it (a review of a6c5a00).  A ring refuses nobody: what the oldest nonce dropped costs is that a request
 * seen on the way is answered again once 64 others came with its ticket, which only members' requests make (an
 * honest house's are a few per newer list, one per member that lacks it), and what it is answered with is a list the
 * site hands anyone, from an Echo whose announcement already says it has one.  No clock: the Echos' wall clocks need
 * not agree, or be set at all */
#define TICKET_RING 64

static pthread_mutex_t dl = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dc = PTHREAD_COND_INITIALIZER;
/* dl.  serial: the highest it told that waits to be asked for (0 nothing waits), since: when that began, heard: its
 * last word, last: when it was last asked (0 never), ticket: the last it named */
static struct sender { uint8_t id[8], ticket[8]; unsigned ip; int port, used; long long serial, since, heard, last; char node[40]; } senders[SENDERS];
static int tell_now, my_port, started;                                                /* dl */
static long long budget_at;             /* dl: fetches from members in all (MEMBERS_BURST, MEMBERS_EVERY), as a GCRA's time */
/* tk: this Echo's tickets, [0] the newest; made 0: none */
static pthread_mutex_t tk = PTHREAD_MUTEX_INITIALIZER;
static struct ticket { uint8_t t[8]; long long made; unsigned nused; uint8_t used[TICKET_RING][8]; } tickets[2];  /* used: a ring, nused taken in all */
/* the daily thread's own: the members' log lines (MLOG_*) */
static long long mlog_at; static long mlog_left;
/* the first look 5 min after the start and within the hour after that, then a day apart give or take an hour (at a
 * random time, so that the Echos of the world do not all ask at once); HASSMIC_PLUGIN_DAILY=<first>:<period>:<tell>
 * (ms, no randomness) for the tests; HASSMIC_PLUGIN_MEMBER=<ms>[:<burst>:<every ms>] the time between two fetches
 * from one place, and the fetches from all of them */
static long long first_ms = 5 * 60 * 1000, first_spread = 3600LL * 1000, period_ms = DAY_MS, period_spread = 2 * 3600LL * 1000, tell_ms = TELL_MS, member_ms = MEMBER_MS;
static long long members_burst = MEMBERS_BURST, members_every = MEMBERS_EVERY;

static unsigned long long jitter;
static long long rnd(long long below)       /* a time at random in [0, BELOW): xorshift, seeded by the clock and the pid */
{
    jitter ^= jitter << 13; jitter ^= jitter >> 7; jitter ^= jitter << 17;
    return below > 0 ? (long long)(jitter % (unsigned long long)below) : 0;
}

static int plugins_wanted(void)             /* plugins on, and at least one installed: else nothing is asked or told */
{
    const struct ps_plugin *l; int n;
    if (!plugin_feature_on()) return 0;
    ps_lock(); n = ps_list(&l); ps_unlock();
    return n > 0;
}

/* a newer list taken, whichever way: what it names that runs here is stopped now, the members told (no lock held) */
static void took(void)
{
    plugin_withdrawn_changed();
    pthread_mutex_lock(&dl); tell_now = 1; pthread_cond_signal(&dc); pthread_mutex_unlock(&dl);
}

/* The daily thread's: relists[] and unlisted[] held to the versions installed now (S4: an entry for a plugin removed
 * or updated since, or a failure for a version the index no longer lists, is no reason to ask again).  1 where a
 * listing of a version still installed failed to be taken (R_FAILED): asked again at the next look.  SKIP (PS_MAX
 * places): unlisted[] as it is then, and every version whose listing the index has and this Echo cannot take
 * (R_NOTHING for that very version: under the least, by a key this build does not read, none in the package), for
 * ps_relist_reason (the store's lock is never taken under fl).  Without those, a listing that does not hold and
 * whose package brings none that does stayed a reason 2: index.json read hourly and at every return of the
 * network for the whole run, for a package relist_due never asks for again (a review of 9cd7a77, T3).  One id has
 * one version installed and one entry in each: SKIP holds both lists */
static int relist_upkeep(struct ps_idv *skip, int *nskip)
{
    const struct ps_plugin *l; int n, k = 0, failed = 0, w = 0;
    struct ps_idv *mine = calloc(PS_MAX, sizeof *mine);
    *nskip = 0;
    if (!mine) return 0;
    ps_lock(); n = ps_list(&l);
    for (int i = 0; i < n && k < PS_MAX; i++) if (l[i].here && !l[i].unread) { snprintf(mine[k].id, sizeof mine[k].id, "%s", l[i].m.id); mine[k++].version = l[i].m.version; }
    ps_unlock();
    pthread_mutex_lock(&fl);
    for (int u = 0; u < nunlisted; u++) {
        int in = 0;
        for (int i = 0; i < k && !in; i++) in = mine[i].version == unlisted[u].version && !strcmp(mine[i].id, unlisted[u].id);
        if (in) unlisted[w++] = unlisted[u];
    }
    nunlisted = w;
    for (int r = 0; r < PS_MAX; r++) {
        struct relist *q = &relists[r]; int in = 0, gone = 0;
        if (!q->id[0] || q->state == R_PENDING) continue;
        for (int i = 0; i < k && !in; i++) in = mine[i].version == q->version && !strcmp(mine[i].id, q->id);
        for (int u = 0; u < nunlisted && !gone; u++) gone = unlisted[u].version == q->version && !strcmp(unlisted[u].id, q->id);
        if (!in || gone) memset(q, 0, sizeof *q);
        else if (q->state == R_FAILED) failed = 1;
    }
    memcpy(skip, unlisted, sizeof *skip * (size_t)nunlisted); *nskip = nunlisted;
    for (int r = 0; r < PS_MAX && *nskip < PS_MAX; r++)
        if (relists[r].id[0] && relists[r].state == R_NOTHING) { snprintf(skip[*nskip].id, sizeof skip[0].id, "%s", relists[r].id); skip[(*nskip)++].version = relists[r].version; }
    pthread_mutex_unlock(&fl);
    free(mine);
    return failed;
}

/* The daily look (and the first, 5 to 65 minutes after the start).  The revocation list by itself, unless a listing
 * installed here may be one the index lists anew (ps_relist_reason: 5.2's change of the index key; a listing under
 * the build's least) or one failed to be taken: then the whole look, revocation list first, and the index's listings
 * taken after it as from the page.  Only from the page, an owner who did not open the Plugins page between the
 * index's switch and the build of its new key alone had his official plugins as files' under that build, the wake
 * word engine's too, until he did (a review of 3c5d7ff, R2).  A step-(1) build reads index.json once a day while a
 * plugin is listed by the first key: one more file a day for the time of a key's change.  A listing that does not
 * hold here is relist_tick's to look for, sooner */
static void daily_fetch(void)
{
    unsigned char *rj = NULL, *rs = NULL; size_t rjn = 0, rsn = 0; char why[240] = ""; int state = PF_OK, r = PI_REFUSED, failed, reason, nskip;
    struct ps_idv *skip = calloc(PS_MAX, sizeof *skip);
    if (!skip) return;
    failed = relist_upkeep(skip, &nskip); reason = ps_relist_reason(skip, nskip); free(skip);
    /* a listing that does not hold is a reason here whatever the last look found (T4 of a review of 9cd7a77): SKIP
     * lasts the run, and only the page's look made it anew, so an index that lists the version again, or a
     * revocation lifted, was never seen until a restart, the wake word engine's plugin stopped meanwhile.  Once a
     * day while such a version is installed: what relist_tick spares the hour, the daily look still asks */
    if (!reason && ps_relist_reason(NULL, 0) == 2) reason = 2;
    if (failed || reason) {
        fprintf(stderr, "plugins: the daily look reads the plugin index too: %s\n", failed ? "a listing of it was not taken at the last look" : "a listing installed here may be one it lists anew");
        if (!pf_look(NULL, PF_DAILY, &pf_stop, why, sizeof why)) relist_due(0);
        return;
    }
    if (!get("revoked.json", PI_REVOKED_MAX, &pf_stop, 0, &rj, &rjn, &state, why, sizeof why) && !get("revoked.json.sig", SIG_MAX, &pf_stop, 0, &rs, &rsn, &state, why, sizeof why))
        r = pi_revoked_take((const char *)rj, rjn, rs, rsn, PI_SITE, why, sizeof why);
    free(rj); free(rs);
    if (r < 0) fprintf(stderr, "plugins: the daily look at the revocation list: %s%s\n", state == PF_UNREACHABLE ? "not reachable: " : "", why);
    else fprintf(stderr, "plugins: the daily look at the revocation list: serial %lld%s\n", pi_revoked_serial(), r == PI_TAKEN ? ", new" : ", as before");
    if (r == PI_TAKEN) took();
}

/* A listing installed that does not hold under this build (ps_relist_reason 2: 5.2's step (3) on an Echo that did not
 * look at the index while its build read both keys, or a step back to a build of the old key): its plugin is a
 * file's meanwhile, with community plugins off (the default) stopped, the wake word engine's too.  The daily look's
 * first came 5 to 65 minutes after the start, and one that failed (the network not up yet after a boot) waited a
 * day: the wake word down for as long (a review of 3fcd319, S2).  So this, every RELIST_CHECK_MS on the daily
 * thread: whether such a listing is there (the store, nothing asked of anyone) and whether the Echo has a network
 * (net_online); the index read RELIST_FIRST_MS after the start, again RELIST_EVERY_MS after each look while the
 * reason stands (a look that took the listing ends it; a version the index does not list is no reason, S4, nor one
 * whose listing there this Echo cannot take, T3: the daily look asks for both, T4), and at once where the network
 * came back, a look RELIST_GAP_MS apart at the least (back sooner: when the gap is over); with no network, nothing.  What it
 * costs: an Echo cut off from the internet with such a listing asks once an hour, and once per return of its
 * network.  RELIST_* (ms): HASSMIC_PLUGIN_RELIST=<first>:<every>[:<check>:<gap>] for the tests */
#define RELIST_FIRST_MS (60LL * 1000)
#define RELIST_EVERY_MS (3600LL * 1000)
#define RELIST_CHECK_MS (60LL * 1000)
#define RELIST_GAP_MS   (5LL * 60 * 1000)
static long long relist_first = RELIST_FIRST_MS, relist_every = RELIST_EVERY_MS, relist_check = RELIST_CHECK_MS, relist_gap = RELIST_GAP_MS;
static long long relist_next, relist_last = -1;      /* the daily thread's: the next look due (0 at once), the last one's time (-1 none) */
static int relist_up = 1;                            /* the daily thread's: the network at the last look at it */
static void relist_tick(void)
{
    char why[240]; int nskip, up, came, reason; long long now = mono_ms();
    struct ps_idv *skip = calloc(PS_MAX, sizeof *skip);
    if (!skip) return;
    relist_upkeep(skip, &nskip); reason = ps_relist_reason(skip, nskip); free(skip);
    up = net_online(); came = up && !relist_up; relist_up = up;
    if (reason < 2 || !up) return;
    /* the network back within RELIST_GAP_MS of the last look: the look when the gap is over, not an hour on (T2 of a
     * review of 9cd7a77: the return was forgotten, relist_up already 1 at the next check) */
    if (came && relist_last >= 0 && now - relist_last < relist_gap && relist_last + relist_gap < relist_next) relist_next = relist_last + relist_gap;
    if (now < relist_next && !(came && relist_last >= 0 && now - relist_last >= relist_gap)) return;
    relist_last = now; relist_next = now + relist_every;
    fprintf(stderr, "plugins: a listing installed here does not hold under this hassmic: the plugin index read for it%s\n", came ? " (the network is back)" : "");
    if (!pf_look(NULL, PF_DAILY, &pf_stop, why, sizeof why)) relist_due(0);
    else fprintf(stderr, "plugins: read again in %lld min, or when the network comes back: %s\n", relist_every / 60000, why);
}

/* the daily thread's: a line on a member's handover, within MLOG_* unless ALWAYS; what was left out said with it */
static void mlog(int always, const char *fmt, ...)
{
    long long now = mono_ms(); va_list ap; char t[400];
    if (!always && mlog_at > now + (MLOG_BURST - 1) * MLOG_EVERY) { mlog_left++; return; }
    if (mlog_at < now) mlog_at = now;
    if (!always) mlog_at += MLOG_EVERY;
    va_start(ap, fmt); vsnprintf(t, sizeof t, fmt, ap); va_end(ap);
    if (mlog_left) fprintf(stderr, "plugins: %s (and %ld such lines on members' lists left out before it: more than the log takes)\n", t, mlog_left);
    else fprintf(stderr, "plugins: %s\n", t);
    mlog_left = 0;
}
static void mlog_upkeep(void)               /* the daily thread's: lines left out said once one may go out */
{
    long long now = mono_ms();
    if (!mlog_left || mlog_at > now + (MLOG_BURST - 1) * MLOG_EVERY) return;
    if (mlog_at < now) mlog_at = now;
    mlog_at += MLOG_EVERY;
    fprintf(stderr, "plugins: %ld lines on members' revocation lists left out: more than the log takes\n", mlog_left);
    mlog_left = 0;
}

/* The handover is for members only (a review, 2026-11-21: unsigned and open, it told any host of the LAN whether an
 * Echo has plugins; the announcement says as much to whoever reads arb's port, M_REVOKED above).  The asker proves
 * the network's key: X-HM-Ticket one of the tickets this Echo told (16 hex digits), X-HM-Ctr a nonce of its own (16
 * hex digits), X-HM-Mac BLAKE2b-128 keyed with a key derived from the network's (arb_derive) over the request, the
 * ticket and the nonce.  A nonce is refused while it is among the last TICKET_RING taken with its ticket, the ticket
 * taken for two hours */
static int member_mac(const char *ticket, const char *nonce, char hex[33])
{
    uint8_t k[32], mac[16]; char msg[100]; int n;
    if (strlen(nonce) != 16 || strlen(ticket) != 16) return -1;
    for (const char *c = nonce; *c; c++) if (!isxdigit((unsigned char)*c)) return -1;
    for (const char *c = ticket; *c; c++) if (!isxdigit((unsigned char)*c)) return -1;
    if (arb_derive("hassmic plugin revoked 2", k)) return -1;
    n = snprintf(msg, sizeof msg, "GET /api/plugins/revoked\n%s\n%s", ticket, nonce);
    crypto_blake2b_keyed(mac, sizeof mac, k, sizeof k, (const uint8_t *)msg, (size_t)n);
    for (int i = 0; i < 16; i++) snprintf(hex + 2 * i, 3, "%02x", mac[i]);
    crypto_wipe(k, sizeof k);
    return 0;
}

static int hex8(const char *h, uint8_t out[8])
{
    for (int i = 0; i < 8; i++) { unsigned v; if (!isxdigit((unsigned char)h[2 * i]) || !isxdigit((unsigned char)h[2 * i + 1]) || sscanf(h + 2 * i, "%2x", &v) != 1) return -1; out[i] = (uint8_t)v; }
    return 0;
}

/* a new ticket for the next M_REVOKED, the one before it kept beside (tk) */
static void ticket_new(uint8_t out[8])
{
    uint8_t t[8]; FILE *u = fopen("/dev/urandom", "rb");
    if (!u || fread(t, 1, 8, u) != 8) for (int i = 0; i < 8; i++) t[i] = (uint8_t)rnd(256);
    if (u) fclose(u);
    pthread_mutex_lock(&tk);
    tickets[1] = tickets[0];
    memset(&tickets[0], 0, sizeof tickets[0]); memcpy(tickets[0].t, t, 8); tickets[0].made = mono_ms();
    pthread_mutex_unlock(&tk);
    memcpy(out, t, 8);
}

size_t pf_revoked_serve(const char *ticket, const char *nonce, const char *mac, char **out)
{
    char want[33], got[33]; uint8_t t[8], nc[8]; int ok = 0; long long now = mono_ms();
    *out = NULL;
    if (!ticket || !nonce || !mac || strlen(mac) != 32 || member_mac(ticket, nonce, want) || hex8(ticket, t) || hex8(nonce, nc)) return 0;
    for (int i = 0; i < 33; i++) got[i] = (char)tolower((unsigned char)mac[i]);
    if (crypto_verify16((const uint8_t *)got, (const uint8_t *)want) | crypto_verify16((const uint8_t *)got + 16, (const uint8_t *)want + 16)) return 0;
    pthread_mutex_lock(&tk);
    for (int i = 0; i < 2 && !ok; i++) {
        struct ticket *x = &tickets[i]; int seen = 0; unsigned held = x->nused < TICKET_RING ? x->nused : TICKET_RING;
        if (!x->made || now - x->made > 2 * tell_ms || memcmp(x->t, t, 8)) continue;
        for (unsigned u = 0; u < held && !seen; u++) seen = !memcmp(x->used[u], nc, 8);
        if (seen) break;                /* (a request again: refused) */
        memcpy(x->used[x->nused++ % TICKET_RING], nc, 8); ok = 1;      /* (the oldest dropped) */
    }
    pthread_mutex_unlock(&tk);
    return ok ? pi_revoked_file(out) : 0;
}

/* The list a member has, from its settings port: GET /api/plugins/revoked, the signature's hex line and the list.
 * The member is only who said it has one; what is taken is what the index key signed, under the same rules as the
 * site's.  1 taken */
static int member_fetch(unsigned ip, int port, long long serial, const char *node, const uint8_t ticket[8])
{
    char url[80], a[INET_ADDRSTRLEN], why[240], nonce[17], tick[17], mac[33], h1[40], h2[48], h3[40]; const char *hdrs[4] = { h1, h2, h3, NULL };
    unsigned char *d = NULL; size_t n = 0; uint8_t sig[64], rb[8]; int r = PI_REFUSED; struct in_addr ia = { ip }; FILE *u;
    if (!inet_ntop(AF_INET, &ia, a, sizeof a) || port < 1 || port > 65535) return 0;
    if (!(u = fopen("/dev/urandom", "rb")) || fread(rb, 1, 8, u) != 8) { if (u) fclose(u); return 0; }
    fclose(u);
    for (int i = 0; i < 8; i++) { snprintf(nonce + 2 * i, 3, "%02x", rb[i]); snprintf(tick + 2 * i, 3, "%02x", ticket[i]); }
    if (member_mac(tick, nonce, mac)) return 0;               /* (out of the network meanwhile) */
    snprintf(h1, sizeof h1, "X-HM-Ctr: %s", nonce); snprintf(h2, sizeof h2, "X-HM-Mac: %s", mac); snprintf(h3, sizeof h3, "X-HM-Ticket: %s", tick);
    snprintf(url, sizeof url, "http://%s:%d/api/plugins/revoked", a, port);
    if (update_fetch_lan(url, hdrs, PI_REVOKED_MAX + 129, MEMBER_S, &d, &n, why, sizeof why)) { mlog(0, "the revocation list of %s (serial %lld) not had: %s", node, serial, bare(why, url)); return 0; }
    if (n > 129 && d[128] == '\n') {
        int ok = 1;
        for (int i = 0; i < 64 && ok; i++) { unsigned v; ok = sscanf((const char *)d + 2 * i, "%2x", &v) == 1 && isxdigit(d[2 * i]) && isxdigit(d[2 * i + 1]); sig[i] = (uint8_t)v; }
        if (ok) r = pi_revoked_take((const char *)d + 129, n - 129, sig, 64, PI_MEMBER, why, sizeof why);
        else snprintf(why, sizeof why, "not a signature and a list");
    } else snprintf(why, sizeof why, "not a signature and a list");
    free(d);
    if (r < 0) mlog(0, "the revocation list %s handed over (serial %lld) not taken: %s", node, serial, why);
    else if (r == PI_TAKEN) mlog(1, "the revocation list taken from %s: serial %lld", node, pi_revoked_serial());
    else mlog(0, "%s handed over the revocation list this Echo has (serial %lld), not the %lld it told", node, pi_revoked_serial(), serial);
    return r == PI_TAKEN;
}

static void tell(void)
{
    long long s = pi_revoked_serial(); uint8_t m[M_LEN];
    if (s < 0) return;
    m[0] = M_REVOKED;
    for (int i = 0; i < 8; i++) m[1 + i] = (uint8_t)(s >> (56 - 8 * i));
    m[9] = (uint8_t)(my_port >> 8); m[10] = (uint8_t)my_port;
    ticket_new(m + 11);
    arb_tell(NULL, m, sizeof m);
}

/* dl held: the member to ask now (its place's index), -1 none; *WHEN the soonest one waits for, else unchanged.
 * Nothing while the fetches in all are spent (MEMBERS_*): *WHEN then when the next may be */
static int sender_ready(long long now, long long *when)
{
    int k = -1; long long free_at = budget_at - (members_burst - 1) * members_every;
    for (int i = 0; i < SENDERS; i++) {
        const struct sender *x = &senders[i]; long long at;
        if (!x->used || !x->serial) continue;
        at = x->last ? x->last + member_ms : 0;
        if (at > now) { if (at < *when) *when = at; continue; }
        if (k < 0 || x->since < senders[k].since) k = i;        /* (the one that began to wait first) */
    }
    if (k >= 0 && free_at > now) { if (free_at < *when) *when = free_at; return -1; }
    return k;
}

static void *daily_thread(void *arg)
{
    long long next_fetch = mono_ms() + first_ms + rnd(first_spread), next_tell = mono_ms() + tell_ms / 60, next_check = mono_ms() + relist_first;
    (void)arg;
    for (;;) {
        unsigned ip = 0; int port = 0, now_tell, k; long long serial = 0; char node[40] = ""; uint8_t id[8] = { 0 }, ticket[8] = { 0 };
        pthread_mutex_lock(&dl);
        for (;;) {
            long long now = mono_ms(), until = next_fetch < next_tell ? next_fetch : next_tell; struct timespec ts;
            if (next_check < until) until = next_check;
            if (pf_stop) { pthread_mutex_unlock(&dl); return NULL; }
            if ((k = sender_ready(now, &until)) >= 0 || tell_now || now >= until) break;
            clock_gettime(CLOCK_REALTIME, &ts);
            { long long w = until - now; if (w > 60000) w = 60000;          /* (a minute at most: the realtime clock may be set meanwhile) */
              ts.tv_sec += (time_t)(w / 1000); ts.tv_nsec += (long)(w % 1000) * 1000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; } }
            pthread_cond_timedwait(&dc, &dl, &ts);
            pthread_mutex_unlock(&dl); pi_revoked_upkeep(); mlog_upkeep(); pthread_mutex_lock(&dl);     /* (the floor line, .revoked-at, the log: when owed) */
        }
        if (k >= 0) { ip = senders[k].ip; port = senders[k].port; serial = senders[k].serial; memcpy(id, senders[k].id, 8); memcpy(ticket, senders[k].ticket, 8); snprintf(node, sizeof node, "%s", senders[k].node); senders[k].serial = 0; }
        now_tell = tell_now; tell_now = 0;
        pthread_mutex_unlock(&dl);
        pi_revoked_upkeep();
        if (pf_stop) return NULL;
        if (!plugins_wanted()) {            /* plugins off or none installed: nothing asked, nothing told */
            long long now = mono_ms();
            if (now >= next_fetch) next_fetch = now + period_ms;
            if (now >= next_tell) next_tell = now + tell_ms;
            if (now >= next_check) next_check = now + relist_check;
            continue;
        }
        if (mono_ms() >= next_check) { relist_tick(); next_check = mono_ms() + relist_check; }
        if (k >= 0 && serial > pi_revoked_serial()) {
            pthread_mutex_lock(&dl);
            { long long now = mono_ms(); if (budget_at < now) budget_at = now; budget_at += members_every; }
            if (senders[k].used && !memcmp(senders[k].id, id, 8) && senders[k].ip == ip && senders[k].port == port) senders[k].last = mono_ms();
            pthread_mutex_unlock(&dl);
            if (member_fetch(ip, port, serial, node, ticket)) took();
        }
        if (mono_ms() >= next_fetch) { daily_fetch(); next_fetch = mono_ms() + period_ms - period_spread / 2 + rnd(period_spread); }
        if (now_tell || mono_ms() >= next_tell) { tell(); next_tell = mono_ms() + tell_ms; }
    }
    return NULL;
}

void pf_start(int web_port)
{
    pthread_t t; const char *e = getenv("HASSMIC_PLUGIN_DAILY"), *m = getenv("HASSMIC_PLUGIN_MEMBER"), *rl = getenv("HASSMIC_PLUGIN_RELIST"); long long a, b, c, d;
    if (e && sscanf(e, "%lld:%lld:%lld", &a, &b, &c) == 3 && a >= 0 && b > 0 && c > 0) { first_ms = a; period_ms = b; tell_ms = c; first_spread = period_spread = 0; }
    if (rl && sscanf(rl, "%lld:%lld", &a, &b) == 2 && a >= 0 && b > 0) {
        relist_first = a; relist_every = b;
        if (sscanf(rl, "%lld:%lld:%lld:%lld", &a, &b, &c, &d) == 4 && c > 0 && d >= 0) { relist_check = c; relist_gap = d; }
    }
    if (m && sscanf(m, "%lld:%lld:%lld", &a, &b, &c) >= 1 && a > 0) { member_ms = a; if (sscanf(m, "%lld:%lld:%lld", &a, &b, &c) == 3 && b > 0 && c > 0) { members_burst = b; members_every = c; } }
    jitter = ((unsigned long long)mono_ms() * 2654435761u ^ (unsigned long long)getpid() << 32) | 1;
    pthread_mutex_lock(&dl); my_port = web_port; if (started) { pthread_mutex_unlock(&dl); return; } started = 1; pthread_mutex_unlock(&dl);
    if (!pthread_create(&t, NULL, daily_thread, NULL)) pthread_detach(t);
}

void pf_shutdown(void)
{
    pf_stop = 1;
    pthread_mutex_lock(&dl); pthread_cond_signal(&dc); pthread_mutex_unlock(&dl);
}

void pf_message(const unsigned char from[8], const char *node, unsigned ip, const unsigned char *p, size_t n)
{
    long long s = 0, now = mono_ms(); int port, k = -1;
    if (n != M_LEN || p[0] != M_REVOKED) return;
    /* a member is on this network: its word comes from a private or link-local address (loopback: this host itself,
     * the PC's tests' members), never from the internet.  The address is the datagram's source and the port the
     * message's, so a word from anywhere else would have this Echo fetch from an address of the sender's choosing
     * (a review of 6631974, 6; member_fetch's socket cannot leave the network either) */
    if (!pnet_local(ip) && (ntohl(ip) >> 24) != 127) return;
    for (int i = 0; i < 8; i++) s = s << 8 | p[1 + i];
    port = p[9] << 8 | p[10];
    if (s <= 0 || s <= pi_revoked_serial() || port < 1) return;
    pthread_mutex_lock(&dl);
    for (int i = 0; i < SENDERS && k < 0; i++) if (senders[i].used && !memcmp(senders[i].id, from, 8) && senders[i].ip == ip && senders[i].port == port) k = i;
    if (k < 0) {
        /* a newcomer: a free place, else one that waits for nothing (the one asked longest ago), else one that waits
         * out its ten minutes (the one asked last: it had its turn), else the one that began to wait last (it was the
         * one heard last: an honest member's hourly word made its place the next to go) */
        for (int i = 0; i < SENDERS && k < 0; i++) if (!senders[i].used) k = i;
        if (k < 0) for (int i = 0; i < SENDERS; i++)
            if (!senders[i].serial && (k < 0 || senders[i].last < senders[k].last)) k = i;
        if (k < 0) for (int i = 0; i < SENDERS; i++)
            if (senders[i].last && now - senders[i].last < member_ms && (k < 0 || senders[i].last > senders[k].last)) k = i;
        if (k < 0) for (int i = 0; i < SENDERS; i++)
            if (k < 0 || senders[i].since > senders[k].since) k = i;
        memset(&senders[k], 0, sizeof senders[k]); senders[k].used = 1; memcpy(senders[k].id, from, 8); senders[k].ip = ip; senders[k].port = port;
    }
    senders[k].heard = now; memcpy(senders[k].ticket, p + 11, 8);
    if (!senders[k].serial) senders[k].since = now;
    if (s > senders[k].serial) senders[k].serial = s;
    snprintf(senders[k].node, sizeof senders[k].node, "%s", node && node[0] ? node : "another Echo");
    pthread_cond_signal(&dc);
    pthread_mutex_unlock(&dl);
}

/* 5.6's warning: plugins run here (on, one switched on) and the newest revocation list this Echo has is older than a
 * week, or it never had one, or it has one older than this build wants (its least: an update raised it, and nothing
 * newer came since).  "Older" by when it last had word that its list is the newest (pi_revoked_age: the site's
 * answer, or a newer list from anywhere): the list's own serial is the time of its last change, which says nothing
 * about whether a newer one exists.  No word known at all counts as old (it counted as fresh: a restart while the
 * clock was not set, a Wyoming Echo after any restart, silenced it; a review, 2026-11-21).  1 and the words for the
 * page; 0 */
int pf_revoked_stale(char *why, size_t cap)
{
    const struct ps_plugin *l; int n, on = 0; long long age, serial;
    if (!plugin_feature_on()) return 0;
    ps_lock(); n = ps_list(&l); for (int i = 0; i < n; i++) on |= l[i].enabled; ps_unlock();
    if (!on) return 0;
    serial = pi_revoked_serial(); age = pi_revoked_age();
    if (serial >= 0 && pi_revoked_current() && age >= 0 && age <= PI_STALE_S) return 0;
    if (serial < 0 && pi_revoked_unread()) snprintf(why, cap, "Plugins: the list of withdrawn plugins this Echo kept is signed by a plugin index key this hassmic does not read (a hassmic of other index keys kept it): what it withdrew would run on here. A list this hassmic reads comes with a look at the index (Plugins page), once a day by itself while the Echo reaches the internet, or from another Echo of its network that has it");
    else if (serial < 0) snprintf(why, cap, "Plugins: this Echo has never had the plugin index's list of withdrawn plugins. Plugins it names would run on here. It comes with a look at the index (Plugins page), once a day by itself while the Echo reaches the internet, or from another Echo of its network that has it");
    else if (!pi_revoked_current()) snprintf(why, cap, "Plugins: this Echo's list of withdrawn plugins is older than this hassmic wants: what it withdraws stays withdrawn, but a plugin withdrawn since would run on here. It comes with a look at the index (Plugins page), once a day by itself while the Echo reaches the internet, or from another Echo of its network that has it");
    else if (age < 0) snprintf(why, cap, "Plugins: this Echo does not know when it last had word on the plugin index's list of withdrawn plugins. A plugin withdrawn since would run on here. It comes with a look at the index (Plugins page), once a day by itself while the Echo reaches the internet, or from another Echo of its network that has a newer one");
    else snprintf(why, cap, "Plugins: this Echo last had word on the plugin index's list of withdrawn plugins %lld days ago. A plugin withdrawn since would run on here. It comes with a look at the index (Plugins page), once a day by itself while the Echo reaches the internet, or from another Echo of its network that has a newer one", age / 86400);
    return 1;
}
