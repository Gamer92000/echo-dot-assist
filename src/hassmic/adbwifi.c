/*
 * adb over Wi-Fi is a root shell without a password (the unlock turns adbd's key check off), so an installed Echo keeps
 * it closed: lockdown.sh runs adbd without its TCP listener and takes stock's firewall rule for port 5555 out.  For
 * debugging it can be opened for half an hour from Home Assistant.  hassmic is unprivileged and faces the network, so as
 * with push updates (ota.c) it only asks: a request file in state/, which root's firewall watcher takes within 5 s.  How
 * long the window lasts and when it ends is that side's business (a reboot always ends it); it answers with the file
 * adb-open in a directory only root writes, which exists exactly while the port is open.
 */
#include "adbwifi.h"
#include "statefile.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define ANSWER_SECS 15          /* the watcher looks every 5 s; no answer after this long: nobody is there */

static atomic_int asked = -1;   /* request not answered yet: 0 / 1 */
static time_t asked_at;
static void (*on_change)(void);

static const char *req_path(void)
{
    static char p[300];
    const char *e = getenv("HASSMIC_STATE");
    if (!p[0]) snprintf(p, sizeof p, "%s/adb-request", e ? e : "/data/local/hassmic/state");
    return p;
}
static const char *open_path(void) { const char *e = getenv("HASSMIC_ADB_OPEN"); return e ? e : "/data/local/hassmic/adb-open"; }
static time_t now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec; }

int adbwifi_open(void)
{
    int a = atomic_load(&asked);
    return a >= 0 ? a : access(open_path(), F_OK) == 0;
}

int adbwifi_granted(void) { return access(req_path(), F_OK) && !access(open_path(), F_OK); }

void adbwifi_ask(int on)
{
    if (statefile_write(req_path(), 0666, on ? "1\n" : "0\n", 2)) {
        fprintf(stderr, "adb over Wi-Fi: cannot write %s\n", req_path());
        return;
    }
    asked_at = now();
    atomic_store(&asked, on != 0);
    fprintf(stderr, "adb over Wi-Fi: asked to %s it\n", on ? "open" : "close");
}

static void *watcher(void *arg)
{
    int last = adbwifi_open();
    (void)arg;
    for (;;) {
        sleep(2);
        if (atomic_load(&asked) >= 0) {
            if (access(req_path(), F_OK)) atomic_store(&asked, -1);             /* taken: adb-open is the answer */
            else if (now() - asked_at > ANSWER_SECS) {
                unlink(req_path()); atomic_store(&asked, -1);
                fprintf(stderr, "adb over Wi-Fi: no answer (is the hassmic_fw service running?)\n");
            }
        }
        int o = adbwifi_open();
        if (o == last) continue;
        last = o;                                   /* the firewall service logs the opening and closing itself */
        if (on_change) on_change();
    }
    return NULL;
}

void adbwifi_start(void (*changed)(void))
{
    pthread_t t;
    on_change = changed;
    if (!pthread_create(&t, NULL, watcher, NULL)) pthread_detach(t);
}
