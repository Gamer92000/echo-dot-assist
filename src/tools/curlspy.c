// LD_PRELOAD shim: log what a stock daemon asks libcurl for (URL, method, request headers, body), before TLS hides it.
// Made for assetmgrd (DAVS artifact downloads, e.g. wake-word models): its libcurl 7.50.1 has no SSLKEYLOGFILE, so a
// packet capture only ever shows host names.
//   CURLSPY_LOG=/data/local/tmp/curlspy.log LD_PRELOAD=/data/local/tmp/libcurlspy.so /system/bin/assetmgrd
// The log holds the device's bearer token: keep it out of git.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

struct slist { char *data; struct slist *next; };       // curl_slist

enum { OPT_URL = 10002, OPT_POSTFIELDS = 10015, OPT_HTTPHEADER = 10023, OPT_CUSTOMREQUEST = 10036, OPT_OFF_T = 30000 };

static void say(void *h, const char *what, const char *s)
{
    const char *path = getenv("CURLSPY_LOG");
    FILE *f = fopen(path ? path : "/data/local/tmp/curlspy.log", "a");
    if (!f) return;
    fprintf(f, "%ld pid=%d h=%p %s %s\n", (long)time(NULL), getpid(), h, what, s ? s : "(null)");
    fclose(f);
}

int curl_easy_setopt(void *h, int opt, ...)
{
    static int (*real)(void *, int, ...);
    va_list ap;
    int rc;
    if (!real) real = dlsym(RTLD_NEXT, "curl_easy_setopt");
    va_start(ap, opt);
    if (opt >= OPT_OFF_T) {                             // curl_off_t is 64 bit, everything else is long or a pointer
        rc = real(h, opt, va_arg(ap, long long));
    } else {
        void *p = va_arg(ap, void *);
        if (opt == OPT_URL) say(h, "URL", p);
        else if (opt == OPT_CUSTOMREQUEST) say(h, "METHOD", p);
        else if (opt == OPT_POSTFIELDS) say(h, "BODY", p);
        else if (opt == OPT_HTTPHEADER) for (struct slist *l = p; l; l = l->next) say(h, "HEADER", l->data);
        rc = real(h, opt, p);
    }
    va_end(ap);
    return rc;
}
