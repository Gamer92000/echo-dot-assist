/* The screen UI's content (src/ui/, ui.h has its parts), apart from the platform (src/ui/android.c on the Echo Show,
 * the unit test and tools/uishot.c on the PC): the screen of a satellite whose screen any app may have.
 *
 *   face      a pair of eyes saying what the satellite does (listening, thinking, speaking, muted, Do not disturb, a
 *             ringing timer, a call, setup), a mouth while it speaks, the clock, and what Music Assistant plays
 *   music     Music Assistant: now playing (Sendspin's metadata and controller), and with its API (hassmic's mass.c)
 *             the queue, the library, search, and the speakers with their groups and volumes
 *   drawer    pulled down from the top edge over anything, as on Amazon's own Echo Show: home, the face, music, Do not
 *             disturb, screen off, what plays, the volume
 *
 * The layer shows over hassmic's home activity only (a codeless black one, tools/mkhome.py): touches then land on
 * it, harmlessly, and reach us through evdev.  Over another app the layer is hidden, and the app has the screen.  The
 * edges are ours whatever is in front: a pull down from the top opens the drawer (the app's picture under it, our home
 * activity brought up so that the next touches are ours), a swipe along the band at the bottom is Back.
 *
 * The home app: what the Echo shows at start and brings back when it leaves (three times a minute at most), unless
 * the face or the music was asked for.  Unchosen, it is Kiosk Satellite where that is installed ("kiosk"), else the
 * face; the settings page's Screen page picks another ("home" from hassmic: it applies at once).  While this UI runs,
 * root's kiosk_tick leaves the app's opening to it (main.sh).
 *
 * The grip: a few rows along the top that, like the band's, no app has (android.c, the window manager's overscan).
 * They are above the layer (touches there come with y < 0), so a pull that starts in them reaches no app: over an
 * app only that pull opens the drawer (one from the app's own top would also pull to refresh it, as Kiosk
 * Satellite does).  An app launched leaves our last frame (its picture under the drawer, or our own screen) up until
 * it is in front.
 *
 * Drawing: every frame is a display list (rounded boxes, outlines, arcs, polygons, gradients, text, images, each cut
 * to its scroll area), compared with the last one; what changed is redrawn inside one dirty rectangle
 * (ANativeWindow_lock takes one), on a buffer that keeps the rest.  The eyes blinking cost their own box, the clock
 * its digits, a still screen nothing.
 *
 * From hassmic, one message a line on stdin (screen.c writes them):
 *   p+ <pattern> <ms>, p- <pattern>    the core's patterns as the light ring has them (main.c led()), on (that long
 *                                  ago: to a new UI, what is on; the timed ones end by themselves) and off
 *   lv <0..100>                    the talker's (listening) or the reply's (speaking) level
 *   mute <0|1>, dnd <0|1>, oobe <0|1>, name <text>, tz <POSIX TZ>, act <text>
 *   h12 <0|1>                      (the platform's: android.c from the locale)
 *   ev <0..100>                    the Echo's volume, whenever it changes (the drawer's slider; the band shows the keys')
 *   timers <names>                 ringing timers ("" none);  peer <name>: the other side of a Drop In
 *   web <url>                      the settings page, for what to do where Music Assistant is not set up
 *   kiosk <package>                Kiosk Satellite is installed (the home app when none was chosen)
 *   mt <title>\t<artist>\t<album>  Sendspin metadata ("" for none);  mp <0 stopped|1 playing|2 paused> <ms> <dur ms> <speed x1000>
 *   mc <volume> <muted> <commands, comma separated> [<repeat off|one|all> <shuffle 0|1>]   the group's controller
 *   mx                             no Music Assistant player link any more
 *   home <package>                 the home app ("" = the face, "-" none chosen); after the first, a change applies at once
 *   art <bytes>                    (the platform's: an image follows, decoded into scene_art)
 *   ma <off|connecting|up|auth|error> <text>      Music Assistant's API: not set up, on its way, up (the server's
 *                                  name), the token refused, anything else (what)
 *   pl <id>\t<name>\t<flags>\t<state>\t<volume>\t<leader>\t<members>\t<playing>\t<group volume>   a player (flags: s
 *                                  this Echo, g may join this Echo's group, v has a volume, m muted, a available, l a
 *                                  group player; group volume: a leader's or group player's, its players' average);
 *                                  pl- <id>: gone;  pl.: every player told (after a reconnect)
 *   pq <state>\t<shuffle 0|1>\t<repeat off|one|all>\t<index>\t<items>\t<name>   this Echo's queue;  pq-: none;  qc: its items changed
 *   L <req> <offset> <total>, I <req> <kind>\t<uri>\t<title>\t<subtitle>\t<image>\t<seconds>\t<flags>, E <req> [<error>]
 *                                  a list asked for with "ml" (kinds: t track, a album, r artist, p playlist, o radio,
 *                                  c podcast, e episode, b audiobook, f folder, q queue item; flags: c current, x not
 *                                  available)
 *   merr <text>                    a command to Music Assistant failed: why (a toast)
 *   img <key> <bytes> <size>       (the platform's: an image asked for, JPEG or PNG, at that size; 0 bytes: none)
 *   ping <n>                       answered "pong <n>" (every 3 s; none within 10 s: the UI is taken for hung, restarted)
 *   wo <words 0|1> <timers 0|1> <hold s>     the settings "Speech on screen", "Timers on screen", "Speech stays for"
 *   wh <text>, wr <text>, ws <ms|-1>   what was said, the reply, and how long ago the satellite was idle again (-1: not
 *                                  yet); sent together, in that order, whenever one changes
 *   tl <n> <seq>\t<left ms> <total s> <active 0|1> <name>...   the running timers (the name: its length where it has none)
 * To hassmic, on stdout (screen.c acts on them):
 *   cmd <play|pause|next|previous|volume N|mute 0/1|repeat_off|repeat_one|repeat_all|shuffle|unshuffle|seek MS>
 *   dropin answer, dropin hangup, timer stop, vol <0..100>, dnd <0|1>, log <text>, pong <n>
 *   words clear                    the words tapped away (words.c): gone in hassmic too, a new UI gets none
 *   timers hide <seq>              the running timers of that "tl" tapped away: the screen not held on for them until
 *                                  they change
 *   app <package>\t<label>\t<system 0|1> ... apps.    every launchable app, as listed at start (the page's home app choice)
 *   ml <req> <what> <offset> [<arg>]   a list: artists albums playlists tracks radios podcasts audiobooks recent queue,
 *                                  album_tracks artist_albums artist_tracks playlist_tracks podcast_episodes <uri>,
 *                                  search <text>
 *   mplay <play|next|add|replace|radio> <uri>      on this Echo's queue
 *   mq <index N or ID|delete ID|clear|shuffle 0|1|repeat off|one|all>   (ID: a queue item's)
 *   mg add <player>, mg remove <player>, mg leave  this Echo's group;  mv <player> <0..100>: a player's volume
 *   mgv <0..100>                   the group's volume;  mhere <player>: what that player plays, moved here
 *   img <key> <size>               (the platform's) an image of a list, at 80, 160, 256 or 512 px
 */
#ifndef UI_SCENE_H
#define UI_SCENE_H
#include "gfx.h"

#define UI_HOME_PKG "org.hassmic.home"

struct ui_text { uint8_t *m; int w, h, stride; };               /* a coverage mask, malloc'd */
struct ui_image { uint32_t *px; int w, h; unsigned ver; };      /* opaque, the buffer's byte order, malloc'd */
struct ui_icon { uint32_t *px; int w, h; };                     /* premultiplied, the buffer's byte order */
struct ui_app { char pkg[96], cls[160], label[64]; struct ui_icon icon; int system; };   /* system: on the firmware's partition */

struct ui_platform {
    /* text in the system font, px tall (the line's height), bold or not, cut to maxw with an ellipsis: 0 ok */
    int  (*text)(void *ctx, const char *s, int px, int bold, int maxw, struct ui_text *out);
    void (*launch)(void *ctx, const char *pkg, const char *cls);   /* pkg NULL: hassmic's home activity */
    void (*back)(void *ctx);                                       /* Back to the app in front */
    void (*send)(void *ctx, const char *line);                     /* a line to hassmic */
    void (*apps)(void *ctx);                                       /* list the apps again: scene_apps follows */
    /* an image of a list (scene.h "img") at px x px, cut square from its middle: scene_image follows, maybe much later */
    void (*image)(void *ctx, const char *key, int px);
    void (*screenshot)(void *ctx);                                 /* what shows now: scene_backdrop follows, or not */
    void (*sleep)(void *ctx);                                      /* the screen off */
    /* the width text would have, uncut (as text draws it; NULL: text's own width stands in, by drawing it) */
    int  (*measure)(void *ctx, const char *s, int px, int bold);
};

enum { TOUCH_DOWN, TOUCH_MOVE, TOUCH_UP };

struct scene;
/* w x h: the layer (the screen above the band); band: the rows under it, which touches may reach too */
struct scene *scene_new(int w, int h, int band, const struct ui_platform *p, void *ctx, long long now);
void scene_resize(struct scene *s, int w, int h, int band);
void scene_msg(struct scene *s, const char *line, long long now);
void scene_art(struct scene *s, uint32_t *px, int w, int h);    /* takes the pixels (malloc'd, opaque); NULL: none */
void scene_image(struct scene *s, const char *key, int px, uint32_t *pixels, int w, int h);   /* takes them; NULL: failed */
void scene_backdrop(struct scene *s, uint32_t *px, int w, int h);   /* takes them: the app under the drawer */
void scene_apps(struct scene *s, struct ui_app *apps, int n);   /* takes the list (malloc'd, icons too) */
void scene_top(struct scene *s, const char *pkg, long long now);  /* the activity in front ("" unknown) */
void scene_screen(struct scene *s, int on, long long now);      /* the display is on: off, nothing is drawn */
void scene_touch(struct scene *s, int ev, int x, int y, long long now);  /* the layer's coordinates; y >= h: the band, y < 0: the grip */
int  scene_waiting(const struct scene *s);                     /* an activity asked for and not in front yet: look often */
void scene_launch_failed(struct scene *s, const char *pkg, long long now);   /* the platform could not start it */
/* the drawer is out (pulled or open): the grip above the layer is its top then, in its colour (SURF) and without its
 * handle, which the drawer has at its bottom */
int  scene_drawer(const struct scene *s);
int  scene_visible(const struct scene *s);
/* The next frame: the rectangle to redraw (empty: nothing changed), and in *next_ms when to ask again (-1: only
 * after an event).  Then scene_draw into a buffer whose clip is that rectangle, or a larger one if the buffer did
 * not keep the last frame. */
struct rect scene_prepare(struct scene *s, long long now, int *next_ms);
void scene_draw(struct scene *s, struct gfx *g);
void scene_invalidate(struct scene *s);                        /* everything again (a new buffer) */
/* The words and timers (words.c): a layer the size of the scene's, see-through, over the scene and every app, under
 * the band; hidden while the drawer is out.  0: hidden, 1: shown as last drawn, 2: shown, drawn anew first by
 * scene_words_draw into the whole buffer (RGBA_8888, premultiplied); *alpha the layer's opacity now (fades, a ringing
 * timer's pulse); *next_ms when to ask again (-1: only after a message).  Call after scene_prepare */
int  scene_words(struct scene *s, long long now, float *alpha, int *next_ms);
void scene_words_draw(struct scene *s, struct gfx *g);
void scene_words_lost(struct scene *s);                         /* not drawn after all (no buffer): drawn the next time */
/* Over an app the panel is Android's: the words take its touches only while the platform grabs it for them.
 * scene_words_grab: it can (0), it holds it (1), or it cannot (-1: a ring over an app then brings our screen up for its
 * Stop instead); scene_words_wants_grab: they cover an app now (taken and given back between touches only) */
void scene_words_grab(struct scene *s, int state);
int  scene_words_wants_grab(const struct scene *s);
void scene_free(struct scene *s);
#endif
