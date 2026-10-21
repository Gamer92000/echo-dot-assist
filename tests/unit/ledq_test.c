/* ledq.c against a stand-in ledctrl (a shell script logging its calls): a slow set is not overtaken by the unset after
 * it (issue #40), chat on stdout of any length does not end the wait early and stays out of the log while stderr
 * reaches it, a hanging call is killed with what it started after the timeout and the next one runs, a full queue
 * gives up a set and never an unset, brightness calls keep their order with the ring's, the fake file of the PC tests
 * gets every call at once, sets and unsets queued together go in one ledctrl and a batch killed has its unsets
 * run again (issue #57).  SIGCHLD ignored, as in hassmic.  Timeouts generous where nothing should hang: CI is slow. */
#include "ledq.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char dir[64] = "/tmp/ledq_test.XXXXXX", script[96], log_[96], raw[96], err[96], fake[96], mark[96], orphan[96];
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
static int appeared(const char *f, int within)      /* polls for the stand-in's marker */
{
    for (long long until = ms() + within; ms() < until; usleep(5000)) if (!access(f, F_OK)) return 1;
    return 0;
}

int main(void)
{
    char buf[8192], line[128];
    signal(SIGCHLD, SIG_IGN);
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    snprintf(script, sizeof script, "%s/ledctrl", dir); snprintf(log_, sizeof log_, "%s/calls", dir);
    snprintf(err, sizeof err, "%s/stderr", dir); snprintf(fake, sizeof fake, "%s/fake", dir);
    snprintf(mark, sizeof mark, "%s/started", dir); snprintf(orphan, sizeof orphan, "%s/orphan", dir);
    snprintf(raw, sizeof raw, "%s/runs", dir);
    FILE *f = fopen(script, "w");
    if (!f) { perror(script); return 1; }
    /* one line per run in `raw`, one per pattern (or other call) in `log_`, as ledctrl's getopt loop takes them */
    fprintf(f, "#!/bin/sh\n"
               "echo \"$*\" >> %s\n"
               "while [ $# -gt 0 ]; do\n"
               "  case \"$1\" in -s|-u) a=\"$1 $2\"; p=$2; shift 2 ;; *) a=\"$*\"; p=; shift $# ;; esac\n"
               "  case \"$p\" in\n"
               "    slow) touch %s; sleep 0.3 ;;\n"
               "    chatty) i=0; while [ $i -lt 400 ]; do echo \"ledctrl chat line $i\"; i=$((i+1)); done; sleep 0.2 ;;\n"
               "    hang) touch %s; exec sleep 10 ;;\n"
               "    orphan) (sleep 0.8; touch %s) & exit 0 ;;\n"
               "    oops) echo \"ledctrl: no such pattern\" >&2 ;;\n"
               "  esac\n"
               "  echo \"$a\" >> %s\n"
               "done\n", raw, mark, mark, orphan, log_);
    fclose(f); chmod(script, 0700);
    fflush(stderr);
    int saved = dup(2);
    if (saved < 0 || !freopen(err, "w", stderr)) { perror("stderr to a file"); return 1; }
    setvbuf(stderr, NULL, _IONBF, 0);

    ledq_start(script, NULL, NULL, 5000);
    ledq_run("-s", "slow"); ledq_run("-u", "slow"); ledq_run("-s", "after");
    check(!ledq_wait(10000), "three calls ran");
    check(!strcmp(slurp(log_, buf, sizeof buf), "-s slow\n-u slow\n-s after\n"), "a slow set is not overtaken by its unset");

    empty(log_);
    ledq_run("-s", "chatty"); ledq_run("-u", "chatty");
    check(!ledq_wait(10000), "chatty calls ran");
    check(!strcmp(slurp(log_, buf, sizeof buf), "-s chatty\n-u chatty\n"), "16 kB of chat on stdout: still waited for");

    empty(log_);
    const char *bright[] = { "-a", "off", "-b", "40" };
    ledq_run("-s", "slow"); ledq_args(4, bright); ledq_run("-a", "on");
    check(!ledq_wait(10000), "brightness calls ran");
    check(!strcmp(slurp(log_, buf, sizeof buf), "-s slow\n-a off -b 40\n-a on\n"), "brightness in order, behind the ring's");

    empty(log_); empty(raw); unlink(mark);
    ledq_run("-s", "slow");
    check(appeared(mark, 5000), "slow running");
    ledq_run("-s", "a"); ledq_run("-u", "b"); ledq_run("-s", "c"); ledq_args(4, bright); ledq_run("-s", "d");
    check(!ledq_wait(10000), "queued calls ran");
    check(!strcmp(slurp(raw, buf, sizeof buf), "-s slow\n-s a -u b -s c\n-a off -b 40\n-s d\n")
          && !strcmp(slurp(log_, line, sizeof line), "-s slow\n-s a\n-u b\n-s c\n-a off -b 40\n-s d\n"),
          "sets and unsets queued together in one ledctrl, in order; brightness on its own");

    ledq_start(script, NULL, NULL, 500);
    empty(log_); unlink(mark);
    long long t0 = ms();
    ledq_run("-s", "hang");                         /* alone: a call queued behind it would go in the same ledctrl */
    check(appeared(mark, 5000), "hang running");
    ledq_run("-u", "next");
    check(!ledq_wait(10000), "a hanging call ends");
    long long took = ms() - t0;
    check(took >= 450 && took < 5000, "killed after the timeout (500 ms)");
    check(!strcmp(slurp(log_, buf, sizeof buf), "-u next\n"), "the call after it ran");

    empty(log_); unlink(mark);                      /* a batch that hangs: its unsets again, alone */
    ledq_run("-s", "slow");
    check(appeared(mark, 5000), "slow running");
    ledq_run("-u", "gone"); ledq_run("-s", "hang"); ledq_run("-u", "after"); ledq_run("-s", "lost");
    check(!ledq_wait(10000), "a hanging batch ends");
    check(!strcmp(slurp(log_, buf, sizeof buf), "-s slow\n-u gone\n-u gone\n-u after\n"),
          "killed with the batch: both unsets again, each alone; the set after the hang lost");

    ledq_run("-s", "orphan");                       /* exits at once, a child of it keeps the pipe */
    check(!ledq_wait(10000), "a call that left a child behind ends");
    usleep(1200000);
    check(access(orphan, F_OK) != 0, "what it left behind was killed with it (its process group)");

    ledq_start(script, NULL, NULL, 5000);
    empty(log_);
    ledq_run("-s", "oops");
    check(!ledq_wait(10000), "a failing call ran");

    empty(log_); unlink(mark);
    ledq_run("-s", "slow");
    check(appeared(mark, 5000), "slow running: the queue empty again");
    for (int i = 0; i < LEDQ_MAX - 1; i++) { snprintf(line, sizeof line, "s%d", i); ledq_run("-s", line); }
    ledq_run("-u", "u0");                           /* the queue full */
    ledq_run("-u", "u1");                           /* s0 gives way */
    check(!ledq_wait(30000), "a full queue ran");
    slurp(log_, buf, sizeof buf);
    check(!strstr(buf, "-s s0\n") && strstr(buf, "-s s1\n") && strstr(buf, "-u u0\n-u u1\n"), "full: the oldest set dropped, both unsets kept, in order");

    ledq_start(script, fake, NULL, 5000);
    ledq_run("-s", "a"); ledq_run("-u", "a"); ledq_args(4, bright);
    check(!strcmp(slurp(fake, buf, sizeof buf), "-s a\n-u a\n-a off -b 40\n"), "fake: every call in the file at once, in order");
    check(!ledq_wait(1000), "fake: nothing queued");

    fflush(stderr); dup2(saved, 2);
    slurp(err, buf, sizeof buf);
    check(strstr(buf, "ledctrl: no such pattern") != NULL, "ledctrl's stderr reaches the log");
    check(strstr(buf, "chat line") == NULL, "its stdout does not");
    check(strstr(buf, "-s hang still running after 500 ms, killed") != NULL, "the kill is logged");
    check(strstr(buf, "-s s0 dropped") != NULL, "the dropped set is logged");

    const char *files[] = { script, log_, raw, err, fake, mark, orphan };
    for (unsigned i = 0; i < sizeof files / sizeof *files; i++) unlink(files[i]);
    rmdir(dir);
    printf("ledq: %s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
