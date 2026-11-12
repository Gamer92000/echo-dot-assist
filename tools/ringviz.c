/* What the live ring's music effects make of a track, without an Echo: the PCM (s16le stereo 48 kHz, tools/ringmusic.py
 * makes one) through musiclight.c on a clock of its own, as an output plays it, and each music effect drawn 30 times a
 * second as ringlive.c draws it.
 *   build/ringviz track.raw out-dir      writes out-dir/features.csv (every 10 ms: what musiclight heard) and
 *                                        out-dir/<effect>.f32 (per frame 12 LEDs x RGB as seen, float32)
 * tools/ringviz.py turns them into pictures (a row a frame, the ring cut open at LED 1) and checks the drums and the
 * tempo against the track's ground truth. */
#define _GNU_SOURCE
#include "ringlive.h"
#include "musiclight.h"
#include "ring.h"
#include "board.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* what ringlive.c and ringdesign.c want of ring.c and the board, here stand-ins */
const struct board board = { .volume_steps = 30 };
const struct ring_slot ring_slots[] = { { "listen", "active-start", RING_LISTEN } };
const int ring_nslots = 1;
const char *const ring_effects[] = { "None", "Pulse", "Spin", "Blink", "Rainbow", RING_LIVE_EFFECTS };
const int ring_neffects = 5 + RING_LIVE_NEFFECTS;
int ring_leds(void) { return 12; }
int ring_rotation(int set) { (void)set; return 0; }
int ring_check(const char *a, size_t n, char *e, size_t c) { (void)a; (void)n; (void)e; (void)c; return 0; }
int ring_slot_available(int i) { (void)i; return 1; }
int ring_slot_live(int i) { (void)i; return 0; }
int ring_designs_live(void) { return 0; }
char *ring_stock(int i, size_t *n) { (void)i; (void)n; return NULL; }
void ring_reload(void) {}
int ring_band_rows(int set) { (void)set; return -1; }
int ring_custom(void) { return 1; }
int ring_light_now(struct ring_light *l) { memset(l, 0, sizeof *l); return 0; }
void ring_light_live(int on) { (void)on; }

static long long sim, t0;
static FILE *dbg;
static void debug(long long heard, const float *db, const float *rise)
{
    fprintf(dbg, "%.3f", (heard - t0) / 1000.0);
    for (int b = 0; b < MUSIC_BANDS; b++) fprintf(dbg, ",%.2f", db[b]);
    for (int b = 0; b < MUSIC_BANDS; b++) fprintf(dbg, ",%.2f", rise[b]);
    fprintf(dbg, "\n");
}
static long long sim_now(void) { return sim; }

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: ringviz track.raw out-dir [r g b]\n"); return 2; }
    FILE *in = fopen(argv[1], "rb");
    if (!in) { perror(argv[1]); return 1; }
    float rgb[3] = { 0.1f, 0.45f, 1 };
    if (argc >= 6) for (int i = 0; i < 3; i++) rgb[i] = (float)atof(argv[3 + i]);
    char p[512]; FILE *out[RING_LIVE_NEFFECTS]; int ne = 0, first = 0;
    for (int e = 0; e < 5 + RING_LIVE_NEFFECTS; e++) if (RING_E_IS_MUSIC(e)) {
        if (!ne) first = e;
        snprintf(p, sizeof p, "%s/%s.f32", argv[2], ring_effects[e]);
        for (char *c = strrchr(p, '/') + 1; *c; c++) if (*c == ' ') *c = '_';
        out[ne++] = fopen(p, "wb");
    }
    snprintf(p, sizeof p, "%s/features.csv", argv[2]);
    FILE *csv = fopen(p, "w");
    fprintf(csv, "t,active,level,left,right,energy,bright,bpm,confidence,beat_at,beat_no,kick,snare,hat");
    for (int b = 0; b < MUSIC_BANDS; b++) fprintf(csv, ",b%d", b);
    fprintf(csv, "\n");
    musiclight_clock(sim_now);
    enum { RATE = 48000, CHUNK = 960 };                 /* 20 ms a write, as Sendspin's */
    int16_t pcm[CHUNK * 2]; size_t n;
    /* one pass for what was heard, then one per effect: an effect keeps state from frame to frame (one ring shows one) */
    for (int pass = -1; pass < ne; pass++) {
        rewind(in); musiclight_reset(); musiclight_want(1);
        long long next_frame = 0, next_row = 0;
        sim = 1000000; t0 = sim;
        if (pass < 0) { snprintf(p, sizeof p, "%s/bands.csv", argv[2]); dbg = fopen(p, "w"); musiclight_debug(debug); }
        while ((n = fread(pcm, 4, CHUNK, in)) > 0) {
            musiclight_feed(pcm, n, 2, RATE, 0);
            long long end = sim + (long long)n * 1000 / RATE;
            for (; pass < 0 && t0 + next_row <= end; next_row += 10) {
                struct music_feat f; musiclight_now(t0 + next_row, &f);
                fprintf(csv, "%.3f,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.1f,%.2f,%.3f,%d,%.3f,%.3f,%.3f", next_row / 1000.0, f.active, f.level, f.left, f.right,
                        f.energy, f.bright, f.bpm, f.confidence, f.beat_at ? (f.beat_at - t0) / 1000.0 : -1, f.beat_no,
                        f.kick.at[0] ? (f.kick.at[0] - t0) / 1000.0 : -1, f.snare.at[0] ? (f.snare.at[0] - t0) / 1000.0 : -1, f.hat.at[0] ? (f.hat.at[0] - t0) / 1000.0 : -1);
                for (int b = 0; b < MUSIC_BANDS; b++) fprintf(csv, ",%.3f", f.band[b]);
                fprintf(csv, "\n");
            }
            for (; pass >= 0 && t0 + next_frame <= end; next_frame += 33) {
                struct music_feat f; musiclight_now(t0 + next_frame, &f);
                float o[12][3]; ringlive_music_effect(first + pass, rgb, 1, 12, t0 + next_frame, &f, o);
                fwrite(o, sizeof o, 1, out[pass]);
            }
            sim = end;
        }
        if (pass < 0) { musiclight_debug(NULL); fclose(dbg); }
    }
    for (int k = 0; k < ne; k++) fclose(out[k]);
    fclose(csv); fclose(in);
    return 0;
}
