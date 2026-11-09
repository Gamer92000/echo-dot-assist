/* The Echo Show's screen UI (src/ui/) on the PC, as pictures: every view in the states worth looking at, drawn by the
 * same scene.c and gfx.c as on the Echo, text by FreeType in the firmware's own font (Typeface.DEFAULT is Roboto on
 * 8149: system/etc/fonts.xml), Music Assistant's answers made up here.  For working on the layout, and for the guide.
 *
 *   make build/uishot && build/uishot <dir> [<font dir>] [<width> <height> <band>]
 *       writes <dir>/<view>.png; fonts from firmware/checkers/rootfs/system/fonts, else <font dir> (Roboto-Regular.ttf
 *       and Roboto-Bold.ttf, or any two TTFs named so); 960 470 10 by default (the Echo Show 5)
 */
#include "../src/ui/scene.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static int W = 960, H = 470, BAND = 10;
static uint32_t *fb;
static FT_Library ft; static FT_Face face[2];
static long long now = 1000000;
static struct scene *sc;
static const char *out;

/* ---------------------------------------------------------------- text, as android.c's p_text lays it out */

static unsigned utf8(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p; unsigned c = *s++;
    if (c >= 0xF0 && s[0] && s[1] && s[2]) { c = (c & 7) << 18 | (s[0] & 63) << 12 | (s[1] & 63) << 6 | (s[2] & 63); s += 3; }
    else if (c >= 0xE0 && s[0] && s[1]) { c = (c & 15) << 12 | (s[0] & 63) << 6 | (s[1] & 63); s += 2; }
    else if (c >= 0xC0 && s[0]) { c = (c & 31) << 6 | (s[0] & 63); s += 1; }
    *p = (const char *)s;
    return c;
}

static int measure(FT_Face f, const unsigned *cp, int n)
{
    int w = 0;
    for (int i = 0; i < n; i++) if (!FT_Load_Char(f, cp[i], FT_LOAD_DEFAULT)) w += (int)(f->glyph->advance.x >> 6);
    return w;
}

static int t_text(void *ctx, const char *s, int px, int bold, int maxw, struct ui_text *o)
{
    (void)ctx;
    FT_Face f = face[bold ? 1 : 0];
    FT_Set_Pixel_Sizes(f, 0, (FT_UInt)(px * 0.8f));
    unsigned cp[512]; int n = 0;
    for (const char *p = s; *p && n < 500; ) cp[n++] = utf8(&p);
    int w = measure(f, cp, n);
    if (w > maxw) {                                             /* TextUtils.ellipsize: cut, then the ellipsis */
        while (n > 0 && (w = measure(f, cp, n) + measure(f, (unsigned[]){ 0x2026 }, 1)) > maxw) n--;
        cp[n++] = 0x2026;
        w = measure(f, cp, n);
    }
    if (w <= 1) return -1;
    o->w = w + 2; o->h = px; o->stride = o->w; o->m = calloc((size_t)o->w * o->h, 1);
    float asc = f->size->metrics.ascender / 64.0f, desc = -f->size->metrics.descender / 64.0f;
    int base = (int)lroundf((px - (asc + desc)) / 2 + asc), x = 0;
    for (int i = 0; i < n; i++) {
        if (FT_Load_Char(f, cp[i], FT_LOAD_RENDER)) continue;
        FT_GlyphSlot g = f->glyph;
        for (unsigned yy = 0; yy < g->bitmap.rows; yy++)
            for (unsigned xx = 0; xx < g->bitmap.width; xx++) {
                int X = x + g->bitmap_left + (int)xx, Y = base - g->bitmap_top + (int)yy;
                if (X < 0 || Y < 0 || X >= o->w || Y >= o->h) continue;
                unsigned v = o->m[Y * o->w + X] + g->bitmap.buffer[yy * g->bitmap.pitch + xx];
                o->m[Y * o->w + X] = (uint8_t)(v > 255 ? 255 : v);
            }
        x += (int)(g->advance.x >> 6);
    }
    return 0;
}

/* ---------------------------------------------------------------- made-up pictures */

static unsigned hash(const char *k) { unsigned h = 2166136261u; while (*k) h = (h ^ (unsigned char)*k++) * 16777619u; return h; }

static uint32_t rgb(int r, int g, int b) { return 0xFF000000u | (uint32_t)(b & 255) << 16 | (uint32_t)(g & 255) << 8 | (uint32_t)(r & 255); }

/* a cover: two colours from the key, rings and a band, so that each looks its own */
static uint32_t *picture(const char *key, int px)
{
    unsigned h = hash(key); uint32_t *p = malloc((size_t)px * px * 4);
    int r0 = 40 + h % 180, g0 = 40 + (h >> 8) % 180, b0 = 40 + (h >> 16) % 180, kind = h >> 24 & 3;
    for (int y = 0; y < px; y++) for (int x = 0; x < px; x++) {
        float u = (float)x / px, v = (float)y / px, k;
        if (kind == 0) k = 0.5f + 0.5f * sinf((u * u + v * v) * 18);
        else if (kind == 1) k = v;
        else if (kind == 2) k = fabsf(u - 0.5f) < 0.18f ? 1 : 0.3f;
        else k = (int)(u * 4 + v * 4) % 2 ? 0.9f : 0.4f;
        p[y * px + x] = rgb((int)(r0 * k + 20 * (1 - k)), (int)(g0 * (1 - k * 0.5f)), (int)(b0 * k + 60 * (1 - k)));
    }
    return p;
}

struct pend { char key[48]; int px; } pend[256]; static int npend;
static void t_image(void *ctx, const char *key, int px) { (void)ctx; if (npend < 256) { snprintf(pend[npend].key, 48, "%s", key); pend[npend++].px = px; } }
static void deliver(void) { for (int i = 0; i < npend; i++) scene_image(sc, pend[i].key, pend[i].px, picture(pend[i].key, pend[i].px), pend[i].px, pend[i].px); npend = 0; }

static int want_backdrop;
static void t_shot(void *ctx) { (void)ctx; want_backdrop = 1; }

/* ---------------------------------------------------------------- Music Assistant, made up */

static const char *artists[] = { "Radiohead", "Billie Eilish", "Daft Punk", "Nils Frahm", "Khruangbin", "Rosalía", "Fleetwood Mac",
                                 "Kendrick Lamar", "Björk", "Bonobo", "Portishead", "Massive Attack", "Caribou", "Jon Hopkins", "Beach House" };
static const char *albums[] = { "OK Computer", "Happier Than Ever", "Random Access Memories", "All Melody", "Mordechai", "Motomami",
                                "Rumours", "To Pimp a Butterfly", "Homogenic", "Migration", "Dummy", "Mezzanine", "Suddenly", "Immunity", "Bloom" };
static const char *songs[] = { "Paranoid Android", "Lost Cause", "Get Lucky", "Sunson", "Time (You and I)", "Saoko", "Dreams",
                               "Alright", "Jóga", "Kerala", "Glory Box", "Teardrop", "Never Come Back", "Open Eye Signal", "Myth" };

static void line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void line(const char *fmt, ...)
{
    char b[1024]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    scene_msg(sc, b, now);
}

static void answer(const char *l)
{
    int req, off; char what[32], arg[200] = "";
    if (sscanf(l, "ml %d %31s %d %199[^\n]", &req, what, &off, arg) < 3) return;
    int k = off ? 0 : 15;
    line("L %d %d %d", req, off, !strcmp(what, "search") ? 15 : !strcmp(what, "queue") ? 12 : !strcmp(what, "playlists") ? 8 : !strcmp(what, "radios") ? 6 : 15);
    if (off) { line("E %d", req); return; }
    if (!strcmp(what, "artists")) for (int i = 0; i < k; i++) line("I %d r\tlibrary://artist/%d\t%s\t%d albums\tar%d\t0\t", req, i, artists[i], 2 + i % 5, i);
    else if (!strcmp(what, "albums") || !strcmp(what, "artist_albums")) for (int i = 0; i < k; i++) line("I %d a\tlibrary://album/%d\t%s\t%s · %d\tal%d\t0\t", req, i, albums[i], artists[i], 1994 + i, i);
    else if (!strcmp(what, "playlists")) for (int i = 0; i < 8; i++) line("I %d p\tlibrary://playlist/%d\t%s\t%d tracks\tpl%d\t0\t", req, i, (const char *[]){ "Morning", "Focus", "Dinner", "Running", "Discover Weekly", "Liked songs", "Rainy day", "Party" }[i], 20 + i * 7, i);
    else if (!strcmp(what, "radios")) for (int i = 0; i < 6; i++) line("I %d o\tradiobrowser://radio/%d\t%s\t%s\tra%d\t0\t", req, i, (const char *[]){ "BBC Radio 6 Music", "FIP", "KEXP", "Radio Paradise", "SomaFM Groove Salad", "NTS 1" }[i], "Radio", i);
    else if (!strcmp(what, "queue")) for (int i = 0; i < 12; i++) line("I %d q\tqi%d\t%s\t%s\tal%d\t%d\t%s", req, i, songs[i], artists[i], i, 180 + i * 17, i == 3 ? "c" : "");
    else if (!strcmp(what, "search")) {
        for (int i = 0; i < 3; i++) line("I %d r\tlibrary://artist/%d\t%s\tArtist\tar%d\t0\t", req, i, artists[i], i);
        for (int i = 0; i < 4; i++) line("I %d a\tlibrary://album/%d\t%s\t%s\tal%d\t0\t", req, i, albums[i], artists[i], i);
        for (int i = 0; i < 8; i++) line("I %d t\tlibrary://track/%d\t%s\t%s · %s\tal%d\t%d\t", req, i, songs[i], artists[i], albums[i], i, 200 + i * 13);
    } else for (int i = 0; i < k; i++) line("I %d t\tlibrary://track/%d\t%s\t%s\tal%d\t%d\t", req, i, songs[i], artists[i % 15], i, 190 + i * 11);
    line("E %d", req);
}

static void t_send(void *ctx, const char *l) { (void)ctx; if (getenv("UISHOT_LOG")) fprintf(stderr, "> %s\n", l); if (!strncmp(l, "ml ", 3)) answer(l); }

static struct ui_app apps_[6];
static void t_apps(void *ctx)
{
    (void)ctx;
    static const char *n[] = { "Kiosk Satellite", "Home Assistant", "Simple Keyboard", "Fully Kiosk", "View Assist", "Settings" };
    struct ui_app *a = calloc(6, sizeof *a);
    for (int i = 0; i < 6; i++) {
        snprintf(a[i].pkg, sizeof a[i].pkg, i ? "org.example.app%d" : "me.jxl.kiosk_satellite", i);
        snprintf(a[i].label, sizeof a[i].label, "%s", n[i]);
        if (i != 2) {
            a[i].icon.w = a[i].icon.h = 96; a[i].icon.px = malloc(96 * 96 * 4);
            for (int p = 0; p < 96 * 96; p++) {
                int x = p % 96 - 48, y = p / 96 - 48; float d = sqrtf((float)(x * x + y * y));
                float al = d < 44 ? 1 : d < 46 ? (46 - d) / 2 : 0; unsigned A = (unsigned)(al * 255);
                unsigned r = (unsigned)((60 + i * 37) % 256 * al), g = (unsigned)((180 - i * 25) % 256 * al), b = (unsigned)((90 + i * 50) % 256 * al);
                if (abs(x) < 14 && abs(y) < 14 && al == 1) r = g = b = 240;
                a[i].icon.px[p] = A << 24 | b << 16 | g << 8 | r;
            }
        }
    }
    (void)apps_;
    scene_apps(sc, a, 6);
}
static int t_measure(void *ctx, const char *s, int px, int bold)  /* as android.c's p_measure: the paint's width, uncut */
{
    (void)ctx;
    FT_Face f = face[bold ? 1 : 0];
    FT_Set_Pixel_Sizes(f, 0, (FT_UInt)(px * 0.8f));
    unsigned cp[1024]; int n = 0;
    for (const char *p = s; *p && n < 1024; ) cp[n++] = utf8(&p);
    return measure(f, cp, n) + 1;
}
static void t_launch(void *ctx, const char *pkg, const char *cls) { (void)ctx; (void)pkg; (void)cls; }
static void t_back(void *ctx) { (void)ctx; }
static void t_sleep(void *ctx) { (void)ctx; }
static const struct ui_platform plat = { t_text, t_launch, t_back, t_send, t_apps, t_image, t_shot, t_sleep, t_measure };

/* ---------------------------------------------------------------- frames and pictures */

static void dashboard(uint32_t *p);
static void frame(void)
{
    int next;
    for (int pass = 0; pass < 2; pass++) {
        struct rect d = scene_prepare(sc, now, &next);
        if (!rect_empty(d)) { struct gfx g = { fb, W, H, W, d }; scene_draw(sc, &g); }
        if (!npend && !want_backdrop) break;
        deliver();
        if (want_backdrop) {                                    /* the app under the drawer: a made-up dashboard */
            want_backdrop = 0; uint32_t *p = malloc((size_t)W * H * 4);
            dashboard(p);
            scene_backdrop(sc, p, W, H);
        }
    }
}

static void run(int ms) { long long end = now + ms; while (now < end) { frame(); now += 16; } frame(); }

static void dashboard(uint32_t *p)                              /* an app: a made-up dashboard */
{
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++)
        p[y * W + x] = (y < 56) ? rgb(3, 169, 244) : ((x / 240 + y / 150) % 2) ? rgb(250, 250, 250) : rgb(236, 239, 241);
}

static void crc_put(FILE *f, const char *type, const unsigned char *d, unsigned n)
{
    unsigned char b[4] = { (unsigned char)(n >> 24), (unsigned char)(n >> 16), (unsigned char)(n >> 8), (unsigned char)n };
    fwrite(b, 1, 4, f); fwrite(type, 1, 4, f); if (n) fwrite(d, 1, n, f);
    uLong c = crc32(0, (const Bytef *)type, 4); if (n) c = crc32(c, d, n);
    unsigned char cb[4] = { (unsigned char)(c >> 24), (unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c };
    fwrite(cb, 1, 4, f);
}

static void png(const char *name)
{
    char path[512]; snprintf(path, sizeof path, "%s/%s.png", out, name);
    size_t raw_n = (size_t)(W * 3 + 1) * H; unsigned char *raw = malloc(raw_n);
    for (int y = 0; y < H; y++) {
        raw[(size_t)y * (W * 3 + 1)] = 0;
        for (int x = 0; x < W; x++) { uint32_t p = fb[y * W + x]; unsigned char *o = raw + (size_t)y * (W * 3 + 1) + 1 + x * 3; o[0] = p & 255; o[1] = p >> 8 & 255; o[2] = p >> 16 & 255; }
    }
    uLongf zn = compressBound(raw_n); unsigned char *z = malloc(zn); compress2(z, &zn, raw, raw_n, 6);
    FILE *f = fopen(path, "wb"); if (!f) { perror(path); exit(1); }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    unsigned char ih[13] = { (unsigned char)(W >> 24), (unsigned char)(W >> 16), (unsigned char)(W >> 8), (unsigned char)W,
                             (unsigned char)(H >> 24), (unsigned char)(H >> 16), (unsigned char)(H >> 8), (unsigned char)H, 8, 2, 0, 0, 0 };
    crc_put(f, "IHDR", ih, 13); crc_put(f, "IDAT", z, (unsigned)zn); crc_put(f, "IEND", NULL, 0);
    fclose(f); free(raw); free(z);
    printf("%s\n", path);
}

/* the words' layer (words.c), faded in, over what shows (or an app), as SurfaceFlinger blends it: premultiplied, times
 * the layer's alpha */
static void words_png(const char *name, int over_app)
{
    static uint32_t *wb; if (!wb) wb = calloc((size_t)W * H, 4);
    float a = 0; int nx;
    for (int i = 0; i < 60 && (i < 30 || a < 0.98f); i++) {     /* (a ringing one: at the top of its pulse) */
        now += 33;
        if (scene_words(sc, now, &a, &nx) == 2) { struct gfx g = { wb, W, H, W, { 0, 0, W, H } }; scene_words_draw(sc, &g); }
    }
    uint32_t *save = malloc((size_t)W * H * 4); memcpy(save, fb, (size_t)W * H * 4);
    if (over_app) dashboard(fb);
    for (int i = 0; i < W * H; i++) {
        uint32_t s = wb[i], d = fb[i], o = 0xFF000000u; float k = 1 - (s >> 24) / 255.0f * a;
        for (int c = 0; c < 24; c += 8) o |= (uint32_t)fminf(255, (s >> c & 255) * a + (d >> c & 255) * k + 0.5f) << c;
        fb[i] = o;
    }
    png(name);
    memcpy(fb, save, (size_t)W * H * 4); free(save);
}

static void tap(int x, int y) { scene_touch(sc, TOUCH_DOWN, x, y, now); now += 50; scene_touch(sc, TOUCH_UP, x, y, now); run(40); }
static void pull(int y1) { scene_touch(sc, TOUCH_DOWN, W / 2, -8, now); for (int y = -8; y <= y1; y += 20) { now += 16; scene_touch(sc, TOUCH_MOVE, W / 2, y, now); frame(); } now += 16; scene_touch(sc, TOUCH_UP, W / 2, y1, now); run(600); }
static void longpress(int x, int y) { scene_touch(sc, TOUCH_DOWN, x, y, now); run(700); scene_touch(sc, TOUCH_UP, x, y, now); run(100); }

static void music_data(void)
{
    line("mc 45 0 play,pause,next,previous,volume,mute,seek,shuffle,unshuffle,repeat_off,repeat_one,repeat_all all 1");
    line("mt Paranoid Android\tRadiohead\tOK Computer");
    line("mp 1 141000 387000 1000");
    scene_art(sc, picture("nowplaying", 400), 400, 400);
}

static void ma_data(void)
{
    line("ma up Music Assistant");
    line("pl self1\tEcho Show 5 kitchen\tsgva\tplaying\t45\t\tself1,dot2\tParanoid Android · Radiohead");
    line("pl dot2\tEcho Dot living room\tgva\tplaying\t30\tself1\t\tParanoid Android · Radiohead");
    line("pl sonos\tSonos Move\tgva\tpaused\t22\t\t\tTeardrop · Massive Attack");
    line("pl cast\tBedroom speaker\tgva\tidle\t60\t\t\t");
    line("pl tv\tLiving room TV\tva\tplaying\t15\t\t\tNews");
    line("pl all\tWhole house\tlva\tidle\t50\t\t\t");
    line("pl.");
    line("pq playing\t1\tall\t3\t12\tEcho Show 5 kitchen + 1");
}

int main(int argc, char **argv)
{
    out = argc > 1 ? argv[1] : "build/uishot";
    const char *fd = argc > 2 ? argv[2] : "firmware/checkers/rootfs/system/fonts";
    if (argc > 5) { W = atoi(argv[3]); H = atoi(argv[4]); BAND = atoi(argv[5]); }
    char p0[512], p1[512]; snprintf(p0, sizeof p0, "%s/Roboto-Regular.ttf", fd); snprintf(p1, sizeof p1, "%s/Roboto-Bold.ttf", fd);
    if (FT_Init_FreeType(&ft) || FT_New_Face(ft, p0, 0, &face[0]) || FT_New_Face(ft, p1, 0, &face[1])) {
        fprintf(stderr, "uishot: no fonts in %s (Roboto-Regular.ttf, Roboto-Bold.ttf)\n", fd); return 1;
    }
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    fb = calloc((size_t)W * H, 4);
    sc = scene_new(W, H, BAND, &plat, NULL, now);
    scene_top(sc, UI_HOME_PKG, now);
    line("name Echo Show 5 fdd111"); line("home -"); line("web http://192.168.1.40:28931"); line("ev 40");
    run(500); png("face");
    /* what was said, the reply, the timers (words.c): over the face and over an app */
    line("wo 1 1 8"); line("wh What's the weather like tomorrow?");
    line("wr Tomorrow will be sunny with highs of 18 degrees and a light breeze from the west."); line("ws -1");
    words_png("words", 0); words_png("words_over_app", 1);
    line("wh Was steht heute in meinem Kalender, und muss ich etwas mitbringen?");
    line("wr Heute stehen drei Termine im Kalender: um neun Uhr das Teamtreffen im Büro, um zwölf das Mittagessen mit Anna "
         "im Café am Markt und um sechzehn Uhr der Zahnarzt. Für das Teamtreffen bringst du den Laptop mit, für den Zahnarzt "
         "die Versichertenkarte. Am Abend ist nichts eingetragen, und morgen früh beginnt der Tag erst um zehn Uhr mit einem "
         "Anruf bei der Bank, der etwa eine halbe Stunde dauern soll."); line("ws -1");
    words_png("words_long", 0);
    line("wh "); line("wr "); line("ws -1");
    line("tl 3\t299000 600 1 Pizza\t3725000 7200 1 2 h\t1200000 2400 0 Kuchen");
    words_png("timers", 0); words_png("timers_over_app", 1);
    line("tl 1\t2700000 3600 1 Laundry"); words_png("timer_one", 0);
    line("timers Pizza"); words_png("timer_ringing", 0);
    line("timers "); line("tl 0"); line("wo 0 0 8"); words_png("words_gone", 0);
    music_data(); run(400); png("face_music");
    pull(300); png("drawer");
    tap(W / 2, H - 20); run(500);                               /* the shade: closed again */
    line("ma off"); tap(W / 2, (int)(0.87f * H)); run(500);     /* the music, from the face's bottom line */
    png("music_now");
    tap(46, 76 + 0 * 79 + 140); run(300); png("music_setup");     /* the queue's tab without Music Assistant's API */
    ma_data(); run(200);
    tap(46, (int)(76 + 1.5f * (H - 76) / 5)); run(400); png("music_queue");
    tap(46, (int)(76 + 2.5f * (H - 76) / 5)); run(400); png("music_library");
    tap(140, 36); run(100);                                     /* chips: artists */
    tap(92 + 60, 36); run(400); png("music_artists");
    tap(92 + 28 + 80, 70 + 80); run(400); png("music_artist");   /* an artist: its albums */
    tap(92 + 28 + 80, 70 + 18 + 132 + 26 + 80); run(400); png("music_album");
    longpress(400, 70 + 176 + 30 + 64 * 1); png("music_sheet");
    tap(20, 20); run(300);
    tap(46, (int)(76 + 3.5f * (H - 76) / 5)); run(300);
    const char *q = "radio"; for (const char *c = q; *c; c++) { (void)c; }
    line("ma up Music Assistant");
    /* type "radio" on the keyboard: its keys by position (letters row 2 and 1) */
    { float kw = (W - 92) / 10.0f, kh = fminf(232, H * 0.52f) / 4, ky = H - fminf(232, H * 0.52f);
      const char *rows[3] = { "qwertyuiop", "asdfghjkl", "zxcvbnm" };
      for (const char *c = q; *c; c++)
          for (int r = 0; r < 3; r++) { const char *f = strchr(rows[r], *c); if (!f) continue;
              int len = (int)strlen(rows[r]); float x = 92 + ((W - 92) - len * kw) / 2 + (f - rows[r]) * kw + kw / 2;
              tap((int)x, (int)(ky + r * kh + kh / 2)); } }
    run(600); png("music_search");
    tap(92 + (W - 92) / 2, (int)(H - 30)); run(100);            /* space... then Done */
    { float kw = (W - 92) / 10.0f; tap((int)(92 + 8.9f * kw), H - 20); } run(600); png("music_results");
    tap(46, (int)(76 + 4.5f * (H - 76) / 5)); run(400); png("music_speakers");
    /* the drawer over an app: the app's picture under it */
    line("kiosk me.jxl.kiosk_satellite");
    tap(46, 38); run(300);
    scene_top(sc, "me.jxl.kiosk_satellite", now); run(3500);    /* past the moment our home was asked for */
    pull(330); png("drawer_over_app");
    printf("uishot: ok\n");
    scene_free(sc); free(fb);
    return 0;
}
