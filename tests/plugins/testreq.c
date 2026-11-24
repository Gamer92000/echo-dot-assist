/* A stand-in listener for hassmic's own tests (tests/fake_plugins_request.py): what docs/plugins.md phase 6 gives a
 * plugin (request.audio, request.text), used as a speaker-recognition or logging plugin would, against the
 * SDK's client.  It says in its log (hassmic's: "plugin testreq: ...", private lines there) what it got of each
 * request, for the test to hold against what Home Assistant got:
 *   run N start source S slot L phrase P
 *   run N end why W samples X dropped D: got B bytes in K blocks, crc C, first offset F, gaps G, outside 0
 *     (crc: CRC-32 of every byte of audio it got, in order, as zlib's; gaps: blocks whose offset was not the last one
 *     plus its count; outside: blocks that came for another run than the one begun)
 *   run N text heard|reply: T
 * Its settings, as the page sets them:
 *   deaf   on: it takes 12 s over each request's start before it reads anything (a plugin that does not keep up; the
 *          request's socket holds 6 s of the start's packets of 1024 samples, ~8 s of the PC's live ones of 800,
 *          plugin.c REQ_SNDBUF): blocks are dropped, and Home Assistant loses nothing
 *   late   ms it waits at each request's start before it reads (an Echo's plugin that gets REQ_START late: what
 *          came meanwhile, the lead-in, the replay, the live audio, must still be in the socket)
 *   try    "send": it sends hassmic the request's own messages (REQ_START, REQ_END, REQ_TEXT), which are hassmic's to
 *          send and never a plugin's, straight into its control socket, and says so; hassmic answers BAD (the
 *          client drops answers nobody waits for) and nothing begins */
#include "hassmic_plugin.h"
#include "plugmsg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int deaf, late, in_run, gaps, blocks, outside;
static uint32_t run_now, crc, bytes, first_off, next_off;
static char tried[16];

static uint32_t crc32_add(uint32_t c, const uint8_t *p, size_t n)
{
    static uint32_t t[256]; static int made;
    if (!made) { for (uint32_t i = 0; i < 256; i++) { uint32_t k = i; for (int j = 0; j < 8; j++) k = k & 1 ? 0xEDB88320u ^ (k >> 1) : k >> 1; t[i] = k; } made = 1; }
    c = ~c;
    while (n--) c = t[(c ^ *p++) & 0xff] ^ (c >> 8);
    return ~c;
}

static void config(void *ctx, const struct hm_setting *s, int n)
{
    (void)ctx;
    for (int i = 0; i < n; i++) {
        if (!strcmp(s[i].name, "deaf")) deaf = !strcmp(s[i].value, "1");
        if (!strcmp(s[i].name, "late")) late = atoi(s[i].value);
        if (!strcmp(s[i].name, "try") && strcmp(s[i].value, tried)) {
            snprintf(tried, sizeof tried, "%s", s[i].value);
            if (!strcmp(tried, "send")) {
                /* hassmic's own messages, from the plugin: the protocol has none by which a plugin starts, ends or puts
                 * text into a request (4.9) */
                uint8_t b[256]; struct pm_w w; struct pm_req_start rs = { 99, PM_REQSRC_BUTTON, 0, pm_s("Alexa") };
                struct pm_req_end re = { 99, PM_REQEND_HA, 0, 0 }; struct pm_req_text rt = { 99, PM_REQTEXT_HEARD, pm_s("open the door") };
                pm_winit(&w, b, sizeof b); pm_put_req_start(&w, 9001, &rs); if (send(3, b, (size_t)pm_wlen(&w), MSG_NOSIGNAL) < 0) { }
                pm_winit(&w, b, sizeof b); pm_put_req_text(&w, 9002, &rt); if (send(3, b, (size_t)pm_wlen(&w), MSG_NOSIGNAL) < 0) { }
                pm_winit(&w, b, sizeof b); pm_put_req_end(&w, 9003, &re); if (send(3, b, (size_t)pm_wlen(&w), MSG_NOSIGNAL) < 0) { }
                hm_log("sent REQ_START, REQ_TEXT, REQ_END itself");
            }
        }
    }
}

/* (the capabilities granted as bits, for the test's log line: hm_granted per capability) */
static unsigned granted(void) { unsigned c = 0; for (int k = 0; k <= HM_CAP_BUTTON; k++) if (hm_granted(k)) c |= 1u << k; return c; }
static void ready(void *ctx) { (void)ctx; hm_log("ready, capabilities %08x", granted()); }

static void start(void *ctx, uint32_t run, int source, int slot, const char *phrase)
{
    (void)ctx;
    in_run = 1; run_now = run; crc = 0; bytes = 0; blocks = 0; gaps = 0; first_off = 0xffffffffu; next_off = 0;
    hm_log("run %u start source %d slot %d phrase %s", run, source, slot, phrase);
    if (deaf) { struct timespec t = { 12, 0 }; nanosleep(&t, NULL); }
    else if (late > 0) { struct timespec t = { late / 1000, (long)(late % 1000) * 1000000L }; nanosleep(&t, NULL); }
}

static void audio(void *ctx, uint32_t run, uint32_t off, const int16_t *pcm, int n)
{
    (void)ctx;
    if (!in_run || run != run_now) { outside++; hm_log("audio outside a run (run %u, offset %u)", run, off); return; }
    if (first_off == 0xffffffffu) first_off = off;
    else if (off != next_off) gaps++;
    next_off = off + (uint32_t)n;
    crc = crc32_add(crc, (const uint8_t *)pcm, (size_t)n * 2); bytes += (uint32_t)n * 2; blocks++;
}

static void end(void *ctx, uint32_t run, int why, uint32_t samples, uint32_t dropped)
{
    (void)ctx;
    hm_log("run %u end why %d samples %u dropped %u: got %u bytes in %d blocks, crc %08x, first offset %u, gaps %d, outside %d",
           run, why, samples, dropped, bytes, blocks, crc, first_off == 0xffffffffu ? 0 : first_off, gaps, outside);
    in_run = 0;
}

static void text(void *ctx, uint32_t run, int kind, const char *t)
{
    (void)ctx;
    hm_log("run %u text %s: %s", run, kind == HM_TEXT_HEARD ? "heard" : kind == HM_TEXT_REPLY ? "reply" : "?", t);
}

int main(void)
{
    static const struct hm_callbacks cb = { .config = config, .ready = ready, .request_start = start, .request_audio = audio, .request_end = end, .request_text = text };
    return hm_run(&cb, NULL) ? 1 : 0;
}
