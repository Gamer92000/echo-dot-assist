/* See ui.h: the face.  Two eyes say what the satellite does (the core's patterns, as the light ring shows them), a
 * mouth moves with the reply, the clock below; a ringing timer, a call or setup put words and buttons there instead,
 * and what Music Assistant plays sits there with its own play/pause while nothing else does. */
#include "ui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define POKE_MS 900

/* the core's patterns (ring.h, main.c led()), as ring_band.c has them: how long each shows, -1 until unset */
enum { P_RESET, P_LISTEN, P_THINK, P_TALK, P_END, P_CALL_OUT, P_CALL_IN, P_CALL, P_CALL_END, P_ERROR, P_MUTE, P_UNMUTE,
       P_DND, P_IDENTIFY, P_LOGIN, P_PAIRING, P_TIMER, P_TIMER_SHORT, P_SETUP, P_COUNT };
static const struct { const char *name; int ms; } pats[P_COUNT] = {
    [P_RESET] = { "factory-reset", -1 }, [P_LISTEN] = { "active-start", -1 }, [P_THINK] = { "active-thinking", -1 },
    [P_TALK] = { "active-talking", -1 }, [P_END] = { "active-end", 600 }, [P_CALL_OUT] = { "call_outbound_ringing", -1 },
    [P_CALL_IN] = { "call_incoming", -1 }, [P_CALL] = { "call_connected", -1 }, [P_CALL_END] = { "call_connected_end", 1480 },
    [P_ERROR] = { "anim_start_error_short", 3300 }, [P_MUTE] = { "mics-off_on", -1 }, [P_UNMUTE] = { "mics-off_end", 1100 },
    [P_DND] = { "do_not_disturb", 1760 }, [P_IDENTIFY] = { "zzz_rainbow", -1 }, [P_LOGIN] = { "authenticated_setup_mode", -1 },
    [P_PAIRING] = { "scone-setup", -1 }, [P_TIMER] = { "ready-timer", -1 }, [P_TIMER_SHORT] = { "ready-timer-short", 2233 },
    [P_SETUP] = { "setup-mode", -1 },
};

/* what the face shows, first wins */
enum { M_IDLE, M_RESET, M_CALL_IN, M_CALL_OUT, M_CALL, M_LISTEN, M_THINK, M_TALK, M_TIMER, M_ERROR, M_MUTE, M_DND,
       M_IDENTIFY, M_LOGIN, M_PAIRING, M_SETUP };

int pat_find(const char *name)
{
    for (int i = 0; i < P_COUNT; i++) if (eq(pats[i].name, name)) return i;
    return -1;
}

static int pat_on(struct scene *s, int p)
{
    if (!s->pat[p].on) return 0;
    if (pats[p].ms >= 0 && s->now >= s->pat[p].at + pats[p].ms) { s->pat[p].on = 0; return 0; }
    return 1;
}

/* a pattern on (ago ms ago: a new UI told what is on) or off; what asks for the satellite's attention now brings the
 * face back from the music, and closes the drawer that was pulled over our own screen */
void face_pattern(struct scene *s, int p, int on, long long ago)
{
    if (on) { s->pat[p].at = s->now - ago; s->pat[p].on = 1; if (p == P_LISTEN) s->level = 0; }
    else s->pat[p].on = 0;
    if (on && ago < 1000 && (p == P_LISTEN || p == P_CALL_IN || p == P_TIMER)) {
        if (s->view == V_MUSIC) s->view = V_FACE;
        if (drawer_open(s) && !s->dr.from_app) {
            int go = s->dr.go; drawer_close(s, 0); s->dr.go = go;   /* (a Home tapped: still) */
            if (s->edge == 1) s->edge = 0;                      /* a finger pulling it: no pull again with that finger */
        }
    }
}

int face_mode(struct scene *s)
{
    if (pat_on(s, P_RESET)) return M_RESET;
    if (pat_on(s, P_CALL_IN)) return M_CALL_IN;
    if (pat_on(s, P_CALL_OUT)) return M_CALL_OUT;
    if (pat_on(s, P_CALL)) return M_CALL;
    if (pat_on(s, P_LISTEN)) return M_LISTEN;
    if (pat_on(s, P_THINK)) return M_THINK;
    if (pat_on(s, P_TALK)) return M_TALK;
    if (pat_on(s, P_TIMER) || pat_on(s, P_TIMER_SHORT) || s->timers[0]) return M_TIMER;
    if (pat_on(s, P_ERROR)) return M_ERROR;
    if (s->mute || pat_on(s, P_MUTE)) return M_MUTE;
    if (s->dnd) return M_DND;
    if (pat_on(s, P_IDENTIFY)) return M_IDENTIFY;
    if (pat_on(s, P_LOGIN)) return M_LOGIN;
    if (pat_on(s, P_PAIRING)) return M_PAIRING;
    if (pat_on(s, P_SETUP) || s->oobe) return M_SETUP;
    return M_IDLE;
}

static void soon(long long *due, long long t, long long now) { if (t > now && (!*due || t < *due)) *due = t; }

/* the soonest something changes by itself: a timed pattern's end, a blink, a glance */
long long face_due(struct scene *s)
{
    long long due = 0;
    for (int i = 0; i < P_COUNT; i++) if (s->pat[i].on && pats[i].ms >= 0) soon(&due, s->pat[i].at + pats[i].ms, s->now);
    if (s->view == V_FACE) {
        int m = face_mode(s);
        if (m != M_MUTE && m != M_DND) soon(&due, s->face.blink_at, s->now);
        if (m == M_IDLE || m == M_SETUP) soon(&due, s->face.sacc_at, s->now);
    }
    return due;
}

static uint32_t hue(float h)
{
    h = (h - floorf(h)) * 6; float f = h - floorf(h); int q = (int)(f * 255);
    switch ((int)h) {
    case 0: return 0xFF0000u | (uint32_t)q << 8;
    case 1: return (uint32_t)(255 - q) << 16 | 0xFF00u;
    case 2: return 0x00FF00u | (uint32_t)q;
    case 3: return (uint32_t)(255 - q) << 8 | 0xFFu;
    case 4: return (uint32_t)q << 16 | 0xFFu;
    default: return 0xFF0000u | (uint32_t)(255 - q);
    }
}

static void clock_text(struct scene *s, char *out, size_t n)
{
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    if (s->h12) snprintf(out, n, "%d:%02d", tm.tm_hour % 12 ? tm.tm_hour % 12 : 12, tm.tm_min);
    else snprintf(out, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

/* the eyes and the mouth; returns whether anything still moves */
static int eyes(struct scene *s, int m, float dt)
{
    float w = s->w, h = s->h, t = (float)(s->now % 600000);
    float ew = 1, lh = 1, rh = 1, gx = 0, gy = 0, mouth = 0, k = 1; uint32_t col = 0xDCEBFF;
    int blinks = 1, moving = 0;
    float lv = s->level / 100.0f;
    switch (m) {
    case M_LISTEN: ew = 1.06f; lh = rh = 1.1f + 0.15f * lv; col = CYAN; break;
    case M_THINK: lh = 0.72f; gx = 0.035f + 0.03f * sinf(t * 6.2832f / 1600); gy = -0.07f; col = 0x8FDCFF; moving = 1; break;
    case M_TALK: lh = rh = 0.92f; mouth = 1; col = 0xD8F4FF; break;
    case M_TIMER: ew = 1.08f; lh = rh = 1.15f; gy = -0.02f + 0.025f * fabsf(sinf(t * 3.1416f / 400)); col = 0xFFA51E; moving = 1; break;
    case M_ERROR: lh = rh = 0.5f; gy = 0.03f; col = 0xFF4D4D; break;
    case M_MUTE: lh = rh = 0.07f; col = RED; k = 0.8f; blinks = 0; break;
    case M_DND: lh = rh = 0.07f; gy = 0.04f; col = VIOLET; k = 0.55f; blinks = 0; break;
    case M_CALL_IN: col = GREEN; gy = -0.02f + 0.02f * fabsf(sinf(t * 3.1416f / 650)); moving = 1; break;
    case M_CALL_OUT: col = GREEN; gx = 0.05f * sinf(t * 6.2832f / 1800); moving = 1; break;
    case M_CALL: col = GREEN; break;
    case M_RESET: lh = rh = 0.5f; col = ORANGE; break;
    case M_IDENTIFY: col = hue(t / 2000); moving = 1; break;
    case M_LOGIN: lh = rh = 0.9f + 0.1f * sinf(t * 6.2832f / 1800); col = BLUE; moving = 1; break;
    case M_PAIRING: col = BLUE; gx = 0.06f * sinf(t * 6.2832f / 2000); moving = 1; break;
    case M_SETUP: col = ORANGE; break;
    default: break;
    }
    if (m == M_IDLE || m == M_SETUP) {                          /* glancing about now and then */
        if (s->now >= s->face.sacc_at) {
            int centre = frand(s) < 0.35f;
            s->face.sgx = centre ? 0 : (frand(s) - 0.5f) * 0.14f; s->face.sgy = centre ? 0 : (frand(s) - 0.5f) * 0.1f;
            s->face.sacc_at = s->now + (m == M_SETUP ? 900 : 1800) + (long long)(frand(s) * (m == M_SETUP ? 1200 : 4200));
        }
        gx += s->face.sgx; gy += s->face.sgy;
    }
    if (s->face.poke_at && s->now - s->face.poke_at < POKE_MS) { /* touched: a squinting smile towards the finger */
        lh = rh = 0.5f; gy -= 0.02f; gx = s->face.pokex; moving = 1;
    }
    float bl = 1;
    if (blinks) {
        if (s->now >= s->face.blink_at + 180) s->face.blink_at = s->now + 2500 + (long long)(frand(s) * 4000);
        long long b = s->now - s->face.blink_at;
        if (b >= 0 && b < 180) { bl = b < 70 ? 1 - b / 70.0f : (b - 70) / 110.0f; bl = clampf(bl, 0.06f, 1); moving = 1; }
    }
    struct face *f = &s->face;
    f->ew.t = ew; f->lh.t = lh * bl; f->rh.t = rh * bl; f->gx.t = gx; f->gy.t = gy;
    f->cr.t = (col >> 16 & 255); f->cg.t = (col >> 8 & 255); f->cb.t = (col & 255); f->br.t = k;
    f->mw.t = mouth ? 0.7f + 0.6f * lv : 0.5f; f->mh.t = mouth ? 0.15f + lv : 0; f->ma.t = mouth;
    moving |= ease(&f->ew, dt, 90) | ease(&f->lh, dt, bl < 1 ? 20 : 70) | ease(&f->rh, dt, bl < 1 ? 20 : 70);
    moving |= ease(&f->gx, dt, 70) | ease(&f->gy, dt, 70) | ease(&f->br, dt, 200);
    moving |= ease(&f->cr, dt, 120) | ease(&f->cg, dt, 120) | ease(&f->cb, dt, 120);
    moving |= ease(&f->mw, dt, 60) | ease(&f->mh, dt, 45) | ease(&f->ma, dt, 90);
    uint32_t c = gfx_scale((uint32_t)clampf(f->cr.v, 0, 255) << 16 | (uint32_t)clampf(f->cg.v, 0, 255) << 8 | (uint32_t)clampf(f->cb.v, 0, 255), f->br.v);

    float EW = 0.135f * w, EH = 0.38f * h, cy = 0.43f * h + f->gy.v * h, sep = 0.165f * w, cx = w / 2 + f->gx.v * w;
    for (int e = 0; e < 2; e++) {
        float ex = cx + (e ? sep : -sep), ww = EW * f->ew.v, hh = fmaxf(EH * (e ? f->rh.v : f->lh.v), 0.025f * h);
        rrect(s, ID_FACE + 1 + e, ex - ww / 2, cy - hh / 2, ex + ww / 2, cy + hh / 2, fminf(0.4f * EW, hh / 2), c, 255);
    }
    if (f->ma.v > 0.01f) {
        float mw = 0.09f * w * f->mw.v, mh = fmaxf(0.012f * h, 0.11f * h * f->mh.v), my = 0.76f * h + f->gy.v * h;
        rrect(s, ID_FACE + 3, cx - mw / 2, my - mh / 2, cx + mw / 2, my + mh / 2, mh / 2, c, (int)(255 * clampf(f->ma.v, 0, 1)));
    }
    hit(s, cx - sep - EW, cy - EH, cx + sep + EW, cy + EH, A_POKE, 0);
    return moving;
}

static void pill(struct scene *s, int id, float cx, float cy, float w, const char *label, uint32_t col, int act)
{
    float h = U(60);
    int on = pressed(s, act, 0);
    rrect(s, id, cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2, h / 2, on ? gfx_scale(col, 0.7f) : col, 255);
    text(s, id + 1, label, U(28), 1, w - U(16), cx, cy - U(14), 1, 0x000000, 255);
    hit(s, cx - w / 2 - U(8), cy - h / 2 - U(8), cx + w / 2 + U(8), cy + h / 2 + U(8), act, 0);
}

static void caption(struct scene *s, int m, int *used)
{
    float w = s->w, h = s->h, y = 0.8f * h, px = U(30); char buf[300] = "";
    uint32_t c = FG;
    *used = 1;
    switch (m) {
    case M_TIMER:
        text(s, ID_FACE + 10, s->timers[0] ? s->timers : "Timer", px, 1, 0.5f * w, 0.3f * w, y - U(15), 1, c, 255);
        pill(s, ID_FACE + 60, 0.72f * w, y, 0.22f * w, "Stop", 0xFFA51E, A_TIMERSTOP);
        break;
    case M_CALL_IN:
        text(s, ID_FACE + 10, s->peer[0] ? s->peer : "Drop In", px, 1, 0.4f * w, 0.22f * w, y - U(15), 1, c, 255);
        pill(s, ID_FACE + 60, 0.55f * w, y, 0.2f * w, "Decline", RED, A_HANGUP);
        pill(s, ID_FACE + 63, 0.8f * w, y, 0.2f * w, "Answer", GREEN, A_ANSWER);
        break;
    case M_CALL_OUT: case M_CALL:
        snprintf(buf, sizeof buf, m == M_CALL_OUT ? "Calling %s" : "%s", s->peer[0] ? s->peer : "Drop In");
        text(s, ID_FACE + 10, buf, px, 1, 0.5f * w, 0.3f * w, y - U(15), 1, c, 255);
        pill(s, ID_FACE + 60, 0.75f * w, y, 0.2f * w, "End", RED, A_HANGUP);
        break;
    case M_MUTE: text(s, ID_FACE + 10, "Microphone off", px, 0, 0.8f * w, w / 2, y - U(15), 1, RED, 255); break;
    case M_DND: text(s, ID_FACE + 10, "Do not disturb", px, 0, 0.8f * w, w / 2, y - U(15), 1, gfx_scale(VIOLET, 0.8f), 255); break;
    case M_SETUP:
        snprintf(buf, sizeof buf, "Add %s in Home Assistant", s->name[0] ? s->name : "me");
        text(s, ID_FACE + 10, buf, px, 0, 0.9f * w, w / 2, y - U(15), 1, c, 255);
        break;
    case M_LOGIN:
        snprintf(buf, sizeof buf, "A browser wants in: press %s", s->act[0] ? s->act : "the action button");
        text(s, ID_FACE + 10, buf, px, 0, 0.92f * w, w / 2, y - U(15), 1, c, 255);
        break;
    case M_PAIRING: text(s, ID_FACE + 10, "Bluetooth pairing", px, 0, 0.8f * w, w / 2, y - U(15), 1, c, 255); break;
    case M_RESET: text(s, ID_FACE + 10, "Resetting", px, 0, 0.8f * w, w / 2, y - U(15), 1, c, 255); break;
    case M_IDENTIFY: text(s, ID_FACE + 10, s->name, px, 1, 0.8f * w, w / 2, y - U(15), 1, c, 255); break;
    default: *used = 0;
    }
}

/* the face's bottom line: the track playing, with its own play/pause; a tap anywhere else on it opens the music */
static void miniplayer(struct scene *s)
{
    float w = s->w, h = s->h, y0 = 0.79f * h, y1 = 0.95f * h, x0 = 0.22f * w, x1 = 0.78f * w, ph = y1 - y0;
    rrect(s, ID_FACE + 30, x0, y0, x1, y1, ph / 2, pressed(s, A_TOMUSIC, 0) ? 0x2A2E35 : DARK, 255);
    float a = ph - U(12);
    if (!image(s, ID_FACE + 31, &s->art, x0 + U(6), y0 + U(6), a, a, a / 2)) {
        rrect(s, ID_FACE + 31, x0 + U(6), y0 + U(6), x0 + U(6) + a, y0 + U(6) + a, a / 2, 0x30343B, 255);
        icon(s, ID_FACE + 32, IC_NOTE, x0 + U(6) + a / 2, y0 + ph / 2, a * 0.32f, DIM, 0x30343B);
    }
    float tx = x0 + U(18) + a, tw = x1 - tx - ph - U(8);
    text(s, ID_FACE + 35, s->title[0] ? s->title : "Music Assistant", U(23), 1, tw, tx, y0 + U(8), 0, FG, 255);
    text(s, ID_FACE + 36, s->artist, U(19), 0, tw, tx, y0 + U(34), 0, DIM, 255);
    hit(s, x0, y0, x1 - ph, y1, A_TOMUSIC, 0);
    if (can(s, s->mstate == 1 ? "pause" : "play")) {
        float bx = x1 - ph / 2, by = (y0 + y1) / 2;
        rrect(s, ID_FACE + 37, bx - a / 2, by - a / 2, bx + a / 2, by + a / 2, a / 2, pressed(s, A_PLAYPAUSE, 0) ? 0xB0B4BA : FG, 255);
        icon(s, ID_FACE + 38, s->mstate == 1 ? IC_PAUSE : IC_PLAY, bx + (s->mstate == 1 ? 0 : a * 0.03f), by, a * 0.27f, DARK, FG);
        hit(s, x1 - ph, y0, x1, y1, A_PLAYPAUSE, 0);
    }
}

int face_view(struct scene *s, float dt)
{
    /* Full brightness whatever the room: the backlight dims as stock's does (screen_brightness_auto, main.sh
     * backlight), and dimming the face under it as well left it at half in lit rooms (issue #126) */
    int m = face_mode(s), used = 0, moving;
    char clk[16];
    fill(s, ID_FACE, (struct rect){ 0, 0, s->w, s->h }, BG);
    moving = eyes(s, m, dt);
    caption(s, m, &used);
    clock_text(s, clk, sizeof clk);
    if (!used && s->music && (s->title[0] || s->mstate)) miniplayer(s), used = 1;
    if (used) text(s, ID_FACE + 20, clk, U(33), 0, s->w, s->w - U(28), U(14), 2, DIM, 255);
    else text(s, ID_FACE + 20, clk, U(47), 0, s->w, s->w / 2.0f, 0.83f * s->h, 1, DIM, 255);
    if (s->face.poke_at && s->now - s->face.poke_at < POKE_MS + 400) moving = 1;
    return moving;
}

int face_act(struct scene *s, int a, int arg, int ev)
{
    (void)arg;
    if (ev != EV_TAP) return 0;
    switch (a) {
    case A_POKE: s->face.poke_at = s->now; s->face.pokex = clampf((s->x - s->w / 2.0f) / s->w * 0.2f, -0.08f, 0.08f); break;
    case A_PLAYPAUSE: music_toggle(s); break;
    case A_TOMUSIC: music_show(s, 0); break;
    case A_ANSWER: send(s, "dropin %s", "answer"); break;
    case A_HANGUP: send(s, "dropin %s", "hangup"); break;
    case A_TIMERSTOP: send(s, "timer %s", "stop"); break;
    default: return 0;
    }
    return 1;
}

void face_init(struct scene *s, long long now)
{
    struct face *f = &s->face;
    f->blink_at = now + 2000; f->sacc_at = now + 1500;
    f->ew.v = f->lh.v = f->rh.v = f->br.v = 1; f->mw.v = 0.5f;
    f->cr.v = 0xDC; f->cg.v = 0xEB; f->cb.v = 0xFF;
}
