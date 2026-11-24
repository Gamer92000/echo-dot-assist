/* A stand-in phone plugin for hassmic's own tests (tests/fake_plugins_call.py): what docs/plugins.md phase 5 gives a
 * plugin (`call`), used as a SIP plugin would, against the SDK's client, with no far side but itself.  Its
 * actions, from Home Assistant or its card:
 *   call <who>   (marked "calls") a call goes out with the action's token; the "far side" rings at once and takes it
 *                0.3 s later (hm_call_live)
 *   ring <who>   a call comes in (hm_call_incoming); answered here (call_answer), it goes live at once
 *   answer       (marked "calls") the ringing call answered from Home Assistant or the card with the action's token:
 *                hassmic refuses it (DENIED): only a person at the Echo answers a call coming in, no token does
 *   hang_up      hm_call_end
 *   try <what>   what it may not do, each tried and its answer said: live_early (hm_call_live while it rings, nobody
 *                answered), out_bare (hm_call_out without a token), out_made_up (a token of its own making), out_again
 *                (the last token it was given, again), out_old (a token kept 11 s), there_none (CALL_RINGING_THERE with
 *                no call: no id, nothing answered), end_none, live_none, ring_twice, keep (keeps the next "call"'s
 *                token instead of using it), quiet (the far side silent, or loud again), slow (answered here, it
 *                goes live 1.5 s later, at a tick; again: at once), crash (_exit mid-call)
 * While a call is live it answers every frame of the room with one of its own (a 440 Hz tone at -20 dBFS, at the
 * room's pace), and says once a second how many frames of the room it had, their level and their pitch by zero
 * crossings, for the test to read in hassmic's log ("plugin testcall: ...").  Its "line" entity says idle, ringing,
 * calling, live.
 * And a copy of the room's socket, as a plugin that wanted the room after the end would keep one: dup()ed as CALL_LIVE
 * hands it over, and after each end (CALL_HANGUP, its own CALL_END) read for 1.5 s: "kept copy after ...: N frames
 * later than 0.3 s, its end closed" (hassmic's end shut down: nothing more of the room, EOF), which the test reads. */
#include "hassmic_plugin.h"
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int live, ringing, out, keep_next, frames, zc, sec_frames, quiet, slow;
static long long answered_at;           /* slow: answered here then, live 1.5 s later (a far side that takes its time) */
static double energy, phase;
static char kept[33], last_tok[33];
static long long live_at;
static uint32_t tx_seq;
static int room_copy = -1;              /* a dup() of the room's socket, kept past the end */
static unsigned char open_before[256];

static long long mono_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static void line(const char *s) { hm_entity_state("line", s); hm_log("line %s", s); }

/* the descriptors open now, and after CALL_LIVE the new one (the SDK's copy of the room's socket): dup()ed */
static void fds_mark(void) { for (int fd = 0; fd < 256; fd++) open_before[fd] = fcntl(fd, F_GETFD) >= 0; }
static void copy_keep(void)
{
    for (int fd = 0; fd < 256; fd++) {
        int type = 0; socklen_t tl = sizeof type;
        if (open_before[fd] || fcntl(fd, F_GETFD) < 0 || getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tl) || type != SOCK_SEQPACKET) continue;
        if (room_copy >= 0) close(room_copy);
        room_copy = dup(fd);
        return;
    }
}
static void copy_check(const char *after)
{
    long long t0 = mono_ms(); int frames = 0, eof = 0; char b[700];
    if (room_copy < 0) return;
    while (!eof && mono_ms() - t0 < 1500) {
        struct pollfd pf = { room_copy, POLLIN, 0 };
        if (poll(&pf, 1, 100) <= 0) continue;
        long k = recv(room_copy, b, sizeof b, MSG_DONTWAIT);
        if (k == 0) eof = 1;
        else if (k > 0 && mono_ms() - t0 > 300) frames++;       /* (before: what hassmic sent ahead of the end) */
    }
    close(room_copy); room_copy = -1;
    hm_log("kept copy after %s: %d frames later than 0.3 s, %s", after, frames, eof ? "its end closed" : "still open");
}

static void go_live(const char *token)
{
    int why = 0, r;
    fds_mark();
    r = hm_call_live(token, &why);
    if (!r) copy_keep();
    hm_log("live: %d (%d)", r, why);
    if (!r) { live = 1; ringing = out = 0; frames = sec_frames = zc = 0; energy = 0; live_at = mono_ms(); line("live"); hm_event("result", "connected"); }
}

static void on_answer(void *ctx) { (void)ctx; hm_log("answered here"); if (ringing && slow) answered_at = mono_ms(); else if (ringing) go_live(""); }

static void on_hangup(void *ctx, int why)
{
    (void)ctx;
    hm_log("hung up by hassmic: %d after %d frames of the room", why, frames);
    live = ringing = out = 0; answered_at = 0; line("idle"); hm_event("result", "ended");
    copy_check("CALL_HANGUP");
}

/* the room: counted and measured; answered with a frame of the tone, number for number */
static void on_audio(void *ctx, uint32_t seq, const int16_t *pcm, int n)
{
    int16_t t[HM_CALL_FRAME];
    (void)ctx; (void)seq;
    if (!live) { hm_log("room audio while no call is live (%d samples)", n); return; }
    frames++; sec_frames++;
    for (int i = 0; i < n; i++) { energy += (double)pcm[i] * pcm[i]; if (i && (pcm[i - 1] < 0) != (pcm[i] < 0)) zc++; }
    if (sec_frames == 50) {
        double db = 10 * log10(energy / (50.0 * HM_CALL_FRAME) / (32768.0 * 32768.0) + 1e-12);
        hm_log("room: %d frames, %.1f dBFS, %d Hz", frames, db, zc / 2);
        sec_frames = 0; energy = 0; zc = 0;
    }
    for (int i = 0; i < HM_CALL_FRAME; i++) { t[i] = quiet ? 0 : (int16_t)(32768 * 0.1 * sqrt(2) * sin(phase)); phase += 2 * M_PI * 440 / 16000; if (phase > 2 * M_PI) phase -= 2 * M_PI; }
    if (hm_call_audio(tx_seq++, t)) hm_log("send full");
}

static void try_(const char *what)
{
    int why = 0, r = 0;
    if (!strcmp(what, "live_early")) r = hm_call_live("", &why);
    else if (!strcmp(what, "out_bare")) r = hm_call_out("", "Nobody", &why);
    else if (!strcmp(what, "out_made_up")) r = hm_call_out("0123456789abcdef0123456789abcdef", "Nobody", &why);
    else if (!strcmp(what, "out_again")) r = hm_call_out(last_tok, "Nobody", &why);
    else if (!strcmp(what, "out_old")) r = hm_call_out(kept, "Nobody", &why);
    else if (!strcmp(what, "there_none")) r = hm_call_ringing_there();
    else if (!strcmp(what, "end_none")) r = hm_call_end(HM_RESULT_HUNG_UP);
    else if (!strcmp(what, "live_none")) r = hm_call_live("", &why);
    else if (!strcmp(what, "ring_twice")) r = hm_call_incoming("Second", &why);
    else if (!strcmp(what, "keep")) { keep_next = 1; r = 0; }
    else if (!strcmp(what, "quiet")) { quiet = !quiet; r = quiet; }
    else if (!strcmp(what, "slow")) { slow = !slow; r = slow; }
    else if (!strcmp(what, "crash")) { hm_log("crashing with a call %s", live ? "live" : "not live"); _exit(3); }
    hm_log("try %s: %d (%d)", what, r, why);
}

static void on_action(void *ctx, const char *name, int from, const struct hm_arg *a, int n)
{
    const char *tok = hm_action_token(); int why = 0, r;
    (void)ctx;
    hm_log("action %s from %d, %s", name, from, tok[0] ? "with a token" : "no token");
    if (tok[0]) snprintf(last_tok, sizeof last_tok, "%s", tok);
    if (!strcmp(name, "call") && n) {
        if (keep_next) { keep_next = 0; snprintf(kept, sizeof kept, "%s", tok); hm_log("token kept"); return; }
        r = hm_call_out(tok, a[0].value, &why); hm_log("call out: %d (%d)", r, why);
        if (!r) { out = 1; line("calling"); hm_call_ringing_there(); }
        else hm_event("result", "refused");
    } else if (!strcmp(name, "ring") && n) {
        r = hm_call_incoming(a[0].value, &why); hm_log("ring: %d (%d)", r, why);
        if (!r) { ringing = 1; line("ringing"); } else hm_event("result", "refused");
    } else if (!strcmp(name, "answer")) { if (ringing) go_live(tok); else hm_log("answer: nothing rings"); }
    else if (!strcmp(name, "hang_up")) { r = hm_call_end(HM_RESULT_HUNG_UP); hm_log("hang up: %d after %d frames of the room", r, frames); live = ringing = out = 0; answered_at = 0; line("idle"); hm_event("result", "ended");
                                        copy_check("CALL_END"); }
    else if (!strcmp(name, "try") && n) try_(a[0].value);
}

static void on_tick(void *ctx)
{
    static int first = 1;
    (void)ctx;
    if (first) { first = 0; line("idle"); }
    if (out && !live) go_live("");                     /* the far side takes it on the next tick */
    if (answered_at && ringing && mono_ms() - answered_at >= 1500) { answered_at = 0; go_live(""); }
    if (live && !frames && mono_ms() - live_at > 3000) hm_log("no room audio in 3 s");
}

static void on_ready(void *ctx) { (void)ctx; hm_log("ready"); }

int main(void)
{
    static const struct hm_callbacks cb = { .ready = on_ready, .tick = on_tick, .action = on_action, .call_answer = on_answer, .call_hangup = on_hangup,
                                            .call_audio = on_audio };
    return hm_run(&cb, NULL) ? 1 : 0;
}
