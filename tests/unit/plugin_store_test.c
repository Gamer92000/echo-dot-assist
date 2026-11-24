/* Packages as the Echo checks them (src/hassmic/plugin_store.c, docs/plugins.md 5.1): manifests, programs and
 * whole packages that are wrong in one way each, then thousands that are wrong at random; what an update may be
 * without the button, pair by pair and at random; what a manifest requires against this build's scopes, and a plugin
 * that needs a newer hassmic (a fall back); stored settings against rows that go, come back and change their kind;
 * FIFOs where files are read.  Built with ASan and
 * UBSan: what a browser may upload must be refused, never read past.
 *   plugin_store_test <a static program for this machine> <a dynamic one> */
#define _GNU_SOURCE
#include "plugin_store.h"
#include "plugmsg.h"
#include "plugin_index.h"
#include "json.h"
#include "../../src/third_party/monocypher.h"
#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
/* Every manifest here requires core and the scope of each capability it asks for, at version 1, as a manifest must
 * (sdk/PROTOCOL.md "Versions"): with_requires() writes that "requires" into the text from its "capabilities", so that
 * a test about something else is not refused for it.  auto_requires 0: the text as it is (the tests of requires) */
static int auto_requires = 1;
/* "<scope>:<n>,...": those scopes required at N instead of 1 (the demo's later versions, SCOPES_DEMO) */
static const char *req_up = "";
static int req_of(const char *name)
{
    size_t n = strlen(name); const char *p = req_up;
    while ((p = strstr(p, name))) { if ((p == req_up || p[-1] == ',') && p[n] == ':') return atoi(p + n + 1); p += n; }
    return 1;
}
static size_t with_requires(char *t, size_t cap)
{
    struct json j = { 0 }; size_t n = strlen(t), o, tail; int caps, req; char reqs[700];
    if (!auto_requires || json_parse(&j, t, n)) { json_free(&j); return n; }
    caps = json_get(&j, 0, "capabilities"); req = json_get(&j, 0, "requires");
    if (caps < 0 || req < 0 || json_type(&j, caps) != J_OBJ) { json_free(&j); return n; }
    o = (size_t)snprintf(reqs, sizeof reqs, "{ \"core\": 1");
    for (int k = json_first(&j, caps); k >= 0; ) {
        int v = j.t[k].next; char name[40];
        if (v >= j.t[caps].next) break;
        if (json_str(&j, k, name, sizeof name)) for (int b = 0; ps_cap_name(b); b++) if (!strcmp(name, ps_cap_name(b))) o += (size_t)snprintf(reqs + o, sizeof reqs - o, ", \"%s\": %d", name, req_of(name));
        k = json_next(&j, caps, v);
    }
    o += (size_t)snprintf(reqs + o, sizeof reqs - o, " }");
    { size_t a = (size_t)j.t[req].start, e = (size_t)j.t[req].end; tail = n - e;
      if (n - (e - a) + o < cap) { memmove(t + a + o, t + e, tail + 1); memcpy(t + a, reqs, o); n = n - (e - a) + o; } }
    json_free(&j);
    return n;
}
/* a plain ustar member at OFF of OUT (header and data) */
static void tar_put(uint8_t *out, size_t off, const char *name, const void *data, size_t n)
{
    uint8_t *h = out + off; unsigned sum = 0;
    memset(h, 0, 512); snprintf((char *)h, 100, "%s", name); memcpy(h + 100, "0000600", 8); snprintf((char *)h + 124, 12, "%011lo", (unsigned long)n);
    h[156] = '0'; memcpy(h + 257, "ustar", 6); memset(h + 148, ' ', 8); for (int i = 0; i < 512; i++) sum += h[i]; snprintf((char *)h + 148, 8, "%06o", sum);
    memcpy(h + 512, data, n);
}
static int gone(const char *p, const struct stat *s, int t, struct FTW *f) { (void)s; (void)t; (void)f; return remove(p); }
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s (line %d): %s\n", what, __LINE__, err); fails++; } else printf("ok   %s\n", what); } while (0)

static char err[300];
static struct ps_manifest m;
static uint8_t seed[32] = { 1, 2, 3 }, sk[64], pk[32], rsk[64], rpk[32];     /* the author's; rsk: the plugin index's */

static const char *MAN =
    "{ \"id\": \"probe\", \"name\": \"Probe\", \"version\": 3, \"requires\": { \"core\": 1 }, \"author\": \"someone\","
    "  \"author_key\": \"%s\", \"digest\": \"%s\", \"memory_mb\": 16, \"threads\": 2,"
    "  \"capabilities\": { %s },"
    "  \"settings\": [ { \"name\": \"server\", \"label\": \"Server\", \"type\": \"host\" },"
    "                  { \"name\": \"port\", \"label\": \"Port\", \"type\": \"int\", \"min\": 1, \"max\": 65535, \"default\": 514 } %s ] }";
static const char *FILES_SETTING =
    ", { \"name\": \"models\", \"label\": \"Models\", \"type\": \"files\", \"item\": { \"by\": \"stem\", \"files\": [ { \"suffix\": \".bin\", \"max_kb\": 64 } ] }, \"max_items\": 4,"
    "    \"fields\": [ { \"name\": \"level\", \"label\": \"Level\", \"type\": \"int\", \"min\": 0, \"max\": 9 } ] }";

static void hex(char *o, const uint8_t *b, size_t n) { for (size_t i = 0; i < n; i++) sprintf(o + 2 * i, "%02x", b[i]); }

static size_t manifest(char *out, size_t cap, const char *caps, const char *more, const uint8_t digest[32])
{
    char k[65], d[65]; hex(k, pk, 32); hex(d, digest, 32);
    snprintf(out, cap, MAN, k, d, caps, more);
    return with_requires(out, cap);
}

static int parse(const char *caps, const char *more)
{
    static char t[4096]; uint8_t dg[32] = { 0 };
    size_t n = manifest(t, sizeof t, caps, more, dg);
    err[0] = 0;
    return ps_manifest_parse(t, n, &m, err, sizeof err);
}

/* A manifest with one of everything the Echo reads, for the rules of how it is written (an audit of what the reader
 * took for something else, 2026-11-19): with() has it with one text of it replaced, parsed */
static const char *RICH =
    "{ \"id\": \"probe\", \"name\": \"Probe\", \"version\": 3, \"author\": \"someone\","
    " \"author_key\": \"0101010101010101010101010101010101010101010101010101010101010101\","
    " \"digest\": \"0202020202020202020202020202020202020202020202020202020202020202\","
    " \"memory_mb\": 16, \"threads\": 2, \"models\": [\"*\"],"
    " \"capabilities\": { \"wake\": true, \"files\": 2 }, \"requires\": { \"core\": 1, \"wake\": 1, \"files\": 1 },"
    " \"settings\": [ { \"name\": \"server\", \"label\": \"Server\", \"type\": \"host\" },"
    " { \"name\": \"port\", \"label\": \"Port\", \"type\": \"int\", \"min\": 1, \"max\": 65535, \"default\": 514 },"
    " { \"name\": \"mode\", \"label\": \"Mode\", \"type\": \"select\", \"options\": [\"fast\", \"slow\"], \"default\": \"fast\" },"
    " { \"name\": \"on\", \"label\": \"On\", \"type\": \"bool\", \"default\": true },"
    " { \"name\": \"note\", \"label\": \"Note\", \"type\": \"text\", \"default\": \"hello\" },"
    " { \"name\": \"models\", \"label\": \"Models\", \"type\": \"files\", \"max_items\": 4,"
    "   \"item\": { \"by\": \"stem\", \"files\": [ { \"suffix\": \".bin\", \"max_kb\": 64, \"optional\": true } ] },"
    "   \"fields\": [ { \"name\": \"level\", \"label\": \"Level\", \"type\": \"int\", \"min\": 0, \"max\": 9 },"
    "               { \"name\": \"cutoff\", \"label\": \"Cutoff\", \"type\": \"number\", \"min\": 0.5, \"max\": 0.99, \"step\": 0.01 } ],"
    "   \"catalog\": { \"label\": \"Models\", \"items\": [ { \"id\": \"a\", \"title\": \"A\" } ] } } ] }";
static char rich[8192]; static size_t rich_len;
static int with(const char *from, const char *to)
{
    const char *at = strstr(RICH, from);
    err[0] = 0;
    if (!at) { snprintf(err, sizeof err, "the test's manifest has no %s", from); return -99; }
    rich_len = (size_t)snprintf(rich, sizeof rich, "%.*s%s%s", (int)(at - RICH), RICH, to, at + strlen(from));
    rich_len = with_requires(rich, sizeof rich);
    return ps_manifest_parse(rich, rich_len, &m, err, sizeof err);
}
/* taken with A in FROM's place, refused with B there, and the reason holds WHY */
static int edge(const char *from, const char *a, const char *b, const char *why)
{
    if (with(from, a)) return 0;
    return with(from, b) == -1 && strstr(err, why) != NULL;
}
/* refused with each of TO (a list ended by NULL) in FROM's place, the reason holding WHY: how many were not */
static int each(const char *from, const char *why, ...) __attribute__((sentinel));
#include <stdarg.h>
static int each(const char *from, const char *why, ...)
{
    va_list ap; const char *to; int wrong = 0;
    va_start(ap, why);
    while ((to = va_arg(ap, const char *))) if (with(from, to) != -1 || !strstr(err, why)) { printf("     (%s -> %s: %s)\n", from, to, err[0] ? err : "taken"); wrong++; }
    va_end(ap);
    return wrong;
}

/* a plain tar member */
static size_t member(uint8_t *o, const char *name, const void *data, size_t len)
{
    memset(o, 0, 512); snprintf((char *)o, 100, "%s", name); snprintf((char *)o + 124, 12, "%011lo", (unsigned long)len); o[156] = '0';
    memcpy(o + 512, data, len); memset(o + 512 + len, 0, (512 - len % 512) % 512);
    return 512 + (len + 511) / 512 * 512;
}

static const char *listing_serial = "0\n";      /* what a listing's text ends in */
static const char *listing_tier = "official";   /* and the tier it names */
static const uint8_t *listing_key = rsk;        /* and the index key that signs it */
static size_t package(uint8_t *o, const char *man, size_t ml, const uint8_t *prog, size_t pl, int listing, const uint8_t *signer)
{
    static uint8_t msg[40000]; uint8_t sig[64], dg[32]; char text[400], lst[600], dh[65]; size_t n = 0, hl = strlen("hassmic plugin manifest 1\n");
    memcpy(msg, "hassmic plugin manifest 1\n", hl); memcpy(msg + hl, man, ml);
    crypto_eddsa_sign(sig, signer, msg, hl + ml);
    n += member(o + n, "manifest.json", man, ml); n += member(o + n, "manifest.sig", sig, 64); n += member(o + n, "plugin", prog, pl);
    if (listing) {
        crypto_blake2b(dg, 32, (const uint8_t *)man, ml); hex(dh, dg, 32);
        snprintf(text, sizeof text, "hassmic plugin listing 2\nid probe\nversion %d\ntier %s\nmanifest %s\nserial %s", listing, listing_tier, dh, listing_serial);
        crypto_eddsa_sign(sig, listing_key, (const uint8_t *)text, strlen(text));
        hex(lst, sig, 64); snprintf(lst + 128, sizeof lst - 128, "\n%s", text);
        n += member(o + n, "listing.sig", lst, strlen(lst));
    }
    memset(o + n, 0, 1024);
    return n + 1024;
}

static struct ps_changes ch;                /* what the last check found an update to ask for beyond what was accepted */
static int staged(const uint8_t *pkg, size_t n, int *tier, int *button)
{
    err[0] = 0;
    if (ps_stage_begin(1, (long)n, err, sizeof err) || ps_stage_chunk(1, 0, pkg, n, err, sizeof err)) return -2;
    return ps_stage_check(&m, tier, button, &ch, err, sizeof err);
}

/* An update of the installed "probe": VERSION with these capabilities and settings, and (FROM: a text of the manifest
 * replaced by another of its length) a name or a label changed.  1 the button is needed, 0 not, -1 refused */
static const uint8_t *u_prog; static size_t u_pl; static int u_listed = 1;     /* u_listed 0: no listing (a file) */
static int update(int version, const char *caps, const char *more, const char *from, const char *to)
{
    static char man[8192]; static uint8_t pkg[5 << 20]; uint8_t dg[32]; size_t ml, n; char *v; int tier = -1, button = -1;
    crypto_blake2b(dg, 32, u_prog, u_pl);
    ml = manifest(man, sizeof man, caps, more, dg);
    v = strstr(man, "\"version\": 3"); v[11] = (char)('0' + version);
    if (from && (v = strstr(man, from))) memcpy(v, to, strlen(to));
    n = package(pkg, man, ml, u_prog, u_pl, u_listed ? version : 0, sk);      /* (listed as the installed one is: a lower tier is wider by itself) */
    return staged(pkg, n, &tier, &button) ? -1 : button;
}
/* the same requiring scope SCOPE at version VER (its "requires" as with_requires makes it, that one entry raised) */
static int req_pkg(int version, const char *scope, int ver, const char *caps, const char *more)
{
    static char man[8192]; static uint8_t pkg[5 << 20]; uint8_t dg[32]; size_t ml, n; char *v, k[40]; int tier = -1, button = -1;
    crypto_blake2b(dg, 32, u_prog, u_pl);
    ml = manifest(man, sizeof man, caps, more, dg);
    v = strstr(man, "\"version\": 3"); v[11] = (char)('0' + version);
    snprintf(k, sizeof k, "\"%s\": 1", scope);
    if ((v = strstr(strstr(man, "\"requires\""), k))) v[strlen(k) - 1] = (char)('0' + ver);
    n = package(pkg, man, ml, u_prog, u_pl, version, sk);
    return staged(pkg, n, &tier, &button) ? -1 : button;
}
/* "probe" in VERSION (any number) with the index key's listing: 1 the button is needed, 0 not, -1 refused */
static int official(int version, const char *caps, const char *more)
{
    static char man[8192], man2[8300]; static uint8_t pkg[5 << 20]; uint8_t dg[32]; size_t ml; char *v; int tier = -1, button = -1;
    crypto_blake2b(dg, 32, u_prog, u_pl);
    ml = manifest(man, sizeof man, caps, more, dg); (void)ml;
    v = strstr(man, "\"version\": 3");
    snprintf(man2, sizeof man2, "%.*s\"version\": %d%s", (int)(v - man), man, version, v + 12);
    return staged(pkg, package(pkg, man2, strlen(man2), u_prog, u_pl, version, sk), &tier, &button) ? -1 : tier != PS_OFFICIAL ? -3 : button;
}
/* a file of the test's state folder as text: its length */
static size_t slurp(const char *rel, char *out, size_t cap)
{
    char p[400]; FILE *f; size_t n;
    snprintf(p, sizeof p, "%s/%s", getenv("HASSMIC_STATE"), rel);
    out[0] = 0;
    if (!(f = fopen(p, "rb"))) return 0;
    n = fread(out, 1, cap - 1, f); out[n] = 0; fclose(f);
    return n;
}
/* the path REL in the test's state folder DIR (two at a time) */
static const char *path_of(const char *dir, const char *rel) { static char b[2][400]; static int k; k ^= 1; snprintf(b[k], sizeof b[k], "%s/%s", dir, rel); return b[k]; }
static const char *fs(const char *more) { static char b[2000]; snprintf(b, sizeof b, "%s%s", FILES_SETTING, more); return b; }     /* the file list and more settings */
/* the one line of ch, if it is of KIND and holds TEXT */
static int one(int kind, const char *text) { return ch.update && ch.n == 1 && ch.c[0].kind == kind && strstr(ch.c[0].line, text) != NULL; }

/* ---- an update held against what was accepted, pair by pair (a review, 2026-11-19: 31 pairs of an old and a new
 * manifest run through ps_stage_check, most of which were installed without the button).  pair(): "probe" removed,
 * the old one installed as version 1 with the button, the new one checked as version 2.  1 the button is asked,
 * 0 not, -1 the new one is refused, -3 the old one was */
static char *full(char *out, size_t cap, int version, const char *top, const char *caps, const char *settings)
{
    char k[65], d[65]; uint8_t dg[32];
    crypto_blake2b(dg, 32, u_prog, u_pl); hex(k, pk, 32); hex(d, dg, 32);
    snprintf(out, cap, "{ \"id\": \"probe\", \"version\": %d, \"requires\": { \"core\": 1 }, \"author_key\": \"%s\", \"digest\": \"%s\", %s, \"capabilities\": { %s }, \"settings\": [ %s ] }", version, k, d, top, caps, settings);
    with_requires(out, cap);
    return out;
}
static int check_text(const char *man)
{
    static uint8_t pkg[5 << 20]; int tier = -1, button = -1; size_t n = package(pkg, man, strlen(man), u_prog, u_pl, 0, sk);
    return staged(pkg, n, &tier, &button) ? -1 : button;
}
static int pair(const char *otop, const char *ocaps, const char *oset, const char *ntop, const char *ncaps, const char *nset)
{
    static char man[16384];
    ps_remove("probe");
    if (check_text(full(man, sizeof man, 1, otop, ocaps, oset)) != 1 || ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err)) return -3;
    return check_text(full(man, sizeof man, 2, ntop, ncaps, nset));
}
#define TOP "\"name\": \"Probe\", \"author\": \"someone\", \"memory_mb\": 16, \"threads\": 2"
#define HOST "{ \"name\": \"server\", \"label\": \"Server\", \"type\": \"host\" }"
#define FL(label, files, items) "{ \"name\": \"models\", \"label\": \"" label "\", \"type\": \"files\", \"item\": { \"by\": \"stem\", \"files\": [ " files " ] }, \"max_items\": " items " }"
#define BIN(kb) "{ \"suffix\": \".bin\", \"max_kb\": " kb " }"
enum { FREE = 0, ASKS = 1, NEVER_FREE = 2 };
/* phase 4's capabilities with parameters, as the Echo reads them (plugin_store.c entities_parse, actions_parse) */
#define ENT_A "{ \"name\": \"a\", \"kind\": \"sensor\", \"label\": \"A\" }"
#define ENT_B "{ \"name\": \"b\", \"kind\": \"switch\", \"label\": \"B\" }"
#define ACT_A "{ \"name\": \"say\", \"label\": \"Say\", \"args\": [ { \"name\": \"n\", \"type\": \"int\" } ] }"
#define ACT_B "{ \"name\": \"go\", \"label\": \"Go\", \"page\": true }"        /* NEVER_FREE: asked for, or refused as a manifest: not installed without the button */
struct pairc { const char *what; int want; const char *otop, *ocaps, *oset, *ntop, *ncaps, *nset; };
/* the reviewer's 31, in their order and words */
static const struct pairc review[] = {
 { "CONTROL identical caps, new description only (asked since the sixth read: the description is what the owner reads)", ASKS, TOP, "\"storage\": 4", HOST, TOP ", \"description\": \"x\"", "\"storage\": 4", HOST },
 { "CONTROL new capability", ASKS, TOP, "\"storage\": 4", HOST, TOP, "\"storage\": 4, \"net.internet\": [ { \"proto\": \"tcp\", \"ports\": [ 443 ] } ]", HOST },
 { "CONTROL wake dropped", ASKS, TOP, "\"wake\": true, \"files\": 1", FL("Models", BIN("64"), "4"), TOP, "\"files\": 1", FL("Models", BIN("64"), "4") },
 { "files list renamed", ASKS, TOP, "\"files\": 8", FL("Wake word models", BIN("64"), "4"), TOP, "\"files\": 8", FL("Home Assistant backups and SSH keys", BIN("64"), "4") },
 { "files list: other suffixes, 256x size, 16x items", ASKS, TOP, "\"files\": 8", FL("Models", BIN("64"), "4"), TOP, "\"files\": 8", FL("Models", "{ \"suffix\": \".key\", \"max_kb\": 16384 }, { \"suffix\": \".pem\", \"max_kb\": 16384 }", "64") },
 { "memory 16->96 MB, threads 2->16 (limits: shown, no button)", FREE, TOP, "\"storage\": 4", HOST, "\"name\": \"Probe\", \"author\": \"someone\", \"memory_mb\": 96, \"threads\": 16", "\"storage\": 4", HOST },
 /* (refused since a review of 08399dc: a row net.server names has no default; the owner's own server or none) */
 { "host setting gains a default", NEVER_FREE, TOP, "\"net.server\": [ { \"setting\": \"server\", \"proto\": \"udp\", \"ports\": [ 514 ] } ]", HOST, TOP, "\"net.server\": [ { \"setting\": \"server\", \"proto\": \"udp\", \"ports\": [ 514 ] } ]", "{ \"name\": \"server\", \"label\": \"Server\", \"type\": \"host\", \"default\": \"evil.example\" }" },
 /* (refused: net.server's entry names a row of type host) */
 { "host setting named by net.server retyped to select", NEVER_FREE, TOP, "\"net.server\": [ { \"setting\": \"server\", \"proto\": \"udp\", \"ports\": [ 514 ] } ]", HOST, TOP, "\"net.server\": [ { \"setting\": \"server\", \"proto\": \"udp\", \"ports\": [ 514 ] } ]", "{ \"name\": \"server\", \"label\": \"Server\", \"type\": \"select\", \"options\": [ \"evil.example\" ], \"default\": \"evil.example\" }" },
 { "text row's help rewritten", ASKS, TOP, "\"storage\": 4", "{ \"name\": \"note\", \"label\": \"Note\", \"type\": \"text\" }", TOP, "\"storage\": 4", "{ \"name\": \"note\", \"label\": \"Note\", \"type\": \"text\", \"help\": \"Enter your Home Assistant password here\" }" },
 { "new int row 'Alarm code'", ASKS, TOP, "\"storage\": 4", HOST, TOP, "\"storage\": 4", HOST ", { \"name\": \"pin\", \"label\": \"Your alarm system's code\", \"type\": \"int\", \"min\": 0, \"max\": 99999999 }" },
 { "int row renamed", ASKS, TOP, "\"storage\": 4", "{ \"name\": \"port\", \"label\": \"Port\", \"type\": \"int\" }", TOP, "\"storage\": 4", "{ \"name\": \"port\", \"label\": \"Your bank card's PIN\", \"type\": \"int\" }" },
 { "port setting named by net.server: range 514..514 -> 1..65535", ASKS, TOP, "\"net.server\": [ { \"setting\": \"server\", \"proto\": \"udp\", \"ports\": \"setting:port\" } ]", HOST ", { \"name\": \"port\", \"label\": \"Port\", \"type\": \"int\", \"min\": 514, \"max\": 514 }", TOP, "\"net.server\": [ { \"setting\": \"server\", \"proto\": \"udp\", \"ports\": \"setting:port\" } ]", HOST ", { \"name\": \"port\", \"label\": \"Port\", \"type\": \"int\", \"min\": 1, \"max\": 65535 }" },
 { "number: 2 -> 1.0000000000000000000000000000000000000e9", NEVER_FREE, TOP, "\"net.udp\": 2", HOST, TOP, "\"net.udp\": 1.0000000000000000000000000000000000000e9", HOST },
 { "number: 2 -> 0000000000000000000000000000000000000002000000", NEVER_FREE, TOP, "\"net.udp\": 2", HOST, TOP, "\"net.udp\": 0000000000000000000000000000000000000002000000", HOST },
 { "number: 443 -> 1-65535", NEVER_FREE, TOP, "\"net.lan\": [ { \"proto\": \"tcp\", \"ports\": [ 443 ] } ]", HOST, TOP, "\"net.lan\": [ { \"proto\": \"tcp\", \"ports\": [ 1-65535 ] } ]", HOST },
 { "number: 5353 -> 353 (a port, not a count)", ASKS, TOP, "\"net.discover\": { \"udp\": 5353, \"to\": \"broadcast\" }", HOST, TOP, "\"net.discover\": { \"udp\": 353, \"to\": \"broadcast\" }", HOST },
 { "number: 2 -> -1", NEVER_FREE, TOP, "\"net.udp\": 2", HOST, TOP, "\"net.udp\": -1", HOST },
 { "number: 2 -> 0", NEVER_FREE, TOP, "\"net.udp\": 2", HOST, TOP, "\"net.udp\": 0", HOST },
 { "list: one entry repeated, once with its keys in another order (an entity twice: refused)", NEVER_FREE, TOP, "\"ha.entities\": [ " ENT_A " ]", HOST, TOP, "\"ha.entities\": [ " ENT_A ", { \"label\": \"A\", \"kind\": \"sensor\", \"name\": \"a\" }, " ENT_A " ]", HOST },
 { "list of numbers [443] -> [80]", ASKS, TOP, "\"net.lan\": [ { \"proto\": \"tcp\", \"ports\": [ 443 ] } ]", HOST, TOP, "\"net.lan\": [ { \"proto\": \"tcp\", \"ports\": [ 80 ] } ]", HOST },
 { "list of numbers [443] -> [443.0] (a port is a whole number as written: refused)", NEVER_FREE, TOP, "\"net.lan\": [ { \"proto\": \"tcp\", \"ports\": [ 443 ] } ]", HOST, TOP, "\"net.lan\": [ { \"proto\": \"tcp\", \"ports\": [ 443.0 ] } ]", HOST },
 { "nested object dup key", NEVER_FREE, TOP, "\"net.lan\": [ { \"proto\": \"tcp\", \"ports\": [ 443 ] } ]", HOST, TOP, "\"net.lan\": [ { \"proto\": \"tcp\", \"ports\": [ 443 ], \"ports\": [ 22 ] } ]", HOST },
 { "nested key escaped", NEVER_FREE, TOP, "\"net.lan\": [ { \"proto\": \"tcp\", \"ports\": [ 443 ] } ]", HOST, TOP, "\"net.lan\": [ { \"proto\": \"tcp\", \"p\\u006frts\": [ 443 ] } ]", HOST },
 { "cap key escaped", NEVER_FREE, TOP, "\"net.udp\": 2", HOST, TOP, "\"net.ud\\u0070\": 9", HOST },
 { "list -> true", NEVER_FREE, TOP, "\"ha.entities\": [ " ENT_A " ]", HOST, TOP, "\"ha.entities\": true", HOST },
 { "list -> empty object", NEVER_FREE, TOP, "\"ha.entities\": [ " ENT_A " ]", HOST, TOP, "\"ha.entities\": {}", HOST },
 { "second capabilities object at top level", NEVER_FREE, TOP, "\"storage\": 4", HOST, TOP ", \"capabilities\": { \"storage\": 4 }, \"x\": 1", "\"net.internet\": [ { \"proto\": \"tcp\", \"ports\": [ 443 ] } ]", HOST },
 { "name same text by escape (the same name: nothing to ask)", FREE, TOP, "\"storage\": 4", HOST, "\"name\": \"Pr\\u006fbe\", \"author\": \"someone\"", "\"storage\": 4", HOST },
 { "secret retyped to text (shown in clear)", ASKS, TOP, "\"storage\": 4", "{ \"name\": \"pw\", \"label\": \"Password\", \"type\": \"secret\" }", TOP, "\"storage\": 4", "{ \"name\": \"pw\", \"label\": \"Password\", \"type\": \"text\" }" },
 { "secret retyped to select", ASKS, TOP, "\"storage\": 4", "{ \"name\": \"pw\", \"label\": \"Password\", \"type\": \"secret\" }", TOP, "\"storage\": 4", "{ \"name\": \"pw\", \"label\": \"Password\", \"type\": \"select\", \"options\": [ \"a\" ] }" },
 { "storage 4 -> 1.0000000000000000000000000000000000000e3", NEVER_FREE, TOP, "\"storage\": 4", HOST, TOP, "\"storage\": 1.0000000000000000000000000000000000000e3", HOST },
};
/* and what the rule is made of: each thing that is less, without the button; each thing near it that is not */
#define LAN2 "\"net.lan\": [ { \"proto\": \"tcp\", \"ports\": [ 514, 80 ] }, { \"proto\": \"udp\", \"ports\": \"any\" } ]"
#define PORT "{ \"name\": \"port\", \"label\": \"Port\", \"type\": \"int\", \"min\": 1, \"max\": 65535, \"unit\": \"tcp\", \"help\": \"Where it listens\" }"
#define MODE "{ \"name\": \"mode\", \"label\": \"Mode\", \"type\": \"select\", \"options\": [ \"fast\", \"slow\" ], \"default\": \"fast\" }"
#define FLX(kb, opt, items, fields, cat) "{ \"name\": \"models\", \"label\": \"Models\", \"type\": \"files\", \"item\": { \"by\": \"stem\", \"files\": [ { \"suffix\": \".bin\", \"max_kb\": " kb ", \"as\": \"m.bin\" }, { \"suffix\": \".json\", \"max_kb\": 8, \"optional\": " opt " } ] }" items \
    ", \"fields\": [ " fields " ], \"catalog\": { \"base\": \"" cat "\", \"items\": [ { \"id\": \"a\" } ] } }"
#define LEVEL "{ \"name\": \"level\", \"label\": \"Level\", \"type\": \"int\", \"min\": 0, \"max\": 9 }"
#define FL0 FLX("64", "true", ", \"max_items\": 4", LEVEL, "https://example.org/models/")
static const struct pairc rule[] = {
 /* less: no button */
 { "a smaller storage number", FREE, TOP, "\"storage\": 8", HOST, TOP, "\"storage\": 4", HOST },
 { "a smaller files number", FREE, TOP, "\"files\": 8", FL0, TOP, "\"files\": 1", FL0 },
 { "a file's max_kb smaller", FREE, TOP, "\"files\": 8", FL0, TOP, "\"files\": 8", FLX("32", "true", ", \"max_items\": 4", LEVEL, "https://example.org/models/") },
 { "max_items smaller", FREE, TOP, "\"files\": 8", FL0, TOP, "\"files\": 8", FLX("64", "true", ", \"max_items\": 1", LEVEL, "https://example.org/models/") },
 { "max_items left out (16) made 8", FREE, TOP, "\"files\": 8", FLX("64", "true", "", LEVEL, "u"), TOP, "\"files\": 8", FLX("64", "true", ", \"max_items\": 8", LEVEL, "u") },
 { "max_items 16 left out: the same 16", FREE, TOP, "\"files\": 8", FLX("64", "true", ", \"max_items\": 16", LEVEL, "u"), TOP ", \"version_name\": \"x\", \"license\": \"MIT\"", "\"files\": 8", FLX("64", "true", "", LEVEL, "u") },
 { "a capability gone", FREE, TOP, "\"storage\": 4, \"net.internet\": [ { \"proto\": \"tcp\", \"ports\": [ 443 ] } ], \"state\": true", HOST, TOP, "\"storage\": 4", HOST },
 { "a row gone", FREE, TOP, "\"storage\": 4", HOST ", " PORT ", " MODE, TOP, "\"storage\": 4", HOST ", " MODE },
 { "every row gone, and the list with them", FREE, TOP, "\"storage\": 4", HOST ", " PORT, TOP, "\"storage\": 4", "" },
 { "the keys of an entry and of a row in another order, the capabilities too", FREE, TOP, "\"storage\": 4, " LAN2, PORT, TOP, "\"net.lan\": [ { \"ports\": [ 514, 80 ], \"proto\": \"tcp\" }, { \"ports\": \"any\", \"proto\": \"udp\" } ], \"storage\": 4",
   "{ \"help\": \"Where it listens\", \"type\": \"int\", \"max\": 65535, \"unit\": \"tcp\", \"label\": \"Port\", \"min\": 1, \"name\": \"port\" }" },
 { "a label written with an escape for one of its letters: the same label", FREE, TOP, "\"storage\": 4", HOST, TOP, "\"storage\": 4", "{ \"name\": \"server\", \"label\": \"Serv\\u0065r\", \"type\": \"host\" }" },
 { "all of it at once: quota, max_kb, max_items, a row, a capability", FREE, TOP, "\"files\": 8, \"state\": true, " LAN2, FL0 ", " PORT, TOP, "\"files\": 2, " LAN2, FLX("1", "true", ", \"max_items\": 2", LEVEL, "https://example.org/models/") },
 { "the version's label and the licence's name, changed, added, gone: no part of what was accepted", FREE, TOP ", \"version_name\": \"1.0\", \"license\": \"MIT\"", "\"storage\": 4", HOST, TOP ", \"version_name\": \"2.0 beta\"", "\"storage\": 4", HOST },
 /* near it, and not less */
 /* a list is no list of grants until the capability's own code says what its entries mean (the sixth read) */
 { "an entry gone from a list (net.lan)", ASKS, TOP, LAN2, HOST, TOP, "\"net.lan\": [ { \"proto\": \"udp\", \"ports\": \"any\" } ]", HOST },
 { "every entry gone: [80, 443] made [] (refused: an entry at least)", NEVER_FREE, TOP, "\"net.lan\": [ { \"proto\": \"tcp\", \"ports\": [ 80, 443 ] } ]", HOST, TOP, "\"net.lan\": []", HOST },
 { "an entry of a form the Echo does not read, added (\"not\")", NEVER_FREE, TOP, "\"net.lan\": [ { \"proto\": \"tcp\", \"ports\": \"any\" } ]", HOST, TOP, "\"net.lan\": [ { \"proto\": \"tcp\", \"ports\": \"any\" }, { \"not\": \"192.168.1.1\" } ]", HOST },
 { "an entity more, the same keys in another order", ASKS, TOP, "\"ha.entities\": [ " ENT_A " ]", HOST, TOP, "\"ha.entities\": [ { \"label\": \"A\", \"kind\": \"sensor\", \"name\": \"a\" }, " ENT_B " ]", HOST },
 { "an entity gone", ASKS, TOP, "\"ha.entities\": [ " ENT_A ", " ENT_B " ]", HOST, TOP, "\"ha.entities\": [ " ENT_A " ]", HOST },
 { "an action gone", ASKS, TOP, "\"ha.actions\": [ " ACT_A ", " ACT_B " ]", HOST, TOP, "\"ha.actions\": [ " ACT_A " ]", HOST },
 { "an action gets its button on the card", ASKS, TOP, "\"ha.actions\": [ { \"name\": \"go\", \"label\": \"Go\" } ]", HOST, TOP, "\"ha.actions\": [ " ACT_B " ]", HOST },
 { "an action marked \"calls\" (it brings a call token)", ASKS, TOP, "\"call\": true, \"ha.actions\": [ " ACT_B " ]", HOST, TOP, "\"call\": true, \"ha.actions\": [ { \"name\": \"go\", \"label\": \"Go\", \"page\": true, \"calls\": true } ]", HOST },
 { "the \"calls\" mark dropped (no list is one of grants: an action changed asks)", ASKS, TOP, "\"call\": true, \"ha.actions\": [ { \"name\": \"go\", \"label\": \"Go\", \"calls\": true } ]", HOST, TOP, "\"call\": true, \"ha.actions\": [ { \"name\": \"go\", \"label\": \"Go\" } ]", HOST },
 { "call added", ASKS, TOP, "\"storage\": 4", HOST, TOP, "\"storage\": 4, \"call\": true", HOST },
 /* request.audio and request.text added, and a way out added beside request.audio (which also wants the
  * second press: ps_stage_install, below) */
 { "request.audio added", ASKS, TOP, "\"storage\": 4", HOST, TOP, "\"storage\": 4, \"request.audio\": true", HOST },
 { "request.text added", ASKS, TOP, "\"storage\": 4", HOST, TOP, "\"storage\": 4, \"request.text\": true", HOST },
 { "net.udp added beside request.audio", ASKS, TOP, "\"request.audio\": true", HOST, TOP, "\"request.audio\": true, \"net.udp\": 1", HOST },
 { "request.audio gone", FREE, TOP, "\"request.audio\": true, \"request.text\": true", HOST, TOP, "\"request.text\": true", HOST },
 /* button (a capability like any: added asks, gone is less), and an entity's "page" (a row on the card,
  * no power: its own rule, plugin_store.c x_skip, X_ENT) */
 { "button added", ASKS, TOP, "\"storage\": 4", HOST, TOP, "\"storage\": 4, \"button\": true", HOST },
 { "button gone", FREE, TOP, "\"storage\": 4, \"button\": true", HOST, TOP, "\"storage\": 4", HOST },
 { "an entity shown on the card (\"page\" added)", FREE, TOP, "\"ha.entities\": [ " ENT_A ", " ENT_B " ]", HOST, TOP, "\"ha.entities\": [ { \"name\": \"a\", \"kind\": \"sensor\", \"label\": \"A\", \"page\": true }, " ENT_B " ]", HOST },
 { "... and no longer (\"page\": false), the other's kept", FREE, TOP, "\"ha.entities\": [ { \"name\": \"a\", \"kind\": \"sensor\", \"label\": \"A\", \"page\": true }, { \"name\": \"b\", \"kind\": \"switch\", \"label\": \"B\", \"page\": true } ]", HOST,
   TOP, "\"ha.entities\": [ { \"name\": \"a\", \"kind\": \"sensor\", \"label\": \"A\", \"page\": false }, { \"page\": true, \"name\": \"b\", \"kind\": \"switch\", \"label\": \"B\" } ]", HOST },
 { "\"page\" added and the label changed with it: the label asks", ASKS, TOP, "\"ha.entities\": [ " ENT_A " ]", HOST, TOP, "\"ha.entities\": [ { \"name\": \"a\", \"kind\": \"sensor\", \"label\": \"Your alarm code\", \"page\": true } ]", HOST },
 { "\"page\" added and an entity more: the entity asks", ASKS, TOP, "\"ha.entities\": [ " ENT_A " ]", HOST, TOP, "\"ha.entities\": [ { \"name\": \"a\", \"kind\": \"sensor\", \"label\": \"A\", \"page\": true }, " ENT_B " ]", HOST },
 { "\"page\" on an action (its button on the card) is no entity's: it asks as before", ASKS, TOP, "\"ha.actions\": [ { \"name\": \"go\", \"label\": \"Go\" } ]", HOST, TOP, "\"ha.actions\": [ { \"name\": \"go\", \"label\": \"Go\", \"page\": false } ]", HOST },
 { "an entity's label", ASKS, TOP, "\"ha.entities\": [ " ENT_A " ]", HOST, TOP, "\"ha.entities\": [ { \"name\": \"a\", \"kind\": \"sensor\", \"label\": \"Your alarm code\" } ]", HOST },
 { "an entry gone from net.server", ASKS, TOP, "\"net.server\": [ { \"setting\": \"server\", \"proto\": \"udp\", \"ports\": [ 514 ] }, { \"setting\": \"server\", \"proto\": \"tcp\", \"ports\": [ 80 ] } ]", HOST, TOP, "\"net.server\": [ { \"setting\": \"server\", \"proto\": \"udp\", \"ports\": [ 514 ] } ]", HOST },
 /* a row that a kept capability names cannot go: the manifest that takes it out is none */
 { "a row gone that net.server still names", NEVER_FREE, TOP, "\"net.server\": [ { \"setting\": \"server\", \"proto\": \"udp\", \"ports\": \"setting:port\" } ]", HOST ", " PORT, TOP, "\"net.server\": [ { \"setting\": \"server\", \"proto\": \"udp\", \"ports\": \"setting:port\" } ]", HOST },
 { "... and with the capability gone too: less", FREE, TOP, "\"storage\": 4, \"net.server\": [ { \"setting\": \"server\", \"proto\": \"udp\", \"ports\": \"setting:port\" } ]", HOST ", " PORT, TOP, "\"storage\": 4", HOST },
 /* what the owner reads of the plugin itself */
 { "a description where there was none", ASKS, TOP, "\"storage\": 4", HOST, TOP ", \"description\": \"Type your Home Assistant password into Port\"", "\"storage\": 4", HOST },
 { "another description", ASKS, TOP ", \"description\": \"Sends your logs to your syslog server.\"", "\"storage\": 4", HOST, TOP ", \"description\": \"Type your Home Assistant password into Port\"", "\"storage\": 4", HOST },
 { "the description gone", ASKS, TOP ", \"description\": \"Sends your logs.\"", "\"storage\": 4", HOST, TOP, "\"storage\": 4", HOST },
 { "another source address", ASKS, TOP ", \"source\": \"https://github.com/someone/probe\"", "\"storage\": 4", HOST, TOP ", \"source\": \"https://evil.example/login\"", "\"storage\": 4", HOST },
 { "a source address where there was none", ASKS, TOP, "\"storage\": 4", HOST, TOP ", \"source\": \"https://evil.example/login\"", "\"storage\": 4", HOST },
 { "max_items in a row that is no file list (a key this version reads nothing of there)", ASKS, TOP, "\"storage\": 4", "{ \"name\": \"port\", \"label\": \"Port\", \"type\": \"int\", \"max_items\": 4 }", TOP, "\"storage\": 4", "{ \"name\": \"port\", \"label\": \"Port\", \"type\": \"int\", \"max_items\": 2 }" },
 { "... and added to one", ASKS, TOP, "\"storage\": 4", HOST, TOP, "\"storage\": 4", "{ \"name\": \"server\", \"label\": \"Server\", \"type\": \"host\", \"max_items\": 1 }" },
 { "a larger storage number", ASKS, TOP, "\"storage\": 4", HOST, TOP, "\"storage\": 5", HOST },
 { "a file's max_kb larger", ASKS, TOP, "\"files\": 8", FL0, TOP, "\"files\": 8", FLX("65", "true", ", \"max_items\": 4", LEVEL, "https://example.org/models/") },
 { "max_items larger", ASKS, TOP, "\"files\": 8", FL0, TOP, "\"files\": 8", FLX("64", "true", ", \"max_items\": 5", LEVEL, "https://example.org/models/") },
 { "max_items 4 left out: 16", ASKS, TOP, "\"files\": 8", FL0, TOP, "\"files\": 8", FLX("64", "true", "", LEVEL, "https://example.org/models/") },
 { "net.udp's count made smaller (its own rule: fewer ports of its own)", FREE, TOP, "\"net.udp\": 2", HOST, TOP, "\"net.udp\": 1", HOST },
 { "net.udp's count larger", ASKS, TOP, "\"net.udp\": 2", HOST, TOP, "\"net.udp\": 3", HOST },
 { "audio.volume added to audio.music", ASKS, TOP, "\"audio.music\": true", HOST, TOP, "\"audio.music\": true, \"audio.volume\": true", HOST },
 { "a list's entries in another order", ASKS, TOP, LAN2, HOST, TOP, "\"net.lan\": [ { \"proto\": \"udp\", \"ports\": \"any\" }, { \"proto\": \"tcp\", \"ports\": [ 514, 80 ] } ]", HOST },
 { "a list inside an entry made shorter", ASKS, TOP, LAN2, HOST, TOP, "\"net.lan\": [ { \"proto\": \"tcp\", \"ports\": [ 514 ] }, { \"proto\": \"udp\", \"ports\": \"any\" } ]", HOST },
 { "a key gone from an entry (proto: refused)", NEVER_FREE, TOP, LAN2, HOST, TOP, "\"net.lan\": [ { \"proto\": \"tcp\", \"ports\": [ 514, 80 ] }, { \"ports\": \"any\" } ]", HOST },
 { "a key more in an entry (one the Echo does not read: refused)", NEVER_FREE, TOP, LAN2, HOST, TOP, "\"net.lan\": [ { \"proto\": \"tcp\", \"ports\": [ 514, 80 ], \"tls\": true }, { \"proto\": \"udp\", \"ports\": \"any\" } ]", HOST },
 { "net.discover's port changed", ASKS, TOP, "\"net.discover\": { \"udp\": 5353, \"to\": \"broadcast\" }", HOST, TOP, "\"net.discover\": { \"udp\": 1900, \"to\": \"broadcast\" }", HOST },
 { "net.discover's group made the broadcast", ASKS, TOP, "\"net.discover\": { \"udp\": 5353, \"to\": \"224.0.0.251\" }", HOST, TOP, "\"net.discover\": { \"udp\": 5353, \"to\": \"broadcast\" }", HOST },
 { "the catalog's address", ASKS, TOP, "\"files\": 8", FL0, TOP, "\"files\": 8", FLX("64", "true", ", \"max_items\": 4", LEVEL, "https://evil.example/models/") },
 { "a file no longer optional", ASKS, TOP, "\"files\": 8", FL0, TOP, "\"files\": 8", FLX("64", "false", ", \"max_items\": 4", LEVEL, "https://example.org/models/") },
 { "a field's range", ASKS, TOP, "\"files\": 8", FL0, TOP, "\"files\": 8", FLX("64", "true", ", \"max_items\": 4", "{ \"name\": \"level\", \"label\": \"Level\", \"type\": \"int\", \"min\": 0, \"max\": 99 }", "https://example.org/models/") },
 { "a field's label", ASKS, TOP, "\"files\": 8", FL0, TOP, "\"files\": 8", FLX("64", "true", ", \"max_items\": 4", "{ \"name\": \"level\", \"label\": \"Your PIN\", \"type\": \"int\", \"min\": 0, \"max\": 9 }", "https://example.org/models/") },
 { "a field gone", ASKS, TOP, "\"files\": 8", FL0, TOP, "\"files\": 8", FLX("64", "true", ", \"max_items\": 4", "", "https://example.org/models/") },
 { "a number's unit", ASKS, TOP, "\"storage\": 4", PORT, TOP, "\"storage\": 4", "{ \"name\": \"port\", \"label\": \"Port\", \"type\": \"int\", \"min\": 1, \"max\": 65535, \"default\": 514, \"unit\": \"digits\", \"help\": \"Where it listens\" }" },
 { "a number's default", ASKS, TOP, "\"storage\": 4", PORT, TOP, "\"storage\": 4", "{ \"name\": \"port\", \"label\": \"Port\", \"type\": \"int\", \"min\": 1, \"max\": 65535, \"default\": 22, \"unit\": \"tcp\", \"help\": \"Where it listens\" }" },
 { "a number's range made smaller (no rule names it)", ASKS, TOP, "\"storage\": 4", PORT, TOP, "\"storage\": 4", "{ \"name\": \"port\", \"label\": \"Port\", \"type\": \"int\", \"min\": 1, \"max\": 1024, \"default\": 514, \"unit\": \"tcp\", \"help\": \"Where it listens\" }" },
 { "a number's help gone", ASKS, TOP, "\"storage\": 4", PORT, TOP, "\"storage\": 4", "{ \"name\": \"port\", \"label\": \"Port\", \"type\": \"int\", \"min\": 1, \"max\": 65535, \"default\": 514, \"unit\": \"tcp\" }" },
 { "a choice more", ASKS, TOP, "\"storage\": 4", MODE, TOP, "\"storage\": 4", "{ \"name\": \"mode\", \"label\": \"Mode\", \"type\": \"select\", \"options\": [ \"fast\", \"slow\", \"send it all to me\" ], \"default\": \"fast\" }" },
 { "a choice gone", ASKS, TOP, "\"storage\": 4", MODE, TOP, "\"storage\": 4", "{ \"name\": \"mode\", \"label\": \"Mode\", \"type\": \"select\", \"options\": [ \"fast\" ], \"default\": \"fast\" }" },
 { "a key the Echo does not read, in a row", ASKS, TOP, "\"storage\": 4", HOST, TOP, "\"storage\": 4", "{ \"name\": \"server\", \"label\": \"Server\", \"type\": \"host\", \"sync\": true }" },
 { "a switch more", ASKS, TOP, "\"storage\": 4", HOST, TOP, "\"storage\": 4", HOST ", { \"name\": \"quiet\", \"label\": \"Quiet\", \"type\": \"bool\" }" },
 { "a choice more among the rows", ASKS, TOP, "\"storage\": 4", HOST, TOP, "\"storage\": 4", HOST ", " MODE },
 { "the rows in another order", ASKS, TOP, "\"storage\": 4", HOST ", " PORT, TOP, "\"storage\": 4", PORT ", " HOST },
 { "a row under another name", ASKS, TOP, "\"storage\": 4", HOST, TOP, "\"storage\": 4", "{ \"name\": \"host\", \"label\": \"Server\", \"type\": \"host\" }" },
};
static int pairs(const struct pairc *c, size_t n)      /* how many went another way than they should */
{
    int wrong = 0;
    for (size_t i = 0; i < n; i++) {
        int r = pair(c[i].otop, c[i].ocaps, c[i].oset, c[i].ntop, c[i].ncaps, c[i].nset);
        int ok = c[i].want == FREE ? r == 0 : c[i].want == ASKS ? r == 1 && ch.update && ch.ask : r == 1 || r == -1;
        if (!ok) { wrong++; printf("     (%s: %d, not %s: %s)\n", c[i].what, r, c[i].want == FREE ? "without the button" : "the button", r < 0 ? err : ch.n ? ch.c[0].line : ""); }
    }
    return wrong;
}

/* ---- the same at random: one value of a manifest's capabilities or settings changed, and what is installed without
 * the button is only what the rule names.  A site is a value with where it stands */
struct site { int tok, parent, key; char path[160]; };
static struct site sites[600]; static int nsites;
static void walk(const struct json *j, int t, int parent, int key, const char *path)
{
    int type = json_type(j, t), i = 0;
    if (nsites == 600) return;
    sites[nsites] = (struct site){ t, parent, key, "" }; snprintf(sites[nsites++].path, sizeof sites[0].path, "%s", path);
    if (type == J_ARR) JSON_EACH(j, t, e) { char p[160]; snprintf(p, sizeof p, "%.140s/%d", path, i++); walk(j, e, t, -1, p); }
    if (type == J_OBJ)
        for (int k = json_first(j, t); k >= 0; ) {
            int v = j->t[k].next; char p[160], name[60];
            if (v >= j->t[t].next) break;
            json_str(j, k, name, sizeof name); snprintf(p, sizeof p, "%.100s/%s", path, name); walk(j, v, t, k, p);
            k = json_next(j, t, v);
        }
}
/* a value's text with its quotes */
static int vs(const struct json *j, int t) { return j->t[t].start - (j->t[t].type == J_STR); }
static int ve(const struct json *j, int t) { return j->t[t].end + (j->t[t].type == J_STR); }
/* TEXT with [a, b) replaced by WITH */
static void splice(char *out, size_t cap, const char *text, int a, int b, const char *with) { snprintf(out, cap, "%.*s%s%s", a, text, with, text + b); }
/* what a path is, for the rule */
static int path_is(const char *path, const char *pat)        /* pat: parts, "*" any one, "#" a number */
{
    for (;;) {
        size_t pl = strcspn(path, "/"), ql = strcspn(pat, "/");
        if (ql == 1 && *pat == '#') { for (size_t i = 0; i < pl; i++) if (path[i] < '0' || path[i] > '9') return 0; if (!pl) return 0; }
        else if (!(ql == 1 && *pat == '*') && (pl != ql || memcmp(path, pat, pl))) return 0;
        path += pl; pat += ql;
        if (!*path || !*pat) return !*path && !*pat;
        path++; pat++;
    }
}
/* what of the manifest's own texts is accepted with it (asked when it changes) and what is free to change */
static int top_asks(const char *path) { return path_is(path, "description") || path_is(path, "source"); }
static int top_free(const char *path) { return path_is(path, "version_name") || path_is(path, "license"); }
static int quantity(const char *path)
{
    return path_is(path, "capabilities/storage") || path_is(path, "capabilities/files") || path_is(path, "capabilities/net.udp") || path_is(path, "settings/#/max_items") || path_is(path, "settings/#/item/files/#/max_kb");
}
enum { M_UP, M_DOWN, M_TEXT, M_KIND, M_REMOVE, M_KEY, M_ELEMENT, M_SWAP, M_RENAME, M_N };
static const char *const m_names[] = { "a number up", "a number down", "another text", "another kind of value", "taken out", "a key more", "an element more", "two elements swapped", "a key renamed" };
/* One change of BASE (parsed as j) at SITE, into out.  -1 nothing to do there; else what the rule says of it:
 * FREE or ASKS.  (A manifest the Echo then refuses is neither: the caller sees that) */
static int mutate(const struct json *j, const char *base, const struct site *s, int op, char *out, size_t cap)
{
    int t = s->tok, type = json_type(j, t), a = vs(j, t), b = ve(j, t); char v[4000]; const char *path = s->path;
    switch (op) {
    case M_UP: case M_DOWN: {
        long n; char *end;
        if (type != J_NUM) return -1;
        n = strtol(base + a, &end, 10);
        if (end != base + b) return -1;                          /* (0.5, in the catalog: left to M_KIND) */
        snprintf(v, sizeof v, "%ld", op == M_UP ? n + 1 + rand() % 3 : n - 1 - rand() % 2);
        splice(out, cap, base, a, b, v);
        return op == M_DOWN && quantity(path) ? FREE : ASKS; }
    case M_TEXT:
        if (type != J_STR) return -1;
        if (path_is(path, "settings/#/type")) { static const char *const ty[] = { "bool", "int", "select", "text", "host", "secret", "files" }; const char *n = ty[rand() % 7]; if (!strncmp(base + a + 1, n, strlen(n))) return -1; snprintf(v, sizeof v, "\"%s\"", n); }
        else if (strstr(path, "suffix")) snprintf(v, sizeof v, "\".x%d\"", rand() % 10);
        else if (rand() % 2) snprintf(v, sizeof v, "\"%.*sx\"", b - a - 2, base + a + 1);
        else snprintf(v, sizeof v, "\"q%d\"", rand() % 100);
        splice(out, cap, base, a, b, v);
        return top_free(path) ? FREE : ASKS;
    case M_KIND: {
        static const char *const lit[] = { "true", "false", "7", "\"x\"", "[]", "{}", "null", "0.5", "[ 1 ]", "{ \"a\": 1 }" }; const char *n = lit[rand() % 10];
        if ((size_t)(b - a) == strlen(n) && !memcmp(base + a, n, strlen(n))) return -1;
        splice(out, cap, base, a, b, n);
        /* (one of them is less of what was there: a quantity made 7.  A list made empty is not: no list is one of
         * grants yet) */
        if (type == J_NUM && quantity(path) && !strcmp(n, "7")) return atol(base + a) > 7 ? FREE : ASKS;
        return top_free(path) ? FREE : ASKS; }
    case M_REMOVE: case M_SWAP: {
        /* the members of its container: [ms, me) each, a key with its value in an object */
        int ms[80], me[80], n = 0, at = -1, par = s->parent;
        if (par < 0) return -1;
        if (json_type(j, par) == J_ARR) JSON_EACH(j, par, e) { if (n == 80) return -1; if (e == t) at = n; ms[n] = vs(j, e); me[n++] = ve(j, e); }
        else for (int k = json_first(j, par); k >= 0; ) { int w = j->t[k].next; if (w >= j->t[par].next || n == 80) break; if (w == t) at = n; ms[n] = vs(j, k); me[n++] = ve(j, w); k = json_next(j, par, w); }
        if (at < 0) return -1;
        if (op == M_SWAP) {
            if (json_type(j, par) != J_ARR || at + 1 >= n) return -1;
            snprintf(v, sizeof v, "%.*s, %.*s", me[at + 1] - ms[at + 1], base + ms[at + 1], me[at] - ms[at], base + ms[at]);
            if (me[at + 1] - ms[at + 1] == me[at] - ms[at] && !memcmp(base + ms[at], base + ms[at + 1], (size_t)(me[at] - ms[at]))) return -1;   /* two that are the same */
            splice(out, cap, base, ms[at], me[at + 1], v);
            return ASKS;
        }
        if (at + 1 < n) splice(out, cap, base, ms[at], ms[at + 1], ""); else if (at) splice(out, cap, base, me[at - 1], me[at], ""); else splice(out, cap, base, ms[at], me[at], "");
        if (path_is(path, "capabilities/wake")) return ASKS;        /* a wake plugin that stops being one */
        return path_is(path, "capabilities/*") || path_is(path, "settings/#") || top_free(path) ? FREE : ASKS; }
    case M_KEY:
        if (type != J_OBJ) return -1;
        snprintf(v, sizeof v, "{ \"zz%d\": %s%s", rand() % 10, rand() % 2 ? "1" : "\"x\"", json_first(j, t) >= 0 ? ", " : " ");
        splice(out, cap, base, a, a + 1, v);
        return ASKS;
    case M_ELEMENT: {
        int f = json_first(j, t), l = f;
        if (type != J_ARR) return -1;
        JSON_EACH(j, t, e) l = e;
        /* at its end: one of its own again, or something else */
        if (f >= 0 && rand() % 2) snprintf(v, sizeof v, ", %.*s ]", ve(j, f) - vs(j, f) > 300 ? 0 : ve(j, f) - vs(j, f), base + vs(j, f));
        else snprintf(v, sizeof v, "%s%s ]", f >= 0 ? ", " : "", rand() % 2 ? "9" : "\"zz\"");
        if (v[0] == ',' && v[2] == ' ' && v[3] == ']') return -1;
        splice(out, cap, base, f >= 0 ? ve(j, l) : a + 1, b, v);
        return ASKS; }
    case M_RENAME:
        if (s->key < 0) return -1;
        snprintf(v, sizeof v, "\"%.*sx\"", j->t[s->key].end - j->t[s->key].start, base + j->t[s->key].start);
        splice(out, cap, base, vs(j, s->key), ve(j, s->key), v);
        return top_free(path) ? FREE : ASKS;                    /* (under another key it is gone, as M_REMOVE's) */
    }
    return -1;
}
/* ROUNDS changes of the manifest made of TOP, CAPS and SETTINGS, installed as version 1.  *judged: how many the Echo
 * took as manifests; *free_: how many of those without the button.  How many went another way than the rule says.
 * TEXTS: only the manifest's own four texts are changed (description, source: accepted; version_name, license: not) */
static int at_random(const char *top, const char *caps, const char *settings, int rounds, int texts, int *judged, int *free_)
{
    static char base[16384], man[17000]; struct json j = { 0 }; int wrong = 0;
    ps_remove("probe");
    if (check_text(full(base, sizeof base, 1, top, caps, settings)) != 1 || ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err)) { printf("     (the manifest to change is not taken: %s)\n", err); return 1; }
    full(base, sizeof base, 2, top, caps, settings);
    if (check_text(base) != 0 || ch.n) { printf("     (unchanged, it asks: %s)\n", ch.n ? ch.c[0].line : err); return 1; }
    if (json_parse(&j, base, strlen(base))) return 1;
    nsites = 0;
    /* the manifest's own texts that the page shows */
    for (int k = json_first(&j, 0); k >= 0; ) {
        int v = j.t[k].next; char name[60];
        if (v >= j.t[0].next) break;
        json_str(&j, k, name, sizeof name);
        if (top_asks(name) || top_free(name)) walk(&j, v, 0, k, name);
        k = json_next(&j, 0, v);
    }
    /* the two sections themselves are no sites: without "capabilities" there is no manifest */
    for (int pass = 0; pass < 2 && !texts; pass++) {
        const char *sec = pass ? "settings" : "capabilities"; int t = json_get(&j, 0, sec), i = 0;
        if (json_type(&j, t) == J_ARR) JSON_EACH(&j, t, e) { char p[40]; snprintf(p, sizeof p, "%s/%d", sec, i++); walk(&j, e, t, -1, p); }
        else for (int k = json_first(&j, t); k >= 0; ) { int v = j.t[k].next; char p[160], name[60]; if (v >= j.t[t].next) break; json_str(&j, k, name, sizeof name); snprintf(p, sizeof p, "%s/%s", sec, name); walk(&j, v, t, k, p); k = json_next(&j, t, v); }
    }
    for (int i = 0; i < rounds; i++) {
        const struct site *s = &sites[rand() % nsites]; int op = rand() % M_N, want = mutate(&j, base, s, op, man, sizeof man), r;
        if (want < 0) { i--; continue; }                        /* nothing of that kind to do there: another */
        r = check_text(man);
        if (r < 0) continue;                                    /* no manifest any more: refused, which is not "installed" */
        ++*judged; *free_ += r == 0;
        if (r != want || (r && !(ch.update && ch.ask && ch.n))) { if (wrong++ < 12) printf("     (%s, %s: %s, not %s: %s)\n", s->path, m_names[op], r ? "the button" : "no button", want ? "the button" : "none", ch.n ? ch.c[0].line : ""); }
    }
    json_free(&j);
    return wrong;
}

#ifdef SCOPES_DEMO
/* ---- this build as a later hassmic's would be (tests/unit/scopes_demo.py: net.lan 2 widens what the capability lets
 * a plugin do, its entries' key "names" came with it; ha.entities 2 is within, a sensor's "precision" came with it):
 * a key of a later version than the manifest requires refuses it (finding 3 of a review of 4e0a202), an update that
 * comes to require a widening version asks for the button and one within does not, every raise listed (finding 4) */
#define LAN(extra) "\"net.lan\": [ { \"proto\": \"tcp\", \"ports\": [ 80 ]" extra " } ]"
#define SENSOR(extra) "\"ha.entities\": [ { \"name\": \"t\", \"kind\": \"sensor\", \"label\": \"T\"" extra " } ]"
static int pair_rs(const char *oreq, const char *ocaps, const char *oset, const char *nreq, const char *ncaps, const char *nset)
{
    static char man[16384]; int r;
    ps_remove("probe");
    req_up = oreq;
    if (check_text(full(man, sizeof man, 1, TOP, ocaps, oset)) != 1 || ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err)) { req_up = ""; return -3; }
    req_up = nreq;
    r = check_text(full(man, sizeof man, 2, TOP, ncaps, nset));
    req_up = "";
    return r;
}
static int pair_r(const char *oreq, const char *ocaps, const char *nreq, const char *ncaps) { return pair_rs(oreq, ocaps, HOST, nreq, ncaps, HOST); }
static void demo(void)
{
    const char *both = LAN("") ", " SENSOR("");
    CHECK(pm_scope_version(PM_SCOPE_NET_LAN) == 4 && pm_scope_version(PM_SCOPE_HA_ENTITIES) == 2 && pm_widens(PM_SCOPE_NET_LAN, 1, 2) && !pm_widens(PM_SCOPE_NET_LAN, 2, 3) && pm_widens(PM_SCOPE_NET_LAN, 3, 4)
          && !pm_widens(PM_SCOPE_HA_ENTITIES, 1, 2), "(the demo's build: net.lan 2 widens, 3 within, 4 widens; ha.entities 2 within)");
    /* the keys */
    CHECK(parse(LAN(", \"names\": true"), "") == -1 && strstr(err, "\"net.lan\": the key \"names\" is net.lan 2's, and the manifest requires net.lan 1"), "net.lan's key \"names\" (net.lan 2's) under requires net.lan 1: refused, named");
    req_up = "net.lan:2";
    CHECK(!parse(LAN(", \"names\": true"), "") && m.requires[PM_SCOPE_NET_LAN] == 2, "... under requires net.lan 2: taken");
    CHECK(parse(LAN(", \"tls\": true"), "") == -1 && strstr(err, "and nothing else"), "a key no version has, under requires net.lan 2: refused as ever (a capability's entries are strict)");
    ps_scope_set(PM_SCOPE_NET_LAN, 1);
    CHECK(parse(LAN(", \"names\": true"), "") && m.newer && !strcmp(err, "needs a newer hassmic: net.lan 2"), "a build of net.lan 1 (a fall back): \"needs a newer hassmic: net.lan 2\", not a key it does not read");
    ps_scope_set(PM_SCOPE_NET_LAN, 4); req_up = "";
    CHECK(parse(SENSOR(", \"precision\": 2"), "") == -1 && strstr(err, "\"ha.entities.sensor\": the key \"precision\" is ha.entities 2's, and the manifest requires ha.entities 1"), "a sensor's \"precision\" (ha.entities 2's) under ha.entities 1: refused");
    req_up = "ha.entities:2";
    CHECK(!parse(SENSOR(", \"precision\": 2"), "") && parse(SENSOR(", \"unit\": \"C\", \"precision\": 2"), "") == 0, "... under ha.entities 2: taken");
    CHECK(parse("\"ha.entities\": [ { \"name\": \"t\", \"kind\": \"switch\", \"label\": \"T\", \"precision\": 2 } ]", "") == -1 && strstr(err, "a switch has a key this version does not read"), "... and a switch's \"precision\" (a sensor's only): refused as ever");
    req_up = "";
    /* an update that comes to require a later version */
    CHECK(!strcmp(ps_change_kind(PS_SPEAKS), "speaks") && !strcmp(ps_change_kind(PS_LIMIT), "limit") && !strcmp(ps_change_kind(PS_MORE), "more") && !strcmp(ps_change_kind(PS_CHANGED), "changed"),
          "the kinds as the page names them (web.c's check): a raise within is \"speaks\", which asks for nothing");
    CHECK(pair_r("", both, "net.lan:2", both) == 1 && ch.n == 1 && ch.c[0].kind == PS_MORE && strstr(ch.c[0].line, "Speaks net.lan 2, was 1, which lets a plugin do more: net.lan 2, the demo's"),
          "requires net.lan 1 -> 2 (a version that widens): the button, \"More: Speaks net.lan 2, was 1, which lets a plugin do more ...\"");
    CHECK(pair_r("", both, "ha.entities:2", both) == 0 && ch.update && ch.n == 1 && ch.c[0].kind == PS_SPEAKS && !ch.ask && strstr(ch.c[0].line, "Speaks ha.entities 2, was 1: ha.entities 2, the demo's"),
          "requires ha.entities 1 -> 2 (a version within): no button, the line shown");
    CHECK(pair_r("", both, "net.lan:2,ha.entities:2", both) == 1 && ch.n == 2, "both raised: the button, a line each");
    CHECK(pair_r("net.lan:2,ha.entities:2", both, "", both) == 0 && !ch.n, "both lowered: nothing to ask, nothing to say");
    CHECK(pair_r("", SENSOR(""), "net.lan:2", both) == 1 && ch.n >= 1 && ch.c[0].kind == PS_NEW && !strstr(ch.c[0].line, "Speaks"), "net.lan new at 2: a capability more (its line), not a raise");
    /* Every widening version's words up to the one required, and a raise said version by version (finding 5 of a
     * review of 60a8589: a raise over several versions said one version's words, a capability asked for anew or a
     * first install only version 1's line) */
#define LINE(i, k, text) (ch.n > (i) && ch.c[i].kind == (k) && strstr(ch.c[i].line, text))
    CHECK(pair_r("", both, "net.lan:4", both) == 1 && ch.n == 3 && LINE(0, PS_MORE, "Speaks net.lan 2, was 1, which lets a plugin do more: net.lan 2, the demo's")
          && LINE(1, PS_SPEAKS, "Speaks net.lan 3, was 1: net.lan 3, the demo's") && LINE(2, PS_MORE, "Speaks net.lan 4, was 1, which lets a plugin do more: net.lan 4, the demo's"),
          "requires net.lan 1 -> 4: the button, a line for each of 2 (widens), 3 (within) and 4 (widens), each in its words");
    CHECK(pair_r("net.lan:2", both, "net.lan:4", both) == 1 && ch.n == 2 && LINE(0, PS_SPEAKS, "Speaks net.lan 3, was 2") && LINE(1, PS_MORE, "Speaks net.lan 4, was 2, which lets a plugin do more: net.lan 4, the demo's"),
          "requires net.lan 2 -> 4: 3 and 4, each said");
    CHECK(pair_r("net.lan:2", both, "net.lan:3", both) == 0 && ch.n == 1 && !ch.ask && LINE(0, PS_SPEAKS, "Speaks net.lan 3, was 2: net.lan 3, the demo's"), "requires net.lan 2 -> 3 (within): no button, the line");
    CHECK(pair_r("", SENSOR(""), "net.lan:4", both) == 1 && ch.n == 3 && ch.c[0].kind == PS_NEW && strstr(ch.c[0].line, "Connects to any device on your network")
          && LINE(1, PS_NEW, "Speaks net.lan 2, which lets a plugin do more: net.lan 2, the demo's") && LINE(2, PS_NEW, "Speaks net.lan 4, which lets a plugin do more: net.lan 4, the demo's"),
          "net.lan asked for anew at 4: its line, and the words of 2 and 4 (not 3's, within) as part of what is new");
    /* Many raises (finding 4 of a review of 4c55b54: the version lines came before the capabilities' and could fill
     * every place, a capability asked for anew left out): versions within one after another are one line, the
     * capabilities' lines come first, and the last PS_CHANGES_ASK places hold only lines that ask.  The demo's state
     * goes to 35: 2 to 4 within, then widening (odd) and within (even) by turns */
#define ST "\"state\": true, "
    { char sb[512]; const char *stb = sb; snprintf(sb, sizeof sb, ST "%s", both);
    CHECK(pair_r("", stb, "state:4", stb) == 0 && ch.update && ch.n == 1 && !ch.ask
          && LINE(0, PS_SPEAKS, "Speaks state 2 to 4, was 1: state 2, the demo's; state 3, the demo's; state 4, the demo's"),
          "requires state 1 -> 4, three versions within: one line with the words of each, no button");
    CHECK(pair_r("state:3", stb, "state:6", stb) == 1 && ch.n == 3 && LINE(0, PS_SPEAKS, "Speaks state 4, was 3: state 4, the demo's")
          && LINE(1, PS_MORE, "Speaks state 5, was 3, which lets a plugin do more") && LINE(2, PS_SPEAKS, "Speaks state 6, was 3: state 6, the demo's"), "state 3 -> 6: 4 (within), 5 (widens), 6 (within), a line each");
    { int r = pair_r("", ST LAN(""), "state:35", stb), shown_late = 0, speaks = 0;
      for (int i = 0; i < ch.n; i++) { speaks += ch.c[i].kind == PS_SPEAKS; shown_late |= i >= PS_CHANGES - PS_CHANGES_ASK && (ch.c[i].kind == PS_SPEAKS || ch.c[i].kind == PS_LIMIT); }
      CHECK(r == 1 && ch.ask && ch.more && ch.n == PS_CHANGES && LINE(0, PS_NEW, "Home Assistant") && LINE(1, PS_SPEAKS, "Speaks state 2 to 4, was 1") && LINE(2, PS_MORE, "Speaks state 5, was 1")
            && !shown_late && speaks == (PS_CHANGES - PS_CHANGES_ASK) / 2,
            "ha.entities asked for anew and state 1 -> 35 (33 lines for 24 places): the capability first, then the versions; the last places only lines that ask, \"and more\"");
      if (r != 1 || shown_late || !ch.more) for (int i = 0; i < ch.n; i++) printf("     (%d %s)\n", ch.c[i].kind, ch.c[i].line); }
    /* ... and a setting's row asked for anew beside them (finding 3 of a review of 61e3d8e: the versions' lines came
     * before the settings' rows, eight "More: Speaks state N" filled the places that ask, and a new secret row was
     * left out with "And more." for all of it): the rows first, the versions last, and what has no place counted */
    { int r = pair_rs("", stb, HOST, "state:35", stb, HOST ", { \"name\": \"token\", \"label\": \"Token\", \"type\": \"secret\" }"), asked = 0;
      for (int i = 0; i < ch.n; i++) asked += ch.c[i].kind == PS_NEW && strstr(ch.c[i].line, "Asks you for \"Token\" (a secret)") != NULL;
      CHECK(r == 1 && ch.ask && asked == 1 && LINE(0, PS_NEW, "Asks you for \"Token\"") && ch.n == PS_CHANGES && ch.more > 0 && ch.n + ch.more + ch.more_shown == 33,
            "state 1 -> 35 and a new secret row (33 lines for 24 places): the row first, never left out; the versions after it; the 9 left out counted (that ask apart from that show)");
      if (r != 1 || !asked || ch.n + ch.more + ch.more_shown != 33) { printf("     (n %d, more %d, shown %d)\n", ch.n, ch.more, ch.more_shown); for (int i = 0; i < ch.n; i++) printf("     (%d %s)\n", ch.c[i].kind, ch.c[i].line); } } }
#undef ST
#undef LINE
    { char wj[PS_WIDENS_JSON];
      req_up = "net.lan:4,ha.entities:2";
      CHECK(!parse(both, "") && ps_widens_json(&m, wj, sizeof wj) && !strcmp(wj, "[{\"scope\":\"net.lan\",\"version\":2,\"words\":\"net.lan 2, the demo's\"},{\"scope\":\"net.lan\",\"version\":4,\"words\":\"net.lan 4, the demo's\"}]"),
            "a first install requiring net.lan 4 (the dialog's \"widens\", web.c check_json, and the card's): the words of 2 and 4, none of ha.entities 2 (within)");
      if (strcmp(wj, "[{\"scope\":\"net.lan\",\"version\":2,\"words\":\"net.lan 2, the demo's\"},{\"scope\":\"net.lan\",\"version\":4,\"words\":\"net.lan 4, the demo's\"}]")) printf("     (%s)\n", wj);
      CHECK(ps_widens_json(&m, wj, 10) == 2 && !strcmp(wj, "[]"), "... where it does not fit: [] (never cut)");
      req_up = "";
      CHECK(!parse(both, "") && ps_widens_json(&m, wj, sizeof wj) == 2 && !strcmp(wj, "[]"), "... requiring version 1 of each: none"); }
    /* the version installed one this build cannot read (a fall back from a build of ha.entities 2): what it required
     * is not known, so a widening version is held against version 1 */
    { const struct ps_plugin *l; static char man[16384];
      ps_remove("probe"); req_up = "net.lan:2,ha.entities:2";
      CHECK(check_text(full(man, sizeof man, 1, TOP, both, HOST)) == 1 && !ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err), "(installed: net.lan 2, ha.entities 2)");
      ps_scope_set(PM_SCOPE_HA_ENTITIES, 1); ps_scan(&l);
      req_up = "net.lan:2";
      CHECK(check_text(full(man, sizeof man, 2, TOP, both, HOST)) == 1 && ch.update && ch.c[0].kind == PS_MORE && strstr(ch.c[0].line, "Speaks net.lan 2 (what the version installed speaks cannot be read)"),
            "an update of one this build cannot read, requiring net.lan 2: the button (held against net.lan 1)");
      ps_scope_set(PM_SCOPE_HA_ENTITIES, 2); req_up = ""; ps_remove("probe"); ps_scan(&l); }
}
#endif

int main(int argc, char **argv)
{
    static uint8_t prog[4 << 20], dyn[4 << 20], pkg[5 << 20], mut[5 << 20]; static char man[8192], dir[200], path[300];
    size_t pl, dl, ml, n; uint8_t dg[32]; FILE *f; int tier = -1, button = -1;
    if (argc < 3) return 2;
    f = fopen(argv[1], "rb"); pl = f ? fread(prog, 1, sizeof prog, f) : 0; if (f) fclose(f);
    f = fopen(argv[2], "rb"); dl = f ? fread(dyn, 1, sizeof dyn, f) : 0; if (f) fclose(f);
    if (!pl || !dl || pl > 3 << 20) { printf("cannot read the test programs\n"); return 2; }
    /* HASSMIC_PLUGIN_SCOPES (ps_scopes_env, read once): lower only, a scope it does not know and a version not a number
     * left alone; ps_scope_set puts back what the tests below expect */
    setenv("HASSMIC_PLUGIN_SCOPES", "button:0,wake:9,nosuch:0,storage:x", 1);
    CHECK(ps_scope_have(PM_SCOPE_BUTTON) == 0 && ps_scope_have(PM_SCOPE_WAKE) == 1 && ps_scope_have(PM_SCOPE_STORAGE) == 1 && ps_scope_have(PM_SCOPE_CORE) == 1,
          "HASSMIC_PLUGIN_SCOPES: button lowered to 0, wake never above this build's 1, the rest as built");
    ps_scope_set(PM_SCOPE_BUTTON, 1); unsetenv("HASSMIC_PLUGIN_SCOPES");
    ps_community(1);            /* (community plugins on, as the Plugins page's switch leaves them: sideloads below are about other things) */
    snprintf(dir, sizeof dir, "/tmp/plugin_store_test.%d", (int)getpid()); mkdir(dir, 0700); setenv("HASSMIC_STATE", dir, 1);
    crypto_eddsa_key_pair(sk, pk, seed); seed[0] = 9; crypto_eddsa_key_pair(rsk, rpk, seed);
    snprintf(path, sizeof path, "%s/index.pub", dir); f = fopen(path, "wb"); fwrite(rpk, 1, 32, f); fclose(f); setenv("HASSMIC_PLUGINS_PUB", path, 1);
#ifdef SCOPES_DEMO
    u_prog = prog; u_pl = pl; (void)dyn; (void)dl; (void)pkg; (void)mut; (void)man; (void)ml; (void)n; (void)dg; (void)tier; (void)button;
    demo();
    printf("plugin_store (the demo's later versions): %s\n", fails ? "FAILED" : "all passed");
    return fails != 0;
#endif

    /* ---- the manifest */
    CHECK(!parse("\"storage\": 4", "") && m.storage_kb == 4 && m.nsettings == 2 && m.version == 3 && !strcmp(m.settings[1].def, "514"), "a manifest as the design's example");
    { uint8_t a[32]; memcpy(a, m.accepted, 32);
      CHECK(!parse("\"storage\": 4", "") && !memcmp(a, m.accepted, 32), "the accepted section's digest is the same for the same manifest");
      CHECK(!parse("\"storage\": 8", "") && memcmp(a, m.accepted, 32), "... another with a larger quota");
      CHECK(!parse("\"storage\": 4", ", { \"name\": \"password\", \"label\": \"Home Assistant password\", \"type\": \"secret\" }") && memcmp(a, m.accepted, 32), "... and with one more setting"); }
    CHECK(parse("\"teleport\": true", "") && strstr(err, "does not know"), "a capability this version does not know");
    CHECK(parse("\"storage\": 4, \"storage\": 4", ""), "a capability twice");
    CHECK(parse("\"storage\": 4000", ""), "a quota over the limit");
    CHECK(parse("\"wake\": false", ""), "a capability set to false");
    CHECK(!parse("\"wake\": true, \"files\": 2", FILES_SETTING) && m.settings[2].type == PS_FILES && m.settings[2].nfiles == 1 && !strcmp(m.settings[2].files[0].as, "file.bin") && m.settings[2].max_items == 4, "a file list");
    CHECK(parse("\"wake\": true", FILES_SETTING) && strstr(err, "does not ask for"), "a file list without the files capability");
    CHECK(parse("\"storage\": 4", ", { \"name\": \"server\", \"label\": \"x\", \"type\": \"text\" }"), "a setting twice");
    CHECK(parse("\"storage\": 4", ", { \"name\": \"Bad Name\", \"label\": \"x\", \"type\": \"text\" }"), "a setting's name with a space");
    CHECK(parse("\"storage\": 4", ", { \"name\": \"x\", \"label\": \"x\", \"type\": \"html\" }"), "a type there is none of");
    { static char t[40000]; size_t k = (size_t)snprintf(t, sizeof t, "{ \"id\": \"probe\", \"name\": \""); memset(t + k, 'x', 300); strcpy(t + k + 300, "\" }");
      CHECK(ps_manifest_parse(t, strlen(t), &m, err, sizeof err), "a name of 300 characters");
      memset(t, '[', sizeof t - 1); t[sizeof t - 1] = 0;
      CHECK(ps_manifest_parse(t, strlen(t), &m, err, sizeof err), "40000 open brackets"); }

    /* ---- the manifest as it is written: each thing the reader took for another, at its limit and past it */
    CHECK(!with("\"id\"", "\"id\"") && m.nsettings == 6 && m.memory_mb == 16 && m.threads == 2 && !strcmp(m.settings[1].def, "514") && !strcmp(m.settings[2].def, "fast")
          && !strcmp(m.settings[3].def, "1") && !strcmp(m.settings[4].def, "hello") && m.settings[5].files[0].optional == 1 && m.settings[5].fields[1].max == 0.99, "a manifest with one of everything");
    /* 1: numbers.  A whole number as written, within 32 bits; nothing else stands for one or for the default */
    CHECK(edge("\"memory_mb\": 16", "\"memory_mb\": 96", "\"memory_mb\": 97", "memory_mb is 8 to 96") && edge("\"memory_mb\": 16", "\"memory_mb\": 8", "\"memory_mb\": 7", "memory_mb is 8 to 96"), "memory_mb: 8 and 96 taken, 7 and 97 refused");
    CHECK(!each("\"memory_mb\": 16", "memory_mb is 8 to 96", "\"memory_mb\": 4294967312", "\"memory_mb\": \"16\"", "\"memory_mb\": null", "\"memory_mb\": true", "\"memory_mb\": 16.0", "\"memory_mb\": 1.6e1",
                "\"memory_mb\": 16e0", "\"memory_mb\": [16]", "\"memory_mb\": 100000000000000000000000000000000000016", NULL), "memory_mb 4294967312 (16 in its low 32 bits), \"16\", null, true, 16.0, 1.6e1, [16]: refused, never 16 nor the default");
    CHECK(edge("\"threads\": 2", "\"threads\": 16", "\"threads\": 17", "threads is 1 to 16") && !each("\"threads\": 2", "threads is 1 to 16", "\"threads\": 0", "\"threads\": \"2\"", "\"threads\": 4294967298", "\"threads\": 2.5", NULL), "threads: 16 taken; 17, 0, \"2\", 4294967298, 2.5 refused");
    CHECK(edge("\"version\": 3", "\"version\": 1000000000", "\"version\": 1000000001", "version: a whole number") && !each("\"version\": 3", "version: a whole number", "\"version\": 4294967299", "\"version\": \"3\"", "\"version\": 3.0", "\"version\": 0", "\"version\": -3", "\"version\": 1e3", NULL),
          "version: 1000000000 taken; one more, 4294967299 (3 on the Echo, not on the PC), \"3\", 3.0, 1e3 refused");
    CHECK(edge("\"files\": 2 }", "\"files\": 64 }", "\"files\": 65 }", "\"files\": 1 to 64") && !each("\"files\": 2 }", "\"files\": 1 to 64", "\"files\": 4294967298 }", "\"files\": 2.0 }", "\"files\": \"2\" }", "\"files\": true }", "\"files\": [2] }", NULL), "files: 64 MB taken; 65, 4294967298, 2.0, \"2\", true refused");
    CHECK(!with("\"wake\": true", "\"storage\": 1024") && m.storage_kb == 1024 && !each("\"wake\": true", "\"storage\": 1 to 1024", "\"storage\": 1025", "\"storage\": 4294967300", "\"storage\": 4e0", "\"storage\": {}", "\"storage\": true", NULL), "storage: 1024 kB taken; 1025, 4294967300, 4e0 refused");
    CHECK(edge("\"max_kb\": 64", "\"max_kb\": 16384", "\"max_kb\": 16385", "max_kb is 1 to 16384") && !each("\"max_kb\": 64", "max_kb is 1 to 16384", "\"max_kb\": \"64\"", "\"max_kb\": 4294967360", "\"max_kb\": 6.4e1", "\"x\": 0", NULL), "a file's max_kb: 16384 taken; 16385, \"64\", 4294967360, 6.4e1, none refused");
    CHECK(edge("\"max_items\": 4", "\"max_items\": 64", "\"max_items\": 65", "max_items 1..64") && !each("\"max_items\": 4", "max_items 1..64", "\"max_items\": 4.0", "\"max_items\": \"4\"", "\"max_items\": 4294967300", "\"max_items\": null", NULL)
          && !with("\"max_items\": 4,", "") && m.settings[5].max_items == 16, "max_items: 64 taken; 65, 4.0, \"4\", 4294967300, null refused; left out: 16");
    CHECK(edge("\"max\": 65535", "\"max\": 2147483647", "\"max\": 2147483648", "max is a whole number") && edge("\"min\": 1,", "\"min\": -2147483648,", "\"min\": -2147483649,", "min is a whole number")
          && !each("\"min\": 1,", "min is a whole number", "\"min\": \"1\",", "\"min\": 1.5,", "\"min\": true,", NULL) && with("\"min\": 1,", "\"min\": 65536,") == -1 && strstr(err, "min over max"), "a setting's min and max: 32 bits taken, one past refused; \"1\", 1.5, true refused; min over max");
    /* 2: texts.  What is written is what is shown: no escape that json_str makes a space of or drops */
    CHECK(!each("\"Probe\"", "control character (0x7f)", "\"Pro\x7f" "be\"", NULL) && !each("\"title\": \"A\"", "control character (0x7f)", "\"title\": \"A\x7f\"", "\"ti\x7ftle\": \"A\"", NULL) && !each("\"Probe\"", "not JSON", "\"Pro\tbe\"", NULL) && !each("\"Probe\"", "written as an escape", "\"Pro\\bbe\"", "\"Pro\\nbe\"", "\"Pro\\tbe\"", "\"Pro\\rbe\"", "\"Pro\\fbe\"", "\"Pro\\u0000be\"",
                "\"Pro\\u0007be\"", "\"Pro\\u001fbe\"", "\"Pro\\u007Fbe\"", "\"Pro\\ud83dbe\"", "\"Pro\\ude00be\"", "\"Pro\\ud83d\\u0041\"", "\"Pro\\ud83d\"", NULL), "a name with \\b (read as \"Probe\" before), \\n, \\t, \\u0007, \\u007f, half a surrogate pair: refused");
    CHECK(!with("\"Probe\"", "\"Pr\\u006fbe \\ud83d\\ude00 \\/ \\\\ \\\" \\u0020\"") && !strcmp(m.name, "Probe \xf0\x9f\x98\x80 / \\ \"  "), "... with escapes for a letter, a whole pair, a slash, a quote: taken as what they say");
    CHECK(!each("\"title\": \"A\"", "written as an escape", "\"title\": \"A\\n\"", NULL) && !each("\"fast\", \"slow\"", "written as an escape", "\"fast\", \"sl\\bow\"", NULL), "... the same in an option and in the catalog, which the page reads");
    /* 3: keys */
    CHECK(!each("\"threads\"", "written with an escape", "\"thre\\u0061ds\"", NULL) && !each("\"wake\"", "written with an escape", "\"wa\\u006be\"", NULL) && !each("\"title\"", "written with an escape", "\"t\\u0069tle\"", NULL),
          "a key written with an escape (threads: the default before; wake; one in the catalog): refused");
    CHECK(!each("\"threads\": 2", "twice in one object", "\"threads\": 2, \"threads\": 16", NULL) && !each("\"label\": \"Port\"", "twice in one object", "\"label\": \"Port\", \"label\": \"Password\"", NULL)
          && !each("\"wake\": true", "twice in one object", "\"wake\": true, \"wake\": true", NULL) && !each("\"title\": \"A\"", "twice in one object", "\"title\": \"A\", \"title\": \"B\"", NULL)
          && !with("\"label\": \"Port\"", "\"label\": \"Port\", \"help\": \"Port\""), "a key twice in one object (the manifest's, a setting's, the capabilities', the catalog's): refused; the same key in two objects is not that");
    /* 4: a list is a list, an object an object */
    CHECK(!with("[\"*\"]", "[\"dot9\", \"*\"]") && !each("[\"*\"]", "models are a list of names", "{\"a\": \"*\"}", "\"*\"", "[1, \"*\"]", "[\"*\", null]", "[[\"*\"]]", "[\"\"]", "true", NULL)
          && !each("[\"*\"]", "not for this model", "[\"dot9\"]", "[]", NULL), "models: a list of names; an object (walked as a list before), a text, a number in it: refused; another model's: not for this one");
    CHECK(!each("\"settings\": [", "settings are a list", "\"settings\": {}, \"x\": [", "\"settings\": \"none\", \"x\": [", "\"settings\": null, \"x\": [", "\"settings\": 0, \"x\": [", NULL) && !with("\"settings\": [", "\"x\": [") && !m.nsettings,
          "settings: {} or a text or null (no settings before) refused; left out: none");
    CHECK(!each("[\"fast\", \"slow\"]", "options are a list", "{\"fast\": \"slow\"}", "\"fast\"", NULL) && !each("[\"fast\", \"slow\"]", "its options", "[\"fast\", [\"slow\"]]", "[\"fast\", true]", "[\"fast\", 1.5]", "[\"fast\", \"\"]", NULL)
          && !each("[\"fast\", \"slow\"]", "without options", "[]", NULL) && !with("\"slow\"]", "7]") && !strcmp(m.settings[2].options[1], "7"), "options: a list of texts or whole numbers; an object, true, 1.5, an empty text refused");
    CHECK(!each("\"files\": [ { \"suffix\": \".bin\", \"max_kb\": 64, \"optional\": true } ]", "no files (a list)", "\"files\": { \"suffix\": \".bin\", \"max_kb\": 64 }", "\"files\": []", "\"files\": \".bin\"", "\"x\": 0", NULL)
          && !each("{ \"suffix\": \".bin\", \"max_kb\": 64, \"optional\": true }", "not an object", "\".bin\"", "[]", NULL) && !each("\"item\": {", "grouped \"by\"", "\"item\": [], \"x\": {", "\"x\": {", NULL)
          && !each("\"by\": \"stem\"", "grouped \"by\"", "\"by\": \"name\"", "\"by\": [\"stem\"]", "\"by\": 1", NULL), "an item: an object with by \"stem\" and a list of files, each an object");
    CHECK(!each("\"fields\": [", "fields are a list", "\"fields\": {}, \"x\": [", "\"fields\": \"level\", \"x\": [", NULL) && !each("{ \"name\": \"level\", \"label\": \"Level\", \"type\": \"int\", \"min\": 0, \"max\": 9 }", "a field is not an object", "\"level\"", "[]", NULL)
          && !with("\"fields\": [", "\"x\": [") && !m.settings[5].nfields, "fields: a list of objects; left out: none");
    CHECK(!each("{ \"wake\": true, \"files\": 2 }", "no \"capabilities\"", "[\"wake\"]", "\"wake\"", "true", NULL), "capabilities: an object");
    /* 5: a default of its row's kind, within its limits, never cut */
    CHECK(!with("\"default\": true", "\"default\": false") && !strcmp(m.settings[3].def, "0") && !each("\"default\": true", "its default is true or false", "\"default\": 1", "\"default\": \"on\"", "\"default\": \"true\"", "\"default\": null", "\"default\": []", NULL), "a switch's default: true or false; 1, \"on\", null refused");
    CHECK(edge("\"default\": 514", "\"default\": 65535", "\"default\": 65536", "a whole number within min and max") && edge("\"default\": 514", "\"default\": 1", "\"default\": 0", "a whole number within min and max")
          && !each("\"default\": 514", "a whole number within min and max", "\"default\": \"514\"", "\"default\": 514.0", "\"default\": 5.14e2", "\"default\": true", "\"default\": 4294967810", NULL), "a number's default: min and max taken, one past each refused; \"514\", 514.0, 4294967810 refused");
    CHECK(!with("\"default\": \"fast\"", "\"default\": \"slow\"") && !strcmp(m.settings[2].def, "slow") && !each("\"default\": \"fast\"", "one of its options", "\"default\": \"medium\"", "\"default\": \"\"", "\"default\": 0", "\"default\": true", "\"default\": [\"fast\"]", NULL)
          && !with("\"slow\"], \"default\": \"fast\"", "7], \"default\": 7") && !strcmp(m.settings[2].def, "7"), "a choice's default: one of its options");
    { char a[200], b[200]; snprintf(a, sizeof a, "\"default\": \"%064d\"", 0); snprintf(b, sizeof b, "\"default\": \"%065d\"", 0);
      CHECK(edge("\"default\": \"hello\"", a, b, "text of at most 64 bytes") && strlen(m.settings[4].def) == 0 && !with("\"default\": \"hello\"", a) && strlen(m.settings[4].def) == 64
            && !each("\"default\": \"hello\"", "text of at most 64 bytes", "\"default\": 5", "\"default\": true", "\"default\": null", "\"default\": {}", NULL), "a text's default: 64 bytes taken, 65 refused (cut to 64 before); a number, true refused"); }
    CHECK(!with("\"type\": \"host\"", "\"type\": \"host\", \"default\": \"192.168.1.20\"") && !strcmp(m.settings[0].def, "192.168.1.20")
          && !each("\"type\": \"host\"", "a host name or an address", "\"type\": \"host\", \"default\": \"my server\"", "\"type\": \"host\", \"default\": \"http://a/\"", "\"type\": \"host\", \"default\": 1", NULL), "a host's default: what the row itself would take");
    CHECK(!each("\"max_items\": 4,", "a file list has none", "\"max_items\": 4, \"default\": \"a\",", NULL), "a file list's default: there is none");
    CHECK(!each("[\"fast\", \"slow\"]", "the option \"fast\" twice", "[\"fast\", \"slow\", \"fast\"]", NULL) && !each("\"cutoff\"", "field level twice", "\"level\"", NULL), "an option twice, a field twice: refused");
    CHECK(!each("\"min\": 0.5", "min, max and step are numbers", "\"min\": \"0.5\"", "\"min\": null", "\"min\": true", NULL) && !each("\"max\": 9 }", "min, max and step are numbers", "\"max\": \"9\" }", NULL) && !each("\"step\": 0.01", "min, max and step are numbers", "\"step\": \"0.01\"", "\"step\": []", NULL)
          && edge("\"min\": 0.5", "\"min\": 0.99", "\"min\": 0.991", "min over max") && !with(", \"min\": 0, \"max\": 9 }", " }") && m.settings[5].fields[0].max == 1e9, "a field's min, max, step: numbers (a max of \"9\" was 1e9); min up to max; left out: 0 to 1e9");
    /* 6 */
    CHECK(!with("\"optional\": true", "\"optional\": false") && !m.settings[5].files[0].optional && !with(", \"optional\": true", "") && !m.settings[5].files[0].optional
          && !each("\"optional\": true", "optional is true or false", "\"optional\": 1", "\"optional\": 0", "\"optional\": \"yes\"", "\"optional\": null", "\"optional\": []", NULL), "a file's optional: true, false or left out; 1, \"yes\", null refused");
    /* 7: a capability without parameters is asked for with true */
    CHECK(!each("\"wake\": true", "true, or left out", "\"wake\": 0", "\"wake\": 1", "\"wake\": \"yes\"", "\"wake\": []", "\"wake\": {}", NULL) && !each("\"wake\": true", "leave it out instead of false", "\"wake\": false", "\"wake\": null", NULL)
          && !each("\"wake\": true", "true, or left out", "\"state\": \"no\"", "\"log.read\": 1", "\"call\": {}", "\"request.audio\": 0", "\"audio.music\": [1]", "\"log.read.full\": 2", "\"audio.volume\": \"\"", "\"request.text\": 1.0", NULL)
          && !with("\"wake\": true", "\"state\": true, \"net.lan\": [ { \"proto\": \"tcp\", \"ports\": \"any\" } ], \"ha.entities\": [ " ENT_A " ]"), "\"wake\": 0, \"state\": \"no\": refused; those with parameters keep theirs");
    /* net.lan and net.internet, each a list of { "proto", "ports" } (a list of ports or "any"), as net.server's
     * entries are read: exactly those keys, ports whole and never 5555; the line names each entry's protocol and ports */
#define LANE_A "\"net.lan\": [ { \"proto\": \"tcp\", \"ports\": [ 80, 443 ] }, { \"proto\": \"udp\", \"ports\": \"any\" } ], \"net.internet\": [ { \"proto\": \"tcp\", \"ports\": [ 443 ] } ]"
    { char line[PS_CAP_LINE], line2[PS_CAP_LINE]; int r = with("\"wake\": true", LANE_A);
      if (!r) { ps_cap_line(&m, PM_CAP_NET_LAN, line, sizeof line); ps_cap_line(&m, PM_CAP_NET_INTERNET, line2, sizeof line2); }
      CHECK(!r && m.nlan == 2 && !m.lan[0].udp && m.lan[0].nports == 2 && m.lan[0].ports[1] == 443 && m.lan[1].udp && m.lan[1].any && m.ninet == 1 && m.inet[0].ports[0] == 443
            && !strcmp(line, "Connects to any device on your network (TCP, ports 80, 443; UDP, any port)") && !strcmp(line2, "Connects to the internet (TCP, port 443)"),
            "net.lan and net.internet: their entries read, each line with every entry's protocol and ports");
      CHECK(!each("\"wake\": true", "is a list", "\"net.lan\": true", "\"net.internet\": { \"proto\": \"tcp\", \"ports\": [ 1 ] }", "\"net.lan\": 443", NULL)
            && !each("\"wake\": true", "has no entry", "\"net.lan\": []", "\"net.internet\": []", NULL)
            && !each("\"wake\": true", "and nothing else", "\"net.lan\": [ { \"proto\": \"tcp\", \"ports\": [ 1 ], \"tls\": true } ]", "\"net.lan\": [ { \"ports\": [ 1 ], \"to\": \"lan\" } ]", "\"net.internet\": [ 443 ]", NULL)
            && !each("\"wake\": true", "\"proto\" is", "\"net.lan\": [ { \"proto\": \"sctp\", \"ports\": [ 1 ] } ]", "\"net.internet\": [ { \"ports\": [ 1 ] } ]", NULL)
            && !each("\"wake\": true", "not 5555", "\"net.lan\": [ { \"proto\": \"tcp\", \"ports\": [ 5555 ] } ]", "\"net.internet\": [ { \"proto\": \"tcp\", \"ports\": [ 0 ] } ]",
                     "\"net.lan\": [ { \"proto\": \"tcp\", \"ports\": [ 65536 ] } ]", "\"net.lan\": [ { \"proto\": \"tcp\", \"ports\": [ 80.0 ] } ]", "\"net.lan\": [ { \"proto\": \"tcp\", \"ports\": [ \"80\" ] } ]", NULL)
            && !each("\"wake\": true", "a list of ports, or \"any\"", "\"net.lan\": [ { \"proto\": \"tcp\", \"ports\": \"all\" } ]", "\"net.internet\": [ { \"proto\": \"tcp\", \"ports\": \"setting:port\" } ]", NULL)
            && !each("\"wake\": true", "names no port", "\"net.lan\": [ { \"proto\": \"tcp\", \"ports\": [] } ]", NULL)
            && !each("\"wake\": true", "entries at most", "\"net.lan\": [ { \"proto\": \"tcp\", \"ports\": [ 1 ] }, { \"proto\": \"tcp\", \"ports\": [ 2 ] }, { \"proto\": \"tcp\", \"ports\": [ 3 ] }, { \"proto\": \"tcp\", \"ports\": [ 4 ] }, { \"proto\": \"tcp\", \"ports\": [ 5 ] } ]", NULL)
            && !each("\"wake\": true", "ports an entry at most", "\"net.internet\": [ { \"proto\": \"tcp\", \"ports\": [ 1, 2, 3, 4, 5, 6, 7, 8, 9 ] } ]", NULL),
            "net.lan, net.internet: a list of 1 to 4 entries of exactly proto and ports, 1 to 8 whole ports (never 5555) or \"any\"; anything else refused, in words");
      /* N1 of a review of 7be9343: net.lan never at DNS's ports (the router's forwarder asks the internet: a way out
       * for "your network"), net.discover's search neither; net.internet takes them (it has the internet) */
      CHECK(!each("\"wake\": true", "asking the internet (DNS)", "\"net.lan\": [ { \"proto\": \"udp\", \"ports\": [ 53 ] } ]", "\"net.lan\": [ { \"proto\": \"tcp\", \"ports\": [ 80, 853 ] } ]",
                  "\"net.lan\": [ { \"proto\": \"tcp\", \"ports\": [ 53 ] } ]", "\"net.discover\": { \"udp\": 53, \"to\": \"broadcast\" }", "\"net.discover\": { \"udp\": 853, \"to\": \"239.255.255.250\" }", NULL)
            && !with("\"wake\": true", "\"net.internet\": [ { \"proto\": \"udp\", \"ports\": [ 53, 853 ] } ]") && !with("\"wake\": true", "\"net.lan\": [ { \"proto\": \"udp\", \"ports\": [ 54, 852, 5353 ] } ]"),
            "N1: net.lan's ports and net.discover's never DNS's 53 or 853, TCP or UDP; net.internet's may be; the ports beside them are"); }
    /* 9: one object, from the first byte, and nothing but white space after it */
    CHECK(!with("\"id\"", "\"id\"") && ps_manifest_parse(rich, rich_len + 5, &m, err, sizeof err) == -1 && strstr(err, "and nothing else") && (memcpy(rich + rich_len, "\0{}", 3), ps_manifest_parse(rich, rich_len + 3, &m, err, sizeof err)) == -1,
          "a NUL after the object, and more after it: refused");
    { static char t[8300]; size_t l;
      with("\"id\"", "\"id\"");
      l = (size_t)snprintf(t, sizeof t, " %s", rich);
      CHECK(ps_manifest_parse(t, l, &m, err, sizeof err) == -1 && strstr(err, "and nothing else"), "a space before it: refused");
      l = (size_t)snprintf(t, sizeof t, "\xef\xbb\xbf%s", rich);
      CHECK(ps_manifest_parse(t, l, &m, err, sizeof err) == -1, "a byte order mark before it: refused");
      l = (size_t)snprintf(t, sizeof t, "%s\n", rich);
      CHECK(!ps_manifest_parse(t, l, &m, err, sizeof err), "a line end after it, as an editor leaves one: taken");
      l = (size_t)snprintf(t, sizeof t, "%s {}", rich);
      CHECK(ps_manifest_parse(t, l, &m, err, sizeof err) == -1, "a second object after it: refused");
      l = (size_t)snprintf(t, sizeof t, "[%s]", rich);
      CHECK(ps_manifest_parse(t, l, &m, err, sizeof err) == -1 && strstr(err, "not an object"), "a list with it inside: refused"); }
    CHECK(!each("\"threads\": 2", "not written as JSON writes one", "\"threads\": 02", "\"threads\": 2.", "\"threads\": -", "\"threads\": 2e", "\"threads\": 2e+", "\"threads\": 1-1", "\"threads\": 2.e1", "\"threads\": -02", "\"threads\": 2E5.5", "\"threads\": 2+2", NULL)
          && !each("\"threads\": 2", "not JSON", "\"threads\": 0x2", "\"threads\": +2", "\"threads\": .5", "\"threads\": NaN", NULL), "numbers json.c lets by (02, 2., 2e, 1-1, 2+2): refused");
    CHECK(!each("\"id\": \"a\"", "not written as JSON writes one", "\"id\": \"a\", \"weight\": 01.5", "\"id\": \"a\", \"n\": 5.", NULL), "... also in the catalog, which the Echo does not read");
    CHECK(!each("\"id\": \"a\"", "past what a number holds", "\"id\": \"a\", \"weight\": 1e999", "\"id\": \"a\", \"weight\": -1e999", NULL) && !with("\"id\": \"a\"", "\"id\": \"a\", \"weight\": -1.5e-3, \"n\": 0, \"k\": -0.0E+2"), "1e999: refused; -1.5e-3, 0, -0.0E+2 taken");

    /* 10: "requires" (sdk/PROTOCOL.md "Versions"): the least version of each scope it speaks, core and the scope of
     * each capability and nothing else, whole numbers from 1, read before anything a scope's version may bring.  A
     * scope this build has not, or a later version than its own: one for a newer hassmic, never a misreading */
    auto_requires = 0;
    {
      CHECK(!with("\"requires\": { \"core\": 1, \"wake\": 1, \"files\": 1 }", "\"requires\": { \"core\": 1, \"wake\": 1, \"files\": 1 }") && m.requires[PM_SCOPE_CORE] == 1 && m.requires[PM_SCOPE_WAKE] == 1 && m.requires[PM_SCOPE_FILES] == 1 && !m.requires[PM_SCOPE_BUTTON] && !m.newer,
            "requires: core, wake and files at 1, as the manifest's capabilities are");
      CHECK(with("\"requires\": { \"core\": 1, \"wake\": 1, \"files\": 1 }" ", ", "") == -1 && strstr(err, "has no \"requires\""), "requires left out: refused");
      CHECK(with("\"requires\": { \"core\": 1, \"wake\": 1, \"files\": 1 }", "\"requires\": [ \"core\" ]") == -1 && strstr(err, "has no \"requires\"") && with("\"requires\": { \"core\": 1, \"wake\": 1, \"files\": 1 }", "\"requires\": 1") == -1, "requires as a list or a number: refused");
      CHECK(with("\"requires\": { \"core\": 1, \"wake\": 1, \"files\": 1 }", "\"requires\": { \"wake\": 1, \"files\": 1 }") == -1 && strstr(err, "no version of core"), "core missing: refused");
      CHECK(with("\"requires\": { \"core\": 1, \"wake\": 1, \"files\": 1 }", "\"requires\": { \"core\": 1, \"wake\": 1 }") == -1 && strstr(err, "capability \"files\": its scope is not in \"requires\""), "a capability without its scope: refused");
      CHECK(with("\"requires\": { \"core\": 1, \"wake\": 1, \"files\": 1 }", "\"requires\": { \"core\": 1, \"wake\": 1, \"files\": 1, \"storage\": 1 }") == -1 && strstr(err, "names storage, which the manifest does not ask for"), "a scope it does not ask for: refused");
      CHECK(with("\"requires\": { \"core\": 1, \"wake\": 1, \"files\": 1 }", "\"requires\": { \"core\": 1, \"wake\": 1, \"files\": 1, \"telepathy\": 1 }") == -1 && !strcmp(err, "needs a newer hassmic: telepathy 1") && m.newer
            && !strcmp(m.id, "probe") && !strcmp(m.name, "Probe") && m.version == 3 && !strcmp(m.author, "someone") && !m.caps,
            "a scope this build does not know: \"needs a newer hassmic: telepathy 1\", the plugin's id, name, author and version kept, nothing else read");
      CHECK(with("\"requires\": { \"core\": 1, \"wake\": 1, \"files\": 1 }", "\"requires\": { \"core\": 1, \"wake\": 2, \"files\": 1 }") == -1 && !strcmp(err, "needs a newer hassmic: wake 2") && m.newer, "a later version of one it has: \"needs a newer hassmic: wake 2\"");
      CHECK(with("\"requires\": { \"core\": 1, \"wake\": 1, \"files\": 1 }", "\"requires\": { \"core\": 9, \"wake\": 2, \"files\": 1, \"zz\": 3 }") == -1 && !strcmp(err, "needs a newer hassmic: core 9, wake 2, zz 3"), "all of them named, in the manifest's order");
      /* (a key of a later version's capability: never read where requires already says "newer") */
      { const char *at = strstr(RICH, "\"capabilities\": { \"wake\": true, \"files\": 2 }");
        static char t3[8192]; size_t l3 = (size_t)snprintf(t3, sizeof t3, "%.*s\"capabilities\": { \"wake\": { \"mode\": \"later\" }, \"files\": 2 }%s", (int)(at - RICH), RICH, at + strlen("\"capabilities\": { \"wake\": true, \"files\": 2 }"));
        memcpy(strstr(t3, "\"wake\": 1"), "\"wake\": 2", 9);
        CHECK(ps_manifest_parse(t3, l3, &m, err, sizeof err) == -1 && !strcmp(err, "needs a newer hassmic: wake 2"), "wake 2 with a form of wake that version 1 does not read: \"needs a newer hassmic\", not a misreading of the form");
        memcpy(strstr(t3, "\"wake\": 2"), "\"wake\": 1", 9);
        CHECK(ps_manifest_parse(t3, l3, &m, err, sizeof err) == -1 && strstr(err, "true, or left out"), "... and with wake 1 that form is wake 1's error"); }
      CHECK(!each("\"requires\": { \"core\": 1, \"wake\": 1, \"files\": 1 }", "a version, a whole number from 1", "\"requires\": { \"core\": 1.0, \"wake\": 1, \"files\": 1 }", "\"requires\": { \"core\": \"1\", \"wake\": 1, \"files\": 1 }",
                  "\"requires\": { \"core\": true, \"wake\": 1, \"files\": 1 }", "\"requires\": { \"core\": 0, \"wake\": 1, \"files\": 1 }", "\"requires\": { \"core\": -1, \"wake\": 1, \"files\": 1 }",
                  "\"requires\": { \"core\": 65536, \"wake\": 1, \"files\": 1 }", "\"requires\": { \"core\": 1e0, \"wake\": 1, \"files\": 1 }", "\"requires\": { \"core\": null, \"wake\": 1, \"files\": 1 }", NULL)
            && !with("\"requires\": { \"core\": 1, \"wake\": 1, \"files\": 1 }", "\"requires\": { \"core\": 1, \"wake\": 1, \"files\": 1 }") && !with("\"requires\": { \"core\": 1, \"wake\": 1, \"files\": 1 }", "\"requires\": { \"files\": 1, \"wake\": 1, \"core\": 1 }"),
            "a version: 1.0, \"1\", true, 0, -1, 65536, 1e0, null refused; any order of the keys taken");
      CHECK(with("\"requires\": { \"core\": 1, \"wake\": 1, \"files\": 1 }", "\"requires\": { \"core\": 1, \"core\": 1, \"wake\": 1, \"files\": 1 }") == -1 && strstr(err, "twice"), "a scope twice: refused");
      CHECK(!each("\"requires\": { \"core\": 1, \"wake\": 1, \"files\": 1 }", "a scope's name is 1 to 24", "\"requires\": { \"core\": 1, \"Wake\": 1, \"wake\": 1, \"files\": 1 }", "\"requires\": { \"core\": 1, \"\": 1, \"wake\": 1, \"files\": 1 }",
                  "\"requires\": { \"core\": 1, \"aaaaaaaaaaaaaaaaaaaaaaaaa\": 1, \"wake\": 1, \"files\": 1 }", "\"requires\": { \"core\": 1, \"a b\": 1, \"wake\": 1, \"files\": 1 }", NULL)
            && with("\"requires\": { \"core\": 1, \"wake\": 1, \"files\": 1 }", "\"requires\": { \"core\": 1, \"aaaaaaaaaaaaaaaaaaaaaaaa\": 1, \"wake\": 1, \"files\": 1 }") == -1 && !strcmp(err, "needs a newer hassmic: aaaaaaaaaaaaaaaaaaaaaaaa 1"),
            "a scope's name: \"Wake\", \"\", 25 letters, \"a b\" refused as no name; 24 letters is a name (of a later hassmic's scope)");
      /* this build lowered, as HASSMIC_PLUGIN_SCOPES makes one on the PC: what it requires of it is "newer" */
      ps_scope_set(PM_SCOPE_WAKE, 0);
      CHECK(with("\"requires\": { \"core\": 1, \"wake\": 1, \"files\": 1 }", "\"requires\": { \"core\": 1, \"wake\": 1, \"files\": 1 }") == -1 && !strcmp(err, "needs a newer hassmic: wake 1") && m.newer, "a build without wake: the same manifest needs a newer hassmic");
      ps_scope_set(PM_SCOPE_WAKE, 1);
      CHECK(!with("\"requires\": { \"core\": 1, \"wake\": 1, \"files\": 1 }", "\"requires\": { \"core\": 1, \"wake\": 1, \"files\": 1 }") && !m.newer && ps_scope_have(PM_SCOPE_WAKE) == 1, "(wake back at this build's 1)");
      ps_scope_set(PM_SCOPE_WAKE, 7);
      CHECK(ps_scope_have(PM_SCOPE_WAKE) == 1, "never above this build's own");
      { int same = 1; for (int b = 0; ps_cap_name(b); b++) { int sc = pm_scope_of(ps_cap_name(b), strlen(ps_cap_name(b))); same &= sc > 0 && pm_scope_cap(sc) == b; }
        CHECK(same && !ps_cap_name(PM_SCOPES - 1), "each capability is the scope of its name (messages.def's), and there is no other"); } }
    auto_requires = 1;
    /* 11: every text is UTF-8 (they go to the page and to Home Assistant, which drops the link on one that is not) */
    CHECK(!with("\"Probe\"", "\"Pr\xc3\xb6" "be \xe2\x82\xac \xf0\x9f\x98\x80\"") && !strcmp(m.name, "Pr\xc3\xb6" "be \xe2\x82\xac \xf0\x9f\x98\x80")
          && !each("\"Probe\"", "not UTF-8", "\"Pro\xff" "be\"", "\"Pro\xc3\"", "\"Pro\x80" "be\"", "\"Pro\xc0\xaf" "be\"", "\"Pro\xed\xa0\x80" "be\"", "\"Pro\xf4\x90\x80\x80" "be\"", "\"Pro\xe2\x82" "be\"", "\"Pro\xf8\x88\x80\x80\x80\"", NULL)
          && !each("\"label\": \"Port\"", "not UTF-8", "\"label\": \"P\xe9rt\"", NULL) && !each("\"fast\", \"slow\"", "not UTF-8", "\"fast\", \"sl\xa0w\"", NULL) && !each("\"title\": \"A\"", "not UTF-8", "\"title\": \"\xfe\"", "\"t\xfftle\": \"A\"", NULL),
          "a name in UTF-8 taken; a byte that is no character, one cut short, one written too long, half a pair, one past U+10FFFF, in a name, a label, an option, the catalog: refused");

    /* 12: a number is read whole or not at all (json_num reads 39 bytes of one) */
    { char a[200], b[200]; snprintf(a, sizeof a, "\"max\": 0.9%036d", 0); snprintf(b, sizeof b, "\"max\": 0.9%037d", 0);        /* 39 and 40 characters */
      CHECK(edge("\"max\": 0.99", a, b, "more than 39 characters") && !each("\"max\": 0.99", "more than 39 characters", "\"max\": 1.0000000000000000000000000000000000000e9", NULL)
            && !each("\"id\": \"a\"", "more than 39 characters", "\"id\": \"a\", \"n\": 1000000000000000000000000000000000000000", NULL), "a number of 39 characters taken, of 40 refused (a field's max with its exponent past the 39th; one in the catalog)"); }
    /* 13: a host's default is a host name; -0 is no way to write a number that is kept as its text */
    CHECK(!with("\"type\": \"host\"", "\"type\": \"host\", \"default\": \"a-b.example.org\"") && !with("\"type\": \"host\"", "\"type\": \"host\", \"default\": \"x\"")
          && !each("\"type\": \"host\"", "a host name or an address", "\"type\": \"host\", \"default\": \"\"", "\"type\": \"host\", \"default\": \"-\"", "\"type\": \"host\", \"default\": \"...\"", "\"type\": \"host\", \"default\": \".a\"",
                   "\"type\": \"host\", \"default\": \"a.\"", "\"type\": \"host\", \"default\": \"a..b\"", "\"type\": \"host\", \"default\": \"-a.b\"", "\"type\": \"host\", \"default\": \"a-.b\"", "\"type\": \"host\", \"default\": \"a.b-\"",
                   "\"type\": \"host\", \"default\": \"a_b\"", "\"type\": \"host\", \"default\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.b\"", NULL)
          && !with("\"type\": \"host\"", "\"type\": \"host\", \"default\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.b\""),
          "a host's default: a name or an address of at most 64 bytes; \"\", \"-\", \"...\", a label empty or with a hyphen at its end, 66 bytes: refused");
    CHECK(!with("\"slow\"], \"default\": \"fast\"", "0, -7], \"default\": -7") && !strcmp(m.settings[2].def, "-7")
          && !each("\"slow\"]", "its options", "-0]", NULL) && !each("\"slow\"], \"default\": \"fast\"", "one of its options", "0], \"default\": -0", NULL)
          && !each("\"min\": 1, \"max\": 65535, \"default\": 514", "a whole number within min and max", "\"min\": -5, \"max\": 65535, \"default\": -0", NULL) && !each("\"min\": 1,", "min is a whole number", "\"min\": -0,", NULL),
          "an option, a choice's default, a number's default written -0: refused (0 and -7 taken)");

    /* ---- the program */
    CHECK(!ps_elf_check(prog, pl, err, sizeof err), "a static program");
    CHECK(ps_elf_check(dyn, dl, err, sizeof err) && strstr(err, "interpreter"), "a dynamic one: it names an interpreter");
    CHECK(ps_elf_check(prog, 40, err, sizeof err), "40 bytes of one");
    CHECK(ps_elf_check((const uint8_t *)"#!/bin/sh\necho hello\n", 21, err, sizeof err), "a script");

    /* ---- whole packages */
    crypto_blake2b(dg, 32, prog, pl);
    ml = manifest(man, sizeof man, "\"wake\": true, \"files\": 2", FILES_SETTING, dg);
    n = package(pkg, man, ml, prog, pl, 3, sk);
    CHECK(!staged(pkg, n, &tier, &button) && tier == PS_OFFICIAL && button == 1, "an official package: taken, the button needed");
    n = package(pkg, man, ml, prog, pl, 0, sk);
    CHECK(!staged(pkg, n, &tier, &button) && tier == PS_SIDELOAD, "one without a listing: a sideload");
    n = package(pkg, man, ml, prog, pl, 2, sk);
    CHECK(staged(pkg, n, &tier, &button) == -1 && strstr(err, "another plugin or version"), "a listing for another version");
    listing_tier = "community"; n = package(pkg, man, ml, prog, pl, 3, sk);
    CHECK(!staged(pkg, n, &tier, &button) && tier == PS_COMMUNITY, "a listing as community: a community plugin");
    { static const char *const odd[] = { "Official", "officia", "official ", "sideload", "" }; int bad = 0;
      for (size_t i = 0; i < sizeof odd / sizeof odd[0]; i++) { listing_tier = odd[i]; n = package(pkg, man, ml, prog, pl, 3, sk); bad += staged(pkg, n, &tier, &button) == -1 && strstr(err, "listing is broken") != NULL; }
      CHECK(bad == 5, "a tier that is neither official nor community: a broken listing (5 of them)"); }
    listing_tier = "official";
    /* a second index key (docs/plugins.md 5.2, a new key's first step): a listing by either is taken, by a third key
     * never, and by the second only where the build reads one */
    { uint8_t k2[64], p2[32], k3[64], p3[32], s2[32] = { 10 }, s3[32] = { 11 }; char kp[300]; FILE *kf;
      crypto_eddsa_key_pair(k2, p2, s2); crypto_eddsa_key_pair(k3, p3, s3);
      snprintf(kp, sizeof kp, "%s/index2.pub", dir); kf = fopen(kp, "wb"); fwrite(p2, 1, 32, kf); fclose(kf);
      listing_key = k2; n = package(pkg, man, ml, prog, pl, 3, sk);
      CHECK(staged(pkg, n, &tier, &button) == -1 && strstr(err, "not signed by the plugin index's key"), "no second key: a listing by another key than the first is refused");
      setenv("HASSMIC_PLUGINS_PUB2", kp, 1);
      CHECK(!staged(pkg, n, &tier, &button) && tier == PS_OFFICIAL && button == 1, "a second key read: its listing taken, official as it says");
      listing_tier = "community"; n = package(pkg, man, ml, prog, pl, 3, sk); listing_tier = "official";
      CHECK(!staged(pkg, n, &tier, &button) && tier == PS_COMMUNITY, "... and its community listing as community");
      listing_key = rsk; n = package(pkg, man, ml, prog, pl, 3, sk);
      CHECK(!staged(pkg, n, &tier, &button) && tier == PS_OFFICIAL, "the first key's listing still taken beside it");
      listing_key = k3; n = package(pkg, man, ml, prog, pl, 3, sk);
      CHECK(staged(pkg, n, &tier, &button) == -1 && strstr(err, "not signed by the plugin index's key"), "a third key's listing: refused (neither key's)");
      listing_key = rsk; unsetenv("HASSMIC_PLUGINS_PUB2"); }
    /* A key under which any signature holds (edkey.h, K3 of a review of a30d6ed) as the author's: the manifest "signed"
     * with the null signature verifies under it, and the check refuses it all the same */
    { static char mz[8192]; static uint8_t pz[5 << 20]; char *at; size_t mzl, nz, off; static const uint8_t nullsig[64] = { 1 };
      memcpy(mz, man, ml + 1); at = strstr(mz, "\"author_key\": \""); memset(at + 15, '0', 64); mzl = strlen(mz);
      nz = package(pz, mz, mzl, prog, pl, 0, sk);
      off = 512 + (mzl + 511) / 512 * 512 + 512; memcpy(pz + off, nullsig, 64);           /* manifest.sig: the null signature */
      CHECK(staged(pz, nz, &tier, &button) == -1 && strstr(err, "any signature holds"), "an author key of small order (all zero), with the null signature as the manifest's: refused at the check");
      memset(pz + off, 0x55, 64);
      CHECK(staged(pz, nz, &tier, &button) == -1 && strstr(err, "any signature holds"), "... and with any other signature: refused the same, before the signature is asked"); }
    /* 5.2's move to a new index key (K2 of a review of a30d6ed): an official plugin installed with the first key's
     * listing, community plugins off (their default).  The index moves to the second key: the Echo takes its listing
     * of the very version installed (no button, no install), and a build of the second key alone runs it as
     * official.  Without the move that build has it as a file's, not started, and no wake word engine (the review's
     * REPRO); the listing taken then brings it back.  Never a lower tier, another version or another manifest */
    { uint8_t k2[64], p2[32], s2[32] = { 10 }, dg2[32], other[32] = { 1 }; char kp[300], rp[300], why[400]; FILE *kf; const struct ps_plugin *l; int k, t = -1, off = -1, acc = -1;
      static uint8_t pk2[5 << 20], pc2[5 << 20], pz2[5 << 20]; size_t n2, nc2, nz2;
      crypto_eddsa_key_pair(k2, p2, s2);
      snprintf(kp, sizeof kp, "%s/index2.pub", dir); kf = fopen(kp, "wb"); fwrite(p2, 1, 32, kf); fclose(kf);
      snprintf(rp, sizeof rp, "%s", getenv("HASSMIC_PLUGINS_PUB"));
      #define PROBE_STATE() do { ps_lock(); k = ps_scan(&l); t = off = acc = -1; why[0] = 0; for (int i = 0; i < k; i++) if (!strcmp(l[i].m.id, "probe")) { t = l[i].tier; off = l[i].community_off; acc = l[i].accepted; snprintf(why, sizeof why, "%s", l[i].refused); } ps_unlock(); } while (0)
      setenv("HASSMIC_PLUGINS_PUB2", kp, 1); ps_community(0);
      n = package(pkg, man, ml, prog, pl, 3, sk);
      CHECK(!staged(pkg, n, &tier, &button) && tier == PS_OFFICIAL && !ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err), "K2: official by the first key, installed with community plugins off");
      crypto_blake2b(dg2, 32, (const uint8_t *)man, ml);
      listing_key = k2; n2 = package(pk2, man, ml, prog, pl, 3, sk);
      listing_tier = "community"; nc2 = package(pc2, man, ml, prog, pl, 3, sk); listing_tier = "official"; listing_key = rsk;
      PROBE_STATE();
      CHECK(ps_relist_reason(NULL, 0) == 1, "R2: a listing by the first of two keys: a reason for the daily look to read the index (5.2's second key is the new one)");
      CHECK(ps_relist_wanted("probe", 3, dg2, PS_OFFICIAL, 1, 0) == 1, "the index signed by the second key lists the version installed, official: its listing wanted");
      CHECK(!ps_relist_wanted("probe", 3, dg2, PS_OFFICIAL, 0, 0) && !ps_relist_wanted("probe", 3, dg2, PS_COMMUNITY, 1, 0) && !ps_relist_wanted("probe", 4, dg2, PS_OFFICIAL, 1, 0)
            && !ps_relist_wanted("probe", 3, other, PS_OFFICIAL, 1, 0) && !ps_relist_wanted("nothere", 3, dg2, PS_OFFICIAL, 1, 0),
            "... not by the same key and tier, not a lower tier, not another version, another manifest or a plugin not installed");
      CHECK(ps_relist(pc2, nc2, err, sizeof err) == 0, "a community listing of it by the second key: nothing taken (a lower tier)");
      CHECK(ps_relist(pk2, n2, err, sizeof err) == 1, "the second key's official listing: taken");
      CHECK(ps_relist(pk2, n2, err, sizeof err) == 0 && !ps_relist_wanted("probe", 3, dg2, PS_OFFICIAL, 1, 0), "again: nothing to take, none wanted");
      PROBE_STATE();
      CHECK(ps_relist_reason(NULL, 0) == 0, "R2: its listing the second key's: no reason (the daily look reads the revocation list alone)");
      setenv("HASSMIC_PLUGINS_LEAST", "4:0", 1); PROBE_STATE();
      CHECK(ps_relist_reason(NULL, 0) == 1, "R2: a listing under the build's least index serial: a reason");
      unsetenv("HASSMIC_PLUGINS_LEAST"); PROBE_STATE();
      unsetenv("HASSMIC_PLUGINS_PUB2"); setenv("HASSMIC_PLUGINS_PUB", kp, 1);           /* the build of the second key alone */
      PROBE_STATE();
      CHECK(t == PS_OFFICIAL && !off && !why[0] && acc == 1, "a build of the second key alone: official, started, still accepted (no button, no install)");
      { char w2[300] = ""; CHECK(!ps_engine_refused("probe", w2, sizeof w2), "... and the wake word engine as before"); }
      /* the review's REPRO: the first key's listing under that build */
      n = package(pkg, man, ml, prog, pl, 3, sk);                                     /* (listed by the first key) */
      { char lp[400]; snprintf(lp, sizeof lp, "%s/probe/pkg/listing.sig", ps_dir());
        uint8_t *a = memmem(pkg, n, "listing.sig", 11); size_t at = (size_t)(a - pkg), sz = strtoul((char *)pkg + at + 124, NULL, 8);
        kf = fopen(lp, "wb"); fwrite(pkg + at + 512, 1, sz, kf); fclose(kf); }
      setenv("HASSMIC_PLUGINS_PUB", kp, 1);
      PROBE_STATE();
      CHECK(t == PS_SIDELOAD && off == 1 && strstr(why, "plugin index key this hassmic does not read") && strstr(why, "by itself within a minute and takes its listing anew") && !strcmp(why + strlen(why) - 22, "takes its listing anew") && acc == 1,
            "the first key's listing under the build of the second alone: a file's, not started (community off): the review's REPRO, its card saying why, whole (the Echo's own look within a minute: T6 of a review of 9cd7a77)");
      CHECK(ps_relist_reason(NULL, 0) == 2, "R2: a listing there that does not hold: a reason to read the index, and an urgent one (S2 of a review of 3fcd319: a look within a minute, then hourly)");
      { struct ps_idv sk1[2] = { { "probe", 4 }, { "other", 3 } }, sk2[1] = { { "probe", 3 } };
        CHECK(ps_relist_reason(sk1, 2) == 2 && ps_relist_reason(sk2, 1) == 0,
              "S4: a version the last look found not listed is no reason (the daily look read index.json for good); another version of it, or another plugin's, does not count"); }
      { int u = 0; ps_lock(); for (int i = 0; i < k; i++) if (!strcmp(l[i].m.id, "probe")) u = l[i].listing_unread; ps_unlock(); CHECK(u == 1, "... said as such (listing_unread: plugin.c's self test does not take it for the owner's choice)"); }
      /* R6: a listing that could not be written now (the flash: listing.sig a folder, which rename refuses) is -2,
       * not the package's own answer: asked again at the next look */
      { char lp[400], lq[450]; snprintf(lp, sizeof lp, "%s/probe/pkg/listing.sig", ps_dir()); snprintf(lq, sizeof lq, "%s/x", lp);
        unlink(lp); mkdir(lp, 0700); kf = fopen(lq, "w"); if (kf) fclose(kf);
        CHECK(ps_relist(pk2, n2, err, sizeof err) == -2 && strstr(err, "cannot write"), "R6: the listing not written (the flash): -2, asked again later, not \"nothing to take\"");
        unlink(lq); rmdir(lp); }
      n = package(pkg, man, ml, prog, pl, 3, sk);                                     /* (listed by the first key, again) */
      { char lp[400]; snprintf(lp, sizeof lp, "%s/probe/pkg/listing.sig", ps_dir());
        uint8_t *a = memmem(pkg, n, "listing.sig", 11); size_t at = (size_t)(a - pkg), sz = strtoul((char *)pkg + at + 124, NULL, 8);
        kf = fopen(lp, "wb"); fwrite(pkg + at + 512, 1, sz, kf); fclose(kf); }
      { char w2[300] = ""; CHECK(ps_engine_refused("probe", w2, sizeof w2) && strstr(w2, "from a file") && !strstr(w2, "a community plugin"), "... refused as the wake word engine, in words that say a file's, not a community plugin"); }
      CHECK(ps_relist_wanted("probe", 3, dg2, PS_COMMUNITY, 0, 0) == 1 && ps_relist(pk2, n2, err, sizeof err) == 1, "the index's listing of that version (a look at it): wanted, taken");
      PROBE_STATE();
      CHECK(t == PS_OFFICIAL && !off && !why[0] && acc == 1, "official again, started, accepted: the remedy");
      /* a file's (no listing at all) takes the index's listing of the same package; a package of another manifest is
       * refused */
      { char lp[400]; snprintf(lp, sizeof lp, "%s/probe/pkg/listing.sig", ps_dir()); unlink(lp); }
      CHECK(ps_relist(pc2, nc2, err, sizeof err) == 1, "a plugin from a file: the index's community listing of the same package taken (a higher tier than none)");
      PROBE_STATE();
      CHECK(t == PS_COMMUNITY && ps_relist_reason(NULL, 0) == 0, "(community now; no reason, by the key this build reads)");
      setenv("HASSMIC_PLUGINS_PUB", rp, 1);                                             /* the first key alone: k2's listing is none */
      CHECK(ps_relist(pk2, n2, err, sizeof err) == -1 && strstr(err, "not signed"), "a listing by a key this build does not read: refused");
      { static char m3[8192]; size_t m3l; char *v3; memcpy(m3, man, ml + 1); v3 = strstr(m3, "\"Probe\""); v3[1] = 'Q'; m3l = strlen(m3);
        nz2 = package(pz2, m3, m3l, prog, pl, 3, sk);
        CHECK(ps_relist(pz2, nz2, err, sizeof err) == -1 && strstr(err, "not installed here as this very version"), "the listing of another manifest of the same version: refused"); }
      #undef PROBE_STATE
      unsetenv("HASSMIC_PLUGINS_PUB2"); setenv("HASSMIC_PLUGINS_PUB", rp, 1); ps_community(1);
      ps_lock(); ps_remove("probe"); ps_scan(&l); ps_unlock(); }
    /* community plugins switched off (docs/plugins.md 2.7): nothing but an official package is taken, at the check
     * and again as the store writes; one installed is kept, refused, its card saying why, and is no wake word engine */
    { static char m2[8192]; size_t m2l; char *at; const struct ps_plugin *l; int k, found = 0;
      ps_community(0);
      n = package(pkg, man, ml, prog, pl, 0, sk);
      CHECK(staged(pkg, n, &tier, &button) == -1 && strstr(err, "community plugins are switched off") && strstr(err, "a file"), "community plugins off: a package from a file refused at the check");
      listing_tier = "community"; n = package(pkg, man, ml, prog, pl, 3, sk); listing_tier = "official";
      CHECK(staged(pkg, n, &tier, &button) == -1 && strstr(err, "community plugins are switched off") && strstr(err, "lists it, not as official"), "... and the index's community listing");
      n = package(pkg, man, ml, prog, pl, 3, sk);
      CHECK(!staged(pkg, n, &tier, &button) && tier == PS_OFFICIAL, "... and an official one taken");
      /* another id (probe's versions below stay new): staged while on, the switch off before the press */
      memcpy(m2, man, ml + 1); at = strstr(m2, "\"probe\""); memmove(at + 7, at + 6, strlen(at + 6) + 1); at[6] = '2'; m2l = strlen(m2);
      ps_community(1); n = package(pkg, m2, m2l, prog, pl, 0, sk);
      CHECK(!staged(pkg, n, &tier, &button) && tier == PS_SIDELOAD, "(probe2 from a file checked while they are on)");
      ps_community(0);
      CHECK(ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err) == -1 && strstr(err, "community plugins are switched off"), "switched off before the press: the install writes nothing");
      ps_community(1);
      CHECK(!staged(pkg, n, &tier, &button) && !ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err), "on: probe2 installed with the press");
      ps_community(0);
      ps_lock(); k = ps_scan(&l);
      for (int i = 0; i < k; i++) if (!strcmp(l[i].m.id, "probe2")) found = l[i].community_off && strstr(l[i].refused, "from a file") && strstr(l[i].refused, PS_COMMUNITY_OFF) && l[i].accepted;
      ps_unlock();
      CHECK(found, "off: the plugin from a file kept, accepted, and refused with the switch's words");
      { char why[300] = "";
        CHECK(ps_engine_refused("probe2", why, sizeof why) && strstr(why, "community plugins are switched off"), "... and refused as the wake word engine"); }
      ps_community(1);
      ps_lock(); k = ps_scan(&l); found = 0;
      for (int i = 0; i < k; i++) if (!strcmp(l[i].m.id, "probe2")) found = 1 + (!l[i].community_off && !l[i].refused[0]);
      ps_unlock();
      CHECK(found == 2, "on again: nothing refuses it");
      ps_lock(); CHECK(!ps_remove("probe2"), "(probe2 removed)"); ps_scan(&l); ps_unlock(); }
    /* the first form, the release key's, signed by the key the Echo has: no listing of this build's */
    { static uint8_t old[5 << 20]; size_t k = package(old, man, ml, prog, pl, 3, sk); uint8_t *at = memmem(old, k, "listing 2", 9);
      if (at) { at[8] = '1'; } /* (and its signature no longer holds either: what is checked first) */
      CHECK(at && staged(old, k, &tier, &button) == -1 && strstr(err, "not signed by the plugin index's key"), "a listing of the first form: refused"); }
    /* a listing the index key did sign, with more than the serial after this manifest's lines */
    { static const char *more[] = { "0\nid probe\nversion 9\ntier official\nmanifest 00\nserial 0\n", "0", "\n", "0\n\n", "0 \n", "1234567890123\n", "x\n" };
      int bad = 0;
      for (size_t i = 0; i < sizeof more / sizeof more[0]; i++) {
          listing_serial = more[i]; n = package(pkg, man, ml, prog, pl, 3, sk);
          bad += staged(pkg, n, &tier, &button) == -1 && strstr(err, "listing is broken") != NULL;
      }
      listing_serial = "1795200000\n"; n = package(pkg, man, ml, prog, pl, 3, sk);
      CHECK(bad == 7 && !staged(pkg, n, &tier, &button) && tier == PS_OFFICIAL, "a listing's text ends with its serial and nothing else (7 that do not: refused)");
      listing_serial = "0\n"; }
    /* withdrawn by the index's revocation list (5.6): what is not official, by id up to a version or by author key */
    { static const char *const lists[] = { "{\"revoked\": 1, \"serial\": 2, \"entries\": [{\"id\": \"probe\", \"upto\": 3, \"reason\": \"sends what it hears\"}]}",
                                          "{\"revoked\": 1, \"serial\": 3, \"entries\": [{\"author_key\": \"%s\", \"reason\": \"its key was taken\"}]}",
                                          "{\"revoked\": 1, \"serial\": 4, \"entries\": [{\"id\": \"probe\", \"upto\": 2, \"reason\": \"old\"}]}" };
      char text[600], kh[65]; static uint8_t msg[1000]; uint8_t sig[64]; size_t hl = strlen(PI_SIGN_REVOKED), tl;
      hex(kh, pk, 32);
      for (int i = 0; i < 3; i++) {
          snprintf(text, sizeof text, lists[i], kh); tl = strlen(text);
          memcpy(msg, PI_SIGN_REVOKED, hl); memcpy(msg + hl, text, tl); crypto_eddsa_sign(sig, rsk, msg, hl + tl);
          CHECK(pi_revoked_take(text, tl, sig, 64, PI_SITE, err, sizeof err) == 1 && pi_revoked_serial() == 2 + i, "a newer revocation list, signed by the index key: taken");
          n = package(pkg, man, ml, prog, pl, 0, sk);
          int side = staged(pkg, n, &tier, &button);
          listing_tier = "community"; n = package(pkg, man, ml, prog, pl, 3, sk); int comm = staged(pkg, n, &tier, &button); listing_tier = "official";
          char said[300]; snprintf(said, sizeof said, "%s", err);
          n = package(pkg, man, ml, prog, pl, 3, sk); int off = staged(pkg, n, &tier, &button);
          /* every tier: the index key that makes a plugin official may withdraw it (as built 2026-11-21: an official
           * listing was out of the list's reach, and a leaked author key of an official plugin could not be stopped) */
          if (i < 2) CHECK(side == -1 && comm == -1 && off == -1 && strstr(said, "withdrew this version of probe") && strstr(said, i ? "its key was taken" : "sends what it hears") && strstr(err, "withdrew"), i ? "an author key withdrawn: its sideload, its community and its official plugin refused" : "probe up to version 3 withdrawn: the sideload, the community and the official version 3 refused");
          else CHECK(!side && !comm && !off, "probe up to version 2 only: version 3 taken however it comes");
      }
      memcpy(msg, PI_SIGN_REVOKED, hl); memcpy(msg + hl, lists[0], strlen(lists[0])); crypto_eddsa_sign(sig, rsk, msg, hl + strlen(lists[0]));
      CHECK(pi_revoked_take(lists[0], strlen(lists[0]), sig, 64, PI_SITE, err, sizeof err) == PI_OLDER && strstr(err, "older") && pi_revoked_serial() == 4, "an older list than the one kept: refused");
      sig[5] ^= 1;
      CHECK(pi_revoked_take(lists[0], strlen(lists[0]), sig, 64, PI_SITE, err, sizeof err) == -1 && strstr(err, "signature"), "a list whose signature does not hold: refused");
      pi_forget();
      CHECK(pi_revoked_serial() == 4, "the newest list is kept on the flash, read again at the next start");
      snprintf(text, sizeof text, "%s/.revoked", ps_dir()); FILE *rf = fopen(text, "r+");
      if (rf) { static char kb[2000]; size_t kn = fread(kb, 1, sizeof kb, rf); char *at = memmem(kb, kn, "\"serial\": 4", 11); if (at) at[10] = '9'; rewind(rf); fwrite(kb, 1, kn, rf); fclose(rf); }
      pi_forget();
      CHECK(pi_revoked_serial() == -1, "a kept list changed on the flash: no list, until the next look");
      snprintf(text, sizeof text, "%s/.revoked", ps_dir()); unlink(text); pi_forget(); }
    n = package(pkg, man, ml, prog, pl, 3, rsk);
    CHECK(staged(pkg, n, &tier, &button) == -1 && strstr(err, "not signed by the author key"), "a manifest signed by another key than it names");
    n = package(pkg, man, ml, dyn, dl, 3, sk);
    CHECK(staged(pkg, n, &tier, &button) == -1 && strstr(err, "not the one the manifest names"), "another program than the manifest names");
    { uint8_t d2[32]; crypto_blake2b(d2, 32, dyn, dl); ml = manifest(man, sizeof man, "\"wake\": true, \"files\": 2", FILES_SETTING, d2);
      n = package(pkg, man, ml, dyn, dl, 3, sk);
      CHECK(staged(pkg, n, &tier, &button) == -1 && strstr(err, "interpreter"), "a dynamic program, rightly named and signed"); }
    ml = manifest(man, sizeof man, "\"wake\": true, \"net.lan\": [ { \"proto\": \"tcp\", \"ports\": \"any\" } ]", "", dg);
    n = package(pkg, man, ml, prog, pl, 3, sk);
    CHECK(staged(pkg, n, &tier, &button) == -1 && strstr(err, "way out"), "wake with a way out, listed by the index key: sealed all the same");
    ml = manifest(man, sizeof man, "\"log.read.full\": true", "", dg);
    n = package(pkg, man, ml, prog, pl, 0, sk);
    CHECK(staged(pkg, n, &tier, &button) == -1 && strstr(err, "official plugins only"), "the whole log for a sideload");
    ml = manifest(man, sizeof man, "\"wake\": true, \"files\": 2", FILES_SETTING, dg);
    n = package(pkg, man, ml, prog, pl, 3, sk);
    { size_t k = n - 1024; k += member(pkg + k, "extra", "x", 1); memset(pkg + k, 0, 1024);
      CHECK(staged(pkg, k + 1024, &tier, &button) == -1 && strstr(err, "should not"), "a file in the package that does not belong there"); }
    n = package(pkg, man, ml, prog, pl, 3, sk);
    pkg[156] = '2';                                         /* the manifest as a symbolic link */
    CHECK(staged(pkg, n, &tier, &button) == -1 && strstr(err, "plain file"), "a member that is no plain file");
    pkg[156] = '0';
    CHECK(staged(pkg, 700, &tier, &button) == -2 || staged(pkg, 2000, &tier, &button) == -1, "a package cut off");

    /* installed, then: the same again, a newer one, another author */
    n = package(pkg, man, ml, prog, pl, 3, sk);
    CHECK(!staged(pkg, n, &tier, &button) && !ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err), "installed");
    { const struct ps_plugin *l; int k = ps_scan(&l);
      CHECK(k == 1 && !strcmp(l[0].m.id, "probe") && l[0].tier == PS_OFFICIAL && l[0].accepted && l[0].enabled && !strcmp(l[0].how, "button"), "listed: official, accepted with the button, enabled"); }
    CHECK(staged(pkg, n, &tier, &button) == -1 && strstr(err, "not newer"), "the same version again");
    { char *v = strstr(man, "\"version\": 3"); v[11] = '4'; n = package(pkg, man, ml, prog, pl, 4, sk);
      CHECK(!staged(pkg, n, &tier, &button) && button == 0 && ch.update && !ch.n, "version 4, nothing else changed: no button");
      { uint8_t ok[64], osk[64], opk[32], s2[32] = { 7 }; (void)ok; crypto_eddsa_key_pair(osk, opk, s2);
        memcpy(pk, opk, 32); ml = manifest(man, sizeof man, "\"wake\": true, \"files\": 2", FILES_SETTING, dg); v = strstr(man, "\"version\": 3"); v[11] = '5';
        n = package(pkg, man, ml, prog, pl, 5, osk);
        CHECK(staged(pkg, n, &tier, &button) == -1 && strstr(err, "another author"), "version 5 under another author's key: not an update");
        crypto_eddsa_key_pair(sk, pk, (uint8_t[32]){ 1, 2, 3 }); } }

    /* ---- an update against what was accepted (3.3 "wider"), parameter by parameter: the same or less needs no
     * button, anything else does and is named.  Installed: wake, files 2 MB with one list, a host and a number */
    u_prog = prog; u_pl = pl;
    /* 7: what this build lacks a scope for is not installed (a build without two of them, as HASSMIC_PLUGIN_SCOPES
     * makes one on the PC), nor one that requires a later version than this build's; the rest of these checks is about
     * updates as such */
    ps_scope_set(PM_SCOPE_NET_LAN, 0); ps_scope_set(PM_SCOPE_NET_INTERNET, 0);
    CHECK(update(4, "\"files\": 2, \"net.internet\": [ { \"proto\": \"tcp\", \"ports\": [ 443 ] } ]", FILES_SETTING, NULL, NULL) == -1 && !strcmp(err, "needs a newer hassmic: net.internet 1")
          && update(4, "\"files\": 2, \"net.lan\": [ { \"proto\": \"udp\", \"ports\": [ 514 ] } ]", FILES_SETTING, NULL, NULL) == -1 && !strcmp(err, "needs a newer hassmic: net.lan 1")
          && update(4, "\"files\": 2", FILES_SETTING, NULL, NULL) == 1, "the internet, the LAN in a build without them: \"needs a newer hassmic: net.internet 1\"; files alone: taken");
    ps_scope_set(PM_SCOPE_NET_LAN, 1); ps_scope_set(PM_SCOPE_NET_INTERNET, 1);
    CHECK(req_pkg(4, "files", 2, "\"files\": 2", FILES_SETTING) == -1 && !strcmp(err, "needs a newer hassmic: files 2"), "files 2, a version this build has not: refused at install, \"needs a newer hassmic: files 2\"");
    /* request.audio and request.text are built, and sealed away from wake as any way out is (3.3) */
    CHECK(update(4, "\"wake\": true, \"files\": 2, \"request.audio\": true", FILES_SETTING, NULL, NULL) == -1 && strstr(err, "way out")
          && update(4, "\"wake\": true, \"files\": 2, \"request.text\": true", FILES_SETTING, NULL, NULL) == -1 && strstr(err, "way out"), "wake with request.audio or request.text: refused");
    /* call is built, and sealed away from wake as any way out is (3.3): an update of this wake plugin with it */
    CHECK(update(4, "\"wake\": true, \"files\": 2, \"call\": true", FILES_SETTING, NULL, NULL) == -1 && strstr(err, "way out"), "wake with call: refused (a call carries the room out)");
    { const char *W = "\"wake\": true, \"files\": 2";
      CHECK(update(4, "\"wake\": true, \"files\": 1", FILES_SETTING, NULL, NULL) == 0 && ch.update && !ch.n, "a smaller quota: narrower, no button");
      CHECK(update(4, "\"wake\": true, \"files\": 3", FILES_SETTING, NULL, NULL) == 1 && one(PS_MORE, "up to 3 MB), was 2 MB"), "a larger one: the button, and \"More: ... was 2 MB\"");
      CHECK(update(4, "\"wake\": true, \"files\": 2, \"storage\": 4", FILES_SETTING, NULL, NULL) == 1 && one(PS_NEW, "Keeps up to 4 kB"), "a capability that was not accepted: \"New: ...\"");
      CHECK(update(4, W, fs(", { \"name\": \"quiet\", \"label\": \"Quiet\", \"type\": \"bool\" }"), NULL, NULL) == 1 && one(PS_NEW, "A setting it did not have: \"Quiet\" (a switch)"), "one more switch among its settings: the button (a row is a row)");
      CHECK(update(4, W, fs(", { \"name\": \"password\", \"label\": \"Home Assistant password\", \"type\": \"secret\" }"), NULL, NULL) == 1
            && one(PS_NEW, "Asks you for \"Home Assistant password\" (a secret)"), "a new row that asks for a secret: the button");
      CHECK(update(4, W, fs(", { \"name\": \"where\", \"label\": \"Where\", \"type\": \"text\" }"), NULL, NULL) == 1 && one(PS_NEW, "(text)"), "... for text");
      CHECK(update(4, W, FILES_SETTING, "\"Server\"", "\"Secret\"") == 1 && one(PS_CHANGED, "\"Server\" is now called \"Secret\""), "the label of a row that takes a host changed: the button");
      CHECK(update(4, W, FILES_SETTING, "\"Port\"", "\"Pork\"") == 1 && one(PS_CHANGED, "\"Port\" is now called \"Pork\""), "the label of a number changed: the button");
      CHECK(update(4, W, FILES_SETTING, "\"memory_mb\": 16, \"threads\": 2", "\"memory_mb\": 96, \"threads\": 9") == 0 && ch.update && !ch.ask && ch.n == 2 && ch.c[0].kind == PS_LIMIT && strstr(ch.c[0].line, "up to 96 MB of memory, was 16 MB")
            && ch.c[1].kind == PS_LIMIT && strstr(ch.c[1].line, "up to 9 threads, was 2"), "more memory and threads: no button, and two lines that say so");
      CHECK(update(4, W, FILES_SETTING, "\"Probe\"", "\"Prone\"") == 1 && one(PS_CHANGED, "\"Prone\", was \"Probe\""), "another name for the plugin: the button");
      CHECK(update(4, W, FILES_SETTING, "\"someone\"", "\"anybody\"") == 1 && one(PS_CHANGED, "\"anybody\", was \"someone\""), "another author's name: the button");
      CHECK(update(4, W, fs(", { \"name\": \"extra\", \"label\": \"Extras\", \"type\": \"files\", \"item\": { \"by\": \"stem\", \"files\": [ { \"suffix\": \".bin\", \"max_kb\": 64 } ] } }"), NULL, NULL) == 1
            && one(PS_MORE, "another list of files you give it: \"Extras\""), "another file list: the button");
      CHECK(update(4, "\"files\": 2", FILES_SETTING, NULL, NULL) == 1 && one(PS_CHANGED, "No longer a wake word plugin"), "an update that drops wake: asked, though it has less");
      CHECK(update(4, "\"wake\": true, \"files\": 9, \"storage\": 4", fs(", { \"name\": \"where\", \"label\": \"Where\", \"type\": \"text\" }"), "\"Probe\"", "\"Prone\"") == 1
            && ch.n == 4, "four things at once: four lines");
      /* narrower is accepted as it is installed: what comes after is held against it */
      CHECK(update(4, "\"wake\": true, \"files\": 1", FILES_SETTING, NULL, NULL) == 0 && !ps_stage_install("update", NULL, PS_NO_PRESS, err, sizeof err), "version 4 with 1 MB installed without the button");
      CHECK(update(5, W, FILES_SETTING, NULL, NULL) == 1 && one(PS_MORE, "was 1 MB"), "version 5 back at 2 MB: more than what is accepted now");
      CHECK(update(5, "\"wake\": true, \"files\": 1", FILES_SETTING, NULL, NULL) == 0, "version 5 at 1 MB: no button");
      /* where it comes from is part of what was accepted: a lower tier than the version installed is wider, whoever
       * brings it (a review, 2026-11-21: an unlisted package by a leaked author key of an official plugin went in
       * without the button) */
      u_listed = 0;
      CHECK(update(5, "\"wake\": true, \"files\": 1", FILES_SETTING, NULL, NULL) == 1 && one(PS_CHANGED, "Comes from a file, not listed; the version installed here came from the plugin index, official"),
            "the same update, unlisted, over an official one: the button, and where each came from");
      u_listed = 1; listing_tier = "community";
      CHECK(update(5, "\"wake\": true, \"files\": 1", FILES_SETTING, NULL, NULL) == 1 && one(PS_CHANGED, "Comes from the plugin index, community"), "listed as community over an official one: the button");
      listing_tier = "official";
      /* a listing made by an index older than this build takes (5.5): no word of the index's any more, a file */
      setenv("HASSMIC_PLUGINS_LEAST", "1795200001:0", 1);
      { int t = -1, b = -1; static char man5[8192]; static uint8_t p5[5 << 20]; uint8_t d5[32]; size_t m5, k5; char *v5;
        crypto_blake2b(d5, 32, u_prog, u_pl); m5 = manifest(man5, sizeof man5, "\"wake\": true, \"files\": 1", FILES_SETTING, d5);
        v5 = strstr(man5, "\"version\": 3"); v5[11] = '5'; k5 = package(p5, man5, m5, u_prog, u_pl, 5, sk);
        CHECK(!staged(p5, k5, &t, &b) && t == PS_SIDELOAD && b == 1 && ch.n == 2 && strstr(ch.c[0].line, "Listed by an index older than this hassmic takes") && strstr(ch.c[1].line, "Comes from a file"),
              "a listing under this build's least index serial: taken as a file's package, and so the button");
        listing_serial = "1795200001\n"; k5 = package(p5, man5, m5, u_prog, u_pl, 5, sk);
        CHECK(!staged(p5, k5, &t, &b) && t == PS_OFFICIAL && b == 0, "one at the least: official, as before");
        listing_serial = "0\n"; }
      unsetenv("HASSMIC_PLUGINS_LEAST");
      /* the decision is the store's, as it writes (the eighth read): "no button" from the check, then the plugin
       * removed, then the install with nothing to show for a press */
      { struct stat st; char dir[400]; const struct ps_plugin *l; snprintf(dir, sizeof dir, "%s/probe", ps_dir());
        CHECK(!ps_remove("probe") && ps_stage_install("update: nothing new to accept", NULL, PS_NO_PRESS, err, sizeof err) == PS_NEEDS_BUTTON && strstr(err, "needs the action button now") && stat(dir, &st),
              "checked as an update that needs no button, the plugin removed, installed without a press: refused, nothing written");
        u_listed = 0;
        CHECK(update(5, "\"wake\": true, \"files\": 1", FILES_SETTING, NULL, NULL) == 1 && ps_stage_install("synced from echo-x", NULL, PS_VOUCHED, err, sizeof err) == PS_NEEDS_BUTTON && stat(dir, &st),
              "...nor on a voucher, for a package the index key did not list");
        u_listed = 1;
        CHECK(!ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err) && ps_scan(&l) == 1 && l[0].accepted && l[0].m.version == 5, "...and with the press: installed, the package was kept for it");
        CHECK(update(6, "\"wake\": true, \"files\": 1", FILES_SETTING, NULL, NULL) == 0 && !ps_stage_install("update", NULL, PS_NO_PRESS, err, sizeof err), "(and an update of that needs none again)"); }
      /* a piece is written only for the claim that began the file */
      { static uint8_t junk[2048];
        CHECK(!ps_stage_begin(7, 4096, err, sizeof err) && !ps_stage_chunk(7, 0, junk, 2048, err, sizeof err), "a claim's upload: its pieces are written");
        CHECK(ps_stage_chunk(6, 2048, junk, 2048, err, sizeof err) == -1 && strstr(err, "another install has taken its place") && ps_stage_chunk(0, 2048, junk, 2048, err, sizeof err) == -1, "a piece for another claim: refused");
        CHECK(!ps_stage_begin(8, 4096, err, sizeof err) && ps_stage_chunk(7, 0, junk, 2048, err, sizeof err) == -1 && !ps_stage_chunk(8, 0, junk, 2048, err, sizeof err),
              "the next claim began the file anew: a late piece of the one before does not land at its offset 0, its own does");
        ps_stage_drop();
        CHECK(ps_stage_chunk(8, 2048, junk, 2048, err, sizeof err) == -1, "dropped: nobody's"); } }
    /* capabilities this version reads no parameter of: the same, or gone (no list is a list of grants whose entries
     * may go: the capability's own code will say what an entry less means) */
    { const char *N = "\"net.lan\": [ { \"proto\": \"udp\", \"ports\": [ 514 ] }, { \"proto\": \"tcp\", \"ports\": [ 80 ] } ], \"net.udp\": 2"; int tier2 = -1, button2 = -1; char fp[20], want[17]; uint8_t fd[32];
      CHECK(!ps_remove("probe") && update(3, N, "", NULL, NULL) == 1 && !ch.update && !ch.n && !ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err), "another plugin, with the network: new, so all of it is accepted with the button");
      CHECK(update(4, N, "", NULL, NULL) == 0, "the same again: no button");
      CHECK(update(4, "\"net.udp\": 2, \"net.lan\": [ { \"ports\": [ 514 ], \"proto\": \"udp\" }, { \"ports\": [ 80 ], \"proto\": \"tcp\" } ]", "", NULL, NULL) == 0 && !ch.n, "the capabilities and an entry's keys in another order: no button");
      CHECK(update(4, "\"net.udp\": 2, \"net.lan\": [ { \"proto\": \"tcp\", \"ports\": [ 80 ] }, { \"proto\": \"udp\", \"ports\": [ 514 ] } ]", "", NULL, NULL) == 1 && one(PS_CHANGED, "not with the parameters you accepted"), "the entries in another order: the button");
      CHECK(update(4, "\"net.lan\": [ { \"proto\": \"udp\", \"ports\": [ 514 ] } ], \"net.udp\": 2", "", NULL, NULL) == 1 && one(PS_CHANGED, "Connects to any device on your network (UDP, port 514): not with the parameters you accepted"), "an entry less: the button (it was \"less\" until the sixth read)");
      CHECK(update(4, "\"net.lan\": [], \"net.udp\": 2", "", NULL, NULL) == -1 && strstr(err, "has no entry"), "every entry gone: refused (an entry at least,; it asked for the button before)");
      CHECK(update(4, "\"net.lan\": [ { \"proto\": \"udp\", \"ports\": [ 514 ] }, { \"proto\": \"tcp\", \"ports\": [ 80 ] } ], \"net.udp\": 1", "", NULL, NULL) == 0 && !ch.n, "net.udp's count smaller: no button (its own rule)");
      CHECK(update(4, "\"net.lan\": [ { \"proto\": \"udp\", \"ports\": [ 514 ] }, { \"proto\": \"tcp\", \"ports\": [ 80 ] } ]", "", NULL, NULL) == 0 && !ch.n, "a capability less: no button");
      CHECK(update(4, "\"net.lan\": [ { \"proto\": \"udp\", \"ports\": [ 514 ] }, { \"proto\": \"tcp\", \"ports\": \"any\" } ], \"net.udp\": 2", "", NULL, NULL) == 1 && one(PS_CHANGED, "Connects to any device on your network"), "another entry: the button");
      CHECK(update(4, "\"net.lan\": [ { \"proto\": \"udp\", \"ports\": [ 514 ] }, { \"proto\": \"udp\", \"ports\": [ 80 ] } ], \"net.udp\": 2", "", NULL, NULL) == 1 && one(PS_CHANGED, "your network"), "an entry changed: the button");
      CHECK(update(4, "\"net.lan\": [ { \"proto\": \"udp\", \"ports\": [ 514 ] }, { \"proto\": \"tcp\", \"ports\": [ 80 ] } ], \"net.udp\": 3", "", NULL, NULL) == 1 && one(PS_MORE, "reached from your network"), "a larger count: the button (\"More\")");
      CHECK(update(4, "\"net.lan\": true, \"net.udp\": 2", "", NULL, NULL) == -1 && strstr(err, "is a list"), "a list turned into \"true\": refused (it asked for the button before API 0.6 read its entries)");
      CHECK(update(4, "\"net.lan\": [ { \"proto\": \"udp\", \"ports\": [ 514 ] }, { \"proto\": \"tcp\", \"ports\": [ 80 ] } ], \"net.internet\": [ { \"proto\": \"tcp\", \"ports\": [ 443 ] } ]", "", NULL, NULL) == 1 && one(PS_NEW, "Connects to the internet"), "\"New: Connects to the internet\"");
      /* back to the wake plugin the rest of the test goes on with */
      ml = manifest(man, sizeof man, "\"wake\": true, \"files\": 2", FILES_SETTING, dg); n = package(pkg, man, ml, prog, pl, 3, sk);
      CHECK(!ps_remove("probe") && !staged(pkg, n, &tier2, &button2) && button2 == 1 && !ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err), "(the first plugin installed again)");
      /* the author key's fingerprint: 16 hex digits of its BLAKE2b-256, in fours */
      ps_fingerprint(pk, fp); crypto_blake2b(fd, 32, pk, 32); hex(want, fd, 8);
      CHECK(strlen(fp) == 19 && fp[4] == ' ' && fp[9] == ' ' && fp[14] == ' ' && !memcmp(fp, want, 4) && !memcmp(fp + 5, want + 4, 4) && !memcmp(fp + 15, want + 12, 4), "a key's fingerprint"); }

    /* ---- the review's pairs, the rule's own, and the same at random (each with a plugin of its own, installed anew) */
    { int judged = 0, free_ = 0, w;
      w = pairs(review, sizeof review / sizeof review[0]); err[0] = 0;
      CHECK(!w && sizeof review / sizeof review[0] == 31, "the review's 31 pairs: none is installed without the button but the same, the same name, and other limits");
      CHECK(pair(TOP, "\"storage\": 4", HOST, "\"name\": \"Probe\", \"author\": \"someone\", \"memory_mb\": 96, \"threads\": 16", "\"storage\": 4", HOST) == 0 && ch.update && !ch.ask && ch.n == 2 && ch.c[0].kind == PS_LIMIT && ch.c[1].kind == PS_LIMIT,
            "... and the limits are two lines to show (5.4)");
      CHECK(pair(TOP, "\"files\": 8", FL("Wake word models", BIN("64"), "4"), TOP, "\"files\": 8", FL("Home Assistant backups and SSH keys", BIN("64"), "4")) == 1 && one(PS_CHANGED, "\"Wake word models\" is now called \"Home Assistant backups and SSH keys\""), "a file list's new label is named");
      CHECK(pair(TOP, "\"files\": 8", FL("Models", BIN("64"), "4"), TOP, "\"files\": 8", FL("Models", BIN("16384"), "64")) == 1 && ch.n == 2 && ch.c[0].kind == PS_MORE && strstr(ch.c[0].line, "holds up to 64, was 4")
            && ch.c[1].kind == PS_MORE && strstr(ch.c[1].line, ".bin files of up to 16384 kB, was 64 kB"), "more and larger files: \"More: ... was 4\", \"... was 64 kB\"");
      CHECK(pair(TOP, "\"net.server\": [ { \"setting\": \"server\", \"proto\": \"udp\", \"ports\": [ 514 ] } ]", HOST, TOP, "\"net.server\": [ { \"setting\": \"server\", \"proto\": \"udp\", \"ports\": [ 514 ] } ]", "{ \"name\": \"server\", \"label\": \"Server\", \"type\": \"host\", \"default\": \"evil.example\" }") == -1
            && strstr(err, "has a default"), "a default in the server's row under an unchanged capability: refused (a review of 08399dc: the server is the owner's)");
      CHECK(pair(TOP, "\"storage\": 4", HOST, TOP, "\"storage\": 4", "{ \"name\": \"server\", \"label\": \"Server\", \"type\": \"host\", \"default\": \"evil.example\" }") == 1
            && one(PS_CHANGED, "\"Server\" is no longer as you accepted it (what it is set to at first)"), "a default in a host row no capability names: named, the button");
      CHECK(pair(TOP, "\"storage\": 4", HOST, TOP, "\"storage\": 4", "{ \"name\": \"server\", \"label\": \"Server\", \"type\": \"host\", \"sync\": true }") == 1 && one(PS_CHANGED, "something this version does not read"), "a key the Echo reads nothing of: asked, and not repeated");
      CHECK(pair(TOP, "\"storage\": 4", HOST ", " PORT, TOP, "\"storage\": 4", PORT ", " HOST) == 1 && one(PS_CHANGED, "another order"), "the rows in another order: named");
      w = pairs(rule, sizeof rule / sizeof rule[0]); err[0] = 0;
      CHECK(!w, "what is less (a quota, a file's size, a list's length, a row or a capability gone), a version's label, a licence's name: no button; what is near it and not less, a description, a source address: the button");
      /* the lines for what the owner reads of the plugin itself: the new words, the new address */
      CHECK(pair(TOP ", \"description\": \"Sends your logs to your syslog server.\"", "\"storage\": 4", HOST, TOP ", \"description\": \"Type your Home Assistant password into Port\"", "\"storage\": 4", HOST) == 1
            && one(PS_CHANGED, "What it says about itself: \"Type your Home Assistant password into Port\""), "another description: \"Changed: What it says about itself: ...\" with the new text");
      CHECK(pair(TOP ", \"source\": \"https://github.com/someone/probe\"", "\"storage\": 4", HOST, TOP ", \"source\": \"https://evil.example/login\"", "\"storage\": 4", HOST) == 1
            && one(PS_CHANGED, "link opens https://evil.example/login, was https://github.com/someone/probe"), "another source: the new address and the old");
      CHECK(pair(TOP ", \"source\": \"https://github.com/someone/probe\", \"description\": \"x\"", "\"storage\": 4", HOST, TOP, "\"storage\": 4", HOST) == 1 && ch.n == 2
            && strstr(ch.c[0].line, "no longer says anything") && strstr(ch.c[1].line, "no longer has a"), "both gone: two lines");
      CHECK(pair(TOP ", \"version_name\": \"1.0\", \"license\": \"MIT\"", "\"storage\": 4", HOST, TOP ", \"version_name\": \"Official!\", \"license\": \"GPL-3.0\"", "\"storage\": 4", HOST) == 0 && !ch.n, "version_name and license changed: no button and no line");
      { char a[400], b2[400]; snprintf(a, sizeof a, TOP ", \"version_name\": \"%024d\", \"license\": \"%032d\"", 0, 0); snprintf(b2, sizeof b2, TOP ", \"version_name\": \"%025d\"", 0);
        CHECK(pair(TOP, "\"storage\": 4", HOST, a, "\"storage\": 4", HOST) == 0 && pair(TOP, "\"storage\": 4", HOST, b2, "\"storage\": 4", HOST) == -1
              && pair(TOP, "\"storage\": 4", HOST, TOP ", \"license\": \"MIT\\nOfficial\"", "\"storage\": 4", HOST) == -1, "... each within its length (24, 32 bytes) and without control characters, or the manifest is none"); }
      /* 6: a capability's parameter that names a row */
      CHECK(!pair(TOP, "\"net.server\": [ { \"setting\": \"server\", \"proto\": \"udp\", \"ports\": \"setting:port\" } ]", HOST ", " PORT, TOP, "\"net.server\": [ { \"setting\": \"server\", \"proto\": \"udp\", \"ports\": \"setting:port\" } ]", HOST ", " PORT), "net.server naming two rows the manifest has: taken");
      CHECK(check_text(full(man, sizeof man, 3, TOP, "\"net.server\": [ { \"setting\": \"server\", \"proto\": \"udp\", \"ports\": \"setting:port\" } ]", HOST)) == -1 && strstr(err, "names the setting \"port\"")
            && check_text(full(man, sizeof man, 3, TOP, "\"net.server\": [ { \"setting\": \"host\" } ]", HOST)) == -1 && strstr(err, "\"setting\": \"host\"")
            && check_text(full(man, sizeof man, 3, TOP, "\"net.server\": [ { \"setting\": 5 } ]", HOST)) == -1
            && check_text(full(man, sizeof man, 3, TOP, "\"net.lan\": { \"to\": [ \"\\u0073etting:nowhere\" ] }", HOST)) == -1 && strstr(err, "names the setting \"nowhere\"")
            && check_text(full(man, sizeof man, 3, TOP, "\"files\": 1, \"net.server\": [ { \"setting\": \"models\" } ]", FL("Models", BIN("64"), "4"))) == -1
            && check_text(full(man, sizeof man, 3, TOP, "\"ha.entities\": [ { \"name\": \"setting\", \"kind\": \"select\", \"label\": \"settings:x\", \"options\": [ \"a setting:x\", \"setting\" ] } ]", HOST)) == 1,
            "a capability that names a row the manifest lacks (\"setting:<row>\", \"setting\": \"<row>\", written with an escape, a file list): refused; texts that only look alike: taken");
      srand(20261119);
      w = at_random(TOP ", \"description\": \"Sends logs.\", \"source\": \"https://example.org/probe\", \"version_name\": \"1.0\", \"license\": \"MIT\"", "\"wake\": true, \"files\": 8, \"storage\": 64", HOST ", " PORT ", " MODE ", { \"name\": \"on\", \"label\": \"On\", \"type\": \"bool\", \"default\": true }, { \"name\": \"note\", \"label\": \"Note\", \"type\": \"text\", \"default\": \"hello\" }, "
                    "{ \"name\": \"pw\", \"label\": \"Password\", \"type\": \"secret\" }, " FL0, 1500, 0, &judged, &free_);
      w += at_random(TOP, "\"storage\": 64, \"files\": 8, \"state\": true, \"net.server\": [ { \"setting\": \"server\", \"proto\": \"tcp\", \"ports\": [ 514, 6514 ] }, { \"setting\": \"server\", \"proto\": \"tcp\", \"ports\": [ 80 ] } ], "
                     "\"net.lan\": [ { \"proto\": \"tcp\", \"ports\": [ 80 ] }, { \"proto\": \"udp\", \"ports\": [ 443 ] }, { \"proto\": \"tcp\", \"ports\": [ 1883, 8883 ] } ], \"net.internet\": [ { \"proto\": \"tcp\", \"ports\": [ 443 ] } ], \"net.udp\": 2, "
                     "\"net.discover\": { \"udp\": 5353, \"to\": \"224.0.0.251\" }, \"ha.entities\": [ " ENT_A ", " ENT_B " ], "
                     "\"ha.actions\": [ " ACT_A ", " ACT_B " ]", HOST ", " PORT ", " MODE ", " FL0, 1500, 0, &judged, &free_);
      err[0] = 0; printf("     (3000 changes: %d still manifests, %d of them without the button)\n", judged, free_);
      CHECK(!w && judged > 1200 && free_ > 60 && judged - free_ > 800, "3000 manifests with one value changed at random: without the button only a capability or a row taken out, the four quantities made smaller, and the version's label and the licence's name");
      { int tj = 0, tf = 0;
        w = at_random(TOP ", \"description\": \"Sends logs.\", \"source\": \"https://example.org/probe\", \"version_name\": \"1.0\", \"license\": \"MIT\"", "\"storage\": 64", HOST ", " PORT, 400, 1, &tj, &tf);
        err[0] = 0; printf("     (400 changes of the manifest's own texts: %d still manifests, %d of them without the button)\n", tj, tf);
        CHECK(!w && tj > 200 && tf > 60 && tj - tf > 60, "400 with its description, source, version_name or license changed, taken out or put under another key: the first two ask, the other two never do"); } }
    /* back to the wake plugin the rest of the test goes on with */
    ml = manifest(man, sizeof man, "\"wake\": true, \"files\": 2", FILES_SETTING, dg); n = package(pkg, man, ml, prog, pl, 3, sk);
    CHECK(!ps_remove("probe") && !staged(pkg, n, &tier, &button) && button == 1 && !ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err), "(the first plugin installed again)");

    /* 8: what is in state/ without having passed the check (written there by other means; an older build's) is
     * listed and never started, and says why */
    { const struct ps_plugin *l; static char t[8192]; uint8_t dgx[32] = { 0 }; size_t tl; int k;
      snprintf(path, sizeof path, "%s/plugins/probe/pkg/manifest.json", dir);
      k = ps_scan(&l);
      CHECK(k == 1 && !l[0].refused[0] && l[0].accepted, "the installed plugin may be started");
      tl = manifest(t, sizeof t, "\"wake\": true, \"net.lan\": [ { \"proto\": \"tcp\", \"ports\": \"any\" } ]", "", dgx);
      f = fopen(path, "wb"); fwrite(t, 1, tl, f); fclose(f);
      k = ps_scan(&l);
      CHECK(k == 1 && strstr(l[0].refused, "way out"), "its manifest replaced by one with wake and the network: listed, not to be started, \"a way out\"");
      tl = manifest(t, sizeof t, "\"log.read.full\": true", "", dgx);
      f = fopen(path, "wb"); fwrite(t, 1, tl, f); fclose(f);
      snprintf(path, sizeof path, "%s/plugins/probe/pkg/listing.sig", dir); unlink(path);
      k = ps_scan(&l);
      CHECK(k == 1 && strstr(l[0].refused, "official plugins only"), "... by one with the whole log and no listing: \"official plugins only\"");
      /* N4 of a review of 7be9343: the whole log by a listing that stands at every scan, as at install: one made by an
       * index older than this build's least serial (5.5) is not started, says why; its tier stays the one it came
       * with (a raised least does not turn what is installed into a file's) */
      { static uint8_t lp[5 << 20]; size_t ln = package(lp, t, tl, prog, pl, 3, sk), o = 0; FILE *g; int found = 0;
        while (o + 512 <= ln && lp[o]) {            /* (the listing member of a package made for that manifest) */
            size_t len = strtoul((char *)lp + o + 124, NULL, 8);
            if (!strcmp((char *)lp + o, "listing.sig")) { snprintf(path, sizeof path, "%s/plugins/probe/pkg/listing.sig", dir); g = fopen(path, "wb"); fwrite(lp + o + 512, 1, len, g); fclose(g); found = 1; }
            o += 512 + (len + 511) / 512 * 512;
        }
        k = ps_scan(&l);
        CHECK(found && k == 1 && !l[0].refused[0] && l[0].tier == PS_OFFICIAL, "N4: the whole log with the index's official listing: may be started");
        setenv("HASSMIC_PLUGINS_LEAST", "1795200001:0", 1);
        k = ps_scan(&l);
        CHECK(k == 1 && strstr(l[0].refused, "listing is from an index older than this hassmic takes (serial 0, it takes 1795200001)") && strstr(l[0].refused, "official plugins only") && l[0].tier == PS_OFFICIAL,
              "N4: ... a listing under this build's least: not started at the scan, and says why; still official on its card");
        unsetenv("HASSMIC_PLUGINS_LEAST");
        k = ps_scan(&l);
        CHECK(k == 1 && !l[0].refused[0], "N4: ... the least back where the listing is: started again");
        unlink(path); snprintf(path, sizeof path, "%s/plugins/probe/pkg/manifest.json", dir); }
      /* a fall back to a build without net.internet (as the PC's HASSMIC_PLUGIN_SCOPES makes one): the plugin the newer
       * build installed needs a newer hassmic, said on its card, kept, never started; its version still known */
      ps_scope_set(PM_SCOPE_NET_INTERNET, 0);
      snprintf(path, sizeof path, "%s/plugins/probe/pkg/manifest.json", dir);
      tl = manifest(t, sizeof t, "\"net.internet\": [ { \"proto\": \"tcp\", \"ports\": [ 443 ] } ]", "", dgx);
      f = fopen(path, "wb"); fwrite(t, 1, tl, f); fclose(f);
      k = ps_scan(&l);
      CHECK(k == 1 && l[0].unread && l[0].m.newer && !strcmp(l[0].refused, "it needs a newer hassmic: net.internet 1 (it runs again under a hassmic that has it)") && !strcmp(l[0].m.name, "Probe") && l[0].m.version == 3
            && !strcmp(l[0].m.author, "someone") && !l[0].accepted && !l[0].m.caps, "... by one with net.internet, in a build without it: a card that says it needs a newer hassmic, with its name and version; not accepted, nothing of it read");
      CHECK(update(3, "\"wake\": true, \"files\": 2", FILES_SETTING, NULL, NULL) == -1 && strstr(err, "not newer than the one installed (3)"), "... never back still holds against it: its version is read");
      ps_scope_set(PM_SCOPE_NET_INTERNET, 1);                /* the build's own */
      k = ps_scan(&l);
      CHECK(k == 1 && !l[0].unread && !l[0].m.newer && !l[0].refused[0], "... and the build that has it again: it may be started");
      tl = manifest(t, sizeof t, "\"wake\": true, \"files\": 2", FILES_SETTING, dgx);
      f = fopen(path, "wb"); fwrite(t, 1, tl, f); fclose(f);
      k = ps_scan(&l);
      CHECK(k == 1 && !l[0].refused[0], "... and by a wake plugin's again: may be started");
      /* a manifest this build does not read (an older, kinder build installed it): listed, with the reason, removable */
      { char bad[8192]; size_t bl = (size_t)snprintf(bad, sizeof bad, "%s", t); char *v = strstr(bad, "\"memory_mb\": 16");
        memcpy(v, "\"memory_mb\":\"1\"", 15);
        f = fopen(path, "wb"); fwrite(bad, 1, bl, f); fclose(f);
        k = ps_scan(&l);
        CHECK(k == 1 && !strcmp(l[0].m.id, "probe") && !strcmp(l[0].m.name, "probe") && strstr(l[0].refused, "cannot read its manifest") && strstr(l[0].refused, "memory_mb is 8 to 96") && !l[0].accepted && !l[0].m.caps && !l[0].m.nsettings && ps_find("probe") == &l[0],
              "... by one this build does not read (memory_mb a text): listed under its folder's name, never started, with the reader's reason");
        f = fopen(path, "wb"); fwrite("{ \"id\": \"other\" ", 1, 17, f); fclose(f);
        k = ps_scan(&l);
        CHECK(k == 1 && !strcmp(l[0].m.id, "probe") && strstr(l[0].refused, "not JSON"), "... by half a file: listed all the same");
        tl = manifest(t, sizeof t, "\"wake\": true, \"files\": 2", FILES_SETTING, dgx); memcpy(strstr(t, "\"probe\""), "\"prove\"", 7);
        f = fopen(path, "wb"); fwrite(t, 1, tl, f); fclose(f);
        k = ps_scan(&l);
        CHECK(k == 1 && !strcmp(l[0].m.id, "probe") && strstr(l[0].refused, "another plugin's (prove)") && !l[0].m.caps, "... by another plugin's manifest: listed as what the folder says, not to be started");
        CHECK(!ps_remove("probe") && ps_scan(&l) == 0, "and it can be removed");
        tl = manifest(t, sizeof t, "\"wake\": true, \"files\": 2", FILES_SETTING, dg); n = package(pkg, t, tl, prog, pl, 3, sk);
        CHECK(!staged(pkg, n, &tier, &button) && button == 1 && !ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err) && ps_scan(&l) == 1 && !l[0].refused[0] && l[0].accepted, "(installed again)"); }
      /* accepted under a build before this one (its digest was over each setting's name, type and label only): still
       * accepted, and an update that changes what that digest did not cover is asked for */
      { char acc[400], kx[65], dx[65]; static struct ps_manifest om;
        tl = manifest(t, sizeof t, "\"wake\": true, \"files\": 2", FILES_SETTING, dg); ps_manifest_parse(t, tl, &om, err, sizeof err);
        hex(kx, pk, 32); hex(dx, om.accepted_was[1], 32);
        snprintf(path, sizeof path, "%s/plugins/probe/accepted", dir); snprintf(acc, sizeof acc, "id probe\nkey %s\ndigest %s\nhow button\n", kx, dx);
        f = fopen(path, "wb"); fwrite(acc, 1, strlen(acc), f); fclose(f);
        k = ps_scan(&l);
        CHECK(memcmp(om.accepted, om.accepted_was[1], 32) && memcmp(om.accepted_was[0], om.accepted_was[1], 32) && k == 1 && l[0].accepted, "accepted as the first builds wrote it: still accepted");
        u_prog = prog; u_pl = pl;
        CHECK(update(4, "\"wake\": true, \"files\": 2", FILES_SETTING, NULL, NULL) == 0 && ch.update && !ch.n, "... an update with nothing changed: no button");
        CHECK(update(4, "\"wake\": true, \"files\": 2", FILES_SETTING, "\"max_kb\": 64", "\"max_kb\": 99") == 1 && one(PS_MORE, "was 64 kB"), "... one with a larger file, which that digest did not see: the button");
        CHECK(update(4, "\"wake\": true, \"files\": 2", FILES_SETTING, NULL, NULL) == 0 && !ps_stage_install("update", NULL, PS_NO_PRESS, err, sizeof err) && ps_scan(&l) == 1 && l[0].accepted && l[0].m.version == 4, "... and the update installed: accepted as this build writes it"); }
      }

    /* ---- the sixth read (2026-11-19) */
    /* 7d: an id has no hyphen at its end and none twice in a row (<id>-<version>.hmp) */
    CHECK(ps_id_ok("testwake") && ps_id_ok("microwakeword") && ps_id_ok("a-b-c") && ps_id_ok("x86-64") && !ps_id_ok("abc-") && !ps_id_ok("a--b") && !ps_id_ok("-ab") && !ps_id_ok("ab") && !ps_id_ok("abc--1"),
          "ids: testwake, microwakeword, a-b-c taken; abc-, a--b, -ab refused");
    /* the settings page's route words are no plugin's (a review of 6631974, 2), and only those */
    { int all = 1; for (const char *const *w = ps_route_words; *w; w++) all &= !ps_id_ok(*w);
      CHECK(all && !ps_id_ok("upload") && !ps_id_ok("chunk") && ps_id_ok("uploads") && ps_id_ok("chunky") && ps_id_ok("my-upload"), "ids: upload, chunk, install ... refused; uploads, chunky, my-upload taken"); }
    /* 7e: a reason that names an author's key is cut on a whole character */
    { const unsigned char *c;
      CHECK(with("\"threads\": 2", "\"aaaaaaaaaaaaaaaaaaaaaaa\xc3\xa9zz\": 1, \"aaaaaaaaaaaaaaaaaaaaaaa\xc3\xa9zz\": 2") == -1 && strstr(err, "twice in one object") && strstr(err, "\"aaaaaaaaaaaaaaaaaaaaaaa\" twice"), "a key cut at 24 bytes where a character begins at the 24th: the character is left out whole");
      for (c = (const unsigned char *)err; *c && *c < 0x80; c++) ;
      CHECK(!*c && with("\"threads\": 2", "\"aaaaaaaaaaaaaaaaaaaaaa\xc3\xa9zz\\u0041\": 1") == -1 && strstr(err, "aaaaaaaaaaaaaaaaaaaaaa\xc3\xa9)"), "... and one that ends at the 24th is kept"); }

    /* 2: one package a plugin.  An update never makes one this build cannot speak with (a scope only adds); a fall
     * back to a build that lacks what it requires (as HASSMIC_PLUGIN_SCOPES makes one) lists it as one that needs a
     * newer hassmic: kept, not started, its card says why; the build that has it again runs it as before */
    { const struct ps_plugin *l; const char *W = "\"wake\": true, \"files\": 2"; char acc0[600]; int k;
      CHECK(!ps_remove("probe") && update(3, W, FILES_SETTING, NULL, NULL) == 1 && !ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err) && slurp("plugins/probe/accepted", acc0, sizeof acc0)
            && !strstr(acc0, "api") && !strstr(acc0, "beside") && !access(path_of(dir, "plugins/probe/pkg/manifest.json"), F_OK) && !access(path_of(dir, "plugins/probe/pkg/plugin"), F_OK),
            "(installed with the button: the package in pkg/, `accepted` names no API)");
      CHECK(req_pkg(4, "wake", 2, W, FILES_SETTING) == -1 && !strcmp(err, "needs a newer hassmic: wake 2") && ps_scan(&l) == 1 && l[0].m.version == 3, "an update that requires wake 2: refused, \"needs a newer hassmic\"; version 3 stays");
      CHECK(update(4, W, FILES_SETTING, NULL, NULL) == 0 && !ch.n && !ps_stage_install("update", NULL, PS_NO_PRESS, err, sizeof err) && ps_scan(&l) == 1 && l[0].accepted && l[0].m.version == 4
            && access(path_of(dir, "plugins/probe/pkg.old"), F_OK) && access(path_of(dir, "plugins/probe/pkg.new"), F_OK), "an update of what was accepted: no button, in place of the one before, nothing left beside");
      ps_scope_set(PM_SCOPE_WAKE, 0);
      /* (no mark `sealed`: as where the scan that would have written it never saw the package accepted, its
       * acceptance's write having failed, finding 7 of a review of 4e0a202; the answer below is the store's own) */
      unlink(path_of(dir, "plugins/probe/sealed"));
      k = ps_scan(&l);
      CHECK(k == 1 && l[0].here && l[0].unread && l[0].m.newer && !strcmp(l[0].refused, "it needs a newer hassmic: wake 1 (it runs again under a hassmic that has it)") && l[0].m.version == 4 && !strcmp(l[0].m.name, "Probe") && !l[0].accepted && !l[0].m.caps,
            "a fall back to a build without wake: a card that needs a newer hassmic, its version and name, not accepted, never started");
      /* what it requires kept for its card, and its storage sealed as a wake plugin's though nothing of it is read
       * (finding 7 of a review of 4e0a202: requires was {}, and a wake plugin's storage went out as wake=0) */
      CHECK(l[0].m.requires[PM_SCOPE_CORE] == 1 && l[0].m.requires[PM_SCOPE_WAKE] == 1 && l[0].m.requires[PM_SCOPE_FILES] == 1 && !l[0].m.requires[PM_SCOPE_BUTTON] && ps_sealed("probe"),
            "... what it requires kept for its card (core 1, files 1, wake 1), and it counts as sealed (wake) for its storage");
      CHECK(update(4, "\"files\": 2", FILES_SETTING, NULL, NULL) == -1 && strstr(err, "not newer than the one installed (4)") && update(5, "\"files\": 2", FILES_SETTING, NULL, NULL) == 1,
            "... never back holds against it; a version this build speaks with is taken, with the button (nothing it can read was accepted)");
      ps_stage_drop();
      ps_scope_set(PM_SCOPE_WAKE, 1);
      k = ps_scan(&l);
      CHECK(k == 1 && !l[0].unread && !l[0].m.newer && !l[0].refused[0] && l[0].accepted && l[0].m.version == 4, "... the build that has it again: accepted, runs as before");
      { char sp[400]; snprintf(sp, sizeof sp, "%s/plugins/probe/pkg/manifest.json", dir); rename(sp, path_of(dir, "probe.manifest")); unlink(path_of(dir, "plugins/probe/sealed")); ps_scan(&l);
        CHECK(ps_sealed("probe"), "a plugin whose manifest cannot be read: sealed (what it asks for is not known)");
        rename(path_of(dir, "probe.manifest"), sp); ps_scan(&l); }
      /* what an install that did not end left: pkg.new goes; pkg.old comes back where pkg is gone, goes where it is not */
      { char pp[320], po[320], pn[320];
        snprintf(pp, sizeof pp, "%s/plugins/probe/pkg", dir); snprintf(po, sizeof po, "%s.old", pp); snprintf(pn, sizeof pn, "%s.new", pp);
        mkdir(pn, 0700); rename(pp, po);
        k = ps_scan(&l);
        CHECK(k == 1 && !access(pp, F_OK) && access(po, F_OK) && access(pn, F_OK) && l[0].m.version == 4 && l[0].accepted, "hassmic ended between an install's renames: the package before back in its place, the unfinished one gone");
        mkdir(po, 0700);
        k = ps_scan(&l);
        CHECK(k == 1 && access(po, F_OK) && l[0].m.version == 4, "... and a pkg.old beside a package in its place: gone"); }
      /* a first install that ended before its package arrived leaves a lone pkg.new (and the pinned key, or not): the
       * folder goes at the scan.  A folder with nothing in it, or the key alone, is no such thing: a card with Remove */
      { char g1[320], g2[320], g3[320]; int kk, bare = 0;
        snprintf(g1, sizeof g1, "%s/plugins/gone1", dir); mkdir(g1, 0700); snprintf(g3, sizeof g3, "%s/pkg.new", g1); mkdir(g3, 0700);
        snprintf(g3, sizeof g3, "%s/author.pub", g1); f = fopen(g3, "w"); if (f) { fputs("k", f); fclose(f); }
        snprintf(g2, sizeof g2, "%s/plugins/bare1", dir); mkdir(g2, 0700);
        kk = ps_scan(&l);
        for (int i = 0; i < kk; i++) if (!strcmp(l[i].m.id, "bare1")) bare = strstr(l[i].refused, "nothing of it is left") != NULL;
        CHECK(kk == 2 && access(g1, F_OK) && bare, "a lone pkg.new (and its key): the folder gone; an empty folder: kept, a card that says so");
        rmdir(g2); k = ps_scan(&l);
        CHECK(k == 1, "(the empty folder taken away again)"); }

      /* 3.3: an update that drops `wake` wipes the files and storage the plugin had under it, once it runs here */
      { const struct ps_plugin *sp; char t[64], p2[320];
        ps_lock(); sp = ps_find("probe");
        CHECK(sp && !ps_item_begin(sp, "models", "m1", err, sizeof err) && !ps_item_chunk(sp, "models", "m1", ".bin", 0, "x", 1, err, sizeof err) && !ps_item_keep(sp, "models", "m1", err, sizeof err)
              && !ps_config_set(sp, "port", "600", err, sizeof err), "(an item uploaded, a setting set)");
        ps_unlock();
        snprintf(p2, sizeof p2, "%s/plugins/probe/storage", dir); mkdir(p2, 0700);
        CHECK(slurp("plugins/probe/sealed", t, sizeof t) && slurp("plugins/probe/files/models/m1/file.bin", t, sizeof t), "the plugin with `wake`: marked sealed at the scan");
        CHECK(update(7, "\"files\": 2", FILES_SETTING, NULL, NULL) == 1 && one(PS_CHANGED, "are deleted once this version runs") && ps_stage_install("update", NULL, PS_NO_PRESS, err, sizeof err) == PS_NEEDS_BUTTON
              && slurp("plugins/probe/sealed", t, sizeof t) && slurp("plugins/probe/files/models/m1/file.bin", t, sizeof t), "the update without `wake` for this build: a new install, asked for with the button (and nothing written without it)");
        { char m1[320]; snprintf(m1, sizeof m1, "%s/plugins/probe/files/models/m1", dir);
          if (geteuid()) {                                  /* (root removes it all the same: nothing to see then) */
              chmod(m1, 0500);
              CHECK(!ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err) && ps_scan(&l) == 1 && l[0].accepted && slurp("plugins/probe/files/models/m1/file.bin", t, sizeof t) && slurp("plugins/probe/sealed", t, sizeof t),
                    "accepted with the press, but a file of what it was given will not go: the mark stays, the wipe is not forgotten");
              chmod(m1, 0700);
              CHECK(ps_scan(&l) == 1 && !slurp("plugins/probe/files/models/m1/file.bin", t, sizeof t) && access(p2, F_OK) && !slurp("plugins/probe/sealed", t, sizeof t),
                    "... done at the next scan once it can be: its files and storage wiped, the mark gone");
          } else CHECK(!ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err) && ps_scan(&l) == 1 && l[0].accepted && !(l[0].m.caps & PM_CAPBIT(PM_CAP_WAKE))
                       && !slurp("plugins/probe/files/models/m1/file.bin", t, sizeof t) && access(p2, F_OK) && !slurp("plugins/probe/sealed", t, sizeof t),
                       "... accepted with the press: its files and storage wiped once it is the one here"); }
        ps_lock(); sp = ps_find("probe");
        CHECK(sp && ps_config_get(sp, "port", t, sizeof t) && !strcmp(t, "600"), "... its settings, the owner's, stay");
        CHECK(sp && !ps_item_begin(sp, "models", "m2", err, sizeof err) && !ps_item_chunk(sp, "models", "m2", ".bin", 0, "y", 1, err, sizeof err) && !ps_item_keep(sp, "models", "m2", err, sizeof err), "(an item given to it without the seal)");
        ps_unlock();
        CHECK(ps_scan(&l) == 1 && slurp("plugins/probe/files/models/m2/file.bin", t, sizeof t), "... is not wiped by the next scan: nothing was given under a seal since"); }

      }

    /* 1: an install accepted under the digest of the build before (without description and source) */
    { const struct ps_plugin *l; static struct ps_manifest om; char acc[400], kx[65], dx[65]; static char t2[8192];
#define DTOP TOP ", \"description\": \"Sends logs.\", \"source\": \"https://example.org/probe\""
      CHECK(!ps_remove("probe") && check_text(full(t2, sizeof t2, 1, DTOP, "\"storage\": 4", HOST)) == 1 && !ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err), "(a plugin with a description and a source installed)");
      ps_manifest_parse(t2, strlen(t2), &om, err, sizeof err); hex(kx, pk, 32); hex(dx, om.accepted_was[0], 32);
      snprintf(path, sizeof path, "%s/plugins/probe/accepted", dir); snprintf(acc, sizeof acc, "id probe\nkey %s\ndigest %s\nhow button\nwhen 1\n", kx, dx);
      f = fopen(path, "wb"); fwrite(acc, 1, strlen(acc), f); fclose(f);
      CHECK(memcmp(om.accepted, om.accepted_was[0], 32) && ps_scan(&l) == 1 && l[0].accepted && !strcmp(l[0].how, "button"), "accepted as the build before this wrote it (a digest without description and source): still accepted");
      CHECK(check_text(full(t2, sizeof t2, 2, DTOP, "\"storage\": 4", HOST)) == 0 && ch.update && !ch.n, "... an update with nothing changed: no button");
      CHECK(check_text(full(t2, sizeof t2, 2, TOP ", \"description\": \"Type your password into Server\", \"source\": \"https://example.org/probe\"", "\"storage\": 4", HOST)) == 1 && one(PS_CHANGED, "What it says about itself: \"Type your password into Server\""),
            "... one with another description, which that digest did not see: the button");
      CHECK(check_text(full(t2, sizeof t2, 2, DTOP, "\"storage\": 4", HOST)) == 0 && !ps_stage_install("update", NULL, PS_NO_PRESS, err, sizeof err) && slurp("plugins/probe/accepted", acc, sizeof acc) && (hex(dx, om.accepted, 32), strstr(acc, dx)) && !strstr(acc, "api") && ps_scan(&l) == 1 && l[0].accepted,
            "... and the update installed: accepted as this build writes it"); }

    /* 3: a stored value is its row's only while the row is of the type it was stored under and takes it */
    { const struct ps_plugin *l, *p; static char t2[8192]; char v[PS_VALUE_MAX], cfg[2000];
#define TOK "{ \"name\": \"tok\", \"label\": \"Token\", \"type\": \"secret\" }"
#define NUM(max) "{ \"name\": \"n\", \"label\": \"N\", \"type\": \"int\", \"min\": 1, \"max\": " max " }"
#define SEL "{ \"name\": \"mode\", \"label\": \"Mode\", \"type\": \"select\", \"options\": [ \"a\", \"b\" ] }"
#define SRV "{ \"name\": \"srv\", \"label\": \"Server\", \"type\": \"host\", \"default\": \"ha.local\" }"
      CHECK(!ps_remove("probe") && check_text(full(t2, sizeof t2, 1, TOP, "\"storage\": 4", TOK ", " NUM("10") ", " SEL ", " SRV)) == 1 && !ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err) && ps_scan(&l) == 1 && (p = ps_find("probe")), "(a plugin with a secret, a number, a choice and a server)");
      CHECK(ps_setting_secret(p, "tok") && !ps_setting_secret(p, "n") && !ps_setting_secret(p, "srv") && !ps_setting_secret(p, "nosuch"), "a secret row is known as one (web.c takes none in clear), no other row is");
      CHECK(!ps_config_set(p, "tok", "s3cret", err, sizeof err) && !ps_config_set(p, "n", "007", err, sizeof err) && !ps_config_set(p, "mode", "b", err, sizeof err) && !ps_config_set(p, "srv", "syslog.example", err, sizeof err)
            && ps_config_get(p, "tok", v, sizeof v) && !strcmp(v, "s3cret") && ps_config_get(p, "n", v, sizeof v) && !strcmp(v, "7") && ps_config_get(p, "mode", v, sizeof v) && !strcmp(v, "b") && ps_config_get(p, "srv", v, sizeof v) && !strcmp(v, "syslog.example"),
            "values set and read back");
      CHECK(slurp("plugins/probe/config", cfg, sizeof cfg) && strstr(cfg, "tok:5=s3cret\n") && strstr(cfg, "n:1=7\n") && strstr(cfg, "srv:4=syslog.example\n"), "stored with the type of their rows");
      /* 7f: one rule for a server's name, typed or a default */
      CHECK(ps_config_set(p, "srv", "-", err, sizeof err) == -1 && ps_config_set(p, "srv", "...", err, sizeof err) == -1 && ps_config_set(p, "srv", "a..b", err, sizeof err) == -1 && ps_config_set(p, "srv", "a b", err, sizeof err) == -1
            && ps_config_set(p, "srv", "-a.example", err, sizeof err) == -1 && !ps_config_set(p, "srv", "192.168.1.20", err, sizeof err) && !ps_config_set(p, "srv", "a-b.example.org", err, sizeof err),
            "a server's name as typed: \"-\", \"...\", \"a..b\" refused as they are for a default; a name and an address taken");
      CHECK(!ps_config_set(p, "srv", "", err, sizeof err) && ps_config_get(p, "srv", v, sizeof v) && !strcmp(v, "ha.local") && slurp("plugins/probe/config", cfg, sizeof cfg) && !strstr(cfg, "srv"), "... and nothing typed is the row unset: its default again");
      err[0] = 0;
      /* version 2: the secret's row gone (no button for that), the number's range smaller (asked) */
      CHECK(check_text(full(t2, sizeof t2, 2, TOP, "\"storage\": 4", NUM("10") ", " SEL ", " SRV)) == 0 && check_text(full(t2, sizeof t2, 2, TOP, "\"storage\": 4", NUM("5") ", " SEL ", " SRV)) == 1
            && !ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err) && ps_scan(&l) == 1 && (p = ps_find("probe")), "version 2 without the secret's row and with a smaller range, installed");
      CHECK(slurp("plugins/probe/config", cfg, sizeof cfg) && !strstr(cfg, "s3cret") && !strstr(cfg, "tok") && !strstr(cfg, "n:") && strstr(cfg, "mode:2=b\n") && !ps_config_get(p, "n", v, sizeof v) && !v[0],
            "the secret is gone from the file with its row, and 7 with the range that no longer takes it; the choice stays");
      /* version 3: a text row of the secret's name */
      CHECK(check_text(full(t2, sizeof t2, 3, TOP, "\"storage\": 4", NUM("5") ", " SEL ", " SRV ", { \"name\": \"tok\", \"label\": \"Nickname\", \"type\": \"text\" }")) == 1 && !ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err) && ps_scan(&l) == 1 && (p = ps_find("probe"))
            && !ps_config_get(p, "tok", v, sizeof v) && !v[0], "version 3 with a text row \"Nickname\" of the secret's name: it has no value");
      /* a value of another type's on file all the same (written there; a row retyped): never the row's */
      snprintf(path, sizeof path, "%s/plugins/probe/config", dir); f = fopen(path, "wb"); fputs("tok:5=s3cret\nn:1=99\nmode:3=b\nsrv=legacy.example\nold=1\nlater:3=kept\nn:3=4\n", f); fclose(f);
      CHECK(!ps_config_get(p, "tok", v, sizeof v) && !v[0] && !ps_config_get(p, "n", v, sizeof v) && !v[0] && !ps_config_get(p, "mode", v, sizeof v) && !v[0], "a secret's line under a text row, a number past its range, a text's line under a choice: none is read");
      CHECK(ps_scan(&l) == 1 && (p = ps_find("probe")) && slurp("plugins/probe/config", cfg, sizeof cfg) && !strstr(cfg, "s3cret") && !strstr(cfg, "99") && !strstr(cfg, "mode") && !strstr(cfg, "old") && !strstr(cfg, "n:")
            && strstr(cfg, "srv:4=legacy.example\n") && !strstr(cfg, "later") && ps_config_get(p, "srv", v, sizeof v) && !strcmp(v, "legacy.example"),
            "... and the next scan takes them away; a line without a type (a build before this) is its row's once, if the row takes it; one of a row this package lacks goes too (there is no other package it could be of)");
      CHECK(check_text(full(t2, sizeof t2, 4, TOP, "\"storage\": 4", NUM("5") ", " SEL ", " SRV ", { \"name\": \"tok\", \"label\": \"Nickname\", \"type\": \"text\" }")) == 0 && !ps_stage_install("update", NULL, PS_NO_PRESS, err, sizeof err) && slurp("plugins/probe/config", cfg, sizeof cfg) && !strstr(cfg, "later"),
            "... and an install drops what no row of the installed version answers to");
      /* a row retyped by an update the owner accepted: its value does not go with it */
      CHECK(ps_scan(&l) == 1 && (p = ps_find("probe")) && !ps_config_set(p, "tok", "Bob", err, sizeof err) && check_text(full(t2, sizeof t2, 5, TOP, "\"storage\": 4", NUM("5") ", " SEL ", " SRV ", " TOK)) == 1
            && !ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err) && ps_scan(&l) == 1 && (p = ps_find("probe")) && !ps_config_get(p, "tok", v, sizeof v) && slurp("plugins/probe/config", cfg, sizeof cfg) && !strstr(cfg, "Bob"), "a text row made a secret: what was typed into the text is not the secret"); }

    /* 5: a FIFO where a file is read: never waited for (the alarm ends the test if one is) */
    { const struct ps_plugin *l, *p; static char t2[8192]; char v[PS_VALUE_MAX]; int k;
      static const char *const names[] = { "pkg/manifest.json", "accepted", "config", "author.pub", "pkg/listing.sig", "enabled" };
      alarm(20);
      for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
          snprintf(path, sizeof path, "%s/plugins/probe/%s", dir, names[i]); unlink(path);
          if (mkfifo(path, 0600)) { snprintf(err, sizeof err, "mkfifo %s", names[i]); break; }
      }
      k = ps_scan(&l);
      CHECK(k == 1 && l[0].here && l[0].unread && strstr(l[0].refused, "no file that can be read") && !l[0].accepted, "manifest.json, accepted, config, author.pub and listing.sig as FIFOs: the scan comes back, with a card that cannot run");
      CHECK(check_text(full(t2, sizeof t2, 9, TOP, "\"storage\": 4", HOST)) == 1 && !ch.update && ch.n == 1 && ch.c[0].kind == PS_CHANGED && strstr(ch.c[0].line, "cannot read: whether this one is newer cannot be checked"), "... a package for it is checked without waiting either, and needs the button: nothing says which version is there");
      CHECK(!ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err) && ps_scan(&l) == 1 && (p = ps_find("probe")) && l[0].accepted && !l[0].unread && !ps_config_get(p, "server", v, sizeof v), "... and installed over them");
      snprintf(path, sizeof path, "%s/plugins/.stage", dir); unlink(path); mkfifo(path, 0600);
      CHECK(ps_stage_check(&m, &tier, &button, &ch, err, sizeof err) == -1 && strstr(err, "no package was uploaded"), "the staging file as a FIFO: no package");
      unlink(path);
      alarm(0); }

    /* 7c: never back, when the installed manifest is one this build does not read */
    { const struct ps_plugin *l; static char t2[8192]; size_t tl; int k;
      ml = manifest(man, sizeof man, "\"wake\": true, \"files\": 2", FILES_SETTING, dg); n = package(pkg, man, ml, prog, pl, 3, sk);
      CHECK(!ps_remove("probe") && !staged(pkg, n, &tier, &button) && tier == PS_OFFICIAL && !ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err), "(an official version 3 installed)");
      snprintf(path, sizeof path, "%s/plugins/probe/pkg/manifest.json", dir);
      tl = (size_t)snprintf(t2, sizeof t2, "%s", man); memcpy(strstr(t2, "\"memory_mb\": 16"), "\"memory_mb\":\"1\"", 15);
      f = fopen(path, "wb"); fwrite(t2, 1, tl, f); fclose(f);
      k = ps_scan(&l);
      /* the card says that version (finding 7 of a review of 60a8589: it said 0, and the browse page offered any) */
      CHECK(k == 1 && l[0].unread && !l[0].m.newer && l[0].m.version == 3 && strstr(l[0].refused, "cannot read its manifest"), "its manifest made one this build does not read: its card says version 3, the version its listing names");
      CHECK(k == 1 && l[0].unread && update(3, "\"wake\": true, \"files\": 2", FILES_SETTING, NULL, NULL) == -1 && strstr(err, "not newer than the one installed (3)"), "its manifest made one this build does not read: version 3 again is still refused, by the version its listing names");
      CHECK(update(4, "\"wake\": true, \"files\": 2", FILES_SETTING, NULL, NULL) == 0 && !ch.n, "... version 4 is a newer one (and what was accepted, to the byte: no button)");
      snprintf(path, sizeof path, "%s/plugins/probe/pkg/listing.sig", dir); unlink(path);
      CHECK(update(2, "\"wake\": true, \"files\": 2", FILES_SETTING, NULL, NULL) == 1 && ch.n == 1 && strstr(ch.c[0].line, "whether this one is newer cannot be checked"), "... without the listing nothing says which version is there: the button, and why (version 2 here, older than the 3 that was installed: it cannot be told)");
      ml = manifest(man, sizeof man, "\"wake\": true, \"files\": 2", FILES_SETTING, dg); n = package(pkg, man, ml, prog, pl, 3, sk);
      CHECK(!ps_remove("probe") && !staged(pkg, n, &tier, &button) && button == 1 && !ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err), "(the first plugin installed again)"); }

    /* ---- at random: a good manifest, a good package, the program's head, with bytes changed.  Nothing may read
     * past what it was given; what is said about them does not matter */
    srand(7);
    ml = manifest(man, sizeof man, "\"wake\": true, \"files\": 2, \"net.server\": [ { \"setting\": \"server\", \"ports\": \"514\" } ]", FILES_SETTING, dg);
    for (int i = 0; i < 30000; i++) {
        char *t = malloc(ml + 1); size_t l = ml;
        memcpy(t, man, ml);
        for (int k = rand() % 6 + 1; k; k--) t[rand() % (int)ml] = (char)(rand() % 3 ? "{}[]\",:0\\ntfu-e."[rand() % 17] : rand());
        if (rand() % 5 == 0) l = (size_t)(rand() % (int)ml);
        ps_manifest_parse(t, l, &m, err, sizeof err);
        free(t);
    }
    for (int i = 0; i < 20000; i++) {
        size_t l = 4096 < pl ? 4096 : pl; uint8_t *e = malloc(l);
        memcpy(e, prog, l);
        for (int k = rand() % 8 + 1; k; k--) e[rand() % 128] = (uint8_t)rand();
        ps_elf_check(e, rand() % 4 ? l : (size_t)(rand() % 128), err, sizeof err);
        free(e);
    }
    ml = manifest(man, sizeof man, "\"wake\": true, \"files\": 2", FILES_SETTING, dg); { char *v = strstr(man, "\"version\": 3"); v[11] = '9'; }
    n = package(pkg, man, ml, prog, pl, 9, sk);
    for (int i = 0; i < 300; i++) {
        memcpy(mut, pkg, n);
        for (int k = rand() % 4 + 1; k; k--) { size_t at = rand() % 3 ? (size_t)(rand() % 2600) : (size_t)rand() % n; mut[at] = (uint8_t)rand(); }
        staged(mut, rand() % 4 ? n : (size_t)(1024 + rand() % 4000), &tier, &button);
    }
    err[0] = 0;
    /* The acceptance voucher (5.4): its text, field for field, against a MAC made outside this code (Python:
     * hashlib.blake2b(b"hassmic plugin acceptance 1" + target + nonce + browser + node + b"\0" + id + b"\0" +
     * manifest + author + accepted, key=key, digest_size=16), each binary field counting up from the last).  Two
     * builds that order the fields differently refuse each other's vouchers, which shows as "wants its own button"
     * on every Echo and nowhere as a failure: hence a vector, and each field shown to count */
    { uint8_t v[208], mac[16], other[16]; char h[33]; int differs = 1;
      for (int i = 0; i < 208; i++) v[i] = (uint8_t)i;
      ps_voucher(mac, v, v + 32, v + 64, v + 80, "echo-dot-3-5695c4", "microwakeword", v + 112, v + 144, v + 176); hex(h, mac, 16);
      CHECK(!strcmp(h, "7e1fa22f5d9e8a2298d8d53cbfa277de"), "the acceptance voucher's MAC is the one its layout gives");
      for (int f = 0; f < 6; f++) {
          uint8_t w[208]; memcpy(w, v, sizeof w);
          w[(const int[]){ 0, 32, 64, 80, 112, 144 }[f]] ^= 1;
          ps_voucher(other, w, w + 32, w + 64, w + 80, "echo-dot-3-5695c4", "microwakeword", w + 112, w + 144, v + 176); differs &= memcmp(mac, other, 16) != 0;
      }
      v[176] ^= 1; ps_voucher(other, v, v + 32, v + 64, v + 80, "echo-dot-3-5695c4", "microwakeword", v + 112, v + 144, v + 176); differs &= memcmp(mac, other, 16) != 0; v[176] ^= 1;
      ps_voucher(other, v, v + 32, v + 64, v + 80, "echo-dot-3-5695c5", "microwakeword", v + 112, v + 144, v + 176); differs &= memcmp(mac, other, 16) != 0;
      ps_voucher(other, v, v + 32, v + 64, v + 80, "echo-dot-3-5695c4", "microwakeworc", v + 112, v + 144, v + 176); differs &= memcmp(mac, other, 16) != 0;
      CHECK(differs, "... and another key, target, nonce, browser, node, plugin, manifest, author key or accepted section is another voucher");
      ps_voucher(mac, v, v + 32, v + 64, v + 80, "kitchen", "-1abc", v + 112, v + 144, v + 176);
      ps_voucher(other, v, v + 32, v + 64, v + 80, "kitchen-1", "abc", v + 112, v + 144, v + 176);
      CHECK(memcmp(mac, other, 16), "... and where the node ends and the plugin's id begins is part of it"); }
    /* ---- a package is in its folder whole or not at all, and the highest version ever installed is kept (the
     * tenth read: an update that failed after its program was written left it under the old manifest; a plugin
     * removed, which takes no button, was replaced by an older version on a voucher) */
    { const struct ps_plugin *l; char a[500], b[500], flag[300]; struct stat st; uint8_t have[32]; static uint8_t buf[4 << 20]; size_t k; FILE *pf;
      const char *W = "\"wake\": true, \"files\": 2";
      snprintf(flag, sizeof flag, "%s/index.pub", dir);
      ps_remove("probe");
      CHECK(official(20, W, FILES_SETTING) == 1 && !ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err) && ps_installed_max("probe") == 20 && ps_installed_max("never") == 0,
            "version 20 installed: the highest version installed here is 20, of a plugin never here 0");
      setenv("HASSMIC_PLUGIN_FAIL_PACKAGE", flag, 1);
      CHECK(official(21, W, FILES_SETTING) == 0 && ps_stage_install("update", NULL, PS_NO_PRESS, err, sizeof err) == -1 && strstr(err, "cannot write"), "an update whose manifest cannot be written after its program was: not installed");
      unsetenv("HASSMIC_PLUGIN_FAIL_PACKAGE");
      snprintf(a, sizeof a, "%s/probe/pkg/plugin", ps_dir()); pf = fopen(a, "rb"); k = pf ? fread(buf, 1, sizeof buf, pf) : 0; if (pf) fclose(pf);
      crypto_blake2b(have, 32, buf, k);
      snprintf(a, sizeof a, "%s/probe/pkg.new", ps_dir()); snprintf(b, sizeof b, "%s/probe/pkg.old", ps_dir());
      CHECK(ps_scan(&l) == 1 && l[0].here && !l[0].unread && l[0].m.version == 20 && l[0].accepted && !memcmp(have, l[0].m.digest, 32) && stat(a, &st) && stat(b, &st),
            "...and the folder is as it was: version 20's manifest with version 20's program, accepted, nothing left beside it");
      CHECK(ps_installed_max("probe") == 20, "(a version whose package never was whole is not one that was installed here)");
      /* hassmic ended between the two renames: the old package aside, the new one not in yet */
      snprintf(flag, sizeof flag, "%s/probe/pkg", ps_dir());
      CHECK(!rename(flag, b) && !mkdir(a, 0700) && ps_scan(&l) == 1 && l[0].here && l[0].m.version == 20 && l[0].accepted && stat(a, &st) && stat(b, &st),
            "an install that ended between taking the old package aside and putting the new one in: the old one is back, the unfinished one gone");
      CHECK(!mkdir(a, 0700) && !mkdir(b, 0700) && ps_scan(&l) == 1 && l[0].m.version == 20 && stat(a, &st) && stat(b, &st), "...and one that ended after: what was left of both is removed");
      CHECK(official(22, W, FILES_SETTING) == 0 && !ps_stage_install("update", NULL, PS_NO_PRESS, err, sizeof err) && ps_scan(&l) == 1 && l[0].m.version == 22 && l[0].accepted && stat(a, &st) && stat(b, &st),
            "an update goes in as before, nothing left beside it");
      /* removed (no button), then an older one on a voucher */
      CHECK(!ps_remove("probe") && ps_installed_max("probe") == 22, "removed: the highest version that was installed stays known");
      CHECK(official(21, W, FILES_SETTING) == 1 && ps_stage_install(PS_HOW_SYNCED "echo-x", NULL, PS_VOUCHED, err, sizeof err) == PS_NEEDS_BUTTON && strstr(err, "own action button") && ps_scan(&l) == 0,
            "an official version older than one that was installed here, on a voucher: this Echo's own button, nothing written");
      CHECK(official(22, W, FILES_SETTING) == 1 && !ps_stage_install(PS_HOW_SYNCED "echo-x", NULL, PS_VOUCHED, err, sizeof err) && ps_scan(&l) == 1 && l[0].m.version == 22, "the version that was installed, on a voucher: taken");
      CHECK(!ps_remove("probe") && official(21, W, FILES_SETTING) == 1 && !ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err) && ps_scan(&l) == 1 && l[0].m.version == 21 && ps_installed_max("probe") == 22,
            "with this Echo's button an older one is the owner's to take, and the floor stays where it was"); }
    /* ---- the eleventh read: the floor is raised when the package is in place, a first install that never arrived
     * leaves nothing, and more ids than the file keeps closes the sync over a restart too */
    { const struct ps_plugin *l; char a[500], b[600], flag[300], text[8192]; struct stat st; FILE *f; size_t k;
      const char *W = "\"wake\": true, \"files\": 2";
      snprintf(flag, sizeof flag, "%s/index.pub", dir);
      CHECK(!ps_remove("probe") && ps_installed_max("probe") == 22, "(removed; the floor is 22)");
      /* hassmic ended in a first install, after the author's key and before the package: the folder with the key,
       * pkg/ with the unfinished package */
      snprintf(a, sizeof a, "%s/probe", ps_dir()); mkdir(a, 0700);
      snprintf(b, sizeof b, "%s/author.pub", a); f = fopen(b, "w"); if (f) { fwrite(pk, 1, 32, f); fclose(f); }
      snprintf(b, sizeof b, "%s/pkg.new", a); mkdir(b, 0700);
      CHECK(ps_scan(&l) == 0 && stat(a, &st), "a first install that ended before its package was in place: nothing of it is left, no plugin listed");
      /* the floor could not be written as version 23 went in, hassmic ended: the next start knows 23 from the folder */
      setenv("HASSMIC_PLUGIN_FAIL_FLOOR", flag, 1);
      CHECK(official(23, W, FILES_SETTING) == 1 && !ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err) && ps_installed_max("probe") == LONG_MAX,
            "version 23 installed and the floor not written: installed, and no floor is told as known meanwhile");
      unsetenv("HASSMIC_PLUGIN_FAIL_FLOOR");
      ps_floors_forget();
      CHECK(ps_installed_max("probe") == 22 && ps_scan(&l) == 1 && l[0].m.version == 23 && ps_installed_max("probe") == 23, "after a restart the scan raises the floor to what is installed");
      /* ... and without a restart: owed, written at the next scan; and by Remove for what it removes */
      setenv("HASSMIC_PLUGIN_FAIL_FLOOR", flag, 1);
      CHECK(official(24, W, FILES_SETTING) == 0 && !ps_stage_install("update", NULL, PS_NO_PRESS, err, sizeof err) && ps_installed_max("probe") == LONG_MAX, "(version 24, the floor not written)");
      unsetenv("HASSMIC_PLUGIN_FAIL_FLOOR");
      ps_floors_forget();
      CHECK(ps_installed_max("probe") == 23 && !ps_remove("probe") && ps_installed_max("probe") == 24, "Remove raises the floor to the version it removes");
      /* FLOOR_MAX ids on file and one more comes: closed, in the file, and still after a restart */
      snprintf(a, sizeof a, "%s/.installed", ps_dir()); f = fopen(a, "w");
      for (int i = 0; f && i < 64; i++) fprintf(f, "other%d %d\n", i, i + 1);
      if (f) fclose(f);
      ps_floors_forget();
      CHECK(ps_installed_max("other7") == 8 && ps_installed_max("probe") == 0, "64 other plugins were installed here once: their floors known");
      CHECK(official(5, W, FILES_SETTING) == 1 && !ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err) && ps_installed_max("probe") == LONG_MAX && ps_installed_max("other7") == LONG_MAX,
            "a 65th is installed with the button: no floor is known from now on");
      ps_floors_forget();
      CHECK(ps_installed_max("probe") == LONG_MAX && ps_installed_max("other7") == LONG_MAX && ps_scan(&l) == 1 && ps_installed_max("probe") == LONG_MAX, "...and none after a restart (it was forgotten: the 65th's floor read 0)");
      CHECK(official(6, W, FILES_SETTING) == 0 && ps_stage_install(PS_HOW_SYNCED "echo-x", NULL, PS_VOUCHED, err, sizeof err) == PS_NEEDS_BUTTON, "...so a voucher installs nothing there");
      /* an id the file has keeps rising while it is closed */
      CHECK(!ps_remove("probe"), "(removed)");
      f = fopen(a, "w");
      for (int i = 0; f && i < 63; i++) fprintf(f, "other%d %d\n", i, i + 1);
      if (f) { fprintf(f, "probe 3\n+\n"); fclose(f); }
      ps_floors_forget();
      CHECK(official(6, W, FILES_SETTING) == 1 && !ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err), "(one of the 64 installed in version 6, the file closed)");
      f = fopen(a, "r"); k = f ? fread(text, 1, sizeof text - 1, f) : 0; text[k] = 0; if (f) fclose(f);
      CHECK(strstr(text, "probe 6\n") && strstr(text, "\n+\n") && strstr(text, "other62 63\n") && ps_installed_max("probe") == LONG_MAX, "its floor is raised in the file, which stays closed");
      unlink(a); ps_floors_forget();
      /* ---- the twelfth read: floors owed for more than one plugin.  The flash refuses while two plugins go in:
       * closed, at a scan that still cannot write too, and open again with the first scan that can (it was "more
       * than one is owed", which nothing but a restart ended); a removed plugin's floor is among what is written */
      CHECK(!ps_remove("probe") && !unlink(a) && (ps_floors_forget(), ps_scan(&l)) == 0 && ps_installed_max("probe") == 0, "(nothing installed, no floors: as after a reset)");
      setenv("HASSMIC_PLUGIN_FAIL_FLOOR", flag, 1);
      CHECK(official(7, W, FILES_SETTING) == 1 && !ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err)
            && (u_listed = 0, update(4, W, FILES_SETTING, "\"id\": \"probe\"", "\"id\": \"twinx\"")) == 1 && (u_listed = 1, !ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err))
            && ps_installed_max("probe") == LONG_MAX && ps_installed_max("twinx") == LONG_MAX, "two plugins installed while the floors cannot be written: both in, no floor told as known");
      CHECK(ps_scan(&l) == 2 && ps_installed_max("probe") == LONG_MAX && stat(a, &st), "a scan that cannot write them either: still closed, nothing on file");
      unsetenv("HASSMIC_PLUGIN_FAIL_FLOOR");
      CHECK(ps_scan(&l) == 2 && ps_installed_max("probe") == 7 && ps_installed_max("twinx") == 4, "the first scan that can: both floors known again, without a restart");
      ps_floors_forget();
      CHECK(ps_installed_max("probe") == 7 && ps_installed_max("twinx") == 4, "...and on file");
      setenv("HASSMIC_PLUGIN_FAIL_FLOOR", flag, 1);
      CHECK(official(8, W, FILES_SETTING) == 0 && !ps_stage_install("update", NULL, PS_NO_PRESS, err, sizeof err) && !ps_remove("probe") && !ps_remove("twinx") && ps_installed_max("probe") == LONG_MAX,
            "(version 8 in and removed again, with the other plugin, the floors not written)");
      unsetenv("HASSMIC_PLUGIN_FAIL_FLOOR");
      CHECK(ps_scan(&l) == 0 && ps_installed_max("probe") == 8 && ps_installed_max("twinx") == 4 && (ps_floors_forget(), ps_installed_max("probe")) == 8, "a floor owed for a plugin that is gone: written by the next scan that can");
      CHECK(official(8, W, FILES_SETTING) == 1 && !ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err), "(probe back)");
      unlink(a); ps_floors_forget(); }
    CHECK(!ps_remove("probe") && ps_remove("../etc") == -1, "removed; an id that is a path is none");

    /* ---- phase 2 (docs/plugins.md): net.server's entries as the Echo reads them, the line the owner
     * accepts, and storage */
    { static char t2[8192]; static struct ps_manifest nm; char line[400];
#define NS(e) "\"net.server\": [ " e " ]"
#define PROW "{ \"name\": \"port\", \"label\": \"Port\", \"type\": \"int\", \"min\": 1, \"max\": 65535 }"
#define NSET HOST ", " PROW ", " MODE
      static const struct { const char *e, *why; } bad[] = {
          { "{ \"setting\": \"mode\", \"proto\": \"udp\", \"ports\": [ 514 ] }", "a row of type host" },
          { "{ \"setting\": \"server\", \"proto\": \"sctp\", \"ports\": [ 514 ] }", "\"tcp\" or \"udp\"" },
          { "{ \"setting\": \"server\", \"ports\": [ 514 ] }", "\"tcp\" or \"udp\"" },
          { "{ \"setting\": \"server\", \"proto\": \"udp\" }", "\"ports\" is a list" },
          { "{ \"setting\": \"server\", \"proto\": \"udp\", \"ports\": [ ] }", "names no port" },
          { "{ \"setting\": \"server\", \"proto\": \"udp\", \"ports\": [ 5555 ] }", "not 5555" },
          { "{ \"setting\": \"server\", \"proto\": \"udp\", \"ports\": [ 0 ] }", "from 1 to 65535" },
          { "{ \"setting\": \"server\", \"proto\": \"udp\", \"ports\": [ 65536 ] }", "from 1 to 65535" },
          { "{ \"setting\": \"server\", \"proto\": \"udp\", \"ports\": [ \"514\" ] }", "whole number" },
          { "{ \"setting\": \"server\", \"proto\": \"udp\", \"ports\": [ 1, 2, 3, 4, 5, 6, 7, 8, 9 ] }", "8 ports" },
          { "{ \"setting\": \"server\", \"proto\": \"udp\", \"ports\": \"setting:mode\" }", "type int" },
          { "{ \"setting\": \"server\", \"proto\": \"udp\", \"ports\": \"any\" }", "\"setting:<row>\"" },
          { "{ \"setting\": \"server\", \"proto\": \"udp\", \"ports\": [ 514 ], \"any\": true }", "does not read" },
          { "{ \"setting\": \"server\", \"proto\": \"udp\", \"ports\": [ 514 ] }, 5", "an entry is an object" },
      };
      CHECK(!(full(t2, sizeof t2, 1, TOP, NS("{ \"setting\": \"server\", \"proto\": \"udp\", \"ports\": \"setting:port\" }, { \"setting\": \"server\", \"proto\": \"tcp\", \"ports\": [ 6514, 443 ] }"), NSET), ps_manifest_parse(t2, strlen(t2), &nm, err, sizeof err))
            && nm.nnet == 2 && nm.net[0].udp && !strcmp(nm.net[0].port_row, "port") && !nm.net[1].udp && nm.net[1].nports == 2 && nm.net[1].ports[1] == 443, "net.server: two servers read, by row, protocol and ports");
      ps_cap_line(&nm, PM_CAP_NET_SERVER, line, sizeof line);
      CHECK(!strcmp(line, "Connects to the server you enter in \"Server\" (UDP, the port you enter in \"Port\"); and the one in \"Server\" (TCP, ports 6514, 443)"), line);
      for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
          char e[600]; int r;
          snprintf(e, sizeof e, NS("%s"), bad[i].e);
          r = (full(t2, sizeof t2, 1, TOP, e, NSET), ps_manifest_parse(t2, strlen(t2), &nm, err, sizeof err));
          CHECK(r == -1 && strstr(err, bad[i].why), bad[i].e); if (r != -1 || !strstr(err, bad[i].why)) printf("     (%s)\n", err);
      }
      /* N1 of a review of 08399dc: a server row with its author's default connected out of the box, outside the network
       * too, with nothing the owner typed: refused, the port row's too; and what the broker connects to is what the
       * owner stored (ps_config_owner), never a default */
      CHECK((full(t2, sizeof t2, 1, TOP, NS("{ \"setting\": \"server\", \"proto\": \"udp\", \"ports\": [ 514 ] }"), "{ \"name\": \"server\", \"label\": \"Server\", \"type\": \"host\", \"default\": \"198.51.100.7\" }"),
             ps_manifest_parse(t2, strlen(t2), &nm, err, sizeof err)) == -1 && strstr(err, "the row server, where the owner enters the server, has a default"),
            "a server row with a default (its author's server): refused");
      CHECK((full(t2, sizeof t2, 1, TOP, NS("{ \"setting\": \"server\", \"proto\": \"udp\", \"ports\": \"setting:port\" }"), HOST ", { \"name\": \"port\", \"label\": \"Port\", \"type\": \"int\", \"min\": 1, \"max\": 65535, \"default\": 514 }"),
             ps_manifest_parse(t2, strlen(t2), &nm, err, sizeof err)) == -1 && strstr(err, "the row port, where the owner enters the port, has a default"),
            "a port row net.server names, with a default: refused");
      CHECK(!(full(t2, sizeof t2, 1, TOP, "\"storage\": 2", "{ \"name\": \"server\", \"label\": \"Server\", \"type\": \"host\", \"default\": \"example.org\" }"), ps_manifest_parse(t2, strlen(t2), &nm, err, sizeof err)),
            "a host row with a default that no net.server entry names: taken (it is a text the plugin reads, no grant)");
      { const struct ps_plugin *l; char v[PS_VALUE_MAX];
        CHECK(!ps_remove("probe") && check_text(t2) == 1 && !ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err) && ps_scan(&l) == 1
              && ps_config_get(ps_find("probe"), "server", v, sizeof v) && !strcmp(v, "example.org") && !ps_config_owner(ps_find("probe"), "server", v, sizeof v) && !v[0]
              && !ps_config_set(ps_find("probe"), "server", "10.0.0.5", err, sizeof err) && ps_config_owner(ps_find("probe"), "server", v, sizeof v) && !strcmp(v, "10.0.0.5")
              && !ps_config_set(ps_find("probe"), "server", "", err, sizeof err) && !ps_config_owner(ps_find("probe"), "server", v, sizeof v),
              "ps_config_owner: what the owner stored and nothing else (the default is ps_config_get's)");
        CHECK(ps_net_row(ps_find("probe"), "server") == 0, "(a row no entry names is none of net.server's)"); }
      /* N3: the lines the owner accepts are whole, however long the manifest's bounds let them be (they were cut at
       * 300 bytes, and a second server was not in the dialog) */
      { static char caps[4000], rows[8000], longl[PS_CAP_LINE + 600]; size_t cn = 0, rn = 0; const char *P8 = "[ 1514, 2514, 3514, 4514, 5514, 6514, 7514, 8514 ]";
        cn += (size_t)snprintf(caps + cn, sizeof caps - cn, "\"files\": 8, \"net.server\": [ ");
        for (int i = 0; i < 4; i++) cn += (size_t)snprintf(caps + cn, sizeof caps - cn, "%s{ \"setting\": \"s%d\", \"proto\": \"tcp\", \"ports\": %s }", i ? ", " : "", i, P8);
        cn += (size_t)snprintf(caps + cn, sizeof caps - cn, " ]");
        for (int i = 0; i < 4; i++) { char lab[65]; snprintf(lab, sizeof lab, "Server number %d, as a name or an address on your home network and more", i);
            rn += (size_t)snprintf(rows + rn, sizeof rows - rn, "%s{ \"name\": \"s%d\", \"label\": \"%s\", \"type\": \"host\" }", i ? ", " : "", i, lab); }
        for (int i = 0; i < 26; i++) { char lab[65]; snprintf(lab, sizeof lab, "File list %02d with a label of sixty-four bytes to fill the line and more", i);
            rn += (size_t)snprintf(rows + rn, sizeof rows - rn, ", { \"name\": \"f%d\", \"label\": \"%s\", \"type\": \"files\", \"item\": { \"by\": \"stem\", \"files\": [ { \"suffix\": \".bin\", \"max_kb\": 4 } ] }, \"max_items\": 1 }", i, lab); }
        { static char t3[64000]; full(t3, sizeof t3, 1, TOP, caps, rows);
          int r = ps_manifest_parse(t3, strlen(t3), &nm, err, sizeof err); if (r) printf("     (%s)\n", err);
          CHECK(!r, "(a manifest at net.server's bounds, and 26 file lists)");
          ps_cap_line(&nm, PM_CAP_NET_SERVER, longl, sizeof longl);
          CHECK(strlen(longl) < PS_CAP_LINE && strstr(longl, "Server number 3, as a name or an address on your home network an\" (TCP, ports 1514, 2514, 3514, 4514, 5514, 6514, 7514, 8514)"),
                "net.server's line at its longest: whole, the fourth server and its eighth port at its end");
          ps_cap_line(&nm, PM_CAP_FILES, longl, sizeof longl);
          CHECK(strlen(longl) < PS_CAP_LINE && strstr(longl, "File list 25 with a label") && strstr(longl, "up to 8 MB)"), "files' line with 26 lists: whole");
          /* and so in what an update asks: the line, then what it was */
          { static char t4[64000]; char c2[4100]; snprintf(c2, sizeof c2, "%s", caps); memcpy(strstr(c2, "\"files\": 8"), "\"files\": 9", 11);
            if (!ps_remove("probe")) { }
            full(t4, sizeof t4, 1, TOP, caps, rows);
            CHECK(check_text(t4) == 1 && !ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err), "(installed)");
            full(t4, sizeof t4, 2, TOP, c2, rows);
            CHECK(check_text(t4) == 1 && ch.n >= 1 && strstr(ch.c[0].line, "File list 25") && strstr(ch.c[0].line, "was 8 MB"), "an update's line: the whole line and what it was"); } } }
      CHECK((full(t2, sizeof t2, 1, TOP, NS("{ \"setting\": \"server\", \"proto\": \"udp\", \"ports\": \"setting:port\" }"), HOST ", { \"name\": \"port\", \"label\": \"Port\", \"type\": \"int\", \"min\": 0, \"max\": 65535 }"), ps_manifest_parse(t2, strlen(t2), &nm, err, sizeof err)) == -1
            && (full(t2, sizeof t2, 1, TOP, "\"net.server\": []", NSET), ps_manifest_parse(t2, strlen(t2), &nm, err, sizeof err)) == -1 && strstr(err, "names no server"),
            "a port row that takes 0, and no server at all: refused");
      /* what an update of it may be: the same, or the capability gone; anything of its entries otherwise asks */
      CHECK(pair(TOP, NS("{ \"setting\": \"server\", \"proto\": \"udp\", \"ports\": [ 514 ] }"), HOST, TOP, NS("{ \"setting\": \"server\", \"proto\": \"udp\", \"ports\": [ 514 ] }"), HOST) == 0
            && pair(TOP, NS("{ \"setting\": \"server\", \"proto\": \"udp\", \"ports\": [ 514 ] }"), HOST, TOP, NS("{ \"setting\": \"server\", \"proto\": \"tcp\", \"ports\": [ 514 ] }"), HOST) == 1 && one(PS_CHANGED, "not with the parameters you accepted")
            && pair(TOP, NS("{ \"setting\": \"server\", \"proto\": \"udp\", \"ports\": [ 514, 515 ] }"), HOST, TOP, NS("{ \"setting\": \"server\", \"proto\": \"udp\", \"ports\": [ 514 ] }"), HOST) == 1
            && pair(TOP, "\"storage\": 2, \"log.read\": true", HOST, TOP, "\"storage\": 2, \"log.read\": true, " NS("{ \"setting\": \"server\", \"proto\": \"udp\", \"ports\": [ 514 ] }"), HOST) == 1 && one(PS_NEW, "Connects to the server you enter in \"Server\" (UDP, port 514)")
            && pair(TOP, "\"storage\": 2", HOST, TOP, "\"storage\": 2, \"log.read\": true", HOST) == 1 && one(PS_NEW, "Reads this Echo's technical log")
            && pair(TOP, "\"storage\": 2, \"log.read\": true", HOST, TOP, "\"storage\": 2", HOST) == 0,
            "net.server the same: free; another protocol, a port fewer (no rule for less: asked), the server added, the log added: the button, named; the log gone: free");

      /* storage: within its kB, by key, kept and wiped */
      { char v[64]; size_t n; static char big[2100]; uint8_t d1[32], d2[32]; void *tar = NULL; size_t tn = 0; const struct ps_plugin *l;
        memset(big, 'b', sizeof big);
        CHECK(!ps_remove("probe") && check_text(full(t2, sizeof t2, 1, TOP, "\"storage\": 2", HOST)) == 1 && !ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err) && ps_scan(&l) == 1, "(a plugin with 2 kB of storage)");
        CHECK(ps_store_get("probe", "cursor", v, sizeof v, &n) == PS_STORE_NONE && !ps_store_put("probe", "cursor", "abc", 3) && !ps_store_get("probe", "cursor", v, sizeof v, &n) && n == 3 && !memcmp(v, "abc", 3),
              "a key: none, then kept and read back");
        CHECK(!ps_store_put("probe", "a.b-c_9", "", 0) && !ps_store_get("probe", "a.b-c_9", v, sizeof v, &n) && n == 0, "an empty value is one");
        CHECK(ps_store_put("probe", "big", big, sizeof big) == PS_STORE_FULL && ps_store_get("probe", "big", v, sizeof v, &n) == PS_STORE_NONE && ps_storage_used("probe", NULL) == 3,
              "2000 bytes more than 2 kB holds beside what is kept: refused, nothing written");
        CHECK(!ps_store_put("probe", "cursor", big, 2000) && ps_store_put("probe", "cursor", big, 2049) == PS_STORE_FULL, "a key's own old value does not count against its new one; one byte over the kB: refused");
        { int ok = 1; const char *const no[] = { "", ".hidden", "Upper", "a/b", "../x", "a b", "x.tmp", "x.tmp7", "0123456789012345678901234567890123456789012345678", NULL };
          for (int i = 0; no[i]; i++) ok &= !ps_store_key_ok(no[i]) && ps_store_put("probe", no[i], "x", 1) == -1;
          CHECK(ok && ps_store_key_ok("x.tmpx") && ps_store_key_ok("a.tmp.b"), "keys that are none: empty, hidden, upper case, a path, a space, statefile's temporary names, 49 bytes"); }
        CHECK(!ps_storage_digest("probe", d1) && (tar = ps_storage_tar("probe", &tn)) && tn >= 3 * 512, "its digest, and all of it as a tar");
        CHECK(!ps_store_del("probe", "cursor") && ps_store_del("probe", "cursor") == PS_STORE_NONE && !ps_storage_digest("probe", d2) && memcmp(d1, d2, 32), "a key away: another digest");
        CHECK(!ps_storage_replace("probe", tar, tn, 0, err, sizeof err) && !ps_storage_digest("probe", d2) && !memcmp(d1, d2, 32), "the tar back in: the same digest");
        free(tar);
        { static uint8_t over[5120]; tar_put(over, 0, "big", big, 2000); tar_put(over, 512 + 2048, "more", big, 100);
          CHECK(ps_storage_replace("probe", over, sizeof over, 0, err, sizeof err) == -1 && strstr(err, "2 kB") && !ps_store_get("probe", "a.b-c_9", v, sizeof v, &n), "a tar over its kB: refused whole, nothing of it taken");
          tar_put(over, 0, "../up", "x", 1); memset(over + 1024, 0, sizeof over - 1024);
          CHECK(ps_storage_replace("probe", over, 2048, 1, err, sizeof err) == -1 && strstr(err, "cannot keep"), "a key that is none in it: refused"); }
        /* S5 of a review of 08399dc: a replacement that ended between its two renames (no storage/ at all): the new
         * folder in place if it was written whole, else the old one back, at the next scan */
        { char sd[400], sn[420], so[420], f[460]; FILE *w;
          snprintf(sd, sizeof sd, "%s/plugins/probe/storage", dir); snprintf(sn, sizeof sn, "%s.new", sd); snprintf(so, sizeof so, "%s.old", sd);
          CHECK(!ps_store_put("probe", "a.b-c_9", "old", 3) && !rename(sd, so), "(the old folder aside, as the replacement does)");
          mkdir(sn, 0700); snprintf(f, sizeof f, "%s/a.b-c_9", sn); w = fopen(f, "w"); fputs("new", w); fclose(w);
          snprintf(f, sizeof f, "%s/.whole", sn); w = fopen(f, "w"); fputs("1\n", w); fclose(w);
          ps_scan(&l);
          CHECK(!ps_store_get("probe", "a.b-c_9", v, sizeof v, &n) && n == 3 && !memcmp(v, "new", 3) && access(so, F_OK) && access(sn, F_OK), "... the new one written whole: put in place");
          CHECK(!rename(sd, so), "(again, the new folder not written whole)");
          mkdir(sn, 0700); snprintf(f, sizeof f, "%s/a.b-c_9", sn); w = fopen(f, "w"); fputs("hal", w); fclose(w);
          ps_scan(&l);
          CHECK(!ps_store_get("probe", "a.b-c_9", v, sizeof v, &n) && n == 3 && !memcmp(v, "new", 3) && access(so, F_OK) && access(sn, F_OK), "... the new one not whole: the old one back"); }
        { int rr = ps_remove("probe"), rg = (ps_scan(&l), ps_store_get("probe", "a.b-c_9", v, sizeof v, &n));
          CHECK(!rr && rg == -1, "removed: its storage with it"); if (rr || rg != -1) printf("     (%d %d)\n", rr, rg); }
        { char sd[400]; snprintf(sd, sizeof sd, "%s/plugins/probe/storage", dir); CHECK(access(sd, F_OK) != 0, "(the folder gone)"); } } }
    /* ---- phase 6: request.audio with a way out wants a second press wherever the button is asked for (3.3,
     * 5.4): the store's word as it writes, whatever stood for the first (a press, a voucher, which never stands for the
     * second) */
    { const struct ps_plugin *l; const char *RA = "\"request.audio\": true, \"net.udp\": 1"; char line[160], fl[400];
      ps_remove("probe");
      snprintf(fl, sizeof fl, "%s/.installed", ps_dir()); unlink(fl); ps_floors_forget();      /* (the floors of the reads before: none) */
      CHECK(official(30, RA, "") == 1 && ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err) == PS_NEEDS_SECOND && strstr(err, "second press") && ps_scan(&l) == 0,
            "request.audio with net.udp, one press: the store wants the second, nothing written");
      CHECK(ps_stage_second(line, sizeof line) && !strcmp(line, "This plugin hears every request and can send what it hears to your network."), "the second screen's words for net.udp");
      CHECK(ps_stage_install(PS_HOW_SYNCED "echo-x", NULL, PS_VOUCHED, err, sizeof err) == PS_NEEDS_SECOND && ps_scan(&l) == 0, "a voucher for the official package: the second press all the same, nothing written");
      CHECK(!ps_stage_install("button", NULL, PS_VOUCHED_PRESSED, err, sizeof err) && ps_scan(&l) == 1 && l[0].accepted, "a voucher and this Echo's own press: installed");
      CHECK(official(31, RA, "") == 0 && !ps_stage_install("update", NULL, PS_NO_PRESS, err, sizeof err), "an update that asks for nothing new: neither press");
      CHECK(official(32, "\"request.audio\": true, \"net.udp\": 2", "") == 1 && ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err) == PS_NEEDS_SECOND
            && !ps_stage_install("button", NULL, PS_PRESSED2, err, sizeof err) && ps_scan(&l) == 1 && l[0].m.version == 32, "wider (a port more): both presses again, and with both it goes in");
      ps_remove("probe");
      CHECK(official(33, "\"request.audio\": true", "") == 1 && !ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err), "request.audio without a way out: one press");
      CHECK(official(34, "\"request.audio\": true, \"net.discover\": { \"udp\": 3483, \"to\": \"broadcast\" }", "") == 1 && ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err) == PS_NEEDS_SECOND
            && ps_stage_second(line, sizeof line) && strstr(line, "your network"), "a net.* added to a request.audio plugin: the button, and the second press");
      ps_remove("probe");
      CHECK(official(35, "\"request.audio\": true, \"net.server\": [ { \"setting\": \"server\", \"proto\": \"tcp\", \"ports\": [ 80 ] } ], \"net.udp\": 1", "") == 1
            && ps_stage_second(line, sizeof line) && !strcmp(line, "This plugin hears every request and can send what it hears to your network and the server you enter."), "two ways out: both named");
      CHECK(ps_stage_install("button", NULL, PS_VOUCHED_PRESSED, err, sizeof err) == 0, "(a voucher and a press for an official one)");
      ps_remove("probe");
      /* net.lan and net.internet are ways out as the others are */
      CHECK(official(36, "\"request.audio\": true, \"net.lan\": [ { \"proto\": \"tcp\", \"ports\": [ 80 ] } ]", "") == 1 && ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err) == PS_NEEDS_SECOND
            && ps_stage_second(line, sizeof line) && !strcmp(line, "This plugin hears every request and can send what it hears to your network."), "request.audio with net.lan: the second press, \"your network\"");
      CHECK(official(37, "\"request.audio\": true, \"net.internet\": [ { \"proto\": \"tcp\", \"ports\": [ 443 ] } ], \"net.lan\": [ { \"proto\": \"tcp\", \"ports\": [ 80 ] } ]", "") == 1
            && ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err) == PS_NEEDS_SECOND && ps_stage_second(line, sizeof line)
            && !strcmp(line, "This plugin hears every request and can send what it hears to the internet and your network.") && !ps_stage_install("button", NULL, PS_PRESSED2, err, sizeof err) && ps_scan(&l) == 1,
            "request.audio with net.internet (an official plugin: the index refuses a community one) and net.lan: the second press names both, and both presses install it");
      ps_remove("probe");
      { int was = u_listed; u_listed = 0;
      CHECK(update(6, "\"request.audio\": true, \"net.udp\": 1", "", NULL, NULL) == 1 && ps_stage_install(PS_HOW_SYNCED "echo-x", NULL, PS_VOUCHED_PRESSED, err, sizeof err) == PS_NEEDS_BUTTON,
"a voucher and a press for a package the index does not list: not the voucher's (both presses here)");
      ps_remove("probe");
      /* 3.3's partner: the engine a wake plugin nobody vouches for, with nothing beside it that hears requests */
      CHECK(update(7, "\"wake\": true, \"files\": 1", FILES_SETTING, NULL, NULL) == 1 && !ps_stage_install("button", NULL, PS_PRESSED, err, sizeof err), "(a wake plugin from a file)");
      ps_engine_set("probe");
      CHECK(ps_engine_refused("probe", line, sizeof line) == 0, "the engine with nothing that hears requests beside it: may be chosen");
      ps_engine_set(""); ps_remove("probe"); u_listed = was; }
    }
    nftw(dir, gone, 16, FTW_DEPTH | FTW_PHYS);                 /* the test's own folder under /tmp */
    /* ---- phase 4 (docs/plugins.md): net.udp, net.discover, ha.entities, ha.actions as the Echo reads them,
     * the lines the owner accepts, and the ports a plugin keeps */
    { static char t3[8192]; static struct ps_manifest nm; char line[PS_CAP_LINE];
      static const struct { const char *caps, *why; } bad[] = {
          { "\"net.udp\": 0", "1 to 4 ports" }, { "\"net.udp\": 5", "1 to 4 ports" }, { "\"net.udp\": true", "1 to 4 ports" },
          { "\"net.discover\": 3483", "and nothing else" }, { "\"net.discover\": { \"udp\": 3483 }", "to is" },
          { "\"net.discover\": { \"udp\": 5555, \"to\": \"broadcast\" }", "not 5555" }, { "\"net.discover\": { \"udp\": 3483, \"to\": \"10.0.0.255\" }", "multicast group" },
          { "\"net.discover\": { \"udp\": 3483, \"to\": \"239.255.255.250\", \"ttl\": 4 }", "nothing else" },
          { "\"net.discover\": { \"udp\": 3483, \"to\": \"239.255.255.025\" }", "without zeros" },
          { "\"ha.entities\": []", "names no entity" }, { "\"ha.entities\": [ { \"name\": \"a\", \"kind\": \"light\", \"label\": \"A\" } ]", "no kind \"light\"" },
          { "\"ha.entities\": [ { \"name\": \"A\", \"kind\": \"sensor\", \"label\": \"A\" } ]", "a name is 1 to 32" },
          { "\"ha.entities\": [ { \"name\": \"a\", \"kind\": \"sensor\" } ]", "its label" },
          { "\"ha.entities\": [ { \"name\": \"a\", \"kind\": \"switch\", \"label\": \"A\", \"unit\": \"W\" } ]", "a key this version does not read" },
          { "\"ha.entities\": [ { \"name\": \"a\", \"kind\": \"sensor\", \"label\": \"A\", \"icon\": \"fa:x\" } ]", "an icon is" },
          { "\"ha.entities\": [ { \"name\": \"a\", \"kind\": \"number\", \"label\": \"A\", \"min\": 5, \"max\": 5 } ]", "min below max" },
          { "\"ha.entities\": [ { \"name\": \"a\", \"kind\": \"select\", \"label\": \"A\" } ]", "its options are a list" },
          { "\"ha.entities\": [ { \"name\": \"a\", \"kind\": \"event\", \"label\": \"A\", \"types\": [ \"Press\" ] } ]", "types: 1 to 8 names" },
          { "\"ha.entities\": [ " ENT_A ", " ENT_A " ]", "twice" },
          /* H3 of a review of efde36e: Home Assistant keeps an entity by its name, which is the plugin's name and the label */
          { "\"ha.entities\": [ " ENT_A ", { \"name\": \"b\", \"kind\": \"switch\", \"label\": \"A\" } ]", "two are labelled \"A\"" },
          { "\"ha.actions\": [ " ACT_B ", { \"name\": \"go2\", \"label\": \"Go\" } ]", "two are labelled \"Go\"" },
          /* A1 of a review of efde36e: a search to a port of hassmic's own came back to the Echo's own sockets */
          { "\"net.discover\": { \"udp\": 28930, \"to\": \"broadcast\" }", "hassmic's own" },
          { "\"net.discover\": { \"udp\": 28940, \"to\": \"239.255.255.250\" }", "hassmic's own" },
          { "\"net.discover\": { \"udp\": 28928, \"to\": \"broadcast\" }", "hassmic's own" },
          { "\"net.discover\": { \"udp\": 28999, \"to\": \"broadcast\" }", "hassmic's own" },
          { "\"net.discover\": { \"udp\": 2330, \"to\": \"broadcast\" }", "hassmic's own" },
          /* an action that places calls, only with the capability call */
          { "\"ha.actions\": [ { \"name\": \"go\", \"label\": \"Go\", \"calls\": true } ]", "needs the capability \"call\"" },
          { "\"call\": true, \"ha.actions\": [ { \"name\": \"go\", \"label\": \"Go\", \"calls\": \"yes\" } ]", "calls is true or false" },
          { "\"call\": 1", "true, or left out" },
          { "\"ha.actions\": [ { \"name\": \"go\", \"label\": \"Go\", \"page\": true, \"args\": [ { \"name\": \"n\", \"type\": \"int\" } ] } ]", "without arguments" },
          { "\"ha.actions\": [ { \"name\": \"go\", \"label\": \"Go\", \"args\": [ { \"name\": \"n\", \"type\": \"float\" } ] } ]", "\"string\" or \"int\"" },
          { "\"ha.actions\": [ { \"name\": \"go\", \"label\": \"Go\", \"page\": 1 } ]", "page is true or false" },
          { "\"audio.music\": { \"rate\": 48000 }", "true, or left out" },
          /* button is parameterless; an entity's "page" is true or false, on an entity with a state */
          { "\"button\": 1", "true, or left out" }, { "\"button\": { \"keys\": [ \"action\" ] }", "true, or left out" },
          { "\"ha.entities\": [ { \"name\": \"a\", \"kind\": \"sensor\", \"label\": \"A\", \"page\": 1 } ]", "page is true or false" },
          { "\"ha.entities\": [ { \"name\": \"a\", \"kind\": \"sensor\", \"label\": \"A\", \"page\": \"yes\" } ]", "page is true or false" },
          { "\"ha.entities\": [ { \"name\": \"a\", \"kind\": \"button\", \"label\": \"A\", \"page\": true } ]", "no state to show" },
          { "\"ha.entities\": [ { \"name\": \"a\", \"kind\": \"event\", \"label\": \"A\", \"types\": [ \"p\" ], \"page\": true } ]", "no state to show" },
          { "\"ha.entities\": [ { \"name\": \"a\", \"kind\": \"sensor\", \"label\": \"A\", \"page\": true, \"page\": false } ]", "twice" },
      };
      for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
          int r = (full(t3, sizeof t3, 1, TOP, bad[i].caps, HOST), ps_manifest_parse(t3, strlen(t3), &nm, err, sizeof err));
          CHECK(r == -1 && strstr(err, bad[i].why), bad[i].caps); if (r != -1 || !strstr(err, bad[i].why)) printf("     (%s)\n", err);
      }
      CHECK(!(full(t3, sizeof t3, 1, TOP, "\"audio.music\": true, \"audio.volume\": true, \"state\": true, \"net.udp\": 2, \"net.discover\": { \"udp\": 3483, \"to\": \"broadcast\" }, "
                   "\"ha.entities\": [ " ENT_A ", " ENT_B ", { \"name\": \"lvl\", \"kind\": \"number\", \"label\": \"Level\", \"min\": 0, \"max\": 10, \"step\": 0.5, \"unit\": \"dB\", \"icon\": \"mdi:volume-high\" },"
                   " { \"name\": \"mode\", \"kind\": \"select\", \"label\": \"Mode\", \"options\": [ \"a\", \"b\" ] }, { \"name\": \"pressed\", \"kind\": \"event\", \"label\": \"Remote\", \"types\": [ \"press\", \"hold\" ] } ], "
                   "\"ha.actions\": [ " ACT_A ", " ACT_B " ]", HOST), ps_manifest_parse(t3, strlen(t3), &nm, err, sizeof err))
            && nm.udp_ports == 2 && nm.discover_port == 3483 && !nm.discover_group && nm.nentities == 5 && nm.entities[2].kind == PE_NUMBER && nm.entities[2].step == 0.5
            && nm.entities[3].noptions == 2 && nm.entities[4].ntypes == 2 && nm.nactions == 2 && nm.actions[0].nargs == 1 && nm.actions[0].args[0].is_int && nm.actions[1].page,
            "phase 4's capabilities read: the counts, the port, each entity by its kind, each action with its arguments");
      ps_cap_line(&nm, PM_CAP_NET_UDP, line, sizeof line);
      CHECK(!strcmp(line, "Can be reached from your network, and sends to devices on it (2 UDP ports of its own)"), line);
      ps_cap_line(&nm, PM_CAP_NET_DISCOVER, line, sizeof line);
      CHECK(!strcmp(line, "Searches your network for its server (UDP port 3483, to every device on it), and can be reached from your network on a UDP port of its own, where the answers come"), line);
      ps_cap_line(&nm, PM_CAP_HA_ENTITIES, line, sizeof line);
      CHECK(!strcmp(line, "Shows in Home Assistant: \"A\" (a number), \"B\" (a switch), \"Level\" (a number to set), \"Mode\" (a choice), \"Remote\" (an event)"), line);
      ps_cap_line(&nm, PM_CAP_HA_ACTIONS, line, sizeof line);
      CHECK(!strcmp(line, "Home Assistant can tell it to: \"Say\", \"Go\" (also a button on its card)"), line);
      /* button, its line; "page" on each kind with a state, false on a button and an event (nothing to show) */
      CHECK(!(full(t3, sizeof t3, 1, TOP, "\"button\": true, \"ha.entities\": [ { \"name\": \"a\", \"kind\": \"sensor\", \"label\": \"A\", \"unit\": \"W\", \"page\": true }, "
                   "{ \"name\": \"b\", \"kind\": \"binary_sensor\", \"label\": \"B\", \"page\": true }, { \"name\": \"c\", \"kind\": \"text_sensor\", \"label\": \"C\", \"page\": true }, "
                   "{ \"name\": \"d\", \"kind\": \"switch\", \"label\": \"D\", \"page\": true }, { \"name\": \"e\", \"kind\": \"number\", \"label\": \"E\", \"page\": true }, "
                   "{ \"name\": \"f\", \"kind\": \"select\", \"label\": \"F\", \"options\": [ \"x\" ], \"page\": true }, { \"name\": \"g\", \"kind\": \"button\", \"label\": \"G\", \"page\": false }, "
                   "{ \"name\": \"h\", \"kind\": \"event\", \"label\": \"H\", \"types\": [ \"p\" ], \"page\": false }, { \"name\": \"i\", \"kind\": \"sensor\", \"label\": \"I\" } ]", HOST),
              ps_manifest_parse(t3, strlen(t3), &nm, err, sizeof err))
            && (nm.caps & PM_CAPBIT(PM_CAP_BUTTON)) && nm.nentities == 9 && nm.entities[0].page && nm.entities[1].page && nm.entities[2].page && nm.entities[3].page && nm.entities[4].page && nm.entities[5].page
            && !nm.entities[6].page && !nm.entities[7].page && !nm.entities[8].page, "button taken; \"page\" read on every kind with a state, false on a button and an event, left out: none");
      ps_cap_line(&nm, PM_CAP_BUTTON, line, sizeof line);
      CHECK(!strcmp(line, "Hears presses of this Echo's buttons, as Home Assistant's \"Button\" does"), line);
      /* "page" is part of ha.entities 1: no version of its own to require */
      CHECK(!(full(t3, sizeof t3, 1, TOP, "\"ha.entities\": [ { \"name\": \"a\", \"kind\": \"sensor\", \"label\": \"A\", \"page\": true } ]", HOST), ps_manifest_parse(t3, strlen(t3), &nm, err, sizeof err))
            && nm.entities[0].page && nm.requires[PM_SCOPE_HA_ENTITIES] == 1, "an entity's \"page\" with ha.entities 1: taken");
      CHECK((full(t3, sizeof t3, 1, TOP, "\"wake\": true, \"files\": 1, \"button\": true", FL0), check_text(t3)) == -1 && strstr(err, "way out"), "wake with button: refused (the seal, 3.3: only files and storage beside wake)");
      ps_scope_set(PM_SCOPE_BUTTON, 0);
      CHECK((full(t3, sizeof t3, 1, TOP, "\"button\": true", HOST), check_text(t3)) == -1 && !strcmp(err, "needs a newer hassmic: button 1"), "button in a build without it (HASSMIC_PLUGIN_SCOPES=button:0): \"needs a newer hassmic: button 1\"");
      ps_scope_set(PM_SCOPE_BUTTON, 1);
      CHECK(!(full(t3, sizeof t3, 1, TOP, "\"net.discover\": { \"udp\": 1900, \"to\": \"239.255.255.250\" }", HOST), ps_manifest_parse(t3, strlen(t3), &nm, err, sizeof err))
            && nm.discover_group == 0xEFFFFFFAu && (ps_cap_line(&nm, PM_CAP_NET_DISCOVER, line, sizeof line), !strcmp(line, "Searches your network for its server (UDP port 1900, to the group 239.255.255.250), and can be reached from your network on a UDP port of its own, where the answers come")), "net.discover to a group: the line names it");
      CHECK((full(t3, sizeof t3, 1, TOP, "\"wake\": true, \"files\": 1, \"audio.music\": true", FL0), check_text(t3)) == -1 && strstr(err, "way out"), "wake with audio.music: refused (the seal, 3.3)");
      /* the ports: the lowest free, kept, the same again; past what a plugin may have, none (another installed plugin's are left out: tests/fake_plugins_music.py has two) */
      { unsigned short a[5], b[5]; const struct ps_plugin *l; FILE *f;
        { char d2[400]; snprintf(d2, sizeof d2, "%s/plugins/probe", dir); mkdir(d2, 0700); }
        ps_lock(); ps_scan(&l);
        CHECK(ps_udp_ports("probe", 3, a) == 3 && a[0] == PS_PORT_FIRST && a[1] == PS_PORT_FIRST + 1 && a[2] == PS_PORT_FIRST + 2, "a plugin's ports: the lowest of 28940..28999");
        CHECK(ps_udp_ports("probe", 3, b) == 3 && !memcmp(a, b, sizeof a[0] * 3) && ps_udp_ports("probe", 4, b) == 4 && b[3] == PS_PORT_FIRST + 3 && !memcmp(a, b, sizeof a[0] * 3), "the same again, and one more after them");
        ps_unlock();
        CHECK(ps_udp_ports("probe", 6, b) == -1, "more than a plugin may have: none");
        { char d2[400]; snprintf(d2, sizeof d2, "%s/plugins/probe/ports", dir); f = fopen(d2, "w"); fputs("28999 28998\n", f); fclose(f); }
        ps_lock(); CHECK(ps_udp_ports("probe", 3, b) == 3 && b[0] == 28999 && b[1] == 28998 && b[2] == PS_PORT_FIRST, "the ones kept first, the lowest free one after them"); ps_unlock(); } }


    printf("plugin_store: %s\n", fails ? "FAILED" : "all passed");
    return fails != 0;
}
