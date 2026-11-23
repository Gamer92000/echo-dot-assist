/*
 * libcurl from the firmware, loaded once (fwcurl.h).
 *
 * One loader, with the lock in it, because the first use is not for two threads at once: dlopen and the dlsyms
 * fill a table the others read, and libcurl sets itself up inside the first curl_easy_init (curl_global_init, which
 * its own documentation calls not thread safe: OpenSSL's tables among it).  update.c, https.c and davs.c each had a
 * loader of their own, with one lock (update.c), a pthread_once (https.c) and none (davs.c): three threads that had
 * never used libcurl could each be inside that first init, one for an update's look, one for a reply's audio over
 * HTTPS, one for Amazon's login (a review, 2026-11-19).
 *
 * A load that failed is tried again, RETRY_MS after the last try at the earliest.  It was tried once in a process's
 * life: a dlopen that failed for want of memory at one moment (the library and BoringSSL are megabytes to map) left
 * the Echo without online updates, HTTPS media and Amazon's downloads until hassmic was started again, with one line
 * in the log from hours before.  Not at every call: a reply's audio asks per URL, and an Echo that has no libcurl at
 * all would search the library path and write the log each time.
 */
#include "fwcurl.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

#define RETRY_MS 60000

static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

const struct fwcurl *fwcurl(void)
{
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    static struct fwcurl f; static _Atomic(const struct fwcurl *) loaded; static long long tried;   /* when, 0 never */
    static int wrong;       /* loaded, and not the library hassmic knows: that does not change, never tried again */
    const struct fwcurl *r = atomic_load(&loaded);
    void *h, *first; long long now;
    if (r) return r;
    pthread_mutex_lock(&m);
    now = now_ms();
    if (!atomic_load(&loaded) && !wrong && (!tried || now - tried >= RETRY_MS)) {
        char e1[200] = "";
        tried = now ? now : 1;
        /* (dlerror's text is the last call's: the first one's kept before the second is made) */
        if (!(h = dlopen("libcurl.so", RTLD_NOW))) { const char *e = dlerror(); snprintf(e1, sizeof e1, "%s", e ? e : "?"); }
        if (!h && !(h = dlopen("libcurl.so.4", RTLD_NOW))) {                                     /* Echo; PC */
            const char *e = dlerror();
            fprintf(stderr, "libcurl: not loaded (libcurl.so: %s; libcurl.so.4: %s): no HTTPS; tried again in %d s at the earliest\n", e1, e ? e : "?", RETRY_MS / 1000);
        } else {
            f.init = (void *(*)(void))dlsym(h, "curl_easy_init");
            f.setopt = (int (*)(void *, int, ...))dlsym(h, "curl_easy_setopt");
            f.perform = (int (*)(void *))dlsym(h, "curl_easy_perform");
            f.cleanup = (void (*)(void *))dlsym(h, "curl_easy_cleanup");
            f.getinfo = (int (*)(void *, int, ...))dlsym(h, "curl_easy_getinfo");
            f.strerror = (const char *(*)(int))dlsym(h, "curl_easy_strerror");
            f.slist_append = (struct curl_slist *(*)(struct curl_slist *, const char *))dlsym(h, "curl_slist_append");
            f.slist_free_all = (void (*)(struct curl_slist *))dlsym(h, "curl_slist_free_all");
            if (!f.init || !f.setopt || !f.perform || !f.cleanup || !f.getinfo || !f.strerror || !f.slist_append || !f.slist_free_all)
                { fprintf(stderr, "libcurl: not the library hassmic knows (a function is missing): no HTTPS\n"); wrong = 1; }
            else {
                /* the library's own first init, here and under the lock: every handle after it finds it done */
                if ((first = f.init())) f.cleanup(first);
                atomic_store(&loaded, &f);
            }
        }
    }
    r = atomic_load(&loaded);
    pthread_mutex_unlock(&m);
    return r;
}
