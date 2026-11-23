/* The log's real writers against what a plugin with log.read gets (S1 of a review of 08399dc): log_private and
 * log_sealed (logread.c), clock.c's reader that stamps every line (clock_log_start: the same pipe, the same forward()
 * hassmic runs), and logread_public over what lands in the file.  Sentinels go in through every way a marked line
 * was seen to leak (a line break inside a value, a marked line longer than the reader's cut, a marked line joined to
 * the end of another that lost its line end, a plugin's line sealed while it was a wake plugin) and none may come out
 * of the filter.  The unmarked lines beside them come out: the filter is not simply closed. */
#define _GNU_SOURCE
#include "clock.h"
#include "logread.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void screen_language_changed(void) { }             /* (clock.c's one call outside the log) */

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s (line %d)\n", what, __LINE__); fails++; } else printf("ok   %s\n", what); } while (0)

int main(void)
{
    char path[200], big[3200], *text = NULL; int fd, keep1, keep2; size_t n = 0;
    snprintf(path, sizeof path, "/tmp/logwriters_test.%d.log", (int)getpid());
    if ((fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600)) < 0) return 2;
    keep1 = dup(1); keep2 = dup(2);
    dup2(fd, 2); close(fd);
    setenv("HASSMIC_CLOCK_SYNCED", "0", 1);
    clock_log_start();                              /* fd 2 (and 1) a pipe to the reader, which appends to the file */

    /* 1. a value with line breaks in it (a reply, a transcript) */
    log_private("transcript: remind me\nSENTINEL-1 about the appointment\r\nand SENTINEL-2");
    log_sealed("plugin evilwake: score\nSENTINEL-3 0x4f6b");
    /* 2. a marked line longer than the reader cuts (2048): written raw, as anything but log_private would */
    memset(big, 'x', sizeof big); memcpy(big, LOG_PRIVATE_MARK, strlen(LOG_PRIVATE_MARK));
    memcpy(big + 2300, "SENTINEL-4", 10); big[sizeof big - 1] = '\n';
    if (write(2, big, sizeof big) < 0) return 2;
    /* ... and through log_private: cut short of the reader's cut, the rest gone, nothing of it unmarked */
    { char v[2600]; memset(v, 'y', sizeof v - 1); v[sizeof v - 1] = 0; memcpy(v + 2000, "SENTINEL-5", 10); log_private("reply: %s", v); }
    /* 3. a line whose end was lost (a write the full pipe took part of), the next marked line joined to it */
    if (write(2, "an unmarked line without its end ", 33) < 0) return 2;
    log_private("transcript: SENTINEL-6");
    /* 3b. a mark cut in two by the reader's 2048-byte cut ("(priv" in one piece, "ate) ..." in the next): a line
     * without its end before it (a full pipe, a writer's chunks) puts the mark anywhere, so every place near the cut
     * (Q3 of a review of 0dabfa6) */
    { static char pad[2100]; memset(pad, 'x', sizeof pad);
      for (int L = 2030; L <= 2080; L++) { while (write(2, pad, (size_t)L) < 0) usleep(1000); log_private("transcript: SENTINEL-7-%d what was said", L); } }
    /* 4. control lines that must come through */
    fprintf(stderr, "public: CONTROL-1\n");
    log_marked_f(NULL, "public: CONTROL-2 %s", "x");
    fprintf(stderr, "END\n");

    /* the reader ends when the last writer closes the pipe */
    fflush(stdout); dup2(keep2, 2); close(keep2); dup2(keep1, 1); close(keep1);
    for (int i = 0; i < 100; i++) {
        FILE *f = fopen(path, "r"); size_t cap = 0; ssize_t k;
        free(text); text = NULL; n = 0;
        if (f) { char *line = NULL; while ((k = getline(&line, &cap, f)) > 0) { text = realloc(text, n + (size_t)k + 1); memcpy(text + n, line, (size_t)k); n += (size_t)k; text[n] = 0; } free(line); fclose(f); }
        if (text && strstr(text, " END\n")) break;
        usleep(50000);
    }
    CHECK(text && strstr(text, " END\n"), "the reader wrote every line into the file");
    CHECK(text && strstr(text, "SENTINEL-1") && strstr(text, "SENTINEL-4") && strstr(text, "SENTINEL-6"), "(the sentinels are in the log: the page shows them)");

    /* what a plugin gets: every line through the filter */
    { int leaked = 0, control = 0, cut4 = 0; char *save = NULL;
      for (char *l = text ? strtok_r(text, "\n", &save) : NULL; l; l = strtok_r(NULL, "\n", &save)) {
          size_t ln = strlen(l); char b[4096];
          if (ln >= sizeof b) ln = sizeof b - 1;
          memcpy(b, l, ln); b[ln] = 0;
          if (strstr(b, "SENTINEL-4")) cut4 += !!strstr(b, LOG_PRIVATE_MARK);
          if (!logread_public(b, &ln, NULL, NULL)) continue;
          b[ln] = 0;
          if (strstr(b, "SENTINEL")) { leaked++; printf("     (passed: %.120s)\n", b); }
          control += strstr(b, "CONTROL-") != NULL;
      }
      CHECK(!leaked, "no sentinel reaches a plugin: line breaks, a cut line, a joined line, a sealed line, a mark cut in two");
      CHECK(cut4 == 1, "the piece of a cut marked line carries the mark (clock.c forward)");
      CHECK(control == 2, "the unmarked lines beside them do"); }
    unlink(path); free(text);
    printf("logwriters: %s\n", fails ? "FAILED" : "all passed");
    return fails != 0;
}
