// rcpbridge SOCKET RCP [ARGS...]
// The Echo's 802.15.4 radio as a stream for one client at a time: Amazon's RCP program (the Echo 4: /system/bin/ot-rcp,
// OpenThread's RCP on Qorvo's GP712 platform, Spinel in HDLC-lite on its standard input and output) started anew for
// every connection to the unix socket SOCKET, and ended when that connection ends.  Root's main.sh runs it (as the
// radio's user, runas openthread openthread,fifteen_four: /dev/gp is zigbee:fifteen_four 0660) while the radio is a
// remote one: hassmic forwards a TCP port to the socket for the address it lets in (radio.c), Zigbee2MQTT's zoh
// adapter or an OpenThread border router of the owner's on the other end.
//
// Why a process of root's and not one of hassmic's own: a radio program that hassmic forked would be its child for as
// long as the radio is in use, and Amazon's DHA module (davs.c's login) waits for all of hassmic's children.  And the
// radio is one thing for one host at a time: root starts this or our border router (otbr-agent, which runs the same
// RCP itself), never both, so the two never meet on /dev/gp.
//
// Why the RCP is ended with each connection: every host starts with a reset of its own, and a host gone (the network
// dropped, Zigbee2MQTT restarted) leaves the radio in whatever it was told last, transmitting on a channel nobody listens
// to; a fresh process for the next one is the state a host expects at its reset.  ot-rcp takes a socket for its
// standard input and output (measured on the Echo 4, 2026-11-27: PROTOCOL_VERSION 4.3, NCP_VERSION, RCP_API_VERSION 11
// over toybox nc's TCP connection), so it gets one end of a socketpair and this process copies between that and the
// client: the client's end of the connection is seen here, and its EOF ends the RCP (SIGTERM, SIGKILL after a second).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop;
static void on_term(int s) { (void)s; stop = 1; }

static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }

/* all of n bytes to fd, waiting for room (the RCP reads at its own pace): 0, or -1 when fd is gone */
static int put(int fd, const char *b, size_t n)
{
    while (n) {
        ssize_t w = write(fd, b, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        b += w; n -= (size_t)w;
    }
    return 0;
}

/* its input closed already: 200 ms to end on that, then SIGTERM, then a second, then SIGKILL */
static void end_child(pid_t pid)
{
    int st;
    for (long long until = now_ms() + 200; now_ms() < until; ) {
        if (waitpid(pid, &st, WNOHANG) == pid) return;
        usleep(10000);
    }
    kill(pid, SIGTERM);
    for (long long until = now_ms() + 1000; now_ms() < until; ) {
        if (waitpid(pid, &st, WNOHANG) == pid) return;
        usleep(20000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
}

/* one client: its own RCP until either end goes */
static void serve(int c, char **rcp)
{
    int sp[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sp)) { perror("rcpbridge: socketpair"); return; }
    pid_t pid = fork();
    if (pid < 0) { perror("rcpbridge: fork"); close(sp[0]); close(sp[1]); return; }
    if (!pid) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);       /* this process killed outright (init's stop): the radio's too */
        dup2(sp[1], 0); dup2(sp[1], 1);
        signal(SIGPIPE, SIG_DFL); signal(SIGTERM, SIG_DFL);
        execv(rcp[0], rcp);
        perror(rcp[0]);
        _exit(127);
    }
    close(sp[1]);
    fprintf(stderr, "rcpbridge: a host connected, %s started (pid %d)\n", rcp[0], (int)pid);
    char buf[4096]; long long in = 0, out = 0; const char *why = "the host left";
    while (!stop) {
        struct pollfd p[2] = { { c, POLLIN, 0 }, { sp[0], POLLIN, 0 } };
        if (poll(p, 2, 1000) < 0) { if (errno == EINTR) continue; why = "poll failed"; break; }
        int st;
        if (waitpid(pid, &st, WNOHANG) == pid) {
            pid = 0;
            why = WIFEXITED(st) ? "the RCP exited" : "the RCP was killed";
            break;
        }
        if (p[1].revents) {
            ssize_t r = read(sp[0], buf, sizeof buf);
            if (r <= 0) { why = "the RCP closed its output"; break; }
            if (put(c, buf, (size_t)r)) break;
            out += r;
        }
        if (p[0].revents) {
            ssize_t r = read(c, buf, sizeof buf);
            if (r <= 0) break;
            if (put(sp[0], buf, (size_t)r)) { why = "the RCP closed its input"; break; }
            in += r;
        }
    }
    if (stop) why = "rcpbridge stopped";
    close(sp[0]);
    if (pid) end_child(pid);
    fprintf(stderr, "rcpbridge: %s (%lld bytes to the radio, %lld from it)\n", why, in, out);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: rcpbridge SOCKET RCP [ARGS...]\n"); return 2; }
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    if (strlen(argv[1]) >= sizeof a.sun_path) { fprintf(stderr, "rcpbridge: socket path too long\n"); return 2; }
    strcpy(a.sun_path, argv[1]);
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa = { .sa_handler = on_term };        /* no SA_RESTART: accept() returns to look at stop */
    sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL);
    int l = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    unlink(a.sun_path);
    /* the socket's group may connect (hassmic is in the radio's group): its mode at bind is the umask's */
    umask(007);
    if (l < 0 || bind(l, (struct sockaddr *)&a, sizeof a) || listen(l, 1)) { perror("rcpbridge: listen"); return 1; }
    fprintf(stderr, "rcpbridge: %s serves %s\n", a.sun_path, argv[2]);
    while (!stop) {
        int c = accept4(l, NULL, NULL, SOCK_CLOEXEC);
        if (c < 0) { if (errno == EINTR || errno == ECONNABORTED) continue; perror("rcpbridge: accept"); sleep(1); continue; }
        serve(c, argv + 2);
        close(c);
    }
    unlink(a.sun_path);
    return 0;
}
