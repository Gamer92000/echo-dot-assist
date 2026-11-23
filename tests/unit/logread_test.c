/* logread.c: what a plugin with log.read gets of a line (docs/plugins.md 4.5), and where in the log it is (a cursor
 * that survives main.sh's rotation by copy and truncate).  Under ASan and UBSan. */
#include "logread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s (line %d)\n", what, __LINE__); fails++; } else printf("ok   %s\n", what); } while (0)

static int wake_is(const char *id, size_t n, void *arg) { (void)arg; return n == 4 && !memcmp(id, "wake", 4); }

/* the line as a plugin gets it, "" when it is left out */
static const char *pub(const char *line)
{
    static char b[1100]; size_t n = strlen(line);
    memcpy(b, line, n + 1);
    if (!logread_public(b, &n, wake_is, NULL)) return "";
    b[n] = 0;
    return b;
}
/* ... with log.read.full: the page's view but for a wake word plugin's lines */
static const char *full(const char *line)
{
    static char b[1100]; size_t n = strlen(line);
    memcpy(b, line, n + 1);
    if (!logread_full(b, &n, wake_is, NULL)) return "";
    b[n] = 0;
    return b;
}

static char dir[200], path[260], path1[270];
static void put(const char *p, const char *mode, const char *text) { FILE *f = fopen(p, mode); fputs(text, f); fclose(f); }
static void rotate(void)                                    /* main.sh rotate_log: cp, then : > */
{
    char cmd[900]; snprintf(cmd, sizeof cmd, "cp '%s' '%s' && : > '%s'", path, path1, path); if (system(cmd)) abort();
}

int main(void)
{
    static char lines[64][1024]; struct logpos at, next; int gap, n; char cur[41], big[3000];
    /* ---- the filter */
    CHECK(!strcmp(pub("2026-11-21 10:00:00.123Z hello world"), "2026-11-21 10:00:00.123Z hello world"), "hassmic's own line (its stamp has milliseconds): kept");
    CHECK(!strcmp(pub("boot+12.345 hello"), "boot+12.345 hello"), "... before the clock is set: kept");
    CHECK(!*pub("2026-11-21 10:00:00.123Z " LOG_PRIVATE_MARK "transcript: turn on the light") && !*pub("boot+12.345 " LOG_PRIVATE_MARK "reply: done"), "a line marked private: left out");
    CHECK(!*pub("boot+12.345 " LOG_SEALED_MARK "plugin gone: a wake plugin's line, written while it was one"), "a line marked sealed: left out, whatever is installed now");
    /* a mark that is not at the start of the text is a marked line joined to another (a write that lost its end in a
     * full pipe, S5 of a review of 08399dc): the whole line goes, never the marked text in it */
    CHECK(!*pub("boot+12.345 media: a cut line that lost its end(private) transcript: what was said") && !*pub("boot+12.345 x" LOG_SEALED_MARK "y") && !*pub("2026-11-21 10:00:00Z == x (private) y"),
          "a mark anywhere in a line: left out whole");
    CHECK(!strcmp(pub("2026-11-21 10:00:00Z == update 1 installed"), "2026-11-21 10:00:00Z == update 1 installed") && *pub("boot+3.14 !! IPv6 NOT OFF") && *pub("boot+3 hassmic exited rc=1, restart in 3 s"),
          "root's events (its stamp without milliseconds): kept");
    CHECK(!*pub("2026-11-21 10:00:00Z wifi: joined MyNetwork") && !*pub("boot+3.14 dhcp: lease"), "root's other lines: left out");
    CHECK(!*pub("no stamp at all") && !*pub("") && !*pub("boot+ x") && !*pub("2026-11-21 10:00:00.12Z short"), "a line without a stamp: left out");
    CHECK(!*pub("boot+1.000 plugin wake: its line") && !*pub("boot+1.000 plugins: wake: cannot be used: a reason") && *pub("boot+1.000 plugin other: its line") && *pub("boot+1.000 plugin wakeful: x"),
          "a wake word plugin's lines and the broker's about it: left out; another plugin's kept");
    CHECK(!strcmp(pub("boot+1.000 bt aa:bb:cc:dd:ee:ff, AA-BB-CC-DD-EE-0F; a2dp 001122aabbcc."), "boot+1.000 bt **:**:**:**:**:**, **-**-**-**-**-**; a2dp ************."),
          "MAC and Bluetooth addresses blanked, colons, dashes, twelve hex digits");
    CHECK(!strcmp(pub("boot+1.000 digest 0123456789abcdef0123 and x001122aabbcc and 10.0.0.5"), "boot+1.000 digest 0123456789abcdef0123 and x001122aabbcc and 10.0.0.5"),
          "other hex runs and IPv4 addresses kept");
    CHECK(!strcmp(pub("boot+1.000 sendspin: pairing token SP:0ABCDEF234"), "boot+1.000 sendspin: pairing token SP:0*********"), "the pairing token blanked");
    /* log.read.full: what the settings page shows, what was said included; never a wake word plugin's line */
    CHECK(!strcmp(full("2026-11-21 10:00:00.123Z " LOG_PRIVATE_MARK "transcript: turn on the light"), "2026-11-21 10:00:00.123Z " LOG_PRIVATE_MARK "transcript: turn on the light")
          && !strcmp(full("boot+12.345 media: a cut line that lost its end(private) transcript: x"), "boot+12.345 media: a cut line that lost its end(private) transcript: x"),
          "log.read.full: a private line kept as the page shows it, its mark too");
    CHECK(!strcmp(full("2026-11-21 10:00:00Z wifi: joined MyNetwork"), "2026-11-21 10:00:00Z wifi: joined MyNetwork") && !strcmp(full("no stamp at all"), "no stamp at all")
          && !strcmp(full("boot+1.000 bt aa:bb:cc:dd:ee:ff"), "boot+1.000 bt aa:bb:cc:dd:ee:ff"), "log.read.full: root's lines whatever they say, a line without a stamp, addresses as they are");
    CHECK(!*full("boot+12.345 " LOG_SEALED_MARK "plugin gone: a wake plugin's line") && !*full("boot+12.345 " LOG_PRIVATE_MARK "x" LOG_SEALED_MARK "y")
          && !*full("boot+1.000 plugin wake: its line") && !*full("boot+1.000 plugins: wake: a reason") && *full("boot+1.000 plugin other: its line"),
          "log.read.full: a sealed line (the mark anywhere, behind a private one too) and a wake word plugin's left out; another plugin's kept");
    CHECK(!strcmp(full("boot+1.000 sendspin: pairing token SP:0ABCDEF234"), "boot+1.000 sendspin: pairing token SP:0*********"), "log.read.full: the pairing token blanked, as on the page");

    /* ---- the cursor */
    snprintf(dir, sizeof dir, "/tmp/logread_test.%d", (int)getpid()); mkdir(dir, 0700);
    snprintf(path, sizeof path, "%s/boot.log", dir); snprintf(path1, sizeof path1, "%s.1", path);
    setenv("HASSMIC_LOG", path, 1);
    logread_now(&at);
    CHECK(at.id == 0 && at.off == 0, "no log yet: from now is its start");
    put(path, "w", "boot+0.900 young\n");
    n = logread_lines(&at, lines, 64, 24000, &next, &gap, 0, NULL, NULL);
    CHECK(n == 0 && !gap && next.id == 0 && next.off == 0, "a log too young for a name: nothing of it read yet (no cursor points into it)");
    logread_now(&at);
    CHECK(at.id == 0 && at.off == 0, "... and from now in it, without a .1 before it, is {0, 0}");
    put(path, "w", "boot+1.000 the first line of the log, long enough to be its name: sixty-four bytes\nboot+1.001 two\n");
    n = logread_lines(&at, lines, 64, 24000, &next, &gap, 0, NULL, NULL);
    CHECK(n == 2 && !gap && next.id && next.off == (long)strlen("boot+1.000 the first line of the log, long enough to be its name: sixty-four bytes\nboot+1.001 two\n"), "from a young log's start (the same text, grown): its lines, and its name once it has one");
    logread_cursor(&next, cur, sizeof cur);
    CHECK(!logread_cursor_parse(cur, &at) && !memcmp(&at, &next, sizeof at) && strlen(cur) < 40, "a cursor as text and back");
    CHECK(logread_cursor_parse("start", &at) && logread_cursor_parse("0123:5", &at) && logread_cursor_parse("0123456789ABCDEF:5", &at) && logread_cursor_parse("0123456789abcdef:-5", &at)
          && logread_cursor_parse("0123456789abcdef:5 ", &at) && !logread_cursor_parse("0123456789abcdef:5", &at), "cursors that are none: refused");
    logread_cursor_parse(cur, &at);
    put(path, "a", "boot+1.002 three\nboot+1.003 a line still being writ");
    n = logread_lines(&at, lines, 64, 24000, &next, &gap, 0, NULL, NULL);
    CHECK(n == 1 && !strcmp(lines[0], "boot+1.002 three"), "whole lines only: one still being written waits");
    at = next;
    put(path, "a", "ten\n");
    rotate();
    put(path, "a", "boot+2.000 after the rotation, a line long enough for the new text's name: sixty-four bytes\n");
    n = logread_lines(&at, lines, 64, 24000, &next, &gap, 0, NULL, NULL);
    CHECK(n == 2 && !gap && !strcmp(lines[0], "boot+1.003 a line still being written") && strstr(lines[1], "after the rotation"), "rotated: the rest of the old text from .1, then the new one, nothing lost");
    at = next;
    logread_start(&at);
    n = logread_lines(&at, lines, 64, 24000, &next, &gap, 0, NULL, NULL);
    CHECK(n == 5 && strstr(lines[0], "the first line"), "start: the oldest line kept (.1's first)");
    logread_cursor_parse(cur, &at);                         /* the cursor of the first read, in the text now in .1 */
    rotate(); put(path, "a", "boot+3.000 a third text, long enough to have a name of its own: sixty-four bytes and more\n");
    n = logread_lines(&at, lines, 64, 24000, &next, &gap, 0, NULL, NULL);
    CHECK(gap && n >= 1 && strstr(lines[0], "after the rotation"), "rotated twice since the cursor: gap, from the oldest line kept");
    /* young texts after a rotation (S5 of a review of 08399dc): from now in a fresh log with less than 64 bytes is the
     * end of .1, and what is written next comes, nothing skipped */
    rotate(); put(path, "a", "boot+3.500 short\n");
    logread_now(&at);
    CHECK(at.id && at.off > 0, "from now in a young log: the end of .1, which has a name");
    put(path, "a", "boot+3.501 after it, enough for the text to have a name now: sixty-four bytes\n");
    rotate(); put(path, "a", "boot+3.600 a third fresh text, long enough for a name of its own here, sixty-four\n");
    n = logread_lines(&at, lines, 64, 24000, &next, &gap, 0, NULL, NULL);
    CHECK(gap && n >= 1, "... rotated away twice since: said as a gap, not skipped silently");
    { struct logpos y = { 0, 0 };
      n = logread_lines(&y, lines, 64, 24000, &next, &gap, 0, NULL, NULL);
      CHECK(gap && n >= 1 && strstr(lines[0], "short"), "a cursor {0, x} (the first log's, young) once a .1 is there: .1, said as a gap"); }
    /* a line longer than a message: cut, and read past */
    memset(big, 'x', sizeof big); memcpy(big, "boot+4.000 ", 11); big[sizeof big - 2] = '\n'; big[sizeof big - 1] = 0;
    logread_now(&at); put(path, "a", big); put(path, "a", "boot+4.001 after it\n");
    n = logread_lines(&at, lines, 64, 2000, &next, &gap, 0, NULL, NULL);
    if (n != 1 || strlen(lines[0]) != 1023) printf("     (%d lines, %zu, %ld -> %ld)\n", n, n ? strlen(lines[0]) : 0, at.off, next.off);
    CHECK(n == 1 && strlen(lines[0]) == 1023 && next.off > at.off + 2000, "a line longer than the budget: cut to 1023 bytes, and the next read goes on after it");
    at = next; n = logread_lines(&at, lines, 64, 2000, &next, &gap, 0, NULL, NULL);
    CHECK(n == 1 && !strcmp(lines[0], "boot+4.001 after it"), "... with the line after it");
    /* Q1 of a review of 0dabfa6: a cursor is the plugin's to hand back, so it may point into a line: from just past a
     * sealed line's mark, the rest of it passed as a line of its own.  Read from the next line's start, said as a gap */
    { long before; FILE *f; struct logpos mid;
      logread_now(&at);
      f = fopen(path, "a"); fputs("boot+5.000 a public line\n", f); before = ftell(f);
      fputs("boot+5.001 " LOG_SEALED_MARK "plugin evilwake: boot+1.000 the household said: remind me to call the bank\n", f);
      fputs("boot+5.002 " LOG_PRIVATE_MARK "transcript: unlock the front door\n", f);
      fputs("boot+5.003 another public line\n", f); fclose(f);
      n = logread_lines(&at, lines, 64, 24000, &next, &gap, 0, NULL, NULL);
      CHECK(n == 2 && !gap, "(hassmic's own cursor: the two public lines)");
      mid = next; mid.off = before + (long)strlen("boot+5.001 " LOG_SEALED_MARK "plugin evilwake: ");
      logread_cursor(&mid, cur, sizeof cur);
      CHECK(!logread_cursor_parse(cur, &mid), "(a made-up cursor into the sealed line's text: a cursor as any)");
      n = logread_lines(&mid, lines, 64, 24000, &next, &gap, 0, NULL, NULL);
      CHECK(n == 1 && gap && !strcmp(lines[0], "boot+5.003 another public line"), "Q1: a cursor into a line: from the next line's start, said as a gap; nothing of the sealed line");
      mid.off = before + 3; f = fopen(path, "a"); fputs("boot+5.004 a line still being writ", f); fclose(f);
      { struct stat st; stat(path, &st); mid.off = (long)st.st_size - 5; }
      n = logread_lines(&mid, lines, 64, 24000, &next, &gap, 0, NULL, NULL);
      CHECK(n == 0 && next.off == mid.off, "... into a line still being written: nothing until it is whole");
      put(path, "a", "ten\nboot+5.005 after it\n");
      n = logread_lines(&mid, lines, 64, 24000, &next, &gap, 0, NULL, NULL);
      CHECK(n == 1 && gap && !strcmp(lines[0], "boot+5.005 after it"), "... then from the line after it"); }
    /* Q5 of a review of 0dabfa6: a marked line cut at LOG_MARKED_MAX is not cut inside a character */
    { char v[3000]; size_t m = strlen(LOG_PRIVATE_MARK); int pp[2], k2 = dup(2); char out[4096]; ssize_t got;
      memset(v, 'a', sizeof v); v[sizeof v - 1] = 0;
      for (size_t i = LOG_MARKED_MAX - m - 1; i + 1 < sizeof v - 1; i += 2) { v[i] = (char)0xC3; v[i + 1] = (char)0xA9; }
      if (pipe(pp)) abort();
      dup2(pp[1], 2); log_private("%s", v); dup2(k2, 2); close(k2); close(pp[1]);
      got = read(pp[0], out, sizeof out); close(pp[0]);
      CHECK(got > 2 && out[got - 1] == '\n' && ((unsigned char)out[got - 2] & 0xC0) != 0xC0 && got - 1 <= LOG_MARKED_MAX, "Q5: a marked line cut at its bound: not inside a character"); }
    { char cmd[300]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir); if (system(cmd)) { } }
    printf("logread: %s\n", fails ? "FAILED" : "all passed");
    return fails != 0;
}
