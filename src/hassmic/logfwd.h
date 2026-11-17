/* boot.log, followed: to a syslog server and to the ESPHome API clients that subscribed to logs.  See logfwd.c. */
#ifndef LOGFWD_H
#define LOGFWD_H
#include <stddef.h>

/* A line's severity, as the page's log viewer colours it (web/app.js logLine): the words of a failure, root's "=="
 * marks, the rest.  The log has no levels of its own. */
enum { LOGFWD_WARN, LOGFWD_NOTICE, LOGFWD_INFO };

#define LOGFWD_SERVER_MAX 128           /* the setting syslog_server: a host name, an address, either with ":port" */
#define LOGFWD_PORT 514

/* The thread.  live: each line written to boot.log from now on, while logfwd_live() counts someone who wants them: its
 * text without the time in front, the pairing token blanked; called from logfwd's thread, no lock held.  Once. */
void logfwd_start(const char *node, void (*live)(int level, const char *msg, size_t n));
void logfwd_live(int delta);            /* clients that want live lines: +1 / -1.  Any thread, any lock */
/* The syslog settings (settings.c, core_lock held): never blocks, the thread picks them up */
void logfwd_syslog(int on, const char *server, int tcp, int rfc3164);
/* {"state":...,"server":...,"sent":n,"behind":bytes,"error":...} for the settings page.  Any lock */
size_t logfwd_status_json(char *o, size_t cap);

const char *logfwd_path(void);          /* boot.log (HASSMIC_LOG for tests) */
/* The Sendspin pairing token ("SP:0" and base32, sendspin_pairing_token) blanked in place: it lets a server pair.  The
 * page's viewer and everything sent from here */
void logfwd_redact(char *t, size_t n);

/* "host", "host:port", "[v6]:port", a bare IPv6 address: 0 and host/port, or -1.  "" is fine (host "") */
int logfwd_server_parse(const char *s, char *host, size_t cap, int *port);

/* Parts of a line, for logfwd.c and tests/unit/logfwd_test.c */
struct logfwd_time { int known; long long sec; int ms; };
/* The time in front of a line (clock.c's stamp, main.sh's): 1 UTC ("2026-10-06 15:18:02.417Z ", t set), 2 seconds
 * since boot ("boot+29935.512 ", *boot_s), 0 none.  *skip: its length with the space after it */
int logfwd_stamp(const char *l, size_t n, struct logfwd_time *t, double *boot_s, size_t *skip);
int logfwd_level(const char *msg, size_t n);
/* One syslog message (RFC 5424, or 3164) of at most cap - 1 bytes, cut on a UTF-8 boundary; its length.  A 3164
 * message without a known time takes now's, as it has no way to say none */
size_t logfwd_format(char *o, size_t cap, int rfc3164, int level, const struct logfwd_time *t, const char *host, const char *msg, size_t n);
#endif
