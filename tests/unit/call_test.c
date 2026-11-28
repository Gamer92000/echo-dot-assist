/* call.c on the PC (make unit-call): the claim (call.h), and the media without a codec (CALL_CODEC_PCM, a plugin's
 * call, docs/plugins.md 4.8): the frames sent are the gated, gain-corrected 20 ms frames in order; what comes in plays
 * by its sequence number, a missing frame is the last one faded and then silence, a frame of the wrong size is
 * dropped; nothing is sent after call_end.  The Voip stream and the core are stand-ins here: a stream that plays in
 * real time into memory, a core that is never muted. */
#include "call.h"
#include "audio.h"
#include "core.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (c) printf("ok   "); else { printf("FAIL "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)

static long long now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000000 + t.tv_nsec / 1000; }

/* ---- the stand-ins: the Voip stream plays at its rate into PLAYED; no call microphone; the core's three */
#define PLAYED_MAX (16000 * 10)
static int16_t played[PLAYED_MAX];
static atomic_int nplayed;
static long long voip_t0, voip_written_us;
static pthread_mutex_t vm = PTHREAD_MUTEX_INITIALIZER;
int voip_open(unsigned rate, unsigned channels) { (void)rate; (void)channels; pthread_mutex_lock(&vm); voip_t0 = now_us(); voip_written_us = 0; pthread_mutex_unlock(&vm); return 0; }
int voip_write(const void *data, size_t len)
{
    int n = atomic_load(&nplayed), k = (int)(len / 2);
    if (n + k <= PLAYED_MAX) { memcpy(played + n, data, len); atomic_store(&nplayed, n + k); }
    pthread_mutex_lock(&vm); voip_written_us += (long long)k * 1000000 / 16000; pthread_mutex_unlock(&vm);
    return 0;
}
long long voip_queued_us(void)
{
    pthread_mutex_lock(&vm); long long q = voip_written_us - (now_us() - voip_t0); pthread_mutex_unlock(&vm);
    return q > 0 ? q : 0;
}
void voip_close(void) {}
static void tone(int16_t *out, int n, double hz, double dbfs, long *phase);
/* The call microphone: none, but for H1 (hfp_on): micHfp as a Dot's call path gives it, in 50 ms blocks at its pace,
 * the far end already taken out by Amazon's canceller: this side's voice (a 500 Hz tone at -25 dBFS) while near_at()
 * says it talks, the room at -85 otherwise */
static atomic_int hfp_on;
static long long hfp_t0;                              /* H1's start (now_us) */
static int (*near_at)(long long ms), (*echo_at)(long long ms);
static long long hfp_next;
float callmic_echo_cap(void) { return -30; }
int callmic_present(void) { return atomic_load(&hfp_on); }
int callmic_open(void) { if (!atomic_load(&hfp_on)) return -1; hfp_next = now_us(); return 0; }
int callmic_read(const void **data)
{
    static int16_t b[800]; static long ph;
    if (!atomic_load(&hfp_on)) return -1;
    hfp_next += 50000;
    long long w = hfp_next - now_us(); if (w > 0) usleep((useconds_t)w);
    long long ms = (now_us() - hfp_t0) / 1000;
    if (near_at && near_at(ms)) tone(b, 800, 500, -25, &ph);
    else if (echo_at && echo_at(ms)) tone(b, 800, 300, -36, &ph);   /* what the canceller left of the far end: 28 dB under */
    else for (int i = 0; i < 800; i++) b[i] = (int16_t)(rand() % 3 - 1);
    *data = b;
    return (int)sizeof b;
}
void callmic_close(void) {}
void core_call_listen(int on) { (void)on; }
int core_own_sound(void) { return 0; }
int core_muted(void) { return 0; }

/* ---- what the media sends */
#define SENT_MAX 800
static struct { uint32_t seq; size_t len; int16_t pcm[CALL_FRAME]; } sent[SENT_MAX];
static atomic_int nsent;
static void on_send(const uint8_t *frame, size_t len, uint32_t seq, void *arg)
{
    int n = atomic_load(&nsent);
    (void)arg;
    if (n == SENT_MAX) return;
    sent[n].seq = seq; sent[n].len = len;
    if (len == CALL_PCM_BYTES) memcpy(sent[n].pcm, frame, len);
    atomic_store(&nsent, n + 1);
}

static double db(const int16_t *s, int n)
{
    double e = 0;
    for (int i = 0; i < n; i++) e += (double)s[i] * s[i];
    return 10 * log10(e / (n ? n : 1) / (32768.0 * 32768.0) + 1e-12);
}

static void tone(int16_t *out, int n, double hz, double dbfs, long *phase)
{
    double a = 32768 * pow(10, dbfs / 20) * sqrt(2);
    for (int i = 0; i < n; i++, (*phase)++) out[i] = (int16_t)(a * sin(2 * M_PI * hz * (double)*phase / 16000));
}

/* a frame of one value, and whether one such frame played */
static void fill(int16_t *f, int v) { for (int i = 0; i < CALL_FRAME; i++) f[i] = (int16_t)v; }
static int has(int v) { int n = atomic_load(&nplayed), c = 0; for (int i = 0; i < n; i++) c += played[i] == v; return c >= CALL_FRAME; }

/* H1's script: rounds of H1_ROUND_MS, the far end from their start for H1_FAR_MS, this side from H1_GAP_MS after that */
#define H1_ROUNDS 4
#define H1_ROUND_MS 2500
#define H1_FAR_MS 1000
#define H1_GAP_MS 300
#define H1_NEAR_MS 1000
static int h1_near(long long ms)
{
    long long t = ms % H1_ROUND_MS;
    return ms / H1_ROUND_MS < H1_ROUNDS && t >= H1_FAR_MS + H1_GAP_MS && t < H1_FAR_MS + H1_GAP_MS + H1_NEAR_MS;
}

static int h2_echo(long long ms)
{
    long long t = ms % H1_ROUND_MS;
    return ms / H1_ROUND_MS < 2 && t >= 100 && t < H1_FAR_MS + 300;
}

int main(void)
{
    /* ---- the claim */
    CHECK(call_claimed() == CALL_NOBODY, "nobody has the call at first");
    CHECK(call_claim(CALL_DROPIN) == 0 && call_claimed() == CALL_DROPIN, "Drop In claims it");
    CHECK(call_claim(CALL_PLUGIN + 1) == -1 && call_claimed() == CALL_DROPIN, "a plugin is refused while Drop In has it");
    CHECK(call_claim(CALL_DROPIN) == 0, "the one that has it, asking again: still its own");
    call_release(CALL_PLUGIN + 1);
    CHECK(call_claimed() == CALL_DROPIN, "one that has it not lets nothing go");
    CHECK(call_begin(CALL_PLUGIN + 1, CALL_CODEC_PCM, on_send, NULL) == -1, "no media for one without the claim");
    call_release(CALL_DROPIN);
    CHECK(call_claim(CALL_PLUGIN + 1) == 0 && call_claimed() == CALL_PLUGIN + 1, "let go: a plugin claims it");
    CHECK(call_claim(CALL_DROPIN) == -1, "and Drop In is refused the other way round");
    CHECK(call_begin(CALL_PLUGIN + 1, 7, on_send, NULL) == -1, "no media of a codec there is none of");

    /* ---- PCM out: the room's tone through the gate and the gain, frame by frame in order */
    CHECK(call_start() == 0, "the playout thread starts");
    int gen = call_begin(CALL_PLUGIN + 1, CALL_CODEC_PCM, on_send, NULL);
    CHECK(gen >= 0 && call_live(), "the plugin's media begins");
    { int16_t b[800]; long ph = 0;
      for (int k = 0; k < 40; k++) { tone(b, 800, 700, -35, &ph); call_mic(b, 800, 0); } }       /* 2 s in 50 ms blocks, as micAsr comes */
    int n = atomic_load(&nsent), inorder = 1, sizes = 1;
    for (int i = 0; i < n; i++) { inorder &= sent[i].seq == (uint32_t)i; sizes &= sent[i].len == CALL_PCM_BYTES; }
    CHECK(n == 100, "2 s of microphone: 100 frames of 20 ms sent (%d)", n);
    CHECK(inorder && sizes, "numbered from 0 in order, each %d bytes of s16le", CALL_PCM_BYTES);
    double out = db(sent[n - 1].pcm, CALL_FRAME);
    CHECK(out > -33 && out < -10, "the gain corrected: micAsr's -35 dBFS talker goes out louder (%.1f dBFS), as Drop In's does before Opus", out);

    /* ---- PCM in: frames by their number, a missing one faded, a wrong size dropped */
    int16_t frames[60][CALL_FRAME]; long ph2 = 0;
    for (int i = 0; i < 60; i++) tone(frames[i], CALL_FRAME, 440, -20, &ph2);
    struct call_stats st0; call_stats(&st0);
    for (int i = 0; i < 60; i++) {
        if (i == 30) continue;                         /* lost on the way */
        if (i == 20) call_rx(gen, 99999, (const uint8_t *)frames[i], 300);   /* not a frame of this media: dropped */
        call_rx(gen, (uint32_t)i, (const uint8_t *)frames[i], CALL_PCM_BYTES);
        if (i >= 6) usleep(20000);                      /* six ahead, then at the call's pace: 100 ms of slack for a busy PC */
    }
    usleep(600000);                                     /* what was ahead, and the concealment after it, played */
    struct call_stats st; call_stats(&st);
    CHECK(st.rx - st0.rx == 59, "59 frames taken, the one of the wrong size not (%u)", st.rx - st0.rx);
    /* find frame 0 in what played (the playout started with silence ahead of it), then compare */
    int np = atomic_load(&nplayed), at = -1;
    for (int i = 0; i + CALL_FRAME <= np && at < 0; i++) if (!memcmp(played + i, frames[0], sizeof frames[0])) at = i;
    CHECK(at >= 0 && at % CALL_FRAME == 0, "frame 0 plays as it came, sample for sample (at %d)", at);
    if (at >= 0 && at + 60 * CALL_FRAME <= np) {
        int same = 1;
        for (int i = 0; i < 30; i++) same &= !memcmp(played + at + i * CALL_FRAME, frames[i], sizeof frames[0]);
        CHECK(same, "frames 0 to 29 play in order, untouched");
        const int16_t *c = played + at + 30 * CALL_FRAME;
        double lc = db(c, CALL_FRAME), l29 = db(frames[29], CALL_FRAME);
        int faded = c[0] == frames[29][0] && abs(c[CALL_FRAME - 1]) <= abs(frames[29][CALL_FRAME - 1]) / 2 + 1;
        CHECK(memcmp(c, frames[31], sizeof frames[0]) && faded && lc < l29 - 1 && lc > l29 - 6,
              "frame 30, lost: the last one again, faded from its level to half (%.1f against %.1f dBFS)", lc, l29);
        int rest = 1;
        for (int i = 31; i < 60; i++) rest &= !memcmp(played + at + i * CALL_FRAME, frames[i], sizeof frames[0]);
        CHECK(rest, "frames 31 to 59 play as they came");
        int tail = at + 60 * CALL_FRAME, quiet = 1;
        /* nothing more came: concealment for PLC_FRAMES, then silence */
        if (tail + 10 * CALL_FRAME <= atomic_load(&nplayed)) {
            CHECK(db(played + tail, CALL_FRAME) < db(frames[59], CALL_FRAME) - 1, "after the last frame: concealment, faded");
            for (int i = tail + 6 * CALL_FRAME; i < tail + 10 * CALL_FRAME; i++) quiet &= played[i] == 0;
            CHECK(quiet, "and silence after it");
        } else CHECK(0, "the playout went on after the last frame (%d of %d)", atomic_load(&nplayed), tail + 10 * CALL_FRAME);
    } else CHECK(0, "all of it played (%d samples)", np);

    /* ---- the end: nothing more sent, nothing more taken */
    call_end(gen);
    int before = atomic_load(&nsent);
    { int16_t b[800]; long ph = 0; for (int k = 0; k < 10; k++) { tone(b, 800, 700, -35, &ph); call_mic(b, 800, 0); } }
    CHECK(atomic_load(&nsent) == before && !call_live(), "after call_end the microphone goes nowhere");
    call_rx(gen, 61, (const uint8_t *)frames[0], CALL_PCM_BYTES);
    call_stats(&st);
    CHECK(st.rx - st0.rx == 59, "and a frame of the call that ended is not taken");
    call_release(CALL_PLUGIN + 1);
    CHECK(call_claimed() == CALL_NOBODY, "the claim let go");

    /* ---- the jitter buffer's numbering (a review of fc43c40): frames of one value each, found again in what played */
    usleep(100000);
    { int16_t f[CALL_FRAME]; int g, after;
      /* W4: a frame JB (32) behind the newest has the newest's slot: dropped as far too old, not played in its place */
      atomic_store(&nplayed, 0); call_claim(CALL_PLUGIN); g = call_begin(CALL_PLUGIN, CALL_CODEC_PCM, on_send, NULL);
      for (int s = 0; s < 60; s++) {
          fill(f, 1000 + s); call_rx(g, (uint32_t)s, (const uint8_t *)f, CALL_PCM_BYTES);
          if (s == 45) { fill(f, 7777); call_rx(g, (uint32_t)(s - 32), (const uint8_t *)f, CALL_PCM_BYTES); }   /* frame 13 again, 640 ms late */
          if (s >= 8) usleep(19500);                      /* a burst first, then the call's pace */
      }
      usleep(400000);
      CHECK(has(1045) && !has(7777), "W4: a frame 32 behind the newest (its slot) is dropped: the newest plays, the late one not");
      call_end(g); call_release(CALL_PLUGIN); usleep(100000);
      /* W3: one stray number far ahead, then the stream as before: it plays on (the stray dropped) */
      atomic_store(&nplayed, 0); call_claim(CALL_PLUGIN); g = call_begin(CALL_PLUGIN, CALL_CODEC_PCM, on_send, NULL);
      for (int s = 0; s < 150; s++) {
          fill(f, 2000 + s); call_rx(g, (uint32_t)s, (const uint8_t *)f, CALL_PCM_BYTES);
          if (s == 50) { fill(f, 9999); call_rx(g, 50u + 1000000u, (const uint8_t *)f, CALL_PCM_BYTES); }
          if (s >= 8) usleep(19500);
      }
      usleep(400000);
      after = 0; for (int s = 52; s < 150; s++) after += has(2000 + s);
      CHECK(after >= 95 && !has(9999), "W3: a stray frame numbered 1000050 in a call at 50: dropped, the frames after it play (%d of 98)", after);
      call_end(g); call_release(CALL_PLUGIN); usleep(100000);
      /* W3: a numbering that starts again (16-bit RTP numbers wrap: 65535, then 0): the jitter buffer starts anew after
       * three frames of the new numbering, and the rest of the call plays */
      atomic_store(&nplayed, 0); call_claim(CALL_PLUGIN); g = call_begin(CALL_PLUGIN, CALL_CODEC_PCM, on_send, NULL);
      for (int s = 0; s < 200; s++) {
          uint32_t seq = s < 100 ? 65436u + (uint32_t)s : (uint32_t)(s - 100);
          fill(f, 3000 + s); call_rx(g, seq, (const uint8_t *)f, CALL_PCM_BYTES);
          if (s >= 8) usleep(19500);
      }
      usleep(400000);
      { int before = 0; after = 0; for (int s = 0; s < 100; s++) before += has(3000 + s); for (int s = 102; s < 200; s++) after += has(3000 + s);
        /* (what of the old numbering still waited in the buffer as it started anew is lost: ~100 ms) */
        CHECK(before >= 90 && after >= 95, "W3: numbered 65436..65535, then from 0 again: both play (%d of 100, then %d of the 98 after the first two)", before, after); }
      call_end(g); call_release(CALL_PLUGIN); usleep(100000);
      /* the same, numbered back by less (a plugin that numbers each call from 0 and restarted its stream at 100) */
      atomic_store(&nplayed, 0); call_claim(CALL_PLUGIN); g = call_begin(CALL_PLUGIN, CALL_CODEC_PCM, on_send, NULL);
      for (int s = 0; s < 200; s++) {
          fill(f, 4000 + s); call_rx(g, (uint32_t)(s < 100 ? s : s - 100), (const uint8_t *)f, CALL_PCM_BYTES);
          if (s >= 8) usleep(19500);
      }
      usleep(400000);
      after = 0; for (int s = 102; s < 200; s++) after += has(4000 + s);
      CHECK(after >= 95, "W3: numbered 0..99, then from 0 again: the second run plays (%d of 98)", after);
      call_end(g); call_release(CALL_PLUGIN); usleep(100000); }

    /* ---- D1 (a review of e8d52b9): Drop In's frames are never numbered anew.  A host on the LAN that sends frames of
     * the call again (they pass the AEAD: dropin.c drops them, this is call.c's own rule behind it) had three in a row
     * more than 64 behind start the buffer anew from them: the far side's own frames were then "far" and dropped, and
     * the Echo played its earlier words again */
    { int16_t f[CALL_FRAME]; int g, genuine = 0, twice = 0;
      atomic_store(&nplayed, 0); call_claim(CALL_DROPIN); g = call_begin(CALL_DROPIN, CALL_CODEC_PCM, on_send, NULL);
      for (int s = 0; s < 300; s++) {
          if (s == 150) for (int r = 10; r < 13; r++) { fill(f, 1000 + r); call_rx(g, (uint32_t)r, (const uint8_t *)f, CALL_PCM_BYTES); }
          if (s > 150) { int r = s - 138; fill(f, 1000 + r); call_rx(g, (uint32_t)r, (const uint8_t *)f, CALL_PCM_BYTES); }   /* one old frame ahead of each */
          fill(f, 1000 + s); call_rx(g, (uint32_t)s, (const uint8_t *)f, CALL_PCM_BYTES);
          if (s >= 8) usleep(19500);
      }
      usleep(400000);
      for (int s = 160; s < 300; s++) genuine += has(1000 + s);
      for (int s = 13; s < 150; s++) { int c = 0, n = atomic_load(&nplayed); for (int i = 0; i < n; i++) c += played[i] == 1000 + s; twice += c >= 2 * CALL_FRAME; }
      /* (the sender here runs 2.5 % fast: on a busy PC the playout may skip ahead once, ~10 frames) */
      CHECK(genuine >= 120 && twice == 0, "D1: Drop In's frames 130 behind, three in a row and on: dropped, the far side's own play (%d of 140; %d old ones again)", genuine, twice);
      call_end(g); call_release(CALL_DROPIN); usleep(100000); }

    /* ---- D2: a plugin that switches between two numberings every third frame: the buffer starts anew each time, and
     * says so once (then at most once in 10 s), and the count at the call's end */
    { int16_t f[CALL_FRAME]; int g, fd[2], saved, lines = 0, end_said = 0; char buf[65536]; ssize_t n, tot = 0;
      fflush(stderr);
      if (pipe(fd)) { CHECK(0, "a pipe"); return 1; }
      saved = dup(2); dup2(fd[1], 2);
      call_claim(CALL_PLUGIN); g = call_begin(CALL_PLUGIN, CALL_CODEC_PCM, on_send, NULL);
      for (int k = 0; k < 100; k++) for (int j = 0; j < 3; j++) { fill(f, 5); call_rx(g, ((k & 1) ? 1000000u : 0u) + (uint32_t)(k * 3 + j), (const uint8_t *)f, CALL_PCM_BYTES); }
      call_end(g); call_release(CALL_PLUGIN);
      fflush(stderr); dup2(saved, 2); close(saved); close(fd[1]);
      while ((n = read(fd[0], buf + tot, sizeof buf - 1 - (size_t)tot)) > 0) tot += n;
      close(fd[0]); buf[tot] = 0;
      for (char *l = buf; (l = strstr(l, "numbered anew (")); l++) lines++;
      end_said = strstr(buf, "numbered anew 99 times in this call") != NULL;
      CHECK(lines == 1 && end_said, "D2: 99 new numberings in a call: one line as it began, the count at the end (%d lines, %zd bytes)", lines, tot);
      usleep(100000); }

    /* ---- W5: what was left of a 20 ms frame of the room as one call ended does not start the next */
    { int16_t a[800], b2[320]; int g, neg = 0, n;
      for (int i = 0; i < 800; i++) a[i] = (int16_t)(i < 640 ? 2222 : -2222);
      for (int i = 0; i < 320; i++) b2[i] = 2222;
      call_claim(CALL_PLUGIN); g = call_begin(CALL_PLUGIN, CALL_CODEC_PCM, on_send, NULL);
      call_mic(a, 800, 0);                                   /* 50 ms of micAsr: two frames sent, 10 ms (negative) left */
      call_end(g); call_release(CALL_PLUGIN);                /* (the capture thread feeds call_mic only while a call is live) */
      call_claim(CALL_DROPIN); atomic_store(&nsent, 0); g = call_begin(CALL_DROPIN, CALL_CODEC_PCM, on_send, NULL);
      call_mic(b2, 320, 0);
      n = atomic_load(&nsent);
      for (int i = 0; n && i < CALL_FRAME; i++) neg += sent[0].pcm[i] < 0;
      CHECK(n == 1 && neg == 0, "W5: the next call's first frame is its own room only (%d frames, %d samples of the call before)", n, neg);
      call_end(g); call_release(CALL_DROPIN); }
    /* ---- H1 (issue #233): micHfp, a far end that plays loud (-8 dBFS, as a phone's on a Dot 5) and this side
     * answering 300 ms after each of its sentences.  micHfp's AGC puts this side near the playback's level, and the
     * echo estimate of micAsr's kind took each reply's first frames for echo, rose to its cap and kept every quick
     * reply ducked by SUPPRESS_HFP_DB (a user's traces: 7..36 % passed within 1 s of the far end).  Now: capped well
     * under what plays, and a reply passes from its third frame on */
    { int16_t f[CALL_FRAME]; long ph = 0; int g, talk = 0, full = 0, late = 0;
      atomic_store(&nsent, 0); atomic_store(&hfp_on, 1);
      near_at = h1_near;
      call_claim(CALL_PLUGIN); hfp_t0 = now_us(); g = call_begin(CALL_PLUGIN, CALL_CODEC_PCM, on_send, NULL);
      for (int r = 0; r < H1_ROUNDS; r++) {
          for (int k = 0; k < H1_FAR_MS / 20; k++) {         /* the far end's sentence, at the call's pace */
              tone(f, CALL_FRAME, 300, -8, &ph); call_rx(g, (uint32_t)(r * 1000 + k), (const uint8_t *)f, CALL_PCM_BYTES);
              long long w = hfp_t0 + ((long long)r * H1_ROUND_MS + (k + 1) * 20) * 1000 - now_us(); if (w > 0) usleep((useconds_t)w);
          }
          long long w = hfp_t0 + (long long)(r + 1) * H1_ROUND_MS * 1000 - now_us(); if (w > 0) usleep((useconds_t)w);
      }
      call_end(g); call_release(CALL_PLUGIN); atomic_store(&hfp_on, 0);
      /* the frames of this side's voice (the room and the far end's echo are 40 dB under), past the first round, whose
       * estimate starts empty: at full level (-25 dBFS) or ducked by 12 dB */
      int n = atomic_load(&nsent), seen = 0;
      for (int i = 0; i < n; i++) {
          double l = db(sent[i].pcm, CALL_FRAME);
          if (l < -45) continue;
          if (++seen <= H1_NEAR_MS / 20) continue;          /* the first reply */
          talk++; full += l > -27; late += l > -27 ? 0 : 1;
      }
      CHECK(talk > 100 && full >= talk * 85 / 100, "H1: micHfp, a loud far end, replies 300 ms after it: %d of %d frames of them at full level (%d ducked)", full, talk, late);
      usleep(100000);
      /* H2: no reply, but what the canceller left of the far end in micHfp (28 dB under its playback, from 100 ms after
       * it plays until 300 ms after it stopped): still not this side talking, ducked */
      int leaked = 0, echoed = 0;
      atomic_store(&nsent, 0); near_at = NULL; echo_at = h2_echo; atomic_store(&hfp_on, 1);
      call_claim(CALL_PLUGIN); hfp_t0 = now_us(); g = call_begin(CALL_PLUGIN, CALL_CODEC_PCM, on_send, NULL);
      for (int r = 0; r < 2; r++) {
          for (int k = 0; k < H1_FAR_MS / 20; k++) {
              tone(f, CALL_FRAME, 300, -8, &ph); call_rx(g, (uint32_t)(r * 1000 + k), (const uint8_t *)f, CALL_PCM_BYTES);
              long long w = hfp_t0 + ((long long)r * H1_ROUND_MS + (k + 1) * 20) * 1000 - now_us(); if (w > 0) usleep((useconds_t)w);
          }
          long long w = hfp_t0 + (long long)(r + 1) * H1_ROUND_MS * 1000 - now_us(); if (w > 0) usleep((useconds_t)w);
      }
      call_end(g); call_release(CALL_PLUGIN); atomic_store(&hfp_on, 0); echo_at = NULL;
      n = atomic_load(&nsent);
      for (int i = 0; i < n; i++) { double l = db(sent[i].pcm, CALL_FRAME); echoed += l > -60; leaked += l > -40; }
      CHECK(echoed > 50 && leaked <= echoed / 10, "H2: an echo left in micHfp 28 dB under the far end stays ducked (%d of %d frames at its level)", leaked, echoed);
      usleep(100000); }

    printf("call: %s\n", fails ? "FAILED" : "all good");
    return fails != 0;
}
