/* Tiny helpers shared by the command-line tools: one-client TCP listener and full writes. */
#ifndef NETIO_H
#define NETIO_H
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static inline int net_listen(int port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = INADDR_ANY };
    if (s < 0) return -1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0 || listen(s, 1) < 0) { close(s); return -1; }
    return s;
}

static inline int net_accept(int s)
{
    int c, one = 1;
    do c = accept(s, NULL, NULL); while (c < 0 && errno == EINTR);
    if (c >= 0) setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return c;
}

/* accept() for a server that must outlive whatever happens to its listener (issue #241: an Echo Dot 2's ESPHome port
 * refused Home Assistant for hours while the rest of hassmic ran on).  Every loop used to end at the first error,
 * without a word; ESPHome's then ran main()'s end, which closes the capture under the capture thread.  Linux
 * hands a queued connection's own failure out through accept() (accept(2): ENETDOWN, EPROTO, ENOPROTOOPT, EHOSTDOWN,
 * ENONET, EHOSTUNREACH, EOPNOTSUPP, ENETUNREACH; and ECONNABORTED, a connection reset or destroyed before it was
 * taken), and EMFILE, ENFILE, ENOBUFS, ENOMEM, EPERM pass too: none of them is the listener's, so it is asked again
 * after a moment.  EINVAL (no longer listening: shut down, or aborted by SOCK_DESTROY), EBADF, ENOTSOCK (the number
 * is closed or someone else's): the listener is lost, a new one is opened on the same port (once a second until it
 * binds), the old one closed on EINVAL only (still a socket of ours: the caller's loop is its one owner, nothing else
 * closes it; EBADF and ENOTSOCK say the number is not ours any more).  The connection or -1 for "nothing this time": the
 * caller goes round again.  Said at most once in 10 s per caller's file, with how many since. */
static inline int net_accept_kept(int *ls, int port, const char *who)
{
    static time_t said; static unsigned unsaid;
    int c = *ls >= 0 ? net_accept(*ls) : -1, e = *ls >= 0 ? errno : EBADF, lost;
    if (c >= 0) return c;
    lost = e == EINVAL || e == EBADF || e == ENOTSOCK;
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    if (lost) {
        if (*ls >= 0 && e == EINVAL) close(*ls);
        *ls = net_listen(port);
    }
    unsaid++;
    if (!said || now.tv_sec - said >= 10) {
        fprintf(stderr, "%s: accept on port %d: %s%s (%u since the last word)\n", who, port, strerror(e),
                !lost ? "" : *ls >= 0 ? ", listening again" : ", cannot listen yet, trying every second", unsaid);
        said = now.tv_sec ? now.tv_sec : 1; unsaid = 0;
    }
    if (lost) { if (*ls < 0) sleep(1); return -1; }
    usleep(e == EMFILE || e == ENFILE || e == ENOBUFS || e == ENOMEM ? 100000 : 10000);
    return -1;
}

static inline int write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; n -= w;
    }
    return 0;
}

static inline ssize_t read_full(int fd, void *buf, size_t n)
{
    char *p = buf; size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) break;
        got += r;
    }
    return got;
}
#endif
