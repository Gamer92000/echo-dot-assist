/* wakeclip.c: a clip is the ring's samples from 1 s ahead of the word to the setting's time past it, copied as the
 * capture puts them in (so the part after the word is never lost to the 4 s ring), with the word's place, its level
 * and the room's; ten of each kind, the oldest of that kind going first; a word reported too long is cut to its last
 * 2 s; samples already out of the ring stay silent; an outcome goes to the newest wake word only, once and in time;
 * labels, deletes, off drops all, nothing is kept while off; the page's JSON.  Under ASan/UBSan. */
#include "wakeclip.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAP (WAKECLIP_RATE * 4)     /* main.c's ring */
static int16_t ring[CAP];
static uint64_t ring_n;
static int fails;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }

/* sample i of the "room": a ramp anyone can predict, quiet ahead of the word, loud inside it */
static int16_t at(uint64_t i) { return (int16_t)((i * 7) % 1000) - 500; }

/* the capture: blocks of 800 into the ring, each handed to wakeclip_feed as main.c does */
static void capture(uint64_t samples, uint64_t loud_from, uint64_t loud_to)
{
    for (uint64_t done = 0; done < samples; done += 800) {
        for (int k = 0; k < 800; k++) {
            uint64_t i = ring_n + k;
            ring[i % CAP] = i >= loud_from && i < loud_to ? (int16_t)(at(i) * 30) : at(i);
        }
        ring_n += 800;
        wakeclip_feed(ring, CAP, ring_n);
    }
}

static int count(int kind)
{
    struct wakeclip_info i; int n = 0;
    for (int k = 0; !wakeclip_get(k, &i); k++) if (kind < 0 || i.kind == kind) n++;
    return n;
}

int main(void)
{
    struct wakeclip_info i; size_t n; int16_t *pcm; char js[16384];
    const uint64_t R = WAKECLIP_RATE;

    /* off: nothing kept */
    capture(R * 2, 0, 0);
    wakeclip_mark(WAKECLIP_HIT, "ALEXA", ring_n - R, ring_n - R / 2, 0, 0, 1000);
    check(count(-1) == 0, "off (the default): a wake word leaves no clip");

    wakeclip_set(1, 1000);
    capture(R * 2, 0, 0);
    uint64_t b = ring_n - R * 6 / 10, e = ring_n - R / 10;          /* a 0.5 s word that ended 0.1 s ago */
    for (uint64_t s = b; s < e; s++) ring[s % CAP] = (int16_t)(at(s) * 30);
    wakeclip_mark(WAKECLIP_HIT, "ALEXA", b, e, WAKECLIP_MUSIC, 1790000000, 5000);
    check(!wakeclip_get(0, &i) && !i.done && i.len == (uint32_t)(e - (b - R) + R) && i.word_at == R && i.word_len == e - b,
          "marked: 1 s before, the word, 1 s after; not done until the 1 s after has come");
    check(!wakeclip_audio(i.id, &n), "a clip not done yet is not handed out");
    capture(R * 3 / 2, 0, 0);
    check(!wakeclip_get(0, &i) && i.done, "done once the second after the word is in");
    pcm = wakeclip_audio(i.id, &n);
    int same = pcm && n == i.len;
    for (uint64_t s = 0; same && s < n; s++) {
        uint64_t x = i.from + s;
        same = pcm[s] == (x >= b && x < e ? (int16_t)(at(x) * 30) : at(x));
    }
    check(same, "the clip is the ring's samples, byte for byte, the part after the word copied as it came");
    free(pcm);
    check(i.word_db > i.floor_db + 25 && i.peak > 10000 && i.peak < 32767, "levels: the word ~30 dB over the room before it, not clipped");
    check(!strcmp(i.word, "ALEXA") && i.playing == WAKECLIP_MUSIC && i.wall == 1790000000, "its word, what played, when");

    /* outcomes: the newest wake word's, once, in time */
    wakeclip_outcome("no_speech", 5000 + 3000);
    wakeclip_outcome("request", 5000 + 4000);
    check(!wakeclip_get(0, &i) && !strcmp(i.outcome, "no_speech"), "an outcome goes to the newest wake word once; a later one does not replace it");
    wakeclip_mark(WAKECLIP_HIT, "ALEXA", ring_n - R, ring_n - R / 2, 0, 0, 100000);
    wakeclip_outcome("request", 100000 + WAKECLIP_OUTCOME_MS + 1);
    check(!wakeclip_get(0, &i) && !i.outcome[0], "nor one that comes too late to be its own");

    /* near misses and the ten of each */
    for (int k = 0; k < 14; k++) { wakeclip_mark(WAKECLIP_MISS, "ALEXA", ring_n - R, ring_n - R / 2, 0, 0, 0); capture(1600, 0, 0); }
    check(count(WAKECLIP_MISS) == WAKECLIP_KEEP && count(WAKECLIP_HIT) == 2, "fourteen near misses keep ten, and push out no wake word");
    unsigned newest = 0, oldest = ~0u;
    for (int k = 0; !wakeclip_get(k, &i); k++) if (i.kind == WAKECLIP_MISS) { if (i.id > newest) newest = i.id; if (i.id < oldest) oldest = i.id; }
    check(newest - oldest == WAKECLIP_KEEP - 1, "the oldest ones went");
    check(!wakeclip_get(0, &i) && i.id == newest, "listed newest first");

    /* a word too long, and one from before the ring */
    capture(R * 2, 0, 0);
    wakeclip_mark(WAKECLIP_HIT, "STOP", ring_n - R * 3, ring_n, 0, 0, 0);
    check(!wakeclip_get(0, &i) && i.word_len == R * WAKECLIP_WORD_MAX_MS / 1000, "a word reported 3 s long is cut to its last 2 s");
    wakeclip_mark(WAKECLIP_HIT, "x\"};<", ring_n - R / 2, ring_n - R / 2 + 10, 0, 0, 0);
    check(!wakeclip_get(0, &i) && !strcmp(i.word, "x"), "the engine's word keeps letters, digits, - and _ only (the list is JSON)");
    wakeclip_mark(WAKECLIP_MISS, "ALEXA", 10, 20, 0, 0, 0);       /* long out of the ring */
    capture(R * 2, 0, 0);
    int found = 0;
    for (int k = 0; !wakeclip_get(k, &i); k++) if (i.from == 0) {
        found = i.done; pcm = wakeclip_audio(i.id, &n);
        int silent = pcm != NULL; for (size_t s = 0; silent && s < n; s++) silent = !pcm[s];
        found &= silent; free(pcm);
    }
    check(found, "samples that left the ring before the mark stay silent, and the clip still finishes");

    /* labels, delete, JSON */
    wakeclip_get(0, &i);
    check(!wakeclip_label(i.id, WAKECLIP_NOT_WAKE) && wakeclip_label(i.id, 7) && wakeclip_label(99999, WAKECLIP_WAKE),
          "labels: known ones on known clips only");
    wakeclip_json(js, sizeof js, 7000);
    check(strstr(js, "\"on\":true,\"now_ms\":7000,\"after_ms\":1000,") && strstr(js, "\"label\":\"not_wake\"") && strstr(js, "\"kind\":\"near_miss\"")
          && strstr(js, "\"music\":true") && js[strlen(js) - 1] == '}', "the page's JSON");
    { const char *e = strstr(js, "\"env\":\""); size_t el = e ? strcspn(e + 7, "\"") : 0;
      check(el == 2 * WAKECLIP_ENV && strspn(e + 7, "0123456789abcdef") == el, "each done clip's waveform: 96 levels in hex"); }
    char small[64]; size_t l = wakeclip_json(small, sizeof small, 0);
    check(l == sizeof small - 1 && strlen(small) == l, "JSON cut to its buffer, terminated");
    int before = count(-1);
    check(!wakeclip_delete(i.id) && count(-1) == before - 1 && wakeclip_delete(i.id) == -1, "delete one");

    /* the setting's time after the word */
    wakeclip_set(1, 300);
    wakeclip_mark(WAKECLIP_HIT, "ALEXA", ring_n - R / 2, ring_n, 0, 0, 0);
    check(!wakeclip_get(0, &i) && i.len == R + R / 2 + R * 3 / 10, "0.3 s after the word");
    wakeclip_set(1, 9000);
    check(wakeclip_after_ms() == WAKECLIP_AFTER_MAX_MS, "never more than 3 s after it");

    wakeclip_set(0, 1000);
    check(count(-1) == 0, "off drops every clip");
    wakeclip_set(1, 1000);
    check(count(-1) == 0 && !wakeclip_delete(0), "on again: none came back; delete all on none");
    for (int k = 0; k < 3; k++) wakeclip_mark(WAKECLIP_HIT, "ALEXA", ring_n - R, ring_n - R / 2, 0, 0, 0);
    wakeclip_delete(0);
    capture(R * 2, 0, 0);
    check(count(-1) == 0, "delete all, clips still filling included");
    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails != 0;
}
