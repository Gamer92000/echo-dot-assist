/* The front end's record in the micAsr samples' lowest bits (afemeta.c), on frames written here as libasp.so lays them
 * out (docs/re-vad.md, version 3.3): fields read back, a broken checksum or sync refused, plain audio refused.  The
 * captures in device-logs/ (not in git) all pass: docs/re-vad.md "Against the captures". */
#include <stdio.h>
#include <stdlib.h>
#include "afemeta.h"

static int bad;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) bad = 1; }

static void put(int16_t *s, int at, int n, unsigned v) { while (n--) { s[at] = (int16_t)((s[at] & ~1) | ((v >> n) & 1)); at++; } }

static void frame(int16_t *s, int vad, int rms_db, int playback, int muted)
{
    for (int i = 0; i < 128; i++) s[i] = (int16_t)((rand() % 4000) - 2000);     /* audio in the upper bits */
    for (int i = 0; i < 128; i++) s[i] &= ~1;
    put(s, 0, 8, 0xa5); put(s, 8, 8, 0x63); put(s, 16, 7, 0x68);
    put(s, 23, 6, 17); put(s, 29, 16, 9559);             /* frame counter, clock */
    put(s, 54, 8, (unsigned)(rms_db + 256)); put(s, 67, 1, (unsigned)playback); put(s, 75, 1, (unsigned)muted);
    put(s, 89, 5, 20); put(s, 94, 5, 27); put(s, 113, 2, (unsigned)vad);
    unsigned ones = 0; for (int i = 0; i < 120; i++) ones += s[i] & 1;
    put(s, 120, 8, ones & 0xff);
}

int main(void)
{
    int16_t s[128]; struct afemeta m;
    for (int v = 0; v < 4; v++) {
        frame(s, v, -47, v & 1, 0);
        int ok = afemeta_frame(s, &m) && m.vad == v && m.rms_db == -47 && m.playback == (v & 1) && !m.muted;
        char what[64]; snprintf(what, sizeof what, "VAD %d, level, playback read back", v); check(ok, what);
    }
    frame(s, 3, -90, 0, 1);
    check(afemeta_frame(s, &m) && m.muted && m.rms_db == -90, "muted, -90 dBFS");
    frame(s, 2, -50, 0, 0); s[113] ^= 1;
    check(!afemeta_frame(s, &m), "a flipped bit: the checksum refuses it");
    frame(s, 2, -50, 0, 0); s[2] ^= 1; s[3] ^= 1;      /* 0xa5: a 1 and a 0, so the count of ones stays */
    check(!afemeta_frame(s, &m), "sync broken (checksum unchanged): refused");
    frame(s, 2, -50, 0, 0);
    int16_t t[256]; for (int i = 0; i < 256; i++) t[i] = s[i % 128];
    check(!afemeta_frame(t + 64, &m), "half a frame late: refused (the caller finds the phase)");
    int pass = 0;
    for (int k = 0; k < 10000; k++) { for (int i = 0; i < 128; i++) s[i] = (int16_t)((rand() % 4000) - 2000); pass += afemeta_frame(s, &m); }
    check(pass == 0, "10000 frames of plain audio: none taken for a record");
    return bad;
}
