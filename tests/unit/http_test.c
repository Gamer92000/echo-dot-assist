/* http.c and tls.c from the command line, for tests/unit/http_test.py: one request, the body into -o, then one
 * line "rc=<http_run's> status=<n> bytes=<n> err=<words>".
 *   http_test [-k] [-n redirects] [-S] [-P] [-L] [-H header]... [-d body] [-c connect_s] [-t total_s]
 *             [-l low_s:low_bytes] [-e end_after_bytes] [-x stop_after_ms] [-D body_file] [-w first_piece_ms] -o file URL
 * -k: no certificate check; -S: redirects to https:// only; -P: http:// only; -L: a plain socket */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "http.h"

struct out { FILE *f; long long n, end_after; long hold_ms; };

static int on_data(void *arg, const void *d, size_t n)
{
    struct out *o = arg;
    if (o->hold_ms > 0) { usleep((useconds_t)o->hold_ms * 1000); o->hold_ms = 0; }   /* a reader that holds the first piece back */
    fwrite(d, 1, n, o->f); o->n += (long long)n;
    return o->end_after >= 0 && o->n >= o->end_after;
}

static volatile int stop;
static void *stopper(void *arg) { usleep((useconds_t)(long)arg * 1000); stop = 1; return NULL; }

int main(int argc, char **argv)
{
    struct http r = { .verify = 1, .connect_s = 10, .agent = "hassmic-test" };
    const char *hdrs[16]; int nh = 0, o; const char *path = NULL; struct out out = { .end_after = -1 }; long stop_ms = -1;
    while ((o = getopt(argc, argv, "kn:SPLH:d:c:t:l:e:x:o:D:w:")) != -1) switch (o) {
    case 'D': {
        FILE *f = fopen(optarg, "rb"); static char *b; long l;
        if (!f || fseek(f, 0, SEEK_END) || (l = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) || !(b = malloc((size_t)l + 1)) || fread(b, 1, (size_t)l, f) != (size_t)l) return 2;
        fclose(f); r.body = b; r.body_len = (size_t)l; break;
    }
    case 'w': out.hold_ms = atol(optarg); break;
    case 'k': r.verify = 0; break;
    case 'n': r.redirects = atoi(optarg); break;
    case 'S': r.redirect_https = 1; break;
    case 'P': r.plain = 1; break;
    case 'L': r.lan = 1; break;
    case 'H': if (nh < 15) hdrs[nh++] = optarg; break;
    case 'd': r.body = optarg; r.body_len = strlen(optarg); break;
    case 'c': r.connect_s = atoi(optarg); break;
    case 't': r.total_s = atoi(optarg); break;
    case 'l': r.low_s = atoi(optarg); r.low_bytes = atol(strchr(optarg, ':') ? strchr(optarg, ':') + 1 : "1"); break;
    case 'e': out.end_after = atoll(optarg); break;
    case 'x': stop_ms = atol(optarg); break;
    case 'o': path = optarg; break;
    default: return 2;
    }
    if (optind >= argc || !path || !(out.f = fopen(path, "wb"))) return 2;
    hdrs[nh] = NULL; if (nh) r.headers = hdrs;
    r.url = argv[optind]; r.data = on_data; r.arg = &out;
    if (stop_ms >= 0) { pthread_t t; r.stop = &stop; pthread_create(&t, NULL, stopper, (void *)stop_ms); pthread_detach(t); }
    char err[300];
    int rc = http_run(&r, err, sizeof err);
    fclose(out.f);
    printf("rc=%d status=%d bytes=%lld err=%s\n", rc, r.status, out.n, err);
    return 0;
}
