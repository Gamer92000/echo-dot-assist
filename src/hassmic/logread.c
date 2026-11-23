/* logread.h: hassmic's log for the settings page and for plugins (docs/plugins.md 4.5).
 *
 * The page shows the owner his own log as it is, with one thing blanked: the Sendspin pairing token.  A plugin with
 * `log.read` gets less, because the log also carries what was said (transcripts, replies), names (Wi-Fi networks,
 * Bluetooth devices, timers, the Music Assistant server), paired devices' addresses and root's lines, and a plugin may
 * send whatever it reads to a server the owner entered:
 *  - a line hassmic marked as private or sealed where it wrote it (log_private: what was said, a reply, a name, a
 *    media address; log_sealed: a wake word plugin's) is dropped whole, wherever in it the mark is (one not at the
 *    start of the text is a marked line joined to another one, by a write that lost its end in a full pipe), and so
 *    is a line that cannot be told to be hassmic's own by its stamp;
 *  - root's lines (main.sh's `say`, the firewall watcher's, wifi.sh's: stamped by the shell, without milliseconds)
 *    pass only when they begin with a word root uses for its events ("== ", "!! ", ...): wifi.sh names networks,
 *    and what else a script prints is nobody's guess;
 *  - MAC and Bluetooth addresses are blanked ("aa:bb:cc:dd:ee:ff", "aa-bb-...", and a2dp.c's twelve hex digits);
 *  - a wake word plugin's lines (its stderr, "plugin <id>: ...", and the broker's lines about it, which carry the
 *    reasons it gave, "plugins: <id>: ...") go to no plugin: `wake` is sealed (3.3), and a forwarder must not be its
 *    way out.  They are marked sealed where they are written (plugin.c); the id of a wake plugin installed now is
 *    asked as well, for a line written by a build before the marks;
 *  - the pairing token, as on the page.
 * Addresses of the network (IPv4) are kept: they are what a forwarder's reader looks for when something does not
 * connect, and the server is the owner's.
 * log.read.full (official plugins only: plugin_store.c combination) gets the page's view (logread_full): every
 * line as it is, the private ones with their mark, root's whatever they say, the pairing token blanked; but for a wake
 * word plugin's lines (sealed, wherever the mark is, and the lines that name a wake plugin installed now), which go to
 * no plugin (3.3: a forwarder must not be the seal's way out, whatever it may read).
 *
 * Where a line is, a cursor says: the text of boot.log is named by its first LOGREAD_ID_BYTES bytes (each begins
 * with a time to the millisecond), and main.sh rotates by copy and truncate, so the same text, with the same name, is
 * in boot.log.1 after a rotation and the offset into it still holds.  A cursor whose text is in neither file was
 * rotated away twice: the reader starts at the oldest line kept and says so (gap).  A text shorter than that has no
 * name: no line of it is read (it is the first line of a fresh log, there in a moment), so no cursor points into one;
 * "from now" in such a text is the end of the one before it (boot.log.1), and only where there is none at all (the
 * Echo's first log) the cursor is {0, 0}: its text is boot.log while that has no .1, and .1 once a rotation made one,
 * said as a gap (it may have been rotated away twice by then; a review, 2026-11-21: the young cursor read the next
 * text from its offset, and the lines in between were skipped without a word). */
#include "logread.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const char *logread_path(void) { const char *e = getenv("HASSMIC_LOG"); return e ? e : "/data/local/hassmic/boot.log"; }

void logread_redact(char *t, size_t n)
{
    for (size_t i = 0; i + 4 <= n; i++) {
        if (memcmp(t + i, "SP:0", 4)) continue;
        size_t j = i + 4;
        while (j < n && (isupper((unsigned char)t[j]) || isdigit((unsigned char)t[j]))) t[j++] = '*';
        i = j;
    }
}

/* ---------------------------------------------------------------- marked lines (logread.h) */

void log_marked(const char *mark, const char *fmt, va_list ap)
{
    /* LOG_MARKED_MAX bytes of line at most, and one more formatted: the first byte the cut leaves out, which says
     * whether the cut is inside a character (vsnprintf's NUL stood there, and it never backed off: Q5 of a review,
     * 2026-11-21); then the line end and the NUL */
    char b[LOG_MARKED_MAX + 3]; size_t m = mark ? strlen(mark) : 0, n; int k; ssize_t w;
    memcpy(b, mark ? mark : "", m);
    if ((k = vsnprintf(b + m, LOG_MARKED_MAX + 2 - m, fmt, ap)) < 0) return;
    n = m + (size_t)k;
    if (n > LOG_MARKED_MAX) {
        n = LOG_MARKED_MAX;
        while (n > m && ((unsigned char)b[n] & 0xC0) == 0x80) n--;        /* cut: not inside a character */
    } else if (n > m && b[n - 1] == '\n') n--;              /* (a caller's own line end) */
    /* every control character a space: a line break in a value made a second line, and that line had no mark */
    for (size_t i = m; i < n; i++) if ((unsigned char)b[i] < 0x20 || b[i] == 0x7f) b[i] = ' ';
    b[n++] = '\n';
    while ((w = write(2, b, n)) < 0 && errno == EINTR) ;      /* one write: whole, or (a full pipe) not at all */
}

void log_private(const char *fmt, ...) { va_list ap; va_start(ap, fmt); log_marked(LOG_PRIVATE_MARK, fmt, ap); va_end(ap); }
void log_sealed(const char *fmt, ...) { va_list ap; va_start(ap, fmt); log_marked(LOG_SEALED_MARK, fmt, ap); va_end(ap); }
void log_marked_f(const char *mark, const char *fmt, ...) { va_list ap; va_start(ap, fmt); log_marked(mark, fmt, ap); va_end(ap); }

const char *logread_mark(const char *l, size_t n)
{
    static const char *const marks[] = { LOG_PRIVATE_MARK, LOG_SEALED_MARK };
    for (size_t i = 0; i + 9 <= n; i++)
        for (size_t k = 0; k < 2; k++) { size_t ml = strlen(marks[k]); if (i + ml <= n && !memcmp(l + i, marks[k], ml)) return marks[k]; }
    return NULL;
}

/* a regular file of the log, never waited for (a FIFO of that name) */
static int open_log(int part)
{
    char p[400]; struct stat st; int fd;
    snprintf(p, sizeof p, part ? "%s.1" : "%s", logread_path());
    if ((fd = open(p, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW)) < 0) return -1;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); return -1; }
    return fd;
}

char *logread_tail(int part, size_t max, size_t *n)
{
    int fd = open_log(part); struct stat st; char *t; size_t from, got = 0, skip = 0; ssize_t k;
    *n = 0;
    if (fd < 0) return NULL;
    if (fstat(fd, &st) || st.st_size < 0) { close(fd); return NULL; }
    from = (size_t)st.st_size > max ? (size_t)st.st_size - max : 0;
    if (!(t = malloc((size_t)st.st_size - from + 1))) { close(fd); *n = 1; return NULL; }
    /* others append meanwhile: what was there when we looked */
    while (got < (size_t)st.st_size - from && (k = pread(fd, t + got, (size_t)st.st_size - from - got, (off_t)(from + got))) > 0) got += (size_t)k;
    close(fd);
    if (from) { char *nl = memchr(t, '\n', got); skip = nl ? (size_t)(nl - t) + 1 : got; }     /* whole lines only */
    memmove(t, t + skip, got - skip); got -= skip;
    logread_redact(t, got);
    *n = got;
    return t;
}

/* ---------------------------------------------------------------- what a plugin may have of a line */

static int digits(const char *s, size_t n) { for (size_t i = 0; i < n; i++) if (!isdigit((unsigned char)s[i])) return 0; return 1; }

/* hassmic's own stamp (clock.c: "2026-10-06 15:18:02.417Z " or "boot+29935.512 "): its length, 0 none */
static size_t own_stamp(const char *l, size_t n)
{
    if (n >= 25 && digits(l, 4) && l[4] == '-' && digits(l + 5, 2) && l[7] == '-' && digits(l + 8, 2) && l[10] == ' ' && digits(l + 11, 2) && l[13] == ':'
        && digits(l + 14, 2) && l[16] == ':' && digits(l + 17, 2) && l[19] == '.' && digits(l + 20, 3) && l[23] == 'Z' && l[24] == ' ') return 25;
    if (n > 5 && !memcmp(l, "boot+", 5)) {
        size_t i = 5;
        while (i < n && isdigit((unsigned char)l[i])) i++;
        if (i > 5 && i + 5 <= n && l[i] == '.' && digits(l + i + 1, 3) && l[i + 4] == ' ') return i + 5;
    }
    return 0;
}

/* root's (main.sh stamp(): "2026-10-06 15:18:02Z " or "boot+29935.51 ", /proc/uptime's two places or none) */
static size_t root_stamp(const char *l, size_t n)
{
    if (n >= 21 && digits(l, 4) && l[4] == '-' && digits(l + 5, 2) && l[7] == '-' && digits(l + 8, 2) && l[10] == ' ' && digits(l + 11, 2) && l[13] == ':'
        && digits(l + 14, 2) && l[16] == ':' && digits(l + 17, 2) && l[19] == 'Z' && l[20] == ' ') return 21;
    if (n > 5 && !memcmp(l, "boot+", 5)) {
        size_t i = 5, d = 0;
        while (i < n && isdigit((unsigned char)l[i])) i++;
        if (i == 5) return 0;
        if (i < n && l[i] == '.') { i++; while (i < n && isdigit((unsigned char)l[i]) && d < 3) { i++; d++; } if (!d || d > 2) return 0; }
        if (i < n && l[i] == ' ') return i + 1;
    }
    return 0;
}

/* root's events: what main.sh says of updates, resets, restarts and the firewall's alarms.  Anything else of root's
 * (wifi.sh names networks; what a script prints is nobody's guess) goes to no plugin */
static const char *const root_words[] = { "== ", "!! ", "hassmic exited", "lockdown re-applied", "egress limited", "adb over Wi-Fi", "OTA guard" };

static int hexc(int c) { return isxdigit(c); }
/* MAC and Bluetooth addresses: six pairs of hex digits joined by ':' or '-', and a run of exactly twelve hex digits
 * with no letter or digit on either side (a2dp.c prints a device's as %012llx) */
static void blank_addresses(char *l, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (i && (isalnum((unsigned char)l[i - 1]))) continue;
        if (i + 17 <= n) {
            int ok = 1; char sep = l[i + 2];
            if (sep != ':' && sep != '-') ok = 0;
            for (int k = 0; k < 6 && ok; k++) {
                size_t at = i + (size_t)k * 3;
                if (!hexc((unsigned char)l[at]) || !hexc((unsigned char)l[at + 1]) || (k < 5 && l[at + 2] != sep)) ok = 0;
            }
            if (ok && (i + 17 == n || !isalnum((unsigned char)l[i + 17]))) { for (size_t k = i; k < i + 17; k++) if (l[k] != sep) l[k] = '*'; i += 16; continue; }
        }
        if (i + 12 <= n) {
            size_t k = i;
            while (k < n && hexc((unsigned char)l[k])) k++;
            if (k - i == 12 && (k == n || !isalnum((unsigned char)l[k]))) { memset(l + i, '*', 12); i = k - 1; continue; }
        }
    }
}

/* "plugin <id>: " or "plugins: <id>: " at T: the id's place, else NULL */
static const char *plugin_id(const char *t, size_t n, size_t *len)
{
    size_t at = n >= 7 && !memcmp(t, "plugin ", 7) ? 7 : n >= 9 && !memcmp(t, "plugins: ", 9) ? 9 : 0, k;
    if (!at) return NULL;
    for (k = at; k < n && (islower((unsigned char)t[k]) || isdigit((unsigned char)t[k]) || t[k] == '-'); k++) ;
    if (k == at || k + 1 >= n || t[k] != ':' || t[k + 1] != ' ') return NULL;
    *len = k - at;
    return t + at;
}

int logread_public(char *l, size_t *n, int (*sealed)(const char *id, size_t len, void *arg), void *arg)
{
    size_t s = own_stamp(l, *n), r, idn; const char *id;
    if (logread_mark(l, *n)) return 0;                      /* private or sealed, the mark wherever it is */
    if (!s) {
        if (!(r = root_stamp(l, *n))) return 0;
        for (size_t i = 0; i < sizeof root_words / sizeof root_words[0]; i++)
            if (*n - r >= strlen(root_words[i]) && !memcmp(l + r, root_words[i], strlen(root_words[i]))) { blank_addresses(l, *n); logread_redact(l, *n); return 1; }
        return 0;
    }
    if (sealed && (id = plugin_id(l + s, *n - s, &idn)) && sealed(id, idn, arg)) return 0;
    blank_addresses(l, *n);
    logread_redact(l, *n);
    return 1;
}

/* LOG_SEALED_MARK anywhere in the line (logread_mark gives the first mark, and a private line joined to a sealed one
 * has both) */
static int sealed_mark(const char *l, size_t n)
{
    size_t ml = strlen(LOG_SEALED_MARK);
    for (size_t i = 0; i + ml <= n; i++) if (!memcmp(l + i, LOG_SEALED_MARK, ml)) return 1;
    return 0;
}

int logread_full(char *l, size_t *n, int (*sealed)(const char *id, size_t len, void *arg), void *arg)
{
    size_t s = own_stamp(l, *n), idn; const char *id;
    if (sealed_mark(l, *n)) return 0;
    if (s && sealed && (id = plugin_id(l + s, *n - s, &idn)) && sealed(id, idn, arg)) return 0;
    logread_redact(l, *n);
    return 1;
}

/* ---------------------------------------------------------------- where a line is */

/* the name of the text in PART (0 none: shorter than LOGREAD_ID_BYTES, or no such file), and its size (-1 none) */
static uint64_t text_id(int part, long *size)
{
    int fd = open_log(part); struct stat st; uint8_t b[LOGREAD_ID_BYTES]; uint64_t h = 1469598103934665603ULL;
    *size = -1;
    if (fd < 0) return 0;
    if (fstat(fd, &st)) { close(fd); return 0; }
    *size = (long)st.st_size;
    if (st.st_size < LOGREAD_ID_BYTES || pread(fd, b, sizeof b, 0) != (ssize_t)sizeof b) { close(fd); return 0; }
    close(fd);
    for (size_t i = 0; i < sizeof b; i++) { h ^= b[i]; h *= 1099511628211ULL; }         /* FNV-1a: a name, not a secret */
    return h ? h : 1;
}

void logread_cursor(const struct logpos *p, char *out, size_t cap) { snprintf(out, cap, "%016llx:%ld", (unsigned long long)p->id, p->off); }

int logread_cursor_parse(const char *t, struct logpos *p)
{
    unsigned long long id = 0; long off = 0; int k = 0;
    if (strlen(t) < 18 || t[16] != ':') return -1;
    for (int i = 0; i < 16; i++) if (!isxdigit((unsigned char)t[i]) || isupper((unsigned char)t[i])) return -1;
    if (!digits(t + 17, strlen(t + 17)) || strlen(t + 17) > 12 || sscanf(t, "%16llx:%ld%n", &id, &off, &k) != 2 || (size_t)k != strlen(t) || off < 0) return -1;
    p->id = id; p->off = off;
    return 0;
}

/* "from now": the end of boot.log; in a text too young for a name, the end of the one before it (or {0, 0}: none) */
void logread_now(struct logpos *p)
{
    long s, s1; uint64_t id1;
    if ((p->id = text_id(0, &s))) { p->off = s; return; }
    id1 = text_id(1, &s1);
    p->id = id1; p->off = id1 ? s1 : 0;
}
void logread_start(struct logpos *p) { long s; uint64_t id1 = text_id(1, &s); if (id1) { p->id = id1; p->off = 0; } else { p->id = text_id(0, &s); p->off = 0; } }

#define READ_MAX 65536
#define LONG_MAX_BYTES (1 << 20)
/* how far from OFF the line that began before it ends (its '\n' included); what is there when none comes within
 * LONG_MAX_BYTES or before the file's end (a line still being written: taken as far as it is) */
static size_t rest_of_line(int part, long off)
{
    char b[4096]; size_t n = 0; ssize_t k; int fd = open_log(part);
    if (fd < 0) return 0;
    while (n < LONG_MAX_BYTES && (k = pread(fd, b, sizeof b, (off_t)off + (off_t)n)) > 0) {
        char *nl = memchr(b, '\n', (size_t)k);
        if (nl) { n += (size_t)(nl - b) + 1; break; }
        n += (size_t)k;
    }
    close(fd);
    return n;
}
/* whether OFF in PART is where a line begins (the byte before it a line end) */
static int line_start(int part, long off)
{
    char c = 0; int fd = open_log(part), ok;
    if (fd < 0) return 0;
    ok = pread(fd, &c, 1, (off_t)off - 1) == 1 && c == '\n';
    close(fd);
    return ok;
}
/* the start of the line after the one OFF is in, -1 when that line is not whole yet (a line longer than LONG_MAX_BYTES:
 * past what was looked at) */
static long next_line(int part, long off)
{
    size_t n = rest_of_line(part, off); char c = 0; int fd = open_log(part);
    if (fd < 0) return -1;
    if (!n || (n < LONG_MAX_BYTES && (pread(fd, &c, 1, (off_t)off + (off_t)n - 1) != 1 || c != '\n'))) { close(fd); return -1; }
    close(fd);
    return off + (long)n;
}
int logread_lines(const struct logpos *at, char (*lines)[1024], int max, size_t budget, struct logpos *next, int *gap, int full,
                  int (*sealed)(const char *id, size_t len, void *arg), void *arg)
{
    long s0, s1; uint64_t id0 = text_id(0, &s0), id1 = text_id(1, &s1); struct logpos cur = *at; int part, count = 0; char *b;
    *gap = 0; *next = *at;
    if (s0 < 0) return 0;                                   /* no log at all (yet) */
    if (cur.id && cur.id == id0) part = 0;
    /* {0, x}: the first log of all, while it was young (logread_now hands out no other young cursor).  Its text is
     * boot.log while there is no .1, else .1 (a rotation since), which cannot be told from a second rotation: a gap */
    else if (!cur.id && !id1) { part = 0; cur.id = id0; if (cur.off > s0) { *gap = 1; cur.off = 0; } }
    else if (!cur.id) { part = 1; cur.id = id1; *gap = 1; if (cur.off > s1) cur.off = 0; }
    else if (cur.id && cur.id == id1) part = 1;
    else { *gap = 1; logread_start(&cur); part = cur.id && cur.id == id1 ? 1 : 0; }
    if (part == 0 && cur.off > s0) { *gap = 1; cur.off = 0; }                 /* emptied without a copy: what was there is gone */
    if (part == 1 && cur.off > s1) { *gap = 1; if (id0) { part = 0; cur.id = id0; cur.off = 0; } else cur.off = s1; }
    /* Only from a line's start.  A cursor is the plugin's to keep and hand back, so it may be made up: one that pointed
     * into a line had its text, from there to the line's end, filtered as a line of its own, and the mark at the
     * line's start was behind it (a wake plugin wrote a stamp of its own into its sealed line, a reader asked from just
     * past the mark; Q1 of a review, 2026-11-21).  Past the next line end then, and said as a gap; a line still being
     * written is waited for */
    if (cur.off > 0 && !line_start(part, cur.off)) {
        long to = next_line(part, cur.off);
        if (to < 0) { *next = cur; return 0; }
        cur.off = to; *gap = 1;
    }
    if (!(b = malloc(READ_MAX + 1))) return 0;
    size_t taken = 0;                                       /* bytes read through in this call */
    while (count < max && budget) {
        int fd; ssize_t got; size_t from = 0, want = budget < READ_MAX ? budget : READ_MAX;
        if (part == 0 && !id0) break;                       /* a text without a name yet: its lines in a moment */
        if ((fd = open_log(part)) < 0) break;
        got = pread(fd, b, want, (off_t)cur.off);
        close(fd);
        if (got <= 0) {
            /* .1 read to its end: boot.log from its start, once it has a name (until then the cursor stays at .1's
             * end, which names the place as well) */
            if (part == 1 && id0) { part = 0; cur.id = id0; cur.off = 0; continue; }
            break;
        }
        for (;;) {
            char *nl = memchr(b + from, '\n', (size_t)got - from); size_t len, keep, skip = 0;
            /* a line longer than a whole read (the first of this call): taken as one, cut, and read past to its end
             * (looked for up to LONG_MAX_BYTES further); left, it would be read again and again and nothing after it
             * ever, and taken in pieces, its tail would pass for lines of their own */
            if (!nl && !from && !taken && (size_t)got == want) { nl = b + got - 1; skip = rest_of_line(part, cur.off + got); }
            if (!nl) break;
            len = (size_t)(nl - (b + from)); keep = len;
            if (keep && b[from + keep - 1] == '\r') keep--;
            if (full ? logread_full(b + from, &keep, sealed, arg) : logread_public(b + from, &keep, sealed, arg)) {
                if (keep > 1023) { keep = 1023; while (keep && ((unsigned char)b[from + keep] & 0xC0) == 0x80) keep--; }     /* (not inside a character) */
                memcpy(lines[count], b + from, keep); lines[count][keep] = 0;
                for (size_t k = 0; k < keep; k++) if ((unsigned char)lines[count][k] < 0x20) lines[count][k] = ' ';
                count++;
            }
            from += len + 1; cur.off += (long)(len + 1 + skip); taken += len + 1 + skip;
            budget = budget > len + 1 + skip ? budget - len - 1 - skip : 0;
            if (skip) { from = (size_t)got; break; }
            if (count == max || !budget || from >= (size_t)got) break;
        }
        if (from < (size_t)got || count == max || !budget) {
            if (from == 0) break;                                   /* only a line still being written */
            if (count == max || !budget) break;
            continue;
        }
        /* all of this read was whole lines: on, in the same part (or past .1's end, next time round) */
    }
    free(b);
    *next = cur;
    return count;
}
