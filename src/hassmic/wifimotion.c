/*
 * Wi-Fi motion (experimental): the receive level of the Wi-Fi radio as a motion sensor.
 *
 * A person walking through the path between the Echo and the access point shadows and reflects it: the level of the
 * frames the radio receives jumps by a few dB.  donut's MT7668 driver (wlan_mt76x8_sdio.ko, MediaTek gen4m) reports it
 * through its private iwpriv command "driver", sub-command RX_STAT: "RCPI RX0 = 112" is the last received frame's
 * level in half dB (dBm = RCPI / 2 - 110; 255 = none).  Needs no root (works as puffin).  Its proper channel state
 * information (SET_CSI / GET_CSI, amplitude and phase per subcarrier, which would show breathing too) is accepted by the
 * driver, but the chip firmware never delivers any.
 * That is the firmware's figure for the last frame from anyone on the channel, though, other stations and neighbouring
 * networks included.  Our kernel module reads the RCPI of every frame from the access point in the driver's receive
 * path instead and words it like RX_STAT in /proc/<module> (RAM: nothing touches the flash), with a frame counter so
 * that a poll without a new frame is skipped: src/kmod/hassmic_rcpi4m.c on the Dot 3, src/kmod/hassmic_rcpi.c on the
 * Dot 2 and Echo 2, whose driver (MediaTek's gen2, built into the kernel) reports no frame levels at all.
 * HASSMIC_WIFI_KMOD names that file where main.sh can load the module (device.conf KMOD), and then it is the source:
 * main.sh loads it once this is switched on, so until then the file is missing and the poller waits.  RX_STAT stays
 * for a donut build without the module (no kernel toolchain, see DEVELOPMENT.md).
 *
 * Measured on the Dot 3 (2026-10-01, router one room behind it, 10 readings a second, 7.5 min): the scatter of RCPI
 * over 2 s is 0.5-0.6 with the room empty or someone sitting still, 1.2 on average while walking, up to 2.2 when
 * crossing the path; single odd frames reach 2.4 in an empty room, so a median of three goes first and the scatter
 * has to stay over the threshold for 1 s of the last 2.  Threshold 1.2 (sensitivity 5): motion started on leaving,
 * on walking, on leaving again and once more (someone in the router's room?), never with the room empty or still.
 * That was RX_STAT; the module's readings (2026-10-01, side by side for 60 s) have the same level and range, about 17-30
 * frames a second from the access point.
 */
#include "wifimotion.h"
#include <limits.h>
#include <linux/wireless.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define IFACE "wlan0"
#define HOLD_MS 30000                   /* motion stays on this long after the last movement, as a PIR's */
#define FROZEN_MS 60000                 /* the module's frame counter unchanged this long: no frames reach the hook */
#define POLL_MS 100
#define BUF 4096                        /* the command's answer: iwpriv's buffer size; the driver declares 2000 */

static atomic_int enabled, sensitivity = WIFIMOTION_SENS_DEFAULT, motion = -1;
static int present = -1, sock = -1, cmd = -1;
static const char *file;                /* readings from a file instead of the ioctl: the module's, or HASSMIC_FAKE_WIFI's (tests) */
static int fake;
static void (*changed_cb)(void);

static long long mono_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }

/* ---------------------------------------------------------------- detector */

void wm_reset(struct wm_det *d) { memset(d, 0, sizeof *d); d->last_ms = LLONG_MIN / 2; }

static float med3(float a, float b, float c) { return a > b ? (b > c ? b : a > c ? c : a) : (a > c ? a : b > c ? c : b); }

int wm_feed(struct wm_det *d, float rcpi, long long now_ms, int sens, int hold_ms)
{
    /* sensitivity 5: 1.2 (see above); each step 15 %, so 1 takes 2.1 (only crossing the path), 10 takes 0.6 (about
     * what a still room scatters: twitchy) */
    float thr = 1.2f * powf(1.15f, (float)(WIFIMOTION_SENS_DEFAULT - sens)), sum = 0, sq = 0;
    int i = d->n++, above = 0, count = 0;
    d->raw[i % 3] = rcpi;
    d->med[i % WM_WINDOW] = i < 2 ? rcpi : med3(d->raw[0], d->raw[1], d->raw[2]);
    if (d->n >= WM_WINDOW) {
        for (int k = 0; k < WM_WINDOW; k++) { sum += d->med[k]; sq += d->med[k] * d->med[k]; }
        float mean = sum / WM_WINDOW, var = sq / WM_WINDOW - mean * mean;
        above = var > 0 && sqrtf(var) > thr;
    }
    d->above[i % WM_WINDOW] = above;
    for (int k = 0; k < WM_WINDOW; k++) count += d->above[k];
    if (count >= WM_WINDOW / 2) d->last_ms = now_ms;
    return now_ms - d->last_ms < hold_ms;
}

/* The access point sends each rate at its own power: on the Dot 3 (5 GHz, 2026-10-01) its broadcasts (legacy OFDM,
 * 6 Mbit/s) arrive at RCPI 100, its unicast frames (VHT80) at 93 with MCS 8 and 91 with MCS 9, interleaved frame by
 * frame, MCS 8 and 9 in runs of 1-5 s.  Last-frame readings then scattered by 2.2 over 2 s with nobody about: motion
 * all the time.  So a reading counts as its distance from the running level of its kind, which follows slow drift
 * (30 s) but not a person walking through; the first readings of a kind only set its level.  The kind is what the
 * module can tell (KIND): on donut the rate itself (same recording: scatter 0.21, no motion; the labelled walk
 * recording detects as before), on biscuit and radar only broadcast or not (gen2 has no rate per frame; replayed
 * that way the same recording still shows motion 19 % of the time, from the MCS 8/9 steps: on 5 GHz with VHT; the
 * Dot 2 and Echo 2 have no VHT).  RX_STAT has no kind. */
int wm_kind_norm(struct wm_kinds *w, int rcpi, unsigned key, long long now_ms, float *out)
{
    struct wm_kind *e = NULL, *old = &w->k[0];
    for (int i = 0; i < WM_KINDS && !e; i++) {
        if (w->k[i].n && w->k[i].key == key) e = &w->k[i];
        else if (!w->k[i].n || (old->n && w->k[i].last_ms < old->last_ms)) old = &w->k[i];
    }
    if (!e) { e = old; e->key = key; e->n = 0; }                /* new kind, or the one unseen longest gives way */
    float a = e->n ? (now_ms - e->last_ms) / 30000.0f : 1;
    if (a < 1.0f / (e->n + 1)) a = 1.0f / (e->n + 1);         /* plain mean over the first readings */
    if (a > 1) a = 1;
    *out = rcpi - e->level;
    int ok = e->n >= 5;
    e->level += (rcpi - e->level) * a; e->last_ms = now_ms; e->n++;
    return ok;
}

int wm_parse_rcpi(const char *s)
{
    const char *p = strstr(s, "RCPI RX0");
    if (!p || !(p = strchr(p, '='))) return -1;
    long v = strtol(p + 1, NULL, 10);
    return v > 0 && v < 255 ? (int)v : -1;
}

/* ---------------------------------------------------------------- driver */

static int rx_stat(char *buf)
{
    if (file) {
        FILE *f = fopen(file, "r"); size_t n;
        if (!f) return -1;
        n = fread(buf, 1, BUF - 1, f); fclose(f); buf[n] = 0;
        return 0;
    }
    struct iwreq w;
    memset(&w, 0, sizeof w); strncpy(w.ifr_name, IFACE, IFNAMSIZ - 1);
    strcpy(buf, "RX_STAT");                         /* in and out through the same buffer, as iwpriv does it */
    w.u.data.pointer = buf; w.u.data.length = strlen(buf) + 1;
    if (ioctl(sock, cmd, &w) < 0) return -1;
    buf[w.u.data.length < BUF ? w.u.data.length : BUF - 1] = 0;
    return 0;
}

int wifimotion_present(void)
{
    if (present >= 0) return present;
    present = 0;
    char *buf = malloc(BUF);
    if (!buf) return 0;
    if ((file = getenv("HASSMIC_FAKE_WIFI"))) { fake = 1; present = rx_stat(buf) == 0; }
    else if ((file = getenv("HASSMIC_WIFI_KMOD"))) present = 1;                       /* loaded once switched on */
    else if ((sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)) >= 0) {
        /* the command's number from the driver's own table rather than a constant (donut: 0x8BEF) */
        static struct iw_priv_args args[256]; struct iwreq w;
        memset(&w, 0, sizeof w); strncpy(w.ifr_name, IFACE, IFNAMSIZ - 1);
        w.u.data.pointer = args; w.u.data.length = sizeof args / sizeof args[0];
        if (ioctl(sock, SIOCGIWPRIV, &w) == 0)
            for (int i = 0; i < w.u.data.length; i++) if (!strcmp(args[i].name, "driver")) cmd = args[i].cmd;
        present = cmd >= 0 && rx_stat(buf) == 0 && wm_parse_rcpi(buf) >= 0;
        if (!present) { close(sock); sock = -1; }
    }
    free(buf);
    fprintf(stderr, "wifi motion (experimental): %s%s\n", present ? "available, off unless switched on" : "not available on this Wi-Fi driver",
            present && file && !fake ? " (through the kernel module)" : "");
    return present;
}

static void *poll_thread(void *arg)
{
    (void)arg;
    char *buf = malloc(BUF); struct wm_det d; struct wm_kinds kinds; int fails = 0, frozen = 0; long frames = -1; float v;
    long long same_ms = 0;
    /* the PC test cannot wait 30 s for motion to clear, nor a minute for a frozen counter */
    int hold = fake ? 3000 : HOLD_MS, frozen_ms = fake ? 3000 : FROZEN_MS;
    if (!buf) return NULL;
    wm_reset(&d); memset(&kinds, 0, sizeof kinds);
    for (;;) {
        if (!atomic_load(&enabled)) {
            if (atomic_exchange(&motion, -1) != -1 && changed_cb) changed_cb();
            wm_reset(&d); memset(&kinds, 0, sizeof kinds); fails = 0;
            usleep(200000);
            continue;
        }
        long long t = mono_ms();
        int r = rx_stat(buf) == 0 ? wm_parse_rcpi(buf) : -1;
        const char *fr = r >= 0 ? strstr(buf, "FRAMES =") : NULL;     /* the module's counter: no new frame, no reading */
        if (fr) {
            long n = strtol(fr + 8, NULL, 10);
            /* The counter stuck for a minute: nothing reaches the hook.  Android unloads the Wi-Fi driver whenever Wi-Fi
             * goes off and on, and the hook goes with it (an Echo Show 5, 2026-11-13: 37 h without a frame, the sensor
             * never anything but unknown); the module hooks the driver again once it is back, and the readings resume
             * on a fresh baseline (the levels may have moved with the new link).  Said once, and unknown meanwhile:
             * "still" from before the freeze would be a lie */
            if (n == frames) {
                r = -2;
                if (!same_ms) same_ms = t;
                else if (!frozen && t - same_ms >= frozen_ms) {
                    frozen = 1;
                    fprintf(stderr, "wifi motion: no frame from the access point for %d s (Wi-Fi driver reloaded? the module hooks it again once it is back)\n", frozen_ms / 1000);
                    if (atomic_exchange(&motion, -1) != -1 && changed_cb) changed_cb();
                }
            } else {
                if (frozen) { fprintf(stderr, "wifi motion: frames again\n"); wm_reset(&d); memset(&kinds, 0, sizeof kinds); }
                frozen = 0; same_ms = 0;
            }
            frames = n;
        }
        /* the module's kind of that frame; RX_STAT has none: one level for all */
        const char *kd = r >= 0 ? strstr(buf, "KIND =") : NULL;
        if (r >= 0 && !wm_kind_norm(&kinds, r, kd ? (unsigned)strtoul(kd + 6, NULL, 16) : 0, t, &v)) r = -2;
        if (r == -2) ;
        else if (r < 0) {
            if (++fails == 50) fprintf(stderr, "wifi motion: no readings for 5 s%s\n", file && !fake ? " (kernel module not loaded yet?)" : "");
        } else {
            int m = wm_feed(&d, v, t, atomic_load(&sensitivity), hold);
            if (fails >= 50) fprintf(stderr, "wifi motion: readings again\n");
            fails = 0;
            if (atomic_exchange(&motion, m) != m) {
                fprintf(stderr, "wifi motion: %s\n", m ? "moving" : "still");
                if (changed_cb) changed_cb();
            }
        }
        long long left = t + POLL_MS - mono_ms();
        if (left > 0) usleep((useconds_t)left * 1000);
    }
    return NULL;
}

void wifimotion_start(void (*changed)(void))
{
    static int started;
    if (started || !wifimotion_present()) return;
    started = 1; changed_cb = changed;
    pthread_t t;
    if (pthread_create(&t, NULL, poll_thread, NULL) == 0) pthread_detach(t);
}

int wifimotion_enable(int set)
{
    if (set >= 0 && atomic_exchange(&enabled, set != 0) != (set != 0))
        fprintf(stderr, "wifi motion (experimental): switched %s\n", set ? "on" : "off");
    return atomic_load(&enabled);
}

int wifimotion_sensitivity(int set)
{
    if (set >= 0) atomic_store(&sensitivity, set < WIFIMOTION_SENS_MIN ? WIFIMOTION_SENS_MIN : set > WIFIMOTION_SENS_MAX ? WIFIMOTION_SENS_MAX : set);
    return atomic_load(&sensitivity);
}

int wifimotion_motion(void) { return atomic_load(&motion); }
