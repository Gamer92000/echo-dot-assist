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
#include <unistd.h>
#ifdef __ANDROID__
#include <sys/syscall.h>
#endif

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
#ifdef __ANDROID__
    /* athena's libc (Android 9, Amazon's build): accept() and accept4() both fail with EBADF after the connection came
     * in, which the kernel then drops (RST), while the same system call made directly works (2026-10-14).  Every
     * listener of hassmic ended on that first error: no connection to ESPHome, the page or the push port. */
    do c = (int)syscall(__NR_accept4, s, NULL, NULL, 0); while (c < 0 && errno == EINTR);
#else
    do c = accept(s, NULL, NULL); while (c < 0 && errno == EINTR);
#endif
    if (c >= 0) setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return c;
}

/* connect() fails the same way on athena (EBADF, nothing sent: hassmic could not fetch Home Assistant's spoken replies,
 * "cannot connect" within a millisecond; toybox's ping fails too), the system call made directly works.  sendto,
 * recvfrom, bind and getsockname are fine (2026-10-14). */
static inline int net_connect_fd(int s, const struct sockaddr *a, socklen_t len)
{
#ifdef __ANDROID__
    return (int)syscall(__NR_connect, s, a, len);
#else
    return connect(s, a, len);
#endif
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
