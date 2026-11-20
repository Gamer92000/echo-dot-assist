/*
 * Replies on another speaker: the assistant's spoken replies play on another Echo of the network or on a Home
 * Assistant media player, not on this Echo's speaker (a bathroom Echo over the shower, a better speaker in the room;
 * TECHO5 has "Replies play on" with a typed media player).  Here: where they go, what Home Assistant said of the
 * player, and the link between Echos.  The run itself stays on this Echo (proto_esphome.c media_thread): while the
 * reply plays elsewhere this Echo plays it silently (core_tts_begin_elsewhere), so its state, ring, barge-in, "stop"
 * and the listening after a question follow the reply as if it played here.
 *
 * Targets (state/reply_speaker, set from the page or Home Assistant's select):
 *  - another Echo of the arbitration network: the reply's URL goes to it over TCP (port -r, 28933), it fetches and
 *    plays it at its own replies' volume and says when it has played, so the run here ends with the reply there.
 *    Signed with a key every member derives from the network's (arb_derive), over a challenge of the playing Echo:
 *    nobody outside the network makes an Echo play anything, and nothing said on the link can be replayed;
 *  - a Home Assistant media player, named or the one in this Echo's area (proto_esphome.c asks Home Assistant): played
 *    with media_player.play_media as an announcement (music there ducks and goes on), which needs "Allow the device to
 *    perform Home Assistant actions".  Nothing says when such a player is done: the run here waits the reply's length
 *    plus the player's start (setting reply_speaker_delay), as TECHO5 does.
 * Whatever cannot take the reply (the Echo gone, busy, refusing; the player unavailable; Home Assistant refusing the
 * action) has it play here instead, so no reply is lost: what came of the last one is on the page.
 *
 * Echos that take replies say so to every member (T_MSG M_PORT, with their port, every ANNOUNCE_MS and when asked:
 * M_ASK at start), so the page and Home Assistant's select list only those, and an Echo started with another -r is
 * reached.  The playing side refuses while it is busy (a run, a reply or media of its own, a Drop In, a timer
 * ringing), with do not disturb on and with reply_from_others off; a run of its own that starts before the reply's
 * audio does makes it give the reply up (proto_esphome.c remote_yields), and the asking Echo plays it.
 *
 * Wire (TCP, after the playing Echo's hello "HMRS" 1 id[8] challenge[16]):
 *   ask:    from[8] flags l0[2] l1[2] chime[l0] url[l1] mac[16]       flags 1: an announcement; lengths big endian
 *   events: ev seq mac[16]       A accepted | B busy | O off | D do not disturb | U cannot | N refused (key), then
 *                                S audio began, and one of P played, C cut there, F nothing played; H every
 *                                HEARTBEAT_MS meanwhile (none for GONE_MS: the playing side is gone, or hangs)
 *   stop:   'X' seq mac[16]      from the asking Echo, any time
 * Every MAC: BLAKE2b-128 keyed with arb_derive(KEY_LABEL) over its label, the challenge and the frame.
 */
#include "replyspk.h"
#include "arb.h"
#include "core.h"
#include "json.h"
#include "net.h"
#include "statefile.h"
#include "../include/netio.h"
#include "../third_party/monocypher.h"
#include <ctype.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define KEY_LABEL   "hassmic reply speaker 1"
#define URL_MAX     1023
#define M_PORT      0x43            /* { M_PORT, port lo, port hi, flags (1: takes replies) }: arb T_MSG to every member */
#define M_ASK       0x44            /* { M_ASK }: say M_PORT soon (an Echo that just started) */
/* Rarely: every member's broadcast costs Wi-Fi airtime the arbitration's claims share (broadcasts go out at the lowest
 * rate, and a lost one is not repeated).  A change goes out at once, and a member that starts asks (M_ASK) */
#define ANNOUNCE_MS 120000          /* and up to 30 s more: members' announcements do not line up */
#define MEMBER_MS   400000          /* a member that said nothing for three of them is gone (arb's members: 5 min) */
#define NCONN       8               /* links being served at once; more are closed at once (nobody's key needed to open one) */
#define NMEM        16
#define NKNOWN      16
#define NSESS       4               /* replies from other Echos at once: one plays, the rest are refused busy */
#define OPEN_MS     4000            /* rs_open: connect, hello and the yes */
#define ASK_MS      5000            /* the playing side: hello to the whole request, whatever comes in between */
#define REPLY_MAX_MS (15 * 60 * 1000)   /* nothing plays this long: both sides give up waiting */
#define HEARTBEAT_MS 2000           /* the playing side's "still here" ... */
#define GONE_MS      6500           /* ... three of them missed: gone (power, Wi-Fi, a hung process TCP would not see) */

const char *const replyspk_modes[RS_MODES] = { "here", "room", "player", "echo" };

static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static struct rs_target tgt = { RS_HERE, "", "" };
static atomic_int announce_, accept_ = 1, delay_ = RS_DELAY_DEFAULT;
static char known[NKNOWN][RS_NODE_MAX]; static int nknown;
/* members that take replies (their M_PORT), by id */
static struct { uint8_t id[8]; char node[RS_NODE_MAX]; unsigned ip; unsigned port; int accepts; long long seen; } mem[NMEM];
/* what Home Assistant says of media players we subscribed to (the player in use, the room's) */
#define NSTATE 4
static struct { char e[RS_ENTITY_MAX], st[40], name[RS_NAME_MAX]; long long at; } hs[NSTATE];
static int actions = -1;
static struct { int asked, found; char area[RS_NAME_MAX], e[RS_ENTITY_MAX], name[RS_NAME_MAX], names[200]; int others; long long at; } room;
static struct { char where[RS_ENTITY_MAX], result[8], why[160]; long long at; } last;
static long long room_asked_at;
static int room_want;                   /* "room" chosen again (the page's Find again): Home Assistant is asked now */

static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static const char *state_dir(void) { const char *e = getenv("HASSMIC_STATE"); return e ? e : "/data/local/hassmic/state"; }

static int valid_node(const char *n)
{
    size_t k = strlen(n);
    if (!k || k >= RS_NODE_MAX) return 0;
    for (; *n; n++) if (!(islower((unsigned char)*n) || isdigit((unsigned char)*n) || *n == '-' || *n == '_' || *n == '.')) return 0;
    return 1;
}

int replyspk_valid_entity(const char *e)
{
    if (strncmp(e, "media_player.", 13) || !e[13] || strlen(e) >= RS_ENTITY_MAX) return 0;
    for (e += 13; *e; e++) if (!(islower((unsigned char)*e) || isdigit((unsigned char)*e) || *e == '_')) return 0;
    return 1;
}

/* ---------------------------------------------------------------- the target, on file */

static void know(const char *node)      /* lk held: a node the select lists from now on */
{
    if (!valid_node(node)) return;
    for (int i = 0; i < nknown; i++) if (!strcmp(known[i], node)) return;
    if (nknown == NKNOWN) {                 /* the oldest goes, never the one chosen */
        int drop = !strcmp(known[0], tgt.node) ? 1 : 0;
        memmove(known + drop, known + drop + 1, sizeof known[0] * (size_t)(NKNOWN - 1 - drop)); nknown--;
    }
    snprintf(known[nknown++], RS_NODE_MAX, "%s", node);
}

static void save(void)                  /* lk held */
{
    char p[300]; struct statefile s;
    snprintf(p, sizeof p, "%s/reply_speaker", state_dir());
    if (statefile_open(&s, p, 0666)) { fprintf(stderr, "reply speaker: cannot write %s\n", p); return; }
    fprintf(s.f, "# hassmic: where the assistant's replies play (replyspk.c); the settings page and Home Assistant write it\n");
    fprintf(s.f, "mode=%s\n", replyspk_modes[tgt.mode]);
    if (tgt.node[0]) fprintf(s.f, "node=%s\n", tgt.node);
    if (tgt.player[0]) fprintf(s.f, "player=%s\n", tgt.player);
    for (int i = 0; i < nknown; i++) fprintf(s.f, "known=%s\n", known[i]);
    if (statefile_commit(&s)) fprintf(stderr, "reply speaker: cannot write %s\n", p);
}

void replyspk_load(void)
{
    char p[300], line[200]; FILE *f;
    snprintf(p, sizeof p, "%s/reply_speaker", state_dir());
    if (!(f = fopen(p, "r"))) return;
    pthread_mutex_lock(&lk);
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='), *v;
        if (line[0] == '#' || !eq) continue;
        *eq = 0; v = eq + 1; v[strcspn(v, "\r\n")] = 0;
        if (!strcmp(line, "mode")) { for (int i = 0; i < RS_MODES; i++) if (!strcmp(v, replyspk_modes[i])) tgt.mode = i; }
        else if (!strcmp(line, "node") && valid_node(v)) snprintf(tgt.node, sizeof tgt.node, "%s", v);
        else if (!strcmp(line, "player") && replyspk_valid_entity(v)) snprintf(tgt.player, sizeof tgt.player, "%s", v);
        else if (!strcmp(line, "known")) know(v);
    }
    fclose(f);
    /* a mode without what it needs (a file written by hand) is this Echo */
    if ((tgt.mode == RS_ECHO && (!tgt.node[0] || !strcmp(tgt.node, core_node_name()))) || (tgt.mode == RS_PLAYER && !tgt.player[0])) tgt.mode = RS_HERE;
    if (tgt.node[0] && strcmp(tgt.node, core_node_name())) know(tgt.node);     /* offered by the select, whatever the file had */
    if (tgt.mode != RS_HERE)
        fprintf(stderr, "reply speaker: replies play on %s\n", tgt.mode == RS_ECHO ? tgt.node : tgt.mode == RS_PLAYER ? tgt.player : "the room's speaker");
    pthread_mutex_unlock(&lk);
}

void replyspk_get(struct rs_target *t) { pthread_mutex_lock(&lk); *t = tgt; pthread_mutex_unlock(&lk); }

static int member_by_node(const char *node, long long now)        /* lk held: index, -1 */
{
    for (int i = 0; i < NMEM; i++) if (mem[i].seen && now - mem[i].seen < MEMBER_MS && !strcmp(mem[i].node, node)) return i;
    return -1;
}

int replyspk_set(const char *spec, char *err, size_t errsz)
{
    char w[16] = "", arg[RS_ENTITY_MAX + 8] = ""; int k = 0;
    sscanf(spec, "%15s %n", w, &k);
    if (k) { snprintf(arg, sizeof arg, "%s", spec + k); arg[strcspn(arg, " \r\n")] = 0; }
    pthread_mutex_lock(&lk);
    struct rs_target was = tgt;
    if (!strcmp(w, "here")) tgt.mode = RS_HERE;
    else if (!strcmp(w, "room")) { tgt.mode = RS_ROOM; room_want = 1; }
    else if (!strcmp(w, "player")) {
        if (!strcmp(arg, "-")) {                         /* cleared (Home Assistant's text emptied): this Echo again */
            tgt.player[0] = 0;
            if (tgt.mode == RS_PLAYER) tgt.mode = RS_HERE;
            goto set;
        }
        if (arg[0] && !replyspk_valid_entity(arg)) { snprintf(err, errsz, "\"%s\" is not a media player's entity id (media_player.<name>)", arg); goto no; }
        if (arg[0]) snprintf(tgt.player, sizeof tgt.player, "%s", arg);
        if (!tgt.player[0]) { snprintf(err, errsz, "name the media player (media_player.<name>)"); goto no; }
        tgt.mode = RS_PLAYER;
    } else if (!strcmp(w, "echo")) {
        if (arg[0] && !valid_node(arg)) { snprintf(err, errsz, "\"%s\" is not an Echo's node name", arg); goto no; }
        if (arg[0] && !strcmp(arg, core_node_name())) { snprintf(err, errsz, "that is this Echo: choose \"This Echo\""); goto no; }
        if (arg[0]) {
            int ok = member_by_node(arg, now_ms()) >= 0;
            for (int i = 0; !ok && i < nknown; i++) ok = !strcmp(known[i], arg);
            if (!ok) { snprintf(err, errsz, "%s is no Echo of this one's network that takes replies", arg); goto no; }
            snprintf(tgt.node, sizeof tgt.node, "%s", arg);
        }
        if (!tgt.node[0]) { snprintf(err, errsz, "choose the Echo"); goto no; }
        tgt.mode = RS_ECHO;
        know(tgt.node);
    } else { snprintf(err, errsz, "\"%s\": here, room, player [media_player.x] or echo [node]", w); goto no; }
set:;
    int changed = memcmp(&was, &tgt, sizeof tgt) != 0;
    struct rs_target now = tgt;
    if (changed) save();
    pthread_mutex_unlock(&lk);
    if (changed) {
        fprintf(stderr, "reply speaker: replies play on %s\n", now.mode == RS_HERE ? "this Echo" : now.mode == RS_ECHO ? now.node
                        : now.mode == RS_PLAYER ? now.player : "the room's speaker");
        core_settings_changed();                        /* Home Assistant's select and text; the player's subscription */
    }
    return 0;
no:
    pthread_mutex_unlock(&lk);
    return -1;
}

int replyspk_announce(int set) { if (set >= 0) atomic_store(&announce_, !!set); return atomic_load(&announce_); }
int replyspk_delay(int set) { if (set >= 0) atomic_store(&delay_, set > RS_DELAY_MAX ? RS_DELAY_MAX : set); return atomic_load(&delay_); }
static void announce_soon(void);
int replyspk_accept(int set)
{
    if (set >= 0 && atomic_exchange(&accept_, !!set) != !!set) announce_soon();      /* the others' lists follow */
    return atomic_load(&accept_);
}

/* ---------------------------------------------------------------- Home Assistant's side */

int replyspk_ha_state(const char *entity, const char *attr, const char *state)
{
    if (strncmp(entity, "media_player.", 13)) return 0;
    pthread_mutex_lock(&lk);
    int i = 0, old = 0;
    while (i < NSTATE && strcmp(hs[i].e, entity)) i++;
    if (i == NSTATE) { for (int j = 1; j < NSTATE; j++) if (hs[j].at < hs[old].at) old = j; i = old; memset(&hs[i], 0, sizeof hs[i]); }
    snprintf(hs[i].e, sizeof hs[i].e, "%s", entity);
    if (attr && !strcmp(attr, "friendly_name")) snprintf(hs[i].name, sizeof hs[i].name, "%s", state);
    else if (!attr || !attr[0]) snprintf(hs[i].st, sizeof hs[i].st, "%s", state);
    hs[i].at = now_ms();
    pthread_mutex_unlock(&lk);
    return 1;
}

static int hs_find(const char *e)       /* lk held */
{
    for (int i = 0; i < NSTATE; i++) if (e[0] && !strcmp(hs[i].e, e) && hs[i].st[0]) return i;
    return -1;
}

void replyspk_player(char *out, size_t cap)
{
    pthread_mutex_lock(&lk);
    snprintf(out, cap, "%s", tgt.mode == RS_PLAYER ? tgt.player : tgt.mode == RS_ROOM ? room.e : "");
    pthread_mutex_unlock(&lk);
}

int replyspk_player_ready(const char *entity, char *why, size_t cap)
{
    pthread_mutex_lock(&lk);
    int i = hs_find(entity), ok = 0;
    if (i < 0) snprintf(why, cap, "Home Assistant has told nothing of %s (no such media player?)", entity);
    else if (!strcmp(hs[i].st, "unavailable") || !strcmp(hs[i].st, "unknown")) snprintf(why, cap, "%s is %s in Home Assistant", entity, hs[i].st);
    else ok = 1;
    pthread_mutex_unlock(&lk);
    return ok;
}

int replyspk_actions(int set)
{
    pthread_mutex_lock(&lk);
    if (set == -2) actions = -1; else if (set >= 0) actions = !!set;
    int r = actions;
    pthread_mutex_unlock(&lk);
    return r;
}

void replyspk_room(int found, const char *area, const char *entity, const char *name, int others, const char *names)
{
    pthread_mutex_lock(&lk);
    int moved = strcmp(room.e, entity);
    room.asked = 1; room.found = found; room.others = others; room.at = now_ms();
    snprintf(room.area, sizeof room.area, "%s", area); snprintf(room.e, sizeof room.e, "%s", entity);
    snprintf(room.name, sizeof room.name, "%s", name); snprintf(room.names, sizeof room.names, "%s", names);
    pthread_mutex_unlock(&lk);
    if (moved || !found)
        fprintf(stderr, "reply speaker: the room %s%s%s%s\n", !found ? "could not be asked: " : area[0] ? area : "(this Echo has no area in Home Assistant)",
                found && area[0] ? ": " : "", found && area[0] ? (entity[0] ? entity : "no other media player there") : "", !found ? area : "");
}
void replyspk_room_asked(void) { pthread_mutex_lock(&lk); room_asked_at = now_ms(); room_want = 0; pthread_mutex_unlock(&lk); }
int replyspk_room_wanted(void) { pthread_mutex_lock(&lk); int w = room_want; pthread_mutex_unlock(&lk); return w; }
long long replyspk_room_age_ms(void)
{
    pthread_mutex_lock(&lk); long long a = room_asked_at ? now_ms() - room_asked_at : -1; pthread_mutex_unlock(&lk);
    return a;
}

void replyspk_outcome(const char *where, const char *result, const char *why)
{
    pthread_mutex_lock(&lk);
    snprintf(last.where, sizeof last.where, "%s", where); snprintf(last.result, sizeof last.result, "%s", result);
    snprintf(last.why, sizeof last.why, "%s", why ? why : ""); last.at = now_ms();
    pthread_mutex_unlock(&lk);
    fprintf(stderr, "reply speaker: %s %s%s%s\n", !strcmp(result, "here") ? "played here, not on" : !strcmp(result, "cut") ? "cut on" : "played on",
            where, why && why[0] ? ": " : "", why ? why : "");
}

static size_t jstr(char *o, size_t n, size_t cap, const char *s)   /* s as a JSON string at n */
{
    if (n + 2 >= cap) return n;
    int k = json_esc(o + n, cap - n, s);
    return k < 0 ? n : n + (size_t)k;
}

/* appended at n, never past cap - 1 (what does not fit is cut, as web.c's app) */
static size_t app(char *o, size_t n, size_t cap, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static size_t app(char *o, size_t n, size_t cap, const char *fmt, ...)
{
    if (n + 1 >= cap) return cap ? cap - 1 : 0;
    va_list ap; va_start(ap, fmt); int k = vsnprintf(o + n, cap - n, fmt, ap); va_end(ap);
    return k < 0 ? n : n + (size_t)k < cap ? n + (size_t)k : cap - 1;
}

size_t replyspk_json(char *o, size_t cap)
{
    size_t n = 0; long long now = now_ms(); char ip[20]; int k = 0;
#define J(...) (n = app(o, n, cap, __VA_ARGS__))
    pthread_mutex_lock(&lk);
    J("{\"running\":%s,\"mode\":\"%s\",\"node\":", replyspk_running() ? "true" : "false", replyspk_modes[tgt.mode]); n = jstr(o, n, cap, tgt.node);
    J(",\"player\":"); n = jstr(o, n, cap, tgt.player);
    J(",\"announce\":%s,\"accept\":%s,\"delay\":%d,\"actions\":%s,\"echos\":[", atomic_load(&announce_) ? "true" : "false",
      atomic_load(&accept_) ? "true" : "false", atomic_load(&delay_), actions < 0 ? "null" : actions ? "true" : "false");
    /* members that take replies or not, then nodes known but not heard from now */
    for (int i = 0; i < NMEM; i++) if (mem[i].seen && now - mem[i].seen < MEMBER_MS) {
        snprintf(ip, sizeof ip, "%u.%u.%u.%u", mem[i].ip & 255, mem[i].ip >> 8 & 255, mem[i].ip >> 16 & 255, mem[i].ip >> 24);
        J("%s{\"node\":", k++ ? "," : ""); n = jstr(o, n, cap, mem[i].node);
        J(",\"ip\":\"%s\",\"online\":true,\"accepts\":%s}", ip, mem[i].accepts ? "true" : "false");
    }
    for (int i = 0; i < nknown; i++) if (member_by_node(known[i], now) < 0) {
        J("%s{\"node\":", k++ ? "," : ""); n = jstr(o, n, cap, known[i]); J(",\"ip\":\"\",\"online\":false,\"accepts\":false}");
    }
    J("],\"player_info\":");
    const char *pe = tgt.mode == RS_ROOM ? room.e : tgt.player; int h = hs_find(pe);
    if (!pe[0]) J("null");
    else {
        J("{\"entity\":"); n = jstr(o, n, cap, pe);
        J(",\"state\":"); n = jstr(o, n, cap, h >= 0 ? hs[h].st : "");
        J(",\"name\":"); n = jstr(o, n, cap, h >= 0 ? hs[h].name : ""); J("}");
    }
    J(",\"room\":{\"asked\":%s,\"found\":%s,\"area\":", room.asked ? "true" : "false", room.found ? "true" : "false"); n = jstr(o, n, cap, room.area);
    J(",\"entity\":"); n = jstr(o, n, cap, room.e); J(",\"name\":"); n = jstr(o, n, cap, room.name);
    J(",\"others\":%d,\"names\":", room.others); n = jstr(o, n, cap, room.names); J("},\"last\":");
    if (!last.at) J("null");
    else {
        J("{\"where\":"); n = jstr(o, n, cap, last.where); J(",\"result\":\"%s\",\"why\":", last.result); n = jstr(o, n, cap, last.why);
        J(",\"ago_s\":%lld}", (now - last.at) / 1000);
    }
    J("}");
    pthread_mutex_unlock(&lk);
#undef J
    return n;
}

/* ---------------------------------------------------------------- the network's Echos */

int replyspk_known(char (*nodes)[RS_NODE_MAX], int max)
{
    int n = 0;
    pthread_mutex_lock(&lk);
    for (int i = 0; i < nknown && n < max; i++) memcpy(nodes[n++], known[i], RS_NODE_MAX);
    pthread_mutex_unlock(&lk);
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && strcmp(nodes[j - 1], nodes[j]) > 0; j--) {     /* a few: insertion */
        char t[RS_NODE_MAX]; memcpy(t, nodes[j], RS_NODE_MAX); memcpy(nodes[j], nodes[j - 1], RS_NODE_MAX); memcpy(nodes[j - 1], t, RS_NODE_MAX);
    }
    return n;
}

int replyspk_learn(void)
{
    int grew = 0; long long now = now_ms();
    pthread_mutex_lock(&lk);
    for (int i = 0; i < NMEM; i++) if (mem[i].seen && now - mem[i].seen < MEMBER_MS && mem[i].accepts) {
        int had = 0;
        for (int j = 0; j < nknown; j++) had |= !strcmp(known[j], mem[i].node);
        if (!had) { know(mem[i].node); grew = 1; }
    }
    if (grew) save();
    pthread_mutex_unlock(&lk);
    return grew;
}

/* arb's thread: another member's M_PORT or M_ASK (any member's message to all, arb.h) */
static atomic_int answer_due;
static int wake_w = -1;
void replyspk_message(const unsigned char from[8], const char *node, unsigned ip, const unsigned char *p, size_t n)
{
    if (!n) return;
    if (p[0] == M_ASK) { atomic_store(&answer_due, 1); announce_soon(); return; }
    if (p[0] != M_PORT || n < 4 || !valid_node(node)) return;
    long long now = now_ms(); int at = -1, old = 0;
    pthread_mutex_lock(&lk);
    for (int i = 0; i < NMEM; i++) if (mem[i].seen && !memcmp(mem[i].id, from, 8)) at = i;
    if (at < 0) { for (int i = 1; i < NMEM; i++) if (mem[i].seen < mem[old].seen) old = i; at = old; }
    int fresh = !mem[at].seen || now - mem[at].seen >= MEMBER_MS || memcmp(mem[at].id, from, 8);
    memcpy(mem[at].id, from, 8); snprintf(mem[at].node, sizeof mem[at].node, "%s", node);
    mem[at].ip = ip; mem[at].port = (unsigned)(p[1] | p[2] << 8); mem[at].accepts = p[3] & 1; mem[at].seen = now;
    pthread_mutex_unlock(&lk);
    if (fresh) fprintf(stderr, "reply speaker: %s %s replies (port %u)\n", node, p[3] & 1 ? "takes" : "does not take", (unsigned)(p[1] | p[2] << 8));
}

/* ---------------------------------------------------------------- the link: MACs */

static void mac16(uint8_t out[16], const uint8_t k[32], const char *label, const uint8_t ch[16], const void *a, size_t an, const void *b, size_t bn)
{
    crypto_blake2b_ctx c;
    crypto_blake2b_keyed_init(&c, 16, k, 32);
    crypto_blake2b_update(&c, (const uint8_t *)label, strlen(label));
    crypto_blake2b_update(&c, ch, 16);
    if (an) crypto_blake2b_update(&c, a, an);
    if (bn) crypto_blake2b_update(&c, b, bn);
    crypto_blake2b_final(&c, out);
}

/* A peer that loses power or Wi-Fi mid-reply sends nothing more, not even a reset: TCP's keepalive notices it within
 * about 11 s (5 s idle, 3 probes 2 s apart), and every read gives up after 5 s (a frame comes whole or not at all) */
static void link_opts(int fd)
{
    int on = 1, idle = 5, intvl = 2, cnt = 3; struct timeval tv = { 5, 0 };
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle); setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

/* n bytes before the monotonic ms "until", or -1: a sender that trickles a byte now and then holds no thread for long */
static int read_until(int fd, void *buf, size_t n, long long until)
{
    for (size_t got = 0; got < n; ) {
        int left = (int)(until - now_ms());
        struct pollfd q = { fd, POLLIN, 0 };
        if (left <= 0 || poll(&q, 1, left) <= 0) return -1;
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static int random_bytes(uint8_t *b, size_t n)
{
    int f = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    int ok = f >= 0 && read(f, b, n) == (ssize_t)n;
    if (f >= 0) close(f);
    return ok ? 0 : -1;
}

/* one event or command: what, its number, its MAC */
static int frame_send(int fd, const uint8_t k[32], const char *label, const uint8_t ch[16], int what, int seq)
{
    uint8_t f[18] = { (uint8_t)what, (uint8_t)seq };
    mac16(f + 2, k, label, ch, f, 2, NULL, 0);
    return write_all(fd, f, sizeof f) < 0 ? -1 : 0;
}
/* 1 and *what for a good frame numbered seq; 0 a bad one (the link is ended); -1 the link is gone */
static int frame_read(int fd, const uint8_t k[32], const char *label, const uint8_t ch[16], int seq, int *what)
{
    uint8_t f[18], m[16];
    if (read_full(fd, f, sizeof f) != (ssize_t)sizeof f) return -1;
    mac16(m, k, label, ch, f, 2, NULL, 0);
    if (crypto_verify16(m, f + 2) || f[1] != (uint8_t)seq) return 0;
    *what = f[0];
    return 1;
}

/* ---------------------------------------------------------------- the playing side */

static int lport, lsock = -1;
static atomic_int running;
static unsigned next_id;
static atomic_int nconn;
static struct sess { unsigned id; int pipe[2]; } sess[NSESS];
static pthread_mutex_t sl = PTHREAD_MUTEX_INITIALIZER;

int replyspk_running(void) { return atomic_load(&running); }

void replyspk_event(unsigned id, int ev)
{
    pthread_mutex_lock(&sl);
    for (int i = 0; i < NSESS; i++) if (sess[i].id == id) { char c = (char)ev; if (write(sess[i].pipe[1], &c, 1) < 0) {} }
    pthread_mutex_unlock(&sl);
}

static int final_ev(int ev) { return ev == RS_EV_PLAYED || ev == RS_EV_CUT || ev == RS_EV_FAILED; }

static void *serve(void *arg)
{
    int fd = (int)(long)arg, seq = 0, slot = -1; uint8_t k[32], ch[16], me[8], hello[29], head[13], m[16], *urls = NULL;
    long long ask_until = now_ms() + ASK_MS;
    link_opts(fd);
    if (arb_derive(KEY_LABEL, k) || random_bytes(ch, 16)) goto out;
    arb_self(me);
    memcpy(hello, "HMRS", 4); hello[4] = 1; memcpy(hello + 5, me, 8); memcpy(hello + 13, ch, 16);
    if (write_all(fd, hello, sizeof hello) < 0 || read_until(fd, head, sizeof head, ask_until)) goto out;
    unsigned l0 = (unsigned)head[9] << 8 | head[10], l1 = (unsigned)head[11] << 8 | head[12];
    if (l0 > URL_MAX || !l1 || l1 > URL_MAX || !(urls = malloc(l0 + l1 + 2))) goto out;
    if (read_until(fd, urls, l0 + l1, ask_until) || read_until(fd, m, 16, ask_until)) goto out;
    {   /* the MAC over who it is meant for, the frame and the URLs */
        crypto_blake2b_ctx c; uint8_t want[16];
        crypto_blake2b_keyed_init(&c, 16, k, 32);
        crypto_blake2b_update(&c, (const uint8_t *)"HMRS req", 8); crypto_blake2b_update(&c, ch, 16); crypto_blake2b_update(&c, me, 8);
        crypto_blake2b_update(&c, head, sizeof head); crypto_blake2b_update(&c, urls, l0 + l1); crypto_blake2b_final(&c, want);
        if (crypto_verify16(want, m) || !memcmp(head, me, 8)) {
            fprintf(stderr, "reply speaker: a request without the network's key refused\n");
            frame_send(fd, k, "HMRS ev", ch, 'N', seq++); goto out;
        }
    }
    char chime[URL_MAX + 1], url[URL_MAX + 1], from[RS_NODE_MAX] = "another Echo";
    memcpy(chime, urls, l0); chime[l0] = 0; memcpy(url, urls + l0, l1); url[l1] = 0;
    for (char *c = chime; *c; c++) if ((unsigned char)*c < 0x20) *c = '_';     /* one line each, for the log */
    for (char *c = url; *c; c++) if ((unsigned char)*c < 0x20) *c = '_';
    if ((chime[0] && strncmp(chime, "http://", 7) && strncmp(chime, "https://", 8)) || (strncmp(url, "http://", 7) && strncmp(url, "https://", 8))) {
        frame_send(fd, k, "HMRS ev", ch, 'U', seq++); goto out;
    }
    pthread_mutex_lock(&lk);
    for (int i = 0; i < NMEM; i++) if (mem[i].seen && !memcmp(mem[i].id, head, 8)) snprintf(from, sizeof from, "%s", mem[i].node);
    pthread_mutex_unlock(&lk);
    if (!replyspk_accept(-1)) {
        fprintf(stderr, "reply speaker: %s's reply refused: \"Play other Echos' replies\" is off\n", from);
        frame_send(fd, k, "HMRS ev", ch, 'O', seq++); goto out;
    }
    pthread_mutex_lock(&sl);
    for (int i = 0; i < NSESS && slot < 0; i++) if (!sess[i].id && !pipe(sess[i].pipe)) {
        slot = i; if (!++next_id) ++next_id;            /* 0 is no session */
        sess[i].id = next_id;
    }
    unsigned id = slot >= 0 ? sess[slot].id : 0;
    pthread_mutex_unlock(&sl);
    if (slot < 0) { frame_send(fd, k, "HMRS ev", ch, 'B', seq++); goto out; }
    int r = core_remote_reply(chime, url, head[8] & 1, id);
    if (r) {
        fprintf(stderr, "reply speaker: %s's reply refused (%s)\n", from, r == 'B' ? "busy" : r == 'D' ? "do not disturb" : "cannot play it");
        frame_send(fd, k, "HMRS ev", ch, r, seq++); goto done;
    }
    fprintf(stderr, "reply speaker: playing %s's %s\n", from, head[8] & 1 ? "announcement" : "reply");
    int cseq = 0, asker = 1;
    if (frame_send(fd, k, "HMRS ev", ch, 'A', seq++)) { core_remote_stop(id); asker = 0; }     /* it cannot hear what comes of it */
    struct pollfd p[2] = { { sess[slot].pipe[0], POLLIN, 0 }, { fd, POLLIN, 0 } };
    long long until = now_ms() + REPLY_MAX_MS, beat = now_ms() + HEARTBEAT_MS;   /* a lost event ends the wait then */
    while (now_ms() < until) {
        p[1].fd = asker ? fd : -1;
        if (asker && now_ms() >= beat) { beat = now_ms() + HEARTBEAT_MS; if (frame_send(fd, k, "HMRS ev", ch, 'H', seq++)) asker = 0; }
        if (poll(p, 2, 500) < 0) continue;
        if (p[0].revents & POLLIN) {
            char ev; if (read(p[0].fd, &ev, 1) != 1) break;
            if (asker && frame_send(fd, k, "HMRS ev", ch, ev, seq++)) asker = 0;
            if (final_ev(ev)) break;
        }
        if (asker && p[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            int what = 0, got = frame_read(fd, k, "HMRS cmd", ch, cseq, &what);
            if (got == 1 && what == 'X') { cseq++; fprintf(stderr, "reply speaker: %s asked to stop\n", from); core_remote_stop(id); }
            else if (got <= 0) asker = 0;               /* gone: what plays plays to its end (a link that dropped is no stop) */
        }
    }
done:
    pthread_mutex_lock(&sl);
    if (slot >= 0) { close(sess[slot].pipe[0]); close(sess[slot].pipe[1]); sess[slot].id = 0; }
    pthread_mutex_unlock(&sl);
out:
    free(urls);
    crypto_wipe(k, sizeof k);
    close(fd);
    atomic_fetch_sub(&nconn, 1);
    return NULL;
}

/* ---------------------------------------------------------------- telling the network */

static void announce_soon(void) { if (wake_w >= 0) { char c = 1; if (write(wake_w, &c, 1) < 0) {} } }

static void say_port(int ask)
{
    uint8_t m[4] = { M_PORT, (uint8_t)(lport & 255), (uint8_t)(lport >> 8), (uint8_t)(atomic_load(&accept_) ? 1 : 0) }, a = M_ASK;
    arb_tell(NULL, m, sizeof m);
    if (ask) arb_tell(NULL, &a, 1);
}

static void *loop(void *arg)
{
    int wp[2]; long long next = now_ms(), asks = 2;      /* at start: ours, and the others' (twice: Wi-Fi drops broadcasts) */
    (void)arg;
    if (pipe(wp)) wp[0] = wp[1] = -1;
    wake_w = wp[1];
    unsigned seed = (unsigned)now_ms();
    for (;;) {
        long long now = now_ms(); int wait = next > now ? (int)(next - now) : 0;
        struct pollfd p[2] = { { lsock, POLLIN, 0 }, { wp[0], POLLIN, 0 } };
        if (poll(p, wp[0] >= 0 ? 2 : 1, wait) > 0) {
            if (p[1].revents & POLLIN) {                  /* asked, or the setting changed: soon, not all members at once */
                char b[16]; if (read(wp[0], b, sizeof b) < 0) {}
                long long soon = now_ms() + (atomic_exchange(&answer_due, 0) ? 50 + rand_r(&seed) % 400 : 0);
                if (soon < next) next = soon;
            }
            if (p[0].revents & POLLIN) {
                int c = net_accept(lsock);
                pthread_t t;
                if (c >= 0 && atomic_fetch_add(&nconn, 1) >= NCONN) { close(c); atomic_fetch_sub(&nconn, 1); }
                else if (c >= 0 && pthread_create(&t, NULL, serve, (void *)(long)c)) { close(c); atomic_fetch_sub(&nconn, 1); }
                else if (c >= 0) pthread_detach(t);
            }
        }
        replyspk_learn();                                 /* what members said meanwhile, for Home Assistant's select */
        if (now_ms() >= next) {
            say_port(asks > 0);
            next = now_ms() + (asks-- > 0 ? 1500 : ANNOUNCE_MS + rand_r(&seed) % 30000);
        }
    }
    return NULL;
}

int replyspk_start(int port)
{
    pthread_t t;
    if (!arb_running()) return -1;
    if ((lsock = net_listen(port)) < 0) { perror("reply speaker: listen"); return -1; }
    listen(lsock, 4);
    fcntl(lsock, F_SETFD, FD_CLOEXEC);
    lport = port;
    atomic_store(&running, 1);
    if (pthread_create(&t, NULL, loop, NULL)) { atomic_store(&running, 0); close(lsock); lsock = -1; return -1; }
    pthread_detach(t);
    fprintf(stderr, "reply speaker: port %d\n", port);
    return 0;
}

/* ---------------------------------------------------------------- the asking side */

struct rs_peer { int fd, seq, cseq; long long since, heard; uint8_t k[32], ch[16]; };

int rs_open(const char *node, const char *chime, const char *url, int announce, struct rs_peer **out, char *why, size_t cap)
{
    uint8_t id[8], hello[29], me[8], head[13]; unsigned ip, port = 0; char host[20], ps[12]; int i, accepts = 0;
    size_t l0 = strlen(chime), l1 = strlen(url);
    *out = NULL;
    if (l0 > URL_MAX || l1 > URL_MAX || !l1) { snprintf(why, cap, "the reply's address is too long"); return -1; }
    if (arb_member(node, id, &ip)) { snprintf(why, cap, "not heard from on the network"); return -1; }
    pthread_mutex_lock(&lk);
    if ((i = member_by_node(node, now_ms())) >= 0) { port = mem[i].port; accepts = mem[i].accepts; }
    pthread_mutex_unlock(&lk);
    if (!port) { snprintf(why, cap, "it does not say it takes replies (an older version?)"); return -1; }
    if (!accepts) { snprintf(why, cap, "\"Play other Echos' replies\" is off there"); return -1; }
    struct rs_peer *p = calloc(1, sizeof *p);
    if (!p) { snprintf(why, cap, "no memory"); return -1; }
    if (arb_derive(KEY_LABEL, p->k)) { snprintf(why, cap, "this Echo is in no network"); free(p); return -1; }
    snprintf(host, sizeof host, "%u.%u.%u.%u", ip & 255, ip >> 8 & 255, ip >> 16 & 255, ip >> 24); snprintf(ps, sizeof ps, "%u", port);
    if ((p->fd = net_connect(host, ps, 2)) < 0) { snprintf(why, cap, "cannot connect to %s:%s", host, ps); free(p); return -1; }
    link_opts(p->fd);
    struct timeval tv = { OPEN_MS / 1000, 0 };
    setsockopt(p->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (read_full(p->fd, hello, sizeof hello) != (ssize_t)sizeof hello || memcmp(hello, "HMRS", 4) || hello[4] != 1 || memcmp(hello + 5, id, 8)) {
        snprintf(why, cap, "it did not answer as an Echo of this network"); goto fail;
    }
    memcpy(p->ch, hello + 13, 16);
    arb_self(me);
    memcpy(head, me, 8); head[8] = announce ? 1 : 0;
    head[9] = (uint8_t)(l0 >> 8); head[10] = (uint8_t)l0; head[11] = (uint8_t)(l1 >> 8); head[12] = (uint8_t)l1;
    {
        crypto_blake2b_ctx c; uint8_t m[16];
        crypto_blake2b_keyed_init(&c, 16, p->k, 32);
        crypto_blake2b_update(&c, (const uint8_t *)"HMRS req", 8); crypto_blake2b_update(&c, p->ch, 16); crypto_blake2b_update(&c, id, 8);
        crypto_blake2b_update(&c, head, sizeof head); crypto_blake2b_update(&c, (const uint8_t *)chime, l0);
        crypto_blake2b_update(&c, (const uint8_t *)url, l1); crypto_blake2b_final(&c, m);
        if (write_all(p->fd, head, sizeof head) < 0 || write_all(p->fd, chime, l0) < 0 || write_all(p->fd, url, l1) < 0 || write_all(p->fd, m, 16) < 0) {
            snprintf(why, cap, "the link broke"); goto fail;
        }
    }
    int what = 0, r = frame_read(p->fd, p->k, "HMRS ev", p->ch, p->seq, &what);
    if (r != 1) { snprintf(why, cap, r ? "no answer" : "an answer without the network's key"); goto fail; }
    p->seq++;
    if (what != 'A') {
        snprintf(why, cap, "%s", what == 'B' ? "it is busy" : what == 'O' ? "\"Play other Echos' replies\" is off there"
                 : what == 'D' ? "do not disturb is on there" : what == 'N' ? "it refused this Echo's key" : "it cannot play it");
        goto fail;
    }
    tv.tv_sec = 5; tv.tv_usec = 0;                      /* from here rs_wait polls; a frame comes whole within that */
    setsockopt(p->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    p->since = p->heard = now_ms();
    *out = p;
    return 0;
fail:
    close(p->fd); crypto_wipe(p, sizeof *p); free(p);
    return -1;
}

int rs_wait(struct rs_peer *p, int ms)
{
    struct pollfd q = { p->fd, POLLIN, 0 };
    if (now_ms() - p->since > REPLY_MAX_MS || now_ms() - p->heard > GONE_MS) return -1;     /* gone, or given up there too */
    int r = poll(&q, 1, ms), what = 0;
    if (r <= 0) return 0;
    r = frame_read(p->fd, p->k, "HMRS ev", p->ch, p->seq, &what);
    if (r != 1) return -1;
    p->seq++; p->heard = now_ms();
    return what == 'H' ? 0 : what;
}

void rs_stop(struct rs_peer *p) { if (p && p->fd >= 0) frame_send(p->fd, p->k, "HMRS cmd", p->ch, 'X', p->cseq++); }

void rs_close(struct rs_peer *p)
{
    if (!p) return;
    if (p->fd >= 0) close(p->fd);
    crypto_wipe(p, sizeof *p); free(p);
}
