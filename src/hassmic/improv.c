/*
 * Improv Wi-Fi over Bluetooth LE (https://www.improv-wifi.com/ble/): how a phone sets the Echo up without a network.
 * The Home Assistant app and Home Assistant's improv_ble integration (py-improv-ble-client) find it by its advertising,
 * connect, may scan for networks, send a network name and password, and get the settings page's address back once the
 * Echo is on that network; Home Assistant then finds it by mDNS like any ESPHome device.
 *
 * When it advertises (worker thread, every second):
 *   - not set up yet (core_oobe: after an install or a reset, until Home Assistant took the Echo on) and no network for
 *     OFFLINE_OOBE s: authorized from the start, nobody needs to press anything on a device that has nothing to lose;
 *   - set up, but no network for OFFLINE_LOST s (the router changed, the Echo moved): "authorization required".  A
 *     phone connects, the ring shows that it waits, and a press of the action button while it does authorizes for
 *     AUTH_SECS, as the spec suggests.  Anyone in Bluetooth range could otherwise move an Echo that works onto a
 *     network of theirs;
 *   - once on a network: "provisioned" for PROVISIONED_SECS more (Home Assistant drops its discovery), then not at all.
 * A phone that is connected keeps it going, each mode for a time of its own and none for good (below).
 *
 * Who may switch the network or ask for a scan (may()): a fact with an origin, asked anew for every command, never
 * read off the state that is shown or off the mode:
 *   - the Echo is not set up and advertises for that (M_OOBE and core_oobe()), or
 *   - it is set up, advertises for a lost network (M_LOST), and the action button was pressed less than AUTH_SECS
 *     ago.  The press is for the Echo, not for one link: whichever phone is connected in that time may, the one that
 *     was there at the press or one that comes after it (Home Assistant's app and its integration both connect anew
 *     between their steps); none that comes later than AUTH_SECS, and a switch that worked spends it: the next one
 *     needs the button again.  It ends with M_LOST too.
 * Anything else, "provisioned" and advertising stopped (M_OFF) included: not authorized.  It was "the state is
 * authorized or provisioned", and M_OFF kept the state at authorized: a phone connected with the network back,
 * that ended its link and connected again on the advertising ble.c switches on as a link ends (the worker had not
 * yet taken the advertising's data away), was a link in M_OFF, where every switch and scan was taken with no button
 * and nothing ever let it go: an Echo that works joined and saved an open network of the phone's choosing (a review,
 * 2026-11-20, 5 times of 5; the same since the first version).  And "provisioned" had no end while the phone stayed:
 * one press was good for any number of switches.  So, beside may():
 *   - a link that is up while nothing is advertised (M_OFF) is ended, as it comes (on_connection) and by the worker;
 *   - the advertising's data is taken away before a link is ended for M_OFF, so its end switches nothing on;
 *   - a phone that reads the state gets what may() says (show()), in M_OFF "authorization required".
 *
 * Which press is the phone's (improv_authorize): only one made while a phone is connected, is served, and is shown
 * "authorization required", which is exactly while the ring shows the wait (show(): `waits`).  It was any press in
 * M_LOST: an Echo ten minutes without its network took the press meant for the music or the assistant, with no
 * phone there and nothing on the ring, opened AUTH_SECS for whoever connected next and said "authorized" in its
 * advertising for everyone in range to see (a review, 2026-11-20).  The time a press opens is still the Echo's (the
 * clients connect anew between their steps), so within it the advertising does say "authorized": that is the spec's
 * state byte, and what it tells is that somebody pressed for a phone a moment ago.
 * What the press cannot tell: WHOSE phone waits.  Improv has no pairing and a link no identity; the ring shows that
 * some central is connected and waits, and a press while it shows lets whatever is connected within AUTH_SECS switch
 * the network once.  Somebody in Bluetooth range who connects first and waits for the owner to press for his own
 * phone gets that press.  And there is one link: while a central holds it nothing is advertised, the owner's own
 * app cannot find or connect to the Echo, and the ring shows a wait all the while.  A device in range that means
 * to can hold that link, and the ring's wait with it, for as long as the Echo is without its network: it need only
 * read something now and then, or leave and come back, under whatever address it likes.  NOTHING HERE BOUNDS THAT,
 * and nothing can without what Improv lacks (a code shown and typed, a pairing).  What the owner can go by, and
 * docs/SECURITY.md tells him in these words: press only when your own app has asked for the button; if the app
 * cannot find or connect to the Echo while the ring shows the wait, something else holds the link: do not press.
 * It was said to be bounded: by an end after WAIT_SECS counted from the connection, and the address
 * of a central ended so not served for a minute (the "shun").  That bound nothing for a central that meant to
 * stay: one that left a second before the time and came back at once, or took a new address each time, held link
 * and ring for 20.0 of 20.0 s; and it broke the honest client.  Home Assistant's integration connects FIRST, then
 * shows its menu, identify, the form for the network's name and password, and only then asks for the button
 * (improv_ble's config flow), so the time ran while the owner typed; and its adapter or Bluetooth proxy has one
 * fixed address, which was then turned away for the minute after: the flow ended in "cannot connect" (a review,
 * 2026-11-20).  So the shun is gone, and what is left is housekeeping and claims to be no more:
 *   - a link that is not authorized is ended once nothing has come from it for WAIT_SECS: no read, no subscribe,
 *     no command, no request of any kind (ble.c tells of each: on_heard), counted from the last of those or from
 *     when the link was last without authorization, whichever is later.  That is for the phone left on a table
 *     with the app open, which would else keep the one link, the ring's wait and every login's "busy" until
 *     somebody found it.  WAIT_SECS against the real client: py-improv-ble-client ends its own link 90 s after it
 *     last asked or heard anything (DISCONNECT_DELAY) and connects anew at its next step, so a link that has been
 *     silent for 120 s is never that client's in a flow that is still going.  What the owner has to reach the
 *     Echo is therefore that client's time, not ours: asked for the button by Home Assistant, which then only
 *     waits for the state's notification, he has 90 s; then the client ends its link, the ring's wait goes off,
 *     and when the flow asks again it connects anew and the ring shows the wait again.  In the gap no phone
 *     waits: a press then is the assistant's (the Echo listens), not the phone's, and must be made again once
 *     the ring is back; and each such end is the end of a wait for web.c (web_quiet: strangers' logins kept
 *     back QUIET_MS from it).  The wait is not kept showing over the gap: a ring with no phone connected is
 *     the press with nobody there, which opened the button's time for whoever connected next (above).
 *     docs/GUIDE.md and docs/SECURITY.md tell the owner what he sees;
 *   - identify (a sound and a rainbow each) is done once in IDENT_OWN_SECS, the rainbow's own time, and for a phone
 *     that is not authorized on an Echo that is set up once in IDENT_SECS, for all links together: it was served
 *     to anyone as often as asked.  The ones over that are answered as done and change nothing.
 * And web.c is told of it at both ends (improv_holds; the attention hook -> main.c -> web_quiet): no login of a
 * browser that is not approved is armed while a phone is connected to an Echo that advertises for its lost network
 * (M_LOST), or while the button's
 * time, a switch or "provisioned" runs, nor for web.c's quiet time after the ring's wait for a phone went off, by
 * the press or without one; and a login that was armed before the phone's wait began ends when that wait goes off
 * (web.c: the press that follows may have been meant for the phone).  The login shows the same ring, and it was
 * armed the moment the phone's state said "authorized": the owner's second press ("did the first one take?") let in
 * a browser he had never seen.  All of that is M_LOST's: an Echo that is not set up shows no wait, takes no press
 * for a phone and holds no login back, and one taken on while it advertised so never comes to (below).  While
 * the button is a phone's business web.c also keeps its time on the flash (its hold_save: a restart of hassmic
 * forgets the phone, the link and the advertising, which begins again only after OFFLINE_LOST, but not whoever
 * is on his way to press for it).
 *
 * How long a phone is kept.  Each of these ends the link; only the first is moved by what a central does without
 * the button (above):
 *   - set up, not authorized: WAIT_SECS without anything from it;
 *   - "provisioned": PROVISIONED_SECS after the switch worked, whatever is connected.  It lasted while the phone
 *     stayed, and where the new network went away again that was for good: the state said "provisioned", no press
 *     was taken (nothing waited), every switch was refused, and the Echo could not be set up again until that
 *     phone left;
 *   - on a network again (the router was restarted; or a network given by other means to an Echo that is not set
 *     up): after ONLINE_SECS on the network, counted together however often the network goes and returns
 *     (on_ms; the worker looks once a second, and every look that finds the network counts the time since the
 *     look before it, a second at the most where that one found none: it was counted only between two looks
 *     that both found it, so a network back for less than a second at a time, 0.7 s of every 2 s for 30 s,
 *     counted nothing and kept the phone), and begun anew only by a switch's end and, set up, by what the button
 *     paid for: the press itself, a scan asked for within its time.  Not set up, a scan does not begin it anew:
 *     it did ("everything is paid for there"), and a central that asked for one every few seconds kept its link
 *     to an Echo that had got its network by other means for as long as it liked (20 s of 20 with ONLINE_SECS
 *     12), and was there when Home Assistant took the Echo on (below).  A first setup loses nothing by that:
 *     the phone connects, scans and sends the network while the Echo has none, and no time on a network runs
 *     before the switch, whose end begins it.  What holds, then: on a network, a link lasts ONLINE_SECS from
 *     the last switch's end at the most, and set up also from the last press or a scan within its AUTH_SECS;
 *     nothing a central can do without the button or a switch moves it.  A switch is the one thing left that
 *     does on an Echo that is not set up, and there a switch is what any phone may ask for anyway.
 *     It was counted from the network's last return (and before
 *     that from the link's beginning): a network that went for a moment once per ONLINE_SECS, with a central that
 *     was never authorized, kept M_LOST, the ring's wait and "busy" for every login for good.  It was "nothing under
 *     way, no scan" before that: the scan asked for no authorization, each one began the time anew, with a request
 *     to root and a write to the flash for each.  So on an Echo that is set up a scan needs the button as the
 *     switch does (not authorized: the error, nothing asked of root; beside a switch that runs after the
 *     button's time: "busy", nothing asked either).  The spec says of the switch that it requires
 *     authorization and of identify and device info that they do not; of the scan it says neither.  The clients
 *     lose nothing: on an Echo that is not set up everything is authorized from the start, and on one that is, the
 *     list of networks is for the person who pressed.
 * With no phone connected and nothing under way the advertising follows the network alone: back, it ends at once.
 * What "back" is not: the carrier a switch of our own brings up.  wifi.sh joins the new network before it knows
 * whether that works (an address, the router's answer: up to its 30 s each), and dhcpcd (-K) leaves the old address
 * on the interface meanwhile, which is all online() asks for.  A look that found that counted as the network's
 * return and began the time without a network anew: the switch failed (no lease, a router that does not answer),
 * the phone left to ask for the password again, as Home Assistant's client does after every failed command, and
 * the Echo, "ten seconds without a network", stopped advertising for OFFLINE_LOST, in the middle of its setup.
 * So no look counts while a switch runs, nor for SETTLE_SECS after one failed unless a look in between found the
 * carrier gone (root writes its answer as it lets go of the network; where it got back onto the old one, the
 * network is back for good and is counted from then) or another switch began.
 *
 * Home Assistant takes the Echo on while it advertises as not set up (M_OOBE; a phone may be connected: the network
 * came by its switch a moment ago, or by other means).  From then the Echo is set up, and:
 *   - M_LOST is entered in one way only: set up and OFFLINE_LOST without a network, counted from when it was taken
 *     on at the earliest.  A mode that is kept for what runs is never turned into it;
 *   - what is left of the setup ends as it would have: a switch under way is answered, and its "provisioned"
 *     stands for PROVISIONED_SECS, with the phone that set the Echo up connected or connecting anew to read the
 *     result and the page's address (Home Assistant finds a new Echo by mDNS within seconds of the switch, often
 *     before the app has read them).  Nothing that needs authorization is served in that time: may() is false
 *     (not core_oobe(), no button's time outside M_LOST), so no switch and no scan, the state says "provisioned",
 *     no wait is shown and no press taken.  What the spec asks no authorization for still is, as for any link
 *     that is served: device info, and identify once in IDENT_SECS (set up now and not authorized);
 *   - a scan that was asked for before is dropped as the Echo is taken on, whatever else stands: its answer is
 *     nobody's (the list of networks is for an Echo that is not set up, or for the person who pressed), and it
 *     holds nothing.  It held: stale() does not look while "provisioned" stands, "provisioned" did not end
 *     while a scan was out, so a central that asked for one after its switch kept the state, the mode and its
 *     link until root answered or SCAN_SECS, 19.3 s where PROVISIONED_SECS was 3, and was notified the list
 *     after the Echo was set up (a review, 2026-11-20).  So "provisioned" ends at its time whether or not a
 *     scan is out, taken on or not (not set up, where a scan may still be asked for beside it, the scan then
 *     keeps the mode until it is answered, and is answered to whoever is connected);
 *   - with neither a switch nor "provisioned", the link is ended at once and the network alone decides what
 *     is advertised: none while the Echo is on one.
 * It was "set up from then: M_LOST's rules" for whatever was connected: a central that had connected to the open
 * Echo and kept its link by scans (above) was, the moment Home Assistant took the Echo on, a phone that waits for
 * the button on an Echo that is online and freshly set up.  The ring showed the wait; the owner's press for the
 * login of his own browser, armed before (nothing is held back on an Echo that is not set up), was refused for the
 * browser and taken for the central, which then moved the Echo to an open network of its choosing; with no press,
 * the central leaving ended the owner's login (a review, 2026-11-20).  web.c's reasoning, main.c's and
 * docs/SECURITY.md's all say that a phone's wait exists only on an Echo ten minutes without its network: now it does.
 *
 * Locks: mu guards everything here and is held for nothing that waits: no write to the flash, and no other module's
 * lock but main.c's for the ring (below).  The controller's thread takes it for every
 * read, write and link (it is also the Bluetooth proxy's and A2DP's thread) and the button's thread for a press; so
 * what a command asks of others is decided under mu and done after it was let go (ask(): wifi.c's request is a
 * state file, two fsyncs on the Echo's flash, and a password's PBKDF2; the core's identify), with the command kept
 * as "being handled" meanwhile, and its answer's id put in place under mu again.  web.c asks improv_waits() and
 * improv_holds() under its own lock from every /api/login: those read two atomics the worker keeps and take
 * nothing, so web.c's lock is never held while mu is asked for.  The hooks: attention under mu (it takes main.c's
 * own small lock, held only over the ring's queue; for web.c it leaves a mark, web_quiet: an atomic and a signal.
 * It took web.c's lock there, which web.c holds over its clients' file, two fsyncs on the flash: the controller's
 * thread, the proxy's and A2DP's too, waited behind a browser being let in), authorized with none.
 * The worker's timed wait is on the monotonic clock: root sets the wall clock when Home Assistant's link returns,
 * which is when the network comes back, and a step backwards held up every end above for as long as the step.
 *
 * The network: wifi.c asks root (scripts/device/wifi.sh), as the settings page's switch does: the Echo goes onto the
 * network, and only once it has an address there and its router answers is it saved; else wpa_supplicant goes back to
 * what it had.  Only the PSK goes to root.  Improv itself sends the password in the clear over Bluetooth: that is the
 * protocol (ESPHome and the Voice PE do the same), and why an Echo that is set up wants the button first.
 * WPA2-PSK and open networks only (wifi.c), so the scan answer leaves out the networks it could not join.
 *
 * Names: the Echo's is made from the model and its MAC address (main.c name_make) and never set; what people call it is
 * given in Home Assistant when they add it.  So no host name or device name commands (capabilities without bit 3), the
 * device info tells the name.
 *
 * Characteristics, in ble.c's order (notifications go out lowest first: the state before the result): state, error,
 * RPC command, RPC result, capabilities.  An RPC packet: command, length, data, checksum (sum of the bytes before it);
 * data is strings, each a length and its bytes.  Writes may come in pieces (20 bytes each at the default MTU): they are
 * put together until the length says the packet is whole.
 */
#include "improv.h"
#include "net.h"
#include "ble.h"
#include "board.h"
#include "core.h"
#include "wifi.h"
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* HASSMIC_IMPROV_TIMES="<oobe> <lost> <auth> <provisioned> [<online> [<wait> [<identify>]]]" (tests) */
static int OFFLINE_OOBE = 20;       /* s without a network before a new Echo advertises: one with a saved network gets on first */
static int OFFLINE_LOST = 600;      /* s without a network before an Echo that is set up advertises */
static int AUTH_SECS = 60;          /* the button's authorization (the spec's suggestion) */
static int PROVISIONED_SECS = 60;   /* "provisioned" shown and advertised this long after it worked, then the phone is let go */
static int ONLINE_SECS = 60;        /* a phone still connected is let go after this long on a network (the button's own minute; the header says what begins it anew) */
static int WAIT_SECS = 120;         /* set up: a link without the button's authorization is ended once nothing came from it for this long (the header: past the real client's own 90 s) */
static int IDENT_SECS = 60;         /* set up and not authorized: one identify in this time */
#define IDENT_OWN_SECS   10     /* else one in the rainbow's own time (main.c IDENTIFY_MS) */
#define SCAN_SECS        25     /* root looks every 2 s and scans for 5 s */
#define JOIN_SECS       240     /* wifi.sh: 30 s to join, 30 s for an address, 45 s back; then it surely answered */
#define SETTLE_SECS       5     /* a carrier found this long after a switch failed is still that switch's (the header) */

enum { ST_AUTH_REQUIRED = 1, ST_AUTHORIZED, ST_PROVISIONING, ST_PROVISIONED };
enum { E_NONE, E_INVALID_RPC, E_UNKNOWN_RPC, E_UNABLE_TO_CONNECT, E_NOT_AUTHORIZED, E_BAD_HOSTNAME, E_UNKNOWN = 0xff };
enum { CMD_WIFI = 1, CMD_IDENTIFY, CMD_INFO, CMD_SCAN };
enum { C_STATE, C_ERROR, C_RPC, C_RESULT, C_CAPS };
enum { CAPS = 0x07 };           /* identify, device info, Wi-Fi scan */
enum { M_OFF, M_OOBE, M_LOST };

/* 00467768-6228-2272-4663-27747826800x, least significant octet first */
#define IMPROV_UUID(x) { x, 0x80, 0x26, 0x78, 0x74, 0x27, 0x63, 0x46, 0x72, 0x22, 0x28, 0x62, 0x68, 0x77, 0x46, 0x00 }

static const struct improv_hooks *H;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv;       /* improv_start: on the monotonic clock */
/* mu */
static int mode, st = ST_AUTH_REQUIRED, err, connected;
static int ended;               /* the link that is up is being ended by us (ble_server_drop asked): not served, not counted, no wait shown for it */
static int provisioned;         /* the last switch worked: "provisioned" is shown, for PROVISIONED_SECS */
static int waits;               /* what the hook was last told: a connected phone waits for the button */
static time_t auth_until, provisioned_at;   /* auth_until: the button's time, 0 none; only improv_authorize() sets it, in M_LOST */
static unsigned char rx[3 + 256], result[3 + 256]; static size_t rxn, resn;   /* command being put together; last result */
static unsigned char cmd[3 + 256]; static size_t cmdn;                          /* a whole command for the worker, until it is done with it */
static unsigned scan_id, join_id; static time_t scan_at, join_at;
static long long on_ms;         /* time on a network, all returns together, since advertising began or since what the button last paid for (the press, a scan asked for, a switch ended): at ONLINE_SECS a phone is let go */
static time_t wait_since;       /* M_LOST: since when the link that is up has been without authorization, 0: it is not (or none is up) */
static time_t heard_at;         /* when the link that is up last sent anything (on_heard; its beginning) */
static time_t settle_until;     /* a switch failed: until then a carrier is not the network's return (the header), 0 none */
static time_t ident_at;         /* the last identify done, 0 none */
/* what web.c asks under its own lock (improv_waits, improv_holds): kept by show(), read without mu */
static atomic_int a_waits, a_holds;

static time_t mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec; }
static long long mono_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }

/* net.c net_online: an IPv4 address on an interface with a carrier (HASSMIC_FAKE_ONLINE for the tests) */
static int online(void) { return net_online(); }

/* ---------------------------------------------------------------- packets */

static unsigned char sum(const unsigned char *p, size_t n) { unsigned s = 0; while (n--) s += *p++; return (unsigned char)s; }

/* the RPC result: command, then strings (NULL-terminated list of pointers, lengths in lens or strlen) */
static void set_result(int c, const char *const *s, const size_t *lens)
{
    size_t n = 2;
    for (int i = 0; s && s[i]; i++) {
        size_t l = lens ? lens[i] : strlen(s[i]);
        if (l > 255 || n + 1 + l > 2 + 255) break;
        result[n++] = (unsigned char)l; memcpy(result + n, s[i], l); n += l;
    }
    result[0] = (unsigned char)c; result[1] = (unsigned char)(n - 2);
    result[n] = sum(result, n); resn = n + 1;
    ble_server_notify(C_RESULT);
}
static void result1(int c, const char *s) { const char *l[] = { s, NULL }; set_result(c, l, NULL); }

static void set_state(int s) { if (s != st) { st = s; ble_server_notify(C_STATE); } }
static void set_error(int e) { if (e != err) { err = e; ble_server_notify(C_ERROR); } }

/* mu held.  Whether a switch or a scan may be asked for now: the header's rule.  Nothing else decides it */
static int may(time_t now)
{
    if (mode == M_OOBE) return core_oobe();
    return mode == M_LOST && auth_until && now < auth_until;
}

/* mu held.  The state a phone is shown follows from the facts, never the other way round; and the ring's pattern
 * for a phone that waits for the button (the hook is told only what changed); and what web.c reads: whether that
 * wait shows, and whether the button is a phone's setup's business at all just now (the header) */
static void show(time_t now)
{
    int here = connected && !ended;
    set_state(join_id ? ST_PROVISIONING : provisioned ? ST_PROVISIONED : may(now) ? ST_AUTHORIZED : ST_AUTH_REQUIRED);
    int w = here && mode == M_LOST && st == ST_AUTH_REQUIRED;
    int h = mode == M_LOST && (here || join_id || scan_id || provisioned || may(now));
    /* What web.c reads goes up before the ring shows a wait and down only after the mark of its end was left (the
     * hook -> web_quiet): at no time has a wait ended for web.c with neither the mark nor these saying so, and a
     * login it arms on a 0 read here was asked for before the ring showed anything for a phone */
    if (w) atomic_store(&a_waits, 1);
    if (h) atomic_store(&a_holds, 1);
    if (w != waits) { waits = w; if (H && H->attention) H->attention(w); }
    atomic_store(&a_waits, w);
    atomic_store(&a_holds, h);
}

/* mu held.  Home Assistant took the Echo on while it advertised as one that is not set up, and nothing of that
 * setup is left to end (a switch that runs, its "provisioned"): the advertising and whatever link there is are
 * nobody's any more (the header: M_LOST is not entered from here) */
static int stale(void) { return mode == M_OOBE && !core_oobe() && !join_id && !provisioned; }

/* mu held.  What it advertises: flags, the service, its data (state, capabilities); the name in the scan response */
static void advertise(void)
{
    static const unsigned char head[] = { 2, 0x01, 0x06, 17, 0x07, 0x00, 0x80, 0x26, 0x78, 0x74, 0x27, 0x63, 0x46, 0x72, 0x22, 0x28, 0x62, 0x68, 0x77, 0x46, 0x00 };
    unsigned char a[31], r[31]; size_t n = sizeof head, k = strlen(core_name);
    if (mode == M_OFF) { ble_advertise(NULL, 0, NULL, 0); return; }
    memcpy(a, head, n);
    a[n++] = 9; a[n++] = 0x16; a[n++] = 0x77; a[n++] = 0x46; a[n++] = (unsigned char)st; a[n++] = CAPS;
    memset(a + n, 0, 4); n += 4;
    int cut = k > 29;
    if (cut) { k = 29; while (k && ((unsigned char)core_name[k] & 0xc0) == 0x80) k--; }  /* not inside a UTF-8 character */
    r[0] = (unsigned char)(k + 1); r[1] = cut ? 0x08 : 0x09; memcpy(r + 2, core_name, k);
    ble_advertise(a, n, r, k + 2);
}

/* mu held.  The link that is up is ended by us: from here it is neither served nor counted as a phone that is
 * there, until the controller says it is gone (the worker asks again every second, should this one be lost) */
static void end_link(void) { ended = 1; ble_server_drop(); }

/* ---------------------------------------------------------------- GATT (controller thread) */

static size_t on_read(int c, uint8_t *out, size_t cap)
{
    size_t n = 0;
    if (!cap) return 0;
    pthread_mutex_lock(&mu);
    switch (c) {
    case -1: n = strlen(core_name); if (n > cap) n = cap; memcpy(out, core_name, n); break;
    case C_STATE: out[0] = (uint8_t)st; n = 1; break;
    case C_ERROR: out[0] = (uint8_t)err; n = 1; break;
    case C_CAPS: out[0] = CAPS; n = 1; break;
    case C_RESULT: n = resn < cap ? resn : cap; memcpy(out, result, n); break;
    }
    pthread_mutex_unlock(&mu);
    return n;
}

static void on_write(int c, const uint8_t *d, size_t n)
{
    if (c != C_RPC) return;
    pthread_mutex_lock(&mu);
    if (rxn + n > sizeof rx) rxn = 0;                   /* longer than any packet: what came before was garbage */
    if (n <= sizeof rx) { memcpy(rx + rxn, d, n); rxn += n; }
    if (rxn >= 3 && rxn >= (size_t)rx[1] + 3) {
        size_t len = (size_t)rx[1] + 3;
        if (rxn > len || rx[len - 1] != sum(rx, len - 1)) set_error(E_INVALID_RPC);
        else if (ended || mode == M_OFF) { }            /* a link we are ending: nothing of its is taken (command() says the same) */
        else if (cmdn) set_error(E_UNKNOWN);            /* the one before is still being handled */
        else { memcpy(cmd, rx, len); cmdn = len; pthread_cond_signal(&cv); }
        rxn = 0;
    }
    pthread_mutex_unlock(&mu);
}

/* anything at all from the central (ble.c: every request, command or answer of its, before it is looked at): the
 * link is somebody's who is doing something, and WAIT_SECS counts from here (the header) */
static void on_heard(void)
{
    pthread_mutex_lock(&mu);
    heard_at = mono();
    pthread_mutex_unlock(&mu);
}

static void on_connection(int up, uint64_t addr)
{
    (void)addr;                 /* whatever the central chose to call itself: nothing is decided by it (the header) */
    pthread_mutex_lock(&mu);
    time_t now = mono();
    /* A phone that connects starts with no error: Home Assistant's app reads it as it connects, and a failed switch
     * left there (unable to connect) made it give up before sending anything (cannoli, 2026-11-15).  Not while a
     * switch runs: a phone that dropped during it and comes back is to hear how it ends.  Left as it is when a phone
     * leaves. */
    connected = up; rxn = 0; ended = 0; wait_since = 0; heard_at = now;
    if (up && !join_id) err = E_NONE;
    if (up) fprintf(stderr, "improv: a phone connected\n");
    else fprintf(stderr, "improv: the phone left\n");
    /* A link while nothing is advertised: the advertising ble.c switched on as the last link ended, before the
     * worker took its data away, or a controller that had not yet heard "off".  Not ours to serve: ended at once
     * (the worker does the same every second, should this one be lost); may() refuses its commands meanwhile */
    if (up && mode == M_OFF) { fprintf(stderr, "improv: ...while nothing is advertised: its connection is ended\n"); end_link(); }
    else if (up && stale()) { fprintf(stderr, "improv: ...to an Echo that Home Assistant has taken on meanwhile: its connection is ended\n"); end_link(); }
    show(now);
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mu);
}

static const struct ble_server server = {
    .uuid = IMPROV_UUID(0x00), .nchr = 5,
    .chr = { { IMPROV_UUID(0x01), BLE_PROP_READ | BLE_PROP_NOTIFY }, { IMPROV_UUID(0x02), BLE_PROP_READ | BLE_PROP_NOTIFY },
             { IMPROV_UUID(0x03), BLE_PROP_WRITE | BLE_PROP_WRITE_NR }, { IMPROV_UUID(0x04), BLE_PROP_READ | BLE_PROP_NOTIFY },
             { IMPROV_UUID(0x05), BLE_PROP_READ } },
    .read = on_read, .write = on_write, .connection = on_connection, .heard = on_heard,
};

/* ---------------------------------------------------------------- commands (worker) */

/* the strings of a packet, at most max; -1 if they do not fill it exactly */
static int strings(const unsigned char *p, size_t n, const unsigned char **s, size_t *l, int max)
{
    size_t i = 2, end = n - 1; int k = 0;
    while (i < end) {
        if (k == max || i + 1 + p[i] > end) return -1;
        l[k] = p[i]; s[k++] = p + i + 1; i += 1 + p[i];
    }
    return i == end ? k : -1;
}

/* What a command asks of somebody else, decided under mu and done without it (ask(); the header's "Locks") */
enum { A_NONE, A_JOIN, A_SCAN, A_IDENTIFY };
struct ask { int kind, paid; unsigned char ssid[255]; size_t n; char pass[65]; };

/* mu held: the command is checked and answered as far as this file can; what is left for ask() goes into A */
static void command(const unsigned char *p, size_t n, time_t now, struct ask *a)
{
    const unsigned char *s[8]; size_t l[8]; int k = strings(p, n, s, l, 8);
    a->kind = A_NONE;
    if (ended || mode == M_OFF) return;                 /* (came in as the link was being ended) */
    if (k < 0) { set_error(E_INVALID_RPC); return; }
    set_error(E_NONE);
    switch (p[0]) {
    case CMD_WIFI:
        if (k != 2) { set_error(E_INVALID_RPC); return; }
        if (!may(now)) { set_error(E_NOT_AUTHORIZED); return; }
        if (join_id) { set_error(E_UNKNOWN); return; }  /* one switch at a time */
        if (l[1] >= sizeof a->pass || memchr(s[1], 0, l[1])) { set_error(E_UNABLE_TO_CONNECT); return; }
        memcpy(a->pass, s[1], l[1]); a->pass[l[1]] = 0;
        memcpy(a->ssid, s[0], l[0]); a->n = l[0];
        a->kind = A_JOIN;
        break;
    case CMD_IDENTIFY: {
        /* a sound and a rainbow each: not as often as anyone in range asks (the header) */
        int gap = !core_oobe() && !may(now) ? IDENT_SECS : IDENT_OWN_SECS;
        if (ident_at && now - ident_at < gap) break;
        ident_at = now; a->kind = A_IDENTIFY;
        break; }
    case CMD_INFO: {
        const char *l4[] = { "hassmic", VERSION, board.model, core_name, NULL };
        set_result(CMD_INFO, l4, NULL);
        break; }
    case CMD_SCAN:
        /* set up and no button, or its time over or spent: no scan (the header says why; a scan is a request to
         * root and a file on the flash, and it kept the link).  A switch of this phone's that runs was authorized
         * and is not to be told otherwise: busy, as wifi.c would say of a scan beside a switch, but said here
         * with nothing asked of wifi.c.  It was asked: join_id stands until the worker has read root's answer,
         * which it did only after the command, so a scan sent between root's answer and that look, with the
         * button's time long over, was taken: a request to root, a file on the flash, the list of networks, and
         * link and mode kept for the scan's time (a review, 2026-11-20: 2 times of 2) */
        if (!may(now)) { set_error(join_id ? E_UNKNOWN : E_NOT_AUTHORIZED); return; }
        if (scan_id) break;                             /* the answer to the one running answers this one too */
        a->kind = A_SCAN; a->paid = mode == M_LOST;     /* (may() held: asked for within the button's time) */
        break;
    default: set_error(E_UNKNOWN_RPC);
    }
}

/* mu NOT held (worker): what command() left.  0 and *ID: root was asked; -1: not, said in the log */
static int ask(struct ask *a, unsigned *id)
{
    char e[200] = ""; int busy = 0, r = 0;
    *id = 0;
    if (a->kind == A_IDENTIFY) core_identify();
    else if (a->kind == A_SCAN) { if ((r = wifi_scan(id, &busy, e, sizeof e))) fprintf(stderr, "improv: no scan: %s\n", e); }
    else if (a->kind == A_JOIN) {
        /* A name typed on a phone may end in a space its keyboard added after a word it completed (Dot 2, 2026-10-07:
         * "...NetzDerDinge " from the Home Assistant app, notfound).  Spaces at either end go unless a network of exactly
         * that name was seen in the last scan; a network named so on purpose is far rarer than that keyboard. */
        const unsigned char *sp = a->ssid; size_t sl = a->n;
        while (sl && sp[sl - 1] == ' ') sl--;
        while (sl && sp[0] == ' ') { sp++; sl--; }
        if (sl && sl != a->n && wifi_seen(a->ssid, a->n) != 1)
            fprintf(stderr, "improv: the network name came with spaces at its end, taken without (no network of that name seen)\n");
        else { sp = a->ssid; sl = a->n; }
        if ((r = wifi_join(sp, sl, a->pass, id, &busy, e, sizeof e))) fprintf(stderr, "improv: no network switch: %s\n", e);
        memset(a->pass, 0, sizeof a->pass);
    }
    return r;
}

/* mu held again: the answer of ask() put in place */
static void asked(const struct ask *a, int r, unsigned id, time_t now)
{
    if (a->kind == A_JOIN) {
        if (r) { set_error(E_UNABLE_TO_CONNECT); return; }
        join_id = id; join_at = now; provisioned = 0;
        settle_until = 0;                               /* (the last switch's carrier is not this one's business: left, a
                                                         * switch that worked within SETTLE_SECS of one that failed had its
                                                         * network not counted for the rest of them) */
        show(now);
        advertise();
    } else if (a->kind == A_SCAN) {
        if (r) { set_error(E_UNKNOWN); return; }
        scan_id = id; scan_at = now;
        if (a->paid) on_ms = 0;
    }
}

/* The scan's networks into one result (the client takes one): SSID, RSSI, security, as many as fit, strongest first */
static void scan_answer(const struct wifi_net *nets, int k)
{
    const char *s[3 * 32 + 1]; size_t l[3 * 32]; char rssi[32][8]; int m = 0, j = 0; size_t room = 255;
    for (int i = 0; i < k && j < 32; i++) {
        const char *sec = !strcmp(nets[i].sec, "open") ? "NO" : !strcmp(nets[i].sec, "psk") ? "WPA2" : NULL;
        if (!sec) continue;                             /* WPA3 only, enterprise, WEP: wifi.c cannot join them */
        snprintf(rssi[j], sizeof rssi[j], "%d", nets[i].signal);
        size_t need = 3 + nets[i].n + strlen(rssi[j]) + strlen(sec);
        if (need > room) break;
        room -= need;
        s[m] = (const char *)nets[i].ssid; l[m++] = nets[i].n;
        s[m] = rssi[j]; l[m++] = strlen(rssi[j]);
        s[m] = sec; l[m++] = strlen(sec);
        j++;
    }
    s[m] = NULL;
    set_result(CMD_SCAN, s, l);
}

/* ---------------------------------------------------------------- worker */

static void *worker(void *arg)
{
    (void)arg;
    time_t offline_since = mono(); long long tick = mono_ms(), t_ms; int was_on = 0, was_oobe = core_oobe();
    pthread_mutex_lock(&mu);
    for (;;) {
        /* (the monotonic clock, cv's: the header's "Locks") */
        struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); t.tv_sec += 1;
        if (!cmdn) pthread_cond_timedwait(&cv, &mu, &t);
        time_t now = mono();
        if (cmdn) {
            /* cmdn stays set while root or the core is asked without mu: the next command is told "still being
             * handled" (on_write), and nothing but this thread starts a switch or a scan */
            unsigned char c[sizeof cmd]; size_t n = cmdn; struct ask a; unsigned id; int r;
            memcpy(c, cmd, n); command(c, n, now, &a);
            if (a.kind) {
                pthread_mutex_unlock(&mu);
                r = ask(&a, &id);
                pthread_mutex_lock(&mu);
                asked(&a, r, id, now = mono());
            }
            memset(&a, 0, sizeof a); cmdn = 0;
        }

        /* Taken on by Home Assistant while advertising as not set up: a scan that is out is nobody's from here,
         * whatever else stands (a switch, "provisioned": the header), and before its answer is looked for.  One
         * asked for after that is refused by may(); one whose ask() was under way as the Echo was taken on
         * comes to here with its id just put in place */
        if (scan_id && mode == M_OOBE && !core_oobe()) scan_id = 0;
        if (scan_id) {
            struct wifi_net nets[48]; int k = wifi_scan_result(scan_id, nets, 48);
            if (k >= 0) { scan_answer(nets, k); scan_id = 0; }
            else if (now - scan_at > SCAN_SECS) { fprintf(stderr, "improv: the scan did not answer\n"); scan_id = 0; set_error(E_UNKNOWN); }
        }
        if (join_id) {
            char ip[48] = "", why[32] = ""; int r = wifi_join_result(join_id, ip, sizeof ip, why, sizeof why);
            if (!r && now - join_at > JOIN_SECS) { r = -1; snprintf(why, sizeof why, "timeout"); }
            if (r > 0) {
                char url[80] = "";
                if (core_web_port) snprintf(url, sizeof url, "http://%s:%d/", ip, core_web_port);
                fprintf(stderr, "improv: on the network, address %s\n", ip);
                /* the button's time is spent on it: another switch needs another press (not set up: may() does
                 * not ask the button) */
                join_id = 0; provisioned = 1; provisioned_at = now; on_ms = 0; auth_until = 0;
                show(now);
                if (url[0]) result1(CMD_WIFI, url); else set_result(CMD_WIFI, NULL, NULL);
            } else if (r < 0) {
                fprintf(stderr, "improv: the network switch failed (%s)\n", why);
                set_error(E_UNABLE_TO_CONNECT);
                join_id = 0; on_ms = 0;                   /* what is left of the button's time is still the phone's */
                settle_until = now + SETTLE_SECS;
            }
        }

        /* The network, as far as it is one: not the carrier of a switch that runs, nor of one that just failed
         * until it was seen gone (the header: it sent an Echo in the middle of its setup to M_OFF for OFFLINE_LOST) */
        int on = online();
        if (join_id) on = 0;
        else if (settle_until) { if (!on || now >= settle_until) settle_until = 0; else on = 0; }
        if (on) offline_since = 0; else if (!offline_since) offline_since = now;
        /* OFFLINE_LOST is time without a network as an Echo that is set up: what came before it was taken on does
         * not count towards it */
        int oobe = core_oobe();
        if (was_oobe && !oobe && offline_since) offline_since = now;
        was_oobe = oobe;
        /* time on a network, all its returns together (the header): a look that finds it counts the time since
         * the look before, and where that one found none, a second of it at the most (the worker's own pace; a
         * command's PBKDF2 or a slow flash between two looks is not time on a network nobody saw) */
        t_ms = mono_ms();
        if (on) on_ms += was_on || t_ms - tick < 1000 ? t_ms - tick : 1000;
        was_on = on; tick = t_ms;
        /* "provisioned" has its time and no more, whoever is connected: the phone has had the address, and a
         * state that stayed for as long as it did left an Echo whose new network went away again with no way
         * to be set up (no press taken: nothing waited; every switch refused).  A scan that is out does not
         * hold it either (it did: the header, at "taken on") */
        if (provisioned && !join_id && now - provisioned_at >= PROVISIONED_SECS) {
            provisioned = 0;
            if (connected && !ended) { fprintf(stderr, "improv: on the network for %d s and the phone is still connected: its connection is ended\n", PROVISIONED_SECS); end_link(); }
        }
        /* What is to be advertised.  Nobody here and nothing under way: by the network alone.  Else what runs is
         * kept in the mode it began in, with two ends of its own (the header): Home Assistant taking on an Echo
         * that advertised as not set up, which leaves only a switch and its "provisioned" to end; and the Echo
         * having been on a network for ONLINE_SECS in all since a switch ended or the button last paid for
         * something, never counted from a link's beginning or from the network's last return.  M_LOST comes of
         * the first line of the three below alone: a mode kept is never turned into it */
        int here = connected && !ended;
        if (stale()) {                                      /* (a scan went as the Echo was taken on: above) */
            if (here) { fprintf(stderr, "improv: Home Assistant took the Echo on with a phone still connected: set up now, its connection is ended\n"); end_link(); here = 0; }
        }
        int held = mode != M_OFF && (here || join_id || scan_id || provisioned), want, let_go = 0;
        if (!on && !oobe && now - offline_since >= OFFLINE_LOST) want = M_LOST;
        else if (!on && oobe && now - offline_since >= OFFLINE_OOBE) want = M_OOBE;
        else want = M_OFF;
        if (held) {
            want = mode;
            if (on && !join_id && on_ms >= ONLINE_SECS * 1000LL) { want = M_OFF; let_go = here; }
        }
        if (want != mode) {
            if (let_go) fprintf(stderr, "improv: on a network for %d s and the phone is still connected with nothing to do: its connection is ended\n", ONLINE_SECS);
            fprintf(stderr, "improv: %s\n", want == M_OOBE ? "not set up and no network: advertising, open to any phone" :
                    want == M_OFF ? "advertising stopped" : "no network for a while: advertising, a phone needs the action button");
            if (mode == M_OFF || want == M_OFF) err = E_NONE;
            if (mode == M_OFF) on_ms = 0;
            mode = want;
            /* the button's time and "provisioned" do not outlast what they were for */
            if (mode != M_LOST) auth_until = 0;
            if (mode == M_OFF) provisioned = 0;
            show(now);
            advertise();                                    /* (M_OFF: the data taken away, before the link is ended below) */
        }
        /* a link while nothing is advertised (on_connection says how one comes about), or one whose end the
         * controller has not told yet */
        if (connected && (mode == M_OFF || ended)) end_link();
        /* Set up, and the phone that is connected has no authorization, nor anything under way that one paid for:
         * once nothing has come from it for WAIT_SECS, on a network or not, the link is the next phone's to take.
         * A phone that was forgotten; one that means to stay only has to ask for something (the header) */
        if (mode == M_LOST && connected && !ended && !may(now) && !join_id && !scan_id && !provisioned) {
            if (!wait_since) wait_since = now;
            else if (now - (heard_at > wait_since ? heard_at : wait_since) >= WAIT_SECS) {
                fprintf(stderr, "improv: nothing from the phone for %d s, and no press of the action button for it: its connection is ended\n", WAIT_SECS);
                end_link();
            }
        } else wait_since = 0;
        show(now);                                          /* the button's time running out, a switch's end */
        static int last_st;
        if (mode != M_OFF && st != last_st) advertise();    /* the state is in the advertising too */
        last_st = st;

    }
    return NULL;
}

void improv_start(const struct improv_hooks *h)
{
    pthread_t t;
    H = h;
    if (!ble_present()) return;
    const char *times = getenv("HASSMIC_IMPROV_TIMES");
    if (times) sscanf(times, "%d %d %d %d %d %d %d", &OFFLINE_OOBE, &OFFLINE_LOST, &AUTH_SECS, &PROVISIONED_SECS, &ONLINE_SECS, &WAIT_SECS, &IDENT_SECS);
    /* the worker's wait by the monotonic clock: root sets the wall clock as the network returns (the header) */
    { pthread_condattr_t ca; pthread_condattr_init(&ca); pthread_condattr_setclock(&ca, CLOCK_MONOTONIC); pthread_cond_init(&cv, &ca); pthread_condattr_destroy(&ca); }
    ble_serve(&server);
    if (!pthread_create(&t, NULL, worker, NULL)) pthread_detach(t);
}

/* Whether a press of the action button is waited for by a phone right now (what the ring shows for it; only ever
 * in M_LOST, an Echo that is set up and was OFFLINE_LOST without its network): a login armed before that wait began
 * is not approved by the press (web.c web_approve).  Any thread, no lock */
int improv_waits(void) { return atomic_load(&a_waits); }

/* Whether the button is a phone's setup's business just now: set up, advertising for a lost network, and a phone
 * connected, or the button's time, a switch or "provisioned" running.  web.c arms no login of a browser it does
 * not know meanwhile: the ring would show the same, and the press after the phone's, or the one for it, would let
 * that browser in.  Any thread, no lock */
int improv_holds(void) { return atomic_load(&a_holds); }

int improv_authorize(void)
{
    pthread_mutex_lock(&mu);
    time_t now = mono();
    show(now);
    /* Only the press somebody was asked for: a phone is connected and served, shown "authorization required",
     * and the ring shows that wait (`waits` is all of that, as the hook was told).  So no switch under way, none
     * just done, the button's time not running already; and never a press with no phone there, which is the
     * music's or the assistant's (the header).  The one place that makes may() true on an Echo that is set up */
    int take = waits;
    if (take) {
        auth_until = now + AUTH_SECS; on_ms = 0;
        show(now);
        fprintf(stderr, "improv: authorized for %d s\n", AUTH_SECS);
        pthread_cond_signal(&cv);
    }
    pthread_mutex_unlock(&mu);
    if (take && H && H->authorized) H->authorized();    /* (the sound; web.c heard of the press from show(): the wait went off) */
    return take;
}
