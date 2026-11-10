/* See ui.h: Music Assistant on the screen.  A rail on the left, five pages:
 *
 *   Now playing   the cover, title, artist, album, where in the track (seek), shuffle, previous, play/pause, next,
 *                 repeat, the group's volume.  From Sendspin (metadata@v1, artwork@v1, the controller): works with no
 *                 more than Music Assistant playing on the Echo
 *   Queue         what this Echo's queue holds, the current track marked; tap plays from there
 *   Library       artists, albums, playlists, tracks, radio, podcasts, audiobooks; an artist opens its albums, an album
 *                 or a playlist its tracks, with Play, Shuffle, Add to queue and Radio
 *   Search        a keyboard and what Music Assistant finds, as you type
 *   Speakers      every player Music Assistant has: which are in this Echo's group (tap to add or take out), each one's
 *                 volume and the group's, what plays elsewhere moved here (long press)
 *
 * The last four need Music Assistant's API (hassmic's mass.c, a token on the settings page): lists come as "ml"
 * requests and their answers (scene.h), a page of 60 at a time, more as the list scrolls towards its end.  A long press on
 * anything playable is a sheet of what to do with it (play now, next, add to the queue, start a radio from it).
 */
#include "ui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAIL    92
#define HDR     70
#define ROW     64
#define PAGE    60
enum { T_NOW, T_QUEUE, T_LIBRARY, T_SEARCH, T_SPEAKERS, T_COUNT };
static const struct { const char *label; int icon; } tabs[T_COUNT] = {
    { "Playing", IC_NOTE }, { "Queue", IC_QUEUE }, { "Library", IC_LIBRARY }, { "Search", IC_SEARCH }, { "Speakers", IC_SPEAKERS },
};
static const struct { const char *label, *what; char kind; } cats[] = {
    { "Artists", "artists", 'r' }, { "Albums", "albums", 'a' }, { "Playlists", "playlists", 'p' }, { "Tracks", "tracks", 't' },
    { "Radio", "radios", 'o' }, { "Podcasts", "podcasts", 'c' }, { "Audiobooks", "audiobooks", 'b' },
};
#define NCATS ((int)(sizeof cats / sizeof cats[0]))

/* ids inside ID_MUSIC .. ID_DRAWER */
#define I_RAIL  (ID_MUSIC)                      /* + 10 a tab */
#define I_HEAD  (ID_MUSIC + 100)
#define I_NOW   (ID_MUSIC + 300)
#define I_LIST  (ID_MUSIC + 500)                /* + slot * 16, 200 slots: a cover's 8, the texts after */
#define I_DET   (ID_MUSIC + 3700)
#define I_SPK   (ID_MUSIC + 3800)               /* + 10 + slot * 24, 80 slots */
#define I_KB    (ID_MUSIC + 5800)
#define I_CARD  (ID_MUSIC + 6100)
#define I_SHEET (ID_MUSIC + 6150)
#define I_TOAST (ID_MUSIC + 6200)
#define SLOT(i) (I_LIST + ((i) % 200) * 16)

static const char *kind_name(char k)
{
    switch (k) {
    case 't': return "Song"; case 'a': return "Album"; case 'r': return "Artist"; case 'p': return "Playlist";
    case 'o': return "Radio"; case 'c': return "Podcast"; case 'e': return "Episode"; case 'b': return "Audiobook";
    case 'f': return "Folder"; default: return "";
    }
}
static const char *kind_plural(char k)
{
    switch (k) {
    case 't': return "Songs"; case 'a': return "Albums"; case 'r': return "Artists"; case 'p': return "Playlists";
    case 'o': return "Radio"; case 'c': return "Podcasts"; case 'e': return "Episodes"; case 'b': return "Audiobooks";
    default: return "";
    }
}
static int opens(char k) { return k == 'a' || k == 'r' || k == 'p' || k == 'c'; }       /* a tap shows what is in it */
static int grid_kind(char k) { return k == 'a' || k == 'r' || k == 'p' || k == 'c' || k == 'b'; }
static int up(const struct scene *s) { return s->m.ma == 2; }

/* ---------------------------------------------------------------- lists */

static void list_reset(struct mlist *l)
{
    l->n = 0; l->total = -1; l->done = l->failed = l->loading = 0; l->err[0] = 0; l->req = 0; l->fill = -1; l->refill = l->again = 0;
}

static void list_ask(struct scene *s, struct mlist *l, int offset)
{
    l->req = ++s->m.req_next; l->loading = 1; l->err[0] = 0;
    if (l->arg[0]) send(s, "ml %d %s %d %s", l->req, l->what, offset, l->arg);
    else send(s, "ml %d %s %d", l->req, l->what, offset);
}

static void list_load(struct scene *s, struct mlist *l, int offset)
{
    if (!up(s)) return;
    if (!offset) { l->n = 0; l->done = l->failed = 0; l->total = -1; l->fill = -1; }
    list_ask(s, l, offset);
}

/* the same list again (its items changed; Music Assistant says so for every track radio adds): the rows replaced where
 * they are, page by page as far as it was loaded, so no "Loading…" and the place in it kept.  Asked while a page is
 * on its way: once that is in */
static void list_refresh(struct scene *s, struct mlist *l)
{
    if (!up(s)) return;
    if (l->loading) { l->again = 1; return; }
    l->done = l->failed = 0; l->total = -1; l->fill = 0; l->refill = l->n; l->again = 0;
    list_ask(s, l, 0);
}

static struct mlist *list_new(const char *what, const char *arg)
{
    struct mlist *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    snprintf(l->what, sizeof l->what, "%.*s", (int)sizeof l->what - 1, what); snprintf(l->arg, sizeof l->arg, "%.*s", (int)sizeof l->arg - 1, arg ? arg : "");
    list_reset(l);
    return l;
}

static void list_free(struct scene *s, struct mlist *l) { if (l) { forget_scroller(s, &l->sc); free(l->it); free(l); } }

static struct mlist *by_req(struct scene *s, int req)
{
    struct music *m = &s->m;
    for (int i = 0; i < m->depth; i++) if (m->stack[i] && m->stack[i]->req == req) return m->stack[i];
    if (m->queue.req == req) return &m->queue;
    if (m->search.req == req) return &m->search;
    return NULL;
}

/* near the end of what is loaded: the next page (one that failed: again, 10 s on, once the list moves there) */
static void list_more(struct scene *s, struct mlist *l, int last_shown)
{
    if (l->failed && l->n > 0 && s->now - l->failed_at >= 10000) { l->failed = 0; l->err[0] = 0; }
    if (!l->loading && !l->done && !l->failed && l->n > 0 && last_shown >= l->n - 12) list_load(s, l, l->n);
}

/* a list that failed, or came to nothing while none is on its way: a tap on what opened it asks again */
static int stale(const struct mlist *l) { return l->failed || (!l->n && !l->loading); }

static void cat_open(struct scene *s, int c)
{
    struct music *m = &s->m;
    for (int i = 0; i < m->depth; i++) list_free(s, m->stack[i]);
    m->depth = 0; m->cat = c;
    struct mlist *l = list_new(cats[c].what, NULL);
    if (!l) return;
    l->kind = cats[c].kind; snprintf(l->title, sizeof l->title, "%s", cats[c].label);
    m->stack[m->depth++] = l;
    list_load(s, l, 0);
}

/* the library's top: its category (a search result opened is all there may be on the stack) */
static void lib_top(struct scene *s)
{
    struct music *m = &s->m;
    while (m->depth > 1) list_free(s, m->stack[--m->depth]);
    if (m->depth == 1 && m->stack[0]->arg[0]) cat_open(s, m->cat);
}

/* an album, an artist, a playlist, a podcast: what is in it, on top of the library's stack */
static void item_open(struct scene *s, const struct mitem *from)
{
    struct music *m = &s->m;
    struct mitem copy = *from; const struct mitem *it = &copy;  /* it may be in the list freed just below */
    if (m->depth >= MDEPTH) { list_free(s, m->stack[MDEPTH - 1]); m->depth = MDEPTH - 1; }
    const char *what = it->kind == 'a' ? "album_tracks" : it->kind == 'r' ? "artist_albums" : it->kind == 'p' ? "playlist_tracks" : "podcast_episodes";
    struct mlist *l = list_new(what, it->uri);
    if (!l) return;
    l->kind = it->kind;                                         /* (the same sizes as the item's) */
    memcpy(l->title, it->title, sizeof l->title); memcpy(l->sub, it->sub, sizeof l->sub); memcpy(l->img, it->img, sizeof l->img);
    m->stack[m->depth++] = l;
    m->tab = T_LIBRARY;
    list_load(s, l, 0);
}

static void tab_open(struct scene *s, int t)
{
    struct music *m = &s->m;
    m->tab = t; m->sheet = 0;
    if (t == T_QUEUE && up(s)) { scroll_to(&m->queue.sc, 0); m->q_goto = 1; list_load(s, &m->queue, 0); }
    if (t == T_LIBRARY && up(s) && (!m->depth || (m->depth == 1 && !m->stack[0]->arg[0] && stale(m->stack[0])))) cat_open(s, m->cat);
    if (t == T_SEARCH && !m->query[0]) m->kb = 1;
}

void music_show(struct scene *s, int tab)
{
    if (tab < 0) tab = s->music && (s->title[0] || s->mstate) ? T_NOW : up(s) ? T_LIBRARY : T_NOW;
    go_home(s, V_MUSIC); ask_own(s);                            /* (asked for: the home app does not come over it) */
    tab_open(s, tab);
}

/* ---------------------------------------------------------------- drawing helpers */

/* a cover, or the kind's own icon where there is none (yet) */
static void cover(struct scene *s, int id, const char *key, char kind, float x, float y, float a, float rad)
{
    int px = (int)lroundf(a);
    const struct ui_image *im = img_get(s, key, px);
    if (image(s, id, im, x, y, a, a, rad)) return;
    rrect(s, id, x, y, x + a, y + a, rad, 0x22262D, 255);
    int ic = kind == 'r' ? IC_PERSON : kind == 'a' ? IC_ALBUM : kind == 'p' ? IC_LIST : kind == 'o' ? IC_RADIO :
             kind == 'c' || kind == 'e' ? IC_MIC : IC_NOTE;
    icon(s, id + 1, ic, x + a / 2, y + a / 2, a * 0.22f, FAINT, 0x22262D);
}

static void header(struct scene *s, const char *title, const char *sub, int back)
{
    float x0 = U(RAIL), x = x0 + U(28);
    if (back) {
        round_button(s, I_HEAD, x0 + U(40), U(HDR) / 2, U(22), IC_BACK, BG, FG, A_M_BACK, 0);
        x = x0 + U(72);
    }
    text(s, I_HEAD + 10, title, U(26), 1, s->w - x - U(260), x, sub && sub[0] ? U(10) : U(20), 0, FG, 255);
    if (sub && sub[0]) text(s, I_HEAD + 11, sub, U(16), 0, s->w - x - U(260), x, U(42), 0, DIM, 255);
}

static void rail(struct scene *s)
{
    float rw = U(RAIL), h = s->h, th = (h - U(76)) / T_COUNT;
    fill(s, I_RAIL, (struct rect){ 0, 0, (int)rw, s->h }, 0x0B0D10);
    round_button(s, I_RAIL + 1, rw / 2, U(38), U(24), IC_FACE, BG, DIM, A_M_HOME, 0);
    for (int i = 0; i < T_COUNT; i++) {
        float y = U(76) + i * th, cy = y + th / 2 - U(9);
        int on = s->m.tab == i, p = pressed(s, A_M_TAB, i);
        if (on || p) rrect(s, I_RAIL + 10 + 10 * i, rw / 2 - U(28), cy - U(16), rw / 2 + U(28), cy + U(16), U(16), on ? 0x123D4F : SURF2, 255);
        icon(s, I_RAIL + 11 + 10 * i, tabs[i].icon, rw / 2, cy, U(12), on ? ACCENT : DIM, on ? 0x123D4F : p ? SURF2 : 0x0B0D10);
        text(s, I_RAIL + 19 + 10 * i, tabs[i].label, U(14), on, rw - U(6), rw / 2, cy + U(22), 1, on ? FG : DIM, 255);
        hit(s, 0, y, rw, y + th, A_M_TAB, i);
    }
}

/* where Music Assistant's API is not there: what to do about it */
static void setup_card(struct scene *s, float y0)
{
    float x0 = U(RAIL) + U(40), w = s->w - x0 - U(40), y = y0 + U(30);
    const char *t, *b; char buf[300];
    switch (s->m.ma) {
    case 1: t = "Connecting to Music Assistant"; b = s->m.ma_text[0] ? s->m.ma_text : "One moment."; break;
    case 3: t = "Music Assistant refused the token";
        snprintf(buf, sizeof buf, "Make a new long-lived token in Music Assistant (your profile) and paste it on the settings page%s%s.",
                 s->web[0] ? ": " : "", s->web); b = buf; break;
    case 4: t = "Music Assistant cannot be reached"; b = s->m.ma_text[0] ? s->m.ma_text : "It will be tried again."; break;
    default: t = "Connect Music Assistant";
        snprintf(buf, sizeof buf, "To browse, search and group speakers here, open the settings page%s%s, Music, and paste a "
                 "long-lived token from Music Assistant (your profile).", s->web[0] ? " at " : "", s->web); b = buf;
    }
    icon(s, I_CARD, IC_LIBRARY, x0 + U(28), y + U(26), U(22), ACCENT, BG);
    text(s, I_CARD + 5, t, U(24), 1, w - U(80), x0 + U(76), y + U(10), 0, FG, 255);
    /* the explanation on up to three lines, cut at words */
    const char *p = b; float ly = y + U(52); int line = 0;
    while (*p && line < 4) {
        char ln[200]; int n = (int)strlen(p), cut = n;
        while (cut > 0 && text_width(s, (snprintf(ln, sizeof ln, "%.*s", cut, p), ln), U(18), 0, 4096) > w - U(80)) {
            int c = cut - 1; while (c > 0 && p[c] != ' ') c--;
            cut = c > 0 ? c : cut - 1;
        }
        if (cut <= 0) break;
        snprintf(ln, sizeof ln, "%.*s", cut, p);
        text(s, I_CARD + 10 + line, ln, U(18), 0, w - U(80), x0 + U(76), ly, 0, DIM, 255);
        p += cut; while (*p == ' ') p++; ly += U(27); line++;
    }
}

static struct mplayer *leader(struct scene *s);
static int group_vol(const struct mplayer *ld);

/* ---------------------------------------------------------------- now playing */

static void now_playing(struct scene *s, long long *due)
{
    struct music *m = &s->m;
    float x0 = U(RAIL), h = s->h, w = s->w, pad = U(34);
    uint32_t tint = s->art.px ? gfx_mix(0x000000, s->art_avg, 0.42f) : 0x0E1013;
    grad(s, I_NOW, (struct rect){ (int)x0, 0, s->w, s->h }, tint, 0x000000);
    float A = fminf(h - 2 * pad, (w - x0) * 0.42f), ax = x0 + pad, ay = (h - A) / 2;
    if (!image(s, I_NOW + 1, &s->art, ax, ay, A, A, U(14))) {
        rrect(s, I_NOW + 1, ax, ay, ax + A, ay + A, U(14), 0x1C1F25, 255);
        icon(s, I_NOW + 2, IC_NOTE, ax + A / 2, ay + A / 2, A * 0.2f, 0x3A3F47, 0x1C1F25);
    }
    float rx = ax + A + pad, rw = w - rx - pad, y = ay + U(4);
    int playing = s->music && (s->title[0] || s->mstate);
    /* where it plays: the group, a tap away from the speakers */
    char where[160];
    snprintf(where, sizeof where, "%s", m->q_known && m->q_name[0] ? m->q_name : s->name[0] ? s->name : "This Echo");
    icon(s, I_NOW + 5, IC_SPEAKER, rx + U(9), y + U(9), U(9), DIM, tint);
    text(s, I_NOW + 9, where, U(16), 0, rw - U(26), rx + U(26), y, 0, DIM, 255);
    if (up(s)) hit(s, rx, y - U(8), rx + rw, y + U(26), A_M_TAB, T_SPEAKERS);
    y += U(40);
    if (!playing) {
        text(s, I_NOW + 10, "Nothing playing", U(30), 1, rw, rx, y, 0, FG, 255);
        text(s, I_NOW + 11, up(s) ? "Pick something from the library, or ask for it." : "Ask for music, or start it in Music Assistant.",
             U(18), 0, rw, rx, y + U(44), 0, DIM, 255);
        if (up(s)) button(s, I_NOW + 12, rx, y + U(90), rx + U(170), y + U(138), "Library", IC_LIBRARY, ACCENT, 0x0B0C0F, A_M_TAB, T_LIBRARY);
        return;
    }
    text(s, I_NOW + 10, s->title[0] ? s->title : "Music Assistant", U(30), 1, rw, rx, y, 0, FG, 255);
    text(s, I_NOW + 11, s->artist, U(21), 0, rw, rx, y + U(42), 0, 0xC8CDD3, 255);
    text(s, I_NOW + 12, s->album, U(17), 0, rw, rx, y + U(72), 0, DIM, 255);
    /* where in the track */
    float py = ay + A * 0.56f;
    if (s->mdur > 0) {
        long long pos = m->seek_drag ? (long long)(m->seek_frac * s->mdur) : music_pos(s);
        float v = clampf((float)pos / s->mdur, 0, 1);
        char t1[24], t2[24];
        if (can(s, "seek")) { slider(s, I_NOW + 20, rx, rx + rw, py, v, FG, A_M_SEEK, 0); m->sl0[0] = rx; m->sl1[0] = rx + rw; }
        else {
            rrect(s, I_NOW + 20, rx, py - U(3), rx + rw, py + U(3), U(3), 0x3A3F47, 255);
            rrect(s, I_NOW + 21, rx, py - U(3), rx + fmaxf(U(6), rw * v), py + U(3), U(3), FG, 255);
        }
        fmt_time(t1, sizeof t1, pos); fmt_time(t2, sizeof t2, s->mdur);
        text(s, I_NOW + 24, t1, U(15), 0, rw, rx, py + U(14), 0, DIM, 255);
        text(s, I_NOW + 25, t2, U(15), 0, rw, rx + rw, py + U(14), 2, DIM, 255);
        if (s->mstate == 1 && !m->seek_drag) { long long next = s->now + 1000 - pos % 1000; if (!*due || next < *due) *due = next; }
    }
    /* the buttons */
    float by = ay + A * 0.79f, step = rw / 5;
    int sh = can(s, "shuffle") || can(s, "unshuffle"), rp = can(s, "repeat_all") || can(s, "repeat_off");
    if (sh) round_button(s, I_NOW + 30, rx + step * 0.5f, by, U(24), IC_SHUFFLE, BG, s->mshuffle ? ACCENT : DIM, A_M_SHUFFLE, 0);
    round_button(s, I_NOW + 40, rx + step * 1.5f, by, U(28), IC_PREV, BG, can(s, "previous") ? FG : FAINT, A_M_PREV, 0);
    int pp = can(s, s->mstate == 1 ? "pause" : "play");
    round_button(s, I_NOW + 50, rx + step * 2.5f, by, U(36), s->mstate == 1 ? IC_PAUSE : IC_PLAY, pp ? FG : 0x3A3F47, 0x0B0C0F, A_M_PLAY, 0);
    round_button(s, I_NOW + 60, rx + step * 3.5f, by, U(28), IC_NEXT, BG, can(s, "next") ? FG : FAINT, A_M_NEXT, 0);
    if (rp) round_button(s, I_NOW + 70, rx + step * 4.5f, by, U(24), s->mrepeat == 1 ? IC_REPEAT1 : IC_REPEAT, BG, s->mrepeat ? ACCENT : DIM, A_M_REPEAT, 0);
    /* the group's volume: Sendspin's controller where it offers one (Music Assistant 2.10 only for groups whose players
     * declare it), else the API's group volume */
    struct mplayer *ld = leader(s);
    if (can(s, "volume") || (up(s) && ld)) {
        float vy = ay + A - U(8);
        icon(s, I_NOW + 80, IC_SPEAKER, rx + U(10), vy, U(11), DIM, BG);
        float v = m->vol_drag ? m->vol_frac : can(s, "volume") ? s->mvol / 100.0f : group_vol(ld) / 100.0f;
        slider(s, I_NOW + 84, rx + U(34), rx + rw - U(8), vy, v, DIM, A_M_VOL, 0); m->sl0[1] = rx + U(34); m->sl1[1] = rx + rw - U(8);
    }
}

/* ---------------------------------------------------------------- lists and grids */

static int list_row_n(struct scene *s, int id, const struct mitem *it, float x0, float x1, float y, int index, int act, int show_kind, int number);
static int list_row(struct scene *s, int id, const struct mitem *it, float x0, float x1, float y, int index, int act, int show_kind)
{
    return list_row_n(s, id, it, x0, x1, y, index, act, show_kind, 0);
}

static int list_row_n(struct scene *s, int id, const struct mitem *it, float x0, float x1, float y, int index, int act, int show_kind, int number)
{
    int cur = strchr(it->flags, 'c') != NULL, gone = strchr(it->flags, 'x') != NULL;
    if (pressed(s, act, index)) rrect(s, id, x0 - U(10), y + U(2), x1 + U(10), y + U(ROW) - U(2), U(12), SURF2, 255);
    else if (cur) rrect(s, id, x0 - U(10), y + U(2), x1 + U(10), y + U(ROW) - U(2), U(12), 0x0F2B37, 255);
    float a = U(46), ty = y + (U(ROW) - a) / 2;
    if (number) {                                               /* an album's tracks: their place, its cover is above */
        char nb[12]; snprintf(nb, sizeof nb, "%d", number);
        text(s, id + 1, nb, U(18), 0, a, x0 + a / 2, y + U(22), 1, DIM, 255);
    } else cover(s, id + 1, it->img, it->kind == 'q' ? 't' : it->kind, x0, ty, a, it->kind == 'r' ? a / 2 : U(6));
    char right[32] = "";
    if (it->dur > 0) fmt_time(right, sizeof right, it->dur * 1000LL);
    else if (show_kind) snprintf(right, sizeof right, "%s", kind_name(it->kind));
    float rw = right[0] ? (float)text_width(s, right, U(15), 0, U(120)) + U(12) : 0, tx = x0 + a + U(16), tw = x1 - tx - rw;
    uint32_t tc = gone ? FAINT : cur ? ACCENT : FG;
    text(s, id + 9, it->title, U(19), cur, tw, tx, y + U(12), 0, tc, 255);
    text(s, id + 10, it->sub, U(15), 0, tw, tx, y + U(36), 0, gone ? FAINT : DIM, 255);
    if (right[0]) text(s, id + 11, right, U(15), 0, U(120), x1, y + U(24), 2, FAINT, 255);
    hit_key(s, x0 - U(10), y, x1 + U(10), y + U(ROW), act, index, ui_hash(it->uri[0] ? it->uri : it->title));
    return id + 10;
}

static void grid_tile(struct scene *s, int id, const struct mitem *it, float x, float y, float tw, int index, int act)
{
    float a = tw - U(18), cx = x + tw / 2;
    if (pressed(s, act, index)) rrect(s, id, x + U(2), y - U(6), x + tw - U(2), y + a + U(64), U(14), SURF2, 255);
    cover(s, id + 1, it->img, it->kind, cx - a / 2, y, a, it->kind == 'r' ? a / 2 : U(10));
    int al = it->kind == 'r' ? 1 : 0; float tx = al ? cx : cx - a / 2;
    text(s, id + 9, it->title, U(17), 1, a, tx, y + a + U(10), al, FG, 255);
    text(s, id + 10, it->sub, U(15), 0, a, tx, y + a + U(33), al, DIM, 255);
    hit_key(s, x, y - U(6), x + tw, y + a + U(64), act, index, ui_hash(it->uri[0] ? it->uri : it->title));
}

/* the empty and the failed: words where a list would be */
static int list_state(struct scene *s, const struct mlist *l, float x0, float y, float w, const char *empty)
{
    if (l->n) return 0;
    const char *t = l->err[0] ? l->err : l->loading || (l->total < 0 && !l->done) ? "Loading…" : empty;   /* (ended with
                                                                   nothing, no total told: empty all the same) */
    text(s, I_LIST, t, U(19), 0, w, x0 + w / 2, y + U(60), 1, DIM, 255);
    return 1;
}

/* the detail on top of an album's, playlist's, artist's list: its cover, its name, what to do with it */
static float detail_head(struct scene *s, const struct mlist *l, float x0, float y, float w)
{
    float a = U(132);
    cover(s, I_DET, l->img, l->kind, x0, y, a, l->kind == 'r' ? a / 2 : U(12));
    float tx = x0 + a + U(28), tw = w - a - U(28);
    text(s, I_DET + 10, kind_name(l->kind), U(14), 1, tw, tx, y + U(4), 0, ACCENT, 255);
    text(s, I_DET + 11, l->title, U(28), 1, tw, tx, y + U(26), 0, FG, 255);
    text(s, I_DET + 12, l->sub, U(17), 0, tw, tx, y + U(64), 0, DIM, 255);
    float by = y + a - U(42), bx = tx;
    button(s, I_DET + 20, bx, by, bx + U(124), by + U(42), "Play", IC_PLAY, ACCENT, 0x0B0C0F, A_M_DPLAY, 0); bx += U(136);
    if (l->kind != 'c') { button(s, I_DET + 30, bx, by, bx + U(132), by + U(42), "Shuffle", IC_SHUFFLE, SURF2, FG, A_M_DSHUF, 0); bx += U(144); }
    button(s, I_DET + 40, bx, by, bx + U(52), by + U(42), NULL, IC_PLUS, SURF2, FG, A_M_DADD, 0); bx += U(64);
    if (l->kind != 'c') button(s, I_DET + 50, bx, by, bx + U(52), by + U(42), NULL, IC_RADIO, SURF2, FG, A_M_DRADIO, 0);
    return a + U(26);
}

/* a list or a grid in r, scrolled by l->sc, with the detail on top where there is one */
static void list_view(struct scene *s, struct mlist *l, struct rect r, int act, int show_kind, int detail)
{
    struct rect saved; float x0 = r.x0 + U(28), w = r.x1 - x0 - U(28), top = (float)r.y0;
    clip_push(s, r, &saved);
    int grid = l->n && grid_kind(l->it[0].kind) && !show_kind;
    int cols = grid ? (int)fmaxf(1, floorf((w + U(14)) / U(162))) : 1;
    float tw = grid ? w / cols : w, th = grid ? tw - U(18) + U(76) : U(ROW);
    float head = detail ? U(18) + U(132) + U(26) : 0;
    int rows = (l->n + cols - 1) / cols;
    float content = head + rows * th + ((l->loading || l->failed) && l->n ? U(50) : 0) + U(16);
    scroll_area(s, r, &l->sc, 1, content);
    float off = top - l->sc.pos;
    if (detail) detail_head(s, l, x0, off + U(18), w);
    if (!list_state(s, l, x0, off + head, w, show_kind ? "Nothing found" : "Nothing here")) {
        int first = (int)fmaxf(0, floorf((l->sc.pos - head) / th)), last = -1;
        for (int row = first; row < rows; row++) {
            float y = off + head + row * th;
            if (y > r.y1) break;
            for (int c = 0; c < cols; c++) {
                int i = row * cols + c;
                if (i >= l->n) break;
                int id = SLOT(i);
                if (grid) grid_tile(s, id, &l->it[i], x0 + c * tw, y + U(10), tw, i, act);
                else list_row_n(s, id, &l->it[i], x0, x0 + w, y, i, act, show_kind, detail && l->kind == 'a' ? i + 1 : 0);
                last = i;
            }
        }
        list_more(s, l, last);
        if ((l->loading || l->failed) && l->n)                  /* (a later page that failed: why, where it would be) */
            text(s, I_DET + 99, l->loading ? "Loading…" : l->err, U(16), 0, w, x0 + w / 2, off + head + rows * th + U(14), 1, FAINT, 255);
    }
    clip_pop(s, saved);
}

/* search results come grouped by kind (hassmic's order: artists, albums, songs, playlists, radio...): a heading where
 * each starts, the rows under it */
static void results_view(struct scene *s, struct mlist *l, struct rect r)
{
    struct rect saved; float x0 = r.x0 + U(28), w = r.x1 - x0 - U(28), hh = U(42);
    clip_push(s, r, &saved);
    float content = U(10);
    for (int i = 0; i < l->n; i++) content += (i == 0 || l->it[i].kind != l->it[i - 1].kind ? hh : 0) + U(ROW);
    scroll_area(s, r, &l->sc, 1, content + U(16));
    if (!list_state(s, l, x0, r.y0 - l->sc.pos, w, "Nothing found")) {
        float y = r.y0 - l->sc.pos + U(10); int sec = 0;
        for (int i = 0; i < l->n && y <= r.y1; i++) {
            if (i == 0 || l->it[i].kind != l->it[i - 1].kind) {
                if (y + hh >= r.y0 && sec < 20) text(s, I_DET + 60 + 2 * sec, kind_plural(l->it[i].kind), U(15), 1, w, x0, y + U(16), 0, ACCENT, 255);
                y += hh; sec++;
            }
            if (y + U(ROW) >= r.y0) list_row(s, SLOT(i), &l->it[i], x0, x0 + w, y, i, A_M_ITEM, 0);
            y += U(ROW);
        }
    }
    clip_pop(s, saved);
}

static void queue_view(struct scene *s)
{
    struct music *m = &s->m;
    char sub[64] = "";
    if (m->q_known && m->q_items > 0) snprintf(sub, sizeof sub, "%d %s", m->q_items, m->q_items == 1 ? "track" : "tracks");
    header(s, "Queue", sub, 0);
    if (!up(s)) { setup_card(s, U(HDR)); return; }
    float bx = s->w - U(28);
    button(s, I_HEAD + 20, bx - U(110), U(14), bx, U(56), "Clear", -1, SURF2, FG, A_M_QCLEAR, 0);
    button(s, I_HEAD + 30, bx - U(250), U(14), bx - U(122), U(56), "Shuffle", IC_SHUFFLE, m->q_shuffle ? 0x123D4F : SURF2,
           m->q_shuffle ? ACCENT : FG, A_M_SHUFFLE, 1);
    list_view(s, &m->queue, (struct rect){ (int)U(RAIL), (int)U(HDR), s->w, s->h }, A_M_QITEM, 0, 0);
}

static void library_view(struct scene *s)
{
    struct music *m = &s->m;
    if (!up(s)) { header(s, "Library", NULL, 0); setup_card(s, U(HDR)); return; }
    if (!m->depth) cat_open(s, m->cat);
    if (!m->depth) return;
    struct mlist *l = m->stack[m->depth - 1];
    if (m->depth == 1 && !l->arg[0]) {                          /* the categories, as chips along the top */
        struct rect cr = { (int)U(RAIL), 0, s->w, (int)U(HDR) }, saved;
        clip_push(s, cr, &saved);
        float x = U(RAIL) + U(24) - m->cats.pos, total = 0;
        for (int i = 0; i < NCATS; i++) {
            float tw = (float)text_width(s, cats[i].label, U(18), 1, U(200)) + U(36);
            int on = m->cat == i, p = pressed(s, A_M_CAT, i);
            rrect(s, I_HEAD + 30 + 4 * i, x, U(16), x + tw, U(56), U(20), on ? FG : p ? 0x3A3F47 : SURF2, 255);
            text(s, I_HEAD + 31 + 4 * i, cats[i].label, U(18), 1, U(200), x + tw / 2, U(25), 1, on ? 0x0B0C0F : FG, 255);
            hit(s, x, U(10), x + tw, U(62), A_M_CAT, i);
            x += tw + U(10); total += tw + U(10);
        }
        scroll_area(s, cr, &m->cats, 0, total + U(48));
        clip_pop(s, saved);
        list_view(s, l, (struct rect){ (int)U(RAIL), (int)U(HDR), s->w, s->h }, A_M_ITEM, 0, 0);
    } else {
        header(s, m->depth > 1 ? m->stack[m->depth - 2]->title : "Library", NULL, 1);   /* where it came from (a search: the
                                                                                             * library); its own name is below */
        list_view(s, l, (struct rect){ (int)U(RAIL), (int)U(HDR), s->w, s->h }, A_M_ITEM, 0, 1);
    }
}

static void search_view(struct scene *s)
{
    struct music *m = &s->m;
    if (!up(s)) { header(s, "Search", NULL, 0); setup_card(s, U(HDR)); return; }
    float x0 = U(RAIL) + U(24), x1 = s->w - U(24), fy0 = U(12), fy1 = U(58);
    rrect(s, I_HEAD, x0, fy0, x1, fy1, (fy1 - fy0) / 2, m->kb ? 0x2A2E35 : SURF2, 255);
    if (m->kb) stroke(s, I_HEAD + 1, x0, fy0, x1, fy1, (fy1 - fy0) / 2, U(2), ACCENT, 255);
    icon(s, I_HEAD + 2, IC_SEARCH, x0 + U(26), (fy0 + fy1) / 2, U(11), DIM, SURF2);
    float tx = x0 + U(48), tw = x1 - tx - U(56);
    int qw = m->query[0] ? text(s, I_HEAD + 8, m->query, U(20), 0, tw, tx, fy0 + U(12), 0, FG, 255)
                         : (text(s, I_HEAD + 8, "Artists, songs, albums, radio…", U(20), 0, tw, tx, fy0 + U(12), 0, FAINT, 255), 0);
    if (m->kb && (s->now / 530) % 2 == 0) rrect(s, I_HEAD + 9, tx + qw + U(2), fy0 + U(11), tx + qw + U(4), fy1 - U(11), 0, ACCENT, 255);
    hit(s, x0, fy0, x1 - U(50), fy1, A_M_FIELD, 0);
    if (m->query[0]) round_button(s, I_HEAD + 12, x1 - U(26), (fy0 + fy1) / 2, U(18), IC_CLOSE, BG, DIM, A_M_CLEARQ, 0);
    float kb_h = m->kb ? fminf(U(232), s->h * 0.52f) : 0;
    struct rect res = { (int)U(RAIL), (int)U(HDR), s->w, (int)(s->h - kb_h) };
    if (m->query_sent && (m->search.n || m->search.loading || m->search.done || m->search.failed)) results_view(s, &m->search, res);   /* (its error too) */
    else if (!m->kb || res.y1 - res.y0 > U(80))
        text(s, I_LIST, !m->query[0] ? "Search Music Assistant and everything it is linked to." : strlen(m->query) < 2 ? "A letter more"
             : "Searching…", U(18), 0,
             s->w - U(RAIL) - U(60), U(RAIL) + (s->w - U(RAIL)) / 2, U(HDR) + U(40), 1, DIM, 255);
    if (m->kb) keyboard(s, I_KB, (struct rect){ (int)U(RAIL), (int)(s->h - kb_h), s->w, s->h }, m->kb_shift, m->kb_sym, A_M_KEY);
}

/* ---------------------------------------------------------------- speakers */

static struct mplayer *player(struct scene *s, const char *id)
{
    for (int i = 0; i < s->m.npl; i++) if (eq(s->m.pl[i].id, id)) return &s->m.pl[i];
    return NULL;
}

static struct mplayer *player_key(struct scene *s, int key)
{
    for (int i = 0; i < s->m.npl; i++) if (s->m.pl[i].key == key) return &s->m.pl[i];
    return NULL;
}
static struct mplayer *self(struct scene *s)
{
    for (int i = 0; i < s->m.npl; i++) if (strchr(s->m.pl[i].flags, 's')) return &s->m.pl[i];
    return NULL;
}
/* who leads this Echo's group: the one it follows, or itself */
/* the group's volume as Music Assistant has it (its players' average), else its leader's own */
static int group_vol(const struct mplayer *ld) { return ld->gvol >= 0 ? ld->gvol : ld->vol; }

static struct mplayer *leader(struct scene *s)
{
    struct mplayer *me = self(s), *l = me && me->leader[0] ? player(s, me->leader) : NULL;
    return l ? l : me;
}
static int in_list(const char *list, const char *id)
{
    size_t n = strlen(id);
    for (const char *p = list; (p = strstr(p, id)); p += n)
        if ((p == list || p[-1] == ',') && (p[n] == ',' || !p[n])) return 1;
    return 0;
}
static int member(struct scene *s, const struct mplayer *p)
{
    struct mplayer *l = leader(s);
    return l && (p == l || in_list(l->members, p->id) || eq(p->leader, l->id));
}

static void speakers_view(struct scene *s)
{
    struct music *m = &s->m;
    struct mplayer *me = self(s), *ld = leader(s);
    char sub[200] = "";
    if (me && ld && ld != me) snprintf(sub, sizeof sub, "This Echo plays with %s", ld->name);
    else if (me) {
        int n = 0; for (int i = 0; i < m->npl; i++) if (member(s, &m->pl[i])) n++;
        if (n > 1) snprintf(sub, sizeof sub, "This Echo and %d more play together", n - 1);
        else snprintf(sub, sizeof sub, "Tap a speaker to play on it with this Echo");
    }
    header(s, "Speakers", sub, 0);
    if (!up(s)) { setup_card(s, U(HDR)); return; }
    if (me && ld && ld != me) button(s, I_HEAD + 20, s->w - U(140), U(14), s->w - U(28), U(56), "Leave", -1, SURF2, FG, A_M_LEAVE, 0);
    struct rect r = { (int)U(RAIL), (int)U(HDR), s->w, s->h }, saved;
    float x0 = U(RAIL) + U(28), x1 = s->w - U(28), rh = U(76);
    int nmem = 0; for (int i = 0; i < m->npl; i++) if (member(s, &m->pl[i])) nmem++;
    float gh = nmem > 1 && ld && (strchr(ld->flags, 'v') || ld->gvol >= 0) ? U(64) : 0;
    clip_push(s, r, &saved);
    scroll_area(s, r, &m->spk, 1, gh + m->npl * rh + U(20));
    float y = r.y0 - m->spk.pos;
    if (gh > 0) {                                               /* the group's own volume */
        rrect(s, I_SPK, x0 - U(10), y + U(6), x1 + U(10), y + gh - U(4), U(14), 0x0F2B37, 255);
        text(s, I_SPK + 1, "Group volume", U(17), 1, U(200), x0 + U(8), y + U(24), 0, FG, 255);
        float v = m->spk_drag == -1 ? m->spk_frac : group_vol(ld) / 100.0f;
        slider(s, I_SPK + 2, x0 + U(180), x1 - U(16), y + gh / 2 + U(1), v, ACCENT, A_M_GVOL, 0); m->sl0[3] = x0 + U(180); m->sl1[3] = x1 - U(16);
        y += gh;
    }
    if (!m->npl) text(s, I_SPK + 5, m->pl_all ? "Music Assistant has no players" : "Loading…", U(19), 0, x1 - x0, (x0 + x1) / 2, y + U(50), 1, DIM, 255);
    /* this Echo first, then its group, then the others */
    int order[MPLAYERS], n = 0;
    for (int pass = 0; pass < 3; pass++)
        for (int i = 0; i < m->npl; i++) {
            struct mplayer *p = &m->pl[i]; int mine = p == me, mem = member(s, p);
            if ((pass == 0 && mine) || (pass == 1 && !mine && mem) || (pass == 2 && !mine && !mem)) order[n++] = i;
        }
    for (int k = 0; k < n; k++, y += rh) {
        if (y + rh < r.y0 || y > r.y1) continue;
        struct mplayer *p = &m->pl[order[k]];
        int id = I_SPK + 10 + (k % 80) * 24, mem = member(s, p), mine = p == me, canj = strchr(p->flags, 'g') != NULL;
        int avail = strchr(p->flags, 'a') != NULL;
        if (pressed(s, A_M_SPK, p->key)) rrect(s, id, x0 - U(10), y + U(4), x1 + U(10), y + rh - U(4), U(14), SURF2, 255);
        /* the check: in the group or not; none where it cannot join */
        float cx = x0 + U(16), cy = y + rh / 2;
        if (mem) { rrect(s, id + 1, cx - U(14), cy - U(14), cx + U(14), cy + U(14), U(14), mine ? 0x3A3F47 : ACCENT, 255);
                   icon(s, id + 2, IC_CHECK, cx, cy, U(9), 0x0B0C0F, ACCENT); }
        else if (canj && avail) stroke(s, id + 1, cx - U(14), cy - U(14), cx + U(14), cy + U(14), U(14), U(2), FAINT, 255);
        icon(s, id + 8, strchr(p->flags, 'l') ? IC_SPEAKERS : IC_SPEAKER, x0 + U(58), cy, U(12), avail ? FG : FAINT, BG);
        char st[220];
        if (!avail) snprintf(st, sizeof st, "Not available");
        else if (p->leader[0] && !mine) { struct mplayer *l = player(s, p->leader); snprintf(st, sizeof st, "With %s", l ? l->name : "another speaker"); }
        else if (eq(p->state, "playing") && p->playing[0]) snprintf(st, sizeof st, "Playing · %s", p->playing);
        else if (eq(p->state, "paused")) snprintf(st, sizeof st, "Paused%s%s", p->playing[0] ? " · " : "", p->playing);
        else snprintf(st, sizeof st, "%s", mine ? "This Echo" : "Idle");
        int hasvol = strchr(p->flags, 'v') && avail;
        float tw = (hasvol ? x1 - U(230) : x1) - (x0 + U(84));
        text(s, id + 16, p->name, U(19), mem, tw, x0 + U(84), y + U(14), 0, avail ? FG : FAINT, 255);
        text(s, id + 17, st, U(15), 0, tw, x0 + U(84), y + U(40), 0, eq(p->state, "playing") ? ACCENT : DIM, 255);
        hit(s, x0 - U(10), y, hasvol ? x1 - U(240) : x1, y + rh, A_M_SPK, p->key);
        if (hasvol) {
            float v = m->spk_drag == p->key ? m->spk_frac : p->vol / 100.0f;
            slider(s, id + 20, x1 - U(210), x1 - U(10), cy, v, mem ? ACCENT : DIM, A_M_SPKVOL, p->key); m->sl0[2] = x1 - U(210); m->sl1[2] = x1 - U(10);
        }
    }
    clip_pop(s, saved);
}

/* ---------------------------------------------------------------- the view */

static const char *sheet_rows_play[] = { "Play now", "Play next", "Add to queue", "Start radio" };
static const char *sheet_rows_queue[] = { "Play from here", "Remove from queue" };
static const char *sheet_rows_clear[] = { "Clear the queue", "Keep it" };
static const char *sheet_rows_spk[] = { "Play what it plays here", "Add to this group", "Take out of this group" };

void music_view(struct scene *s, float dt, int *moving, long long *due)
{
    struct music *m = &s->m;
    (void)dt;
    fill(s, ID_MUSIC + 99, (struct rect){ (int)U(RAIL), 0, s->w, s->h }, BG);
    switch (m->tab) {
    case T_QUEUE: queue_view(s); break;
    case T_LIBRARY: library_view(s); break;
    case T_SEARCH: search_view(s); if (m->kb) { long long b = s->now + 530 - s->now % 530; if (!*due || b < *due) *due = b; } break;
    case T_SPEAKERS: speakers_view(s); break;
    default: now_playing(s, due);
    }
    rail(s);
    /* the search goes out once the typing pauses */
    if (m->query_at && s->now >= m->query_at) {
        m->query_at = 0;
        if (strlen(m->query) >= 2) { snprintf(m->search.arg, sizeof m->search.arg, "%s", m->query); scroll_to(&m->search.sc, 0); list_load(s, &m->search, 0); m->query_sent = 1; }
        else { list_reset(&m->search); m->query_sent = 0; }      /* too short to ask: no results of a longer one either */
    } else if (m->query_at && (!*due || m->query_at < *due)) *due = m->query_at;
    if (m->sheet) {
        const char *title = m->sheet == 1 || m->sheet == 4 ? m->sheet_it.title : m->sheet == 2 ? m->sheet_it.title : "Clear the queue?";
        if (m->sheet == 1) sheet(s, I_SHEET, title, sheet_rows_play, m->sheet_it.kind == 'o' || m->sheet_it.kind == 'e' ? 1 : 4, A_M_SHEET, A_M_SHADE);
        else if (m->sheet == 2) sheet(s, I_SHEET, title, sheet_rows_queue, 2, A_M_SHEET, A_M_SHADE);
        else if (m->sheet == 3) sheet(s, I_SHEET, title, sheet_rows_clear, 2, A_M_SHEET, A_M_SHADE);
        else if (m->sheet == 4) sheet(s, I_SHEET, title, sheet_rows_spk, 3, A_M_SHEET, A_M_SHADE);
    }
    if (m->toast_until > s->now) {
        toast(s, I_TOAST, m->toast);
        if (!*due || m->toast_until < *due) *due = m->toast_until;
    }
    (void)moving;
}

static void say(struct scene *s, const char *msg)
{
    snprintf(s->m.toast, sizeof s->m.toast, "%s", msg); s->m.toast_until = s->now + 1800;
}

static void play(struct scene *s, const char *option, const char *uri, const char *what)
{
    send(s, "mplay %s %s", option, uri);
    if (eq(option, "next")) say(s, "Plays next");
    else if (eq(option, "add")) say(s, "Added to the queue");
    else if (eq(option, "radio")) say(s, "Radio started");
    (void)what;
}

static struct mlist *shown_list(struct scene *s)
{
    struct music *m = &s->m;
    if (m->tab == T_QUEUE) return &m->queue;
    if (m->tab == T_SEARCH) return &m->search;
    if (m->tab == T_LIBRARY && m->depth) return m->stack[m->depth - 1];
    return NULL;
}

static void key(struct scene *s, int k)
{
    struct music *m = &s->m; size_t n = strlen(m->query);
    if (k == K_BACKSPACE) {
        if (n) { do n--; while (n && ((unsigned char)m->query[n] & 0xC0) == 0x80); m->query[n] = 0; }
    } else if (k == K_SHIFT) { m->kb_shift = !m->kb_shift; return; }
    else if (k == K_SYM) { m->kb_sym = !m->kb_sym; return; }
    else if (k == K_DONE) { m->kb = 0; m->query_at = m->query[0] ? s->now : 0; return; }
    else if (k == K_SPACE) { if (n && n + 1 < sizeof m->query && m->query[n - 1] != ' ') { m->query[n] = ' '; m->query[n + 1] = 0; } }
    else if (k >= 32 && n + 1 < sizeof m->query) { m->query[n] = (char)k; m->query[n + 1] = 0; m->kb_shift = 0; }
    m->query_at = s->now + 400;
    if (!m->query[0]) { m->query_sent = 0; list_reset(&m->search); m->query_at = 0; }
}

/* the row the finger is on still the item it was when it went down (a list asked again meanwhile: rows moved) */
static int same(const struct scene *s, const struct mitem *it) { return ui_hash(it->uri[0] ? it->uri : it->title) == s->pkey; }

/* a speaker into this Echo's group or out of it (want: 1 in, 0 out, -1 whichever it is not): never this Echo, nor the
 * leader it plays with (out of its own group: Leave is the way), nor one that cannot play with it */
static void group_set(struct scene *s, struct mplayer *p, int want)
{
    struct mplayer *me = self(s), *ld = leader(s);
    if (!p || strchr(p->flags, 's')) return;
    if (me && ld && ld != me && p == ld) { say(s, "This Echo plays with it: Leave to stop"); return; }
    int in = member(s, p);
    if (want < 0) want = !in;
    if (!want) { if (in) { send(s, "mg remove %s", p->id); say(s, "Taken out of the group"); } else say(s, "It is not in this group"); }
    else if (in) say(s, "It is in this group already");
    else if (strchr(p->flags, 'g') && strchr(p->flags, 'a')) { send(s, "mg add %s", p->id); say(s, "Added to the group"); }
    else say(s, "That speaker cannot play with this Echo");
}

int music_act(struct scene *s, int a, int arg, int ev)
{
    struct music *m = &s->m;
    /* the sliders: the finger's moves */
    if (a == A_M_SEEK || a == A_M_VOL || a == A_M_SPKVOL || a == A_M_GVOL) {
        if (ev != EV_DRAG && ev != EV_DROP) return 0;
        int drop = ev == EV_DROP;
        if (a == A_M_SEEK) {
            m->seek_drag = !drop; m->seek_frac = slider_value(s, m->sl0[0], m->sl1[0]);
            if (drop) { send(s, "cmd seek %lld", (long long)(m->seek_frac * s->mdur)); s->mpos = (long long)(m->seek_frac * s->mdur); s->mpos_at = s->now; }
        } else if (a == A_M_VOL) {
            m->vol_drag = !drop; m->vol_frac = slider_value(s, m->sl0[1], m->sl1[1]);
            if (drop || s->now - m->vol_sent_at >= 200) {
                int v = (int)lroundf(m->vol_frac * 100); struct mplayer *ld = leader(s);
                if (can(s, "volume")) { s->mvol = v; send(s, "cmd volume %d", v); }
                else if (ld) { *(ld->gvol >= 0 ? &ld->gvol : &ld->vol) = v; send(s, "mgv %d", v); }
                m->vol_sent_at = s->now;
            }
        } else {
            int gv = a == A_M_GVOL;
            m->spk_drag = drop ? 0 : gv ? -1 : arg;
            m->spk_frac = slider_value(s, m->sl0[gv ? 3 : 2], m->sl1[gv ? 3 : 2]);
            int v = (int)lroundf(m->spk_frac * 100); struct mplayer *p;
            if (drop || s->now - m->spk_sent_at >= 250) {
                struct mplayer *ld = leader(s);
                if (gv && ld) { send(s, "mgv %d", v); *(ld->gvol >= 0 ? &ld->gvol : &ld->vol) = v; }
                else if (!gv && (p = player_key(s, arg))) { send(s, "mv %s %d", p->id, v); p->vol = v; }
                m->spk_sent_at = s->now;
            }
        }
        return 1;
    }
    if (ev == EV_LONG) {
        struct mlist *l = shown_list(s);
        if (a == A_M_ITEM && l && arg < l->n && l->it[arg].uri[0] && same(s, &l->it[arg])) { m->sheet = 1; m->sheet_it = l->it[arg]; return 1; }
        if (a == A_M_QITEM && arg < m->queue.n && same(s, &m->queue.it[arg])) { m->sheet = 2; m->sheet_it = m->queue.it[arg]; return 1; }
        struct mplayer *p = a == A_M_SPK ? player_key(s, arg) : NULL;
        if (p && !strchr(p->flags, 's')) {
            m->sheet = 4; memset(&m->sheet_it, 0, sizeof m->sheet_it);
            snprintf(m->sheet_it.title, sizeof m->sheet_it.title, "%s", p->name); snprintf(m->sheet_it.uri, sizeof m->sheet_it.uri, "%s", p->id);
            return 1;
        }
        return 0;
    }
    if (ev != EV_TAP) return 0;
    switch (a) {
    case A_M_HOME: go_home(s, V_FACE); ask_own(s); s->mp_off = 1; s->mp_auto = 0; break;
    case A_M_TAB:
        if (arg == T_LIBRARY && m->tab == T_LIBRARY && m->depth && (m->depth > 1 || m->stack[0]->arg[0])) lib_top(s);   /* again: its top */
        else tab_open(s, arg);
        break;
    case A_M_PLAY: if (can(s, s->mstate == 1 ? "pause" : "play")) music_toggle(s); break;   /* as drawn: dimmed, nothing */
    case A_M_PREV: if (can(s, "previous")) send(s, "cmd previous"); break;
    case A_M_NEXT: if (can(s, "next")) send(s, "cmd next"); break;
    case A_M_SHUFFLE:
        if (arg == 1) { m->q_shuffle = !m->q_shuffle; send(s, "mq shuffle %d", m->q_shuffle); }
        else if (can(s, s->mshuffle ? "unshuffle" : "shuffle")) { s->mshuffle = !s->mshuffle; send(s, "cmd %s", s->mshuffle ? "shuffle" : "unshuffle"); }
        break;                                                  /* (one the controller does not offer: no state of ours) */
    case A_M_REPEAT: {                                          /* off, one, all: on to the next it offers */
        static const char *rc[] = { "repeat_off", "repeat_one", "repeat_all" };
        for (int i = 1; i < 3; i++) if (can(s, rc[(s->mrepeat + i) % 3])) { s->mrepeat = (s->mrepeat + i) % 3; send(s, "cmd %s", rc[s->mrepeat]); break; }
    } break;
    case A_M_CAT: if (arg != m->cat || m->depth != 1 || stale(m->stack[0])) cat_open(s, arg); break;
    case A_M_BACK: if (m->depth > 1) list_free(s, m->stack[--m->depth]); else lib_top(s); break;
    case A_M_ITEM: {
        struct mlist *l = shown_list(s);
        if (!l || arg >= l->n || !same(s, &l->it[arg])) break;
        struct mitem *it = &l->it[arg];
        if (opens(it->kind)) item_open(s, it);
        else if (it->uri[0]) play(s, "play", it->uri, it->title);
    } break;
    case A_M_DPLAY: case A_M_DSHUF: case A_M_DADD: case A_M_DRADIO: {
        struct mlist *l = m->depth ? m->stack[m->depth - 1] : NULL;
        if (!l || !l->arg[0]) break;
        if (a == A_M_DSHUF) { send(s, "mq shuffle 1"); m->q_shuffle = 1; }
        else if (a == A_M_DPLAY && m->q_shuffle) { send(s, "mq shuffle 0"); m->q_shuffle = 0; }
        play(s, a == A_M_DADD ? "add" : a == A_M_DRADIO ? "radio" : "play", l->arg, l->title);
        if (a == A_M_DPLAY || a == A_M_DSHUF) say(s, a == A_M_DSHUF ? "Shuffled" : "Playing");
    } break;
    case A_M_QITEM: if (arg < m->queue.n && same(s, &m->queue.it[arg])) send(s, "mq index %s", m->queue.it[arg].uri); break;   /* by its id: the
                                                                   queue may have moved (and the row under the finger with it) */
    case A_M_QCLEAR: m->sheet = 3; break;
    case A_M_FIELD: m->kb = 1; break;
    case A_M_CLEARQ: m->query[0] = 0; m->query_sent = 0; list_reset(&m->search); m->kb = 1; break;
    case A_M_KEY: key(s, arg); break;
    case A_M_LEAVE: send(s, "mg leave"); say(s, "Left the group"); break;
    case A_M_SPK: group_set(s, player_key(s, arg), -1); break;
    case A_M_SHADE: m->sheet = 0; break;
    case A_M_SHEET: {
        int sh = m->sheet; m->sheet = 0;
        if (sh == 1) {
            static const char *opt[] = { "play", "next", "add", "radio" };
            if (arg >= 0 && arg < 4) play(s, opt[arg], m->sheet_it.uri, m->sheet_it.title);
        } else if (sh == 2) {
            if (arg == 0) send(s, "mq index %s", m->sheet_it.uri);
            else if (arg == 1) { send(s, "mq delete %s", m->sheet_it.uri); say(s, "Removed"); }
        } else if (sh == 3) {
            if (arg == 0) { send(s, "mq clear"); m->queue.n = 0; say(s, "Queue cleared"); }
        } else if (sh == 4 && player(s, m->sheet_it.uri)) {
            struct mplayer *p = player(s, m->sheet_it.uri);
            if (arg == 0) { send(s, "mhere %s", p->id); say(s, "Moved here"); }
            else if (arg == 1 || arg == 2) group_set(s, p, arg == 1);   /* (as a tap on its row would) */
        }
    } break;
    default: return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------- messages */

static void field(char *out, size_t n, const char **p)          /* up to the next tab */
{
    size_t k = 0;
    while (**p && **p != '\t') { if (k + 1 < n) out[k++] = **p; (*p)++; }
    out[k] = 0;
    if (**p == '\t') (*p)++;
}

void music_msg(struct scene *s, const char *cmd, const char *a)
{
    struct music *m = &s->m;
    if (eq(cmd, "ma")) {
        char st[16] = ""; int n = 0;
        sscanf(a, "%15s %n", st, &n);
        int was = m->ma;
        m->ma = eq(st, "connecting") ? 1 : eq(st, "up") ? 2 : eq(st, "auth") ? 3 : eq(st, "error") ? 4 : 0;
        snprintf(m->ma_text, sizeof m->ma_text, "%s", n ? a + n : "");
        if (m->ma != 2) { m->npl = 0; m->pl_all = 0; m->q_known = 0; }
        if (m->ma == 2 && was != 2) {                           /* (again): what the page shows, loaded anew */
            for (int i = 0; i < m->depth; i++) list_free(s, m->stack[i]);
            m->depth = 0; list_reset(&m->queue);
            if (s->view == V_MUSIC) tab_open(s, m->tab);
            if (strlen(m->query) >= 2) m->query_at = s->now;    /* a search typed meanwhile (never sent, or lost) */
        }
    } else if (eq(cmd, "pl")) {
        struct mplayer p; memset(&p, 0, sizeof p); char vol[8];
        field(p.id, sizeof p.id, &a); field(p.name, sizeof p.name, &a); field(p.flags, sizeof p.flags, &a);
        field(p.state, sizeof p.state, &a); field(vol, sizeof vol, &a); field(p.leader, sizeof p.leader, &a);
        field(p.members, sizeof p.members, &a); field(p.playing, sizeof p.playing, &a);
        char gv[12]; field(gv, sizeof gv, &a);
        p.vol = atoi(vol); p.gvol = gv[0] ? atoi(gv) : -1;
        if (!p.id[0]) return;
        struct mplayer *e = player(s, p.id);
        if (e) p.key = e->key;
        else if (m->npl < MPLAYERS) { e = &m->pl[m->npl++]; p.key = ++m->pl_keys; }
        if (e) *e = p;
    } else if (eq(cmd, "pl-")) {
        for (int i = 0; i < m->npl; i++) if (eq(m->pl[i].id, a)) { memmove(&m->pl[i], &m->pl[i + 1], (size_t)(m->npl - i - 1) * sizeof m->pl[0]); m->npl--; break; }
    } else if (eq(cmd, "pl.")) m->pl_all = 1;
    else if (eq(cmd, "pq")) {
        char sh[4], rp[8], ix[12], ni[12];
        field(m->q_state, sizeof m->q_state, &a); field(sh, sizeof sh, &a); field(rp, sizeof rp, &a);
        field(ix, sizeof ix, &a); field(ni, sizeof ni, &a); field(m->q_name, sizeof m->q_name, &a);
        m->q_shuffle = atoi(sh); m->q_repeat = eq(rp, "all") ? 2 : eq(rp, "one") ? 1 : 0;
        int idx = atoi(ix), changed = idx != m->q_index;
        m->q_index = idx; m->q_items = atoi(ni); m->q_known = 1;
        for (int i = 0; changed && i < m->queue.n; i++) {     /* the current track's mark moves on */
            char *f = m->queue.it[i].flags, *c = strchr(f, 'c');
            if (c) memmove(c, c + 1, strlen(c));
            if (i == idx && strlen(f) + 1 < sizeof m->queue.it[i].flags) strcat(f, "c");
        }
    } else if (eq(cmd, "pq-")) m->q_known = 0;
    else if (eq(cmd, "merr")) { snprintf(m->toast, sizeof m->toast, "%s", a); m->toast_until = s->now + 3500; }
    else if (eq(cmd, "qc")) { if (s->view == V_MUSIC && m->tab == T_QUEUE) list_refresh(s, &m->queue); }
    else if (eq(cmd, "L")) {
        int req = 0, off = 0, total = -1;
        if (sscanf(a, "%d %d %d", &req, &off, &total) < 2) return;
        struct mlist *l = by_req(s, req);
        if (!l) return;
        if (l->fill >= 0) l->fill = off < 0 ? 0 : off > l->n ? l->n : off;   /* asked again: these replace what is
                                                                   there (an offset past it never asked for: on at its end) */
        else if (!off) l->n = 0;
        l->total = total; l->page0 = l->fill >= 0 ? l->fill : l->n;
    } else if (eq(cmd, "I")) {
        int req = atoi(a); const char *p = strchr(a, ' ');
        struct mlist *l = p ? by_req(s, req) : NULL;
        if (!l) return;
        int at = l->fill >= 0 ? l->fill : l->n;
        if (at > l->n) return;                                  /* rows come in order (an offset never asked for: no row) */
        if (at >= l->cap) {
            int cap = l->cap ? l->cap * 2 : 64; struct mitem *it = realloc(l->it, (size_t)cap * sizeof *it);
            if (!it) return;
            l->it = it; l->cap = cap;
        }
        struct mitem *it = &l->it[at]; memset(it, 0, sizeof *it); char k[4], dur[12];
        p++;
        field(k, sizeof k, &p); field(it->uri, sizeof it->uri, &p); field(it->title, sizeof it->title, &p);
        field(it->sub, sizeof it->sub, &p); field(it->img, sizeof it->img, &p); field(dur, sizeof dur, &p);
        field(it->flags, sizeof it->flags, &p);
        it->kind = k[0]; it->dur = atoi(dur);
        if (l->fill >= 0) l->fill++;
        if (at >= l->n) l->n = at + 1;
    } else if (eq(cmd, "E")) {
        int req = atoi(a); const char *p = strchr(a, ' ');
        struct mlist *l = by_req(s, req);
        if (!l) return;
        l->loading = 0;
        int refreshed = l->fill >= 0;
        if (p && p[1]) { snprintf(l->err, sizeof l->err, "%s", p + 1); l->failed = 1; l->fill = -1; l->failed_at = s->now; }
        else if (refreshed) {                                   /* asked again: on as far as it was, or the end */
            int last = l->fill == l->page0 || (l->total >= 0 && l->fill >= l->total);
            if (last) { l->n = l->fill; l->done = 1; }
            if (last || l->fill >= l->refill) l->fill = -1;
            else { list_ask(s, l, l->fill); return; }
        } else if (l->n == l->page0 || (l->total >= 0 && l->n >= l->total)) l->done = 1;   /* a page with nothing: the end */
        /* the queue opens where it plays: pages on to that row first (it may be far past the first), unless the
         * finger moved it meanwhile; asked again meanwhile (qc), that after */
        if (l == &m->queue && !refreshed && m->q_goto) {
            if (l->sc.pos != 0 || !m->q_known || m->q_index <= 2 || l->failed) m->q_goto = 0;
            else if (l->n <= m->q_index && !l->done) { list_ask(s, l, l->n); return; }
            else {
                m->q_goto = 0;
                float mx = fmaxf(0, l->n * U(ROW) + U(16) - (s->h - U(HDR)));   /* (as list_view has it: the last frame's */
                if (l->n) l->sc.pos = fminf(fmaxf(0, (fminf(m->q_index, l->n) - 2) * U(ROW)), mx);   /* max, of fewer rows, */
                l->sc.max = mx;                                 /* not to spring back from, nor past the end) */
            }
        }
        if (l->again) list_refresh(s, l);
    }
}

void music_init(struct scene *s)
{
    struct music *m = &s->m;
    m->tab = T_NOW; m->cat = 1;
    snprintf(m->queue.what, sizeof m->queue.what, "queue"); list_reset(&m->queue); m->queue.kind = 'q';
    snprintf(m->search.what, sizeof m->search.what, "search"); list_reset(&m->search);
}

void music_free(struct scene *s)
{
    struct music *m = &s->m;
    for (int i = 0; i < m->depth; i++) list_free(s, m->stack[i]);
    m->depth = 0;
    free(m->queue.it); free(m->search.it);
    m->queue.it = m->search.it = NULL;
}

