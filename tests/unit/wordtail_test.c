/* Where the replay after a wake word starts (wordtail.c), on level contours in 10 ms frames from Pryon's end.  The first
 * two are a user's Home Assistant debug recordings (issue #72, Echo Dot 2 in a kitchen, 50 ms frames, the gain taken
 * back out), each 50 ms value held for five frames with a little flicker; the rest are the cases the rules are for.
 * Every contour is also fed a frame at a time, as the capture thread does: the answer must come at once and be the
 * same as with all frames at hand. */
#include <stdio.h>
#include <stdlib.h>
#include "wordtail.h"

static int bad;
static float db[200];

static int fill(const float *v50, int n50, int at)                 /* 50 ms values from frame at on */
{
    for (int i = 0; i < n50; i++)
        for (int k = 0; k < 5; k++) db[at + i * 5 + k] = v50[i] + (rand() / (float)RAND_MAX - 0.5f) * 2;
    return at + n50 * 5;
}

static void flat(int from, int to, float v) { for (int i = from; i < to; i++) db[i] = v + (rand() / (float)RAND_MAX - 0.5f) * 2; }

static void check(const char *what, int n, float keyword, int lo, int hi)
{
    const char *why = "", *w = "";
    int all = wordtail_cut(db, n, keyword, 1, &why), step = -1, at = 0;
    for (at = 1; at <= n && (step = wordtail_cut(db, at, keyword, 1, &w)) < 0; at++) ;
    int ok = all >= lo && all <= hi && step == all;
    printf("%s %s: frame %d (%s), decided after %d frames%s\n", ok ? "ok  " : "FAIL", what, all, why, at,
           step == all ? "" : ", fed a frame at a time it differs");
    if (!ok) bad = 1;
}

int main(void)
{
    srand(1);
    /* 07:28: keyword -40.3 dBFS, 14.3 dB of gain; the last vowel on for 200 ms after the end, the command at 0.9 s.
     * Nothing was left out (the 3 dB rise came at once) */
    { static const float v[] = { -40, -34, -36, -38, -43, -48, -51, -54, -56, -54, -55, -57, -61, -62, -64, -67, -68, -58,
                                 -31, -28, -36, -46 };
      int n = fill(v, sizeof v / sizeof *v, 0);
      check("07:28, the vowel after the end", n, -40.3f, 22, 32); }
    /* 07:39: keyword -42.4 dBFS, 16.4 dB of gain, 60 ms left out (12 dB under the keyword, at the "ks"): 60 ms of the
     * word before what the recording has, then a quiet frame, 150 ms of the last syllable, the decay, the room, the
     * command at about 0.96 s */
    { static const float v[] = { -56.4f, -40.4f, -38.4f, -42.4f, -47.4f, -54.4f, -54.4f, -56.4f, -61.4f, -61.4f, -64.4f,
                                 -67.4f, -68.4f, -71.4f, -71.4f, -72.4f, -72.4f, -71.4f, -62.4f, -63.4f };
      flat(0, 3, -44); flat(3, 6, -50);
      int n = fill(v, sizeof v / sizeof *v, 6);
      check("07:39, \"ks\" then the last syllable", n, -42.4f, 27, 36); }
    /* espeak's "Alexa" (testdata/alexa_espeak.raw under pryon_test): its "a" ends 35 ms after Pryon's end, then
     * digital silence */
    flat(0, 4, -18); flat(4, 60, -100);
    check("synthetic: the word ends at once", 60, -22.4f, 3, 6);
    /* the word over before Pryon was sure, then 100 ms of room and the command: nothing of the command left out */
    flat(0, 10, -62); flat(10, 60, -36);
    check("pause, then the command", 60, -40, 0, 2);
    /* a command spoken straight on: the word's end, 50 ms of dip, the command's syllables with dips of their own.
     * No pause: the early dip, not one inside the command */
    flat(0, 8, -38); flat(8, 13, -58);
    for (int i = 13; i < 80; i++) db[i] = (i - 13) % 20 < 16 ? -36 : -52;
    check("straight on: the early dip", 80, -40, 8, 12);
    /* the word over before Pryon was sure and the command at once, its first dip ("turn | on") at 160 ms: no dip
     * early enough to be the word's end, nothing left out */
    flat(0, 16, -36); flat(16, 21, -55); flat(21, 80, -36);
    check("straight on, the word already over: nothing", 80, -40, 0, 0);
    /* an early dip (120 ms) where the first version had stopped sooner, at a 3 dB rise (30 ms): never more than it */
    for (int i = 0; i < 12; i++) db[i] = i < 3 ? -40 : i == 3 ? -45 : -38;   /* no flicker: the rise is frame 4's */
    flat(12, 15, -55); flat(15, 80, -36);
    check("never more than the first version", 80, -40, 3, 3);
    /* a room as loud as the word (3 dB under it), the vowel on for 150 ms: no dip, nothing left out */
    flat(0, 15, -38); flat(15, 80, -44);
    check("loud room: nothing", 80, -41, 0, 0);
    /* a child's "Alexaaaa": the vowel 450 ms long, then its decay; no pause within 500 ms and its dip late: nothing
     * (the tail stays, as before) */
    flat(0, 45, -36); for (int i = 45; i < 55; i++) db[i] = -36 - (i - 44) * 3; flat(55, 100, -66);
    check("long vowel: nothing", 100, -40, 0, 0);
    /* the room flickering 4 dB around -50 under a -40 keyword, the vowel on for 100 ms: the pause still counts */
    flat(0, 10, -36);
    for (int i = 10; i < 80; i++) db[i] = -50 + ((i * 7) % 5 - 2) * 2;
    check("flickering room", 80, -40, 10, 14);
    /* "Alexa lights on" in one breath (issue #79): the "ts" closure 320 ms after Pryon's end as long as a pause.  It
     * passes for one, unless an aligner gave no answer: then nothing left out */
    flat(0, 30, -36); flat(30, 42, -56); flat(42, 80, -36);
    check("the closure in \"lights\" taken for a pause", 80, -40, 30, 42);
    const char *why = "";
    int least = wordtail_cut(db, 80, -40, 0, &why);
    printf("%s no aligner's answer: no pause: frame %d (%s)\n", least == 0 ? "ok  " : "FAIL", least, why);
    if (least) bad = 1;
    /* a real pause early after the word: without pause_ok the first dip's lowest, at most what the first version took */
    flat(0, 5, -36); flat(5, 30, -62); flat(30, 80, -36);
    least = wordtail_cut(db, 80, -40, 0, &why);
    printf("%s no aligner's answer, a pause: inside it, at most 120 ms: frame %d (%s)\n", least >= 5 && least <= 12 ? "ok  " : "FAIL", least, why);
    if (least < 5 || least > 12) bad = 1;
    if (wordtail_cut(db, 20, -40, 0, &why) >= 0) { printf("FAIL no aligner's answer: decided before its 500 ms\n"); bad = 1; }
    return bad;
}
