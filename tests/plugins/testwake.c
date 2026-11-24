/* A stand-in wake word plugin for hassmic's own tests (tests/fake_plugins.py): its "models" are text files that
 * start with "MODEL <phrase>", and its wake word is a burst of sound: a stretch over a level between two quiet
 * ones, 0.3 to 1.5 s long.  That is enough to drive everything a real engine drives: files, checks, slots, the
 * stream's clock, detections.  The setting `mode` makes it misbehave:
 *   late: detections reported 2 s old;  future: ending ahead of the stream;  burst: one for every block;
 *   deaf: it stops reading the stream;  closed: it closes its control socket and sleeps;  relist: it sends another
 *   list of wake words with every block;  stuck: it stops reading its control socket and sleeps;  spin: 3 s of
 *   nothing but CPU each time its settings arrive.
 * A model whose text has BROKEN fails to load, one with SLOW does not answer in time, one with CRASH aborts. */
#include "hassmic_plugin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RATE 16000
static char mode[16] = "listen";
static struct { int on, level; uint64_t begin, quiet; int loud; } slot[2];
static uint64_t expect;
static int deaf;

static int model_read(const char *list, const char *item, char *text, size_t cap)
{
    void *d; size_t n;
    if (hm_file_read(list, item, ".model", 16384, &d, &n)) return -1;
    if (n >= cap) n = cap - 1;
    memcpy(text, d, n); text[n] = 0; free(d);
    return 0;
}

static void on_config(void *ctx, const struct hm_setting *s, int n)
{
    (void)ctx;
    for (int i = 0; i < n; i++) {
        if (!strcmp(s[i].name, "mode")) snprintf(mode, sizeof mode, "%s", s[i].value);
        hm_log("setting %s=%s", s[i].name, strcmp(s[i].name, "pin") ? s[i].value : s[i].value[0] ? "(set)" : "(unset)");
    }
    deaf = !strcmp(mode, "deaf");
    if (!strcmp(mode, "closed")) { close(3); for (;;) { struct timespec t = { 1, 0 }; nanosleep(&t, NULL); } }
    if (!strcmp(mode, "stuck")) for (;;) { struct timespec t = { 1, 0 }; nanosleep(&t, NULL); }
    if (!strcmp(mode, "spin")) {
        struct timespec a, t; volatile unsigned long x = 0;
        clock_gettime(CLOCK_MONOTONIC, &a);
        do { for (int i = 0; i < 100000; i++) x += (unsigned long)i; clock_gettime(CLOCK_MONOTONIC, &t); } while (t.tv_sec - a.tv_sec < 3);
    }
}

static void on_files(void *ctx, const char *list, const struct hm_item *items, int n)
{
    const char *ids[16]; int k = 0;
    (void)ctx;
    for (int i = 0; i < n && k < 16; i++) {
        hm_log("item %s \"%s\" checked=%d fields=%d files=%d", items[i].id, items[i].title, items[i].checked, items[i].nfields, items[i].nfiles);
        for (int f = 0; f < items[i].nfields; f++) hm_log("  field %s=%s", items[i].fields[f].name, items[i].fields[f].value);
        if (items[i].checked) ids[k++] = items[i].id;
    }
    hm_wake_words(list, ids, k);
}

static int on_check(void *ctx, const char *list, const char *item, struct hm_check *out)
{
    char text[256];
    (void)ctx;
    if (model_read(list, item, text, sizeof text)) { snprintf(out->why, sizeof out->why, "it has no .model file"); return HM_FILE_BAD_MISSING; }
    if (strncmp(text, "MODEL ", 6)) { snprintf(out->why, sizeof out->why, "not a model: it starts with \"%.8s\"", text); return HM_FILE_BAD_FORMAT; }
    text[strcspn(text, "\n")] = 0;
    snprintf(out->title, sizeof out->title, "%.64s", text + 6);
    snprintf(out->fields[0].name, sizeof out->fields[0].name, "level"); snprintf(out->fields[0].value, sizeof out->fields[0].value, "2000");
    out->nfields = 1;
    return 0;
}

static int on_open(void *ctx, int s, const char *word)
{
    char text[256];
    (void)ctx;
    if (model_read("models", word, text, sizeof text)) return HM_WAKE_FAIL_NO_MODEL;
    if (strstr(text, "BROKEN")) return HM_WAKE_FAIL_BAD_MODEL;
    if (strstr(text, "CRASH")) abort();
    if (strstr(text, "SLOW")) { struct timespec t = { 7, 0 }; nanosleep(&t, NULL); }
    memset(&slot[s], 0, sizeof slot[s]); slot[s].on = 1; slot[s].level = 2000;
    hm_log("slot %d listens for %s", s, word);
    return 0;
}

static void on_close(void *ctx, int s) { (void)ctx; slot[s].on = 0; hm_log("slot %d closed", s); }
static void on_reset(void *ctx) { (void)ctx; slot[0].loud = slot[1].loud = 0; }

static void on_audio(void *ctx, uint64_t pos, const int16_t *pcm, int n)
{
    int peak = 0;
    (void)ctx;
    if (deaf) { struct timespec t = { 0, 200000000 }; nanosleep(&t, NULL); return; }     /* falls behind: blocks are lost */
    if (!strcmp(mode, "relist")) { static int flip; const char *ids[1] = { "burst" }; hm_wake_words("models", ids, (flip = !flip)); }
    if (expect && pos != expect) { hm_log("gap: %llu samples", (unsigned long long)(pos - expect)); slot[0].loud = slot[1].loud = 0; }
    expect = pos + (uint64_t)n;
    for (int i = 0; i < n; i++) { int v = pcm[i] < 0 ? -pcm[i] : pcm[i]; if (v > peak) peak = v; }
    for (int s = 0; s < 2; s++) {
        if (!slot[s].on) continue;
        if (!strcmp(mode, "burst")) { hm_wake_heard(s, pos > RATE / 2 ? pos - RATE / 2 : 0, pos + (uint64_t)n, 255); continue; }
        if (peak > slot[s].level) { if (!slot[s].loud) slot[s].begin = pos; slot[s].loud = 1; slot[s].quiet = 0; continue; }
        if (!slot[s].loud) continue;
        if (!slot[s].quiet) slot[s].quiet = pos;                 /* the word's end: the first quiet block */
        if (pos + (uint64_t)n - slot[s].quiet < RATE / 10) continue;      /* 100 ms of quiet after it */
        slot[s].loud = 0;
        /* testdata/alexa_espeak.raw: its first word is 0.6 s, the others longer: one detection a loop */
        if (slot[s].quiet - slot[s].begin < RATE * 45 / 100 || slot[s].quiet - slot[s].begin > RATE * 68 / 100) continue;
        if (!strcmp(mode, "late")) { struct timespec t = { 2, 0 }; nanosleep(&t, NULL); }
        hm_wake_heard(s, slot[s].begin, !strcmp(mode, "future") ? pos + RATE * 10 : slot[s].quiet, 200);
    }
}

static void on_ready(void *ctx)
{
    const struct hm_info *i = hm_info();
    (void)ctx;
    { unsigned c = 0; for (int k = 0; k <= HM_CAP_BUTTON; k++) if (hm_granted(k)) c |= 1u << k;
      hm_log("ready on %s (%s), capabilities %x, clock %s", i->name, i->model, c, i->clock_set ? "set" : "not set"); }
}

int main(void)
{
    static const struct hm_callbacks cb = { on_config, on_files, on_check, on_ready, on_open, on_close, on_reset, on_audio, NULL, NULL, NULL, NULL };
    return hm_run(&cb, NULL) ? 1 : 0;
}
