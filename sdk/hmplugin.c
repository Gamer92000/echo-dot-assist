/* hassmic_plugin.h: the reference client.  One thread, one loop over the control socket (descriptor 3) and, for a
 * wake plugin, the microphone stream.  A request that waits for its answer (hm_file_open, hm_store_get, hm_connect)
 * answers PING meanwhile and keeps what else arrives for afterwards, in order: the loop hands on what it kept before
 * anything newer.  A STOP that comes while a request waits ends the wait (hassmic answers only storage writes after
 * STOP), and the stop callback runs as soon as the callback that asked returns.  MIT. */
#define _GNU_SOURCE
#include "hassmic_plugin.h"
#include "plugmsg.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#define CTL 3
#define SDK_VERSION 14                  /* (14: a request not of what is spoken refused HM_ERR_NOT_SPOKEN, not HM_ERR_BAD; 13: a value whose fallback is none not sent, one buffer one send; 12: a value of a later version than spoken sent as its fallback, answers held to what is spoken; 11: the greeting without a set of capability bits, enum values held to the versions spoken) */
/* What arrives while a request waits for its answer is kept for the loop, in order: as much as this, and then the
 * plugin ends and says why (dropped silently, it lost lines of the log and moved its cursor past them, a CONFIG or a
 * STOP; a review, 2026-11-21).  hassmic sends the next LOG only once the client answered the last one (OK, after the
 * log callback returned: sdk/PROTOCOL.md "The log"), so what comes meanwhile is a few messages: PING (answered at
 * once, never kept), a LOG at most, a CONFIG, a check */
#define KEPT_BYTES (2 << 20)
#define KEPT_MAX 1024
#define PENDING 8                       /* storage writes the Echo said BUSY to, kept for later: the latest per key */
#define RETRY_MS 10000                  /* asked again this often: the Echo's bucket takes a write a minute again, and a
                                         * refusal costs it nothing but the message */

static struct hm_info info;
/* the version of each scope spoken with hassmic (WELCOME's: the manifest's requires), 0 none: what is read and sent
 * is held to it (pm_check), so that a plugin never sees what it did not ask for */
static uint16_t ver[PM_SCOPES];
static const struct hm_callbacks *cb;
static void *ctx;
static int stream = -1, stopping, ready_said, running;      /* running: READY went out */
/* a STOP came while a request waited (kept for the loop): from here hassmic answers nothing but storage writes, so a
 * request other than those fails at once, unasked */
static int stop_heard;
static uint32_t next_id;
/* old: a LOG of the place followed before, kept aside while hm_log_follow waits; dropped when hassmic moved the
 * place, handed on as any when it refused to */
static struct { uint8_t *data; size_t len; int fd, old; } kept[KEPT_MAX];
static int nkept; static size_t kept_bytes;
static struct { char key[49]; uint8_t *v; size_t n; } pend[PENDING];
static int npend;
static long long pend_at, tick_at;
static int music_fd = -1;               /* the stream hm_music_open gave */
static int music_playing;               /* the last hm_music_state said PLAYING: the loop polls the stream for writing
                                         * only then (a writable stream that a paused player does not write to would
                                         * have the loop spin) */
static int call_fd = -1;                /* the call's audio socket while it is live */
static int req_fd = -1;                 /* the request's audio socket, one per request: until its end is read */
static const char *action_token = "";   /* the token of the action being handled */
#define WATCH_MAX 8
static struct { int fd; void (*readable)(void *ctx, int fd); } watch[WATCH_MAX];
static int nwatch;

static long long mono_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

/* The development flag (the plugin's card on the settings page): a system call the sandbox refuses raises SIGSYS
 * instead of answering EPERM.  Which call it was goes into the log, the one thing a handler can do (write), then the
 * signal again with its default: the plugin ends, and its card says the sandbox stopped it */
static void on_sigsys(int sig, siginfo_t *si, void *uc)
{
    char b[96] = "the sandbox refused system call "; size_t n = strlen(b); int nr = si->si_syscall, d = 1;
    struct sigaction dfl; (void)uc;
    if (nr < 0) nr = 0;
    while (nr / d >= 10) d *= 10;
    for (; d; d /= 10) b[n++] = (char)('0' + nr / d % 10);
    { static const char tail[] = " (sdk/filter.def has what may be called)\n"; memcpy(b + n, tail, sizeof tail - 1); n += sizeof tail - 1; }
    if (write(2, b, n) < 0) { }
    memset(&dfl, 0, sizeof dfl); dfl.sa_handler = SIG_DFL;
    sigaction(sig, &dfl, NULL);
    syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), sig);      /* (blocked in here: it comes as this returns) */
}

const struct hm_info *hm_info(void) { return &info; }
int hm_scope(const char *name) { int s = name ? pm_scope_of(name, strlen(name)) : -1; return s < 0 ? 0 : ver[s]; }
/* granted: the capability's scope is spoken (WELCOME names core and the scope of each capability the owner accepted) */
int hm_granted(int cap) { for (int s = 1; s < PM_SCOPES; s++) if (pm_scope_cap(s) == cap) return ver[s] != 0; return 0; }

void hm_log(const char *fmt, ...)
{
    char line[400]; va_list ap; int n;
    va_start(ap, fmt); n = vsnprintf(line, sizeof line - 1, fmt, ap); va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
    line[n++] = '\n';
    if (write(2, line, (size_t)n) < 0) { }
}

int hm_random(void *out, size_t n)
{
    uint8_t *o = out;
    while (n) { long k = syscall(SYS_getrandom, o, n > 16 ? 16 : n, 0); if (k <= 0) return -1; o += k; n -= (size_t)k; }
    return 0;
}

/* Only what hassmic speaks with this plugin goes out (WELCOME's versions; before WELCOME the greeting, by this client's
 * own): no message or field of a later version than spoken, and no value an enum has in no version (a plugin's
 * hm_music_state(9, ...)): refused here, -1, as hassmic would refuse it (sdk/PROTOCOL.md "Versions").  A value of a
 * later version of its scope than spoken goes as its fallback, the value of the version spoken that messages.def
 * names for it, as hassmic sends them; where it names none (`>-`: no value of the version spoken is right) the
 * packet is not sent, -1 without a word, as hassmic drops such a one (pm_check would refuse it as well: said here,
 * not left to it).  That is only ever a message marked `drop` (pm_droppable), none of which a plugin sends: a
 * plugin's every value of a later version goes as its fallback.  pm_fit rewrites W in place for the one send here
 * (every caller encodes its packet for it) and marks it: a buffer that went once never goes again.  0 sent; -1 not
 * sent, and put_why says why for a request's *why: HM_ERR_NOT_SPOKEN where the message, or a value of it, is not of
 * what is spoken with hassmic (a request of a scope its manifest does not require: hassmic would answer it
 * ERR_UNKNOWN, and nothing is wrong with the request itself), HM_ERR_BAD for anything else */
static int put_why;
static int put(struct pm_w *w)
{
    long n = pm_wlen(w); struct pm_hdr h; const uint8_t *body; size_t bl; const uint16_t *v = ver[PM_SCOPE_CORE] ? ver : NULL; int c;
    put_why = HM_ERR_BAD;
    if (n <= 0 || w->fitted || pm_head(w->b, (size_t)n, &h, &body, &bl)) return -1;
    w->fitted = 1;
    if (pm_fit(h.type, w->b + PM_HEADER, bl, v) == 1) { put_why = HM_ERR_NOT_SPOKEN; return -1; }
    if ((c = pm_check(h.type, body, bl, v))) { if (c < 0) put_why = HM_ERR_NOT_SPOKEN; return -1; }
    return send(CTL, w->b, (size_t)n, MSG_NOSIGNAL) == n ? 0 : -1;
}

/* one packet of the control socket and the descriptor that came with it (-1 none): its length, 0 at the end */
static long get(uint8_t *buf, size_t cap, int *fd)
{
    union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int))]; } c;
    struct iovec iov = { buf, cap }; struct msghdr h = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = &c, .msg_controllen = sizeof c.b };
    long n;
    *fd = -1;
    while ((n = recvmsg(CTL, &h, 0)) < 0 && errno == EINTR) ;
    if (n <= 0) return 0;
    for (struct cmsghdr *m = CMSG_FIRSTHDR(&h); m; m = CMSG_NXTHDR(&h, m))
        if (m->cmsg_level == SOL_SOCKET && m->cmsg_type == SCM_RIGHTS && m->cmsg_len >= CMSG_LEN(sizeof(int))) memcpy(fd, CMSG_DATA(m), sizeof(int));
    return n;
}

static uint32_t new_id(void) { if (!++next_id) next_id = 1; return next_id; }

/* strings of a message as C strings, in one block the caller frees after the callback */
struct pool { char *b; size_t n, cap; };
static const char *keep(struct pool *p, struct pm_str s)
{
    char *o;
    if (p->n + s.n + 1 > p->cap) return "";
    o = p->b + p->n; memcpy(o, s.s, s.n); o[s.n] = 0; p->n += (size_t)s.n + 1;
    return o;
}

/* One packet of the request's socket to request_audio: a u32 run, a u32 offset, then the block.  1 read one (or one
 * that was no block), 0 nothing there now or its end (closed then) */
static int req_read(int wait)
{
    static union { uint8_t b[8 + PM_REQ_SAMPLES_MAX * 2 + 4]; int16_t align; } a;
    static int16_t pcm[PM_REQ_SAMPLES_MAX];
    long k = recv(req_fd, a.b, sizeof a.b, wait ? 0 : MSG_DONTWAIT);
    if (k >= 8 && !((k - 8) & 1) && k - 8 <= PM_REQ_SAMPLES_MAX * 2) {
        uint32_t run = 0, off = 0;
        for (int i = 0; i < 4; i++) { run |= (uint32_t)a.b[i] << (8 * i); off |= (uint32_t)a.b[4 + i] << (8 * i); }
        memcpy(pcm, a.b + 8, (size_t)(k - 8));
        if (cb->request_audio) cb->request_audio(ctx, run, off, pcm, (int)((k - 8) / 2));
        return 1;
    }
    if (k > 0) return 1;
    if (k == 0 || (errno != EAGAIN && errno != EINTR)) { close(req_fd); req_fd = -1; }
    return 0;
}

static void handle(const uint8_t *pkt, size_t len, int fd)
{
    /* out: what is answered, as large as any packet may be.  It was 1024 bytes, and FILE_OK at its bound (a title and
     * 8 fields of hm_check's longest) is 2412: put() refused it, nothing went back, and the owner's upload waited for
     * its 5 s and was refused without a word of why (a review of 6631974, 5) */
    struct pm_hdr h; const uint8_t *body; size_t blen; static uint8_t out[PM_MAX_PACKET]; struct pm_w w; int unread;
    struct pool pool = { NULL, 0, len + 4096 };
    /* (malformed, or of nothing spoken with this plugin: neither is acted on.  A type it does not know, or one of a
     * scope or a version not spoken, with an id: ERR_UNKNOWN, as sdk/PROTOCOL.md says and the Rust client does; it
     * was dropped here, and hassmic's request waited for its time (a review of 6631974, 9).  One malformed is not
     * answered: what it was asked cannot be read) */
    if (pm_head(pkt, len, &h, &body, &blen)) { if (fd >= 0) close(fd); return; }
    if ((unread = pm_check(h.type, body, blen, ver))) {
        if (unread < 0 && h.id && h.type != PM_ERR && h.type != PM_OK) { struct pm_err e = { PM_ERR_UNKNOWN }; pm_winit(&w, out, sizeof out); pm_put_err(&w, h.id, &e); put(&w); }
        if (fd >= 0) close(fd);
        return;
    }
    pm_winit(&w, out, sizeof out);
    if (!(pool.b = malloc(pool.cap))) { if (fd >= 0) close(fd); return; }
    switch (h.type) {
    case PM_CONFIG: {
        struct pm_config m; struct pm_config_settings e; struct hm_setting s[32]; int n = 0;
        pm_get_config(body, blen, &m);
        while (pm_next_config_settings(&m.settings, &e) && n < 32) { s[n].name = keep(&pool, e.name); s[n].value = keep(&pool, e.value); n++; }
        if (cb->config) cb->config(ctx, s, n);
        if (!ready_said) {                      /* the first one closes the start: the files came ahead of it */
            ready_said = 1;
            if (cb->ready) cb->ready(ctx);
            pm_put_ready(&w, 0); put(&w); running = 1;
        }
        break; }
    case PM_FILES: {
        struct pm_files m; struct pm_files_items it; struct hm_item *items; struct hm_field *fields; struct hm_file *files; int n = 0, nf = 0, nl = 0;
        pm_get_files(body, blen, &m);
        items = calloc(64, sizeof *items); fields = calloc(64 * 16, sizeof *fields); files = calloc(64 * 8, sizeof *files);
        if (items && fields && files) {
            const char *list = keep(&pool, m.list);
            while (pm_next_files_items(&m.items, &it) && n < 64) {
                struct pm_files_items_fields f; struct pm_files_items_files fl;
                items[n].id = keep(&pool, it.id); items[n].title = keep(&pool, it.title); items[n].checked = it.checked;
                items[n].fields = fields + nf; items[n].files = files + nl;
                while (pm_next_files_items_fields(&it.fields, &f)) { fields[nf].name = keep(&pool, f.name); fields[nf].value = keep(&pool, f.value); nf++; items[n].nfields++; }
                while (pm_next_files_items_files(&it.files, &fl)) { files[nl].suffix = keep(&pool, fl.name); files[nl].size = fl.size; nl++; items[n].nfiles++; }
                n++;
            }
            if (cb->files) cb->files(ctx, list, items, n);
        }
        free(items); free(fields); free(files);
        break; }
    case PM_FILE_CHECK: {
        struct pm_file_check m; struct hm_check *c = calloc(1, sizeof *c); int r = HM_FILE_BAD_OTHER;
        pm_get_file_check(body, blen, &m);
        if (c && cb->file_check) r = cb->file_check(ctx, keep(&pool, m.list), keep(&pool, m.item), c);
        if (c && !r) {
            struct pm_file_ok ok = { pm_s(c->title), { { 0 }, (uint16_t)(c->nfields < 0 ? 0 : c->nfields > 8 ? 8 : c->nfields) } };
            pm_put_file_ok(&w, h.id, &ok);
            for (int i = 0; i < ok.fields.n; i++) { struct pm_file_ok_fields e = { pm_s(c->fields[i].name), pm_s(c->fields[i].value) }; pm_add_file_ok_fields(&w, &e); }
            /* what the callback filled that the protocol does not take (no text of UTF-8, a field's name that is no
             * setting's): said here and to the owner, never left unanswered */
            if (put(&w)) {
                struct pm_file_bad bad = { HM_FILE_BAD_OTHER, pm_s("the plugin's answer about this file could not be sent (its title or a field is not what hassmic takes)") };
                hm_log("file_check of %s/%s: its answer (FILE_OK) is not one hassmic takes: answered FILE_BAD", keep(&pool, m.list), keep(&pool, m.item));
                pm_winit(&w, out, sizeof out); pm_put_file_bad(&w, h.id, &bad); put(&w);
            }
        } else { struct pm_file_bad bad = { (uint16_t)r, pm_s(c ? c->why : "") }; pm_put_file_bad(&w, h.id, &bad); put(&w); }
        free(c);
        break; }
    case PM_PING: pm_put_pong(&w, h.id); put(&w); break;
    case PM_LOG: {
        struct pm_log m; struct pm_log_lines e; const char **l = calloc(64, sizeof *l); int n = 0;
        pm_get_log(body, blen, &m);
        if (l) {
            const char *cur = keep(&pool, m.cursor);
            while (pm_next_log_lines(&m.lines, &e) && n < 64) l[n++] = keep(&pool, e.line);
            if (cb->log) cb->log(ctx, cur, m.gap, l, n);
            free(l);
        }
        /* handled: hassmic sends the next one after this OK (flow control: what a slow callback leaves waits on the
         * Echo, where the log keeps it, not here, where it filled the client's room and ended the plugin; R9 of a
         * review, 2026-11-21) */
        if (h.id) { pm_put_ok(&w, h.id); put(&w); }
        break; }
    case PM_STOP: stopping = stop_heard = 1; running = 0; if (cb->stop) cb->stop(ctx); break;
    case PM_TIME: {
        struct pm_time m; pm_get_time(body, blen, &m);
        info.clock_set = (int)m.clock_set; pm_cstr(info.zone, sizeof info.zone, m.zone);
        if (cb->time) cb->time(ctx);
        break; }
    case PM_WAKE_OPEN: {
        struct pm_wake_open m; int r = HM_WAKE_FAIL_OTHER;
        pm_get_wake_open(body, blen, &m);
        if (fd >= 0) { if (stream >= 0) close(stream); stream = fd; fd = -1; }      /* the stream comes with the first one */
        if (cb->wake_open) r = cb->wake_open(ctx, m.slot, keep(&pool, m.word));
        if (!r) { struct pm_wake_ready ok = { m.slot }; pm_put_wake_ready(&w, 0, &ok); }
        else { struct pm_wake_failed bad = { m.slot, (uint16_t)r }; pm_put_wake_failed(&w, 0, &bad); }
        put(&w);
        break; }
    case PM_WAKE_CLOSE: { struct pm_wake_close m; pm_get_wake_close(body, blen, &m); if (cb->wake_close) cb->wake_close(ctx, m.slot); break; }
    case PM_WAKE_RESET: if (cb->wake_reset) cb->wake_reset(ctx); break;
    case PM_MUSIC_POS: { struct pm_music_pos m; pm_get_music_pos(body, blen, &m); if (cb->music_pos) cb->music_pos(ctx, m.frames, m.at, m.queued); break; }
    case PM_MUSIC_CMD: { struct pm_music_cmd m; pm_get_music_cmd(body, blen, &m); if (cb->music_cmd) cb->music_cmd(ctx, m.cmd, m.why); break; }
    case PM_VOLUME: { struct pm_volume m; pm_get_volume(body, blen, &m); if (cb->volume) cb->volume(ctx, m.percent); break; }
    case PM_STATE: { struct pm_state m; pm_get_state(body, blen, &m); if (cb->state) cb->state(ctx, m.state, m.muted, m.volume); break; }
    case PM_NET: { struct pm_net m; pm_get_net(body, blen, &m); if (cb->net) cb->net(ctx, m.address, m.mask, m.up); break; }
    case PM_ENTITY_SET: { struct pm_entity_set m; pm_get_entity_set(body, blen, &m); if (cb->entity_set) cb->entity_set(ctx, keep(&pool, m.name), keep(&pool, m.value)); break; }
    case PM_ACTION: {
        struct pm_action m; struct pm_action_args e; struct hm_arg a[4]; int n = 0; const char *name;
        pm_get_action(body, blen, &m); name = keep(&pool, m.name);
        while (pm_next_action_args(&m.args, &e) && n < 4) { a[n].name = keep(&pool, e.name); a[n].value = keep(&pool, e.value); n++; }
        action_token = keep(&pool, m.token);
        if (cb->action) cb->action(ctx, name, m.from, a, n);
        action_token = "";
        break; }
    case PM_CALL_ANSWER: if (cb->call_answer) cb->call_answer(ctx); break;
    case PM_CALL_HANGUP: {
        struct pm_call_hangup m; pm_get_call_hangup(body, blen, &m);
        if (call_fd >= 0) { close(call_fd); call_fd = -1; }       /* (hassmic ended its end already) */
        if (cb->call_hangup) cb->call_hangup(ctx, m.why);
        break; }
    case PM_REQ_START: {
        struct pm_req_start m; pm_get_req_start(body, blen, &m);
        if (fd >= 0) { if (req_fd >= 0) close(req_fd); req_fd = fd; fd = -1; }       /* (each request's audio comes on a socket of its own) */
        if (cb->request_start) cb->request_start(ctx, m.run, m.source, m.slot, keep(&pool, m.phrase));
        break; }
    case PM_REQ_END: {
        /* everything of the request is in its socket by now (hassmic sent it before it said the end): handed on first */
        struct pm_req_end m; pm_get_req_end(body, blen, &m);
        while (req_fd >= 0 && req_read(0)) ;
        if (req_fd >= 0) { close(req_fd); req_fd = -1; }
        if (cb->request_end) cb->request_end(ctx, m.run, m.why, m.samples, m.dropped);
        break; }
    case PM_REQ_TEXT: { struct pm_req_text m; pm_get_req_text(body, blen, &m); if (cb->request_text) cb->request_text(ctx, m.run, m.kind, keep(&pool, m.text)); break; }
    case PM_BUTTON: { struct pm_button m; pm_get_button(body, blen, &m); if (cb->button) cb->button(ctx, m.key, m.how); break; }
    case PM_OK: case PM_ERR: break;             /* an answer nobody waits for any more */
    default:                                    /* a type this client does not read: said so, never a reason to end */
        if (h.id) { struct pm_err e = { PM_ERR_UNKNOWN }; pm_put_err(&w, h.id, &e); put(&w); }
    }
    if (fd >= 0) close(fd);
    free(pool.b);
}

/* waits for the answer to request ID; a PING is answered meanwhile (a connect may take 10 s), what else comes is kept
 * for the loop.  The answer's type (PM_OK, PM_ERR, or the request's own answer, PM_STORE_VALUE), 0 when hassmic went
 * away or (WRITE 0) said STOP meanwhile; *code an ERR's code, *body and *blen the answer's fields (good until the
 * next answer).  WRITE: a storage write, which hassmic answers after STOP too (a forwarder keeps its cursor as it
 * ends); anything else it leaves unanswered once the plugin is stopping, and the wait held the plugin until it was
 * killed, its stop callback never run (R2 of a review, 2026-11-21) */
static int follow_wait;                 /* hm_log_follow waits: LOG that comes meanwhile is the old place's */
static void keep_it(const uint8_t *b, size_t n, int fd, int old)
{
    if (nkept == KEPT_MAX || kept_bytes + n > KEPT_BYTES || !(kept[nkept].data = malloc(n))) {
        static const char why[] = "the plugin's client cannot keep what hassmic sends while a request waits: it ends (sdk/hmplugin.c KEPT_BYTES)\n";
        if (write(2, why, sizeof why - 1) < 0) { }
        _exit(70);                              /* (counted as a crash, and started again: nothing of it is lost unsaid) */
    }
    memcpy(kept[nkept].data, b, n); kept[nkept].len = n; kept[nkept].fd = fd; kept[nkept++].old = old; kept_bytes += n;
}
static int answer(uint32_t id, int write, int *fd, int *code, const uint8_t **ans, size_t *alen)
{
    static uint8_t buf[PM_MAX_PACKET];
    for (;;) {
        struct pm_hdr h; const uint8_t *body; size_t blen; int got; long n = get(buf, sizeof buf, &got);
        if (n <= 0) return 0;
        /* (an answer is what hassmic speaks with this plugin, as everything handle() takes: ERR with a code of a
         * later version than spoken, an OK with a body, a STORE_VALUE or UDP_OPENED that does not read is no answer,
         * and is dropped as handle() would drop it; finding 6 of a review of 60a8589: answer() took them unchecked) */
        if (pm_head(buf, (size_t)n, &h, &body, &blen) || pm_check(h.type, body, blen, ver)) { if (got >= 0) close(got); continue; }
        if (h.id == id && (h.type == PM_OK || h.type == PM_ERR || h.type == PM_STORE_VALUE || h.type == PM_UDP_OPENED)) {
            *fd = got;
            if (code) { struct pm_err e = { 0 }; *code = h.type == PM_ERR && !pm_get_err(body, blen, &e) ? e.code : 0; }
            if (ans) { *ans = body; *alen = blen; }
            return h.type;
        }
        if (h.type == PM_PING) {
            uint8_t o[16]; struct pm_w w; pm_winit(&w, o, sizeof o); pm_put_pong(&w, h.id); put(&w);
            if (got >= 0) close(got);
            continue;
        }
        keep_it(buf, (size_t)n, got, follow_wait && h.type == PM_LOG);
        if (h.type == PM_STOP) { stop_heard = 1; if (!write) return 0; }
    }
}
/* a request other than a storage write once STOP came: hassmic would not answer it */
static int stopped_now(int *why) { if (stop_heard) { if (why) *why = HM_ERR_FAILED; return 1; } return 0; }

int hm_file_open(const char *list, const char *item, const char *suffix)
{
    uint8_t out[256]; struct pm_w w; struct pm_file_open m = { pm_s(list), pm_s(item), pm_s(suffix) }; uint32_t id = new_id(); int fd = -1;
    if (stopped_now(NULL)) return -1;
    pm_winit(&w, out, sizeof out); pm_put_file_open(&w, id, &m);
    if (put(&w) || answer(id, 0, &fd, NULL, NULL, NULL) != PM_OK) { if (fd >= 0) close(fd); return -1; }
    return fd;
}

/* ---- storage */

static int store_put_now(const char *key, const void *data, size_t len, int *why)
{
    static uint8_t out[PM_MAX_PACKET]; struct pm_w w; struct pm_store_put m = { pm_s(key), pm_b(data, len) }; uint32_t id = new_id(); int fd = -1, code = 0, r;
    if (why) *why = 0;
    pm_winit(&w, out, sizeof out); pm_put_store_put(&w, id, &m);
    if (put(&w)) { if (why) *why = put_why; return -1; }
    r = answer(id, 1, &fd, &code, NULL, NULL);
    if (fd >= 0) close(fd);
    if (why) *why = r ? code : HM_ERR_FAILED;
    return r == PM_OK ? 0 : r == PM_ERR && code == PM_ERR_BUSY ? 1 : -1;
}

/* 0 kept (the latest of its key); -1 not: no room for another key (PENDING), or no memory.  The caller says so: a
 * value it was told is kept and is not was a lie (a review, 2026-11-21) */
static int pend_keep(const char *key, const void *data, size_t len)
{
    int i; uint8_t *v;
    for (i = 0; i < npend && strcmp(pend[i].key, key); i++) ;
    if (i == npend && npend == PENDING) return -1;
    if (!(v = malloc(len ? len : 1))) return -1;
    memcpy(v, data, len);
    if (i == npend) snprintf(pend[npend++].key, sizeof pend[0].key, "%s", key);
    else free(pend[i].v);
    pend[i].v = v; pend[i].n = len;
    if (!pend_at) pend_at = mono_ms() + RETRY_MS;
    return 0;
}

/* the loop: the writes kept, one by one, when the Echo takes them again.  One the Echo refuses for good now (over the
 * kB, the key, gone) is dropped and said in the plugin's log: it was told "kept", and the store has not got it */
static void pend_try(void)
{
    if (!npend || mono_ms() < pend_at) return;
    while (npend) {
        int why = 0, r = store_put_now(pend[0].key, pend[0].v, pend[0].n, &why);
        if (r == 1) { pend_at = mono_ms() + RETRY_MS; return; }
        if (r < 0) hm_log("storage: the value of \"%s\" kept to be written later was refused (error %d): not stored", pend[0].key, why);
        free(pend[0].v);
        memmove(pend, pend + 1, sizeof pend[0] * (size_t)--npend);
    }
    pend_at = 0;
}

static void pend_drop(int i) { free(pend[i].v); memmove(pend + i, pend + i + 1, sizeof pend[0] * (size_t)(npend - i - 1)); npend--; }

/* hm_run ends: what is kept for later is tried once more (hassmic answers storage writes until the plugin is gone:
 * exited or killed), and what still does not go is said: told "kept, written later", it would have been lost without a word
 * (R4 of a review, 2026-11-21) */
static void pend_flush(void)
{
    while (npend) {
        int why = 0, r = store_put_now(pend[0].key, pend[0].v, pend[0].n, &why);
        if (r) hm_log("storage: the value of \"%s\" kept to be written later is not stored: the plugin ends (error %d)", pend[0].key, why);
        pend_drop(0);
    }
    pend_at = 0;
}

int hm_store_put(const char *key, const void *data, size_t len, int *why)
{
    int r;
    if (len > 16384) { if (why) *why = HM_ERR_LIMIT; return -1; }
    /* Stopping: now or not at all.  Nothing is "written later" by a client whose loop ends after this callback; a
     * value kept for the key before is this one's elder, and goes */
    if (stop_heard) {
        for (int i = 0; i < npend; i++) if (!strcmp(pend[i].key, key)) { pend_drop(i); break; }
        r = store_put_now(key, data, len, why);
        return r == 1 ? -1 : r;                 /* (BUSY: *why says it) */
    }
    for (int i = 0; i < npend; i++) if (!strcmp(pend[i].key, key)) {             /* (in order: after the one kept) */
        if (pend_keep(key, data, len)) { if (why) *why = HM_ERR_BUSY; return -1; }
        if (why) *why = HM_ERR_BUSY;
        return 1;
    }
    r = store_put_now(key, data, len, why);
    if (r == 1 && pend_keep(key, data, len)) { if (why) *why = HM_ERR_BUSY; return -1; }      /* BUSY, and not kept either */
    return r;
}

int hm_store_get(const char *key, void *buf, size_t cap, size_t *len)
{
    uint8_t out[128]; struct pm_w w; struct pm_store_get m = { pm_s(key) }; uint32_t id = new_id(); int fd = -1, code = 0, r; const uint8_t *b; size_t bl;
    *len = 0;
    for (int i = 0; i < npend; i++) if (!strcmp(pend[i].key, key)) {            /* what it wrote last, though not yet on the Echo */
        memcpy(buf, pend[i].v, pend[i].n < cap ? pend[i].n : cap); *len = pend[i].n; return 0;
    }
    if (stopped_now(NULL)) return -1;
    pm_winit(&w, out, sizeof out); pm_put_store_get(&w, id, &m);
    if (put(&w)) return -1;
    r = answer(id, 0, &fd, &code, &b, &bl);
    if (fd >= 0) close(fd);
    if (r == PM_STORE_VALUE) {
        struct pm_store_value v;
        if (pm_get_store_value(b, bl, &v)) return -1;
        memcpy(buf, v.value.s, v.value.n < cap ? v.value.n : cap); *len = v.value.n;
        return 0;
    }
    return r == PM_ERR && code == PM_ERR_NOT_FOUND ? 1 : -1;
}

int hm_store_del(const char *key)
{
    uint8_t out[128]; struct pm_w w; struct pm_store_del m = { pm_s(key) }; uint32_t id = new_id(); int fd = -1, code = 0, r;
    for (int i = 0; i < npend; i++) if (!strcmp(pend[i].key, key)) { pend_drop(i); break; }
    pm_winit(&w, out, sizeof out); pm_put_store_del(&w, id, &m);
    if (put(&w)) return -1;
    r = answer(id, 1, &fd, &code, NULL, NULL);
    if (fd >= 0) close(fd);
    return r == PM_OK ? 0 : r == PM_ERR && code == PM_ERR_NOT_FOUND ? 1 : -1;
}

/* ---- the log, the network */

/* LOG that came before the answer is of the place followed before: the broker sends LOG and answers LOG_FOLLOW on one
 * thread, so whatever it sent ahead of the OK is the old place's, and after it the new one's.  Handed on, the lines
 * came twice and out of order after a rewind (F2, found by a forwarder's tests).  So they are put aside, the kept ones and
 * those that come while it waits: dropped once hassmic took the new place (it waits for no OK of them then), and
 * handed on as before when it refused it (ERR BAD keeps the old place: dropped, its lines were lost without a gap;
 * R8 of a review, 2026-11-21) */
int hm_log_follow(const char *from)
{
    uint8_t out[128]; struct pm_w w; struct pm_log_follow m = { pm_s(from) }; uint32_t id = new_id(); int fd = -1, r, k = 0;
    if (stopped_now(NULL)) return -1;
    for (int i = 0; i < nkept; i++) { struct pm_hdr h; const uint8_t *body; size_t blen; kept[i].old = !pm_head(kept[i].data, kept[i].len, &h, &body, &blen) && h.type == PM_LOG; }
    pm_winit(&w, out, sizeof out); pm_put_log_follow(&w, id, &m);
    if (put(&w)) { for (int i = 0; i < nkept; i++) kept[i].old = 0; return -1; }
    follow_wait = 1; r = answer(id, 0, &fd, NULL, NULL, NULL); follow_wait = 0;
    if (fd >= 0) close(fd);
    for (int i = 0; i < nkept; i++) {
        if (kept[i].old && r == PM_OK) { if (kept[i].fd >= 0) close(kept[i].fd); kept_bytes -= kept[i].len; free(kept[i].data); continue; }
        kept[i].old = 0; kept[k++] = kept[i];
    }
    nkept = k;
    return r == PM_OK ? 0 : -1;
}

/* CONNECT of KIND (net.server, net.lan, net.internet: one message) */
static int connect_kind(int kind, int entry, const char *host, int port, int *why)
{
    uint8_t out[320]; struct pm_w w; struct pm_connect m = { (uint8_t)kind, (uint8_t)entry, pm_s(host ? host : ""), (uint16_t)port }; uint32_t id = new_id(); int fd = -1, code = 0, r;
    if (why) *why = 0;
    if (stopped_now(why)) return -1;
    /* not before READY (the ready callback runs before it): hassmic refuses it there, a connect may take longer than
     * READY may.  From tick, or any callback after ready */
    if (!running || entry < 0 || entry > 255 || port < 0 || port > 65535 || (host && strlen(host) > 253)) { if (why) *why = HM_ERR_BAD; return -1; }
    pm_winit(&w, out, sizeof out); pm_put_connect(&w, id, &m);
    if (put(&w)) { if (why) *why = put_why; return -1; }
    r = answer(id, 0, &fd, &code, NULL, NULL);
    if (r == PM_OK && fd >= 0) return fd;
    if (fd >= 0) close(fd);
    if (why) *why = r == PM_ERR ? code : stop_heard ? HM_ERR_FAILED : HM_ERR_UNKNOWN;
    return -1;
}
int hm_connect(int entry, int port, int *why) { return connect_kind(PM_CAP_NET_SERVER, entry, "", port, why); }
int hm_connect_lan(int entry, const char *address, int port, int *why) { return connect_kind(PM_CAP_NET_LAN, entry, address ? address : "", port, why); }
int hm_connect_internet(int entry, const char *host, int port, int *why) { return connect_kind(PM_CAP_NET_INTERNET, entry, host ? host : "", port, why); }

/* ---- music, the volume, UDP, proposals, Home Assistant */

/* a request and its answer: 0 OK, -1 (and *why) */
static int ask(struct pm_w *w, uint32_t id, int *fd, int *why, const uint8_t **ans, size_t *alen)
{
    int code = 0, r, got = -1;
    if (why) *why = 0;
    if (put(w)) { if (why) *why = put_why; return -1; }
    r = answer(id, 0, &got, &code, ans, alen);
    if (fd) *fd = got; else if (got >= 0) close(got);
    if (r == PM_OK || r == PM_UDP_OPENED) return 0;
    if (why) *why = r == PM_ERR ? code : stop_heard ? HM_ERR_FAILED : HM_ERR_UNKNOWN;
    if (fd && got >= 0) { close(got); *fd = -1; }
    return -1;
}

int hm_music_open(unsigned rate, int channels, int *why)
{
    uint8_t out[32]; struct pm_w w; struct pm_music_open m = { rate, (uint8_t)channels }; uint32_t id = new_id(); int fd = -1;
    if (stopped_now(why)) return -1;
    if (!running) { if (why) *why = HM_ERR_BAD; return -1; }
    pm_winit(&w, out, sizeof out); pm_put_music_open(&w, id, &m);
    if (ask(&w, id, &fd, why, NULL, NULL) || fd < 0) return -1;
    if (music_fd >= 0) close(music_fd);
    music_fd = fd; music_playing = 1;           /* (hassmic takes an open stream as playing) */
    return fd;
}

int hm_music_state(int state, const char *title, const char *artist)
{
    uint8_t out[400]; struct pm_w w; struct pm_music_state m = { (uint8_t)state, pm_s(title ? title : ""), pm_s(artist ? artist : "") };
    pm_winit(&w, out, sizeof out); pm_put_music_state(&w, 0, &m);
    if (put(&w)) return -1;                     /* (a state MUSIC_* has in no version, or audio.music not spoken: not sent, nothing changes) */
    music_playing = state == HM_MUSIC_PLAYING;
    return 0;
}

int hm_music_close(void)
{
    uint8_t out[16]; struct pm_w w; uint32_t id = new_id(); int r;
    if (music_fd < 0 || stopped_now(NULL)) return -1;
    pm_winit(&w, out, sizeof out); pm_put_music_close(&w, id);
    r = ask(&w, id, NULL, NULL, NULL, NULL);
    close(music_fd); music_fd = -1;             /* (what is in the socket's buffer is read by hassmic and plays out) */
    return r;
}

int hm_volume_set(int percent)
{
    uint8_t out[16]; struct pm_w w; struct pm_volume_set m = { (uint8_t)(percent < 0 ? 0 : percent > 100 ? 100 : percent) }; uint32_t id = new_id();
    if (stopped_now(NULL)) return -1;
    pm_winit(&w, out, sizeof out); pm_put_volume_set(&w, id, &m);
    return ask(&w, id, NULL, NULL, NULL, NULL);
}

int hm_udp_open(int kind, int entry, int *handle, int *port, int *why)
{
    uint8_t out[16]; struct pm_w w; struct pm_udp_open m = { (uint8_t)kind, (uint8_t)entry }; uint32_t id = new_id(); int fd = -1; const uint8_t *b; size_t bl; struct pm_udp_opened o;
    if (stopped_now(why)) return -1;
    if (!running) { if (why) *why = HM_ERR_BAD; return -1; }
    pm_winit(&w, out, sizeof out); pm_put_udp_open(&w, id, &m);
    if (ask(&w, id, &fd, why, &b, &bl) || fd < 0 || pm_get_udp_opened(b, bl, &o)) { if (fd >= 0) close(fd); return -1; }
    if (handle) *handle = o.handle;
    if (port) *port = o.port;
    return fd;
}

int hm_udp_peer(int handle, uint32_t address, int port)
{
    uint8_t out[32]; struct pm_w w; struct pm_udp_peer m = { (uint8_t)handle, address, (uint16_t)port }; uint32_t id = new_id();
    if (stopped_now(NULL)) return -1;
    pm_winit(&w, out, sizeof out); pm_put_udp_peer(&w, id, &m);
    return ask(&w, id, NULL, NULL, NULL, NULL);
}

int hm_udp_sendto(int handle, uint32_t address, int port, const void *data, size_t len)
{
    uint8_t out[1600]; struct pm_w w; struct pm_sendto m = { (uint8_t)handle, address, (uint16_t)port, pm_b(data, len) };
    if (len > 1472) return -1;
    pm_winit(&w, out, sizeof out); pm_put_sendto(&w, 0, &m);
    return put(&w);
}

int hm_broadcast(int handle, const void *data, size_t len)
{
    uint8_t out[1600]; struct pm_w w; struct pm_broadcast m = { (uint8_t)handle, pm_b(data, len) };
    if (len > 1472) return -1;
    pm_winit(&w, out, sizeof out); pm_put_broadcast(&w, 0, &m);
    return put(&w);
}

int hm_propose(const char *setting, const char *value)
{
    uint8_t out[128]; struct pm_w w; struct pm_propose m = { pm_s(setting), pm_s(value) }; uint32_t id = new_id();
    if (stopped_now(NULL)) return -1;
    pm_winit(&w, out, sizeof out); pm_put_propose(&w, id, &m);
    return ask(&w, id, NULL, NULL, NULL, NULL);
}

int hm_entity_state(const char *name, const char *value)
{
    uint8_t out[400]; struct pm_w w; struct pm_entity_state m = { pm_s(name), pm_s(value) };
    pm_winit(&w, out, sizeof out); pm_put_entity_state(&w, 0, &m);
    return put(&w);
}

int hm_event(const char *name, const char *type)
{
    uint8_t out[128]; struct pm_w w; struct pm_event m = { pm_s(name), pm_s(type) };
    pm_winit(&w, out, sizeof out); pm_put_event(&w, 0, &m);
    return put(&w);
}

/* ---- calls */

const char *hm_action_token(void) { return action_token; }

int hm_call_incoming(const char *who, int *why)
{
    uint8_t out[128]; struct pm_w w; struct pm_call_incoming m = { pm_s(who ? who : "") }; uint32_t id = new_id();
    if (stopped_now(why)) return -1;
    if (!running) { if (why) *why = HM_ERR_BAD; return -1; }
    pm_winit(&w, out, sizeof out); pm_put_call_incoming(&w, id, &m);
    return ask(&w, id, NULL, why, NULL, NULL);
}

int hm_call_out(const char *token, const char *who, int *why)
{
    uint8_t out[160]; struct pm_w w; struct pm_call_out m = { pm_s(token ? token : ""), pm_s(who ? who : "") }; uint32_t id = new_id();
    if (stopped_now(why)) return -1;
    if (!running) { if (why) *why = HM_ERR_BAD; return -1; }
    pm_winit(&w, out, sizeof out); pm_put_call_out(&w, id, &m);
    return ask(&w, id, NULL, why, NULL, NULL);
}

int hm_call_ringing_there(void)
{
    uint8_t out[16]; struct pm_w w;
    pm_winit(&w, out, sizeof out); pm_put_call_ringing_there(&w, 0);
    return put(&w);
}

int hm_call_live(const char *token, int *why)
{
    uint8_t out[64]; struct pm_w w; struct pm_call_live m = { pm_s(token ? token : "") }; uint32_t id = new_id(); int fd = -1;
    if (stopped_now(why)) return -1;
    if (!running) { if (why) *why = HM_ERR_BAD; return -1; }
    pm_winit(&w, out, sizeof out); pm_put_call_live(&w, id, &m);
    if (ask(&w, id, &fd, why, NULL, NULL) || fd < 0) return -1;
    if (call_fd >= 0) close(call_fd);
    call_fd = fd;                               /* (the loop polls it: call_audio) */
    return 0;
}

int hm_call_end(int result)
{
    uint8_t out[16]; struct pm_w w; struct pm_call_end m = { (uint8_t)result }; uint32_t id = new_id();
    if (call_fd >= 0) { close(call_fd); call_fd = -1; }
    if (stopped_now(NULL)) return -1;
    pm_winit(&w, out, sizeof out); pm_put_call_end(&w, id, &m);
    return ask(&w, id, NULL, NULL, NULL, NULL);
}

int hm_call_audio(uint32_t seq, const int16_t *pcm)
{
    uint8_t pkt[4 + HM_CALL_FRAME * 2];
    if (call_fd < 0) return -1;
    for (int i = 0; i < 4; i++) pkt[i] = (uint8_t)(seq >> 8 * i);
    memcpy(pkt + 4, pcm, HM_CALL_FRAME * 2);    /* (s16le: the samples as they are in memory on every target hassmic runs on) */
    return send(call_fd, pkt, sizeof pkt, MSG_DONTWAIT | MSG_NOSIGNAL) == (long)sizeof pkt ? 0 : -1;
}

int hm_watch(int fd, void (*readable)(void *ctx, int fd))
{
    if (fd < 0 || !readable || nwatch == WATCH_MAX) return -1;
    for (int i = 0; i < nwatch; i++) if (watch[i].fd == fd) { watch[i].readable = readable; return 0; }
    watch[nwatch].fd = fd; watch[nwatch++].readable = readable;
    return 0;
}

int hm_unwatch(int fd)
{
    for (int i = 0; i < nwatch; i++) if (watch[i].fd == fd) { watch[i] = watch[--nwatch]; return 0; }
    return -1;
}

int hm_file_read(const char *list, const char *item, const char *suffix, size_t max, void **data, size_t *len)
{
    int fd = hm_file_open(list, item, suffix); uint8_t *b = NULL, *nb; size_t n = 0, cap = 0; long k = 0;
    if (fd < 0) return -1;
    for (;;) {
        if (n == cap) { cap = cap ? cap * 2 : 16384; if (cap > max + 1) cap = max + 1; if (!(nb = realloc(b, cap))) { k = -1; break; } b = nb; }
        if (n > max) { k = -1; break; }
        while ((k = read(fd, b + n, cap - n)) < 0 && errno == EINTR) ;
        if (k <= 0) break;
        n += (size_t)k;
    }
    close(fd);
    if (k < 0) { free(b); return -1; }
    *data = b; *len = n;
    return 0;
}

int hm_wake_words(const char *list, const char *const *ids, int n)
{
    uint8_t out[1024]; struct pm_w w; struct pm_wake_words m = { pm_s(list), { { 0 }, (uint16_t)(n < 0 ? 0 : n) } };
    pm_winit(&w, out, sizeof out); pm_put_wake_words(&w, 0, &m);
    for (int i = 0; i < n; i++) { struct pm_wake_words_words e = { pm_s(ids[i]) }; pm_add_wake_words_words(&w, &e); }
    return put(&w);
}

int hm_wake_heard(int slot, uint64_t begin, uint64_t end, int score)
{
    uint8_t out[64]; struct pm_w w; struct pm_wake_heard m = { (uint8_t)slot, (uint8_t)(score < 0 ? 0 : score > 255 ? 255 : score), begin, end };
    pm_winit(&w, out, sizeof out); pm_put_wake_heard(&w, 0, &m);
    return put(&w);
}

int hm_run(const struct hm_callbacks *callbacks, void *context)
{
    /* (out: the greeting at its bound, every scope there will ever be, PM_SCOPES_MAX of 2 + 24 + 2 bytes) */
    static uint8_t buf[PM_MAX_PACKET], out[PM_HEADER + 8 + PM_SCOPES_MAX * 28]; struct pm_w w; struct pm_hello hello = { SDK_VERSION, { { 0 }, PM_SCOPES } };
    struct pm_hdr h; const uint8_t *body; size_t blen; struct pm_welcome wm; struct pm_welcome_scopes ws; int fd; long n;
    _Static_assert(sizeof out >= PM_HEADER + 4 + 2 + PM_SCOPES_MAX * (2 + 24 + 2), "HELLO at its bound: every scope there will ever be");
    cb = callbacks; ctx = context;
    { struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_sigsys; sa.sa_flags = SA_SIGINFO; sigemptyset(&sa.sa_mask); sigaction(SIGSYS, &sa, NULL); }
    /* every scope this client speaks, at the version of its sdk/messages.def: hassmic answers with what of it is
     * spoken (the manifest's requires), or REFUSED where the manifest requires more than this client has */
    pm_winit(&w, out, sizeof out); pm_put_hello(&w, new_id(), &hello);
    for (int s = 0; s < PM_SCOPES; s++) { struct pm_hello_scopes e = { pm_s(pm_scope_name(s)), (uint16_t)pm_scope_version(s) }; pm_add_hello_scopes(&w, &e); }
    if (put(&w) || (n = get(buf, sizeof buf, &fd)) <= 0 || pm_head(buf, (size_t)n, &h, &body, &blen) || h.type != PM_WELCOME || pm_get_welcome(body, blen, &wm)) return -1;
    while (pm_next_welcome_scopes(&wm.scopes, &ws)) {
        int s = pm_scope_of(ws.name.s, ws.name.n);
        /* (what this client does not have, or twice: no hassmic that read its HELLO says it) */
        if (s < 0 || ver[s] || !ws.version || ws.version > pm_scope_version(s)) { static const char why[] = "plugin client: hassmic speaks a scope this client does not have\n"; if (write(2, why, sizeof why - 1) < 0) { } return -1; }
        ver[s] = ws.version;
    }
    if (!ver[PM_SCOPE_CORE]) return -1;
    info.clock_set = (int)wm.clock_set;
    pm_cstr(info.model, sizeof info.model, wm.model); pm_cstr(info.node, sizeof info.node, wm.node);
    pm_cstr(info.name, sizeof info.name, wm.name); pm_cstr(info.zone, sizeof info.zone, wm.zone);
    while (!stopping) {
        struct pollfd pf[5 + WATCH_MAX]; int nw, wfd[WATCH_MAX];
        /* what came while a request waited, in its order, before anything newer; nothing after a STOP (its callback
         * was the last) */
        while (nkept && !stopping) {
            uint8_t *d = kept[0].data; size_t l = kept[0].len; int f = kept[0].fd;
            for (int i = 1; i < nkept; i++) kept[i - 1] = kept[i];
            nkept--; kept_bytes -= l;
            handle(d, l, f); free(d);
        }
        if (stopping) break;
        pend_try();
        /* The tick, then the sockets in any case: a tick that takes longer than its second went straight to the next
         * one, PINGs went unanswered and STOP unread, and the plugin was ended as one that stopped answering (a
         * review, 2026-11-21).  After a tick the poll waits for nothing (what came meanwhile, at once) */
        if (running && cb->tick && mono_ms() >= tick_at) { tick_at = mono_ms() + 1000; cb->tick(ctx); if (nkept || stopping) continue; }
        /* what is polled, as the callbacks just left it (a stream opened or a socket watched in this tick counts at once) */
        pf[0] = (struct pollfd){ CTL, POLLIN, 0 }; pf[1] = (struct pollfd){ stream, POLLIN, 0 };
        pf[2] = (struct pollfd){ music_fd >= 0 && music_playing && cb->music_writable ? music_fd : -1, POLLOUT, 0 };
        nw = nwatch;
        for (int i = 0; i < nw; i++) { pf[3 + i].fd = wfd[i] = watch[i].fd; pf[3 + i].events = POLLIN; pf[3 + i].revents = 0; }
        pf[3 + nw] = (struct pollfd){ call_fd, POLLIN, 0 };     /* (a call's audio while it is live; -1 is left out) */
        pf[4 + nw] = (struct pollfd){ req_fd, POLLIN, 0 };      /* (a request's audio) */
        { long long now = mono_ms(), next = -1;
          if (running && cb->tick) next = tick_at;
          if (npend && (next < 0 || pend_at < next)) next = pend_at;
          /* (a negative descriptor is left out by poll: the stream and the music only while there are) */
          if (poll(pf, (nfds_t)(5 + nw), next < 0 ? -1 : next > now ? (int)(next - now) : 0) < 0) { if (errno == EINTR) continue; return -1; } }
        if (music_fd >= 0 && pf[2].fd == music_fd && (pf[2].revents & (POLLOUT | POLLERR | POLLHUP)) && cb->music_writable) cb->music_writable(ctx, music_fd);
        for (int i = 0; i < nw; i++) if (pf[3 + i].revents & (POLLIN | POLLERR | POLLHUP))
            for (int k = 0; k < nwatch; k++) if (watch[k].fd == wfd[i]) { watch[k].readable(ctx, wfd[i]); break; }
        if (call_fd >= 0 && pf[3 + nw].fd == call_fd && (pf[3 + nw].revents & (POLLIN | POLLHUP | POLLERR))) {
            /* the room's frames: a u32 sequence number, then 20 ms of PCM.  Its end: hassmic ended the call (its
             * CALL_HANGUP follows on the control socket, or the plugin's own hm_call_end came first) */
            static union { uint8_t b[4 + HM_CALL_FRAME * 2 + 4]; int16_t align; } c;
            long k = recv(call_fd, c.b, sizeof c.b, MSG_DONTWAIT);
            if (k == 4 + HM_CALL_FRAME * 2) {
                static int16_t pcm[HM_CALL_FRAME]; uint32_t seq = 0;
                for (int i = 0; i < 4; i++) seq |= (uint32_t)c.b[i] << (8 * i);
                memcpy(pcm, c.b + 4, sizeof pcm);
                if (cb->call_audio) cb->call_audio(ctx, seq, pcm, HM_CALL_FRAME);
            } else if (k == 0 || (k < 0 && errno != EAGAIN && errno != EINTR)) { close(call_fd); call_fd = -1; }
        }
        if (req_fd >= 0 && pf[4 + nw].fd == req_fd && (pf[4 + nw].revents & (POLLIN | POLLHUP | POLLERR))) req_read(0);
        if (stream >= 0 && (pf[1].revents & POLLIN)) {
            /* u64 position, then the block: 16 kHz mono s16le, PM_WAKE_SAMPLES_MAX samples at most (hassmic holds
             * to it: a packet longer than this buffer would be cut, its rest lost) */
            static union { uint8_t b[8 + PM_WAKE_SAMPLES_MAX * 2]; int16_t align; } a;
            long k = recv(stream, a.b, sizeof a.b, MSG_DONTWAIT);
            if (k >= 8 && !((k - 8) & 1) && cb->wake_audio) {
                uint64_t pos = 0; static int16_t pcm[PM_WAKE_SAMPLES_MAX];
                for (int i = 0; i < 8; i++) pos |= (uint64_t)a.b[i] << (8 * i);
                memcpy(pcm, a.b + 8, (size_t)(k - 8));
                cb->wake_audio(ctx, pos, pcm, (int)((k - 8) / 2));
            }
        } else if (stream >= 0 && (pf[1].revents & (POLLHUP | POLLERR))) { close(stream); stream = -1; }
        if (pf[0].revents & POLLIN) {
            /* everything that waits, up to a bound: one message a round fell behind a plugin whose tick is slow.  But
             * not past what a callback's request kept meanwhile: that is older, and the loop hands it on first (read
             * on here, a LOG overtook the one kept before it and the cursor went backwards; R3 of a review,
             * 2026-11-21) */
            for (int k = 0; k < 64 && !stopping && !nkept; k++) {
                if (k) { struct pollfd q = { CTL, POLLIN, 0 }; if (poll(&q, 1, 0) != 1 || !(q.revents & POLLIN)) break; }
                if ((n = get(buf, sizeof buf, &fd)) <= 0) return -1;
                handle(buf, (size_t)n, fd);
            }
        } else if (pf[0].revents & (POLLHUP | POLLERR)) return -1;
    }
    while (nkept) { if (kept[--nkept].fd >= 0) close(kept[nkept].fd); free(kept[nkept].data); }        /* (after STOP: not for this plugin) */
    if (call_fd >= 0) { close(call_fd); call_fd = -1; }
    if (req_fd >= 0) { close(req_fd); req_fd = -1; }
    kept_bytes = 0;
    pend_flush();
    return 0;
}
