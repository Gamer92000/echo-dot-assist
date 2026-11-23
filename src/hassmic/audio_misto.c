/* EchoRoot adapter: preserve Amazon's audio DSP; subprocess boundary avoids
 * linking glibc/PulseAudio stock libraries into the static Linux executable.
 * Native audiod owns the speaker volume; PulseAudio's sink stays at 100%.
 * No BT routing or AFE control. */
#define _GNU_SOURCE
#include "audio.h"
#include "misto_lipc.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int capfd = -1;
static pid_t cappid;
static char capdir[128], fifo[160];
static short pcm[800];
static size_t cap_used;
void (*audio_earcon_started)(unsigned lead_ms);
static void stop_child(pid_t pid) {
    if (pid > 0) { kill(pid, SIGTERM); while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {} }
}
void cap_close(void) {
    if (capfd >= 0) close(capfd);
    capfd = -1; cap_used = 0; stop_child(cappid); cappid = 0;
    if (*fifo) unlink(fifo);
    if (*capdir) rmdir(capdir);
    *fifo = *capdir = 0;
}
int cap_open(void) {
    const char *tool = getenv("MISTO_SHMBUF");
    if (!tool) tool = "/usr/local/bin/shmbuf_tool";
    snprintf(capdir, sizeof capdir, "/tmp/hassmic-mic-XXXXXX");
    if (!mkdtemp(capdir)) return -1;
    snprintf(fifo, sizeof fifo, "%s/audio", capdir);
    if (mkfifo(fifo, 0600)) { cap_close(); return -1; }
    /* RDWR prevents startup EOF and blocking open. cap_read checks child health. */
    capfd = open(fifo, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (capfd < 0) { cap_close(); return -1; }
    cappid = fork();
    if (!cappid) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() == 1) _exit(127);
        dup2(STDERR_FILENO, STDOUT_FILENO);
        execl(tool, tool, "-m", "2", "-s", "1", "-S", "BMicsOut.shm", "-o", fifo, (char *)NULL);
        _exit(127);
    }
    if (cappid < 0) { cappid = 0; cap_close(); return -1; }
    return 0;
}
int cap_read(const void **data) {
    static unsigned char buf[sizeof pcm];

    struct pollfd p = { .fd = capfd, .events = POLLIN };
    int r = poll(&p, 1, 500);
    if (r < 0) return errno == EINTR ? 0 : -1;
    if (!r) return kill(cappid, 0) < 0 ? -1 : 0;
    ssize_t n = read(capfd, buf + cap_used, sizeof buf - cap_used);
    if (n < 0) return errno == EAGAIN || errno == EINTR ? 0 : -1;
    if (!n) return -1;
    cap_used += (size_t)n;
    if (cap_used < sizeof buf) return 0;
    memcpy(pcm, buf, sizeof pcm); cap_used = 0; *data = pcm;
    return sizeof pcm;
}
struct stream { int fd, done; pid_t pid; unsigned rate, channels; };
static struct stream tts = { .fd = -1, .done = -1 }, music = { .fd = -1, .done = -1 }, call = { .fd = -1, .done = -1 };
static float tts_gain = 1, music_gain = 1, call_gain = 1;
static void s_close(struct stream *s, int drain) {
    if (s->fd >= 0) close(s->fd);
    s->fd = -1;
    if (s->done >= 0) {
        struct pollfd p = { .fd = s->done, .events = POLLIN };
        int alive = poll(&p, 1, 0) == 0;
        if (alive && !drain && s->pid > 0) kill(s->pid, SIGTERM);
        int r;
        do { r = poll(&p, 1, 10000); } while (r < 0 && errno == EINTR);
        if (!r && s->pid > 0) { kill(s->pid, SIGTERM); fprintf(stderr, "misto: playback drain timed out\n"); }
        close(s->done); s->done = -1;
    }
    if (s->pid > 0) while (waitpid(s->pid, NULL, 0) < 0 && errno == EINTR) {}
    s->pid = 0;
}
static int s_open(struct stream *s, unsigned rate, unsigned channels) {
    int f[2], done[2]; char r[32], c[32];
    const char *tool = getenv("MISTO_PAPLAY");
    if (!tool) tool = "/usr/bin/paplay";
    if (!rate || !channels || channels > 2) return -1;
    s_close(s, 0);
    snprintf(r, sizeof r, "--rate=%u", rate);
    snprintf(c, sizeof c, "--channels=%u", channels);
    if (pipe2(f, O_CLOEXEC)) return -1;
    if (pipe2(done, O_CLOEXEC)) { close(f[0]); close(f[1]); return -1; }
    s->pid = fork();
    if (!s->pid) {
        close(done[0]);
        fcntl(done[1], F_SETFD, 0); /* EOF when paplay exits, even with SIGCHLD ignored */
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() == 1) _exit(127);
        dup2(f[0], STDIN_FILENO); close(f[0]); close(f[1]);
        execl(tool, tool, "--raw", "--format=s16le", "--latency-msec=100", r, c, (char *)NULL);
        _exit(127);
    }
    close(f[0]); close(done[1]);
    if (s->pid < 0) { close(f[1]); close(done[0]); s->pid = 0; return -1; }
    s->done = done[0];
    s->fd = f[1]; s->rate = rate; s->channels = channels;
    return 0;
}
static int s_write(struct stream *s, const void *data, size_t n, float gain) {
    if (s->fd < 0 || n % 2) return -1;
    unsigned char buf[4096];
    while (n) {
        size_t z = n < sizeof buf ? n : sizeof buf;
        memcpy(buf, data, z);
        if (gain != 1) for (size_t i = 0; i < z; i += 2) {
            short v; memcpy(&v, buf+i, 2);
            float scaled = v * gain;
            v = (short)lrintf(fmaxf(-32768, fminf(32767, scaled)));
            memcpy(buf+i, &v, 2);
        }
        size_t off = 0;
        while (off < z) {
            struct pollfd p = { .fd = s->fd, .events = POLLOUT };
            int r = poll(&p, 1, 5000);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0 || (p.revents & (POLLHUP|POLLERR|POLLNVAL))) return -1;
            ssize_t w = write(s->fd, buf+off, z-off);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) return -1;
            off += w;
        }
        data = (const char *)data+z; n -= z;
    }
    return 0;
}
int play_open(unsigned r, unsigned c) { return s_open(&tts, r, c); }
int play_write(const void *p, size_t n) { return s_write(&tts, p, n, tts_gain); }
void play_close(int drain) { s_close(&tts, drain); }
void audio_tts_gain(float g) { tts_gain = g; }
/* paplay has no latency query on its stdin. Keep an estimate of submitted PCM
 * versus monotonic playback time, so Sendspin does not repeatedly pad every
 * packet as if the output queue were empty. This is not a DAC measurement. */
static long long music_end_us;
static long long audio_now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec*1000000LL+t.tv_nsec/1000; }
int music_open(unsigned r, unsigned c) { music_end_us=0; return s_open(&music, r, c); }
int music_write(const void *p, size_t n) {
    long long at=audio_now_us();
    if (music_end_us<at) music_end_us=at;
    if (music.rate && music.channels) music_end_us+=(long long)(n/(2*music.channels))*1000000/music.rate;
    int rc=s_write(&music,p,n,music_gain);
    if (rc) music_end_us=0;
    return rc;
}
void music_close(void) { s_close(&music, 1); music_end_us=0; }
long long music_queued_us(void) { long long q=music_end_us-audio_now_us(); return q>0?q:0; }
long long music_prime_us(void) { return 0; }
void audio_music_gain(float g) { music_gain = g; }
/* Pace only Drop In: without this the playout loop fills the pipe and paplay's
 * buffers with silence before incoming packets arrive. This estimates submitted
 * audio, not PulseAudio or the hardware's actual latency. */
static long long call_end_us;
int voip_open(unsigned r, unsigned c) { call_end_us=0; return s_open(&call, r, c); }
int voip_write(const void *p, size_t n) {
    long long at=audio_now_us();
    if (call_end_us<at) call_end_us=at;
    if (call.rate && call.channels) call_end_us+=(long long)(n/(2*call.channels))*1000000/call.rate;
    int rc=s_write(&call,p,n,call_gain);
    if (rc) call_end_us=0;
    return rc;
}
void voip_close(void) { s_close(&call, 0); call_end_us=0; }
long long voip_queued_us(void) { long long q=call_end_us-audio_now_us(); return q>0?q:0; }
void audio_call_gain(float g) { call_gain = g; }
int bt_open(unsigned r, unsigned c) { (void)r; (void)c; return -1; }
int bt_write(const void *p, size_t n) { (void)p; (void)n; return -1; }
void bt_close(void) {}
long long bt_queued_us(void) { return 0; }
int audio_a2dp_route(int on, uint64_t a, const char *n) { (void)on;(void)a;(void)n;return -1; }
int audio_a2dp_lost(uint64_t a) { (void)a; return -1; }
void audio_a2dp_reset(void) {}
int audio_a2dp_aipc(void) { return 0; }
/* EchoRoot has one shared MainVolume (0..100), not the Dots' TTSVolume.
 * Its rawVolume is a different internal control and must not be driven here.
 * VOL_MUTE is speaker mute, not buttond's physical microphone latch. */
int vol_read(enum vol which, int fallback) {
    int v;
    if (which != VOL_MAIN) return fallback;
    return !misto_lipc_int("com.doppler.audiod", "MainVolume", NULL, &v) && v >= 0 && v <= 100 ? v : fallback;
}
void vol_write(enum vol which, int v) {
    if (which != VOL_MAIN) return;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    if (!misto_lipc_int("com.doppler.audiod", "MainVolume", &v, NULL))
        fprintf(stderr, "misto: MainVolume=%d\n", v);
}
void afe_listening(int on) { (void)on; }
void afe_stream_stopped(void) {}
int afe_arbitration(long a,long b,char *j,size_t n) { (void)a;(void)b; if(n) *j=0;return 0; }
int afe_eq_get(char *j,size_t n) { if(n) *j=0;return 0; }
void afe_eq_set(const char *j) { (void)j; }
void play_earcon(const short *p,size_t n,unsigned r) {
    struct stream s = { .fd = -1, .done = -1 };
    if (s_open(&s,r,1)) return;
    if (audio_earcon_started) audio_earcon_started(0);
    s_write(&s,p,n*2,1); s_close(&s,1);
}
void play_earcon_while(const short *p,size_t n,unsigned r,int (*go)(void)) {
    struct stream s = { .fd = -1, .done = -1 };
    if (s_open(&s,r,1)) return;
    while (n && go()) { size_t z=n<r/50?n:r/50; if(s_write(&s,p,z*2,1)) break; p+=z;n-=z; }
    s_close(&s,n==0);
}
