/*
 * Music Assistant's own API, for the screen's music pages (src/ui/music.c).  Sendspin (sendspin.c) is how Music
 * Assistant plays here and says what plays; browsing the library, searching, the queue and grouping speakers are not
 * in it, and are the API Music Assistant's own frontend uses: a websocket on its port 8095 (ws://<server>:8095/ws).
 *
 *   server     the address the page set (state/mass "url="; the page gives the one it found when none is typed), else,
 *              without a token, the one a Sendspin session came from (mass_peer: Music Assistant itself, its API on
 *              8095 of the same address), only to say what it is
 *   login      the first message is the server's info; from schema 28 (Music Assistant 2.7) a long-lived token is
 *              needed ("auth"), made in Music Assistant's frontend (the user's profile) and pasted on the settings page,
 *              which seals it like a Wi-Fi password (web.c).  It stays in state/mass, 0600, never sent anywhere else
 *   messages   {"message_id","command","args"} out; results {"message_id","result"} or {"message_id","error_code",
 *              "details"}; events {"event","object_id","data"} for everything that changes, once logged in
 *   this Echo  the player whose id is Sendspin's client id (or that has it among its output protocols: a device merged
 *              from several protocols); its queue is the one it plays from (player_queues/get_active_queue: a group's
 *              leader's when it follows one)
 *
 * What the UI gets is digested here into short lines (scene.h): "ma" (the API's state), "pl" (a player, written again
 * only when what it shows changes: Music Assistant sends player_updated for every second of play), "pq" (this Echo's
 * queue), "qc" (its items changed), lists as "L"/"I"/"E", images as "img" with the bytes (the server's imageproxy
 * scales them: sizes 80, 160, 256, 512).  Tested against a stand-in server by tests/fake_ma_api.py.
 */
#include "mass.h"
#include "json.h"
#include "net.h"
#include "screen.h"
#include "sendspin.h"
#include "statefile.h"
#include "ws.h"
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PAGE      60                    /* a list's page, as the UI scrolls */
#define NPEND     64
#define NPL       128                   /* players kept: those shown, and this Echo's (player_put) */
#define NIMG      2048                  /* image keys remembered (key -> where Music Assistant has it) */
#define IMG_MAX   (2 << 20)

static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER, iq_cv = PTHREAD_COND_INITIALIZER;
static char url[256], token[2048], peer[64];                /* the page's; Sendspin's server */
static unsigned gen;                                        /* a new url, token or peer: connect again */
static int started;

/* the connection (under lk where both threads touch it) */
static struct ws w; static int up, state;                   /* state: 0 off, 1 connecting, 2 up, 3 auth refused, 4 error */
/* w is sent on by any thread (the UI's requests) and closed by the session's: both under wslk, and only while wsopen
 * (ws_close destroys w's own lock, ws_connect makes it anew) */
static pthread_mutex_t wslk = PTHREAD_MUTEX_INITIALIZER; static int wsopen;
static char text_[200], host[200]; static int port, schema;
static unsigned up_gen;                                     /* the gen the session up runs for: host and token belong together */
static char self[96], queue[96], last_ma[260], last_pq[400];
static int next_id;

enum { K_LIST = 1, K_CMD, K_GROUP, K_PLAYERS, K_QUEUE, K_AUTH };
struct pend { int id, kind, ui_req, offset, paged, sent; char what[24]; char fallback[1300]; };   /* sent: items told of a listing in pieces */
static struct pend pend[NPEND];

/* the players as last told, and what the UI was told of each */
struct player {
    char id[96], provider[64], name[64], state[12], synced[96], group[96], playing[160], members[600], can[2400], outs[300];
    int vol, gvol, muted, avail, shown, is_group, volctl, told;   /* gvol: a group's (its players' average), -1 none */
    unsigned long long line;                                /* what the UI was told: its line's hash */
};
static struct player pl[NPL]; static int npl;

struct imgkey { char key[12]; char proxy[96]; char path[520]; char provider[64]; unsigned at; };
static struct imgkey imgs[NIMG]; static unsigned img_at;     /* a few slots from the key's own, the least recent goes */
static struct { char key[12]; int size; } iq[64]; static int iqn;

static void ui(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void ui(const char *fmt, ...)
{
    char b[4096]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    screen_line(b);
}

static void wipe(void *p, size_t n) { volatile char *c = p; while (n--) *c++ = 0; }   /* the token, not left on the stack */

/* free text on one line of the UI's: no tabs, no line ends */
static void clean(char *s) { for (; *s; s++) if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' '; }

static void state_path(char *out, size_t n)
{
    const char *d = getenv("HASSMIC_STATE");
    snprintf(out, n, "%s/mass", d ? d : "/data/local/hassmic/state");
}

static void load(void)
{
    char p[320], line[2200]; state_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "url=", 4)) snprintf(url, sizeof url, "%.*s", (int)sizeof url - 1, line + 4);
        else if (!strncmp(line, "token=", 6)) snprintf(token, sizeof token, "%.*s", (int)sizeof token - 1, line + 6);
    }
    wipe(line, sizeof line);
    fclose(f);
}

static int save(void)                   /* lk held */
{
    char p[320], b[2400]; state_path(p, sizeof p);
    int n = snprintf(b, sizeof b, "url=%s\ntoken=%s\n", url, token);
    int rc = statefile_write(p, 0600, b, (size_t)n);
    wipe(b, sizeof b);
    return rc;
}

static void set_state(int st, const char *fmt, ...)       /* lk held */
{
    static const char *names[] = { "off", "connecting", "up", "auth", "error" };
    va_list ap; va_start(ap, fmt); vsnprintf(text_, sizeof text_, fmt, ap); va_end(ap);
    clean(text_); state = st;
    snprintf(last_ma, sizeof last_ma, "ma %s %s", names[st], text_);
    screen_line(last_ma);
}

/* ---------------------------------------------------------------- commands */

static struct pend *pend_of(int id);

static int ws_put(const char *m, size_t n)
{
    pthread_mutex_lock(&wslk);
    int rc = wsopen ? ws_send(&w, WS_TEXT, m, n) : -1;
    pthread_mutex_unlock(&wslk);
    return rc;
}

/* the receive timeout (HASSMIC_MASS_QUIET_S for the tests); a ping goes out every third of it */
static int quiet_s(void) { const char *e = getenv("HASSMIC_MASS_QUIET_S"); int v = e ? atoi(e) : 90; return v > 2 ? v : 90; }

static void ws_ping(void)
{
    pthread_mutex_lock(&wslk);
    if (wsopen) ws_send(&w, WS_PING, "hm", 2);
    pthread_mutex_unlock(&wslk);
}

static void ws_shut(void)
{
    pthread_mutex_lock(&wslk);
    if (wsopen) { wsopen = 0; ws_close(&w); }
    pthread_mutex_unlock(&wslk);
}

/* a command; how (kind 0: no answer wanted) is what its answer is for, filled in before it goes out (an answer may come
 * back before send returns) */
static int send_how(const struct pend *how, const char *command, const char *args)
{
    char *m = malloc(strlen(args) + 300); int id;
    if (!m) return -1;
    pthread_mutex_lock(&lk);
    id = ++next_id;
    if (how && how->kind) {
        int k = 0; while (k < NPEND && pend[k].id) k++;
        if (k == NPEND) {                                     /* answers that never came: the oldest go (a list */
            k = id % NPEND;                                   /* the UI waits for: ended, it may be asked again) */
            if (pend[k].kind == K_LIST) ui("E %d no answer", pend[k].ui_req);
        }
        pend[k] = *how; pend[k].id = id;
    }
    pthread_mutex_unlock(&lk);
    int n = sprintf(m, "{\"message_id\":\"%d\",\"command\":\"%s\",\"args\":%s}", id, command, args);
    int rc = ws_put(m, (size_t)n);
    wipe(m, (size_t)n); free(m);                               /* the login's has the token */
    if (rc && how && how->kind) { pthread_mutex_lock(&lk); struct pend *p = pend_of(id); if (p) p->id = 0; pthread_mutex_unlock(&lk); }   /* never sent */
    return rc ? -1 : id;
}

static int send_cmd(int kind, int ui_req, const char *command, const char *args)
{
    struct pend how; memset(&how, 0, sizeof how); how.kind = kind; how.ui_req = ui_req;
    return send_how(&how, command, args);
}

static struct pend *pend_of(int id)                        /* lk held; ids start at 1: 0 is a free slot */
{
    if (id <= 0) return NULL;
    for (int k = 0; k < NPEND; k++) if (pend[k].id == id) return &pend[k];
    return NULL;
}

/* "library://album/12" -> item 12 of library */
static int uri_parts(const char *uri, char *prov, size_t pn, char *item, size_t in)
{
    const char *s = strstr(uri, "://");
    if (!s) return -1;
    snprintf(prov, pn, "%.*s", (int)(s - uri), uri);
    const char *t = strchr(s + 3, '/');
    if (!t || !t[1]) return -1;
    snprintf(item, in, "%s", t + 1);
    return 0;
}

/* ---------------------------------------------------------------- what an item shows */

static unsigned long long fnv(const char *a, const char *b)
{
    unsigned long long h = 1469598103934665603ULL;
    for (; *a; a++) h = (h ^ (unsigned char)*a) * 1099511628211ULL;
    h = (h ^ 0x1F) * 1099511628211ULL;
    for (; b && *b; b++) h = (h ^ (unsigned char)*b) * 1099511628211ULL;
    return h;
}

/* a key's place: one of 8 slots from the one its low bits name; make: else a free one, else the least recent.
 * NULL: not there.  lk held */
static struct imgkey *img_slot(const char *key, int make)
{
    unsigned long long h = strtoull(key, NULL, 16); struct imgkey *old = NULL;
    for (int i = 0; i < 8; i++) {
        struct imgkey *e = &imgs[(h + (unsigned)i) % NIMG];
        if (!strcmp(e->key, key)) return e;
        if (make && (!old || (old->key[0] && (!e->key[0] || e->at < old->at)))) old = e;
    }
    return old;
}

/* the image of a MediaItemImage object as a short key the UI asks for again; "" none */
static void image_key(struct json *j, int im, char *key)
{
    char path[520] = "", prov[64] = "", proxy[96] = "";
    key[0] = 0;
    if (json_type(j, im) != J_OBJ) return;
    json_str(j, json_get(j, im, "path"), path, sizeof path);
    json_str(j, json_get(j, im, "provider"), prov, sizeof prov);
    json_str(j, json_get(j, im, "proxy_id"), proxy, sizeof proxy);
    if (!proxy[0] && (!path[0] || strlen(path) >= sizeof path - 1)) return;
    unsigned long long h = proxy[0] ? fnv(proxy, NULL) : fnv(path, prov);
    snprintf(key, 12, "%010llx", h & 0xFFFFFFFFFFULL);
    pthread_mutex_lock(&lk);
    struct imgkey *e = img_slot(key, 1);
    snprintf(e->key, sizeof e->key, "%s", key); snprintf(e->proxy, sizeof e->proxy, "%s", proxy);
    snprintf(e->path, sizeof e->path, "%s", path); snprintf(e->provider, sizeof e->provider, "%s", prov);
    e->at = ++img_at;
    pthread_mutex_unlock(&lk);
}

/* an item's picture: its own thumb, else its album's, else (a queue item) the one the queue has */
static void item_image(struct json *j, int it, char *key)
{
    int md = json_get(j, it, "metadata"), ims = json_get(j, md, "images");
    key[0] = 0;
    JSON_EACH(j, ims, im) {
        if (json_streq(j, json_get(j, im, "type"), "thumb")) { image_key(j, im, key); if (key[0]) return; }
    }
    image_key(j, json_get(j, it, "image"), key);
    if (!key[0]) image_key(j, json_get(j, json_get(j, it, "album"), "image"), key);
}

static void names(struct json *j, int arr, char *out, size_t n)    /* "A, B" of a list of objects with "name" (or strings) */
{
    size_t o = 0; out[0] = 0;
    JSON_EACH(j, arr, a) {
        char nm[128] = "";
        if (json_type(j, a) == J_STR) json_str(j, a, nm, sizeof nm); else json_str(j, json_get(j, a, "name"), nm, sizeof nm);
        if (!nm[0]) continue;
        o += (size_t)snprintf(out + o, o < n ? n - o : 0, "%s%s", o ? ", " : "", nm);
        if (o >= n) { out[n - 1] = 0; break; }
    }
}

static char kind_of(const char *mt)
{
    static const struct { const char *t; char k; } m[] = { { "track", 't' }, { "album", 'a' }, { "artist", 'r' }, { "playlist", 'p' },
        { "radio", 'o' }, { "podcast", 'c' }, { "podcast_episode", 'e' }, { "audiobook", 'b' }, { "folder", 'f' } };
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) if (!strcmp(m[i].t, mt)) return m[i].k;
    return 't';
}

/* one item of a list as the UI's "I" line; ctx: what the list is (the subtitle leaves out what the page says) */
static void item_line(int req, struct json *j, int it, const char *ctx, int index, int current)
{
    char mt[24] = "", uri[220] = "", name[200] = "", ver[64] = "", sub[260] = "", img[12], who[200] = "", alb[160] = "", flags[8] = "";
    int queue_item = json_get(j, it, "queue_item_id") >= 0, src = it;
    if (queue_item) {                                           /* a queue item: its track inside, its own id to act on */
        int mi = json_get(j, it, "media_item");
        json_str(j, json_get(j, it, "queue_item_id"), uri, sizeof uri);
        if (json_type(j, mi) == J_OBJ) src = mi;
        snprintf(mt, sizeof mt, "queue");
    } else {
        json_str(j, json_get(j, it, "media_type"), mt, sizeof mt);
        char whole[640] = ""; json_str(j, json_get(j, it, "uri"), whole, sizeof whole);
        if (strlen(whole) < sizeof uri) snprintf(uri, sizeof uri, "%s", whole);   /* else none: shown, not playable (cut, it would be another) */
    }
    json_str(j, json_get(j, src, "name"), name, sizeof name);
    if (!name[0]) json_str(j, json_get(j, it, "name"), name, sizeof name);
    json_str(j, json_get(j, src, "version"), ver, sizeof ver);
    if (ver[0] && strlen(name) + strlen(ver) + 4 < sizeof name) { strcat(name, " ("); strcat(name, ver); strcat(name, ")"); }
    names(j, json_get(j, src, "artists"), who, sizeof who);
    if (!who[0]) names(j, json_get(j, src, "authors"), who, sizeof who);
    json_str(j, json_get(j, json_get(j, src, "album"), "name"), alb, sizeof alb);
    char k = queue_item ? 'q' : kind_of(mt);
    long long year = json_int(j, json_get(j, src, "year"), 0);
    if (k == 't' || k == 'q') {
        if (alb[0] && strcmp(ctx, "album_tracks")) snprintf(sub, sizeof sub, "%s%s%s", who, who[0] ? " · " : "", alb);
        else snprintf(sub, sizeof sub, "%s", who);
    } else if (k == 'a') {
        if (year > 0 && strcmp(ctx, "artist_albums")) snprintf(sub, sizeof sub, "%s%s%lld", who, who[0] ? " · " : "", year);
        else if (year > 0) snprintf(sub, sizeof sub, "%lld", year);
        else snprintf(sub, sizeof sub, "%s", who);
    } else if (k == 'p') json_str(j, json_get(j, src, "owner"), sub, sizeof sub);
    else if (k == 'e') json_str(j, json_get(j, json_get(j, src, "podcast"), "name"), sub, sizeof sub);
    else if (k == 'b') snprintf(sub, sizeof sub, "%s", who);
    if (queue_item && !name[0]) json_str(j, json_get(j, it, "name"), name, sizeof name);
    item_image(j, queue_item ? it : src, img);
    if (!img[0] && queue_item) item_image(j, src, img);
    long long dur = json_int(j, json_get(j, queue_item ? it : src, "duration"), 0);
    if (current) strcat(flags, "c");
    if (json_type(j, json_get(j, it, "available")) == J_FALSE) strcat(flags, "x");
    clean(name); clean(sub); clean(uri);
    (void)index;
    ui("I %d %c\t%s\t%s\t%s\t%s\t%lld\t%s", req, k, uri, name, sub, img, dur > 0 ? dur : 0, flags);
}

/* ---------------------------------------------------------------- players */

static struct player *player_of(const char *id)            /* lk held */
{
    for (int i = 0; i < npl; i++) if (!strcmp(pl[i].id, id)) return &pl[i];
    return NULL;
}

static int in_csv(const char *list, const char *id);

/* this Echo: Sendspin's client id, or a player that has it as one of its outputs.  Music Assistant may have both (the
 * protocol player and the one around it): the one shown wins, and once picked it stays (each one's player_updated
 * would flip it every second of play).  sendspin.c reads its id at start, which may come after the players did. */
static int self_rank(const struct player *e, const char *cid)
{
    if (!cid || !cid[0] || (strcmp(e->id, cid) && !in_csv(e->outs, cid))) return 0;
    return e->shown > 0 ? 2 : 1;
}

static struct player *me(void)                              /* lk held */
{
    const char *cid = sendspin_client_id();
    struct player *best = self[0] ? player_of(self) : NULL; int br = best ? self_rank(best, cid) : 0;
    for (int i = 0; i < npl; i++) { int r = self_rank(&pl[i], cid); if (r > br) { best = &pl[i]; br = r; } }
    if (best && strcmp(best->id, self)) snprintf(self, sizeof self, "%.95s", best->id);
    return best;
}

static int in_csv(const char *list, const char *id)
{
    size_t n = strlen(id);
    if (!n) return 0;
    for (const char *p = list; (p = strstr(p, id)); p += n) if ((p == list || p[-1] == ',') && (p[n] == ',' || !p[n])) return 1;
    return 0;
}

static void csv(struct json *j, int arr, char *out, size_t n)
{
    size_t o = 0; out[0] = 0;
    JSON_EACH(j, arr, a) {
        char v[128] = "";
        if (json_type(j, a) == J_OBJ) json_str(j, json_get(j, a, "output_protocol_id"), v, sizeof v); else json_str(j, a, v, sizeof v);
        if (!v[0] || o + strlen(v) + 2 >= n) continue;
        o += (size_t)snprintf(out + o, n - o, "%s%s", o ? "," : "", v);
    }
}

static void player_gone(const char *id);

/* speakers only: not protocol players (under a universal one, hidden in Music Assistant's own UI), displays, lights,
 * sources, nor private ones (never a target for other clients) */
static int speaker_shown(struct json *j, int p, const char *ty)
{
    int speaker = !ty[0] || !strcmp(ty, "player") || !strcmp(ty, "stereo_pair") || !strcmp(ty, "group") || !strcmp(ty, "unknown");
    return speaker && json_bool(j, json_get(j, p, "enabled"), 1) && !json_bool(j, json_get(j, p, "hide_in_ui"), 0)
           && !json_bool(j, json_get(j, p, "private"), 0);
}

/* a Player object into the table; lk held */
static void player_put(struct json *j, int p)
{
    char id[96] = "";
    json_str(j, json_get(j, p, "player_id"), id, sizeof id);
    if (!id[0]) return;
    struct player *e = player_of(id);
    if (!e) {
        /* new: kept if the UI shows it or it may be this Echo (Music Assistant 2.10 has two or three players a device,
         * most of them protocol players nobody sees); with the table full, this Echo in place of one not shown */
        const char *cid = sendspin_client_id(); char outs[300], ty[24] = "";
        csv(j, json_get(j, p, "output_protocols"), outs, sizeof outs);
        json_str(j, json_get(j, p, "type"), ty, sizeof ty);
        int mine = cid && cid[0] && (!strcmp(id, cid) || in_csv(outs, cid));
        if (!mine && !speaker_shown(j, p, ty)) return;
        static const int order[3] = { 0, -1, 1 };              /* one hidden first, then one not in this answer yet, then
                                                                   one shown (past 128 speakers: this Echo before them) */
        for (int k = 0; npl == NPL && mine && k < 3; k++)
            for (int i = npl - 1; i >= 0 && npl == NPL; i--) if (pl[i].shown == order[k] && strcmp(pl[i].id, self)) player_gone(pl[i].id);
        if (npl == NPL) return;
        e = &pl[npl++]; memset(e, 0, sizeof *e); snprintf(e->id, sizeof e->id, "%s", id);
    }
    json_str(j, json_get(j, p, "provider"), e->provider, sizeof e->provider);
    if (!json_str(j, json_get(j, p, "name"), e->name, sizeof e->name) || !e->name[0]) json_str(j, json_get(j, p, "display_name"), e->name, sizeof e->name);
    if (!json_str(j, json_get(j, p, "playback_state"), e->state, sizeof e->state)) json_str(j, json_get(j, p, "state"), e->state, sizeof e->state);
    json_str(j, json_get(j, p, "synced_to"), e->synced, sizeof e->synced);
    json_str(j, json_get(j, p, "active_group"), e->group, sizeof e->group);
    csv(j, json_get(j, p, "group_members"), e->members, sizeof e->members);
    if (!e->members[0]) csv(j, json_get(j, p, "group_childs"), e->members, sizeof e->members);   /* before 2.4 */
    csv(j, json_get(j, p, "can_group_with"), e->can, sizeof e->can);
    csv(j, json_get(j, p, "output_protocols"), e->outs, sizeof e->outs);
    e->vol = (int)json_int(j, json_get(j, p, "volume_level"), -1);
    e->gvol = (int)json_int(j, json_get(j, p, "group_volume"), -1);   /* (a leader's, a group player's: not its own) */
    e->muted = json_bool(j, json_get(j, p, "volume_muted"), 0);
    e->avail = json_bool(j, json_get(j, p, "available"), 1);
    char ty[24] = ""; json_str(j, json_get(j, p, "type"), ty, sizeof ty); e->is_group = !strcmp(ty, "group");
    e->shown = speaker_shown(j, p, ty);
    char vc[24] = ""; json_str(j, json_get(j, p, "volume_control"), vc, sizeof vc);
    e->volctl = e->vol >= 0 && strcmp(vc, "none");
    int cm = json_get(j, p, "current_media"); char t[128] = "", a[96] = "";
    json_str(j, json_get(j, cm, "title"), t, sizeof t); json_str(j, json_get(j, cm, "artist"), a, sizeof a);
    snprintf(e->playing, sizeof e->playing, "%s%s%s", t, t[0] && a[0] ? " · " : "", a);
}

/* every player's line again, those that changed written (who may join this Echo's group goes by its leader) */
static void players_tell(int all)          /* lk held */
{
    struct player *m = me(), *ld = m && m->synced[0] ? player_of(m->synced) : m;
    for (int i = 0; i < npl; i++) {
        struct player *e = &pl[i]; char f[12] = "", line[1400];
        if (!e->shown && e != m) { if (e->told) { ui("pl- %s", e->id); e->told = 0; e->line = 0; } continue; }   /* this Echo always */
        if (e == m) strcat(f, "s");
        if (ld && e != m && e != ld && (in_csv(ld->can, e->id) || in_csv(ld->can, e->provider))) strcat(f, "g");
        if (e->volctl) strcat(f, "v");
        if (e->muted) strcat(f, "m");
        if (e->avail) strcat(f, "a");
        if (e->is_group) strcat(f, "l");
        char gv[12] = ""; if (e->gvol >= 0 && (e->members[0] || e->is_group)) snprintf(gv, sizeof gv, "%d", e->gvol);
        snprintf(line, sizeof line, "pl %.95s\t%.63s\t%.11s\t%.11s\t%d\t%.95s\t%.599s\t%.159s\t%s", e->id, e->name, f, e->state, e->vol < 0 ? 0 : e->vol,
                 e->synced, e->members, e->playing, gv);
        line[1199] = 0;
        for (char *c = line + 3; *c; c++) if (*c == '\n' || *c == '\r') *c = ' ';
        unsigned long long hl = fnv(line, NULL);
        if (all || hl != e->line) { e->line = hl; screen_line(line); e->told = 1; }
    }
}

static void player_gone(const char *id)    /* lk held */
{
    for (int i = 0; i < npl; i++) if (!strcmp(pl[i].id, id)) {
        if (pl[i].told) ui("pl- %s", id);
        if (!strcmp(id, self)) {                               /* this Echo gone from Music Assistant: no queue of its own */
            self[0] = queue[0] = 0; snprintf(last_pq, sizeof last_pq, "pq-"); screen_line(last_pq);
        }
        memmove(&pl[i], &pl[i + 1], (size_t)(npl - i - 1) * sizeof pl[0]); npl--;
        return;
    }
}

/* this Echo's queue: asked for whenever who it plays with may have changed */
static void ask_queue(void)
{
    char a[700], e[600]; pthread_mutex_lock(&lk); int ok = self[0] != 0; json_esc(e, sizeof e, self); pthread_mutex_unlock(&lk);
    if (!ok) return;
    snprintf(a, sizeof a, "{\"player_id\":%s}", e);
    send_cmd(K_QUEUE, 0, "player_queues/get_active_queue", a);
}

static void queue_put(struct json *j, int q)               /* lk held */
{
    char id[96] = "", st[16] = "", rp[8] = "", name[96] = "";
    if (json_type(j, q) != J_OBJ) { queue[0] = 0; snprintf(last_pq, sizeof last_pq, "pq-"); screen_line(last_pq); return; }
    json_str(j, json_get(j, q, "queue_id"), id, sizeof id);
    json_str(j, json_get(j, q, "state"), st, sizeof st);
    json_str(j, json_get(j, q, "repeat_mode"), rp, sizeof rp);
    json_str(j, json_get(j, q, "display_name"), name, sizeof name); clean(name);
    snprintf(queue, sizeof queue, "%s", id);
    char line[sizeof last_pq];
    snprintf(line, sizeof line, "pq %s\t%d\t%s\t%lld\t%lld\t%s", st, json_bool(j, json_get(j, q, "shuffle_enabled"), 0), rp[0] ? rp : "off",
             json_int(j, json_get(j, q, "current_index"), -1), json_int(j, json_get(j, q, "items"), 0), name);
    if (strcmp(line, last_pq)) { snprintf(last_pq, sizeof last_pq, "%s", line); screen_line(line); }
}

/* ---------------------------------------------------------------- answers and events */

/* last 0: a piece of a long listing (Music Assistant's async generators answer 500 items a message, "partial"): its
 * items told at once, one L ahead of all of them, the total left unknown */
static void on_list(struct pend *p, struct json *j, int r, int last)
{
    int n = 0, cur = -1;
    if (!strcmp(p->what, "search")) {                           /* one object of lists: in the order the UI groups them */
        static const char *keys[] = { "artists", "albums", "tracks", "playlists", "radio", "podcasts", "audiobooks" };
        int total = 0;
        for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++) total += json_len(j, json_get(j, r, keys[k]));
        ui("L %d 0 %d", p->ui_req, total);
        for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++) JSON_EACH(j, json_get(j, r, keys[k]), it) item_line(p->ui_req, j, it, "search", n++, 0);
        ui("E %d", p->ui_req);
        return;
    }
    if (json_type(j, r) == J_OBJ && json_get(j, r, "items") >= 0) r = json_get(j, r, "items");   /* a paged object */
    int len = json_len(j, r);
    if (!strcmp(p->what, "queue")) {                            /* the current index: pq's fourth field */
        pthread_mutex_lock(&lk);
        char *c = strchr(last_pq, '\t'); if (c && (c = strchr(c + 1, '\t')) && (c = strchr(c + 1, '\t'))) cur = atoi(c + 1);
        pthread_mutex_unlock(&lk);
    }
    /* the total: known when a page came short of a full one */
    if (!p->sent) ui("L %d %d %d", p->ui_req, p->offset, !last || (p->paged && len >= PAGE) ? -1 : p->offset + len);
    int at = p->offset + p->sent;
    JSON_EACH(j, r, it) { item_line(p->ui_req, j, it, p->what, at + n, at + n == cur); n++; }
    if (last) ui("E %d", p->ui_req);
}

static void on_partial(int id, struct json *j, int r)
{
    pthread_mutex_lock(&lk);
    struct pend *pp = pend_of(id), p;
    if (!pp || pp->kind != K_LIST || !strcmp(pp->what, "search")) { pthread_mutex_unlock(&lk); return; }
    p = *pp;
    int its = json_type(j, r) == J_OBJ && json_get(j, r, "items") >= 0 ? json_get(j, r, "items") : r;
    pp->sent += json_len(j, its);
    pthread_mutex_unlock(&lk);
    on_list(&p, j, r, 0);
}

static void on_result(int id, struct json *j, int r, const char *err)
{
    pthread_mutex_lock(&lk);
    struct pend *pp = pend_of(id), p;
    if (!pp) { pthread_mutex_unlock(&lk); return; }
    p = *pp; pp->id = 0;
    pthread_mutex_unlock(&lk);
    switch (p.kind) {
    case K_LIST:
        if (err) { char e[200]; snprintf(e, sizeof e, "%s", err); clean(e); ui("E %d %s", p.ui_req, e); }
        else on_list(&p, j, r, 1);
        break;
    case K_GROUP:                                               /* set_members not known (before 2.4): the older way */
        if (err && p.fallback[0]) {
            char *sp = strchr(p.fallback, ' ');
            if (sp) { *sp = 0; send_cmd(K_CMD, 0, p.fallback, sp + 1); }
        } else if (err) ui("merr %s", err);
        break;
    case K_CMD: if (err) { char e[200]; snprintf(e, sizeof e, "%s", err); clean(e); ui("merr %s", e); } break;
    case K_PLAYERS:
        if (err) break;
        pthread_mutex_lock(&lk);
        for (int i = 0; i < npl; i++) pl[i].shown = -1;          /* those not in the answer go */
        JSON_EACH(j, r, it) player_put(j, it);
        for (int i = 0; i < npl; i++) if (pl[i].shown == -1) { player_gone(pl[i].id); i--; }
        players_tell(1);
        screen_line("pl.");
        if (!self[0]) fprintf(stderr, "mass: %d players, none of them this Echo (%s) yet\n", npl, sendspin_client_id());
        pthread_mutex_unlock(&lk);
        ask_queue();
        break;
    case K_QUEUE:
        pthread_mutex_lock(&lk); if (!err) queue_put(j, r); pthread_mutex_unlock(&lk);
        break;
    }
}

static void on_event(struct json *j, int root)
{
    char ev[40] = "", obj[96] = "";
    json_str(j, json_get(j, root, "event"), ev, sizeof ev);
    json_str(j, json_get(j, root, "object_id"), obj, sizeof obj);
    int d = json_get(j, root, "data");
    if (!strcmp(ev, "player_added") || !strcmp(ev, "player_updated")) {
        pthread_mutex_lock(&lk);
        char was[96], ws_[96], wg[96]; snprintf(was, sizeof was, "%s", self);
        struct player *m = me();
        snprintf(ws_, sizeof ws_, "%s", m ? m->synced : ""); snprintf(wg, sizeof wg, "%s", m ? m->group : "");
        player_put(j, d);
        m = me();
        int moved = strcmp(was, self) || (m && (strcmp(ws_, m->synced) || strcmp(wg, m->group)));
        players_tell(moved);
        pthread_mutex_unlock(&lk);
        if (moved) ask_queue();
    } else if (!strcmp(ev, "player_removed")) {
        pthread_mutex_lock(&lk); player_gone(obj); pthread_mutex_unlock(&lk);
    } else if (!strcmp(ev, "queue_updated")) {
        pthread_mutex_lock(&lk); if (queue[0] && !strcmp(obj, queue)) queue_put(j, d); pthread_mutex_unlock(&lk);
    } else if (!strcmp(ev, "queue_items_updated")) {
        pthread_mutex_lock(&lk); int mine = queue[0] && !strcmp(obj, queue); pthread_mutex_unlock(&lk);
        if (mine) screen_line("qc");
    }
}

/* ---------------------------------------------------------------- the connection */

/* where to: the page's URL (http://host:port, ws:// alike, or host:port), else, without a token, Sendspin's server on
 * 8095 (only to say what it is and that a token is wanted: a token goes to the address it was saved with alone, never
 * to one a Sendspin session suggested, which any host on the network can) */
static int stopped;                                         /* mass_stop: no session any more (lk) */

static int target(char *h, size_t hn, int *p)               /* lk held */
{
    const char *u = stopped ? "" : url[0] ? url : token[0] ? "" : peer;
    if (!u[0] || !strncmp(u, "https://", 8) || !strncmp(u, "wss://", 6)) return -1;   /* (mass_set refuses them) */
    const char *s = strstr(u, "://"); s = s ? s + 3 : u;
    *p = 8095;
    if (*s == '[') {                                           /* [v6]:port */
        const char *e = strchr(s, ']');
        if (!e) return -1;
        snprintf(h, hn, "%.*s", (int)(e - s - 1), s + 1);
        if (e[1] == ':') *p = atoi(e + 2);
        return 0;
    }
    size_t n = strcspn(s, ":/");
    snprintf(h, hn, "%.*s", (int)n, s);
    if (s[n] == ':') *p = atoi(s + n + 1);
    return h[0] && *p > 0 ? 0 : -1;
}

/* waits up to ms for a new configuration (or for ever, ms < 0); 1 if it came.  lk held, as every condition wait */
static int wait_change(unsigned seen, int ms)
{
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000; ts.tv_nsec += (ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    while (gen == seen) if ((ms < 0 ? pthread_cond_wait(&cv, &lk) : pthread_cond_timedwait(&cv, &lk, &ts)) == ETIMEDOUT) return 0;
    return 1;
}

static int was_up;                                          /* the last session logged in (the session's thread) */

static void session(unsigned seen)
{
    int op; uint8_t *d; size_t n; struct json j = { 0 }; char h[200], tok[2048]; int p;
    pthread_mutex_lock(&lk);
    int ok = !target(h, sizeof h, &p);
    snprintf(tok, sizeof tok, "%s", token);
    if (ok) { snprintf(host, sizeof host, "%s", h); port = p; set_state(1, "%s:%d", h, p); }
    pthread_mutex_unlock(&lk);
    if (!ok) { wipe(tok, sizeof tok); return; }
    if (ws_connect(&w, h, p, "/ws")) {
        wipe(tok, sizeof tok);
        pthread_mutex_lock(&lk); set_state(4, "%s:%d does not answer", h, p); pthread_mutex_unlock(&lk);
        return;
    }
    /* Music Assistant pings every 25 s (aiohttp's heartbeat; ws.c answers), and so does the image thread (its own client
     * pings too: a server without a heartbeat, or a proxy between, answers): silence this long is a server gone */
    struct timeval tv = { quiet_s(), 0 }; setsockopt(w.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    pthread_mutex_lock(&wslk); wsopen = 1; pthread_mutex_unlock(&wslk);
    /* the server's info first */
    if (ws_recv(&w, &op, &d, &n) != 1 || json_parse(&j, (char *)d, n) || json_get(&j, 0, "schema_version") < 0) {
        wipe(tok, sizeof tok);
        pthread_mutex_lock(&lk); set_state(4, "%s:%d is not Music Assistant", h, p); pthread_mutex_unlock(&lk);
        json_free(&j); ws_shut(); return;
    }
    char ver[40] = ""; json_str(&j, json_get(&j, 0, "server_version"), ver, sizeof ver);
    schema = (int)json_int(&j, json_get(&j, 0, "schema_version"), 0);
    fprintf(stderr, "mass: Music Assistant %s (schema %d) at %s:%d\n", ver, schema, h, p);
    if (schema >= 28) {                                         /* a login, before anything else answers */
        char e[6 * 2048 + 3], a[sizeof e + 20];
        if (!tok[0]) {
            json_free(&j); ws_shut();
            pthread_mutex_lock(&lk); set_state(0, "a token is needed"); wait_change(seen, -1); pthread_mutex_unlock(&lk);
            return;
        }
        json_esc(e, sizeof e, tok); snprintf(a, sizeof a, "{\"token\":%s}", e);
        int id = send_cmd(0, 0, "auth", a);
        int good = 0, answered = 0;
        for (int tries = 0; tries < 20 && ws_recv(&w, &op, &d, &n) == 1; tries++) {
            json_free(&j);
            if (json_parse(&j, (char *)d, n)) continue;
            char mid[16] = ""; json_str(&j, json_get(&j, 0, "message_id"), mid, sizeof mid);
            if (atoi(mid) != id) continue;
            int r = json_get(&j, 0, "result"), t = json_type(&j, r); char one[2] = "";   /* as its own client: anything falsy is no */
            if (t == J_STR) json_str(&j, r, one, sizeof one);
            good = json_get(&j, 0, "error_code") < 0 && t > 0 && t != J_NULL && t != J_FALSE && !((t == J_OBJ || t == J_ARR) && !json_len(&j, r))
                   && !(t == J_NUM && json_num(&j, r, 0) == 0) && !(t == J_STR && !one[0]); answered = 1;
            break;
        }
        wipe(e, sizeof e); wipe(a, sizeof a);
        if (!answered) {                                       /* closed, or no answer (setup not done, a new address) */
            wipe(tok, sizeof tok); json_free(&j); ws_shut();
            pthread_mutex_lock(&lk); set_state(4, "%s:%d did not answer the login", h, p); pthread_mutex_unlock(&lk);
            return;
        }
        if (!good) {
            fprintf(stderr, "mass: the token was refused\n");
            wipe(tok, sizeof tok); json_free(&j); ws_shut();
            pthread_mutex_lock(&lk); set_state(3, "the token was refused"); wait_change(seen, 600000); pthread_mutex_unlock(&lk);
            return;
        }
    }
    wipe(tok, sizeof tok);
    pthread_mutex_lock(&lk); up = 1; up_gen = seen; set_state(2, "Music Assistant %s", ver); pthread_mutex_unlock(&lk);
    was_up = 1;
    send_cmd(K_PLAYERS, 0, "players/all", "{}");
    int got;
    while ((got = ws_recv(&w, &op, &d, &n)) >= 1) {
        pthread_mutex_lock(&lk); int changed = gen != seen; pthread_mutex_unlock(&lk);
        if (changed) break;
        if (op != WS_TEXT) continue;
        if (got == 2) {                                         /* beyond 4 MB (a whole listing of a big artist, before
                                                                   paging): that answer an error, the session goes on */
            const char *mi = strstr((char *)d, "\"message_id\"");
            int id = mi ? atoi(mi + 12 + strspn(mi + 12, " :\"")) : 0;
            fprintf(stderr, "mass: an answer too big for this Echo (message %d), left out\n", id);
            if (id) on_result(id, &j, -1, "too long for this Echo");
            continue;
        }
        json_free(&j);
        if (json_parse(&j, (char *)d, n)) {                     /* not JSON, or too much of it: that answer an error */
            const char *mi = strstr((char *)d, "\"message_id\""); int id = mi ? atoi(mi + 12 + strspn(mi + 12, " :\"")) : 0;
            fprintf(stderr, "mass: %zu bytes that are not JSON (or too many values) for message %d\n", n, id);
            if (id) on_result(id, &j, -1, "too long for this Echo");
            continue;
        }
        if (json_get(&j, 0, "event") >= 0) { on_event(&j, 0); continue; }
        char mid[16] = ""; json_str(&j, json_get(&j, 0, "message_id"), mid, sizeof mid);
        if (!mid[0]) continue;
        if (json_get(&j, 0, "error_code") >= 0) {
            char e[200] = ""; json_str(&j, json_get(&j, 0, "details"), e, sizeof e);
            on_result(atoi(mid), &j, -1, e[0] ? e : "error");
        } else if (json_bool(&j, json_get(&j, 0, "partial"), 0)) on_partial(atoi(mid), &j, json_get(&j, 0, "result"));
        else on_result(atoi(mid), &j, json_get(&j, 0, "result"), NULL);
    }
    json_free(&j);
    ws_shut();                                                  /* first: a request after this fails (send_how), none waits for ever */
    pthread_mutex_lock(&lk);
    up = 0; self[0] = queue[0] = 0; last_pq[0] = 0;
    for (int i = 0; i < npl; i++) if (pl[i].told) ui("pl- %s", pl[i].id);
    npl = 0;
    for (int k = 0; k < NPEND; k++) if (pend[k].id && pend[k].kind == K_LIST) ui("E %d connection lost", pend[k].ui_req);
    memset(pend, 0, sizeof pend);
    set_state(4, "the connection to %s:%d ended", h, p);
    pthread_mutex_unlock(&lk);
}

static void *thread_main(void *arg)
{
    (void)arg; int fails = 0;
    for (;;) {
        pthread_mutex_lock(&lk);
        unsigned seen = gen; char h[200]; int p, have = !target(h, sizeof h, &p);
        if (!have) { set_state(0, "%s", ""); wait_change(seen, -1); pthread_mutex_unlock(&lk); continue; }
        pthread_mutex_unlock(&lk);
        long long t0 = time(NULL);
        was_up = 0; session(seen);
        if (was_up || time(NULL) - t0 > 60) fails = 0;          /* one that was up: back soon (5 s), not after the refusals' waits */
        int wait = fails < 5 ? 5000 << fails : 120000; fails++;
        pthread_mutex_lock(&lk); wait_change(seen, wait); pthread_mutex_unlock(&lk);
    }
    return NULL;
}

/* ---------------------------------------------------------------- images */

static void urlenc(char *out, size_t n, const char *in)
{
    size_t o = 0;
    for (; *in && o + 4 < n; in++) {
        unsigned char c = (unsigned char)*in;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-_.~", c)) out[o++] = (char)c;
        else o += (size_t)snprintf(out + o, n - o, "%%%02X", c);
    }
    out[o] = 0;
}

/* GET of the imageproxy, HTTP/1.0 (no chunks), the token along where the server wants one */
static uint8_t *http_get(const char *h, int p, const char *path, const char *tok, size_t *len)
{
    char ps[8]; snprintf(ps, sizeof ps, "%d", p);
    int fd = net_connect(h, ps, 5);
    if (fd < 0) return NULL;
    char *req = malloc(strlen(path) + strlen(tok) + 300);
    if (!req) { close(fd); return NULL; }
    int v6 = strchr(h, ':') != NULL;                            /* [v6]:port */
    int rn = sprintf(req, "GET %s HTTP/1.0\r\nHost: %s%s%s:%d\r\n%s%s%sUser-Agent: hassmic\r\n\r\n", path, v6 ? "[" : "", h, v6 ? "]" : "", p,
                     tok[0] ? "Authorization: Bearer " : "", tok, tok[0] ? "\r\n" : "");
    int bad = send(fd, req, (size_t)rn, MSG_NOSIGNAL) != rn;
    wipe(req, (size_t)rn); free(req);
    size_t cap = 65536, have = 0; uint8_t *b = malloc(cap);
    while (!bad && b) {
        if (have == cap) { if (cap >= IMG_MAX + 8192) { bad = 1; break; } uint8_t *nb = realloc(b, cap * 2); if (!nb) { bad = 1; break; } b = nb; cap *= 2; }
        ssize_t r = recv(fd, b + have, cap - have, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        have += (size_t)r;
    }
    close(fd);
    if (bad || !b || have < 12 || memcmp(b, "HTTP/1.", 7) || memcmp(b + 9, "200", 3)) { free(b); return NULL; }
    uint8_t *body = NULL;
    for (size_t i = 0; i + 3 < have; i++) if (!memcmp(b + i, "\r\n\r\n", 4)) { body = b + i + 4; break; }
    if (!body) { free(b); return NULL; }
    *len = have - (size_t)(body - b);
    memmove(b, body, *len);
    return b;
}

static void *image_thread(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&lk);
        while (!iqn) {                                         /* (idle: the session's pings, see session()) */
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += quiet_s() / 3;
            if (pthread_cond_timedwait(&iq_cv, &lk, &ts) == ETIMEDOUT && up) { pthread_mutex_unlock(&lk); ws_ping(); pthread_mutex_lock(&lk); }
        }
        char key[12]; int size = iq[iqn - 1].size;              /* the newest first: what the UI shows now */
        snprintf(key, sizeof key, "%s", iq[iqn - 1].key); iqn--;
        struct imgkey e; memset(&e, 0, sizeof e); struct imgkey *f = img_slot(key, 0); if (f) e = *f;
        /* host and token of one configuration: a new one (gen) may have a token for another host, and the session
         * still up is the old one's until it ends */
        char h[200], tok[2048]; int p = port, ok = up && up_gen == gen && f;
        snprintf(h, sizeof h, "%s", host); snprintf(tok, sizeof tok, "%s", ok ? token : "");
        pthread_mutex_unlock(&lk);
        size_t n = 0; uint8_t *img = NULL;
        if (ok) {
            char path[4400], a[3 * 520 + 8], b[5 * 520 + 8];     /* the path once encoded, twice: 3 and 5 bytes a byte at most */
            if (e.proxy[0]) { urlenc(a, sizeof a, e.proxy); snprintf(path, sizeof path, "/imageproxy/%s?size=%d&fmt=jpeg", a, size); }
            else { urlenc(a, sizeof a, e.path); urlenc(b, sizeof b, a); urlenc(a, sizeof a, e.provider);   /* the path encoded twice, as the frontend does */
                   snprintf(path, sizeof path, "/imageproxy?path=%s&provider=%s&size=%d&fmt=jpeg", b, a, size); }
            img = http_get(h, p, path, tok, &n);
        }
        wipe(tok, sizeof tok);
        char head[64]; snprintf(head, sizeof head, "img %s %zu %d", key, img ? n : 0, size);   /* the size: which of the UI's wants */
        screen_bin(head, img, img ? n : 0);
        free(img);
    }
    return NULL;
}

static void image_ask(const char *key, int size)
{
    static const int sizes[] = { 80, 160, 256, 512 };
    int s = 512;
    for (int i = 0; i < 4; i++) if (sizes[i] >= size) { s = sizes[i]; break; }
    pthread_mutex_lock(&lk);
    for (int i = 0; i < iqn; i++) if (!strcmp(iq[i].key, key) && iq[i].size == s) { pthread_mutex_unlock(&lk); return; }
    if (iqn == (int)(sizeof iq / sizeof iq[0])) { memmove(iq, iq + 1, sizeof iq - sizeof iq[0]); iqn--; }   /* the oldest go */
    snprintf(iq[iqn].key, sizeof iq[iqn].key, "%s", key); iq[iqn].size = s; iqn++;
    pthread_cond_signal(&iq_cv);
    pthread_mutex_unlock(&lk);
}

/* ---------------------------------------------------------------- the UI's requests */

static void list_ask(int req, const char *what, int off, const char *arg)
{
    char a[2400], e1[1300], e2[600], prov[96], item[200], libcmd[48]; const char *cmd = NULL; int paged = 0;
    static const char *lib[] = { "artists", "albums", "playlists", "tracks", "radios", "podcasts", "audiobooks" };
    for (size_t i = 0; i < sizeof lib / sizeof lib[0]; i++)
        if (!strcmp(what, lib[i])) {
            snprintf(libcmd, sizeof libcmd, "music/%s/library_items", what); cmd = libcmd; paged = 1;
            snprintf(a, sizeof a, "{\"limit\":%d,\"offset\":%d,\"order_by\":\"sort_name\"}", PAGE, off);
        }
    if (!cmd && !strcmp(what, "recent")) { cmd = "music/recently_played_items"; snprintf(a, sizeof a, "{\"limit\":40}"); }
    if (!cmd && !strcmp(what, "queue")) {
        pthread_mutex_lock(&lk); json_esc(e1, sizeof e1, queue); int q = queue[0] != 0; pthread_mutex_unlock(&lk);
        if (!q) { ui("L %d 0 0", req); ui("E %d", req); return; }
        cmd = "player_queues/items"; paged = 1;
        snprintf(a, sizeof a, "{\"queue_id\":%s,\"limit\":%d,\"offset\":%d}", e1, PAGE, off);
    }
    if (!cmd && !strcmp(what, "search")) {
        cmd = "music/search"; json_esc(e1, sizeof e1, arg);
        snprintf(a, sizeof a, "{\"search_query\":%s,\"limit\":8,\"media_types\":[\"artist\",\"album\",\"track\",\"playlist\",\"radio\",\"podcast\",\"audiobook\"]}", e1);
    }
    /* paged from that schema on (music_assistant_client 1.7.1's ALBUM_TRACKS_, ARTIST_, PLAYLIST_PODCAST_LISTING_
     * SCHEMA_VERSION); before it the server ignores limit/offset and answers all of it (in pieces of 500), every time */
    static const struct { const char *what, *cmd; int paged_from; } in[] = {
        { "album_tracks", "music/albums/album_tracks", 89 }, { "artist_albums", "music/artists/artist_albums", 92 },
        { "artist_tracks", "music/artists/artist_tracks", 92 }, { "playlist_tracks", "music/playlists/playlist_tracks", 93 },
        { "podcast_episodes", "music/podcasts/podcast_episodes", 93 } };
    for (size_t i = 0; !cmd && i < sizeof in / sizeof in[0]; i++)
        if (!strcmp(what, in[i].what)) {
            if (uri_parts(arg, prov, sizeof prov, item, sizeof item)) { ui("E %d not a Music Assistant item", req); return; }
            int pg = in[i].paged_from && schema >= in[i].paged_from;
            if (off && !pg) { ui("L %d %d %d", req, off, off); ui("E %d", req); return; }   /* all of it came at once */
            json_esc(e1, sizeof e1, item); json_esc(e2, sizeof e2, prov); cmd = in[i].cmd; paged = pg;
            if (paged) snprintf(a, sizeof a, "{\"item_id\":%s,\"provider_instance_id_or_domain\":%s,\"limit\":%d,\"offset\":%d}", e1, e2, PAGE, off);
            else snprintf(a, sizeof a, "{\"item_id\":%s,\"provider_instance_id_or_domain\":%s}", e1, e2);
        }
    if (!cmd) { ui("E %d no such list", req); return; }
    struct pend how; memset(&how, 0, sizeof how);
    how.kind = K_LIST; how.ui_req = req; how.offset = off; how.paged = paged; snprintf(how.what, sizeof how.what, "%s", what);
    if (send_how(&how, cmd, a) < 0) ui("E %d not connected", req);
}

/* bytes that are no UTF-8 as '?': Music Assistant (aiohttp) closes the whole session on a text message that is not */
static void utf8_fix(char *s)
{
    for (unsigned char *p = (unsigned char *)s; *p; ) {
        int n = *p < 0x80 ? 1 : (*p & 0xE0) == 0xC0 && *p >= 0xC2 ? 2 : (*p & 0xF0) == 0xE0 ? 3 : (*p & 0xF8) == 0xF0 && *p <= 0xF4 ? 4 : 0;
        int ok = n > 0;
        for (int k = 1; ok && k < n; k++) ok = (p[k] & 0xC0) == 0x80;
        if (ok && n == 3) ok = !(p[0] == 0xE0 && p[1] < 0xA0) && !(p[0] == 0xED && p[1] >= 0xA0);   /* overlong, surrogates */
        if (ok && n == 4) ok = !(p[0] == 0xF0 && p[1] < 0x90) && !(p[0] == 0xF4 && p[1] >= 0x90);
        if (!ok) { *p++ = '?'; continue; }
        p += n;
    }
}

void mass_ui(const char *line_in)
{
    char line[1100]; snprintf(line, sizeof line, "%s", line_in); utf8_fix(line);   /* (the UI is not trusted) */
    char cmd[16] = "", a1[128] = "", a2[600] = ""; int n = 0;   /* a1: a player's id too (up to 95) */
    if (sscanf(line, "%15s %n", cmd, &n) < 1) return;
    const char *rest = line + n;
    char q[600], me_[600], ld[600], x[6000];
    pthread_mutex_lock(&lk);
    int had = self[0] != 0; struct player *m = me();
    json_esc(q, sizeof q, queue); json_esc(me_, sizeof me_, self);
    json_esc(ld, sizeof ld, m && m->synced[0] ? m->synced : self);
    int has_q = queue[0] != 0, has_me = self[0] != 0, isup = up;
    pthread_mutex_unlock(&lk);
    if (!had && has_me && isup) ask_queue();
    if (!strcmp(cmd, "ml")) {
        int req = 0, off = 0, k = 0; char what[24] = "";
        if (sscanf(rest, "%d %23s %d %n", &req, what, &off, &k) < 3) return;
        if (off < 0 || off > 1000000) { ui("E %d no such place in it", req); return; }   /* (no list is that long: the
                                                                   sums of where its rows go stay ints) */
        if (!isup) { ui("E %d not connected", req); return; }
        list_ask(req, what, off, k ? rest + k : "");
        return;
    }
    if (!strcmp(cmd, "img")) { int size = 0; if (sscanf(rest, "%127s %d", a1, &size) == 2) image_ask(a1, size); return; }
    if (!isup) { ui("merr Music Assistant is not connected"); return; }
    sscanf(rest, "%127s %599[^\n]", a1, a2);
    if (!strcmp(cmd, "mplay")) {
        if (!has_q) { ui("merr This Echo has no queue in Music Assistant yet"); return; }
        int radio = !strcmp(a1, "radio"); char e[3700];
        if (strcmp(a1, "play") && strcmp(a1, "next") && strcmp(a1, "add") && strcmp(a1, "replace") && !radio) return;
        /* radio: from schema 34 a radio playlist of the item (radio_mode is deprecated: music_assistant_client 1.7.1
         * turns it into one itself), before it the flag */
        char seed[700]; int rp = radio && schema >= 34;
        snprintf(seed, sizeof seed, "%s%s", rp && strncmp(a2, "radio_playlist://", 17) ? "radio_playlist://playlist/" : "", a2);
        json_esc(e, sizeof e, seed);
        snprintf(x, sizeof x, "{\"queue_id\":%s,\"media\":%s,\"option\":\"%s\",\"radio_mode\":%s}", q, e, radio ? "play" : a1, radio && !rp ? "true" : "false");
        send_cmd(K_CMD, 0, "player_queues/play_media", x);
    } else if (!strcmp(cmd, "mq")) {
        if (!has_q) return;
        char e[3700]; json_esc(e, sizeof e, a2);
        if (!strcmp(a1, "index")) {                            /* a position, or (the UI's) a queue item's id */
            if (a2[0] && strspn(a2, "0123456789") == strlen(a2)) {
                if (strlen(a2) > 7) return;                     /* (a place no queue has, and atoi's range) */
                snprintf(x, sizeof x, "{\"queue_id\":%s,\"index\":%d}", q, atoi(a2));
            }
            else snprintf(x, sizeof x, "{\"queue_id\":%s,\"index\":%s}", q, e);
            send_cmd(K_CMD, 0, "player_queues/play_index", x);
        }
        else if (!strcmp(a1, "delete")) { snprintf(x, sizeof x, "{\"queue_id\":%s,\"item_id_or_index\":%s}", q, e); send_cmd(K_CMD, 0, "player_queues/delete_item", x); }
        else if (!strcmp(a1, "clear")) { snprintf(x, sizeof x, "{\"queue_id\":%s}", q); send_cmd(K_CMD, 0, "player_queues/clear", x); }
        else if (!strcmp(a1, "shuffle")) { snprintf(x, sizeof x, "{\"queue_id\":%s,\"shuffle_enabled\":%s}", q, atoi(a2) ? "true" : "false"); send_cmd(K_CMD, 0, "player_queues/shuffle", x); }
        else if (!strcmp(a1, "repeat") && (!strcmp(a2, "off") || !strcmp(a2, "one") || !strcmp(a2, "all"))) {
            snprintf(x, sizeof x, "{\"queue_id\":%s,\"repeat_mode\":\"%s\"}", q, a2); send_cmd(K_CMD, 0, "player_queues/repeat", x);
        }
    } else if (!strcmp(cmd, "mg")) {
        if (!has_me) { ui("merr Music Assistant does not know this Echo yet"); return; }
        char e[600]; json_esc(e, sizeof e, a2);
        if (!strcmp(a1, "leave")) { snprintf(x, sizeof x, "{\"player_id\":%s}", me_); send_cmd(K_CMD, 0, "players/cmd/ungroup", x); return; }
        int add = !strcmp(a1, "add");
        if (!add && strcmp(a1, "remove")) return;
        snprintf(x, sizeof x, "{\"target_player\":%s,\"%s\":[%s]}", ld, add ? "player_ids_to_add" : "player_ids_to_remove", e);
        struct pend how; memset(&how, 0, sizeof how); how.kind = K_GROUP;   /* Music Assistant before set_members: one at a time */
        if (add) snprintf(how.fallback, sizeof how.fallback, "players/cmd/group {\"player_id\":%s,\"target_player\":%s}", e, ld);
        else snprintf(how.fallback, sizeof how.fallback, "players/cmd/ungroup {\"player_id\":%s}", e);
        send_how(&how, "players/cmd/set_members", x);
    } else if (!strcmp(cmd, "mv")) {
        char e[400]; json_esc(e, sizeof e, a1); int v = atoi(a2);
        if (v < 0 || v > 100) return;
        snprintf(x, sizeof x, "{\"player_id\":%s,\"volume_level\":%d}", e, v);
        send_cmd(K_CMD, 0, "players/cmd/volume_set", x);
    } else if (!strcmp(cmd, "mgv")) {
        int v = atoi(a1);
        if (v < 0 || v > 100 || !has_me) return;
        snprintf(x, sizeof x, "{\"player_id\":%s,\"volume_level\":%d}", ld, v);
        send_cmd(K_CMD, 0, "players/cmd/group_volume", x);
    } else if (!strcmp(cmd, "mhere")) {
        if (!has_q) return;
        char src[96] = "", e[600];
        pthread_mutex_lock(&lk);
        struct player *o = player_of(a1);
        if (o) snprintf(src, sizeof src, "%s", o->group[0] ? o->group : o->synced[0] ? o->synced : o->id);
        pthread_mutex_unlock(&lk);
        if (!src[0]) return;
        json_esc(e, sizeof e, src);
        snprintf(x, sizeof x, "{\"source_queue_id\":%s,\"target_queue_id\":%s,\"auto_play\":true}", e, q);
        send_cmd(K_CMD, 0, "player_queues/transfer", x);
    }
}

/* ---------------------------------------------------------------- the rest of the daemon */

void mass_replay(void)
{
    pthread_mutex_lock(&lk);
    for (int k = 0; k < NPEND; k++) if (pend[k].kind == K_LIST) pend[k].id = 0;   /* the last UI's lists: not this one's */
    if (last_ma[0]) screen_line(last_ma);
    if (up) { players_tell(1); screen_line("pl."); if (last_pq[0]) screen_line(last_pq); }
    pthread_mutex_unlock(&lk);
}

void mass_peer(const char *ip)
{
    pthread_mutex_lock(&lk);
    if (ip && ip[0] && strcmp(ip, peer)) {
        snprintf(peer, sizeof peer, "%s", ip);
        if (!url[0]) { gen++; pthread_cond_broadcast(&cv); }   /* that server is the one: there now */
    }
    pthread_mutex_unlock(&lk);
}

/* -2: a token and no address; -3: the token kept for another address; -4 https (the API is plain ws on 8095 here).  The page sends the address it showed (the
 * server that played here, mass_status_json "found") when none was typed: never one that came after, which the user did
 * not see.  A token goes only to the address it was given with: a new address takes it again */
int mass_set(const char *u, const char *t)
{
    pthread_mutex_lock(&lk);
    char nu[sizeof url]; snprintf(nu, sizeof nu, "%s", u ? u : "");
    int tok = t && !strcmp(t, "-") ? 0 : t && t[0] ? 1 : token[0] != 0;
    if (tok && !nu[0]) { pthread_mutex_unlock(&lk); return -2; }
    if (tok && !(t && t[0]) && strcmp(nu, url)) { pthread_mutex_unlock(&lk); return -3; }
    if (!strncmp(nu, "https://", 8) || !strncmp(nu, "wss://", 6)) { pthread_mutex_unlock(&lk); return -4; }   /* plain only */
    char ou[sizeof url], ot[sizeof token]; memcpy(ou, url, sizeof url); memcpy(ot, token, sizeof token);
    memcpy(url, nu, sizeof url);
    if (t && !strcmp(t, "-")) wipe(token, sizeof token);
    else if (t && t[0]) snprintf(token, sizeof token, "%s", t);
    int rc = save();
    if (rc) { memcpy(url, ou, sizeof url); memcpy(token, ot, sizeof token); }   /* not saved: as it was (the page says 500) */
    wipe(ot, sizeof ot);
    if (rc) { pthread_mutex_unlock(&lk); return rc; }
    gen++; pthread_cond_broadcast(&cv);
    set_state(nu[0] || (!tok && peer[0]) ? 1 : 0, "%s", "");   /* the old session's state is no answer to this */
    pthread_mutex_unlock(&lk);
    pthread_mutex_lock(&wslk);
    if (wsopen) shutdown(w.fd, SHUT_RDWR);                     /* the session sees the new settings at once */
    pthread_mutex_unlock(&wslk);
    fprintf(stderr, "mass: %s, %s\n", u && u[0] ? u : "server found by itself", token[0] ? "a token" : "no token");
    return rc;
}

size_t mass_status_json(char *o, size_t cap)
{
    static const char *names[] = { "off", "connecting", "up", "auth", "error" };
    char eu[6 * 256 + 3], et[6 * 200 + 3], ep[600];
    pthread_mutex_lock(&lk);
    json_esc(eu, sizeof eu, url); json_esc(et, sizeof et, text_);
    char f[80] = ""; if (peer[0]) snprintf(f, sizeof f, "%s:8095", peer);
    json_esc(ep, sizeof ep, f);
    int n = snprintf(o, cap, "{\"state\":\"%s\",\"text\":%s,\"url\":%s,\"found\":%s,\"token\":%s,\"self\":%s}", names[state], et, eu, ep,
                     token[0] ? "true" : "false", self[0] ? "true" : "false");
    pthread_mutex_unlock(&lk);
    return n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap - 1;
}

/* the screen UI given up (screen.c): no session, nothing to show its lines; until hassmic starts again */
void mass_stop(void)
{
    pthread_mutex_lock(&lk); stopped = 1; gen++; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&lk);
    pthread_mutex_lock(&wslk);
    if (wsopen) shutdown(w.fd, SHUT_RDWR);
    pthread_mutex_unlock(&wslk);
}

void mass_start(void)
{
    pthread_t t;
    pthread_mutex_lock(&lk);
    int go = !started; started = 1;
    if (go) load();
    pthread_mutex_unlock(&lk);
    if (!go) return;
    if (!pthread_create(&t, NULL, thread_main, NULL)) pthread_detach(t);
    if (!pthread_create(&t, NULL, image_thread, NULL)) pthread_detach(t);
}
