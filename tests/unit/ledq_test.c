/* ledq.c against a stand-in ledctrl (a shell script logging its calls): a slow set is not overtaken by the unset after
 * it (issue #40), chat on stdout of any length does not end the wait early and stays out of the log while stderr
 * reaches it, a hanging call is killed after the timeout and the next one runs, a full queue gives up a set and never
 * an unset, the fake file of the PC tests gets every call in order.  SIGCHLD ignored, as in hassmic. */
#include "ledq.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char dir[64], script[96], log_[96], err[96], fake[96];
static int fails;

static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }
static char *slurp(const char *f, char *buf, size_t n)
{
    FILE *fp = fopen(f, "r"); size_t l = 0;
    if (fp) { l = fread(buf, 1, n - 1, fp); fclose(fp); }
    buf[l] = 0; return buf;
}
static void empty(const char *f) { FILE *fp = fopen(f, "w"); if (fp) fclose(fp); }
static long long ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }

int main(void)
{
    char buf[8192], line[64];
    signal(SIGCHLD, SIG_IGN);
    snprintf(dir, sizeof dir, "/tmp/ledq_test.%d", (int)getpid()); mkdir(dir, 0700);
    snprintf(script, sizeof script, "%s/ledctrl", dir); snprintf(log_, sizeof log_, "%s/calls", dir);
    snprintf(err, sizeof err, "%s/stderr", dir); snprintf(fake, sizeof fake, "%s/fake", dir);
    FILE *f = fopen(script, "w");
    fprintf(f, "#!/bin/sh\n"
               "case \"$2\" in\n"
               "  slow) sleep 0.3 ;;\n"
               "  chatty) i=0; while [ $i -lt 400 ]; do echo \"ledctrl chat line $i\"; i=$((i+1)); done; sleep 0.2 ;;\n"
               "  hang) exec sleep 10 ;;\n"
               "  oops) echo \"ledctrl: no such pattern\" >&2 ;;\n"
               "esac\n"
               "echo \"$1 $2\" >> %s\n", log_);
    fclose(f); chmod(script, 0700);
    fflush(stderr); int saved = dup(2); freopen(err, "w", stderr); setvbuf(stderr, NULL, _IONBF, 0);

    ledq_start(script, NULL, NULL, 500);
    ledq_run("-s", "slow"); ledq_run("-u", "slow"); ledq_run("-s", "after");
    check(!ledq_wait(3000), "three calls ran");
    check(!strcmp(slurp(log_, buf, sizeof buf), "-s slow\n-u slow\n-s after\n"), "a slow set is not overtaken by its unset");

    empty(log_);
    ledq_run("-s", "chatty"); ledq_run("-u", "chatty");
    check(!ledq_wait(3000), "chatty calls ran");
    check(!strcmp(slurp(log_, buf, sizeof buf), "-s chatty\n-u chatty\n"), "16 kB of chat on stdout: still waited for");

    empty(log_);
    long long t0 = ms();
    ledq_run("-s", "hang"); ledq_run("-u", "next");
    check(!ledq_wait(3000), "a hanging call ends");
    long long took = ms() - t0;
    check(took >= 450 && took < 1500, "killed after the timeout (500 ms)");
    check(!strcmp(slurp(log_, buf, sizeof buf), "-u next\n"), "the call after it ran");

    empty(log_);
    ledq_run("-s", "oops");
    check(!ledq_wait(3000), "a failing call ran");

    empty(log_);
    ledq_run("-s", "slow");
    usleep(100000);                                 /* taken off the queue and running: the queue is empty again */
    for (int i = 0; i < LEDQ_MAX - 1; i++) { snprintf(line, sizeof line, "s%d", i); ledq_run("-s", line); }
    ledq_run("-u", "u0");                           /* the queue full */
    ledq_run("-u", "u1");                           /* s0 gives way */
    check(!ledq_wait(10000), "a full queue ran");
    slurp(log_, buf, sizeof buf);
    check(!strstr(buf, "-s s0\n") && strstr(buf, "-s s1\n") && strstr(buf, "-u u0\n-u u1\n"), "full: the oldest set dropped, both unsets kept, in order");

    ledq_start(NULL, fake, NULL, 500);
    ledq_run("-s", "a"); ledq_run("-u", "a"); ledq_run("-s", "b");
    check(!ledq_wait(3000), "fake: calls ran");
    check(!strcmp(slurp(fake, buf, sizeof buf), "-s a\n-u a\n-s b\n"), "fake: every call in the file, in order");

    fflush(stderr); dup2(saved, 2);
    slurp(err, buf, sizeof buf);
    check(strstr(buf, "ledctrl: no such pattern") != NULL, "ledctrl's stderr reaches the log");
    check(strstr(buf, "chat line") == NULL, "its stdout does not");
    check(strstr(buf, "-s hang still running after 500 ms, killed") != NULL, "the kill is logged");
    check(strstr(buf, "-s s0 dropped") != NULL, "the dropped set is logged");

    unlink(script); unlink(log_); unlink(err); unlink(fake); rmdir(dir);
    printf("ledq: %s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
