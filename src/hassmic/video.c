/* The other side's picture on this screen: see video.h.
 *
 * One writer thread: records wait in a queue (QMAX records, QBYTES bytes; a key frame of 720p is some 60 KB) and go to
 * the player's stdin in order.  The player is started by the first key frame or codec config of a call (deltas before
 * it are useless to a decoder), and goes with video_stop: its stdin closed (cam.c hides the picture and exits), killed
 * if it is still there after STOP_MS.  A player that died mid-call (its write failed) is started again at the next key
 * frame, at most once every RESTART_MS.  Behind (the queue full): what waits is dropped and the queue takes only a key
 * frame again, so the picture skips ahead rather than falls behind the call's sound.
 *
 * Two channels, each its own queue, writer and player: the other side's picture (full screen) and this Show's own while
 * it sends one (self-view: small in a corner, mirrored, as stock's; decoded from the very records that leave, so it
 * shows what the other side gets).  A frosted start (video_blur): the caller's screen shows the picture of an Echo that
 * did not ring blurred for its first seconds (cam.c's HMCAM_BLUR_MS), as stock's Drop In: whoever is dropped in on has
 * a moment before they are seen.  A player started again mid-call gets what is left of it.
 *
 * The picture must never outlive its call (a frozen face on someone's Show): video_stop for every end of a call
 * (dropin.c's end_call) kills the player at once, whatever the writer is doing; a new call's records end the old one's
 * player first, and cam.c ends by itself after 3 s without frames (PLAY_IDLE_MS). */
#include "video.h"
#include "board.h"
#include "camera.h"
#include "screen.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define QMAX       64
#define QBYTES     (2 << 20)
#define STOP_MS    1500
#define RESTART_MS 3000

struct rec { int type; long long pts; uint8_t *d; size_t n; };
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
struct chan {
    int self;                                   /* 0 the other side's picture, 1 this Show's own */
    pthread_cond_t cv;
    struct rec q[QMAX]; unsigned qh, qt; size_t qbytes;
    int cur_gen, need_key, stop_req, started;
    /* The player's pid, for video_stop to kill it itself: a player that stops reading without dying (a decoder hung
     * in the driver) would hold the writer in write() on a full pipe, where it never sees stop_req, and the picture
     * would stay on the screen after the call.  Killed, its pipe's read end goes, the write fails, and the writer
     * reaps it */
    pid_t player;
    long long last_start;
    int shown_n;
    int blur_gen, blur_ms; long long blur_t0;   /* video_blur's: call blur_gen frosted for blur_ms from its first player */
};
static struct chan ch[2] = {
    { .self = 0, .cv = PTHREAD_COND_INITIALIZER, .cur_gen = -1, .need_key = 1, .player = -1, .blur_gen = -1 },
    { .self = 1, .cv = PTHREAD_COND_INITIALIZER, .cur_gen = -1, .need_key = 1, .player = -1, .blur_gen = -1 },
};

static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }

int video_present(void)
{
    if (getenv("HASSMIC_FAKE_VIDEO")) return 1;
    return camera_present() && !getenv("HASSMIC_FAKE_CAMERA") && screen_present();
}

int video_shown(void) { pthread_mutex_lock(&lk); int n = ch[0].shown_n; pthread_mutex_unlock(&lk); return n; }
int video_self_shown(void) { pthread_mutex_lock(&lk); int n = ch[1].shown_n; pthread_mutex_unlock(&lk); return n; }

static void drop_all(struct chan *c)            /* lk held */
{
    while (c->qt != c->qh) { free(c->q[c->qt % QMAX].d); c->qt++; }
    c->qbytes = 0;
}

static void *writer(void *arg);

static void put(struct chan *c, int gen, int type, long long pts_us, const uint8_t *data, size_t len)
{
    if (!video_present() || !len || len > QBYTES) return;
    pthread_mutex_lock(&lk);
    if (!c->started) { pthread_t t; if (!pthread_create(&t, NULL, writer, c)) { pthread_detach(t); c->started = 1; } }
    if (gen != c->cur_gen) { drop_all(c); c->cur_gen = gen; c->need_key = 1; c->stop_req = 1; if (c->player > 0) kill(c->player, SIGKILL); }   /* the last call's player goes */
    if (type == CAM_FRAME && c->need_key) { pthread_mutex_unlock(&lk); return; }
    if (c->qh - c->qt >= QMAX || c->qbytes + len > QBYTES) {                          /* behind: skip to a key frame */
        drop_all(c); c->need_key = 1;
        if (type == CAM_FRAME) { pthread_mutex_unlock(&lk); return; }
    }
    uint8_t *d = malloc(len);
    if (d) {
        memcpy(d, data, len);
        c->q[c->qh % QMAX] = (struct rec){ type, pts_us, d, len }; c->qh++; c->qbytes += len;
        if (type != CAM_FRAME) c->need_key = 0;
        pthread_cond_signal(&c->cv);
    }
    pthread_mutex_unlock(&lk);
}

void video_show(int gen, int type, long long pts_us, const uint8_t *data, size_t len) { put(&ch[0], gen, type, pts_us, data, len); }
void video_self(int gen, int type, long long pts_us, const uint8_t *data, size_t len) { put(&ch[1], gen, type, pts_us, data, len); }

void video_blur(int gen, int ms)
{
    pthread_mutex_lock(&lk); ch[0].blur_gen = gen; ch[0].blur_ms = ms; ch[0].blur_t0 = 0; pthread_mutex_unlock(&lk);
}

static void stop_chan(struct chan *c, int gen)  /* lk held */
{
    if (gen != c->cur_gen) return;
    drop_all(c); c->need_key = 1; c->stop_req = 1; c->cur_gen = -1;
    if (c->player > 0) kill(c->player, SIGKILL);
    pthread_cond_signal(&c->cv);
}

void video_stop(int gen)
{
    pthread_mutex_lock(&lk); stop_chan(&ch[0], gen); stop_chan(&ch[1], gen); pthread_mutex_unlock(&lk);
}

void video_self_stop(int gen)
{
    pthread_mutex_lock(&lk); stop_chan(&ch[1], gen); pthread_mutex_unlock(&lk);
}

static int write_all(int fd, const void *p, size_t n)
{
    const uint8_t *c = p;
    while (n) { ssize_t w = write(fd, c, n); if (w < 0 && errno == EINTR) continue; if (w <= 0) return -1; c += w; n -= (size_t)w; }
    return 0;
}

static void end_player(struct chan *c, pid_t *pid, int *fd)
{
    if (*fd >= 0) { close(*fd); *fd = -1; }
    if (*pid <= 0) return;
    long long t0 = now_ms(); int st;
    while (waitpid(*pid, &st, WNOHANG) == 0) {
        if (now_ms() - t0 > STOP_MS) { kill(*pid, SIGKILL); waitpid(*pid, &st, 0); break; }
        usleep(20000);
    }
    fprintf(stderr, c->self ? "video: this Show's own picture ended\n" : "video: the other side's picture ended\n");
    pthread_mutex_lock(&lk); if (c->player == *pid) c->player = -1; pthread_mutex_unlock(&lk);
    *pid = -1;
}

static void *writer(void *arg)
{
    struct chan *c = arg; pid_t pid = -1; int fd = -1;
    signal(SIGPIPE, SIG_IGN);                   /* a player that died: write() fails, it is started again */
    for (;;) {
        pthread_mutex_lock(&lk);
        while (c->qt == c->qh && !c->stop_req) pthread_cond_wait(&c->cv, &lk);
        int stop = c->stop_req; c->stop_req = 0;
        struct rec r = { 0 };
        if (!stop && c->qt != c->qh) { r = c->q[c->qt % QMAX]; c->qt++; c->qbytes -= r.n; }
        pthread_mutex_unlock(&lk);
        if (stop) { end_player(c, &pid, &fd); if (!r.d) continue; }
        if (pid < 0 && r.type != CAM_FRAME && now_ms() - c->last_start >= RESTART_MS) {
            c->last_start = now_ms();
            pthread_mutex_lock(&lk);
            int blur = 0;
            if (!c->self && c->blur_gen == c->cur_gen && c->blur_ms > 0) {      /* frosted: what is left of it */
                if (!c->blur_t0) c->blur_t0 = c->last_start;
                long long left = c->blur_ms - (c->last_start - c->blur_t0);
                blur = left > 0 ? (int)left : 0;
            }
            pthread_mutex_unlock(&lk);
            if ((pid = camera_player(&fd, c->self, blur)) < 0) fprintf(stderr, "video: no player for %s\n", c->self ? "this Show's own picture" : "the other side's picture");
            else {
                if (c->self) fprintf(stderr, "video: this Show's own picture in the corner\n");
                else if (blur) fprintf(stderr, "video: the other side's picture on the screen, frosted for %d ms\n", blur);
                else fprintf(stderr, "video: the other side's picture on the screen\n");
                pthread_mutex_lock(&lk);
                c->player = pid;
                int gone = c->stop_req;                 /* stopped while it started: the kill above found no pid */
                pthread_mutex_unlock(&lk);
                if (gone) kill(pid, SIGKILL);
            }
        }
        if (pid > 0 && fd >= 0) {
            uint8_t h[20] = { 'H', 'M', 'C', 'V', (uint8_t)r.type };
            for (int i = 0; i < 4; i++) h[8 + i] = (uint8_t)(r.n >> (8 * i));
            for (int i = 0; i < 8; i++) h[12 + i] = (uint8_t)((unsigned long long)r.pts >> (8 * i));
            if (write_all(fd, h, sizeof h) || write_all(fd, r.d, r.n)) {
                fprintf(stderr, "video: the player went\n");
                end_player(c, &pid, &fd);
                pthread_mutex_lock(&lk); c->need_key = 1; pthread_mutex_unlock(&lk);
            } else { pthread_mutex_lock(&lk); c->shown_n++; pthread_mutex_unlock(&lk); }
        }
        free(r.d);
    }
    return NULL;
}
