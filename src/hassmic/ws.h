/* Minimal RFC 6455 WebSocket endpoint: accept an upgrade on a connected socket, or connect out.  Blocking I/O.
 * One reader thread; ws_send may be called from several threads (serialised internally). */
#ifndef WS_H
#define WS_H
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

enum { WS_TEXT = 1, WS_BINARY = 2, WS_CLOSE = 8, WS_PING = 9, WS_PONG = 10 };

struct tls;
struct ws { int fd, client; uint8_t *buf; size_t cap; pthread_mutex_t wlock; struct tls *tls; int rcv_ms; };

int  ws_accept(struct ws *w, int fd, char *path, size_t pathsz);           /* server side; path of the GET, may be NULL */
int  ws_connect(struct ws *w, const char *host, int port, const char *path);
/* the same over TLS (wss://; tls.c): VERIFY checks the server's certificate; ERR (may be NULL) says why it failed */
int  ws_connect_tls(struct ws *w, const char *host, int port, const char *path, int verify, char *err, size_t cap);
/* the receive timeout of a client's connection: silence this long ends ws_recv (0) */
void ws_timeout(struct ws *w, int secs);
int  ws_send(struct ws *w, int opcode, const void *data, size_t len);
/* 1 = complete message (fragments joined, NUL appended, *data valid until the next call), 2 = one beyond 4 MB, read
 * and dropped (*data: its first 256 bytes), 0 = closed, -1 = error.
 * Ping is answered, close is echoed. */
int  ws_recv(struct ws *w, int *opcode, uint8_t **data, size_t *len);
void ws_close(struct ws *w);
void ws_random(void *out, size_t len);
#endif
