/* The SDK's C client (sdk/hmplugin.c) against a stand-in broker on descriptor 3, as a plugin runs it: what a review of
 * 08399dc and the syslog plugin's tests found (P1, P2, P4, N4, F2), each as it was seen.
 *   P1  what arrives while a request waits is kept whole, past the 32 it dropped silently (LOG lines lost, the cursor
 *       moved past them), and past the client's bound the plugin ends and says so: never dropped silently;
 *   P2  hm_store_put says -1 (BUSY) for a value it cannot keep, never 1 ("kept, written later");
 *   P4  a tick longer than a second does not starve the control socket: PINGs are answered;
 *   N4  hm_connect before READY (in the ready callback): -1, HM_ERR_BAD, nothing asked of hassmic;
 *   F2  hm_log_follow drops the LOG of the old place that comes before its answer.
 * And what a review of 0dabfa6 found (R2, R3, R4, R8, R9):
 *   R2  a STOP that comes while a request waits ends the wait: the stop callback runs, the plugin ends by itself;
 *   R3  what a request kept is handed on before anything newer: LOG in order, the cursor never backwards;
 *   R4  a write kept for later is written as the plugin ends, and one made in the stop callback goes at once;
 *   R8  LOG kept while hm_log_follow waits is handed on when hassmic refuses the new place (ERR keeps the old one);
 *   R9  every LOG answered OK (its id) once the log callback returned: hassmic's flow control. */
#define _GNU_SOURCE
#include "hassmic_plugin.h"
#include "plugmsg.h"
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s (line %d)\n", what, __LINE__); fails++; } else printf("ok   %s\n", what); } while (0)

static uint8_t b[PM_MAX_PACKET];
static void sendw(int s, struct pm_w *w) { if (send(s, w->b, (size_t)pm_wlen(w), 0) < 0) { } }
/* the next packet's type within MS (0 none); its id and body */
static int next(int s, int ms, struct pm_hdr *h, const uint8_t **body, size_t *bl)
{
    struct pollfd pf = { s, POLLIN, 0 }; long n;
    if (poll(&pf, 1, ms) != 1 || (n = recv(s, b, sizeof b, 0)) <= 0 || pm_head(b, (size_t)n, h, body, bl)) return 0;
    return h->type;
}
/* the child's stderr, all of it once it ended */
static char errs[1 << 16];
static void read_errs(int fd) { size_t n = 0; long k; while (n < sizeof errs - 1 && (k = read(fd, errs + n, sizeof errs - 1 - n)) > 0) n += (size_t)k; errs[n] = 0; }

/* the HELLO a client sent: 1 when it names every scope of this build once, at its version */
static int hello_whole(const uint8_t *body, size_t bl)
{
    struct pm_hello m; struct pm_hello_scopes e; int seen[PM_SCOPES] = { 0 }, n = 0;
    if (pm_get_hello(body, bl, &m)) return 0;
    while (pm_next_hello_scopes(&m.scopes, &e)) { int sc = pm_scope_of(e.name.s, e.name.n); if (sc < 0 || seen[sc]++ || e.version != pm_scope_version(sc)) return 0; n++; }
    return n == PM_SCOPES;
}
static int hellos_whole = 1;
/* a plugin with these callbacks in a child: S the broker's end, E its stderr; WELCOME (core and the scope of each
 * capability of CAPS at version 1, as a manifest of them requires; BUMP: one of them at a version past this build's)
 * and CONFIG already sent */
static pid_t plugin_v(const struct hm_callbacks *cb, pm_caps_t caps, int *s, int *e, int bump)
{
    int sv[2], ep[2]; pid_t p; struct pm_w w; struct pm_hdr h; const uint8_t *body; size_t bl; uint16_t n = 1;
    socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv); if (pipe(ep)) return -1;
    if (!(p = fork())) { dup2(sv[1], 3); dup2(ep[1], 2); close(sv[0]); close(ep[0]); alarm(60); _exit(hm_run(cb, NULL) ? 1 : 0); }
    close(sv[1]); close(ep[1]); *s = sv[0]; *e = ep[0];
    if (next(*s, 5000, &h, &body, &bl) != PM_HELLO || !hello_whole(body, bl)) hellos_whole = 0;
    for (int sc = 1; sc < PM_SCOPES; sc++) n += !!(caps & PM_CAPBIT(pm_scope_cap(sc)));
    struct pm_welcome wm = { 1, pm_s("pc"), pm_s("n"), pm_s("n"), pm_s("UTC"), { { 0 }, n } };
    pm_winit(&w, b, sizeof b); pm_put_welcome(&w, 0, &wm);
    for (int sc = 0; sc < PM_SCOPES; sc++)
        if (!sc || caps & PM_CAPBIT(pm_scope_cap(sc))) { struct pm_welcome_scopes x = { pm_s(pm_scope_name(sc)), (uint16_t)(bump && sc ? pm_scope_version(sc) + 1 : 1) }; pm_add_welcome_scopes(&w, &x); }
    sendw(*s, &w);
    struct pm_config cf = { { { 0 }, 0 } }; pm_winit(&w, b, sizeof b); pm_put_config(&w, 0, &cf); sendw(*s, &w);
    return p;
}
static pid_t plugin(const struct hm_callbacks *cb, pm_caps_t caps, int *s, int *e) { return plugin_v(cb, caps, s, e, 0); }
static void log_to(int s, int i)
{
    struct pm_w w; char cur[40], line[16]; struct pm_log m; struct pm_log_lines e;
    snprintf(cur, sizeof cur, "0000000000000000:%d", i + 1); snprintf(line, sizeof line, "L%d", i);
    m = (struct pm_log){ pm_s(cur), 0, { { 0 }, 1 } }; e = (struct pm_log_lines){ pm_s(line) };
    pm_winit(&w, b, sizeof b); pm_put_log(&w, 1000 + (uint32_t)i, &m); pm_add_log_lines(&w, &e); sendw(s, &w);
}
/* STOP sent, then whatever the plugin sends read (and counted by type into SEEN, PM_* indexes) until it ended: its
 * exit status, or -1 when it did not end by itself within MS */
static int stop_and_wait(int s, pid_t p, int ms, int *seen)
{
    struct pm_w w; struct pm_hdr h; const uint8_t *body; size_t bl; int st, t;
    struct timespec t0, t1;
    pm_winit(&w, b, sizeof b); pm_put_stop(&w, 0); sendw(s, &w);
    /* (MS by the clock: a count of 20 ms rounds ran faster than time wherever messages came, and under make -j16
     * unit an ASan plugin's exit after its stop callback took longer than R2's 1.5 s: a review of a30d6ed's flake) */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if ((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000 >= ms) break;
        while ((t = next(s, 20, &h, &body, &bl))) {
            if (seen && t < 64) seen[t]++;
            if (t == PM_STORE_PUT) { pm_winit(&w, b, sizeof b); pm_put_ok(&w, h.id); sendw(s, &w); }        /* (storage writes are answered after STOP) */
        }
        if (waitpid(p, &st, WNOHANG) == p) return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    }
    kill(p, SIGKILL); waitpid(p, &st, 0);
    return -1;
}

/* ---- P1 and N4 */
static int got, first = -1, last = -1, ticks1, ready_why = -1, ready_fd = 0;
static void p1_log(void *c, const char *cur, int gap, const char *const *l, int n)
{ (void)c; (void)cur; (void)gap; for (int i = 0; i < n; i++) { int k = atoi(l[i] + 1); if (first < 0) first = k; last = k; got++; } }
static void p1_ready(void *c) { (void)c; ready_fd = hm_connect(0, 0, &ready_why); }
static void p1_tick(void *c) { int why; (void)c; if (!ticks1++) { int fd = hm_connect(0, 0, &why); fprintf(stderr, "connect -> %d (%d)\n", fd, why); } }
static void p1_stop(void *c) { (void)c; fprintf(stderr, "lines %d first L%d last L%d ready_connect %d (%d)\n", got, first, last, ready_fd, ready_why); }

/* ---- P2 */
static void p2_ready(void *c) { (void)c; }
static void p2_tick(void *c)
{
    static int done; (void)c;
    if (done++) return;
    for (int i = 0; i < 9; i++) { char k[8], v[8]; int why = 0; snprintf(k, sizeof k, "k%d", i); snprintf(v, sizeof v, "v%d", i);
        int r = hm_store_put(k, v, strlen(v), &why); fprintf(stderr, "put %s = %d (%d)\n", k, r, why); }
}

/* ---- net.lan, net.internet: hm_connect_lan, hm_connect_internet: one CONNECT each, of their kind, with the plugin's own host */
static void l6_tick(void *c)
{
    static int n; int why = 0, fd; (void)c;
    if (n++) return;
    fd = hm_connect_lan(1, "10.0.0.9", 80, &why); fprintf(stderr, "lan -> %d (%d)\n", fd, why);
    fd = hm_connect_internet(0, "example.org", 443, &why); fprintf(stderr, "internet -> %d (%d)\n", fd, why);
    { static char longer[300]; memset(longer, 'a', 299); fd = hm_connect_internet(0, longer, 443, &why); fprintf(stderr, "long -> %d (%d)\n", fd, why); }
}

/* ---- P4 */
static void p4_tick(void *c) { struct timespec t = { 1, 100000000 }; (void)c; nanosleep(&t, NULL); }

/* ---- F2 */
static int f2_got[64], f2_n, f2_followed;
static void f2_log(void *c, const char *cur, int gap, const char *const *l, int n) { (void)c; (void)cur; (void)gap; for (int i = 0; i < n && f2_n < 64; i++) f2_got[f2_n++] = atoi(l[i] + 1); }
static void f2_tick(void *c) { (void)c; if (!f2_followed++) fprintf(stderr, "follow %d\n", hm_log_follow("start")); }
static void f2_stop(void *c) { (void)c; fprintf(stderr, "got"); for (int i = 0; i < f2_n; i++) fprintf(stderr, " L%d", f2_got[i]); fprintf(stderr, "\n"); }

/* ---- R3 */
static int r3_got[16], r3_n;
static void r3_log(void *c, const char *cur, int gap, const char *const *l, int n)
{ static int once; char v[8]; size_t len; (void)c; (void)cur; (void)gap;
  for (int i = 0; i < n && r3_n < 16; i++) r3_got[r3_n++] = atoi(l[i] + 1);
  if (!once++) hm_store_get("k", v, sizeof v, &len); }
static void r3_stop(void *c) { (void)c; fprintf(stderr, "got"); for (int i = 0; i < r3_n; i++) fprintf(stderr, " L%d", r3_got[i]); fprintf(stderr, "\n"); }
/* ---- an answer held to what is spoken (finding 6 of a review of 60a8589) */
static void a6_tick(void *c)
{
    static int n; char v[16] = ""; size_t len = 0; int r; (void)c;
    if (n++) return;
    r = hm_store_get("k", v, sizeof v - 1, &len); v[len < sizeof v ? len : sizeof v - 1] = 0;
    fprintf(stderr, "get -> %d (%s)\n", r, v);
}
#ifdef SCOPES_DEMO
/* ---- a value of a later version (the demo's call 2, RESULT_ELSEWHERE=6@2>FAILED) from a plugin that speaks call 1:
 * sent as its fallback, never dropped (finding 4 of a review of 60a8589) */
static void v4_tick(void *c) { static int n; (void)c; if (!n++) fprintf(stderr, "end -> %d\n", hm_call_end(PM_RESULT_ELSEWHERE)); }
/* ---- a state of a later version (the demo's audio.music 2, MUSIC_BUFFERING=4@2>PLAYING): from a plugin that speaks
 * audio.music 1 sent as PLAYING, never left unsent (finding 1 of a review of 61e3d8e: a state is no message to drop,
 * the last one told would stand), the next state as ever */
static void v5_tick(void *c) { static int n; (void)c; if (!n++) { int a = hm_music_state(PM_MUSIC_BUFFERING, "t", "a"); fprintf(stderr, "buffering -> %d, playing -> %d\n", a, hm_music_state(HM_MUSIC_PLAYING, "t", "a")); } }
#endif
/* ---- R2 */
static void r2_tick(void *c) { static int n; int why = 0; (void)c; if (!n++) { int fd = hm_connect(0, 0, &why); fprintf(stderr, "connect -> %d (%d)\n", fd, why); } }
static void r2_stop(void *c) { (void)c; fprintf(stderr, "STOP callback ran\n"); }
/* ---- R4 */
static void r4_tick(void *c) { static int n; int why; (void)c; if (!n++) { hm_store_put("other", "A", 1, &why); hm_store_put("cursor", "A", 1, &why); } }
static void r4_stop(void *c) { int why = 0; (void)c; int r = hm_store_put("cursor", "B-at-stop", 9, &why); fprintf(stderr, "put at stop -> %d (%d)\n", r, why); }
/* ---- R8 */
static void r8_tick(void *c) { (void)c; if (!f2_followed++) fprintf(stderr, "follow %d\n", hm_log_follow("0000000000000000:999")); }

/* ---- audio.music, net.udp: a music stream and a UDP socket, as the client takes them; the writable callback only while playing */
static int m_fd = -1, m_writes, m_paused_writes, m_paused;
static void m_tick(void *c)
{
    static int n; int why = 0, h = -1, port = 0; (void)c;
    if (n++) return;
    /* a value its enum has in no version, from the plugin: refused by the client, never sent (finding 3 of a review
     * of 4e0a202: hassmic would take it as malformed; the stand-in broker's next message must be MUSIC_OPEN) */
    fprintf(stderr, "bad state -> %d, bad kind -> %d", hm_music_state(9, "t", "a"), hm_udp_open(99, 0, &h, &port, &why));
    fprintf(stderr, " (%d)\n", why);
    m_fd = hm_music_open(48000, 2, &why);
    fprintf(stderr, "music -> %s (%d)\n", m_fd >= 0 ? "fd" : "none", why);
    int u = hm_udp_open(HM_CAP_NET_UDP, 0, &h, &port, &why);
    fprintf(stderr, "udp -> %s handle %d port %d (%d)\n", u >= 0 ? "fd" : "none", h, port, why);
}
static void m_writable(void *c, int fd) { static int16_t z[960]; (void)c; m_writes++; if (m_paused) m_paused_writes++; while (write(fd, z, sizeof z) > 0) ; }
static void m_cmd(void *c, int cmd, int why) { (void)c; fprintf(stderr, "cmd %d why %d\n", cmd, why); if (cmd == HM_MCMD_PAUSE) { m_paused = 1; hm_music_state(HM_MUSIC_PAUSED, "t", "a"); } }
static void m_stop(void *c) { (void)c; fprintf(stderr, "writes %d, while paused %d\n", m_writes, m_paused_writes); }

/* call: a call going out with the action's token, live, the room's frames echoed back, ended by hassmic */
static int c_frames;
static void c_action(void *c, const char *name, int from, const struct hm_arg *a, int n)
{
    char tok[33]; int why = 0, r; (void)c; (void)a; (void)n;
    snprintf(tok, sizeof tok, "%s", hm_action_token());
    fprintf(stderr, "action %s from %d token %s\n", name, from, tok);
    r = hm_call_out(tok, "Bob", &why); fprintf(stderr, "out -> %d (%d)\n", r, why);
    hm_call_ringing_there();
    r = hm_call_live("", &why); fprintf(stderr, "live -> %d (%d)\n", r, why);
}
static void c_audio(void *c, uint32_t seq, const int16_t *pcm, int n) { (void)c; if (n == HM_CALL_FRAME && !hm_call_audio(seq, pcm)) c_frames++; }
/* request.audio, request.text: a request's copy as the client hands it on: start, audio (run, offset, samples), end, text */
static int q_blocks, q_samples, q_inorder = 1; static uint32_t q_next;
static void q_start(void *c, uint32_t run, int source, int slot, const char *phrase) { (void)c; q_next = 0; fprintf(stderr, "start %u source %d slot %d phrase %s\n", run, source, slot, phrase); }
static void q_audio(void *c, uint32_t run, uint32_t off, const int16_t *pcm, int n) { (void)c; (void)run; q_inorder &= off == q_next && pcm[0] == (int16_t)off; q_next = off + (uint32_t)n; q_blocks++; q_samples += n; }
static void q_end(void *c, uint32_t run, int why, uint32_t samples, uint32_t dropped) { (void)c; fprintf(stderr, "end %u why %d samples %u dropped %u; got %d blocks %d samples in order %d\n", run, why, samples, dropped, q_blocks, q_samples, q_inorder); }
static void q_text(void *c, uint32_t run, int kind, const char *t) { (void)c; fprintf(stderr, "text %u kind %d: %s\n", run, kind, t); }
/* button: BUTTON to the button callback, key and how as hassmic sent them */
static void b_button(void *c, int key, int how) { (void)c; fprintf(stderr, "button %d how %d\n", key, how); }
static void c_hangup(void *c, int why) { (void)c; fprintf(stderr, "hangup %d after %d frames; audio now %d\n", why, c_frames, hm_call_audio(9, (const int16_t[HM_CALL_FRAME]){ 0 })); }

/* file_check: "whole" fills hm_check to its bounds (a title of 64, 8 fields of 32 and 256 bytes); "over" leaves the
 * title without its NUL (65 bytes and on into the next field's), which no FILE_OK takes */
static int f_check(void *ctx, const char *list, const char *item, struct hm_check *c)
{
    (void)ctx; (void)list;
    if (!strcmp(item, "over")) { memset(c->title, 'x', sizeof c->title); c->fields[0].name[0] = 0; c->nfields = 0; return 0; }
    memset(c->title, 't', 64); c->title[64] = 0; c->nfields = 8;
    for (int i = 0; i < 8; i++) { memset(c->fields[i].name, 'a' + i, 32); c->fields[i].name[32] = 0; memset(c->fields[i].value, 'v', 256); c->fields[i].value[256] = 0; }
    return 0;
}

int main(void)
{
    struct pm_w w; struct pm_hdr h; const uint8_t *body; size_t bl; int s, e, st; pid_t p;
    signal(SIGPIPE, SIG_IGN); setvbuf(stdout, NULL, _IONBF, 0);
    dup2(2, 3); dup2(2, 4);                 /* (3 and 4 taken: a socketpair's ends are never the child's descriptor 3) */

    /* P1 and N4: 300 LOG events while a CONNECT waits, then its answer: every one of them handed on, in order */
    { static const struct hm_callbacks cb = { .log = p1_log, .tick = p1_tick, .stop = p1_stop, .ready = p1_ready };
      p = plugin(&cb, PM_CAPBIT(PM_CAP_LOG_READ) | PM_CAPBIT(PM_CAP_NET_SERVER), &s, &e);
      CHECK(next(s, 5000, &h, &body, &bl) == PM_READY, "READY after the ready callback (whose connect asked nothing of hassmic)");
      CHECK(next(s, 5000, &h, &body, &bl) == PM_CONNECT, "the tick's CONNECT");
      uint32_t cid = h.id;
      for (int i = 0; i < 300; i++) log_to(s, i);
      { struct pm_err er = { PM_ERR_FAILED }; pm_winit(&w, b, sizeof b); pm_put_err(&w, cid, &er); sendw(s, &w); }
      { int seen[64] = { 0 }; stop_and_wait(s, p, 3000, seen); read_errs(e); close(s); close(e);
        CHECK(seen[PM_OK] == 300, "R9: each of the 300 LOG answered OK once handled"); }
      CHECK(strstr(errs, "lines 300 first L0 last L299"), "P1: 300 LOG events that came while a request waited: all handed on, in order (it kept 32)");
      CHECK(strstr(errs, "ready_connect -1 (3)"), "N4: hm_connect in the ready callback: -1, HM_ERR_BAD"); if (fails) printf("     (%s)\n", errs); }

    /* net.lan's and net.internet's CONNECT, the kind and the plugin's own host on the wire */
    { static const struct hm_callbacks cb = { .tick = l6_tick }; struct pm_connect cm; char host[254]; int ok = 1;
      p = plugin(&cb, PM_CAPBIT(PM_CAP_NET_LAN) | PM_CAPBIT(PM_CAP_NET_INTERNET), &s, &e);
      CHECK(next(s, 5000, &h, &body, &bl) == PM_READY, "(READY)");
      for (int k = 0; k < 2; k++) {
          int t = next(s, 5000, &h, &body, &bl); struct pm_err er = { PM_ERR_DENIED };
          ok &= t == PM_CONNECT && !pm_get_connect(body, bl, &cm) && !pm_cstr(host, sizeof host, cm.host)
                && (k ? cm.kind == PM_CAP_NET_INTERNET && cm.entry == 0 && cm.port == 443 && !strcmp(host, "example.org")
                      : cm.kind == PM_CAP_NET_LAN && cm.entry == 1 && cm.port == 80 && !strcmp(host, "10.0.0.9"));
          pm_winit(&w, b, sizeof b); pm_put_err(&w, h.id, &er); sendw(s, &w);
      }
      { int seen[64] = { 0 }; stop_and_wait(s, p, 3000, seen); read_errs(e); close(s); close(e); }
      CHECK(ok && strstr(errs, "lan -> -1 (2)") && strstr(errs, "internet -> -1 (2)") && strstr(errs, "long -> -1 (3)"),
            "hm_connect_lan and hm_connect_internet: a CONNECT of their kind with the address or name; a host over 253 bytes BAD, nothing sent"); if (fails) printf("     (%s)\n", errs); }

    /* P1: past the client's bound (2 MB of what came) the plugin ends, and says so.  (log.read granted: a LOG it does
     * not speak is no message to keep, dropped as it comes since answer() holds what it reads to what is spoken) */
    { static const struct hm_callbacks cb = { .tick = p1_tick };
      p = plugin(&cb, PM_CAPBIT(PM_CAP_NET_SERVER) | PM_CAPBIT(PM_CAP_LOG_READ), &s, &e);
      next(s, 5000, &h, &body, &bl); next(s, 5000, &h, &body, &bl);           /* READY, CONNECT */
      { static char big[1000]; memset(big, 'x', sizeof big - 1);
        for (int i = 0; i < 400 && waitpid(p, &st, WNOHANG) != p; i++) {
            struct pm_log m = { pm_s("0000000000000000:1"), 0, { { 0 }, 24 } }; struct pm_log_lines ln = { pm_s(big) };
            pm_winit(&w, b, sizeof b); pm_put_log(&w, 0, &m); for (int k = 0; k < 24; k++) pm_add_log_lines(&w, &ln);
            if (pm_wlen(&w) < 0) { printf("     (a LOG that does not fit)\n"); break; }
            if (send(s, w.b, (size_t)pm_wlen(&w), MSG_DONTWAIT) < 0) { usleep(2000); i--; }
        } }
      if (!WIFEXITED(st) && !WIFSIGNALED(st)) waitpid(p, &st, 0);
      read_errs(e); close(s); close(e);
      CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 70 && strstr(errs, "cannot keep what hassmic sends while a request waits"), "P1: past its bound the plugin ends and says why (never a silent drop)"); }

    /* P2: BUSY to every write, the client keeps 8 keys: the ninth is -1 BUSY, not 1 */
    { static const struct hm_callbacks cb = { .tick = p2_tick, .ready = p2_ready };
      p = plugin(&cb, PM_CAPBIT(PM_CAP_STORAGE), &s, &e);
      for (int t; (t = next(s, 3000, &h, &body, &bl)); )
          if (t == PM_STORE_PUT) { struct pm_err er = { PM_ERR_BUSY }; pm_winit(&w, b, sizeof b); pm_put_err(&w, h.id, &er); sendw(s, &w); }
      stop_and_wait(s, p, 3000, NULL); read_errs(e); close(s); close(e);
      CHECK(strstr(errs, "put k7 = 1 (4)") && strstr(errs, "put k8 = -1 (4)"), "P2: eight kept for later (1), the ninth not kept: -1, HM_ERR_BUSY"); if (!strstr(errs, "put k8 = -1 (4)")) printf("     (%s)\n", errs); }

    /* P2: a kept value the Echo then refuses for good: said in the plugin's log */
    { static const struct hm_callbacks cb = { .tick = p2_tick };
      int puts = 0; p = plugin(&cb, PM_CAPBIT(PM_CAP_STORAGE), &s, &e);
      for (int t; (t = next(s, 12000, &h, &body, &bl)); )
          if (t == PM_STORE_PUT) { struct pm_err er = { ++puts <= 9 ? PM_ERR_BUSY : PM_ERR_LIMIT }; pm_winit(&w, b, sizeof b); pm_put_err(&w, h.id, &er); sendw(s, &w); if (puts == 10) break; }
      stop_and_wait(s, p, 3000, NULL); read_errs(e); close(s); close(e);
      CHECK(strstr(errs, "kept to be written later was refused (error 5): not stored"), "P2: a kept write refused at its retry: said, not dropped silently"); }

    /* P4: a tick of 1.1 s, PINGs answered all the same */
    { static const struct hm_callbacks cb = { .tick = p4_tick }; int answered = 0;
      p = plugin(&cb, 0, &s, &e);
      next(s, 5000, &h, &body, &bl);
      for (int i = 0; i < 3; i++) { pm_winit(&w, b, sizeof b); pm_put_ping(&w, 100 + (uint32_t)i); sendw(s, &w); answered += next(s, 5000, &h, &body, &bl) == PM_PONG; }
      kill(p, SIGKILL); waitpid(p, &st, 0); close(s); close(e);
      CHECK(answered == 3, "P4: a tick longer than a second: every PING answered"); }

    /* F2: LOG from the old place that comes while hm_log_follow waits: dropped; after its answer, the new place's */
    { static const struct hm_callbacks cb = { .tick = f2_tick, .log = f2_log, .stop = f2_stop };
      p = plugin(&cb, PM_CAPBIT(PM_CAP_LOG_READ), &s, &e);
      next(s, 5000, &h, &body, &bl);                                   /* READY */
      CHECK(next(s, 5000, &h, &body, &bl) == PM_LOG_FOLLOW, "(LOG_FOLLOW asked)");
      for (int i = 50; i < 55; i++) log_to(s, i);                      /* the old place's, sent before the answer */
      pm_winit(&w, b, sizeof b); pm_put_ok(&w, h.id); sendw(s, &w);
      for (int i = 0; i < 3; i++) log_to(s, i);                        /* the new place's */
      usleep(300000); stop_and_wait(s, p, 3000, NULL); read_errs(e); close(s); close(e);
      CHECK(strstr(errs, "follow 0") && strstr(errs, "got L0 L1 L2\n"), "F2: the old place's LOG before the answer dropped, the new place's handed on"); if (!strstr(errs, "got L0 L1 L2\n")) printf("     (%s)\n", errs); }

    /* R3: the log callback asks for a value on its first LOG (a round trip); L1 comes while it waits, the answer, L2
     * at once behind it: handed on L0, L1, L2, each answered OK with its own id, in that order */
    { static const struct hm_callbacks cb = { .log = r3_log, .stop = r3_stop };
      int oks[8], no = 0; p = plugin(&cb, PM_CAPBIT(PM_CAP_LOG_READ) | PM_CAPBIT(PM_CAP_STORAGE), &s, &e);
      next(s, 5000, &h, &body, &bl);                                   /* READY */
      log_to(s, 0);
      { int t; while ((t = next(s, 3000, &h, &body, &bl)) && t != PM_STORE_GET) ; }
      uint32_t gid = h.id;
      log_to(s, 1);
      { struct pm_store_value v = { pm_b("x", 1) }; pm_winit(&w, b, sizeof b); pm_put_store_value(&w, gid, &v); sendw(s, &w); }
      log_to(s, 2);
      for (int t; (t = next(s, 500, &h, &body, &bl)); ) if (t == PM_OK && no < 8) oks[no++] = (int)h.id;
      stop_and_wait(s, p, 3000, NULL); read_errs(e); close(s); close(e);
      CHECK(strstr(errs, "got L0 L1 L2\n"), "R3: LOG kept while a request waited is handed on before a newer one (it was L0 L2 L1)"); if (!strstr(errs, "got L0 L1 L2\n")) printf("     (%s)\n", errs);
      CHECK(no == 3 && oks[0] == 1000 && oks[1] == 1001 && oks[2] == 1002, "R9: each LOG answered OK, its own id, once handled"); }

    /* An answer is held to what is spoken, as everything else the client takes (finding 6 of a review of 60a8589:
     * answer() took ERR, OK, STORE_VALUE and UDP_OPENED with this id unchecked): an ERR with a code ERR_* has in no
     * version, an OK with a byte in it, a STORE_VALUE with a byte after its value are no answer and dropped; the
     * well-formed STORE_VALUE after them is the answer */
    { static const struct hm_callbacks cb = { .tick = a6_tick };
      p = plugin(&cb, PM_CAPBIT(PM_CAP_STORAGE), &s, &e);
      next(s, 5000, &h, &body, &bl);                                   /* READY */
      { int t; while ((t = next(s, 3000, &h, &body, &bl)) && t != PM_STORE_GET) ; }
      uint32_t gid = h.id;
      { struct pm_err er = { 99 }; pm_winit(&w, b, sizeof b); pm_put_err(&w, gid, &er); sendw(s, &w); }
      { pm_winit(&w, b, sizeof b); pm_put_ok(&w, gid); pm_wn(&w, 0, 1); sendw(s, &w); }
      { struct pm_store_value v = { pm_b("bad", 3) }; pm_winit(&w, b, sizeof b); pm_put_store_value(&w, gid, &v); pm_wn(&w, 0, 1); sendw(s, &w); }
      { struct pm_store_value v = { pm_b("good", 4) }; pm_winit(&w, b, sizeof b); pm_put_store_value(&w, gid, &v); sendw(s, &w); }
      stop_and_wait(s, p, 3000, NULL); read_errs(e); close(s); close(e);
      CHECK(strstr(errs, "get -> 0 (good)"), "an ERR of no ERR_* value, an OK with a body, a STORE_VALUE with a byte after it: no answer; the well-formed one is"); if (!strstr(errs, "get -> 0 (good)")) printf("     (%s)\n", errs); }

#ifdef SCOPES_DEMO
    /* the demo's later SDK: CALL_END with a value of call 2 from a plugin whose WELCOME says call 1 goes as FAILED, the
     * value call 1 has in its place; to one of call 2 as it is.  (plugin_v's bump: every granted scope one past this
     * build's would be refused; call 2 is this build's own here, so the WELCOME is written out) */
    for (int two = 0; two < 2; two++) {
      static const struct hm_callbacks cb = { .tick = v4_tick }; struct pm_call_end ce = { 0 }; int t, ok = 0;
      int sv[2], ep[2]; struct pm_welcome wm = { 1, pm_s("pc"), pm_s("n"), pm_s("n"), pm_s("UTC"), { { 0 }, 2 } };
      socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv); if (pipe(ep)) return 1;
      if (!(p = fork())) { dup2(sv[1], 3); dup2(ep[1], 2); close(sv[0]); close(ep[0]); alarm(60); _exit(hm_run(&cb, NULL) ? 1 : 0); }
      close(sv[1]); close(ep[1]); s = sv[0]; e = ep[0];
      next(s, 5000, &h, &body, &bl);                                   /* HELLO */
      pm_winit(&w, b, sizeof b); pm_put_welcome(&w, 0, &wm);
      { struct pm_welcome_scopes x = { pm_s("core"), 1 }; pm_add_welcome_scopes(&w, &x); x = (struct pm_welcome_scopes){ pm_s("call"), (uint16_t)(1 + two) }; pm_add_welcome_scopes(&w, &x); }
      sendw(s, &w);
      { struct pm_config cf = { { { 0 }, 0 } }; pm_winit(&w, b, sizeof b); pm_put_config(&w, 0, &cf); sendw(s, &w); }
      while ((t = next(s, 3000, &h, &body, &bl)) && t != PM_CALL_END) ;
      if (t == PM_CALL_END) { uint16_t v[PM_SCOPES] = { 0 }; v[PM_SCOPE_CORE] = 1; v[PM_SCOPE_CALL] = (uint16_t)(1 + two);
          ok = !pm_get_call_end(body, bl, &ce) && !pm_check(PM_CALL_END, body, bl, v); pm_winit(&w, b, sizeof b); pm_put_ok(&w, h.id); sendw(s, &w); }
      stop_and_wait(s, p, 3000, NULL); read_errs(e); close(s); close(e);
      if (!two) CHECK(ok && ce.result == PM_RESULT_FAILED && strstr(errs, "end -> 0"), "demo: RESULT_ELSEWHERE (call 2) from a plugin of call 1: sent as RESULT_FAILED, answered");
      else CHECK(ok && ce.result == PM_RESULT_ELSEWHERE && strstr(errs, "end -> 0"), "demo: ... from a plugin of call 2: sent as it is");
      if (!ok) printf("     (%s)\n", errs);
    }
    /* the demo's MUSIC_BUFFERING=4@2>PLAYING: a plugin whose WELCOME says audio.music 1 sends it as PLAYING (0), and
     * PLAYING after it as ever; one of audio.music 2 sends it as it is */
    for (int two = 0; two < 2; two++) {
      static const struct hm_callbacks cb = { .tick = v5_tick }; struct pm_music_state ms = { 0 }; int t, ok = 0, first = -1, count = 0;
      int sv[2], ep[2]; struct pm_welcome wm = { 1, pm_s("pc"), pm_s("n"), pm_s("n"), pm_s("UTC"), { { 0 }, 2 } };
      socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv); if (pipe(ep)) return 1;
      if (!(p = fork())) { dup2(sv[1], 3); dup2(ep[1], 2); close(sv[0]); close(ep[0]); alarm(60); _exit(hm_run(&cb, NULL) ? 1 : 0); }
      close(sv[1]); close(ep[1]); s = sv[0]; e = ep[0];
      next(s, 5000, &h, &body, &bl);                                   /* HELLO */
      pm_winit(&w, b, sizeof b); pm_put_welcome(&w, 0, &wm);
      { struct pm_welcome_scopes x = { pm_s("core"), 1 }; pm_add_welcome_scopes(&w, &x); x = (struct pm_welcome_scopes){ pm_s("audio.music"), (uint16_t)(1 + two) }; pm_add_welcome_scopes(&w, &x); }
      sendw(s, &w);
      { struct pm_config cf = { { { 0 }, 0 } }; pm_winit(&w, b, sizeof b); pm_put_config(&w, 0, &cf); sendw(s, &w); }
      while ((t = next(s, 2500, &h, &body, &bl)))
          if (t == PM_MUSIC_STATE) { uint16_t v[PM_SCOPES] = { 0 }; v[PM_SCOPE_CORE] = 1; v[PM_SCOPE_AUDIO_MUSIC] = (uint16_t)(1 + two);
              ok = !pm_get_music_state(body, bl, &ms) && !pm_check(PM_MUSIC_STATE, body, bl, v); if (first < 0) first = ms.state; count++; }
      stop_and_wait(s, p, 3000, NULL); read_errs(e); close(s); close(e);
      if (!two) CHECK(ok && count == 2 && first == PM_MUSIC_PLAYING && strstr(errs, "buffering -> 0, playing -> 0"), "demo: MUSIC_BUFFERING (audio.music 2, to PLAYING) from a plugin of audio.music 1: sent as PLAYING; PLAYING after it sent");
      else CHECK(ok && count == 2 && first == PM_MUSIC_BUFFERING && strstr(errs, "buffering -> 0, playing -> 0"), "demo: ... from a plugin of audio.music 2: sent as it is");
      if (count != 2) printf("     (%d MUSIC_STATE, the first %d: %s)\n", count, first, errs);
    }
#endif

    /* R2: STOP while hm_connect waits (hassmic answers a stopping plugin's CONNECT with FAILED now; here: not at all,
     * as before): the wait ends, the stop callback runs, the plugin ends by itself */
    { static const struct hm_callbacks cb = { .tick = r2_tick, .stop = r2_stop };
      p = plugin(&cb, PM_CAPBIT(PM_CAP_NET_SERVER), &s, &e);
      { int t; while ((t = next(s, 3000, &h, &body, &bl)) && t != PM_CONNECT) ; }
      int ex = stop_and_wait(s, p, 5000, NULL); read_errs(e); close(s); close(e);
      CHECK(ex == 0 && strstr(errs, "connect -> -1 (7)") && strstr(errs, "STOP callback ran"), "R2: STOP while a CONNECT waits: hm_connect -1 (FAILED), the stop callback runs, the plugin ends by itself");
      if (ex != 0 || !strstr(errs, "connect -> -1 (7)")) printf("     (exit %d: %s)\n", ex, errs); }

    /* R4: a write kept for later (BUSY) and STOP: written as the plugin ends; the stop callback's own write goes at once */
    { static const struct hm_callbacks cb = { .tick = r4_tick, .stop = r4_stop }; int seen[64] = { 0 };
      p = plugin(&cb, PM_CAPBIT(PM_CAP_STORAGE), &s, &e);
      for (int t, k = 0; k < 2 && (t = next(s, 3000, &h, &body, &bl)); ) if (t == PM_STORE_PUT) { struct pm_err er = { PM_ERR_BUSY }; pm_winit(&w, b, sizeof b); pm_put_err(&w, h.id, &er); sendw(s, &w); k++; }
      usleep(200000);
      int ex = stop_and_wait(s, p, 3000, seen); read_errs(e); close(s); close(e);
      CHECK(ex == 0 && seen[PM_STORE_PUT] == 2 && strstr(errs, "put at stop -> 0"), "R4: after STOP the stop callback's write (at once, 0) and the one kept for later (flushed): two writes, none lost");
      if (seen[PM_STORE_PUT] != 2) printf("     (%d writes after STOP: %s)\n", seen[PM_STORE_PUT], errs); }

    /* R8: hm_log_follow refused (ERR BAD keeps the old place): the old place's LOG that came meanwhile is handed on */
    { static const struct hm_callbacks cb = { .tick = r8_tick, .log = f2_log, .stop = f2_stop };
      f2_n = 0; f2_followed = 0;
      p = plugin(&cb, PM_CAPBIT(PM_CAP_LOG_READ), &s, &e);
      next(s, 5000, &h, &body, &bl);                                   /* READY */
      CHECK(next(s, 5000, &h, &body, &bl) == PM_LOG_FOLLOW, "(LOG_FOLLOW asked)");
      log_to(s, 7);                                                    /* the old place's, which goes on */
      { struct pm_err er = { PM_ERR_BAD }; pm_winit(&w, b, sizeof b); pm_put_err(&w, h.id, &er); sendw(s, &w); }
      log_to(s, 8);
      usleep(300000);
      stop_and_wait(s, p, 3000, NULL); read_errs(e); close(s); close(e);
      CHECK(strstr(errs, "follow -1") && strstr(errs, "got L7 L8\n"), "R8: a refused follow: the old place's LOG kept meanwhile handed on, nothing lost"); if (!strstr(errs, "got L7 L8\n")) printf("     (%s)\n", errs); }

    /* MUSIC_OPEN's OK brings the stream, UDP_OPEN's UDP_OPENED the socket; the stream is polled for writing
     * while the player plays and not once it says paused (a writable stream nobody writes to had the loop spin) */
    { static const struct hm_callbacks cb = { .tick = m_tick, .music_writable = m_writable, .music_cmd = m_cmd, .stop = m_stop };
      int ms[2], us[2], fd; uint32_t id;
      p = plugin(&cb, PM_CAPBIT(PM_CAP_AUDIO_MUSIC) | PM_CAPBIT(PM_CAP_NET_UDP), &s, &e);
      next(s, 5000, &h, &body, &bl);                                   /* READY */
      CHECK(next(s, 5000, &h, &body, &bl) == PM_MUSIC_OPEN, "the tick's MUSIC_OPEN");
      socketpair(AF_UNIX, SOCK_STREAM, 0, ms); fcntl(ms[1], F_SETFL, O_NONBLOCK);
      { union { struct cmsghdr c; char x[CMSG_SPACE(sizeof(int))]; } u; struct iovec iov; struct msghdr mh = { 0 };
        pm_winit(&w, b, sizeof b); pm_put_ok(&w, h.id); iov.iov_base = w.b; iov.iov_len = (size_t)pm_wlen(&w);
        mh.msg_iov = &iov; mh.msg_iovlen = 1; mh.msg_control = u.x; mh.msg_controllen = sizeof u.x;
        u.c.cmsg_level = SOL_SOCKET; u.c.cmsg_type = SCM_RIGHTS; u.c.cmsg_len = CMSG_LEN(sizeof(int)); memcpy(CMSG_DATA(&u.c), &ms[1], sizeof(int));
        sendmsg(s, &mh, 0); close(ms[1]);
        CHECK(next(s, 5000, &h, &body, &bl) == PM_UDP_OPEN, "then its UDP_OPEN");
        id = h.id; socketpair(AF_UNIX, SOCK_DGRAM, 0, us); fd = us[1];
        { struct pm_udp_opened o = { 3, 28941 }; pm_winit(&w, b, sizeof b); pm_put_udp_opened(&w, id, &o); iov.iov_base = w.b; iov.iov_len = (size_t)pm_wlen(&w); }
        memcpy(CMSG_DATA(&u.c), &fd, sizeof(int)); sendmsg(s, &mh, 0); close(us[1]); }
      { char d[65536]; long got = 0, k; for (int i = 0; i < 50 && got < 20000; i++) { usleep(10000); while ((k = recv(ms[0], d, sizeof d, MSG_DONTWAIT)) > 0) got += k; }
        CHECK(got >= 20000, "what the writable callback writes comes through the stream"); }
      { struct pm_music_cmd mc = { PM_MCMD_PAUSE, PM_MWHY_BUTTON }; pm_winit(&w, b, sizeof b); pm_put_music_cmd(&w, 0, &mc); sendw(s, &w); }
      { int t; while ((t = next(s, 1000, &h, &body, &bl)) && t != PM_MUSIC_STATE) ; CHECK(t == PM_MUSIC_STATE, "MUSIC_CMD pause: MUSIC_STATE back"); }
      { char d[65536]; for (int i = 0; i < 30; i++) { usleep(10000); while (recv(ms[0], d, sizeof d, MSG_DONTWAIT) > 0) ; } }   /* (the stream's room, free again) */
      stop_and_wait(s, p, 3000, NULL); read_errs(e); close(s); close(e); close(ms[0]); close(us[0]);
      CHECK(strstr(errs, "music -> fd (0)") && strstr(errs, "udp -> fd handle 3 port 28941 (0)") && strstr(errs, "cmd 1 why 1"), "hm_music_open and hm_udp_open: their descriptors, the handle and the port");
      CHECK(strstr(errs, "bad state -> -1, bad kind -> -1 (3)") != NULL, "a MUSIC_STATE and a UDP_OPEN with values their enums do not have: refused by the client (BAD), not sent");
      CHECK(strstr(errs, "while paused 0\n") != NULL, "paused: the writable callback is not called, though the stream takes more"); if (fails) printf("     (%s)\n", errs); }

    /* the action's token reaches the callback (hm_action_token), CALL_OUT carries it, CALL_LIVE's OK brings the
     * audio socket, the loop hands the room's frames to call_audio, hm_call_audio sends; CALL_HANGUP closes it */
    { static const struct hm_callbacks cb = { .action = c_action, .call_audio = c_audio, .call_hangup = c_hangup };
      int cs[2], t; char tok[40] = "";
      p = plugin(&cb, PM_CAPBIT(PM_CAP_CALL) | PM_CAPBIT(PM_CAP_HA_ACTIONS), &s, &e);
      next(s, 5000, &h, &body, &bl);                                   /* READY */
      { struct pm_action a = { pm_s("dial"), PM_FROM_HOME_ASSISTANT, pm_s("0123456789abcdef0123456789abcdef"), { { 0 }, 0 } }; pm_winit(&w, b, sizeof b); pm_put_action(&w, 0, &a); sendw(s, &w); }
      t = next(s, 5000, &h, &body, &bl);
      if (t == PM_CALL_OUT) { struct pm_call_out co; if (!pm_get_call_out(body, bl, &co)) pm_cstr(tok, sizeof tok, co.token); }
      CHECK(t == PM_CALL_OUT && !strcmp(tok, "0123456789abcdef0123456789abcdef"), "the action's token: CALL_OUT carries it");
      pm_winit(&w, b, sizeof b); pm_put_ok(&w, h.id); sendw(s, &w);
      CHECK(next(s, 5000, &h, &body, &bl) == PM_CALL_RINGING_THERE && !h.id, "CALL_RINGING_THERE, unanswered (no id)");
      CHECK(next(s, 5000, &h, &body, &bl) == PM_CALL_LIVE, "then CALL_LIVE");
      socketpair(AF_UNIX, SOCK_SEQPACKET, 0, cs);
      { union { struct cmsghdr c; char x[CMSG_SPACE(sizeof(int))]; } u; struct iovec iov; struct msghdr mh = { 0 };
        pm_winit(&w, b, sizeof b); pm_put_ok(&w, h.id); iov.iov_base = w.b; iov.iov_len = (size_t)pm_wlen(&w);
        mh.msg_iov = &iov; mh.msg_iovlen = 1; mh.msg_control = u.x; mh.msg_controllen = sizeof u.x;
        u.c.cmsg_level = SOL_SOCKET; u.c.cmsg_type = SCM_RIGHTS; u.c.cmsg_len = CMSG_LEN(sizeof(int)); memcpy(CMSG_DATA(&u.c), &cs[1], sizeof(int));
        sendmsg(s, &mh, 0); close(cs[1]); }
      { uint8_t f[4 + 640] = { 0 }, g[700]; int back = 0, inorder = 1; long k;
        for (int i = 0; i < 3; i++) { f[0] = (uint8_t)(10 + i); f[4] = (uint8_t)i; if (send(cs[0], f, sizeof f, 0) < 0) { } }
        for (int i = 0; i < 100 && back < 3; i++) { struct pollfd pf = { cs[0], POLLIN, 0 }; if (poll(&pf, 1, 50) == 1 && (k = recv(cs[0], g, sizeof g, 0)) == 644) { inorder &= g[0] == 10 + back && g[4] == back; back++; } }
        CHECK(back == 3 && inorder, "the room's frames reach call_audio, and hm_call_audio sends them back as they came: 644 bytes, the number in front"); }
      { struct pm_call_hangup hu = { PM_HANGUP_BUTTON }; pm_winit(&w, b, sizeof b); pm_put_call_hangup(&w, 0, &hu); sendw(s, &w); }
      { uint8_t g[8]; long k = -1; for (int i = 0; i < 100 && k != 0; i++) { struct pollfd pf = { cs[0], POLLIN, 0 }; if (poll(&pf, 1, 20) == 1) k = recv(cs[0], g, sizeof g, 0); }
        CHECK(k == 0, "CALL_HANGUP: the client closes the audio socket"); }
      stop_and_wait(s, p, 3000, NULL); read_errs(e); close(s); close(e); close(cs[0]);
      CHECK(strstr(errs, "action dial from 1 token 0123456789abcdef0123456789abcdef") && strstr(errs, "out -> 0 (0)") && strstr(errs, "live -> 0 (0)")
            && strstr(errs, "hangup 1 after 3 frames; audio now -1"), "hm_action_token, hm_call_out, hm_call_live; the hangup callback, no audio after it");
      if (!strstr(errs, "hangup 1")) printf("     (%s)\n", errs); }

    /* REQ_START brings the request's socket; its blocks go to request_audio in order with their offsets; at
     * REQ_END what is still in the socket is handed on first, then request_end; REQ_TEXT to request_text */
    { static const struct hm_callbacks cb = { .request_start = q_start, .request_audio = q_audio, .request_end = q_end, .request_text = q_text };
      int rs[2];
      p = plugin(&cb, PM_CAPBIT(PM_CAP_REQUEST_AUDIO) | PM_CAPBIT(PM_CAP_REQUEST_TEXT), &s, &e);
      next(s, 5000, &h, &body, &bl);                                   /* READY */
      socketpair(AF_UNIX, SOCK_SEQPACKET, 0, rs);
      { struct pm_req_start m = { 7, PM_REQSRC_WAKE_WORD, 1, pm_s("Alexa") }; union { struct cmsghdr c; char x[CMSG_SPACE(sizeof(int))]; } u; struct iovec iov; struct msghdr mh = { 0 };
        pm_winit(&w, b, sizeof b); pm_put_req_start(&w, 0, &m); iov.iov_base = w.b; iov.iov_len = (size_t)pm_wlen(&w);
        mh.msg_iov = &iov; mh.msg_iovlen = 1; mh.msg_control = u.x; mh.msg_controllen = sizeof u.x;
        u.c.cmsg_level = SOL_SOCKET; u.c.cmsg_type = SCM_RIGHTS; u.c.cmsg_len = CMSG_LEN(sizeof(int)); memcpy(CMSG_DATA(&u.c), &rs[1], sizeof(int));
        sendmsg(s, &mh, 0); close(rs[1]); }
      /* 40 blocks of 1024 samples, the last sent right before the end (still in the socket when REQ_END is read) */
      { static uint8_t pk[8 + 2048]; for (uint32_t i = 0; i < 40; i++) { uint32_t off = i * 1024; for (int k = 0; k < 4; k++) { pk[k] = (uint8_t)(7 >> 8 * k); pk[4 + k] = (uint8_t)(off >> 8 * k); }
            for (int k = 0; k < 1024; k++) { pk[8 + 2 * k] = (uint8_t)off; pk[9 + 2 * k] = (uint8_t)(off >> 8); }
            if (send(rs[0], pk, sizeof pk, 0) < 0) { } } }
      close(rs[0]);
      { struct pm_req_end m = { 7, PM_REQEND_HEARD_HERE, 40960, 0 }; pm_winit(&w, b, sizeof b); pm_put_req_end(&w, 0, &m); sendw(s, &w); }
      { struct pm_req_text m = { 7, PM_REQTEXT_HEARD, pm_s("turn on the lights") }; pm_winit(&w, b, sizeof b); pm_put_req_text(&w, 0, &m); sendw(s, &w); }
      usleep(200000);
      stop_and_wait(s, p, 3000, NULL); read_errs(e); close(s); close(e);
      CHECK(strstr(errs, "start 7 source 1 slot 1 phrase Alexa") && strstr(errs, "end 7 why 2 samples 40960 dropped 0; got 40 blocks 40960 samples in order 1")
            && strstr(errs, "text 7 kind 1: turn on the lights"), "a request: start, every block in order before the end, the end, the text");
      if (!strstr(errs, "got 40 blocks")) printf("     (%s)\n", errs); }

    /* BUTTON (an event, id 0) to the button callback; one without the callback set is dropped, the plugin goes on */
    { static const struct hm_callbacks cb = { .button = b_button }, none = { .config = NULL };
      p = plugin(&cb, PM_CAPBIT(PM_CAP_BUTTON), &s, &e);
      next(s, 5000, &h, &body, &bl);                                   /* READY */
      { struct pm_button m1 = { PM_BUTTON_KEY_ACTION, PM_BUTTON_HOW_PRESS }, m2 = { PM_BUTTON_KEY_VOLUME_DOWN, PM_BUTTON_HOW_HOLD };
        pm_winit(&w, b, sizeof b); pm_put_button(&w, 0, &m1); sendw(s, &w); pm_winit(&w, b, sizeof b); pm_put_button(&w, 0, &m2); sendw(s, &w); }
      usleep(200000);
      st = stop_and_wait(s, p, 3000, NULL); read_errs(e); close(s); close(e);
      CHECK(st == 0 && strstr(errs, "button 1 how 1\nbutton 3 how 2\n"), "BUTTON: the button callback, with the key and how, in order");
      if (!strstr(errs, "button 1 how 1")) printf("     (%s)\n", errs);
      p = plugin(&none, PM_CAPBIT(PM_CAP_BUTTON), &s, &e);
      next(s, 5000, &h, &body, &bl);
      { struct pm_button m1 = { PM_BUTTON_KEY_ACTION, PM_BUTTON_HOW_PRESS }; pm_winit(&w, b, sizeof b); pm_put_button(&w, 0, &m1); sendw(s, &w); }
      usleep(100000);
      st = stop_and_wait(s, p, 3000, NULL); read_errs(e); close(s); close(e);
      CHECK(st == 0, "BUTTON to a plugin without the callback: dropped, it ends as asked"); }

    /* Versions (sdk/PROTOCOL.md): HELLO names every scope this client has, at its version; what WELCOME says is what
     * is spoken: a BUTTON to a plugin whose WELCOME did not name button reaches no callback, and a WELCOME that names
     * a version past this client's ends it (no hassmic that read its HELLO sends one) */
    { static const struct hm_callbacks cb = { .button = b_button };
      CHECK(hellos_whole, "every HELLO named each scope of this build once, at its version");
      p = plugin(&cb, PM_CAPBIT(PM_CAP_STATE), &s, &e);
      next(s, 5000, &h, &body, &bl);                                   /* READY */
      { struct pm_button m1 = { PM_BUTTON_KEY_ACTION, PM_BUTTON_HOW_PRESS }; pm_winit(&w, b, sizeof b); pm_put_button(&w, 0, &m1); sendw(s, &w); }
      usleep(200000);
      st = stop_and_wait(s, p, 3000, NULL); read_errs(e); close(s); close(e);
      CHECK(st == 0 && !strstr(errs, "button 1"), "BUTTON while button is not spoken: dropped, no callback");
      p = plugin_v(&cb, PM_CAPBIT(PM_CAP_BUTTON), &s, &e, 1);
      /* until it ended, 5 s at most by the clock: once it closed its end, next() answers at once (EOF), and 50 rounds of
       * it were over before a loaded CI runner let the child exit (st was the plugin's before, and it failed) */
      { int any = 0; struct timespec w0, w1; clock_gettime(CLOCK_MONOTONIC, &w0);
        for (;;) {
            if (waitpid(p, &st, WNOHANG) == p) break;
            clock_gettime(CLOCK_MONOTONIC, &w1);
            if ((w1.tv_sec - w0.tv_sec) * 1000 + (w1.tv_nsec - w0.tv_nsec) / 1000000 >= 5000) { kill(p, SIGKILL); waitpid(p, &st, 0); break; }
            if (next(s, 100, &h, &body, &bl) > 0) any = 1; else usleep(10000);
        }
        read_errs(e); close(s); close(e);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 1 && !any && strstr(errs, "hassmic speaks a scope this client does not have"), "a WELCOME past this client's version: it ends before READY, and says why"); } }

    /* FILE_CHECK answered at its bound: FILE_OK of a title of 64 and 8 fields of 256 is 2412 bytes, and the client's
     * buffer was 1024 (a review of 6631974, 5: nothing went back, the owner's upload refused after 5 s without a
     * word).  An answer the protocol does not take: FILE_BAD and a line in the log, never silence */
    { static const struct hm_callbacks cb = { .file_check = f_check };
      p = plugin(&cb, PM_CAPBIT(PM_CAP_FILES), &s, &e);
      next(s, 5000, &h, &body, &bl);                                   /* READY */
      { struct pm_file_check m = { pm_s("models"), pm_s("whole") }; pm_winit(&w, b, sizeof b); pm_put_file_check(&w, 41, &m); sendw(s, &w); }
      { int t = next(s, 5000, &h, &body, &bl), n = 0, longest = 0; struct pm_file_ok ok; struct pm_file_ok_fields f;
        if (t == PM_FILE_OK && !pm_get_file_ok(body, bl, &ok)) { while (pm_next_file_ok_fields(&ok.fields, &f)) { n++; if (f.value.n > longest) longest = f.value.n; } }
        CHECK(t == PM_FILE_OK && h.id == 41 && ok.title.n == 64 && n == 8 && longest == 256, "FILE_OK at its bound (a title of 64, 8 fields of 256): sent whole"); }
      { struct pm_file_check m = { pm_s("models"), pm_s("over") }; pm_winit(&w, b, sizeof b); pm_put_file_check(&w, 42, &m); sendw(s, &w); }
      { int t = next(s, 5000, &h, &body, &bl); struct pm_file_bad fb = { 0, { "", 0 } };
        if (t == PM_FILE_BAD) pm_get_file_bad(body, bl, &fb);
        CHECK(t == PM_FILE_BAD && h.id == 42 && fb.code == HM_FILE_BAD_OTHER && fb.why.n > 0, "a FILE_OK the protocol does not take (a title past 64): FILE_BAD, with why"); }
      /* a type this client does not read, and one of a scope not spoken (STORE_VALUE: storage, not granted), each
       * with an id: ERR_UNKNOWN, as PROTOCOL.md says (a review of 6631974, 9: dropped, hassmic waited for its time) */
      pm_winit(&w, b, sizeof b); pm_whead(&w, 999, 43); sendw(s, &w);
      { int t = next(s, 5000, &h, &body, &bl); struct pm_err er = { 0 }; if (t == PM_ERR) pm_get_err(body, bl, &er);
        CHECK(t == PM_ERR && h.id == 43 && er.code == PM_ERR_UNKNOWN, "a type it does not know, with an id: ERR_UNKNOWN"); }
      { struct pm_button m = { PM_BUTTON_KEY_ACTION, PM_BUTTON_HOW_PRESS }; pm_winit(&w, b, sizeof b); pm_put_button(&w, 44, &m); sendw(s, &w); }
      { int t = next(s, 5000, &h, &body, &bl); struct pm_err er = { 0 }; if (t == PM_ERR) pm_get_err(body, bl, &er);
        CHECK(t == PM_ERR && h.id == 44 && er.code == PM_ERR_UNKNOWN, "a message of a scope not spoken (button), with an id: ERR_UNKNOWN"); }
      pm_winit(&w, b, sizeof b); pm_whead(&w, 999, 0); sendw(s, &w);
      CHECK(next(s, 300, &h, &body, &bl) == 0, "... without an id: nothing (nothing to answer)");
      st = stop_and_wait(s, p, 3000, NULL); read_errs(e); close(s); close(e);
      CHECK(st == 0 && strstr(errs, "file_check of models/over: its answer (FILE_OK) is not one hassmic takes"), "the FILE_BAD said in the log too, and the plugin ends as asked");
      if (!strstr(errs, "file_check of")) printf("     (%s)\n", errs); }

    printf("hmplugin: %s\n", fails ? "FAILED" : "all passed");
    return fails != 0;
}
