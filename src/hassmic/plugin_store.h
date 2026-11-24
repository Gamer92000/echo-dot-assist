/* Plugins as files (docs/plugins.md 4.1 to 4.4, 5.1, 5.4): packages and their checks, what the owner accepted, each
 * plugin's settings and the files the owner gave it.  All of it under state/plugins/, hassmic's own, every write
 * through statefile.c.  The broker (plugin.c) runs what is here; the settings page (web.c) writes it.
 *
 *   state/plugins/<id>/pkg/           manifest.json, manifest.sig, listing.sig, plugin (0755): the one package.  A
 *                                     hassmic update never breaks it (sdk/PROTOCOL.md "Versions": a scope only
 *                                     adds), and a hassmic it falls back to that lacks a scope version it requires
 *                                     lists it as one that needs a newer hassmic (ps_manifest.newer)
 *   state/plugins/<id>/author.pub     the author's key, pinned at the first install
 *   state/plugins/<id>/accepted       what the owner accepted, and how (plugin_store.c, "what was accepted")
 *   state/plugins/<id>/config         0600: name:<type>=value, secrets among them (a value is its row's only while
 *                                     the row is of the type it was stored under)
 *   state/plugins/<id>/enabled        there: it runs
 *   state/plugins/<id>/files/<list>/<item>/   the item's files, `fields`, `title`, `checked`
 *   state/plugins/<id>/storage/<key>  what the plugin keeps (`storage`, 4.4): a file a key, written by the broker
 *   state/plugins/<id>/develop        there: the owner's development flag (a refused system call stops it, SIGSYS)
 *   state/plugins/<id>/sealed         the package accepted here has `wake`: an update without it wipes files/ and
 *                                     storage/ once it runs (3.3; plugin_store.c seal_upkeep)
 *   state/plugins/.stage              a package being uploaded
 *
 * Functions take no lock of the core's.  ps_lock guards the folder and what ps_scan read of it: the page's threads
 * and the broker's hold it while they use a struct ps_plugin (it is recursive: they may call back in here).  Order:
 * core_lock, then ps_lock. */
#ifndef PLUGIN_STORE_H
#define PLUGIN_STORE_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "plugmsg.h"

#define PS_MAX          16              /* plugins installed */
#define PS_ID_MAX       25              /* [a-z0-9-]{3,24} and the NUL */
#define PS_SETTINGS     32
#define PS_OPTIONS      12
#define PS_FIELDS       8
#define PS_ITEM_FILES   4
#define PS_ITEMS        64
#define PS_PACKAGE_MAX  (4 << 20)       /* a package: the index's limit */
#define PS_PROGRAMS_MAX (32 << 20)      /* all programs together */
#define PS_FREE_MIN     (32LL << 20)    /* left free on the partition, as sound packs leave it */
#define PS_VALUE_MAX    513
#define PS_ACCEPTED_WAS 2               /* forms of the accepted section's digest that earlier builds wrote */
#define PS_ITEM_ID_MAX  41              /* [a-z0-9_-]{1,40} */
#define PS_SCOPE_NAME   24              /* a scope's name in "requires": 1 to 24 of a-z, 0-9, . and _ */

enum { PS_BOOL, PS_INT, PS_SELECT, PS_TEXT, PS_HOST, PS_SECRET, PS_FILES };
enum { PS_SIDELOAD, PS_COMMUNITY, PS_OFFICIAL };    /* by the listing, the index key's (plugin_index.h): none, "community", "official" */

struct ps_field { char name[33], label[65], type[8]; double min, max, step; };  /* type: text, number, int */
/* an entry of `net.server` (4.6): the server the owner writes into the host row SETTING, over PROTO, at one of PORTS
 * or at the port the owner writes into the int row PORT_ROW */
#define PS_NET      4
#define PS_PORTS    8
struct ps_net { char setting[33], port_row[33]; int udp, nports; unsigned short ports[PS_PORTS]; };
/* an entry of `net.lan` or `net.internet` (4.6): any private address (lan) or any public address and name
 * (internet) the plugin names at CONNECT, over PROTO, at one of PORTS, or at any port but 5555 (ANY; for net.lan also
 * none of PS_PORT_RELAY's) */
struct ps_reach { int udp, any, nports; unsigned short ports[PS_PORTS]; };
/* Ports where a device of the network answers by asking the internet for the plugin: DNS (53) and DNS over TLS (853),
 * TCP and UDP.  The home's router forwards a query for any name it does not know, so a plugin limited to "your
 * network" (net.lan, net.udp, net.discover's search) spelled what it heard into names under a domain of its author's
 * and had it carried out by the router, past the firewall (N1 of a review of 7be9343).  Refused for those as 5555
 * is for all: in the manifest, under "any", by the broker and by plugin_net.c's worker.  Not a wall: a device that
 * relays by other means (a proxy, a router's other services) can still carry data further, which is why a plugin
 * that hears requests and reaches the network asks for the second press (ps_second) */
#define PS_PORT_RELAY(port) ((port) == 53 || (port) == 853)
/* docs/plugins.md 4.6, 4.10: net.udp, a count of UDP ports of its own (28940 to 28999, kept per plugin in
 * state/plugins/<id>/ports, the same at every start); net.discover, one port and where its search goes (every
 * network's broadcast address, or a multicast group); ha.entities and ha.actions, Home Assistant's entities and
 * actions named by the manifest */
#define PS_UDP_MAX      4
#define PS_PORT_FIRST   28940
#define PS_PORT_LAST    28999
/* ports net.discover may not name: hassmic's own (UDP 28930, 28932, the plugins' 28940-28999; TCP 28928-28931) and
 * Kiosk Satellite's claims (2330, arb.c): a broadcast comes back to the Echo's own sockets on its port */
#define PS_DISCOVER_OWN_FIRST 28928
#define PS_DISCOVER_OWN_LAST  28999
#define PS_DISCOVER_KIOSK     2330
#define PS_ENTITIES     16
#define PS_ACTIONS      8
#define PS_ARGS         4
#define PS_TYPES        8
enum { PE_BINARY_SENSOR, PE_SENSOR, PE_TEXT_SENSOR, PE_SWITCH, PE_BUTTON, PE_NUMBER, PE_SELECT, PE_EVENT, PE_KINDS };
struct ps_entity {
    char name[33], label[65], icon[65], unit[13];
    int kind, decimals;
    int page;                                                   /* a row of its own on the plugin's card too */
    double min, max, step;                                      /* PE_NUMBER */
    char options[PS_OPTIONS][33]; int noptions;                 /* PE_SELECT */
    char types[PS_TYPES][33]; int ntypes;                       /* PE_EVENT */
};
struct ps_action { char name[33], label[65]; int page, calls, nargs; struct { char name[33]; int is_int; } args[PS_ARGS]; };
/* The longest line ps_cap_line writes, from the manifest's bounds, so that no caller cuts one (a review, 2026-11-21:
 * buffers of 300 bytes cut net.server's line without a mark, and a second server's row was not in what the owner
 * accepted).  The longest is files': every file list's label (64 bytes, ", " between); net.server's, every entry's
 * row label, protocol and ports (or its port row's label), is shorter, which the assertion in plugin_store.c holds */
#define PS_CAP_LINE     (64 + PS_SETTINGS * 66)
/* the check's answer as the page reads it (web.c check_json): every line escaped (each byte two at most) */
#define PS_CHECK_JSON   (4096 + 18 * (2 * PS_CAP_LINE + 48) + 24 * (2 * (PS_CAP_LINE + 128) + 48) + PS_WIDENS_JSON)
struct ps_item_file { char suffix[17], as[33]; int max_kb, optional; };         /* as: the name it is kept under */
struct ps_setting {
    char name[33], label[65], help[241], unit[13], def[65];
    int type; long min, max;
    char options[PS_OPTIONS][33]; int noptions;                 /* PS_SELECT */
    struct ps_item_file files[PS_ITEM_FILES]; int nfiles;       /* PS_FILES: what an item is made of */
    struct ps_field fields[PS_FIELDS]; int nfields, max_items;
    int catalog;                                                /* the manifest's token of its catalog, -1 none (ps_catalog_json) */
};
struct ps_manifest {
    char id[PS_ID_MAX], name[49], version_name[25], author[49], source[201], license[33], description[241];
    long version; int memory_mb, threads;
    /* "requires" (sdk/PROTOCOL.md "Versions"): the least version of each scope it speaks (PM_SCOPE_*; 0 none), which
     * is what the broker speaks with it: core and the scope of each capability, nothing else */
    uint16_t requires[PM_SCOPES];
    /* ps_manifest_parse refused it as one for a newer hassmic: "requires" names a scope this build has not, or a
     * version above its own.  Only id, name, author, version and version_name were read, and the reason is "needs a
     * newer hassmic: <scope> <version>, ..." */
    int newer;
    uint8_t author_key[32], digest[32];         /* the program's BLAKE2b-256 */
    pm_caps_t caps;                             /* PM_CAPBIT(PM_CAP_*) */
    int storage_kb, files_mb;
    struct ps_net net[PS_NET]; int nnet;        /* net.server's entries, in their order (CONNECT's entry) */
    struct ps_reach lan[PS_NET]; int nlan;      /* net.lan's, in their order */
    struct ps_reach inet[PS_NET]; int ninet;    /* net.internet's */
    int udp_ports;                              /* net.udp's count */
    int discover_port; uint32_t discover_group; /* net.discover's port, its group (host order; 0: the broadcast) */
    struct ps_entity entities[PS_ENTITIES]; int nentities;     /* ha.entities, in their order */
    struct ps_action actions[PS_ACTIONS]; int nactions;        /* ha.actions */
    struct ps_setting settings[PS_SETTINGS]; int nsettings;
    uint8_t accepted[32];                       /* the digest of the accepted section (3.3): name, author, what it
                                                 * says of itself and where its source is, the capabilities with
                                                 * their parameters, the settings whole */
    uint8_t accepted_was[PS_ACCEPTED_WAS][32];  /* the same as builds before this one made it, newest first: what
                                                 * they wrote into `accepted` still counts (plugin_store.c) */
    uint8_t manifest_digest[32];                /* of manifest.json's bytes */
};

/* an installed plugin, as the page and the broker see it */
struct ps_plugin {
    struct ps_manifest m;
    int tier, enabled;
    int accepted;               /* what runs is what the owner accepted (the manifest's section is accepted's) */
    char how[80];               /* "button", "synced, voucher named <node>" (PS_HOW_SYNCED; the name is what the
                                 * voucher said, not proof of which Echo: web.c sync), "migration" */
    char refused[200];          /* why it may not be started whatever was accepted (3.3's combinations, a capability
                                 * this build lacks): "" where it may.  A package that came past ps_stage_check */
    char withdrawn[201];        /* the plugin index's revocation list names this version: its reason ("" where not) */
    int anyway;                 /* ... and the owner said to run it anyway, for the list this Echo has (ps_anyway) */
    int develop;                /* the owner's development flag (ps_develop): plugin.c spawns it so */
    int community_off;          /* not official (a community listing, or none), and community plugins are switched off
                                 * on this Echo (ps_community): refused says so; it is kept, and runs when they are on */
    int here;                   /* it has a package (pkg/).  0: nothing of it is left but its folder */
    int unread;                 /* its manifest is one this build does not read: m is the folder's name, or, where
                                 * it needs a newer hassmic (m.newer), its id, name, author and version */
    /* its listing.sig as this build reads it: there and not holding (no key of this build signed it: 5.2's key, the
     * build of the new one alone; or broken), the index key that signed it (pi_signer's; -1 none), the index's serial
     * in it (-1 none).  What ps_relist_reason and plugin.c's self test go by */
    int listing_unread, listing_key; long long listing_serial;
    /* ... and where it does not hold, the tier its own line gives (PS_OFFICIAL, PS_COMMUNITY; -1 none or not that
     * package's): a community listing ran by the owner's switch, as a file's does (plugin_selftest_ok) */
    int listing_said;
    /* Home Assistant's keys of its entities and actions, by the manifest's order: given once and kept in
     * state/plugins/.hakeys (ps_scan), never one another plugin's or hassmic's own has had; 0 where none could be
     * given (the file cannot be written): that one is not listed */
    uint32_t ent_key[PS_ENTITIES], act_key[PS_ACTIONS];
};
/* the plugins' keys in Home Assistant: a range of their own, above every key of hassmic's own (proto_esphome.c) */
#define PS_HAKEY_FIRST  0x40000000u
#define PS_HAKEY_LAST   0x7fffffffu
void ps_hakeys_forget(void);                                /* (tests: read state/plugins/.hakeys again) */

const char *ps_dir(void);                                   /* state/plugins */
/* ID's UDP ports (net.udp's count, then net.discover's one): the ones it had, or the lowest of 28940..28999 that no
 * other installed plugin has, kept in state/plugins/<id>/ports so that a plugin is reachable at the same port after
 * every start (a server that keeps its address, a phone's other end).  Lock held.  The count of NEED written into
 * OUT, or -1 when no port is left */
int  ps_udp_ports(const char *id, int need, unsigned short *out);
/* entity and action names (ha.entities, ha.actions): the manifest's, [a-z0-9_]{1,32} */
const char *ps_entity_kind(int kind);                       /* "sensor", "switch", ... */
void ps_lock(void);
void ps_unlock(void);
int  ps_id_ok(const char *id);                 /* [a-z0-9-]{3,24}, no - at either end or twice, no route word */
extern const char *const ps_route_words[];      /* (NULL-ended: the settings page's routes under /api/plugins/) */
int  ps_item_id_ok(const char *id);
/* The version of each scope this build has (PM_SCOPE_*: plugmsg.h's PM_SCOPE_VERSION_*), lowered for the PC's tests
 * by HASSMIC_PLUGIN_SCOPES=<scope>:<version>,... (0: this build lacks it; a version above the build's own is none it
 * can have and is ignored): a manifest whose requires name more is one for a newer hassmic (ps_manifest_parse).  The
 * index's cross-check builds this file so.  ps_scopes_env reads the variable, once; ps_scope_have after it */
void ps_scopes_env(void);
unsigned ps_scope_have(int scope);
void ps_scope_set(int scope, unsigned version);     /* (the unit tests: as the variable would, never above the build's) */
/* a host name or an IPv4 address as one is written (a host row's rule): net.internet's CONNECT names one so */
int  ps_host_name_ok(const char *v);
const char *ps_cap_name(int bit);                           /* "wake", "net.server", ...; NULL past the last */
/* The line the owner accepts for a capability (3.3), with the manifest's parameters filled in */
void ps_cap_line(const struct ps_manifest *m, int bit, char *out, size_t cap);
/* every version up to what M requires that widens what its capability lets a plugin do (pm_widens), its words beside
 * the capabilities' lines: [{"scope","version","words"}], PS_WIDENS_JSON bytes at most */
#define PS_WIDENS_JSON  (8 + PM_WIDENING * (2 * PM_VERSION_DOC_MAX + 64))
size_t ps_widens_json(const struct ps_manifest *m, char *out, size_t cap);

/* Every plugin installed (lock held by the caller, or at start), those that cannot run here with the reason (refused).
 * *list: PS_MAX entries of the store's own, good until the next ps_scan; the count */
int  ps_scan(const struct ps_plugin **list);
int  ps_list(const struct ps_plugin **list);               /* what the last ps_scan found, without reading anything */
const struct ps_plugin *ps_find(const char *id);            /* after ps_scan; NULL */
/* The seal (3.3, plugin_store.c seal_upkeep): BUSY, when set, says whether a process of ID runs a package other than
 * the one whose manifest has digest MANIFEST (plugin.c: the version told to stop, until it is gone); the wipe of an
 * update without `wake` waits for none to.  ps_seal_settle: the wipe now if it is due (plugin.c, as a new version
 * starts).  ps_sealed: what ID keeps was given under the seal: the installed manifest has `wake`, or a wipe is still
 * due (state/plugins/<id>/sealed) */
extern int (*ps_busy)(const char *id, const uint8_t manifest[32]);
void ps_seal_settle(const char *id);
int  ps_sealed(const char *id);
int  ps_program(const char *id, char *path, size_t cap);    /* the program's path, for the broker to open */

/* the least version of an official plugin this build installs or starts (5.2), 0 where it names none */
long ps_official_least(const char *id);
/* the highest version of ID that was ever installed on this Echo (state/plugins/.installed, only rising; 0: never),
 * LONG_MAX where that cannot be known (the file unreadable, or more ids than it keeps): for web.c's sync, which
 * installs nothing older without the button.  Takes the store's lock */
long ps_installed_max(const char *id);
void ps_floors_forget(void);        /* tests: what a restart of hassmic forgets of that file */
/* 5.2's move to a new index key (a review of a30d6ed, K2): the index's listing for the version installed here,
 * without the button and without an install (plugin_store.c says when).  ps_relist_wanted: 1 where ID's package
 * is VERSION with the manifest MANIFEST (BLAKE2b-256) and a listing of TIER by index key KEY
 * (pi_signer's) with SERIAL would replace its own (plugin_fetch.c asks it before it downloads).  ps_relist: PKG, the
 * index's package of it, decides by its own listing: 1 taken (listing.sig replaced; the caller has the plugins
 * scanned again: plugin_withdrawn_changed), 0 nothing to take, -1 refused (ERR: not the version installed, no
 * listing that holds).  Both take the store's lock */
int  ps_relist_wanted(const char *id, long version, const uint8_t manifest[32], int tier, int key, long long serial);
int  ps_relist(const uint8_t *pkg, size_t len, char *err, size_t errsz);
/* (ps_relist: -2 where it could not be taken now, out of memory or the flash refusing the write: asked again at a
 * later look, the row says why; -1 is the package's own answer, never asked again for it.)  ps_relist_reason: where
 * an installed plugin's listing may be one the index lists anew, as far as this Echo can tell without the index: 2
 * one that does not hold under this build's keys (plugin_fetch.c looks within a minute, then hourly), else 1 one by
 * the first of two keys (5.2's step 1: the second is the new one) or under the build's least index serial (the daily
 * look then reads the index too), else 0.  The versions in SKIP (NSKIP of them) are left out: the last look found
 * them not listed.  Takes the store's lock */
struct ps_idv { char id[PS_ID_MAX]; long version; };
int  ps_relist_reason(const struct ps_idv *skip, int nskip);

/* ---- a package on its way in: upload, check, install */
/* GEN: the claim the staging file is for (web.c's install slot; any number but 0, new with each claim).  begin
 * makes the file that claim's; a piece is written only for the claim that began the file (plugin_store.c stage_gen) */
int  ps_stage_begin(unsigned gen, long size, char *err, size_t errsz);
int  ps_stage_chunk(unsigned gen, long off, const void *data, size_t n, char *err, size_t errsz);
/* What of an update is not what the owner accepted (3.3 "wider"), a line each as the page shows it.  The last
 * PS_CHANGES_ASK places are for lines that ask (PS_NEW, PS_MORE, PS_CHANGED): many lines that only show (a raise
 * within over many versions, the limits) never leave out one the owner presses the button for (finding 4 of a review
 * of 4c55b54) */
#define PS_CHANGES      24
#define PS_CHANGES_ASK  8
enum { PS_NEW = 1,              /* a capability that was not accepted, a row the settings did not have */
       PS_MORE,                 /* a quantity beyond what was accepted: a larger quota, another list, another entry */
       PS_CHANGED,              /* anything else that is not as accepted, more or not: the plugin's name, its author's,
                                 * a row's label, help, default, limits or files, a capability's parameters */
       PS_LIMIT,                /* memory_mb or threads: limits, not powers (5.4).  Shown; no button for it */
       PS_SPEAKS };             /* it requires a later version of a scope, one that only says more of what its
                                 * capability's line says (sdk/messages.def "within").  Shown; no button for it.  A
                                 * version that widens is PS_MORE, and asks */
/* a change's kind as the page names it (web.c's check: "new", "more", "changed", "limit", "speaks"; web/plugins.js
 * shows the last two apart and asks for the button for none of them) */
const char *ps_change_kind(int kind);
struct ps_changes {
    int update;                 /* the owner accepted an earlier version of this author's: c[] is against that */
    int ask;                    /* a line of another kind than PS_LIMIT and PS_SPEAKS (held or not): the button */
    int unsure;                 /* the version installed here says its number by nothing (a manifest this build does
                                 * not read, no listing): "never back" cannot be held, so the button whoever vouches */
    int n;
    int more, more_shown;       /* lines c[] has no place for: that ask (the page's "and N more" under what asks), that
                                 * only show (PS_LIMIT, PS_SPEAKS: under the later version's lines) */
    struct { int kind; char line[PS_CAP_LINE + 128]; } c[PS_CHANGES];     /* a capability's line and what it was */
};
/* Everything 5.4 lists, on the staged package; nothing is kept yet.  0: *m is its manifest, *tier where it comes
 * from, *button whether the owner's press is needed: a new plugin, another author key, or an update that is not what
 * was accepted or less of it (*ch, NULL when the caller does not show it: which lines; plugin_store.c wider() has
 * the rule).  -1 with the reason */
int  ps_stage_check(struct ps_manifest *m, int *tier, int *button, struct ps_changes *ch, char *err, size_t errsz);
/* a key's fingerprint, for an owner to compare with what the author publishes: "xxxx xxxx xxxx xxxx" */
void ps_fingerprint(const uint8_t key[32], char out[20]);
/* how many of S's first N bytes fit into MAX without cutting a UTF-8 character: for "%.*s" of a manifest's words */
int  ps_cut(const char *s, size_t n, size_t max);
/* strict UTF-8 (plugin_store.c ps_utf8_char): the length of the character at S, 0 none; whether all of N bytes are;
 * a text mended in place for the log and the page (what is no character "?", a control character a space) */
size_t ps_utf8_char(const char *s, size_t n);
int  ps_utf8_ok(const char *s, size_t n);
void ps_utf8_mend(char *t);
/* The checked package into place, accepted as HOW says.  EXPECT: the manifest's digest of the package the caller
 * means (ps_stage_check's m->manifest_digest), NULL for whichever was checked last.  PRESS: what the caller has for
 * the owner's acceptance; the store checks the package again under its lock and installs without a press only what
 * needs none NOW.  0; -1 and why; PS_NEEDS_BUTTON and why: nothing written, the package stays staged and checked */
enum { PS_NO_PRESS,             /* nothing: only what is accepted already, or less of it */
       PS_PRESSED,              /* the action button, for this package's dialog */
       PS_VOUCHED,              /* another Echo's voucher in its place (web.c sync): official packages only */
       PS_PRESSED2,             /* the action button twice: the dialog's, then the second confirmation's (ps_second) */
       PS_VOUCHED_PRESSED };    /* a voucher for the first press, this Echo's button for the second */
#define PS_NEEDS_BUTTON (-2)
/* the package hears every request and can send what it hears off the Echo (ps_second), and PRESS did not have the
 * second press: nothing written, the package stays staged and checked; the caller asks for it and comes again */
#define PS_NEEDS_SECOND (-3)
/* how `accepted` words an install on another Echo's voucher, the node after it.  A prefix code reads (web.c, the
 * page); builds of the branch before wrote "synced from <node>", which the page still reads as the same */
#define PS_HOW_SYNCED "synced, voucher named "
int  ps_stage_install(const char *how, const uint8_t expect[32], int press, char *err, size_t errsz);
/* request.audio's rules beside the combinations (3.3; plugin_store.c "request.audio's two rules").
 * ps_second: M hears every request and has a way out (any net.*): wherever the button is asked for, the Echo asks
 * for it twice; LINE (may be NULL) the second screen's words, "This plugin hears every request and can send what it
 * hears to the internet / your network / the server you enter."  1, or 0.
 * ps_engine_set: the plugin that is the wake word engine ("" none), the core's word.  Any thread, any lock.
 * ps_partner (ps_lock held): M (of TIER) may not be installed (INSTALL 1: both halves) or started (0: only the
 * first): it has request.audio and the engine is a plugin nobody vouches for; INSTALL, it is that engine's plugin, not
 * official, and a plugin with request.audio is switched on.  -1 and why, 0.
 * ps_engine_refused: plugin ID may not be chosen as the wake word engine now: not official, and a plugin with
 * request.audio is switched on; or not official while community plugins are switched off.  1 and why.  Takes ps_lock */
int  ps_second(const struct ps_manifest *m, char *line, size_t cap);
int  ps_stage_second(char *line, size_t cap);          /* ps_second of the package checked last (staged) */
void ps_engine_set(const char *id);
int  ps_partner(const struct ps_manifest *m, int tier, int install, char *err, size_t errsz);
int  ps_engine_refused(const char *id, char *why, size_t cap);
void ps_stage_drop(void);
int  ps_remove(const char *id);
/* Community plugins (docs/plugins.md 2.7, 3.3): whatever is not listed as official by the plugin index, a community
 * listing or none (a file), is installed and started only while the owner has switched them on (the setting
 * plugins_community, off by default: the settings page's, an import's, a preset's as any other setting; each such
 * plugin is still installed with the action button).  Off: ps_stage_check refuses such a package, ps_scan marks those
 * installed community_off (refused, kept).  ON: 1, 0, or -1 to read.  Any thread, any lock; the caller of a change
 * rescans (plugin.c plugin_community).  PS_COMMUNITY_OFF: the card's words, the tier's in front */
int  ps_community(int on);
/* whether the switch keeps installed plugin E off as it stands NOW (ps_lock held): not official, the switch off, and
 * nothing else that ps_scan says first (its own mark, community_off, is the scan's and lags a change of the switch
 * until plugin.c's broker has rescanned).  For whoever judges a request by it: the wake word engine's choice and its
 * words (ps_engine_refused, plugin.c plugin_community_off_now) */
int  ps_community_keeps_off(const struct ps_plugin *e);
#define PS_COMMUNITY_OFF "community plugins are switched off on this Echo: switch them on on the Plugins page, or remove it"
/* "Run it anyway" (docs/plugins.md 5.6): ID runs though the revocation list of SERIAL withdraws it, until a list with
 * another serial is taken (web.c, after the action button; the caller rescans).  0, or -1 */
int  ps_anyway(const char *id, long long serial);
/* a file of the plugins' state opened to read: a regular file or NULL, never waited for (a FIFO of that name) */
FILE *ps_fopen(const char *path);
int  ps_enable(const char *id, int on);
/* The development flag (docs/plugins.md 2.2, 5.8): a system call the filter refuses raises SIGSYS in the plugin
 * instead of answering EPERM, and the SDK's client says which before it ends.  The owner's, per plugin, on its card;
 * takes effect at its next start.  0, or -1 */
int  ps_develop(const char *id, int on);

/* ---- storage (4.4, capability `storage`): the plugin's own, a value per key, within the accepted kB.  ID's, by its
 * id; the store's lock taken inside.  Keys: [a-z0-9._-], 1 to PS_KEY_MAX, not beginning with a dot nor ending in
 * ".tmp" and digits (statefile.c's temporary names).  What the broker hands on; the write bucket is the broker's */
#define PS_KEY_MAX      48
#define PS_KEYS         128
#define PS_VALUE_BYTES  16384
int  ps_store_key_ok(const char *key);
/* 0 and *n (the value cut to CAP: *n is its size), PS_STORE_NONE no such key, -1 (no such plugin, no `storage`) */
#define PS_STORE_NONE   1
#define PS_STORE_FULL   2
int  ps_store_get(const char *id, const char *key, void *out, size_t cap, size_t *n);
/* 0; PS_STORE_FULL: over the accepted kB or PS_KEYS keys (nothing written); -1 */
int  ps_store_put(const char *id, const char *key, const void *v, size_t n);
int  ps_store_del(const char *id, const char *key);        /* 0, PS_STORE_NONE, -1 */
/* the bytes it keeps, and *keys how many keys (NULL: not asked) */
long long ps_storage_used(const char *id, int *keys);
/* All of it as one digest (BLAKE2b-256 over each key in name order: the key, a NUL, its size as 8 bytes LE, its
 * value): what the page's copy between Echos compares (5.4).  0, or -1 (no storage) */
int  ps_storage_digest(const char *id, uint8_t out[32]);
/* All of it as one tar of its keys (the page carries it to another Echo): malloc'd, *n its length; NULL */
void *ps_storage_tar(const char *id, size_t *n);
/* Everything it keeps replaced by the keys in the tar DATA, or nothing: each key checked, the accepted kB held to.
 * The plugin is stopped by the caller first (plugin.c plugin_storage_replace).  CHECK_ONLY: the checks alone, nothing
 * written (asked before the plugin is stopped).  0, or -1 with the reason */
int  ps_storage_replace(const char *id, const void *data, size_t n, int check_only, char *err, size_t errsz);

/* ---- settings (4.3): the owner's to write */
/* a setting's value into out ("" and 0 when unset and without default; secrets too: the broker's, never the page's).
 * Only one that was stored for a row of this row's type and that the row as it is now would take */
int  ps_config_get(const struct ps_plugin *p, const char *name, char *out, size_t cap);
/* the same without the row's default: only what the owner stored (net.server's rows, which have no default either:
 * connect_message, web.c's restart when one changes).  1 a value, 0 none */
int  ps_config_owner(const struct ps_plugin *p, const char *name, char *out, size_t cap);
/* whether row NAME is one a net.server entry names, as its server or its port */
int  ps_net_row(const struct ps_plugin *p, const char *name);
/* a host row set to "": unset (its default again); a secret too; a text row keeps "" as its value */
int  ps_config_set(const struct ps_plugin *p, const char *name, const char *value, char *err, size_t errsz);

/* ---- files (4.4): items of a files setting.  LIST is the setting's name */
struct ps_item {
    char id[PS_ITEM_ID_MAX], title[65]; int checked;
    struct { char name[33], value[257]; } fields[PS_FIELDS]; int nfields;
    struct { char suffix[17]; long size; } files[PS_ITEM_FILES]; int nfiles;
};
int  ps_items(const struct ps_plugin *p, const char *list, struct ps_item *out, int max);
/* a new item's files arrive in pieces into a folder aside; ps_item_keep puts it into place (after the plugin's
 * FILE_OK, or unchecked while the plugin is down), ps_item_drop removes what was uploaded */
int  ps_item_begin(const struct ps_plugin *p, const char *list, const char *item, char *err, size_t errsz);
int  ps_item_chunk(const struct ps_plugin *p, const char *list, const char *item, const char *suffix, long off, const void *data, size_t n, char *err, size_t errsz);
int  ps_item_keep(const struct ps_plugin *p, const char *list, const char *item, char *err, size_t errsz);
void ps_item_drop(const struct ps_plugin *p, const char *list, const char *item);
int  ps_item_delete(const struct ps_plugin *p, const char *list, const char *item);
/* title: what the plugin's FILE_OK named it (letters, digits, spaces; 40 at most: it is said to Home Assistant) */
int  ps_item_checked(const struct ps_plugin *p, const char *list, const char *item, const char *title);
int  ps_item_field(const struct ps_plugin *p, const char *list, const char *item, const char *name, const char *value, int only_if_unset, char *err, size_t errsz);
/* the same into an upload still aside (ps_item_begin): kept with its files by ps_item_keep, or dropped with them */
int  ps_item_field_staged(const struct ps_plugin *p, const char *list, const char *item, const char *name, const char *value, char *err, size_t errsz);
const struct ps_field *ps_item_field_of(const struct ps_plugin *p, const char *list, const char *name);     /* NULL: none */
/* At start, after the first scan (plugin.c plugin_start): an Echo whose microWakeWord plugin read the built-in
 * engine's folder state/mww as its list (builds until 2026-11-21) gets copies of the models it had checked there in
 * its own list, once (state/plugins/.mww-own), only the official plugin with `wake`, never over a model it has: the
 * files and the tuned fields, unchecked (it checks and names them as it runs).  state/mww loses the plugin's marks
 * (checked, title, fields) with that pass and at every start after it, so that it is the built-in engine's alone;
 * without that plugin there and read, everything is left for a later start.  The models copied */
int  ps_mww_own(void);
/* a file of an item, opened read-only (staged: the one just uploaded, for the plugin's check): the descriptor, -1 */
int  ps_item_open(const struct ps_plugin *p, const char *list, const char *item, const char *suffix, int staged);
long long ps_files_used(const struct ps_plugin *p);         /* bytes of all its items */

/* An item's files as one digest: BLAKE2b-256 over, per file it has in the manifest's order, the suffix, a NUL, the
 * size (8 bytes, little-endian) and the content.  For the page's copy between Echos (5.4): an item both have with
 * the same digest is not sent again.  0, or -1 (no such plugin, list or item; or a file changed while it was read).
 * By the plugin's id, and called WITHOUT ps_lock: it takes the lock itself, only to open the files, and reads them
 * (megabytes) without it */
int  ps_item_digest(const char *id, const char *list, const char *item, uint8_t out[32]);
/* 1 when NAME is a secret row of P: what a caller that got the value in clear must not store (web.c /set) */
int  ps_setting_secret(const struct ps_plugin *p, const char *name);

/* ---- sync between Echos (5.4 "the acceptance travels"; web.c has the rules)
 * A piece of the installed package of ID, by member (manifest.json, manifest.sig, plugin,
 * LICENSE, listing.sig): *data (the caller's to free) and *n bytes from OFF, LEN at most; *total the member's size.
 * 0, or -1: no such plugin, package or member.  The store keeps a package's members, not its tar: the page that
 * carries one to another Echo packs them again (web/tar.js), and "the very package" is what its signatures say:
 * the manifest by its digest, the program by the digest the manifest names, the listing by the index key */
int  ps_pkg_read(const char *id, const char *member, long off, long len, void **data, size_t *n, long *total);
/* The acceptance voucher's MAC (16 bytes), keyed with the network's page key (arb_web_key) over what it binds:
 *   "hassmic plugin acceptance 1" | target's web key (32) | nonce (16) | browser's key (32) | source's node, NUL |
 *   plugin id, NUL | manifest.json's digest (32) | author key (32) | accepted section's digest (32, the form this
 *   build writes)
 * Both ends are builds of this file or disagree: tests/unit/plugin_store_test.c holds the layout to a vector made
 * outside it, tests/fake_plugins_sync.py makes vouchers in Python */
void ps_voucher(uint8_t out[16], const uint8_t key[32], const uint8_t target[32], const uint8_t nonce[16], const uint8_t browser[32],
                const char *node, const char *id, const uint8_t manifest[32], const uint8_t author[32], const uint8_t accepted[32]);

/* the manifest's catalog of a files setting, as the manifest has it (JSON), "" none */
size_t ps_catalog_json(const struct ps_plugin *p, const char *list, char *out, size_t cap);

/* for tests and tools: a manifest's text parsed and checked as ps_stage_check does (no signatures) */
int  ps_manifest_parse(const char *text, size_t len, struct ps_manifest *m, char *err, size_t errsz);
int  ps_elf_check(const uint8_t *elf, size_t len, char *err, size_t errsz);
#endif
