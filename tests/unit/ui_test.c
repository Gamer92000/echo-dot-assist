/* The screen UI's scene (src/ui/) on the PC, against a stand-in platform: text as blocks, images as their key's colour.
 * What it checks:
 *   - every frame drawn into its dirty rectangle only, on the last one, is byte for byte the frame drawn whole
 *     (the display list diff misses nothing), through scripted runs of every view, pattern, scroll and touch; no id
 *     twice in a frame
 *   - a still face asks for no frames, a blink costs about its eyes' box
 *   - the drawer: pulled from the top over our screen, over an app only from the grip above the layer (a pull from the
 *     app's own top is the app's); its picture asked for, our home brought up, the app back when it closes with nothing
 *     chosen and its picture held, nothing drawn, until the app is in front; a tap near the top still presses
 *   - the home app: Kiosk Satellite when installed and none chosen, back when it leaves (three times a minute), one the
 *     page picks applies at once (with the drawer out: once it is in), the drawer's Home goes to it after the drawer is
 *     gone; Back along the band over an app; a drawer over an app that took the screen back goes, the app's picture
 *     never shows under a later launch
 *   - the apps listed to hassmic (the page's choice of home app)
 *   - Music Assistant: lists asked for page by page as they scroll, a fling (a tap stops it, nothing more), opening an
 *     album, the action sheet, the keyboard's search, the speakers' group and volumes (a swipe across a slider is not
 *     the slider's, a tap on it is), the queue asked again in place, the setup card without the API
 *   - the messages to hassmic: play/pause/next/seek/volume, mplay, mq, mg, mv, Drop In, timer stop, vol, dnd
 * and how long frames take (on the PC: the Echo Show's A53 is roughly 10x slower).
 *   ui_test [dir]     also writes the frames named as PPM into dir
 */
#include "../../src/ui/ui.h"                               /* (the scene's state, to look at) */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define W 960
#define H 470
#define BAND 10

static uint32_t inc[W * H], ref[W * H];
static char sent[256][160]; static int nsent;
static char launched[64][200]; static int nlaunched, backs, apps_asked, shots_asked, sleeps;
static char asked[64][48]; static int asked_px[64], nasked;
static const char *shots;
static struct scene *S;

static int fake_text(void *ctx, const char *s, int px, int bold, int maxw, struct ui_text *out)
{
    (void)ctx;
    int n = (int)strlen(s), cw = px / 2 + (bold ? 2 : 0), w = n * cw;
    if (w > maxw) w = maxw - maxw % cw;
    if (w <= 0) return -1;
    out->w = w; out->h = px; out->stride = w;
    out->m = calloc((size_t)w * px, 1);
    for (int i = 0; i * cw < w; i++)                           /* a block per character, its code in its height */
        for (int y = px / 4 + ((unsigned char)s[i] % 7); y < px - px / 5; y++)
            for (int x = i * cw + 1; x < (i + 1) * cw - 1; x++) out->m[y * w + x] = (uint8_t)(160 + ((unsigned char)s[i] * 13) % 96);
    return 0;
}
static void fake_launch(void *ctx, const char *pkg, const char *cls)
{
    (void)ctx;
    assert(nlaunched < 64);
    snprintf(launched[nlaunched++], sizeof launched[0], "%s/%s", pkg ? pkg : "HOME", cls ? cls : "");
}
static void fake_back(void *ctx) { (void)ctx; backs++; }
static long long now = 1000000;
static void msg(const char *m) { scene_msg(S, m, now); }
static void answer(const char *l);
static int hold_answers; static char held[160];                 /* an answer on its way: kept until the test gives it */
static void fake_send(void *ctx, const char *l)
{
    (void)ctx;
    if (nsent < 256) snprintf(sent[nsent++], sizeof sent[0], "%s", l);
    if (!strncmp(l, "ml ", 3)) { if (hold_answers) snprintf(held, sizeof held, "%s", l); else answer(l); }
}
static void fake_apps(void *ctx) { (void)ctx; apps_asked++; }
static void fake_image(void *ctx, const char *key, int px) { (void)ctx; if (nasked < 64) { asked_px[nasked] = px; snprintf(asked[nasked++], sizeof asked[0], "%s", key); } }
static void fake_shot(void *ctx) { (void)ctx; shots_asked++; }
static void fake_sleep(void *ctx) { (void)ctx; sleeps++; }
static const struct ui_platform plat = { fake_text, fake_launch, fake_back, fake_send, fake_apps, fake_image, fake_shot, fake_sleep, NULL };

/* Music Assistant's answers: pages of 60 out of 150 albums, the queue's items (8 unless the test says more), everything
 * else small */
static int pages, queue_total = 8, fail_lists, no_totals;      /* fail_lists: each answered with an error; no_totals: none told */
static int queue_gone = -1;                                      /* the queue's item there removed: the rows after it move up */
static void answer(const char *l)
{
    int req, off; char what[32];
    if (sscanf(l, "ml %d %31s %d", &req, what, &off) < 3) return;
    char b[400];
    if (fail_lists) { snprintf(b, sizeof b, "E %d timed out", req); msg(b); return; }
    int total = !strcmp(what, "albums") ? 150 : !strcmp(what, "queue") ? queue_total : 12, n = total - off < 60 ? total - off : 60;
    if (n < 0) n = 0;
    pages++;
    snprintf(b, sizeof b, "L %d %d %d", req, off, no_totals ? -1 : total); msg(b);
    char kind = !strcmp(what, "albums") || !strcmp(what, "artist_albums") ? 'a' : !strcmp(what, "artists") ? 'r' : !strcmp(what, "queue") ? 'q' : 't';
    for (int i = off; i < off + n; i++) {
        int u = kind == 'q' && queue_gone >= 0 && i >= queue_gone ? i + 1 : i;
        snprintf(b, sizeof b, "I %d %c\t%s://%s/%d\tItem %d of %s\tSomeone %d\timg%c%d\t%d\t%s", req, kind, kind == 'q' ? "qi" : "library", what, u, u,
                 what, i % 7, kind, i % 20, kind == 't' || kind == 'q' ? 200 + i : 0, kind == 'q' && i == 2 ? "c" : "");
        msg(b);
    }
    snprintf(b, sizeof b, "E %d", req); msg(b);
}

static int sent_has(const char *l) { for (int i = 0; i < nsent; i++) if (!strcmp(sent[i], l)) return 1; return 0; }
static int sent_prefix(const char *l) { for (int i = 0; i < nsent; i++) if (!strncmp(sent[i], l, strlen(l))) return 1; return 0; }

static double secs(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static int frames, drawn_px; static double t_inc, t_full, worst;

static void ppm(const char *name, const uint32_t *px)
{
    if (!shots) return;
    char path[512]; snprintf(path, sizeof path, "%s/ui_%s.ppm", shots, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) { unsigned char c[3] = { px[i] & 255, px[i] >> 8 & 255, px[i] >> 16 & 255 }; fwrite(c, 1, 3, f); }
    fclose(f);
}

/* the images asked for, given as the colour of their key: at the size asked (as the device's decoder gives them, drawn
 * without scaling), or every other one at 64 px (scaled) */
static void deliver(void)
{
    for (int i = 0; i < nasked; i++) {
        int px = i % 2 ? 64 : asked_px[i]; uint32_t *p = malloc((size_t)px * px * 4); unsigned h = 0;
        for (const char *k = asked[i]; *k; k++) h = h * 31 + (unsigned char)*k;
        for (int j = 0; j < px * px; j++) p[j] = 0xFF000000u | (h & 0xFFFFFF) | (uint32_t)(j % px) << 2;
        scene_image(S, asked[i], asked_px[i], p, px, px);
    }
    nasked = 0;
}

/* every id once a frame: one drawn twice is dirty in every frame (diff goes by its box), and a still screen never is */
static void ids_once(void)
{
    static unsigned char seen[MAXID];
    memset(seen, 0, sizeof seen);
    for (int i = 0; i < S->n[S->cur]; i++) {
        int id = S->list[S->cur][i].id;
        if (seen[id]++) { fprintf(stderr, "frame at %lld: id %d drawn twice (view %d)\n", now, id, S->view); exit(1); }
    }
}

/* one frame: dirty-only into inc, whole into ref, compared; the delay asked for */
static int frame(void)
{
    int next;
    double a = secs();
    struct rect d = scene_prepare(S, now, &next);
    ids_once();
    if (!rect_empty(d)) {
        struct gfx g = { inc, W, H, W, d };
        scene_draw(S, &g);
        double b = secs();
        t_inc += b - a; if (b - a > worst) worst = b - a;
        drawn_px += (d.x1 - d.x0) * (d.y1 - d.y0); frames++;
        memset(ref, 0x55, sizeof ref);
        struct gfx gr = { ref, W, H, W, { 0, 0, W, H } };
        scene_draw(S, &gr);
        t_full += secs() - b;
    } else if (scene_visible(S) && S->screen_on && S->leaving_until <= now) {   /* nothing to draw: nothing missed either
                                                                   (hidden or held, the layer shows nothing new) */
        memset(ref, 0x55, sizeof ref);
        struct gfx gr = { ref, W, H, W, { 0, 0, W, H } };
        scene_draw(S, &gr);
    }
    else memcpy(ref, inc, sizeof ref);
    if (memcmp(inc, ref, sizeof inc)) {
        for (int i = 0; i < W * H; i++) if (inc[i] != ref[i]) {
            fprintf(stderr, "frame at %lld: pixel %d,%d differs (dirty %d,%d-%d,%d)\n", now, i % W, i / W, d.x0, d.y0, d.x1, d.y1);
            ppm("FAIL_inc", inc); ppm("FAIL_ref", ref); exit(1);
        }
    }
    deliver();
    return next;
}

/* runs ms of time: frames when asked for, as the platform's loop does */
static void run(int ms)
{
    long long end = now + ms;
    while (now < end) {
        int next = frame();
        long long step = next < 0 ? end - now : next;
        if (step < 1) step = 1;
        now += step < end - now ? step : end - now;
    }
    frame();
}

static void tap(int x, int y) { scene_touch(S, TOUCH_DOWN, x, y, now); now += 60; scene_touch(S, TOUCH_UP, x, y, now); run(40); }
static void hold(int x, int y) { scene_touch(S, TOUCH_DOWN, x, y, now); run(700); scene_touch(S, TOUCH_UP, x, y, now); run(60); }
static void drag(int x0, int y0, int x1, int y1, int steps, int ms_each)
{
    scene_touch(S, TOUCH_DOWN, x0, y0, now);
    for (int i = 1; i <= steps; i++) { now += ms_each; scene_touch(S, TOUCH_MOVE, x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps, now); frame(); }
    scene_touch(S, TOUCH_UP, x1, y1, now); run(50);
}
static void pull_drawer(void) { drag(W / 2, 3, W / 2, 300, 12, 16); run(600); }
/* a text of the frame last built that has this in it */
static int drawn_text(const char *needle)
{
    for (int i = 0; i < S->n[S->cur]; i++) {
        const struct item *it = &S->list[S->cur][i];
        if (it->kind == K_TEXT && it->ref && strstr(((const struct ui_text_e *)it->ref)->key, needle)) return 1;
    }
    return 0;
}

/* the middle of the (last drawn) hit for act (arg < 0: any) */
static void hit_mid(int act, int arg, int *x, int *y)
{
    for (int i = S->nhits - 1; i >= 0; i--) if (S->hits[i].act == act && (arg < 0 || S->hits[i].arg == arg)) {
        *x = (S->hits[i].r.x0 + S->hits[i].r.x1) / 2; *y = (S->hits[i].r.y0 + S->hits[i].r.y1) / 2; return;
    }
    fprintf(stderr, "no hit %d\n", act); exit(1);
}
static int tab_y(int t) { return (int)(76 + (t + 0.5f) * (H - 76) / 5); }   /* the rail, music.c's layout */

int main(int argc, char **argv)
{
    shots = argc > 1 ? argv[1] : NULL;
    setenv("TZ", "UTC0", 1);
    S = scene_new(W, H, BAND, &plat, NULL, now);
    scene_top(S, UI_HOME_PKG, now);
    msg("name Echo Show 5 fdd111"); msg("act both volume keys"); msg("web http://10.0.0.5:28931"); msg("ev 40"); msg("home -");
    assert(nlaunched == 0);                                       /* no home app: the face */

    nsent = 0; msg("ping 7"); assert(sent_has("pong 7"));      /* hassmic's: this UI's main loop goes on */
    /* the still face: frames only for blinks, glances and the clock */
    run(300);
    int f0 = frames, px0 = drawn_px;
    run(20000);
    printf("idle face: %d frames in 20 s, %.0f px each\n", frames - f0, (double)(drawn_px - px0) / (frames - f0 ? frames - f0 : 1));
    assert(frames - f0 < 20000 / 33 / 2);                       /* far from every frame */
    assert((drawn_px - px0) / (frames - f0 ? frames - f0 : 1) < W * H / 4);   /* each about the eyes' box */
    ppm("face", inc);

    /* a conversation */
    msg("p+ active-start"); for (int i = 0; i < 20; i++) { char l[16]; snprintf(l, sizeof l, "lv %d", (i * 37) % 100); msg(l); run(50); }
    msg("p- active-start"); msg("p+ active-thinking"); run(800);
    msg("p- active-thinking"); msg("p+ active-talking");
    for (int i = 0; i < 30; i++) { char l[16]; snprintf(l, sizeof l, "lv %d", (i * 53) % 100); msg(l); run(40); }
    ppm("talking", inc);
    msg("p- active-talking"); msg("p+ active-end"); run(1000);
    /* told to a new UI with how long ago it came on: a timed one long over stays over, a recent one shows */
    int pe = pat_find("anim_start_error_short");
    msg("p+ anim_start_error_short 10000"); run(100); assert(!S->pat[pe].on);
    msg("p+ anim_start_error_short 200"); run(100); assert(S->pat[pe].on);
    run(3500); assert(!S->pat[pe].on);

    /* states with captions and buttons */
    msg("mute 1"); msg("p+ mics-off_on"); run(500); ppm("muted", inc);
    msg("mute 0"); msg("p- mics-off_on"); msg("dnd 1"); run(500);
    msg("dnd 0"); msg("timers tea, 5 min"); msg("p+ ready-timer"); run(600); ppm("timer", inc);
    tap((int)(0.72f * W), (int)(0.8f * H)); assert(sent_has("timer stop"));
    nsent = 0; hold((int)(0.72f * W), (int)(0.8f * H)); assert(sent_has("timer stop"));   /* a firm press: a long press
                                                                     nothing takes is still a tap */
    msg("timers "); msg("p- ready-timer"); run(300);
    msg("peer echo-dot-3-5695c4"); msg("p+ call_incoming"); run(400);
    tap((int)(0.8f * W), (int)(0.8f * H)); assert(sent_has("dropin answer"));
    msg("p- call_incoming"); msg("p+ call_connected"); run(300);
    tap((int)(0.75f * W), (int)(0.8f * H)); assert(sent_has("dropin hangup"));
    msg("p- call_connected"); msg("p+ call_connected_end"); run(1600);
    msg("oobe 1"); run(2000); msg("oobe 0");

    /* Sendspin's music: the face's bottom line, then the music's own page */
    msg("mc 35 0 play,pause,next,previous,volume,mute,seek,shuffle,unshuffle,repeat_off,repeat_one,repeat_all off 0");
    msg("mt Bohemian Rhapsody\tQueen\tA Night at the Opera");
    msg("mp 1 61000 354000 1000");
    uint32_t *art = malloc(400 * 400 * 4);
    for (int y = 0; y < 400; y++) for (int x = 0; x < 400; x++) art[y * 400 + x] = 0xFF000000u | (uint32_t)(x * 255 / 400) << 16 | (uint32_t)(y * 255 / 400) | 0x4000;
    scene_art(S, art, 400, 400);
    run(400); ppm("face_music", inc);
    tap((int)(0.78f * W) - 37, (int)(0.87f * H));                 /* the bottom line's play/pause */
    assert(sent_has("cmd pause"));
    /* shown at once: an older word from Sendspin does not flip it back, its own confirms it; a tap it never agrees
     * with goes back to what it said, after 2.5 s */
    assert(S->mstate == 2);
    msg("mp 1 62000 354000 1000"); run(100); assert(S->mstate == 2);
    msg("mp 2 62100 354000 1000"); run(100); assert(S->mstate == 2);
    nsent = 0; tap((int)(0.78f * W) - 37, (int)(0.87f * H));
    assert(sent_has("cmd play") && S->mstate == 1);
    run(2000); assert(S->mstate == 1);
    run(700); assert(S->mstate == 2 && music_pos(S) == 62100);
    msg("mp 1 62100 354000 1000"); run(100);
    /* pause, then play at once: a word older than both that agrees with the play, then the pause's answer: still play */
    tap((int)(0.78f * W) - 37, (int)(0.87f * H)); run(300); tap((int)(0.78f * W) - 37, (int)(0.87f * H)); assert(S->mstate == 1);
    msg("mp 1 62200 354000 1000"); run(50); msg("mp 2 62300 354000 1000"); run(100); assert(S->mstate == 1);
    msg("mp 1 62300 354000 1000"); run(2600); assert(S->mstate == 1);
    tap(W / 2, (int)(0.87f * H)); run(1500); ppm("music_now", inc);
    nsent = 0;
    { int x, y; hit_mid(A_M_NEXT, -1, &x, &y); tap(x, y); } run(100);   /* next: 4th of 5 buttons right of the cover */
    assert(sent_has("cmd next"));
    { int x, y; hit_mid(A_M_VOL, -1, &x, &y); drag(x - 60, y, x + 40, y, 6, 16); } run(100);   /* the group's volume */
    assert(sent_prefix("cmd volume "));
    msg("mc 35 0 next,previous off 0"); run(100); nsent = 0;     /* play/pause not offered: drawn dimmed, does nothing */
    { int x, y, st = S->mstate; hit_mid(A_M_PLAY, -1, &x, &y); tap(x, y); run(100); assert(!sent_prefix("cmd ") && S->mstate == st); }
    /* repeat and shuffle on to what the controller offers only (no state it would never confirm) */
    msg("mc 35 0 play,pause,next,volume,seek,repeat_all,shuffle off 0"); run(100);
    { int x, y; nsent = 0; hit_mid(A_M_REPEAT, -1, &x, &y); tap(x, y); run(50);
      assert(nsent == 1 && sent_has("cmd repeat_all") && S->mrepeat == 2);   /* (off: past one, not offered, to all) */
      nsent = 0; tap(x, y); run(50); assert(!nsent && S->mrepeat == 2);      /* (all: neither off nor one offered) */
      msg("mc 35 0 play,pause,next,volume,seek,repeat_all,shuffle all 1"); run(50);
      nsent = 0; hit_mid(A_M_SHUFFLE, 0, &x, &y); tap(x, y); run(50); assert(!nsent && S->mshuffle == 1); }
    msg("mc 35 0 play,pause,next,previous,volume,mute,seek,shuffle,unshuffle,repeat_off,repeat_one,repeat_all off 0"); run(100);
    /* seek: the bar dragged */
    drag(560, 256, 760, 256, 10, 16);
    assert(sent_prefix("cmd seek "));
    /* a finger resting on the bar as an app comes up: let go over the app, no seek */
    { int x, y; hit_mid(A_M_SEEK, -1, &x, &y); nsent = 0;
      scene_touch(S, TOUCH_DOWN, x + 100, y, now); run(100);
      scene_top(S, "org.example.app09", now); run(3500); assert(!scene_visible(S));   /* (past the grace for our own) */
      now += 16; scene_touch(S, TOUCH_UP, x + 100, y, now); run(100); assert(!sent_prefix("cmd seek"));
      scene_top(S, UI_HOME_PKG, now); run(300); assert(scene_visible(S)); }
    msg("p+ active-start"); run(200); assert(scene_visible(S) && S->view == V_FACE); msg("p- active-start"); run(200);   /* the face for a request */
    music_show(S, 0); run(300); assert(S->view == V_MUSIC);

    /* without Music Assistant's API: the setup card */
    tap(W / 2, (int)(0.87f * H)); run(300);
    tap(46, tab_y(2)); run(300); ppm("music_setup", inc);
    assert(!sent_prefix("ml "));
    /* with it: the library, page by page */
    msg("ma up Music Assistant"); run(200);
    assert(sent_prefix("ml ") && pages == 1);
    for (int i = 0; i < 6; i++) drag(500, 420, 500, 120, 8, 16);  /* scrolled towards the end: more pages */
    run(1500);
    assert(pages >= 2);
    ppm("music_library", inc);
    /* a fling: the list goes on after the finger; a tap stops it, and opens nothing */
    drag(500, 400, 500, 300, 4, 10); run(30);
    int fl = frames; run(100); assert(frames - fl > 2);
    struct mlist *fll = S->m.stack[S->m.depth - 1]; int fd = S->m.depth; nsent = 0;
    assert(fll->sc.vel != 0);
    tap(500, 250); run(100);
    assert(fll->sc.vel == 0 && S->m.depth == fd && !sent_prefix("ml ") && !sent_prefix("mplay"));
    tap(500, 250); run(300); assert(S->m.depth == fd + 1);        /* the next tap is a tap */
    tap(40 + 92, 35); run(300); assert(S->m.depth == fd);         /* (Back) */
    /* a fling, then another tab while it moves: back at it, it stays where it was and a tap is a tap */
    drag(500, 400, 500, 300, 4, 10); run(30);
    struct mlist *fl2 = S->m.stack[S->m.depth - 1]; assert(fl2->sc.vel != 0);
    tap(46, tab_y(0)); run(500); tap(46, tab_y(2)); run(100);
    { float p0 = fl2->sc.pos; run(1000); assert(fl2->sc.vel == 0 && fabsf(fl2->sc.pos - p0) < 1); }
    tap(500, 250); run(300); assert(S->m.depth == fd + 1);
    tap(40 + 92, 35); run(300); assert(S->m.depth == fd);
    /* a finger on a row as a request brings the face: let go, it opens nothing (the row is not what shows) */
    nsent = 0; scene_touch(S, TOUCH_DOWN, 500, 250, now); run(50);
    msg("p+ active-start"); run(100); now += 30; scene_touch(S, TOUCH_UP, 500, 250, now); run(100);
    assert(S->view == V_FACE && S->m.depth == fd && !sent_prefix("ml ") && !sent_prefix("mplay"));
    msg("p- active-start"); run(800); music_show(S, 2); run(300);
    /* the artists, one opened, its album, its tracks */
    tap(92 + 24 + 40, 36); run(300);
    int p0 = pages; tap(200, 150); run(300);
    assert(pages == p0 + 1 && sent_prefix("ml ") && strstr(sent[nsent - 1], "artist_albums"));
    tap(200, 70 + 18 + 132 + 26 + 60); run(300);
    assert(strstr(sent[nsent - 1], "album_tracks"));
    ppm("music_album", inc);
    nsent = 0;
    tap(340, 70 + 18 + 132 - 20); run(100);                       /* Play */
    assert(sent_prefix("mplay play library://"));
    { struct mlist *al = S->m.stack[S->m.depth - 1]; float p0 = al->sc.pos;   /* held: the sheet; the finger on: no scroll under it */
      scene_touch(S, TOUCH_DOWN, 500, 70 + 176 + 64 + 30, now); run(700); assert(S->m.sheet == 1);
      for (int i = 1; i <= 6; i++) { now += 16; scene_touch(S, TOUCH_MOVE, 500, 70 + 176 + 64 + 30 - i * 30, now); run(16); }
      now += 16; scene_touch(S, TOUCH_UP, 500, 70 + 176 + 64 + 30 - 180, now); run(300);
      assert(al->sc.pos == p0 && S->m.sheet == 1);
      tap(W / 2, 30); run(100); assert(!S->m.sheet); }          /* (the shade: closed) */
    hold(500, 70 + 176 + 64 + 30); run(100); ppm("music_sheet", inc);
    assert(S->m.sheet == 1);
    nsent = 0; tap(W / 2, H / 2 - 20); run(100);                  /* a row of the sheet */
    assert(sent_prefix("mplay ") && !S->m.sheet);
    { int d = S->m.depth; tap(130, 12); run(200);                 /* back: in the top edge, still a tap */
      assert(!drawer_open(S) && S->m.depth == d - 1); }
    tap(130, 35); run(200);

    /* the queue */
    nsent = 0; tap(46, tab_y(1)); run(300); ppm("music_queue", inc);
    assert(sent_prefix("ml ") && strstr(sent[0], "queue"));
    tap(500, 70 + 64 + 30); run(100); assert(sent_has("mq index qi://queue/1"));   /* by its id */
    nsent = 0; msg("qc"); run(100); assert(sent_prefix("ml ") && strstr(sent[nsent - 1], " queue 0"));
    /* its items changed while it was scrolled (radio adds a track): the rows and the place stay while it is asked again */
    drag(500, 400, 500, 150, 8, 16); run(800);
    float qp = S->m.queue.sc.pos; int qn = S->m.queue.n;
    assert(qp > 10 && qn > 0);
    hold_answers = 1; msg("qc"); run(300);
    assert(S->m.queue.n == qn && fabsf(S->m.queue.sc.pos - qp) < 1);
    hold_answers = 0; answer(held); run(300);
    assert(S->m.queue.n == qn && fabsf(S->m.queue.sc.pos - qp) < 1);
    /* a long one, scrolled past its first page: refreshed page by page as far as it was, the place kept */
    queue_total = 150; msg("qc"); run(300);
    for (int i = 0; i < 12; i++) drag(500, 420, 500, 100, 8, 16);
    run(1500);
    qp = S->m.queue.sc.pos; qn = S->m.queue.n;
    assert(qn > 60 && qp > 60 * 64);
    nsent = 0; msg("qc"); run(300);
    { int again = 0; for (int i = 0; i < nsent; i++) { int r, o; if (sscanf(sent[i], "ml %d queue %d", &r, &o) == 2 && o == 60) again = 1; }
      assert(again && S->m.queue.n == qn && fabsf(S->m.queue.sc.pos - qp) < 1); }   /* its second page asked again too */
    /* at its top it stays there (the jump to what plays is the first opening's) */
    for (int i = 0; i < 20; i++) drag(500, 100, 500, 440, 6, 16);
    run(1500); assert(S->m.queue.sc.pos == 0);
    msg("pq playing\t0\toff\t40\t150\tQueue"); msg("qc"); run(500);
    assert(S->m.queue.sc.pos == 0);
    /* opened again with what plays far past its first page: pages on to it, and there it stays */
    msg("pq playing\t0\toff\t100\t150\tQueue");
    tap(46, tab_y(0)); run(300); tap(46, tab_y(1)); run(1500);
    assert(S->m.queue.n > 100 && fabsf(S->m.queue.sc.pos - 98 * 64) < 1);
    /* the same with its items changed (qc) while the first page is on its way: there all the same */
    tap(46, tab_y(0)); run(300);
    hold_answers = 1; tap(46, tab_y(1)); run(50); msg("qc"); run(50);
    nsent = 0; hold_answers = 0; answer(held); run(1500);
    assert(S->m.queue.n > 100 && fabsf(S->m.queue.sc.pos - 98 * 64) < 1);
    { int again = 0; for (int i = 0; i < nsent; i++) again |= strstr(sent[i], " queue 0") != NULL;
      assert(again); }                                            /* (and the qc's: asked again once that was in) */
    /* stopped past its top on the way back, then dragged: on from there, not by a jump */
    for (int i = 0; i < 25; i++) drag(500, 100, 500, 440, 6, 16);
    run(1500);                                                    /* (at its top) */
    drag(500, 150, 500, 440, 12, 16);                             /* past it, let go: on its way back */
    { float p1; assert(S->m.queue.sc.pos < -30); scene_touch(S, TOUCH_DOWN, 500, 250, now); run(16); p1 = S->m.queue.sc.pos;
      run(300); assert(fabsf(S->m.queue.sc.pos - p1) < 1);        /* (the finger resting on it: it stays there) */
      now += 16; scene_touch(S, TOUCH_MOVE, 500, 250 + TAP_PX + 2, now); run(16);
      assert(fabsf(S->m.queue.sc.pos - p1) < TAP_PX + 4); now += 16; scene_touch(S, TOUCH_UP, 500, 250 + TAP_PX + 2, now); run(800); }
    /* an answer at an offset never asked for (asked again, in place), to its end: no write past the list, nor rows
     * counted that are not there (the current mark moved over them next) */
    { int n0 = S->m.queue.n; hold_answers = 1; msg("qc"); run(50);
      char b[160]; snprintf(b, sizeof b, "L %d 1000 -1", S->m.queue.req); msg(b);
      snprintf(b, sizeof b, "I %d q\tqi://x/1\tStray\t\t\t0\t", S->m.queue.req); msg(b);
      snprintf(b, sizeof b, "E %d", S->m.queue.req); msg(b);
      assert(S->m.queue.n <= n0 + 1 && S->m.queue.n <= S->m.queue.cap);
      msg("pq playing\t0\toff\t7\t150\tQueue"); run(50);
      hold_answers = 0; answer(held); run(300); assert(S->m.queue.n <= S->m.queue.cap); }
    queue_total = 8;

    /* search: typed on the keyboard, sent when the typing pauses */
    nsent = 0; tap(46, tab_y(3)); run(300);
    float kw = (W - 92) / 10.0f, kh = 232 / 4.0f, ky = H - 232;
    tap((int)(92 + 3.5f * kw), (int)(ky + kh / 2)); tap((int)(92 + 2.5f * kw), (int)(ky + kh / 2));   /* r e */
    assert(!sent_prefix("ml "));
    run(600);
    assert(sent_prefix("ml ") && strstr(sent[nsent - 1], "search 0 re"));
    ppm("music_search", inc);
    /* one letter left: no search, and not the last one's results either */
    tap((int)(92 + 9.5f * kw), (int)(ky + 2.5f * kh)); run(700);  /* (backspace: "r") */
    assert(!strcmp(S->m.query, "r") && !S->m.query_sent && !S->m.search.n);
    /* a search Music Assistant answers with an error: that error, not "Searching…" for ever */
    hold_answers = 1; hold((int)(92 + 4.5f * kw), (int)(ky + kh / 2)); run(700);  /* (r t: "rt"; a key held: typed) */
    assert(!strcmp(S->m.query, "rt"));
    { int req = 0; sscanf(held, "ml %d search", &req); char b[80]; snprintf(b, sizeof b, "E %d the search failed", req); msg(b); }
    hold_answers = 0; run(300);
    assert(drawn_text("the search failed") && !drawn_text("Searching"));
    tap((int)(92 + 9.5f * kw), (int)(ky + 2.5f * kh)); run(700);  /* (back to "r") */
    tap((int)(92 + 2.5f * kw), (int)(ky + kh / 2)); run(700);    /* (and "re" again) */
    /* an artist opened from the results: its page on top of the library, Back to where the library was */
    int d0 = S->m.depth;
    { float kw2 = (W - 92) / 10.0f; tap((int)(92 + 8.9f * kw2), H - 20); } run(700);   /* Done: the keyboard away, searched again */
    { int req = 0; for (int i = 0; i < nsent; i++) sscanf(sent[i], "ml %d search", &req);
      char b[200]; snprintf(b, sizeof b, "L %d 0 1", req); msg(b);
      snprintf(b, sizeof b, "I %d r\tlibrary://artist/7\tSomeone\t\tar7\t0\t", req); msg(b);
      snprintf(b, sizeof b, "E %d", req); msg(b); }
    run(100);
    tap(400, 70 + 10 + 42 + 30); run(300);                        /* the heading, then the artist's row */
    assert(S->m.tab == 2 && S->m.depth == d0 + 1 && S->m.stack[d0]->arg[0] && strstr(sent[nsent - 1], "artist_albums"));
    tap(40 + 92, 35); run(300);                                   /* Back */
    assert(S->m.depth >= 1 && !S->m.stack[S->m.depth - 1]->arg[0]);

    /* the speakers: who plays with this Echo */
    msg("pl self1\tEcho Show 5\tsgva\tplaying\t45\t\tself1,dot2\tBohemian Rhapsody");
    msg("pl dot2\tEcho Dot\tgva\tplaying\t30\tself1\t\t");
    msg("pl sonos\tSonos Move\tgva\tpaused\t22\t\t\tTeardrop");
    msg("pl tv\tTV\tva\tidle\t15\t\t\t"); msg("pl.");
    nsent = 0; tap(46, tab_y(4)); run(300); ppm("music_speakers", inc);
    tap(300, 70 + 64 + 3 * 76 - 38); run(100);                    /* Sonos: into the group */
    assert(sent_has("mg add sonos"));
    tap(300, 70 + 64 + 2 * 76 - 38); run(100);                    /* the Dot: out of it */
    assert(sent_has("mg remove dot2"));
    drag(940 - 200, 70 + 64 + 2 * 76 - 38, 940 - 100, 70 + 64 + 2 * 76 - 38, 6, 16);
    assert(sent_prefix("mv dot2 "));
    drag(400, 70 + 32, 800, 70 + 32, 6, 16);                      /* the group's volume */
    assert(sent_prefix("mgv "));
    nsent = 0; drag(940 - 150, 70 + 64 + 2 * 76 - 38, 940 - 150, 70 + 64 + 2 * 76 - 38 - 150, 8, 16);   /* a swipe up on a volume */
    assert(!sent_prefix("mv "));
    run(600);                                                     /* (the list back from past its end) */
    tap(940 - 150, 70 + 64 + 2 * 76 - 38); run(100);              /* a tap on it: that volume */
    assert(sent_prefix("mv dot2 "));
    hold(300, 70 + 64 + 3 * 76 - 38);                             /* long press: play what it plays here */
    msg("pl- dot2"); run(50);                                     /* (another speaker gone meanwhile: still the Sonos) */
    tap(W / 2, H / 2 - 50); run(100);
    assert(sent_has("mhere sonos"));
    msg("pl dot2\tEcho Dot\tgva\tplaying\t30\tself1\t\t"); run(50);
    /* this Echo plays with the Sonos: its row is no way out of its own group (Leave is) */
    msg("pl self1\tEcho Show 5\tsgva\tplaying\t45\tsonos\t\tTeardrop"); msg("pl sonos\tSonos Move\tgva\tplaying\t22\t\tsonos,self1\tTeardrop"); run(100);
    nsent = 0; { int x, y; struct mplayer *so = NULL; for (int i = 0; i < S->m.npl; i++) if (!strcmp(S->m.pl[i].id, "sonos")) so = &S->m.pl[i];
      hit_mid(A_M_SPK, so->key, &x, &y); tap(x, y); } run(100);
    assert(!sent_prefix("mg remove"));
    /* nor its sheet's "Take out of this group"; nor "Add to this group" for a speaker that cannot play with this Echo:
     * the sheet as the tap */
    for (int k = 0; k < 2; k++) {
        const char *id = k ? "tv" : "sonos"; struct mplayer *p = NULL; int y = -1;
        for (int i = 0; i < S->m.npl; i++) if (!strcmp(S->m.pl[i].id, id)) p = &S->m.pl[i];
        for (int i = 0; i < S->nhits; i++) if (S->hits[i].act == A_M_SPK && S->hits[i].arg == p->key) y = (S->hits[i].r.y0 + S->hits[i].r.y1) / 2;
        nsent = 0; hold(300, y); assert(S->m.sheet == 4);
        int x, yy; hit_mid(A_M_SHEET, k ? 1 : 2, &x, &yy); S->m.toast[0] = 0; tap(x, yy); run(100);
        assert(!S->m.sheet && !sent_prefix("mg ") && strstr(S->m.toast, k ? "cannot play with this Echo" : "plays with it"));
    }
    msg("pl self1\tEcho Show 5\tsgva\tplaying\t45\t\tself1,dot2\tBohemian Rhapsody"); msg("pl sonos\tSonos Move\tgva\tpaused\t22\t\t\tTeardrop"); run(100);
    /* the group's volume as Music Assistant has it (its players' average), not its leader's own; a leader whose own
     * volume is none: the group's row all the same */
    msg("pl self1\tEcho Show 5\tsga\tplaying\t0\t\tself1,dot2\tBohemian Rhapsody\t50"); run(100);
    { struct mplayer *me_ = NULL; for (int i = 0; i < S->m.npl; i++) if (!strcmp(S->m.pl[i].id, "self1")) me_ = &S->m.pl[i];
      assert(me_ && me_->gvol == 50 && drawn_text("Group volume"));
      nsent = 0; drag(400, 70 + 32, 800, 70 + 32, 6, 16); assert(sent_prefix("mgv ") && me_->gvol != 50 && me_->vol == 0); }
    msg("pl self1\tEcho Show 5\tsgva\tplaying\t45\t\tself1,dot2\tBohemian Rhapsody"); run(100);
    /* a finger on a speaker's row as the rows move (another speaker joins this Echo's group elsewhere): let go, not that
     * speaker's tap where another row is now */
    { struct mplayer *so = NULL; for (int i = 0; i < S->m.npl; i++) if (!strcmp(S->m.pl[i].id, "sonos")) so = &S->m.pl[i];
      int x, y; hit_mid(A_M_SPK, so->key, &x, &y); nsent = 0; scene_touch(S, TOUCH_DOWN, 300, y, now); run(50);
      msg("pl self1\tEcho Show 5\tsgva\tplaying\t45\t\tself1,dot2,tv\tBohemian Rhapsody"); msg("pl tv\tTV\tva\tplaying\t15\tself1\t\t"); run(50);
      int x2, y2; hit_mid(A_M_SPK, so->key, &x2, &y2); assert(y2 != y);
      now += 30; scene_touch(S, TOUCH_UP, 300, y, now); run(100); assert(!sent_prefix("mg "));
      msg("pl self1\tEcho Show 5\tsgva\tplaying\t45\t\tself1,dot2\tBohemian Rhapsody"); msg("pl tv\tTV\tva\tidle\t15\t\t\t"); run(100); }
    /* a finger resting on a row without a long press (this Echo's own), then moving: the list scrolls */
    { char b[120]; for (int i = 0; i < 10; i++) { snprintf(b, sizeof b, "pl x%d\tPlayer %d\tgva\tidle\t30\t\t\t", i, i); msg(b); }
      run(100); float p0 = S->m.spk.pos;
      scene_touch(S, TOUCH_DOWN, 300, 70 + 64 + 76 - 38, now); run(700); assert(!S->m.sheet);
      for (int i = 1; i <= 8; i++) { now += 16; scene_touch(S, TOUCH_MOVE, 300, 70 + 64 + 76 - 38 - i * 30, now); run(16); }
      now += 16; scene_touch(S, TOUCH_UP, 300, 70 + 64 + 76 - 38 - 240, now); run(600);
      assert(S->m.spk.max > 0 && S->m.spk.pos > p0 + 100);
      for (int i = 0; i < 10; i++) { snprintf(b, sizeof b, "pl- x%d", i); msg(b); }
      run(800); assert(S->m.spk.pos == 0); }

    /* the drawer over our own screen: DND, the volume; no apps in it */
    nsent = 0; pull_drawer();
    assert(shots_asked == 0);
    struct ui_app *apps = calloc(2, sizeof *apps);
    snprintf(apps[0].pkg, sizeof apps[0].pkg, "me.jxl.kiosk_satellite"); snprintf(apps[0].label, sizeof apps[0].label, "Kiosk Satellite");
    snprintf(apps[1].pkg, sizeof apps[1].pkg, "org.example.app01"); snprintf(apps[1].label, sizeof apps[1].label, "App 1"); apps[1].system = 1;
    scene_apps(S, apps, 2);
    assert(sent_has("app me.jxl.kiosk_satellite\tKiosk Satellite\t0") && sent_has("app org.example.app01\tApp 1\t1") && sent_has("apps."));
    run(300); ppm("drawer", inc);
    assert(!drawn_text("App 1"));                                 /* (the apps listed: none of them in it) */
    tap(278, 60); run(100); assert(sent_has("dnd 1"));            /* Home, Music, Do not disturb, Screen off */
    /* a drag goes on whatever the hits become meanwhile: music starts under the volume slider's drag */
    msg("mx"); run(4000); nsent = 0;                              /* (the toasts gone) */
    { int nx = frame(); assert(nx > 0 && nx <= 60005); }         /* its clock, over the music too: once a minute */
    scene_touch(S, TOUCH_DOWN, 500, 168, now); now += 30; scene_touch(S, TOUCH_MOVE, 550, 168, now); run(30);
    msg("mt Song\tSomeone\tAlbum"); msg("mc 50 0 play,pause - off 0"); msg("mp 1 0 100000 1000"); run(50);
    now += 30; scene_touch(S, TOUCH_MOVE, 650, 168, now); now += 30; scene_touch(S, TOUCH_UP, 650, 168, now); run(50);
    assert(!S->dr.vol_drag && sent_has("vol 69"));                /* (where it was let go: the drag went on) */
    nsent = 0; drag(500, 168, 700, 168, 6, 30); assert(sent_has("vol 75"));
    nsent = 0; drag(500, 168, 500, 20, 8, 16); run(600);          /* a swipe up from the volume: the drawer goes, the volume stays */
    assert(!drawer_open(S) && !sent_prefix("vol "));
    pull_drawer(); assert(drawer_open(S));
    msg("p+ active-start"); run(600); assert(!drawer_open(S));   /* a request: the drawer over our screen goes, the face */
    msg("p- active-start"); run(300);
    /* the same while a finger pulls it: it goes, and that finger moving on pulls it no more */
    scene_touch(S, TOUCH_DOWN, W / 2, 3, now);
    for (int i = 1; i <= 5; i++) { now += 16; scene_touch(S, TOUCH_MOVE, W / 2, 3 + i * 30, now); run(16); }
    assert(S->dr.pulling); msg("p+ active-start"); run(16);
    for (int i = 6; i <= 9; i++) { now += 16; scene_touch(S, TOUCH_MOVE, W / 2, 3 + i * 30, now); run(16); }
    now += 16; scene_touch(S, TOUCH_UP, W / 2, 280, now); run(600);
    assert(!drawer_open(S));
    msg("p- active-start"); run(300);
    pull_drawer(); assert(drawer_open(S));
    /* the page picks a home app while the drawer is out: it comes once the drawer is in */
    int nl = nlaunched;
    msg("home org.example.app02"); run(50); assert(nlaunched == nl);
    tap(W / 2, 420); run(600);                                    /* the shade: closed */
    assert(nlaunched == nl + 1 && strstr(launched[nl], "org.example.app02/"));
    scene_top(S, "org.example.app02", now); run(100);
    /* the page picks a home app: it comes at once; the drawer's Home goes to it once the drawer is gone */
    nl = nlaunched;
    msg("home org.example.app01"); run(50);
    assert(nlaunched == nl + 1 && strstr(launched[nl], "org.example.app01/"));
    scene_top(S, "org.example.app01", now); run(100); assert(!scene_visible(S));
    /* over the app: a pull from the app's own top is the app's; from the grip above it, the drawer's */
    nl = nlaunched;
    drag(W / 2, 4, W / 2, 300, 12, 16); run(300);
    assert(shots_asked == 0 && nlaunched == nl && !scene_visible(S));
    drag(W / 2, -8, W / 2, 300, 12, 16); run(600);
    assert(shots_asked == 1 && !strncmp(launched[nl], "HOME", 4) && scene_visible(S));
    uint32_t *pic = malloc((size_t)W * H * 4); for (int i = 0; i < W * H; i++) pic[i] = 0xFF808080u;
    scene_backdrop(S, pic, W, H);
    scene_top(S, UI_HOME_PKG, now); run(300); ppm("drawer_app", inc);
    tap(140, 60); run(100);                                       /* Face: the drawer's second button with a home app */
    assert(scene_visible(S) && nlaunched == nl + 1);
    run(600);
    assert(!S->backdrop.px);                                      /* the app's picture gone with it: never under a later launch */
    /* again over the app; closed with nothing chosen: back to it, its picture held until it is in front (no face) */
    scene_top(S, "org.example.app01", now); run(3500);            /* (the app came back by itself, past the face's grace) */
    drag(W / 2, -8, W / 2, 300, 12, 16); run(600);
    pic = malloc((size_t)W * H * 4); for (int i = 0; i < W * H; i++) pic[i] = 0xFF808080u;
    scene_backdrop(S, pic, W, H);
    scene_top(S, UI_HOME_PKG, now); run(300);
    nl = nlaunched;
    tap(W / 2, 420); run(600);
    assert(nlaunched == nl + 1 && strstr(launched[nl], "org.example.app01/"));
    assert(scene_visible(S) && inc[(H / 2) * W + W / 2] == 0xFF808080u);   /* the app's picture, not the face */
    int f1 = frames; run(1000); assert(frames == f1);             /* and nothing drawn over it while the app comes */
    scene_top(S, "org.example.app01", now); run(100);
    assert(!scene_visible(S));
    /* the drawer's Home: the drawer goes first, then the home app */
    drag(W / 2, -8, W / 2, 300, 12, 16); run(600);
    scene_top(S, UI_HOME_PKG, now); run(100);
    nl = nlaunched; tap(66, 60);
    assert(nlaunched == nl);                                      /* not yet: the drawer closes first */
    run(600);
    assert(nlaunched == nl + 1 && strstr(launched[nl], "org.example.app01/"));
    scene_top(S, "org.example.app01", now); run(5000);
    assert(!scene_visible(S));
    /* a short pull let go over the app: the app back, and our home reported only after that launches nothing more */
    nl = nlaunched; drag(W / 2, -8, W / 2, 40, 4, 200); run(600);   /* (slowly: no flick) */
    assert(!drawer_open(S) && nlaunched == nl + 2 && strstr(launched[nl + 1], "org.example.app01/"));
    scene_top(S, UI_HOME_PKG, now); run(100);
    assert(nlaunched == nl + 2);
    scene_top(S, "org.example.app01", now); run(100);
    /* the same before our home came at all: the app launched while still in front, and the grip pulls again at once */
    drag(W / 2, -8, W / 2, 40, 4, 200); run(600);
    { int sh = shots_asked; drag(W / 2, -8, W / 2, 300, 12, 16); run(600); assert(shots_asked == sh + 1 && drawer_open(S)); }
    scene_top(S, UI_HOME_PKG, now); run(300); tap(W / 2, 420); run(600);   /* (the shade: back to the app) */
    scene_top(S, "org.example.app01", now); run(5000);
    /* Back along the band over the app; the app leaves: the home app comes back, three times a minute */
    scene_touch(S, TOUCH_DOWN, 700, H + 3, now); now += 40; scene_touch(S, TOUCH_MOVE, 500, H + 2, now); scene_touch(S, TOUCH_UP, 500, H + 2, now);
    assert(backs == 1);
    nl = nlaunched;
    for (int i = 0; i < 4; i++) { scene_top(S, UI_HOME_PKG, now); now += 1000; scene_top(S, "org.example.app01", now); now += 1000; }
    assert(nlaunched == nl + 3 && strstr(launched[nl], "org.example.app01"));
    /* the 4th time within the minute: the face meanwhile, the app again once that minute is past */
    scene_top(S, UI_HOME_PKG, now); run(100); assert(nlaunched == nl + 3);
    run(60000); assert(nlaunched == nl + 4 && strstr(launched[nl + 3], "org.example.app01"));
    scene_top(S, "org.example.app01", now); run(100);
    /* the page picks the face: back to it at once */
    nl = nlaunched; msg("home "); run(100);
    assert(nlaunched == nl + 1 && !strncmp(launched[nl], "HOME", 4));
    scene_top(S, UI_HOME_PKG, now); run(3500);
    /* the screen off: a touch is no gesture (android.c wakes the screen with it, and gives the scene none of it); on
     * again, a pull: our own screen, nothing relaunched, no frames held */
    scene_screen(S, 0, now); nl = nlaunched; int sh0 = shots_asked;
    drag(W / 2, -8, W / 2, 300, 12, 16); run(300);
    assert(!drawer_open(S));
    scene_screen(S, 1, now); drag(W / 2, -8, W / 2, 300, 12, 16); run(600);
    assert(drawer_open(S) && !S->dr.from_app && shots_asked == sh0);
    tap(W / 2, 420); run(800);
    assert(!drawer_open(S) && nlaunched == nl && !S->leaving_until);
    /* a pull while an app is on its way is ignored; an app that takes the screen closes a drawer left out */
    msg("home org.example.app01"); run(50);
    drag(W / 2, -8, W / 2, 300, 12, 16); run(100);
    assert(!drawer_open(S));
    scene_top(S, "org.example.app01", now); run(4000);
    msg("home "); run(100); scene_top(S, UI_HOME_PKG, now); run(3500);
    pull_drawer(); assert(drawer_open(S));
    scene_top(S, "org.example.app01", now); run(100);
    assert(!drawer_open(S));
    /* pulled over the app, which takes the front back from our home at once (as a kiosk app may): the drawer goes
     * once our home's grace is over, and the grip pulls it again */
    drag(W / 2, -8, W / 2, 300, 12, 16); scene_top(S, UI_HOME_PKG, now); run(200);
    scene_top(S, "org.example.app01", now); run(4000);
    assert(!drawer_open(S) && !scene_drawer(S) && !S->backdrop.px);
    sh0 = shots_asked; drag(W / 2, -8, W / 2, 300, 12, 16); run(600);
    assert(shots_asked == sh0 + 1 && drawer_open(S));

    /* a fresh start with Kiosk Satellite installed and nothing chosen: it is the home app */
    scene_free(S); nlaunched = 0;
    S = scene_new(W, H, BAND, &plat, NULL, now);
    scene_top(S, UI_HOME_PKG, now);
    msg("kiosk me.jxl.kiosk_satellite"); msg("home -");
    assert(nlaunched == 1 && !strncmp(launched[0], "me.jxl.kiosk_satellite/", 23));
    scene_free(S); nlaunched = 0;
    S = scene_new(W, H, BAND, &plat, NULL, now);              /* the face chosen: it stays */
    scene_top(S, UI_HOME_PKG, now);
    msg("kiosk me.jxl.kiosk_satellite"); msg("home ");
    assert(nlaunched == 0);
    /* nothing chosen, Kiosk Satellite installed later (the guided setup's order): it comes then; and at a clock under a
     * minute, an app leaving is still opened again; a request while the drawer's Home closes keeps that Home */
    scene_free(S); nlaunched = 0;
    long long t_keep = now; now = 30000;
    S = scene_new(W, H, BAND, &plat, NULL, now);
    scene_top(S, UI_HOME_PKG, now);
    msg("home -"); run(100); assert(nlaunched == 0);
    msg("kiosk me.jxl.kiosk_satellite"); run(100);
    assert(nlaunched == 1 && !strncmp(launched[0], "me.jxl.kiosk_satellite/", 23));
    scene_top(S, "me.jxl.kiosk_satellite", now); run(4000);
    scene_top(S, UI_HOME_PKG, now); run(100);                 /* it left, 34 s after the clock started */
    assert(nlaunched == 2);
    scene_top(S, "me.jxl.kiosk_satellite", now); run(4000);
    drag(W / 2, -8, W / 2, 300, 12, 16); run(600); scene_top(S, UI_HOME_PKG, now); run(300);
    tap(140, 60); run(4000);                                      /* the drawer's Face: our own screen */
    int nk = nlaunched; pull_drawer(); assert(drawer_open(S));
    tap(66, 60); run(100);                                        /* Home, and a wake word while the drawer closes */
    msg("p+ active-start 0"); run(600);
    assert(!drawer_open(S) && nlaunched == nk + 1 && !strncmp(launched[nk], "me.jxl.kiosk_satellite/", 23));
    /* Home, then a tap on the shade while the drawer closes (a double tap): Home still */
    msg("p- active-start"); scene_top(S, "me.jxl.kiosk_satellite", now); run(4000);
    drag(W / 2, -8, W / 2, 300, 12, 16); run(600); scene_top(S, UI_HOME_PKG, now); run(300);
    tap(140, 60); run(4000);                                      /* (Face: our own screen again) */
    nk = nlaunched; pull_drawer(); tap(66, 60); run(30); tap(W / 2, 420); run(600);
    assert(nlaunched == nk + 1 && !strncmp(launched[nk], "me.jxl.kiosk_satellite/", 23));
    /* the drawer over Kiosk Satellite, and the page picks the face meanwhile: closed, the face, not Kiosk again */
    msg("p- active-start"); scene_top(S, "me.jxl.kiosk_satellite", now); run(4000);
    drag(W / 2, -8, W / 2, 300, 12, 16); run(600); scene_top(S, UI_HOME_PKG, now); run(300);
    nk = nlaunched; msg("home "); run(100);
    tap(W / 2, 420); run(800);
    for (int i = nk; i < nlaunched; i++) assert(strncmp(launched[i], "me.jxl.kiosk_satellite/", 23));
    assert(scene_visible(S) && S->view == V_FACE && !S->backdrop.px);
    /* a home app that does not start (uninstalled): nothing held for it, and not tried again as our home returns */
    msg("home org.example.gone"); run(50);
    scene_launch_failed(S, "org.example.gone", now); run(50);
    assert(!S->leaving_until && !scene_waiting(S));
    nk = nlaunched; scene_top(S, "me.jxl.kiosk_satellite", now); run(100); scene_top(S, UI_HOME_PKG, now); run(100);
    assert(nlaunched == nk);
    scene_top(S, "org.example.gone", now); run(100);              /* it starts after all (an update had it away) */
    assert(!S->home_broken);
    nk = nlaunched; scene_top(S, UI_HOME_PKG, now); run(100); assert(nlaunched == nk + 1);   /* so it comes back again */
    scene_launch_failed(S, "org.example.gone", now); run(50); assert(S->home_broken);
    nk = nlaunched; msg("home org.example.gone"); run(50);       /* the page picks it again: tried again */
    assert(!S->home_broken && nlaunched == nk + 1);
    scene_top(S, "org.example.gone", now); run(4000);
    /* one picked while an app is in front, our home there first: our screen drawn anew, not a frame never shown held */
    msg("home org.example.app02"); scene_top(S, "me.jxl.kiosk_satellite", now); run(100);
    msg("home org.example.app03"); run(100);                      /* (launched while hidden: nothing drawn for it) */
    assert(S->leaving_until > now && !S->leaving_drawn);
    scene_top(S, UI_HOME_PKG, now); run(200); assert(S->leaving_drawn);   /* our home first: a frame of its own, then held */
    /* pulled over another app, the drawer's Home: its picture goes at once (ours while the home app comes) */
    msg("home me.jxl.kiosk_satellite"); scene_top(S, "me.jxl.kiosk_satellite", now); run(100);
    scene_top(S, "org.example.other", now); run(4000);
    drag(W / 2, -8, W / 2, 300, 12, 16); run(600); scene_top(S, UI_HOME_PKG, now); run(300);
    { uint32_t *pic = malloc((size_t)W * H * 4); for (int i = 0; i < W * H; i++) pic[i] = 0xFF808080u; scene_backdrop(S, pic, W, H); }
    run(100); tap(66, 60); run(50);
    assert(!S->backdrop.px);
    run(600);
    /* an app takes the front while the drawer is pulled over our own screen: the finger moving on pulls nothing */
    scene_top(S, UI_HOME_PKG, now); run(4500);
    { int sh = shots_asked; scene_touch(S, TOUCH_DOWN, W / 2, 3, now);
      for (int i = 1; i <= 6; i++) { now += 16; scene_touch(S, TOUCH_MOVE, W / 2, 3 + i * 30, now); run(16); }
      assert(S->dr.pulling);
      scene_top(S, "org.example.other", now); run(16);
      for (int i = 7; i <= 10; i++) { now += 16; scene_touch(S, TOUCH_MOVE, W / 2, 3 + i * 30, now); run(16); }
      now += 16; scene_touch(S, TOUCH_UP, W / 2, 300, now); run(600);
      assert(!drawer_open(S) && shots_asked == sh); }
    now = t_keep > now ? t_keep : now;

    /* on scenes of their own: */
    #define FRESH() do { scene_free(S); nsent = nlaunched = 0; queue_total = 8; fail_lists = no_totals = 0; queue_gone = -1; hold_answers = 0; \
                         S = scene_new(W, H, BAND, &plat, NULL, now); scene_top(S, UI_HOME_PKG, now); \
                         msg("name Echo"); msg("home -"); run(300); } while (0)
    /* pulled over an app that keeps the front (our home never comes), the finger held on: one pull, not one every 3 s */
    FRESH(); msg("home org.example.app"); run(100); scene_top(S, "org.example.app", now); run(5000);
    { int sh = shots_asked; scene_touch(S, TOUCH_DOWN, W / 2, -8, now);
      for (int i = 1; i <= 400; i++) { now += 16; scene_touch(S, TOUCH_MOVE, W / 2, -8 + (i < 12 ? i * 25 : 280 + i % 2), now); frame(); }
      now += 16; scene_touch(S, TOUCH_UP, W / 2, 280, now); run(600);
      assert(shots_asked == sh + 1 && !drawer_open(S) && !scene_drawer(S)); }
    { int sh = shots_asked; drag(W / 2, -8, W / 2, 300, 12, 16); run(300); assert(shots_asked == sh + 1); }   /* (the grip pulls again) */
    /* a wake word while the drawer is pulled over the home app: it stays (it is that app's), the shade goes back to it */
    FRESH(); msg("kiosk me.jxl.kiosk_satellite"); run(100); scene_top(S, "me.jxl.kiosk_satellite", now); run(4000);
    drag(W / 2, -8, W / 2, 300, 12, 16); run(300); scene_top(S, UI_HOME_PKG, now); run(300);
    msg("p+ active-start 0"); run(300); assert(drawer_open(S) && S->dr.from_app);
    { int nl = nlaunched; msg("p- active-start"); tap(W / 2, 420); run(600);
      assert(nlaunched == nl + 1 && !strncmp(launched[nl], "me.jxl.kiosk_satellite/", 23)); }
    /* the drawer's Music over the home app: asked for, so another app coming and going does not bring the app back */
    FRESH(); msg("kiosk me.jxl.kiosk_satellite"); run(100); scene_top(S, "me.jxl.kiosk_satellite", now); run(4000);
    drag(W / 2, -8, W / 2, 300, 12, 16); run(300); scene_top(S, UI_HOME_PKG, now); run(300);
    { int x, y; hit_mid(A_DR_MUSIC, -1, &x, &y); tap(x, y); run(4000); }
    { int nl = nlaunched; scene_top(S, "com.android.settings", now); run(2000); scene_top(S, UI_HOME_PKG, now); run(100);
      assert(nlaunched == nl && S->view == V_MUSIC); }
    /* a swipe along the band over our own screen: no Back (that is over an app only) */
    { int b0 = backs; scene_touch(S, TOUCH_DOWN, 700, H + 3, now); now += 40; scene_touch(S, TOUCH_MOVE, 500, H + 2, now);
      scene_touch(S, TOUCH_UP, 500, H + 2, now); run(100); assert(backs == b0); }
    /* a list Music Assistant tells no total of: it ends at the first page that brings nothing, and asks no more */
    FRESH(); no_totals = 1; msg("ma up Music Assistant"); music_show(S, 2); run(300);
    for (int i = 0; i < 12; i++) { drag(500, 420, 500, 100, 8, 16); run(200); }
    { int asks = 0; for (int i = 0; i < nsent; i++) asks += strstr(sent[i], " albums ") != NULL;
      assert(S->m.stack[0]->n == 150 && S->m.stack[0]->done && asks == 4);
      for (int i = 0; i < 4; i++) { drag(500, 420, 500, 100, 8, 16); run(200); }
      int again = 0; for (int i = 0; i < nsent; i++) again += strstr(sent[i], " albums ") != NULL;
      assert(again == asks); }
    /* the queue: shorter when asked again (a track removed): the rows past its new end go; the current mark moves with
     * pq; asked again when Music Assistant is back */
    FRESH(); msg("ma up Music Assistant"); msg("pq playing\t0\toff\t2\t8\tQueue"); music_show(S, 0); run(300);
    tap(46, tab_y(1)); run(300); assert(S->m.queue.n == 8 && strchr(S->m.queue.it[2].flags, 'c'));
    queue_total = 5; msg("qc"); run(300); assert(S->m.queue.n == 5);
    msg("pq playing\t0\toff\t4\t5\tQueue"); run(100); assert(strchr(S->m.queue.it[4].flags, 'c') && !strchr(S->m.queue.it[2].flags, 'c'));
    msg("ma connecting"); run(100); nsent = 0; msg("ma up Music Assistant"); run(300);
    assert(sent_prefix("ml ") && strstr(sent[0], " queue 0") && S->m.queue.n == 5);
    /* the home app kept leaving (4 times a minute), its next try due while the drawer is out: tried once it is in */
    FRESH(); msg("kiosk me.jxl.kiosk_satellite"); run(100);
    for (int i = 0; i < 4; i++) { scene_top(S, "me.jxl.kiosk_satellite", now); now += 1000; scene_top(S, UI_HOME_PKG, now); now += 1000; }
    { int nl = nlaunched; assert(S->home_retry_at); now = S->home_retry_at - 2000; run(10);
      drag(W / 2, 3, W / 2, 300, 12, 16); run(3000); assert(drawer_open(S) && nlaunched == nl);
      tap(W / 2, 420); run(2500); assert(nlaunched == nl + 1 && !strncmp(launched[nl], "me.jxl.kiosk_satellite/", 23)); }
    /* the drawer's Home after its Face once (the face asked for), the app not starting: tried again all the same */
    FRESH(); msg("kiosk me.jxl.kiosk_satellite"); run(100); scene_top(S, "me.jxl.kiosk_satellite", now); run(4000);
    drag(W / 2, -8, W / 2, 300, 12, 16); run(300); scene_top(S, UI_HOME_PKG, now); run(300); tap(140, 60); run(4000);   /* Face */
    pull_drawer(); { int nl = nlaunched; tap(66, 60); run(600); assert(nlaunched == nl + 1);           /* Home */
      scene_launch_failed(S, "me.jxl.kiosk_satellite", now); run(31000); assert(nlaunched == nl + 2); }
    /* the page picks a home app while the drawer is out, then the drawer's Face: the face, not that app over it */
    FRESH(); pull_drawer(); msg("home org.example.k"); run(100); tap(140, 60); run(1500);
    assert(nlaunched == 0 && S->view == V_FACE && !drawer_open(S));
    /* Home tapped, the drawer grabbed again as it closes and put away with nothing chosen: no Home */
    FRESH(); msg("home org.example.k"); run(100); scene_top(S, "org.example.k", now); run(4000);
    scene_top(S, "org.example.other", now); run(4000);
    drag(W / 2, -8, W / 2, 300, 12, 16); run(300); scene_top(S, UI_HOME_PKG, now); run(300);
    { int nl = nlaunched; tap(66, 60); run(60); assert(S->dr.go);
      drag(W / 2, 3, W / 2, 300, 12, 16); run(300); assert(drawer_open(S) && !S->dr.go);
      drag(W / 2, 300, W / 2, 20, 8, 16); run(800); assert(!drawer_open(S));
      for (int i = nl; i < nlaunched; i++) assert(strncmp(launched[i], "org.example.k", 13)); }   /* (back to the app it was over) */
    /* a tap on the action sheet itself (its title): it stays */
    FRESH(); msg("ma up Music Assistant"); music_show(S, 2); run(300); hold(500, 100); assert(S->m.sheet == 1);
    { struct scene *s = S; float w = fminf(U(440), W - U(40)), h = U(56) + 4 * U(54) + U(12), x0 = (W - w) / 2, y0 = fmaxf(U(16), (H - h) / 2);
      tap((int)(x0 + U(40)), (int)(y0 + U(28))); run(100); assert(S->m.sheet == 1); }
    /* the drawer grabbed again as it closes: it comes with the finger */
    FRESH(); pull_drawer(); tap(W / 2, 420); run(60); assert(drawer_open(S) && !S->dr.t);
    drag(W / 2, 3, W / 2, 300, 12, 16); run(300); assert(drawer_open(S) && S->dr.t);
    /* our own home activity that does not start as the drawer is pulled over an app: the app keeps the screen, the drawer
     * goes (not shown over an app that has the touches) */
    FRESH(); scene_top(S, "org.example.app", now); run(4000);
    drag(W / 2, -8, W / 2, 300, 12, 16); assert(drawer_open(S) && scene_visible(S));
    scene_launch_failed(S, NULL, now); run(100); assert(!drawer_open(S) && !scene_visible(S));
    /* the home app that does not start (the system not up yet): tried again while our own screen shows, 30 s on,
     * doubling; no more once it is in front */
    FRESH(); msg("kiosk me.jxl.kiosk_satellite"); run(100); assert(nlaunched == 1);
    scene_launch_failed(S, "me.jxl.kiosk_satellite", now); run(1000);
    scene_top(S, "com.android.settings", now); run(1000); scene_top(S, UI_HOME_PKG, now); run(27000);   /* (another app came, went) */
    assert(nlaunched == 1);
    run(2000); assert(nlaunched == 2 && !strncmp(launched[1], "me.jxl.kiosk_satellite/", 23));
    scene_launch_failed(S, "me.jxl.kiosk_satellite", now); run(50000); assert(nlaunched == 2);
    run(12000); assert(nlaunched == 3);
    scene_top(S, "me.jxl.kiosk_satellite", now); run(300000); assert(nlaunched == 3 && !S->home_broken);
    /* a tap at the top edge of our own screen whose finger slips a little down: still a tap (the rail's face button),
     * not a pull of the drawer let go at once */
    FRESH(); msg("ma up Music Assistant"); music_show(S, 0); run(4000);
    scene_touch(S, TOUCH_DOWN, 46, 16, now); now += 30; frame(); scene_touch(S, TOUCH_MOVE, 46, 28, now); now += 30; frame();
    scene_touch(S, TOUCH_UP, 46, 28, now); run(800); assert(S->view == V_FACE && !drawer_open(S));
    /* an empty queue whose total is not told: empty, not "Loading…" for ever */
    FRESH(); no_totals = 1; queue_total = 0; msg("ma up Music Assistant"); music_show(S, 0); run(300); tap(46, tab_y(1)); run(300);
    assert(S->m.queue.done && !S->m.queue.n && !drawn_text("Loading"));
    FRESH(); msg("ma up Music Assistant"); msg("pq playing\t0\toff\t2\t8\tQueue"); music_show(S, 0); run(300); tap(46, tab_y(1)); run(300);
    /* a finger on a queue row as a track above it goes (asked again): let go, that row is another track now; with a
     * frame between, and without */
    for (int k = 0; k < 2; k++) {
        queue_total = 8; queue_gone = -1; msg("qc"); run(300); assert(!strcmp(S->m.queue.it[3].uri, "qi://queue/3"));
        nsent = 0; scene_touch(S, TOUCH_DOWN, 500, 70 + 3 * 64 + 32, now); run(50);
        queue_total = 7; queue_gone = 1; msg("qc");
        if (!k) { int nx = frame(); assert(!S->pressed && nx > 0 && nx <= 50); run(50); }   /* (a frame at once: no highlight
                                                                     left on the row that came under the finger) */
        now += 10; scene_touch(S, TOUCH_UP, 500, 70 + 3 * 64 + 32, now); run(100);
        assert(!strcmp(S->m.queue.it[3].uri, "qi://queue/4") && !sent_prefix("mq index"));
    }
    /* the queue opened where it plays, that row past its short end: at the end it has, no spring back */
    FRESH(); queue_total = 3; msg("ma up Music Assistant"); msg("pq playing\t0\toff\t5\t3\tQueue"); run(100);
    music_show(S, 0); run(200); tap(46, tab_y(1)); run(30);
    assert(S->m.queue.n == 3 && S->m.queue.sc.pos == 0);
    /* a category that failed: its chip, or the Library tab, asks again */
    FRESH(); msg("ma up Music Assistant"); fail_lists = 1; music_show(S, 2); run(300);
    assert(S->m.depth == 1 && S->m.stack[0]->failed);
    { int x, y; fail_lists = 0; nsent = 0; hit_mid(A_M_CAT, S->m.cat, &x, &y); tap(x, y); run(200);
      assert(sent_prefix("ml ") && S->m.stack[0]->n);
      fail_lists = 1; msg("ma connecting"); msg("ma up Music Assistant"); run(200); assert(S->m.depth == 1 && S->m.stack[0]->failed);
      fail_lists = 0; nsent = 0; tap(46, tab_y(2)); run(200); assert(sent_prefix("ml ") && !S->m.stack[0]->failed); }
    /* a finger held still on a row: the sheet; the same as an app comes up (no move to let go by): none under the app */
    FRESH(); msg("ma up Music Assistant"); music_show(S, 2); run(3500);   /* (past the grace for our own screen to come) */
    scene_touch(S, TOUCH_DOWN, 500, 100, now); run(1000); assert(S->m.sheet == 1);
    now += 16; scene_touch(S, TOUCH_UP, 500, 100, now); run(100); tap(W / 2, 30); run(100); assert(!S->m.sheet);
    scene_touch(S, TOUCH_DOWN, 500, 100, now); run(100);
    scene_top(S, "org.example.app", now); run(1000); assert(!S->m.sheet);
    now += 16; scene_touch(S, TOUCH_UP, 500, 100, now); run(100);
    /* the drawer pulled over our own screen while it is on its way (the face picked on the page over an app: the
     * grace): an app takes the front, the drawer goes (the grace is a pull's over an app, not this) */
    FRESH(); msg("home org.example.app"); run(300); scene_top(S, "org.example.app", now); run(4000);
    msg("home "); assert(S->want_home_until > now && scene_visible(S));
    drag(W / 2, 3, W / 2, 300, 12, 16); run(300); assert(drawer_open(S) && !S->dr.from_app && S->want_home_until > now);
    scene_top(S, "org.example.other", now); run(100); assert(!drawer_open(S));
    /* pulled over an app, Home tapped as the grace ends with our home not seen yet: that Home all the same */
    FRESH(); msg("kiosk me.jxl.kiosk_satellite"); run(100); scene_top(S, "me.jxl.kiosk_satellite", now); run(4000);
    scene_top(S, "org.example.other", now); run(4000);
    drag(W / 2, -8, W / 2, 300, 12, 16); run(2600);
    { int nl = nlaunched; tap(66, 60); run(1000);
      assert(nlaunched == nl + 1 && !strncmp(launched[nl], "me.jxl.kiosk_satellite/", 23) && !drawer_open(S)); }
    /* Home chosen in the drawer over an app, that app back in front as the drawer closes: Home all the same */
    FRESH(); msg("kiosk me.jxl.kiosk_satellite"); run(100); scene_top(S, "me.jxl.kiosk_satellite", now); run(4000);
    scene_top(S, "org.example.other", now); run(4000);
    drag(W / 2, -8, W / 2, 300, 12, 16); run(300); scene_top(S, UI_HOME_PKG, now); run(300);
    { int nl = nlaunched; tap(66, 60); run(50); assert(drawer_open(S) && S->dr.go);
      scene_top(S, "org.example.other", now); run(500);
      assert(nlaunched == nl + 1 && !strncmp(launched[nl], "me.jxl.kiosk_satellite/", 23)); }
    /* the face asked for from the drawer over the home app: another app that comes and goes does not bring it back */
    FRESH(); msg("kiosk me.jxl.kiosk_satellite"); run(100); scene_top(S, "me.jxl.kiosk_satellite", now); run(4000);
    drag(W / 2, -8, W / 2, 300, 12, 16); run(300); scene_top(S, UI_HOME_PKG, now); run(300);
    tap(140, 60); run(4000);                                      /* Face */
    { int nl = nlaunched; scene_top(S, "com.android.settings", now); run(2000); scene_top(S, UI_HOME_PKG, now); run(100);
      assert(nlaunched == nl && S->view == V_FACE); }
    /* Kiosk Satellite that did not start, then gone: not marked so for the next home app */
    scene_launch_failed(S, "me.jxl.kiosk_satellite", now); assert(S->home_broken);
    msg("kiosk "); run(100); assert(!S->home_broken);
    /* a later page that fails: why, at the list's end; scrolled there again 10 s on, asked again */
    FRESH(); msg("ma up Music Assistant"); hold_answers = 1; music_show(S, 2); run(300);
    { hold_answers = 0; answer(held); run(100);                  /* (the first page) */
      hold_answers = 1;
      for (int i = 0; i < 10; i++) drag(500, 420, 500, 100, 8, 16);
      run(1500); int r2 = 0; assert(sscanf(held, "ml %d albums 60", &r2) == 1 && r2);
      char b[80]; snprintf(b, sizeof b, "E %d no answer", r2); msg(b); hold_answers = 0;
      for (int i = 0; i < 4; i++) drag(500, 420, 500, 100, 8, 16);
      run(1500); assert(S->m.stack[0]->n == 60 && drawn_text("no answer"));
      nsent = 0; run(10000); for (int i = 0; i < 4; i++) drag(500, 420, 500, 100, 8, 16);
      run(500); assert(sent_prefix("ml ") && S->m.stack[0]->n > 60); }
    /* a fling as an app comes up: stopped there, not on its way again when our screen comes back */
    run(3500); scene_top(S, UI_HOME_PKG, now);
    { struct mlist *l = S->m.stack[0]; scroll_to(&l->sc, 600); run(500); drag(500, 400, 500, 100, 4, 16);   /* (a fast one) */
      assert(l->sc.vel != 0); scene_top(S, "org.example.app", now); run(100); float p = l->sc.pos;
      assert(l->sc.vel == 0); scene_top(S, UI_HOME_PKG, now); run(1000); assert(fabsf(l->sc.pos - p) < 1); }
    /* the queue opening where it plays, moved by the finger while a page is on its way: it stays where the finger left it */
    FRESH(); queue_total = 150; msg("ma up Music Assistant"); msg("pq playing\t0\toff\t100\t150\tQueue"); music_show(S, 0); run(300);
    hold_answers = 1; tap(46, tab_y(1)); run(50); answer(held); run(50);   /* (the first page; the next one held) */
    drag(500, 400, 500, 200, 8, 16); run(3000);                   /* (its fling over) */
    { float p = S->m.queue.sc.pos; assert(p > 50); hold_answers = 0; answer(held); run(500);
      assert(fabsf(S->m.queue.sc.pos - p) < 1); }
    /* our screen asked for over an app, and come: an app that comes next takes the screen at once */
    FRESH(); scene_top(S, "org.example.app", now); run(4000);
    music_show(S, 0); run(100); assert(scene_visible(S));          /* (on its way: shown meanwhile) */
    scene_top(S, UI_HOME_PKG, now); run(100); scene_top(S, "org.example.other", now); run(50); assert(!scene_visible(S));
    /* a search whose typing pause ends while Music Assistant is away: sent once it is back */
    FRESH(); msg("ma up Music Assistant"); music_show(S, 3); run(300);
    { float kw2 = (W - 92) / 10.0f, kh2 = 232 / 4.0f, ky2 = H - 232;
      tap((int)(92 + 3.5f * kw2), (int)(ky2 + kh2 / 2)); tap((int)(92 + 2.5f * kw2), (int)(ky2 + kh2 / 2)); }   /* r e */
    msg("ma connecting"); run(600); nsent = 0;
    msg("ma up Music Assistant"); run(1000);
    assert(sent_prefix("ml ") && strstr(sent[nsent - 1], "search 0 re") && !drawn_text("Searching"));

    /* the words and timers (words.c): a layer of their own, drawn on a change, faded by its alpha */
    {
        static uint32_t wb[W * H];
        struct gfx wg = { wb, W, H, W, { 0, 0, W, H } };
        float a = 0; int nx = 0, k;
        #define WSTEP(ms) do { long long e_ = now + (ms); while (now < e_) { now += 33; k = scene_words(S, now, &a, &nx); if (k == 2) scene_words_draw(S, &wg); } } while (0)
        #define CACHED(str) ({ int f_ = 0; for (int i_ = 0; i_ < NTEXT; i_++) if (S->text[i_].used && strstr(S->text[i_].key, str)) f_ = 1; f_; })
        #define TEXT_IS(str) ({ int f_ = 0; for (int i_ = 0; i_ < NTEXT; i_++) if (S->text[i_].used && eq(strrchr(S->text[i_].key, '|') + 1, str)) f_ = 1; f_; })
        FRESH();
        assert(scene_words(S, now, &a, &nx) == 0 && nx == -1);      /* nothing told: hidden, nothing to wait for */
        msg("wh What is the weather"); msg("wr Sunny"); msg("ws -1");
        assert(scene_words(S, now, &a, &nx) == 0);                 /* "Speech on screen" off: hidden */
        msg("wo 1 0 8"); msg("wh Wie wird das Wetter morgen?"); msg("wr Morgen wird es sonnig bei bis zu 18 Grad."); msg("ws -1");
        k = scene_words(S, now, &a, &nx); assert(k == 2 && a < 0.2f && nx > 0);   /* drawn, fading in */
        scene_words_draw(S, &wg);
        int shade = 0, lit = 0, bar = 0;
        for (int i = 0; i < W * H; i++) { shade += wb[i] == 0xBF000000u; lit += wb[i] >> 24 == 0xFF; bar += wb[i] == gfx_px(CYAN); }
        assert(shade > W * H / 2 && lit > 1000 && bar > 20);          /* the shade, the words over it, the bar by the reply */
        if (shots) ppm("words", wb);
        assert(CACHED("Morgen") && CACHED("Wetter"));
        assert(scene_words(S, now, &a, &nx) == 1);                 /* the same: not drawn again */
        WSTEP(300); assert(k == 1 && a == 1 && nx == -1);           /* faded in; still while the Echo answers */
        msg("ws 0"); WSTEP(7000); assert(k == 1 && a == 1 && nx > 0 && nx <= 1000);   /* idle: 8 s */
        WSTEP(1000); WSTEP(700); assert(k == 0);                   /* then faded out */
        /* a new request takes them away at once */
        msg("wh Und am Montag?"); msg("wr "); msg("ws -1"); WSTEP(300); assert(k == 1 && a == 1);
        msg("wh "); msg("wr "); msg("ws -1"); assert(scene_words(S, now, &a, &nx) == 0);
        /* the drawer has the screen: hidden while it is out, back after it */
        msg("wh Licht an"); msg("ws -1"); WSTEP(300); assert(k == 1);
        pull_drawer(); assert(drawer_open(S) && scene_words(S, now, &a, &nx) == 0);
        drawer_close_now(S); run(50); WSTEP(300); assert(k == 1 && a == 1);
        scene_screen(S, 0, now); assert(scene_words(S, now, &a, &nx) == 0); scene_screen(S, 1, now);
        /* 0: until the next request */
        msg("wo 1 0 0"); msg("ws 0"); WSTEP(30000); assert(k == 1 && a == 1 && nx == -1);
        /* a reply longer than the screen, broken UTF-8 in it: cut, inside the buffer (ASan) */
        { char longer[1001] = ""; while (strlen(longer) < 900) strcat(longer, "Heute stehen drei Termine im Kalender \xc3\xa4 ");
          strcat(longer, "\xff\xfe kaputt \xe2\x82"); msg("ws -1"); char l[1100]; snprintf(l, sizeof l, "wr %s", longer); msg(l);
          WSTEP(100); scene_words_draw(S, &wg); }
        { char word[600]; memset(word, 'W', sizeof word - 1); word[sizeof word - 1] = 0; char l[700]; snprintf(l, sizeof l, "wr %s", word); msg(l);
          WSTEP(100); scene_words_draw(S, &wg); }                  /* one word longer than a line: broken */
        msg("wo 0 0 8"); WSTEP(700); assert(k == 0);               /* off: gone */
        /* timers: counted down here from what hassmic told, the one that ends first on top, a paused one last */
        msg("wh "); msg("wr "); msg("ws -1");
        msg("wo 0 1 8"); msg("tl 2\t30000 600 0 Tea\t65000 300 1 Pizza");
        assert(S->wd.nt == 2 && !strcmp(S->wd.t[1].name, "Pizza") && S->wd.t[1].active && S->wd.t[1].total == 300);
        k = scene_words(S, now, &a, &nx); assert(k == 2 && nx > 0); scene_words_draw(S, &wg);
        if (shots) ppm("timers", wb);
        assert(TEXT_IS("1:05") && TEXT_IS("30") && TEXT_IS("Pizza") && TEXT_IS("min") && TEXT_IS("s"));   /* (30 s: the seconds alone) */
        WSTEP(300); assert(k == 1 && nx > 0 && nx <= 1000);         /* faded in: the next second */
        now += nx; k = scene_words(S, now, &a, &nx); assert(k == 2); scene_words_draw(S, &wg); assert(TEXT_IS("1:04"));
        now += 100; assert(scene_words(S, now, &a, &nx) == 1);      /* within the second: as drawn */
        msg("tl 1\t30000 600 0 Tea"); WSTEP(300); assert(k == 1 && nx == -1);   /* paused alone: nothing moves */
        /* a ringing one: orange, pulsing by the layer's alpha alone */
        msg("timers Tea, 10 min"); k = scene_words(S, now, &a, &nx); assert(k == 2 && nx == 66); scene_words_draw(S, &wg);
        { int orange = 0; for (int i = 0; i < W * H; i++) orange += wb[i] == gfx_px(ORANGE); assert(orange > 200); }
        if (shots) ppm("ringing", wb);
        { float lo = 1, hi = 0; int drawn = 0; WSTEP(400);
          for (int i = 0; i < 40; i++) { now += 66; k = scene_words(S, now, &a, &nx); drawn += k == 2; lo = fminf(lo, a); hi = fmaxf(hi, a); }
          assert(!drawn && lo < 0.5f && hi > 0.95f); }
        msg("wo 1 1 8"); msg("wh Stopp"); msg("ws -1"); assert(scene_words(S, now, &a, &nx) == 2 && S->wd.pic == 1);   /* words first */
        msg("wh "); msg("ws -1"); msg("timers "); assert(scene_words(S, now, &a, &nx) == 2 && S->wd.pic == 3);         /* the timers again */
        /* what hassmic tells is checked: records that do not parse are left out, the rest kept */
        msg("tl 3\tgarbage\t1000 60 1\t5000 x 1 Bad"); assert(S->wd.nt == 1 && S->wd.t[0].total == 60 && !S->wd.t[0].name[0]);
        msg("tl 0"); WSTEP(700); assert(S->wd.nt == 0 && k == 0);
        /* touches over our own screen: the layer's, nothing under it gets them; a tap takes the words away */
        FRESH(); msg("wo 1 1 8"); msg("wh Licht an"); msg("wr Erledigt"); msg("ws -1"); WSTEP(300); assert(k == 1);
        { long long poke = S->face.poke_at; nsent = 0; tap(W / 2, H / 2);      /* on the eyes, under the words */
          assert(S->face.poke_at == poke && sent_has("words clear") && !S->wd.heard[0]);
          WSTEP(700); assert(k == 0);
          tap(W / 2, H / 2); assert(S->face.poke_at != poke); }      /* gone: the eyes have it again */
        /* the running timers: tapped away until their list changes */
        msg("tl 1\t65000 300 1 Pizza"); WSTEP(300); assert(k == 1 && S->wd.pic == 3);
        nsent = 0; tap(W / 2, H / 2); WSTEP(700); assert(k == 0 && S->wd.timers_hidden && sent_prefix("timers hide "));
        msg("tl 2\t65000 300 1 Pizza\t30000 60 1 Tea"); WSTEP(300); assert(k == 1 && !S->wd.timers_hidden);
        /* a ringing one: Stop on its pill, nothing elsewhere; the drawer still comes from the top */
        msg("timers Pizza"); k = scene_words(S, now, &a, &nx); assert(k == 2); scene_words_draw(S, &wg);
        if (shots) ppm("ringing_stop", wb);
        WSTEP(300);                                                /* (faded in: before that a tap is the face's) */
        { struct rect r = S->wd.stop; assert(!rect_empty(r) && r.y1 <= H); nsent = 0;
          tap(W / 2, r.y0 - 80); assert(!sent_has("timer stop"));
          tap((r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2); assert(sent_has("timer stop")); }
        pull_drawer(); assert(drawer_open(S) && scene_words(S, now, &a, &nx) == 0);
        drawer_close_now(S); run(50);
        msg("timers "); msg("tl 0"); WSTEP(700); assert(k == 0);
        /* over an app: its touches stay its own; a ring brings our screen up for Stop, and the app back after */
        msg("home org.example.app"); run(100); scene_top(S, "org.example.app", now); run(100);
        msg("wh Licht aus"); msg("ws -1"); WSTEP(300); nsent = 0;
        tap(W / 2, H / 2); assert(!sent_has("words clear") && S->wd.heard[0]);
        msg("wh "); msg("ws -1"); WSTEP(700);
        nlaunched = 0; msg("timers Pizza"); WSTEP(100);
        assert(nlaunched == 1 && !strcmp(launched[0], "HOME/") && eq(S->wd.ring_from, "org.example.app"));
        scene_top(S, UI_HOME_PKG, now); run(100); WSTEP(300);
        assert(scene_visible(S) && k >= 1 && nlaunched == 1);        /* the home app not brought back over the ring */
        msg("timers "); WSTEP(100);
        assert(nlaunched == 2 && !strcmp(launched[1], "org.example.app/") && !S->wd.ring_from[0]);
        scene_top(S, "org.example.app", now); run(100);
        /* over another app than the home app: that one back after, and the home app again once it goes */
        scene_top(S, "org.example.other", now); run(100); nlaunched = 0;
        msg("timers Pizza"); WSTEP(100); assert(nlaunched == 1 && eq(S->wd.ring_from, "org.example.other"));
        scene_top(S, UI_HOME_PKG, now); run(100); msg("timers "); WSTEP(100);
        assert(nlaunched == 2 && !strcmp(launched[1], "org.example.other/"));
        scene_top(S, "org.example.other", now); run(100); now += 61000; scene_top(S, UI_HOME_PKG, now); run(100);   /* it left */
        assert(nlaunched == 3 && !strcmp(launched[2], "org.example.app/"));
        scene_top(S, "org.example.app", now); run(100);
        /* our screen does not come (the app keeps the front): asked for again every 10 s while it rings */
        nlaunched = 0; msg("timers Pizza"); WSTEP(100); assert(nlaunched == 1);
        WSTEP(5000); assert(nlaunched == 1 && !scene_visible(S));
        WSTEP(5000); assert(nlaunched == 2 && eq(S->wd.ring_from, "org.example.app"));
        scene_top(S, UI_HOME_PKG, now); run(100); msg("timers "); WSTEP(100); assert(!S->wd.ring_from[0]);
        scene_top(S, "org.example.app", now); run(100);
        /* words just coming (alpha under half): a tap is still the face's */
        msg("home -"); run(100); scene_top(S, UI_HOME_PKG, now); run(100); WSTEP(700); assert(k == 0);
        { long long poke = S->face.poke_at; msg("wh Neu"); msg("ws -1"); k = scene_words(S, now, &a, &nx);
          assert(k >= 1 && a < 0.5f); nsent = 0; tap(W / 2, H / 2);
          assert(S->face.poke_at != poke && !sent_has("words clear")); }
        msg("wh "); msg("ws -1"); WSTEP(700);
        /* the panel grabbable (the Echo): over an app the words want it, and while it is theirs a tap is theirs; a ring
         * there needs no bring-up, its Stop is tapped where it is */
        scene_words_grab(S, 0);
        scene_top(S, "org.example.app", now); run(100);
        assert(!scene_words_wants_grab(S));                          /* nothing shown: Android's */
        msg("wh Licht aus"); msg("ws -1"); WSTEP(300);
        assert(scene_words_wants_grab(S)); nsent = 0;
        tap(W / 2, H / 2); assert(!sent_has("words clear"));          /* (not grabbed yet: the app's) */
        scene_words_grab(S, 1);
        tap(W / 2, H / 2); assert(sent_has("words clear") && !S->wd.heard[0]);
        WSTEP(700); assert(k == 0 && !scene_words_wants_grab(S)); scene_words_grab(S, 0);   /* gone: given back */
        nlaunched = 0; msg("timers Pizza"); WSTEP(300);
        assert(nlaunched == 0 && !S->wd.ring_from[0] && scene_words_wants_grab(S));
        scene_words_grab(S, 1);
        { struct rect r = S->wd.stop; assert(!rect_empty(r)); nsent = 0;
          tap((r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2); assert(sent_has("timer stop")); }
        msg("timers "); WSTEP(700); assert(!scene_words_wants_grab(S)); scene_words_grab(S, 0);
        scene_words_grab(S, -1); scene_top(S, UI_HOME_PKG, now); run(100);
        /* two long texts that share their start past the cache key's size (a line of Cyrillic): masks of their own */
        { char a1[400], a2[400]; memset(a1, 'A', 300); a1[300] = 0; strcpy(a2, a1); strcat(a1, "one"); strcat(a2, "two");
          const struct ui_text *m1 = text_mask(S, a1, 20, 0, 4096), *m2 = text_mask(S, a2, 20, 0, 4096);
          assert(m1 && m2 && m1 != m2 && text_mask(S, a1, 20, 0, 4096) == m1); }
        #undef WSTEP
        #undef CACHED
        #undef TEXT_IS
    }

    printf("frames %d, dirty %.0f px a frame on average; drawing dirty-only %.3f ms a frame (worst %.3f), whole %.3f ms\n",
           frames, (double)drawn_px / frames, t_inc / frames * 1000, worst * 1000, t_full / frames * 1000);
    scene_free(S);
    printf("ui_test: ok\n");
    return 0;
}
