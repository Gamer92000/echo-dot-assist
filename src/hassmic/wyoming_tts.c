/* Home Assistant's text to speech made on the Echo (wyoming_tts.h): a Wyoming TTS server on the Echo's own voice
 * (speech.h).  Home Assistant's Wyoming integration finds it by mDNS (_wyoming._tcp, in the service file
 * proto_esphome.c writes) or by address and port; its voice assistant then names it as its text-to-speech engine, so
 * the computer running Home Assistant makes no speech, and the voice is there for every speaker Home Assistant has
 * (tts.speak, announcements).
 *
 * The protocol (wyoming 1.7+, as Home Assistant's integration speaks it, homeassistant/components/wyoming/tts.py of
 * 2026.10): "describe" -> "info" with one TTS program, its voices and supports_synthesize_streaming.  Offered, so Home
 * Assistant sends every text as a stream (whole ones too: tts/__init__.py wraps a string in a one-chunk stream for an
 * engine that streams): "synthesize-start" {voice}, "synthesize-chunk" {text} as the LLM writes, a "synthesize" with
 * the whole text (for servers without streaming: left out here), "synthesize-stop".  The answer: "audio-start"
 * {rate, width, channels} once, "audio-chunk"s sentence after sentence, "audio-stop", then "synthesize-stopped", which
 * is what Home Assistant's reader waits for (it takes the first audio-start for its WAV header and ignores
 * audio-stop).  A plain "synthesize" outside a stream is answered as before: audio-start, chunks, audio-stop.
 * The streamed text is cut into sentences (wytts_sentence) and each is made as soon as it is complete, so the first
 * plays while the LLM still writes.  The audio goes out as the engine makes it (~6x real time on the Show 5).
 *
 * A stream that is this Echo's own run's reply (main.c core_reply_claim: own_voice_replies on, the run waiting for its
 * streamed reply) is not made for Home Assistant: its sentences are said here at once (core_reply_part) and Home
 * Assistant gets 100 ms of silence, which it plays nowhere (the Echo does not fetch it). */
#include "wyoming_tts.h"
#include "speech.h"
#include "wyoming.h"
#include "netio.h"
#include "core.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CHUNK 4096                  /* bytes per audio-chunk: ~93 ms at 22 050 Hz */
#define TEXT_MAX 4096
#define SENTENCE_MAX 400            /* a "sentence" without an end longer than this is cut at a space: an LLM's lists */
#define SILENCE_RATE 16000          /* Home Assistant's silence for a reply said here: 100 ms (its ffmpeg takes any rate) */
#define CLAIM_WAIT_MS 500           /* how long a synthesis waits for its run's event (core_reply_claim -1) */
#include <time.h>

/* one synthesize: the speech thread appends, the connection's thread sends */
struct job {
    pthread_mutex_t m; pthread_cond_t c;
    unsigned char *buf; size_t len, cap;
    unsigned rate; int begun, done, ok, failed;
    struct speech_sink sink;
};

static void j_begin(void *arg, unsigned rate)
{ struct job *j = arg; pthread_mutex_lock(&j->m); j->rate = rate; j->begun = 1; pthread_cond_signal(&j->c); pthread_mutex_unlock(&j->m); }

static void j_data(void *arg, const void *pcm, unsigned len)
{
    struct job *j = arg;
    pthread_mutex_lock(&j->m);
    if (!j->failed && j->len + len > j->cap) {
        size_t cap = j->cap ? j->cap * 2 : 1 << 16;
        while (cap < j->len + len) cap *= 2;
        unsigned char *b = realloc(j->buf, cap);
        if (b) { j->buf = b; j->cap = cap; } else j->failed = 1;   /* out of memory: what came is sent, then an error */
    }
    if (!j->failed) { memcpy(j->buf + j->len, pcm, len); j->len += len; }
    pthread_cond_signal(&j->c);
    pthread_mutex_unlock(&j->m);
}

static void j_end(void *arg, int ok)
{ struct job *j = arg; pthread_mutex_lock(&j->m); j->done = 1; j->ok = ok && !j->failed; pthread_cond_signal(&j->c); pthread_mutex_unlock(&j->m); }

/* the data block's "text" as UTF-8: JSON escapes, \uXXXX with surrogate pairs (Python's json writes non-ASCII so) */
static int hex4(const char *p, unsigned *v)
{
    *v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i]; *v <<= 4;
        if (c >= '0' && c <= '9') *v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') *v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') *v |= (unsigned)(c - 'A' + 10);
        else return -1;
    }
    return 0;
}
static size_t utf8(unsigned cp, char *o)
{
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xc0 | cp >> 6); o[1] = (char)(0x80 | (cp & 0x3f)); return 2; }
    if (cp < 0x10000) { o[0] = (char)(0xe0 | cp >> 12); o[1] = (char)(0x80 | (cp >> 6 & 0x3f)); o[2] = (char)(0x80 | (cp & 0x3f)); return 3; }
    o[0] = (char)(0xf0 | cp >> 18); o[1] = (char)(0x80 | (cp >> 12 & 0x3f)); o[2] = (char)(0x80 | (cp >> 6 & 0x3f)); o[3] = (char)(0x80 | (cp & 0x3f)); return 4;
}
int wytts_json_text(const char *json, const char *key, char *out, size_t cap)
{
    char k[64]; snprintf(k, sizeof k, "\"%s\"", key);
    const char *p = strstr(json, k);
    if (!p || !cap) return -1;
    p += strlen(k);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p++ != ':') return -1;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p++ != '"') return -1;
    size_t n = 0;
    while (*p && *p != '"') {
        char b[4]; size_t m = 1;
        if (*p != '\\') b[0] = *p++;
        else {
            p++;
            switch (*p) {
            case 'n': b[0] = '\n'; break;                        /* a sentence's end to wytts_sentence; said as a space */
            case 't': case 'r': b[0] = ' '; break;
            case 'b': case 'f': b[0] = ' '; break;
            case 'u': {
                unsigned cp, lo;
                if (hex4(p + 1, &cp)) return -1;
                p += 4;
                if (cp >= 0xd800 && cp < 0xdc00 && p[1] == '\\' && p[2] == 'u' && !hex4(p + 3, &lo) && lo >= 0xdc00 && lo < 0xe000) {
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00); p += 6;
                }
                m = utf8(cp, b);
                break;
            }
            case 0: return -1;
            default: b[0] = *p;                                  /* \" \\ \/ */
            }
            p++;
        }
        if (n + m >= cap) break;                                 /* longer than we take: the start of it */
        memcpy(out + n, b, m); n += m;
    }
    out[n] = 0;
    return 0;
}

/* Where the first sentence of S (N bytes) ends: its length, or 0 while it may go on (more text to come; FINAL: none).
 * Ends: ".", "!", "?" (and closing quotes or brackets after them) before white space, and a line break.  Not after a
 * number of one or two digits ("3. November", "1. Step") or a single letter ("J. Smith", "U.S. "), nor after the usual short
 * abbreviations ("Dr.", "e.g.", "z.B."), so the voice does not pause in the middle.  A run of SENTENCE_MAX bytes with
 * no end is cut at its last space */
static int abbrev(const char *s, size_t i)                  /* s[i] is the '.' */
{
    static const char *const ab[] = { "mr", "mrs", "ms", "dr", "st", "vs", "etc", "e.g", "i.e", "prof", "sr", "jr",
                                      "approx", "ca", "no", "z.b", "bzw", "usw", "nr", "inc", "ltd", "fig", NULL };
    size_t b = i;
    while (b > 0 && s[b - 1] != ' ' && s[b - 1] != '\n' && s[b - 1] != '(' && s[b - 1] != '"') b--;
    size_t n = i - b;
    if (!n) return 0;
    int digits = 1;                                          /* "3. November", "1. Step"; not "in 1998." */
    for (size_t k = b; k < i; k++) if (s[k] < '0' || s[k] > '9') digits = 0;
    if (digits && n <= 2) return 1;
    if (n == 1 && ((s[b] >= 'A' && s[b] <= 'Z') || (s[b] >= 'a' && s[b] <= 'z'))) return 1;
    if (n >= 3 && s[i - 2] == '.') return 1;                /* "U.S." "e.g." */
    for (int k = 0; ab[k]; k++) {
        size_t m = strlen(ab[k]);
        if (m != n) continue;
        size_t j = 0;
        while (j < m && (s[b + j] | 0x20) == ab[k][j]) j++;
        if (j == m) return 1;
    }
    return 0;
}
size_t wytts_sentence(const char *s, size_t n, int final)
{
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\n') return i + 1;
        if (c != '.' && c != '!' && c != '?') continue;
        size_t e = i + 1;
        for (;;) {                                          /* "...", "?!", closing quotes and brackets (also "”" "’") */
            if (e < n && (s[e] == '.' || s[e] == '!' || s[e] == '?' || s[e] == '"' || s[e] == '\'' || s[e] == ')')) { e++; continue; }
            if (e + 3 <= n && (unsigned char)s[e] == 0xe2 && (unsigned char)s[e + 1] == 0x80 &&
                ((unsigned char)s[e + 2] == 0x9d || (unsigned char)s[e + 2] == 0x99)) { e += 3; continue; }
            break;
        }
        if (e == n) return final ? n : 0;                   /* the end of what came: more of it may follow */
        if (s[e] != ' ' && s[e] != '\n' && s[e] != '\t') { i = e - 1; continue; }   /* "3.5", "example.com" */
        if (c == '.' && e == i + 1 && abbrev(s, i)) continue;
        return e;
    }
    if (final) return n;
    if (n >= SENTENCE_MAX) {
        size_t sp = SENTENCE_MAX;
        while (sp > SENTENCE_MAX / 2 && s[sp - 1] != ' ') sp--;
        return sp > SENTENCE_MAX / 2 ? sp : SENTENCE_MAX;
    }
    return 0;
}

/* what is said of a piece of text: line breaks as spaces, the spaces around it gone; nothing but white space = nothing */
static int speakable(char *t)
{
    for (char *p = t; *p; p++) if (*p == '\n' || *p == '\t' || *p == '\r') *p = ' ';
    size_t b = strspn(t, " "), n = strlen(t + b);
    memmove(t, t + b, n + 1);
    while (n && t[n - 1] == ' ') t[--n] = 0;
    return n > 0;
}

static long long now_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* one link: the reader thread, and while a stream is made for Home Assistant its sender thread, write; one at a time */
struct link {
    int fd;
    pthread_mutex_t wm;
};
static int lw(struct link *l, const char *type, const char *data, const void *payload, size_t len)
{
    pthread_mutex_lock(&l->wm);
    int rc = wy_write(l->fd, type, data, payload, len);
    pthread_mutex_unlock(&l->wm);
    return rc;
}

static int send_info(struct link *l)
{
    char d[1400], v[64], lg[32];
    const char *voice = speech_voice(), *lang = speech_language();
    snprintf(v, sizeof v, "%s", *voice ? voice : "none");
    snprintf(lg, sizeof lg, "%s", *lang ? lang : "en-US");
    for (char *s = v; *s; s++) if (*s == '"' || *s == '\\') *s = '_';
    const char *attr = "{\"name\":\"Amazon (Ivona), on the Echo\",\"url\":\"https://github.com/Nt-f/echo-dot-assist\"}";
    snprintf(d, sizeof d,
             "{\"asr\":[],\"tts\":[{\"name\":\"hassmic\",\"attribution\":%s,\"installed\":true,\"description\":\"The Echo's own voice\","
             "\"version\":\"" VERSION "\",\"supports_synthesize_streaming\":true,\"voices\":[{\"name\":\"%s\",\"attribution\":%s,"
             "\"installed\":true,\"description\":\"%s\",\"version\":null,\"languages\":[\"%s\"],\"speakers\":null}]}],"
             "\"handle\":[],\"intent\":[],\"wake\":[],\"mic\":[],\"snd\":[]}",
             attr, v, attr, v, lg);
    return lw(l, "info", d, NULL, 0);
}

/* The audio of one text for the caller, as the engine makes it.  *STARTED: audio-start went out already (a stream sends
 * it once); RATE its rate.  0, or -1 when the link is gone (the rest is read to the end anyway); *OK: all of it came */
static int render(struct link *l, const char *text, int *started, unsigned *rate_out, int *ok_out)
{
    struct job *j = calloc(1, sizeof *j);
    *ok_out = 0;
    if (!j) return 0;
    pthread_mutex_init(&j->m, NULL); pthread_cond_init(&j->c, NULL);
    j->sink = (struct speech_sink){ j_begin, j_data, j_end, j };
    if (speech_render(text, &j->sink)) {
        pthread_mutex_destroy(&j->m); pthread_cond_destroy(&j->c); free(j);
        fprintf(stderr, "wyoming tts: the voice is not ready or busy\n");
        return 0;
    }
    size_t sent = 0; int rc = 0, ok, sent_start = 0;
    unsigned char chunk[CHUNK]; char hd[96];
    for (;;) {
        pthread_mutex_lock(&j->m);
        while (!j->done && !(j->begun && !sent_start) && j->len - sent < CHUNK) pthread_cond_wait(&j->c, &j->m);
        size_t n = j->len - sent; if (n > CHUNK) n = CHUNK;
        if (n) memcpy(chunk, j->buf + sent, n);
        int done = j->done && j->len - sent <= n, begun = j->begun; unsigned rate = j->rate;
        ok = j->ok;
        pthread_mutex_unlock(&j->m);
        if (begun && !sent_start) {
            sent_start = 1;
            if (*started && rate != *rate_out) fprintf(stderr, "wyoming tts: %u Hz after %u Hz in one stream\n", rate, *rate_out);
            snprintf(hd, sizeof hd, "{\"rate\":%u,\"width\":2,\"channels\":1}", rate);
            if (!*started && rc == 0) { rc = lw(l, "audio-start", hd, NULL, 0); *started = 1; *rate_out = rate; }
        }
        if (n && rc == 0) rc = lw(l, "audio-chunk", hd, chunk, n);
        sent += n;
        if (done) break;                                         /* rc < 0 (Home Assistant left): read on to the end */
    }
    fprintf(stderr, "wyoming tts: \"%.60s%s\" %s, %zu bytes\n", text, strlen(text) > 60 ? "..." : "", ok ? "made" : "failed", sent);
    pthread_mutex_destroy(&j->m); pthread_cond_destroy(&j->c);
    free(j->buf); free(j);
    *ok_out = ok;
    return rc;
}

/* a whole text ("synthesize" outside a stream): as before streaming */
static int synthesize(struct link *l, char *text)
{
    int started = 0, ok; unsigned rate = 0;
    speakable(text);
    int rc = render(l, text, &started, &rate, &ok);
    if (rc == 0 && !started && !ok)
        return lw(l, "error", "{\"text\":\"the Echo's voice is not ready or busy\",\"code\":\"busy\"}", NULL, 0);
    if (rc == 0 && started) rc = lw(l, "audio-stop", "{}", NULL, 0);
    if (rc == 0 && !ok) rc = lw(l, "error", "{\"text\":\"the Echo's voice failed on this text\",\"code\":\"failed\"}", NULL, 0);
    return rc;
}

/* 100 ms of silence: Home Assistant's audio for a reply the Echo says itself */
static int silence(struct link *l)
{
    static const unsigned char zero[SILENCE_RATE / 10 * 2];
    char hd[96];
    snprintf(hd, sizeof hd, "{\"rate\":%u,\"width\":2,\"channels\":1}", SILENCE_RATE);
    int rc = lw(l, "audio-start", hd, NULL, 0);
    if (rc == 0) rc = lw(l, "audio-chunk", hd, zero, sizeof zero);
    if (rc == 0) rc = lw(l, "audio-stop", "{}", NULL, 0);
    return rc;
}

/* A stream (synthesize-start .. synthesize-stop) */
enum { S_RENDER, S_OWN, S_SILENT };
struct stream {
    struct link *l;
    int mode;                                   /* S_* */
    char text[TEXT_MAX]; size_t n;              /* what came that is no whole sentence yet */
    /* S_RENDER: the sentences, made one after the other by the sender thread */
    pthread_mutex_t m; pthread_cond_t c; pthread_t th; int th_on;
    char **q; size_t qn, qcap, qhead; int closed;
    int started, rc; unsigned rate;
    int sentences;
};

static void *sender(void *arg)
{
    struct stream *st = arg;
    for (;;) {
        pthread_mutex_lock(&st->m);
        while (st->qhead == st->qn && !st->closed) pthread_cond_wait(&st->c, &st->m);
        if (st->qhead == st->qn) { pthread_mutex_unlock(&st->m); break; }
        char *t = st->q[st->qhead]; st->q[st->qhead++] = NULL;
        pthread_mutex_unlock(&st->m);
        int ok;
        if (st->rc == 0) st->rc = render(st->l, t, &st->started, &st->rate, &ok);
        free(t);
    }
    return NULL;
}

static void stream_sentence(struct stream *st, char *t)
{
    if (!speakable(t)) return;
    st->sentences++;
    if (st->mode == S_OWN) {
        int r; long long until = now_ms() + 30000;           /* the voice's queue full: it says ~6 sentences a second */
        while ((r = core_reply_part(t)) == 1 && now_ms() < until) usleep(50000);
        if (r) { st->mode = S_SILENT; fprintf(stderr, "wyoming tts: the reply was cut, the rest is not said\n"); }
        return;
    }
    if (st->mode != S_RENDER) return;
    char *d = strdup(t);
    if (!d) return;
    pthread_mutex_lock(&st->m);
    if (st->qn == st->qcap) {
        size_t cap = st->qcap ? st->qcap * 2 : 16;
        char **q = realloc(st->q, cap * sizeof *q);
        if (!q) { pthread_mutex_unlock(&st->m); free(d); return; }
        st->q = q; st->qcap = cap;
    }
    st->q[st->qn++] = d;
    pthread_cond_signal(&st->c);
    pthread_mutex_unlock(&st->m);
}

/* TEXT came (NULL: the end): every whole sentence goes on */
static void stream_text(struct stream *st, const char *text)
{
    if (text) {
        size_t m = strlen(text);
        while (m) {                                          /* longer than the buffer: what is in it goes first */
            size_t k = sizeof st->text - 1 - st->n; if (k > m) k = m;
            memcpy(st->text + st->n, text, k); st->n += k; st->text[st->n] = 0; text += k; m -= k;
            for (size_t e; st->n && (e = wytts_sentence(st->text, st->n, st->n == sizeof st->text - 1)); ) {
                char t[TEXT_MAX];
                memcpy(t, st->text, e); t[e] = 0;
                memmove(st->text, st->text + e, st->n - e); st->n -= e; st->text[st->n] = 0;
                stream_sentence(st, t);
            }
        }
        return;
    }
    if (st->n) { char t[TEXT_MAX]; memcpy(t, st->text, st->n + 1); st->n = 0; st->text[0] = 0; stream_sentence(st, t); }
}

static struct stream *stream_start(struct link *l)
{
    struct stream *st = calloc(1, sizeof *st);
    if (!st) return NULL;
    st->l = l;
    pthread_mutex_init(&st->m, NULL); pthread_cond_init(&st->c, NULL);
    /* whose: the Echo's own run's reply, or someone else's (core_reply_claim); its event may come a moment after it */
    int r; long long until = now_ms() + CLAIM_WAIT_MS;
    while ((r = core_reply_claim()) < 0 && now_ms() < until) usleep(20000);
    st->mode = r == 1 ? S_OWN : r == 2 ? S_SILENT : S_RENDER;
    if (st->mode == S_RENDER) st->th_on = pthread_create(&st->th, NULL, sender, st) == 0;
    fprintf(stderr, "wyoming tts: streamed synthesis, %s\n", st->mode == S_OWN ? "this Echo's reply: said here" :
            st->mode == S_SILENT ? "this Echo's reply, said already: silence" : "made for Home Assistant");
    return st;
}

/* the end of the stream (STOPPED: synthesize-stop came; else the link is gone): the rest of the text, the answer */
static int stream_end(struct stream *st, int stopped)
{
    int rc = 0;
    stream_text(st, NULL);
    if (st->mode == S_OWN) core_reply_final();
    if (st->th_on) {
        pthread_mutex_lock(&st->m); st->closed = 1; pthread_cond_signal(&st->c); pthread_mutex_unlock(&st->m);
        pthread_join(st->th, NULL);
        rc = st->rc;
    }
    if (stopped && rc == 0) {
        if (st->mode != S_RENDER || !st->started) rc = silence(st->l);     /* nothing to say is 100 ms of nothing too */
        else rc = lw(st->l, "audio-stop", "{}", NULL, 0);
        if (rc == 0) rc = lw(st->l, "synthesize-stopped", NULL, NULL, 0);
    }
    fprintf(stderr, "wyoming tts: streamed synthesis ended, %d sentences\n", st->sentences);
    for (size_t i = st->qhead; i < st->qn; i++) free(st->q[i]);
    free(st->q);
    pthread_mutex_destroy(&st->m); pthread_cond_destroy(&st->c);
    free(st);
    return rc;
}

static void *conn(void *arg)
{
    struct link l = { (int)(intptr_t)arg, PTHREAD_MUTEX_INITIALIZER };
    struct wy_reader r; struct wy_event *ev = malloc(sizeof *ev);
    char *text = malloc(TEXT_MAX);
    struct stream *st = NULL;
    if (!ev || !text || wy_reader_init(&r, l.fd)) { free(ev); free(text); close(l.fd); return NULL; }
    while (wy_read(&r, ev) == 1) {
        int rc = 0;
        if (!strcmp(ev->type, "describe")) rc = send_info(&l);
        else if (!strcmp(ev->type, "synthesize-start")) {
            if (st) rc = stream_end(st, 0);                     /* a new one without the last one's stop */
            st = stream_start(&l);
            if (!st) rc = lw(&l, "error", "{\"text\":\"out of memory\",\"code\":\"failed\"}", NULL, 0);
        } else if (!strcmp(ev->type, "synthesize-chunk")) {
            if (st && !ev->data_dropped && !wytts_json_text(ev->json, "text", text, TEXT_MAX)) stream_text(st, text);
        } else if (!strcmp(ev->type, "synthesize-stop")) {
            if (st) { rc = stream_end(st, 1); st = NULL; }
            else rc = lw(&l, "synthesize-stopped", NULL, NULL, 0);
        } else if (!strcmp(ev->type, "synthesize")) {
            if (st) ;                                           /* the whole text beside its stream: made already */
            else if (ev->data_dropped || wytts_json_text(ev->json, "text", text, TEXT_MAX) || !*text)
                rc = lw(&l, "error", "{\"text\":\"no text\",\"code\":\"no_text\"}", NULL, 0);
            else rc = synthesize(&l, text);
        }
        if (rc < 0) break;
    }
    if (st) stream_end(st, 0);                                  /* the link went: a reply said here ends with what came */
    wy_reader_free(&r); free(ev); free(text);
    close(l.fd);
    return NULL;
}

static void *server(void *arg)
{
    int port = (int)(intptr_t)arg, ls = net_listen(port);
    if (ls < 0) { fprintf(stderr, "wyoming tts: cannot listen on %d\n", port); return NULL; }
    listen(ls, 4);                                              /* net_listen's backlog is 1: HA describes and speaks at once */
    fprintf(stderr, "wyoming tts: the Echo's voice for Home Assistant on %d\n", port);
    for (;;) {
        int c = net_accept(ls);
        if (c < 0) { usleep(100000); continue; }
        struct timeval tv = { 300, 0 };                         /* a link Home Assistant left open and forgot */
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        pthread_t t;
        if (pthread_create(&t, NULL, conn, (void *)(intptr_t)c)) close(c); else pthread_detach(t);
    }
    return NULL;
}

void wytts_start(int port)
{
    if (!speech_installed()) return;
    pthread_t t;
    if (pthread_create(&t, NULL, server, (void *)(intptr_t)port) == 0) pthread_detach(t);
}
