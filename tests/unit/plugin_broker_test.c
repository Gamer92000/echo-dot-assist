/* The broker's watchdog and hassmic's crash marks (src/hassmic/plugin.c, docs/plugins.md 2.6), each rule both ways,
 * with a stand-in plugin whose end of the control socket the test holds and a clock the test moves
 * (tests/unit/plugin_harness.h):
 *   pings        three unanswered end a plugin; answered they do not; and unanswered while the broker itself was
 *                held up (its rounds a minute apart) they do not either
 *   the queue    a control socket that takes nothing for 5 s ends a plugin; one that is read again does not
 *   CPU          asked of the launcher once a window (never of an older one, never for the wake engine's); an
 *                ordinary plugin over its share by the launcher's two numbers is ended, an idle one is not
 *   crash marks  written by a fatal signal once a plugin was started, with the plugin whose message the broker's
 *                thread was reading, and with none once that message is the core's to act on; not without a plugin
 *                started, not by SIGKILL, not in a forked child; one that names a plugin counted at the next start
 *                and the plugin held back (the wake engine's: tried again after the retry time), at three all but
 *                the wake engine's, until switched on again; one that names nobody not counted; forgotten after a
 *                day up.  A fault in what the core does with a plugin's report (a detection, its list of words) is a
 *                mark of its own kind: never counted towards those three, never a refusal, and the third within
 *                the window makes that plugin's start wait the retry time, each time anew
 *   places       the wake word engine's plugin always has one of the few: the newest of the others is stopped
 *                for it; an ordinary plugin over the count is refused
 * Built with ASan and UBSan. */
#include "plugin_harness.h"
#include "plugin_index.h"
#include "buttons.h"               /* BUTTON_*: the keys main.c button_event hands plugin_button */
#include <sys/prctl.h>
#include <arpa/inet.h>
#include <math.h>

static int fails;
static void h_count(const struct plugin_entity *x, void *arg) { (void)x; (*(int *)arg)++; }
/* what the broker sent since the last look: whether a message of TYPE was among it (a stream's MUSIC_POS comes
 * between, five a second) */
static int got(int type) { uint8_t b[PM_MAX_PACKET]; ssize_t r; int seen = 0; while ((r = recv(h_peer, b, sizeof b, MSG_DONTWAIT)) > 0) seen |= r >= 2 && (b[0] | b[1] << 8) == type; return seen; }
/* the key the store gave ID's entity (or action) NAME (plugin_store.c .hakeys), 0 none */
static unsigned key_of(const char *id, const char *name, int action)
{
    const struct ps_plugin *sp; unsigned k = 0;
    ps_lock();
    if ((sp = ps_find(id))) {
        if (!action) for (int x = 0; x < sp->m.nentities; x++) { if (!strcmp(sp->m.entities[x].name, name)) k = sp->ent_key[x]; }
        else for (int x = 0; x < sp->m.nactions; x++) if (!strcmp(sp->m.actions[x].name, name)) k = sp->act_key[x];
    }
    ps_unlock();
    return k;
}
/* entry X made another plugin's, as start() makes a free entry its own (memset, no lane) */
static void h_reuse(struct plug *x) { pthread_mutex_lock(&bl); memset(x, 0, sizeof *x); snprintf(x->id, sizeof x->id, "other"); x->ctl = x->err = x->stream = x->slot = -1; x->state = P_DEAD; x->lane = -1; pthread_mutex_unlock(&bl); }
static struct plug *h_find(const char *id) { pthread_mutex_lock(&bl); struct plug *x = plug_of(id); pthread_mutex_unlock(&bl); return x; }
static void h_free(struct plug *x) { pthread_mutex_lock(&bl); x->state = P_FREE; pthread_mutex_unlock(&bl); }
static void h_exited(struct plug *x) { pthread_mutex_lock(&bl); exited(x, "it ended", 0); pthread_mutex_unlock(&bl); }
static void h_call_timers(void) { pthread_mutex_lock(&bl); call_timers(now_ms()); pthread_mutex_unlock(&bl); }
static void h_runnable(int l) { pthread_mutex_lock(&bl); launcher = l; sandbox_ok = l >= 0; feature = l >= 0; pthread_mutex_unlock(&bl); }    /* a launcher there, or none */
/* WHY names, as the one that hears every request, a plugin of the store that does (enabled, request.audio).  Not one
 * id: the refusal names the first such plugin in the store's order, which is ps_scan's readdir order of state/plugins
 * (tmpfs on a PC: reqplug first; ext4 on GitHub's runner: x-srv, installed earlier for the second press, first) */
static int names_listener(const char *why)
{
    const struct ps_plugin *l; int n, ok = 0; char at[PS_ID_MAX + 32];
    ps_lock(); n = ps_list(&l);
    for (int i = 0; i < n && !ok; i++) {
        if (!l[i].enabled || !(l[i].m.caps & PM_CAPBIT(PM_CAP_REQUEST_AUDIO))) continue;
        snprintf(at, sizeof at, "%s hears every request", l[i].m.id);
        ok = strstr(why, at) != NULL;
    }
    ps_unlock();
    return ok;
}
static void h_partner_due(int v) { pthread_mutex_lock(&bl); partner_due = v; pthread_mutex_unlock(&bl); }
static void h_stop(struct plug *x) { pthread_mutex_lock(&bl); stop(x, "test"); pthread_mutex_unlock(&bl); }
static void set_feature(int on) { pthread_mutex_lock(&bl); feature = on; relist_due = 2; pthread_mutex_unlock(&bl); }      /* (as plugin_feature leaves it) */
/* the entities and actions listed, by name and object id */
/* the CPU time of the process's other threads (hassmic's, as the stream's thread) since h_cpu_mark, in ms */
static struct timespec h_c0, h_m0;
static void h_cpu_mark(void) { clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &h_c0); clock_gettime(CLOCK_THREAD_CPUTIME_ID, &h_m0); }
static long long h_cpu_others(void)
{
    struct timespec c1, m1; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c1); clock_gettime(CLOCK_THREAD_CPUTIME_ID, &m1);
    return ((c1.tv_sec - h_c0.tv_sec) * 1000LL + (c1.tv_nsec - h_c0.tv_nsec) / 1000000) - ((m1.tv_sec - h_m0.tv_sec) * 1000LL + (m1.tv_nsec - h_m0.tv_nsec) / 1000000);
}
struct listing { int n; char names[64][120], oids[64][64]; unsigned keys[64]; int kinds[64]; };
static void h_list(const struct plugin_entity *x, void *arg) { struct listing *l = arg; if (l->n < 64) { snprintf(l->names[l->n], 120, "%s", x->name); snprintf(l->oids[l->n], 64, "%s", x->object_id); l->kinds[l->n] = x->kind; l->keys[l->n++] = x->key; } }
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s (line %d)\n", what, __LINE__); fails++; } else printf("ok   %s\n", what); } while (0)

static void rounds(int ms, int answer)     /* the broker's rounds, 250 ms apart; answer: the plugin answers its pings */
{
    for (int t = 0; t < ms; t += 250) {
        uint8_t b[64]; ssize_t r;
        h_skew_ms += 250;
        while (answer && (r = recv(h_peer, b, sizeof b, MSG_DONTWAIT)) > 0) {
            struct pm_hdr h; const uint8_t *body; size_t bl; uint8_t o[16]; struct pm_w w;
            if (pm_head(b, (size_t)r, &h, &body, &bl) || h.type != PM_PING) continue;
            pm_winit(&w, o, sizeof o); pm_put_pong(&w, h.id); message(0, o, (size_t)pm_wlen(&w));
        }
        timers();
    }
}

static void fill(struct plug *p)            /* its control socket's queue, until a send comes back */
{
    int small = 1;
    setsockopt(p->ctl, SOL_SOCKET, SO_SNDBUF, &small, sizeof small);        /* the kernel's least: a few messages */
    pthread_mutex_lock(&bl);
    for (int i = 0; i < 100000 && !p->full_since; i++) send_ok(p, 1, -1);
    pthread_mutex_unlock(&bl);
}

/* the launcher's end: how many PLUGRUN_USAGE the broker sent since the last look (slot 0's) */
static int l_peer = -1;
static int asked(void)
{
    struct plugrun_msg m; int n = 0;
    while (recv(l_peer, &m, sizeof m, MSG_DONTWAIT) == (ssize_t)sizeof m) n += m.type == PLUGRUN_USAGE && m.slot == 0;
    return n;
}
/* and its answer: PID's CPU time MS when the launcher's clock said AT */
static void used(pid_t pid, uint32_t ms, uint32_t at, int ok)
{
    struct plugrun_msg m = { PLUGRUN_USED, 0, (uint32_t)pid, ms, at, (uint32_t)ok };
    launcher_message(&m);
}
static void heard(void)                     /* a detection that holds, from the plugin of entry 0 */
{
    uint8_t b[64]; struct pm_w w; struct pm_wake_heard m = { 0, 200, 0, 0 }; struct plug *p = &plugs[0];
    p->ws[0].state = W_OPEN; snprintf(p->ws[0].word, sizeof p->ws[0].word, "word"); p->ws[0].heard = 0; p->pos = 48000;
    m.begin = 48000 - 12000; m.end = 48000 - 1600;
    wake_user_cb = h_wake_cb;
    pm_winit(&w, b, sizeof b); pm_put_wake_heard(&w, 0, &m); message(0, b, (size_t)pm_wlen(&w));
}

static int mark(char *out, size_t cap)      /* the mark's text; -1 no file */
{
    char p[300]; FILE *f; size_t n;
    snprintf(p, sizeof p, "%s/plugins/.fall", h_dir);
    if (!(f = fopen(p, "r"))) return -1;
    n = fread(out, 1, cap - 1, f); out[n] = 0; fclose(f);
    return (int)n;
}
static int cores(void)                      /* how many marks of the second kind are kept (state/plugins/.cores) */
{
    char p[300], line[80]; FILE *f; int n = 0;
    snprintf(p, sizeof p, "%s/plugins/.cores", h_dir);
    if (!(f = fopen(p, "r"))) return 0;
    while (fgets(line, sizeof line, f)) n++;
    fclose(f);
    return n;
}
static int count(void)                      /* state/plugins/.falls; -1 no file */
{
    char p[300]; FILE *f; int n = -1;
    snprintf(p, sizeof p, "%s/plugins/.falls", h_dir);
    if (!(f = fopen(p, "r"))) return -1;
    if (fscanf(f, "%d", &n) != 1) n = -2;
    fclose(f);
    return n;
}

/* a process as hassmic: armed or not, then a fatal signal on the broker's thread or another, or its own child's */
enum { F_SEGV_BROKER, F_ABORT_OTHER, F_UNARMED, F_KILLED, F_CHILD, F_SEGV_CORE, F_SEGV_WORDS, F_SEGV_CORE_OTHER };
static int fall(int how)
{
    pid_t c = fork(); int st = 0;
    if (!c) {
        signal(SIGSEGV, SIG_DFL);           /* not the sanitizer's report: the default, as on an Echo without debuggerd */
        prctl(PR_SET_DUMPABLE, 0);          /* and no core of it on the PC the test runs on */
        /* (armed anew where the test itself is by now: the line above took its handler away) */
        if (how != F_UNARMED) { if (fall_fd >= 0) { close(fall_fd); fall_fd = -1; } fall_arm(); own_pid = getpid(); }      /* (the test is armed itself in the end: this child is a hassmic of its own) */
        snprintf(plugs[0].id, sizeof plugs[0].id, H_ID); handling = 0;
        broker_tid = how == F_ABORT_OTHER ? 0 : (pid_t)syscall(SYS_gettid);
        if (how == F_KILLED) raise(SIGKILL);
        if (how == F_ABORT_OTHER) abort();
        if (how == F_CHILD) { pid_t g = fork(); if (!g) raise(SIGSEGV); waitpid(g, &st, 0); _exit(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV ? 0 : 3); }
        /* the broker's thread in a plugin's message, as F_SEGV_BROKER, but the fault is in what the core does with
         * the detection that message reported */
        if (how == F_SEGV_CORE) { h_plug(P_RUN, PM_CAPBIT(PM_CAP_WAKE)); h_fault = 1; heard(); _exit(8); }
        /* the same from its list of words, which the broker's clockwork hands to the core two seconds on */
        if (how == F_SEGV_WORDS) { struct plug *q = h_plug(P_RUN, PM_CAPBIT(PM_CAP_WAKE)); handling = -1; q->words_dirty = 1; q->words_told = 0; h_fault = 2; h_skew_ms += 3000; timers(); _exit(8); }
        /* the core at work on a plugin's report, and the fault on another thread: nothing says it is that work */
        if (how == F_SEGV_CORE_OTHER) { handling = -1; in_core = 0; broker_tid = 0; raise(SIGSEGV); _exit(9); }
        raise(SIGSEGV);
        _exit(9);                           /* not reached: the signal went on to its default */
    }
    waitpid(c, &st, 0);
    return st;
}

/* a true/false FIELD of plugin ID in /api/plugins' list (plugin_json): 1, 0, -1 not there */
static int plug_bool(const char *id, const char *field)
{
    static char o[262144]; char key[80], f[48]; const char *at, *w;
    plugin_json(o, sizeof o);
    snprintf(key, sizeof key, "{\"id\":\"%s\"", id); snprintf(f, sizeof f, "\"%s\":", field);
    if (!(at = strstr(o, key)) || !(w = strstr(at, f))) return -1;
    return !strncmp(w + strlen(f), "true", 4);
}
#define json_wake(id) plug_bool(id, "wake")
/* whether plugin ID's object in /api/plugins' list has TEXT before the next plugin's */
static int plug_json_has(const char *id, const char *text)
{
    static char o[262144]; char key[80]; const char *at, *next, *w;
    plugin_json(o, sizeof o);
    snprintf(key, sizeof key, "{\"id\":\"%s\"", id);
    if (!(at = strstr(o, key))) return 0;
    next = strstr(at + 1, "{\"id\":\"");
    return (w = strstr(at, text)) && (!next || w < next);
}
static int said(const char *file, const char *text)     /* lines of FILE with TEXT in them */
{
    FILE *f = fopen(file, "r"); char *l = NULL; size_t cap = 0; int n = 0;
    if (!f) return 0;
    while (getline(&l, &cap, f) > 0) n += !!strstr(l, text);
    free(l); fclose(f);
    return n;
}

/* plugin_net.c's answers (N2 of a review of 7be9343): by request id */
static atomic_int nd_n; static struct { uint32_t id; int err; long long at; } nd[64];
static void nd_done(const struct pnet_req *r, int fd, int err)
{
    int k = atomic_fetch_add(&nd_n, 1);
    if (fd >= 0) close(fd);
    if (k < 64) { nd[k].id = r->id; nd[k].err = fd >= 0 ? 0 : err; nd[k].at = now_ms(); }
}
static typeof(&nd[0]) nd_of(uint32_t id) { for (int k = 0; k < atomic_load(&nd_n) && k < 64; k++) if (nd[k].id == id && nd[k].at) return &nd[k]; return NULL; }

/* h_hello's: the entry's process ended and a new one was spawned (a new generation, waiting for its HELLO) */
static void h_respawn(const char *id) { (void)id; pthread_mutex_lock(&bl); if (!++gen_next) gen_next = 1; plugs[0].gen = gen_next; plugs[0].state = P_HELLO; pthread_mutex_unlock(&bl); }

/* h_between's: OTHER's start switched on, as another thread would, while a start had bl let go (P2 of a review of
 * 7be9343); once */
static const char *h_other;
static void h_start_other(const char *id)
{
    const char *o = h_other;
    if (!o || !strcmp(id, o)) return;
    h_other = NULL; plugin_enable(o, 1); start(o);
}
/* ... and a launcher slot held where places() does not look (an entry not running that has one), until released */
static struct plug *h_held; static int h_held_slot;
static void h_hold_slot(const char *id)
{
    (void)id;
    pthread_mutex_lock(&bl);
    for (int k = 0; k < PLUGRUN_SLOTS && !h_held; k++) {
        int taken = 0; for (int i = 0; i < NPLUG; i++) taken |= plugs[i].state != P_FREE && plugs[i].slot == k;
        if (!taken) for (int i = 0; i < NPLUG && !h_held; i++) if (plugs[i].state == P_DEAD && !plugs[i].want && plugs[i].slot < 0) { h_held = &plugs[i]; h_held_slot = k; h_held->slot = k; }
    }
    pthread_mutex_unlock(&bl);
}
static void h_release_slot(void) { if (h_held) { h_held->slot = -1; h_held = NULL; } }

int main(void)
{
    struct plug *p; char text[80]; int st, last = 0, n; pid_t c;
    setvbuf(stdout, NULL, _IOLBF, 0);
    ps_community(1);            /* (community plugins on, as the Plugins page's switch leaves them: the stand-ins come from files) */
    h_install();
    freopen("/dev/null", "w", stderr);      /* the broker's log lines */
    blast = now_ms();

    /* ---- the start, as a plugin makes it */
    { uint8_t b[1024]; struct pm_w w; struct pm_hello hm = { 1, { { 0 }, PM_SCOPES } };
      p = h_plug(P_HELLO, PM_CAPBIT(PM_CAP_WAKE) | PM_CAPBIT(PM_CAP_FILES));
      pm_winit(&w, b, sizeof b); pm_put_hello(&w, 1, &hm);
      for (int s = 0; s < PM_SCOPES; s++) { struct pm_hello_scopes e = { pm_s(pm_scope_name(s)), (uint16_t)pm_scope_version(s) }; pm_add_hello_scopes(&w, &e); }
      message(0, b, (size_t)pm_wlen(&w));
      { uint8_t a[PM_MAX_PACKET]; ssize_t k = recv(h_peer, a, sizeof a, MSG_DONTWAIT); struct pm_hdr hh; const uint8_t *body; size_t bl; struct pm_welcome wm; struct pm_welcome_scopes ws; char names[200] = "";
        int okw = k > 0 && !pm_head(a, (size_t)k, &hh, &body, &bl) && hh.type == PM_WELCOME && !pm_get_welcome(body, bl, &wm);
        while (okw && pm_next_welcome_scopes(&wm.scopes, &ws)) snprintf(names + strlen(names), sizeof names - strlen(names), "%.*s %u,", ws.name.n, ws.name.s, ws.version);
        CHECK(okw && !strcmp(names, "core 1,files 1,wake 1,"), "HELLO: WELCOME names what is spoken with it, its requires: core, files and wake at 1");
        if (!okw || strcmp(names, "core 1,files 1,wake 1,")) printf("     (%s)\n", names); }
      n = h_drain(&last);
      CHECK(p->state == P_CONFIG && n == 2 && last == PM_CONFIG, "... then the file list, CONFIG");
      pm_winit(&w, b, sizeof b); pm_put_ready(&w, 0); message(0, b, (size_t)pm_wlen(&w));
      CHECK(p->state == P_RUN, "READY: it runs");
      /* the process ended and the entry was started again while HELLO's answer had bl let go (WELCOME's reads of the
       * core): the new process, in P_HELLO itself, gets neither the old one's WELCOME nor CONFIG (an observation of
       * a review of 6631974: it got both, and its own HELLO was then a message out of its state) */
      p = h_plug(P_HELLO, PM_CAPBIT(PM_CAP_WAKE) | PM_CAPBIT(PM_CAP_FILES)); h_drain(NULL);
      h_hello = h_respawn;
      pm_winit(&w, b, sizeof b); pm_put_hello(&w, 1, &hm);
      for (int s = 0; s < PM_SCOPES; s++) { struct pm_hello_scopes e = { pm_s(pm_scope_name(s)), (uint16_t)pm_scope_version(s) }; pm_add_hello_scopes(&w, &e); }
      message(0, b, (size_t)pm_wlen(&w));
      h_hello = NULL;
      n = h_drain(&last);
      CHECK(n == 0 && p->state == P_HELLO, "HELLO's answer when the process changed meanwhile: nothing sent, the new one still waits for its own HELLO");
      /* the wake stream: a capture block longer than PM_WAKE_SAMPLES_MAX goes as several packets, each with its own
       * position, none longer (a review of 6631974, 9: the C client reads 4096 samples, and nothing held the
       * backend's block to it) */
      { static int16_t blk[6000]; static uint8_t got[2 * 8 + 2 * 6000 + 64]; int sv[2], np = 0, ok = 1; ssize_t k; size_t samples = 0;
        p = h_plug(P_RUN, PM_CAPBIT(PM_CAP_WAKE) | PM_CAPBIT(PM_CAP_FILES)); h_drain(NULL);
        socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv); p->stream = sv[0]; p->ws[0].state = W_OPEN;
        for (int i = 0; i < 6000; i++) blk[i] = (int16_t)i;
        wp_feed(0, blk, 6000);
        while ((k = recv(sv[1], got, sizeof got, MSG_DONTWAIT)) > 0) {
            uint64_t pos = 0; for (int i = 0; i < 8; i++) pos |= (uint64_t)got[i] << (8 * i);
            ok &= k <= 8 + 2 * PM_WAKE_SAMPLES_MAX && pos == samples && (got[8] | got[9] << 8) == (int)(samples & 0xffff);
            samples += (size_t)(k - 8) / 2; np++;
        }
        CHECK(ok && np == 2 && samples == 6000 && p->pos == 6000, "the wake stream: 6000 samples as 4096 and 1904, each packet at its own position");
        close(sv[1]); }
    }

    /* ---- versions (sdk/PROTOCOL.md "Versions"): what is spoken with a plugin is its manifest's requires.  A client
     * that does not speak a scope at the version required is refused at HELLO; a granted capability's message of a
     * scope it does not speak (or a later version) is unknown to it both ways; a packet is exactly its layout */
    { uint8_t b[1024], a[PM_MAX_PACKET]; struct pm_w w; struct pm_hdr hh; const uint8_t *body; size_t bl; ssize_t k; struct pm_refused rf; struct pm_err e;
#define HELLO_OF(skip, twice) do { struct pm_hello hm = { 1, { { 0 }, (uint16_t)(PM_SCOPES - ((skip) >= 0) + (twice)) } }; pm_winit(&w, b, sizeof b); pm_put_hello(&w, 1, &hm); \
        for (int s_ = 0; s_ < PM_SCOPES; s_++) if (s_ != (skip)) { struct pm_hello_scopes e_ = { pm_s(pm_scope_name(s_)), (uint16_t)pm_scope_version(s_) }; pm_add_hello_scopes(&w, &e_); if ((twice) && !s_) pm_add_hello_scopes(&w, &e_); } \
        message(0, b, (size_t)pm_wlen(&w)); } while (0)
#define FIRST() ((k = recv(h_peer, a, sizeof a, MSG_DONTWAIT)) > 0 && !pm_head(a, (size_t)k, &hh, &body, &bl))
      p = h_plug(P_HELLO, PM_CAPBIT(PM_CAP_WAKE) | PM_CAPBIT(PM_CAP_FILES));
      HELLO_OF(PM_SCOPE_WAKE, 0);
      CHECK(FIRST() && hh.type == PM_REFUSED && !pm_get_refused(body, bl, &rf) && rf.why == PM_REFUSED_SCOPES && p->state == P_STOPPING && !p->want && strstr(p->why, "does not speak wake 1"),
            "HELLO without wake, which its manifest requires: REFUSED (SCOPES), stopped, the card says its client does not speak wake 1");
      p = h_plug(P_HELLO, PM_CAPBIT(PM_CAP_WAKE) | PM_CAPBIT(PM_CAP_FILES));
      HELLO_OF(PM_SCOPE_BUTTON, 0);
      CHECK(FIRST() && hh.type == PM_WELCOME && p->state == P_CONFIG, "HELLO without button, which it does not require: WELCOME (a client may speak less than this build has)");
      p = h_plug(P_HELLO, PM_CAPBIT(PM_CAP_WAKE) | PM_CAPBIT(PM_CAP_FILES));
      HELLO_OF(-1, 1);
      CHECK(FIRST() && hh.type == PM_REFUSED && strstr(p->why, "twice"), "HELLO naming a scope twice: REFUSED");
      p = h_plug(P_HELLO, PM_CAPBIT(PM_CAP_WAKE) | PM_CAPBIT(PM_CAP_FILES)); p->ver[PM_SCOPE_WAKE] = 2;
      HELLO_OF(-1, 0);
      CHECK(FIRST() && hh.type == PM_REFUSED && strstr(p->why, "does not speak wake 2"), "a manifest that requires wake 2 (no build of this one's has it; set by hand): its client's wake 1 is refused");
      /* every scope required one past what its client offers: the card names those that fit whole and says that there
       * are more (finding 7 of a review of 4c55b54: the rest went without a word) */
      p = h_plug(P_HELLO, PM_CAPBIT(PM_CAP_WAKE) | PM_CAPBIT(PM_CAP_FILES));
      for (int s = 0; s < PM_SCOPES; s++) p->ver[s] = (uint16_t)(pm_scope_version(s) + 1);
      HELLO_OF(-1, 0);
      CHECK(FIRST() && hh.type == PM_REFUSED && strstr(p->why, "does not speak core 2, storage 2, ") && strstr(p->why, " and more, which its manifest requires") && strlen(p->why) < sizeof p->why - 1,
            "every scope lacking: the card names what fits, whole, and \"and more\"");
      if (!strstr(p->why, " and more")) printf("     (%s)\n", p->why);
      /* one encoded packet, one plugin (finding 7 of a review of 4c55b54): psend makes the packet what its plugin
       * speaks in place, so the buffer that went to one never goes to another */
      p = h_plug(P_RUN, PM_CAPBIT(PM_CAP_BUTTON)); h_drain(NULL);
      { struct pm_button bm = { PM_BUTTON_KEY_ACTION, PM_BUTTON_HOW_PRESS }; pm_winit(&w, b, sizeof b); pm_put_button(&w, 0, &bm);
        CHECK(!psend(p, &w, -1) && FIRST() && hh.type == PM_BUTTON, "a packet encoded for it: sent");
        CHECK(psend(p, &w, -1) == -1 && !FIRST(), "the same buffer once more: hassmic's error, not sent"); }
      /* a running plugin with button granted and button not spoken (set by hand: a manifest cannot say so) */
      p = h_plug(P_RUN, PM_CAPBIT(PM_CAP_BUTTON) | PM_CAPBIT(PM_CAP_NET_UDP)); p->ver[PM_SCOPE_BUTTON] = 0; p->ver[PM_SCOPE_NET_UDP] = 0; h_drain(NULL);
      plugin_button(BUTTON_ACTION, 0);
      CHECK(!FIRST(), "BUTTON to a plugin that does not speak button: never sent (psend), whatever it was granted");
      { struct pm_udp_peer up = { 1, 0x0a000002u, 9 }; pm_winit(&w, b, sizeof b); pm_put_udp_peer(&w, 77, &up); message(0, b, (size_t)pm_wlen(&w)); }
      CHECK(FIRST() && hh.type == PM_ERR && hh.id == 77 && !pm_get_err(body, bl, &e) && e.code == PM_ERR_UNKNOWN, "UDP_PEER of net.udp, granted and not spoken: ERR UNKNOWN (as to a hassmic without it)");
      p->caps = PM_CAPBIT(PM_CAP_BUTTON);
      { struct pm_udp_peer up = { 1, 0x0a000002u, 9 }; pm_winit(&w, b, sizeof b); pm_put_udp_peer(&w, 78, &up); message(0, b, (size_t)pm_wlen(&w)); }
      CHECK(FIRST() && hh.type == PM_ERR && !pm_get_err(body, bl, &e) && e.code == PM_ERR_DENIED, "... not granted: DENIED, as ever");
      p = h_plug(P_RUN, PM_CAPBIT(PM_CAP_STORAGE)); h_drain(NULL);
      { struct pm_store_get g = { pm_s("k") }; pm_winit(&w, b, sizeof b); pm_put_store_get(&w, 79, &g); b[pm_wlen(&w)] = 0; message(0, b, (size_t)pm_wlen(&w) + 1); }
      CHECK(FIRST() && hh.type == PM_ERR && hh.id == 79 && !pm_get_err(body, bl, &e) && e.code == PM_ERR_BAD, "a STORE_GET with a byte after its last field: BAD (the layout spoken has none)");
      { int t = 0; pm_winit(&w, b, sizeof b); pm_whead(&w, 999, 80); message(0, b, (size_t)pm_wlen(&w)); t = FIRST() && hh.type == PM_ERR && !pm_get_err(body, bl, &e) && e.code == PM_ERR_UNKNOWN;
        CHECK(t && p->state == P_RUN, "a type this build does not know: ERR UNKNOWN, never a reason to end"); }
      /* an enum's value the plugin does not speak (finding 3 of a review of 4e0a202): never sent, whatever the
       * caller (psend holds the packet to what the plugin speaks), and taken from it as malformed */
      h_drain(NULL); send_err(p, 81, 99);
      CHECK(!FIRST(), "ERR with a code ERR_* has in no version: never sent (psend)");
      send_err(p, 82, PM_ERR_BUSY);
      CHECK(FIRST() && hh.type == PM_ERR && hh.id == 82, "... one it has: sent");
      { struct pm_err ee = { 99 }; pm_winit(&w, b, sizeof b); pm_put_err(&w, 83, &ee); message(0, b, (size_t)pm_wlen(&w)); }
      CHECK(FIRST() && hh.type == PM_ERR && hh.id == 83 && !pm_get_err(body, bl, &e) && e.code == PM_ERR_BAD && p->state == P_RUN, "an ERR from it with a code ERR_* has not: BAD, never a reason to end");
#ifdef SCOPES_DEMO
      /* A value a later version brought (the demo's call 2: HANGUP_ELSEWHERE=7@2>STOPPED), to a plugin of call 1: sent
       * as the value call 1 has in its place, never dropped (finding 4 of a review of 60a8589: a version-1 plugin lost
       * its CALL_HANGUP), whoever sends it (psend: every caller); to a plugin of call 2 as it is.  A value no version
       * has: never sent, as before */
      for (int two = 0; two < 2; two++) {
          struct pm_call_hangup ch, m = { PM_HANGUP_ELSEWHERE };
          p = h_plug(P_RUN, PM_CAPBIT(PM_CAP_CALL)); p->ver[PM_SCOPE_CALL] = (uint16_t)(1 + two); h_drain(NULL);
          pm_winit(&w, b, sizeof b); pm_put_call_hangup(&w, 0, &m);
          CHECK(!psend(p, &w, -1) && FIRST() && hh.type == PM_CALL_HANGUP && !pm_get_call_hangup(body, bl, &ch) && ch.why == (two ? PM_HANGUP_ELSEWHERE : PM_HANGUP_STOPPED),
                two ? "demo: CALL_HANGUP with HANGUP_ELSEWHERE to a plugin of call 2: as it is" : "demo: CALL_HANGUP with HANGUP_ELSEWHERE (call 2) to a plugin of call 1: sent, as HANGUP_STOPPED");
      }
      { struct pm_call_hangup m = { 99 }; pm_winit(&w, b, sizeof b); pm_put_call_hangup(&w, 0, &m);
        CHECK(psend(p, &w, -1) && !FIRST(), "demo: a HANGUP_* no version has: never sent"); }
      /* A value whose fallback is none (the demo's button 2: BUTTON_KEY_MIC=4@2>-; finding 3 of a review of 4c55b54):
       * no key of button 1 is right (the action button would be a press that never happened), so a plugin of button 1
       * gets no BUTTON for it at all: psend says 0 (done with, nothing to try again), counts it and says it once a
       * start, not as hassmic's error; a plugin of button 2 gets it as it is */
      for (int two = 0; two < 2; two++) {
          struct pm_button bt, m = { PM_BUTTON_KEY_MIC, PM_BUTTON_HOW_PRESS };
          p = h_plug(P_RUN, PM_CAPBIT(PM_CAP_BUTTON)); p->ver[PM_SCOPE_BUTTON] = (uint16_t)(1 + two); h_drain(NULL);
          for (int again = 0; again < 2; again++) {
              pm_winit(&w, b, sizeof b); pm_put_button(&w, 0, &m);
              if (two) CHECK(!psend(p, &w, -1) && FIRST() && hh.type == PM_BUTTON && !pm_get_button(body, bl, &bt) && bt.key == PM_BUTTON_KEY_MIC && !p->unsent, "demo: BUTTON_KEY_MIC to a plugin of button 2: as it is");
              else CHECK(!psend(p, &w, -1) && !FIRST() && p->unsent == 1 + again && p->state == P_RUN, again ? "demo: ... and again: not sent, counted" : "demo: BUTTON_KEY_MIC (button 2, `>-`) to a plugin of button 1: not sent, 0 (done with), counted");
          }
          pm_winit(&w, b, sizeof b); m.key = PM_BUTTON_KEY_ACTION; pm_put_button(&w, 0, &m);
          CHECK(!psend(p, &w, -1) && FIRST() && hh.type == PM_BUTTON, "demo: ... the action button to the same plugin: sent");
      }
      /* ... and pressed (plugin_button): fitted before its budget is asked (finding 1 of a review of 61e3d8e), so the
       * presses of a key a plugin of button 1 never hears spend none of the five a second its keys have */
      { int u0, got_ = 0;
        p = h_plug(P_RUN, PM_CAPBIT(PM_CAP_BUTTON)); h_drain(NULL); u0 = p->unsent;
        for (int i = 0; i < 3 * BUTTON_PER_SEC; i++) plugin_button(PM_BUTTON_KEY_MIC - PM_BUTTON_KEY_ACTION, 0);
        CHECK(!FIRST() && p->unsent == u0 + 3 * BUTTON_PER_SEC && !p->button_n, "demo: fifteen presses of MIC to a plugin of button 1: none sent, each counted unsent, none past its budget");
        for (int i = 0; i < 2 * BUTTON_PER_SEC; i++) plugin_button(0, 0);
        while (FIRST()) got_ += hh.type == PM_BUTTON;
        CHECK(got_ == 2 * BUTTON_PER_SEC && !p->button_n, "demo: ... then ten of the action button at once: all ten passed on (its budget untouched)"); }
      /* psend's 0 for a packet left unsent is a message marked drop's alone: one that must arrive and would be left
       * unsent (a definition that broke mkplugmsg's rule) is hassmic's error, -1, so action_send, entity_set and the
       * told_* never take it for sent */
      { struct pm_button m = { PM_BUTTON_KEY_MIC, PM_BUTTON_HOW_PRESS }; int u0;
        p = h_plug(P_RUN, PM_CAPBIT(PM_CAP_BUTTON)); h_drain(NULL); u0 = p->unsent; h_nodrop = 1;
        pm_winit(&w, b, sizeof b); pm_put_button(&w, 0, &m);
        CHECK(psend(p, &w, -1) == -1 && !FIRST() && p->unsent == u0, "demo: a packet left unsent on a message taken for one that must arrive: -1 (hassmic's error), never 0 as if it went");
        h_nodrop = 0; }
      CHECK(pm_droppable(PM_BUTTON) && !pm_droppable(PM_ACTION) && !pm_droppable(PM_ENTITY_SET) && !pm_droppable(PM_STATE) && !pm_droppable(PM_ERR) && !pm_droppable(PM_CALL_HANGUP),
            "demo: of the messages hassmic sends, only BUTTON may be left unsent (pm_droppable)");
      printf("plugin_broker (demo): %s\n", fails ? "FAILED" : "all passed");
      return fails != 0;
#endif
#undef HELLO_OF
#undef FIRST
    }

    /* ---- pings */
    p = h_plug(P_RUN, 0); rounds(38000, 0);
    CHECK(p->state == P_RUN && p->pings == PINGS_MAX, "three pings without an answer: still running at 38 s");
    rounds(4000, 0);
    CHECK(p->state == P_STOPPING && strstr(p->why, "stopped answering"), "and ended at the fourth");
    p = h_plug(P_RUN, 0); rounds(90000, 1);
    CHECK(p->state == P_RUN && p->pings <= 1, "a plugin that answers runs on");
    p = h_plug(P_RUN, 0); timers();
    for (int i = 0; i < 8; i++) { h_skew_ms += 60000; timers(); }
    CHECK(p->state == P_RUN && p->pings == 0, "eight minutes in which the broker's thread came round eight times: no ping counted against it");
    rounds(42000, 0);
    CHECK(p->state == P_STOPPING, "and silent while the broker runs: ended as any");

    /* ---- the control socket's queue */
    p = h_plug(P_RUN, 0); fill(p);
    CHECK(p->full_since != 0, "a queue that takes no more: noted");
    rounds(4500, 0);
    CHECK(p->state == P_RUN && p->full_since, "full for 4.5 s: still running");
    rounds(1500, 0);
    CHECK(p->state == P_STOPPING && strstr(p->why, "does not read"), "full for 5 s: ended");
    p = h_plug(P_RUN, 0); fill(p); rounds(3000, 0); h_drain(NULL); rounds(500, 0);
    CHECK(p->state == P_RUN && !p->full_since, "read again after 3 s: not full any more");
    rounds(5000, 1);
    CHECK(p->state == P_RUN, "and running on");
    p = h_plug(P_RUN, 0); fill(p); timers(); h_skew_ms += 60000; timers();
    CHECK(p->state == P_RUN, "full while the broker's thread was held up for a minute: that is not 5 s of the broker's");

    /* ---- CPU: half a core over 300 ms here.  The launcher reads the time (a plugin is another user's process:
     * hassmic's own look at /proc found nothing on an Echo); the test is the launcher */
    { int sv[2]; if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv)) return 2;
      fcntl(sv[0], F_SETFL, O_NONBLOCK); fcntl(sv[1], F_SETFL, O_NONBLOCK); launcher = sv[0]; l_peer = sv[1]; launcher_ver = 2; }
    cpu_max = 50; cpu_window = 300; c = 4242;
    p = h_plug(P_RUN, 0); p->pid = 0; cpu_watch();
    CHECK(asked() == 0, "a plugin the launcher has not said SPAWNED for: nothing asked");
    p->pid = c; cpu_watch();
    CHECK(asked() == 1 && p->cpu_asked, "an ordinary plugin: the launcher is asked for its CPU time");
    cpu_watch(); h_skew_ms += 1000; cpu_watch();
    CHECK(asked() == 0, "not again while the answer is out");
    used(c, 70000, 5000, 1);
    CHECK(p->state == P_RUN && p->cpu_have && !p->cpu_asked, "the first answer: nothing to hold it against yet");
    cpu_watch(); h_skew_ms += 200; cpu_watch();
    CHECK(asked() == 0, "not asked again before the window is up");
    h_skew_ms += 150; cpu_watch();
    CHECK(asked() == 1, "asked again a window later");
    used(c, 70000 + 140, 5000 + 300, 1);
    CHECK(p->state == P_RUN && p->cpu_ms == 70140, "140 ms of CPU in 300: under its share, left alone");
    used(999, 70000 + 99999, 5000 + 600, 1);
    CHECK(p->state == P_RUN && p->cpu_ms == 70140, "an answer for another process than the slot's now: not its");
    used(c, 70140 + 10, 5300 + 20, 1);
    CHECK(p->state == P_RUN && p->cpu_ms == 70140, "a second answer right behind the first (one was late): not judged on 20 ms");
    h_skew_ms += 60000;                     /* the broker got round to it a minute late: the numbers are the launcher's */
    used(c, 70140 + 151, 5300 + 300, 1);
    CHECK(p->state == P_STOPPING && strstr(p->why, "more CPU"), "151 ms of CPU in 300: over its share, ended (whenever the broker reads the answer)");
    p = h_plug(P_RUN, 0); p->pid = c; used(c, 0xFFFFFF00u, 0xFFFFFFF0u, 1); used(c, 0x00000000u, 0x00000200u, 1);
    CHECK(p->state == P_RUN && p->cpu_have && p->cpu_ms == 0, "both numbers past 2^32 (49 days): 256 ms in 528, left alone");
    used(c, 400, 0x200 + 500, 1);
    CHECK(p->state == P_STOPPING, "and 400 ms in 500 after that: ended");
    p = h_plug(P_RUN, 0); p->pid = c; cpu_watch(); asked(); h_skew_ms += CPU_ANSWER_MS + 100; cpu_watch();
    CHECK(asked() == 1, "no answer in 5 s (the launcher's sends do not wait): asked again");
    p = h_plug(P_RUN, 0); p->pid = c; snprintf(wake_id, sizeof wake_id, H_ID); cpu_watch();
    CHECK(asked() == 0, "the wake word engine's plugin: never asked for");
    used(c, 0, 0, 1); used(c, 9000, 300, 1);
    CHECK(p->state == P_RUN && !p->cpu_have, "and whatever is said of it: never ended for working");
    wake_id[0] = 0;
    p = h_plug(P_RUN, 0); p->pid = c; used(c, 0, 0, 1); used(c, 0, 300, 0); used(c, 9000, 600, 1);
    CHECK(p->state == P_RUN && cpu_blind, "a time the launcher cannot read: said once, and nothing ended on half a reading");
    cpu_blind = 0; launcher_ver = 1;
    p = h_plug(P_RUN, 0); p->pid = c; cpu_watch(); h_skew_ms += 1000; cpu_watch();
    CHECK(asked() == 0 && cpu_blind && p->state == P_RUN, "a launcher from before the question: never asked (it would end over it), said once, plugins run on");
    launcher_ver = 2; cpu_blind = 0;

    /* ---- hassmic's own crashes */
    fall_begin();
    CHECK(falls == 0 && !falls_off && !fall_held[0] && count() == -1, "no mark: nothing counted");
    st = fall(F_UNARMED);
    CHECK(WIFSIGNALED(st) && mark(text, sizeof text) == -1, "a crash before any plugin was started: no mark");
    st = fall(F_KILLED);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL && mark(text, sizeof text) == 0, "SIGKILL (init's stop, an update, a power cut): an empty file, which is no mark");
    fall_begin();
    CHECK(falls == 0 && count() == -1, "and not counted");
    st = fall(F_CHILD);
    CHECK(WIFEXITED(st) && !WEXITSTATUS(st) && mark(text, sizeof text) == 0, "a fatal signal in a child of hassmic's: no mark");
    /* a crash that names no plugin is not the plugins': three of them, and nothing is counted or switched off */
    for (int i = 0; i < 3; i++) {
        st = fall(F_ABORT_OTHER);
        if (i == 0) CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT && mark(text, sizeof text) > 0 && !strcmp(text, "fall -\n"), "abort() on another thread: a mark that names no plugin");
        fall_begin();
    }
    CHECK(falls == 0 && !falls_off && !fall_held[0] && count() == -1 && mark(text, sizeof text) == -1, "three such crashes: marks taken away, nothing counted, nothing off");
    /* where a plugin's message ends: what the core does with a detection is the core's */
    p = h_plug(P_RUN, PM_CAPBIT(PM_CAP_WAKE)); handling = 0; h_marked = -2; heard(); handling = -1;
    CHECK(h_heard == 1 && h_marked == -1 && h_core == 0, "a detection reaches the core with no plugin named as the one whose message it is: a fault in the wake path is hassmic's own, at work on that plugin's report");
    p->ws[0].state = W_OPENING; handling = 0; h_marked = -2;
    { uint8_t b[16]; struct pm_w w; struct pm_wake_ready rm = { 0 }; pm_winit(&w, b, sizeof b); pm_put_wake_ready(&w, 0, &rm); message(0, b, (size_t)pm_wlen(&w)); }
    handling = -1;
    CHECK(p->ws[0].state == W_OPEN && h_marked == -1 && h_core == 0, "the same when the core is told that a word listens");
    /* a fault there: a mark of its own kind, which is not a plugin's fault and is not counted as one, but is kept:
     * the third within the window makes that plugin's start wait (3 in 60 s here, the retry 3 s) */
    retry_ms = 3000; core_marks = 3; core_window = 60000;
    st = fall(F_SEGV_CORE);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV && mark(text, sizeof text) > 0 && !strcmp(text, "core " H_ID "\n"), "SIGSEGV in the core's wake path, on the broker's thread, from a plugin's detection: a mark of the second kind");
    fall_begin();
    CHECK(falls == 0 && !fall_held[0] && !core_held[0] && core_count == 1 && count() == -1 && mark(text, sizeof text) == -1 && cores() == 1, "the next start: not counted against the plugins, nobody held back, the mark kept with its time");
    st = fall(F_SEGV_WORDS); mark(text, sizeof text);
    CHECK(WIFSIGNALED(st) && !strcmp(text, "core " H_ID "\n"), "the same from its list of words, when the broker hands that to the core");
    fall_begin();
    CHECK(falls == 0 && !core_held[0] && core_count == 2, "the second: still nobody held back");
    st = fall(F_SEGV_CORE_OTHER); mark(text, sizeof text);
    CHECK(!strcmp(text, "fall -\n"), "a fault on another thread while the core is at that work: names nobody");
    fall_begin();
    CHECK(core_count == 2 && cores() == 2, "and is no mark of that kind");
    fall(F_SEGV_CORE); fall_begin();
    CHECK(falls == 0 && !falls_off && !fall_held[0] && !strcmp(core_held, H_ID) && core_count == 3 && count() == -1, "the third in a minute: that plugin's start waits; still nothing counted, nothing off");
    sandbox_ok = 1; feature = 1; why_none[0] = 0;
    h_plug(P_FREE, 0); plugs[0].state = P_FREE; snprintf(wake_id, sizeof wake_id, H_ID); start(H_ID);
    CHECK(plugs[0].state == P_DEAD && plugs[0].want && plugs[0].next_start && strstr(plugs[0].why, "3 times in 1 minutes") && strstr(plugs[0].why, "a fault of hassmic's") && strstr(plugs[0].why, "tried again"),
          "the wake word engine's plugin: not started in main(), not refused, and its card says what over");
    timers(); h_skew_ms += 2500; timers(); start(H_ID);
    CHECK(plugs[0].state == P_DEAD && plugs[0].want && !strcmp(core_held, H_ID), "2.5 s later: still waiting, whoever asks");
    h_skew_ms += 600; timers();
    CHECK(plugs[0].state == P_SPAWNING && !core_held[0], "after the retry time: started, by the broker's own clockwork");
    plugs[0].state = P_DEAD; plugs[0].slot = -1; core_until = 0;
    fall(F_SEGV_CORE); fall_begin();
    CHECK(!strcmp(core_held, H_ID) && core_count == 4, "hassmic falls over it again: the next start waits again, without end");
    plugin_enable(H_ID, 1);
    CHECK(plugs[0].state == P_SPAWNING && !core_held[0], "the owner's own switch starts it at once");
    plugs[0].state = P_DEAD; plugs[0].slot = -1; wake_id[0] = 0;
    /* a plugin that is not the wake word engine's: its marks count as its crashes do.  (Four stand; the window
     * here is a minute, the retry 3 s, the third's wait the shorter of 30 s and the retry) */
    plugs[0].state = P_FREE; snprintf(core_held, sizeof core_held, H_ID); core_count = 3; core_until = 0; start(H_ID);
    CHECK(plugs[0].state == P_DEAD && plugs[0].want && plugs[0].next_start && !plugs[0].off && strstr(plugs[0].why, "tried again"), "another plugin at its third mark: its start waits, as the engine's");
    plugs[0].state = P_FREE; core_until = 0;
    fall(F_SEGV_CORE); fall_begin();
    CHECK(!strcmp(core_held, H_ID) && core_count == 5, "hassmic falls over it a fifth time within the window");
    start(H_ID);
    CHECK(plugs[0].state == P_DEAD && !plugs[0].want && plugs[0].off && !plugs[0].next_start && strstr(plugs[0].why, "switched off") && strstr(plugs[0].why, "a fault of hassmic's"),
          "...and it is switched off, as a plugin that crashed five times is: no start every five minutes for good");
    h_skew_ms += 4000; timers(); start(H_ID);
    CHECK(plugs[0].state == P_DEAD && plugs[0].off, "after the retry time: still off, whoever asks");
    snprintf(wake_id, sizeof wake_id, H_ID); plugs[0].off = 0; plugs[0].state = P_FREE; core_until = 0; start(H_ID);
    CHECK(plugs[0].state == P_DEAD && plugs[0].want && plugs[0].next_start && !plugs[0].off, "the same five marks for the wake word engine's plugin: never off, its start waits");
    wake_id[0] = 0; plugin_enable(H_ID, 1);
    CHECK(plugs[0].state == P_SPAWNING && !core_held[0] && !plugs[0].off, "the owner's own switch starts the one switched off");
    plugs[0].state = P_DEAD; plugs[0].slot = -1;
    /* community plugins switched off (docs/plugins.md 2.7): the store's flag at once, the stop and the start the
     * broker's (plugin_community leaves them to its thread: the caller holds core_lock); the stand-in is from a file */
    { int g = go; go = 1;
      plugin_community(0);
      CHECK(!ps_community(-1) && community_due, "community plugins off: the store's flag at once, the rest left to the broker's thread");
      community_due = 0; plugin_withdrawn_changed();
      plugs[0].state = P_FREE; start(H_ID);
      CHECK(plugs[0].state == P_DEAD && !plugs[0].want && plugs[0].withdrawn && strstr(plugs[0].why, "community plugins are switched off"), "the plugin from a file is not started, and its card says why");
      plugin_community(1); community_due = 0; plugin_withdrawn_changed();
      CHECK(plugs[0].state == P_SPAWNING && !plugs[0].withdrawn, "on again: the rescan starts it");
      plugs[0].state = P_DEAD; plugs[0].slot = -1; go = g; }
    h_skew_ms += 61000; fall(F_SEGV_CORE); fall_begin();
    CHECK(!core_held[0] && core_count == 1 && cores() == 1, "one more than a minute later: the old ones are forgotten, nobody waits");
    { char pth[300]; snprintf(pth, sizeof pth, "%s/plugins/.cores", h_dir); unlink(pth); }
    fall_begin();
    CHECK(falls == 0 && !fall_held[0], "(and nothing of it is counted)");
    st = fall(F_SEGV_BROKER);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV && mark(text, sizeof text) > 0 && !strcmp(text, "fall " H_ID "\n"),
          "SIGSEGV on the broker's thread in a plugin's message: the mark names the plugin, and the signal ends hassmic as before");
    fall_begin();
    CHECK(falls == 1 && !strcmp(fall_held, H_ID) && !falls_off && count() == 1 && mark(text, sizeof text) == -1, "the next start counts it, takes the mark away and holds that plugin back");
    fall_held[0] = 0; fall(F_SEGV_BROKER); fall_begin();
    CHECK(falls == 2 && !falls_off && count() == 2, "counted: two");

    /* what start() makes of it: a launcher that takes a SPAWN and says nothing */
    sandbox_ok = 1; feature = 1; why_none[0] = 0;
    h_plug(P_FREE, 0); plugs[0].state = P_FREE;
    snprintf(fall_held, sizeof fall_held, H_ID); start(H_ID);
    CHECK(plugs[0].state == P_DEAD && !plugs[0].want && !plugs[0].next_start && strstr(plugs[0].why, "held back for this run"), "the plugin the mark names is not started");
    plugin_enable(H_ID, 1);
    CHECK(plugs[0].state == P_SPAWNING && !fall_held[0], "the owner's own switch starts it");
    /* the wake engine's plugin, named by a mark: held back as one that keeps crashing, then tried (3 s here) */
    plugs[0].state = P_FREE; plugs[0].slot = -1; retry_ms = 3000;
    snprintf(fall_held, sizeof fall_held, H_ID); snprintf(wake_id, sizeof wake_id, H_ID); start(H_ID);
    CHECK(plugs[0].state == P_DEAD && plugs[0].want && plugs[0].next_start && strstr(plugs[0].why, "tried again"), "the wake word engine's plugin named by a mark: not started now, and not refused for the run");
    timers(); h_skew_ms += 2500; timers(); start(H_ID);
    CHECK(plugs[0].state == P_DEAD && plugs[0].want && !strcmp(fall_held, H_ID), "2.5 s later: still held back, whoever asks");
    h_skew_ms += 600; timers();
    CHECK(plugs[0].state == P_SPAWNING && !fall_held[0], "after the retry time: started, by the broker's own clockwork");
    plugs[0].state = P_DEAD; plugs[0].slot = -1; wake_id[0] = 0; fall_until = 0;
    h_skew_ms += 25 * 3600 * 1000LL; falls_keep();
    CHECK(falls == 0 && count() == -1, "a day up: two crashes forgotten");
    start_ms = now_ms(); falls = 2; falls_dirty = 1; falls_keep();
    st = fall(F_SEGV_BROKER); fall_begin(); fall_held[0] = 0;
    CHECK(falls == FALLS_MAX && falls_off && count() == 3, "the third in a plugin's message: plugins are off");
    h_skew_ms += 25 * 3600 * 1000LL; falls_keep();
    CHECK(falls == FALLS_MAX && count() == 3, "and that is not forgotten by itself");
    start(H_ID);
    CHECK(plugs[0].state == P_DEAD && strstr(plugs[0].why, "stay off"), "an ordinary plugin is not started");
    snprintf(wake_id, sizeof wake_id, H_ID); start(H_ID);
    CHECK(plugs[0].state == P_SPAWNING, "the wake word engine's is");
    plugs[0].state = P_DEAD; plugs[0].slot = -1; wake_id[0] = 0;
    plugin_resume(); falls_keep();
    CHECK(!falls && !falls_off && count() == -1, "switched on again: the count is gone");
    start(H_ID);
    CHECK(plugs[0].state == P_SPAWNING, "and the plugin starts");

    /* ---- places: RUN_MAX at once and run_mem MB of their memory limits, a plugin without one waits for one (and
     * starts when one is free, the oldest first), and the wake word engine's plugin always has one */
    { const struct ps_plugin *l; struct plugrun_msg gone = { PLUGRUN_EXITED, 0, 0, 0, 0, 0 }; char id[24]; int ok = 0;
      for (int k = 0; k < RUN_MAX + 1; k++) { snprintf(id, sizeof id, "other-%c", 'a' + k); h_add(id, 0); }
      ps_lock(); n = ps_scan(&l); ps_unlock();
      plugs[0].state = P_FREE; plugs[0].slot = -1; go = 1; wake_id[0] = 0; asked();
      for (int k = 0; k < RUN_MAX; k++) { snprintf(id, sizeof id, "other-%c", 'a' + k); start(id); ok += plug_of(id) && plug_of(id)->state == P_SPAWNING; }
      CHECK(n == RUN_MAX + 2 && RUN_MAX == 8 && run_mem == 128 && ok == RUN_MAX, "eight ordinary plugins started at once (phase 1 ran two)");
      snprintf(id, sizeof id, "other-%c", 'a' + RUN_MAX); start(id);
      p = plug_of(id);
      CHECK(p && p->state == P_DEAD && p->want && p->waits && !p->next_start && strstr(p->why, "waits for a place: 8 plugins run at once") && plug_json_has(id, "\"state\":\"waiting\""),
            "a ninth: it waits for a place, says why, and the page says \"waiting\"");
      /* (the broker's clock moved by less than HELLO_MS * 2 in all below: the launcher never answers here, and a start
       * it does not answer in 10 s ends as a crash, which frees a place) */
      h_skew_ms += 2000; timers();
      CHECK(p->state == P_DEAD && p->waits, "no place free: it goes on waiting, nothing started behind anyone's back");
      { struct plug *a = plug_of("other-c"); a->want = 0; stop(a, "switched off"); gone.slot = (uint32_t)a->slot; launcher_message(&gone); }
      h_skew_ms += 300; timers();
      CHECK(p->state == P_SPAWNING && !p->waits && plug_of("other-c")->state == P_DEAD, "one switched off and gone: the one that waited takes its place at once");
      /* the wake word engine's plugin: the newest of the others is stopped for it and waits for a place itself */
      snprintf(wake_id, sizeof wake_id, H_ID); start(H_ID);
      { struct plug *w = plug_of(H_ID), *v = plug_of(id);
        CHECK(w && w->state == P_DEAD && w->want && w->room && w->next_start && v->state == P_STOPPING && v->want && v->waits && strstr(v->why, "stopped for the wake word engine's plugin"),
              "the wake word engine's plugin with every place taken: the newest of the others stopped for it, which waits for a place again");
        h_skew_ms += 300; timers();
        CHECK(w->state == P_DEAD && v->state == P_STOPPING && plug_of("other-a")->state == P_SPAWNING, "while that one is on its way out: nobody else stopped");
        gone.slot = (uint32_t)v->slot; launcher_message(&gone);
        /* P6 of a review of 7be9343: the place it left is the engine's, which holds it while it waits for its start
         * (room): a round before the engine's start is due starts nobody that waits in its place */
        h_skew_ms += 100; timers();
        CHECK(w->state == P_DEAD && w->room && v->state == P_DEAD && v->waits, "P6: gone, the engine's start not due yet: the place it left is the engine's, the one that waits is not started in it");
        h_skew_ms += 300; timers();
        CHECK(w->state == P_SPAWNING && v->state == P_DEAD && v->waits, "once it is gone: the engine's plugin started; the one stopped for it waits (the engine's place is not taken from it)");
        CHECK(v->ncrash == 0 && !v->off && v->want, "P1: stopped to make room for the engine: no crash counted");
        h_skew_ms += 2000; timers();
        CHECK(v->state == P_DEAD && v->waits, "... and goes on waiting while every place is taken");
        /* P3: the engine down between a crash and its start again (1 s): its place stays its own; the one that waits
         * is not started in it, to be stopped again at the engine's start */
        { struct plugrun_msg crash = { PLUGRUN_EXITED, 0, 9, 0, 0, 0 };
          w->state = P_RUN; w->ping_at = btime + 1000000; crash.slot = (uint32_t)w->slot; launcher_message(&crash);
          CHECK(w->state == P_DEAD && w->want && w->next_start, "(the engine's plugin crashed: started again in 1 s)");
          h_skew_ms += 300; timers();
          CHECK(w->state == P_DEAD && v->state == P_DEAD && v->waits, "P3: the engine down between a crash and its start: its place is not given to the one that waits");
          h_skew_ms += 900; timers();
          CHECK(w->state == P_SPAWNING && v->state == P_DEAD && v->waits && v->ncrash == 0, "P3: ... the engine started again in it; nobody stopped for it a second time"); }
        /* P1: stopped for the engine's place five times over (the engine crashing and taking it each time): never a
         * crash of its own, never switched off.  Counted as crashes, the fifth switched a healthy plugin off */
        { struct plug *x = plug_of("other-b"); int sl = x->slot, ok = 1;
          for (int k = 0; k < 6; k++) {
              x->state = P_RUN; x->slot = sl; x->want = 1; x->waits = 1; x->wait_since = now_ms();
              stop(x, "stopped for the wake word engine's plugin, which always has a place: it starts again when a place is free");
              gone.slot = (uint32_t)sl; launcher_message(&gone);
              ok &= x->state == P_DEAD && x->ncrash == 0 && !x->off && x->want;
          }
          CHECK(ok && plug_json_has("other-b", "\"state\":\"waiting\""), "P1: stopped six times to make room for the engine: no crash counted, never switched off, waiting");
          x->state = P_RUN; x->slot = sl; x->waits = 0; }
        { struct plug *a = plug_of("other-a"); a->want = 0; stop(a, "switched off"); gone.slot = (uint32_t)a->slot; launcher_message(&gone); }
        h_skew_ms += 300; timers();
        CHECK(v->state == P_SPAWNING, "a place free: the one stopped for the engine runs again by itself (it was off until its owner switched it on, in phase 1)");
        /* P2 of a review of 7be9343: the engine's start again after a crash and another plugin's start at once (the page
         * switches one on as the broker restarts the engine): while the engine's start has bl let go for its program,
         * the other's may not take the place, and with it the last launcher slot, the engine is still owed */
        { struct plug *c = plug_of("other-c"); struct plugrun_msg crash = { PLUGRUN_EXITED, 0, 9, 0, 0, 0 }; int used, mem;
          v->want = 0; stop(v, "switched off"); gone.slot = (uint32_t)v->slot; launcher_message(&gone);
          start("other-c");
          places(NULL, &used, &mem);
          CHECK(c->state == P_SPAWNING && w->state == P_SPAWNING && used == RUN_MAX, "(seven others and the engine: every place taken)");
          for (int i = 0; i < NPLUG; i++) if (running(&plugs[i])) { plugs[i].state = P_RUN; plugs[i].ping_at = btime + 1000000; }
          w->ncrash = 0; crash.slot = (uint32_t)w->slot; launcher_message(&crash);     /* (its first crash: again in 1 s) */
          h_between = h_start_other; h_other = "other-i";
          h_skew_ms += 1100; timers();
          h_between = NULL;
          CHECK(h_other == NULL && w->state == P_SPAWNING && v->state == P_DEAD && v->waits, "P2: another start while the engine's had bl let go: it waits, the engine has its place and its launcher slot");
          /* ... and wanted, left without a launcher slot all the same (one held where places() does not look, here by
           * hand): asked again in a moment, never left down */
          for (int i = 0; i < NPLUG; i++) if (running(&plugs[i])) { plugs[i].state = P_RUN; plugs[i].ping_at = btime + 1000000; }
          w->ncrash = 0; crash.slot = (uint32_t)w->slot; launcher_message(&crash);
          h_between = h_hold_slot;
          h_skew_ms += 1100; timers();
          h_between = NULL;
          CHECK(w->state == P_DEAD && w->want && w->next_start && w->room, "P2: no launcher slot free as it was to spawn: asked again in a moment, as one that waits for room");
          h_release_slot();
          h_skew_ms += 300; timers();
          CHECK(w->state == P_SPAWNING && v->state == P_DEAD, "P2: ... and started once a slot is free");
          /* F1 of a review of 5aa32ee: the engine's start again after a crash fails before the launcher has it (its
           * program cannot be opened: an install renaming it in, descriptors run out).  It was left P_DEAD, wanted,
           * with no next_start: never started again, and its place (places() counts the engine whenever it is
           * wanted) held from every plugin that waits, for good */
          { char pp[500], pq[520]; int used0, mem0, used1, mem1;
            for (int i = 0; i < NPLUG; i++) if (running(&plugs[i])) { plugs[i].state = P_RUN; plugs[i].ping_at = btime + 1000000; }
            ps_program(H_ID, pp, sizeof pp); snprintf(pq, sizeof pq, "%s.away", pp); rename(pp, pq);
            w->ncrash = 0; crash.slot = (uint32_t)w->slot; launcher_message(&crash);
            places(NULL, &used0, &mem0);
            h_skew_ms += 1100; timers();
            CHECK(w->state == P_DEAD && w->want && w->next_start && w->next_start - now_ms() <= 1000 && strstr(w->why, "could not be started")
                  && plug_json_has(H_ID, "\"state\":\"restarting\""), "F1: the engine's program not opened: asked again in 1 s, the card says why");
            h_skew_ms += 1100; timers();
            CHECK(w->state == P_DEAD && w->next_start && w->next_start - now_ms() > 1000 && w->next_start - now_ms() <= 5000, "F1: ... failing again: the next in 5 s, as a crash's back-off");
            CHECK(w->ncrash == 1 && !w->off, "F1: ... never counted as a crash");
            rename(pq, pp);                                 /* (the failure was a passing one) */
            for (int k = 0; k < 20 && w->state == P_DEAD; k++) { h_skew_ms += 300; timers(); }
            places(NULL, &used1, &mem1);
            CHECK(w->state == P_SPAWNING && w->failn == 0 && v->state == P_DEAD && v->waits && used0 == RUN_MAX && used1 == RUN_MAX,
                  "F1: ... the program back: the engine started again in its place, the one that waits still waits"); } } }
      /* memory: the limits of those that run, together, within run_mem */
      for (int i = 0; i < NPLUG; i++) if (plugs[i].state != P_FREE) { plugs[i].state = P_DEAD; plugs[i].slot = -1; plugs[i].want = 0; plugs[i].waits = 0; }
      wake_id[0] = 0;
      h_mem = 96; h_add_full("big-a", "\"files\": 1", "", "big a\n"); h_add_full("big-b", "\"files\": 1", "", "big b\n"); h_mem = 16;
      ps_lock(); ps_scan(&l); ps_unlock();
      start("big-a"); start("other-a"); start("big-b");
      p = plug_of("big-b");
      CHECK(plug_of("big-a")->state == P_SPAWNING && plug_of("big-a")->mem == 96 && plug_of("other-a")->state == P_SPAWNING && p && p->state == P_DEAD && p->waits
            && strstr(p->why, "may use 128 MB of memory between them, 112 MB are taken and this one may use 96 MB"),
            "two plugins of 96 MB: the second waits (96 + 16 + 96 over 128 MB), and says how much is taken");
      start("other-b");
      CHECK(plug_of("other-b")->state == P_SPAWNING, "... a plugin of 16 MB that fits beside them starts (a place is not kept empty for the larger one)");
      /* a place free by count and 16 MB by memory: the one of 16 MB that waits starts in it, though the one of 96 MB
       * waited longer */
      start("other-c");
      CHECK(plug_of("other-c")->state == P_DEAD && plug_of("other-c")->waits, "(another of 16 MB with 128 MB taken: it waits too, after the one of 96 MB)");
      { struct plug *b = plug_of("other-b"); b->want = 0; stop(b, "switched off"); gone.slot = (uint32_t)b->slot; launcher_message(&gone); }
      h_skew_ms += 300; timers();
      CHECK(plug_of("other-c")->state == P_SPAWNING && p->state == P_DEAD && p->waits, "16 MB free: the one of 16 MB starts in it, the one of 96 MB that waited longer goes on waiting");
      { struct plug *x = plug_of("other-c"); x->want = 0; stop(x, "switched off"); gone.slot = (uint32_t)x->slot; launcher_message(&gone); }
      start("other-b");
      { struct plug *a = plug_of("big-a"); a->want = 0; stop(a, "switched off"); gone.slot = (uint32_t)a->slot; launcher_message(&gone); }
      h_skew_ms += 300; timers();
      CHECK(p->state == P_SPAWNING, "the first one of 96 MB gone: the second one starts");
      /* the places asked again at the spawn: the list read before an update had the plugin at 16 MB, the package in its
       * folder (read again by program_open) may use 96 (two starts at once are the other way to get there) */
      { struct plug *g, *b = plug_of("other-b");
        h_add_full("grow", "\"files\": 1", "", "grow 1\n"); ps_lock(); ps_scan(&l); ps_unlock();
        h_mem = 96; h_add_full("grow", "\"files\": 1", "", "grow 2\n"); h_mem = 16;
        b->want = 0; stop(b, "switched off"); gone.slot = (uint32_t)b->slot; launcher_message(&gone);       /* (112 of 128 MB taken) */
        start("grow"); g = plug_of("grow");
        CHECK(g && g->state == P_DEAD && g->want && g->waits && g->mem == 96 && strstr(g->why, "none was left as it started"),
              "a place that was free for the list's 16 MB, not for the 96 MB of the version it starts as: it waits, never over the bound");
        ps_remove("grow"); if (g) g->state = P_FREE; }
      for (int i = 0; i < NPLUG; i++) if (plugs[i].state != P_FREE) { plugs[i].state = P_DEAD; plugs[i].slot = -1; plugs[i].want = 0; plugs[i].waits = 0; }
      /* P6 of a review of 7be9343: two that wait, of different ages: the one that waited longer starts first */
      { struct plug *i9, *sd; ok = 0;
        for (int k = 0; k < RUN_MAX; k++) { snprintf(id, sizeof id, "other-%c", 'a' + k); plugin_enable(id, 1); start(id); ok += plug_of(id)->state == P_SPAWNING; }
        snprintf(id, sizeof id, "other-%c", 'a' + RUN_MAX); start(id); i9 = plug_of(id);
        h_skew_ms += 500; start(H_ID); sd = plug_of(H_ID);
        CHECK(ok == RUN_MAX && i9->waits && sd && sd->waits && sd->wait_since > i9->wait_since, "(eight running; two wait, the stand-in the shorter time)");
        { struct plug *a = plug_of("other-a"); a->want = 0; stop(a, "switched off"); gone.slot = (uint32_t)a->slot; launcher_message(&gone); }
        h_skew_ms += 300; timers();
        CHECK(i9->state == P_SPAWNING && sd->state == P_DEAD && sd->waits, "P6: a place free: the one that waited longer takes it, the other goes on waiting");
        for (int i = 0; i < NPLUG; i++) if (plugs[i].state != P_FREE) { plugs[i].state = P_DEAD; plugs[i].slot = -1; plugs[i].want = 0; plugs[i].waits = 0; } }
      /* P6 of a review of 7be9343: the last place by count, 16 MB left: the timers' round holds each that waits to the
       * memory left, so the one of 96 MB that waited longer does not take the round's one place and leave the one of
       * 16 MB waiting behind it (for good: the next round picks it first again) */
      { struct plug *bb, *ii; ok = 0;
        for (int k = 0; k < RUN_MAX; k++) { snprintf(id, sizeof id, "other-%c", 'a' + k); plugin_enable(id, 1); start(id); ok += plug_of(id)->state == P_SPAWNING; }
        start("big-b"); bb = plug_of("big-b");
        h_skew_ms += 500; snprintf(id, sizeof id, "other-%c", 'a' + RUN_MAX); start(id); ii = plug_of(id);
        CHECK(ok == RUN_MAX && bb->waits && ii->waits && bb->wait_since < ii->wait_since, "(eight of 16 MB running; one of 96 MB waits, then one of 16)");
        { struct plug *a = plug_of("other-h"); a->want = 0; stop(a, "switched off"); gone.slot = (uint32_t)a->slot; launcher_message(&gone); }
        h_skew_ms += 300; timers(); h_skew_ms += 300; timers();
        CHECK(ii->state == P_SPAWNING && bb->state == P_DEAD && bb->waits, "P6: one place and 16 MB free: the one of 16 MB starts, the one of 96 MB that waited longer does not hold it up");
        for (int i = 0; i < NPLUG; i++) if (plugs[i].state != P_FREE) { plugs[i].state = P_DEAD; plugs[i].slot = -1; plugs[i].want = 0; plugs[i].waits = 0; } }
      /* P5: the places' memory from /proc/meminfo's MemTotal, which leaves the kernel and what is reserved out: a Dot
       * 2's 512 MB read as some 470 MB still gets 128 MB, eight plugins of 16 MB (4.1) */
      CHECK(run_mem_of(470 * 1024L) == 128 && run_mem_of(512 * 1024L) == 128 && run_mem_of(1900 * 1024L) == 128 && run_mem_of(0) == 128 && run_mem_of(-1) == 128
            && run_mem_of(230 * 1024L) == 64 && run_mem_of(100 * 1024L) == 32 && run_mem_of(1024) == 32,
            "P5: MemTotal rounded up to the next 128 MB before the quarter: 470 MB (a Dot 2) 128 MB, 230 MB 64 MB, 100 MB the 32 MB at least");
      /* (the store holds PS_MAX: the plugins of this part go, but for the three the tests below go on with) */
      for (int k = 3; k < RUN_MAX + 1; k++) { snprintf(id, sizeof id, "other-%c", 'a' + k); ps_remove(id); if ((p = plug_of(id))) p->state = P_FREE; }
      ps_remove("big-a"); ps_remove("big-b");
      if ((p = plug_of("big-a"))) p->state = P_FREE;
      if ((p = plug_of("big-b"))) p->state = P_FREE;
      ps_lock(); ps_scan(&l); ps_unlock(); }

    /* ---- the program in the folder is the one the manifest names, or it is not started (a package whose members
     * did not belong together ran: an update that broke off after its program was written) */
    { char rel[120]; struct plug *c;
      snprintf(rel, sizeof rel, "plugins/other-c/pkg/plugin"); h_file(rel, "another program\n");
      for (int i = 0; i < NPLUG; i++) if (plugs[i].state != P_FREE && strcmp(plugs[i].id, "other-c")) { plugs[i].state = P_DEAD; plugs[i].slot = -1; plugs[i].want = 0; }
      wake_id[0] = 0; start("other-c"); c = plug_of("other-c");
      CHECK(c && c->state == P_DEAD && !c->want && !c->next_start && strstr(c->why, "not the one its manifest names"), "a program that is not the one its manifest names: not started, with why");
      h_skew_ms += 6000; timers();
      CHECK(c->state == P_DEAD, "...nor by the broker's clockwork later");
      h_add("other-c", 0); start("other-c");
      CHECK(c->state == P_SPAWNING, "the right program back: started");
      /* an update put another package in the folder (its renames, under the store's lock) and the list of plugins
       * has not been read again yet: the start reads it again before it calls a healthy plugin broken (it held
       * the old manifest's digest against the new program: "not the one its manifest names ... Remove it") */
      c->state = P_DEAD; c->slot = -1; c->want = 0;
      h_add_program("other-c", 0, "the next version's program\n"); start("other-c");
      CHECK(c->state == P_SPAWNING && !strstr(c->why, "not the one"), "a package replaced since the list was read: read again, and started");
      /* ...and the new package's manifest says something else of the program (here `wake` added; one dropped is
       * the same code): the broker holds the program to the manifest it was started from, never to the one the
       * list had before the reread (digest new, capabilities and limits old) */
      c->state = P_DEAD; c->slot = -1; c->want = 0;
      h_add_program("other-c", 1, "a third version's program\n"); start("other-c");
      { const struct ps_plugin *sp; pm_caps_t caps;
        ps_lock(); sp = ps_find("other-c"); caps = sp ? sp->m.caps : 0; ps_unlock();
        CHECK(c->state == P_SPAWNING && (caps & PM_CAPBIT(PM_CAP_WAKE)) && c->caps == caps, "...with another capability set: the program runs under its own manifest's capabilities"); } }

    /* ---- an item the plugin cannot look at (it does not run): the page's upload keeps it unchecked for its next
     * start; the move from the built-in microWakeWord (web.c mww_migrate) keeps nothing, and says so, since what it
     * calls copied is what the plugin took */
    { const struct ps_plugin *sp; char err[160] = "", path[400]; int r1, r2; struct stat st2;
      p = h_plug(P_DEAD, PM_CAPBIT(PM_CAP_WAKE) | PM_CAPBIT(PM_CAP_FILES));
      ps_lock(); sp = ps_find(H_ID);
      r1 = sp && !ps_item_begin(sp, "models", "moved", err, sizeof err) && !ps_item_chunk(sp, "models", "moved", ".model", 0, "M\n", 2, err, sizeof err); ps_unlock();
      r2 = plugin_item_checked_only(H_ID, "models", "moved", err, sizeof err);
      snprintf(path, sizeof path, "%s/plugins/%s/files/models/moved", h_dir, H_ID);
      CHECK(r1 && r2 == 0 && strstr(err, "does not run") && stat(path, &st2), "the move's copy while the plugin does not run: nothing kept, said");
      ps_lock(); sp = ps_find(H_ID);
      r1 = sp && !ps_item_begin(sp, "models", "page", err, sizeof err) && !ps_item_chunk(sp, "models", "page", ".model", 0, "P\n", 2, err, sizeof err); ps_unlock();
      r2 = plugin_item_uploaded(H_ID, "models", "page", err, sizeof err);
      snprintf(path, sizeof path, "%s/plugins/%s/files/models/page/file.model", h_dir, H_ID);
      CHECK(r1 && r2 == 2 && !stat(path, &st2), "the page's upload meanwhile: kept unchecked, for the plugin's next start"); }

    /* ---- storage, the log, the network, as the broker answers them (docs/plugins.md phase 2) */
    { const struct ps_plugin *l; int last = 0; char logp[300]; FILE *lf;
      pm_caps_t caps = PM_CAPBIT(PM_CAP_STORAGE) | PM_CAPBIT(PM_CAP_LOG_READ) | PM_CAPBIT(PM_CAP_NET_SERVER);
      h_add_full("netplug", "\"storage\": 1, \"log.read\": true, \"net.server\": [ { \"setting\": \"server\", \"proto\": \"udp\", \"ports\": [ 514 ] } ]",
                 "{ \"name\": \"server\", \"label\": \"Server\", \"type\": \"host\" },", "the forwarder's program\n");
      ps_lock(); ps_scan(&l); ps_unlock();
      p = h_plug(P_RUN, caps); h_as(p, "netplug"); p->tokens = STORE_BUCKET; p->token_at = now_ms(); h_drain(NULL);
      #define SEND(put, ...) do { uint8_t b_[PM_MAX_PACKET]; struct pm_w w_; pm_winit(&w_, b_, sizeof b_); put(&w_, __VA_ARGS__); message(0, b_, (size_t)pm_wlen(&w_)); } while (0)
      { struct pm_store_put m = { pm_s("cursor"), pm_b("abc", 3) }; int ok = 0, busy = 0;
        for (int i = 0; i < 12; i++) { SEND(pm_put_store_put, 50 + (uint32_t)i, &m); h_drain(&last); ok += last == PM_OK; busy += last == PM_ERR; }
        CHECK(ok == STORE_BUCKET && busy == 2, "storage: ten writes, then refused (BUSY) until the bucket refills");
        h_skew_ms += STORE_REFILL; SEND(pm_put_store_put, 70, &m); h_drain(&last);
        CHECK(last == PM_OK, "a minute later one more"); }
      { struct pm_store_get g = { pm_s("cursor") }; uint8_t b[PM_MAX_PACKET]; ssize_t r; struct pm_hdr h; const uint8_t *body; size_t bl; struct pm_store_value v;
        SEND(pm_put_store_get, 71, &g);
        r = recv(h_peer, b, sizeof b, MSG_DONTWAIT);
        CHECK(r > 0 && !pm_head(b, (size_t)r, &h, &body, &bl) && h.type == PM_STORE_VALUE && h.id == 71 && !pm_get_store_value(body, bl, &v) && v.value.n == 3 && !memcmp(v.value.s, "abc", 3),
              "read back as STORE_VALUE, its id"); }
      { static uint8_t big[1100]; struct pm_store_put m = { pm_s("big"), pm_b(big, sizeof big) }; struct pm_store_put bad = { pm_s("../x"), pm_b("x", 1) }; int t0 = p->tokens;
        h_skew_ms += STORE_REFILL * 3;
        SEND(pm_put_store_put, 72, &m); h_drain(&last);
        CHECK(last == PM_ERR && p->tokens >= t0, "over its 1 kB: refused (LIMIT), and the write not spent");
        SEND(pm_put_store_put, 73, &bad); h_drain(&last);
        CHECK(last == PM_ERR, "a key that is none: refused");
        /* F1 (the syslog plugin's tests): after STOP its storage is still answered (its cursor, as it ends), nothing else */
        { struct pm_store_put cm = { pm_s("cursor"), pm_b("end", 3) }; struct pm_log_follow lf0 = { pm_s("") };
          h_skew_ms += STORE_REFILL; p->state = P_STOPPING;
          SEND(pm_put_store_put, 74, &cm); h_drain(&last);
          CHECK(last == PM_OK, "STORE_PUT between STOP and the kill: answered (OK), written");
          last = 0; SEND(pm_put_log_follow, 75, &lf0); h_drain(&last);
          CHECK(last == PM_ERR, "... any other request then: FAILED at once (R2: left unanswered, it held the client until the kill, its stop callback never run)");
          p->state = P_RUN; } }
      /* the network: what the owner entered, nothing else */
      { struct pm_connect c = { PM_CAP_NET_SERVER, 0, pm_s(""), 0 }; struct pm_connect other = { PM_CAP_NET_SERVER, 0, pm_s("elsewhere.example"), 0 };
        struct pm_connect port = { PM_CAP_NET_SERVER, 0, pm_s(""), 22 }, entry = { PM_CAP_NET_SERVER, 1, pm_s(""), 0 }, lan = { PM_CAP_NET_LAN, 0, pm_s(""), 0 };
        uint8_t b[64]; ssize_t r; struct pm_hdr h; const uint8_t *body; size_t bl; struct pm_err e = { 0 };
        #define ERRCODE() (r = recv(h_peer, b, sizeof b, MSG_DONTWAIT), r > 0 && !pm_head(b, (size_t)r, &h, &body, &bl) && h.type == PM_ERR && !pm_get_err(body, bl, &e) ? e.code : 0)
        SEND(pm_put_connect, 80, &c);
        CHECK(ERRCODE() == PM_ERR_UNSET, "no server entered: UNSET");
        { const struct ps_plugin *sp; char err[100]; ps_lock(); sp = ps_find("netplug"); ps_config_set(sp, "server", "127.0.0.1", err, sizeof err); ps_unlock(); }
        SEND(pm_put_connect, 81, &other);
        CHECK(ERRCODE() == PM_ERR_DENIED, "another host than the one entered: DENIED, before any lookup");
        SEND(pm_put_connect, 82, &port);
        CHECK(ERRCODE() == PM_ERR_DENIED, "a port the entry does not list: DENIED");
        SEND(pm_put_connect, 83, &entry);
        CHECK(ERRCODE() == PM_ERR_DENIED, "an entry the manifest has not: DENIED");
        SEND(pm_put_connect, 84, &lan);
        CHECK(ERRCODE() == PM_ERR_DENIED, "another capability's kind: DENIED");
        SEND(pm_put_connect, 85, &c);
        for (int i = 0; i < 100 && (r = recv(h_peer, b, sizeof b, MSG_DONTWAIT)) <= 0; i++) usleep(20000);
        CHECK(r > 0 && !pm_head(b, (size_t)r, &h, &body, &bl) && h.type == PM_ERR && h.id == 85 && !pm_get_err(body, bl, &e) && e.code == PM_ERR_DENIED && !p->net_pending,
              "the server entered is loopback: refused by the worker (DENIED), nothing pending");
        /* (10.0.0.9 is reachable where this PC routes it out of an interface: `ip route get` says so; in a namespace
         * with only lo it is not, and pnet_address_ok says no as well) */
        int routed = !system("ip route get 10.0.0.9 2>/dev/null | grep -q ' dev ' && ! ip route get 10.0.0.9 2>/dev/null | grep -qE '^(local|broadcast)| dev lo '");
        /* N4 of a review of 08399dc: no CONNECT before READY (it may take 10 s, READY is due in 5): BAD, nothing asked */
        p->state = P_CONFIG; SEND(pm_put_connect, 86, &c);
        CHECK(ERRCODE() == PM_ERR_BAD && !p->net_pending, "a CONNECT before READY: BAD, nothing asked of the workers");
        p->state = P_RUN;
        /* a request of a process that ended is not worked on (net_live), and the answer of one goes to nobody */
        { struct pnet_req rq = { 0, p->gen, 5, "netplug", "127.0.0.1", 514, 1 };
          CHECK(net_live(&rq), "(the process that asked runs)");
          rq.gen = p->gen + 1; CHECK(!net_live(&rq), "another start's request: not alive, dropped by the worker"); }
        /* thirty in any minute, a window that moves: not sixty around a minute's turn (N6) */
        { int busy = 0; memset(p->net_asked, 0, sizeof p->net_asked); p->net_next = 0; p->net_pending = 0;
          for (int i = 0; i < NET_MINUTE; i++) { p->net_asked[p->net_next] = now_ms() - 30000; p->net_next = (p->net_next + 1) % NET_MINUTE; }
          SEND(pm_put_connect, 87, &c); busy = ERRCODE() == PM_ERR_BUSY;
          CHECK(busy && !p->net_pending, "thirty asked in the last 30 s: the next refused (BUSY), though a new minute began");
          h_skew_ms += 31000; SEND(pm_put_connect, 88, &c);
          for (int i = 0; i < 100 && (r = recv(h_peer, b, sizeof b, MSG_DONTWAIT)) <= 0; i++) usleep(20000);
          CHECK(r > 0 && !pm_head(b, (size_t)r, &h, &body, &bl) && h.id == 88 && h.type == PM_ERR && !pm_get_err(body, bl, &e) && e.code == PM_ERR_DENIED,
                "a minute after the oldest of them: asked of the worker (loopback: DENIED there)"); }
        CHECK(!pnet_address_ok(inet_addr("127.0.0.2"), 514) && !pnet_address_ok(inet_addr("0.0.0.0"), 514) && !pnet_address_ok(inet_addr("224.0.0.251"), 514)
              && !pnet_address_ok(inet_addr("255.255.255.255"), 514) && !pnet_address_ok(inet_addr("10.0.0.9"), 5555) && pnet_address_ok(inet_addr("10.0.0.9"), 514) == routed
              && pnet_local(inet_addr("192.168.1.2")) && pnet_local(inet_addr("169.254.0.1")) && !pnet_local(inet_addr("8.8.8.8")) && !pnet_local(inet_addr("172.32.0.1")),
              "the addresses no plugin reaches whatever its entry (loopback, 0/8, multicast, broadcast, port 5555); what counts as local"); }
      /* the log: from the start, filtered, with a cursor; a send that does not fit is sent again from the same place */
      snprintf(logp, sizeof logp, "%s/boot.log", h_dir); setenv("HASSMIC_LOG", logp, 1);
      lf = fopen(logp, "w");
      fprintf(lf, "boot+1.000 the first line of the log, long enough to name it: sixty-four bytes and more\n");
      fprintf(lf, "boot+1.001 " LOG_PRIVATE_MARK "transcript: what was said\n");
      fprintf(lf, "boot+1.002 plugin " H_ID ": a wake plugin's own line\n");
      fprintf(lf, "boot+1.003 a public line aa:bb:cc:dd:ee:ff\n");
      fclose(lf);
      { struct pm_log_follow f = { pm_s("start") }, badc = { pm_s("not a cursor") }; uint8_t b[PM_MAX_PACKET]; ssize_t r; struct pm_hdr h; const uint8_t *body; size_t bl; struct pm_log lg; struct pm_log_lines e;
        char got[4][200]; int n = 0;
        SEND(pm_put_log_follow, 90, &badc); h_drain(&last);
        CHECK(last == PM_ERR && !p->log_on, "a cursor that is none: refused");
        SEND(pm_put_log_follow, 91, &f); h_drain(&last);
        CHECK(last == PM_OK && p->log_on, "follows the log from its start");
        log_pump();
        r = recv(h_peer, b, sizeof b, MSG_DONTWAIT);
        if (r > 0 && !pm_head(b, (size_t)r, &h, &body, &bl) && h.type == PM_LOG && !pm_get_log(body, bl, &lg)) while (n < 4 && pm_next_log_lines(&lg.lines, &e)) pm_cstr(got[n++], sizeof got[0], e.line);
        CHECK(n == 2 && strstr(got[0], "the first line") && strstr(got[1], "a public line **:**:**:**:**:**"), "LOG: hassmic's lines, without the private one and the wake plugin's, addresses blanked");
        /* R9 of a review of 0dabfa6: one LOG at a time, the next once the client answered this one (OK, its id) */
        { uint32_t lid = h.id; struct logpos at = p->log_at; uint8_t ok[16]; struct pm_w w;
          lf = fopen(logp, "a"); fprintf(lf, "boot+1.0031 after the first LOG\n"); fclose(lf);
          p->log_next = 0; log_pump();
          CHECK(lid && p->log_wait == lid && recv(h_peer, b, sizeof b, MSG_DONTWAIT) < 0 && !memcmp(&at, &p->log_at, sizeof at), "R9: a LOG has an id; no next one before its OK");
          /* a review of 7144548, 2: a client from before flow control never answers; said once, in the log and on its card */
          { char logq[300]; snprintf(logq, sizeof logq, "%s/unanswered.log", h_dir); freopen(logq, "w", stderr); setvbuf(stderr, NULL, _IONBF, 0);
            rounds(9000, 1);
            CHECK(!said(logq, "does not answer LOG") && plug_bool("netplug", "log_unanswered") == 0, "a LOG unanswered for 9 s: nothing said yet");
            rounds(1000, 1);
            CHECK(said(logq, "netplug: does not answer LOG (no OK in 10 s): built on an SDK before flow control?") == 1 && plug_bool("netplug", "log_unanswered") == 1,
                  "unanswered for 10 s: said in the log, and on its card (/api/plugins log_unanswered)");
            rounds(30000, 1);
            CHECK(said(logq, "does not answer LOG") == 1 && p->state == P_RUN, "said once, and the plugin left running");
            freopen("/dev/null", "w", stderr); }
          pm_winit(&w, ok, sizeof ok); pm_put_ok(&w, lid + 1); message(0, ok, (size_t)pm_wlen(&w)); log_pump();
          CHECK(recv(h_peer, b, sizeof b, MSG_DONTWAIT) < 0, "... an OK of another id is not its");
          pm_winit(&w, ok, sizeof ok); pm_put_ok(&w, lid); message(0, ok, (size_t)pm_wlen(&w)); log_pump();
          r = recv(h_peer, b, sizeof b, MSG_DONTWAIT);
          CHECK(!p->log_wait || (r > 0 && !pm_head(b, (size_t)r, &h, &body, &bl) && h.type == PM_LOG && h.id == p->log_wait), "... its OK: the next one at once");
          CHECK(plug_bool("netplug", "log_unanswered") == 0, "... answered: the card's word gone");
          /* a review of 655efd7, 1: a correct client whose log callback waits on hm_connect (its CONNECT under way, up to
           * PNET_DEADLINE_MS, as long as LOG_ANSWER_MS) is not one from before flow control */
          { char logq2[300]; snprintf(logq2, sizeof logq2, "%s/unanswered2.log", h_dir); freopen(logq2, "w", stderr); setvbuf(stderr, NULL, _IONBF, 0);
            if (!p->log_wait) { p->log_wait = 777; p->log_sent = btime; }   /* (as log_pump leaves it once a LOG went out) */
            p->log_deaf = 0; p->net_pending = 1;
            rounds(10000, 1);
            CHECK(!said(logq2, "does not answer LOG") && plug_bool("netplug", "log_unanswered") == 0, "a LOG unanswered while its CONNECT runs 10 s: not said, not on its card");
            p->net_pending = 0;
            rounds(9000, 1);
            CHECK(!said(logq2, "does not answer LOG"), "... the 10 s counted from the CONNECT's end");
            rounds(1000, 1);
            CHECK(said(logq2, "does not answer LOG") == 1, "... and said 10 s after it");
            freopen("/dev/null", "w", stderr); }
          if (p->log_wait) { pm_winit(&w, ok, sizeof ok); pm_put_ok(&w, p->log_wait); message(0, ok, (size_t)pm_wlen(&w)); } }
        /* P1 of a review of 08399dc: nothing of the log while a CONNECT of its waits (the client keeps what comes
         * meanwhile, and a connect takes up to 10 s): held, its place kept, sent after */
        { struct logpos at = p->log_at; lf = fopen(logp, "a"); fprintf(lf, "boot+1.003 while it connects\n"); fclose(lf);
          p->net_pending = 1; p->log_next = 0; log_pump();
          CHECK(recv(h_peer, b, sizeof b, MSG_DONTWAIT) < 0 && !memcmp(&at, &p->log_at, sizeof at), "a CONNECT under way: no LOG, its place kept");
          p->net_pending = 0; p->log_next = 0; log_pump();
          r = recv(h_peer, b, sizeof b, MSG_DONTWAIT);
          CHECK(r > 0 && !pm_head(b, (size_t)r, &h, &body, &bl) && h.type == PM_LOG && memcmp(&at, &p->log_at, sizeof at), "... and sent once it is answered"); }
        p->log_wait = 0;
        fill(p); { struct logpos at = p->log_at; lf = fopen(logp, "a"); fprintf(lf, "boot+1.004 one more\n"); fclose(lf);
          p->log_next = 0; log_pump();
          CHECK(!memcmp(&at, &p->log_at, sizeof at) && !p->log_wait, "a LOG that does not fit: its place kept, sent again later"); }
        h_plug(P_DEAD, 0); }
      /* R7 of a review of 0dabfa6: the name's lookup within the request's deadline (getaddrinfo has none of its own;
       * here a resolver that takes 3 s against a deadline of 1 s): FAILED in time, the worker free again */
      { char err[100]; const struct ps_plugin *sp; struct pm_connect c = { PM_CAP_NET_SERVER, 0, pm_s(""), 0 }; uint8_t b[64]; ssize_t r = -1; long long t0;
        p = h_plug(P_RUN, caps); h_as(p, "netplug"); h_drain(NULL); memset(p->net_asked, 0, sizeof p->net_asked);
        ps_lock(); sp = ps_find("netplug"); ps_config_set(sp, "server", "slow.invalid", err, sizeof err); ps_unlock();
        setenv("HASSMIC_PLUGIN_LOOKUP_DELAY", "3000", 1); setenv("HASSMIC_PLUGIN_NET_MS", "1000", 1);
        t0 = now_ms(); SEND(pm_put_connect, 290, &c);
        for (int i = 0; i < 150 && (r = recv(h_peer, b, sizeof b, MSG_DONTWAIT)) <= 0; i++) usleep(20000);
        { struct pm_hdr h; const uint8_t *body; size_t bl; struct pm_err e = { 0 };
          CHECK(r > 0 && !pm_head(b, (size_t)r, &h, &body, &bl) && h.type == PM_ERR && h.id == 290 && !pm_get_err(body, bl, &e) && e.code == PM_ERR_FAILED && now_ms() - t0 < 2000,
                "R7: a lookup that takes 3 s against a deadline of 1 s: FAILED within the deadline"); }
        unsetenv("HASSMIC_PLUGIN_LOOKUP_DELAY"); unsetenv("HASSMIC_PLUGIN_NET_MS");
        h_plug(P_DEAD, 0); }
      /* N2 of a review of 7be9343: one plugin's slow requests delay nobody else's, and its lookups are bounded by its
       * own count (here plugin_net.c alone, entries 3 and 4 and on of the broker's, a resolver that takes 3 s against a
       * deadline of 1 s, so that a lookup's thread outlives its request by 2 s) */
      { char logq[300]; long long t0; struct pnet_req a = { 0 }, b2 = { 0 };
        snprintf(logq, sizeof logq, "%s/lookups.log", h_dir); freopen(logq, "w", stderr); setvbuf(stderr, NULL, _IONBF, 0);
        setenv("HASSMIC_PLUGIN_LOOKUP_DELAY", "3000", 1); setenv("HASSMIC_PLUGIN_NET_MS", "1000", 1);
        a.who = 3; snprintf(a.plugin, sizeof a.plugin, "slowplug"); snprintf(a.host, sizeof a.host, "a.invalid"); a.port = 443; a.kind = PM_CAP_NET_INTERNET;
        b2.who = 4; snprintf(b2.plugin, sizeof b2.plugin, "otherplug"); snprintf(b2.host, sizeof b2.host, "127.0.0.1"); b2.port = 80; b2.kind = PM_CAP_NET_LAN;     /* (DENIED at once by the worker) */
        atomic_store(&nd_n, 0); t0 = now_ms();
        a.id = 1; pnet_ask(&a, nd_done, NULL); a.id = 2; pnet_ask(&a, nd_done, NULL); b2.id = 10; pnet_ask(&b2, nd_done, NULL);
        for (int i = 0; i < 100 && !nd_of(10); i++) usleep(10000);
        CHECK(nd_of(10) && nd_of(10)->err == PM_ERR_DENIED && nd_of(10)->at - t0 < 500 && !nd_of(1) && !nd_of(2), "N2: another plugin's request beside one plugin's two slow ones: answered at once (a worker of its own), the slow ones still under way");
        for (int i = 0; i < 300 && !nd_of(2); i++) usleep(10000);
        CHECK(nd_of(1) && nd_of(2) && nd_of(1)->err == PM_ERR_FAILED && nd_of(2)->at - nd_of(1)->at >= 800, "N2: ... the slow plugin's own two one after the other (FAILED each at its deadline)");
        a.id = 3; t0 = now_ms(); pnet_ask(&a, nd_done, NULL);
        for (int i = 0; i < 100 && !nd_of(3); i++) usleep(10000);
        CHECK(nd_of(3) && nd_of(3)->err == PM_ERR_BUSY && nd_of(3)->at - t0 < 300 && said(logq, "plugins: slowplug: no connection to the internet, port 443 (TCP): two of its name lookups are under way already") == 1,
              "N2: a third lookup while its two still run (their resolver has not given up): BUSY at once, and the log says why");
        b2.id = 11; b2.kind = PM_CAP_NET_SERVER; snprintf(b2.host, sizeof b2.host, "b.invalid"); t0 = now_ms(); pnet_ask(&b2, nd_done, NULL);
        for (int i = 0; i < 300 && !nd_of(11); i++) usleep(10000);
        CHECK(nd_of(11) && nd_of(11)->err == PM_ERR_FAILED && nd_of(11)->at - t0 >= 800, "N2: ... another plugin's lookup meanwhile: begun (FAILED at its deadline, not refused)");
        usleep(3200000);                    /* (every lookup's thread over) */
        /* the bound of the names plugins choose, and the places kept for the names owners entered: eight entries with
         * two each (16), then a ninth's own name refused and its owner's server's looked up */
        atomic_store(&nd_n, 0);
        for (int e = 5; e < 13; e++) for (int k = 0; k < 2; k++) { a.who = e; a.id = (uint32_t)(100 + e * 2 + k); a.kind = PM_CAP_NET_INTERNET; pnet_ask(&a, nd_done, NULL); }
        for (int i = 0; i < 400 && atomic_load(&nd_n) < 16; i++) usleep(10000);
        { int failed = 0; for (int k = 0; k < atomic_load(&nd_n); k++) failed += nd[k].err == PM_ERR_FAILED; CHECK(atomic_load(&nd_n) == 16 && failed == 16, "(sixteen lookups of names plugins chose, under way)"); }
        a.who = 13; a.id = 200; t0 = now_ms(); pnet_ask(&a, nd_done, NULL);
        a.id = 201; a.kind = PM_CAP_NET_SERVER; snprintf(a.host, sizeof a.host, "owner.invalid"); pnet_ask(&a, nd_done, NULL);
        for (int i = 0; i < 300 && !nd_of(201); i++) usleep(10000);
        CHECK(nd_of(200) && nd_of(200)->err == PM_ERR_BUSY && nd_of(200)->at - t0 < 300 && nd_of(201) && nd_of(201)->err == PM_ERR_FAILED && nd_of(201)->at - t0 >= 800
              && said(logq, "this Echo has as many name lookups under way as it takes") == 1,
              "N2: the Echo's bound of names plugins chose reached: a ninth plugin's own refused (BUSY, said), the server its owner entered looked up all the same");
        usleep(3200000);
        unsetenv("HASSMIC_PLUGIN_LOOKUP_DELAY"); unsetenv("HASSMIC_PLUGIN_NET_MS");
        /* N3, N1: the worker's own rules, whatever came past the broker: net.internet never to the network (what a name
         * resolves to is held to it here: its author can point the name at a device of the home), net.lan never at
         * DNS's ports */
        atomic_store(&nd_n, 0);
        a.who = 3; a.id = 300; a.kind = PM_CAP_NET_INTERNET; snprintf(a.host, sizeof a.host, "192.168.1.5"); pnet_ask(&a, nd_done, NULL);
        b2.id = 301; b2.kind = PM_CAP_NET_LAN; snprintf(b2.host, sizeof b2.host, "192.168.1.1"); b2.port = 53; pnet_ask(&b2, nd_done, NULL);
        for (int i = 0; i < 300 && (!nd_of(300) || !nd_of(301)); i++) usleep(10000);
        CHECK(nd_of(300) && nd_of(300)->err == PM_ERR_DENIED && said(logq, "plugins: slowplug: no connection to the internet, port 443 (TCP): an address no plugin may reach on the internet (one on the local network") == 1
              && nd_of(301) && nd_of(301)->err == PM_ERR_DENIED && said(logq, "plugins: otherplug: no connection to a device on the network, port 53: DNS's port") == 1,
              "N3, N1: by the worker too: net.internet to an address on the network DENIED, net.lan at port 53 DENIED, each said");
        freopen("/dev/null", "w", stderr); }
      /* R5 of a review of 0dabfa6: refused CONNECTs are said once in REFUSAL_MS, the rest counted (a line each was 200 a
       * second, past the plugin's own 50 a minute) */
      { char logq[300], err[100]; FILE *f; int lines = 0; char *l = NULL; size_t cap = 0; const struct ps_plugin *sp; int fd2 = dup(2);
        struct pm_connect other = { PM_CAP_NET_SERVER, 0, pm_s("elsewhere.example"), 0 };
        snprintf(logq, sizeof logq, "%s/refusals.log", h_dir); f = freopen(logq, "w", stderr); setvbuf(stderr, NULL, _IONBF, 0);
        p = h_plug(P_RUN, caps); h_as(p, "netplug"); h_drain(NULL);
        ps_lock(); sp = ps_find("netplug"); ps_config_set(sp, "server", "192.168.1.5", err, sizeof err); ps_unlock();
        for (int i = 0; i < 199; i++) { SEND(pm_put_connect, 300 + (uint32_t)i, &other); h_drain(NULL); }
        h_skew_ms += REFUSAL_MS; timers();
        fflush(stderr); freopen("/dev/null", "w", stderr); (void)f; (void)fd2;
        if ((f = fopen(logq, "r"))) { while (getline(&l, &cap, f) > 0) lines += !!strstr(l, "netplug"); fclose(f); } free(l);
        CHECK(p->state == P_RUN && lines == 2, "R5: 199 CONNECTs elsewhere in a second: two lines (the first, then the count), the plugin running");
        h_plug(P_DEAD, 0); }
      /* R1 of a review of 0dabfa6: the seal and a process on its way out.  A wake word plugin updated to a version
       * without `wake`: its old process, told to stop, may still write its storage until it is gone; nothing of what
       * it was given or wrote under the seal may be left for the new version.  And a plugin removed and installed again
       * at once: the removed one's process writes nothing into the new install's storage */
      { const struct ps_plugin *l; char path[400], mark[400]; struct stat st; struct pm_store_put m = { pm_s("heard"), pm_b("what the microphone heard", 25) };
        struct pm_store_get g = { pm_s("early") };
        h_add_full("sealy", "\"wake\": true, \"storage\": 4", "", "v1\n");
        ps_lock(); ps_scan(&l); ps_unlock();
        snprintf(mark, sizeof mark, "%s/plugins/sealy/sealed", h_dir);
        CHECK(!stat(mark, &st), "R1: v1 has wake: the sealed mark is written");
        p = h_plug(P_RUN, PM_CAPBIT(PM_CAP_WAKE) | PM_CAPBIT(PM_CAP_STORAGE)); h_as(p, "sealy"); p->tokens = STORE_BUCKET; p->token_at = now_ms(); h_drain(NULL);
        { struct pm_store_put e = { pm_s("early"), pm_b("before the update", 17) }; SEND(pm_put_store_put, 400, &e); h_drain(&last); }
        CHECK(last == PM_OK, "(v1 writes as it runs)");
        /* the owner accepts version 2, which has no wake (the store wrote it and its acceptance; web.c then calls this) */
        h_add_full("sealy", "\"storage\": 4", "", "v2\n");
        plugin_installed("sealy");
        CHECK(p->state == P_STOPPING && !stat(mark, &st), "R1: v2 accepted, v1's process stopping: the wipe waits for it to be gone (the mark stays)");
        CHECK(json_wake("sealy") == 1, "Q7 (a review of 7144548, 3): /api/plugins says wake while the wipe is due, though v2 has no wake: the page's file copies say \"from-wake\"");
        /* (a review of 7144548, 1) it writes until it is gone, as any process on its way out: the wipe is what keeps
         * the seal, and it waits for that */
        SEND(pm_put_store_put, 401, &m); h_drain(&last);
        snprintf(path, sizeof path, "%s/plugins/sealy/storage/heard", h_dir);
        CHECK(last == PM_OK && !stat(path, &st) && !stat(mark, &st), "R1: a STORE_PUT of v1's process on its way out: written (its stop's cursor, F1), the mark still there");
        { uint8_t b[PM_MAX_PACKET]; ssize_t r; struct pm_hdr h; const uint8_t *body; size_t bl; struct pm_err e = { 0 };
          SEND(pm_put_store_get, 402, &g); r = recv(h_peer, b, sizeof b, MSG_DONTWAIT);
          CHECK(r > 0 && !pm_head(b, (size_t)r, &h, &body, &bl) && h.type == PM_ERR && !pm_get_err(body, bl, &e) && e.code == PM_ERR_FAILED, "... a STORE_GET then: FAILED, as any request but a write after STOP (R2)"); }
        ps_lock(); ps_scan(&l); ps_unlock();
        snprintf(path, sizeof path, "%s/plugins/sealy/storage/early", h_dir);
        CHECK(!stat(path, &st) && !stat(mark, &st), "R1: a look meanwhile wipes nothing (v1's process still there): the mark stays");
        /* v1's process is gone: the start of v2 wipes first */
        { struct plugrun_msg em = { PLUGRUN_EXITED, 0, 0, 0, 0, 0 }; p->slot = 0; launcher_message(&em); }
        CHECK(p->state == P_DEAD, "(v1's process gone)");
        { struct run run; int fd = program_open("sealy", &run, 0); if (fd >= 0) close(fd); }
        { char heard[400]; snprintf(heard, sizeof heard, "%s/plugins/sealy/storage/heard", h_dir);
          CHECK(stat(path, &st) && stat(heard, &st) && stat(mark, &st), "R1: then, before v2 starts: what v1 kept under the seal, and what it wrote on its way out, wiped; the mark gone"); }
        CHECK(json_wake("sealy") == 0, "... and not once it is done");
        /* removed and installed again at once: the old process, stopping, writes nothing into the new install's */
        p = h_plug(P_RUN, PM_CAPBIT(PM_CAP_STORAGE)); h_as(p, "sealy"); p->tokens = STORE_BUCKET; p->token_at = now_ms(); h_drain(NULL);
        plugin_remove("sealy");
        h_add_full("sealy", "\"storage\": 4", "", "v2\n");
        ps_lock(); ps_scan(&l); ps_unlock();
        start("sealy");                                 /* (the install's start: P_STOPPING, it starts when the old one is gone) */
        CHECK(p->state == P_STOPPING && !p->gone && p->want, "(removed, installed again: the old process on its way out, the new start waits for it)");
        snprintf(path, sizeof path, "%s/plugins/sealy/storage/heard", h_dir);
        SEND(pm_put_store_put, 405, &m); h_drain(&last);
        CHECK(last == PM_ERR && stat(path, &st), "R1: the removed one's process writes nothing into the storage of the same plugin installed again");
        h_plug(P_DEAD, 0); }
      /* A review of 7144548, 1: an ordinary update (no `wake` on either side).  The old version's process, told to stop
       * for it, keeps its cursor in its stop callback: written, and the new version starts with it (store_mine refused
       * an older package's process, and every update lost the place a forwarder had come to) */
      { const struct ps_plugin *l; char path[400]; struct stat st; struct pm_store_put m = { pm_s("cursor"), pm_b("at-stop", 7) }; char v[16] = ""; FILE *f;
        h_add_full("plainy", "\"storage\": 4", "", "v1\n");
        ps_lock(); ps_scan(&l); ps_unlock();
        p = h_plug(P_RUN, PM_CAPBIT(PM_CAP_STORAGE)); h_as(p, "plainy"); p->tokens = STORE_BUCKET; p->token_at = now_ms(); h_drain(NULL);
        h_add_full("plainy", "\"storage\": 4", "", "v2\n");
        plugin_installed("plainy");
        CHECK(p->state == P_STOPPING, "(v2 installed: v1's process told to stop)");
        SEND(pm_put_store_put, 501, &m); h_drain(&last);
        snprintf(path, sizeof path, "%s/plugins/plainy/storage/cursor", h_dir);
        CHECK(last == PM_OK && !stat(path, &st), "an ordinary update: the old version's cursor, written at its stop, is kept (OK)");
        { struct plugrun_msg em = { PLUGRUN_EXITED, 0, 0, 0, 0, 0 }; p->slot = 0; launcher_message(&em); }
        { struct run run; int fd = program_open("plainy", &run, 0); if (fd >= 0) close(fd); }
        if ((f = fopen(path, "r"))) { if (!fgets(v, sizeof v, f)) v[0] = 0; fclose(f); }
        snprintf(path, sizeof path, "%s/plugins/plainy/sealed", h_dir);
        CHECK(!strcmp(v, "at-stop") && stat(path, &st), "... and still there as v2 starts: no seal, nothing wiped");
        /* and removed while it stops: from then on nothing of the store's */
        p = h_plug(P_RUN, PM_CAPBIT(PM_CAP_STORAGE)); h_as(p, "plainy"); p->tokens = STORE_BUCKET; p->token_at = now_ms(); h_drain(NULL);
        plugin_remove("plainy"); last = 0;
        SEND(pm_put_store_put, 502, &m); h_drain(&last);
        CHECK(p->state == P_STOPPING && last == PM_ERR, "removed: its process on its way out writes nothing");
        h_plug(P_DEAD, 0); }
      /* R6 of a review of 0dabfa6: an entry taken anew starts no generation over (its `starts` did: an answer for the
       * removed plugin's process went to the next one's) */
      { unsigned g1; struct pnet_req rq = { 0, 0, 5, "genplug", "127.0.0.1", 514, 1 }; const struct ps_plugin *l;
        h_add("genplug", 0); ps_lock(); ps_scan(&l); ps_unlock();
        for (int i = 0; i < NPLUG; i++) if (plugs[i].state != P_FREE && !strcmp(plugs[i].id, "genplug")) plugs[i].state = P_FREE;
        h_plug(P_FREE, 0); plugs[0].state = P_FREE; plugs[0].id[0] = 0; wake_id[0] = 0; core_held[0] = 0; fall_held[0] = 0; falls_off = 0; go = 1;
        start("genplug");
        CHECK(plugs[0].state == P_SPAWNING && plugs[0].starts == 1, "(a plugin's first process in a fresh entry)");
        g1 = plugs[0].gen; rq.gen = g1;
        plugs[0].state = P_RUN; CHECK(net_live(&rq), "(its request is alive)");
        plugs[0].state = P_FREE; plugs[0].slot = -1; start("genplug");
        CHECK(plugs[0].state == P_SPAWNING && plugs[0].starts == 1 && plugs[0].gen != g1, "R6: the entry taken anew: starts 1 again, the generation a new one");
        plugs[0].state = P_RUN; CHECK(!net_live(&rq), "R6: the request of the process before is not the new one's");
        h_plug(P_DEAD, 0); }
      /* F2 of a review of c4ace24: the broker's lines about a listener are private by the capabilities of the package
       * its entry runs.  The marks were a table by id that nothing ever emptied: 20 listeners installed, started and
       * removed one after another (more than the 16 places it had), and the next one's lines were public */
      { const struct ps_plugin *l; char id[PS_ID_MAX], line[80]; int spawned = 0, freed = 0;
        for (int i = 0; i < NPLUG; i++) if (plugs[i].state != P_FREE) { plugs[i].state = P_DEAD; plugs[i].slot = -1; plugs[i].want = 0; }
        wake_id[0] = 0; go = 1; asked();
        for (int k = 0; k <= 20; k++) {
            struct plug *q; struct plugrun_msg gone = { PLUGRUN_EXITED, 0, 0, 0, 0, 0 };
            snprintf(id, sizeof id, "lis%02d", k);
            h_add_full(id, "\"request.audio\": true", "", "a listener\n");
            ps_lock(); ps_scan(&l); ps_unlock();
            start(id);
            q = h_find(id); spawned += q && q->state == P_SPAWNING;
            if (k == 20) break;                                 /* (the last stays) */
            plugin_remove(id); asked();                         /* (the launcher's messages read: the descriptors closed) */
            if (q && q->slot >= 0) { gone.slot = (uint32_t)q->slot; launcher_message(&gone); }
            freed += q && q->state == P_FREE;
        }
        CHECK(spawned == 21 && freed == 20, "(21 listeners started, the first 20 removed and their processes gone)");
        snprintf(line, sizeof line, "%s: the plugin's own value", id);
        CHECK(seal_of(line) == SEAL_PRIVATE, "F2: the broker's lines about the 21st listener installed in one run are private (a table by id was full at 16)");
        { struct plug *q = h_find(id); if (q) { plugin_remove(id); asked(); if (q->slot >= 0) { struct plugrun_msg gone = { PLUGRUN_EXITED, (uint32_t)q->slot, 0, 0, 0, 0 }; launcher_message(&gone); } } }
        /* a review of 7be9343: a plugin with `call` hears the room while a call is live, and the person at the other
         * end: its own lines and the broker's about it private, as a listener's */
        { struct plug *q; char logq[300]; struct plugrun_msg gone = { PLUGRUN_EXITED, 0, 0, 0, 0, 0 };
          h_add_full("caller", "\"call\": true", "", "a caller\n");
          ps_lock(); ps_scan(&l); ps_unlock();
          start("caller"); q = h_find("caller");
          snprintf(logq, sizeof logq, "%s/call.log", h_dir); freopen(logq, "w", stderr); setvbuf(stderr, NULL, _IONBF, 0);
          if (q) { pthread_mutex_lock(&bl); log_lines(q, "the number I dialled\n", 21); say("caller: a value of its own"); pthread_mutex_unlock(&bl); }
          freopen("/dev/null", "w", stderr);
          CHECK(q && q->state == P_SPAWNING && seal_of("caller: x") == SEAL_PRIVATE && said(logq, LOG_PRIVATE_MARK "plugin caller: the number I dialled") == 1
                && said(logq, LOG_PRIVATE_MARK "plugins: caller: a value of its own") == 1,
                "a plugin with call: what it writes and the broker's lines about it are private");
          if (q) { plugin_remove("caller"); asked(); if (q->slot >= 0) { gone.slot = (uint32_t)q->slot; launcher_message(&gone); } } }
        h_plug(P_DEAD, 0); }
      /* ---- net.lan (an address on the network the plugin names, never a name) and net.internet (any name or
       * address), each at its entries' ports; log.read.full (the page's view) */
      { uint8_t b[64]; ssize_t r; struct pm_hdr h; const uint8_t *body; size_t bl; struct pm_err e = { 0 }; int wrong = 0; char logq[300];
        struct { struct pm_connect c; int want; const char *what; } t[] = {
          { { PM_CAP_NET_LAN, 0, pm_s(""), 80 }, PM_ERR_BAD, "net.lan without an address" },
          { { PM_CAP_NET_LAN, 0, pm_s("printer.local"), 80 }, PM_ERR_DENIED, "net.lan with a name (no lookup: nothing spelled into DNS)" },
          { { PM_CAP_NET_LAN, 0, pm_s("8.8.8.8"), 80 }, PM_ERR_DENIED, "net.lan to an address outside the network" },
          { { PM_CAP_NET_LAN, 0, pm_s("10.0.0.09"), 80 }, PM_ERR_DENIED, "net.lan to an address not written as one" },
          { { PM_CAP_NET_LAN, 0, pm_s("127.0.0.1"), 80 }, PM_ERR_DENIED, "net.lan to loopback" },
          { { PM_CAP_NET_LAN, 0, pm_s("224.0.0.251"), 80 }, PM_ERR_DENIED, "net.lan to a multicast group" },
          { { PM_CAP_NET_LAN, 0, pm_s("10.0.0.9"), 22 }, PM_ERR_DENIED, "a port its entry does not list" },
          { { PM_CAP_NET_LAN, 0, pm_s("10.0.0.9"), 0 }, PM_ERR_BAD, "port 0 for an entry of two ports" },
          { { PM_CAP_NET_LAN, 1, pm_s("10.0.0.9"), 5555 }, PM_ERR_DENIED, "\"any\" is any port but 5555" },
          { { PM_CAP_NET_LAN, 1, pm_s("10.0.0.9"), 53 }, PM_ERR_DENIED, "N1: net.lan's \"any\" is not DNS's 53 (the router asks the internet)" },
          { { PM_CAP_NET_LAN, 1, pm_s("192.168.1.1"), 853 }, PM_ERR_DENIED, "N1: ... nor DNS over TLS's 853" },
          { { PM_CAP_NET_INTERNET, 0, pm_s("192.168.1.5"), 443 }, PM_ERR_DENIED, "N3: net.internet to an address on the local network (that is net.lan's)" },
          { { PM_CAP_NET_INTERNET, 0, pm_s("169.254.1.1"), 443 }, PM_ERR_DENIED, "N3: ... a link-local one" },
          { { PM_CAP_NET_LAN, 2, pm_s("10.0.0.9"), 80 }, PM_ERR_DENIED, "an entry the manifest has not" },
          { { PM_CAP_NET_INTERNET, 0, pm_s(""), 443 }, PM_ERR_BAD, "net.internet without a host" },
          { { PM_CAP_NET_INTERNET, 0, pm_s("not a host!"), 443 }, PM_ERR_BAD, "net.internet with a host that is no name or address" },
          { { PM_CAP_NET_INTERNET, 0, pm_s("example.org"), 80 }, PM_ERR_DENIED, "net.internet at a port its entry does not list" },
          { { PM_CAP_NET_INTERNET, 1, pm_s("example.org"), 443 }, PM_ERR_DENIED, "net.internet: an entry the manifest has not" },
          { { PM_CAP_NET_SERVER, 0, pm_s(""), 0 }, PM_ERR_DENIED, "net.server, which it was not given" },
          { { 9, 0, pm_s("10.0.0.9"), 80 }, PM_ERR_DENIED, "a kind that is no CONNECT's" } };
        h_add_full("lanplug", "\"net.lan\": [ { \"proto\": \"tcp\", \"ports\": [ 80, 443 ] }, { \"proto\": \"udp\", \"ports\": \"any\" } ], \"net.internet\": [ { \"proto\": \"tcp\", \"ports\": [ 443 ] } ]",
                   "", "the lan program\n");
        ps_lock(); ps_scan(&l); ps_unlock();
        p = h_plug(P_RUN, PM_CAPBIT(PM_CAP_NET_LAN) | PM_CAPBIT(PM_CAP_NET_INTERNET)); h_as(p, "lanplug"); h_drain(NULL);
        for (size_t k = 0; k < sizeof t / sizeof t[0]; k++) {
            SEND(pm_put_connect, 200 + (uint32_t)k, &t[k].c);
            r = recv(h_peer, b, sizeof b, MSG_DONTWAIT);
            if (!(r > 0 && !pm_head(b, (size_t)r, &h, &body, &bl) && h.type == PM_ERR && h.id == 200 + k && !pm_get_err(body, bl, &e) && e.code == t[k].want) || p->net_pending) {
                printf("     (%s: %s %d)\n", t[k].what, r > 0 ? "answered" : "no answer", r > 0 ? (int)e.code : 0); wrong++;
            }
        }
        CHECK(!wrong, "net.lan, net.internet: the broker refuses before any lookup or socket what the entry does not allow (no address, a name for net.lan, outside the network, loopback, multicast, a port not listed, port 0 of two, 5555 under \"any\", DNS's ports for net.lan, the network for net.internet, an entry or a kind it has not)");
        /* what passes the broker goes to a worker, which holds every address to pnet_address_ok: loopback by name for
         * net.internet (the lookup is its), refused there; and a plugin that hears requests has the worker's lines
         * private (where it connects is its own choice) */
        snprintf(logq, sizeof logq, "%s/lan.log", h_dir); freopen(logq, "w", stderr); setvbuf(stderr, NULL, _IONBF, 0);
        p->caps |= PM_CAPBIT(PM_CAP_REQUEST_AUDIO);
        { struct pm_connect c = { PM_CAP_NET_INTERNET, 0, pm_s("127.0.0.1"), 443 };
          SEND(pm_put_connect, 230, &c);
          CHECK(p->net_pending == 1, "net.internet to a name or address: asked of a worker");
          for (int i = 0; i < 100 && (r = recv(h_peer, b, sizeof b, MSG_DONTWAIT)) <= 0; i++) usleep(20000);
          CHECK(r > 0 && !pm_head(b, (size_t)r, &h, &body, &bl) && h.type == PM_ERR && h.id == 230 && !pm_get_err(body, bl, &e) && e.code == PM_ERR_DENIED && !p->net_pending,
                "... loopback: refused by the worker (DENIED)");
          CHECK(said(logq, LOG_PRIVATE_MARK "plugins: lanplug: no connection to the internet, port 443 (TCP): an address no plugin may reach") == 1,
                "the worker's line about a plugin that hears requests: private"); }
        p->caps &= ~(PM_CAPBIT(PM_CAP_REQUEST_AUDIO));
        { struct pm_connect c = { PM_CAP_NET_LAN, 1, pm_s("169.254.0.1"), 5353 };
          SEND(pm_put_connect, 231, &c);
          CHECK(p->net_pending == 1, "net.lan to a link-local address at any port of an \"any\" entry: asked of a worker");
          for (int i = 0; i < 600 && (r = recv(h_peer, b, sizeof b, MSG_DONTWAIT)) <= 0; i++) usleep(20000);
          CHECK(r > 0 && !pm_head(b, (size_t)r, &h, &body, &bl) && h.id == 231 && !p->net_pending, "... answered (a socket, or why not), nothing left pending"); }
        freopen("/dev/null", "w", stderr);
        /* log.read.full: LOG_FOLLOW with it alone; the page's view, but never a wake word plugin's line */
        { struct pm_log_follow f = { pm_s("start") }; uint8_t lb[PM_MAX_PACKET]; struct pm_log lg; struct pm_log_lines le; char got[64][200]; int n = 0, priv = 0, addr = 0, wakel = 0;
          p = h_plug(P_RUN, PM_CAPBIT(PM_CAP_LOG_READ_FULL)); h_as(p, "lanplug"); h_drain(NULL);
          SEND(pm_put_log_follow, 240, &f); h_drain(&last);
          CHECK(last == PM_OK && p->log_on, "log.read.full: LOG_FOLLOW taken without log.read");
          log_pump();
          r = recv(h_peer, lb, sizeof lb, MSG_DONTWAIT);
          if (r > 0 && !pm_head(lb, (size_t)r, &h, &body, &bl) && h.type == PM_LOG && !pm_get_log(body, bl, &lg)) while (n < 64 && pm_next_log_lines(&lg.lines, &le)) pm_cstr(got[n++], sizeof got[0], le.line);
          for (int k = 0; k < n; k++) { priv += !!strstr(got[k], LOG_PRIVATE_MARK "transcript: what was said"); addr += !!strstr(got[k], "a public line aa:bb:cc:dd:ee:ff"); wakel += !!strstr(got[k], "a wake plugin's own line"); }
          CHECK(n > 2 && priv == 1 && addr == 1 && !wakel, "log.read.full: the private line and the addresses as the page shows them; the wake word plugin's line not");
          p = h_plug(P_RUN, 0); h_as(p, "lanplug"); h_drain(NULL);
          SEND(pm_put_log_follow, 241, &f); h_drain(&last);
          CHECK(last == PM_ERR && !p->log_on, "LOG_FOLLOW with neither log capability: refused (DENIED)"); }
        h_plug(P_DEAD, 0); }
      /* ---- music, the volume, UDP of its own, the search, proposals, the Echo's state, Home Assistant
       * (docs/plugins.md phase 4): every check of the broker's both ways */
      { const struct ps_plugin *l; int fd2 = -1; pm_caps_t caps = PM_CAPBIT(PM_CAP_AUDIO_MUSIC) | PM_CAPBIT(PM_CAP_AUDIO_VOLUME) | PM_CAPBIT(PM_CAP_STATE) | PM_CAPBIT(PM_CAP_NET_UDP)
                                                   | PM_CAPBIT(PM_CAP_NET_DISCOVER) | PM_CAPBIT(PM_CAP_NET_SERVER) | PM_CAPBIT(PM_CAP_HA_ENTITIES) | PM_CAPBIT(PM_CAP_HA_ACTIONS);
        uint8_t b[PM_MAX_PACKET]; ssize_t r; struct pm_hdr h; const uint8_t *body; size_t bl; struct pm_err e = { 0 }; int fd;
        /* an answer, and the descriptor that came with it (-1 none) */
        #define ANSWER(fdp) do { struct iovec iov_ = { b, sizeof b }; union { struct cmsghdr c; char x[CMSG_SPACE(sizeof(int))]; } u_; struct msghdr m_ = { .msg_iov = &iov_, .msg_iovlen = 1, .msg_control = &u_, .msg_controllen = sizeof u_.x }; \
            *(fdp) = -1; do r = recvmsg(h_peer, &m_, MSG_DONTWAIT); while (r >= 2 && (b[0] | b[1] << 8) != PM_OK && (b[0] | b[1] << 8) != PM_ERR && (b[0] | b[1] << 8) != PM_UDP_OPENED); \
            for (struct cmsghdr *c_ = CMSG_FIRSTHDR(&m_); r > 0 && c_; c_ = CMSG_NXTHDR(&m_, c_)) if (c_->cmsg_type == SCM_RIGHTS) memcpy(fdp, CMSG_DATA(c_), sizeof(int)); \
            if (r <= 0 || pm_head(b, (size_t)r, &h, &body, &bl)) h.type = 0; } while (0)
        #define CODE() (h.type == PM_ERR && !pm_get_err(body, bl, &e) ? (int)e.code : h.type == PM_OK ? 0 : -1)
        h_add_full("musicplug", "\"audio.music\": true, \"audio.volume\": true, \"state\": true, \"net.udp\": 1, \"net.discover\": { \"udp\": 3483, \"to\": \"broadcast\" },"
                   " \"net.server\": [ { \"setting\": \"server\", \"proto\": \"tcp\", \"ports\": [ 9000 ] } ],"
                   " \"ha.entities\": [ { \"name\": \"level\", \"kind\": \"number\", \"label\": \"Level\", \"min\": 0, \"max\": 10 },"
                   "                    { \"name\": \"seen\", \"kind\": \"event\", \"label\": \"Seen\", \"types\": [ \"ping\" ] },"
                   "                    { \"name\": \"on\", \"kind\": \"switch\", \"label\": \"On\" },"
                   "                    { \"name\": \"words\", \"kind\": \"text_sensor\", \"label\": \"Words\" },"
                   "                    { \"name\": \"frac\", \"kind\": \"number\", \"label\": \"Fraction\", \"min\": -0.1, \"max\": 0.1, \"step\": 0.01 } ],"
                   " \"ha.actions\": [ { \"name\": \"go\", \"label\": \"Go\", \"page\": true }, { \"name\": \"say\", \"label\": \"Say\", \"args\": [ { \"name\": \"n\", \"type\": \"int\" } ] },"
                   "                   { \"name\": \"tell\", \"label\": \"Tell\", \"args\": [ { \"name\": \"what\", \"type\": \"string\" }, { \"name\": \"n\", \"type\": \"int\" } ] } ]",
                   "{ \"name\": \"server\", \"label\": \"Server\", \"type\": \"host\" },", "the player's program\n");
        ps_lock(); ps_scan(&l); ps_unlock();
        feature = 1;
        /* before READY: nothing of it (a stream, a socket) */
        p = h_plug(P_CONFIG, caps); h_as(p, "musicplug"); p->lane = -1; h_drain(NULL);
        { struct pm_music_open m = { 48000, 2 }; SEND(pm_put_music_open, 900, &m); ANSWER(&fd); }
        CHECK(CODE() == PM_ERR_BAD && !p->ms, "MUSIC_OPEN before READY: BAD");
        p->state = P_RUN;
        { struct pm_music_open bad = { 12345, 2 }, mono3 = { 48000, 3 }, m = { 16000, 1 };
          SEND(pm_put_music_open, 901, &bad); ANSWER(&fd); CHECK(CODE() == PM_ERR_BAD, "a rate it does not take: BAD");
          SEND(pm_put_music_open, 902, &mono3); ANSWER(&fd); CHECK(CODE() == PM_ERR_BAD, "three channels: BAD");
          h_played = 0;
          SEND(pm_put_music_open, 903, &m); ANSWER(&fd);
          CHECK(h.type == PM_OK && h.id == 903 && fd >= 0 && p->ms && lane_mine(p) && h_music[lane_src[p->lane]] == 1, "MUSIC_OPEN: OK with the stream, a lane in the core's table, on (the newest source)");
          { int16_t pcm[1600] = { 1000 }; for (int i = 0; i < 20 && write(fd, pcm, sizeof pcm) > 0; i++) ; for (int i = 0; i < 50 && h_played < 6400; i++) usleep(20000); }
          CHECK(h_played >= 6400, "what it writes reaches the plugin stream, as stereo");
          SEND(pm_put_music_open, 904, &m); ANSWER(&fd2); CHECK(CODE() == PM_ERR_BUSY, "a second one while it is open: BUSY"); if (fd2 >= 0) close(fd2);
          /* the core's table: what the button, the stop word and another source ask of it */
          { const struct music_source *ls = h_src[lane_src[p->lane]]; int src = lane_src[p->lane];
            h_drain(NULL);
            CHECK(ls->button(0) == 1 && got(PM_MUSIC_CMD) && p->mby, "the button's first round: MUSIC_CMD pause, taken");
            { struct pm_music_state st = { PM_MUSIC_PAUSED, pm_s("A"), pm_s("B") }; SEND(pm_put_music_state, 0, &st); }
            for (int i = 0; i < 50 && h_music[src]; i++) usleep(10000);
            CHECK(!h_music[src] && p->mstate == PM_MUSIC_PAUSED, "MUSIC_STATE paused: off in the core's table");
            usleep(400000);
            CHECK(p->ms != NULL, "a paused player writes nothing: its stream stays open through the stream thread's idle polls");
            /* L1 of a review of 92ac2e1: paused, and a few bytes a write into a socket made as small as it goes
             * (the plugin may set its own end's SO_SNDBUF): each read's share of the pace was rounded to whole
             * milliseconds, 0 for one under a millisecond, and this was read unpaced again, 57 % of a core (first: the
             * socket empty, as M1's below leaves it full for a second) */
            /* (twice: as it runs, then the pace alone, with READ_MIN_MS's wait taken out) */
            for (int pass = 0; pass < 2; pass++) { long long sent = 0, t0, cpu, played0 = h_played; int one = 1, was = 0; socklen_t wl = sizeof was; static char tiny[8];
              read_min_ms = pass ? 0 : READ_MIN_MS; usleep(300000); t0 = now_ms();
              getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &was, &wl); setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &one, sizeof one);
              h_cpu_mark();
              while (now_ms() - t0 < 1000) {
                  ssize_t w = write(fd, tiny, sizeof tiny);
                  if (w > 0) sent += w;
                  { struct pollfd pw = { fd, POLLOUT, 0 }; poll(&pw, 1, 50); }
              }
              cpu = h_cpu_others();
              CHECK(sent < 40000 && h_played == played0, !pass ? "L1: paused, 8 bytes a write into the smallest socket: taken at the stream's rate at most, nothing played"
                                                                 : "L1: ... the pace alone (no wait after a small read): the same");
              CHECK(cpu < 150, !pass ? "L1: ... and the stream's thread idles meanwhile (hassmic's threads under 15 % of a core in that second)" : "L1: ... the pace alone: the stream's thread idles all the same");
              if (sent >= 40000 || cpu >= 150) printf("     (%lld bytes taken, %lld ms of CPU besides the writer's)\n", sent, cpu);
              was /= 2; setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &was, sizeof was); }
            read_min_ms = READ_MIN_MS;
            /* M1 of a review of efde36e: one that says "paused" and writes as fast as it can.  What it writes is dropped
             * at the stream's own rate (32 kB a second here: 16 kHz mono), its socket fills, its writes come back
             * EAGAIN; read as fast as it came, it took two thirds of a core and bl some 340 000 times a second */
            { static int16_t junk[4096]; long long sent = 0, played0 = h_played, t0 = now_ms(); struct timespec c0, c1;
              clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c0);
              while (now_ms() - t0 < 1000) {
                  ssize_t w = write(fd, junk, sizeof junk);
                  if (w > 0) sent += w;
                  else { struct pollfd pw = { fd, POLLOUT, 0 }; poll(&pw, 1, 50); }
              }
              clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c1);
              { long long cpu = (c1.tv_sec - c0.tv_sec) * 1000LL + (c1.tv_nsec - c0.tv_nsec) / 1000000;
                CHECK(sent < 200000 && h_played == played0, "M1: paused and writing all the same: taken at the stream's rate (a second's 32 kB and the socket's buffer), nothing played");
                CHECK(cpu < 300, "M1: ... and the stream's thread idles meanwhile (the process's CPU in that second under 30 %)");
                if (sent >= 200000 || cpu >= 300) printf("     (%lld bytes taken, %lld ms of CPU)\n", sent, cpu); } }
            CHECK(ls->button(0) == 0 && ls->button(1) == 1 && got(PM_MUSIC_CMD), "paused by the button: the second round resumes it");
            { struct pm_music_state st = { PM_MUSIC_PLAYING, pm_s("A"), pm_s("B") }; SEND(pm_put_music_state, 0, &st); }
            for (int i = 0; i < 50 && !h_music[src]; i++) usleep(10000);
            CHECK(h_music[src] == 1, "playing again: the newest source again");
            /* ... and while it plays: what it writes is played, and a few bytes a write cost a wakeup of the stream's
             * thread per READ_MIN_MS at most, not one each (L1) */
            { long long sent = 0, t0, cpu, p0; int one = 1, was = 0; socklen_t wl = sizeof was; static char tiny[8];
              do { p0 = h_played; usleep(200000); } while (h_played != p0);        /* (what it wrote while paused played first) */
              t0 = now_ms();
              getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &was, &wl); setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &one, sizeof one);
              h_cpu_mark();
              while (now_ms() - t0 < 1000) {
                  ssize_t w = write(fd, tiny, sizeof tiny);
                  if (w > 0) sent += w;
                  { struct pollfd pw = { fd, POLLOUT, 0 }; poll(&pw, 1, 50); }
              }
              cpu = h_cpu_others();
              for (int i = 0; i < 50 && h_played - p0 < sent; i++) usleep(10000);
              CHECK(sent > 0 && h_played - p0 >= sent && h_played - p0 <= 2 * sent + 64, "L1: playing, 8 bytes a write: all of it played (as stereo)");
              CHECK(cpu < 150, "L1: ... and hassmic's threads under 15 % of a core meanwhile");
              if (cpu >= 150 || h_played - p0 < sent) printf("     (%lld bytes written, %lld played, %lld ms of CPU besides the writer's)\n", sent, h_played - p0, cpu);
              was /= 2; setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &was, sizeof was); }
            /* M6 of a review of efde36e: hassmic's pause holds whether the plugin obeys or not.  This one goes on
             * saying "playing" and writing: the button's pause silences it all the same, the core and Home
             * Assistant hear of it, the next press is no pause again but the resume */
            { static int16_t pcm2[1600]; long long at; int told = h_music_told;
              h_drain(NULL);
              CHECK(ls->button(0) == 1 && got(PM_MUSIC_CMD) && p->myield == MY_HELD, "M6: the button's pause to one that will not obey: held (MY_HELD)");
              music_due();
              CHECK(!h_music[src] && h_music_told > told, "M6: ... off in the core's table, Home Assistant's media player told");
              usleep(100000); at = h_played;
              for (int i = 0; i < 6; i++) { if (write(fd, pcm2, sizeof pcm2) < 0) break; usleep(50000); }
              CHECK(h_played - at <= 2 * 6400, "M6: what it writes on is not played (what was under way at most)");
              CHECK(ls->button(0) == 0, "M6: the next press is not its pause again (it took every press while it said \"playing\")");
              CHECK(ls->button(1) == 1 && p->myield == MY_NONE, "M6: the button's second round resumes it");
              music_due();
              CHECK(h_music[src] == 1, "M6: heard again: on in the core's table");
              /* Home Assistant's pause holds the same way; its play resumes */
              CHECK(plugin_music_ha(PM_MCMD_PAUSE) == 1 && p->myield == MY_HELD && plugin_music_state() == PM_MUSIC_PAUSED, "M6: Home Assistant's pause: held, its media player says paused");
              music_due(); CHECK(!h_music[src], "M6: ... off in the core's table");
              CHECK(plugin_music_ha(PM_MCMD_RESUME) == 1 && p->myield == MY_NONE, "M6: its play: resumed"); music_due();
              CHECK(h_music[src] == 1, "M6: ... on again");
              /* another source: yielded; every other source off: heard again, and the core told (it was not) */
              ls->pause(); CHECK(p->myield == MY_OTHER && !h_music[src], "M6: another source: yielded, off");
              music_due();      /* (what the pause marked told now: what follows is the unyield's own mark, L5 of a review of 92ac2e1) */
              told = h_music_told; ls->unyield(); music_due();
              CHECK(p->myield == MY_NONE && h_music[src] == 1 && h_music_told > told, "M6: every other one off: heard again, on in the core's table, Home Assistant told");
              h_drain(NULL); }
            h_drain(NULL); ls->pause();
            CHECK(got(PM_MUSIC_CMD) && p->myield && !h_music[src], "another source started: MUSIC_CMD pause, it yields, off");
            CHECK(ls->stop() == 0 && ls->button(0) == 0, "yielded: neither the stop word nor the button takes it");
            ls->unyield(); CHECK(!p->myield, "the others stopped: heard again");
            { struct pm_music_state st = { PM_MUSIC_PAUSED, pm_s(""), pm_s("") }, pl = { PM_MUSIC_PLAYING, pm_s(""), pm_s("") };
              SEND(pm_put_music_state, 0, &st); SEND(pm_put_music_state, 0, &pl); }
            for (int i = 0; i < 50 && !h_music[src]; i++) usleep(10000);
            h_drain(NULL);
            CHECK(ls->stop() == 1 && got(PM_MUSIC_CMD), "the stop word: MUSIC_CMD pause, taken");
            CHECK(plugin_music_ha(PM_MCMD_STOP) == 1 && got(PM_MUSIC_CMD), "Home Assistant's media player: its stop reaches the newest plugin source");
            { struct pm_music_state bad = { 9, pm_s(""), pm_s("") }; SEND(pm_put_music_state, 905, &bad); ANSWER(&fd); CHECK(CODE() == PM_ERR_BAD, "a state that is none: BAD"); }
            SEND(pm_put_music_close, 906); ANSWER(&fd2);
            for (int i = 0; i < 100 && p->ms; i++) usleep(20000);
            CHECK(CODE() == 0 && !p->ms && !h_music[src], "MUSIC_CLOSE: the stream ends, off in the core's table"); }
          close(fd);
          for (int i = 0; i < MOPEN_MINUTE; i++) p->mopens[i] = now_ms();
          SEND(pm_put_music_open, 907, &m); ANSWER(&fd); CHECK(CODE() == PM_ERR_LIMIT, "the eleventh stream in a minute: LIMIT");
          memset(p->mopens, 0, sizeof p->mopens); }
        /* M2 of a review of efde36e: a plugin's process that ends while its stream's thread is between the end of the
         * stream and the core's word.  The thread held the gap open for 300 ms (HASSMIC_PLUGIN_MSTREAM_GAP_MS), and
         * exited() comes in it: the lane is still the stream's, and the core hears the source is off */
        { struct pm_music_open m = { 16000, 1 }; int l, src2;
          setenv("HASSMIC_PLUGIN_MSTREAM_GAP_MS", "300", 1);
          SEND(pm_put_music_open, 908, &m); ANSWER(&fd);
          l = p->lane; src2 = l >= 0 ? lane_src[l] : -1;
          CHECK(CODE() == 0 && fd >= 0 && src2 >= 0 && h_music[src2] == 1, "M2: (a stream, on in the core's table)");
          close(fd); usleep(100000);
          h_exited(p);
          for (int i = 0; i < 100 && atomic_load(&mstreams_live); i++) usleep(10000);
          CHECK(src2 >= 0 && !h_music[src2] && atomic_load(&lane_of[l]) == -1, "M2: its process gone while the stream ended: off in the core's table all the same, the lane free");
          unsetenv("HASSMIC_PLUGIN_MSTREAM_GAP_MS");
          p = h_plug(P_RUN, caps); h_as(p, "musicplug"); p->lane = -1; h_drain(NULL); }
        /* L3 of a review of 92ac2e1: the same for a plugin removed (exited() frees its entry at once) while its stream
         * ends.  Another start in that gap took the entry (a memset: no stream, no lane), and the thread's word to the
         * core went by the entry and found no lane: the core's bit on for good.  A start takes another entry now, and
         * the thread tells the core by the lane it opened in, whatever the entry has become (here made another's by
         * hand, as any way into it would) */
        { struct pm_music_open m = { 16000, 1 }; int l, src2; struct plug *q;
          setenv("HASSMIC_PLUGIN_MSTREAM_GAP_MS", "300", 1);
          SEND(pm_put_music_open, 1908, &m); ANSWER(&fd);
          l = p->lane; src2 = l >= 0 ? lane_src[l] : -1;
          CHECK(CODE() == 0 && fd >= 0 && src2 >= 0 && h_music[src2] == 1, "L3: (a stream, on in the core's table)");
          p->gone = 1;                          /* (removed: plugin_remove's mark) */
          close(fd); usleep(100000);
          h_exited(p);
          CHECK(p->state == P_FREE && p->ms, "L3: (removed while its stream ends: the entry free, the stream's thread not done)");
          feature = 0; start("musicplug"); feature = 1;          /* (a start in the gap; not run: plugins "off") */
          q = h_find("musicplug");
          CHECK(q && q != p && p->ms, "L3: a start in that gap takes another entry, not the one whose stream's thread has not ended");
          if (q && q != p) h_free(q);
          h_reuse(p);
          for (int i = 0; i < 100 && atomic_load(&mstreams_live); i++) usleep(10000);
          CHECK(src2 >= 0 && !h_music[src2], "L3: the entry another's meanwhile: off in the core's table all the same (told by the stream's own lane)");
          unsetenv("HASSMIC_PLUGIN_MSTREAM_GAP_MS");
          p = h_plug(P_RUN, caps); h_as(p, "musicplug"); p->lane = -1; h_drain(NULL); }
        /* the volume */
        { struct pm_volume_set v = { 37 }, big = { 101 }; int ok = 0, busy = 0;
          SEND(pm_put_volume_set, 910, &big); ANSWER(&fd); CHECK(CODE() == PM_ERR_BAD, "a volume over 100: BAD");
          for (int i = 0; i < 15; i++) { SEND(pm_put_volume_set, 911 + (uint32_t)i, &v); ANSWER(&fd); ok += CODE() == 0; busy += CODE() == PM_ERR_BUSY; }
          CHECK(h_volume == 37 && ok == 2 * VOL_PER_SEC && busy == 15 - 2 * VOL_PER_SEC, "VOLUME_SET: the Echo's volume, five a second (two seconds' worth at once), then BUSY"); }
        /* M5 of a review of efde36e: twenty a minute, and the Echo's volume set from the latest at most every
         * VOL_APPLY_MS (a log line, the ring and a write to the flash each) */
        { int ok = 0, busy = 0, n0;
          h_skew_ms += 2100; p->msg_sec = 0;
          for (int i = 0; i < 15; i++) { struct pm_volume_set v = { (uint8_t)(40 + i) }; SEND(pm_put_volume_set, 1100 + (uint32_t)i, &v); ANSWER(&fd); ok += CODE() == 0; busy += CODE() == PM_ERR_BUSY; }
          CHECK(ok == VOL_PER_MIN - 2 * VOL_PER_SEC && busy == 15 - ok, "M5: twenty a minute: ten more at once, then BUSY");
          p->r_vol.at = 0; p->msg_sec = 0; busy = 0;           /* (the second's bucket full again; the minute's not) */
          for (int i = 0; i < 4; i++) { struct pm_volume_set v = { 60 }; SEND(pm_put_volume_set, 1120 + (uint32_t)i, &v); ANSWER(&fd); busy += CODE() == PM_ERR_BUSY; }
          CHECK(busy == 4, "M5: ... and with five a second to spend again, still none: the minute's are spent");
          /* coalesced: a burst sets the volume once at once, then its last value once */
          h_skew_ms += 60000; p->msg_sec = 0; p->r_vol.at = 0; p->r_vol_min.at = 0; p->vol_applied = 0; n0 = h_volumes;
          for (int i = 0; i < 8; i++) { struct pm_volume_set v = { (uint8_t)(10 + i) }; SEND(pm_put_volume_set, 1130 + (uint32_t)i, &v); ANSWER(&fd); }
          CHECK(h_volumes == n0 + 1 && h_volume == 10, "M5: eight at once: the Echo's volume set once, at the first");
          vol_flush(0); CHECK(h_volumes == n0 + 1, "M5: ... not again within VOL_APPLY_MS");
          h_skew_ms += VOL_APPLY_MS; vol_flush(0);
          CHECK(h_volumes == n0 + 2 && h_volume == 17, "M5: ... then once more, to the last"); }
        /* L4 of a review of 92ac2e1: a VOLUME_SET that waits for vol_flush, and Home Assistant (or the keys) sets the
         * volume meanwhile: the newer word stands.  The one that waited undid it, up to VOL_APPLY_MS later */
        { struct pm_volume_set a = { 30 }, b2 = { 80 };
          h_volume_echo = 1; plugin_core_state(PM_STATE_IDLE, 0, 25);
          h_skew_ms += 60000; p->msg_sec = 0; p->r_vol.at = 0; p->r_vol_min.at = 0; p->vol_applied = 0;
          SEND(pm_put_volume_set, 1200, &a); ANSWER(&fd);
          CHECK(CODE() == 0 && h_volume == 30 && !p->vol_want, "L4: (the plugin's 30 set at once, its own word back from the core no other's)");
          h_skew_ms += 100; SEND(pm_put_volume_set, 1201, &b2); ANSWER(&fd);
          CHECK(CODE() == 0 && h_volume == 30 && p->vol_want == 81, "L4: (its 80 a moment later waits for vol_flush)");
          h_skew_ms += 200; core_set_volume(20);           /* Home Assistant's */
          h_skew_ms += 300; vol_flush(0);
          CHECK(h_volume == 20 && !p->vol_want, "L4: Home Assistant sets 20 meanwhile: 20 stands, the plugin's 80 that waited is dropped");
          /* (its own value coming back, while another of its waits: not a word from elsewhere) */
          p->vol_want = 91; p->vol_own = 71; plugin_core_state(PM_STATE_IDLE, 0, 70);
          CHECK(p->vol_want == 91, "L4: the volume the plugin itself is setting, coming back: what waits stays");
          plugin_core_state(PM_STATE_IDLE, 0, 72);
          CHECK(p->vol_want == 0, "L4: ... any other: dropped");
          p->vol_own = 0; h_volume_echo = 0; }
        /* what it is told of the Echo */
        h_drain(NULL); cs_vol = -1; p->told_state = p->told_volume = -1;
        plugin_core_state(PM_STATE_LISTENING, 0, 42);
        { int st = 0, vol = 0; while ((r = recv(h_peer, b, sizeof b, MSG_DONTWAIT)) > 0) { if (!pm_head(b, (size_t)r, &h, &body, &bl)) { st |= h.type == PM_STATE; vol |= h.type == PM_VOLUME; } }
          CHECK(st && vol, "the Echo listens: STATE (state) and VOLUME (audio.volume)"); }
        plugin_core_state(PM_STATE_LISTENING, 0, 42); CHECK(h_drain(NULL) == 0, "nothing changed: nothing said");
        /* UDP of its own and the search */
        { struct pm_udp_open lan = { PM_CAP_NET_LAN, 0 }, two = { PM_CAP_NET_UDP, 1 }, u = { PM_CAP_NET_UDP, 0 }, d = { PM_CAP_NET_DISCOVER, 0 }; struct pm_udp_opened o; int ufd, dfd;
          SEND(pm_put_udp_open, 920, &lan); ANSWER(&fd); CHECK(CODE() == PM_ERR_DENIED, "UDP_OPEN of a capability it has not: DENIED");
          SEND(pm_put_udp_open, 921, &two); ANSWER(&fd); CHECK(CODE() == PM_ERR_DENIED, "an entry past its count: DENIED");
          SEND(pm_put_udp_open, 922, &u); ANSWER(&ufd);
          CHECK(h.type == PM_UDP_OPENED && ufd >= 0 && !pm_get_udp_opened(body, bl, &o) && o.port >= PS_PORT_FIRST && o.port <= PS_PORT_LAST, "UDP_OPEN: UDP_OPENED with its socket, a port of the plugins' range");
          { struct sockaddr_in a; socklen_t al = sizeof a; getsockname(ufd, (struct sockaddr *)&a, &al); CHECK(ntohs(a.sin_port) == o.port, "(bound to it)"); }
          { char path[400]; FILE *f; unsigned p1 = 0, p2 = 0; snprintf(path, sizeof path, "%s/plugins/musicplug/ports", h_dir);
            if ((f = fopen(path, "r"))) { if (fscanf(f, "%u %u", &p1, &p2) != 2) p1 = 0; fclose(f); }
            CHECK(p1 == o.port && p2 && p2 != p1, "its ports kept in its folder (net.udp's, then net.discover's): the same at the next start"); }
          { int x; SEND(pm_put_udp_open, 923, &u); ANSWER(&x); CHECK(CODE() == PM_ERR_BUSY, "opened again: BUSY"); if (x >= 0) close(x); }
          /* a datagram from a public address is not heard: the socket's own filter (on a PC the only wall) */
          { int s1 = socket(AF_INET, SOCK_DGRAM, 0); struct sockaddr_in to = { AF_INET, htons(o.port), { htonl(INADDR_LOOPBACK) }, { 0 } }; char rb[16];
            sendto(s1, "x", 1, 0, (struct sockaddr *)&to, sizeof to); usleep(50000);
            CHECK(recv(ufd, rb, sizeof rb, MSG_DONTWAIT) < 0, "a sender that is not on the network (here loopback): dropped by the socket's filter"); close(s1); }
          { struct pm_udp_peer pub = { (uint8_t)o.handle, 0xC6336407u, 4000 }, adb = { (uint8_t)o.handle, 0x0A000009u, 5555 }, lo = { (uint8_t)o.handle, 0x7F000001u, 4000 }, bc = { (uint8_t)o.handle, 0xFFFFFFFFu, 4000 }, lanp = { (uint8_t)o.handle, 0x0A000009u, 4000 };
            /* (each with its bucket full: UDP_PEER is one a second, A2 below) */
            p->r_peer.at = 0; SEND(pm_put_udp_peer, 924, &pub); ANSWER(&fd); CHECK(CODE() == PM_ERR_DENIED, "UDP_PEER to a public address: DENIED");
            p->r_peer.at = 0; SEND(pm_put_udp_peer, 925, &adb); ANSWER(&fd); CHECK(CODE() == PM_ERR_DENIED, "to port 5555: DENIED");
            { struct pm_udp_peer dns = { (uint8_t)o.handle, 0x0A000009u, 53 }, dot = { (uint8_t)o.handle, 0xC0A80101u, 853 };
              p->r_peer.at = 0; SEND(pm_put_udp_peer, 935, &dns); ANSWER(&fd); CHECK(CODE() == PM_ERR_DENIED, "N1: to DNS's port 53 on the network (the router asks the internet): DENIED");
              p->r_peer.at = 0; SEND(pm_put_udp_peer, 936, &dot); ANSWER(&fd); CHECK(CODE() == PM_ERR_DENIED, "N1: ... and 853: DENIED"); }
            p->r_peer.at = 0; SEND(pm_put_udp_peer, 926, &lo); ANSWER(&fd); CHECK(CODE() == PM_ERR_DENIED, "to loopback: DENIED");
            p->r_peer.at = 0; SEND(pm_put_udp_peer, 927, &bc); ANSWER(&fd); CHECK(CODE() == PM_ERR_DENIED, "to the broadcast: DENIED");
            /* (A3: and only where the kernel routes it out of an interface: in a namespace with only lo, as tests/run.py
             * runs this, nothing does, and DENIED is right there) */
            { int routed = pnet_address_ok(htonl(0x0A000009u), 4000);
              p->r_peer.at = 0; SEND(pm_put_udp_peer, 928, &lanp); ANSWER(&fd);
              CHECK(routed ? CODE() != PM_ERR_DENIED : CODE() == PM_ERR_DENIED, routed ? "to a private address: not refused (OK, or FAILED where nothing routes it)" : "to a private address nothing routes (a namespace with only lo): DENIED (A3)"); }
            /* A2 of a review of efde36e: a new peer a second (two at once), or UDP_PEER and send() scanned the network's
             * ports two hundred addresses a second */
            { int busy = 0, c1, c2; p->r_peer.at = 0;
              SEND(pm_put_udp_peer, 960, &pub); ANSWER(&fd); c1 = CODE(); SEND(pm_put_udp_peer, 961, &pub); ANSWER(&fd); c2 = CODE();
              for (int i = 0; i < 20; i++) { SEND(pm_put_udp_peer, 962, &lanp); ANSWER(&fd); busy += CODE() == PM_ERR_BUSY; }
              CHECK(c1 != PM_ERR_BUSY && c2 != PM_ERR_BUSY && busy == 20, "A2: UDP_PEER: two at once, then BUSY (one a second)");
              h_skew_ms += 1100; SEND(pm_put_udp_peer, 963, &lanp); ANSWER(&fd);
              CHECK(CODE() != PM_ERR_BUSY, "A2: ... and one again a second later"); p->msg_sec = 0; } }
          { struct pm_sendto st = { (uint8_t)o.handle, 0x0A000009u, 4000, pm_b("x", 1) }; int busy = 0;
            for (int i = 0; i < 2 * SEND_PER_SEC + 10; i++) { SEND(pm_put_sendto, 0, &st); }
            for (int i = 0; i < 2 * SEND_PER_SEC + 10; i++) { SEND(pm_put_sendto, 930, &st); ANSWER(&fd); busy += CODE() == PM_ERR_BUSY; }
            CHECK(busy > 0, "SENDTO: fifty a second (two seconds' worth at once), then BUSY");
            p->msg_sec = 0; h_skew_ms += 1100; }       /* (past the broker's 200 messages a second, a new second) */
          { struct pm_broadcast bu = { (uint8_t)o.handle, pm_b("?", 1) };
            SEND(pm_put_broadcast, 931, &bu); ANSWER(&fd); CHECK(CODE() == PM_ERR_BAD, "BROADCAST from a net.udp socket: BAD"); }
          SEND(pm_put_udp_open, 932, &d); ANSWER(&dfd);
          CHECK(h.type == PM_UDP_OPENED && dfd >= 0 && !pm_get_udp_opened(body, bl, &o), "net.discover's socket");
          { unsigned char loop = 1; socklen_t ll = sizeof loop; int ul = 1; socklen_t ull = sizeof ul;
            CHECK(!getsockopt(dfd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, &ll) && loop == 0 && !getsockopt(ufd, IPPROTO_IP, IP_MULTICAST_LOOP, &ul, &ull),
                  "A1: the search's socket does not hand its multicast back to the Echo's own sockets (IP_MULTICAST_LOOP 0)"); }
          { struct pm_broadcast bd = { (uint8_t)o.handle, pm_b("?", 1) }; struct pm_udp_peer pd = { (uint8_t)o.handle, 0x0A000009u, 4000 };
            SEND(pm_put_broadcast, 933, &bd); ANSWER(&fd); int c1 = CODE();
            SEND(pm_put_broadcast, 934, &bd); ANSWER(&fd);
            CHECK(c1 != PM_ERR_BUSY && CODE() == PM_ERR_BUSY, "BROADCAST: once a second, the second refused (BUSY)");
            SEND(pm_put_udp_peer, 935, &pd); ANSWER(&fd); CHECK(CODE() == PM_ERR_BAD, "a peer for the search's socket: BAD"); }
          close(ufd); close(dfd); }
        /* proposals */
        { struct pm_propose pub = { pm_s("server"), pm_s("198.51.100.7") }, row = { pm_s("note"), pm_s("10.0.0.9") }, ok = { pm_s("server"), pm_s("10.0.0.9") }, z = { pm_s("server"), pm_s("10.0.0.09") };
          char err[100] = "", have[64] = ""; const struct ps_plugin *sp;
          SEND(pm_put_propose, 940, &pub); ANSWER(&fd); CHECK(CODE() == PM_ERR_DENIED, "PROPOSE a public address: DENIED (the owner types those)");
          SEND(pm_put_propose, 941, &row); ANSWER(&fd); CHECK(CODE() == PM_ERR_DENIED, "for a row its net.server does not name: DENIED");
          SEND(pm_put_propose, 942, &z); ANSWER(&fd); CHECK(CODE() == PM_ERR_DENIED, "an address written otherwise (a zero in front): DENIED");
          /* (an address proposed must leave the Echo by the kernel's routes, A3: where this PC has no route to 10/8 at
           * all, as in a namespace with only lo, there is nothing to check here) */
          if (!pnet_address_ok(htonl(0x0A000009u), 1)) printf("skip proposals of 10.0.0.9: no route to it on this machine\n");
          else {
          SEND(pm_put_propose, 943, &ok); ANSWER(&fd); CHECK(CODE() == 0 && p->prop[0].live, "a private address for its server's row: shown");
          SEND(pm_put_propose, 944, &ok); ANSWER(&fd); CHECK(CODE() == PM_ERR_BUSY, "again within the minute: BUSY");
          ps_lock(); sp = ps_find("musicplug"); ps_config_owner(sp, "server", have, sizeof have); ps_unlock();
          CHECK(!have[0], "nothing written by it");
          CHECK(plugin_propose_take("musicplug", "server", "10.0.0.8", 0, err, sizeof err) == -1, "the owner's click for another value: none such");
          /* a proposal whose row an update took out of net.server: nothing written (a review of efde36e, A4's nit) */
          snprintf(p->prop[1].setting, sizeof p->prop[1].setting, "note"); snprintf(p->prop[1].value, sizeof p->prop[1].value, "10.0.0.9"); p->prop[1].live = 1; p->prop[1].at = now_ms();
          CHECK(plugin_propose_take("musicplug", "note", "10.0.0.9", 0, err, sizeof err) == -1 && strstr(err, "no longer"), "a proposal for a row net.server does not name (any more): not taken");
          CHECK(plugin_propose_take("musicplug", "server", "10.0.0.9", 0, err, sizeof err) == 0, "the owner's click: taken");
          ps_lock(); sp = ps_find("musicplug"); ps_config_owner(sp, "server", have, sizeof have); ps_unlock();
          CHECK(!strcmp(have, "10.0.0.9") && !p->prop[0].live, "written as the owner's own value, the proposal gone"); } }
        /* Home Assistant: entities, events, actions */
        p->state = P_RUN; h_drain(NULL);
        { struct pm_entity_state lv = { pm_s("level"), pm_s("7") }, hi = { pm_s("level"), pm_s("11") }, no = { pm_s("nope"), pm_s("1") }, sw = { pm_s("on"), pm_s("yes") };
          struct pm_event ev = { pm_s("seen"), pm_s("ping") }, evb = { pm_s("seen"), pm_s("pong") };
          unsigned key = key_of("musicplug", "level", 0); int kind; char v[64];
          SEND(pm_put_entity_state, 950, &hi); ANSWER(&fd); CHECK(CODE() == PM_ERR_BAD, "a number over its max: BAD");
          SEND(pm_put_entity_state, 951, &no); ANSWER(&fd); CHECK(CODE() == PM_ERR_BAD, "an entity it has not: BAD");
          SEND(pm_put_entity_state, 952, &sw); ANSWER(&fd); CHECK(CODE() == PM_ERR_BAD, "a switch that is neither on nor off: BAD");
          h_entities = 0;
          SEND(pm_put_entity_state, 953, &lv); ANSWER(&fd); ha_flush();
          CHECK(CODE() == 0 && h_entities == 1 && h_entity == key && plugin_entity_value(key, &kind, v, sizeof v) == 1 && !strcmp(v, "7") && kind == PE_NUMBER,
                "ENTITY_STATE: kept, Home Assistant told by its key, read back");
          SEND(pm_put_event, 954, &evb); ANSWER(&fd); CHECK(CODE() == PM_ERR_BAD, "an event type it does not list: BAD");
          SEND(pm_put_event, 955, &ev); ANSWER(&fd);
          CHECK(CODE() == 0 && h_entity == key_of("musicplug", "seen", 0) && !strcmp(h_event, "ping"), "EVENT: Home Assistant told its type");
          { int busy = 0; h_skew_ms += 3000; for (int i = 0; i < 40; i++) { SEND(pm_put_entity_state, 960, &lv); ANSWER(&fd); busy += CODE() == PM_ERR_BUSY; }
            CHECK(busy >= 40 - 2 * ENT_PER_SEC - 1, "states: ten a second for all of them, then BUSY"); p->msg_sec = 0; h_skew_ms += 1100; }
          h_drain(NULL);
          CHECK(plugin_entity_command(key_of("musicplug", "on", 0), "maybe") == -1, "Home Assistant's command a switch cannot take: not sent");
          CHECK(plugin_entity_command(key_of("musicplug", "on", 0), "on") == 0 && got(PM_ENTITY_SET), "a switch set: ENTITY_SET");
          CHECK(plugin_entity_command(key_of("musicplug", "level", 0), "10") == 0 && plugin_entity_command(key_of("musicplug", "level", 0), "10.5") == -1, "a number: within its limits only");
          h_drain(NULL);
          { char a[4][256] = { "" }; int ints[4] = { 12 };
            CHECK(plugin_action(key_of("musicplug", "say", 1), a, ints, 0) == -1, "an argument missing: not sent");
            CHECK(plugin_action(key_of("musicplug", "say", 1), a, ints, 1) == 0 && got(PM_ACTION), "Home Assistant's call: ACTION"); }
          CHECK(plugin_action_page("musicplug", "say") == -1 && plugin_action_page("musicplug", "go") == 0, "the card's button: only for an action marked page");
          { int n = 0; plugin_entities_each(h_count, &n);
            CHECK(n == 8, "listed: five entities and three actions"); }
          /* not running: unavailable */
          p->state = P_DEAD;
          CHECK(plugin_entity_value(key, &kind, v, sizeof v) == 0, "its plugin not running: unavailable");
          p->state = P_RUN; }
        /* ---- H1 of a review of efde36e: a text a plugin says is strict UTF-8, or it is not Home Assistant's (its link
         * dropped on it, and on every link after).  One bad form at a time, each refused and nothing kept */
        { static const struct { const char *t, *what; } bad[] = {
              { "x\xc0\x80y", "an overlong NUL (C0 80)" }, { "\xc1\xbf", "an overlong two bytes (C1 BF)" }, { "\xe0\x80\x80", "an overlong three bytes (E0 80 80)" },
              { "\xf0\x80\x80\x80", "an overlong four bytes (F0 80 80 80)" }, { "\xed\xa0\x80", "half a surrogate pair (ED A0 80)" },
              { "\xed\xbf\xbf", "the other half (ED BF BF)" }, { "\xf4\x90\x80\x80", "past U+10FFFF (F4 90 80 80)" }, { "\xf5\x80\x80\x80", "a lead byte past F4" },
              { "a\x80" "b", "a lone continuation byte" }, { "\xe2\x82", "a character cut short" }, { "\xf8\x88\x80\x80\x80", "five bytes" }, { "\xff", "FF" } };
          static const char *const good[] = { "caf\xc3\xa9", "\xe2\x82\xac 5", "\xf0\x9f\x98\x80", "\xf4\x8f\xbf\xbf", "\xed\x9f\xbf", "\xee\x80\x80" };
          unsigned wk = key_of("musicplug", "words", 0); int kind, ok = 0; char v[300];
          h_skew_ms += 4000; p->msg_sec = 0; p->r_ent.at = 0; h_drain(NULL);
          for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
              struct pm_entity_state m = { pm_s("words"), pm_s(bad[i].t) }; char what[120];
              p->ent_have &= ~(1u << 3); p->r_ent.at = 0;
              SEND(pm_put_entity_state, 980 + (uint32_t)i, &m); ANSWER(&fd);
              snprintf(what, sizeof what, "H1: a text sensor's state with %s: BAD, nothing kept", bad[i].what);
              CHECK(CODE() == PM_ERR_BAD && plugin_entity_value(wk, &kind, v, sizeof v) == 0 && !ps_utf8_ok(bad[i].t, strlen(bad[i].t)), what);
          }
          for (size_t i = 0; i < sizeof good / sizeof good[0]; i++) {
              struct pm_entity_state m = { pm_s("words"), pm_s(good[i]) };
              p->r_ent.at = 0;
              SEND(pm_put_entity_state, 995 + (uint32_t)i, &m); ANSWER(&fd);
              ok += CODE() == 0 && plugin_entity_value(wk, &kind, v, sizeof v) == 1 && !strcmp(v, good[i]);
          }
          CHECK(ok == (int)(sizeof good / sizeof good[0]), "H1: well-formed two, three and four bytes (U+10FFFF, U+D7FF, U+E000 too): kept as they came");
          /* a title (the card shows it): cut before the first byte that is none */
          { struct pm_music_state st = { PM_MUSIC_PAUSED, pm_s("Song \xed\xa0\x80 one"), pm_s("A\xc0\x80") }; p->r_mstate.at = 0; SEND(pm_put_music_state, 0, &st); }
          CHECK(!strcmp(p->mtitle, "Song ") && !strcmp(p->martist, "A"), "H1: a title or an artist that is not UTF-8: cut where it stops being so");
          /* what only the log and the page show: mended, "?" for each byte that is no character */
          { char t[] = "ok\xc0\x80 \xed\xa0\x80\x01\xe2\x82"; ps_utf8_mend(t);
            CHECK(!strcmp(t, "ok?? ??? ??") && ps_utf8_ok(t, strlen(t)), "H1: a line for the log or a reason for the page: every byte that is no character a \"?\", a control character a space"); }
          /* an action's string argument on its way to the plugin, Home Assistant's: the same rule */
          { char a[4][256] = { "\xed\xa0\x80" }; int ints[4] = { 0, 3 }; h_drain(NULL);
            CHECK(plugin_action(key_of("musicplug", "tell", 1), a, ints, 2) == -1, "H1: an action's text argument that is not UTF-8: not sent"); } }
        /* ---- H6: an empty string is no field on the wire (proto3): typed by the manifest, it reaches the plugin as
         * "", never as the int 0 */
        { char a[4][256] = { "" }; int ints[4] = { 0, 7 }; uint8_t pk[PM_MAX_PACKET]; ssize_t r2; struct pm_action am; struct pm_action_args aa; int seen = 0;
          h_drain(NULL);
          CHECK(plugin_action(key_of("musicplug", "tell", 1), a, ints, 2) == 0, "H6: an action with an empty string argument: sent");
          while ((r2 = recv(h_peer, pk, sizeof pk, MSG_DONTWAIT)) > 0) if (!pm_head(pk, (size_t)r2, &h, &body, &bl) && h.type == PM_ACTION && !pm_get_action(body, bl, &am)) {
              while (pm_next_action_args(&am.args, &aa)) seen |= (pm_streq(aa.name, "what") && pm_streq(aa.value, "")) << 0 | (pm_streq(aa.name, "n") && pm_streq(aa.value, "7")) << 1;
          }
          CHECK(seen == 3, "H6: ... as \"\" (its manifest says string), and the int beside it as 7"); }
        /* ---- H5: a number's limit as Home Assistant has it (float32) is the limit */
        { unsigned fk = key_of("musicplug", "frac", 0); char top[32], bot[32];
          snprintf(top, sizeof top, "%.9g", (double)(float)0.1); snprintf(bot, sizeof bot, "%.9g", (double)(float)-0.1);
          h_drain(NULL);
          CHECK(strtod(top, NULL) > 0.1 && plugin_entity_command(fk, top) == 0 && plugin_entity_command(fk, bot) == 0, "H5: the slider at its top and bottom (0.1 and -0.1 as float32, past the manifest's): taken");
          CHECK(plugin_entity_command(fk, "0.1000001") == -1 && plugin_entity_command(fk, "-0.1000001") == -1, "H5: ... and nothing past them");
          { struct pm_entity_state m = { pm_s("frac"), pm_s(top) }; int kind; char v[64]; p->r_ent.at = 0;
            SEND(pm_put_entity_state, 1001, &m); ANSWER(&fd);
            CHECK(CODE() == 0 && plugin_entity_value(fk, &kind, v, sizeof v) == 1 && strtod(v, NULL) == 0.1, "H5: the plugin's state at it too, kept as the limit itself"); } }
        /* ---- H2: keys given and kept, never two plugins' (FNV-1a in 30 bits gave aaamusic's x_ng81qmb and
         * testmusic's loud the one key 4a8f8208), never hassmic's own range, the same after a restart, never again
         * once a plugin is gone; one that cannot be written down is not used */
        { const struct ps_plugin *l; unsigned k1, k2, k3, all[3]; struct listing li;
          h_add_full("aaamusic", "\"ha.entities\": [ { \"name\": \"x_ng81qmb\", \"kind\": \"switch\", \"label\": \"Aaa\" } ]", "", "aaa\n");
          h_add_full("testmusic", "\"ha.entities\": [ { \"name\": \"loud\", \"kind\": \"switch\", \"label\": \"Loud\" } ]", "", "tm\n");
          ps_lock(); ps_scan(&l); ps_unlock();
          k1 = key_of("aaamusic", "x_ng81qmb", 0); k2 = key_of("testmusic", "loud", 0); k3 = key_of("musicplug", "level", 0);
          CHECK(k1 && k2 && k1 != k2 && k1 != k3 && k2 != k3 && k1 >= PS_HAKEY_FIRST && k2 >= PS_HAKEY_FIRST && k1 <= PS_HAKEY_LAST, "H2: the pair a 30-bit hash gave one key: two keys, both in the plugins' range above hassmic's own");
          ps_hakeys_forget(); ps_lock(); ps_scan(&l); ps_unlock();
          all[0] = key_of("aaamusic", "x_ng81qmb", 0); all[1] = key_of("testmusic", "loud", 0); all[2] = key_of("musicplug", "level", 0);
          CHECK(all[0] == k1 && all[1] == k2 && all[2] == k3, "H2: read back from state/plugins/.hakeys: the same keys after a restart");
          { char d[300]; snprintf(d, sizeof d, "%s/plugins/aaamusic", h_dir); nftw(d, h_rm, 16, FTW_DEPTH | FTW_PHYS); }
          h_add_full("bbbmusic", "\"ha.entities\": [ { \"name\": \"x_ng81qmb\", \"kind\": \"switch\", \"label\": \"Bbb\" } ]", "", "bbb\n");
          ps_lock(); ps_scan(&l); ps_unlock();
          CHECK(key_of("bbbmusic", "x_ng81qmb", 0) > k1 && key_of("bbbmusic", "x_ng81qmb", 0) > k2, "H2: a plugin removed: its key is never given again, a new one is the next");
          setenv("HASSMIC_PLUGIN_FAIL_HAKEYS", "1", 1); ps_hakeys_forget();
          h_add_full("cccmusic", "\"ha.entities\": [ { \"name\": \"c\", \"kind\": \"switch\", \"label\": \"Ccc\" } ]", "", "ccc\n");
          ps_lock(); ps_scan(&l); ps_unlock();
          memset(&li, 0, sizeof li); plugin_entities_each(h_list, &li);
          { int seen = 0; for (int i = 0; i < li.n; i++) seen |= !strcmp(li.oids[i], "p8_cccmusic_c");
            CHECK(!key_of("cccmusic", "c", 0) && !seen && key_of("testmusic", "loud", 0) == k2, "H2: a key that could not be written down: none, not listed (the others as they were)"); }
          unsetenv("HASSMIC_PLUGIN_FAIL_HAKEYS"); ps_hakeys_forget(); ps_lock(); ps_scan(&l); ps_unlock();
          CHECK(key_of("cccmusic", "c", 0) > key_of("bbbmusic", "x_ng81qmb", 0), "H2: written once the flash takes it");
          /* K3 of a review of 92ac2e1: state/ emptied (a factory reset) while Home Assistant keeps what it last
           * listed: the counter starts at a random place, not at PS_HAKEY_FIRST again (where the first plugin's
           * first key would have been the last one's, and Home Assistant took it for that entity renamed) */
          { char hk[400], aside[420]; unsigned s1, s2;
            snprintf(hk, sizeof hk, "%s/plugins/.hakeys", h_dir); snprintf(aside, sizeof aside, "%s.aside", hk);
            CHECK(!rename(hk, aside), "(K3: .hakeys put aside)");
            ps_hakeys_forget(); ps_lock(); ps_scan(&l); ps_unlock(); s1 = key_of("musicplug", "level", 0); unlink(hk);
            ps_hakeys_forget(); ps_lock(); ps_scan(&l); ps_unlock(); s2 = key_of("musicplug", "level", 0); unlink(hk);
            CHECK(s1 >= PS_HAKEY_FIRST && s1 < PS_HAKEY_FIRST + 0x10000000u + 64 && s2 >= PS_HAKEY_FIRST && s2 < PS_HAKEY_FIRST + 0x10000000u + 64 && s1 != s2
                  && s1 - PS_HAKEY_FIRST >= 64 && s2 - PS_HAKEY_FIRST >= 64,
                  "K3: no .hakeys (a factory reset): the keys begin at a random place in the plugins' first quarter, another each time, not at its first key");
            if (s1 == s2 || s1 - PS_HAKEY_FIRST < 64) printf("     (keys %08x %08x)\n", s1, s2);
            CHECK(!rename(aside, hk), "(K3: .hakeys back)");
            ps_hakeys_forget(); ps_lock(); ps_scan(&l); ps_unlock();
            CHECK(key_of("testmusic", "loud", 0) == k2 && key_of("musicplug", "level", 0) == k3, "(K3: the keys of before, read back)"); }
          /* ---- H3, K1, K2: the names Home Assistant keeps entities by and makes their entity ids of.  Never
           * hassmic's own (tests/unit/ha_names_test.py, slugified as Home Assistant does), never two plugins' alike:
           * all of these are "Stand-in" (h_add_full), and two have a switch labelled "Loud".  Both listed, each under
           * a name and an entity id of its own, whichever is switched off (the later one was left out before, and
           * took the first one's name once that was switched off: K2 of a review of 92ac2e1) */
          h_add_full("dddmusic", "\"ha.entities\": [ { \"name\": \"loud\", \"kind\": \"switch\", \"label\": \"Loud\" }, { \"name\": \"q\", \"kind\": \"sensor\", \"label\": \"Loud2\" } ]", "", "ddd\n");
          ps_lock(); ps_scan(&l); ps_unlock();
          memset(&li, 0, sizeof li); plugin_entities_each(h_list, &li);
          { int t_loud = 0, d_loud = 0;
            for (int i = 0; i < li.n; i++) {
                t_loud += li.keys[i] == key_of("testmusic", "loud", 0) && !strcmp(li.names[i], "Plugin 9 testmusic: Loud");
                d_loud += li.keys[i] == key_of("dddmusic", "loud", 0) && !strcmp(li.names[i], "Plugin 8 dddmusic: Loud");
            }
            CHECK(t_loud == 1 && d_loud == 1, "K2: two plugins' switches labelled alike: both listed, each by a name of its own (\"Plugin <length> <id>: <label>\")");
            { int kind; char v[8]; CHECK(plugin_entity_value(key_of("dddmusic", "loud", 0), &kind, v, sizeof v) >= 0 && kind == PE_SWITCH, "K2: ... the later one's key is an entity of the list (unavailable while it does not run)"); } }
          plugin_enable("testmusic", 0);
          memset(&li, 0, sizeof li); plugin_entities_each(h_list, &li);
          { int t_loud = 0, d_loud = 0;
            for (int i = 0; i < li.n; i++) { t_loud |= li.keys[i] == key_of("testmusic", "loud", 0); d_loud += !strcmp(li.names[i], "Plugin 8 dddmusic: Loud"); }
            CHECK(!t_loud && d_loud == 1, "K2: the first one switched off: the other's name is still its own (not the first one's)"); }
          plugin_enable("testmusic", 1);
          memset(&li, 0, sizeof li); plugin_entities_each(h_list, &li);
          { int names_ok = 1, oid_ok = 1, slug_ok = 1;
            /* Home Assistant's slugify on ASCII (ha_names_test.py holds it to Home Assistant's own): every run of
             * anything but a-z0-9 one "_", lower case */
            char sl[64][160];
            for (int i = 0; i < li.n && i < 64; i++) {
                size_t o = 0; int sep = 0;
                if (li.kinds[i] < 0) { snprintf(sl[i], sizeof sl[i], "(action %d)", i); continue; }     /* (an action: its service is its object id) */
                for (const char *c = li.names[i]; *c && o + 2 < sizeof sl[0]; c++) {
                    int al = (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'Z');
                    if (!al) { sep = o > 0; continue; }
                    if (sep) { sl[i][o++] = '_'; sep = 0; }
                    sl[i][o++] = (char)(*c >= 'A' && *c <= 'Z' ? *c + 32 : *c);
                }
                sl[i][o] = 0;
                slug_ok &= !strncmp(sl[i], "plugin_", 7) && sl[i][7] >= '0' && sl[i][7] <= '9';
            }
            for (int i = 0; i < li.n; i++) for (int j = 0; j < i; j++) {
                names_ok &= strcmp(li.names[i], li.names[j]) || li.keys[i] == li.keys[j]; oid_ok &= !!strcmp(li.oids[i], li.oids[j]);
                if (i < 64) slug_ok &= !!strcmp(sl[i], sl[j]);
            }
            CHECK(names_ok && oid_ok && slug_ok && li.n > 4, "H3, K1: every name, entity id and object id listed is one entity's alone, every entity id \"plugin_<digits>_...\""); }
          { int found = 0; for (int i = 0; i < li.n; i++) found |= !strcmp(li.oids[i], "p9_musicplug_level") && !strcmp(li.names[i], "Plugin 9 musicplug: Level");
            CHECK(found, "H3, K1: object id p<length of the id>_<id>_<name>, name \"Plugin <length of the id> <id>: <label>\""); }
          /* the object id: "a-b" with "c" and "a" with "b_c" were both p_a_b_c */
          { char o1[64], o2[64]; ha_object_id("ab-cd", "e", o1, sizeof o1); ha_object_id("ab", "cd_e", o2, sizeof o2);
            CHECK(strcmp(o1, o2) && !strcmp(o1, "p5_ab_cd_e") && !strcmp(o2, "p2_ab_cd_e"), "H3: the id's length in the object id: \"ab-cd\"+\"e\" and \"ab\"+\"cd_e\" two services, not one"); }
          /* ---- H4: whatever changes the list closes Home Assistant's links (ha_relist): an update that drops
           * ha.entities, a plugin switched off, a revocation list (here: a capability this build does not have,
           * which the same scan refuses), plugins off and the launcher gone; nothing changed, nothing closed */
          { int was;
            ha_relist(); was = h_listed; ha_relist();
            CHECK(h_listed == was, "H4: nothing changed: the links stay");
            h_add_full("dddmusic", "\"state\": true", "", "ddd 2\n"); plugin_installed("dddmusic");
            CHECK(h_listed == was + 1 && !key_of("dddmusic", "q", 0), "H4: an update without ha.entities: the links closed, its entities gone (asked by what the old version had too)");
            was = h_listed; plugin_enable("testmusic", 0);
            CHECK(h_listed == was + 1, "H4: switched off: closed");
            was = h_listed; plugin_enable("testmusic", 1);
            CHECK(h_listed == was + 1, "H4: on again: closed, listed again");
            was = h_listed; ps_scope_set(PM_SCOPE_HA_ENTITIES, 0); plugin_withdrawn_changed();
            CHECK(h_listed == was + 1, "H4: refused at a scan (as a revocation list does it): closed");
            ps_scope_set(PM_SCOPE_HA_ENTITIES, 1); was = h_listed; plugin_withdrawn_changed();
            CHECK(h_listed == was + 1, "H4: and back");
            was = h_listed; ps_community(0); plugin_withdrawn_changed();
            CHECK(h_listed == was + 1, "H4: community plugins switched off (these come from files): closed, their entities gone");
            was = h_listed; ps_community(1); plugin_withdrawn_changed();
            CHECK(h_listed == was + 1, "H4: and on again");
            was = h_listed; plugin_resume(); CHECK(h_listed == was, "H4: hassmic's crashes forgiven: the list as it was, nothing closed");
            was = h_listed; { int sb = sandbox_ok; sandbox_ok = 0; ha_relist(); CHECK(h_listed == was + 1, "H4: no sandbox (plugins cannot run): every plugin's entity unlisted"); sandbox_ok = sb; ha_relist(); }
            /* plugins switched off: the feature's own switch closes the links (settings.c); the broker only takes the
             * new list's digest, or Home Assistant would reconnect twice */
            was = h_listed; set_feature(0);
            { int n = 0; plugin_entities_each(h_count, &n); CHECK(n == 0, "H4: plugins off: none listed"); }
            ha_relist_as(relist_due != 2); CHECK(h_listed == was, "H4: ... the broker takes the list's digest without closing the links again (the feature's switch did)");
            ha_relist(); CHECK(h_listed == was, "H4: ... and the list as it is is no change after that");
            set_feature(1); ha_relist_as(0); }
          /* the stand-ins of these checks go again (the store's list is what the rest of this test reads) */
          for (const char *const *g = (const char *const[]){ "testmusic", "bbbmusic", "cccmusic", "dddmusic", NULL }; *g; g++) { char d[300]; snprintf(d, sizeof d, "%s/plugins/%s", h_dir, *g); nftw(d, h_rm, 16, FTW_DEPTH | FTW_PHYS); }
          ps_lock(); ps_scan(&l); ps_unlock();
          /* (and their entries: the checks above started some, and the refusal stopped musicplug's) */
          for (int i = 1; i < NPLUG; i++) if (plugs[i].state != P_FREE && strcmp(plugs[i].id, H_ID)) { plugs[i].state = P_FREE; plugs[i].slot = -1; }
          p->state = P_RUN; p->want = 1; p->why[0] = 0; }
        /* a capability it has not */
        { struct pm_entity_state lv = { pm_s("level"), pm_s("7") }; struct pm_music_open m = { 16000, 1 };
          p->caps = 0;
          SEND(pm_put_entity_state, 970, &lv); ANSWER(&fd); CHECK(CODE() == PM_ERR_DENIED, "ENTITY_STATE without ha.entities: DENIED");
          SEND(pm_put_music_open, 971, &m); ANSWER(&fd); CHECK(CODE() == PM_ERR_DENIED, "MUSIC_OPEN without audio.music: DENIED"); }
        /* ---- calls (docs/plugins.md 3.6, 4.8), every check of the broker's both ways.  The core hears of each
         * state from the broker's teller thread (h_call_state), the claim and the media are call.c's own */
        { const struct ps_plugin *l; uint32_t ccaps = PM_CAPBIT(PM_CAP_CALL) | PM_CAPBIT(PM_CAP_HA_ACTIONS); unsigned dial, note; char tok[33] = "", tok2[33] = "";
          #define TOLD(st) ({ int ok_ = 0; for (int k_ = 0; k_ < 200 && !(ok_ = atomic_load(&h_call_state) == (st)); k_++) usleep(5000); ok_; })
          #define FREED() ({ int ok_ = 0; for (int k_ = 0; k_ < 200 && !(ok_ = call_claimed() == CALL_NOBODY); k_++) usleep(5000); ok_; })
          #define REFILL() (p->r_call = p->r_busy = (struct bucket){ 0, 0 })
          /* the ACTION's token (the last ACTION the plugin got): "" none */
          #define TOKEN_OF(out) do { uint8_t a_[PM_MAX_PACKET]; ssize_t n_; struct pm_hdr h_; const uint8_t *b_; size_t l_; (out)[0] = 0; \
              while ((n_ = recv(h_peer, a_, sizeof a_, MSG_DONTWAIT)) > 0) if (!pm_head(a_, (size_t)n_, &h_, &b_, &l_) && h_.type == PM_ACTION) { struct pm_action m_; if (!pm_get_action(b_, l_, &m_)) pm_cstr(out, 33, m_.token); } } while (0)
          char err[200]; struct ps_manifest m;
          static const char nocall[] = "{ \"id\": \"x-call\", \"name\": \"X\", \"version\": 1, \"requires\": { \"core\": 1, \"ha.actions\": 1%s }, \"author\": \"a\", \"author_key\": \"%064d\", \"digest\": \"%064d\","
                                        " \"capabilities\": { \"ha.actions\": [ { \"name\": \"dial\", \"label\": \"Dial\", \"calls\": %s } ]%s } }";
          char mt[800];
          snprintf(mt, sizeof mt, nocall, "", 0, 0, "true", "");
          CHECK(ps_manifest_parse(mt, strlen(mt), &m, err, sizeof err) && strstr(err, "needs the capability \"call\""), "an action marked calls without the capability call: the manifest is refused");
          snprintf(mt, sizeof mt, nocall, ", \"call\": 1", 0, 0, "1", ", \"call\": true");
          CHECK(ps_manifest_parse(mt, strlen(mt), &m, err, sizeof err) && strstr(err, "calls is true or false"), "calls: true or false, nothing else");
          snprintf(mt, sizeof mt, nocall, ", \"call\": 1", 0, 0, "true", ", \"call\": true");
          { char line[PS_CAP_LINE]; int r_ = ps_manifest_parse(mt, strlen(mt), &m, err, sizeof err); ps_cap_line(&m, PM_CAP_HA_ACTIONS, line, sizeof line);
            CHECK(!r_ && m.actions[0].calls && !strcmp(line, "Home Assistant can tell it to: \"Dial\" (and have it place a call)"), "with call: taken, and its install line says the action places calls");
            ps_cap_line(&m, PM_CAP_CALL, line, sizeof line);
            CHECK(!strcmp(line, "Makes and takes calls: a call coming in is answered only at the Echo, one goes out only when Home Assistant or the settings page asks. Hears the room during a call"), "call's own line at install"); }
          snprintf(mt, sizeof mt, nocall, ", \"call\": 1", 0, 0, "true", ", \"call\": 1");
          CHECK(ps_manifest_parse(mt, strlen(mt), &m, err, sizeof err) != 0, "call: true, no parameters");
          h_add_full("callplug", "\"call\": true, \"ha.actions\": [ { \"name\": \"dial\", \"label\": \"Dial\", \"calls\": true, \"args\": [ { \"name\": \"who\", \"type\": \"string\" } ] },"
                     " { \"name\": \"note\", \"label\": \"Note\" } ]", "", "the phone's program\n");
          ps_lock(); ps_scan(&l); ps_unlock();
          dial = key_of("callplug", "dial", 1); note = key_of("callplug", "note", 1);
          CHECK(dial && note, "its actions listed");
          call_start();
          p = h_plug(P_CONFIG, ccaps); h_as(p, "callplug"); snprintf(p->name, sizeof p->name, "Phone"); h_drain(NULL);
          { struct pm_call_incoming ci = { pm_s("Mum") }; SEND(pm_put_call_incoming, 1001, &ci); ANSWER(&fd); }
          CHECK(CODE() == PM_ERR_BAD && call_claimed() == CALL_NOBODY, "CALL_INCOMING before READY: BAD");
          p->state = P_RUN; p->caps = PM_CAPBIT(PM_CAP_HA_ACTIONS); REFILL();
          { struct pm_call_incoming ci = { pm_s("Mum") }; SEND(pm_put_call_incoming, 1002, &ci); ANSWER(&fd); }
          CHECK(CODE() == PM_ERR_DENIED && call_claimed() == CALL_NOBODY, "CALL_INCOMING without the capability call: DENIED");
          p->caps = ccaps;
          /* a ring, answered with the button, live, audio both ways, ended by the plugin */
          /* W6 of a review of fc43c40: the thread that tells the core could not be made (here: the first try fails);
           * the states wait, and the broker's next round makes it */
          CHECK(!teller_up, "(no call yet: no teller)");
          atomic_store(&h_thread_fail, 1);
          { struct pm_call_incoming ci = { pm_s("Mum\x01") }; SEND(pm_put_call_incoming, 1003, &ci); ANSWER(&fd); }
          usleep(50000);
          CHECK(CODE() == 0 && !teller_up && atomic_load(&h_call_state) == -1, "W6: no teller could be made: the ring's state waits untold");
          h_call_timers();
          CHECK(CODE() == 0 && call_claimed() == CALL_PLUGIN + 0 && TOLD(CALL_RINGING) && !strcmp(h_call_peer, "Phone: Mum "),
                "CALL_INCOMING: OK, the claim the plugin's, the core rings with its name beside the caller's (a control character a space)");
          { struct pm_call_incoming ci = { pm_s("Dad") }; SEND(pm_put_call_incoming, 1004, &ci); ANSWER(&fd); }
          CHECK(CODE() == PM_ERR_BUSY, "a second while it rings: BUSY");
          { struct pm_call_live cl = { pm_s("") }; SEND(pm_put_call_live, 1005, &cl); ANSWER(&fd); }
          CHECK(CODE() == PM_ERR_DENIED && fd < 0 && !call_live(), "CALL_LIVE while it rings, nobody answered: DENIED, no audio socket (3.6)");
          { struct pm_call_live cl = { pm_s("00112233445566778899aabbccddeeff") }; SEND(pm_put_call_live, 1006, &cl); ANSWER(&fd); }
          CHECK(CODE() == PM_ERR_DENIED && !call_live(), "... nor with a token it made up");
          /* C3 of a review of fc43c40: a token is for a call going out.  One of an action marked calls, fresh (Home
           * Assistant's or the page's: an automation's too), answers no call coming in */
          { char targs[1][256] = { "" }; int tints[1] = { 0 }; char ttok[33]; struct pm_call_live cl;
            h_drain(NULL); plugin_action(dial, targs, tints, 1); TOKEN_OF(ttok);
            cl = (struct pm_call_live){ pm_s(ttok) }; REFILL(); SEND(pm_put_call_live, 1009, &cl); ANSWER(&fd);
            CHECK(ttok[0] && CODE() == PM_ERR_DENIED && fd < 0 && !call_live() && pc.state == C_RINGING,
                  "... nor with a fresh token of an action marked calls: only a person at the Echo answers a call coming in"); }
          h_drain(NULL);
          CHECK(plugin_call_button() == 1 && got(PM_CALL_ANSWER) && pc.state == C_ANSWERED && TOLD(CALL_ANSWERING),
                "the action button: CALL_ANSWER to the plugin, the core told it was answered (the ringtone stops at the press)");
          REFILL();
          { struct pm_call_live cl = { pm_s("") }; SEND(pm_put_call_live, 1007, &cl); ANSWER(&fd); }
          CHECK(CODE() == 0 && fd >= 0 && call_live() && TOLD(CALL_LIVE), "answered here, CALL_LIVE: OK with the audio socket, the media begins, the core says connected");
          { int type = -1; socklen_t tl = sizeof type; getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tl); CHECK(type == SOCK_SEQPACKET, "the audio socket is a SOCK_SEQPACKET"); }
          { int16_t room[800]; struct call_stats s0, s1; uint8_t pk[MEDIA_PKT + 8]; ssize_t k; int frames = 0, sizes = 1; uint32_t seq0 = 0;
            for (int i = 0; i < 800; i++) room[i] = (int16_t)(3000 * sin(i * 0.27));
            for (int i = 0; i < 4; i++) call_mic(room, 800, 0);
            while ((k = recv(fd, pk, sizeof pk, MSG_DONTWAIT)) > 0) { if (!frames) seq0 = pk[0] | pk[1] << 8; frames++; sizes &= k == MEDIA_PKT; }
            CHECK(frames == 10 && sizes && seq0 == 0, "the room to the plugin: 200 ms of microphone, ten packets of a sequence number and 20 ms of PCM");
            call_stats(&s0);
            memset(pk, 0, sizeof pk);
            for (int i = 0; i < 5; i++) { pk[0] = (uint8_t)i; if (write(fd, pk, MEDIA_PKT) != MEDIA_PKT) break; }
            if (write(fd, pk, 100) != 100) { }                     /* not a frame: dropped */
            for (int i = 0; i < 100; i++) { call_stats(&s1); if (s1.rx - s0.rx >= 5) break; usleep(10000); }
            usleep(50000); call_stats(&s1);
            CHECK(s1.rx - s0.rx == 5, "the far side from the plugin: five frames into call.c's jitter buffer, the one of another size dropped");
            if (s1.rx - s0.rx != 5) printf("     (%u)\n", s1.rx - s0.rx); }
          /* W2 of a review of fc43c40: packets of one byte as fast as the plugin can write them, for a second.  Every
           * packet counts towards the media's hundred a second, whatever its size: hassmic's reader takes ~100, not
           * ~690 000 (a core's worth) */
          { uint8_t one = 0; long long t0, cpu; long sent = 0;
            h_cpu_mark(); t0 = now_ms();
            while (now_ms() - t0 < 1000) if (send(fd, &one, 1, MSG_DONTWAIT) == 1) sent++; else usleep(200);
            cpu = h_cpu_others();
            CHECK(cpu < 150, "a flood of one-byte packets: hassmic's threads used little CPU");
            printf("     (%ld sent, %lld ms of CPU in hassmic's threads)\n", sent, cpu); }
          { struct pm_call_end ce = { PM_RESULT_HUNG_UP }; REFILL(); SEND(pm_put_call_end, 1008, &ce); ANSWER(&fd2); }
          { uint8_t x[8]; int ended = 0; for (int i = 0; i < 100 && !ended; i++) { ended = recv(fd, x, sizeof x, MSG_DONTWAIT) == 0; if (!ended) usleep(10000); }
            CHECK(CODE() == 0 && !call_live() && TOLD(CALL_IDLE) && FREED() && ended, "CALL_END: the media over, the core idle, the claim let go, the plugin's audio socket at its end"); }
          { int16_t room[800] = { 0 }; uint8_t x[MEDIA_PKT]; call_mic(room, 800, 0); CHECK(recv(fd, x, sizeof x, MSG_DONTWAIT) <= 0, "and no more of the room after it"); }
          close(fd);
          /* a ring ended by the plugin, one ended by "stop", one not answered in time: three; the fourth refused; after
           * the window again */
          { struct pm_call_incoming ci = { pm_s("A") }; struct pm_call_end ce = { PM_RESULT_NO_ANSWER }; int why = -1;
            REFILL(); SEND(pm_put_call_incoming, 1010, &ci); ANSWER(&fd); TOLD(CALL_RINGING);
            SEND(pm_put_call_end, 1011, &ce); ANSWER(&fd); FREED();
            REFILL(); SEND(pm_put_call_incoming, 1012, &ci); ANSWER(&fd); TOLD(CALL_RINGING); h_drain(NULL);
            plugin_call_hangup(PM_HANGUP_SPOKEN);
            { uint8_t a_[64]; ssize_t n_; struct pm_hdr h_; const uint8_t *b_; size_t l_; struct pm_call_hangup hm;
              while ((n_ = recv(h_peer, a_, sizeof a_, MSG_DONTWAIT)) > 0) if (!pm_head(a_, (size_t)n_, &h_, &b_, &l_) && h_.type == PM_CALL_HANGUP && !pm_get_call_hangup(b_, l_, &hm)) why = hm.why; }
            CHECK(why == PM_HANGUP_SPOKEN && FREED(), "\"stop\" while it rings: CALL_HANGUP (spoken) to the plugin, the claim let go");
            REFILL(); SEND(pm_put_call_incoming, 1013, &ci); ANSWER(&fd); TOLD(CALL_RINGING);
            h_skew_ms += RING_MS + 500; h_call_timers();
            CHECK(pc.state == C_NONE && FREED(), "a ring nobody answers ends after its time");
            REFILL(); SEND(pm_put_call_incoming, 1014, &ci); ANSWER(&fd);
            CHECK(CODE() == PM_ERR_LIMIT && call_claimed() == CALL_NOBODY, "the fourth ring in ten minutes with three unanswered: LIMIT");
            h_skew_ms += RINGS_WINDOW; REFILL(); SEND(pm_put_call_incoming, 1015, &ci); ANSWER(&fd);
            CHECK(CODE() == 0 && TOLD(CALL_RINGING), "ten minutes on: it may ring again");
            CHECK(plugin_call_answer() == 1 && pc.state == C_ANSWERED, "the screen's Answer answers it");
            h_skew_ms += LIVE_WAIT_MS + 500; h_call_timers();
            CHECK(pc.state == C_NONE && FREED(), "answered here, and no CALL_LIVE in time: ended");
            memset(p->rings, 0, sizeof p->rings); }
          /* do not disturb, microphones off, Drop In's call: refused */
          { struct pm_call_incoming ci = { pm_s("A") };
            h_refuse_in = "do not disturb is on"; REFILL(); SEND(pm_put_call_incoming, 1020, &ci); ANSWER(&fd);
            CHECK(CODE() == PM_ERR_DENIED && call_claimed() == CALL_NOBODY, "do not disturb: CALL_INCOMING DENIED");
            h_refuse_in = NULL;
            call_claim(CALL_DROPIN); REFILL(); SEND(pm_put_call_incoming, 1021, &ci); ANSWER(&fd);
            CHECK(CODE() == PM_ERR_BUSY && call_claimed() == CALL_DROPIN, "a Drop In has the call: BUSY");
            /* W1 of a review of fc43c40: busy costs a ring: asked again and again while another call has the line, the
             * plugin is answered busy without a look, so that it cannot find the moment the line comes free */
            for (int i = 0; i < 4; i++) { SEND(pm_put_call_incoming, 1022 + (uint32_t)i, &ci); ANSWER(&fd); }
            call_release(CALL_DROPIN);
            p->r_call = (struct bucket){ 0, 0 }; SEND(pm_put_call_incoming, 1026, &ci); ANSWER(&fd);
            CHECK(CODE() == PM_ERR_BUSY && call_claimed() == CALL_NOBODY, "rings asked five times while busy: the next, the line free, BUSY all the same (BUSY_PER_SEC)");
            h_skew_ms += 1100; p->r_call = (struct bucket){ 0, 0 }; SEND(pm_put_call_incoming, 1027, &ci); ANSWER(&fd);
            CHECK(CODE() == 0 && TOLD(CALL_RINGING), "a second later: it rings");
            plugin_call_hangup(PM_HANGUP_SPOKEN); FREED(); memset(p->rings, 0, sizeof p->rings);
            for (int i = 0; i < 4; i++) { p->r_call = (struct bucket){ 0, 0 }; memset(p->rings, 0, sizeof p->rings); SEND(pm_put_call_incoming, 1028 + (uint32_t)i, &ci); ANSWER(&fd); if (CODE()) break; TOLD(CALL_RINGING); plugin_call_hangup(PM_HANGUP_SPOKEN); FREED(); }
            CHECK(CODE() == 0, "rings that found the line free cost nothing of it: four in a row");
            memset(p->rings, 0, sizeof p->rings); }
          /* going out: a token only from an action marked calls, once, for 10 s */
          { char args[1][256] = { "Bob" }; int ints[1] = { 0 }; struct pm_call_out co;
            h_drain(NULL);
            CHECK(!plugin_action(note, NULL, NULL, 0), "an action not marked calls reaches it"); TOKEN_OF(tok);
            CHECK(!tok[0], "... without a token");
            CHECK(!plugin_action(dial, args, ints, 1), "an action marked calls reaches it"); TOKEN_OF(tok);
            CHECK(strlen(tok) == 32, "... with a token of 32 hex digits");
            co = (struct pm_call_out){ pm_s(""), pm_s("Bob") }; REFILL(); SEND(pm_put_call_out, 1030, &co); ANSWER(&fd);
            CHECK(CODE() == PM_ERR_DENIED && call_claimed() == CALL_NOBODY, "CALL_OUT without a token: DENIED");
            co.token = pm_s("ffffffffffffffffffffffffffffffff"); SEND(pm_put_call_out, 1031, &co); ANSWER(&fd);
            CHECK(CODE() == PM_ERR_DENIED, "... with one made up: DENIED");
            h_refuse_out = "the microphones are off"; co.token = pm_s(tok); REFILL(); SEND(pm_put_call_out, 1032, &co); ANSWER(&fd);
            CHECK(CODE() == PM_ERR_DENIED && call_claimed() == CALL_NOBODY, "microphones off: DENIED (and the token spent)");
            h_refuse_out = NULL; SEND(pm_put_call_out, 1033, &co); ANSWER(&fd);
            CHECK(CODE() == PM_ERR_DENIED, "the same token again: DENIED (one use)");
            plugin_action(dial, args, ints, 1); TOKEN_OF(tok); h_skew_ms += TOKEN_MS + 1000;
            co.token = pm_s(tok); REFILL(); SEND(pm_put_call_out, 1034, &co); ANSWER(&fd);
            CHECK(CODE() == PM_ERR_DENIED, "a token older than 10 s: DENIED");
            { struct pm_call_live cl = { pm_s("") }; SEND(pm_put_call_live, 1035, &cl); ANSWER(&fd); CHECK(CODE() == PM_ERR_BAD && fd < 0, "CALL_LIVE with no call: BAD"); }
            SEND(pm_put_call_ringing_there, 1036); ANSWER(&fd); CHECK(CODE() == PM_ERR_BAD, "CALL_RINGING_THERE with no call: BAD");
            { struct pm_call_end ce = { 1 }; SEND(pm_put_call_end, 1037, &ce); ANSWER(&fd); CHECK(CODE() == PM_ERR_BAD, "CALL_END with no call: BAD"); }
            plugin_action(dial, args, ints, 1); TOKEN_OF(tok); plugin_action(dial, args, ints, 1); TOKEN_OF(tok2);
            co.token = pm_s(tok); REFILL(); SEND(pm_put_call_out, 1038, &co); ANSWER(&fd);
            CHECK(CODE() == 0 && TOLD(CALL_CALLING) && !strcmp(h_call_peer, "Phone: Bob") && call_claimed() == CALL_PLUGIN + 0, "CALL_OUT with a fresh token: OK, the core shows it going out");
            co.token = pm_s(tok2); SEND(pm_put_call_out, 1039, &co); ANSWER(&fd);
            CHECK(CODE() == PM_ERR_BUSY, "a second while one goes out: BUSY");
            SEND(pm_put_call_ringing_there, 1040); ANSWER(&fd); CHECK(CODE() == 0, "CALL_RINGING_THERE while it goes out: OK");
            REFILL(); { struct pm_call_live cl = { pm_s("") }; SEND(pm_put_call_live, 1041, &cl); ANSWER(&fd); }
            CHECK(CODE() == 0 && fd >= 0 && call_live() && TOLD(CALL_LIVE), "the far side took it: CALL_LIVE, the media");
            /* the plugin's process ends mid-call: the call ends cleanly with it */
            h_exited(p);
            { uint8_t x[8]; int ended = 0; for (int i = 0; i < 100 && !ended; i++) { ended = recv(fd, x, sizeof x, MSG_DONTWAIT) == 0; if (!ended) usleep(10000); }
              CHECK(!call_live() && TOLD(CALL_IDLE) && FREED() && pc.state == C_NONE && ended, "the plugin gone mid-call: the media over, the core idle, the claim let go"); }
            close(fd); }
          /* "answered" in Home Assistant: the token of an action marked calls answers no ringing call (C3 of a review of
           * fc43c40); the screen's Answer then, and CALL_LIVE (the token there not looked at) */
          { struct pm_call_incoming ci = { pm_s("C") }; char args[1][256] = { "" }; int ints[1] = { 0 };
            p = h_plug(P_RUN, ccaps); h_as(p, "callplug"); h_drain(NULL);
            REFILL(); SEND(pm_put_call_incoming, 1050, &ci); ANSWER(&fd); TOLD(CALL_RINGING);
            plugin_action(dial, args, ints, 1); TOKEN_OF(tok);
            { struct pm_call_live cl = { pm_s(tok) }; SEND(pm_put_call_live, 1051, &cl); ANSWER(&fd); }
            CHECK(CODE() == PM_ERR_DENIED && fd < 0 && !call_live() && pc.state == C_RINGING, "a ringing call with the token of an action marked calls (Home Assistant, the page): DENIED, it rings on");
            CHECK(plugin_call_answer() == 1 && TOLD(CALL_ANSWERING), "the screen's Answer");
            h_drain(NULL);                                          /* (its CALL_ANSWER: ANSWER takes a descriptor from the first answer only) */
            { struct pm_call_live cl = { pm_s(tok) }; SEND(pm_put_call_live, 1052, &cl); ANSWER(&fd); }
            CHECK(CODE() == 0 && fd >= 0 && call_live() && TOLD(CALL_LIVE), "answered here: live");
            /* stopped (an update, the switch): CALL_HANGUP before its STOP, and the call over */
            h_drain(NULL);
            h_stop(p);
            { uint8_t a_[64]; ssize_t n_; struct pm_hdr h_; const uint8_t *b_; size_t l_; int first = -1, why = -1;
              while ((n_ = recv(h_peer, a_, sizeof a_, MSG_DONTWAIT)) > 0) if (!pm_head(a_, (size_t)n_, &h_, &b_, &l_)) {
                  if (first < 0) first = h_.type;
                  if (h_.type == PM_CALL_HANGUP) { struct pm_call_hangup hm; if (!pm_get_call_hangup(b_, l_, &hm)) why = hm.why; } }
              CHECK(first == PM_CALL_HANGUP && why == PM_HANGUP_STOPPED && !call_live() && FREED(), "the plugin stopped mid-call: CALL_HANGUP (stopped) first, then STOP; the call over"); }
            close(fd); }
          /* the rate of its call messages */
          { int busy = 0; p = h_plug(P_RUN, ccaps); h_as(p, "callplug"); h_drain(NULL);
            for (int i = 0; i < 12; i++) { SEND(pm_put_call_ringing_there, 1060 + (uint32_t)i); ANSWER(&fd); busy += CODE() == PM_ERR_BUSY; }
            CHECK(busy >= 2, "call messages over five a second (twelve at once): BUSY"); }
          #undef TOLD
          #undef FREED
          #undef REFILL
          #undef TOKEN_OF
        }
        /* ---- a request's copy (request.audio, request.text; docs/plugins.md 3.5, 3.6, 4.12).  What the core
         * says (plugin_request_*), as the broker's queue hands it on (rq_drain: the broker's round) */
        { uint32_t acaps = PM_CAPBIT(PM_CAP_REQUEST_AUDIO) | PM_CAPBIT(PM_CAP_REQUEST_TEXT); int16_t blk[1024]; uint8_t pk[8 + 4096]; int afd = -1, typ;
          char err[300] = ""; struct ps_manifest m; const struct ps_plugin *l;
          /* the next message of TYPE the plugin got (others skipped), its descriptor into *FDP; 0 none */
          #define NEXT(want, fdp) ({ int t_ = 0; struct iovec iov_ = { b, sizeof b }; union { struct cmsghdr c; char x[CMSG_SPACE(sizeof(int))]; } u_; \
              struct msghdr m_ = { .msg_iov = &iov_, .msg_iovlen = 1, .msg_control = &u_, .msg_controllen = sizeof u_.x }; *(fdp) = -1; \
              while ((r = recvmsg(h_peer, &m_, MSG_DONTWAIT)) >= 2) { if (!pm_head(b, (size_t)r, &h, &body, &bl) && h.type == (want)) { \
                  for (struct cmsghdr *c_ = CMSG_FIRSTHDR(&m_); c_; c_ = CMSG_NXTHDR(&m_, c_)) if (c_->cmsg_type == SCM_RIGHTS) memcpy(fdp, CMSG_DATA(c_), sizeof(int)); \
                  t_ = 1; break; } m_.msg_controllen = sizeof u_.x; } t_; })
          for (int i = 0; i < 1024; i++) blk[i] = (int16_t)(i * 37 - 9000);
          /* the manifest: plain capabilities, true only */
          { static const char req[] = "{ \"id\": \"x-req\", \"name\": \"X\", \"version\": 1, \"requires\": { %s }, \"author\": \"a\", \"author_key\": \"%064d\", \"digest\": \"%064d\","
                                      " \"capabilities\": { %s } }";
            char mt[600], line[PS_CAP_LINE];
            snprintf(mt, sizeof mt, req, h_reqs("\"request.audio\": 1"), 0, 0, "\"request.audio\": 1");
            CHECK(ps_manifest_parse(mt, strlen(mt), &m, err, sizeof err) && strstr(err, "true, or left out"), "request.audio: true, nothing else");
            snprintf(mt, sizeof mt, req, h_reqs("\"request.text\": { }"), 0, 0, "\"request.text\": { }");
            CHECK(ps_manifest_parse(mt, strlen(mt), &m, err, sizeof err) && strstr(err, "true, or left out"), "request.text: true, nothing else");
            snprintf(mt, sizeof mt, req, h_reqs("\"request.audio\": false"), 0, 0, "\"request.audio\": false");
            CHECK(ps_manifest_parse(mt, strlen(mt), &m, err, sizeof err) && strstr(err, "leave it out"), "request.audio: false is no way to say no");
            snprintf(mt, sizeof mt, req, h_reqs("\"request.audio\": true, \"request.text\": true"), 0, 0, "\"request.audio\": true, \"request.text\": true");
            CHECK(!ps_manifest_parse(mt, strlen(mt), &m, err, sizeof err), "request.audio and request.text: taken");
            ps_cap_line(&m, PM_CAP_REQUEST_AUDIO, line, sizeof line);
            CHECK(!strcmp(line, "Hears everything said to this Echo after the wake word or the button"), "request.audio's line at install, as 3.3 words it");
            ps_cap_line(&m, PM_CAP_REQUEST_TEXT, line, sizeof line);
            CHECK(!strcmp(line, "Reads what was said to this Echo and what it answered"), "request.text's line at install");
            CHECK(!ps_second(&m, line, sizeof line) && !line[0], "no way out: no second press");
            h_add_full("x-srv", "\"request.audio\": true, \"net.server\": [ { \"setting\": \"server\", \"proto\": \"tcp\", \"ports\": [ 9000 ] } ], \"net.udp\": 1",
                       "{ \"name\": \"server\", \"label\": \"Server\", \"type\": \"host\" },", "x\n");
            ps_lock(); ps_scan(&l);
            { const struct ps_plugin *x = ps_find("x-srv");
              CHECK(x && ps_second(&x->m, line, sizeof line) && !strcmp(line, "This plugin hears every request and can send what it hears to your network and the server you enter."),
                    "request.audio with net.server and net.udp: the second press, its screen's words"); }
            ps_unlock(); }
          /* a request from the wake word: REQ_START with the socket, the blocks byte for byte with their run and offset,
           * REQ_END with the count; nothing after the end, the socket's end read */
          h_add_full("reqplug", "\"request.audio\": true, \"request.text\": true", "", "the listener's program\n");
          ps_lock(); ps_scan(&l); ps_unlock();
          p = h_plug(P_RUN, acaps); h_as(p, "reqplug"); h_drain(NULL);
          plugin_request_audio(blk, sizeof blk);                     /* (outside a run: nobody gets it) */
          plugin_request_start(PLUGIN_REQ_WAKE_WORD, 1, "Alexa");
          plugin_request_audio(blk, sizeof blk); plugin_request_audio(blk, 512 * 2);
          rq_drain();
          CHECK(NEXT(PM_REQ_START, &afd) && afd >= 0, "a request begins: REQ_START with its socket");
          { struct pm_req_start rs; char ph[65] = ""; int ok_ = !pm_get_req_start(body, bl, &rs); pm_cstr(ph, sizeof ph, rs.phrase);
            CHECK(ok_ && rs.run == req_run && rs.source == PM_REQSRC_WAKE_WORD && rs.slot == 1 && !strcmp(ph, "Alexa"), "... its number, source, slot and phrase"); }
          { ssize_t k1 = recv(afd, pk, sizeof pk, MSG_DONTWAIT), k2; uint32_t run1 = 0, off1 = 0, off2 = 0;
            for (int i = 0; i < 4; i++) { run1 |= (uint32_t)pk[i] << 8 * i; off1 |= (uint32_t)pk[4 + i] << 8 * i; }
            CHECK(k1 == 8 + 2048 && run1 == req_run && off1 == 0 && !memcmp(pk + 8, blk, 2048), "the first block as the core gave it, offset 0 (nothing from before the run)");
            k2 = recv(afd, pk, sizeof pk, MSG_DONTWAIT); for (int i = 0; i < 4; i++) off2 |= (uint32_t)pk[4 + i] << 8 * i;
            CHECK(k2 == 8 + 1024 && off2 == 1024 && !memcmp(pk + 8, blk, 1024), "the second, its offset after the first"); }
          plugin_request_end(PLUGIN_REQEND_HEARD_HERE); rq_drain();
          plugin_request_audio(blk, sizeof blk);                     /* (after the end: nobody) */
          CHECK(NEXT(PM_REQ_END, &fd), "the end: REQ_END");
          { struct pm_req_end re; CHECK(!pm_get_req_end(body, bl, &re) && re.run == req_run && re.why == PM_REQEND_HEARD_HERE && re.samples == 1536 && re.dropped == 0, "... why, and what the run had"); }
          CHECK(recv(afd, pk, sizeof pk, MSG_DONTWAIT) == 0, "nothing after the end: the socket's end");
          close(afd);
          /* texts: to request.text, with the request's number; not one with an empty text */
          plugin_request_text(PLUGIN_TEXT_HEARD, "turn on the lights"); plugin_request_text(PLUGIN_TEXT_REPLY, ""); rq_drain();
          { struct pm_req_text rt; char tx[1025] = ""; int ok_ = NEXT(PM_REQ_TEXT, &fd) && !pm_get_req_text(body, bl, &rt); pm_cstr(tx, sizeof tx, rt.text);
            CHECK(ok_ && rt.run == req_run && rt.kind == PM_REQTEXT_HEARD && !strcmp(tx, "turn on the lights"), "REQ_TEXT: what was heard, for the request's number");
            CHECK(!NEXT(PM_REQ_TEXT, &fd), "an empty text: none"); }
          { char big[3000]; memset(big, 'a', sizeof big); big[sizeof big - 1] = 0; big[1023] = (char)0xc3; big[1024] = (char)0xa9;      /* an é across the cut */
            struct pm_req_text rt; plugin_request_text(PLUGIN_TEXT_REPLY, big); rq_drain();
            CHECK(NEXT(PM_REQ_TEXT, &fd) && !pm_get_req_text(body, bl, &rt) && rt.text.n == 1023 && rt.kind == PM_REQTEXT_REPLY, "a reply over 1024 bytes: cut on a whole character"); }
          /* a plugin that does not read: blocks dropped and counted, never waited for; the offsets show the gap */
          plugin_request_start(PLUGIN_REQ_BUTTON, 0, "Alexa");
          { long long t0 = now_ms(); for (int i = 0; i < 100; i++) plugin_request_audio(blk, sizeof blk);
            CHECK(now_ms() - t0 < 1000, "a hundred blocks into a socket nobody reads: never waited for"); }
          plugin_request_end(PLUGIN_REQEND_HA); rq_drain();
          CHECK(NEXT(PM_REQ_START, &afd) && afd >= 0 && NEXT(PM_REQ_END, &fd), "a run nobody reads: start and end told");
          { struct pm_req_end re; uint32_t last = 0, got_ = 0; ssize_t k;
            CHECK(!pm_get_req_end(body, bl, &re) && re.samples == 102400 && re.dropped > 0 && re.dropped < re.samples && re.why == PM_REQEND_HA, "REQ_END counts what did not fit");
            while ((k = recv(afd, pk, sizeof pk, MSG_DONTWAIT)) > 0) { last = 0; for (int i = 0; i < 4; i++) last |= (uint32_t)pk[4 + i] << 8 * i; got_ += (uint32_t)(k - 8) / 2; }
            CHECK(got_ + re.dropped == re.samples && got_ < re.samples, "what came and what was dropped make the run"); }
          close(afd);
          /* a plugin that is not up when the run begins gets none of it; one with request.text only gets no audio */
          p->state = P_CONFIG; plugin_request_start(PLUGIN_REQ_FOLLOW_UP, 0, ""); plugin_request_audio(blk, sizeof blk); plugin_request_end(PLUGIN_REQEND_CANCELLED); rq_drain();
          CHECK(!NEXT(PM_REQ_START, &fd) && !NEXT(PM_REQ_END, &fd) && !p->rq_open, "before READY: no run of it");
          p->state = P_RUN; p->caps = PM_CAPBIT(PM_CAP_REQUEST_TEXT);
          plugin_request_start(PLUGIN_REQ_WAKE_WORD, 0, "Alexa"); plugin_request_audio(blk, sizeof blk); plugin_request_text(PLUGIN_TEXT_HEARD, "hi"); plugin_request_end(PLUGIN_REQEND_HA); rq_drain();
          typ = 0; { int t_ = NEXT(PM_REQ_TEXT, &fd); typ = t_; }
          CHECK(typ && !p->rq_open, "request.text alone: the text");
          h_drain(NULL);
          plugin_request_start(PLUGIN_REQ_WAKE_WORD, 0, "Alexa"); plugin_request_end(PLUGIN_REQEND_HA); rq_drain();
          CHECK(!NEXT(PM_REQ_START, &fd) && fd < 0, "request.text alone: no REQ_START, no audio socket");
          /* stopped mid-run: the socket's end (STOP says why), no REQ_END */
          p->caps = acaps; plugin_request_start(PLUGIN_REQ_WAKE_WORD, 0, "Alexa"); plugin_request_audio(blk, sizeof blk); rq_drain();
          CHECK(NEXT(PM_REQ_START, &afd) && afd >= 0, "(a run)");
          h_stop(p); plugin_request_audio(blk, sizeof blk); plugin_request_end(PLUGIN_REQEND_HA); rq_drain();
          CHECK(recv(afd, pk, sizeof pk, MSG_DONTWAIT) == 8 + 2048 && recv(afd, pk, sizeof pk, MSG_DONTWAIT) == 0 && !NEXT(PM_REQ_END, &fd), "stopped mid-run: what was sent, then the end; no REQ_END");
          close(afd);
          /* the plugin cannot send them: they are hassmic's (no message starts, ends or answers a request, 4.9) */
          p = h_plug(P_RUN, acaps); h_as(p, "reqplug"); h_drain(NULL);
          { struct pm_req_start rs = { 1, PM_REQSRC_BUTTON, 0, pm_s("x") }; struct pm_req_end re = { 1, 1, 0, 0 }; struct pm_req_text rt = { 1, 1, pm_s("x") };
            SEND(pm_put_req_start, 2001, &rs); ANSWER(&fd); int c1 = CODE(); SEND(pm_put_req_end, 2002, &re); ANSWER(&fd); int c2 = CODE(); SEND(pm_put_req_text, 2003, &rt); ANSWER(&fd);
            CHECK(c1 == PM_ERR_BAD && c2 == PM_ERR_BAD && CODE() == PM_ERR_BAD && !req_live, "REQ_START, REQ_END, REQ_TEXT from a plugin: BAD, nothing begins"); }
          /* 3.3's partner: a wake word engine nobody vouches for (here: from a file, unlisted) and a plugin that hears
           * every request: refused at start, at install (ps_partner), and the engine's choice (ps_engine_refused) */
          h_add("unwake", 1);
          ps_lock(); ps_scan(&l); ps_unlock();
          plugin_wake_engine("unwake");
          { char why[300] = ""; const struct ps_plugin *x; int r1;
            ps_lock(); x = ps_find("reqplug"); r1 = x ? ps_partner(&x->m, x->tier, 0, why, sizeof why) : 0; ps_unlock();
            CHECK(r1 == -1 && strstr(why, "unwake") && strstr(why, "not list as official"), "the engine a plugin from a file: a request.audio plugin may not run");
            p->state = P_DEAD; p->want = 1;
            h_runnable(99);
            start("reqplug");
            { struct plug *q = h_find("reqplug"); CHECK(q && !running(q) && q->partner && strstr(q->why, "unwake"), "at start: not started, the card says why"); }
            h_runnable(-1);
            CHECK(ps_engine_refused("unwake", why, sizeof why) == 1 && names_listener(why), "choosing it as the engine beside the plugin that hears requests: refused");
            ps_lock(); x = ps_find("unwake"); r1 = x ? ps_partner(&x->m, x->tier, 1, why, sizeof why) : 0; ps_unlock();
            CHECK(r1 == -1 && names_listener(why), "installing it, as the engine, beside the plugin that hears requests: refused");
            /* listed official it is vouched for; withdrawn by the index, also when the owner runs it anyway, by nobody
             * (C4 of a review of b294fa0: an official tier was enough).  The store's entry as a scan leaves it for such
             * a plugin: its tier, the list's reason, the owner's anyway */
            { struct ps_plugin *w; int t0;
              ps_lock(); w = (struct ps_plugin *)ps_find("unwake"); t0 = w ? w->tier : 0; if (w) w->tier = PS_OFFICIAL;
              x = ps_find("reqplug"); r1 = x ? ps_partner(&x->m, x->tier, 0, why, sizeof why) : -1; ps_unlock();
              int r2 = ps_engine_refused("unwake", why, sizeof why);
              CHECK(w && r1 == 0 && r2 == 0, "an official engine: the plugin that hears requests may run beside it, and it may be chosen");
              ps_lock(); if (w) { snprintf(w->withdrawn, sizeof w->withdrawn, "sends what it hears"); w->anyway = 1; }
              x = ps_find("reqplug"); r1 = x ? ps_partner(&x->m, x->tier, 0, why, sizeof why) : 0; ps_unlock();
              CHECK(r1 == -1 && strstr(why, "withdrew"), "the same engine withdrawn by the index and run anyway: the plugin that hears requests may not run beside it");
              CHECK(ps_engine_refused("unwake", why, sizeof why) == 1 && strstr(why, "withdrew"), "... nor may it be chosen as the engine beside that plugin");
              ps_lock(); if (w) { w->withdrawn[0] = 0; w->anyway = 0; w->tier = t0; } ps_unlock(); }
            /* ... and a listener that runs beside that engine while it is official is stopped once a new revocation
             * list withdraws the engine and the owner runs it anyway: plugin_withdrawn_changed asks the partner rule
             * again (partner_due; F1 of a review of c4ace24: nothing tested it).  As on an Echo: the engine's
             * listing signed by the index key, the list taken from the site, the owner's anyway for its serial, and
             * the store's own scan deciding tier, withdrawal and anyway */
            { static const char list[] = "{\"revoked\": 1, \"serial\": 5, \"entries\": [{\"id\": \"unwake\", \"upto\": 1, \"reason\": \"sends what it hears\"}]}";
              uint8_t seed[32] = { 7 }, isk[64], ipk[32], sig[64], msg[400]; char path[400], text[400], lst[600], dh[65], err[200] = "";
              const struct ps_plugin *w; size_t hl = strlen(PI_SIGN_REVOKED); FILE *f; int took, tier = -1, wd = 0, any = 0;
              crypto_eddsa_key_pair(isk, ipk, seed);
              snprintf(path, sizeof path, "%s/index.pub", h_dir); if ((f = fopen(path, "wb"))) { fwrite(ipk, 1, 32, f); fclose(f); }
              setenv("HASSMIC_PLUGINS_PUB", path, 1);
              ps_lock(); w = ps_find("unwake"); dh[0] = 0; if (w) for (int i = 0; i < 32; i++) sprintf(dh + 2 * i, "%02x", w->m.manifest_digest[i]); ps_unlock();
              snprintf(text, sizeof text, PI_SIGN_LISTING "id unwake\nversion 1\ntier official\nmanifest %s\nserial 0\n", dh);
              crypto_eddsa_sign(sig, isk, (const uint8_t *)text, strlen(text));
              for (int i = 0; i < 64; i++) sprintf(lst + 2 * i, "%02x", sig[i]);
              snprintf(lst + 128, sizeof lst - 128, "\n%s", text);
              snprintf(path, sizeof path, "plugins/unwake/pkg/listing.sig"); h_file(path, lst);
              ps_lock(); ps_scan(&l); w = ps_find("unwake"); tier = w ? w->tier : -1; x = ps_find("reqplug"); r1 = x ? ps_partner(&x->m, x->tier, 0, why, sizeof why) : -1; ps_unlock();
              p = h_plug(P_RUN, acaps); h_as(p, "reqplug"); h_drain(NULL);
              CHECK(tier == PS_OFFICIAL && r1 == 0, "(the engine listed official: the plugin that hears requests runs beside it)");
              memcpy(msg, PI_SIGN_REVOKED, hl); memcpy(msg + hl, list, strlen(list)); crypto_eddsa_sign(sig, isk, msg, hl + strlen(list));
              took = pi_revoked_take(list, strlen(list), sig, 64, PI_SITE, err, sizeof err);
              h_file("plugins/unwake/anyway", "5\n");
              h_partner_due(0);
              plugin_withdrawn_changed();
              ps_lock(); w = ps_find("unwake"); wd = w && w->withdrawn[0]; any = w && w->anyway; tier = w ? w->tier : -1; ps_unlock();
              CHECK(took == 1 && wd && any && tier == PS_OFFICIAL && running(p), "(a new list withdraws the engine, the owner runs it anyway: the listener still runs until the rule is asked)");
              partner_check();
              CHECK(!running(p) && p->state == P_STOPPING && !p->want && p->partner && strstr(p->why, "withdrew"),
                    "F1: a new revocation list withdrew the engine it runs beside (run anyway): the listener is stopped, the card says why");
              /* (the list and the listing gone: the engine a sideload again, as the checks below have it) */
              snprintf(path, sizeof path, "%s/plugins/.revoked", h_dir); unlink(path);
              snprintf(path, sizeof path, "%s/plugins/unwake/pkg/listing.sig", h_dir); unlink(path);
              snprintf(path, sizeof path, "%s/plugins/unwake/anyway", h_dir); unlink(path);
              pi_forget(); ps_lock(); ps_scan(&l); ps_unlock(); h_drain(NULL); } }
          plugin_wake_engine("");
          { char why[300] = ""; const struct ps_plugin *x; int r1;
            ps_lock(); x = ps_find("reqplug"); r1 = x ? ps_partner(&x->m, x->tier, 0, why, sizeof why) : -1; ps_unlock();
            CHECK(r1 == 0 && ps_engine_refused("standin", why, sizeof why) == 1, "another engine: the plugin may run; another wake plugin from a file is refused as the engine all the same"); }
          #undef NEXT
        }
        /* ---- the buttons (BUTTON, capability button) and rows of a plugin's own on its card (an entity marked
         * "page").  Which presses reach plugin_button is main.c's (button_event, where Home Assistant's "Button" is
         * told: tests/fake_plugins_button.py presses through it); here what the broker does with one */
        { const struct ps_plugin *bl_; struct plug *bp; int got[3][3] = { { 0 } }, n = 0; uint8_t pk[PM_MAX_PACKET]; ssize_t k;
          #define BUTTONS() do { memset(got, 0, sizeof got); n = 0; while ((k = recv(h_peer, pk, sizeof pk, MSG_DONTWAIT)) > 0) { struct pm_hdr hh; const uint8_t *bb; size_t ll; struct pm_button m_; \
              if (!pm_head(pk, (size_t)k, &hh, &bb, &ll) && hh.type == PM_BUTTON && hh.id == 0 && !pm_get_button(bb, ll, &m_) && m_.key >= 1 && m_.key <= 3 && m_.how >= 1 && m_.how <= 2) { got[m_.key - 1][m_.how]++; n++; } } } while (0)
          h_add_full("btnplug", "\"button\": true, \"ha.entities\": [ { \"name\": \"watts\", \"kind\": \"sensor\", \"label\": \"Power\", \"unit\": \"W\", \"decimals\": 1, \"page\": true },"
                     " { \"name\": \"mood\", \"kind\": \"text_sensor\", \"label\": \"Mood\", \"page\": true }, { \"name\": \"hidden\", \"kind\": \"switch\", \"label\": \"Hidden\" } ]", "", "buttons\n");
          ps_lock(); ps_scan(&bl_); ps_unlock();
          h_runnable(launcher >= 0 ? launcher : 0);
          bp = h_plug(P_RUN, PM_CAPBIT(PM_CAP_BUTTON) | PM_CAPBIT(PM_CAP_HA_ENTITIES)); h_as(bp, "btnplug"); h_drain(NULL);
          plugin_button(BUTTON_ACTION, 0); plugin_button(BUTTON_VOLUME_DOWN, 1); plugin_button(BUTTON_VOLUME_UP, 0); BUTTONS();
          CHECK(n == 3 && got[0][PM_BUTTON_HOW_PRESS] == 1 && got[2][PM_BUTTON_HOW_HOLD] == 1 && got[1][PM_BUTTON_HOW_PRESS] == 1,
                "button: the action button pressed, volume down held, volume up pressed: three BUTTON events, each with its key and how (id 0: nothing to answer)");
          plugin_button(3, 0); plugin_button(-1, 1); BUTTONS(); CHECK(n == 0, "button: a key there is none of: nothing");
          bp->caps = PM_CAPBIT(PM_CAP_HA_ENTITIES); plugin_button(BUTTON_ACTION, 0); BUTTONS(); CHECK(n == 0, "button: a plugin without the capability hears nothing");
          bp->caps |= PM_CAPBIT(PM_CAP_BUTTON); bp->state = P_CONFIG; plugin_button(BUTTON_ACTION, 0); BUTTONS(); CHECK(n == 0, "button: nothing before READY");
          bp->state = P_RUN; bp->r_button = (struct bucket){ 0, 0 }; bp->refused_at = 0;
          for (int i = 0; i < 14; i++) plugin_button(BUTTON_ACTION, 0);
          BUTTONS(); CHECK(n == 2 * BUTTON_PER_SEC, "button: fourteen at once: ten passed on (five a second, two seconds' worth)");
          plugin_button(BUTTON_ACTION, 0); BUTTONS(); CHECK(n == 0, "button: ... and none more at once");
          h_skew_ms += 1000; for (int i = 0; i < 9; i++) plugin_button(BUTTON_ACTION, 1);
          BUTTONS(); CHECK(n == BUTTON_PER_SEC && got[0][PM_BUTTON_HOW_HOLD] == BUTTON_PER_SEC, "button: a second later, five more");
          h_skew_ms += 2000; plugin_button(BUTTON_ACTION, 0); BUTTONS(); CHECK(n == 1, "button: and after a pause, at once again");
          /* presses not passed on are counted apart from the plugin's refusals (a review of 541f0a8): a person pressing
           * fast is not the plugin's doing, and must not fold its next refusal into "and N more" for 10 s */
          { char logq[300]; int dropped0;
            snprintf(logq, sizeof logq, "%s/buttons.log", h_dir); freopen(logq, "w", stderr); setvbuf(stderr, NULL, _IONBF, 0);
            h_skew_ms += 3000; bp->refused_at = 0; bp->refused_n = 0; bp->button_at = 0; bp->button_n = 0; dropped0 = bp->dropped;
            for (int i = 0; i < 14; i++) plugin_button(BUTTON_ACTION, 0);
            BUTTONS();
            CHECK(n == 2 * BUTTON_PER_SEC && said(logq, "btnplug: 1 press of the buttons not passed on (5 a second at most; said once in 10 s)") == 1 && bp->dropped == dropped0,
                  "button: four presses not passed on: one line, the rest counted, none of it in the plugin's dropped");
            { struct pm_entity_state a = { pm_s("nosuch"), pm_s("1") }; SEND(pm_put_entity_state, 0, &a); h_drain(NULL); }
            CHECK(said(logq, "btnplug: said what its entity cannot take (no entity ?): refused") == 1 && !said(logq, "more refused") && bp->dropped == dropped0 + 1,
                  "button: the plugin's own refusal right after: its line said at once, not folded into the presses' count; dropped counts it alone");
            h_skew_ms += REFUSAL_MS; timers();
            CHECK(said(logq, "btnplug: 3 more presses of the buttons not passed on in the last 10 s") == 1 && !said(logq, "requests refused in the last") && bp->button_n == 0,
                  "button: the presses counted since said by timers() after the window, apart from the refusals' count");
            freopen("/dev/null", "w", stderr); }
          /* rows: none before it said anything but the rows themselves; the values in the rows, not twice */
          CHECK(plug_json_has("btnplug", "\"rows\":[{\"name\":\"watts\",\"kind\":\"sensor\",\"decimals\":1,\"label\":\"Power\",\"unit\":\"W\"},{\"name\":\"mood\",\"kind\":\"text_sensor\",\"decimals\":0,\"label\":\"Mood\",\"unit\":\"\"}]"),
                "rows: the two entities marked page, by label, kind, unit and decimals, no value before the plugin said one");
          { struct pm_entity_state a = { pm_s("watts"), pm_s("12.25") }, c = { pm_s("mood"), pm_s("<b>fine</b> \"ok\"") }, d = { pm_s("hidden"), pm_s("on") };
            SEND(pm_put_entity_state, 0, &a); SEND(pm_put_entity_state, 0, &c); SEND(pm_put_entity_state, 0, &d); h_drain(NULL); }
          CHECK(plug_json_has("btnplug", "{\"name\":\"watts\",\"kind\":\"sensor\",\"decimals\":1,\"label\":\"Power\",\"unit\":\"W\",\"value\":\"12.25\"}")
                && plug_json_has("btnplug", "{\"name\":\"mood\",\"kind\":\"text_sensor\",\"decimals\":0,\"label\":\"Mood\",\"unit\":\"\",\"value\":\"<b>fine</b> \\\"ok\\\"\"}"),
                "rows: each with the value it said, as JSON text (the page shows it as text)");
          CHECK(plug_json_has("btnplug", "{\"name\":\"watts\",\"kind\":\"sensor\",\"page\":true,\"label\":\"Power\"}") && plug_json_has("btnplug", "{\"name\":\"hidden\",\"kind\":\"switch\",\"page\":false,\"label\":\"Hidden\",\"value\":\"on\"}"),
                "rows: a row's value not again among the entities; an entity without page keeps its value there");
          { struct pm_entity_state a = { pm_s("watts"), pm_s("13") }; SEND(pm_put_entity_state, 0, &a); h_drain(NULL); }
          CHECK(plug_json_has("btnplug", "\"label\":\"Power\",\"unit\":\"W\",\"value\":\"13\"}"), "rows: the value as it changes");
          bp->state = P_STOPPING;
          CHECK(plug_json_has("btnplug", "\"rows\":[]") && plug_json_has("btnplug", "{\"name\":\"watts\",\"kind\":\"sensor\",\"page\":true,\"label\":\"Power\"}"), "rows: none while it does not run (and no state among the entities either)");
          plugin_enable("btnplug", 0); bp->state = P_RUN;       /* (switched off, its process not gone yet) */
          CHECK(plug_json_has("btnplug", "\"rows\":[]") && plug_json_has("btnplug", "\"value\":\"13\"") , "rows: none while Home Assistant does not list it (switched off): its last value only among the entities");
          plugin_enable("btnplug", 1); bp->state = P_RUN;
          CHECK(plug_json_has("btnplug", "\"rows\":[{\"name\":\"watts\""), "rows: back once it is listed and runs");
          /* a row is a state Home Assistant has (a review of 541f0a8): an entity marked page whose key could not be
           * written down is not in Home Assistant, so it has no row; the entities carry its "nokey" and its value */
          setenv("HASSMIC_PLUGIN_FAIL_HAKEYS", "1", 1); ps_hakeys_forget();
          h_add_full("rowkeyless", "\"ha.entities\": [ { \"name\": \"temp\", \"kind\": \"sensor\", \"label\": \"Temp\", \"page\": true } ]", "", "rk\n");
          ps_lock(); ps_scan(&bl_); ps_unlock();
          bp = h_plug(P_RUN, PM_CAPBIT(PM_CAP_HA_ENTITIES)); h_as(bp, "rowkeyless"); h_drain(NULL);
          { struct pm_entity_state a = { pm_s("temp"), pm_s("21") }; SEND(pm_put_entity_state, 0, &a); h_drain(NULL); }
          CHECK(!key_of("rowkeyless", "temp", 0) && plug_json_has("rowkeyless", "\"rows\":[]")
                && plug_json_has("rowkeyless", "{\"name\":\"temp\",\"kind\":\"sensor\",\"page\":true,\"label\":\"Temp\",\"unlisted\":\"nokey\",\"value\":\"21\"}"),
                "rows: an entity marked page whose key could not be written down: no row (Home Assistant has no state of it), its nokey and value among the entities");
          unsetenv("HASSMIC_PLUGIN_FAIL_HAKEYS"); ps_hakeys_forget(); ps_lock(); ps_scan(&bl_); ps_unlock();
          CHECK(key_of("rowkeyless", "temp", 0) && plug_json_has("rowkeyless", "\"rows\":[{\"name\":\"temp\",\"kind\":\"sensor\",\"decimals\":0,\"label\":\"Temp\",\"unit\":\"\",\"value\":\"21\"}]")
                && plug_json_has("rowkeyless", "{\"name\":\"temp\",\"kind\":\"sensor\",\"page\":true,\"label\":\"Temp\"}"),
                "rows: once its key is written down: the row, and neither nokey nor the value again among the entities");
          { char d[300]; snprintf(d, sizeof d, "%s/plugins/btnplug", h_dir); nftw(d, h_rm, 16, FTW_DEPTH | FTW_PHYS);
            snprintf(d, sizeof d, "%s/plugins/rowkeyless", h_dir); nftw(d, h_rm, 16, FTW_DEPTH | FTW_PHYS); }
          ps_lock(); ps_scan(&bl_); ps_unlock();
          for (int i = 1; i < NPLUG; i++) if (plugs[i].state != P_FREE && (!strcmp(plugs[i].id, "btnplug") || !strcmp(plugs[i].id, "rowkeyless"))) { plugs[i].state = P_FREE; plugs[i].slot = -1; }
          #undef BUTTONS
        }
        feature = 0;
        h_plug(P_DEAD, 0);
        #undef ANSWER
        #undef CODE
      }
      #undef SEND
      #undef ERRCODE
    }

    printf("plugin_broker: %s\n", fails ? "FAILED" : "all passed");
    return fails != 0;
}
