/* hassmic's TLS client (mbedTLS 3.6, src/third_party/mbedtls; issue #236): TLS 1.2 and 1.3 over a connected socket.
 * The firmwares' own TLS (Android 7's BoringSSL under their libcurl) has no TLS 1.3, which a Home Assistant on the
 * "Modern" SSL profile asks for alone; every HTTPS fetch of hassmic's (http.c) and Music Assistant over wss:// (ws.c)
 * runs on this. */
#ifndef TLS_H
#define TLS_H
#include <stddef.h>
#include <sys/types.h>

struct tls;

/* The handshake on FD, a connected TCP socket (made non-blocking here; still the caller's to close, after
 * tls_free).  HOST: the name sent (SNI) and checked against the certificate, or an address (checked against its
 * IP entries).  VERIFY 0 accepts any certificate (the settings page's "Check HTTPS certificates" off).  Waits until
 * DEADLINE_MS (the monotonic clock, ms) at most; STOP (may be NULL), set by another thread, ends it within 250 ms.
 * NULL with the reason in ERR on any failure, a certificate's named as such. */
struct tls *tls_open(int fd, const char *host, int verify, long long deadline_ms, const volatile int *stop, char *err, size_t cap);

/* Up to N bytes; waits TIMEOUT_MS at most (-1: for ever).  >0 bytes, 0 the end (the peer's close_notify, or the
 * connection closed or shut down: shutdown() of the socket by another thread ends a wait here), -1 an error, -2
 * nothing in that time.  One reader and one writer at once (writers take turns: ws.c's wlock): each call holds the
 * connection's lock only while mbedTLS works, never while it waits for the socket. */
ssize_t tls_read(struct tls *t, void *buf, size_t n, int timeout_ms);

/* All N bytes; fewer (>0) when the socket took nothing more for TIMEOUT_MS, -2 when it took none of them in that time
 * (-1 for TIMEOUT_MS: no limit); -1 an error.  The caller goes on from what was written: never sends those bytes again */
ssize_t tls_write(struct tls *t, const void *buf, size_t n, int timeout_ms);

/* The last error's words (a failed read or write), "" if none */
const char *tls_error(struct tls *t);

/* The protocol agreed, "TLSv1.3" or "TLSv1.2", for the log */
const char *tls_version(struct tls *t);

/* close_notify (once, without waiting) and everything freed; not the socket */
void tls_free(struct tls *t);

/* Monotonic ms, the clock tls_open's deadline is on */
long long tls_now_ms(void);
#endif
