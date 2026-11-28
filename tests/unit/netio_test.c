/* netio.h net_accept_kept (issue #241): an accept() error never ends a server.  A queued connection's own failure
 * (injected through --wrap=accept) leaves the listener as it is; a listener shut down (EINVAL) or closed behind the
 * server's back (EBADF) is opened again on the same port; one that cannot bind yet is tried again on the next call. */
#include "netio.h"
#include <assert.h>
#include <stdlib.h>

static int inject;                          /* the errno the next accept() fails with, 0: the real one */
int __real_accept(int s, struct sockaddr *a, socklen_t *l);
int __wrap_accept(int s, struct sockaddr *a, socklen_t *l)
{
    if (inject) { errno = inject; inject = 0; return -1; }
    return __real_accept(s, a, l);
}

static int port_of(int s) { struct sockaddr_in a; socklen_t l = sizeof a; getsockname(s, (struct sockaddr *)&a, &l); return ntohs(a.sin_port); }

static int dial(int port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (connect(s, (struct sockaddr *)&a, sizeof a)) { close(s); return -1; }
    return s;
}

/* the listener takes a connection: one dialled, one accepted */
static void takes(int *ls, int port)
{
    int d = dial(port), c, tries = 0;
    assert(d >= 0);
    while ((c = net_accept_kept(ls, port, "test")) < 0) assert(++tries < 5);
    close(c); close(d);
}

int main(void)
{
    int probe = net_listen(0), port = port_of(probe), ls, was, busy;
    close(probe);
    ls = net_listen(port);
    assert(ls >= 0);
    takes(&ls, port);

    /* a queued connection's failure: the listener stays, same descriptor */
    static const int passing[] = { ECONNABORTED, EPROTO, EHOSTDOWN, EHOSTUNREACH, ENETUNREACH, ENETDOWN, EMFILE, ENOBUFS, EPERM };
    for (unsigned i = 0; i < sizeof passing / sizeof passing[0]; i++) {
        was = ls; inject = passing[i];
        assert(net_accept_kept(&ls, port, "test") == -1);
        assert(ls == was);
        takes(&ls, port);
    }

    /* shut down under it (what a SOCK_DESTROY leaves too): no longer listening, EINVAL, a new listener */
    shutdown(ls, SHUT_RDWR);
    assert(dial(port) < 0);                 /* refused, as Home Assistant was */
    assert(net_accept_kept(&ls, port, "test") == -1);
    assert(ls >= 0);
    takes(&ls, port);

    /* closed behind its back: EBADF, opened again (and the number, maybe someone else's by now, not closed) */
    close(ls);
    assert(net_accept_kept(&ls, port, "test") == -1);
    assert(ls >= 0);
    takes(&ls, port);

    /* lost while the port is someone else's: -1 and no listener after a second, then the port free: open again */
    close(ls);
    busy = socket(AF_INET, SOCK_STREAM, 0);
    { struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = INADDR_ANY }; int one = 1;
      setsockopt(busy, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);      /* (the dials' TIME_WAIT; a listener still keeps the port) */
      assert(!bind(busy, (struct sockaddr *)&a, sizeof a) && !listen(busy, 1)); }
    ls = -1;
    assert(net_accept_kept(&ls, port, "test") == -1);
    assert(ls < 0);
    close(busy);
    assert(net_accept_kept(&ls, port, "test") == -1);
    assert(ls >= 0);
    takes(&ls, port);

    close(ls);
    puts("netio: ok");
    return 0;
}
