/*
 * Cards on the Echo Show's screen (cards.h): the card Home Assistant pushes (ESPHome action show_card, a JSON string),
 * checked and digested here into lines for the screen UI (screen.c screen_card, the UI's card.c), kept for as long as
 * it may show.  The format is docs/cards.md's ("The Echo's card"); what people write is Home Assistant's side,
 * hassmic_cards, which turns dashboard cards into these.  Everything shown is Home Assistant's: the words of every
 * row, the state of every switch.  Home Assistant sends the card again (same id, "update") when something on it
 * changed; this Echo composes no text of its own and asks for no states.
 *
 * Two places a card can be in:
 *   notice    from an automation (the doorbell, the laundry): shows from when it came, "timeout" s (0: until tapped
 *             away or cleared).  A voice request hides it while it runs and while its own card shows; it comes back after.
 *             Up to NOTICES at once, the newest shown: one under another comes back when the newer goes.
 *   request   a voice request's (scope "request"): taken while a request runs on this Echo or REQUEST_GRACE_MS after,
 *             only for that request's conversation where the card names one; stays while the Echo answers and
 *             "timeout" s after it is idle; the next request takes it away.  Home Assistant's conversation id lasts a
 *             few minutes across requests, so it narrows, not identifies: hassmic_cards drops a turn's card itself
 *             once the next request in that conversation began.
 *
 * Controls: a row has one only when the card asks ("control"; read-only otherwise), and never for a lock or an alarm
 * panel, whatever the card says: a screen anyone walks past opens no door.  A tap runs the control's Home Assistant
 * action (esphome_action, which needs "Allow the device to perform Home Assistant actions") and shows its result at
 * once (a switch flips, a slider moves) until Home Assistant's update says how it went.  A button runs nothing here:
 * its press is the event esphome.hassmic_card_action {card, action} (events need no permission; Home Assistant adds
 * the device), and hassmic_cards or an automation decides what it does.
 *
 * Home Assistant is told what the screen shows: the event esphome.hassmic_card_state {card, title, state shown|gone}
 * on every change (and again when it subscribes to our actions), from the card thread (never under lk: the event takes
 * core_lock, which comes before lk); a card refused is the event esphome.hassmic_card_refused (proto_esphome.c).
 *
 * A message's picture: the UI asks for it ("img card<seq>.<n> <size>" after the card's "ci" line) and gets the bytes
 * fetched here from the card's URL (esphome_fetch, on a thread of its own: a slow camera never holds the lock), decoded
 * there at its own shape.  With image_refresh, n counts up every so many seconds and the UI asks again; it keeps the
 * last picture up until the next one decoded, so a doorbell's view moves without blanking.  An update keeps it.
 */
#include "cards.h"
#include "core.h"
#include "json.h"
#include "ring.h"
#include "screen.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define CARD_ROWS   8
#define CARD_DAYS   5
#define CARD_ITEMS  12                 /* a list's rows: two columns of 6 on the Show 5's 470 px still read */
#define CARD_BUTTONS 3
#define REQUEST_TIMEOUT 15
#define NOTICE_TIMEOUT  30
#define REQUEST_GRACE_MS 10000         /* a request's card after it: Home Assistant sends it with the answer, which may
                                        * end the run before the card is here (the reply is short, the LLM's turn long) */
#define HOLD_MAX_MS (10 * 60000)       /* a card keeps the screen on this long at most (one until tapped away, one with a long
                                        * timeout), then the screen may sleep (the card is still there when it wakes) */
#define IMAGE_MAX   (3 << 20)          /* a camera's full-size JPEG is ~0.3-1 MB (a doorbell's 1600x1200: ~350 kB) */
#define IMAGE_FAILS 3                  /* fetches in a row that failed: its refreshes stop (a card Home Assistant forgot) */

enum { K_ENTITIES = 'e', K_WEATHER = 'w', K_MESSAGE = 'm', K_LIST = 'l' };
/* controls (the UI's card.c draws and taps them by these letters) */
enum { C_READ = 'r', C_TOGGLE = 't', C_DIM = 's', C_COVER = 'c', C_HEAT = 'h', C_PLAY = 'p', C_RUN = 'b' };
enum { NOTICE, REQUEST };             /* a card's scope */
#define NOTICES 4                      /* notices kept at once: the newest shows, the one before it when it goes (the
                                        * doorbell over "the washer is done", which is back after) */
#define RQ 0                           /* sl[RQ]: the request's card; sl[1..NOTICES]: notices */
#define NSLOT (1 + NOTICES)

struct row {
    char entity[96], name[64], domain[24], value[64];
    int ctl, focus, on, level;                 /* on: -1 none; level: brightness or position 0..100, -1 none */
    double setpoint, step;                     /* a thermostat's (NAN: none) */
};
struct day { char label[32], icon[24], temps[32], precip[16]; };
struct item { char text[96], detail[64]; };
struct button { char label[40], id[64]; int keep; };

struct card {
    int kind, scope, timeout_s, nrow, nday, nitem, nbtn;
    char id[64], conv[96], title[128], subtitle[256], message[600], w_icon[24], w_temp[32], w_detail[96];
    char image[1024]; int refresh_s;           /* a message's picture, and how often it is fetched again (0: once) */
    struct row rows[CARD_ROWS];
    struct day days[CARD_DAYS];
    struct item items[CARD_ITEMS];
    struct button buttons[CARD_BUTTONS];
};

/* a place (lk): its card, whether it holds one, its seq (the UI's taps name it), when it goes (0: not by time), since */
struct slot { struct card c; int has, seq, img_n, img_fails; long long expires, since, img_next; };

static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static int on = 1;                             /* setting screen_cards */
static struct slot sl[NSLOT];
static int seq, told = -1;                     /* seq: the last one given; told: the slot the UI was told (-1 none) */
static int run_active; static char run_conv[96];   /* the satellite's request: running, and its conversation id */
static long long idle_at;                      /* when the last request ended */
static pthread_cond_t img_cv = PTHREAD_COND_INITIALIZER;
static char st_card[64], st_title[128]; static int st_shown, st_dirty;   /* what the screen shows, for Home Assistant (lk) */
static char img_want[40]; static int img_size; /* the UI's newest ask (lk): one fetch at a time, the newest wins */

static long long mono_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }

/* free text on one line of the UI's: no tabs, no line ends (cut at a character boundary) */
static void cut(char *out, size_t n, const char *s)
{
    size_t k = 0;
    if (!s) s = "";
    for (; s[k] && k + 1 < n; k++) out[k] = s[k] == '\t' || s[k] == '\n' || s[k] == '\r' ? ' ' : s[k];
    if (s[k]) while (k && ((unsigned char)s[k] & 0xC0) == 0x80) k--;   /* cut inside a character: back to its start */
    out[k] = 0;
}

/* an entity id as Home Assistant makes them: [a-z0-9_]+ "." [a-z0-9_]+ */
static int entity_ok(const char *e)
{
    const char *dot = strchr(e, '.');
    if (!dot || dot == e || !dot[1] || strchr(dot + 1, '.')) return 0;
    for (const char *p = e; *p; p++) if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '.')) return 0;
    return 1;
}

static int never_controlled(const char *domain) { return !strcmp(domain, "lock") || !strcmp(domain, "alarm_control_panel"); }

/* a switch only where <domain>.toggle switches something on and off; never a cover's or a valve's (a garage door, the
 * water main: open/close are their controls, and which covers may have them is Home Assistant's to say) */
static int toggles(const char *d)
{
    static const char *const ok[] = { "light", "switch", "input_boolean", "fan", "automation", "humidifier", "siren", "media_player" };
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) if (!strcmp(d, ok[i])) return 1;
    return 0;
}

static int control_of(const char *want, const char *domain)
{
    if (never_controlled(domain)) return C_READ;
    if (!strcmp(want, "toggle") && toggles(domain)) return C_TOGGLE;
    if (!strcmp(want, "dim") && !strcmp(domain, "light")) return C_DIM;
    if (!strcmp(want, "cover") && !strcmp(domain, "cover")) return C_COVER;
    if (!strcmp(want, "climate") && !strcmp(domain, "climate")) return C_HEAT;
    if (!strcmp(want, "play") && !strcmp(domain, "media_player")) return C_PLAY;
    if (!strcmp(want, "run") && (!strcmp(domain, "scene") || !strcmp(domain, "script") || !strcmp(domain, "button"))) return C_RUN;
    return C_READ;
}

/* the slot the screen shows: while a request runs, its card (or none); after it, the newest of its card and the
 * notices: a doorbell's notice that comes while the request's card stays on (15 s, or until tapped) shows at once,
 * and the request's card is back under it, as a notice under a newer one is (seq: given when shown, kept by updates) */
static int showing(void)
{
    int best = -1;
    for (int s = 1; s < NSLOT; s++) if (sl[s].has && (best < 0 || sl[s].seq > sl[best].seq)) best = s;
    if (sl[RQ].has && (run_active || best < 0 || sl[RQ].seq > sl[best].seq)) return RQ;
    return run_active ? -1 : best;
}

/* ---------------------------------------------------------------- what the UI is told (scene.h) */

static void hold(void)                          /* lk held: whether the shown card keeps the screen on (told on a change) */
{
    static int held = -1;
    int s = showing(), h = s >= 0 && mono_ms() - sl[s].since < HOLD_MAX_MS;   /* (a long timeout as well: no screen lit for
                                                                   an hour by a card nobody looks at) */
    if (h != held) { held = h; screen_card_hold(h); }
}

static void tell(void)                          /* lk held */
{
    int s = showing();
    told = s;
    hold();
    {   /* what shows, for Home Assistant's event (sent by the card thread) */
        const char *id = s >= 0 ? sl[s].c.id : "", *title = s >= 0 ? sl[s].c.title : "";
        if ((s >= 0) != st_shown || strcmp(id, st_card) || strcmp(title, st_title)) {
            st_shown = s >= 0; snprintf(st_card, sizeof st_card, "%s", id); snprintf(st_title, sizeof st_title, "%s", title); st_dirty = 1;
        }
    }
    if (s < 0) { screen_card("cx\n"); return; }
    const struct card *c = &sl[s].c;
    static char b[8192]; size_t n = 0;
#define ADD(...) do { if (n < sizeof b) { int w_ = snprintf(b + n, sizeof b - n, __VA_ARGS__); n += w_ > 0 ? (size_t)w_ : 0; } } while (0)
    ADD("cb %d %c %d %d\t%s\t%s\n", sl[s].seq, c->kind, c->kind == K_ENTITIES ? c->nrow : c->kind == K_WEATHER ? c->nday : c->kind == K_LIST ? c->nitem : 0,
        c->scope == REQUEST, c->title, c->subtitle);
    if (c->kind == K_ENTITIES)
        for (int i = 0; i < c->nrow; i++) {
            const struct row *r = &c->rows[i];
            ADD("cr %d %c %d %d %d\t%s\t%s\n", i, r->ctl, r->ctl == C_READ || r->ctl == C_RUN || r->ctl == C_HEAT ? -1 : r->on, r->level, r->focus, r->name, r->value);
        }
    if (c->kind == K_WEATHER) {
        ADD("cw %s\t%s\t%s\n", c->w_icon[0] ? c->w_icon : "-", c->w_temp, c->w_detail);
        for (int i = 0; i < c->nday; i++) ADD("cf %d %s\t%s\t%s\t%s\n", i, c->days[i].icon[0] ? c->days[i].icon : "-", c->days[i].label, c->days[i].temps, c->days[i].precip);
    }
    if (c->kind == K_MESSAGE) ADD("cm %s\n", c->message);
    if (c->kind == K_MESSAGE && c->image[0]) ADD("ci card%d.%d\n", sl[s].seq, sl[s].img_n);
    if (c->kind == K_LIST) for (int i = 0; i < c->nitem; i++) ADD("cl %d\t%s\t%s\n", i, c->items[i].text, c->items[i].detail);
    for (int i = 0; i < c->nbtn; i++) ADD("cu %d\t%s\n", i, c->buttons[i].label);
#undef ADD
    if (n + 4 > sizeof b) {                                     /* (the limits above keep a card far below this) */
        n = sizeof b - 4; while (n && b[n - 1] != '\n') n--;
    }
    memcpy(b + n, "ce\n", 4);
    screen_card(b);
}

static void drop(int s, const char *why)        /* lk held */
{
    if (!sl[s].has) return;
    sl[s].has = 0;
    fprintf(stderr, "card: %s \"%s\" gone (%s)\n", s == RQ ? "request's" : "notice", sl[s].c.title, why);
    if (told == s || showing() != told) tell();
}

/* ---------------------------------------------------------------- from Home Assistant */

static void str_of(const struct json *j, int obj, const char *key, char *out, size_t n)
{
    char t[1100]; json_str(j, key ? json_get(j, obj, key) : obj, t, sizeof t); cut(out, n, t);
}

static int parse(const char *js, struct card *c, int *update, char *err, size_t errn)
{
    struct json j = { 0 }; char t[96]; int rc = -1, root = 0;  /* (json_parse grows what it holds: zeroed first) */
    if (!js || json_parse(&j, js, strlen(js)) || json_type(&j, root) != J_OBJ) { snprintf(err, errn, "not a JSON object"); goto out; }
    long long v = json_int(&j, json_get(&j, root, "v"), 0);
    if (v != CARDS_VERSION) { snprintf(err, errn, "card version %lld: this Echo takes %d (hassmic_cards and this Echo out of step?)", v, CARDS_VERSION); goto out; }
    str_of(&j, root, "id", c->id, sizeof c->id);
    *update = json_bool(&j, json_get(&j, root, "update"), 0);
    if (*update && !c->id[0]) { snprintf(err, errn, "an update needs the card's id"); goto out; }
    json_str(&j, json_get(&j, root, "scope"), t, sizeof t);
    c->scope = !strcmp(t, "request") ? REQUEST : NOTICE;
    if (t[0] && strcmp(t, "request") && strcmp(t, "notice")) { snprintf(err, errn, "scope \"%s\": notice or request", t); goto out; }
    str_of(&j, root, "conversation_id", c->conv, sizeof c->conv);
    c->timeout_s = (int)json_int(&j, json_get(&j, root, "timeout"), c->scope == REQUEST ? REQUEST_TIMEOUT : NOTICE_TIMEOUT);
    if (c->timeout_s < 0) c->timeout_s = c->scope == REQUEST ? REQUEST_TIMEOUT : NOTICE_TIMEOUT;   /* (0 means until tapped away) */
    else if (c->timeout_s > 86400) c->timeout_s = 86400;
    str_of(&j, root, "title", c->title, sizeof c->title); str_of(&j, root, "subtitle", c->subtitle, sizeof c->subtitle);
    str_of(&j, root, "message", c->message, sizeof c->message);
    json_str(&j, json_get(&j, root, "type"), t, sizeof t);
    c->kind = !strcmp(t, "entities") ? K_ENTITIES : !strcmp(t, "weather") ? K_WEATHER : !strcmp(t, "message") ? K_MESSAGE : !strcmp(t, "list") ? K_LIST : 0;
    if (!c->kind) {                                            /* a kind of a newer Home Assistant side: its words, at least */
        if (!c->title[0] && !c->message[0]) { snprintf(err, errn, "type \"%s\" unknown, and no title or message to show instead", t); goto out; }
        c->kind = K_MESSAGE;
    }
    if (c->kind == K_ENTITIES) {
        JSON_EACH(&j, json_get(&j, root, "entities"), e) {
            if (c->nrow >= CARD_ROWS) break;
            struct row *r = &c->rows[c->nrow]; memset(r, 0, sizeof *r);
            str_of(&j, e, "entity_id", r->entity, sizeof r->entity);
            if (!entity_ok(r->entity)) continue;
            size_t dl = (size_t)(strchr(r->entity, '.') - r->entity);
            if (dl >= sizeof r->domain) continue;
            memcpy(r->domain, r->entity, dl); r->domain[dl] = 0;
            str_of(&j, e, "name", r->name, sizeof r->name);
            str_of(&j, e, "value", r->value, sizeof r->value);
            char want[12]; str_of(&j, e, "control", want, sizeof want);
            r->ctl = control_of(want, r->domain);
            r->on = json_type(&j, json_get(&j, e, "on")) ? json_bool(&j, json_get(&j, e, "on"), 0) : -1;
            int lv = json_get(&j, e, "level");
            r->level = json_type(&j, lv) == J_NUM ? (int)json_int(&j, lv, -1) : -1;
            if (r->level > 100) r->level = 100; else if (r->level < -1) r->level = -1;
            r->setpoint = json_num(&j, json_get(&j, e, "setpoint"), NAN); r->step = json_num(&j, json_get(&j, e, "step"), NAN);
            if (r->ctl == C_HEAT && (isnan(r->setpoint) || isnan(r->step) || r->step <= 0 || r->step > 10)) r->ctl = C_READ;
            if ((r->ctl == C_TOGGLE || r->ctl == C_DIM || r->ctl == C_PLAY || r->ctl == C_COVER) && r->on < 0) r->on = 0;
            r->focus = json_bool(&j, json_get(&j, e, "focus"), 0);
            c->nrow++;
        }
        if (!c->nrow) { snprintf(err, errn, "entities: none with an entity id"); goto out; }
    } else if (c->kind == K_WEATHER) {
        int w = json_get(&j, root, "weather");
        str_of(&j, w, "condition", c->w_icon, sizeof c->w_icon); str_of(&j, w, "temperature", c->w_temp, sizeof c->w_temp);
        str_of(&j, w, "detail", c->w_detail, sizeof c->w_detail);
        JSON_EACH(&j, json_get(&j, w, "forecast"), d) {
            if (c->nday >= CARD_DAYS) break;
            struct day *y = &c->days[c->nday++];
            str_of(&j, d, "label", y->label, sizeof y->label); str_of(&j, d, "condition", y->icon, sizeof y->icon);
            str_of(&j, d, "temps", y->temps, sizeof y->temps); str_of(&j, d, "precip", y->precip, sizeof y->precip);
        }
        if (!c->w_temp[0] && !c->w_icon[0]) { snprintf(err, errn, "weather: no condition or temperature"); goto out; }
    } else if (c->kind == K_MESSAGE) {
        char im[1100]; json_str(&j, json_get(&j, root, "image"), im, sizeof im);
        if (im[0] && strncmp(im, "http://", 7) && strncmp(im, "https://", 8)) { snprintf(err, errn, "image: http:// or https:// only"); goto out; }
        if (strlen(im) >= sizeof c->image || strpbrk(im, " \t\r\n")) { snprintf(err, errn, "image: not a URL this Echo takes"); goto out; }
        memcpy(c->image, im, strlen(im) + 1);                  /* (fits: checked above) */
        long long r = json_int(&j, json_get(&j, root, "image_refresh"), 0);
        c->refresh_s = !c->image[0] || r <= 0 ? 0 : r > 300 ? 300 : (int)r;
        if (!c->message[0] && !c->title[0] && !c->image[0]) { snprintf(err, errn, "message: no text"); goto out; }
    } else {
        JSON_EACH(&j, json_get(&j, root, "items"), it) {
            if (c->nitem >= CARD_ITEMS) break;
            struct item *x = &c->items[c->nitem];
            if (json_type(&j, it) == J_STR) { str_of(&j, it, NULL, x->text, sizeof x->text); x->detail[0] = 0; }
            else { str_of(&j, it, "text", x->text, sizeof x->text); str_of(&j, it, "detail", x->detail, sizeof x->detail); }
            if (x->text[0]) c->nitem++;
        }
        if (!c->nitem && !c->title[0]) { snprintf(err, errn, "list: no items"); goto out; }
    }
    JSON_EACH(&j, json_get(&j, root, "buttons"), b) {
        if (c->nbtn >= CARD_BUTTONS) break;
        struct button *x = &c->buttons[c->nbtn];
        str_of(&j, b, "label", x->label, sizeof x->label); str_of(&j, b, "id", x->id, sizeof x->id);
        if (!x->label[0] || !x->id[0]) { snprintf(err, errn, "button %d: a label and an id", c->nbtn); goto out; }
        x->keep = json_bool(&j, json_get(&j, b, "keep"), 0);
        c->nbtn++;
    }
    rc = 0;
out:
    json_free(&j);
    return rc;
}

int cards_show(const char *js, char *err, size_t errn)
{
    if (!screen_present()) { snprintf(err, errn, "this Echo has no screen"); return -1; }
    struct card *c = calloc(1, sizeof *c);                       /* parsed apart: what shows stays as it is if this fails */
    int update = 0, rc = -1;
    if (!c) { snprintf(err, errn, "out of memory"); return -1; }
    if (parse(js, c, &update, err, errn)) goto out;
    pthread_mutex_lock(&lk);
    long long now = mono_ms();
    if (!on) { snprintf(err, errn, "cards are switched off on this Echo (screen_cards)"); goto unlock; }
    if (update) {                                               /* the same card again: in its place, its picture and time kept */
        int s = -1;
        for (int k = 0; k < NSLOT && s < 0; k++) if (sl[k].has && !strcmp(sl[k].c.id, c->id)) s = k;
        if (s < 0) { snprintf(err, errn, "card %s: not shown (gone)", c->id); goto unlock; }
        if (strcmp(sl[s].c.image, c->image)) { sl[s].img_n++; sl[s].img_fails = 0; }
        if (c->refresh_s != sl[s].c.refresh_s) sl[s].img_next = c->refresh_s ? now + c->refresh_s * 1000LL : 0;
        int scope = sl[s].c.scope; c->scope = scope;           /* (an update moves no card between places) */
        if (c->timeout_s != sl[s].c.timeout_s) sl[s].expires = !c->timeout_s || (scope == REQUEST && run_active) ? 0 : now + c->timeout_s * 1000LL;
        sl[s].c = *c;
        if (told == s) tell();
        rc = 0;
        goto unlock;
    }
    int s = RQ;
    if (c->scope == NOTICE) {                                   /* its own place again (the same id), else a free one,
                                                                   else the oldest notice's */
        s = -1;
        for (int k = 1; k < NSLOT && s < 0; k++) if (sl[k].has && c->id[0] && !strcmp(sl[k].c.id, c->id)) s = k;
        for (int k = 1; k < NSLOT && s < 0; k++) if (!sl[k].has) s = k;
        if (s < 0) { s = 1; for (int k = 2; k < NSLOT; k++) if (sl[k].seq < sl[s].seq) s = k; }
    } else {
        /* this Echo's request only: while it runs, or just after it (the card comes with the answer), and its
         * conversation where both are known */
        if (!run_active && (!idle_at || now - idle_at > REQUEST_GRACE_MS)) { snprintf(err, errn, "no request on this Echo"); goto unlock; }
        if (c->conv[0] && run_conv[0] && strcmp(c->conv, run_conv)) { snprintf(err, errn, "for another request"); goto unlock; }
    }
    struct slot *p = &sl[s];
    p->c = *c; p->has = 1; p->seq = ++seq; p->img_n = 0; p->img_fails = 0; p->since = now;
    p->expires = !c->timeout_s || (s == RQ && run_active) ? 0 : now + c->timeout_s * 1000LL;
    p->img_next = c->refresh_s ? now + c->refresh_s * 1000LL : 0;
    int n = c->kind == K_ENTITIES ? c->nrow : c->kind == K_LIST ? c->nitem : c->nday;
    fprintf(stderr, "card: %s %c \"%s\"%s, %d row%s, %s\n", s == RQ ? "request's" : "notice", c->kind, c->title,
            c->id[0] ? "" : " (no id)", n, n == 1 ? "" : "s", !c->timeout_s ? "until tapped away" : p->expires ? "timed" : "until idle + timeout");
    tell();
    if (s != RQ && told == s) ring_screen_wake();               /* a notice: the screen on to show it (a request's card
                                                                   comes while the Echo answers: on already) */
    rc = 0;
unlock:
    pthread_mutex_unlock(&lk);
out:
    free(c);
    return rc;
}

void cards_clear(const char *id, const char *why)
{
    pthread_mutex_lock(&lk);
    if (!id || !id[0]) { int s = showing(); if (s >= 0) drop(s, why); }   /* what shows */
    else for (int s = 0; s < NSLOT; s++) if (sl[s].has && !strcmp(sl[s].c.id, id)) drop(s, why);
    pthread_mutex_unlock(&lk);
}

void cards_run(int start)
{
    pthread_mutex_lock(&lk);
    long long now = mono_ms();
    if (start && run_active) { pthread_mutex_unlock(&lk); return; }   /* a follow-up of the same conversation: its card stays */
    if (start) {
        run_active = 1; run_conv[0] = 0;
        if (sl[RQ].has) { sl[RQ].has = 0; fprintf(stderr, "card: request's \"%s\" gone (a new request)\n", sl[RQ].c.title); }
        if (told >= 0) tell();                                  /* a notice hides while the request runs */
    } else if (run_active) {                                    /* (idle after an announcement or media: no request ended) */
        run_active = 0; idle_at = now;
        if (sl[RQ].has && !sl[RQ].expires && sl[RQ].c.timeout_s) sl[RQ].expires = now + sl[RQ].c.timeout_s * 1000LL;
        if (showing() != told) tell();                          /* the notice back, if there is no request's card */
    }
    pthread_mutex_unlock(&lk);
}

void cards_conversation(const char *id)
{
    pthread_mutex_lock(&lk);
    cut(run_conv, sizeof run_conv, id ? id : "");
    if (sl[RQ].has && sl[RQ].c.conv[0] && run_conv[0] && strcmp(sl[RQ].c.conv, run_conv)) drop(RQ, "its request was another");
    pthread_mutex_unlock(&lk);
}

/* ---------------------------------------------------------------- from the UI: taps (scene.h "card ...") */

void cards_ui(const char *line)
{
    int sq = -1, i = -1, n = 0; char op[16] = ""; long arg = 0;
    if (!strncmp(line, "card clear ", 11)) {
        pthread_mutex_lock(&lk);
        int want = atoi(line + 11);
        for (int s = 0; s < NSLOT; s++) if (sl[s].has && sl[s].seq == want && told == s) drop(s, "tapped away");
        pthread_mutex_unlock(&lk);
        return;
    }
    if (sscanf(line, "card tap %d %d %15s%n", &sq, &i, op, &n) < 3) return;
    if (line[n] == ' ') arg = strtol(line + n + 1, NULL, 10);
    pthread_mutex_lock(&lk);
    int s = told;
    if (s < 0 || !sl[s].has || sl[s].seq != sq) { pthread_mutex_unlock(&lk); return; }   /* (an old card's) */
    struct card *c = &sl[s].c;
    long long now = mono_ms();
    if (c->timeout_s && sl[s].expires) sl[s].expires = now + c->timeout_s * 1000LL;   /* someone is using it: it stays */
    if (!strcmp(op, "press")) {                                 /* a button: an event for Home Assistant */
        if (i < 0 || i >= c->nbtn) { pthread_mutex_unlock(&lk); return; }
        struct button b = c->buttons[i]; char id[sizeof c->id]; memcpy(id, c->id, sizeof id);
        pthread_mutex_unlock(&lk);
        const char *kv[4] = { "card", id, "action", b.id };
        pthread_mutex_lock(&core_lock);
        int r = esphome_event("esphome.hassmic_card_action", kv, 2);
        pthread_mutex_unlock(&core_lock);
        fprintf(stderr, "card: button \"%s\" (%s)%s\n", b.label, b.id, r ? ": no Home Assistant to tell, the card stays" : "");
        if (!r && !b.keep) {                                    /* told: the card has done its job here */
            pthread_mutex_lock(&lk);
            if (sl[s].has && sl[s].seq == sq) drop(s, "a button");
            pthread_mutex_unlock(&lk);
        }
        return;
    }
    if (i < 0 || i >= c->nrow) { pthread_mutex_unlock(&lk); return; }
    struct row *w = &c->rows[i];
    char v[24] = "", svc[64] = "", ent[sizeof w->entity];
    const char *key = NULL;
    /* the control's action, and its result shown at once (Home Assistant's update follows) */
    if (!strcmp(op, "toggle") && (w->ctl == C_TOGGLE || w->ctl == C_DIM)) {
        snprintf(svc, sizeof svc, "%s.toggle", w->domain); w->on = !w->on;
    } else if (!strcmp(op, "level") && w->ctl == C_DIM) {
        int l = arg < 1 ? 1 : arg > 100 ? 100 : (int)arg;
        snprintf(svc, sizeof svc, "light.turn_on"); key = "brightness_pct"; snprintf(v, sizeof v, "%d", l);
        w->level = l; w->on = 1;
    } else if (w->ctl == C_COVER && (!strcmp(op, "open") || !strcmp(op, "close") || !strcmp(op, "stop"))) {
        snprintf(svc, sizeof svc, "cover.%s_cover", op);
    } else if (w->ctl == C_HEAT && (!strcmp(op, "up") || !strcmp(op, "down"))) {
        double t = w->setpoint + (op[0] == 'u' ? w->step : -w->step);
        snprintf(svc, sizeof svc, "climate.set_temperature"); key = "temperature"; snprintf(v, sizeof v, "%.1f", t);
        w->setpoint = t;
    } else if (!strcmp(op, "play") && w->ctl == C_PLAY) {
        snprintf(svc, sizeof svc, "media_player.media_play_pause"); w->on = !w->on;
    } else if (!strcmp(op, "run") && w->ctl == C_RUN) {
        snprintf(svc, sizeof svc, "%s.%s", w->domain, !strcmp(w->domain, "button") ? "press" : "turn_on");
    } else { pthread_mutex_unlock(&lk); return; }
    memcpy(ent, w->entity, sizeof ent);
    tell();
    pthread_mutex_unlock(&lk);
    const char *kv[4] = { "entity_id", ent, key, v };
    pthread_mutex_lock(&core_lock);
    int r = esphome_action(svc, kv, key ? 2 : 1);
    pthread_mutex_unlock(&core_lock);
    fprintf(stderr, "card: %s %s%s%s%s%s\n", svc, ent, key ? " " : "", key ? key : "", key ? "=" : "", key ? v : "");
    if (r) fprintf(stderr, "card: no Home Assistant to run it\n");
}

/* ---------------------------------------------------------------- a message's picture */

void cards_image(const char *key, int size)
{
    int sq = -1, n = -1;
    if (sscanf(key, "card%d.%d", &sq, &n) != 2) return;
    pthread_mutex_lock(&lk);
    int s = told;
    if (s >= 0 && sl[s].has && sq == sl[s].seq && sl[s].c.kind == K_MESSAGE && sl[s].c.image[0]) { snprintf(img_want, sizeof img_want, "%s", key); img_size = size; pthread_cond_signal(&img_cv); }
    else { char head[80]; snprintf(head, sizeof head, "img %s 0 %d", key, size); screen_bin(head, NULL, 0); }   /* an old card's: none */
    pthread_mutex_unlock(&lk);
}

static void *image_loop(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&lk);
        while (!img_want[0]) pthread_cond_wait(&img_cv, &lk);
        char key[40], url[sizeof sl[0].c.image] = ""; int size = img_size, sq = -1, n = -1;
        snprintf(key, sizeof key, "%s", img_want); img_want[0] = 0;
        sscanf(key, "card%d.%d", &sq, &n);
        for (int s = 0; s < NSLOT; s++) if (sl[s].has && sl[s].seq == sq && sl[s].c.kind == K_MESSAGE) snprintf(url, sizeof url, "%s", sl[s].c.image);
        pthread_mutex_unlock(&lk);
        size_t len = 0; uint8_t *b = url[0] ? esphome_fetch(url, IMAGE_MAX, &len) : NULL;
        if (url[0] && !b) fprintf(stderr, "card: picture not fetched (%s)\n", key);
        pthread_mutex_lock(&lk);
        for (int s = 0; s < NSLOT; s++) if (sl[s].has && sl[s].seq == sq) sl[s].img_fails = b ? 0 : sl[s].img_fails + 1;
        pthread_mutex_unlock(&lk);
        char head[80]; snprintf(head, sizeof head, "img %s %zu %d", key, b ? len : 0, size);
        screen_bin(head, b, b ? len : 0);
        free(b);
    }
    return NULL;
}

/* ---------------------------------------------------------------- the setting, the thread */

int cards_on(int set)
{
    if (!screen_present()) return -1;
    pthread_mutex_lock(&lk);
    if (set >= 0 && (set != 0) != on) { on = set != 0; if (!on) for (int s = 0; s < NSLOT; s++) drop(s, "switched off"); }
    int r = on;
    pthread_mutex_unlock(&lk);
    return r;
}

static void tick(long long now)                  /* lk held: what time does to the cards */
{
    for (int s = 0; s < NSLOT; s++) if (sl[s].has && sl[s].expires && now >= sl[s].expires) drop(s, "its time is up");
    int s = told;
    /* its picture asked for again: while the card keeps the screen on (HOLD_MAX_MS at most), and not
     * after IMAGE_FAILS fetches in a row failed (Home Assistant forgot the card: its token ended) */
    if (s >= 0 && sl[s].c.refresh_s && sl[s].img_next && now >= sl[s].img_next && sl[s].img_fails < IMAGE_FAILS
        && now - sl[s].since < HOLD_MAX_MS) {
        sl[s].img_n++; sl[s].img_next = now + sl[s].c.refresh_s * 1000LL; tell();
    }
    hold();
}

/* the card thread: what the screen shows told to Home Assistant, outside lk (the event takes core_lock); tried again
 * while no Home Assistant is there to tell */
static void flush_state(void)
{
    char id[sizeof st_card], title[sizeof st_title]; int shown;
    pthread_mutex_lock(&lk);
    int d = st_dirty; st_dirty = 0;
    memcpy(id, st_card, sizeof id); memcpy(title, st_title, sizeof title); shown = st_shown;
    pthread_mutex_unlock(&lk);
    if (!d) return;
    const char *kv[6] = { "card", id, "title", title, "state", shown ? "shown" : "gone" };
    pthread_mutex_lock(&core_lock);
    int r = esphome_event("esphome.hassmic_card_state", kv, 3);
    pthread_mutex_unlock(&core_lock);
    if (r) { pthread_mutex_lock(&lk); if (!strcmp(id, st_card) && shown == st_shown) st_dirty = 1; pthread_mutex_unlock(&lk); }
}

void cards_resend_state(void) { pthread_mutex_lock(&lk); st_dirty = 1; pthread_mutex_unlock(&lk); }

static void *loop(void *arg)
{
    (void)arg;
    for (;;) {
        usleep(200000);
        pthread_mutex_lock(&lk);
        tick(mono_ms());
        pthread_mutex_unlock(&lk);
        flush_state();
    }
    return NULL;
}

void cards_start(void)
{
    pthread_t t;
    if (!screen_present()) return;
    if (!pthread_create(&t, NULL, loop, NULL)) pthread_detach(t);
    if (!pthread_create(&t, NULL, image_loop, NULL)) pthread_detach(t);
}
