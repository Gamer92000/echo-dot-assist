/*
 * Where this Echo's software comes from (issue #175).  Two sources, each the project's (the default) or the owner's:
 *
 *   updates   the releases online updates look at (update.c): a repository on GitHub, or on a Forgejo or Gitea
 *             (their API is GitHub's for what update.c reads: releases?per_page=10, releases/latest, tag_name, and
 *             assets at <owner>/<repo>/releases/download/<tag>/<file>), told apart by the host;
 *   plugins   the plugin index's site (plugin_fetch.c: index.json, revoked.json, packages/), plugin_index.c pi_source.
 *
 * A server alone changes nothing of what is trusted: root installs only bundles signed with the owner's update key or
 * the release key (main.sh ota_watch), and the plugin index's lists and listings count only under the index key.  So
 * an Echo kept off the internet takes the project's builds from a mirror on its LAN, or a fork's own release key's
 * builds from the fork's server, and an approved browser sets that at once.  For a source of the owner's two more:
 *
 *   a key    of the owner's, trusted BESIDE the project's (a bundle under the release key, a list under the index key
 *            still count: the factory copy and the plugins installed stay what they were).  For updates root has to
 *            hold it: hassmic leaves it in state/update-key (32 bytes, or none to take it back), root's ota_watch takes
 *            it into a file of its own (/data/local/hassmic/page-update.pub) and checks every bundle against it too.
 *   no check every signature taken.  For updates that is hassmic's own key (state/update-own.key, made here): root is
 *            told to trust it as above, and update.c signs what it downloads with it (nothing of the server's is
 *            asked for a signature).  The push port (ota.c) still takes the owner's update key only.  For the
 *            plugin index pi_signer takes any 64 bytes as the owner's key's, and a missing .sig as such 64 bytes.
 *
 * What that costs, and why it is the owner's call anyway (the maintainer's decision, 2026-11-24): root trusts a key
 * hassmic names.  Until now nothing hassmic said could get a bundle installed that the owner's or the project's key
 * did not sign (docs/SECURITY.md "hassmic and root"); with this, code that took hassmic over can name its own key and
 * run as root.  Bounded as adb over Wi-Fi is: a key or no check is taken only from an approved browser AND a press of
 * the action button for that very choice (web.c W_SOURCE, sources_widens), and the page shows it in red on every page
 * for as long as it stands.  A key or no check taken away, a server changed back, are taken at once: they trust less.
 *
 * Not exported (a choice of trust for this one Echo: "make like this Echo" and a preset do not copy it), gone with a
 * factory reset (state/; root's file too, main.sh).  One file: state/sources, "<which>_<field> <value>" lines.
 */
#include "sources.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "plugin_index.h"
#include "plugin_store.h"
#include "statefile.h"
#include "../include/edkey.h"
#include "../third_party/monocypher.h"

/* as update.c: the Makefile passes it (keys/release.conf, a fork's secrets/fork.conf) */
#ifndef REPO
#define REPO "Gamer92000/echo-dot-assist"
#endif

static const char *const which_name[SRC_N] = { "updates", "plugins" };
static pthread_mutex_t sl = PTHREAD_MUTEX_INITIALIZER;
static struct sources cur;              /* sl */
static struct source loaded;            /* sl: the plugin index as this start put it in force (its lists in memory) */

static const char *state_dir(void) { const char *e = getenv("HASSMIC_STATE"); return e ? e : "/data/local/hassmic/state"; }
static void spath(char *p, size_t cap, const char *name) { snprintf(p, cap, "%s/%s", state_dir(), name); }
/* root's copy of the key it trusts for the page (main.sh ota_watch writes it, 644): read to tell the page whether it
 * holds what was asked */
static const char *root_pub(void) { const char *e = getenv("HASSMIC_PAGE_UPDATE_PUB"); return e && *e ? e : "/data/local/hassmic/page-update.pub"; }

const char *sources_default(int which) { return which == SRC_UPDATES ? "https://github.com/" REPO : pi_base_builtin(); }

static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static int unhex32(uint8_t out[32], const char *s)
{
    for (int i = 0; i < 32; i++) {
        int a = hexv(s[2 * i]), b = a < 0 ? -1 : hexv(s[2 * i + 1]);
        if (b < 0) return -1;
        out[i] = (uint8_t)(a << 4 | b);
    }
    return s[64] ? -1 : 0;
}
static void hex32(char out[65], const uint8_t k[32]) { for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", k[i]); }

/* ---------------------------------------------------------------- the addresses */

/* "http(s)://host[:port]/path": the scheme's length, or 0.  The rest of URL printable and plain: it goes into
 * http.c's URLs, the log and the page's JSON as it stands.  No query or fragment: paths are appended to it */
static size_t url_scheme(const char *u)
{
    size_t k = !strncasecmp(u, "https://", 8) ? 8 : !strncasecmp(u, "http://", 7) ? 7 : 0;
    if (!k || !u[k] || u[k] == '/') return 0;
    for (const char *c = u; *c; c++)
        if (*c <= 0x20 || *c >= 0x7f || strchr("\"\\<>'`^{}|?#@", *c)) return 0;     /* (@: no user or password, which the log and every page would show) */
    return k;
}

/* the owner's repository URL, "https://host[/prefix]/<owner>/<repo>", into the API's and the downloads' bases.
 * github.com's API is api.github.com/repos/<owner>/<repo>; Forgejo's and Gitea's <root>/api/v1/repos/<owner>/<repo>
 * (root: the server and any path it is served under).  0, or -1 where it is no repository's address */
static int repo_urls(const char *u, char *api, size_t acap, char *dl, size_t dcap)
{
    char b[SRC_URL_MAX + 1], *slash, *repo, *owner; size_t k = url_scheme(u), n;
    if (!k || strlen(u) > SRC_URL_MAX) return -1;
    snprintf(b, sizeof b, "%s", u);
    n = strlen(b); while (n > k && b[n - 1] == '/') b[--n] = 0;
    if (!(slash = strrchr(b + k, '/'))) return -1;
    repo = slash + 1; *slash = 0;
    if (!(slash = strrchr(b + k, '/'))) return -1;
    owner = slash + 1; *slash = 0;
    if (!*owner || !*repo || !strcmp(repo, ".") || !strcmp(repo, "..") || !strcmp(owner, ".") || !strcmp(owner, "..")) return -1;
    /* b is now the root: "https://host" or "https://host/prefix" */
    int gh = !strcasecmp(b + k, "github.com") || !strcasecmp(b + k, "www.github.com");
    if (api) {
        if (gh) snprintf(api, acap, "https://api.github.com/repos/%s/%s", owner, repo);
        else snprintf(api, acap, "%s/api/v1/repos/%s/%s", b, owner, repo);
    }
    if (dl) snprintf(dl, dcap, "%s/%s/%s/releases/download", gh ? "https://github.com" : b, owner, repo);
    return 0;
}

/* the page's word for a source's server into OUT: "" for the default (none given, or the default's own address),
 * else as given, the plugin index's with its "/" at the end.  0, or -1 and why */
static int url_take(int which, const char *in, char *out, size_t cap, char *err, size_t ecap)
{
    char v[SRC_URL_MAX + 2]; size_t n;
    while (isspace((unsigned char)*in)) in++;
    snprintf(v, sizeof v, "%s", in); n = strlen(v);
    while (n && isspace((unsigned char)v[n - 1])) v[--n] = 0;
    if (!n) { out[0] = 0; return 0; }
    if (n > SRC_URL_MAX - 1) { snprintf(err, ecap, "%s: the address is longer than %d characters", which_name[which], SRC_URL_MAX - 1); return -1; }
    if (!url_scheme(v)) { snprintf(err, ecap, "%s: not an http:// or https:// address (no spaces, quotes, ?, # or @: no user or password)", which_name[which]); return -1; }
    if (which == SRC_UPDATES) {
        while (n && v[n - 1] == '/') v[--n] = 0;
        if (repo_urls(v, NULL, 0, NULL, 0)) { snprintf(err, ecap, "updates: give the repository's address, https://<server>/<owner>/<repository>"); return -1; }
        if (!strcasecmp(v, sources_default(SRC_UPDATES))) { out[0] = 0; return 0; }
    } else {
        if (v[n - 1] != '/') { v[n++] = '/'; v[n] = 0; }
        if (!strcmp(v, sources_default(SRC_PLUGINS))) { out[0] = 0; return 0; }
    }
    snprintf(out, cap, "%s", v);
    return 0;
}

/* ---------------------------------------------------------------- state/sources */

static void read_file(struct sources *s)
{
    char p[400], line[400]; FILE *f;
    memset(s, 0, sizeof *s);
    spath(p, sizeof p, "sources");
    /* (O_NONBLOCK: a FIFO in its place opens without a writer and reads as nothing, as plugin_store.c's open_reg) */
    int fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0 || !(f = fdopen(fd, "r"))) { if (fd >= 0) close(fd); return; }
    while (fgets(line, sizeof line, f)) {
        char *sp = strchr(line, ' '), *nl = strchr(line, '\n'); int w;
        if (!sp) continue;
        if (nl) *nl = 0;
        *sp++ = 0;
        for (w = 0; w < SRC_N; w++) {
            size_t l = strlen(which_name[w]);
            if (strncmp(line, which_name[w], l) || line[l] != '_') continue;
            const char *field = line + l + 1; struct source *o = &s->s[w];
            if (!strcmp(field, "url") && url_scheme(sp) && strlen(sp) <= SRC_URL_MAX) snprintf(o->url, sizeof o->url, "%s", sp);
            else if (!strcmp(field, "key") && !unhex32(o->key, sp) && !ed_key_weak(o->key)) o->has_key = 1;
            else if (!strcmp(field, "unchecked")) o->unchecked = !strcmp(sp, "1");
        }
    }
    fclose(f);
    /* a key or no check only for a server of the owner's (sources_parse's rule; a file edited by hand the same) */
    for (int w = 0; w < SRC_N; w++) if (!s->s[w].url[0]) s->s[w].has_key = s->s[w].unchecked = 0;
}

static int write_file(const struct sources *s)
{
    char p[400], t[1200], k[65]; size_t n = 0;
    for (int w = 0; w < SRC_N; w++) {
        const struct source *o = &s->s[w];
        if (o->url[0]) n += (size_t)snprintf(t + n, sizeof t - n, "%s_url %s\n", which_name[w], o->url);
        if (o->has_key) { hex32(k, o->key); n += (size_t)snprintf(t + n, sizeof t - n, "%s_key %s\n", which_name[w], k); }
        if (o->unchecked) n += (size_t)snprintf(t + n, sizeof t - n, "%s_unchecked 1\n", which_name[w]);
    }
    spath(p, sizeof p, "sources");
    if (!n) { if (unlink(p) && errno != ENOENT) return -1; return 0; }
    return statefile_write(p, 0600, t, n);
}

/* ---------------------------------------------------------------- hassmic's own key (updates with no check) */

/* the secret half (64 bytes: seed and public key, as Monocypher keeps it) into SK; MAKE: one is made where there is
 * none.  0, or -1 */
static int own_key(uint8_t sk[64], int make)
{
    char p[400]; uint8_t seed[32], pk[32]; int fd; ssize_t r;
    spath(p, sizeof p, "update-own.key");
    if ((fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC)) >= 0) {
        r = read(fd, sk, 64); close(fd);
        if (r == 64) return 0;
    }
    if (!make) return -1;
    if ((fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC)) < 0) return -1;
    r = read(fd, seed, sizeof seed); close(fd);
    if (r != (ssize_t)sizeof seed) return -1;
    crypto_eddsa_key_pair(sk, pk, seed);                /* (wipes the seed) */
    if (statefile_write(p, 0600, sk, 64)) { crypto_wipe(sk, 64); return -1; }
    fprintf(stderr, "sources: hassmic's own key made for updates without a signature check\n");
    return 0;
}

/* the key root is to trust for the page (the owner's, or hassmic's own with no check) into PK: 1, 0 none (lock held) */
static int root_wants(const struct sources *s, uint8_t pk[32], int make)
{
    const struct source *u = &s->s[SRC_UPDATES]; uint8_t sk[64];
    if (u->unchecked) {
        if (own_key(sk, make)) return -1;
        memcpy(pk, sk + 32, 32); crypto_wipe(sk, sizeof sk);
        return 1;
    }
    if (u->has_key) { memcpy(pk, u->key, 32); return 1; }
    return 0;
}

/* 1 root's file holds PK (or, HAVE 0, there is none) */
static int root_holds(const uint8_t pk[32], int have)
{
    uint8_t k[32]; int fd = open(root_pub(), O_RDONLY | O_NONBLOCK | O_CLOEXEC); ssize_t r = -1;
    if (fd >= 0) { r = read(fd, k, sizeof k); close(fd); }
    if (!have) return fd < 0;
    return r == 32 && !memcmp(k, pk, 32);
}

/* state/update-key for root's ota_watch (main.sh): the key's 32 bytes, or nothing to take its file away.  Root takes
 * it as it takes every request of ours (moved into its own folder, a plain file of one name); the file's length is
 * all it reads into its decision, and what it writes is a key of 32 bytes or none */
static int root_ask(const uint8_t *pk)
{
    char p[400];
    spath(p, sizeof p, "update-key");
    return statefile_write(p, 0644, pk, pk ? 32 : 0);
}

/* ---------------------------------------------------------------- interface */

void sources_load(void)
{
    uint8_t pk[32]; int want; char p[400];
    pthread_mutex_lock(&sl);
    read_file(&cur);
    loaded = cur.s[SRC_PLUGINS];
    /* What was kept of the plugin index before what is trusted for it changed (sources_apply leaves the mark): its
     * serials' floors, its revocation list and the word on it, the versions it listed.  Kept, the new index's lists
     * would be held to another index's serials and refused.  (Installs' own floor, .installed, is this Echo's) */
    spath(p, sizeof p, "sources-forget");
    if (!access(p, F_OK)) {
        static const char *const kept[] = { ".serial", ".revoked", ".revoked-at", ".listed" };
        char f[400];
        for (size_t i = 0; i < sizeof kept / sizeof *kept; i++) { snprintf(f, sizeof f, "%s/%s", ps_dir(), kept[i]); unlink(f); }
        unlink(p);
        fprintf(stderr, "sources: what was kept of the plugin index before (its serials, its list of withdrawn plugins) forgotten\n");
    }
    {   const struct source *g = &cur.s[SRC_PLUGINS];
        pi_source(g->url, g->has_key ? g->key : NULL, g->unchecked);
        if (g->url[0]) fprintf(stderr, "sources: the plugin index is the owner's own, %s%s%s\n", g->url,
                               g->has_key ? ", with a key of the owner's beside the project's" : "", g->unchecked ? ", NO SIGNATURE CHECKED" : ""); }
    {   const struct source *u = &cur.s[SRC_UPDATES];
        if (u->url[0]) fprintf(stderr, "sources: updates come from the owner's own server, %s%s%s\n", u->url,
                               u->has_key ? ", with a key of the owner's beside the release key" : "", u->unchecked ? ", NO SIGNATURE CHECKED" : ""); }
    /* Root's file is what state/sources says, or root is asked again: a request it never took (an older main.sh, a
     * restart in between), a factory reset's leftovers, a file of root's that names another key */
    want = root_wants(&cur, pk, 1);
    if (want >= 0 && !root_holds(pk, want)) {
        if (root_ask(want ? pk : NULL)) fprintf(stderr, "sources: cannot ask root for the update key (state/update-key)\n");
        else fprintf(stderr, "sources: root asked %s the update key of the settings page\n", want ? "to trust" : "to forget");
    }
    pthread_mutex_unlock(&sl);
}

void sources_get(struct sources *out) { pthread_mutex_lock(&sl); *out = cur; pthread_mutex_unlock(&sl); }

int sources_parse(const char *body, const struct sources *base, struct sources *out, char *err, size_t cap)
{
    char line[400];
    err[0] = 0;
    *out = *base;
    for (const char *p = body; *p; ) {
        size_t l = strcspn(p, "\n"), m = strcspn(p, "\r\n"); char *eq; int w;
        if (m >= sizeof line) { snprintf(err, cap, "a line longer than %zu characters", sizeof line - 1); return -1; }
        memcpy(line, p, m); line[m] = 0;
        p += l + (p[l] == '\n');
        if (!line[0]) continue;
        if (!(eq = strchr(line, '='))) { snprintf(err, cap, "not understood: %.60s", line); return -1; }
        *eq++ = 0;
        for (w = 0; w < SRC_N; w++) { size_t k = strlen(which_name[w]); if (!strncmp(line, which_name[w], k) && line[k] == '_') break; }
        if (w == SRC_N) { snprintf(err, cap, "not understood: %.60s", line); return -1; }
        const char *field = line + strlen(which_name[w]) + 1; struct source *o = &out->s[w];
        if (!strcmp(field, "url")) { if (url_take(w, eq, o->url, sizeof o->url, err, cap)) return -1; }
        else if (!strcmp(field, "key")) {
            char *v = eq; size_t n;
            while (isspace((unsigned char)*v)) v++;
            n = strlen(v); while (n && isspace((unsigned char)v[n - 1])) v[--n] = 0;
            if (!n) o->has_key = 0;
            else if (n != 64 || unhex32(o->key, v)) { snprintf(err, cap, "%s: the key is not 64 hex digits (the public key's 32 bytes, as otatool keygen or the index's tool make them: od -An -tx1 -v <key>.pub)", which_name[w]); return -1; }
            else if (ed_key_weak(o->key)) { snprintf(err, cap, "%s: that key is one under which any signature holds: no key", which_name[w]); return -1; }
            else o->has_key = 1;
        }
        else if (!strcmp(field, "unchecked")) o->unchecked = !strcmp(eq, "1") || !strcasecmp(eq, "on");
        else { snprintf(err, cap, "not understood: %.60s", line); return -1; }
    }
    for (int w = 0; w < SRC_N; w++) {
        struct source *o = &out->s[w];
        if ((o->has_key || o->unchecked) && !o->url[0]) {
            snprintf(err, cap, "%s: a key of your own or no signature check only for a server of your own: the project's is checked against the project's key", which_name[w]);
            return -1;
        }
        if (o->unchecked) o->has_key = 0;               /* (no check: a key would say nothing) */
    }
    return 0;
}

static int same(const struct source *a, const struct source *b)
{
    return !strcmp(a->url, b->url) && a->has_key == b->has_key && a->unchecked == b->unchecked && (!a->has_key || !memcmp(a->key, b->key, 32));
}

int sources_equal(const struct sources *a, const struct sources *b)
{
    for (int w = 0; w < SRC_N; w++) if (!same(&a->s[w], &b->s[w])) return 0;
    return 1;
}

int sources_widens(const struct sources *from, const struct sources *to)
{
    for (int w = 0; w < SRC_N; w++)
        if ((to->s[w].has_key || to->s[w].unchecked) && !same(&from->s[w], &to->s[w])) return 1;
    return 0;
}

int sources_apply(const struct sources *expect, const struct sources *to, char *err, size_t cap)
{
    char p[400]; uint8_t pk[32]; int want, plug, upd, trust;
    err[0] = 0;
    pthread_mutex_lock(&sl);
    /* Judged against EXPECT: another browser's change in between (a key taken away, which needs no press) and TO
     * would put back what that one took away, with nobody's press for it (a review, 2026-11-24: the press's choice
     * was put in force after web.c let go of its lock) */
    if (!sources_equal(&cur, expect)) { pthread_mutex_unlock(&sl); snprintf(err, cap, "Sources changed meanwhile (another browser): nothing changed, ask again"); return -2; }
    plug = !same(&cur.s[SRC_PLUGINS], &to->s[SRC_PLUGINS]);
    upd = !same(&cur.s[SRC_UPDATES], &to->s[SRC_UPDATES]);
    if (!plug && !upd) { pthread_mutex_unlock(&sl); return 0; }
    /* hassmic's own key first, where no check is asked for: without it nothing is saved */
    if ((want = root_wants(to, pk, 1)) < 0) { pthread_mutex_unlock(&sl); snprintf(err, cap, "cannot make hassmic's own key (state/update-own.key)"); return -1; }
    /* Whether what is trusted for the plugin index differs from what this start put in force (not from the choice
     * just before: a key set and taken away again before root's restart is no change, and a mirror after a key would
     * have left the mark and the project's floors forgotten; a review, 2026-11-24).  The mark goes on the flash
     * before state/sources: lost between the two, the worst is one needless forget, never the new index held to the
     * old one's floors */
    {   const struct source *a = &loaded, *b = &to->s[SRC_PLUGINS];
        trust = a->has_key != b->has_key || a->unchecked != b->unchecked || (a->has_key && memcmp(a->key, b->key, 32));
        spath(p, sizeof p, "sources-forget");
        if (trust ? statefile_write(p, 0644, "1\n", 2) : (unlink(p) && errno != ENOENT)) {
            pthread_mutex_unlock(&sl); snprintf(err, cap, "cannot mark what was kept of the plugin index (state/sources-forget): nothing changed"); return -1; } }
    if (write_file(to)) { pthread_mutex_unlock(&sl); snprintf(err, cap, "cannot save state/sources"); return -1; }
    cur = *to;
    if (!to->s[SRC_UPDATES].unchecked) { spath(p, sizeof p, "update-own.key"); unlink(p); }     /* (kept only while it is trusted) */
    /* (asked whatever root's file says now: a request for the key taken away may still wait for root's next look,
     * 2 s, and would put it there after this; a review, 2026-11-24.  The last request written is the one root takes) */
    if (upd && root_ask(want ? pk : NULL)) snprintf(err, cap, "saved, but root could not be asked for the update key (state/update-key)");
    {   const struct source *u = &cur.s[SRC_UPDATES]; char k[65] = "";
        if (u->has_key) hex32(k, u->key);
        if (upd) fprintf(stderr, "sources: updates from %s%s%s%s\n", u->url[0] ? u->url : "the project's releases (default)",
                         u->has_key ? ", the owner's key beside the release key: " : "", k, u->unchecked ? ", NO SIGNATURE CHECKED" : ""); }
    if (plug) {
        /* What this Echo kept of the old index is that index's: its serials' floors (.serial, the revocation list's
         * line), its revocation list and the word on it, the versions it listed.  Where what is trusted changed (a key
         * of the owner's, no check, or back), the new index's lists are of other serials and would be held to the old
         * one's and refused: forgotten, the next start reads the new one from scratch.  A server alone (a mirror of
         * the same index, under the same keys) keeps them: forgotten, a mirror could hand back an older revocation
         * list of the project's, and plugins it withdrew since would run again (a review, 2026-11-24).  (Installs'
         * own floor, .installed, is this Echo's and stays.)  The lists in memory are the old index's either way:
         * hassmic restarts (root's main.sh, state/restart) */
        /* (forgotten by the next start, sources_load, before any thread of the plugins runs: deleted here, a look in
         * flight or the revocation list's word wrote them back before root's restart, and the new index was held to
         * the old one's floors after all; a review, 2026-11-24) */
        const struct source *g = &cur.s[SRC_PLUGINS]; char k[65] = "";
        if (g->has_key) hex32(k, g->key);
        fprintf(stderr, "sources: the plugin index is %s%s%s%s: %srestarting\n", g->url[0] ? g->url : "the project's (default)",
                g->has_key ? ", the owner's key beside the index key: " : "", k, g->unchecked ? ", NO SIGNATURE CHECKED" : "",
                trust ? "what was kept of the one before forgotten, " : "");
        spath(p, sizeof p, "restart");
        if (statefile_write(p, 0666, "plugin index\n", 13) && !err[0]) snprintf(err, cap, "saved, but the restart that takes the new plugin index could not be asked for: restart the Echo");
    }
    pthread_mutex_unlock(&sl);
    return 0;
}

int sources_level(void)
{
    int l = 0;
    pthread_mutex_lock(&sl);
    for (int w = 0; w < SRC_N; w++) {
        if (cur.s[w].has_key || cur.s[w].unchecked) l = 2;
        else if (cur.s[w].url[0] && l < 1) l = 1;
    }
    pthread_mutex_unlock(&sl);
    return l;
}

int sources_update_urls(char *api, size_t acap, char *dl, size_t dcap)
{
    int r;
    pthread_mutex_lock(&sl);
    r = cur.s[SRC_UPDATES].url[0] && !repo_urls(cur.s[SRC_UPDATES].url, api, acap, dl, dcap);
    pthread_mutex_unlock(&sl);
    return r;
}

int sources_update_unchecked(void) { int r; pthread_mutex_lock(&sl); r = cur.s[SRC_UPDATES].unchecked; pthread_mutex_unlock(&sl); return r; }

int sources_update_sign(const uint8_t *b, size_t len, uint8_t sig[64])
{
    uint8_t sk[64]; int r = -1;
    pthread_mutex_lock(&sl);
    if (cur.s[SRC_UPDATES].unchecked && !own_key(sk, 0)) { crypto_eddsa_sign(sig, sk, b, len); crypto_wipe(sk, sizeof sk); r = 0; }
    pthread_mutex_unlock(&sl);
    return r;
}

int sources_update_key(uint8_t pk[32])
{
    int r;
    pthread_mutex_lock(&sl);
    r = root_wants(&cur, pk, 0) == 1;
    pthread_mutex_unlock(&sl);
    return r;
}

size_t sources_json(char *o, size_t cap)
{
    size_t n = 0; uint8_t pk[32]; int want, held;
    pthread_mutex_lock(&sl);
    want = root_wants(&cur, pk, 0);
    held = want >= 0 && root_holds(pk, want == 1);
    n += (size_t)snprintf(o + n, cap - n, "{");
    for (int w = 0; w < SRC_N && n < cap; w++) {
        const struct source *s = &cur.s[w]; char k[65] = "";
        if (s->has_key) hex32(k, s->key);
        /* (url and default are url_scheme's: no quote or backslash, nothing to escape) */
        n += (size_t)snprintf(o + n, cap - n, "%s\"%s\":{\"url\":\"%s\",\"default\":\"%s\",\"key\":\"%s\",\"unchecked\":%s", w ? "," : "", which_name[w],
                              s->url, sources_default(w), k, s->unchecked ? "true" : "false");
        /* root: whether root's file is what this side asked for (updates only): "held", "asked" (not yet: root takes
         * it within seconds; an older main.sh never does) */
        if (w == SRC_UPDATES && n < cap) n += (size_t)snprintf(o + n, cap - n, ",\"root\":\"%s\"", held ? "held" : "asked");
        if (n < cap) n += (size_t)snprintf(o + n, cap - n, "}");
    }
    pthread_mutex_unlock(&sl);
    if (n < cap) n += (size_t)snprintf(o + n, cap - n, ",\"level\":%d}", sources_level());
    return n < cap ? n : cap - 1;
}
