/* Reading hassmic's log (boot.log): for the settings page's viewer (web.c) and for plugins with `log.read`
 * (plugin.c; docs/plugins.md 4.5).  What a plugin gets is not what the page shows: see logread.c. */
#ifndef LOGREAD_H
#define LOGREAD_H
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/* In front of the text of a line that carries what was said to the Echo or what it answered, a name a person gave (a
 * network's, a device's, a timer's, a light ring design's, a wake word model's, the Echo's in Amazon's account, a host or
 * server address the owner entered), an address that fetches
 * something (a media URL with its token), a login code, where the home is (its country), or what Home Assistant or a
 * Wyoming server said in words (an error's text may quote the request).  The page shows such lines as they are, "(private)" and all;
 * a plugin with log.read never gets them (one with log.read.full does: the page's view).  Marked where the line is written, not guessed from its words afterwards
 * (docs/plugins.md 9.8): a list of prefixes beside the log's writers goes stale with the next fprintf, a mark at the
 * writer does not.
 * LOG_SEALED: the same for what a wake word plugin said or did (its stderr, the broker's lines about it, the words it
 * reported): `wake` is sealed (3.3), and the mark is the line's from the moment it is written, whatever is installed
 * when it is read (a plugin removed or updated without `wake` leaves its lines as sealed as they were).
 *
 * Only through log_private()/log_sealed(): they make the whole line in one buffer, turn every control character of
 * it into a space (a reply with a line break in it was two lines, and the second had no mark: a review, 2026-11-21)
 * and hand it to the log in one write(2), which a pipe takes whole or not at all (PIPE_BUF); a line printed in pieces
 * (stdio on an unbuffered stream writes the format's parts as they come) could have another thread's text between
 * its mark and its value.  The marks themselves are for logread.c and clock.c's reader */
#define LOG_PRIVATE_MARK "(private) "
#define LOG_SEALED_MARK "(sealed) "
/* the longest marked line, its stamp left out: clock.c's reader cuts a line at 2048 bytes, and a marked line is never
 * cut (the piece after a cut carries the mark again there all the same) */
#define LOG_MARKED_MAX 1900
void log_private(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_sealed(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* MARK (LOG_PRIVATE_MARK, LOG_SEALED_MARK or NULL for none) and the line, made and written as log_private does */
void log_marked(const char *mark, const char *fmt, va_list ap);
void log_marked_f(const char *mark, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* whether line L (N bytes) has a mark anywhere: clock.c carries it onto a cut line's next piece, a plugin gets no
 * such line (a mark that is not at the start of a line's text is a line joined to another: it never passes) */
const char *logread_mark(const char *l, size_t n);

const char *logread_path(void);                 /* boot.log (HASSMIC_LOG for tests) */
/* The Sendspin pairing token ("SP:0" and base32) blanked in place: it lets a server pair.  The page's view, and a
 * plugin's on top of what logread_public does */
void logread_redact(char *t, size_t n);
/* The tail of PART (0 boot.log, 1 its rotated part), MAX bytes at most, whole lines, redacted: malloc'd, *n its
 * length; NULL with *n 0 when there is no such file, NULL with *n 1 when out of memory */
char *logread_tail(int part, size_t max, size_t *n);

/* What a plugin with log.read may have of one line (N bytes, no line end), changed in place: 1 keep (*n the new
 * length), 0 drop.  SEALED: whether a plugin's id (ID, LEN bytes) is a wake word plugin's, whose lines go to no plugin
 * (3.3); NULL none is.  The rules are logread.c's */
int logread_public(char *line, size_t *n, int (*sealed)(const char *id, size_t len, void *arg), void *arg);
/* The same for a plugin with log.read.full (official only): the page's view, the pairing token blanked, every line
 * kept but a wake word plugin's (sealed anywhere, or naming a plugin SEALED says is one) */
int logread_full(char *line, size_t *n, int (*sealed)(const char *id, size_t len, void *arg), void *arg);

/* A place in the log: the text of boot.log is named by the first LOGREAD_ID_BYTES bytes it ever had (a rotation copies
 * the file to boot.log.1 and empties it: the same text, the same name, now in .1).  id 0: boot.log while it is
 * shorter than that, whatever it holds */
#define LOGREAD_ID_BYTES 64
struct logpos { uint64_t id; long off; };
/* A cursor as text ("<16 hex>:<offset>", 40 bytes hold it) and back: 0, or -1 when TEXT is none */
void logread_cursor(const struct logpos *p, char *out, size_t cap);
int  logread_cursor_parse(const char *text, struct logpos *p);
/* where "from now" (the end of boot.log) and "start" (the first line kept: boot.log.1's, else boot.log's) are */
void logread_now(struct logpos *p);
void logread_start(struct logpos *p);

/* Up to MAX lines (each cut to LINE - 1 bytes) and BUDGET bytes of them from AT on, through logread_public (FULL:
 * logread_full): into
 * lines[i] (NUL-terminated), the count.  *next: where the line after the last one read begins (lines dropped by the
 * filter are read too); *gap 1 when AT's text is gone (rotated away twice) and the lines are the oldest kept ones.
 * Only whole lines: a line still being written is left for the next call */
int  logread_lines(const struct logpos *at, char (*lines)[1024], int max, size_t budget, struct logpos *next, int *gap, int full,
                   int (*sealed)(const char *id, size_t len, void *arg), void *arg);
#endif
