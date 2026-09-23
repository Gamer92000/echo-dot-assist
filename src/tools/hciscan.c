/* Raw HCI probe over MediaTek's /dev/stpbt (H4 framing): reset, read BD_ADDR, LE scan.
 * Run with btmanagerd stopped: the device opens beside it and both would lose packets.
 * Usage: hciscan [seconds [interval_ms window_ms [active]]] */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int fd;

static void dump(const char *tag, const uint8_t *b, int n)
{
    printf("%s", tag);
    for (int i = 0; i < n; i++) printf(" %02x", b[i]);
    printf("\n");
}

static int cmd(uint16_t op, const uint8_t *p, int n)
{
    uint8_t b[260] = {1, op & 0xff, op >> 8, n};
    memcpy(b + 4, p, n);
    if (write(fd, b, 4 + n) != 4 + n) { perror("write"); return -1; }
    return 0;
}

static int rd(uint8_t *b, int max, int ms)
{
    struct pollfd p = {fd, POLLIN, 0};
    if (poll(&p, 1, ms) <= 0) return 0;
    int n = read(fd, b, max);
    return n;
}

/* wait for Command Complete of op, print it */
static int wait_cc(uint16_t op)
{
    uint8_t b[1100];
    for (int i = 0; i < 20; i++) {
        int n = rd(b, sizeof b, 2000);
        if (n <= 0) { printf("timeout op %04x\n", op); return -1; }
        dump("<", b, n > 40 ? 40 : n);
        if (n >= 7 && b[0] == 4 && b[1] == 0x0e && (b[4] | b[5] << 8) == op) return b[6];
        if (n >= 7 && b[0] == 4 && b[1] == 0x0f && (b[5] | b[6] << 8) == op) return b[3];
    }
    return -1;
}

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 10, iv = argc > 3 ? atoi(argv[2]) * 8 / 5 : 0x60, win = argc > 3 ? atoi(argv[3]) * 8 / 5 : 0x30;
    fd = open("/dev/stpbt", O_RDWR | O_NOCTTY);
    if (fd < 0) { printf("open: %s\n", strerror(errno)); return 1; }
    uint8_t b[1100];
    while (rd(b, sizeof b, 200) > 0) dump("stale<", b, 16);

    cmd(0x0c03, NULL, 0); printf("reset st=%d\n", wait_cc(0x0c03));
    cmd(0x1009, NULL, 0); printf("bdaddr st=%d\n", wait_cc(0x1009));
    cmd(0x1001, NULL, 0); printf("version st=%d\n", wait_cc(0x1001));
    uint8_t mask[8] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x3f};
    cmd(0x0c01, mask, 8); wait_cc(0x0c01);
    uint8_t lemask[8] = {0x1f, 0, 0, 0, 0, 0, 0, 0};
    cmd(0x2001, lemask, 8); wait_cc(0x2001);
    uint8_t par[7] = {argc > 4, iv & 0xff, iv >> 8, win & 0xff, win >> 8, 0, 0}; /* units of 0.625 ms, public, accept all */
    cmd(0x200b, par, 7); printf("scanparam st=%d\n", wait_cc(0x200b));
    uint8_t en[2] = {1, 0};
    cmd(0x200c, en, 2); printf("scan st=%d\n", wait_cc(0x200c));

    int count = 0;
    time_t end = time(NULL) + secs;
    while (time(NULL) < end) {
        int n = rd(b, sizeof b, 500);
        if (n <= 0) continue;
        if (b[0] == 4 && b[1] == 0x3e && b[3] == 2) {
            count += b[4];
            if (count < 3) dump("adv<", b, n > 48 ? 48 : n);
        } else dump("ev<", b, n > 32 ? 32 : n);
    }
    en[0] = 0; cmd(0x200c, en, 2); wait_cc(0x200c);
    printf("adv reports: %d\n", count);
    return 0;
}
