/* What the screen UI's parts share inside src/ui/ (scene.h is what the platform sees):
 *
 *   scene.c    the display list and its diff, text and image caches, touches (taps, long presses, drags, scrolling
 *              with flings, the edges), frames, the messages from hassmic
 *   widgets.c  icons drawn as shapes, buttons, sliders, list rows and tiles, the keyboard, the action sheet
 *   face.c     the face (what the satellite does) and its bottom line (a timer, a call, the music)
 *   drawer.c   the drawer pulled down from the top edge over anything: home, the face, music, Do not disturb, screen
 *              off, what plays, the volume
 *   music.c    Music Assistant: now playing, the queue, the library, search, the speakers and their groups
 *   words.c    what was said, the reply and the timers, on a see-through layer of their own over everything but the
 *              drawer (scene.h scene_words)
 *
 * Sizes are in units of the Echo Show 5's layer (960 x 470 with a 10-row band): U(x) scales them to the screen.
 */
#ifndef UI_UI_H
#define UI_UI_H
#include "scene.h"
#include <stddef.h>

#define FRAME_MS   33                   /* 30 frames a second while anything moves */
#define DRAG_MS    16                   /* while a finger moves something or a fling runs */
#define MAXITEMS   2400
#define MAXID      8192
#define NTEXT      640
#define NIMG       192
#define NHITS      400
#define NSCROLL    8
#define LONG_MS    550                  /* a long press */
#define TAP_PX     14                   /* a tap moves less than this */

#define U(v)       ((v) * s->u)

/* colours */
#define BG       0x000000
#define SURF     0x15181D               /* panels, the drawer */
#define SURF2    0x22262D               /* raised: rows pressed, chips */
#define LINE     0x30353D
#define FG       0xF2F4F7
#define DIM      0x9AA1AB
#define FAINT    0x5F6670
#define DARK     0x1A1D22
#define ACCENT   0x2EC5FF               /* Music Assistant's views, selections */
#define CYAN     0x00E5FF
#define GREEN    0x34E07A
#define RED      0xFF453A
#define ORANGE   0xFF8A1E
#define VIOLET   0x9B7BFF
#define BLUE     0x4D7CFF

/* id ranges: an item keeps its id from frame to frame, so the diff finds what changed */
#define ID_FACE    100
#define ID_MUSIC   1000                 /* .. 7399 */
#define ID_DRAWER  7400                 /* .. 7899 */
#define ID_TOP     7900                 /* .. 8191 */

/* actions (hits): who handles one goes by its range */
enum {
    A_NONE,
    A_POKE = 1, A_PLAYPAUSE, A_TOMUSIC, A_ANSWER, A_HANGUP, A_TIMERSTOP,               /* face.c */
    A_DR_SHADE = 100, A_DR_HOME, A_DR_MUSIC, A_DR_DND, A_DR_SLEEP, A_DR_VOL, A_DR_PLAY, A_DR_NOW, A_DR_FACE,  /* drawer.c */
    A_M_TAB = 200, A_M_HOME, A_M_PLAY, A_M_PREV, A_M_NEXT, A_M_SHUFFLE, A_M_REPEAT, A_M_SEEK, A_M_VOL,  /* music.c */
    A_M_CAT, A_M_ITEM, A_M_BACK, A_M_DPLAY, A_M_DSHUF, A_M_DADD, A_M_DRADIO, A_M_QITEM, A_M_QDEL, A_M_QCLEAR,
    A_M_FIELD, A_M_KEY, A_M_CLEARQ, A_M_KBHIDE, A_M_SPK, A_M_SPKVOL, A_M_GVOL, A_M_LEAVE, A_M_SHEET, A_M_SHADE,
    A_M_GROUPS, A_M_MORE,
};
enum { EV_TAP, EV_LONG, EV_DRAG, EV_DROP };              /* what a hit gets: a tap, a long press, a drag's moves, its end */
#define HIT_DRAG 1                                        /* a slider: it gets the finger's moves, never a scroller */

enum { V_FACE, V_MUSIC };

enum { K_FILL, K_RRECT, K_POLY, K_TEXT, K_IMG, K_GRAD, K_STROKE, K_ARC, K_ICON };
struct item {
    uint16_t id; uint8_t kind, n; struct rect box, clip;
    float f[8]; uint32_t rgb, rgb2; int alpha; const void *ref; unsigned ver;
};

struct anim { float v, t; };
struct ui_text_e { char key[200]; unsigned hash; struct ui_text t; int ok; unsigned ver; long long used; };
/* an image by key and the size it is shown at: asked of the platform once, here when it came */
struct ui_img { char key[40]; int px; struct ui_image im; int state; long long used, asked; };
enum { IMG_FREE, IMG_ASKED, IMG_OK, IMG_FAILED };
struct hit { struct rect r; int act, arg, flags; unsigned key; };   /* key: what is there, where arg is a row's place */

/* a list or grid that scrolls, by finger and on after it (a fling), back into its bounds when pulled past them */
struct scroller { float pos, vel, max, pos0; int axis, held, moving; float sv[4]; long long st[4]; };
struct scroll_area { struct rect r; struct scroller *sc; int hseq; };   /* hseq: the hits drawn before it */

/* ---------------------------------------------------------------- music.c's model */

struct mitem { char kind; int dur; char uri[224], title[128], sub[128], img[40], flags[8]; };   /* (mass.c's sizes or more) */
struct mlist {
    int req, loading, done, failed, total, page0;   /* page0: how many there were when the page came */
    char what[24], arg[224], title[128], sub[128], img[40], kind;
    struct mitem *it; int n, cap; struct scroller sc; char err[96];
    int fill, refill, again;    /* asked again: rows replaced from fill on, up to refill as many as were there; again: once more */
    long long failed_at;
};
/* key: its own while the UI knows it (the hits' arg: the table shifts when one goes, and is told anew after a reconnect) */
struct mplayer { char id[96], name[64], flags[12], state[12], leader[96], members[600], playing[160]; int vol, gvol, key; };   /* gvol: -1 none */
#define MDEPTH 6
#define MPLAYERS 40

struct music {
    int tab, cat, depth;
    struct mlist *stack[MDEPTH];                /* the library: a category, then what was opened from it */
    struct mlist queue, search;
    struct scroller cats, spk;
    char query[128]; int kb, kb_shift, kb_sym; long long query_at; int query_sent;
    struct mplayer pl[MPLAYERS]; int npl, pl_all, pl_keys;
    int ma; char ma_text[160];
    char q_state[12], q_name[96]; int q_shuffle, q_repeat, q_index, q_items, q_known;
    int q_goto;                                 /* the queue just opened: it opens where it plays (pages loaded to there) */
    int sheet; struct mitem sheet_it; int sheet_from;            /* sheet_it: a copy (a speaker's: its id as uri) */
    int seek_drag; float seek_frac; int vol_drag; float vol_frac; long long vol_sent_at;
    int spk_drag; float spk_frac; long long spk_sent_at;        /* spk_drag: a speaker's key, -1 the group's, 0 none */
    float sl0[4], sl1[4];                       /* where the sliders were drawn: seek, volume, a speaker's, the group's */
    char toast[96]; long long toast_until;
    int req_next;
};

struct face { struct anim ew, lh, rh, gx, gy, mw, mh, ma, cr, cg, cb, br; long long blink_at, sacc_at, poke_at; float sgx, sgy, pokex; };
/* pos 0 closed .. 1 open, t where it goes; pulling: a finger has it (grab: where on it); from_app: pulled over an app
 * (from), the picture of it under the drawer */
struct drawer { float pos, t, grab, vy; long long ty; int pulling, from_app, go; char from[96], go_pkg[96];
                long long vol_sent_at; int vol_drag; float vol_frac; };

/* ---------------------------------------------------------------- words.c's model */

#define WTIMERS 8
/* a running timer: end on our clock (active), left in ms (paused) */
struct wtimer { char name[64]; int total, active; long long end, left; };
struct words {
    int on, timers_on, hold_s;                  /* "wo": the settings */
    char heard[1024], reply[1024]; int idle; long long idle_at;     /* "wh", "wr", "ws" */
    struct wtimer t[WTIMERS]; int nt; unsigned tl_seq;   /* "tl" */
    unsigned ver;                               /* what hassmic told changed */
    int pic; unsigned pic_ver, pic_secs;        /* what the layer's picture shows: which, of which version, which seconds */
    float alpha; long long alpha_at;
    int shown, timers_hidden;                   /* shown: the layer is up (scene_words); timers_hidden: tapped away until
                                                   the list changes */
    struct rect stop;                           /* a ringing timer's Stop, where it was drawn (the layer's = ours) */
    char ring_from[96];                         /* the app a ring brought our screen up over: back to it after */
    long long ring_at; int own_was, solid;      /* ring_at: when it was asked for (again every 10 s while the app keeps
                                                   the front); own_was: own_asked before it; solid: faded in past half */
    int grab;                                   /* the panel (android.c): -1 cannot be taken, 0 Android's, 1 ours */
};

/* ---------------------------------------------------------------- the scene */

struct scene {
    const struct ui_platform *p; void *ctx;
    int w, h, band; float u;
    struct item list[2][MAXITEMS]; int n[2], cur; int16_t idx[MAXID]; int full;
    struct rect clip;                          /* what emitted items are cut to (scroll areas) */
    struct ui_text_e text[NTEXT]; unsigned text_ver;
    struct ui_img img[NIMG];
    struct hit hits[NHITS]; int nhits, pressed, pact, parg;   /* pressed: a finger is on a hit, pact/parg: which (the
                                                              * hits are made anew each frame: never an index into them) */
    unsigned pkey;                                           /* and its key (a list's row: which item it was) */
    struct scroll_area scrolls[NSCROLL]; int nscrolls;
    struct scroller *prev[NSCROLL]; int nprev;          /* the last frame's: one not drawn again stops (its fling) */
    /* the satellite */
    struct { long long at; int on; } pat[32];
    int level, mute, dnd, oobe, h12, evol;
    char name[64], timers[256], peer[64], home[96], act[64], web[96];
    /* music as Sendspin tells it (scene.h: mt, mp, mc, art) */
    int music, mstate, mspeed, mvol, mmuted, mrepeat, mshuffle; long long mpos, mdur, mpos_at;
    /* play/pause shown at once (music_toggle); what Sendspin said meanwhile, for when it never agrees */
    int pend_state, srv_state, srv_speed; long long pend_until, srv_pos, srv_dur, srv_at;
    char title[192], artist[192], album[192], mcmds[256];
    struct ui_image art; uint32_t art_avg;
    /* apps */
    struct ui_app *apps; int napps;
    /* view */
    int view, screen_on; char top[96]; long long now, last, want_home_until, relaunch[3];
    /* an app was launched: the last frame stays (the app's picture under the drawer, or our own screen) until the
     * app is in front, so that neither the face nor hassmic's black home activity shows in between */
    long long leaving_until; int leaving_drawn; char leaving_pkg[96]; int home_broken;   /* home_broken: it did not start */
    long long home_retry_at; int home_retry_ms;     /* and is tried again then (30 s, doubling to 10 min) */
    int own_asked;                                  /* the face or the music asked for: the home app not brought back */
    struct ui_image backdrop;                  /* the app the drawer was pulled over */
    /* parts */
    struct face face;
    struct drawer dr;
    struct music m;
    struct words wd;
    /* touch */
    int down, x0, y0, x, y, edge, fired, longd, dragging, slide, wtap; long long t0; struct scroller *sc; int sc_drag, sc_axis;   /* slide: on a slider, not decided yet; longd: 2 held long, 1 and taken (or let go of: no tap) */
    unsigned rnd;
};

/* scene.c: the display list */
struct item *emit(struct scene *s, int id, int kind);
void fill(struct scene *s, int id, struct rect r, uint32_t rgb);
void rrect(struct scene *s, int id, float x0, float y0, float x1, float y1, float rad, uint32_t rgb, int alpha);
void stroke(struct scene *s, int id, float x0, float y0, float x1, float y1, float rad, float t, uint32_t rgb, int alpha);
void arc(struct scene *s, int id, float cx, float cy, float r, float t, float a0, float a1, uint32_t rgb, int alpha);
void poly(struct scene *s, int id, const float *xy, int n, uint32_t rgb, int alpha);
void grad(struct scene *s, int id, struct rect r, uint32_t top, uint32_t bottom);
/* text with its top at y; align 0 left of x, 1 centred on it, 2 right of it; its width (0: none) */
int  text(struct scene *s, int id, const char *str, float px, int bold, float maxw, float x, float y, int align, uint32_t rgb, int alpha);
int  text_width(struct scene *s, const char *str, float px, int bold, float maxw);
/* the mask of a text from the cache (NULL: none), for drawing outside the display list (words.c's layer) */
const struct ui_text *text_mask(struct scene *s, const char *str, float px, int bold, float maxw);
int  text_measure(struct scene *s, const char *str, float px, int bold);   /* its whole width, uncut */
/* an image: drawn scaled to w x h with rounded corners (rad); one of the image cache is asked for when missing: 1 drawn */
int  image(struct scene *s, int id, const struct ui_image *im, float x, float y, float w, float h, float rad);
const struct ui_image *img_get(struct scene *s, const char *key, int px);
void app_icon(struct scene *s, int id, const struct ui_icon *ic, float x, float y, float w, float h);   /* premultiplied */
const char *home_app(const struct scene *s);                   /* the app the Echo comes back to; "" the face */
void hit(struct scene *s, float x0, float y0, float x1, float y1, int act, int arg);
void hit_drag(struct scene *s, float x0, float y0, float x1, float y1, int act, int arg);
void hit_key(struct scene *s, float x0, float y0, float x1, float y1, int act, int arg, unsigned key);   /* a row: arg its place, key its item */
unsigned ui_hash(const char *k);
int  pressed(struct scene *s, int act, int arg);
void clip_push(struct scene *s, struct rect r, struct rect *saved);
void clip_pop(struct scene *s, struct rect saved);
void scroll_area(struct scene *s, struct rect r, struct scroller *sc, int axis, float content);
void scroll_to(struct scroller *sc, float pos);
int  on_its_way(const struct scene *s);                        /* an app launched, not in front yet */
int  home_pending(void);                                       /* the page picked another home app, not applied yet */
void ask_own(struct scene *s);                                 /* the face or the music asked for: the home app not brought
                                                                  over it, nor a choice the page made meanwhile (Home's) */
void forget_scroller(struct scene *s, const struct scroller *sc);    /* its owner is freed: no frame or touch keeps it */
void send(struct scene *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int  can(const struct scene *s, const char *cmd);              /* Sendspin's controller allows it */
long long music_pos(const struct scene *s);
void music_toggle(struct scene *s);                            /* play/pause: shown at once, sent */
int  is_home(const char *pkg);
void go_home(struct scene *s, int view);
void launch(struct scene *s, const char *pkg, const char *cls);
float clampf(float v, float lo, float hi);
int  ease(struct anim *a, float dt, float tau);
float frand(struct scene *s);
int  eq(const char *a, const char *b);
void fmt_time(char *out, size_t n, long long ms);

/* widgets.c */
enum { IC_PLAY, IC_PAUSE, IC_NEXT, IC_PREV, IC_SHUFFLE, IC_REPEAT, IC_REPEAT1, IC_NOTE, IC_QUEUE, IC_LIBRARY, IC_SEARCH,
       IC_SPEAKER, IC_SPEAKERS, IC_HOME, IC_MOON, IC_POWER, IC_BACK, IC_CLOSE, IC_PLUS, IC_CHECK, IC_RADIO, IC_MIC,
       IC_PERSON, IC_ALBUM, IC_LIST, IC_BACKSPACE, IC_SHIFT, IC_FACE, IC_MORE, IC_GROUP, IC_DISC };
void icon(struct scene *s, int id, int which, float cx, float cy, float r, uint32_t col, uint32_t bg);   /* id .. id + 7 */
/* id .. id + 9 (its icon from id + 1); round_button id .. id + 8 */
void button(struct scene *s, int id, float x0, float y0, float x1, float y1, const char *label, int ic, uint32_t bg, uint32_t fg, int act, int arg);
void round_button(struct scene *s, int id, float cx, float cy, float r, int ic, uint32_t bg, uint32_t fg, int act, int arg);
/* a slider's track from x0 to x1 at y, value 0..1; dragged: the hit (act, arg) gets EV_DRAG/EV_DROP with the finger */
void slider(struct scene *s, int id, float x0, float x1, float y, float v, uint32_t col, int act, int arg);
float slider_value(struct scene *s, float x0, float x1);       /* where the finger is along it */
/* the keyboard over r; its keys are hits act with arg = the character (or K_*) */
enum { K_BACKSPACE = 1, K_SHIFT, K_SYM, K_DONE, K_SPACE };
void keyboard(struct scene *s, int id, struct rect r, int shift, int sym, int act);
/* a modal list of choices over everything: returns the row tapped by act/arg */
void sheet(struct scene *s, int id, const char *title, const char *const *rows, int n, int act, int shade);
void toast(struct scene *s, int id, const char *msg);

/* face.c */
int  face_view(struct scene *s, float dt);                      /* still moving */
long long face_due(struct scene *s);
int  face_act(struct scene *s, int a, int arg, int ev);
int  face_mode(struct scene *s);
void face_init(struct scene *s, long long now);
void face_pattern(struct scene *s, int p, int on, long long ago);
int  pat_find(const char *name);

/* drawer.c */
int  drawer_step(struct scene *s, float dt);                    /* before the frame: 2 while it moves */
void drawer_draw(struct scene *s);
int  drawer_act(struct scene *s, int a, int arg, int ev);
void drawer_pull(struct scene *s, int y, int ev);               /* the top edge's pull: TOUCH_DOWN/MOVE/UP */
int  drawer_open(const struct scene *s);
void drawer_close(struct scene *s, int back_to_app);
void drawer_close_now(struct scene *s);                        /* gone at once, nothing launched */

/* words.c */
int  words_msg(struct scene *s, const char *cmd, const char *a);   /* 1: one of its messages (wo wh wr ws tl) */
int  words_shown(const struct scene *s);                           /* the layer is up over our own screen: its touches */
void words_tap(struct scene *s, int x, int y);                     /* a tap on it */

/* music.c */
void music_view(struct scene *s, float dt, int *moving, long long *due);
int  music_act(struct scene *s, int a, int arg, int ev);
void music_msg(struct scene *s, const char *cmd, const char *a);
void music_init(struct scene *s);
void music_free(struct scene *s);
void music_show(struct scene *s, int tab);
#endif
