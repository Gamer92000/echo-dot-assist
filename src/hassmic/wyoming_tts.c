/* Home Assistant's text to speech made on the Echo (wyoming_tts.h): a Wyoming TTS server on the Echo's own voice
 * (speech.h).  Home Assistant's Wyoming integration finds it by mDNS (_wyoming._tcp, in the service file
 * proto_esphome.c writes) or by address and port; its voice assistant then names it as its text-to-speech engine, so
 * the computer running Home Assistant makes no speech, and the voice is there for every speaker Home Assistant has
 * (tts.speak, announcements).
 *
 * The protocol (wyoming 1.5, as Home Assistant's integration speaks it): "describe" -> "info" with one TTS program and
 * its voices; "synthesize" {text, voice} -> "audio-start" {rate, width, channels}, "audio-chunk"s, "audio-stop".  Not
 * offered: synthesize-start/-chunk/-stop (supports_synthesize_streaming false), so Home Assistant sends whole texts.
 * The audio goes out as the engine makes it (~6x real time on the Show 5), not after the whole sentence. */
#include "wyoming_tts.h"
#include "speech.h"
#include "wyoming.h"
#include "netio.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CHUNK 4096                  /* bytes per audio-chunk: ~93 ms at 22 050 Hz */
#define TEXT_MAX 4096

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
            case 'n': b[0] = ' '; break;                         /* a line break is a pause to the engine anyway */
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

static int send_info(int fd)
{
    char d[1400], v[64], l[32];
    const char *voice = speech_voice(), *lang = speech_language();
    snprintf(v, sizeof v, "%s", *voice ? voice : "none");
    snprintf(l, sizeof l, "%s", *lang ? lang : "en-US");
    for (char *s = v; *s; s++) if (*s == '"' || *s == '\\') *s = '_';
    const char *attr = "{\"name\":\"Amazon (Ivona), on the Echo\",\"url\":\"https://github.com/Nt-f/echo-dot-assist\"}";
    snprintf(d, sizeof d,
             "{\"asr\":[],\"tts\":[{\"name\":\"hassmic\",\"attribution\":%s,\"installed\":true,\"description\":\"The Echo's own voice\","
             "\"version\":\"" VERSION "\",\"supports_synthesize_streaming\":false,\"voices\":[{\"name\":\"%s\",\"attribution\":%s,"
             "\"installed\":true,\"description\":\"%s\",\"version\":null,\"languages\":[\"%s\"],\"speakers\":null}]}],"
             "\"handle\":[],\"intent\":[],\"wake\":[],\"mic\":[],\"snd\":[]}",
             attr, v, attr, v, l);
    return wy_write(fd, "info", d, NULL, 0);
}

static int synthesize(int fd, const char *text)
{
    struct job *j = calloc(1, sizeof *j);
    if (!j) return -1;
    pthread_mutex_init(&j->m, NULL); pthread_cond_init(&j->c, NULL);
    j->sink = (struct speech_sink){ j_begin, j_data, j_end, j };
    if (speech_render(text, &j->sink)) {
        free(j);
        return wy_write(fd, "error", "{\"text\":\"the Echo's voice is not ready or busy\",\"code\":\"busy\"}", NULL, 0);
    }
    size_t sent = 0; int started = 0, rc = 0, ok;
    unsigned char chunk[CHUNK]; char hd[96];
    for (;;) {
        pthread_mutex_lock(&j->m);
        while (!j->done && !(j->begun && !started) && j->len - sent < CHUNK) pthread_cond_wait(&j->c, &j->m);
        size_t n = j->len - sent; if (n > CHUNK) n = CHUNK;
        if (n) memcpy(chunk, j->buf + sent, n);
        int done = j->done && j->len - sent <= n, begun = j->begun; unsigned rate = j->rate;
        ok = j->ok;
        pthread_mutex_unlock(&j->m);
        if (begun && !started && rc == 0) {
            snprintf(hd, sizeof hd, "{\"rate\":%u,\"width\":2,\"channels\":1}", rate);
            rc = wy_write(fd, "audio-start", hd, NULL, 0); started = 1;
        }
        if (n && rc == 0) rc = wy_write(fd, "audio-chunk", hd, chunk, n);
        sent += n;
        if (done) break;                                         /* rc < 0 (Home Assistant left): read on to the end */
    }
    if (rc == 0) {
        if (started) rc = wy_write(fd, "audio-stop", "{}", NULL, 0);
        if (!ok && rc == 0) rc = wy_write(fd, "error", "{\"text\":\"the Echo's voice failed on this text\",\"code\":\"failed\"}", NULL, 0);
    }
    fprintf(stderr, "wyoming tts: \"%.60s%s\" %s, %zu bytes\n", text, strlen(text) > 60 ? "..." : "", ok ? "made" : "failed", sent);
    pthread_mutex_destroy(&j->m); pthread_cond_destroy(&j->c);
    free(j->buf); free(j);
    return rc;
}

static void *conn(void *arg)
{
    int fd = (int)(intptr_t)arg;
    struct wy_reader r; struct wy_event *ev = malloc(sizeof *ev);
    char *text = malloc(TEXT_MAX);
    if (!ev || !text || wy_reader_init(&r, fd)) { free(ev); free(text); close(fd); return NULL; }
    while (wy_read(&r, ev) == 1) {
        int rc = 0;
        if (!strcmp(ev->type, "describe")) rc = send_info(fd);
        else if (!strcmp(ev->type, "synthesize")) {
            if (wytts_json_text(ev->json, "text", text, TEXT_MAX) || !*text)
                rc = wy_write(fd, "error", "{\"text\":\"no text\",\"code\":\"no_text\"}", NULL, 0);
            else rc = synthesize(fd, text);
        }
        if (rc < 0) break;
    }
    wy_reader_free(&r); free(ev); free(text);
    close(fd);
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
