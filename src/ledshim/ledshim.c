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
 * there: a thread serving an abstract socket "@hassmic-led" (SOCK_SEQPACKET), one request a datagram,
 * "<name>\0<animation_bin>", the answer the call's result as text.
 *
 * Guarded: a peer of uid HASSMIC_LED_UID only (root's main.sh gives the daemon's; SO_PEERCRED), names "hassmic_..." only
 * (stock's are never changed), and the data checked as the HAL and the SCP will walk it (ring.c acs_encode makes it:
 * "animagic" "totl" <frames>, each optionally after "loop", "fram" <ms> and 12 pixels; the call takes no length).
 */
#define _GNU_SOURCE
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
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

static void *serve(void *arg)
{
    (void)arg;
    const char *e = getenv("HASSMIC_LED_UID"), *name = "hassmic-led";
    char *end = NULL;
    unsigned long v = e && *e ? strtoul(e, &end, 10) : 0;
    uid_t allowed = end && !*end ? (uid_t)v : (uid_t)-1;   /* unset or not a number: nobody, never root by default */
    int (*update)(const char *, const void *) = (int (*)(const char *, const void *))dlsym(RTLD_DEFAULT, "aceLedMgr_updateAnimationData");
    struct sockaddr_un u = { .sun_family = AF_UNIX };
    int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    memcpy(u.sun_path + 1, name, strlen(name));
    if (!update || s < 0 || bind(s, (struct sockaddr *)&u, offsetof(struct sockaddr_un, sun_path) + 1 + strlen(name)) || listen(s, 4)) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "not serving: %s", update ? "socket" : "no aceLedMgr_updateAnimationData");
        return NULL;
    }
    __android_log_print(ANDROID_LOG_INFO, TAG, "serving uid %u", (unsigned)allowed);
    static unsigned char m[65536];
    for (;;) {
        int c = accept4(s, NULL, NULL, SOCK_CLOEXEC);
        if (c < 0) { usleep(100000); continue; }                         /* EMFILE and the like: no spin */
        struct ucred cr; socklen_t cl = sizeof cr;
        struct timeval tv = { 30, 0 };          /* a client that stops talking is dropped, the next may come */
        if (getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cr, &cl) || cr.uid != allowed) { close(c); continue; }
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        ssize_t k;
        while ((k = recv(c, m, sizeof m - 1, 0)) > 0) {
            size_t nl = strnlen((char *)m, (size_t)k);
            int rc;
            if (nl >= (size_t)k || nl > 30 || strncmp((char *)m, "hassmic_", 8)) rc = -1001;
            else if (!valid(m + nl + 1, (size_t)k - nl - 1)) rc = -1002;
            else rc = update((char *)m, m + nl + 1);
            if (rc) __android_log_print(ANDROID_LOG_WARN, TAG, "%.*s: %d", (int)(nl < 40 ? nl : 40), (char *)m, rc);
            char r[16]; int w = snprintf(r, sizeof r, "%d", rc);
            if (send(c, r, (size_t)w, MSG_NOSIGNAL) < 0) break;
        }
        close(c);
    }
}

__attribute__((constructor)) static void start(void)
{
    pthread_t t;
    if (!pthread_create(&t, NULL, serve, NULL)) pthread_detach(t);
}
