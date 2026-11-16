/*
 * libhmled.so: preloaded into ledcontroller on the Echo Dot 5 (device.mk LEDSHIM; root's main.sh starts ledcontroller
 * itself with it, ledc_start), so that hassmic can change the frames of an animation the LED manager already holds.
 *
 * Why: ledcontroller's LED HAL sends every animation once, at its start, to the LED manager on the SCP co-processor,
 * which keeps them all and refuses an init past about 400 (2026-11-15: stock's 115 plus 285 single-frame ones loaded,
 * 440 did not, and a failed init left it unusable until a reboot).  The palette hassmic plays Home Assistant's light
 * from (ring.c: 588 patterns on the Dots) cannot be loaded that way.  libace_ledmgr.so has the way around it:
 * aceLedMgr_updateAnimationData(name, data) replaces the frames of an animation by name, any length, while it plays
 * (checked on the Echo: a one-frame animation became a 24-frame pulse in place).  Only the process holding the LED
 * manager's session may call it, and that is ledcontroller: a second client's init resets its session.  So it runs in
 * there: a thread serving an abstract socket (up to 4 clients, a thread each) "@hassmic-led" (SOCK_SEQPACKET), one request a datagram,
 * "<name>\0<animation_bin>", the answer the call's result as text; "?leds\0" reads back what the LEDs show.
 *
 * Guarded: a peer of uid HASSMIC_LED_UID only (root's main.sh gives the daemon's; SO_PEERCRED), names "hassmic_..." only
 * (stock's are never changed), and the data checked as the HAL and the SCP will walk it (ring.c acs_encode makes it:
 * "animagic" "totl" <frames>, each optionally after "loop", "fram" <ms> and 12 pixels; the call takes no length).
 */
#define _GNU_SOURCE
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define TAG "hassmic-led"
#define FRAME (8 + 3 * 12)

static uint32_t be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* an animation the HAL and the SCP will walk to its end: exactly its frames, nothing after them */
static int valid(const unsigned char *b, size_t n)
{
    size_t p = 16; uint32_t k = 0, fr;
    if (n < 16 || memcmp(b, "animagic", 8) || memcmp(b + 8, "totl", 4)) return 0;
    fr = be32(b + 12);
    if (!fr || fr > 1000) return 0;
    while (p < n && k < fr) {
        if (n - p >= 4 && !memcmp(b + p, "loop", 4)) { p += 4; continue; }
        if (n - p < FRAME || memcmp(b + p, "fram", 4)) return 0;
        p += FRAME; k++;
    }
    return k == fr && p == n;
}

static int (*update)(const char *, const void *);
static int (*get_state)(uint8_t, void *);
static _Atomic int clients;
#define CLIENTS_MAX 4                           /* hassmic's own, and room for a tool (src/tools/ledstate.c) */

/* An update stops the animation it replaces: the LED manager shows nothing of it until it is started again (the Echo
 * Dot 5, 2026-11-16, read back with "?leds": a slot playing red went dark at its update, green only after ledctrl -s).
 * Started again at once after each update, the ring was dark for the ~30 ms a start takes on every frame of an effect:
 * it blinked.  So the live ring has two slots, LIVE and LIVE_B, next to each other in layer 0 (ring.c), of which
 * ledcontroller knows only LIVE: a frame goes into the one not showing, that one is started, then the other stopped
 * (in a layer only the first active one shows, so the swap has no dark between), as ring.c's light's a/b.
 * ledcontroller's starts and stops of LIVE are followed here (its HAL calls these through the PLT: this library,
 * preloaded, comes first), with the config it started it with (libacehal_led_interface.so acs_start_anim: 20 bytes of
 * values, no pointers), and turned into the slot that holds the frame; under one lock with the updates, so a LIVE
 * ledcontroller stopped (one of the Echo's own came) is never started again by a frame */
#define CFG 20
#define LIVE "hassmic_live"
#define LIVE_B "hassmic_live_b"
static pthread_mutex_t live_lock = PTHREAD_MUTEX_INITIALIZER;
static int live_on, live_cur;                   /* ledcontroller started LIVE; which of the two holds the frame */
static unsigned char live_cfg[CFG];
static int (*real_start)(const char *, const void *);
static int (*real_stop)(const char *);
static int (*real_stop_all)(void);
static const char *live_slot(int k) { return k ? LIVE_B : LIVE; }

int aceLedMgr_startAnimationWithConfig(const char *name, const void *cfg)
{
    if (!real_start) real_start = (int (*)(const char *, const void *))dlsym(RTLD_NEXT, "aceLedMgr_startAnimationWithConfig");
    if (!real_start) return -1;
    if (!name || strcmp(name, LIVE) || !cfg) return real_start(name, cfg);
    pthread_mutex_lock(&live_lock);
    int rc = real_start(live_slot(live_cur), cfg);
    if (!rc) { memcpy(live_cfg, cfg, CFG); live_on = 1; }
    pthread_mutex_unlock(&live_lock);
    return rc;
}

int aceLedMgr_stopAnimation(const char *name)
{
    if (!real_stop) real_stop = (int (*)(const char *))dlsym(RTLD_NEXT, "aceLedMgr_stopAnimation");
    if (!real_stop) return -1;
    if (name && !strcmp(name, LIVE_B)) return 0;    /* only the swap here starts and stops it: not ledcontroller's to stop */
    if (!name || strcmp(name, LIVE)) return real_stop(name);
    pthread_mutex_lock(&live_lock);
    live_on = 0;
    int rc = real_stop(live_slot(live_cur));
    real_stop(live_slot(!live_cur));                /* not playing (an error there, maybe): what counts is the shown one's */
    pthread_mutex_unlock(&live_lock);
    return rc;
}

int aceLedMgr_stopAllAnimations(void)
{
    if (!real_stop_all) real_stop_all = (int (*)(void))dlsym(RTLD_NEXT, "aceLedMgr_stopAllAnimations");
    if (!real_stop_all) return -1;
    pthread_mutex_lock(&live_lock);
    live_on = 0;
    int rc = real_stop_all();
    pthread_mutex_unlock(&live_lock);
    return rc;
}

/* an update; LIVE's goes into the slot not showing, which then takes over (see above) */
static int update_live(const char *name, const void *data)
{
    if (strcmp(name, LIVE)) return update(name, data);
    pthread_mutex_lock(&live_lock);
    int rc;
    if (!live_on || !real_start || !real_stop) rc = update(live_slot(live_cur), data);     /* shown when ledcontroller starts it */
    else {
        int nxt = !live_cur;
        unsigned char cfg[CFG]; memcpy(cfg, live_cfg, CFG);
        if (!(rc = update(live_slot(nxt), data))) {
            int s = real_start(live_slot(nxt), cfg);
            if (s) __android_log_print(ANDROID_LOG_WARN, TAG, "%s: not started: %d", live_slot(nxt), s);
            else { real_stop(live_slot(live_cur)); live_cur = nxt; }
        }
    }
    pthread_mutex_unlock(&live_lock);
    return rc;
}

/* "?leds": what each LED shows now, as the LED manager reports it (aceLedMgr_getLedState(led, out), message 0x13 to
 * the SCP, 5 bytes back a LED): "0" and a space and 10 hex digits per LED, else the error.  Read only */
static int leds_reply(char *r, size_t cap)
{
    if (!get_state) return snprintf(r, cap, "-1003");
    int w = snprintf(r, cap, "0");
    for (int i = 0; i < 12 && (size_t)w + 12 < cap; i++) {
        unsigned char st[8] = { 0 };
        int rc = get_state((uint8_t)i, st);
        if (rc) return snprintf(r, cap, "%d", rc);
        w += snprintf(r + w, cap - (size_t)w, " %02X%02X%02X%02X%02X", st[0], st[1], st[2], st[3], st[4]);
    }
    return w;
}

static void *client(void *arg)
{
    int c = (int)(intptr_t)arg;
    unsigned char *m = malloc(65536);
    ssize_t k;
    while (m && (k = recv(c, m, 65536 - 1, 0)) > 0) {
        size_t nl = strnlen((char *)m, (size_t)k);
        int rc, w; char r[160];
        if (nl < (size_t)k && !strcmp((char *)m, "?leds")) w = leds_reply(r, sizeof r);
        else {
            if (nl >= (size_t)k || nl > 30 || strncmp((char *)m, "hassmic_", 8) || !strcmp((char *)m, LIVE_B)) rc = -1001;   /* LIVE_B: the swap's */
            else if (!valid(m + nl + 1, (size_t)k - nl - 1)) rc = -1002;
            else rc = update_live((char *)m, m + nl + 1);
            if (rc) __android_log_print(ANDROID_LOG_WARN, TAG, "%.*s: %d", (int)(nl < 40 ? nl : 40), (char *)m, rc);
            w = snprintf(r, sizeof r, "%d", rc);
        }
        if (send(c, r, (size_t)w, MSG_NOSIGNAL) < 0) break;
    }
    free(m);
    close(c);
    clients--;
    return NULL;
}

static void *serve(void *arg)
{
    (void)arg;
    const char *e = getenv("HASSMIC_LED_UID"), *name = "hassmic-led";
    char *end = NULL;
    unsigned long v = e && *e ? strtoul(e, &end, 10) : 0;
    uid_t allowed = end && !*end ? (uid_t)v : (uid_t)-1;   /* unset or not a number: nobody, never root by default */
    update = (int (*)(const char *, const void *))dlsym(RTLD_DEFAULT, "aceLedMgr_updateAnimationData");
    get_state = (int (*)(uint8_t, void *))dlsym(RTLD_DEFAULT, "aceLedMgr_getLedState");
    struct sockaddr_un u = { .sun_family = AF_UNIX };
    int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    memcpy(u.sun_path + 1, name, strlen(name));
    if (!update || s < 0 || bind(s, (struct sockaddr *)&u, offsetof(struct sockaddr_un, sun_path) + 1 + strlen(name)) || listen(s, 4)) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "not serving: %s", update ? "socket" : "no aceLedMgr_updateAnimationData");
        return NULL;
    }
    __android_log_print(ANDROID_LOG_INFO, TAG, "serving uid %u", (unsigned)allowed);
    for (;;) {
        int c = accept4(s, NULL, NULL, SOCK_CLOEXEC);
        if (c < 0) { usleep(100000); continue; }                         /* EMFILE and the like: no spin */
        struct ucred cr; socklen_t cl = sizeof cr;
        /* No idle timeout: hassmic keeps its connection for good and takes its end hanging up for ledcontroller gone
         * (ring.c shim_check: what was drawn, drawn again).  A still light sends nothing for hours; the 30 s this
         * had dropped it and looked like a restart.  Only the daemon's uid gets here, and a hassmic that ends closes.
         * A thread a client: the LED manager's calls take its own lock (aceMutex in libace_ledmgr.so) */
        if (getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cr, &cl) || cr.uid != allowed || clients >= CLIENTS_MAX) { close(c); continue; }
        pthread_t t;
        clients++;
        if (pthread_create(&t, NULL, client, (void *)(intptr_t)c)) { clients--; close(c); continue; }
        pthread_detach(t);
    }
}

__attribute__((constructor)) static void start(void)
{
    pthread_t t;
    if (!pthread_create(&t, NULL, serve, NULL)) pthread_detach(t);
}
