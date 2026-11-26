/*
 * The settings page, served by hassmic on its own port.  Plain HTTP: the Echo has no certificate a browser would take,
 * and a self-signed one means a warning on every Echo.  So nothing secret is ever sent (no API key, no Sendspin token,
 * no network keys), and what changes something is signed:
 *
 *   Login: the page makes an X25519 key pair once (kept in the browser) and asks POST /api/login with its public key.
 *   The ring shows that a login waits (hooks->attention); a short press of the action button approves it (main.c
 *   on_action asks web_approve() first); unanswered for LOGIN_MS, it runs out, asked about or not.  One thing waits
 *   for the button at a time, and a press approves only the browser that armed it; when a wait has ended, no other
 *   browser that is not approved is armed until whoever that wait sent to the Echo has pressed in vain ("what waits
 *   for the action button" below has the rule, its argument, and what it does not cover: a login nobody asked for
 *   shows the same ring as the owner's own): a second browser asking while a login waits makes that login contested,
 *   the press then approves nobody and both are told.  Approved keys are kept in state/web_clients.
 *   Requests: X-HM-Pub (the browser's key, hex), X-HM-Ctr (a number new for this browser, see signed_ok) and X-HM-Mac:
 *   keyed BLAKE2b-128 of "METHOD\nPATH\nCTR\nBODY" with K = BLAKE2b-256 keyed with X25519(echo key, browser key) over
 *   "hassmic web 1" + echo key + browser key.  A sniffer has neither secret key; a recorded request does not count
 *   twice (the counter is written to disk for every request that changes something).
 *   Answers: what answers a signed request carries X-HM-Mac as well, keyed BLAKE2b-128 of "RESP\nCTR\nBODY" with the
 *   same K, so the page knows an export or an artifact it carries from one Echo to another is what that Echo sent.
 *   Through another Echo: Echos in one arbitration network trust each other (they share its key K), so a browser
 *   approved on one is let in on the others without their buttons.  The target hands out a nonce for the browser's key
 *   (POST /api/vouch/nonce); the Echo the browser is approved on, asked with a signed request, MACs "hassmic voucher 1",
 *   the target's web key, the browser's key, the nonce and its own name with a key every member derives from K
 *   (arb_web_key); the target checks that with its own copy, uses the nonce up, and approves the browser.  Outside the
 *   network it is the button again.  adb still takes the button of that very Echo.
 *   A plugin through another Echo (docs/plugins.md 5.4): the same trust carries what the owner accepted with the
 *   button on one Echo to the others, for a package the plugin index listed as official and for nothing else; "sync"
 *   below, at the plugins' install slot, has the rules and why a voucher alone is never enough.
 *   adb over Wi-Fi (a root shell for the network) takes a press of its own: POST /api/adb "on" waits for the button
 *   as a login does, ahead of any login not yet approved; the asking browser's own "off", or its login revoked,
 *   withdraws it before the press.
 *   Artifacts (artifacts.c): listed, read and written in pieces over signed requests, so a page logged in to two Echos
 *   copies models from one to the other; root installs them.
 *   microWakeWord's models (mww_store.c): listed, added (uploaded whole, checked by loading them), renamed, tuned,
 *   deleted and picked over signed requests; they are hassmic's own, nothing waits for root.
 *   Amazon itself (davs.c): POST /api/davs/login starts a code pair login (the code on the page, GET /api/davs tells
 *   where it stands), /api/davs/fetch downloads an artifact; installing is the artifacts install.  The tokens of the
 *   registration never reach the page — plain HTTP.
 *   Wi-Fi (wifi.c): GET /api/wifi, POST /api/wifi/scan, /api/wifi/join and /api/wifi/address, signed; root scans,
 *   switches and sets the address.  The one
 *   secret that comes from the page, the network's password, travels sealed (unseal below), and only the PSK made from
 *   it goes on to root.
 *   The timer's own sound (sounds.c): POST /api/timer-sound, a WAV the page made of the file picked, signed; the
 *   Echo checks the format only, it decodes nothing else.
 *   The log: GET /api/log/0 (boot.log) and /1 (its rotated part), signed, for the page's viewer.  What it says is
 *   plain on the network then, like the rest of the page: names, addresses, when the Echo was spoken to.  Secrets do
 *   not go out: the one hassmic logs, the Sendspin pairing token (scripts/lib/setup.sh reads it there over adb), is
 *   blanked on the way.
 *   Limits: someone who can change traffic (not only read it) can change the page itself, as with any plain HTTP
 *   page; and the Echo's public key comes from GET /api/hello unsigned.
 *
 * The page may come from another Echo: answers allow any origin (CORS), as the signature, not a cookie, is what counts.
 */
#include "web.h"
#include "core.h"
#include "clock.h"
#include "adbwifi.h"
#include "arb.h"
#include "dropin.h"
#include "artifacts.h"
#include "board.h"
#include "davs.h"
#include "diag.h"
#include "sound.h"
#include "hash.h"
#include "improv.h"
#include "mww.h"
#include "plugin.h"
#include "plugmsg.h"
#include "plugin_index.h"
#include "plugin_fetch.h"
#include "plugin_store.h"
#include "logread.h"
#include "ringdesign.h"
#include "ringlive.h"
#include "netio.h"
#include "settings.h"
#ifndef INSTALL_METHOD
#define INSTALL_METHOD ""                  /* (the Makefile's, from device.conf INSTALL) */
#endif
#include "sounds.h"
#include "soundpack.h"
#include "update.h"
#include "sources.h"
#include "wifi.h"
#include "ws.h"
#include "statefile.h"
#include "../third_party/monocypher.h"
#include <ctype.h>
#include "mass.h"
#include "screen.h"
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

struct web_asset { const char *path, *type; const unsigned char *data; unsigned len; };
extern const struct web_asset web_assets[];            /* build/web_assets.c (tools/embed.py): gzip'd files of web/ */
extern const int web_nassets;

#define MAX_CONN    8
#define MAX_CLIENTS 32               /* a browser has a key per Echo page it opened, and each Echo may know all of them */
#define MAX_BODY    16384
/* HASSMIC_LOGIN_MS=<wait>[:<quiet>] for the tests (one number: the quiet time half of it) */
static int LOGIN_MS = 60000;        /* a wait for the button lasts this long: a person reads the page, walks to the Echo, presses */
static int QUIET_MS = 30000;        /* after any wait: no login of another browser that is not approved is armed for this long (login section) */
#define REFUSED_MS 3000             /* a refused login is told for this long: the page asks once a second while it is told "waiting", every two while "contested" or "busy" (web/app.js) */
/* Unsigned asks to log in: this many in ASK_WINDOW_MS from one address, and from all together (HASSMIC_LOGIN_RATE=
 * <address>:<all> for the tests).  A page asks once a second while its login waits (those are not counted: login_ask),
 * every two seconds while it is held back; a person's "Try again" a few times more */
#define ASK_WINDOW_MS 10000
static int ASK_ADDR = 40, ASK_ALL = 120;

static const struct web_hooks *hooks;
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;  /* everything below; never held while taking core_lock; state/web_wait is not written under it (hold_flush) */
static uint8_t sk[32], pk[32];
/* Counters a browser may have in flight at once.  The page sends its first calls together (state, export, artifacts,
 * models, Amazon) and they arrive in any order: taking only a counter above the last, the Echo refused the ones that
 * came late, and a card said "cannot list its models" until its next round (forum #172).  ctr: the highest taken (the
 * one on disk); floor: nothing at or below it counts (the persisted ctr after a start, raised to each counter that
 * leaves seen[]); seen: the last CTR_WINDOW taken, so none counts twice */
#define CTR_WINDOW 64
/* button: let in with THIS Echo's action button (web_approve), the one fact about a browser that is stored beside
 * web_clients (state/web_button: the keys, hex, a line each; web_clients' own lines stay as every build reads them,
 * one fallen back to included).  What it decides: who may take a "copied here without the button" note off by a
 * request alone (sync, below).  The positive fact, so that every way of not knowing it fails closed: a key that is
 * not in that file, because a build before it let the browser in, because the file is gone or cannot be read, because
 * the write failed when the button was pressed, or because the browser came in on another Echo's voucher
 * (vouch_api), counts as let in on a voucher.  It was the other way round (state/web_vouched listed the vouched
 * ones), and each of those cases made a member's forged login a browser of the button's.  The cost: after the
 * update every browser counts as vouched until it is let in here with the button once (log out, log in) */
static struct client { uint8_t pub[32]; unsigned long long ctr, floor, seen[CTR_WINDOW]; int nseen, button; char label[64]; } clients[MAX_CLIENTS];
static int nclients;
/* What waits for the button (the login section below has the rule): a login, or for an approved browser adb, a
 * plugin's install or a note's dismissal.  state: 0 none, 1 armed (the press is its), 2 approved, 3 ended without
 * (told to its browser for REFUSED_MS, with `why`), 4 a login contested (not armed: the press is nobody's; held
 * until `quiet`).  at: when it was armed; end: when it ended; last: who asked last while contested; reserved: an
 * approved browser was told "busy" for this login */
enum { W_LOGIN = 1, W_ADB, W_PLUGIN,         /* W_PLUGIN: an install waits for the owner to accept what the plugin may do */
       W_NOTE,                               /* the owner is to say at the Echo, by holding the button a second, that he read what was copied here without a press (sync) */
       W_ANYWAY,                             /* a plugin the index withdrew is to run anyway (the press, below) */
       W_SOURCE };                           /* a key of the owner's or no signature check for updates or plugins (Sources, below) */
static int plugin_install_now(const char *how, unsigned gen, const uint8_t digest[32], int press, int stay, char *why, size_t cap);
static void plugin_withdraw(const char *why);
static int plugin_accepted(unsigned *gen, uint8_t digest[32], int *press);
static void wait_over(long long now, int pressed, const uint8_t *who);
static size_t syn_json(char *o, size_t cap, const uint8_t me[32]);  /* plugins that came from another Echo without this one's button (sync) */
static void note_withdraw(const char *why);
static void anyway_withdraw(const char *why);
/* lk: what the armed press (login, W_ANYWAY) lets run: that plugin, for that revocation list */
static struct { char id[PS_ID_MAX]; long long serial; } anyway_press;
/* lk: what the armed press (login, W_SOURCE) puts in force (sources.c), over what was in force when it was armed */
static struct sources source_press, source_from;
static void source_withdraw(const char *why);
#define SYN_MAX 16              /* notes of copied plugins that stand at a time (sync, below) */
struct syn_ids { unsigned long n[SYN_MAX]; int k; };
static struct syn_ids syn_press;    /* lk: the notes the armed hold (login, W_NOTE) takes off */
static int syn_dismiss(const struct syn_ids *ids, const char *how);
static struct { uint8_t pub[32], last[32]; char label[64]; const char *why; long long at, end, quiet; int state, kind, reserved; } login;
/* lk: no login of a browser not approved is armed before strangers_from (wait_over), but the one of again_pub while
 * again_ok (the comments' `again`): the browser whose own login ran out and asks anew */
static long long strangers_from;
static uint8_t again_pub[32];
static int again_ok;
/* lk: what state/web_wait says (hold_save): until when, on this boot's monotonic clock, no browser that is not
 * approved is armed if hassmic starts anew now; and when waits for a note's dismissal were armed (syn_ask's budget) */
static long long hold_saved;
/* A phone's wait for the button went off (improv.c, by main.c's hook: web_quiet): when, 0 none or taken.  Written
 * with no lock, by a thread that holds improv.c's; taken under lk by whoever looks at the waits next (phone_over) */
static atomic_llong phone_off;
static long long phone_until;       /* lk: the strangers_from the last such end asked for (hold_save: written with time to spare) */
static atomic_int expiry_runs;      /* hold_cv is there to be signalled */
#define NOTE_ARMS 3
static long long note_arms[NOTE_ARMS];
static void hold_save(long long now);
static void hold_flush(int armed);
static void hold_poke(void);
static struct { uint32_t addr; long long at; int n; } asks[8], asks_all;      /* lk: ask_rate */
static atomic_int nconn;
static char start_id[17];           /* new at every start: the page knows a restart from an answer of the old process */
/* A note of the owner's ("Kitchen, by the window"), shown under the name (issue #153): the name is the model and the
 * MAC's end, and several Echos of one model told apart only by six hex digits.  In state/note, one line; in the
 * unsigned hello, as the name is, since the login page is where it helps most (which Echo's button to press): a
 * location, readable to anyone on the LAN who asks.  Not in exports: it is this Echo's, as the name.  Own lock: hello
 * runs on every connection thread without lk */
#define NOTE_MAX 80                 /* bytes; the page cuts at a character's end */
static pthread_mutex_t note_lk = PTHREAD_MUTEX_INITIALIZER;     /* note[] only: never held over the flash */
static pthread_mutex_t note_save_lk = PTHREAD_MUTEX_INITIALIZER; /* one save at a time: file and note[] in one order */
static char note[NOTE_MAX + 1];
/* /api/hello: the rest <= ~820 bytes (name, node 160 each, note 168), members <= 16 x ~104 (arb.c NPEER, NLEN); a
 * hello cut short is no JSON, and the login page cannot start */
#define HELLO_MAX 4096
#define NONCE_MS 120000
static struct { uint8_t pub[32], nonce[16]; long long at; } nonces[8];      /* handed out for a login through another Echo */
static unsigned nonce_next;
/* ... and for a plugin's acceptance through another Echo (sync, below): asked for with a signed request, so each is
 * an approved browser's.  Eight at a time, the oldest overwritten; each good for one answer, within ACC_NONCE_MS */
static int ACC_NONCE_MS = 120000;   /* HASSMIC_PLUGIN_NONCE_MS for the tests */
static struct { uint8_t pub[32], nonce[16]; long long at; } acc_nonces[8];  /* lk */
static unsigned acc_nonce_next;

static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static const char *state_dir(void) { const char *e = getenv("HASSMIC_STATE"); return e ? e : "/data/local/hassmic/state"; }

static void hex(char *out, const uint8_t *b, size_t n) { for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]); }
static int unhex(uint8_t *out, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) { unsigned v; if (!isxdigit((unsigned char)s[2 * i]) || !isxdigit((unsigned char)s[2 * i + 1]) || sscanf(s + 2 * i, "%2x", &v) != 1) return -1; out[i] = (uint8_t)v; }
    return s[2 * n] ? -1 : 0;
}
/* hex of any even length up to CAP bytes: the byte count, or -1 */
static int unhex_any(uint8_t *out, size_t cap, const char *s)
{
    size_t n = strlen(s);
    return n % 2 || n / 2 > cap || unhex(out, s, n / 2) ? -1 : (int)(n / 2);
}

/* ---------------------------------------------------------------- keys and approved browsers (lk held) */

static int load_key(void)
{
    char p[300], t[64] = ""; FILE *f; int ok = 0;
    snprintf(p, sizeof p, "%s/web_key", state_dir());
    /* (this and the two lists below: regular files or nothing, opened without waiting, as the store opens its own.
     * A FIFO at state/web_clients had web_start wait for a writer for good, with lk held, and no page ever came up) */
    if ((f = ps_fopen(p))) { ok = fgets(t, sizeof t, f) && b64_decode(t, strcspn(t, "\r\n"), sk, 32) == 32; fclose(f); }
    if (!ok) {
        ws_random(sk, 32); b64_encode(sk, 32, t, 0, 1); strcat(t, "\n");
        if (statefile_write(p, 0600, t, strlen(t))) { crypto_wipe(t, sizeof t); fprintf(stderr, "web: cannot write %s\n", p); return -1; }
    }
    crypto_wipe(t, sizeof t);
    crypto_x25519_public_key(pk, sk);
    return 0;
}

static void save_clients(void)
{
    char p[300], h[65]; struct statefile s;
    snprintf(p, sizeof p, "%s/web_clients", state_dir());
    if (statefile_open(&s, p, 0600)) { fprintf(stderr, "web: cannot write %s.tmp\n", p); return; }
    for (int i = 0; i < nclients; i++) { hex(h, clients[i].pub, 32); fprintf(s.f, "%s %llu %s\n", h, clients[i].ctr, clients[i].label); }
    if (statefile_commit(&s)) fprintf(stderr, "web: cannot write %s\n", p);
}

static void load_clients(void)
{
    char p[300], line[200], h[80]; unsigned long long c; int n; FILE *f;
    snprintf(p, sizeof p, "%s/web_clients", state_dir());
    if (!(f = ps_fopen(p))) return;
    while (nclients < MAX_CLIENTS && fgets(line, sizeof line, f)) {
        struct client *k = &clients[nclients];
        line[strcspn(line, "\r\n")] = 0;
        if (sscanf(line, "%79s %llu %n", h, &c, &n) < 2 || unhex(k->pub, h, 32)) continue;
        k->ctr = k->floor = c; k->nseen = 0; snprintf(k->label, sizeof k->label, "%s", line + n); nclients++;
    }
    fclose(f);
}

static struct client *client_of(const uint8_t pub[32])
{
    for (int i = 0; i < nclients; i++) if (!memcmp(clients[i].pub, pub, 32)) return &clients[i];
    return NULL;
}

/* lk held.  0, or -1: not on the flash (whoever was to be added is then no browser of the button's) */
static int save_button(void)
{
    char p[300], t[MAX_CLIENTS * 65 + 1]; size_t n = 0;
    snprintf(p, sizeof p, "%s/web_button", state_dir());
    for (int i = 0; i < nclients; i++) if (clients[i].button) { hex(t + n, clients[i].pub, 32); n += 64; t[n++] = '\n'; }
    if (n ? statefile_write(p, 0600, t, n) : unlink(p) && errno != ENOENT) { fprintf(stderr, "web: cannot write %s\n", p); return -1; }
    return 0;
}

/* lk held, at start, after load_clients.  A line that names no browser we know (one revoked or pushed out whose
 * rewrite of this file failed: button_stale, which is memory's and gone with the restart; a file cut short) is not
 * left there: that key coming back on a voucher would be a browser of the button's after the start after that.
 * The file is written again from what was taken, here, and with the next browser added if that fails too */
static int button_stale;
static void load_button(void)
{
    char p[300], line[100]; uint8_t pub[32]; struct client *c; FILE *f; int n;
    snprintf(p, sizeof p, "%s/web_vouched", state_dir()); unlink(p);        /* (the list the other way round, of one build: read no more) */
    snprintf(p, sizeof p, "%s/web_button", state_dir());
    if (!(f = ps_fopen(p))) return;
    while (fgets(line, sizeof line, f)) {
        n = (int)strcspn(line, "\r\n"); line[n] = 0;
        if (n == 64 && !unhex(pub, line, 32) && (c = client_of(pub))) c->button = 1;
        else button_stale = 1;
    }
    fclose(f);
    if (button_stale) {
        fprintf(stderr, "web: %s named a browser that is not approved here: written again without it\n", p);
        if (!save_button()) button_stale = 0;
    }
}

static int session_key(uint8_t k[32], const uint8_t pub[32])
{
    uint8_t shared[32], zero[32] = { 0 }, msg[13 + 64];
    crypto_x25519(shared, sk, pub);
    if (!memcmp(shared, zero, 32)) return -1;           /* a low-order point: no secret at all */
    memcpy(msg, "hassmic web 1", 13); memcpy(msg + 13, pk, 32); memcpy(msg + 45, pub, 32);
    crypto_blake2b_keyed(k, 32, shared, 32, msg, sizeof msg);
    crypto_wipe(shared, sizeof shared);
    return 0;
}

/* ---------------------------------------------------------------- login */

/* lk held.  BUTTON: with this Echo's action button (else on another Echo's voucher).  A browser of the button's
 * that comes again on a voucher stays one; the fact counts only once it is on the flash (save_button).  File and
 * memory in step the other way too: the browser pushed out at MAX_CLIENTS left its line in state/web_button,
 * came back on a voucher (button 0 in memory) and was a browser of the button's again after the next start, which
 * reads the file (a review, 2026-11-20).  So the file is written again whenever a browser of the button's leaves
 * clients[], and a write that failed is tried again with the next browser added (button_stale) and made up for
 * at the next start (load_button) */
static void add_client(const uint8_t pub[32], const char *label, int button)
{
    struct client *k = client_of(pub);
    if (!k) {
        if (nclients == MAX_CLIENTS) {          /* the oldest goes */
            if (clients[0].button) button_stale = 1;
            memmove(clients, clients + 1, sizeof clients[0] * (MAX_CLIENTS - 1)); nclients--;
        }
        k = &clients[nclients++]; memcpy(k->pub, pub, 32); k->ctr = k->floor = 0; k->nseen = 0; k->button = 0;
    }
    if (button_stale && !save_button()) button_stale = 0;
    if (button && !k->button) { k->button = 1; if (save_button()) k->button = 0; }
    snprintf(k->label, sizeof k->label, "%s", label);
    save_clients();
}

/* The voucher another Echo of our network gives for a browser: keyed with arb_web_key over what it binds */
static int voucher(uint8_t out[16], const uint8_t target[32], const uint8_t browser[32], const uint8_t nonce[16], const char *via)
{
    uint8_t k[32]; crypto_blake2b_ctx ctx;
    if (arb_web_key(k)) return -1;
    crypto_blake2b_keyed_init(&ctx, 16, k, 32);
    crypto_blake2b_update(&ctx, (const uint8_t *)"hassmic voucher 1", 17);
    crypto_blake2b_update(&ctx, target, 32); crypto_blake2b_update(&ctx, browser, 32); crypto_blake2b_update(&ctx, nonce, 16);
    crypto_blake2b_update(&ctx, (const uint8_t *)via, strlen(via));
    crypto_blake2b_final(&ctx, out); crypto_wipe(k, sizeof k);
    return 0;
}

/* ---- what waits for the action button.
 * One button answers six questions, five by a press and one by being held for a second: let this browser in
 * (a login), open adb (W_ADB), accept what this plugin may do (W_PLUGIN), run a plugin the index withdrew anyway
 * (W_ANYWAY, below), trust a key of Sources (W_SOURCE, below); held, take the note of a plugin copied here without a
 * press off the pages (W_NOTE, below).
 * (Community plugins, docs/plugins.md 2.7, are no question of the button's: their switch is a setting of the
 * page's as any, from an approved browser, and every one of them is still installed with W_PLUGIN's press.)
 * Whoever stands at the Echo sees a ring and has a page that told him to press; he cannot see whom the press will
 * serve.  So that is fixed when the wait is armed, cannot change under him, and is not handed to anyone else for as
 * long as he may still be on his way.
 *
 * The invariants.
 *  I1, one wait.  At most one wait is armed (login.state 1).  An armed wait ends by the press, by its time
 *    (LOGIN_MS), or by its owner's own signed word (adb "off" from the browser that asked, the install's cancel,
 *    the note's "keep", "run anyway"'s cancel, a revoke of the browser that armed
 *    it).  A login, which has no signed word, also ends when
 *    another browser that is not approved asks (it turns contested: armed for nobody, below), by a press a
 *    phone's setup has a claim to, and with a phone's wait that showed beside it (Improv, below).  No request puts another wait in an armed one's place: whoever
 *    asks while one is armed is told "busy" or "contested", and is not armed.
 *  I2, the press is for who armed it.  A press lets browser K in only while K's login is armed.  And K's login is
 *    armed at a time t only when every wait W that was armed before, other than K's own login that ran out, is
 *    over and left behind by a quiet time: t >= the press that ended W + QUIET_MS, or, where no press ended it,
 *    t >= W's arming + LOGIN_MS + QUIET_MS; and when every press P that came for W and found nothing to serve
 *    (W contested by then, or over and its quiet time still running) is left behind as well: t >= P + QUIET_MS.
 *    So whoever a page sent to the Echo for W has had all the time that page gave him (LOGIN_MS from the arming,
 *    shown counting down) and QUIET_MS more, and from each press of his, whatever it met, QUIET_MS for the next,
 *    before a press can be K's.  QUIET_MS is half of LOGIN_MS: the wait gives a minute for reading the page,
 *    walking over and pressing, and somebody who was on his way when it ended is there within half of that.
 *    Both hold over a restart of hassmic within one boot (an update, a crash, the wake word engine changed):
 *    the time before which no stranger is armed is on the flash (state/web_wait, hold_save) from the moment a
 *    wait is armed, and for as long as the button is a phone's setup's business (Improv, below: that wait is
 *    improv.c's, a restart ends it, and what is kept is the QUIET_MS its end leaves here, written ahead and
 *    renewed while it goes on; the write follows the wait's beginning by the expiry thread's wake-up and the
 *    flash's two fsyncs, since nothing can make a Bluetooth link wait for them as a page's answer does).  The
 *    boot is told by the kernel's boot id; where that cannot be read, by the clock alone
 *    (hold_write's comment: the same hold, and after a reboot up to two minutes of it for nothing).
 * Why they hold.  Every end of an armed wait goes through wait_over, which moves strangers_from to the time I2
 * names and never back, or is the contested turn, whose record (state 4) keeps every unsigned ask out until at
 * least arming + LOGIN_MS + QUIET_MS; wait_arm, which may clear that record for an approved browser, carries its
 * time into strangers_from first.  A press that serves nothing while such a record stands or strangers_from is
 * still ahead moves both to its own time + QUIET_MS (web_approve's end).  login_ask arms a browser that is not
 * approved in one place only, with nothing armed, no contested record and now >= strangers_from; the exception is
 * `again`, the key whose own login ran out with nobody else having asked meanwhile and nobody having pressed
 * since, set by that end alone and cleared by every other and by a press that served nothing (so I2's "from each
 * press of his" has no exception: it had one, the late press was followed by that key armed at once, and the
 * second press of whoever had come for an earlier wait let it in; a review, 2026-11-20).  hold_save writes the latest of those times whenever it moved, and web_start reads it
 * into strangers_from when the boot is the same (`again` is not kept: after a restart that key waits as all do).
 * It was otherwise three times (three reviews, 2026-11-20, each run against the PC build).  First a second key
 * asking ended whatever waited, and its next ask was armed: an owner sent to the Echo to accept a plugin approved
 * a browser he had never seen.  Then that held for signed waits only: after a login had ended, by its time, by
 * being contested for ten seconds, or by the press that approved it, the next key was armed at once, with the
 * same ring, and the late press, the press of the owner whose page had said "waiting" before the contest, or the
 * second press of someone not sure the first one took, let in whoever had asked last.  An approved browser sets
 * everything.  Then the time counted from the wait's end only, whatever the owner did after it: his press into a
 * contested login (the ring dark, his page in a pocket still saying "waiting") or into the quiet time held nobody
 * back, the stranger was armed when the time from the arming was up, seconds later, and the owner's second press
 * ("did the first one take?") let it in; and a restart of hassmic forgot all of it, so a stranger's first ask
 * after an update was armed with the owner on his way.  A press is a person at the Echo: counting from each one
 * gives nobody on the network a way to make the hold longer.
 *
 * The boundaries, each looked at:
 *  - The time runs out as the press lands.  web_approve takes the clock once and runs login_expire with it first:
 *    a press after arming + LOGIN_MS finds nothing armed and is the assistant's, and being a press in vain
 *    inside that login's quiet time it starts QUIET_MS for every key, the one that ran out too.  Only where
 *    nobody pressed may that key be armed again at once (`again`: the page's "Try again"; without it an owner
 *    who never got to the Echo would wait out his own quiet time), and the next press lets it in.  That is the
 *    browser whose ring showed for the whole minute before, alone: had another key asked meanwhile the login had
 *    been contested, had an approved browser asked, reserved (no `again`); and the wait before it was QUIET_MS
 *    past when it was first armed.  What that costs: the owner who pressed a moment too late waits the quiet
 *    time, his page asking again by itself.
 *  - The quiet time ends.  At strangers_from the first to ask is armed, a stranger as well as the owner: by then
 *    I2's time has passed for everything before, and what shows is a ring for a login as any other.
 *  - The key that was armed asks again.  After its time: `again`, above.  After a contest: it is told "contested"
 *    as the other is, the Echo cannot tell which of the two the owner's is.  After the press: "approved".  After a
 *    press that went to a phone: a stranger like any other, kept back QUIET_MS (the second press, again).
 *  - Two browsers that are not approved, asking at once.  Both are a key and a label.  Armed for the first, a
 *    press might approve the wrong one.  So the wait turns contested (state 4): it is no longer armed, the ring
 *    goes dark, the press is nobody's and goes to the assistant as any press does (not swallowed: else whoever
 *    keeps two keys asking takes the button away), and both are told "contested".  The record is held until
 *    QUIET_MS after the later of the first one's arming + LOGIN_MS and the last competing ask, which is an ask by
 *    another key than the one that asked last: the first asker may have left his page at "waiting" and be on his
 *    way for all of the minute it promised, and whoever still competes shows that the Echo cannot tell them apart
 *    yet.  A page that asks alone, again and again, gets its turn when that time is up.  The page keeps asking by
 *    itself while it is told "contested", "busy" or "rate" (web/app.js: every two seconds), so an owner's open
 *    page stays a competitor and a stranger is not armed beside it: an ask of another key turns an armed login
 *    contested also when it is over the count of asks (below) and is otherwise refused for that.  Where the
 *    owner's page is closed, the stranger is armed after that time, and what shows then is a login nobody at the
 *    Echo asked for (the last paragraph).
 *  - A signed ask meanwhile.  A wait an approved browser arms with a signed request (adb, a plugin, a note) is
 *    armed whenever nothing else is: also within the quiet time and over a contested record, whose times stay for
 *    the strangers.  So no unsigned request holds up an install or adb for longer than one login's time: asked
 *    while a login is armed it is told "busy" and leaves a mark (login.reserved), that login is not ended for it
 *    (the person about to let in his phone would open adb instead), and when the login has ended no stranger is
 *    armed for QUIET_MS, not its own key either, while the page that was told "busy" asks again by itself
 *    (web/app.js adbOpen, web/plugins.js plgFinish: when /api/state's button no longer holds a login).  What this
 *    leaves: the late press for a login that just ended can serve an approved browser's wait armed in that
 *    moment.  That browser is one the owner let in, it could set everything on the page before, and it is told
 *    apart from a stranger by its signature; what the press adds for it is adb or a plugin, as a press meant for
 *    the assistant would (W_NOTE, below, says the same).  Held back like a stranger instead, anyone on the LAN
 *    could keep installs and adb away for good with a login asked for once a minute.
 *  - Improv (improv.c), a phone's setup over Bluetooth, shares the button and has its own wait
 *    (improv_authorize; main.c on_action offers the press to web_approve() first, then to it), and shows the same
 *    ring as a login.  That wait exists only on an Echo that is set up and has been without a network for ten
 *    minutes as such (improv.c: its M_LOST is entered in that one way; an Echo taken on by Home Assistant with a
 *    phone still connected from its setup ends that link and shows no wait.  It did show one, on an Echo that was
 *    online, and the owner's press for his own login went to whatever was connected; a review, 2026-11-20), when no
 *    browser reaches this page, and only while a phone is connected (a press with none there is not Improv's);
 *    the overlap is the network back with the phone still there, or a browser on a network the Echo no longer
 *    counts as one.  In it no stranger's login is armed for as long as the button is the phone's business
 *    (improv_holds): a phone connected, waiting or not, and the time its press opened, a switch under way and
 *    "provisioned" after it.  It was only while the phone's wait showed (improv_waits): the press made the phone
 *    "authorized", the wait was over, a stranger's ask half a second later was armed with the same ring, and the
 *    owner's second press let that browser in (a review, 2026-11-20).  And the end of the phone's wait is the
 *    end of a wait for I2 as well (web_quiet, main.c's hook whenever the ring's wait for a phone goes off, leaves
 *    its time; phone_over takes it under lk before anything here looks at the waits: every ask, the press, the
 *    hold, the expiry thread, all through login_expire): by the press, strangers_from is at least QUIET_MS after
 *    it, whatever becomes of the phone; without one (the phone left or was let go, the network came back),
 *    QUIET_MS from then, for whoever the phone's app sent to the Echo and is still on his way.  A login that was
 *    armed before the phone's wait began, or as it did (login_ask reads improv_holds without improv.c's lock: an
 *    ask that read 0 while the phone connected is armed an instant into the wait; never after its end, since
 *    improv.c leaves the mark of the end before it lowers what login_ask reads, and every ask reads that first
 *    and takes the mark after: a 0 read means the mark is already there, or no wait had begun.  It took the
 *    mark first and read after, and a wait that ended between the two was missed for that ask), ends with that
 *    wait, however it ends, and its browser is not approved.
 *    At the press: web_approve hands the press to the phone (it asks improv_waits, in the same order, before
 *    the mark is taken: only a wait that shows takes a press, and one that ended in that instant ends the login
 *    by its mark).  Without one: phone_over ends it as "busy" the moment the wait is over.  It stayed armed then: a
 *    phone connected beside an armed login and left a second later, and the press half a second after that, by
 *    somebody the phone's app had sent to the Echo, let the browser in (a review, 2026-11-20).  Two waits behind
 *    one ring, and the Echo cannot ask which was meant; the phone's is the one that needs somebody within
 *    Bluetooth's reach of the Echo and gives a network's name and password, the login can be anyone's on the LAN
 *    and gives everything.  So the doubt goes to the phone, and the browser, told "busy", asks again by itself
 *    and is armed after the quiet time.  I1 is about this page's waits: a login now also ends by the end of a
 *    phone's wait that showed beside it, and still no request here ends or replaces the phone's wait, which goes
 *    by improv.c's own rules (the press, the link's end, its WAIT_SECS).  What neither invariant covers is which
 *    phone the press serves: improv.c's header.  An approved browser's adb, plugin or note wait is armed beside
 *    the phone's, gets the press first, and is NOT ended when the phone's wait goes off: the owner's own two
 *    requests, the page says which the press serves, and the ring shows the higher (main.c attention).  The late
 *    press for the phone that lands on such a wait opens adb or accepts a plugin for a browser the owner let in,
 *    as a press meant for the assistant would ("A signed ask meanwhile", above, argues the same for a login's
 *    late press); ended with the phone's wait, anybody in Bluetooth's range of an Echo without its network could
 *    take every install and adb request away by connecting and leaving.  What a device in range can do to a
 *    login: keep new ones from being armed for as long as it holds the link or comes and goes (improv.c's header
 *    says nothing bounds that), on an Echo that advertises, which is one ten minutes without its network.
 *  - W_NOTE (sync, at the plugins' install slot, has what it is for).  It is a wait as adb's and a plugin's are,
 *    under the same rule: armed by an approved browser's signed request and only while nothing else is armed
 *    ("busy" otherwise, nothing changed), ended by its time, the asker's own "keep" or its revoke; and it is the
 *    one armed wait, so every other is told "busy" meanwhile.  What ends it at the Echo is not a press but the
 *    action button HELD for a second (web_hold; buttons.c's first hold stage, the one Home Assistant's "Button"
 *    hears as action_hold, which is not told of a hold that served a note).  A press while it is armed goes where
 *    it would have gone without it: to a call that rings, the music, the assistant.  It was a press at first, and
 *    the browser that armed the wait could make the press happen: it armed the wait and then rang a Drop In on
 *    that Echo, and the owner's press to answer took the note off instead and left the call ringing (a review,
 *    2026-11-20); the same with a timer made to ring or music to be paused.  Nothing an approved browser can
 *    start on the Echo is answered by holding the button for a second.  Somebody may still hold it for another
 *    reason: Home Assistant's "Button" event has that hold (action_hold) for automations, and the reset begins
 *    with it.  So only a hold that BEGAN after the wait was armed is the wait's (web_hold's down time against
 *    login.at; a wait armed into a hold under way took the note with it), and while a note's wait shows, a hold
 *    begun after it is the note's and the "Button" event does not fire for it: an owner whose automation hangs
 *    on action_hold loses that one hold, for the minute the wait lasts, three times an hour at the most
 *    (syn_ask), and the ring's `wait` pattern shows it.  What the
 *    hold will do is fixed when the wait is armed: it takes off the notes that were there then, by their numbers
 *    (syn_press), never one that came after, so a plugin copied here while the owner walks to the Echo is not
 *    dismissed unread by the hold he meant for the others.  A hold that goes on is still what it was without a
 *    note: the ring's warning at 5 s, the reset at 10 s (main.c on_hold).  Models without an action button
 *    (board.action_combo): both volume keys held together for a second and let go before the two of the pairing
 *    hold, by the clock (buttons.c: held two seconds or more it is the pairing hold's alone).  How often the wait may be armed is bounded per Echo (syn_ask).
 *  - W_ANYWAY (5.6: a plugin the plugin index withdrew, run regardless).  A wait as adb's and a plugin's install:
 *    armed by an approved browser's signed request (POST /api/plugins/<id>/anyway "run") and only while nothing else
 *    is armed ("busy" otherwise, nothing changed), ended by the press, its time, the asker's own "cancel" or its
 *    revoke; I1 and I2 hold for it as they hold for those, through wait_arm and wait_over, with nothing of its own.
 *    A press, not a hold, as W_PLUGIN's: what it does is of the same weight as an install the owner accepts (a
 *    plugin runs with what was accepted for it, nothing more), and the press for an install is a press; a browser
 *    that can make a press happen (W_NOTE says how) can as well arm an install of anything the index lists.  What
 *    the press does is fixed when it is armed: that plugin, for the revocation list this Echo had then
 *    (anyway_press); the mark it writes holds for that list alone, so a newer list, taken between the arming and
 *    the press or after it, stops the plugin again and asks anew.  The ring shows the plugins' pattern (2).
 *  - W_SOURCE (issue #175: Sources, a key of the owner's or no signature check for updates or the plugin index).  A
 *    wait as adb's: armed by an approved browser's signed POST /api/sources that trusts more than what is in force
 *    (sources_widens), only while nothing else is armed, ended by the press, its time, the asker's "cancel" or its
 *    revoke; I1 and I2 through wait_arm and wait_over.  What the press puts in force is fixed when it is armed
 *    (source_press), and only over what was in force then (source_from): a change made meanwhile (another browser
 *    took a key away, which needs no press) and the press puts nothing in force, its browser told.  The login's
 *    pattern (1), as adb's: what it lets in is of adb's weight (root installs what that key signs).
 * What the Echo shows is what the button will do: hooks->attention(1) while a login or adb is armed, (2) for a
 * plugin, a plugin run anyway or a note (a pattern of its own), (0) when nothing is, a contested login included; on an Echo with a
 * screen the face draws the same patterns.  And the page that armed a wait is told at every poll what the Echo
 * holds ("holds": login, adb, plugin, anyway, note), so it never sends the owner to press for something else.
 *
 * What none of this covers.  A login is armed for whoever asks first when nothing else waits and no quiet time
 * runs: the ring then shows a login, the same ring for a stranger's as for the owner's, and a press on it lets
 * that browser in.  The invariants only say that the press is not turned from one wait to another; they cannot
 * say that the person pressing looked at his own page first.  Press for a login only while your own page says it
 * waits (docs/SECURITY.md says so to the owner).
 * What it costs.  A stranger on the LAN can delay new logins for as long as he keeps asking: contesting each one
 * (both are told, and the hold is LOGIN_MS + QUIET_MS at least, not ten seconds as before), or keeping a login of
 * his own armed, minute after minute.  He cannot delay an approved browser's wait beyond one login's time.
 * Every unsigned ask is counted per source address and overall (ask_rate, a table of eight), but those of the key
 * whose login is armed: it is told "waiting" and nothing changes, and counted they were a way to use up the count
 * of all with one's own armed login and so keep every other page's ask from contesting it (a stranger's login
 * stayed armed, ring on, for 47 of 61 s beside the owner's open page, which was told "rate"; a review,
 * 2026-11-20).  Over the limit an ask is refused ("rate") and arms nothing, so three addresses asking as fast as
 * they may (ASK_ADDR each) use up ASK_ALL and no new login is taken while they do; the one thing such an ask still
 * does is turn an armed login of another key contested: that takes a press away from a stranger and never gives
 * one, it is one flag, and its log line comes once per armed login.  So a login is never armed for long beside
 * anyone else who asks, however the count stands, and `again` (which needs a login that ran out uncontested) is
 * never had by a key beside which asks were dropped.  An ask writes nothing to the flash but a log line and
 * state/web_wait when a wait is armed or contested, and those are bounded by the times above (hold_write says how
 * for a contest that two keys keep alive, and that the write is made without lk held). */

static const char *holds_word(void)     /* lk held: what the press would serve now */
{
    return login.state != 1 ? "none" : login.kind == W_ADB ? "adb" : login.kind == W_PLUGIN ? "plugin" : login.kind == W_NOTE ? "note" : login.kind == W_ANYWAY ? "anyway"
         : login.kind == W_SOURCE ? "sources" : "login";
}

/* lk held: the armed wait has ended; every such end comes through here but a login's turning contested, which
 * keeps its own time (login_ask).  PRESSED: by the press, whatever came of it; else by its time or a withdrawal.
 * No login of a browser that is not approved is armed until whoever this wait sent to the Echo has pressed in
 * vain (I2 above): QUIET_MS from the press, or from the end of the time the wait's page promised.  AGAIN: the one
 * key that may be armed before that, a login's own that ran out, or NULL; not after a login an approved browser
 * was told "busy" for, which goes first */
static void wait_over(long long now, int pressed, const uint8_t *who)
{
    long long until = (pressed ? now : login.at + LOGIN_MS) + QUIET_MS;     /* (not from when a time's running out was noticed) */
    if (until > strangers_from) strangers_from = until;
    again_ok = who && !login.reserved;
    if (again_ok) memcpy(again_pub, who, 32);
    login.reserved = 0;
}

/* lk held: a phone's wait for the button went off since anyone looked (web_quiet).  It counts as the end of a wait
 * here does (I2): no stranger is armed for QUIET_MS from then, nor the key whose own login ran out; and a login
 * that is armed now was armed before that wait began or in the instant it did, not after it ended (none is armed
 * while a phone is there, login_ask; improv.c show() leaves this mark before it lowers what that reads, and
 * login_ask reads it before it comes here) and ends,
 * told "busy": the press on its way may be the phone's (the section's comment, which also says why an approved
 * browser's wait stays).  No sound: nobody pressed, and nothing was refused that a person is waiting to hear of */
static void phone_over(long long now)
{
    long long t = atomic_exchange(&phone_off, 0);
    if (!t) return;
    if (t + QUIET_MS > strangers_from) strangers_from = t + QUIET_MS;
    if (login.state == 4 && t + QUIET_MS > login.quiet) login.quiet = t + QUIET_MS;
    phone_until = t + QUIET_MS;
    again_ok = 0;
    if (login.state == 1 && login.kind == W_LOGIN) {
        login.state = 3; login.end = now; login.why = "busy"; wait_over(t, 1, NULL);
        fprintf(stderr, "web: a phone's setup waited for the button beside the login (%s) and that wait is over: the login is ended, a press now is not its\n", login.label);
        if (hooks->attention) hooks->attention(0);
    }
}

static void login_expire(long long now)     /* lk held; first in everything that looks at the waits */
{
    phone_over(now);
    if (login.state == 1 && now - login.at > LOGIN_MS) {
        login.state = 3; login.end = now; login.why = "timeout";
        fprintf(stderr, "web: %s ran out without the button\n", login.kind == W_ADB ? "adb request" : login.kind == W_PLUGIN ? "plugin install" : login.kind == W_NOTE ? "the wait to take a copied plugin's note off"
                : login.kind == W_ANYWAY ? "the wait to run a withdrawn plugin anyway" : login.kind == W_SOURCE ? "the wait to trust a key of Sources" : "login");
        wait_over(now, 0, login.kind == W_LOGIN ? login.pub : NULL);
        if (hooks->attention) hooks->attention(0);
        if (hooks->approved) hooks->approved(0);
    }
    if (login.state == 4 && now >= login.quiet) login.state = 0;
}

/* An adb request that waits goes once the browser that asked closes adb or is let go: left waiting, the next press
 * opened adb for 30 min against the owner's last word, and the page kept saying it waited (lk held) */
static void adb_withdraw(const char *why)
{
    login.state = 3; login.end = now_ms(); login.why = "busy"; fprintf(stderr, "web: adb request withdrawn (%s)\n", why);
    wait_over(login.end, 0, NULL);
    if (hooks->attention) hooks->attention(0);
}

int web_approve(void)
{
    int r = 0, phone, ppress = PS_PRESSED; unsigned pgen = 0; uint8_t pdigest[32]; long long now = now_ms(); char anyway_id[PS_ID_MAX] = ""; long long anyway_serial = -1;
    struct sources src_to, src_from;
    pthread_mutex_lock(&lk);
    /* Whether a phone waits for this press: read before the mark of such a wait's end is taken (login_ask says
     * why that order).  1: a login armed beside it is not approved, below, also where the wait ended in this
     * instant (the mark, taken next, ends that login itself).  0: no wait showed, or its end's mark is there
     * and login_expire ends the login by it.  Read after, the press approved a login armed before a phone's
     * wait that ended between the two */
    phone = improv_waits();
    login_expire(now);
    /* (a contested login, state 4, is not armed: the press is not this page's, r stays 0) */
    /* the browser that asked pushed out of clients[] meanwhile: logins through other Echos take no press */
    if (login.state == 1 && login.kind == W_ADB && !client_of(login.pub)) {
        adb_withdraw("its browser is no longer let in");
        if (hooks->approved) hooks->approved(0);        /* the press goes on to the assistant, as after one that ran out */
    } else if (login.state == 1 && login.kind == W_PLUGIN && !client_of(login.pub)) {
        plugin_withdraw("its browser is no longer let in");
        if (hooks->approved) hooks->approved(0);
    } else if (login.state == 1 && login.kind == W_PLUGIN && !plugin_accepted(&pgen, pdigest, &ppress)) {
        /* the press was armed for a package that is no longer what the install slot holds (plugin_accepted says
         * what it compares): it accepts nothing, and goes on to the assistant as one after a wait that ran out */
        login.state = 3; login.end = now; login.why = "busy"; wait_over(now, 1, NULL);
        fprintf(stderr, "web: the press came for a plugin's install that is no longer the one its dialog listed: nothing installed\n");
        if (hooks->attention) hooks->attention(0);
        if (hooks->approved) hooks->approved(0);
    } else if (login.state == 1 && login.kind == W_PLUGIN) {
        /* the owner accepts what the page listed: one line per capability, from the package the Echo checked
         * (the slot is INSTALLING from here: not "ran out" to a page that asks while the files are written) */
        login.state = 2; r = 3; wait_over(now, 1, NULL);
        fprintf(stderr, "web: a plugin's install accepted with the button%s\n", ppress == PS_PRESSED2 || ppress == PS_VOUCHED_PRESSED ? " (its second press)" : "");
        if (hooks->attention) hooks->attention(0);
        if (hooks->approved) hooks->approved(1);
    } else if (login.state == 1 && login.kind == W_NOTE && !client_of(login.pub)) {
        note_withdraw("its browser is no longer let in");
        if (hooks->approved) hooks->approved(0);
    } else if (login.state == 1 && login.kind == W_NOTE) {
        /* a note's wait is the hold's (web_hold), never a press's: this one goes where it would have gone without
         * the wait, which stays armed (the section's comment: the browser that armed it can make a press happen) */
    } else if (login.state == 1 && login.kind == W_ANYWAY && !client_of(login.pub)) {
        anyway_withdraw("its browser is no longer let in");
        if (hooks->approved) hooks->approved(0);
    } else if (login.state == 1 && login.kind == W_ANYWAY) {
        login.state = 2; r = 4; wait_over(now, 1, NULL); snprintf(anyway_id, sizeof anyway_id, "%s", anyway_press.id); anyway_serial = anyway_press.serial;
        fprintf(stderr, "web: %s is to run though the plugin index withdrew it: accepted with the button\n", anyway_id);
        if (hooks->attention) hooks->attention(0);
        if (hooks->approved) hooks->approved(1);
    } else if (login.state == 1 && login.kind == W_SOURCE && !client_of(login.pub)) {
        source_withdraw("its browser is no longer let in");
        if (hooks->approved) hooks->approved(0);
    } else if (login.state == 1 && login.kind == W_SOURCE) {
        /* only over what was in force when it was armed: a change since (needs no press when it trusts less) and
         * this press is not for what stands now; it puts nothing in force and goes on as one after a wait that ran
         * out (the page asks again) */
        struct sources now_src; sources_get(&now_src);
        if (!sources_equal(&now_src, &source_from)) {
            login.state = 3; login.end = now; login.why = "busy"; wait_over(now, 1, NULL);
            fprintf(stderr, "web: the press came for Sources that changed since it was asked: nothing put in force\n");
            if (hooks->attention) hooks->attention(0);
            if (hooks->approved) hooks->approved(0);
        } else {
            login.state = 2; r = 5; wait_over(now, 1, NULL); src_to = source_press; src_from = source_from;
            fprintf(stderr, "web: Sources (a key of the owner's or no signature check) accepted with the button\n");
            if (hooks->attention) hooks->attention(0);
            if (hooks->approved) hooks->approved(1);
        }
    } else if (login.state == 1 && login.kind == W_ADB) {
        login.state = 2; r = 2; wait_over(now, 1, NULL);
        fprintf(stderr, "web: adb over Wi-Fi approved with the button\n");
        if (hooks->attention) hooks->attention(0);
        if (hooks->approved) hooks->approved(1);
    } else if (login.state == 1 && phone) {
        /* a login, and a phone's setup began to wait for the button after it was armed (none is armed while one
         * waits: login_ask).  Both show the same ring, and this press may be the phone's: it lets no browser in.
         * The login ends, its browser is told "busy" and may ask again; the press goes on to the phone (r stays
         * 0: main.c asks improv_authorize next, which makes the sound).  The section's comment argues the order */
        login.state = 3; login.end = now; login.why = "busy"; wait_over(now, 1, NULL);
        fprintf(stderr, "web: the press came while a phone's setup waited for the button too: the login (%s) is not approved by it\n", login.label);
        if (hooks->attention) hooks->attention(0);
    } else if (login.state == 1) {
        add_client(login.pub, login.label, 1);
        login.state = 2; r = 1; wait_over(now, 1, NULL);
        fprintf(stderr, "web: login approved (%s)\n", login.label);
        if (hooks->attention) hooks->attention(0);
        if (hooks->approved) hooks->approved(1);
    }
    /* A press that served no wait, while a contested login's record stands or the quiet time after a wait runs:
     * whoever pressed may have come for that wait (his page said "waiting" when he left it) and will press again
     * when nothing happens.  No stranger is armed for QUIET_MS from this press either (I2).  A press is made at
     * the Echo, so nobody on the network makes this hold longer.  (A press while a note's wait is armed serves
     * nothing here by design and is not such a press: strangers_from is then behind, or was moved by what came
     * before, and the same rule holds) */
    if (!r && (login.state == 4 || now < strangers_from)) {
        if (now + QUIET_MS > strangers_from) strangers_from = now + QUIET_MS;
        if (login.state == 4 && now + QUIET_MS > login.quiet) login.quiet = now + QUIET_MS;
        /* ... and not the key whose login ran out either (`again`): whoever pressed just now may not have come for
         * it, and his next press, seconds later, is not to be that key's.  Its owner, if it was he who pressed too
         * late, waits the quiet time as everyone (his page asks again by itself) */
        again_ok = 0;
    }
    hold_save(now); hold_poke();        /* (written by the expiry thread: the button's handler does not wait for the flash) */
    pthread_mutex_unlock(&lk);
    if (r == 2) { adbwifi_ask(1); r = 1; }              /* root's firewall watcher opens it for 30 min */
    if (r == 3) { plugin_install_now("button", pgen, pdigest, ppress, 0, NULL, 0); r = 1; }
    /* (the mark for the list the press was armed for: a newer one taken meanwhile is not it, and the plugin stays) */
    if (r == 4) {
        if (ps_anyway(anyway_id, anyway_serial)) fprintf(stderr, "web: cannot write that %s is to run anyway\n", anyway_id);
        plugin_withdrawn_changed();
        r = 1;
    }
    if (r == 5) {
        char err[200];
        if (sources_apply(&src_from, &src_to, err, sizeof err)) fprintf(stderr, "web: Sources not put in force: %s\n", err);
        else if (err[0]) fprintf(stderr, "web: Sources: %s\n", err);
        update_check();
        r = 1;
    }
    return r;
}

/* A phone's wait for the button (improv.c: the same ring as a login's) is over: by the press that authorized the
 * phone, or without one (the phone left, was let go, the network came back).  Either way somebody may be at the
 * Echo or on his way to it for that wait, sent by the phone's app as a page sends him for a login, and his press,
 * or his second one ("did the first take?"), is not to be a browser's.  What follows from that is phone_over's;
 * this only leaves the time for it and wakes the expiry thread (the ring of a login that ends with it goes dark at
 * once, and what is to be written is).  Called by main.c's hook with improv.c's lock held, on the controller's
 * thread among others, so it takes nothing and waits for nothing: it took lk, which is held over the clients'
 * file on the flash (save_clients, save_button), and wrote state/web_wait for every call: a central connecting
 * and leaving, which costs it nothing, 118 times in 20 s, 118 times temp file, fsync, rename, fsync (a review,
 * 2026-11-20).  The latest time is the one that counts, and one thread at a time calls this (improv.c's lock) */
void web_quiet(void)
{
    atomic_store(&phone_off, now_ms());
    if (atomic_load(&expiry_runs)) hold_poke();
}

/* A phone's wait for the button began (main.c's hook, improv.c's lock held: nothing taken, nothing waited for).
 * The expiry thread looks at once and writes what a restart is to keep of it (hold_save's term for improv_holds,
 * which improv.c raised before it called the hook); it would within its second anyway */
void web_phone(void)
{
    if (atomic_load(&expiry_runs)) hold_poke();
}

/* The action button held for a second (main.c: buttons.c's first hold stage); DOWN_MS: when the button went down, on
 * the monotonic clock.  1: a wait for a note's dismissal was armed before that and this was it; the notes that stood
 * when it was armed go (syn_press).  0: nothing here was waiting for this hold, and it is Home Assistant's "Button"
 * event as ever.  A hold that began before the wait was armed is not that wait's: the person holding had not been
 * asked yet (he holds for an automation, or for the reset), and the browser armed its wait into his hold: the note
 * went with nobody having meant it (a review, 2026-11-20).  The wait stays armed for a hold begun after it.
 * Any lock but lk */
int web_hold(long long down_ms)
{
    int r = 0; long long now = now_ms(); struct syn_ids notes; notes.k = 0;
    pthread_mutex_lock(&lk);
    login_expire(now);
    if (login.state == 1 && login.kind == W_NOTE && !client_of(login.pub)) note_withdraw("its browser is no longer let in");
    else if (login.state == 1 && login.kind == W_NOTE && down_ms <= login.at)
        fprintf(stderr, "web: the action button was held from before the wait for a copied plugin's note was armed: not that wait's, the note stands\n");
    else if (login.state == 1 && login.kind == W_NOTE) {
        login.state = 2; r = 1; wait_over(now, 1, NULL); notes = syn_press;
        if (hooks->attention) hooks->attention(0);
        if (hooks->approved) hooks->approved(1);
    }
    hold_save(now); hold_poke();
    pthread_mutex_unlock(&lk);
    if (r) syn_dismiss(&notes, "with the action button held");
    return r;
}

/* state/web_wait, "<boot id> <ms> <ms> <ms> <ms>": until when no browser that is not approved is armed, and when
 * the last waits for a note's dismissal were armed (0: none), all on the monotonic clock of the boot named.  A
 * restart of hassmic within a boot forgot every wait: an owner sent to the Echo by a page that said "waiting", an
 * update or a crash in between, a stranger's first ask armed at once with the same ring, and the press on its way
 * let that one in (a review, 2026-11-20).  The time kept is the latest that anything standing now can come to:
 * strangers_from, an armed wait's arming + LOGIN_MS + QUIET_MS (where wait_over puts it when the wait runs out; a
 * press moves it nearer, and that is written too, or a browser let in a moment before a restart kept the next one
 * out for the minute nobody was using), a contested record's time.  Not a pause after every start: an Echo that
 * armed nothing has no file, or one whose time has passed, or one of another boot (the clock began anew, and
 * whoever was on his way has had a reboot's time).
 * The flash: written when a wait is armed and when a press ends one, which nobody does faster than the waits'
 * own times; a contest's record moves with every competing ask (QUIET_MS from the last), so there the time is
 * written with QUIET_MS to spare and again only when the record has moved past that: one write per QUIET_MS
 * while two keys keep at it, and a restart in a contest holds strangers back that much longer.  The same for the
 * time after a phone's wait went off (phone_over, phone_until): a Bluetooth device in range moves it with every
 * link it makes and ends, for nothing, as often as it likes; written as it came it was a write per link (118 in
 * 20 s).  Kept all the same: without it a restart inside those QUIET_MS (a crash, an update that was waiting for
 * the network) armed the first stranger to ask, with whoever the phone's app sent to the Echo still on his way,
 * which is the case this file is for.  So: with QUIET_MS to spare, and again only once the time has moved past
 * what is written (the file then says later than need be: after a restart strangers wait up to QUIET_MS longer),
 * or where the file says more than that spare QUIET_MS later than need be, which is a login that was armed
 * (written: its arming + LOGIN_MS + QUIET_MS) and ended with a phone's wait within QUIET_MS of its arming: once
 * per login armed, and none is armed within QUIET_MS of a phone's wait.  That rule read "more than 2 QUIET_MS"
 * and so never held at the times an Echo runs with, LOGIN_MS = 2 QUIET_MS (the file would have had to say later
 * than the login's own end): it fired in the test alone, whose login lasted twenty quiet times (a review,
 * 2026-11-20).  As it stands a login a phone's wait ended later than that leaves the file at most QUIET_MS past
 * what is needed, the spare time everything here has.  And the same while the button is a phone's business
 * (hold_save: QUIET_MS from now for as long as improv_holds says so, a restart being the end of it): written
 * within the expiry thread's wake-up of the wait's beginning (web_phone), with QUIET_MS to spare, and again each
 * time now has moved past that, one write per QUIET_MS for as long as a phone is connected to an Echo without its
 * network; it was not written at all, and a restart with the ring showing a phone's wait armed the first stranger
 * to ask (a review, 2026-11-20).  Where that business ends with no wait going off (the button's time or
 * "provisioned" running out with no phone connected) the file is left as it is, up to 2 QUIET_MS ahead, for
 * nothing: not worth a write that a phone's coming and going could ask for.  One write per
 * QUIET_MS whatever a Bluetooth device does by itself.  With somebody on the LAN asking to log in as well, a
 * login armed (written: its own time) and then ended by a link made and left (written again: the rule above,
 * once per login armed) costs two, and no login is armed within QUIET_MS of a phone's wait: two more per
 * QUIET_MS at the most (39 links made and ended in 6.5 s with QUIET_MS 3 s: 3, the first for
 * the login they ended, LOGIN_MS 60 s there; at LOGIN_MS = 2 QUIET_MS the same rewrite, tests/fake_improv.py
 * taken()).  Measured on the PC
 * (LOGIN_MS 2 s, QUIET_MS 1 s; 3 keys and strangers asking at random, 1336 asks in 30 s): 14 writes, the two
 * nearest 1.09 s apart, about one per wait armed or contested; one key asking alone 47617 times in 10 s: 5, one
 * for each time its login was armed.
 * Not under lk.  It was (statefile.c: two fsyncs), from every ask, the press and the expiry thread: the button's
 * handler and every request of the page waited for the flash behind whoever asked to log in (a review, 2026-11-20).
 * Now hold_save, under lk, only decides and leaves what is to be written (hold_out; memory counts it as written
 * from then, as before: a write that fails is said once and not tried again until the time moves), and hold_flush
 * writes it with no lock but its own: one writer at a time (hold_wr_lk), and each takes, under lk, the latest
 * that was left, so the file never goes back to an earlier word whatever order the threads arrive in.  Who
 * flushes: the request that armed a wait, before it answers "waiting" (the time is on the flash before a page
 * sends anybody to the Echo: it waits for a writer that took its wait's time before it, hold_flush's ARMED); for the press and the hold the expiry thread, woken at once (hold_poke): a press
 * moves the time nearer or to its own + QUIET_MS, and hassmic ending in the milliseconds between leaves the file
 * at the wait's own, later time, or, for a press in vain into the quiet time, up to QUIET_MS short of it.
 * The boot: /proc/sys/kernel/random/boot_id, read once.  Where it cannot be read (said once in the log) the file
 * names "-" and a start holds its time against the clock alone: kept while it is ahead and no further than any
 * wait can put it (LOGIN_MS + 2 QUIET_MS); after a reboot that holds strangers back for two minutes at the most,
 * for nothing.  The notes' budget cannot be told from the clock (a time behind us may be another boot's): there
 * every start counts the waits on file as armed just then, which lengthens the hour and never shortens it */
static const char *boot_now(void)
{
    static char id[48]; static int read_; static pthread_mutex_t one = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&one);
    if (!read_) {
        const char *e = getenv("HASSMIC_BOOT_ID"); FILE *f = fopen(e ? e : "/proc/sys/kernel/random/boot_id", "r");
        if (f) { if (!fgets(id, sizeof id, f)) id[0] = 0; fclose(f); }
        id[strcspn(id, "\r\n ")] = 0;
        if (!id[0] || !strcmp(id, "-")) {
            snprintf(id, sizeof id, "-");
            fprintf(stderr, "web: this boot's id cannot be read (%s): what waits for the action button is kept over a restart by the clock alone\n", e ? e : "/proc/sys/kernel/random/boot_id");
        }
        read_ = 1;
    }
    pthread_mutex_unlock(&one);
    return id;
}

static int hold_dirty;          /* lk: note_arms changed since the file was written */
static struct { int want; long long until, arms[NOTE_ARMS]; } hold_out;    /* lk: what hold_flush is to write */
static pthread_mutex_t hold_wr_lk = PTHREAD_MUTEX_INITIALIZER;             /* one writer of the file; never taken under lk */
static pthread_cond_t hold_cv;                                             /* the expiry thread's (web_start: on the monotonic clock) */

static void hold_write(long long until)     /* lk held: left for hold_flush */
{
    hold_out.want = 1; hold_out.until = until; memcpy(hold_out.arms, note_arms, sizeof note_arms);
    hold_saved = until; hold_dirty = 0;
}

static void hold_poke(void) { pthread_cond_signal(&hold_cv); }     /* lk held or not: the expiry thread flushes now */

/* No lock held (never lk: the flash).  What hold_save left, if anything, onto the flash.  Nothing left to
 * write and the caller about to say nothing of a wait (ARMED 0): back at once, without the writer's lock (the asks
 * that change nothing, nearly all of them, do not queue behind a write).  ARMED: the caller is about to answer
 * "waiting", and that answer sends somebody to the Echo: it waits for the writer whether or not anything is left
 * for itself.  Nothing left says only that some thread took it, not that it is on the flash: the expiry thread,
 * or another ask, may be between its hold_out.want = 0 and the end of its two fsyncs with exactly this wait, and
 * the page was told "waiting" in that time (a review, 2026-11-20).  Behind the writer's lock, the write that took
 * it is done; whatever was left since is written here */
static void hold_flush(int armed)
{
    char p[300], t[200]; int n, want; long long until, arms[NOTE_ARMS];
    pthread_mutex_lock(&lk); want = hold_out.want; pthread_mutex_unlock(&lk);
    if (!want && !armed) return;
    pthread_mutex_lock(&hold_wr_lk);
    pthread_mutex_lock(&lk);
    want = hold_out.want; until = hold_out.until; memcpy(arms, hold_out.arms, sizeof arms); hold_out.want = 0;
    pthread_mutex_unlock(&lk);
    if (want) {
        snprintf(p, sizeof p, "%s/web_wait", state_dir());
        n = snprintf(t, sizeof t, "%s %lld %lld %lld %lld\n", boot_now(), until, arms[0], arms[1], arms[2]);
        /* (not written: said once, and not tried again until the time moves: this is asked every second) */
        if (statefile_write(p, 0600, t, (size_t)n)) fprintf(stderr, "web: cannot write %s: a restart forgets what waits for the button\n", p);
    }
    pthread_mutex_unlock(&hold_wr_lk);
}

static void hold_save(long long now)        /* lk held; the caller flushes once it has let go of lk, or pokes */
{
    long long need = strangers_from; int phone = 0;
    if (login.state == 1 && login.at + LOGIN_MS + QUIET_MS > need) need = login.at + LOGIN_MS + QUIET_MS;
    if (login.state == 4 && login.quiet > need) need = login.quiet;
    /* The button is a phone's setup's business (improv_holds: its wait shows, or the time its press opened, a
     * switch, "provisioned" runs).  That has no end of its own to write down, and hassmic ending now ends it: the
     * phone's link and the advertising are gone with the process.  What a start is to keep is what the end of a
     * phone's wait keeps here (phone_over): QUIET_MS from then, for whoever is on his way to press for the phone.
     * So, for as long as it goes on: QUIET_MS from now (hold_write's comment has the rest) */
    if (improv_holds() && now + QUIET_MS > need) { need = now + QUIET_MS; phone = 1; }
    /* (a time that anyone can keep moving for nothing: a contest's record, the end of a phone's wait, a phone's
     * business going on: hold_write's comment.  The phone's only where it is the latest: an armed wait's own time
     * is written as it is) */
    int moving = login.state == 4 || need == phone_until || phone;
    if (hold_dirty) { hold_write(moving ? need + QUIET_MS : need); return; }
    if (need <= now && hold_saved <= now) return;       /* nothing held, and the file says nothing else */
    if (moving) { if (need > hold_saved || hold_saved > need + QUIET_MS) hold_write(need + QUIET_MS); }
    else if (need != hold_saved && !(need <= now)) hold_write(need);
}

static void hold_load(void)     /* lk held, at start; web_start flushes */
{
    char p[300], line[200], was[48] = ""; const char *id = boot_now(); long long until = 0, a[NOTE_ARMS] = { 0 }, now = now_ms(); FILE *f; int k, same;
    snprintf(p, sizeof p, "%s/web_wait", state_dir());
    if (!(f = ps_fopen(p))) return;
    k = fgets(line, sizeof line, f) ? sscanf(line, "%47s %lld %lld %lld %lld", was, &until, &a[0], &a[1], &a[2]) : 0;
    fclose(f);
    if (k < 2) return;
    same = !strcmp(id, was);
    /* (no boot to name, now and then: by the clock alone, hold_write's comment) */
    if (same && !strcmp(id, "-") && until > now + LOGIN_MS + 2LL * QUIET_MS) until = 0;
    if (same && until > now) {
        strangers_from = hold_saved = until;
        fprintf(stderr, "web: something waited for the action button when hassmic last ended: no login of a browser that is not approved is taken for %lld s\n", (until - now + 999) / 1000);
    }
    if (same && strcmp(id, "-")) for (int i = 0; i < NOTE_ARMS; i++) note_arms[i] = a[i] > 0 && a[i] <= now ? a[i] : 0;
    else {
        /* another boot: its clock says nothing here.  A wait for a note's dismissal armed then counts as armed
         * now, so that a restart, which any approved browser can cause, shortens nothing of that bound */
        int any = 0;
        for (int i = 0; i < NOTE_ARMS; i++) if (a[i] > 0) { note_arms[i] = now; any = 1; }
        if (any) { hold_dirty = 1; hold_save(now); }
    }
}

/* lk held: an approved browser's wait armed, if nothing else is.  NULL: armed; else what the press serves now
 * ("login", "adb", "plugin", "note"): busy, nothing changed but the mark that keeps strangers back once a login
 * has ended */
static const char *wait_arm(const uint8_t pub[32], int kind, const char *label, long long now)
{
    if (login.state == 1) { if (login.kind == W_LOGIN) login.reserved = 1; return holds_word(); }
    /* a contested login's record goes (its browsers are told "busy" from now), its time stays: neither of them is
     * armed before it, whatever becomes of this wait */
    if (login.state == 4) { if (login.quiet > strangers_from) strangers_from = login.quiet; again_ok = 0; }
    memset(&login, 0, sizeof login);
    memcpy(login.pub, pub, 32); snprintf(login.label, sizeof login.label, "%s", label); login.at = now; login.state = 1; login.kind = kind;
    /* (2: not the login's pattern.  This press accepts a plugin, or the hold says a plugin's note was read: plugins'
     * business either way, and never what lets a browser in or opens adb) */
    if (hooks->attention) hooks->attention(kind == W_PLUGIN || kind == W_NOTE || kind == W_ANYWAY ? 2 : 1);
    if (kind != W_NOTE) hold_save(now);         /* (a note's: syn_ask, in one write with its budget) */
    return NULL;
}

/* adb for an approved browser: "waiting" (for the button), "busy" (something else waits: *HOLDS says what), or what
 * it is now.  "off" closes adb whoever says it (closing takes nothing from anyone), but withdraws only the wait
 * the same browser armed: another approved browser's "off" ended an armed wait that was not its own, against I1,
 * and the owner on his way pressed for nothing with no page saying why */
static const char *adb_ask(const uint8_t pub[32], int on, const char **holds)
{
    const char *r, *h; long long now = now_ms();
    *holds = "none";
    if (!on) {
        pthread_mutex_lock(&lk);
        if (login.state == 1 && login.kind == W_ADB && !memcmp(login.pub, pub, 32)) adb_withdraw("closed");
        pthread_mutex_unlock(&lk);
        adbwifi_ask(0); return "closed";
    }
    pthread_mutex_lock(&lk);
    login_expire(now);
    if (login.state == 1 && login.kind == W_ADB && !memcmp(login.pub, pub, 32)) { r = "waiting"; *holds = "adb"; }
    else if ((h = wait_arm(pub, W_ADB, "adb", now))) { r = "busy"; *holds = h; }
    else {
        r = "waiting"; *holds = "adb";
        fprintf(stderr, "web: adb over Wi-Fi waits for the action button\n");
    }
    pthread_mutex_unlock(&lk);
    hold_flush(!strcmp(r, "waiting"));
    return r;
}

/* lk held: a wait to run a withdrawn plugin anyway that no longer counts */
static void anyway_withdraw(const char *why)
{
    if (!(login.state == 1 && login.kind == W_ANYWAY)) return;
    login.state = 3; login.end = now_ms(); login.why = "busy"; wait_over(login.end, 0, NULL);
    fprintf(stderr, "web: the wait to run %s anyway withdrawn (%s)\n", anyway_press.id, why);
    if (hooks->attention) hooks->attention(0);
}

/* "Run it anyway" (5.6) for an approved browser: "waiting" (for the button), "busy" (something else waits: *HOLDS
 * says what), "running" (it may run already: not withdrawn, or run anyway for this list), "none" (no such plugin),
 * "cancelled" (RUN 0: this browser's own wait withdrawn, if it was armed) */
static const char *anyway_ask(const uint8_t pub[32], const char *id, int run, const char **holds)
{
    const struct ps_plugin *sp; const char *r, *h; long long now = now_ms(), serial = pi_revoked_serial(); int have, wd;
    *holds = "none";
    ps_lock(); sp = ps_find(id); have = sp != NULL; wd = sp && sp->withdrawn[0] && !sp->anyway; ps_unlock();
    pthread_mutex_lock(&lk);
    login_expire(now);
    if (!run) {
        /* (its own wait for this plugin only: a cancel for another withdrew it; a review, 2026-11-21) */
        if (login.state == 1 && login.kind == W_ANYWAY && !memcmp(login.pub, pub, 32) && !strcmp(anyway_press.id, id)) anyway_withdraw("cancelled");
        r = "cancelled";
    }
    else if (!have) r = "none";
    else if (!wd || serial < 0) r = "running";
    else if (login.state == 1 && login.kind == W_ANYWAY && !memcmp(login.pub, pub, 32) && !strcmp(anyway_press.id, id)) { r = "waiting"; *holds = "anyway"; }
    else if ((h = wait_arm(pub, W_ANYWAY, "run a plugin anyway", now))) { r = "busy"; *holds = h; }
    else {
        snprintf(anyway_press.id, sizeof anyway_press.id, "%s", id); anyway_press.serial = serial;
        r = "waiting"; *holds = "anyway";
        fprintf(stderr, "web: %s, withdrawn by the plugin index, is to run anyway once the action button is pressed\n", id);
    }
    pthread_mutex_unlock(&lk);
    hold_flush(!strcmp(r, "waiting"));
    return r;
}

/* lk held: a wait to trust a key of Sources that no longer counts */
static void source_withdraw(const char *why)
{
    if (!(login.state == 1 && login.kind == W_SOURCE)) return;
    login.state = 3; login.end = now_ms(); login.why = "busy"; wait_over(login.end, 0, NULL);
    fprintf(stderr, "web: the wait to trust a key of Sources withdrawn (%s)\n", why);
    if (hooks->attention) hooks->attention(0);
}

/* Sources (sources.c) for an approved browser: BODY the page's lines, or "cancel".  "applied" (in force: it trusts no
 * more than before), "waiting" (for the button: a key or no check), "busy" (something else waits: *HOLDS says
 * what), "refused" (WHY says why), "cancelled".  WHY may also carry a word beside "applied" */
static const char *source_ask(const uint8_t pub[32], const char *body, const char **holds, char *why, size_t cap)
{
    struct sources from, to; const char *r, *h; long long now = now_ms();
    *holds = "none"; why[0] = 0;
    if (!strncmp(body, "cancel", 6)) {
        pthread_mutex_lock(&lk);
        if (login.state == 1 && login.kind == W_SOURCE && !memcmp(login.pub, pub, 32)) source_withdraw("cancelled");
        pthread_mutex_unlock(&lk);
        return "cancelled";
    }
    sources_get(&from);
    if (sources_parse(body, &from, &to, why, cap)) return "refused";
    if (!sources_widens(&from, &to)) {
        /* trusts no more than before: in force at once.  A wait this browser armed for another choice goes: the page
         * asked for this one since */
        pthread_mutex_lock(&lk);
        login_expire(now);
        if (login.state == 1 && login.kind == W_SOURCE && !memcmp(login.pub, pub, 32)) source_withdraw("another choice asked for");
        pthread_mutex_unlock(&lk);
        if (sources_apply(&from, &to, why, cap)) return "refused";
        update_check();
        return "applied";
    }
    pthread_mutex_lock(&lk);
    login_expire(now);
    if (login.state == 1 && login.kind == W_SOURCE && !memcmp(login.pub, pub, 32) && sources_equal(&source_press, &to) && sources_equal(&source_from, &from)) { r = "waiting"; *holds = "sources"; }
    else {
        if (login.state == 1 && login.kind == W_SOURCE && !memcmp(login.pub, pub, 32)) source_withdraw("another choice asked for");
        if ((h = wait_arm(pub, W_SOURCE, "Sources", now))) { r = "busy"; *holds = h; }
        else {
            source_press = to; source_from = from;
            r = "waiting"; *holds = "sources";
            fprintf(stderr, "web: Sources: a key of the owner's or no signature check waits for the action button\n");
        }
    }
    pthread_mutex_unlock(&lk);
    hold_flush(!strcmp(r, "waiting"));
    return r;
}

/* lk held: one more unsigned ask from ADDR.  0: taken; else in how many ms the next may come.  Eight addresses are
 * counted at a time, the one seen longest ago making room: someone asking from more than that is held by the count
 * of all, as everyone is then */
static int ask_rate(uint32_t addr, long long now)
{
    static long long said; int at = 0, over = 0;
    if (now - asks_all.at >= ASK_WINDOW_MS) { asks_all.at = now; asks_all.n = 0; }
    for (int i = 0; i < 8; i++) {
        if (asks[i].n && asks[i].addr == addr && now - asks[i].at < ASK_WINDOW_MS) { at = i; goto found; }
        if (asks[i].at < asks[at].at) at = i;
    }
    asks[at].addr = addr; asks[at].at = now; asks[at].n = 0;
found:
    if (asks[at].n >= ASK_ADDR) over = (int)(asks[at].at + ASK_WINDOW_MS - now);
    else if (asks_all.n >= ASK_ALL) over = (int)(asks_all.at + ASK_WINDOW_MS - now);
    else { asks[at].n++; asks_all.n++; return 0; }
    if (now - said >= ASK_WINDOW_MS) { said = now; fprintf(stderr, "web: more asks to log in than %d in %d s from one address or %d from all: those beyond are answered and change nothing\n", ASK_ADDR, ASK_WINDOW_MS / 1000, ASK_ALL); }
    return over > 0 ? over : 1;
}

/* An unsigned ask to log in, from ADDR.  "approved", "waiting", or "refused" with *WHY: "timeout" (it ran out),
 * "contested" (another browser asked while this one's login waited, or the other way round), "busy" (something else
 * waits for the button, or did a moment ago), "rate"; *RETRY_MS: when asking again can do better (0: now).  The
 * section above has the rule.  Approved is what clients[] says: the old record said it to a key revoked or pushed
 * out since, and the page showed "Logged in" over 401s.  A login that ended without approval is told once, then
 * the next ask from that key starts anew: the record stayed until another key asked, and the page keeps one key, so
 * its "Try again" was refused at once, with nothing on the ring for the button.  Told only within REFUSED_MS of the
 * end: a page still asking (web/app.js: once a second while it was told "waiting") hears of it, one opened again
 * later would have been told "Not approved" at its first click.  The browser whose login is armed is told "waiting"
 * whatever the count of asks says, and its asks are not counted: the answer changes nothing, three addresses
 * using up the count of all would else tell the owner's page "refused" while the ring shows his login, and counted
 * they let the armed key use the count up itself (the section's "What it costs").  An ask over the count from
 * another key is refused "rate", and still turns an armed login contested */
static void contest(const uint8_t pub[32], long long now)       /* lk held: a login is armed, and another key asked */
{
    /* Not armed any more, for either, and for nobody that is not approved until the first one's minute and the
     * quiet time after it have passed: its owner may be on his way with "waiting" on a page he no longer looks at
     * (the section's comment) */
    (void)now;
    login.state = 4; login.quiet = login.at + LOGIN_MS + QUIET_MS; memcpy(login.last, pub, 32);
    login.reserved = 0; again_ok = 0;
    fprintf(stderr, "web: a second browser asked to log in while one waited for the button: neither is approved by a press now, and no login is taken until %d s have passed without the two asking against each other\n", QUIET_MS / 1000);
    if (hooks->attention) hooks->attention(0);
    if (hooks->approved) hooks->approved(0);
}

static const char *login_ask(const uint8_t pub[32], const char *label, uint32_t addr, const char **why, long long *retry_ms)
{
    const char *r = "refused"; long long now = now_ms(), q; int over, phone;
    *why = ""; *retry_ms = 0;
    pthread_mutex_lock(&lk);
    /* Whether the button is a phone's business: read BEFORE the mark of a phone's wait's end is taken
     * (login_expire).  improv.c leaves that mark before it lowers this, so a 0 here says that no wait had begun,
     * or that the mark of its end is there to be taken now.  Read after, a wait that ended between the two was
     * missed by this ask: no mark yet, 0 already, and a login armed after that wait's end */
    phone = improv_holds();
    login_expire(now);
    if (client_of(pub)) r = "approved";
    else if (login.state == 1 && login.kind == W_LOGIN && !memcmp(login.pub, pub, 32)) r = "waiting";
    else if ((over = ask_rate(addr, now))) { *why = "rate"; *retry_ms = over; if (login.state == 1 && login.kind == W_LOGIN) contest(pub, now); }
    else if (login.state == 3 && login.kind == W_LOGIN && !memcmp(login.pub, pub, 32) && now - login.end <= REFUSED_MS) {
        login.state = 0; *why = login.why ? login.why : "timeout";
        if (strcmp(*why, "timeout") && now < strangers_from) *retry_ms = strangers_from - now;
    }
    /* an approved browser's wait is armed: nothing of it changes, and it is not said what it is for */
    else if (login.state == 1 && login.kind != W_LOGIN) { *why = "busy"; *retry_ms = login.at + LOGIN_MS - now + QUIET_MS; }
    else if (login.state == 1) { contest(pub, now); *why = "contested"; *retry_ms = login.quiet - now; }    /* a second browser while the first one's login waits */
    else if (login.state == 4) {
        /* still two of them: held until QUIET_MS after this ask, if that is later */
        if (memcmp(login.last, pub, 32)) { memcpy(login.last, pub, 32); if ((q = now + QUIET_MS) > login.quiet) login.quiet = q; }
        *why = "contested"; *retry_ms = login.quiet - now;
    }
    else if (now < strangers_from && !(again_ok && !memcmp(again_pub, pub, 32))) { *why = "busy"; *retry_ms = strangers_from - now; }
    else if (phone) { *why = "busy"; *retry_ms = 5000; }                /* the press is a phone's setup's, or was a moment ago (the section's comment) */
    else {
        memset(&login, 0, sizeof login);
        memcpy(login.pub, pub, 32); snprintf(login.label, sizeof login.label, "%s", label); login.at = now; login.state = 1; login.kind = W_LOGIN;
        r = "waiting";
        fprintf(stderr, "web: login waits for the action button (%s)\n", label);
        if (hooks->attention) hooks->attention(1);
    }
    hold_save(now);
    pthread_mutex_unlock(&lk);
    hold_flush(!strcmp(r, "waiting"));  /* (on the flash before the page is told "waiting"; what an ask writes: hold_write's comment) */
    return r;
}

/* ---------------------------------------------------------------- HTTP */

struct req { char method[8], path[256], pub[80], ctr[24], mac[48], ticket[24], origin[8]; char *body; size_t blen; };

static int send_all(int fd, const void *p, size_t n)
{
    const char *c = p;
    while (n) { ssize_t w = send(fd, c, n, MSG_NOSIGNAL); if (w <= 0) return -1; c += w; n -= (size_t)w; }
    return 0;
}

static void respond(int fd, int code, const char *type, const char *extra, const void *body, size_t len)
{
    char h[1024];
    const char *msg = code == 200 ? "OK" : code == 204 ? "No Content" : code == 400 ? "Bad Request" : code == 401 ? "Unauthorized"
                    : code == 404 ? "Not Found" : code == 409 ? "Conflict" : code == 413 ? "Payload Too Large" : code == 503 ? "Service Unavailable" : "Error";
    int n = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\n"
                     "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Headers: X-HM-Pub, X-HM-Ctr, X-HM-Mac, Content-Type\r\n"
                     "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\nAccess-Control-Allow-Private-Network: true\r\n%s\r\n",
                     code, msg, type, len, extra ? extra : "");
    if (send_all(fd, h, (size_t)n) == 0 && len) send_all(fd, body, len);
}

static void respond_json(int fd, int code, const char *json) { respond(fd, code, "application/json", NULL, json, strlen(json)); }

/* The answer to a signed request (r passed signed_ok), signed in turn with the browser's K over its counter */
static void respond_s(int fd, const struct req *r, int code, const char *type, const char *extra, const void *body, size_t len)
{
    uint8_t pub[32], k[32], mac[16]; char x[600], h[33]; crypto_blake2b_ctx ctx; int bad;
    if (unhex(pub, r->pub, 32)) { respond(fd, code, type, extra, body, len); return; }
    pthread_mutex_lock(&lk); bad = session_key(k, pub); pthread_mutex_unlock(&lk);
    if (bad) { respond(fd, code, type, extra, body, len); return; }
    crypto_blake2b_keyed_init(&ctx, 16, k, 32);
    crypto_blake2b_update(&ctx, (const uint8_t *)"RESP\n", 5);
    crypto_blake2b_update(&ctx, (const uint8_t *)r->ctr, strlen(r->ctr)); crypto_blake2b_update(&ctx, (const uint8_t *)"\n", 1);
    if (len) crypto_blake2b_update(&ctx, body, len);
    crypto_blake2b_final(&ctx, mac); crypto_wipe(k, sizeof k);
    hex(h, mac, 16);
    snprintf(x, sizeof x, "X-HM-Mac: %s\r\nAccess-Control-Expose-Headers: X-HM-Mac\r\n%s", h, extra ? extra : "");
    respond(fd, code, type, x, body, len);
}
static void respond_sjson(int fd, const struct req *r, int code, const char *json) { respond_s(fd, r, code, "application/json", NULL, json, strlen(json)); }

/* JSON string body (no quotes) */
static size_t jesc(char *out, size_t cap, const char *s)
{
    size_t n = 0;
    for (; *s && n + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = (char)c; }
        else if (c < 0x20) n += (size_t)snprintf(out + n, cap - n, "\\u%04x", c);
        else out[n++] = (char)c;
    }
    out[n] = 0;
    return n;
}

/* An approved browser's key in X-HM-Pub.  Not yet whether the request is signed: the MAC (signed_ok) is over the body,
 * so nothing short of reading it tells.  The big bodies are read only for these keys, and the requests they carry are
 * all signed ones: else anyone on the LAN could have the Echo hold 1 MB per connection, 8 at once.  The keys travel in
 * the clear (plain HTTP), so this keeps strangers out, not someone who listens on the LAN: that one gets as far as the
 * read, then the 401 */
static int approved_key(const char *hexpub)
{
    uint8_t pub[32]; int ok;
    if (unhex(pub, hexpub, 32)) return 0;
    pthread_mutex_lock(&lk); ok = client_of(pub) != NULL; pthread_mutex_unlock(&lk);
    return ok;
}

static int read_request(int fd, struct req *r)
{
    char hdr[8192]; size_t n = 0; char *end = NULL, *line, *save; long cl = 0;
    while (n < sizeof hdr - 1 && !end) {
        ssize_t k = recv(fd, hdr + n, sizeof hdr - 1 - n, 0);
        if (k <= 0) return -1;
        n += (size_t)k; hdr[n] = 0;
        end = strstr(hdr, "\r\n\r\n");
    }
    if (!end) return -1;
    size_t hl = (size_t)(end - hdr) + 4;
    *end = 0;
    memset(r, 0, sizeof *r);
    if (sscanf(hdr, "%7s %255s", r->method, r->path) != 2) return -1;
    for (line = strtok_r(strchr(hdr, '\n') ? strchr(hdr, '\n') + 1 : hdr + strlen(hdr), "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        char *c = strchr(line, ':'), *v;
        if (!c) continue;
        *c = 0; v = c + 1; while (*v == ' ') v++;
        if (!strcasecmp(line, "Content-Length")) cl = strtol(v, NULL, 10);
        else if (!strcasecmp(line, "X-HM-Pub")) snprintf(r->pub, sizeof r->pub, "%s", v);
        else if (!strcasecmp(line, "X-HM-Ctr")) snprintf(r->ctr, sizeof r->ctr, "%s", v);
        else if (!strcasecmp(line, "X-HM-Ticket")) snprintf(r->ticket, sizeof r->ticket, "%s", v);      /* (a member's GET /api/plugins/revoked) */
        else if (!strcasecmp(line, "X-HM-Mac")) snprintf(r->mac, sizeof r->mac, "%s", v);
    }
    /* a piece of an artifact, a microWakeWord model, a light ring design and a sound of one's own are the big bodies:
     * everything else is a few lines */
    long max = !strncmp(r->path, "/api/artifact/chunk/", 20) || !strncmp(r->path, "/api/plugins/", 13) ? ART_CHUNK_MAX : !strncmp(r->path, "/api/mww/add/", 13) ? MWW_MODEL_MAX + 16384
             : !strncmp(r->path, "/api/ring/design/", 17) || !strcmp(r->path, "/api/ring/live") ? RD_TEXT_MAX
             : !strcmp(r->path, "/api/timer-sound") || !strncmp(r->path, "/api/sound/", 11) || !strncmp(r->path, "/api/sound-pack/", 16) ? SOUND_CUSTOM_MAX : MAX_BODY;
    if (cl < 0 || (cl > MAX_BODY && (cl > max || !approved_key(r->pub)))) return -2;
    if (!(r->body = malloc((size_t)cl + 1))) return -1;
    size_t have = n - hl < (size_t)cl ? n - hl : (size_t)cl;
    memcpy(r->body, hdr + hl, have);
    while (have < (size_t)cl) { ssize_t k = recv(fd, r->body + have, (size_t)cl - have, 0); if (k <= 0) { free(r->body); r->body = NULL; return -1; } have += (size_t)k; }
    r->body[cl] = 0; r->blen = (size_t)cl;
    return 0;
}

/* a counter not taken before: above the floor and not among the last ones taken */
static int ctr_fresh(const struct client *c, unsigned long long ctr)
{
    if (ctr <= c->floor) return 0;
    for (int i = 0; i < c->nseen; i++) if (c->seen[i] == ctr) return 0;
    return 1;
}

/* taken: into seen[], the oldest of a full window out and the floor up to it */
static void ctr_take(struct client *c, unsigned long long ctr)
{
    if (c->nseen < CTR_WINDOW) c->seen[c->nseen++] = ctr;
    else {
        int lo = 0;
        for (int i = 1; i < CTR_WINDOW; i++) if (c->seen[i] < c->seen[lo]) lo = i;
        if (c->seen[lo] > c->floor) c->floor = c->seen[lo];
        c->seen[lo] = ctr;
    }
    if (ctr > c->ctr) c->ctr = ctr;
}

/* 1 if the request is signed by an approved browser with a fresh counter (the highest written to disk when persist) */
static int signed_ok(const struct req *r, int persist)
{
    uint8_t pub[32], mac[16], want[16], k[32]; crypto_blake2b_ctx ctx; char *end; struct client *c; int ok = 0;
    unsigned long long ctr = strtoull(r->ctr, &end, 10);
    if (unhex(pub, r->pub, 32) || unhex(mac, r->mac, 16) || !r->ctr[0] || *end) return 0;
    pthread_mutex_lock(&lk);
    if ((c = client_of(pub)) && ctr_fresh(c, ctr) && !session_key(k, pub)) {
        crypto_blake2b_keyed_init(&ctx, 16, k, 32);
        crypto_blake2b_update(&ctx, (const uint8_t *)r->method, strlen(r->method)); crypto_blake2b_update(&ctx, (const uint8_t *)"\n", 1);
        crypto_blake2b_update(&ctx, (const uint8_t *)r->path, strlen(r->path)); crypto_blake2b_update(&ctx, (const uint8_t *)"\n", 1);
        crypto_blake2b_update(&ctx, (const uint8_t *)r->ctr, strlen(r->ctr)); crypto_blake2b_update(&ctx, (const uint8_t *)"\n", 1);
        crypto_blake2b_update(&ctx, (const uint8_t *)r->body, r->blen);
        crypto_blake2b_final(&ctx, want);
        if (!crypto_verify16(want, mac)) { ok = 1; ctr_take(c, ctr); if (persist) save_clients(); }
        crypto_wipe(k, sizeof k);
    }
    pthread_mutex_unlock(&lk);
    return ok;
}

/* ---------------------------------------------------------------- API */

/* The note as it is kept: one line (control characters as spaces), trimmed, at most NOTE_MAX bytes, without a
 * character left unfinished at its end, whether the limit split it or the body ended inside it (the page cuts too,
 * but anything may post) */
static void note_clean(char *out, const char *s, size_t len)
{
    size_t i = 0, n = 0, k;
    while (i < len && (s[i] == ' ' || (unsigned char)s[i] < 0x20)) i++;
    for (; i < len && n < NOTE_MAX; i++) { unsigned char c = (unsigned char)s[i]; out[n++] = (char)(c < 0x20 || c == 0x7f ? ' ' : c); }
    for (k = n; k && ((unsigned char)out[k - 1] & 0xc0) == 0x80; k--) ;       /* back to the last character's lead byte */
    if (k && (unsigned char)out[k - 1] >= 0xc0) {
        unsigned char lead = (unsigned char)out[k - 1];
        size_t need = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : 2;
        if (n - (k - 1) < need) n = k - 1;
    }
    while (n && out[n - 1] == ' ') n--;
    out[n] = 0;
}

static void note_load(void)
{
    char p[300], t[NOTE_MAX * 2 + 2] = ""; FILE *f;
    snprintf(p, sizeof p, "%s/note", state_dir());
    if ((f = fopen(p, "r"))) { if (!fgets(t, sizeof t, f)) t[0] = 0; fclose(f); }
    pthread_mutex_lock(&note_lk); note_clean(note, t, strlen(t)); pthread_mutex_unlock(&note_lk);
}

/* 0, or -1 when it could not be written (the note in memory stays the old one) */
static int note_save(const char *s, size_t len)
{
    char p[300], t[NOTE_MAX + 2]; int e;
    note_clean(t, s, len);
    snprintf(p, sizeof p, "%s/note", state_dir());
    pthread_mutex_lock(&note_save_lk);         /* the fsync without note_lk: every hello would wait on the flash */
    if (!t[0]) e = unlink(p) && errno != ENOENT ? -1 : 0;
    else { size_t n = strlen(t); t[n] = '\n'; e = statefile_write(p, 0600, t, n + 1); t[n] = 0; }
    if (!e) { pthread_mutex_lock(&note_lk); memcpy(note, t, strlen(t) + 1); pthread_mutex_unlock(&note_lk); }
    pthread_mutex_unlock(&note_save_lk);
    return e;
}

/* members: the Echos of our network (for the login page's "through another Echo"; what beacons say anyway).
 * port: the page's own, which it calls the other Echos on; the URL it was opened at may be a reverse proxy's (80, 443) */
static size_t hello_json(char *o, size_t cap, int members)
{
    char p[65], name[160], node[160], nt[NOTE_MAX * 2 + 8]; size_t n;    /* no control characters in it: 2 bytes a byte at most */
    hex(p, pk, 32); jesc(name, sizeof name, core_name); jesc(node, sizeof node, core_node_name());
    pthread_mutex_lock(&note_lk); jesc(nt, sizeof nt, note); pthread_mutex_unlock(&note_lk);
    n = (size_t)snprintf(o, cap, "{\"name\":\"%s\",\"node\":\"%s\",\"note\":\"%s\",\"model\":\"%s\",\"codename\":\"%s\",\"version\":\"%s\",\"pub\":\"%s\",\"start\":\"%s\",\"port\":%d",
                         name, node, nt, board.model, board.codename, VERSION, p, start_id, core_web_port);
    /* no action button (board.action_combo): the page names what stands in for it */
    if (board.action_combo && n < cap) n += (size_t)snprintf(o + n, cap - n, ",\"action\":\"volume_keys\"");
    if (members && n < cap) { n += (size_t)snprintf(o + n, cap - n, ",\"members\":"); if (n < cap) n += arb_members_json(o + n, cap - n); }
    if (n < cap) n += (size_t)snprintf(o + n, cap - n, "}");
    return n < cap ? n : cap - 1;
}

/* root's kiosk_tick keeps this file while the Kiosk Satellite app is installed: the daemon's user cannot read packages.list */
static int kiosk_app(void)
{
    const char *e = getenv("HASSMIC_KIOSK_APP");
    return board.kiosk && !access(e ? e : "/data/local/hassmic/kiosk-app", F_OK);
}

/* lock order: core_lock first (settings), lk only inside the helpers above */
/* appended at n of o (cap bytes), never past its end: what does not fit is cut (the page then refuses the answer;
 * the parts are bounded, the screen's apps by what fits) */
static size_t app(char *o, size_t n, size_t cap, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static size_t app(char *o, size_t n, size_t cap, const char *fmt, ...)
{
    if (n + 1 >= cap) return cap ? cap - 1 : 0;
    va_list ap; va_start(ap, fmt); int k = vsnprintf(o + n, cap - n, fmt, ap); va_end(ap);
    return k < 0 ? n : n + (size_t)k < cap ? n + (size_t)k : cap - 1;
}

/* An answer that grows with what is written into it, for the one whose length is the owner's doing (how many files
 * he gave his plugins: /api/plugins/sync).  Whole or not at all: past MAX, or without memory, `bad` is set (2, 1)
 * and stays, nothing more is written, and the caller answers an error in place of a JSON cut short */
#define SYNC_ANSWER_MAX (8u << 20)  /* every plugin's every list full is 4.7 MB (16 x 32 lists x 64 items x 144 bytes) */
struct dyn { char *t; size_t n, cap, max; int bad; };
static void dapp(struct dyn *d, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void dapp(struct dyn *d, const char *fmt, ...)
{
    va_list ap; int k; char *t;
    if (d->bad) return;
    va_start(ap, fmt); k = vsnprintf(NULL, 0, fmt, ap); va_end(ap);
    if (k < 0) { d->bad = 1; return; }
    if (d->n + (size_t)k + 1 > d->cap) {
        size_t want = d->cap ? d->cap * 2 : 4096;
        while (want < d->n + (size_t)k + 1) want *= 2;
        if (want > d->max) want = d->max;
        if (want < d->n + (size_t)k + 1) { d->bad = 2; return; }
        if (!(t = realloc(d->t, want))) { d->bad = 1; return; }
        d->t = t; d->cap = want;
    }
    va_start(ap, fmt); vsnprintf(d->t + d->n, d->cap - d->n, fmt, ap); va_end(ap);
    d->n += (size_t)k;
}

static void state_json(int fd, const struct req *r, const uint8_t me[32])
{
    char wake_raw[400], wake_why[520]; int wake_prob = core_wake_problem(wake_raw, sizeof wake_raw);      /* before core_lock: it takes it */
    jesc(wake_why, sizeof wake_why, wake_raw);
    size_t cap = 98304, n = 0;   /* the settings ~25 KB, the screen's apps up to 10, SYN_MAX notes up to 9 */ char *o = malloc(cap), v[300];
    int whisper_refused = davs_refused("whisper");          /* davs.c's lock, never under core_lock */
    if (!o) { respond_json(fd, 503, "{}"); return; }
    n = app(o, n, cap, "{\"device\":");
    n += hello_json(o + n, cap - n, 0);
    pthread_mutex_lock(&core_lock);
    /* volume: the main one, for the page's Volume card to say what each kind of audio plays at now */
    n = app(o, n, cap, ",\"ha\":%s,\"volume\":%d,\"kiosk_app\":%s,\"settings\":[", core_ha_linked() ? "true" : "false", core_volume(), kiosk_app() ? "true" : "false");
    for (int i = 0, k = settings_count(); i < k && n < cap - 600; i++) {
        const struct setting *s = settings_at(i); const char *const *names; int nc = settings_choices(s, &names);
        n = app(o, n, cap, "%s{\"name\":\"%s\",\"label\":\"%s\",\"group\":\"%s\",\"type\":\"%s\",\"value\":%d,\"min\":%d,\"max\":%d,\"export\":%s,\"feature\":%s,\"unit\":\"%s\",\"choices\":[",
                              i ? "," : "", s->name, s->label, s->group, s->type == S_BOOL ? "bool" : s->type == S_INT ? "int" : "choice",
                              settings_get(s), settings_min(s), settings_max(s), s->export_ ? "true" : "false", s->feature ? "true" : "false", s->unit ? s->unit : "");
        for (int c = 0; c < nc && n < cap - 200; c++) { jesc(v, sizeof v, names[c]); n = app(o, n, cap, "%s\"%s\"", c ? "," : "", v); }
        n = app(o, n, cap, "]}");
    }
    /* what does not work, and why.  Models point at the page's Download from Amazon: these said scripts/artifacts.sh
     * after the page could do it, and people who had taken the USB wires off asked how (forum, 2026-10-17) */
    n = app(o, n, cap, "],\"warnings\":[");
    int w = 0;
    /* the chosen wake word engine is a plugin that does not listen: on every page (app.js renderAttention) */
    if (wake_prob) n = app(o, n, cap, "%s\"%s\"", w++ ? "," : "", wake_why);
    if (!core_ha_linked()) n = app(o, n, cap, "%s\"Home Assistant is not connected\"", w++ ? "," : "");
    /* Amazon refuses the whisper model to some models (the Echo Show 5: issue #100): then only a copy brings it */
    if (core_local_wake && core_whispered() == -2)
        n = app(o, n, cap, "%s\"%s\"", w++ ? "," : "", whisper_refused
                   ? "Whisper detection: no model on this Echo. Amazon does not hand it out to this model of Echo: copy it from another Echo that has it with Copy models (Models page)"
                   : "Whisper detection: no model on this Echo yet. Get it with Download from Amazon (Models page), or copy it from another Echo with Copy models");
    if (core_local_wake && sound_model() == SOUND_NONE)
        n = app(o, n, cap, "%s\"Sound detection: no model on this Echo (neither the firmware's nor one from Download from Amazon)\"", w++ ? "," : "");
    if (core_sound_failed())
        n = app(o, n, cap, "%s\"Sound detection could not start: its model did not load, so it was switched off (boot.log says why)\"", w++ ? "," : "");
    /* 5.6: the revocation list a week old (or none) while plugins run; on Overview and the Plugins page */
    { char sw[400], sj[500]; if (pf_revoked_stale(sw, sizeof sw)) { jesc(sj, sizeof sj, sw); n = app(o, n, cap, "%s\"%s\"", w++ ? "," : "", sj); } }
    { const struct core_wake_word *l; if (core_local_wake && core_wake_engine(-1) == WAKE_AMAZON && core_wake_words(&l) <= 1)
        n = app(o, n, cap, "%s\"Wake words: only Alexa. Amazon's others (Echo, Computer, ...) come with Download from Amazon (Models page), or copy them from another Echo\"", w++ ? "," : ""); }
    pthread_mutex_unlock(&core_lock);
    float t = diag_soc_temp(), cpu = diag_cpu();
    /* "button": what a press of the action button would serve now (none, login, adb, plugin), whether this browser
     * armed it, and for how long still: a page that waits for a press shows it, and says so when it is not its own */
    pthread_mutex_lock(&lk);
    login_expire(now_ms());
    int adb_wait = login.state == 1 && login.kind == W_ADB, mine = login.state == 1 && !memcmp(login.pub, me, 32);
    const char *holds = holds_word(); long long left = login.state == 1 ? (login.at + LOGIN_MS - now_ms() + 999) / 1000 : 0;
    pthread_mutex_unlock(&lk);
    n = app(o, n, cap, "],\"adb\":{\"open\":%s,\"waiting\":%s},\"button\":{\"holds\":\"%s\",\"mine\":%s,\"left_s\":%lld},",
            adbwifi_open() ? "true" : "false", adb_wait ? "true" : "false", holds, mine ? "true" : "false", left < 0 ? 0 : left);
    /* where updates and plugins come from (Sources, issue #175): the page's warning on every page while not the project's */
    { char sj[1400]; sources_json(sj, sizeof sj); n = app(o, n, cap, "\"sources\":%s,\"diag\":{\"soc_temp\":", sj); }
    n = isnan(t) ? app(o, n, cap, "null") : app(o, n, cap, "%.1f", t);
    n = app(o, n, cap, ",\"cpu\":");
    n = isnan(cpu) ? app(o, n, cap, "null") : app(o, n, cap, "%.0f", cpu);
    { static const char *const models[] = { "none", "firmware", "newer" };
      n = app(o, n, cap, "},\"sound\":{\"model\":\"%s\",\"failed\":%s", models[sound_model()], core_sound_failed() ? "true" : "false"); }
    /* what "Language you speak" on Automatic stands on: the zone, how it was found (clock.c), the language it gives */
    { char z[100], zj[200], cc[3][3], l[16]; const char *how; clock_zone_info(z, sizeof z, &how, cc); core_speech_language(l, sizeof l);
      jesc(zj, sizeof zj, z);            /* the countries: two capitals each or "" (clock.c cc_take) */
      n = app(o, n, cap, "},\"zone\":{\"zone\":\"%s\",\"how\":\"%s\",\"country\":\"%s\",\"home\":\"%s\",\"wifi\":\"%s\",\"language\":\"%s\"",
              zj, how, cc[0], cc[1], cc[2], l); }
    /* the timer sounds this Echo has (the stock ones where its firmware keeps them), and the length of its own */
    { static const enum sound snd[] = { SND_TIMER, SND_ALARM, SND_CUSTOM }; static const char *const nm[] = { "timer", "alarm", "custom" };
      n = app(o, n, cap, "},\"timer_sound\":{\"custom_ms\":%u,\"present\":[\"beep\"", sound_own_ms(SND_CUSTOM));
      for (int i = 0; i < 3; i++) if (sound_present(snd[i])) n = app(o, n, cap, ",\"%s\"", nm[i]);
      n = app(o, n, cap, "]"); }
    /* the sounds one's own can stand in for: its length (0: stock's plays), whether stock's is on this image, the longest */
    n = app(o, n, cap, "},\"sounds\":[");
    for (int s = 0, k = 0; s < SND_COUNT; s++) if (sound_id(s))
        n = app(o, n, cap, "%s{\"id\":\"%s\",\"own_ms\":%u,\"stock\":%s,\"max_s\":%u}", k++ ? "," : "", sound_id(s),
                              sound_own_ms(s), sound_stock_present(s) ? "true" : "false", sound_own_secs(s));
    n = app(o, n, cap, "],\"arbitration\":");
    if (arb_running()) n += arb_status_json(o + n, cap - n); else n = app(o, n, cap, "null");
    if (dropin_running()) { char peer[64]; int st = dropin_status(peer, sizeof peer); unsigned at; int res = dropin_last(&at);
        n = app(o, n, cap, ",\"dropin\":{\"state\":\"%s\",\"peer\":\"%s\",\"result\":\"%s\",\"attempt\":%u}",
                              dropin_states[st], peer, res >= 0 ? dropin_results[res] : "", at); }   /* peer: a node, [a-z0-9-] */
    n = app(o, n, cap, ",\"wifi\":"); n += wifi_current_json(o + n, cap - n);
    if (screen_present()) {                                       /* the screen: its home app, its music pages */
        n = app(o, n, cap, ",\"screen\":"); n += screen_status_json(o + n, cap - n);
        n = app(o, n, cap, ",\"mass\":"); n += mass_status_json(o + n, cap - n);
    }
    /* plugins that came here from another Echo without this Echo's button, until dismissed (5.4: said where the
     * owner sees it without looking for it: the page shows them on every page) */
    n = app(o, n, cap, ",\"plugins_synced\":"); n += syn_json(o + n, cap - n, me);
    n = app(o, n, cap, ",\"clients\":[");
    pthread_mutex_lock(&lk);
    for (int i = 0; i < nclients && n < cap - 300; i++) {
        char h[65]; hex(h, clients[i].pub, 32); jesc(v, sizeof v, clients[i].label);
        n = app(o, n, cap, "%s{\"pub\":\"%s\",\"label\":\"%s\",\"me\":%s,\"vouched\":%s}", i ? "," : "", h, v, memcmp(clients[i].pub, me, 32) ? "false" : "true", clients[i].button ? "false" : "true");
    }
    pthread_mutex_unlock(&lk);
    n = app(o, n, cap, "]}");
    respond_s(fd, r, 200, "application/json", NULL, o, n < cap ? n : cap);
    free(o);
}

/* /api/artifacts                          GET   the list (artifacts.c)
 * /api/artifact/read/<id>/<file>/<off>/<len>  GET   a piece
 * /api/artifact/begin                      POST  "<id> <digest>\n<file> <size>\n..."
 * /api/artifact/chunk/<id>/<file>/<off>    POST  a piece (the body, raw)
 * /api/artifact/commit/<id>                POST
 * /api/artifact/install                    POST  root installs what is ready; hassmic restarts
 * All signed; reads and pieces do not write the counter to disk (a replayed piece lands in a staged copy whose digest
 * then fails) */
/* a file name out of the path: "sub/file" travels as "sub%2Ffile" (the page's encodeURIComponent), so the one escape
 * is undone; artifacts.c checks the name */
static void unslash(char *s)
{
    char *w = s;
    for (; *s; s++) {
        if (s[0] == '%' && s[1] == '2' && (s[2] == 'F' || s[2] == 'f')) { *w++ = '/'; s += 2; }
        else *w++ = *s;
    }
    *w = 0;
}

static void artifact_api(int fd, struct req *r)
{
    char err[200], o[300], ej[260], id[80] = "", file[160] = ""; long a = 0, b = 0; int rc = -1, write = strcmp(r->method, "GET");
    int piece = !strncmp(r->path, "/api/artifact/read/", 19) || !strncmp(r->path, "/api/artifact/chunk/", 20);
    if (!signed_ok(r, write && !piece)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); return; }
    err[0] = 0;
    if (!write && !strcmp(r->path, "/api/artifacts")) {
        size_t cap = 262144; char *t = malloc(cap);           /* en-US wake word sets list ~4.5 KB each (69 files) */
        if (!t) { respond_sjson(fd, r, 503, "{\"error\":\"no memory\"}"); return; }
        size_t n = art_list_json(t, cap);
        respond_s(fd, r, 200, "application/json", NULL, t, n); free(t); return;
    }
    if (!write && sscanf(r->path, "/api/artifact/read/%79[^/]/%159[^/]/%ld/%ld", id, file, &a, &b) == 4) {
        void *data; size_t n;
        unslash(file);
        if (!art_read(id, file, a, b, &data, &n, err, sizeof err)) { respond_s(fd, r, 200, "application/octet-stream", NULL, data, n); free(data); return; }
    }
    else if (write && !strcmp(r->path, "/api/artifact/begin")) rc = art_begin(r->body, err, sizeof err);
    else if (write && sscanf(r->path, "/api/artifact/chunk/%79[^/]/%159[^/]/%ld", id, file, &a) == 3) { unslash(file); rc = art_chunk(id, file, a, r->body, r->blen, err, sizeof err); }
    else if (write && sscanf(r->path, "/api/artifact/commit/%79[^/]", id) == 1) rc = art_commit(id, err, sizeof err);
    else if (write && !strcmp(r->path, "/api/artifact/install")) rc = art_install(err, sizeof err);
    else snprintf(err, sizeof err, "not found");
    if (!rc) { respond_sjson(fd, r, 200, "{\"ok\":true}"); return; }
    jesc(ej, sizeof ej, err); snprintf(o, sizeof o, "{\"error\":\"%s\"}", ej);
    respond_sjson(fd, r, 400, o);
}

/* /api/timer-sound       POST  a WAV, 16-bit mono at 48 kHz, up to 30 s (sounds.h): this Echo's own timer sound, which
 *                               the ring then plays; an empty body deletes it (the ring back to the default if it played it)
 * /api/timer-sound/play  POST  one ring of the chosen sound, 10 s at most, at the timer volume
 * Signed. */
static void timer_sound_api(int fd, struct req *r)
{
    char err[200] = "", o[300], ej[260]; int rc = -1;
    if (!signed_ok(r, 1)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); return; }
    if (strcmp(r->method, "POST")) snprintf(err, sizeof err, "not found");
    else if (!strcmp(r->path, "/api/timer-sound/play")) { core_timer_sound_preview(); rc = 0; }
    else if (strcmp(r->path, "/api/timer-sound")) snprintf(err, sizeof err, "not found");
    else if (r->blen) rc = sound_own_save(SND_CUSTOM, r->body, r->blen, err, sizeof err);
    else {
        pthread_mutex_lock(&core_lock);
        int chosen = core_timer_sound(-1) == TIMER_SOUND_CUSTOM;
        if ((rc = sound_own_delete(SND_CUSTOM))) snprintf(err, sizeof err, "cannot delete it");     /* it stays, and stays chosen */
        else if (chosen) { core_timer_sound_default(); settings_save(); core_settings_changed(); }
        pthread_mutex_unlock(&core_lock);
    }
    if (!rc) { respond_sjson(fd, r, 200, "{\"ok\":true}"); return; }
    jesc(ej, sizeof ej, err); snprintf(o, sizeof o, "{\"error\":\"%s\"}", ej);
    respond_sjson(fd, r, 400, o);
}

/* /api/sound/<id>        POST  a WAV as the timer's (sounds.h), up to the sound's max_s: played in place of stock's from
 *                               then on; an empty body deletes it (stock's again)
 * /api/sound/<id>/play   POST  it once, as it plays now (one's own or stock's), at the sounds volume
 * /api/sound/<id>/stock  GET   stock's as a WAV (16-bit mono at its own rate), for the page to measure its loudness
 * <id>: sounds.c's ("wake", "ring_in", ...).  Signed. */
static void sound_api(int fd, struct req *r)
{
    char err[200] = "", o[300], ej[260], id[32]; int rc = -1;
    if (!signed_ok(r, 1)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); return; }
    const char *p = r->path + 11, *slash = strchr(p, '/');
    size_t l = slash ? (size_t)(slash - p) : strlen(p);
    snprintf(id, sizeof id, "%.*s", (int)(l < sizeof id ? l : sizeof id - 1), p);
    int s = sound_by_id(id); const char *what = slash ? slash + 1 : "";
    if (s < 0) snprintf(err, sizeof err, "no such sound");
    else if (!strcmp(r->method, "GET") && !strcmp(what, "stock")) {
        size_t n; unsigned rate; short *pcm = sound_stock(s, &n, &rate);
        if (!pcm) { respond_sjson(fd, r, 404, "{\"error\":\"not on this firmware\"}"); return; }
        unsigned char *w = malloc(44 + n * 2);
        if (!w) { free(pcm); respond_sjson(fd, r, 500, "{\"error\":\"out of memory\"}"); return; }
        uint32_t v[] = { 36 + (uint32_t)n * 2, 16, 1 | 1u << 16, rate, rate * 2, 2 | 16u << 16, (uint32_t)n * 2 };
        memcpy(w, "RIFF", 4); memcpy(w + 4, &v[0], 4); memcpy(w + 8, "WAVEfmt ", 8); memcpy(w + 16, &v[1], 20);
        memcpy(w + 36, "data", 4); memcpy(w + 40, &v[6], 4); memcpy(w + 44, pcm, n * 2);     /* little-endian on every Echo and PC */
        respond_s(fd, r, 200, "audio/wav", NULL, w, 44 + n * 2);
        free(w); free(pcm); return;
    }
    else if (strcmp(r->method, "POST")) snprintf(err, sizeof err, "not found");
    else if (!strcmp(what, "play")) { core_sound_preview(s); rc = 0; }
    else if (*what) snprintf(err, sizeof err, "not found");
    else if (r->blen) rc = sound_own_save(s, r->body, r->blen, err, sizeof err);
    else if ((rc = sound_own_delete(s))) snprintf(err, sizeof err, "cannot delete it");
    if (!rc) { respond_sjson(fd, r, 200, "{\"ok\":true}"); return; }
    jesc(ej, sizeof ej, err); snprintf(o, sizeof o, "{\"error\":\"%s\"}", ej);
    respond_sjson(fd, r, 400, o);
}

/* /api/sound-pack                 GET   {"packs":[{id,name,bytes,sounds}],"bytes","max","packs_max"}
 * /api/sound-pack/new             POST  the name: an empty pack, {"id"}
 * /api/sound-pack/save            POST  the name: one of the sounds of one's own in use now, {"id"}
 * /api/sound-pack/<id>/file/<sound>  POST  a WAV as /api/sound/<sound> takes it; GET  it, to save
 * /api/sound-pack/<id>/apply      POST  its sounds in use, Alexa's where it has none
 * /api/sound-pack/<id>/delete     POST
 * Signed.  Not in exports, as the sounds. */
static void sound_pack_api(int fd, struct req *r)
{
    char err[200] = "", o[300], ej[260]; int rc = -1, get = !strcmp(r->method, "GET");
    if (!signed_ok(r, 1)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); return; }
    const char *p = r->path + 15;
    if (!*p && get) {
        size_t cap = 16384, n = 0; char *j = malloc(cap); int k = 0;
        if (!j) { respond_sjson(fd, r, 500, "{\"error\":\"out of memory\"}"); return; }
        n += (size_t)snprintf(j + n, cap - n, "{\"packs\":[");
        for (int id = 1; id <= SP_PACKS_MAX; id++) {
            char name[SP_NAME_MAX + 1], q[400]; size_t b; unsigned s; int c = 0;
            if (soundpack_info(id, name, &b, &s)) continue;
            jesc(q, sizeof q, name);
            n += (size_t)snprintf(j + n, cap - n, "%s{\"id\":%d,\"name\":\"%s\",\"bytes\":%zu,\"sounds\":[", k++ ? "," : "", id, q, b);
            for (int i = 0; i < SND_COUNT; i++) if (s >> i & 1) n += (size_t)snprintf(j + n, cap - n, "%s\"%s\"", c++ ? "," : "", sound_id(i));
            n += (size_t)snprintf(j + n, cap - n, "]}");
        }
        n += (size_t)snprintf(j + n, cap - n, "],\"bytes\":%zu,\"max\":%zu,\"packs_max\":%d}", soundpack_bytes(), soundpack_max(), SP_PACKS_MAX);
        respond_s(fd, r, 200, "application/json", NULL, j, n);
        free(j); return;
    }
    if (!strcmp(p, "/new") || !strcmp(p, "/save")) {
        int id = get ? -1 : soundpack_new(r->body ? r->body : "", r->blen, p[1] == 's', err, sizeof err);
        if (id > 0) { snprintf(o, sizeof o, "{\"id\":%d}", id); respond_sjson(fd, r, 200, o); return; }
    } else {
        char *end; long id = *p == '/' ? strtol(p + 1, &end, 10) : 0;
        if (id <= 0 || id > SP_PACKS_MAX || *end != '/') snprintf(err, sizeof err, "no such pack");
        else if (!strncmp(end, "/file/", 6)) {
            int s = sound_by_id(end + 6);
            if (get) {
                size_t n; unsigned char *w = soundpack_get((int)id, s, &n);
                if (!w) { respond_sjson(fd, r, 404, "{\"error\":\"not in this pack\"}"); return; }
                respond_s(fd, r, 200, "audio/wav", NULL, w, n);
                free(w); return;
            }
            rc = soundpack_put((int)id, s, r->body, r->blen, err, sizeof err);
        }
        else if (get) snprintf(err, sizeof err, "not found");
        else if (!strcmp(end, "/apply")) rc = soundpack_apply((int)id, err, sizeof err);
        else if (!strcmp(end, "/delete")) { if ((rc = soundpack_delete((int)id))) snprintf(err, sizeof err, "no such pack"); }
        else snprintf(err, sizeof err, "not found");
    }
    if (!rc) { respond_sjson(fd, r, 200, "{\"ok\":true}"); return; }
    jesc(ej, sizeof ej, err[0] ? err : "not found"); snprintf(o, sizeof o, "{\"error\":\"%s\"}", ej);
    respond_sjson(fd, r, 400, o);
}

/* ---- the move from the built-in microWakeWord engine (deprecated) to the plugin (docs/plugins.md 5.9, as built).
 * The owner's own act, from the Wake word page: the page installs the plugin first, through the plugin index and
 * the action button as any install (nothing here installs anything), then asks for this.  Each model of the built-in
 * engine is COPIED into the plugin's own list "models" (state/plugins/microwakeword/files/models: plugin_store.c
 * list_dir) through the store's item path, as the page's upload of an item is: its files staged, its threshold and
 * window as the owner set them on the built-in engine (its manifest.json, which mww_edit writes) staged beside them
 * as the item's fields, brought into the plugin's range where they are outside it (and said), then the plugin's own
 * check (FILE_CHECK); kept only when the plugin looked at it and took it, else refused with its reason and nothing
 * kept.  state/mww is not touched: the built-in engine keeps its models, tuned as they were, and switching back is
 * the engine setting alone.
 *
 * The engine: switched only where the built-in microWakeWord is the engine (the move is from it): the words in its
 * slots (state/mww_word) become the plugin's (state/wake_word-microwakeword), then the setting.  Where another
 * engine listens (Amazon's, another wake plugin, the plugin already), the models are copied and nothing else
 * changes; "switch" is the separate step the page offers, worded as what it is (the engine becomes the plugin,
 * listening for those words).  The engine is the last thing changed: anything that fails before it leaves it as it
 * was.  Only the official microWakeWord (the index's listing) unless the request says "any": the page asks the
 * owner first, naming the plugin's author and tier (a plugin that calls itself microwakeword is not the project's
 * by its id). */
static int mww_field(const struct ps_plugin *sp, const char *name, double v, int whole, char *out, size_t cap, char *note, size_t ncap, const char *what)
{
    const struct ps_field *f = ps_item_field_of(sp, "models", name); double c = v; char t[32]; int past;
    out[0] = 0;
    if (!f) return 0;                                           /* (a version without it: the plugin's own default) */
    /* Within the range first, at the precision the built-in engine keeps it with (a float: 0.99f is 0.99000001 as a
     * double, the plugin's own top and not past it; its page's own top was called outside the range: a review), then
     * written at the field's two decimals.  Rounded first, 0.995 became 1.00 and the note named that, a value the
     * owner never set (a review of 913ec8d); the note names his value and the one written */
    past = v < f->min - 1e-6 || v > f->max + 1e-6;
    if (c < f->min) c = f->min;
    if (c > f->max) c = f->max;
    if (whole) snprintf(out, cap, "%ld", (long)(c + 0.5)); else snprintf(out, cap, "%.2f", c);
    if (past) {
        size_t l = strlen(note);
        if (whole) snprintf(t, sizeof t, "%.0f", v);
        else { snprintf(t, sizeof t, "%.2f", v); if (fabs(strtod(t, NULL) - v) > 1e-6) snprintf(t, sizeof t, "%.6g", v); }
        snprintf(note + l, ncap - l, "%sits %s %s is outside what the plugin takes (%g to %g): copied with %s", l ? "; " : "", what, t, f->min, f->max, out);
    }
    return 1;
}

/* (KEPT: the plugin had the model with a threshold or window of its own, which stays) */
static int mww_to_plugin(const struct mww_info *m, char *why, size_t cap, char *note, size_t ncap, int *kept)
{
    static const char *const f[2][2] = { { "model.tflite", ".tflite" }, { "manifest.json", ".json" } };
    static struct ps_item it[PS_ITEMS];         /* (mww_migrate's one at a time) */
    const struct ps_plugin *sp; char p[400], vc[32] = "", vw[32] = ""; int rc, k, hc = 0, hw = 0;
    note[0] = 0; *kept = 0;
    /* the fields first, before anything is written: brought into what the plugin takes (its manifest's range; the
     * built-in engine took any threshold between 0 and 1), and said.  Not over what the plugin has: a model it has
     * already (a move made before, "Copy the models again") keeps the threshold and window it has, which the owner
     * may have tuned on the plugin's card since (two stores: 5.9), and only its files are replaced (ps_item_keep
     * keeps the fields of the item it replaces where none are staged).  Before, a copy again put the built-in
     * engine's values over them, unsaid (a review, 2026-11-21) */
    ps_lock();
    if (!(sp = ps_find("microwakeword"))) rc = -1;
    else {
        for (int i = 0, n = ps_items(sp, "models", it, PS_ITEMS); i < n; i++) if (!strcmp(it[i].id, m->id) && it[i].nfields) *kept = 1;
        if (!*kept) {
            hc = mww_field(sp, "cutoff", m->cutoff, 0, vc, sizeof vc, note, ncap, "threshold");
            hw = mww_field(sp, "window", m->window, 1, vw, sizeof vw, note, ncap, "window");
        }
        rc = ps_item_begin(sp, "models", m->id, why, cap);
    }
    ps_unlock();
    if (rc) { if (!why[0]) snprintf(why, cap, "the plugin is not installed here"); return -1; }
    for (int i = 0; i < 2 && !rc; i++) {
        FILE *fl; long len; unsigned char *d = NULL;
        snprintf(p, sizeof p, "%s/%s/%s", mww_dir(), m->id, f[i][0]);
        if (!(fl = ps_fopen(p))) { snprintf(why, cap, "cannot read its %s", f[i][0]); rc = -1; break; }
        fseek(fl, 0, SEEK_END); len = ftell(fl); rewind(fl);
        if (len < 0 || len > MWW_MODEL_MAX || !(d = malloc((size_t)len + 1)) || fread(d, 1, (size_t)len, fl) != (size_t)len) { snprintf(why, cap, "cannot read its %s", f[i][0]); rc = -1; }
        fclose(fl);
        if (!rc) { ps_lock(); rc = (sp = ps_find("microwakeword")) ? ps_item_chunk(sp, "models", m->id, f[i][1], 0, d, (size_t)len, why, cap) : -1; ps_unlock(); }
        free(d);
    }
    if (!rc) {
        ps_lock();
        rc = !(sp = ps_find("microwakeword"));
        if (!rc && hc) rc = ps_item_field_staged(sp, "models", m->id, "cutoff", vc, why, cap);
        if (!rc && hw) rc = ps_item_field_staged(sp, "models", m->id, "window", vw, why, cap);
        ps_unlock();
    }
    if (rc) { ps_lock(); if ((sp = ps_find("microwakeword"))) ps_item_drop(sp, "models", m->id); ps_unlock(); return -1; }
    /* kept only where the plugin looked at it and took it: a check that did not happen (the plugin stopped) keeps
     * nothing, so what the answer calls copied is what the plugin has */
    if ((k = plugin_item_checked_only("microwakeword", "models", m->id, why, cap)) != 1) return -1;
    return 0;
}

/* the words in the built-in engine's slots, as it saved them (state/mww_word), that the plugin has in its list
 * now; or the first of its list where none: into ACT, their count */
static int mww_words_for_plugin(char (*act)[64], int max)
{
    char path[400], line[80]; FILE *f; int na = 0, n; const struct ps_plugin *sp; static struct ps_item it[PS_ITEMS];
    ps_lock(); n = (sp = ps_find("microwakeword")) ? ps_items(sp, "models", it, PS_ITEMS) : 0; ps_unlock();
    snprintf(path, sizeof path, "%s/mww_word", state_dir());
    if ((f = ps_fopen(path))) {
        while (na < max && fgets(line, sizeof line, f)) {
            int have = 0;
            line[strcspn(line, "\r\n")] = 0;
            for (int i = 0; i < n; i++) have |= it[i].checked && !strcmp(it[i].id, line);
            for (int a = 0; a < na; a++) have &= strcmp(act[a], line) != 0;
            if (line[0] && have) snprintf(act[na++], sizeof act[0], "%s", line);
        }
        fclose(f);
    }
    for (int i = 0; !na && i < n; i++) if (it[i].checked) snprintf(act[na++], sizeof act[0], "%s", it[i].id);
    return na;
}

static void mww_migrate(int fd, struct req *r)
{
    static pthread_mutex_t one = PTHREAD_MUTEX_INITIALIZER; static struct mww_info l[MWW_MODELS_MAX]; static struct plugin_word words[16];
    static struct { char id[48], why[200]; } bad[MWW_MODELS_MAX], adj[MWW_MODELS_MAX]; static char kept[MWW_MODELS_MAX][48];
    char err[400] = "", w[200], act[WAKE_SLOTS][64], ej[500], body[64] = "", tiername[40] = "", author[49] = "";
    int n = 0, nbad = 0, nadj = 0, nkept = 0, na = 0, ok, st, eng, sw = 0, any, tier = PS_SIDELOAD, switched = 0, copy;
    const struct ps_plugin *sp; size_t cap = 8192, on = 0; char *o;
    if (pthread_mutex_trylock(&one)) { respond_sjson(fd, r, 409, "{\"error\":\"the models are being moved already\"}"); return; }
    snprintf(body, sizeof body, "%.*s", (int)(r->blen < sizeof body - 1 ? r->blen : sizeof body - 1), r->body ? r->body : "");
    /* "go" (the move: the models copied, the engine switched where it was the built-in one), "switch" (the separate
     * step: the engine the plugin, nothing copied), each with "any": the owner said yes to a plugin that is not the
     * official one */
    copy = strncmp(body, "switch", 6) != 0; any = strstr(body, " any") != NULL;
    pthread_mutex_lock(&core_lock); eng = core_wake_engine(-1); pthread_mutex_unlock(&core_lock);
    sw = !copy || eng == WAKE_MWW;
    st = plugin_wake_state("microwakeword");
    ps_lock(); sp = ps_find("microwakeword"); ok = sp && sp->here && sp->accepted && !sp->refused[0] && (sp->m.caps & PM_CAPBIT(PM_CAP_WAKE));
    if (sp) { tier = sp->tier; snprintf(author, sizeof author, "%s", sp->m.author); } ps_unlock();
    snprintf(tiername, sizeof tiername, "%s", tier == PS_OFFICIAL ? "official" : tier == PS_COMMUNITY ? "community" : "a file, not listed");
    /* "switch" to the plugin that listens already would rewrite its own pick with the built-in engine's words, which
     * the owner may have changed on its card since (a review, 2026-11-21): nothing to switch, said */
    { char wid[PS_ID_MAX]; if (!copy && core_wake_plugin(wid, sizeof wid) && !strcmp(wid, "microwakeword")) snprintf(err, sizeof err, "the microWakeWord plugin is the wake word engine already: nothing switched (its wake words are chosen on its card, or in Home Assistant)"); }
    if (err[0]) { /* (said) */ }
    else if (!ok) snprintf(err, sizeof err, "the microWakeWord plugin is not installed here: install it first (the Wake word page's move does, with the action button)");
    else if (tier != PS_OFFICIAL && !any) snprintf(err, sizeof err, "the plugin installed here as microwakeword is not the official microWakeWord (by %s; %s): your models are copied to it only when you say so on the page", author, tiername);
    else if (st != PLUGIN_WAKE_UP) { plugin_wake_why("microwakeword", w, sizeof w); snprintf(err, sizeof err, "the microWakeWord plugin does not run: %s", w[0] ? w : plugin_why()[0] ? plugin_why() : "it has not started yet"); }
    else if (copy && !(n = mww_list(l, MWW_MODELS_MAX))) snprintf(err, sizeof err, "the built-in engine has no model to copy");
    for (int i = 0; copy && i < n && !err[0]; i++) {
        char why[200] = "", note[200] = ""; int kp = 0;
        if (mww_to_plugin(&l[i], why, sizeof why, note, sizeof note, &kp)) {
            snprintf(bad[nbad].id, sizeof bad[0].id, "%s", l[i].id); snprintf(bad[nbad++].why, sizeof bad[0].why, "%s", why[0] ? why : "it cannot be used");
            log_private("wake word: microWakeWord model %s not copied to the plugin: %s", l[i].id, why);
        } else if (note[0]) {
            snprintf(adj[nadj].id, sizeof adj[0].id, "%s", l[i].id); snprintf(adj[nadj++].why, sizeof adj[0].why, "%s", note);
            log_private("wake word: microWakeWord model %s copied to the plugin: %s", l[i].id, note);
        } else if (kp) snprintf(kept[nkept++], sizeof kept[0], "%s", l[i].id);
    }
    if (copy && n && !err[0]) {
        plugin_files_changed("microwakeword", "models");
        fprintf(stderr, "wake word: the built-in microWakeWord engine's %d model%s copied to the plugin (%d not); state/mww left as it was\n", n - nbad, n - nbad == 1 ? "" : "s", nbad);
    }
    if (sw && !err[0]) {
        /* the word the built-in engine listens for now must be one the plugin took, or the switch would change what
         * the Echo answers to: then nothing switches (a second word it did not take is left out) */
        if (copy && eng == WAKE_MWW) {
            char path[400], line[80]; FILE *f; snprintf(path, sizeof path, "%s/mww_word", state_dir());
            if ((f = ps_fopen(path))) {
                if (fgets(line, sizeof line, f)) {
                    line[strcspn(line, "\r\n")] = 0;
                    for (int i = 0; i < nbad; i++) if (!strcmp(bad[i].id, line)) snprintf(err, sizeof err, "the plugin did not take %s, the wake word the built-in engine listens for: %s", line, bad[i].why);
                }
                fclose(f);
            }
        }
        if (!err[0] && !(na = mww_words_for_plugin(act, WAKE_SLOTS))) snprintf(err, sizeof err, copy ? "the plugin took none of the models" : "the plugin has no model checked to listen for: copy the models first");
    }
    /* copied while another engine listens: the words a switch would take, for the page's separate step */
    if (!sw && !err[0]) na = mww_words_for_plugin(act, WAKE_SLOTS);
    /* the plugin lists them once it read its list again: a word it has not listed would not listen */
    for (int t = 0; sw && na && !err[0] && t < 160; t++) {
        int k = plugin_wake_words("microwakeword", words, 16), all = 1;
        for (int a = 0; a < na; a++) { int in = 0; for (int i = 0; i < k; i++) in |= !strcmp(words[i].id, act[a]); all &= in; }
        if (all) break;
        if (t == 159) snprintf(err, sizeof err, "the plugin has not listed %s as a wake word it listens for", act[0]);
        usleep(50000);
    }
    if (sw && na && !err[0]) {
        pthread_mutex_lock(&core_lock);
        if (!core_local_wake) snprintf(err, sizeof err, "hassmic.conf starts the satellite with -w remote: the wake word is Home Assistant's");
        else if (core_wake_switch("plugin:microwakeword", act, na, err, sizeof err) && !err[0]) snprintf(err, sizeof err, "the engine did not switch");
        else switched = 1;
        eng = core_wake_engine(-1);
        pthread_mutex_unlock(&core_lock);
        if (switched) log_private("wake word: the engine is the microWakeWord plugin now, listening for %s%s%s", act[0], na > 1 ? " and " : "", na > 1 ? act[1] : "");
    }
    if (!(o = malloc(cap))) { pthread_mutex_unlock(&one); respond_sjson(fd, r, 503, "{\"error\":\"no memory\"}"); return; }
    { const char *const *names; int ne; pthread_mutex_lock(&core_lock); ne = core_wake_engines(&names); jesc(w, sizeof w, eng >= 0 && eng < ne ? names[eng] : ""); pthread_mutex_unlock(&core_lock); }
    jesc(ej, sizeof ej, err);
    on += (size_t)snprintf(o + on, cap - on, "{%s%s%s\"models\":%d,\"copied\":%d,\"switched\":%s,\"engine\":\"%s\",\"tier\":\"%s\",\"words\":[", err[0] ? "\"error\":\"" : "", ej, err[0] ? "\"," : "",
                           n, n - nbad, switched ? "true" : "false", w, tier == PS_OFFICIAL ? "official" : tier == PS_COMMUNITY ? "community" : "sideload");
    for (int a = 0; switched && a < na; a++) on += (size_t)snprintf(o + on, cap - on, "%s\"%s\"", a ? "," : "", act[a]);
    on += (size_t)snprintf(o + on, cap - on, "],\"offer\":[");
    for (int a = 0; !sw && !err[0] && a < na; a++) on += (size_t)snprintf(o + on, cap - on, "%s\"%s\"", a ? "," : "", act[a]);
    on += (size_t)snprintf(o + on, cap - on, "],\"refused\":[");
    for (int i = 0; i < nbad && on < cap - 600; i++) { jesc(ej, sizeof ej, bad[i].why); on += (size_t)snprintf(o + on, cap - on, "%s{\"id\":\"%s\",\"why\":\"%s\"}", i ? "," : "", bad[i].id, ej); }
    on += (size_t)snprintf(o + on, cap - on, "],\"adjusted\":[");
    for (int i = 0; i < nadj && on < cap - 600; i++) { jesc(ej, sizeof ej, adj[i].why); on += (size_t)snprintf(o + on, cap - on, "%s{\"id\":\"%s\",\"why\":\"%s\"}", i ? "," : "", adj[i].id, ej); }
    on += (size_t)snprintf(o + on, cap - on, "],\"kept\":[");
    for (int i = 0; i < nkept && on < cap - 100; i++) on += (size_t)snprintf(o + on, cap - on, "%s\"%s\"", i ? "," : "", kept[i]);
    on += (size_t)snprintf(o + on, cap - on, "]}");
    pthread_mutex_unlock(&one);
    respond_s(fd, r, 200, "application/json", NULL, o, on);     /* (200 with "error": its answer is signed, the page reads what was copied) */
    free(o);
}

/* /api/mww                              GET   {"engine","active","second","models":[...]}: microWakeWord's models (mww_store.c)
 * /api/mww/add/<id>                     POST  "<manifest length>\n<manifest JSON><model.tflite>": a new model, or a new
 *                                             version of <id> (length 0: no manifest, defaults)
 * /api/mww/edit/<id>                    POST  "name=..", "cutoff=0.85", "window=5" lines
 * /api/mww/delete/<id>                  POST
 * /api/mww/use/<id>                     POST  the active wake word, then the only one (with microWakeWord on)
 * /api/mww/file/<id>/<manifest.json|model.tflite>  GET  the file, to save it
 * /api/mww/migrate                      POST  "go [any]": the models copied to the microWakeWord plugin, the engine to it
 *                                             where the built-in one was the engine; "switch [any]": the engine to it,
 *                                             nothing copied; "any": a plugin not the official one (mww_migrate)
 *                                             -> {"models","copied","switched","engine","tier","words":[...] (switched to),
 *                                             "offer":[...] (what "switch" would listen for, copied beside another
 *                                             engine), "refused":[{"id","why"}],"adjusted":[{"id","why"}],"kept":[ids
 *                                             the plugin had: their files replaced, its threshold and window kept]},
 *                                             "error" where it stopped: the engine as it was; "switch" while the plugin
 *                                             is the engine already: refused, nothing changed
 * All signed. */
static void mww_api(int fd, struct req *r)
{
    char err[200] = "", o[300], ej[260], id[64] = "", file[24] = ""; int rc = -1, write = !strcmp(r->method, "POST");
    if (!signed_ok(r, write)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); return; }
    if (write && !strcmp(r->path, "/api/mww/migrate")) { mww_migrate(fd, r); return; }
    if (!write && !strcmp(r->path, "/api/mww")) {
        size_t cap = 16384, n = 0; char *t = malloc(cap);
        if (!t) { respond_sjson(fd, r, 503, "{\"error\":\"no memory\"}"); return; }
        pthread_mutex_lock(&core_lock);
        const struct core_wake_word *w; int k = core_wake_words(&w), mww = core_wake_engine(-1) == WAKE_MWW;
        int eng = core_wake_engine(-1);
        n += (size_t)snprintf(t + n, cap - n, "{\"engine\":\"%s\",\"active\":", eng == WAKE_HA ? "homeassistant" : mww ? "microwakeword" : "amazon");
        int act[CORE_WAKE_ACTIVE_MAX], na = mww && k ? core_wake_active(act) : 0;
        if (na) { jesc(ej, sizeof ej, w[act[0]].id); n += (size_t)snprintf(t + n, cap - n, "\"%s\"", ej); }
        else n += (size_t)snprintf(t + n, cap - n, "null");
        n += (size_t)snprintf(t + n, cap - n, ",\"second\":");    /* Home Assistant's "Wake word 2" */
        if (na > 1) { jesc(ej, sizeof ej, w[act[1]].id); n += (size_t)snprintf(t + n, cap - n, "\"%s\"", ej); }
        else n += (size_t)snprintf(t + n, cap - n, "null");
        pthread_mutex_unlock(&core_lock);
        n += (size_t)snprintf(t + n, cap - n, ",\"max\":%d,\"models\":", MWW_MODEL_MAX);
        n += mww_list_json(t + n, cap - n - 2);
        n += (size_t)snprintf(t + n, cap - n, "}");
        respond_s(fd, r, 200, "application/json", NULL, t, n); free(t); return;
    }
    if (!write && sscanf(r->path, "/api/mww/file/%63[^/]/%23s", id, file) == 2) {
        char p[400], *data; long len; FILE *f;
        if (!mww_good_id(id) || (strcmp(file, "manifest.json") && strcmp(file, "model.tflite"))) snprintf(err, sizeof err, "no such file");
        else {
            snprintf(p, sizeof p, "%s/%s/%s", mww_dir(), id, file);
            if (!(f = fopen(p, "rb"))) snprintf(err, sizeof err, "no such model");
            else {
                fseek(f, 0, SEEK_END); len = ftell(f); rewind(f);
                data = len >= 0 && len <= MWW_MODEL_MAX ? malloc((size_t)len + 1) : NULL;
                if (data && fread(data, 1, (size_t)len, f) == (size_t)len) {
                    fclose(f);
                    respond_s(fd, r, 200, file[0] == 'm' && file[1] == 'a' ? "application/json" : "application/octet-stream", NULL, data, (size_t)len);
                    free(data); return;
                }
                fclose(f); free(data); snprintf(err, sizeof err, "cannot read it");
            }
        }
    }
    else if (write && sscanf(r->path, "/api/mww/add/%63s", id) == 1) {
        char *nl = memchr(r->body, '\n', r->blen < 16 ? r->blen : 16), *end; long jl = nl ? strtol(r->body, &end, 10) : -1;
        if (!nl || end != nl || jl < 0 || (size_t)jl > r->blen - (size_t)(nl + 1 - r->body)) snprintf(err, sizeof err, "bad request");
        else {
            const char *j = nl + 1; size_t ml = r->blen - (size_t)(nl + 1 - r->body) - (size_t)jl;
            rc = mww_add(id, j, (size_t)jl, j + jl, ml, err, sizeof err);
        }
    }
    else if (write && sscanf(r->path, "/api/mww/edit/%63s", id) == 1) rc = mww_edit(id, r->body, err, sizeof err);
    else if (write && sscanf(r->path, "/api/mww/delete/%63s", id) == 1) rc = mww_delete(id, err, sizeof err);
    else if (write && sscanf(r->path, "/api/mww/use/%63s", id) == 1) {
        pthread_mutex_lock(&core_lock);
        const struct core_wake_word *w; int k = core_wake_words(&w), i;
        for (i = 0; i < k && strcmp(w[i].id, id); i++) ;
        if (core_wake_engine(-1) != WAKE_MWW) snprintf(err, sizeof err, "switch the wake word engine to microWakeWord first");
        else if (i == k) snprintf(err, sizeof err, "no such model");
        else { core_wake_word(i); core_entities_changed(); rc = 0; }  /* Home Assistant's select reads the pick on reconnect */
        pthread_mutex_unlock(&core_lock);
    }
    else snprintf(err, sizeof err, "not found");
    if (!rc) { respond_sjson(fd, r, 200, "{\"ok\":true}"); return; }
    jesc(ej, sizeof ej, err); snprintf(o, sizeof o, "{\"error\":\"%s\"}", ej);
    respond_sjson(fd, r, 400, o);
}

/* /api/ring              GET   the slots, what each has (drafted and on the ring), the library's names (ringdesign.c)
 * /api/ring/designs      GET   every design as text, "design <id>" before each
 * /api/ring/stock/<slot> GET   stock's animation for a slot, as its file has it (the designer starts from it; volume:
 *                              every step, "step NN" before each)
 * /api/ring/design/<id>  POST  a design's text (ringdesign.h); id 0: a new one                        -> {"id"}
 * /api/ring/delete/<id>  POST
 * /api/ring/assign       POST  "<slot> <design id>" lines, 0: stock's
 * /api/ring/packs        GET   every theme pack as text, "pack <id>" before each
 * /api/ring/pack/<id>    POST  a pack as the Echo keeps it (ringdesign.h, version 2), checked; id 0: a new one -> {"id"}
 * /api/ring/pack-delete/<id> POST
 * /api/ring/apply        POST  the assignments onto the ring: root installs them, the ring goes dark for a second
 * /api/ring/live         POST  a design's text: the live ring plays it now, as drawn (no Apply, nothing kept); empty:
 *                              stops what plays.  Only while "Light ring live" is on (ringlive.h).  GET /api/ring says
 *                              "live": {"on", "playing"}
 * All signed. */
static void ring_api(int fd, struct req *r)
{
    char err[200] = "", o[300], ej[260], slot[32]; int rc = -1, id = 0, got = 0, write = !strcmp(r->method, "POST");
    if (!signed_ok(r, write)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); return; }
    if (!write && !strcmp(r->path, "/api/ring")) {
        size_t cap = 65536; char *t = malloc(cap);
        size_t n = t ? rd_status_json(t, cap) : 0;
        if (!n) { free(t); respond_sjson(fd, r, 503, "{\"error\":\"no memory\"}"); return; }
        if (t[n - 1] == '}' && n + 160 < cap) {                 /* the live ring beside the designs */
            char nm[RD_NAME], en[2 * RD_NAME + 8]; ringlive_playing(nm, sizeof nm); jesc(en, sizeof en, nm);
            n += (size_t)snprintf(t + n - 1, cap - n + 1, ",\"live\":{\"on\":%s,\"playing\":\"%s\"}}", ringlive_enabled() ? "true" : "false", en) - 1;
        }
        respond_s(fd, r, 200, "application/json", NULL, t, n); free(t); return;
    }
    if (!write && (!strcmp(r->path, "/api/ring/designs") || !strcmp(r->path, "/api/ring/packs") || sscanf(r->path, "/api/ring/stock/%31s", slot) == 1)) {
        size_t n = 0; char *t = r->path[10] == 'd' ? rd_library_text(&n) : r->path[10] == 'p' ? rd_packs_text(&n) : rd_stock(slot, &n);
        if (!t) { respond_sjson(fd, r, 404, "{\"error\":\"not found\"}"); return; }
        respond_s(fd, r, 200, "text/plain; charset=utf-8", NULL, t, n); free(t); return;
    }
    if (write && sscanf(r->path, "/api/ring/design/%d", &id) == 1) {
        if (!(rc = rd_save(id, r->body, r->blen, &got, err, sizeof err))) { snprintf(o, sizeof o, "{\"id\":%d}", got); respond_sjson(fd, r, 200, o); return; }
    }
    else if (write && sscanf(r->path, "/api/ring/pack/%d", &id) == 1) {
        if (!(rc = rd_pack_save(id, r->body, r->blen, &got, err, sizeof err))) { snprintf(o, sizeof o, "{\"id\":%d}", got); respond_sjson(fd, r, 200, o); return; }
    }
    else if (write && sscanf(r->path, "/api/ring/pack-delete/%d", &id) == 1) rc = rd_pack_delete(id, err, sizeof err);
    else if (write && sscanf(r->path, "/api/ring/delete/%d", &id) == 1) rc = rd_delete(id, err, sizeof err);
    else if (write && !strcmp(r->path, "/api/ring/assign")) rc = rd_assign(r->body, err, sizeof err);
    else if (write && !strcmp(r->path, "/api/ring/apply")) rc = rd_apply(err, sizeof err);
    else if (write && !strcmp(r->path, "/api/ring/live")) {
        if (!ringlive_enabled()) snprintf(err, sizeof err, "\"Light ring live\" is off (Features)");
        else if (!r->blen) { ringlive_stop(); rc = 0; }
        else rc = ringlive_play_text(r->body, r->blen, err, sizeof err);
        if (!rc) { pthread_mutex_lock(&core_lock); core_settings_changed(); pthread_mutex_unlock(&core_lock); }   /* the select's state */
    }
    else snprintf(err, sizeof err, "not found");
    if (!rc) { respond_sjson(fd, r, 200, "{\"ok\":true}"); return; }
    jesc(ej, sizeof ej, err); snprintf(o, sizeof o, "{\"error\":\"%s\"}", ej);
    respond_sjson(fd, r, 400, o);
}

/* /api/davs                       GET   where the Amazon login stands, and the code while one waits (davs.c)
 * /api/davs/login                 POST  an Amazon site ("de", "co.uk", ...): start a login (the code shows in the status)
 * /api/davs/cancel                POST  stop a login that waits for its code
 * /api/davs/logout                POST  deregister from the account, forget the tokens
 * /api/davs/fetch                 POST  "echo de-DE" / "aed de-DE" / "whisper": download, check, stage; install as for
 *                                      a copy (POST /api/artifact/install).  All signed; tokens never in an answer. */
static void davs_api(int fd, struct req *r)
{
    char o[1024], ej[256], err[200] = "";             /* o: the status with its error, name and artifact at full length */
    int write = !strcmp(r->method, "POST"), rc = 0;
    if (!signed_ok(r, write)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); return; }
    if (!write && !strcmp(r->path, "/api/davs"))
    {
        size_t n = davs_status_json(o, sizeof o);
        respond_s(fd, r, 200, "application/json", NULL, o, n);
        return;
    }
    if (write && !strcmp(r->path, "/api/davs/login"))
    {
        char dom[8] = "";
        sscanf(r->body, "%7s", dom);
        rc = davs_login(dom, err, sizeof err);
    }
    else if (write && !strcmp(r->path, "/api/davs/logout")) rc = davs_logout(err, sizeof err);
    else if (write && !strcmp(r->path, "/api/davs/cancel")) rc = davs_cancel(err, sizeof err);
    else if (write && !strcmp(r->path, "/api/davs/fetch"))
    {
        char key[32] = "", loc[8] = "";
        if (sscanf(r->body, "%31s %7s", key, loc) == 2) rc = davs_fetch(key, loc, err, sizeof err);
        else { snprintf(err, sizeof err, "what shall this Echo download?"); rc = -1; }
    }
    else snprintf(err, sizeof err, "not found");
    if (!rc) { respond_sjson(fd, r, 200, "{\"ok\":true}"); return; }
    jesc(ej, sizeof ej, err); snprintf(o, sizeof o, "{\"error\":\"%s\"}", ej);
    respond_sjson(fd, r, 400, o);
}

/* /api/vouch/nonce   "<browser key>"                                    -> {"nonce"}       unsigned
 * /api/vouch/issue   "<target's web key> <browser key> <nonce>"         -> {"voucher","via"}  signed: by a browser approved here
 * /api/vouch/login   "<browser key> <nonce> <voucher> <via, hex> <label>" -> {"login"}       unsigned: the voucher is what counts */
static void vouch_api(int fd, struct req *r)
{
    char a[80] = "", b[80] = "", c[80] = "", d[200] = "", o[400], vh[33], nh[33]; int k = 0;
    uint8_t pub[32], tgt[32], nonce[16], v[16], want[16];
    if (!strcmp(r->path, "/api/vouch/nonce")) {
        if (sscanf(r->body, "%79s", a) != 1 || unhex(pub, a, 32)) { respond_json(fd, 400, "{\"error\":\"key\"}"); return; }
        pthread_mutex_lock(&lk);
        unsigned i = nonce_next++ % 8;
        memcpy(nonces[i].pub, pub, 32); ws_random(nonces[i].nonce, 16); nonces[i].at = now_ms(); hex(nh, nonces[i].nonce, 16);
        pthread_mutex_unlock(&lk);
        snprintf(o, sizeof o, "{\"nonce\":\"%s\"}", nh); respond_json(fd, 200, o);
    } else if (!strcmp(r->path, "/api/vouch/issue")) {
        char via[160];
        if (!signed_ok(r, 1)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); return; }
        if (sscanf(r->body, "%79s %79s %79s", a, b, c) != 3 || unhex(tgt, a, 32) || unhex(pub, b, 32) || unhex(nonce, c, 16)) {
            respond_sjson(fd, r, 400, "{\"error\":\"bad request\"}"); return;
        }
        snprintf(via, sizeof via, "%s", core_name);
        if (voucher(v, tgt, pub, nonce, via)) { respond_sjson(fd, r, 400, "{\"error\":\"this Echo is in no network\"}"); return; }
        char vj[200]; jesc(vj, sizeof vj, via); hex(vh, v, 16);
        snprintf(o, sizeof o, "{\"voucher\":\"%s\",\"via\":\"%s\"}", vh, vj);
        fprintf(stderr, "web: vouched for a browser on another Echo\n");
        respond_sjson(fd, r, 200, o);
    } else if (!strcmp(r->path, "/api/vouch/login")) {
        char via[100] = "", label[64] = "browser"; uint8_t vb[100]; size_t vl;
        if (sscanf(r->body, "%79s %79s %79s %199s %n", a, b, c, d, &k) < 4 || unhex(pub, a, 32) || unhex(nonce, b, 16) || unhex(v, c, 16)
            || (vl = strlen(d) / 2) >= sizeof via || unhex(vb, d, vl)) { respond_json(fd, 400, "{\"error\":\"bad request\"}"); return; }
        memcpy(via, vb, vl); via[vl] = 0;
        for (char *x = via; *x; x++) if ((unsigned char)*x < 0x20 || *x == 0x7f) *x = ' ';     /* anyone's words, into the log: one line */
        if (k) { snprintf(label, sizeof label, "%s", r->body + k); for (char *x = label; *x; x++) if ((unsigned char)*x < 0x20) *x = ' '; }
        int ok = 0; long long now = now_ms();
        pthread_mutex_lock(&lk);
        for (int i = 0; i < 8; i++) {
            if (!nonces[i].at || now - nonces[i].at > NONCE_MS || memcmp(nonces[i].pub, pub, 32) || crypto_verify16(nonces[i].nonce, nonce)) continue;
            nonces[i].at = 0;                                   /* once */
            ok = !voucher(want, pk, pub, nonce, via) && !crypto_verify16(want, v);
            /* the nonce that matched is the one this voucher is held against, and the only one: scanning on, a
             * second entry with the same key and nonce would have been used up too and its answer taken for this one */
            break;
        }
        if (ok) { char l[64]; snprintf(l, sizeof l, "%.30s via %.28s", label, via); add_client(pub, l, 0); }
        pthread_mutex_unlock(&lk);
        fprintf(stderr, "web: login through %s %s\n", via, ok ? "approved" : "refused");
        respond_json(fd, 200, ok ? "{\"login\":\"approved\"}" : "{\"login\":\"refused\"}");
    } else respond_json(fd, 404, "{\"error\":\"not found\"}");
}

/* ---------------------------------------------------------------- Wi-Fi */

/* What the page seals for this Echo alone (a Wi-Fi password: the page is plain HTTP).  XOR with a key stream: block i
 * (64 bytes) = BLAKE2b-512 keyed with EK over the request's counter, "\n" and the byte i, EK = BLAKE2b-256 keyed with K
 * over "hassmic seal 1".  The counter is new with every request of a browser (signed_ok takes each once), so no
 * stream serves twice; the request's MAC covers the sealed bytes.  web/crypto.js seal does the other half. */
static int unseal(const struct req *r, const uint8_t *in, size_t n, uint8_t *out)
{
    uint8_t pub[32], k[32], ek[32], ks[64]; crypto_blake2b_ctx ctx; int bad;
    if (n > 64 * 40 || unhex(pub, r->pub, 32)) return -1;   /* callers bound n by their buffers: a Wi-Fi password, a token */
    pthread_mutex_lock(&lk); bad = session_key(k, pub); pthread_mutex_unlock(&lk);
    if (bad) return -1;
    crypto_blake2b_keyed(ek, 32, k, 32, (const uint8_t *)"hassmic seal 1", 14);
    for (size_t i = 0; i * 64 < n; i++) {
        uint8_t blk = (uint8_t)i;
        crypto_blake2b_keyed_init(&ctx, 64, ek, 32);
        crypto_blake2b_update(&ctx, (const uint8_t *)r->ctr, strlen(r->ctr)); crypto_blake2b_update(&ctx, (const uint8_t *)"\n", 1);
        crypto_blake2b_update(&ctx, &blk, 1);
        crypto_blake2b_final(&ctx, ks);
        for (size_t j = 0; j < 64 && i * 64 + j < n; j++) out[i * 64 + j] = in[i * 64 + j] ^ ks[j];
    }
    crypto_wipe(k, sizeof k); crypto_wipe(ek, sizeof ek); crypto_wipe(ks, sizeof ks);
    return 0;
}

/* What a plugin keeps, on its way between two Echos through the page (5.4; S4 of a review, 2026-11-21): it may hold a
 * token the plugin was issued, and the page is plain HTTP, so it is sealed both ways, as the Wi-Fi password is.  Of
 * any length: XOR with a key stream, block i (64 bytes) = BLAKE2b-512 keyed with EK over the request's counter, "\n",
 * NONCE (16 bytes, where there is one) and i as four bytes (big-endian), EK = BLAKE2b-256 keyed with K over PURPOSE:
 * "hassmic seal 2" for what the browser sends (each piece under its own request's counter, no nonce: the browser's
 * counter is new with every request it makes, and only the browser seals with it), "hassmic seal back 3" for what an
 * Echo answers, under the counter of the request it answers and a nonce of the Echo's own, new with every answer and
 * sent in clear in front of it (web/crypto.js seal2).  The counter alone was the browser's word: a GET replayed after
 * hassmic restarted (GET counters are not kept over a restart) was answered under the same stream again, with what the
 * plugin kept by then, and the two answers XORed gave what both held (Q2 of a review, 2026-11-21).  PUB the browser's
 * key; -1 when it has none here */
static int seal2(const uint8_t pub[32], const char *ctr, const uint8_t *nonce, const char *purpose, uint8_t *d, size_t n)
{
    uint8_t k[32], ek[32], ks[64]; crypto_blake2b_ctx ctx; int bad;
    pthread_mutex_lock(&lk); bad = session_key(k, pub); pthread_mutex_unlock(&lk);
    if (bad) return -1;
    crypto_blake2b_keyed(ek, 32, k, 32, (const uint8_t *)purpose, strlen(purpose));
    for (size_t i = 0; i * 64 < n; i++) {
        uint8_t blk[4] = { (uint8_t)(i >> 24), (uint8_t)(i >> 16), (uint8_t)(i >> 8), (uint8_t)i };
        crypto_blake2b_keyed_init(&ctx, 64, ek, 32);
        crypto_blake2b_update(&ctx, (const uint8_t *)ctr, strlen(ctr)); crypto_blake2b_update(&ctx, (const uint8_t *)"\n", 1);
        if (nonce) crypto_blake2b_update(&ctx, nonce, 16);
        crypto_blake2b_update(&ctx, blk, 4);
        crypto_blake2b_final(&ctx, ks);
        for (size_t j = 0; j < 64 && i * 64 + j < n; j++) d[i * 64 + j] ^= ks[j];
    }
    crypto_wipe(k, sizeof k); crypto_wipe(ek, sizeof ek); crypto_wipe(ks, sizeof ks);
    return 0;
}

/* /api/screen  POST "home <package>" | "home" (the face) | "home -" (none chosen: Kiosk Satellite where installed).  Signed */
static void screen_api(int fd, struct req *r)
{
    if (!signed_ok(r, 1)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); return; }
    if (!screen_present() || strcmp(r->method, "POST")) { respond_sjson(fd, r, 404, "{\"error\":\"not found\"}"); return; }
    char what[16] = "", v[128] = ""; int k = sscanf(r->body, "%15s %127s", what, v);
    if (k < 1 || strcmp(what, "home") || screen_set_home(k < 2 ? "" : !strcmp(v, "-") ? NULL : v)) { respond_sjson(fd, r, 400, "{\"error\":\"bad request\"}"); return; }
    size_t cap = 16384; char *o = malloc(cap);
    if (!o) { respond_sjson(fd, r, 503, "{\"error\":\"no memory\"}"); return; }
    size_t n = screen_status_json(o, cap);
    respond_s(fd, r, 200, "application/json", NULL, o, n); free(o);
}

/* /api/mass  POST "<url hex | -> <token sealed, hex | - | =>": Music Assistant's API for the screen (mass.c): its
 * address ("-": none; without a token the Echo looks at the one Sendspin played from) and a long-lived token, sealed
 * as a Wi-Fi password ("-": forget it, "=": keep it); a token needs an address (409).  Signed.  The token is never
 * sent back: /api/state says only whether there is one. */
static void mass_api(int fd, struct req *r)
{
    char uh[600] = "", th[5000] = "", url[260] = ""; uint8_t sealed[2400], tok[2401]; int tl = 0, ul = 0;
    if (!signed_ok(r, 1)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); return; }
    if (!screen_present() || strcmp(r->method, "POST")) { respond_sjson(fd, r, 404, "{\"error\":\"not found\"}"); return; }
    if (sscanf(r->body, "%599s %4999s", uh, th) != 2 || (strcmp(uh, "-") && (ul = unhex_any((uint8_t *)url, sizeof url - 1, uh)) < 0)
        || (strcmp(th, "-") && strcmp(th, "=") && ((tl = unhex_any(sealed, sizeof sealed, th)) < 0 || unseal(r, sealed, (size_t)tl, tok)))) {
        respond_sjson(fd, r, 400, "{\"error\":\"bad request\"}"); return;
    }
    if (strcmp(uh, "-")) url[ul] = 0;
    for (char *c = url; *c; c++) if ((unsigned char)*c < 0x21 || *c == '"' || *c == '\\' || (unsigned char)*c > 0x7e) {
        crypto_wipe(tok, sizeof tok); crypto_wipe(sealed, sizeof sealed); respond_sjson(fd, r, 400, "{\"error\":\"bad address\"}"); return;
    }
    tok[tl] = 0;                                        /* the page pads with zeros: a token has none */
    for (int i = 0; i < tl && tok[i]; i++) if (tok[i] < 0x21 || tok[i] > 0x7e || tok[i] == '"' || tok[i] == '\\') {
        crypto_wipe(tok, sizeof tok); crypto_wipe(sealed, sizeof sealed); respond_sjson(fd, r, 400, "{\"error\":\"not a token\"}"); return;
    }
    int rc = mass_set(url, !strcmp(th, "-") ? "-" : !strcmp(th, "=") ? "" : (const char *)tok);
    crypto_wipe(tok, sizeof tok); crypto_wipe(sealed, sizeof sealed);
    if (rc == -2) { respond_sjson(fd, r, 409, "{\"error\":\"Give Music Assistant's address\"}"); return; }
    if (rc == -4) { respond_sjson(fd, r, 400, "{\"error\":\"The Echo talks to Music Assistant's API without TLS: give its http address (port 8095)\"}"); return; }
    if (rc == -3) { respond_sjson(fd, r, 409, "{\"error\":\"A new address takes the token again: it only ever goes where it was given\"}"); return; }
    char o[4096]; size_t n = mass_status_json(o, sizeof o);
    respond_s(fd, r, rc ? 500 : 200, "application/json", NULL, o, n);
}

/* /api/wifi        GET   the network now, the last scan, the last switch (wifi.c)
 * /api/wifi/scan   POST  root scans                                              -> {"id"}
 * /api/wifi/join   POST  "<ssid hex> <password sealed, hex | ->": root switches,  -> {"id"}
 *                        and goes back to the network it was on if the Echo does not get on the new one
 * /api/wifi/address POST "auto" | "fixed <ip> <prefix> <gateway> <dns> <dns2|->": the address on this network,
 *                        tried and kept once the Echo was reached there (issue #191)   -> {"id"}
 * All signed; 409 while root is busy switching or a request waits. */
static void wifi_api(int fd, struct req *r)
{
    char err[200], ej[260], o[300]; unsigned id = 0; int write = !strcmp(r->method, "POST"), busy = 0, rc = -1;
    if (!signed_ok(r, write)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); return; }
    err[0] = 0;
    if (!write && !strcmp(r->path, "/api/wifi")) {
        size_t cap = 16384; char *t = malloc(cap);
        if (!t) { respond_sjson(fd, r, 503, "{\"error\":\"no memory\"}"); return; }
        size_t n = wifi_status_json(t, cap);
        respond_s(fd, r, 200, "application/json", NULL, t, n); free(t); return;
    }
    if (write && !strcmp(r->path, "/api/wifi/scan")) rc = wifi_scan(&id, &busy, err, sizeof err);
    else if (write && !strcmp(r->path, "/api/wifi/join")) {
        char sh[80] = "", ph[600] = ""; uint8_t ssid[40], sealed[256], pass[257]; int sl, pl = 0;
        if (sscanf(r->body, "%79s %599s", sh, ph) != 2 || (sl = unhex_any(ssid, sizeof ssid, sh)) < 0
            || (strcmp(ph, "-") && ((pl = unhex_any(sealed, sizeof sealed, ph)) < 0 || unseal(r, sealed, (size_t)pl, pass))))
            snprintf(err, sizeof err, "bad request");
        else {
            if (!strcmp(ph, "-")) pl = 0;
            pass[pl] = 0;                               /* the page pads with zeros: a password has none */
            rc = wifi_join(ssid, (size_t)sl, (const char *)pass, &id, &busy, err, sizeof err);
        }
        crypto_wipe(pass, sizeof pass); crypto_wipe(sealed, sizeof sealed);
    }
    else if (write && !strcmp(r->path, "/api/wifi/address")) {
        char spec[160];
        snprintf(spec, sizeof spec, "%s", r->body ? r->body : "");
        spec[strcspn(spec, "\r\n")] = 0;
        rc = wifi_address(spec, &id, &busy, err, sizeof err);
    }
    else snprintf(err, sizeof err, "not found");
    if (!rc) { snprintf(o, sizeof o, "{\"id\":%u}", id); respond_sjson(fd, r, 200, o); return; }
    jesc(ej, sizeof ej, err); snprintf(o, sizeof o, "{\"error\":\"%s\"}", ej);
    respond_sjson(fd, r, busy ? 409 : 400, o);
}

/* ---------------------------------------------------------------- plugins (plugin.h, plugin_store.h; docs/plugins.md 4.3, 5.4)
 * /api/plugins                              GET   everything the Plugins page shows; "install": a package on its way
 * /api/plugins/upload/begin                 POST  "<size>": a package (.hmp) starts
 * /api/plugins/upload/<offset>              POST  a piece of it, in order
 * /api/plugins/check                        POST  every check on what was uploaded: its name, author, where it comes
 *                                                 from, its author key's fingerprint, one line per capability,
 *                                                 whether the button is needed, and for an update what it asks for
 *                                                 beyond what was accepted
 * /api/plugins/install                      POST  installs what was checked: at once when it asks for nothing beyond
 *                                                 what the owner accepted, else it waits for the action button (asked again: its state)
 * /api/plugins/cancel                       POST  this browser's own install ended: the upload dropped, a wait for
 *                                                 the button withdrawn, a fetch ended
 * /api/plugins/index                        GET   the plugins the plugin index lists, as last looked (plugin_fetch.c
 *                                                 pf_json: per plugin the version this hassmic would install and the
 *                                                 highest there is); asks nobody
 * /api/plugins/index                        POST  "look" (the page, opened) | "again" (its button, pressed): the Echo
 *                                                 asks the index for its lists unless what it knows is young enough
 *                                                 for who asks, an hour or 30 s (plugin_fetch.c; in a thread of its
 *                                                 own, the GET says "looking" meanwhile).  The one request here, with
 *                                                 fetch, that makes the Echo ask outside the house.  After it the
 *                                                 index's listing of each version installed here that it lists anew
 *                                                 is taken (5.2's new key: plugin_fetch.c relist_due); "relist" (a
 *                                                 row's button where that failed): the same, asked again at once
 * /api/plugins/fetch                        POST  "<id>": the Echo downloads that plugin's version for this hassmic
 *                                                 from the plugin index (plugin_fetch.c) and checks it as an upload
 *                                                 is checked; answers "fetching" at once
 * /api/plugins/fetch                        GET   how that goes: "fetching", "failed" with why, or "checked" with
 *                                                 what /check answers for an upload; /install installs it then
 * /api/plugins/accept/nonce                 POST  a one-time nonce for this browser, for a voucher from another Echo
 * /api/plugins/accept/issue                 POST  "<target's web key> <nonce> <id>": this Echo's voucher that the
 *                                                 plugin is installed and was accepted here, for that Echo, that
 *                                                 nonce and the browser that asks -> {"voucher","node","manifest",
 *                                                 "key","accepted"}
 * /api/plugins/install                      POST  "synced <node> <nonce> <voucher>": the checked package installed
 *                                                 without this Echo's button if every rule of "sync" below holds
 *                                                 and its note is written ("installed"); else {"install":"button","why"}: nothing is armed,
 *                                                 the package stays checked for the dialog and a plain install
 * /api/plugins/sync                         GET   per plugin its manifest's digest and each item's (ps_item_digest):
 *                                                 what the page's "Copy plugins" compares between Echos
 * /api/plugins/<id>/pkg/<member>/<off>/<len> GET  a piece of the installed package, by member (ps_pkg_read): the
 *                                                 page packs them into the .hmp it uploads to another Echo
 * /api/plugins/synced                       POST  "dismiss <number>" | "dismiss all": the note that a plugin came
 *                                                 here on another Echo's voucher, read -> {"dismiss":"done"} from a
 *                                                 browser let in with this Echo's button that did not bring the
 *                                                 plugin, else "waiting" for the action button held for a second
 *                                                 ("busy", "none"; "later" and retry_s: three such waits an hour);
 *                                                 "keep": that wait withdrawn
 * /api/plugins/resume                       POST  plugins held back after hassmic's own crashes may start again (2.6)
 * /api/plugins/<id>/enable                  POST  "on" | "off"
 * /api/plugins/<id>/remove                  POST  "remove": the plugin, its settings and files
 * /api/plugins/<id>/propose                 POST  "take <row> <value>" | "drop <row> <value>": what the plugin found
 *                                                 (PROPOSE) written as the owner's own value of that row (the plugin
 *                                                 starts again), or let go
 * /api/plugins/<id>/action                  POST  "<name>": an action marked "page", its button on the card
 * /api/plugins/<id>/develop                 POST  "on" | "off": the development flag (a refused system call stops it
 *                                                 with SIGSYS, and the SDK names the call); a running plugin is started
 *                                                 again for it at once (not under an older launcher, which has no flag)
 * /api/plugins/<id>/storage                 GET   what it keeps (`storage`), as one tar of its keys: the page's copy
 *                                                 to another Echo, only when the owner ticks it (5.4).  Sealed for the
 *                                                 browser (seal2, "hassmic seal back 2"), its first line
 *                                                 "hassmic storage 1 wake=<0|1>"
 * /api/plugins/<id>/storage/begin           POST  "<size>": another Echo's copy comes, in pieces
 * /api/plugins/<id>/storage/chunk/<offset>  POST  a piece of it, sealed ("hassmic seal 2", its own request's counter);
 *                                                 a wake word plugin's data into one that is not: refused at done
 * /api/plugins/<id>/storage/done            POST  all of it in: the plugin stopped, its data replaced whole (or
 *                                                 nothing, with the reason: a key it cannot keep, over its kB),
 *                                                 started again
 * /api/plugins/<id>/set                     POST  "name=value": one setting; never a secret's (refused: it is plain)
 * /api/plugins/<id>/secret/<name>           POST  a secret, sealed as the Wi-Fi password is; never sent back
 * /api/plugins/<id>/item/<list>/<item>/begin               POST  an item's upload starts
 * /api/plugins/<id>/item/<list>/<item>/chunk/<suffix>/<offset>   POST  a piece of one of its files
 * /api/plugins/<id>/item/<list>/<item>/done                POST  all files in: the plugin checks it (5 s), and it
 *                                                 is kept, refused with the plugin's reason, or kept unchecked
 * /api/plugins/<id>/item/<list>/<item>/field               POST  "name=value": one of its fields
 * /api/plugins/<id>/item/<list>/<item>/delete              POST
 * /api/plugins/<id>/file/<list>/<item>/<suffix>            GET   a file of an item back
 * All signed.  What a manifest says is text: the page shows it with textContent, and nothing here interprets it.
 * The Echo decides everything: where a package comes from, whether its signatures hold, whether the button is
 * needed.  The page shows and asks. */
/* ---- the install slot.
 * There is one staging file (plugin_store.c) and one press of the action button at a time, so there is one install
 * at a time, whoever brings the package: a browser's upload, a browser's fetch from the plugin index, or a push from
 * the PC (web_plugin_push, hassmic's own claim).  The slot says whose it is and how far:
 *
 *   NONE -> UPLOADING -> CHECKING -> CHECKED -> CHECKING -> WAITS -> INSTALLING -> INSTALLED | FAILED
 *   NONE -> FETCHING <-> CHECKING -^      ^--------'    \---------^   (nothing new to accept; hassmic's own: from
 *                                                                      the first CHECKING.  Back to CHECKED: the
 *                                                                      button busy, a voucher refused, or the
 *                                                                      store wanting the button after all)
 *   ... INSTALLING (or CHECKING with a voucher) -> SECOND -> CHECKING -> WAITS -> INSTALLING   (request.audio with a
 *   way out: the store wants a second press, PS_NEEDS_SECOND; "the second press" below)
 *
 * The invariant.  Every change of the slot is decided under lk and names the claim it expects: the generation
 * (new with every claim and every release, so no two claims ever share one) and the state it was in; it happens
 * only if the slot still is that, and the caller is told when it is not.  Nobody writes the slot's state on the
 * strength of what it read under an earlier hold of lk.  So:
 *   - a slot is taken from its holder only as pl_rules says (who, from which state, after how long), in pl_claim;
 *   - the staging file is checked and deleted only by the slot's holder, in a state the rules give nobody else
 *     (CHECKING, INSTALLING: /api/plugins/install too looks at it in CHECKING, not in the CHECKED it found).
 *     Written it is in UPLOADING, which a browser loses 30 s after his last piece, and after lk is let go: so
 *     each write names its claim's generation and the store refuses one that is not the claim's that began the
 *     file (plugin_store.c stage_gen);
 *   - the press is armed for exactly one thing (pl_press: the generation, with login.pub the browser and the
 *     digest of the manifest its dialog listed), only from that browser's CHECKED, and it installs only if the
 *     slot still is exactly that at the press (plugin_accepted); the store then installs that digest or nothing
 *     (ps_stage_install's expect), and whether a press was needed it decides itself as it writes (its `press`):
 *     no caller's earlier "no button" installs what needs one by then;
 *   - a thread that fetches (struct bring) belongs to one claim: whoever takes or releases the slot tells it to
 *     stop and lets go of it (pl_reset), and what it does afterwards changes nothing (its generation is gone).
 * Found the hard way (a review, 2026-11-19): /install read the slot, let go of lk for its check, and then armed
 * the press without looking again; another browser's fetch claimed the slot in between, its thread's end wrote
 * "checked" over the wait, and the owner's press for the first browser's dialog installed whatever the second
 * had uploaded by then, a sideload included. */
struct bring { char id[PS_ID_MAX]; unsigned gen; volatile int stop; long long until; };   /* until: bring_thread's deadline */
enum { PL_NONE, PL_UPLOADING,   /* a browser sends a package's pieces */
       PL_FETCHING,             /* the Echo downloads one (bring_thread): from the plugin index */
       PL_CHECKING,             /* the staging file is being written, checked or dropped: seconds */
       PL_CHECKED,              /* a browser's dialog lists it */
       PL_WAITS,                /* ... and the press of the action button is armed for it */
       PL_INSTALLING, PL_INSTALLED, PL_FAILED,
       PL_SECOND };             /* the first press (or a voucher for it) went to it, and the store wants the second: its
                                 * browser shows the screen of its own and asks again */
enum { PO_NOBODY, PO_BROWSER, PO_HASSMIC };     /* PO_HASSMIC: no browser's (a push from the PC): nobody's to see or cancel */
/* at: since when it is in this state (UPLOADING: its last piece); by: the thread that fetches for this claim */
/* first: what stood for the first press of a package that wants two (PS_PRESSED, PS_VOUCHED; 0 none yet), second: the
 * second screen's words (plugin_store.c ps_second) */
static struct { int state, owner, first; unsigned gen; long long at; char id[PS_ID_MAX], err[240], second[160]; uint8_t digest[32], pub[32]; struct bring *by; } pl;    /* lk */
/* lk: what the armed press (login, W_PLUGIN) is for, and what it is with what came before it (PS_PRESSED, or for a
 * second press PS_PRESSED2 / PS_VOUCHED_PRESSED) */
static struct { unsigned gen; uint8_t digest[32]; int press; } pl_press;
/* what a browser is told whose install ended without it: its upload or dialog left so long that another took the
 * slot, a press that found another package than its dialog's.  The slot is not its own any more, so its state has
 * no place for it: kept here for NOTE_MS, or until that browser's next claim */
static struct { uint8_t pub[32]; char err[240]; long long at; } pl_note;    /* lk */
#define NOTE_MS 120000
static char pl_check[PS_CHECK_JSON]; static size_t pl_check_n;      /* lk: what the check said of a fetched package, as /check answers it */

/* Who may take the slot from whom, and after how long in that state.  One table, and pl_claim the one place that
 * reads it.  T_NOW at once, T_NEVER not at all (the state ends by itself, within seconds or with its own timeout),
 * else after that long:
 *   UPLOAD_MS   30 s without a piece: the page sends 200 kB pieces one after the other, and one that stopped is a
 *               page that was closed;
 *   CHECKED_MS  two minutes: a dialog read.  Until then the package the owner is reading about is the one the
 *               slot holds, and no other browser's, nor a push, takes its place;
 *   STUCK_MS    a fetch caps itself at FETCH_MS (bring_thread); half a minute past that its thread sits in what
 *               nothing interrupts (this libcurl's name lookup), and the slot is not held for it;
 *   WAITS ends with the wait for the button, LOGIN_MS.
 * FETCH_MS, five minutes for one fetch as a whole, the look and every candidate: a package is 4 MB at most, which
 * a link down to 256 kbit/s (32 kB/s: an Echo at the edge of its Wi-Fi) brings in 2 min 8 s, and a look is three
 * requests of 15 s at most; five minutes are that with room, and on a usual link all four candidates.  Before, a
 * server that kept each transfer above libcurl's 512 B/s floor held the slot for an hour (four times 900 s).
 * (hassmic's own claim, a push, holds CHECKING and INSTALLING only: within its call, which nobody takes.  Its column
 * "a browser or a push" is what a claim of hassmic's own gives way to; the Echo fetches nothing by itself.)
 * HASSMIC_PLUGIN_SLOT=<checked ms>:<upload ms>:<fetch ms> for the tests. */
enum { T_NOW, T_NEVER, T_UPLOAD, T_CHECKED, T_STUCK };
static int UPLOAD_MS = 30000, CHECKED_MS = 120000, FETCH_MS = 300000;
#define STUCK_MS (FETCH_MS + 30000)
static const struct pl_rule { int same, other, own, from_own; const char *why; } pl_rules[] = {
    /*                  a browser's slot, taken by                   | hassmic's own, taken by
     *                  the same browser, another one, hassmic's own | a browser or a push      and why not */
    [PL_NONE]       = { T_NOW,   T_NOW,     T_NOW,     T_NOW,   NULL },
    [PL_UPLOADING]  = { T_NOW,   T_UPLOAD,  T_UPLOAD,  T_NOW,   "a browser is installing a plugin just now: try again in a minute" },
    [PL_FETCHING]   = { T_NEVER, T_STUCK,   T_STUCK,   T_NOW,   "a plugin is being fetched from the plugin index: try again when that is done" },
    [PL_CHECKING]   = { T_NEVER, T_NEVER,   T_NEVER,   T_NEVER, "a plugin is being checked: try again in a moment" },
    [PL_CHECKED]    = { T_NOW,   T_CHECKED, T_CHECKED, T_NOW,   "a browser is installing a plugin just now: try again in two minutes" },
    [PL_WAITS]      = { T_NEVER, T_NEVER,   T_NEVER,   T_NEVER, "another plugin waits for the action button" },
    [PL_INSTALLING] = { T_NEVER, T_NEVER,   T_NEVER,   T_NEVER, "a plugin is being installed: try again in a moment" },
    [PL_INSTALLED]  = { T_NOW,   T_NOW,     T_NOW,     T_NOW,   NULL },      /* (what came of it stays for its browser's page until the next claim) */
    [PL_FAILED]     = { T_NOW,   T_NOW,     T_NOW,     T_NOW,   NULL },
    [PL_SECOND]     = { T_NOW,   T_CHECKED, T_CHECKED, T_NOW,   "a browser is installing a plugin just now: try again in two minutes" },
};
#define PL_OVERTAKEN "another install took this one's place before it was accepted: nothing was installed. Upload or fetch the package again"
/* HASSMIC_PLUGIN_ASK_DELAY=<ms>[:<file>]: /install sleeps between its check and its ask, that long, or with a file for
 * as long as it exists, that long at most, and says so (tests/fake_plugins_race.py: the window held open until what it
 * tests in it is done, not for a time a busy machine overran) */
static int ASK_DELAY_MS; static char ASK_DELAY_FILE[200];
/* HASSMIC_PLUGIN_SYNC_DELAY=<ms>:<file>: while the file exists a synced install sleeps, that long at most, between
 * its note (pending) and the install (tests/fake_plugins_sync.py) */
static int SYNC_DELAY_MS; static char SYNC_DELAY_FILE[200];
static int OWN_DELAY_MS;        /* HASSMIC_PLUGIN_OWN_DELAY: a push sleeps between its check and its install, and says so */
static void own_delay(const char *id)
{
    if (!OWN_DELAY_MS) return;
    fprintf(stderr, "plugins: (test) %s checked, its install in %d ms\n", id, OWN_DELAY_MS); usleep((useconds_t)OWN_DELAY_MS * 1000);
}

/* all of these: lk held */
static int pl_mine(const uint8_t pub[32]) { return pl.owner == PO_BROWSER && !memcmp(pl.pub, pub, 32); }
static int pl_is(unsigned gen, int state) { return pl.gen == gen && pl.state == state; }
static int pl_move(unsigned gen, int from, int to) { if (!pl_is(gen, from)) return 0; pl.state = to; pl.at = now_ms(); return 1; }
static const char *pl_why(const char *none) { return pl_rules[pl.state].why ? pl_rules[pl.state].why : none; }
static int press_armed(void) { return login.state == 1 && login.kind == W_PLUGIN && pl.state == PL_WAITS && pl_press.gen == pl.gen; }

static void pl_tell(const uint8_t pub[32], const char *why)
{
    memcpy(pl_note.pub, pub, 32); snprintf(pl_note.err, sizeof pl_note.err, "%s", why); pl_note.at = now_ms();
}
static const char *pl_told(const uint8_t pub[32])      /* NULL: nothing */
{
    return pl_note.at && !memcmp(pl_note.pub, pub, 32) && now_ms() - pl_note.at < NOTE_MS ? pl_note.err : NULL;
}

/* the slot is nobody's: its thread, if one fetches for it, is told to stop and is on its own; a new generation */
static void pl_reset(void)
{
    unsigned g = pl.gen + 1;
    if (pl.by) pl.by->stop = 1;
    memset(&pl, 0, sizeof pl); pl.gen = g ? g : 1; pl_check_n = 0;
}

/* a wait whose press is no longer armed for it (it ran out; nothing else takes an armed wait's place) */
static void pl_settle(long long now)
{
    login_expire(now);
    if (pl.state == PL_WAITS && !press_armed()) { pl.state = PL_FAILED; pl.at = now; snprintf(pl.err, sizeof pl.err, "the action button was not pressed in time"); }
}

/* The slot for a new install, in STATE: PUB the browser that asks, NULL hassmic's own (PUSH: from the PC).  Its
 * generation, or 0 and why not.  The one place a slot changes hands */
static unsigned pl_claim(const uint8_t *pub, int push, int state, const char *id, char *err, size_t cap)
{
    long long now = now_ms(), ms; const struct pl_rule *r; int t;
    pl_settle(now);
    r = &pl_rules[pl.state];
    if (pl.owner == PO_HASSMIC) t = pub || push ? r->from_own : r->own;
    else t = pub && pl_mine(pub) ? r->same : pub ? r->other : r->own;
    ms = t == T_UPLOAD ? UPLOAD_MS : t == T_CHECKED ? CHECKED_MS : t == T_STUCK ? STUCK_MS : 0;
    if (t == T_NEVER || now - pl.at < ms) { snprintf(err, cap, "%s", r->why); return 0; }
    /* whoever had it and did not end it himself hears of it when his page asks next */
    if (pl.owner == PO_BROWSER && !(pub && pl_mine(pub)) && (pl.state == PL_UPLOADING || pl.state == PL_CHECKED || pl.state == PL_FETCHING || pl.state == PL_SECOND)) {
        pl_tell(pl.pub, PL_OVERTAKEN);
        fprintf(stderr, "web: a browser's install of a plugin, left for %lld s, makes room for %s\n", (now - pl.at) / 1000, pub ? "another browser's" : "hassmic's own");
    }
    if (pl.owner == PO_HASSMIC && pl.state == PL_FETCHING) fprintf(stderr, "plugins: hassmic's own fetch of %s gives way to %s\n", pl.id, pub ? "the settings page" : "a push");
    pl_reset();
    pl.state = state; pl.owner = pub ? PO_BROWSER : PO_HASSMIC; pl.at = now; snprintf(pl.id, sizeof pl.id, "%s", id);
    if (pub) { memcpy(pl.pub, pub, 32); if (pl_told(pub)) pl_note.at = 0; }
    return pl.gen;
}

/* The end of a claim that is in a state nobody takes the slot in (CHECKING, INSTALLING), not lk: the staging file
 * dropped unless KEEP, then the slot's state (in that order: the file is this install's until the slot is anyone's
 * again).  PL_NONE: the slot is free; else it stays its browser's, for its page to read what came of it */
static void pl_close(unsigned gen, int state, int keep, const char *err)
{
    if (!keep) ps_stage_drop();
    pthread_mutex_lock(&lk);
    if (pl.gen == gen) {
        if (state == PL_NONE) pl_reset();
        else { pl.state = state; pl.at = now_ms(); pl.by = NULL; snprintf(pl.err, sizeof pl.err, "%s", err ? err : ""); }
    }
    pthread_mutex_unlock(&lk);
}

/* lk held: the press came.  1 and what to install, if the slot still is what the press was armed for: the same
 * claim, waiting, the browser whose dialog it was, the manifest that dialog listed.  0: nothing is installed, and
 * that browser is told */
static int plugin_accepted(unsigned *gen, uint8_t digest[32], int *press)
{
    if (press_armed() && pl_mine(login.pub) && !memcmp(pl.digest, pl_press.digest, 32)) {
        pl.state = PL_INSTALLING; pl.at = now_ms(); *gen = pl.gen; memcpy(digest, pl_press.digest, 32); *press = pl_press.press;
        return 1;
    }
    pl_tell(login.pub, PL_OVERTAKEN);
    return 0;
}

static void plugin_withdraw(const char *why)        /* lk held: a wait for the button that no longer counts */
{
    if (!(login.state == 1 && login.kind == W_PLUGIN)) return;
    if (press_armed()) { pl.state = PL_FAILED; pl.at = now_ms(); snprintf(pl.err, sizeof pl.err, "%s", why); }
    login.state = 3; login.end = now_ms(); login.why = "busy"; wait_over(login.end, 0, NULL);
    fprintf(stderr, "web: the plugin's install withdrawn (%s)\n", why);
    if (hooks->attention) hooks->attention(0);
}

/* Installs what the slot holds under GEN, which is INSTALLING (not lk): the package whose manifest has DIGEST, or
 * nothing (the store's own check, ps_stage_install).  PRESS: what stands for the owner's acceptance (PS_NO_PRESS,
 * PS_PRESSED, PS_VOUCHED); whether that is enough is the store's to say, at the moment it writes: what a check
 * said earlier is not handed down.  0 installed; -1 and WHY (may be NULL); PS_NEEDS_BUTTON and WHY: PRESS is not
 * enough any more (the plugin was removed since the check, say), nothing was written.  STAY: the slot stays
 * INSTALLING for the caller to close (hassmic's own); else it ends INSTALLED or FAILED, for its browser's page,
 * or, PS_NEEDS_BUTTON, goes back to CHECKED with the package kept: the next /api/plugins/install asks for the button */
static int plugin_install_now(const char *how, unsigned gen, const uint8_t digest[32], int press, int stay, char *why, size_t cap)
{
    char err[200] = "", id[PS_ID_MAX]; int had, mine, bad; const struct ps_plugin *sp;
    pthread_mutex_lock(&lk); mine = pl_is(gen, PL_INSTALLING); snprintf(id, sizeof id, "%s", pl.id); pthread_mutex_unlock(&lk);
    if (!mine) { if (why) snprintf(why, cap, "%s", PL_OVERTAKEN); return -1; }      /* (INSTALLING is nobody's to take: never) */
    /* "updated" where a package this hassmic runs was here and accepted, "installed" otherwise: a card is also listed
     * for a manifest this build cannot read (one for a newer hassmic among them), which was no plugin the owner had
     * running */
    ps_lock(); had = (sp = ps_find(id)) && sp->here && !sp->unread && sp->accepted; ps_unlock();
    char line[160] = "";
    bad = ps_stage_install(how, digest, press, err, sizeof err);
    if (bad == -1 && !stay) ps_stage_drop();    /* (while the slot is still INSTALLING: the file is this install's) */
    if (bad == PS_NEEDS_SECOND) ps_stage_second(line, sizeof line);     /* (the store's lock: never under lk) */
    pthread_mutex_lock(&lk);
    if (!stay && pl.gen == gen) {
        /* the second press wanted (request.audio with a way out): what came stands for the first, and the slot waits
         * for its browser's second screen (PS_NEEDS_SECOND writes nothing: the package stays checked), its words set
         * with the state (a review of b294fa0: a moment of "second" with none) */
        if (bad == PS_NEEDS_SECOND) { pl.state = PL_SECOND; pl.first = press == PS_VOUCHED ? PS_VOUCHED : PS_PRESSED; pl.err[0] = 0; snprintf(pl.second, sizeof pl.second, "%s", line); }
        else {
            pl.state = bad == PS_NEEDS_BUTTON ? PL_CHECKED : bad ? PL_FAILED : PL_INSTALLED; snprintf(pl.err, sizeof pl.err, "%s", bad == PS_NEEDS_BUTTON ? "" : err);
            /* PS_NEEDS_BUTTON: what stood for the first press does not any more (a voucher that went stale between
             * the presses), so both presses come again, here: the next ask arms a plain first (plugin_ask).  Kept, it
             * armed the voucher's second again and again, and the store refused it each time (a review of b294fa0, M2) */
            if (bad == PS_NEEDS_BUTTON) { pl.first = 0; pl.second[0] = 0; }
        }
        pl.at = now_ms();
    }
    pthread_mutex_unlock(&lk);
    if (why) snprintf(why, cap, "%s", err);
    if (bad == PS_NEEDS_SECOND) {
        fprintf(stderr, "plugins: %s waits for the second press (%s): %s\n", id, press == PS_VOUCHED ? "a voucher stood for the first" : "the first came", err);
        return bad;
    }
    fprintf(stderr, "plugins: %s %s%s%s\n", id, bad ? "not installed: " : "installed (", bad ? err : how, bad ? "" : ")");
    if (bad) return bad;
    /* Home Assistant's "Plugin": one event an install.  `synced` for what came on another Echo's voucher, first
     * install or update: that nobody pressed this Echo's button for it is what an automation or the logbook is to
     * have (5.4), and the core holds that one until Home Assistant has a link to hear it on */
    plugin_installed(id); core_plugin_event(!strncmp(how, PS_HOW_SYNCED, sizeof PS_HOW_SYNCED - 1) ? PLUGIN_SYNCED : had ? PLUGIN_UPDATED : PLUGIN_INSTALLED, id);
    return 0;
}

/* A checked package that needs the owner's press: armed for the claim GEN of browser PUB and the manifest DIGEST
 * its dialog listed, and only if the slot still is that, held in CHECKING by /api/plugins/install for its look at
 * the file.  "waiting"; "busy" (something else waits for the button: *HOLDS says what; the slot is CHECKED again);
 * NULL: the slot is something else by now, nothing is armed */
static const char *plugin_ask(const uint8_t pub[32], unsigned gen, const uint8_t digest[32], const char **holds)
{
    const char *r, *h; long long now = now_ms(); char label[64];
    *holds = "none";
    pthread_mutex_lock(&lk);
    pl_settle(now);
    if (!pl_is(gen, PL_CHECKING) || !pl_mine(pub) || memcmp(pl.digest, digest, 32)) r = NULL;
    else {
        snprintf(label, sizeof label, "plugin %s", pl.id);
        if ((h = wait_arm(pub, W_PLUGIN, label, now))) { r = "busy"; *holds = h; pl.state = pl.first ? PL_SECOND : PL_CHECKED; pl.at = now; }
        else {
            /* the second press is this one where the first came already (or a voucher stood for it) */
            pl_press.gen = gen; memcpy(pl_press.digest, digest, 32);
            pl_press.press = pl.first == PS_VOUCHED ? PS_VOUCHED_PRESSED : pl.first == PS_PRESSED ? PS_PRESSED2 : PS_PRESSED;
            pl.state = PL_WAITS; pl.at = now; r = "waiting"; *holds = "plugin";
            fprintf(stderr, "web: the plugin %s waits for the action button\n", pl.id);
        }
    }
    pthread_mutex_unlock(&lk);
    hold_flush(r && !strcmp(r, "waiting"));     /* (r NULL: the slot was another's by now, nothing armed) */
    return r;
}

/* What the Echo's check says of a staged package, for the page's dialog: the same for one a browser uploaded
 * (/api/plugins/check) and one the Echo fetched (/api/plugins/fetch).  "key": the author key's fingerprint, which the
 * page names for a package nobody vouches for (5.4); "source": the address its card will link to, accepted with
 * the rest (the digest's form 3) and so shown whole in the dialog; "update" and "changes": what of this version is
 * not what was accepted, when a version was ("limit": its memory or threads, shown and never what the button is for) */
/* how this model's policy is written (device.conf INSTALL, the Makefile's INSTALL_METHOD; HASSMIC_INSTALL for tests),
 * for the words that say how the sandbox's missing SELinux wall comes back (web/plugins.js plgFix) */
static const char *install_method(void)
{
    const char *im = getenv("HASSMIC_INSTALL");
    return im && *im && strlen(im) < 16 && !strpbrk(im, "\"\\") ? im : INSTALL_METHOD;
}

static size_t check_json(const struct ps_manifest *m, int tier, int button, const struct ps_changes *ch, char *t, size_t cap)
{
    /* every line whole (PS_CAP_LINE, PS_CHECK_JSON: a review, 2026-11-21, found net.server's cut at 300 bytes), and
     * what does not fit CAP said as an error, never written past it */
    char nj[120], aj[120], dj[520], sj[440], line[PS_CAP_LINE], lj[2 * (PS_CAP_LINE + 128) + 8], fp[20]; size_t n; int k;
#define CJ(...) do { k = snprintf(t + n, n < cap ? cap - n : 0, __VA_ARGS__); n += k > 0 ? (size_t)k : 0; } while (0)
    jesc(nj, sizeof nj, m->name); jesc(aj, sizeof aj, m->author); jesc(dj, sizeof dj, m->description); jesc(sj, sizeof sj, m->source);
    ps_fingerprint(m->author_key, fp);
    n = 0;
    CJ("{\"id\":\"%s\",\"name\":\"%s\",\"author\":\"%s\",\"description\":\"%s\",\"source\":\"%s\",\"version\":%ld,\"tier\":\"%s\",\"key\":\"%s\",\"button\":%s,\"update\":%s,\"requires\":{",
       m->id, nj, aj, dj, sj, m->version, tier == PS_OFFICIAL ? "official" : tier == PS_COMMUNITY ? "community" : "sideload", fp, button ? "true" : "false", ch->update ? "true" : "false");
    /* requires: what it speaks (sdk/PROTOCOL.md "Versions"), which this build has, or the check had refused it */
    for (int s = 0, c = 0; s < PM_SCOPES; s++) if (m->requires[s]) CJ("%s\"%s\":%u", c++ ? "," : "", pm_scope_name(s), m->requires[s]);
    CJ("},\"caps\":[");
    for (int b = 0, first = 1; ps_cap_name(b); b++) if (m->caps & PM_CAPBIT(b)) {
        ps_cap_line(m, b, line, sizeof line); jesc(lj, sizeof lj, line);
        CJ("%s{\"cap\":\"%s\",\"line\":\"%s\"}", first ? "" : ",", ps_cap_name(b), lj); first = 0;
    }
    /* "widens": every version up to the one required that lets it do more than its capability's line says, in that
     * version's words (finding 5 of a review of 60a8589; an update's own lines are in "changes") */
    { char wj[PS_WIDENS_JSON]; ps_widens_json(m, wj, sizeof wj); CJ("],\"widens\":%s", wj); }
    CJ(",\"changes\":[");
    for (int c = 0; c < ch->n; c++) {
        jesc(lj, sizeof lj, ch->c[c].line);
        CJ("%s{\"kind\":\"%s\",\"line\":\"%s\"}", c ? "," : "", ps_change_kind(ch->c[c].kind), lj);
    }
    /* "second": request.audio with a way out wants a second press wherever the button is asked for (3.3): the words of
     * its screen, "" for any other package */
    /* "sandbox", "install_method": this Echo's, for the warning in full that the dialog of a plugin that is not
     * official has where the sandbox misses its SELinux wall (2.7): the dialog is where every such plugin is
     * accepted, whichever way community plugins were switched on (the page's switch, an import, a preset, a copy;
     * a review of 00d492a, 2) */
    { char sl[160]; ps_second(m, sl, sizeof sl); jesc(lj, sizeof lj, sl);
      CJ("],\"changes_more\":%d,\"shown_more\":%d,\"second\":\"%s\",\"sandbox\":\"%s\",\"install_method\":\"%s\"}", ch->more, ch->more_shown, lj, plugin_sandbox(), install_method()); }
#undef CJ
    if (n >= cap) n = (size_t)snprintf(t, cap, "{\"error\":\"what this package asks for is too long to show\"}");
    return n;
}

/* ---- sync: what the owner accepted on one Echo, installed on another without its button (docs/plugins.md 5.4,
 * "the acceptance travels").
 * The page carries the package from the source to the target as it carries any upload: through the install slot,
 * as that browser's claim, checked by the target as every package is.  What is new is the last step.  Where the
 * check says "the button", /api/plugins/install may come with a voucher of the source instead, and the target
 * installs without its own press if ALL of this holds (sync_judge; every line is the target's own finding):
 *   1. the nonce is one this Echo handed this very browser (asked for with a signed request: approved here), not
 *      used before, not older than ACC_NONCE_MS.  A voucher counts once;
 *   2. neither end settles rounds the Kiosk Satellite way (5.4: that protocol has no key.  An Echo of ours in that
 *      mode still holds the network's key; the rule is kept by the mode all the same, each end by its own setting
 *      and the target by what the source's beacons say);
 *   3. the voucher's MAC holds under this Echo's own copy of the network's page key, over this Echo's web key, the
 *      nonce, the browser that asks, the source's node, and the id, manifest digest, author key and accepted
 *      section of the package THIS ECHO just checked, never of anything the request says: a voucher for another
 *      Echo, browser, plugin or package is none;
 *   4. the source is a member this Echo has heard under that node (its signed beacons, arb.c);
 *   5. the package has the index key's listing, as official.  A sideload never syncs: nobody but the owner of the
 *      one Echo it was installed on ever looked at it;
 *   6. the package passed ps_stage_check here: its form and signatures, what it requires (this build has each
 *      scope at that version), the author key pinned here (a plugin installed under another key is never replaced:
 *      the check refuses it), never back, the combinations (`runnable`), the least version, the room;
 *   7. "never back" could be held at all (ps_changes.unsure);
 *   8. the version is not older than the newest this Echo has ever seen listed for the plugin in the plugin
 *      index (plugin_fetch.c pf_seen, kept over restarts), nor than the highest that was ever installed here
 *      (plugin_store.c ps_installed_max, kept until a factory reset).  The store's "never back" (6) is against
 *      what is installed here now; these are for the plugin that is not: without the first a member could bring
 *      an Echo an old official version, one with a fault since mended, as its first; without the second, as it
 *      was, whoever may remove a plugin (no button) removed it and had an older one copied in (a review,
 *      2026-11-20).  An Echo that has seen no list (it never looked: the Plugins page not opened with plugins on,
 *      the index not reachable) and never had the plugin has nothing to hold a version against, and takes any
 *      official one this build runs, down to the least it names itself (ps_official_least).  Where what was
 *      installed before cannot be read: the button;
 *   9. plugins are switched on here.  With them off nothing would run it, no page would be open at it, and Home
 *      Assistant has no "Plugin" entity to hear of it: an install nobody is told of;
 *  10. the note that announces it could be written (syn_begin), which it cannot while SYN_MAX of them stand, read
 *      or not: nobody but a dismissal says a note was read.
 * What a voucher proves is a member's key, NOT the press of a button, and NOT which Echo: every member derives
 * arb_web_key, so any of them can write a voucher in any member's name (4 checks only that such a member exists).
 * A member that was taken over writes vouchers for anything and vouches its own browser onto the others first
 * (vouch_api).  That is why 5 to 10 do not rest on it, and why the node is written everywhere as what the voucher
 * named, never as where the plugin came from.  What such a member can do with this, on every Echo of the network:
 * have a package installed that the plugin index listed as official, within what each target checks itself, and
 * announced on each target in ways it cannot take back by itself (below).  What it cannot: a program of its own
 * making, a sideload, a package under another author's key in place of one installed, a version older than the one
 * installed, than any that was installed there before or than one the target saw listed, anything this build
 * would not run.
 * An update: where what comes is what this Echo accepted or less, ps_stage_check asks for no button and no
 * voucher is looked at (the ordinary install).  Where it is wider than what THIS Echo accepted, the voucher stands
 * in for the press as for a first install: the owner accepted that wider set with a button on the source.
 * Anything missing: {"install":"button"} and why.  Nothing is armed, nothing installed: the package stays checked,
 * the page shows its dialog, and a plain /api/plugins/install waits for this Echo's own button.
 * The slot: a synced install is its browser's claim like any upload.  The decision is taken for the claim and the
 * digest read under lk, and the install happens only if the slot still is that (pl_move), as the install without
 * the button does: never beside pl_rules. */

/* a node as arb.c's beacons name one, and nothing that could be a line of its own in `accepted` */
static int node_ok(const char *s)
{
    size_t n = strlen(s);
    if (!n || n > 63) return 0;
    for (; *s; s++) if (!(islower((unsigned char)*s) || isdigit((unsigned char)*s) || *s == '-' || *s == '_' || *s == '.')) return 0;
    return 1;
}

/* 0: the package M (as ps_stage_check found it just now: TIER, CH) may be installed without this Echo's button, on
 * the voucher V of NODE for NONCE and the browser PUB.  -1 and WHY: this Echo's own button.  Not lk */
static int sync_judge(const uint8_t pub[32], const struct ps_manifest *m, int tier, const struct ps_changes *ch,
                      const char *node, const uint8_t nonce[16], const uint8_t v[16], char *why, size_t cap)
{
    uint8_t key[32], want[16]; int fresh = 0, mode; long long now = now_ms(); long seen, had;
    /* 1: used up before anything else is looked at, whatever comes of the rest */
    pthread_mutex_lock(&lk);
    for (int i = 0; i < 8; i++) {
        if (!acc_nonces[i].at || memcmp(acc_nonces[i].pub, pub, 32) || crypto_verify16(acc_nonces[i].nonce, nonce)) continue;
        fresh = now - acc_nonces[i].at <= ACC_NONCE_MS; acc_nonces[i].at = 0;
    }
    pthread_mutex_unlock(&lk);
    why[0] = 0;
    if (!fresh) snprintf(why, cap, "the voucher's nonce is not one this Echo gave this browser, was used before, or is older than %d s", ACC_NONCE_MS / 1000);
    /* 2 (this end) */
    else if (arb_mode(-1) == ARB_KIOSK) snprintf(why, cap, "this Echo settles wake words the Kiosk Satellite way: plugins are not taken from other Echos without its button in that mode");
    else if (arb_web_key(key)) snprintf(why, cap, "this Echo is in no network with other Echos");
    else {
        /* 3: every field but the node from this Echo's own hands */
        ps_voucher(want, key, pk, nonce, pub, node, m->id, m->manifest_digest, m->author_key, m->accepted);
        crypto_wipe(key, sizeof key);
        if (crypto_verify16(want, v)) snprintf(why, cap, "the voucher is not one of this network for this Echo, this browser and this very package");
        /* 4, and 2 (the other end) */
        else if ((mode = arb_member_mode(node)) < 0) snprintf(why, cap, "%s is no Echo of this network that this Echo has heard", node);
        else if (mode == ARB_KIOSK) snprintf(why, cap, "%s settles wake words the Kiosk Satellite way: plugins are not taken from it without this Echo's button", node);
        /* 5 */
        else if (tier != PS_OFFICIAL) snprintf(why, cap, "%s is not listed as official by the plugin index: what only its author (or the index, for a community plugin) vouches for is accepted on each Echo, with its own button", m->id);
        /* 6 is that ps_stage_check passed */
        /* 7 */
        else if (ch->unsure) snprintf(why, cap, "the version installed here has a manifest this hassmic cannot read: whether this one is newer cannot be checked");
        /* 8 */
        else if ((seen = pf_seen(m->id)) > m->version) snprintf(why, cap, "version %ld of %s is older than version %ld, which this Echo has seen in the plugin index: an older one is installed only with its own button", m->version, m->id, seen);
        else if ((had = ps_installed_max(m->id)) == LONG_MAX) snprintf(why, cap, "this Echo cannot tell which versions of its plugins were installed here before (state/plugins/.installed): nothing is installed here without its button until it can");
        else if (had > m->version) snprintf(why, cap, "version %ld of %s is older than version %ld, which was installed on this Echo before: an older one is installed only with its own button", m->version, m->id, had);
        /* 9 */
        else if (!settings_on("plugins")) snprintf(why, cap, "plugins are switched off on this Echo: nothing is installed here without its button while they are");
        /* What the Echo itself wants a second press for, it wants from a sync too (5.4; 3.3: `request.audio` together
         * with a `net.*`): a voucher is never a press, and stands for the first of the two at most.  Not judged here:
         * the store asks for the second as it writes (ps_stage_install: PS_NEEDS_SECOND for PS_VOUCHED), and the
         * install below answers "second": the slot waits for this Echo's own press on its screen (PS_VOUCHED_PRESSED,
         * which the store holds to the voucher's rules again) */
    }
    return why[0] ? -1 : 0;
}

/* ---- the announcement that stands in for the press (5.4).
 * Nobody stood at this Echo for a plugin that came on a voucher, so the Echo says it where the owner comes by
 * without looking for it: the log, Home Assistant's event `synced` (held until Home Assistant has a link: main.c
 * core_plugin_event), and a note on every page of this Echo and, through /api/state, on those pages of the other
 * Echos that poll this one (a browser's page polls the Echos it is logged in to, while it shows them).  A note is
 * worth what it costs the one it is about to take it off, so:
 *  - Written first.  syn_begin puts the note on the flash (statefile.c, state "pending") BEFORE the install; the
 *    install that follows makes it "done", one that fails takes it away again unless the store has the package
 *    all the same (syn_failed: an install can fail after its files were written).  hassmic ending in between
 *    leaves the note, and the next start reads the store: the version is installed, the note stands; it is not,
 *    the note goes, with a line in the log.  The other order left a plugin installed and nothing saying how.
 *    A pending note is on no page yet, so nothing takes it off: what a dismissal covers is the notes that were
 *    "done" when it was asked for, by their numbers (struct syn_ids), never "all up to the newest".  It was a
 *    number up to which, and that counted the pending one: a press armed in the seconds of an install took off
 *    the note of a plugin no page had shown (a review, 2026-11-20).
 *  - None lost.  One note an install, never one a plugin with the newest replacing the others, and none pushed
 *    out by the next: at SYN_MAX of them standing, syn_begin refuses, and that sync is answered "button" (rule
 *    10).  So nobody makes a note go by copying more.
 *  - Not taken off by the browser it names, nor by one that only a member vouched for.  Each note keeps the key
 *    of the browser that brought the package.  A request alone takes a note off only when it comes from ANOTHER
 *    approved browser that was let in with this Echo's own button (struct client button 1, state/web_button).  A
 *    browser let in on a voucher does not count: the member that can forge an acceptance voucher can forge a
 *    login voucher for as many browser keys as it likes, and "another browser" would be its second key.  A note
 *    whose browser is not known (a line of a build before the key was kept) is nobody's to take off by request.
 *    Everyone else's "dismiss" arms a wait for the action button held for a second (W_NOTE; "what waits for the
 *    action button" above has why that fits the one-armed-wait rule, why a hold and not a press, and what it may
 *    take off).  Most owners have one browser, let in everywhere through the first Echo, and it is the one that
 *    brought the plugin: for them it is the hold, once, on the Echo the plugin went to.  That is the price of the
 *    copy needing none.
 *  - Named by what its bringer did not choose.  The label a note carries is the one the browser gave at its
 *    login, free text, and a login on a voucher is a member's to forge: a note "brought by Julian's phone" proved
 *    nothing.  So the note and the page's list of browsers (System) both show the first eight bytes of the
 *    browser's key beside the label (`key`), which the owner can hold against each other; a key whose first eight
 *    bytes are another browser's is 2^64 tries away.
 *  - That wait is not there for the asking.  Armed again every minute by the browser that brought the plugin, or
 *    by any vouched one, the ring showed `wait` for good.  It was bounded per note at first ("once for a note
 *    never asked about, then once an hour"), and that was neither a bound nor fair (a review, 2026-11-20): a
 *    plugin removed and copied again made a fresh note each round, each with its wait at once; and one browser
 *    arming the wait and saying "keep" in the same breath spent the hour for every other.  Now the bound is the
 *    Echo's and knows no notes: NOTE_ARMS (3) such waits armed in any NOTE_AGAIN_MS (an hour), withdrawn ones
 *    counted, whoever asks and for whichever notes; beyond it "later" and when.  Kept in state/web_wait
 *    (hold_save): a restart within the boot changes nothing, and after a reboot each of them counts as armed
 *    just then, so neither shortens the hour.  What is left to whoever holds an approved browser: three minutes
 *    of the `wait` ring an hour, during which nothing but a deliberate hold takes a note off; and it can spend
 *    the three itself, so that the owner's own browser, where that is the one that brought the plugin, is told
 *    "later".  That cost is taken and said (docs/SECURITY.md): the notes stay on every page meanwhile, at
 *    SYN_MAX of them a sync asks for the button again, and a browser let in with this Echo's own button that did
 *    not bring the plugin takes them off at once.
 *  - The event outlives the note.  A note taken off before Home Assistant took its `synced` event stays on file
 *    as "gone" (on no page, not counted among the standing ones) until the event went out: the core holds such
 *    events in memory only, and a restart between the dismissal and Home Assistant's next link lost it.  Every
 *    note kept, standing or gone, has a place for its event in the core (PLUGIN_HELD is SYN_KEPT), and when
 *    SYN_KEPT are kept, SYN_MAX of them gone with Home Assistant away through all of it, a sync is answered
 *    "button" (rule 10) instead of the oldest of those events being dropped for it.
 *  - Settled by what the flash has.  A note that is "pending" when hassmic starts, or whose install failed, is
 *    kept or dropped by the package in the plugin's folder (the store puts a package there whole or not at all:
 *    ps_stage_install): the one the note was written for, by its manifest's digest, and the note stands; a
 *    package there whose manifest cannot be read, and it stands as well (what is there cannot be said to be
 *    what was there before); anything else, and it goes.  It went by the version at first, and by "not installed"
 *    as the store's answer: an update that failed between its program and its manifest left the new program
 *    under the old manifest and no note (a review, 2026-11-20).
 *  - Not without its file.  state/plugins/.synced that is there and is no regular file, or cannot be read, is
 *    said in the log and fails closed: the notes it may hold are not known, so no plugin is taken on a voucher
 *    (rule 10: "button") until a start finds it readable or gone.
 * What the note does not say: which Echo it came from.  `node` is the name the voucher carried, and any member's
 * key makes a voucher in any member's name; the page words it so.
 * state/plugins/.synced (a dot: no plugin's id), a line each:
 *   "<when> <version> <id> <node>\t<the plugin's name>\t<the browser's label>\t<the browser's key, or ->\t<number> <pending|done|gone> <told> <the manifest's digest, or ->"
 * when: 0 where the clock was not set (clock_synced: 1970 would be a date); number: what a dismiss names, drawn at
 * random and never one that stands (it counted up from 1 after a start with no notes: a tab left open sent
 * "dismiss 1" and hit the next plugin's note); told: Home Assistant got the event (else it is given to the core
 * again at the next start); the digest: of the package the note was written for (a line of a build before it has
 * 0 or 1 there, `asked`, read as none: such a pending note is settled by its version).  A line of the builds
 * before (up to the label): kept; without a key the browser is unknown (so nobody's request takes it off: the
 * button), and it is taken as told.  A line that cannot be read is dropped, with a line in the log; so are
 * standing notes beyond SYN_MAX, which no build writes (struct syn_ids has room for SYN_MAX numbers: 24 "done"
 * lines in the file and a "dismiss all" wrote past it; a review, 2026-11-20). */
#define SYN_KEPT (2 * SYN_MAX)  /* ... and as many "gone" ones whose event Home Assistant has not taken yet */
_Static_assert(SYN_KEPT == PLUGIN_HELD, "every kept note's event, standing or gone, has a place in the core's queue (core.h)");
enum { SYN_PENDING, SYN_DONE, SYN_GONE };
static pthread_mutex_t syn_lk = PTHREAD_MUTEX_INITIALIZER;      /* syn[], nsyn, syn_blind: never held over the flash; lk may be held when it is taken, never the other way */
static pthread_mutex_t syn_save_lk = PTHREAD_MUTEX_INITIALIZER; /* one change at a time: file and syn[] in one order; never taken under lk */
static struct syn { long long at; long version; unsigned long n; int state, told, known, sealed; uint8_t pub[32], digest[32]; char id[PS_ID_MAX], node[64], name[49], by[64]; } syn[SYN_KEPT];
static int nsyn, SYN_NOTES = SYN_MAX;      /* HASSMIC_PLUGIN_NOTES for the tests: fewer */
static int NOTE_AGAIN_MS = 3600000;        /* HASSMIC_PLUGIN_NOTE_AGAIN for the tests: the time NOTE_ARMS waits for the hold may be armed in */
static int syn_blind;                      /* syn_lk: .synced is there and could not be read at start: no note is written, so no sync without the button */

static void syn_path(char *p, size_t cap) { snprintf(p, cap, "%s/plugins/.synced", state_dir()); }
static void syn_clean(char *s) { for (; *s; s++) if ((unsigned char)*s < 0x20 || *s == 0x7f) *s = ' '; }
static int syn_standing(void) { int k = 0; for (int i = 0; i < nsyn; i++) k += syn[i].state != SYN_GONE; return k; }     /* syn_lk held */
static void syn_drop(int i) { memmove(&syn[i], &syn[i + 1], sizeof syn[0] * (size_t)(nsyn - i - 1)); nsyn--; }           /* syn_lk held */

/* syn_lk held: a number for a new note, none that a note has now */
static unsigned long syn_number(void)
{
    for (;;) {
        uint8_t b[4]; unsigned long v; int k;
        ws_random(b, 4); v = (((unsigned long)b[0] << 24 | (unsigned long)b[1] << 16 | (unsigned long)b[2] << 8 | b[3]) % 999999999ul) + 1;   /* (9 digits: what "dismiss" reads) */
        for (k = 0; k < nsyn && syn[k].n != v; k++) ;
        if (k == nsyn) return v;
    }
}

/* syn_save_lk held.  0, or -1: not on the flash */
static int syn_save(void)
{
    char p[300], *t; size_t cap = SYN_KEPT * 500, n = 0; int k, bad;
    if (!(t = malloc(cap))) return -1;
    pthread_mutex_lock(&syn_lk);
    for (int i = 0; i < nsyn; i++) {
        char h[65] = "-", d[65] = "-"; if (syn[i].known) hex(h, syn[i].pub, 32); if (syn[i].sealed) hex(d, syn[i].digest, 32);
        n = app(t, n, cap, "%lld %ld %s %s\t%s\t%s\t%s\t%lu %s %d %s\n", syn[i].at, syn[i].version, syn[i].id, syn[i].node, syn[i].name, syn[i].by, h, syn[i].n,
                syn[i].state == SYN_DONE ? "done" : syn[i].state == SYN_GONE ? "gone" : "pending", syn[i].told, d);
    }
    k = nsyn;
    pthread_mutex_unlock(&syn_lk);
    syn_path(p, sizeof p);
    bad = !k ? unlink(p) && errno != ENOENT : statefile_write(p, 0600, t, n);
    free(t);
    if (bad) fprintf(stderr, "web: cannot write %s\n", p);
    return bad ? -1 : 0;
}

/* Is the package a note was written for in ID's folder now?  1: it is (its manifest has DIGEST; no DIGEST, a note
 * of a build before it: VERSION), or a package is there whose manifest cannot be read: the note stands.  0: the
 * folder has what it had, or nothing.  SCAN: the folder read anew first (after an install that failed).  No lock */
static int syn_there(const char *id, const uint8_t *digest, long version, int scan)
{
    const struct ps_plugin *l, *sp; int in;
    ps_lock();
    if (scan) ps_scan(&l);
    in = (sp = ps_find(id)) && sp->here && (sp->unread || (digest ? !memcmp(sp->m.manifest_digest, digest, 32) : sp->m.version == version));
    ps_unlock();
    return in;
}

/* at start, after the store was read (main(): plugin_start before web_start), no lock held */
static void syn_load(void)
{
    char p[300], line[700], again[SYN_KEPT][PS_ID_MAX]; FILE *f; int changed = 0, nagain = 0, lines = 0, over = 0; struct stat st;
    syn_path(p, sizeof p);
    if (!(f = ps_fopen(p))) {
        /* not there: no notes.  There and not to be read (no regular file, no right to it): the notes it may hold
         * are unknown, and a note written now would be written over them or not at all.  Said, and closed */
        if (!lstat(p, &st) || errno != ENOENT) {
            fprintf(stderr, "plugins: %s is there and cannot be read as the notes of plugins copied here (%s): no plugin is installed here on another Echo's voucher, without this Echo's action button, until a start of hassmic finds it readable or gone\n",
                    p, !lstat(p, &st) && !S_ISREG(st.st_mode) ? "it is no regular file" : "it cannot be opened");
            pthread_mutex_lock(&syn_lk); syn_blind = 1; pthread_mutex_unlock(&syn_lk);
        }
        return;
    }
    pthread_mutex_lock(&syn_save_lk);
    pthread_mutex_lock(&syn_lk);
    nsyn = 0;
    while (nsyn < SYN_KEPT && fgets(line, sizeof line, f)) {
        struct syn *n = &syn[nsyn]; char *t1, *t2, *t3, *t4, st_[12] = "", dg[70] = ""; int k = 0, bad = 1;
        line[strcspn(line, "\r\n")] = 0; lines++;
        memset(n, 0, sizeof *n);
        if (sscanf(line, "%lld %ld %24s %63[^\t]%n", &n->at, &n->version, n->id, n->node, &k) == 4 && n->at >= 0 && ps_id_ok(n->id) && node_ok(n->node)
            && (t1 = line[k] == '\t' ? line + k + 1 : NULL) && (t2 = strchr(t1, '\t'))) {
            *t2++ = 0; n->state = SYN_DONE; n->told = 1; bad = 0;
            if ((t3 = strchr(t2, '\t'))) {
                *t3++ = 0;
                if (!(t4 = strchr(t3, '\t'))) bad = 1;
                else {
                    *t4++ = 0;
                    n->known = strcmp(t3, "-") != 0;
                    if ((n->known && (strlen(t3) != 64 || unhex(n->pub, t3, 32))) || sscanf(t4, "%lu %11s %d %69s", &n->n, st_, &n->told, dg) < 3) bad = 1;
                    n->state = !strcmp(st_, "pending") ? SYN_PENDING : !strcmp(st_, "gone") ? SYN_GONE : SYN_DONE; n->told = !!n->told;
                    n->sealed = strlen(dg) == 64 && !unhex(n->digest, dg, 32);
                }
            }
            if (!bad) { snprintf(n->name, sizeof n->name, "%s", t1); snprintf(n->by, sizeof n->by, "%s", t2); }
        }
        if (bad) { fprintf(stderr, "plugins: line %d of %s cannot be read as the note of a plugin copied here: dropped\n", lines, p); changed = 1; continue; }
        if (n->state == SYN_PENDING) {
            /* hassmic ended between the note and the end of the install: what the folder has says which it was
             * (syn_lk is not the store's to wait under: syn_there takes ps_lock, which never takes syn_lk) */
            int in = syn_there(n->id, n->sealed ? n->digest : NULL, n->version, 0);
            changed = 1;
            if (!in) { fprintf(stderr, "plugins: the install of %s version %ld on a voucher was under way when hassmic ended, and its package is not in the plugin's folder: its note is dropped\n", n->id, n->version); continue; }
            fprintf(stderr, "plugins: the install of %s version %ld on a voucher was under way when hassmic ended, and its package is in the plugin's folder: its note stands\n", n->id, n->version);
            n->state = SYN_DONE; n->told = 0;
        }
        if (n->state == SYN_GONE && n->told) { changed = 1; continue; }     /* (kept for the event only, and that went out) */
        /* SYN_MAX standing at most (syn_begin writes no more: whatever dismisses them counts on it) */
        if (n->state != SYN_GONE && syn_standing() >= SYN_MAX) { over++; changed = 1; continue; }
        /* a number of its own: one of a build that counted them, or none, or one a line above has */
        for (k = 0; k < nsyn && syn[k].n != n->n; k++) ;
        if (!n->n || n->n > 999999999ul || k < nsyn) { nsyn++; syn[nsyn - 1].n = 0; syn[nsyn - 1].n = syn_number(); changed = 1; }
        else nsyn++;
    }
    if (over) fprintf(stderr, "plugins: %s names more than %d notes that stand, which hassmic never writes: %d beyond them dropped\n", p, SYN_MAX, over);
    if (fgets(line, sizeof line, f)) fprintf(stderr, "plugins: %s has more lines than notes are kept (%d): the rest is dropped\n", p, SYN_KEPT);
    for (int i = 0; i < nsyn; i++) if (!syn[i].told) snprintf(again[nagain++], PS_ID_MAX, "%s", syn[i].id);
    pthread_mutex_unlock(&syn_lk);
    fclose(f);
    if (changed) syn_save();
    pthread_mutex_unlock(&syn_save_lk);
    /* what Home Assistant did not hear before the restart: held in the core until its link is up (a place each:
     * SYN_KEPT of them at most, the core's PLUGIN_HELD) */
    for (int i = 0; i < nagain; i++) core_plugin_event(PLUGIN_SYNCED, again[i]);
}

/* M is about to be installed here on a voucher that named NODE, brought by the browser PUB: its note, on the flash
 * before anything is installed.  The note's number; 0 and WHY: no note, so no install without the button.  Not lk */
static unsigned long syn_begin(const struct ps_manifest *m, const char *node, const uint8_t pub[32], char *why, size_t cap)
{
    struct syn n; struct client *c; unsigned long r = 0; int blind, kept, gone;
    memset(&n, 0, sizeof n);
    /* (0: the clock was not set since this boot, and what it says then is no date; the page says so) */
    n.at = clock_synced() ? (long long)time(NULL) : 0; n.version = m->version; n.state = SYN_PENDING; memcpy(n.pub, pub, 32); n.known = 1;
    memcpy(n.digest, m->manifest_digest, 32); n.sealed = 1;
    snprintf(n.id, sizeof n.id, "%s", m->id); snprintf(n.node, sizeof n.node, "%s", node); snprintf(n.name, sizeof n.name, "%s", m->name);
    pthread_mutex_lock(&lk); snprintf(n.by, sizeof n.by, "%s", (c = client_of(pub)) ? c->label : "a browser no longer let in"); pthread_mutex_unlock(&lk);
    syn_clean(n.name); syn_clean(n.by);
    for (char *x = n.name; *x; x++) if (*x == '\t') *x = ' ';
    for (char *x = n.by; *x; x++) if (*x == '\t') *x = ' ';
    pthread_mutex_lock(&syn_save_lk);
    pthread_mutex_lock(&syn_lk);
    blind = syn_blind; kept = nsyn; gone = nsyn - syn_standing();
    /* (a kept "gone" note is an event Home Assistant has not had: none gives way, the sync asks for the button) */
    if (!blind && syn_standing() < SYN_NOTES && nsyn < SYN_KEPT) { n.n = r = syn_number(); syn[nsyn++] = n; }
    pthread_mutex_unlock(&syn_lk);
    if (blind) snprintf(why, cap, "this Echo could not read its notes of plugins copied here when it started (state/plugins/.synced): nothing is installed here without its action button until it can");
    else if (!r && kept >= SYN_KEPT) snprintf(why, cap, "Home Assistant has not been told yet of %d plugins that were copied to this Echo without its action button (it has had no link to this Echo since): until it has, install this one with the button", gone);
    else if (!r) snprintf(why, cap, "%d plugins were copied to this Echo without its action button and nobody has dismissed their notes: read and dismiss those first (they show on every page of this Echo), or install this one with the button", SYN_NOTES);
    else if (syn_save()) {
        pthread_mutex_lock(&syn_lk);
        for (int i = 0; i < nsyn; i++) if (syn[i].n == r) { syn_drop(i); break; }
        pthread_mutex_unlock(&syn_lk);
        snprintf(why, cap, "this Echo cannot write down that the plugin came without its action button (its storage): nothing is installed that way unsaid");
        r = 0;
    }
    pthread_mutex_unlock(&syn_save_lk);
    return r;
}

static int syn_told(void);

/* the install the note N was begun for ended: OK it stands (and is said in the log), else it is taken away */
static void syn_end(unsigned long n, int ok)
{
    pthread_mutex_lock(&syn_save_lk);
    pthread_mutex_lock(&syn_lk);
    for (int i = 0; i < nsyn; i++) if (syn[i].n == n) {
        if (ok) {
            char h[17]; hex(h, syn[i].pub, 8);
            syn[i].state = SYN_DONE;
            fprintf(stderr, "plugins: %s (%s) version %ld was installed here WITHOUT this Echo's action button, on a voucher of this network that named %s "
                            "(any Echo of the network can make such a voucher: the name is what it said, not proof of where the plugin came from); the package was brought by the browser \"%s\" (key %s...)\n",
                    syn[i].id, syn[i].name, syn[i].version, syn[i].node, syn[i].by, h);
        }
        else syn_drop(i);
        break;
    }
    pthread_mutex_unlock(&syn_lk);
    /* (with it, in the one write: Home Assistant heard of it during the install, where its link was up.  A write
     * that fails here leaves "pending" on the flash: the next start reads the store and comes to the same) */
    syn_told();
    syn_save();
    pthread_mutex_unlock(&syn_save_lk);
}

/* The install the note N was begun for failed in the store (ID, the package whose manifest has DIGEST).  The
 * store puts a package into the plugin's folder whole or not at all, and after that still writes the acceptance
 * and the `enabled` mark (ps_stage_install): a failure there leaves this package in the folder with "not
 * installed" as the answer, and the note went while files that came without the button stood unsaid.  So the note
 * is settled against what the folder has now, read anew, as a start settles a pending one (syn_there): this
 * package there, or one whose manifest cannot be read, and the note stands and Home Assistant is told; else it
 * goes */
static void syn_failed(unsigned long n, const char *id, const uint8_t digest[32])
{
    int in = syn_there(id, digest, 0, 1);
    if (in) fprintf(stderr, "plugins: the install of %s on a voucher failed after its package was put into its folder: it is there, so its note stands\n", id);
    syn_end(n, in);
    if (in) core_plugin_event(PLUGIN_SYNCED, id);
}

/* The `synced` events Home Assistant got since the last look (main.c core_plugin_told), marked on their notes: of
 * each id the oldest note not told yet; a "gone" one was kept for this only and goes.  1 when a note changed.
 * syn_save_lk held, not syn_lk, never core_lock (core_plugin_told takes it; the core calls nothing in here) */
static int syn_told(void)
{
    char id[PS_ID_MAX + 8]; int hit = 0;
    while (core_plugin_told(id, sizeof id)) {
        pthread_mutex_lock(&syn_lk);
        for (int i = 0; i < nsyn; i++) if (!syn[i].told && !strcmp(syn[i].id, id)) { syn[i].told = hit = 1; if (syn[i].state == SYN_GONE) syn_drop(i); break; }
        pthread_mutex_unlock(&syn_lk);
    }
    return hit;
}

/* The notes numbered in IDS go, those of them that stand ("done": a number names a note for good, so none that
 * came later, and a pending one has no page to be read on).  How many; HOW for the log.  Not lk */
static int syn_dismiss(const struct syn_ids *ids, const char *how)
{
    int gone = 0;
    pthread_mutex_lock(&syn_save_lk);
    pthread_mutex_lock(&syn_lk);
    for (int i = 0; i < nsyn; i++) {
        int k;
        for (k = 0; k < ids->k && ids->n[k] != syn[i].n; k++) ;
        if (k == ids->k || syn[i].state != SYN_DONE) continue;
        fprintf(stderr, "plugins: the note that %s version %ld came here without this Echo's action button was dismissed %s\n", syn[i].id, syn[i].version, how);
        /* (Home Assistant has not had its event: kept, on no page, until it has) */
        if (!syn[i].told) syn[i].state = SYN_GONE; else { syn_drop(i); i--; }
        gone++;
    }
    pthread_mutex_unlock(&syn_lk);
    if (gone) syn_save();
    pthread_mutex_unlock(&syn_save_lk);
    return gone;
}

static void note_withdraw(const char *why)      /* lk held: a wait to take notes off that no longer counts */
{
    if (!(login.state == 1 && login.kind == W_NOTE)) return;
    login.state = 3; login.end = now_ms(); login.why = "busy"; wait_over(login.end, 0, NULL);
    fprintf(stderr, "web: the wait to take a copied plugin's note off withdrawn (%s)\n", why);
    if (hooks->attention) hooks->attention(0);
}

static void ids_add(struct syn_ids *s, unsigned long n) { if (s->k < SYN_MAX) s->n[s->k++] = n; }     /* (SYN_MAX stand at most: syn_load, syn_begin) */

/* "dismiss <number>" | "dismiss all" (ONE 0) from the browser PUB, over the notes that stand now.  "done": gone;
 * "waiting": the action button held for a second takes them off (*HOLDS "note"); "busy": something else waits for
 * the button (*HOLDS: what; "note" where it is this browser's own wait for other notes than these, which the hold
 * will not take: "waiting" there sent the owner to hold for a note that stayed); "later": NOTE_ARMS such waits
 * were armed on this Echo within NOTE_AGAIN_MS, the next is due in *RETRY_MS; "none": no such note.  What a request
 * alone takes off: the notes another browser brought, for a browser of this Echo's button; its own, and everything
 * for a vouched browser, is the hold's (the section's comment, which also has why that wait is bounded and how).
 * Both in one request: the first go at once, the wait is for the rest */
static const char *syn_ask(const uint8_t pub[32], unsigned long one, const char **holds, long long *retry_ms)
{
    const char *r, *h; long long now = now_ms(), due = 0; int any = 0, direct, at = 0, k; struct client *c; char label[64], how[120];
    struct syn_ids off, press; off.k = press.k = 0;
    *holds = "none"; *retry_ms = 0;
    pthread_mutex_lock(&lk);
    login_expire(now);
    direct = (c = client_of(pub)) && c->button;
    snprintf(label, sizeof label, "%s", c ? c->label : "");
    pthread_mutex_lock(&syn_lk);
    for (int i = 0; i < nsyn; i++) if (syn[i].state == SYN_DONE && (!one || syn[i].n == one)) {
        any = 1;
        ids_add(direct && syn[i].known && memcmp(syn[i].pub, pub, 32) ? &off : &press, syn[i].n);
    }
    pthread_mutex_unlock(&syn_lk);
    /* the budget: the slot armed longest ago, free once NOTE_AGAIN_MS have passed since */
    for (int i = 1; i < NOTE_ARMS; i++) if (note_arms[i] < note_arms[at]) at = i;
    if (note_arms[at] && now - note_arms[at] < NOTE_AGAIN_MS) due = note_arms[at] + NOTE_AGAIN_MS - now;
    if (!any) r = "none";
    else if (!press.k) r = "done";
    else if (login.state == 1 && login.kind == W_NOTE && !memcmp(login.pub, pub, 32)) {
        /* its own wait is armed: for the notes of that moment.  All asked about now among them: the same wait */
        for (k = 0; k < press.k; k++) { int j; for (j = 0; j < syn_press.k && syn_press.n[j] != press.n[k]; j++) ; if (j == syn_press.k) break; }
        r = k == press.k ? "waiting" : "busy"; *holds = "note";
    }
    /* (busy before "later": what the button holds now is the nearer reason, and wait_arm leaves its mark) */
    else if (login.state != 1 && due) { r = "later"; *retry_ms = due; }
    else if ((h = wait_arm(pub, W_NOTE, "plugin notes", now))) { r = "busy"; *holds = h; }
    else {
        syn_press = press; r = "waiting"; *holds = "note";
        note_arms[at] = now; hold_dirty = 1; hold_save(now);        /* (the budget, on the flash with the wait) */
        fprintf(stderr, "web: taking %d copied plugin's note%s off waits for the action button held for a second (asked by the browser \"%s\")\n", press.k, press.k == 1 ? "" : "s", label);
    }
    pthread_mutex_unlock(&lk);
    hold_flush(!strcmp(r, "waiting"));
    if (off.k) { snprintf(how, sizeof how, "by the browser \"%s\", let in with this Echo's button", label); syn_dismiss(&off, how); }
    return r;
}

/* [{"n","at","id","name","version","node","by","key","dismiss"}]: every word of it a manifest's or a browser's own,
 * text to the page.  key: the first eight bytes of the bringing browser's key ("" unknown), which `by`'s author
 * did not choose (the section's comment).  dismiss: what ME's "dismiss" would do, "now" or "button".  Not lk */
static size_t syn_json(char *o, size_t cap, const uint8_t me[32])
{
    size_t n = 0; char nj[120], bj[160], kh[17]; struct client *c; int direct;
    pthread_mutex_lock(&lk); direct = (c = client_of(me)) && c->button; pthread_mutex_unlock(&lk);
    pthread_mutex_lock(&syn_lk);
    n = app(o, n, cap, "[");
    for (int i = 0, k = 0; i < nsyn; i++) {
        if (syn[i].state != SYN_DONE) continue;         /* (pending: its install is on its way, for seconds; gone: dismissed) */
        jesc(nj, sizeof nj, syn[i].name); jesc(bj, sizeof bj, syn[i].by);
        kh[0] = 0; if (syn[i].known) hex(kh, syn[i].pub, 8);
        n = app(o, n, cap, "%s{\"n\":%lu,\"at\":%lld,\"id\":\"%s\",\"name\":\"%s\",\"version\":%ld,\"node\":\"%s\",\"by\":\"%s\",\"key\":\"%s\",\"dismiss\":\"%s\"}", k++ ? "," : "",
                syn[i].n, syn[i].at, syn[i].id, nj, syn[i].version, syn[i].node, bj, kh, direct && syn[i].known && memcmp(syn[i].pub, me, 32) ? "now" : "button");
    }
    n = app(o, n, cap, "]");
    pthread_mutex_unlock(&syn_lk);
    return n;
}

/* ---- packages the Echo brings itself (docs/plugins.md 5.4's fourth way, 5.9's push): from the plugin index when
 * the owner asks on the page, for the wake word engine's plugin after a hassmic update, or pushed from the PC.
 * They go through the store's staging as an upload does and meet the same check; on top of it, what comes this way
 * has the index's listing or it is nothing: the page's "Install from file" is where a sideload comes in, with its
 * warning.  For the browser's fetch the install is then the upload's, word for word (/api/plugins/install: the
 * button for what was not accepted).  For the two nobody stands at the Echo for, only what needs no button is
 * installed: listed, the author key that is pinned, an accepted section that is what was accepted or less (the
 * store's rule, ps_stage_check, which also holds "never back" over every package of the plugin, whichever plugin
 * API each is for); anything else is left for the owner and said in the log. */

/* the bytes into the store's staging and through its check: 0 and what it says, or -1 and why.  Only as the slot's
 * holder, in CHECKING */
static int bring_check(unsigned gen, const unsigned char *d, size_t len, struct ps_manifest *m, int *tier, int *button, struct ps_changes *ch, char *err, size_t cap)
{
    if (len > PS_PACKAGE_MAX) { snprintf(err, cap, "a package is at most %d MB", PS_PACKAGE_MAX >> 20); return -1; }
    if (ps_stage_begin(gen, (long)len, err, cap) || ps_stage_chunk(gen, 0, d, len, err, cap)) return -1;
    return ps_stage_check(m, tier, button, ch, err, cap);
}

/* What the Echo brings itself, or is pushed, has the plugin index's listing (what it requires the check held to this
 * build's scopes: a package for a newer hassmic is refused there).  WHAT names where it came from, in the reason.
 * 0; -1 not to be installed this way */
static int bring_ok(const struct ps_manifest *m, int tier, const char *what, char *err, size_t cap)
{
    (void)m;
    if (tier == PS_SIDELOAD) { snprintf(err, cap, "%s has no listing by the plugin index's key: nothing but a listed plugin is installed this way (a file is installed with \"Install from file\")", what); return -1; }
    return 0;
}

/* One fetch, for the claim b->gen: a browser's.  While it looks and downloads the slot is FETCHING and nothing of
 * the staging file is touched: a cancel lets go of the slot at once and this thread finds out when libcurl next
 * asks (b->stop).  For the staging file it moves the slot to CHECKING, which nobody takes.  The version is the
 * index's choice for this build (pf_best: what it requires, not withdrawn), so one download */
static void *bring_thread(void *arg)
{
    struct bring *b = arg; struct pf_asset a; char err[240] = "", what[80]; int got = 0, tier = 0, button = 1, checking = 0, mine, stopped, r;
    struct ps_manifest *m = malloc(sizeof *m); struct ps_changes *ch = malloc(sizeof *ch); unsigned char *d = NULL; size_t len = 0; long left;
    if (!m || !ch) { snprintf(err, sizeof err, "no memory"); goto out; }
    if (pf_look(b->id, PF_ASKED, &b->stop, err, sizeof err)) goto out;
    if (!pf_best(b->id, &a, err, sizeof err)) goto out;
    /* (not downloaded for nothing: the check would refuse it, ps_stage_check) */
    if (a.tier != PS_OFFICIAL && !ps_community(-1)) { snprintf(err, sizeof err, "%.24s is a community plugin, and community plugins are switched off on this Echo: switch them on on its Plugins page first", a.id); goto out; }
    snprintf(what, sizeof what, "the index's %.24s-%ld.hmp", a.id, a.version);
    if ((left = (long)((b->until - now_ms()) / 1000)) < 1) { snprintf(err, sizeof err, "the plugin index took longer than %d s for it: given up, nothing installed", FETCH_MS / 1000); goto out; }
    { long long t0 = now_ms();
      if (pf_download(&a, left, &b->stop, &d, &len, err, sizeof err)) {
          /* (the transfer had what was left of the fetch's time, in whole seconds: used up, it is the fetch's end) */
          if (!b->stop && now_ms() - t0 >= left * 1000 - 200) snprintf(err, sizeof err, "the plugin index took longer than %d s for it: given up, nothing installed", FETCH_MS / 1000);
          goto out;
      } }
    pthread_mutex_lock(&lk); checking = pl_move(b->gen, PL_FETCHING, PL_CHECKING); pthread_mutex_unlock(&lk);
    if (!checking) goto out;                        /* the slot is someone else's by now */
    r = bring_check(b->gen, d, len, m, &tier, &button, ch, err, sizeof err);
    if (r) goto out;
    /* the index named a manifest, a tier, an id and a version; the package's listing names them too, signed by the
     * same key: where they differ, what the owner was offered is not what came */
    if (strcmp(m->id, a.id) || m->version != a.version || memcmp(m->manifest_digest, a.manifest, 32) || tier != a.tier) {
        snprintf(err, sizeof err, "%s is %s version %ld inside: not what the index names, and not installed", what, m->id, m->version); goto out;
    }
    if (bring_ok(m, tier, what, err, sizeof err)) goto out;
    got = 1; err[0] = 0;
out:
    pthread_mutex_lock(&lk); mine = pl.gen == b->gen && pl.by == b; stopped = b->stop; pthread_mutex_unlock(&lk);
    if (!mine || stopped) {
        /* cancelled by its browser, or the slot went to someone the rules let have it.  Still this claim's only
         * where the cancel came while the staging file was being worked on: then the end is here */
        fprintf(stderr, "plugins: the fetch of %s was cancelled\n", b->id);
        if (mine && checking) pl_close(b->gen, PL_NONE, 0, NULL);
        else if (mine) { pthread_mutex_lock(&lk); if (pl_is(b->gen, PL_FETCHING)) pl_reset(); pthread_mutex_unlock(&lk); }
    }
    else if (!got) {
        fprintf(stderr, "plugins: %s not fetched from the plugin index: %s\n", b->id, err);
        if (checking) pl_close(b->gen, PL_FAILED, 0, err);
        else {
            pthread_mutex_lock(&lk);
            if (pl_is(b->gen, PL_FETCHING)) { pl.state = PL_FAILED; pl.at = now_ms(); pl.by = NULL; snprintf(pl.err, sizeof pl.err, "%s", err); }
            pthread_mutex_unlock(&lk);
        }
    }
    else {
        /* the browser's from here: its dialog, then /api/plugins/install as after an upload's check */
        fprintf(stderr, "plugins: %s version %ld fetched from the plugin index and checked: %s%s\n", m->id, m->version, tier == PS_OFFICIAL ? "official" : "community", button ? ", to be accepted with the action button" : ", nothing new to accept");
        int gone;
        pthread_mutex_lock(&lk);
        if (!(gone = b->stop || !pl_is(b->gen, PL_CHECKING))) {     /* (a cancel that came just now is one all the same) */
            pl_check_n = check_json(m, tier, button, ch, pl_check, sizeof pl_check);
            if (pl_check_n >= sizeof pl_check) pl_check_n = 0;
            memcpy(pl.digest, m->manifest_digest, 32); pl.by = NULL;
            pl.state = PL_CHECKED; pl.at = now_ms();
        }
        pthread_mutex_unlock(&lk);
        if (gone) pl_close(b->gen, PL_NONE, 0, NULL);
    }
    pthread_mutex_lock(&lk); if (pl.by == b) pl.by = NULL; pthread_mutex_unlock(&lk);
    free(m); free(ch); free(d); free(b);
    return NULL;
}

/* PUB the browser that asked.  0: its thread runs; -1 and why */
static int bring_start(const char *id, const uint8_t *pub, char *err, size_t cap)
{
    struct bring *b = calloc(1, sizeof *b); pthread_t t; unsigned gen;
    if (!b) { snprintf(err, cap, "no memory"); return -1; }
    /* claimed in CHECKING first: what an install before left in the staging file (a dialog nobody answered) goes
     * while the slot is nobody's to take */
    pthread_mutex_lock(&lk); gen = pl_claim(pub, 0, PL_CHECKING, id, err, cap); pthread_mutex_unlock(&lk);
    if (!gen) { free(b); return -1; }
    ps_stage_drop();
    snprintf(b->id, sizeof b->id, "%s", id); b->gen = gen; b->until = now_ms() + FETCH_MS;
    pthread_mutex_lock(&lk); pl.by = b; pl_move(gen, PL_CHECKING, PL_FETCHING); pthread_mutex_unlock(&lk);
    if (pthread_create(&t, NULL, bring_thread, b)) {
        pthread_mutex_lock(&lk); if (pl.gen == gen) pl_reset(); pthread_mutex_unlock(&lk);
        free(b); snprintf(err, cap, "no thread"); return -1;
    }
    pthread_detach(t);
    return 0;
}

/* A package pushed from the PC (ota.c, which has checked the update key's signature over it): installed under the
 * same rule, at once, the answer in RES as the push port gives them ("OK ..." | "FAILED ...").  0 installed.
 * Claimed in CHECKING, which nobody takes: it is all done within this call */
int web_plugin_push(const uint8_t *data, size_t len, char *res, size_t cap)
{
    char err[280] = ""; int tier = 0, button = 1, ok = 0; unsigned gen;
    struct ps_manifest *m = malloc(sizeof *m); struct ps_changes *ch = malloc(sizeof *ch);
    if (!m || !ch) { free(m); free(ch); snprintf(res, cap, "FAILED no memory"); return -1; }
    pthread_mutex_lock(&lk); gen = pl_claim(NULL, 1, PL_CHECKING, "", err, sizeof err); pthread_mutex_unlock(&lk);
    if (!gen) { free(m); free(ch); snprintf(res, cap, "FAILED %s", err); return -1; }
    if (bring_check(gen, data, len, m, &tier, &button, ch, err, sizeof err) || bring_ok(m, tier, "the package", err, sizeof err)) { /* err says */ }
    else if (button && !ch->update) snprintf(err, sizeof err, "%s was never accepted on this Echo: install it on the settings page first, where its capabilities are accepted with the action button", m->id);
    else if (button) {
        /* (the line is the manifest's words, UTF-8: cut at a character's end, as the store cuts them) */
        const char *line = ch->n ? ch->c[0].line : "it asks for more";
        snprintf(err, sizeof err, "version %ld of %s is not what was accepted on this Echo (%.*s): update it on the settings page, with the action button", m->version, m->id, ps_cut(line, strlen(line), 100), line);
    }
    else {
        int go, k = -1;
        own_delay(m->id);
        pthread_mutex_lock(&lk); if ((go = pl_move(gen, PL_CHECKING, PL_INSTALLING))) { snprintf(pl.id, sizeof pl.id, "%s", m->id); memcpy(pl.digest, m->manifest_digest, 32); } pthread_mutex_unlock(&lk);
        ok = go && !(k = plugin_install_now("update: pushed from the PC", gen, m->manifest_digest, PS_NO_PRESS, 1, err, sizeof err));
        /* what the check found a moment ago no longer holds (the plugin removed meanwhile): the store wrote nothing */
        if (k == PS_NEEDS_BUTTON) snprintf(err, sizeof err, "%s is no longer what was accepted on this Echo (it changed while the package was checked): install it on the settings page, with the action button", m->id);
    }
    if (ok) snprintf(res, cap, "OK %s version %ld installed", m->id, m->version);
    else { snprintf(res, cap, "FAILED %s", err); fprintf(stderr, "plugins: a pushed package not installed: %s\n", err); }
    pl_close(gen, PL_NONE, ok, NULL);
    free(m); free(ch);
    return ok ? 0 : -1;
}

/* lk held: where this browser's install stands, as the page reads it; ID and the reason (escaped) with it.  Another
 * browser's install is none of this one's: "none", or what it was told when its own ended without it */
static const char *pl_word(const uint8_t pub[32], char *id, size_t idcap, char *ej, size_t ecap)
{
    const char *told;
    pl_settle(now_ms()); id[0] = ej[0] = 0;
    if (!pl_mine(pub)) { if (!(told = pl_told(pub))) return "none"; jesc(ej, ecap, told); return "failed"; }
    snprintf(id, idcap, "%s", pl.id); jesc(ej, ecap, pl.err);
    switch (pl.state) {
    case PL_FETCHING: return "fetching";
    case PL_CHECKING: return pl.by ? "fetching" : "none";      /* a fetched package's check; an upload's is the request's own */
    case PL_CHECKED: return "checked";
    case PL_WAITS: case PL_INSTALLING: return "waiting";
    case PL_INSTALLED: return "installed";
    case PL_FAILED: return "failed";
    case PL_SECOND: return "second";
    default: return "none";
    }
}

/* Another Echo's copy of what a plugin keeps (5.4: storage only when ticked), in pieces as an upload is: one at a time,
 * a browser's, held in memory (the accepted kB, 1024 at most, and the tar's headers), given up after a minute without
 * a piece.  Each piece sealed (seal2), opened as it comes.  What came is the source's: its first line says whether
 * the plugin there is a wake word plugin, and the tar follows.  A wake word plugin's data never goes into a plugin
 * that is not one here: what it kept while it heard the microphone must not go out with what this one may do (3.3's
 * seal; S4 of a review, 2026-11-21; the page does not offer it either).  The store checks every key and the kB this
 * Echo's owner accepted before anything is replaced */
#define STORAGE_HEAD "hassmic storage 1 wake="
/* in front of an answer sealed under a nonce of the Echo's (seal2): this, the nonce as 32 hex digits, a line end */
#define SEALED_HEAD "hassmic sealed 3 "
#define STORAGE_IN_MAX ((PS_KEYS + 4) * 512 + (1 << 20) + PS_KEYS * 512 + 64)
/* A size a request's body names: 1 to 9 digits and nothing else (a line's end allowed), else -1.  atol() of a body was
 * undefined past a long, which on a 32-bit Echo is 2^31 (a review of 6631974, 7); every size taken here is far
 * under 10^9 */
static long body_size(const char *s)
{
    long v = 0; int n = 0;
    if (!s) return -1;
    for (; *s >= '0' && *s <= '9'; s++) { if (++n > 9) return -1; v = v * 10 + (*s - '0'); }
    if (*s == '\r') s++;
    if (*s == '\n') s++;
    return n && !*s ? v : -1;
}

static struct { pthread_mutex_t mu; uint8_t pub[32]; char id[PS_ID_MAX]; uint8_t *b; size_t size, have; long long at; } sin_ = { .mu = PTHREAD_MUTEX_INITIALIZER };
static int storage_in(const uint8_t pub[32], const char *ctr, const char *id, const char *what, const char *line, const char *body, size_t blen, char *err, size_t errsz)
{
    long off = 0; int k = 0, rc = -1; long long now = now_ms();
    pthread_mutex_lock(&sin_.mu);
    if (sin_.b && now - sin_.at > 60000) { free(sin_.b); sin_.b = NULL; }
    if (!strcmp(what, "begin")) {
        long size = body_size(line);
        if (sin_.b && memcmp(sin_.pub, pub, 32)) snprintf(err, errsz, "another browser is copying a plugin's data here just now");
        else if (size <= 0 || size > STORAGE_IN_MAX) snprintf(err, errsz, "not a plugin's data (%ld bytes)", size);
        else {
            free(sin_.b);
            if (!(sin_.b = malloc((size_t)size))) snprintf(err, errsz, "no memory");
            else { memcpy(sin_.pub, pub, 32); snprintf(sin_.id, sizeof sin_.id, "%s", id); sin_.size = (size_t)size; sin_.have = 0; sin_.at = now; rc = 0; }
        }
    } else if (!sin_.b || memcmp(sin_.pub, pub, 32) || strcmp(sin_.id, id)) snprintf(err, errsz, "no copy of this plugin's data was begun (or it waited longer than a minute)");
    else if (sscanf(what, "chunk/%9ld%n", &off, &k) == 1 && !what[k]) {
        if (off != (long)sin_.have || blen > sin_.size - sin_.have) snprintf(err, errsz, "a piece out of order");
        else if (memcpy(sin_.b + sin_.have, body, blen), seal2(pub, ctr, NULL, "hassmic seal 2", sin_.b + sin_.have, blen)) snprintf(err, errsz, "not logged in");
        else { sin_.have += blen; sin_.at = now; rc = 0; }
    } else if (!strcmp(what, "done")) {
        uint8_t *b = sin_.b; size_t n = sin_.have, size = sin_.size, hl = strlen(STORAGE_HEAD); const struct ps_plugin *sp; int wake = -1, here = 0;
        sin_.b = NULL;
        pthread_mutex_unlock(&sin_.mu);
        ps_lock(); if ((sp = ps_find(id))) here = !!(sp->m.caps & PM_CAPBIT(PM_CAP_WAKE)); ps_unlock();
        if (n >= hl + 2 && !memcmp(b, STORAGE_HEAD, hl) && (b[hl] == '0' || b[hl] == '1') && b[hl + 1] == '\n') wake = b[hl] == '1';
        if (n != size) snprintf(err, errsz, "not all of it came");
        else if (wake < 0) snprintf(err, errsz, "not a plugin's data as an Echo sends it (sealed, with its first line)");
        else if (wake && !here) snprintf(err, errsz, "this is a wake word plugin's data, and here the plugin is not one: what it kept while it heard the microphone stays where it is");
        else rc = plugin_storage_replace(id, b + hl + 2, n - hl - 2, err, errsz);
        crypto_wipe(b, n); free(b);
        return rc;
    } else snprintf(err, errsz, "not found");
    pthread_mutex_unlock(&sin_.mu);
    return rc;
}

/* A piece of an upload (the package's, a plugin's storage, a file of an item): signed and taken once like any write,
 * but its counter not written to the flash (one write a piece wore it; the upload it belongs to was begun by a write
 * that was).  By the route the handler below matches, whole: by strstr, a plugin, a list or an item named "chunk"
 * or "upload" made every write of its (enable, remove, set ...) a piece, never kept over a restart, and one
 * captured on the network was taken again after the next one (a review of 6631974, 2).  The bounds are the
 * handler's own (%9ld: no number the long of a 32-bit Echo cannot hold) */
static int piece_route(const char *path)
{
    char id[PS_ID_MAX + 8], list[40], item[48], suffix[24]; long off; int k = 0;
    if (sscanf(path, "/api/plugins/upload/%9ld%n", &off, &k) == 1 && k && !path[k]) return 1;
    k = 0;
    if (sscanf(path, "/api/plugins/%24[a-z0-9-]/storage/chunk/%9ld%n", id, &off, &k) == 2 && k && !path[k]) return 1;
    k = 0;
    return sscanf(path, "/api/plugins/%24[a-z0-9-]/item/%32[a-z0-9_]/%40[a-z0-9_-]/chunk/%20[.a-zA-Z0-9]/%9ld%n", id, list, item, suffix, &off, &k) == 5 && k && !path[k];
}

static void plugins_api(int fd, struct req *r)
{
    char err[240] = "", o[900], ej[300], id[PS_ID_MAX + 8] = "", list[40] = "", item[48] = "", suffix[24] = "", name[40] = ""; long off = 0; int rc = -1, write = strcmp(r->method, "GET"), k = 0;
    uint8_t pub[32]; const struct ps_plugin *sp;
    int piece = piece_route(r->path);
    /* a GET of a plugin's storage is taken once over restarts too, as a write is: its answer is what the plugin keeps
     * (Q2 of a review, 2026-11-21; the answer's own nonce is the other half).  The route as its handler below reads it:
     * by strstr, an id that begins with "storage" (storagebox) found "/storagebox/storage" first, and that GET was not
     * kept (a review of 7144548, 5) */
    int kept = 0;
    if (!write) { char sid[PS_ID_MAX + 8]; int sk = 0; kept = sscanf(r->path, "/api/plugins/%24[a-z0-9-]/storage%n", sid, &sk) == 1 && sk && !r->path[sk]; }
    if (!signed_ok(r, (write && !piece) || kept) || unhex(pub, r->pub, 32)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); return; }
    if (!write && !strcmp(r->path, "/api/plugins")) {
        size_t cap = 262144, n; char *t = malloc(cap), why[400], wj[500]; int prob; char pid[PS_ID_MAX] = "", pe[300] = ""; const char *word;
        if (!t) { respond_sjson(fd, r, 503, "{\"error\":\"no memory\"}"); return; }
        n = plugin_json(t, cap - 12000);       /* (the tail: the wake engine's reason, the install, SYN_MAX notes of 560 bytes at their longest) */
        prob = core_wake_problem(why, sizeof why); jesc(wj, sizeof wj, why);
        const char *holds;      /* what the press would serve now: "plugin" while this page's install waits for it */
        char sj[400] = "";       /* the second screen's words while this browser's install waits for it */
        pthread_mutex_lock(&lk); word = pl_word(pub, pid, sizeof pid, pe, sizeof pe); holds = holds_word(); if (!strcmp(word, "second")) jesc(sj, sizeof sj, pl.second); pthread_mutex_unlock(&lk);
        if (n && t[n - 1] == '}') n--;
        n += (size_t)snprintf(t + n, cap - n, ",\"wake_problem\":%d,\"wake_why\":\"%s\",\"install\":{\"state\":\"%s\",\"id\":\"%s\",\"error\":\"%s\",\"holds\":\"%s\",\"second\":\"%s\"},\"node\":\"%s\",\"synced\":", prob, wj, word, pid, pe, holds, sj, core_node_name());
        n += syn_json(t + n, cap - n, pub);     /* what came here on another Echo's voucher, without this one's button, until dismissed */
        /* whether Home Assistant set the device key: until then nothing it sets reaches a plugin's entities or actions
         * (proto_esphome.c plugin_command: the keyed link only), and the cards say so (K4 of a review of 92ac2e1) */
        n += (size_t)snprintf(t + n, cap - n, ",\"ha_key\":%d", core_ha_keyed());
        /* community plugins switched on (2.7), and how this model's policy is written, for the warning that comes
         * before they go on where the sandbox misses a wall: device.conf's INSTALL (the Makefile's INSTALL_METHOD) */
        n += (size_t)snprintf(t + n, cap - n, ",\"community\":%s,\"install_method\":\"%s\"", ps_community(-1) ? "true" : "false", install_method());
        /* the revocation list this Echo has: its serial (a time), how long ago it last had word that it is the
         * newest (-1 not known), and the week's warning (5.6) */
        { char sw[400], sj[500]; int st = pf_revoked_stale(sw, sizeof sw); jesc(sj, sizeof sj, st ? sw : "");
          n += (size_t)snprintf(t + n, cap - n, ",\"revoked\":{\"serial\":%lld,\"least\":%lld,\"age_s\":%lld,\"stale\":\"%s\"}", pi_revoked_serial(), pi_least_revoked(), pi_revoked_age(), sj); }
        n += (size_t)snprintf(t + n, cap - n, "}");
        respond_s(fd, r, 200, "application/json", NULL, t, n); free(t); return;
    }
    if (!write && !strcmp(r->path, "/api/plugins/index")) {
        /* as long as the list is: a plugin's entry is some 700 bytes at its longest, and the index has as many as
         * its 1 MB hold (plugin_fetch.c pf_json, plugin_index.h) */
        char *t = NULL; size_t n = pf_json(&t);
        if (!n) { respond_sjson(fd, r, 503, "{\"error\":\"no memory\"}"); return; }
        respond_s(fd, r, 200, "application/json", NULL, t, n); free(t); return;
    }
    if (!write && !strcmp(r->path, "/api/plugins/fetch")) {
        size_t cap = sizeof pl_check + 600, n; char *t = malloc(cap), ej[300], pid[PS_ID_MAX]; const char *word;
        if (!t) { respond_sjson(fd, r, 503, "{\"error\":\"no memory\"}"); return; }
        pthread_mutex_lock(&lk);
        word = pl_word(pub, pid, sizeof pid, ej, sizeof ej);
        n = (size_t)snprintf(t, cap, "{\"state\":\"%s\",\"id\":\"%s\",\"error\":\"%s\"", word, pid, ej);
        if (pl_mine(pub) && pl.state == PL_CHECKED && pl_check_n) n += (size_t)snprintf(t + n, cap - n, ",\"check\":%.*s", (int)pl_check_n, pl_check);
        n += (size_t)snprintf(t + n, cap - n, "}");
        pthread_mutex_unlock(&lk);
        respond_s(fd, r, 200, "application/json", NULL, t, n); free(t); return;
    }
    if (!write && !strcmp(r->path, "/api/plugins/sync")) {
        /* For the page's "Copy plugins" (5.4): of each plugin that runs here its package by the manifest's digest and
         * its members' sizes, and each item of its file lists by a digest of its files, so that the page sends an
         * Echo only what it has not got.  Read when that table is shown, not with every look at the page: the
         * digests are over every byte of every item.
         * The store's lock only for the names (which plugins, which items): the digests are megabytes read from the
         * flash, and were made with the lock held, the broker and every other page waiting behind one browser's
         * table.  What changes between the names and the reading shows as it is then: an item deleted has no digest
         * and is left out, one replaced has its new one, a plugin removed lists no package.  The page compares and
         * carries; each Echo checks what it is given.
         * The answer is as long as what there is to say (dyn): 16 plugins with 64 items in each of their lists are
         * megabytes at the bounds, a few hundred bytes on an Echo as people have it.  It is whole or it is an error;
         * the fixed 64 kB before were cut where they ended, and the page read that as "cannot be reached" */
        struct snap { int plugin; char list[33], item[PS_ITEM_ID_MAX]; } *sn = NULL, *sn2; size_t nsn = 0, csn = 0;
        struct { char id[PS_ID_MAX]; uint8_t md[32]; int storage; } pg[PS_MAX]; int npg = 0, np, oom = 0;
        struct dyn d = { .max = SYNC_ANSWER_MAX }; char dh[65]; const struct ps_plugin *l;
        struct ps_item *items = malloc(sizeof *items * PS_ITEMS);
        if (!items) { respond_sjson(fd, r, 503, "{\"error\":\"no memory\"}"); return; }
        ps_lock(); np = ps_list(&l);
        for (int i = 0; i < np && npg < PS_MAX && !oom; i++) {
            const struct ps_plugin *q = &l[i];
            if (!q->here || q->unread) continue;                /* a card that cannot run: nothing of it travels */
            snprintf(pg[npg].id, sizeof pg[0].id, "%s", q->m.id); memcpy(pg[npg].md, q->m.manifest_digest, 32); pg[npg].storage = !!(q->m.caps & PM_CAPBIT(PM_CAP_STORAGE));
            for (int si = 0; si < q->m.nsettings && !oom; si++) {
                const struct ps_setting *st = &q->m.settings[si]; int ni;
                if (st->type != PS_FILES) continue;
                ni = ps_items(q, st->name, items, PS_ITEMS);
                if (nsn + (size_t)ni > csn) {
                    csn = (nsn + (size_t)ni) * 2;
                    if (!(sn2 = realloc(sn, csn * sizeof *sn))) { oom = 1; break; }
                    sn = sn2;
                }
                for (int k = 0; k < ni; k++, nsn++) { sn[nsn].plugin = npg; snprintf(sn[nsn].list, sizeof sn[0].list, "%s", st->name); snprintf(sn[nsn].item, sizeof sn[0].item, "%s", items[k].id); }
            }
            npg++;
        }
        ps_unlock();
        free(items);
        if (oom) { free(sn); respond_sjson(fd, r, 503, "{\"error\":\"no memory\"}"); return; }
        dapp(&d, "{\"node\":\"%s\",\"plugins\":[", core_node_name());
        for (int i = 0; i < npg && !d.bad; i++) {
            static const char *const mem[] = { "manifest.json", "manifest.sig", "plugin", "LICENSE", "listing.sig" };
            hex(dh, pg[i].md, 32);
            dapp(&d, "%s{\"id\":\"%s\",\"manifest\":\"%s\",\"pkg\":[", i ? "," : "", pg[i].id, dh);
            for (int k = 0, c = 0; k < 5; k++) {
                void *data = NULL; size_t dn = 0; long total = 0;
                if (ps_pkg_read(pg[i].id, mem[k], 0, 0, &data, &dn, &total)) continue;
                free(data); dapp(&d, "%s{\"name\":\"%s\",\"size\":%ld}", c++ ? "," : "", mem[k], total);
            }
            dapp(&d, "],\"items\":{");
            for (size_t k = 0, c = 0; k < nsn && !d.bad; k++) {
                uint8_t dg[32];
                if (sn[k].plugin != i || ps_item_digest(pg[i].id, sn[k].list, sn[k].item, dg)) continue;
                hex(dh, dg, 32); dapp(&d, "%s\"%s/%s\":\"%s\"", c++ ? "," : "", sn[k].list, sn[k].item, dh);
            }
            dapp(&d, "}");
            /* what it keeps, as one digest (empty: nothing), for a copy the owner ticks; never without that */
            if (pg[i].storage) { uint8_t dg[32]; long long used = ps_storage_used(pg[i].id, NULL); if (!ps_storage_digest(pg[i].id, dg)) { hex(dh, dg, 32); dapp(&d, ",\"storage\":\"%s\",\"storage_size\":%lld", dh, used); } }
            dapp(&d, "}");
        }
        dapp(&d, "]}");
        free(sn);
        if (d.bad) {
            fprintf(stderr, "web: what this Echo's plugins have, for a copy to another Echo, was not sent: %s\n", d.bad == 2 ? "it is more than an answer may be" : "no memory");
            free(d.t); respond_sjson(fd, r, 503, d.bad == 2 ? "{\"error\":\"this Echo's plugins have more files than it can list in one answer\"}" : "{\"error\":\"no memory\"}"); return;
        }
        respond_s(fd, r, 200, "application/json", NULL, d.t, d.n); free(d.t); return;
    }
    /* (nine digits each: a longer number is no offset into a package of 4 MB, and one that does not fit the type
     * is not sscanf's to convert) */
    long plen = 0;
    if (!write && sscanf(r->path, "/api/plugins/%24[a-z0-9-]/pkg/%20[a-zA-Z.]/%9ld/%9ld%n", id, suffix, &off, &plen, &k) == 4 && !r->path[k]) {
        /* a piece of the package this Echo runs, for an approved browser to carry to another Echo.  Nothing in a
         * package is secret (it is what was uploaded, and official ones are on GitHub); what the other Echo makes
         * of it is its own check's business */
        void *d = NULL; size_t dn = 0; long total = 0;
        if (plen < 0 || plen > ART_CHUNK_MAX || ps_pkg_read(id, suffix, off, plen, &d, &dn, &total)) { respond_sjson(fd, r, 404, "{\"error\":\"no such package\"}"); return; }
        respond_s(fd, r, 200, "application/octet-stream", NULL, d, dn); free(d); return;
    }
    if (!write && sscanf(r->path, "/api/plugins/%24[a-z0-9-]/storage%n", id, &k) == 1 && !r->path[k]) {
        /* what the plugin keeps, for an approved browser to carry to another Echo when the owner ticks it.  Nothing
         * in it is a secret of hassmic's; it is the plugin's (a token it was issued, perhaps), and the owner's to
         * copy: sealed for this browser (seal2, under a nonce of this answer's own, which goes in front in clear:
         * SEALED_HEAD and its hex), its first line whether the plugin is a wake word plugin here (the target's rule,
         * storage_in): `wake` in its manifest, or a wipe of what it was given under the seal still due (Q7 of a review,
         * 2026-11-21: an update without `wake` whose old process had not ended said wake=0).  The request is one this
         * Echo takes once over its restarts too (signed_ok's persist, above) */
        size_t dn = 0, hl, nl; void *d = ps_storage_tar(id, &dn); uint8_t *o, nonce[16]; int wake; char head[40], nh[64];
        if (!d) { respond_sjson(fd, r, 404, "{\"error\":\"it keeps no data here\"}"); return; }
        wake = ps_sealed(id);
        hl = (size_t)snprintf(head, sizeof head, "%s%d\n", STORAGE_HEAD, wake);
        ws_random(nonce, sizeof nonce);
        memcpy(nh, SEALED_HEAD, strlen(SEALED_HEAD)); nl = strlen(SEALED_HEAD); hex(nh + nl, nonce, 16); nl += 32; nh[nl++] = '\n';
        if (!(o = malloc(nl + hl + dn))) { crypto_wipe(d, dn); free(d); respond_sjson(fd, r, 503, "{\"error\":\"no memory\"}"); return; }
        memcpy(o, nh, nl); memcpy(o + nl, head, hl); memcpy(o + nl + hl, d, dn); crypto_wipe(d, dn); free(d);
        if (seal2(pub, r->ctr, nonce, "hassmic seal back 3", o + nl, hl + dn)) { free(o); respond_sjson(fd, r, 401, "{\"error\":\"not logged in\"}"); return; }
        respond_s(fd, r, 200, "application/octet-stream", NULL, o, nl + hl + dn); free(o); return;
    }
    if (!write && sscanf(r->path, "/api/plugins/%24[a-z0-9-]/file/%32[a-z0-9_]/%40[a-z0-9_-]/%20[.a-zA-Z0-9]%n", id, list, item, suffix, &k) == 4 && !r->path[k]) {
        int f = -1; struct stat st; char *data = NULL;
        ps_lock(); if ((sp = ps_find(id))) f = ps_item_open(sp, list, item, suffix, 0); ps_unlock();
        if (f >= 0 && !fstat(f, &st) && st.st_size <= (16 << 20) && (data = malloc((size_t)st.st_size + 1)) && read(f, data, (size_t)st.st_size) == st.st_size) {
            respond_s(fd, r, 200, "application/octet-stream", NULL, data, (size_t)st.st_size); free(data); close(f); return;
        }
        free(data); if (f >= 0) close(f);
        respond_sjson(fd, r, 404, "{\"error\":\"no such file\"}"); return;
    }
    if (!write) { respond_sjson(fd, r, 404, "{\"error\":\"not found\"}"); return; }

    if (!strcmp(r->path, "/api/plugins/upload/begin")) {
        unsigned gen;
        pthread_mutex_lock(&lk); gen = pl_claim(pub, 0, PL_UPLOADING, "", err, sizeof err); pthread_mutex_unlock(&lk);
        if (gen && (rc = ps_stage_begin(gen, body_size(r->body), err, sizeof err))) {
            pthread_mutex_lock(&lk); if (pl_is(gen, PL_UPLOADING)) pl_reset(); pthread_mutex_unlock(&lk);
        }
    }
    else if (sscanf(r->path, "/api/plugins/upload/%9ld%n", &off, &k) == 1 && !r->path[k]) {
        /* a piece: only into this browser's own upload.  Admitted here, written after lk is let go: between the two
         * this browser's cancel and another's upload/begin can pass, so the write goes to the store with the claim it
         * was admitted for, and the store writes it only into the file that claim began (ps_stage_chunk) */
        long long now = now_ms(); unsigned gen = 0;
        pthread_mutex_lock(&lk);
        pl_settle(now);
        if (pl_mine(pub) && pl.state == PL_UPLOADING) { pl.at = now; gen = pl.gen; } else snprintf(err, sizeof err, "%s", pl_why("no upload was started"));
        pthread_mutex_unlock(&lk);
        if (gen) rc = ps_stage_chunk(gen, off, r->body, r->blen, err, sizeof err);
    }
    else if (!strcmp(r->path, "/api/plugins/index")) {
        /* "look": a Plugins page was opened (web/plugins.js says so beside the list); "again": its "Look again"
         * was pressed.  The Echo holds each to its own pace (plugin_fetch.c): what a page asks by itself costs
         * the index a look an hour at most, whatever came of it */
        if (r->blen == 4 && !memcmp(r->body, "look", 4)) { pf_look_async(PF_AUTO); rc = 0; }
        else if (r->blen == 5 && !memcmp(r->body, "again", 5)) { pf_look_async(PF_ASKED); rc = 0; }
        /* "relist": "Browse plugins"' row of an installed version whose listing from the index was not taken (5.2's
         * move to a new key, K2 of a review of a30d6ed): a look at the pace of "again", and that listing asked again */
        else if (r->blen == 6 && !memcmp(r->body, "relist", 6)) { pf_relist_async(); rc = 0; }
        else snprintf(err, sizeof err, "say look, again or relist");
    }
    else if (!strcmp(r->path, "/api/plugins/fetch")) {
        char want[PS_ID_MAX + 8];
        snprintf(want, sizeof want, "%.*s", (int)(r->blen < sizeof want - 1 ? r->blen : sizeof want - 1), r->body ? r->body : "");
        want[strcspn(want, "\r\n")] = 0;
        if (!ps_id_ok(want)) snprintf(err, sizeof err, "not a plugin's id");
        else if (!bring_start(want, pub, err, sizeof err)) { respond_sjson(fd, r, 200, "{\"fetch\":\"fetching\"}"); return; }
    }
    else if (!strcmp(r->path, "/api/plugins/check")) {
        /* "key": the author key's fingerprint, which the page names for a package nobody vouches for (5.4);
         * "update" and "changes": what of this version is not what was accepted, when a version was ("limit": its
         * memory or threads, shown and never what the button is for) */
        struct ps_manifest *m = malloc(sizeof *m); struct ps_changes *ch = malloc(sizeof *ch); int tier = 0, button = 1, ok = 0; unsigned gen = 0; size_t cap = PS_CHECK_JSON, n = 0;      /* 18 capabilities' lines and 24 changes at their longest, escaped */
        char *t = malloc(cap);
        /* this browser's upload (or its package checked before, looked at again); CHECKING while the file is read:
         * nobody's to take */
        pthread_mutex_lock(&lk);
        pl_settle(now_ms());
        if (pl_mine(pub) && (pl.state == PL_UPLOADING || pl.state == PL_CHECKED)) { pl.state = PL_CHECKING; pl.at = now_ms(); gen = pl.gen; }
        else snprintf(err, sizeof err, "%s", pl_why("no package was uploaded"));
        pthread_mutex_unlock(&lk);
        if (gen && m && t && ch && !ps_stage_check(m, &tier, &button, ch, err, sizeof err)) {
            pthread_mutex_lock(&lk);
            if ((ok = pl_is(gen, PL_CHECKING))) {
                pl.state = PL_CHECKED; pl.at = now_ms(); snprintf(pl.id, sizeof pl.id, "%s", m->id); pl.err[0] = 0;
                memcpy(pl.digest, m->manifest_digest, 32);  /* what the dialog lists, and all a press will install */
            }
            pthread_mutex_unlock(&lk);
        }
        if (ok) {
            n = check_json(m, tier, button, ch, t, cap);
            respond_s(fd, r, 200, "application/json", NULL, t, n); free(m); free(t); free(ch); return;
        }
        if (!err[0]) snprintf(err, sizeof err, "no memory");
        if (gen) pl_close(gen, PL_NONE, 0, NULL);       /* what did not pass is not kept */
        free(m); free(t); free(ch);
    }
    else if (!strcmp(r->path, "/api/plugins/install")) {
        struct ps_manifest *m = malloc(sizeof *m); struct ps_changes *ch = malloc(sizeof *ch); int tier = 0, button = 1, st, mine, held = 0, k; unsigned gen; const char *said = "", *told, *holds = "none"; uint8_t listed[32];
        char swhy[320] = "";
        /* "synced <node> <nonce> <voucher>": another Echo of the network vouches that the owner accepted this
         * package there ("sync", above the fetch) */
        int sync = r->body && r->blen > 7 && !memcmp(r->body, "synced ", 7);
        pthread_mutex_lock(&lk);
        pl_settle(now_ms());
        mine = pl_mine(pub); st = pl.state; gen = pl.gen; memcpy(listed, pl.digest, 32);
        /* only the browser whose page listed this package: its dialog is what the owner's press answers */
        if (!mine && (told = pl_told(pub))) snprintf(err, sizeof err, "%s", told);
        else if (!mine) snprintf(err, sizeof err, st == PL_NONE ? "no checked package waits" : "another browser is installing a plugin: upload yours again when it is done");
        /* CHECKING for what follows, which nobody takes the slot in: the look at the staging file and what is made
         * of it.  It was done in CHECKED, the state a browser left for two minutes loses the slot in: the file read
         * here could be the next claim's, written as it was read */
        else if (st == PL_CHECKED) held = pl_move(gen, PL_CHECKED, PL_CHECKING);
        /* the second press asked for, from its screen: the same look at the file, then the press armed as the second
         * (plugin_ask: pl.first says what stood for the first) */
        else if (st == PL_SECOND) held = pl_move(gen, PL_SECOND, PL_CHECKING);
        if (mine && st == PL_WAITS) holds = holds_word();
        if (st == PL_SECOND) sync = 0;      /* (a voucher stood for the first press, or the first came: what is left is this Echo's own) */
        pthread_mutex_unlock(&lk);
        if (!mine) { }
        else if (st == PL_WAITS || st == PL_INSTALLING) said = "waiting";
        else if (st == PL_FETCHING || st == PL_CHECKING) snprintf(err, sizeof err, "the package is still being fetched");
        else if ((st != PL_CHECKED && st != PL_SECOND) || !held) snprintf(err, sizeof err, "no checked package waits");
        /* checked again here: what decides about the button is the Echo's own look at the file, now; and it is the
         * package the page listed, or nothing is installed */
        else if (!m || !ch || ps_stage_check(m, &tier, &button, ch, err, sizeof err)) { if (!err[0]) snprintf(err, sizeof err, "no memory"); }
        else if (memcmp(listed, m->manifest_digest, 32)) snprintf(err, sizeof err, "another package was uploaded since this one was checked");
        else {
            /* lk was let go for that look at the file, and what follows asks the slot again: it happens only if
             * the slot still is this claim, held in CHECKING, with the digest listed (GEN, LISTED: read above,
             * under lk).  What the check said about the button decides only which way is tried: the store looks
             * again when it writes (ps_stage_install), and installs without a press only what needs none then */
            if (ASK_DELAY_MS) {
                fprintf(stderr, "plugins: (test) the install's ask held%s\n", ASK_DELAY_FILE[0] ? " while the file is there" : "");
                if (!ASK_DELAY_FILE[0]) usleep((useconds_t)ASK_DELAY_MS * 1000);
                else for (int ms = 0; ms < ASK_DELAY_MS && !access(ASK_DELAY_FILE, F_OK); ms += 20) usleep(20000);
            }
            if (!button) {
                pthread_mutex_lock(&lk); int go = pl_mine(pub) && !memcmp(pl.digest, listed, 32) && pl_move(gen, PL_CHECKING, PL_INSTALLING); pthread_mutex_unlock(&lk);
                if (!go) snprintf(err, sizeof err, "%s", PL_OVERTAKEN);
                else if (!(k = plugin_install_now("update: nothing new to accept", gen, listed, PS_NO_PRESS, 0, err, sizeof err))) said = "installed";
                held = 0;       /* (the slot is INSTALLED, FAILED or, the button needed by now, CHECKED again) */
            }
            else if (sync) {
                /* The button is needed, and a voucher came in its place.  Judged on what the check above found (M is
                 * the package of LISTED: compared above); installed under the slot's rule as the install without
                 * the button is, for this claim and that digest or not at all.  Refused: nothing is armed, and the
                 * slot stays checked for the dialog and this Echo's own button */
                char node[64] = "", nh[40] = "", vh[40] = "", how[96]; uint8_t nonce[16], v[16]; unsigned long sn;
                if (sscanf(r->body + 7, "%63s %39s %39s", node, nh, vh) != 3 || !node_ok(node) || unhex(nonce, nh, 16) || unhex(v, vh, 16)) snprintf(err, sizeof err, "bad request");
                /* judged, and then its note written before anything is installed (rule 10): no note, no install */
                else if (sync_judge(pub, m, tier, ch, node, nonce, v, swhy, sizeof swhy) || !(sn = syn_begin(m, node, pub, swhy, sizeof swhy))) {
                    said = "button";
                    fprintf(stderr, "plugins: %s is not installed on a voucher (it named %s), without this Echo's action button: %s\n", m->id, node, swhy);
                }
                else {
                    snprintf(how, sizeof how, PS_HOW_SYNCED "%s", node);
                    for (int ms = 0; ms < SYNC_DELAY_MS && !access(SYNC_DELAY_FILE, F_OK); ms += 20) usleep(20000);
                    pthread_mutex_lock(&lk); int go = pl_mine(pub) && !memcmp(pl.digest, listed, 32) && pl_move(gen, PL_CHECKING, PL_INSTALLING); pthread_mutex_unlock(&lk);
                    if (!go) { syn_end(sn, 0); snprintf(err, sizeof err, "%s", PL_OVERTAKEN); }
                    else if (!(k = plugin_install_now(how, gen, listed, PS_VOUCHED, 0, err, sizeof err))) { said = "installed"; syn_end(sn, 1); }
                    else {
                        /* nothing was installed, so there is nothing to announce: unless the store failed after
                         * it had written the package (syn_failed).  PS_NEEDS_BUTTON comes before any write */
                        if (k == PS_NEEDS_BUTTON || k == PS_NEEDS_SECOND) syn_end(sn, 0); else syn_failed(sn, m->id, listed);
                        /* request.audio with a way out (3.3, 5.4): the voucher stood for the first press, never for the
                         * second, which is this Echo's own, on its screen: the slot waits for it (SECOND), no note (a
                         * person will have pressed here) */
                        if (k == PS_NEEDS_SECOND) { said = "second"; err[0] = 0; pthread_mutex_lock(&lk); snprintf(swhy, sizeof swhy, "%s", pl.gen == gen ? pl.second : ""); pthread_mutex_unlock(&lk); }
                        /* what was judged is not what the store found when it wrote: this Echo's own button, as any refusal */
                        if (k == PS_NEEDS_BUTTON) { said = "button"; snprintf(swhy, sizeof swhy, "%s", err); err[0] = 0; fprintf(stderr, "plugins: %s is not installed on a voucher (it named %s), without this Echo's action button: %s\n", m->id, node, swhy); }
                    }
                    held = 0;
                }
            }
            else {
                if (!(said = plugin_ask(pub, gen, listed, &holds))) { said = ""; snprintf(err, sizeof err, "%s", PL_OVERTAKEN); }
                held = 0;       /* (WAITS, or CHECKED again: plugin_ask) */
            }
        }
        /* not installed, not armed: the package stays this browser's, checked, as before the look */
        if (held) { pthread_mutex_lock(&lk); pl_move(gen, PL_CHECKING, pl.first ? PL_SECOND : PL_CHECKED); pthread_mutex_unlock(&lk); }
        free(m); free(ch);
        if (!err[0]) {
            char wj[700]; jesc(wj, sizeof wj, swhy);
            if (!strcmp(said, "second")) snprintf(o, sizeof o, "{\"install\":\"second\",\"second\":\"%s\"}", wj);
            else if (swhy[0]) snprintf(o, sizeof o, "{\"install\":\"%s\",\"why\":\"%s\"}", said, wj); else snprintf(o, sizeof o, "{\"install\":\"%s\",\"holds\":\"%s\"}", said, holds);
            respond_sjson(fd, r, 200, o); return;
        }
    }
    else if (!strcmp(r->path, "/api/plugins/accept/nonce")) {
        /* the target's part of a voucher: a nonce for the browser that asks, which is approved here (signed) */
        char nh[33]; unsigned i;
        pthread_mutex_lock(&lk);
        i = acc_nonce_next++ % 8;
        memcpy(acc_nonces[i].pub, pub, 32); ws_random(acc_nonces[i].nonce, 16); acc_nonces[i].at = now_ms(); hex(nh, acc_nonces[i].nonce, 16);
        pthread_mutex_unlock(&lk);
        snprintf(o, sizeof o, "{\"nonce\":\"%s\"}", nh); respond_sjson(fd, r, 200, o); return;
    }
    else if (!strcmp(r->path, "/api/plugins/accept/issue")) {
        /* the source's part: only for a plugin that is installed, may run and was accepted HERE (with this Echo's
         * button, or by a sync it took itself), and only to a browser approved here (signed: its key is the one
         * the voucher names, never one the request brings).  What it says is this Echo's own reading of its own
         * package; the target holds it against the package it is given */
        char th[80] = "", nh[40] = "", want[PS_ID_MAX + 8] = "", vh[33], mh[65], kh[65], ah[65]; uint8_t tgt[32], nonce[16], key[32], v[16], md[32], ak[32], ad[32]; int ok = 0;
        if (sscanf(r->body ? r->body : "", "%79s %39s %32s", th, nh, want) != 3 || unhex(tgt, th, 32) || unhex(nonce, nh, 16) || !ps_id_ok(want)) snprintf(err, sizeof err, "bad request");
        else {
            ps_lock();
            if ((sp = ps_find(want)) && sp->here && !sp->unread && sp->accepted && !sp->refused[0]) {
                memcpy(md, sp->m.manifest_digest, 32); memcpy(ak, sp->m.author_key, 32); memcpy(ad, sp->m.accepted, 32); ok = 1;
            }
            ps_unlock();
            if (!ok) snprintf(err, sizeof err, "%s is not installed and accepted on this Echo: there is nothing of it to vouch for", want);
            else if (arb_mode(-1) == ARB_KIOSK) snprintf(err, sizeof err, "this Echo settles wake words the Kiosk Satellite way: it vouches for no plugin in that mode");
            else if (arb_web_key(key)) snprintf(err, sizeof err, "this Echo is in no network with other Echos");
            else {
                ps_voucher(v, key, tgt, nonce, pub, core_node_name(), want, md, ak, ad); crypto_wipe(key, sizeof key);
                hex(vh, v, 16); hex(mh, md, 32); hex(kh, ak, 32); hex(ah, ad, 32);
                snprintf(o, sizeof o, "{\"voucher\":\"%s\",\"node\":\"%s\",\"manifest\":\"%s\",\"key\":\"%s\",\"accepted\":\"%s\"}", vh, core_node_name(), mh, kh, ah);
                fprintf(stderr, "plugins: vouched to another Echo that %s was accepted here\n", want);
                respond_sjson(fd, r, 200, o); return;
            }
        }
    }
    else if (!strcmp(r->path, "/api/plugins/synced")) {
        /* a note taken off: at once for a browser let in with this Echo's button that is not the one the note names,
         * else with the action button (syn_ask; "the announcement that stands in for the press") */
        unsigned long one = 0; const char *word = NULL, *holds = "none"; char tail = 0; long long retry = 0;
        if (r->blen == 11 && !memcmp(r->body, "dismiss all", 11)) word = syn_ask(pub, 0, &holds, &retry);
        else if (r->body && sscanf(r->body, "dismiss %9lu%c", &one, &tail) == 1 && one) word = syn_ask(pub, one, &holds, &retry);
        else if (r->blen == 4 && !memcmp(r->body, "keep", 4)) {
            /* the asker's own word ends its wait before the press */
            pthread_mutex_lock(&lk); if (login.state == 1 && login.kind == W_NOTE && !memcmp(login.pub, pub, 32)) note_withdraw("kept"); pthread_mutex_unlock(&lk);
            rc = 0;
        }
        else snprintf(err, sizeof err, "say dismiss all, dismiss <its number>, or keep");
        if (word && !strcmp(word, "later")) { snprintf(o, sizeof o, "{\"dismiss\":\"later\",\"holds\":\"%s\",\"retry_s\":%lld}", holds, (retry + 999) / 1000); respond_sjson(fd, r, 200, o); return; }
        if (word) { snprintf(o, sizeof o, "{\"dismiss\":\"%s\",\"holds\":\"%s\"}", word, holds); respond_sjson(fd, r, 200, o); return; }
    }
    else if (!strcmp(r->path, "/api/plugins/resume")) { plugin_resume(); rc = 0; }
    else if (!strcmp(r->path, "/api/plugins/cancel")) {
        /* a browser ends its own install, and nobody else's: another's upload or dialog is none of its business
         * (before, a cancel wiped whatever was not busy), and one that waits or installs is said so */
        unsigned drop = 0;
        pthread_mutex_lock(&lk);
        pl_settle(now_ms());
        if (!pl_mine(pub)) {
            if (pl.state == PL_FETCHING || pl.state == PL_CHECKING || pl.state == PL_WAITS || pl.state == PL_INSTALLING) snprintf(err, sizeof err, "a plugin is being installed");
            else { if (pl_told(pub)) pl_note.at = 0; rc = 0; }
        }
        else if (pl.state == PL_INSTALLING || (pl.state == PL_CHECKING && !pl.by)) snprintf(err, sizeof err, "a plugin is being installed");
        /* a fetch that only downloads has nothing of the staging file: the slot is free at once and its thread
         * told to stop.  While it works on the file the end is its thread's (dropped from here, a piece of it
         * could land in the next upload's) */
        else if (pl.state == PL_FETCHING) { pl_reset(); rc = 0; }
        else if (pl.state == PL_CHECKING) { pl.by->stop = 1; rc = 0; }
        /* the rest holds the staging file or nothing: CHECKING while it is dropped, so that nobody's new upload is
         * what goes */
        else { plugin_withdraw("cancelled"); pl.state = PL_CHECKING; pl.at = now_ms(); drop = pl.gen; rc = 0; }
        pthread_mutex_unlock(&lk);
        if (drop) pl_close(drop, PL_NONE, 0, NULL);
    }
    else if (sscanf(r->path, "/api/plugins/%24[a-z0-9-]/%n", id, &k) == 1 && k) {
        const char *rest = r->path + k; char line[PS_VALUE_MAX + 48], *eq; int has;
        ps_lock(); has = ps_find(id) != NULL; ps_unlock();
        snprintf(line, sizeof line, "%.*s", (int)(r->blen < sizeof line - 1 ? r->blen : sizeof line - 1), r->body ? r->body : "");
        line[strcspn(line, "\r\n")] = 0; eq = strchr(line, '=');
        if (!has) snprintf(err, sizeof err, "no such plugin");
        else if (!strcmp(rest, "enable")) { rc = plugin_enable(id, !strcmp(line, "on")); if (rc) snprintf(err, sizeof err, "cannot write on this Echo"); }
        /* what the plugin found (PROPOSE): "take <row> <value>" writes it as the owner's own value (the plugin starts
         * again, as for any change of its server), "drop <row> <value>" lets it go.  Only the proposal standing now */
        else if (!strcmp(rest, "propose")) {
            char what[8], row[33], val[16];
            if (sscanf(line, "%7s %32s %15s", what, row, val) != 3 || (strcmp(what, "take") && strcmp(what, "drop"))) snprintf(err, sizeof err, "say take or drop, the row and the value");
            else rc = plugin_propose_take(id, row, val, !strcmp(what, "drop"), err, sizeof err);
        }
        /* an action marked "page": its button on the card (none with arguments: plugin_store.c actions_parse) */
        else if (!strcmp(rest, "action")) { rc = plugin_action_page(id, line); if (rc) snprintf(err, sizeof err, "no such action of a running plugin"); }
        else if (!strcmp(rest, "develop")) { rc = plugin_develop(id, !strcmp(line, "on")); if (rc) snprintf(err, sizeof err, "cannot write on this Echo"); }
        else if (!strncmp(rest, "storage/", 8)) rc = storage_in(pub, r->ctr, id, rest + 8, line, r->body, r->blen, err, sizeof err);
        else if (!strcmp(rest, "anyway")) {
            const char *holds, *word = anyway_ask(pub, id, strcmp(line, "cancel") != 0, &holds);
            snprintf(o, sizeof o, "{\"anyway\":\"%s\",\"holds\":\"%s\"}", word, holds);
            respond_sjson(fd, r, 200, o); return;
        }
        else if (!strcmp(rest, "remove")) { if (strcmp(line, "remove")) snprintf(err, sizeof err, "say remove"); else if ((rc = plugin_remove(id))) snprintf(err, sizeof err, "cannot remove it"); else core_plugin_event(PLUGIN_REMOVED, id); }
        else if (!strcmp(rest, "set") && eq) {
            *eq = 0;
            /* a secret comes sealed (secret/<name>, below) or not at all: this request's body is plain on the network,
             * and whoever reads it there would hold the secret.  The page never sends one this way (plugins.js) */
            /* a server's row (net.server's host or port): what the plugin was connected to is the old one's, and a
             * socket it holds is not taken back by a new CONFIG.  Started again, so that nothing goes to a server the
             * owner no longer means (a review, 2026-11-21: the old server kept getting lines after a change and
             * after the row was cleared) */
            char was[PS_VALUE_MAX] = "", now[PS_VALUE_MAX] = ""; int server = 0;
            ps_lock(); sp = ps_find(id);
            if (sp && ps_setting_secret(sp, line)) snprintf(err, sizeof err, "a secret is not taken in clear: it is set on this Echo's own page, which seals it");
            else if (sp) {
                if ((server = ps_net_row(sp, line))) ps_config_owner(sp, line, was, sizeof was);
                rc = ps_config_set(sp, line, eq + 1, err, sizeof err);
                if (!rc && server) { ps_config_owner(sp, line, now, sizeof now); server = strcmp(was, now) != 0; }
            }
            ps_unlock();
            if (!rc && server) plugin_restart(id, "the server you entered changed");
            else if (!rc) plugin_config_changed(id);
        }
        else if (sscanf(rest, "secret/%32[a-z0-9_]%n", name, &k) == 1 && !rest[k]) {
            /* sealed with the browser's key, as the Wi-Fi password: the page is plain HTTP.  Padded with zeros */
            uint8_t sealed[PS_VALUE_MAX], plain[PS_VALUE_MAX + 1]; int n = unhex_any(sealed, sizeof sealed - 1, line);
            if (n < 0 || unseal(r, sealed, (size_t)n, plain)) snprintf(err, sizeof err, "bad request");
            else {
                plain[n] = 0;
                ps_lock(); sp = ps_find(id); rc = sp ? ps_config_set(sp, name, (const char *)plain, err, sizeof err) : -1; ps_unlock();
                if (!rc) plugin_config_changed(id);
            }
            crypto_wipe(plain, sizeof plain); crypto_wipe(sealed, sizeof sealed);
        }
        else if (sscanf(rest, "item/%32[a-z0-9_]/%40[a-z0-9_-]/%n", list, item, &k) == 2 && k) {
            /* "from-wake" (begin's whole body, or a field's second line): the page copies this from a wake word plugin
             * on another Echo.  Its fields are what that plugin said of the file (FILE_OK) while it heard the
             * microphone: into a plugin that is not one here they do not go (3.3's seal, as storage_in holds storage to
             * it).  Only there: a chunk's body is the file, and a file with those bytes in it was refused (Q7 of a
             * review, 2026-11-21) */
            const char *what = rest + k; int wake_src = 0;
            if (r->body && !strcmp(what, "begin")) wake_src = r->blen == 9 && !memcmp(r->body, "from-wake", 9);
            else if (r->body && !strcmp(what, "field")) { const char *nl = memchr(r->body, '\n', r->blen); wake_src = nl && (size_t)(r->body + r->blen - nl - 1) == 9 && !memcmp(nl + 1, "from-wake", 9); }
            ps_lock(); sp = ps_find(id);
            if (!sp) snprintf(err, sizeof err, "no such plugin");
            else if (wake_src && !(sp->m.caps & PM_CAPBIT(PM_CAP_WAKE))) snprintf(err, sizeof err, "a wake word plugin's file and what it said of it, and here the plugin is not one: not copied");
            else if (!strcmp(what, "begin")) rc = ps_item_begin(sp, list, item, err, sizeof err);
            else if (sscanf(what, "chunk/%20[.a-zA-Z0-9]/%9ld%n", suffix, &off, &k) == 2 && !what[k]) rc = ps_item_chunk(sp, list, item, suffix, off, r->body, r->blen, err, sizeof err);
            else if (!strcmp(what, "field") && eq) { *eq = 0; rc = ps_item_field(sp, list, item, line, eq + 1, 0, err, sizeof err); }
            else if (!strcmp(what, "delete")) { rc = ps_item_delete(sp, list, item); if (rc) snprintf(err, sizeof err, "cannot delete it"); }
            else if (strcmp(what, "done")) snprintf(err, sizeof err, "not found");
            ps_unlock();
            if (!strcmp(what, "done") && sp) {
                int v = plugin_item_uploaded(id, list, item, err, sizeof err);
                if (v) { snprintf(o, sizeof o, "{\"ok\":true,\"checked\":%s}", v == 1 ? "true" : "false"); respond_sjson(fd, r, 200, o); return; }
                if (!err[0]) snprintf(err, sizeof err, "it cannot be used");
            }
            else if (!rc && (!strcmp(what, "field") || !strcmp(what, "delete"))) plugin_files_changed(id, list);
        }
        else snprintf(err, sizeof err, "not found");
    }
    else snprintf(err, sizeof err, "not found");
    if (!rc) { respond_sjson(fd, r, 200, "{\"ok\":true}"); return; }
    jesc(ej, sizeof ej, err); snprintf(o, sizeof o, "{\"error\":\"%s\"}", ej);
    respond_sjson(fd, r, 400, o);
}

/* ---------------------------------------------------------------- the log */

#define LOG_SEND_MAX (2 << 20)      /* main.sh rotates at 1 MB, checked every 10 s: the tail of this much is plenty */

/* (logread.c: the same file and the same blanking as before; what a plugin with log.read gets is less) */
static void log_api(int fd, struct req *r)
{
    int part; char *t; size_t n;
    if (!signed_ok(r, 0)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); return; }
    if (sscanf(r->path, "/api/log/%d", &part) != 1 || part < 0 || part > 1) { respond_sjson(fd, r, 404, "{\"error\":\"not found\"}"); return; }
    if (!(t = logread_tail(part, LOG_SEND_MAX, &n))) {
        if (n) respond_sjson(fd, r, 503, "{\"error\":\"no memory\"}");
        else respond_s(fd, r, 200, "text/plain; charset=utf-8", NULL, "", 0);       /* no rotated part yet */
        return;
    }
    respond_s(fd, r, 200, "text/plain; charset=utf-8", NULL, t, n);
    free(t);
}

/* Online updates come from GitHub: a line of BODY that switches them on (from off) counts only once this Echo reached
 * it (update_probe), else it is turned into a comment and WHY says what failed.  Imports go the same way.  Before
 * core_lock: the probe takes up to 15 s.  Lines are read as settings_apply_text reads them (its first
 * SETTINGS_LINE_MAX - 1 bytes, up to a \r, which ends its line; any white space ahead of the name; the choice in any
 * case, as settings.c parse() takes it): "Beta" passed this gate untouched and switched updates on without the probe,
 * and so did a line padded past what settings_apply_text keeps of it, the rest of the value cut off there. */
static void updates_gate(char *body, char *why, size_t cap)
{
    why[0] = 0;
    for (char *p = body; *p; ) {
        size_t l = strcspn(p, "\n"), m = strcspn(p, "\r\n"); char *k = p, *e, v[16];
        if (m > SETTINGS_LINE_MAX - 1) m = SETTINGS_LINE_MAX - 1;
        e = p + m;
        while (k < e && isspace((unsigned char)*k)) k++;
        if (e - k >= 15 && !strncmp(k, "online_updates=", 15)) {
            snprintf(v, sizeof v, "%.*s", (int)(e - k - 15), k + 15);
            int on = !strcasecmp(v, update_channels[UPDATE_BETA]) || !strcasecmp(v, update_channels[UPDATE_RELEASE]);
            if (on && update_channel(-1) == UPDATE_OFF && (why[0] || update_probe(why, cap))) *k = '#';
        }
        p += l + (p[l] == '\n');
    }
}

static void handle(int fd)
{
    struct req r; int e; uint8_t pub[32];
    struct timeval tv = { 10, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if ((e = read_request(fd, &r))) { if (e == -2) respond_json(fd, 413, "{\"error\":\"too large\"}"); return; }
    char *q = strchr(r.path, '?'); if (q) *q = 0;       /* no query strings in what we serve */

    if (!strcmp(r.method, "OPTIONS")) respond(fd, 204, "text/plain", NULL, NULL, 0);
    else if (!strcmp(r.method, "GET") && strncmp(r.path, "/api/", 5)) {
        int found = 0;
        for (int i = 0; i < web_nassets; i++) if (!strcmp(web_assets[i].path, r.path)) {
            respond(fd, 200, web_assets[i].type, "Content-Encoding: gzip\r\n", web_assets[i].data, web_assets[i].len); found = 1; break;
        }
        if (!found) respond_json(fd, 404, "{\"error\":\"not found\"}");
    }
    else if (!strcmp(r.path, "/api/hello")) { char o[HELLO_MAX]; hello_json(o, sizeof o, 1); respond_json(fd, 200, o); }
    else if (!strncmp(r.path, "/api/vouch/", 11) && !strcmp(r.method, "POST")) vouch_api(fd, &r);
    else if (!strcmp(r.path, "/api/login") && !strcmp(r.method, "POST")) {
        /* body: "<browser key hex> <label>" */
        char h[80] = "", label[64] = "browser", o[160]; int k = 0; const char *why, *word; long long retry;
        struct sockaddr_in sa; socklen_t sl = sizeof sa; uint32_t addr = 0;
        if (!getpeername(fd, (struct sockaddr *)&sa, &sl) && sa.sin_family == AF_INET) addr = sa.sin_addr.s_addr;
        sscanf(r.body, "%79s %n", h, &k);
        if (k) { snprintf(label, sizeof label, "%s", r.body + k); for (char *c = label; *c; c++) if ((unsigned char)*c < 0x20) *c = ' '; }
        if (unhex(pub, h, 32)) respond_json(fd, 400, "{\"error\":\"key\"}");
        else {
            /* "why" and "retry_s" beside "refused": a page of a build before them reads the word alone, as it did */
            word = login_ask(pub, label, addr, &why, &retry);
            if (why[0]) snprintf(o, sizeof o, "{\"login\":\"%s\",\"why\":\"%s\",\"retry_s\":%lld}", word, why, (retry + 999) / 1000);
            else snprintf(o, sizeof o, "{\"login\":\"%s\"}", word);
            respond_json(fd, 200, o);
        }
    }
    else if (!strcmp(r.method, "GET") && !strcmp(r.path, "/api/state")) {
        if (!signed_ok(&r, 0) || unhex(pub, r.pub, 32)) respond_json(fd, 401, "{\"error\":\"not logged in\"}");
        else state_json(fd, &r, pub);
    }
    else if (!strcmp(r.method, "GET") && !strcmp(r.path, "/api/export")) {
        char t[4096], head[200]; size_t n;
        if (!signed_ok(&r, 0)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); free(r.body); return; }
        n = (size_t)snprintf(t, sizeof t, SETTINGS_EXPORT_HEAD "%s (%s %s)\n# not in it: the Echo's name, keys, pairings\n", core_name, board.model, VERSION);
        pthread_mutex_lock(&core_lock); n += settings_text(t + n, sizeof t - n, 1); pthread_mutex_unlock(&core_lock);
        snprintf(head, sizeof head, "Content-Disposition: attachment; filename=\"hassmic-settings.conf\"\r\n");
        respond_s(fd, &r, 200, "text/plain; charset=utf-8", head, t, n);
    }
    else if (!strcmp(r.method, "POST") && !strcmp(r.path, "/api/set")) {
        /* body: "name=value" lines, one setting or a whole export */
        char err[2048], o[2800], ej[2200], why[300], wj[340]; int applied;
        if (!signed_ok(&r, 1)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); free(r.body); return; }
        updates_gate(r.body, why, sizeof why);
        pthread_mutex_lock(&core_lock); applied = settings_apply_text(r.body, err, sizeof err); pthread_mutex_unlock(&core_lock);
        if (why[0]) { size_t n = strlen(err); snprintf(err + n, sizeof err - n, "online_updates: %s cannot be reached from this Echo (%s), so they stay off\n",
                                                 sources_update_urls(NULL, 0, NULL, 0) ? "the update server (Sources)" : "GitHub", why); }
        jesc(ej, sizeof ej, err); jesc(wj, sizeof wj, why);
        /* github_unreachable: the page's warning under "Online updates" */
        snprintf(o, sizeof o, "{\"applied\":%d,\"errors\":\"%s\",\"github_unreachable\":\"%s\"}", applied, ej, wj);
        fprintf(stderr, "web: %d setting%s changed%s%s", applied, applied == 1 ? "" : "s", err[0] ? ", refused: " : "\n", err);
        respond_sjson(fd, &r, 200, o);
    }
    else if (!strcmp(r.method, "POST") && !strcmp(r.path, "/api/identify")) {
        if (!signed_ok(&r, 1)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); free(r.body); return; }
        core_identify();
        respond_sjson(fd, &r, 200, "{\"identify\":true}");
    }
    else if (!strcmp(r.method, "POST") && !strcmp(r.path, "/api/note")) {         /* body: the note, empty clears it */
        char o[NOTE_MAX * 2 + 24], nt[NOTE_MAX * 2 + 8];
        if (!signed_ok(&r, 1)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); free(r.body); return; }
        if (note_save(r.body ? r.body : "", r.body ? r.blen : 0)) { fprintf(stderr, "web: cannot write the note\n"); respond_sjson(fd, &r, 500, "{\"error\":\"cannot write it\"}"); }
        else {
            pthread_mutex_lock(&note_lk); jesc(nt, sizeof nt, note); fprintf(stderr, "web: note %s\n", note[0] ? "set" : "cleared"); pthread_mutex_unlock(&note_lk);
            snprintf(o, sizeof o, "{\"note\":\"%s\"}", nt); respond_sjson(fd, &r, 200, o);
        }
    }
    else if (!strcmp(r.method, "POST") && !strcmp(r.path, "/api/dropin")) {       /* body: the node to drop in on, or "end" */
        char e[120], t[64], o[200], ej[160];
        if (!signed_ok(&r, 1)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); free(r.body); return; }
        snprintf(t, sizeof t, "%.*s", (int)(r.blen < sizeof t - 1 ? r.blen : sizeof t - 1), r.body ? r.body : "");
        t[strcspn(t, "\r\n")] = 0;
        if (!dropin_running()) respond_sjson(fd, &r, 409, "{\"error\":\"Drop In is not running on this Echo\"}");
        else if (!strcmp(t, "end")) { dropin_hangup("settings page"); respond_sjson(fd, &r, 200, "{\"dropin\":\"ended\"}"); }
        else if (dropin_call(t, e, sizeof e)) { jesc(ej, sizeof ej, e); snprintf(o, sizeof o, "{\"error\":\"%s\"}", ej); respond_sjson(fd, &r, 409, o); }
        else respond_sjson(fd, &r, 200, "{\"dropin\":\"calling\"}");
    }
    else if (!strcmp(r.method, "POST") && !strcmp(r.path, "/api/reset")) {       /* body "reset": not by a stray request */
        if (!signed_ok(&r, 1)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); free(r.body); return; }
        if (!r.body || r.blen != 5 || memcmp(r.body, "reset", 5)) respond_sjson(fd, &r, 400, "{\"error\":\"say reset\"}");
        else { respond_sjson(fd, &r, 200, "{\"reset\":true}"); core_reset("settings page"); }
    }
    else if (!strcmp(r.method, "POST") && !strcmp(r.path, "/api/reboot")) {      /* body "reboot", as reset's */
        if (!signed_ok(&r, 1)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); free(r.body); return; }
        if (!r.body || r.blen != 6 || memcmp(r.body, "reboot", 6)) respond_sjson(fd, &r, 400, "{\"error\":\"say reboot\"}");
        else { respond_sjson(fd, &r, 200, "{\"reboot\":true}"); core_reboot("settings page"); }
    }
    else if (!strcmp(r.method, "POST") && !strcmp(r.path, "/api/adb")) {
        char o[80]; const char *holds, *word;
        if (!signed_ok(&r, 1) || unhex(pub, r.pub, 32)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); free(r.body); return; }
        word = adb_ask(pub, !strncmp(r.body, "on", 2), &holds);
        snprintf(o, sizeof o, "{\"adb\":\"%s\",\"holds\":\"%s\"}", word, holds);
        respond_sjson(fd, &r, 200, o);
    }
    else if (!strcmp(r.method, "POST") && !strcmp(r.path, "/api/sources")) {
        /* Sources (sources.c): the page's lines, or "cancel"; an approved browser's, and a key or no check also the
         * action button's (source_ask) */
        char why[400], wj[480], o[640]; const char *holds, *word;
        if (!signed_ok(&r, 1) || unhex(pub, r.pub, 32)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); free(r.body); return; }
        word = source_ask(pub, r.body, &holds, why, sizeof why);
        jesc(wj, sizeof wj, why);
        snprintf(o, sizeof o, "{\"sources\":\"%s\",\"holds\":\"%s\",\"why\":\"%s\"}", word, holds, wj);
        respond_sjson(fd, &r, 200, o);
    }
    else if (!strcmp(r.method, "POST") && !strcmp(r.path, "/api/revoke")) {
        uint8_t who[32]; int found = 0;
        if (!signed_ok(&r, 1)) { respond_json(fd, 401, "{\"error\":\"not logged in\"}"); free(r.body); return; }
        r.body[strcspn(r.body, " \r\n")] = 0;
        if (unhex(who, r.body, 32)) { respond_json(fd, 400, "{\"error\":\"key\"}"); free(r.body); return; }
        pthread_mutex_lock(&lk);
        for (int i = 0; i < nclients; i++) if (!memcmp(clients[i].pub, who, 32)) {
            memmove(clients + i, clients + i + 1, sizeof clients[0] * (size_t)(nclients - i - 1)); nclients--; found = 1; break;
        }
        /* (the list of the button's browsers not written: the one revoked is still in the file, and would be a
         * browser of the button's again if it came back on a voucher before a start read it: tried again with the
         * next browser added, as add_client does for one pushed out) */
        if (found) { save_clients(); if (save_button()) button_stale = 1; }
        if (login.state == 1 && login.kind == W_ADB && !memcmp(login.pub, who, 32)) adb_withdraw("its browser's login revoked");
        if (login.state == 1 && login.kind == W_PLUGIN && !memcmp(login.pub, who, 32)) plugin_withdraw("its browser's login was revoked");
        if (login.state == 1 && login.kind == W_NOTE && !memcmp(login.pub, who, 32)) note_withdraw("its browser's login was revoked");
        if (login.state == 1 && login.kind == W_ANYWAY && !memcmp(login.pub, who, 32)) anyway_withdraw("its browser's login was revoked");
        if (login.state == 1 && login.kind == W_SOURCE && !memcmp(login.pub, who, 32)) source_withdraw("its browser's login was revoked");
        pthread_mutex_unlock(&lk);
        fprintf(stderr, "web: a browser's login %s\n", found ? "revoked" : "to revoke not found");
        respond_sjson(fd, &r, 200, found ? "{\"revoked\":true}" : "{\"revoked\":false}");
    }
    else if (!strncmp(r.path, "/api/artifact", 13)) artifact_api(fd, &r);
    else if (!strncmp(r.path, "/api/davs", 9)) davs_api(fd, &r);
    else if (!strncmp(r.path, "/api/mww", 8)) mww_api(fd, &r);
    /* The revocation list this Echo keeps, for the other Echos of its network (plugin_fetch.c member_fetch; 5.6):
     * the index's public list with the index key's signature, which every reader checks itself.  Only to a member
     * (the network's key, a ticket this Echo told lately, a nonce once: pf_revoked_serve); anyone else, and a member's
     * request sent again, is answered as for a path there is none of, list or not: open, it told any host of the LAN
     * whether this Echo has plugins (a review, 2026-11-21; arb's announcement tells as much to whoever listens there,
     * plugin_fetch.c M_REVOKED) */
    else if (!strcmp(r.method, "GET") && !strcmp(r.path, "/api/plugins/revoked")) {
        char *t = NULL; size_t n = pf_revoked_serve(r.ticket, r.ctr, r.mac, &t);
        if (!n) respond_json(fd, 404, "{\"error\":\"not found\"}");
        else respond(fd, 200, "application/octet-stream", NULL, t, n);
        free(t);
    }
    else if (!strncmp(r.path, "/api/plugins", 12)) plugins_api(fd, &r);
    else if (!strncmp(r.path, "/api/ring", 9)) ring_api(fd, &r);
    else if (!strncmp(r.path, "/api/wifi", 9)) wifi_api(fd, &r);
    else if (!strcmp(r.path, "/api/mass")) mass_api(fd, &r);
    else if (!strcmp(r.path, "/api/screen")) screen_api(fd, &r);
    else if (!strncmp(r.path, "/api/timer-sound", 16)) timer_sound_api(fd, &r);
    else if (!strncmp(r.path, "/api/sound/", 11)) sound_api(fd, &r);
    else if (!strncmp(r.path, "/api/sound-pack", 15)) sound_pack_api(fd, &r);
    else if (!strcmp(r.method, "GET") && !strncmp(r.path, "/api/log/", 9)) log_api(fd, &r);
    else respond_json(fd, 404, "{\"error\":\"not found\"}");
    free(r.body);
}

static void *conn_thread(void *arg)
{
    int fd = (int)(long)arg;
    handle(fd); close(fd);
    atomic_fetch_sub(&nconn, 1);
    return NULL;
}

static int port;
static void *listener(void *arg)
{
    int ls = net_listen(port);
    (void)arg;
    if (ls < 0) perror("web: listen");                          /* (net_accept_kept tries again every second) */
    else fprintf(stderr, "web: settings page on port %d\n", port);
    for (;;) {
        int c = net_accept_kept(&ls, port, "web"); pthread_t t;
        if (c < 0) continue;
        if (atomic_fetch_add(&nconn, 1) >= MAX_CONN) { atomic_fetch_sub(&nconn, 1); close(c); continue; }   /* a browser opens few */
        if (pthread_create(&t, NULL, conn_thread, (void *)(long)c)) { atomic_fetch_sub(&nconn, 1); close(c); }
        else pthread_detach(t);
    }
    return NULL;
}

/* A login nobody asks about any more runs out here.  login_expire ran only when a browser asked or the button was
 * pressed: a page closed while it waited (an iPhone at 00:43 on an Echo 2, 2026-10-25) left the ring's
 * authenticated_setup_mode on until the next morning, as nothing asked again */
static void *expiry(void *arg)
{
    (void)arg;
    atomic_store(&expiry_runs, 1);
    for (;;) {
        /* every second, and at once when a press or a hold left something for the flash, or a phone's wait went
         * off (hold_poke) */
        struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); t.tv_sec += 1;
        pthread_mutex_lock(&lk);
        if (!hold_out.want) pthread_cond_timedwait(&hold_cv, &lk, &t);
        login_expire(now_ms()); hold_save(now_ms());
        pthread_mutex_unlock(&lk);
        hold_flush(0);
        /* `synced` events Home Assistant got since the last look: their notes marked, so that a restart does not
         * say them again (main.c core_plugin_event; here because this thread holds no lock and may write) */
        pthread_mutex_lock(&syn_save_lk); if (syn_told()) syn_save(); pthread_mutex_unlock(&syn_save_lk);
    }
    return NULL;
}

int web_start(int p, const struct web_hooks *h)
{
    pthread_t t; uint8_t id[8];
    const char *e = getenv("HASSMIC_LOGIN_MS");
    if (e && atoi(e) > 0) {
        int w = 0, q = -1; sscanf(e, "%d:%d", &w, &q);          /* (a third number, the contest's time of a build before, is not read) */
        LOGIN_MS = w; QUIET_MS = q >= 0 ? q : w / 2;
    }
    if ((e = getenv("HASSMIC_LOGIN_RATE"))) { int a, b; if (sscanf(e, "%d:%d", &a, &b) == 2 && a > 0 && b > 0) { ASK_ADDR = a; ASK_ALL = b; } }
    if ((e = getenv("HASSMIC_PLUGIN_SLOT"))) { int c, u, f; if (sscanf(e, "%d:%d:%d", &c, &u, &f) == 3 && c >= 0 && u >= 0 && f > 0) { CHECKED_MS = c; UPLOAD_MS = u; FETCH_MS = f; } }
    if ((e = getenv("HASSMIC_PLUGIN_ASK_DELAY")) && atoi(e) > 0) { ASK_DELAY_MS = atoi(e); if (strchr(e, ':')) snprintf(ASK_DELAY_FILE, sizeof ASK_DELAY_FILE, "%s", strchr(e, ':') + 1); }
    if ((e = getenv("HASSMIC_PLUGIN_OWN_DELAY")) && atoi(e) > 0) OWN_DELAY_MS = atoi(e);
    if ((e = getenv("HASSMIC_PLUGIN_SYNC_DELAY")) && sscanf(e, "%d:%199s", &SYNC_DELAY_MS, SYNC_DELAY_FILE) != 2) SYNC_DELAY_MS = 0;
    if ((e = getenv("HASSMIC_PLUGIN_NONCE_MS")) && atoi(e) > 0) ACC_NONCE_MS = atoi(e);
    if ((e = getenv("HASSMIC_PLUGIN_NOTES")) && atoi(e) > 0 && atoi(e) < SYN_MAX) SYN_NOTES = atoi(e);
    if ((e = getenv("HASSMIC_PLUGIN_NOTE_AGAIN")) && atoi(e) > 0) NOTE_AGAIN_MS = atoi(e);
    syn_load();
    ws_random(id, sizeof id); hex(start_id, id, sizeof id);
    hooks = h; port = p;
    { pthread_condattr_t ca; pthread_condattr_init(&ca); pthread_condattr_setclock(&ca, CLOCK_MONOTONIC); pthread_cond_init(&hold_cv, &ca); pthread_condattr_destroy(&ca); }
    pthread_mutex_lock(&lk);
    int r = load_key();
    if (!r) { load_clients(); load_button(); hold_load(); }
    pthread_mutex_unlock(&lk);
    hold_flush(0);
    note_load();
    if (r) return -1;
    if (!pthread_create(&t, NULL, expiry, NULL)) pthread_detach(t);
    return pthread_create(&t, NULL, listener, NULL) ? -1 : (pthread_detach(t), 0);
}
