/* Wake sound removal (earsup.c) on a chime like the stock wake sound, coming back as Amazon's echo canceller leaves it:
 * 100 ms after the mixer took it, with two reflections, at -57 dBFS over white noise at -65 dBFS (PLAN.md "Wake sound in
 * the pipeline's audio": +4 to +8 dB over the floor), and espeak speech at -57 dBFS over it, as a command spoken straight
 * through the sound.  Fed in the capture's 50 ms blocks.
 * usage: earsup_test testdata/alexa_espeak.raw */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "earsup.h"

#define RATE 16000
#define N (RATE * 6)
#define S0 (RATE * 16 / 10)                 /* the sound is handed to the mixer at 1.6 s, over the first phrase */
#define S1 (S0 + 1200)                      /* the mixer takes it 75 ms later */
#define DELAY 1600                          /* its echo 100 ms after that */
static int bad;
static void check(int ok, const char *what, double v) { printf("%s %s: %.2f\n", ok ? "ok  " : "FAIL", what, v); if (!ok) bad = 1; }

static double energy(const float *s, size_t a, size_t b) { double e = 0; for (size_t i = a; i < b; i++) e += (double)s[i] * s[i]; return e; }
static double db(double a, double b) { return 10 * log10((a + 1e-20) / (b + 1e-20)); }

static int16_t chime[RATE * 3 / 4 * 3], file[N], in[N], out[N];
static float speech[N], noise[N], echo[N];

/* 650 ms at 48 kHz: three tones with two harmonics each, 20 ms attack, decaying, as ui_wakesound */
static size_t make_chime(void)
{
    size_t n = 48000 * 65 / 100;
    for (size_t i = 0; i < n; i++) {
        double t = i / 48000.0, env = fmin(1, t / 0.02) * exp(-t / 0.18), v = 0;
        static const double f[3] = { 523.25, 659.25, 1046.5 };
        for (int k = 0; k < 3; k++) v += sin(2 * M_PI * f[k] * t) + 0.3 * sin(4 * M_PI * f[k] * t) + 0.1 * sin(6 * M_PI * f[k] * t);
        chime[i] = (int16_t)lrint(8000 * env * v);
    }
    return n;
}

/* one run: in = speech + noise + echo, through earsup in 800-sample blocks; returns samples out */
static size_t run(float speech_gain, float echo_gain, size_t cn, int enable)
{
    unsigned seed = 7;
    memset(echo, 0, sizeof echo);
    for (size_t j = 0; j * 3 < cn; j++) {         /* every third sample: 16 kHz */
        size_t at = S1 + DELAY + j; float v = chime[j * 3] / 32768.0f * echo_gain;
        if (at < N) echo[at] += v;
        if (at + 48 < N) echo[at + 48] += 0.5f * v;
        if (at + 160 < N) echo[at + 160] += 0.25f * v;
    }
    for (size_t i = 0; i < N; i++) {
        seed = seed * 1103515245u + 12345u;
        noise[i] = (float)(pow(10, -65 / 20.0) * sqrt(3) * (((seed >> 8) & 0xffff) / 32768.0 - 1.0));
        speech[i] = file[i] / 32768.0f * speech_gain;
        in[i] = (int16_t)lrintf((speech[i] + noise[i] + echo[i]) * 32768);
    }
    earsup_enable(enable); earsup_reset();
    size_t made = 0;
    for (size_t at = 0; at < N; at += 800) {
        const int16_t *p; int held;
        if (at == S0) earsup_play(chime, cn, 48000, S0);
        if (at == S1 / 800 * 800) earsup_started(S1);
        size_t m = earsup_run(in + at, 800, at, &p, &held);
        memcpy(out + made, p, m * sizeof *p); made += m;
    }
    return made;
}

int main(int argc, char **argv)
{
    FILE *f = fopen(argc > 1 ? argv[1] : "testdata/alexa_espeak.raw", "rb");
    if (!f) { printf("FAIL cannot open the speech file\n"); return 1; }
    fread(file, 2, N, f); fclose(f);
    size_t cn = make_chime();
    static float a[N], b[N];
    double sp = 0; for (size_t i = RATE * 3 / 2; i < RATE * 6; i++) sp += (double)file[i] * file[i];
    float sg = (float)pow(10, (-57 - 10 * log10(sp / (RATE * 4.5) / (32768.0 * 32768.0))) / 20);     /* speech at -57 dBFS */
    double ce = 0; for (size_t j = 0; j * 3 < cn; j++) ce += (double)chime[j * 3] * chime[j * 3];
    float eg = (float)pow(10, (-57 - 10 * log10(ce / (cn / 3.0) / (32768.0 * 32768.0) * 1.3125)) / 20);   /* echo at -57 dBFS */
    size_t w0 = S1 + DELAY, w1 = S1 + DELAY + cn / 3 + RATE * 3 / 10;          /* where the echo is */
    int d; float match, red;

    /* off: nothing changes */
    size_t made = run(sg, eg, cn, 0);
    check(made == N && !memcmp(in, out, sizeof in), "off: the audio passes untouched", (double)made);

    /* the echo alone, over the noise */
    made = run(0, eg, cn, 1);
    check(made >= N - RATE * 3 / 2 && !memcmp(in, out, S0 * 2), "before the sound: untouched, and the held audio comes out", (double)made);
    check(made == N && !memcmp(in + w1 + RATE, out + w1 + RATE, (N - w1 - RATE) * 2), "after its echo: untouched", (double)(N - w1 - RATE));
    earsup_last(&d, &match, &red);
    check(abs(d - DELAY) <= 128, "the echo's delay found (samples, 1600 true)", d);
    check(match > 0.3f, "it matches the sound", match);
    for (size_t i = 0; i < N; i++) { a[i] = echo[i]; b[i] = out[i] / 32768.0f - noise[i]; }
    double r = db(energy(a, w0, w1), energy(b, w0, w1));
    check(r > 12, "echo alone: what is left of it, dB under the echo", r);

    /* a command spoken through it */
    made = run(sg, eg, cn, 1);
    earsup_last(&d, &match, &red);
    check(abs(d - DELAY) <= 128, "with speech: the echo's delay found", d);
    for (size_t i = 0; i < N; i++) { a[i] = echo[i]; b[i] = out[i] / 32768.0f - noise[i] - speech[i]; }
    r = db(energy(a, w0, w1), energy(b, w0, w1));
    check(r > 8, "with speech: echo left plus speech lost, dB under the echo", r);
    for (size_t i = 0; i < N; i++) { a[i] = speech[i]; b[i] = out[i] / 32768.0f - noise[i]; }
    double kept = db(energy(b, w0, w1), energy(a, w0, w1));
    check(fabs(kept) < 2, "with speech: its level over the sound kept (dB)", kept);

    /* a command 22 dB louder than the echo: still found and taken out; 32 dB louder: left alone, it no longer matters */
    float loud = sg * (float)pow(10, 22 / 20.0);
    made = run(loud, eg, cn, 1);
    earsup_last(&d, &match, &red);
    check(abs(d - DELAY) <= 128, "speech 22 dB over the echo: its delay found", d);
    for (size_t i = 0; i < N; i++) { a[i] = echo[i]; b[i] = out[i] / 32768.0f - noise[i] - speech[i]; }
    r = db(energy(a, w0, w1), energy(b, w0, w1));
    check(r > 6, "speech 22 dB over the echo: echo left plus speech lost, dB under the echo", r);
    for (size_t i = 0; i < N; i++) { a[i] = speech[i]; b[i] = out[i] / 32768.0f - noise[i]; }
    kept = db(energy(b, w0, w1), energy(a, w0, w1));
    check(fabs(kept) < 1, "speech 22 dB over the echo: its level kept (dB)", kept);
    made = run(sg * (float)pow(10, 32 / 20.0), eg, cn, 1);
    earsup_last(&d, &match, &red);
    check(made == N && !memcmp(in, out, sizeof in), "speech 32 dB over the echo: the audio passes untouched", match);

    /* the sound played, but nothing of it came back (the echo canceller took it all): nothing changes */
    made = run(sg, 0, cn, 1);
    earsup_last(&d, &match, &red);
    check(made == N && !memcmp(in, out, sizeof in), "no echo: the audio passes untouched", match);
    return bad;
}
