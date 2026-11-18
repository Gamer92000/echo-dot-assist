/*
 * ledstate: what the Echo Dot 5's LEDs show, read back from its LED manager through libhmled.so in ledcontroller
 * ("?leds" on @hassmic-led; src/ledshim/ledshim.c).  The shim takes only the daemon's uid:
 *   runas puffin aipc /data/local/hassmic/ledstate [-n N] [-i MS]
 * Once by default; -n: N reads MS apart (default 10), a line each time it changed ("<ms> <reply>"), then how often it
 * changed a second and what a read took.  A reply: "0" and 10 hex digits a LED (aceLedMgr_getLedState's 5 bytes).
 * -p RRGGBB x 12: that picture first into slot -s NAME (default hassmic_live), one frame of -m MS (1000) looped, as
 * ring.c's live frames; the answer printed.  Then the reads, if -n.
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static double now_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
    int n = 1, every = 10, ms = 1000, o; const char *put = NULL, *slot = "hassmic_live";
    while ((o = getopt(argc, argv, "n:i:p:m:s:")) != -1) switch (o) {
        case 'n': n = atoi(optarg); break;
        case 'i': every = atoi(optarg); break;
        case 'p': put = optarg; break;
        case 'm': ms = atoi(optarg); break;
        case 's': slot = optarg; break;
        default: fprintf(stderr, "usage: ledstate [-p RRGGBBx12 [-m ms] [-s slot]] [-n reads] [-i ms]\n"); return 2;
    }
    const char *nm = "hassmic-led";
    struct sockaddr_un u = { .sun_family = AF_UNIX };
    memcpy(u.sun_path + 1, nm, strlen(nm));
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&u, (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(nm)))) {
        perror("ledstate: @hassmic-led"); return 1;
    }
    char last[256] = "", r[256];
    if (put) {
        unsigned char m[128]; size_t k = strlen(slot) + 1;
        if (strlen(put) != 72 || k > 31) { fprintf(stderr, "ledstate: -p takes 12 x RRGGBB\n"); return 2; }
        memcpy(m, slot, k);
        memcpy(m + k, "animagictotl\0\0\0\1loopfram", 24); k += 24;
        m[k++] = (unsigned char)(ms >> 24); m[k++] = (unsigned char)(ms >> 16); m[k++] = (unsigned char)(ms >> 8); m[k++] = (unsigned char)ms;
        for (int i = 0; i < 36; i++) { unsigned v; sscanf(put + 2 * i, "%2x", &v); m[k++] = (unsigned char)v; }
        double a = now_ms(); ssize_t g;
        if (send(fd, m, k, MSG_NOSIGNAL) != (ssize_t)k || (g = recv(fd, r, sizeof r - 1, 0)) <= 0) { fprintf(stderr, "ledstate: no answer\n"); return 1; }
        r[g] = 0; printf("put %s: %s (%.1f ms)\n", slot, r, now_ms() - a);
        if (n == 1) n = 0;
    } double t0 = now_ms(), took = 0, worst = 0; int changes = 0;
    for (int i = 0; i < n; i++) {
        double a = now_ms();
        ssize_t k;
        if (send(fd, "?leds", 6, MSG_NOSIGNAL) != 6 || (k = recv(fd, r, sizeof r - 1, 0)) <= 0) { fprintf(stderr, "ledstate: no answer\n"); return 1; }
        r[k] = 0;
        double d = now_ms() - a; took += d; if (d > worst) worst = d;
        if (strcmp(r, last)) { printf("%8.1f %s\n", a - t0, r); fflush(stdout); changes += i > 0; snprintf(last, sizeof last, "%s", r); }
        if (i + 1 < n) usleep((useconds_t)every * 1000);
    }
    if (n > 1) printf("%d changes in %.1f s (%.1f a second); a read %.1f ms, at most %.1f\n", changes, (now_ms() - t0) / 1e3,
                      changes / ((now_ms() - t0) / 1e3), took / n, worst);
    return 0;
}
