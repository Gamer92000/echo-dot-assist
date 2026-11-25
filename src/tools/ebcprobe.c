/* BTCVSD on MediaTek's SoC-integrated Bluetooth (CONSYS behind WMT: the Echo Dot 2, biscuit; issue #233).  There the
 * Bluetooth core neither sends SCO data over HCI nor puts it on PCM pins: it writes each packet of the air bitstream
 * (CVSD, or mSBC in transparent mode) into the BT system's SRAM, and the kernel's AudDrv_BTCVSD hands it out on
 * /dev/ebc.  Stock's audio HAL (audio.primary.mt8163_headless.so, AudioBTCVSDControl) codes CVSD/mSBC in software
 * (libcvsd_mtk.so, libmsbc_mtk.so).  This reads what arrives and writes what goes out, while a synchronous link is
 * up (src/tools/scoprobe.c sets one up):
 *   ebcprobe rx <secs> <out.raw>      every read() as it comes: the driver's packets, payload + "valid" bytes
 *   ebcprobe tx <secs> <in.raw>       the bitstream written as the driver takes it
 * ioctls as MediaTek's driver (magic 'C'): 0xe0 allocate/free (0 TX, 1 free TX, 2 RX, 3 free RX), 0xe1 state
 * (TX 0x00 idle .. 0x03 running, 0x04 ending; RX 0x10 .. 0x13, 0x14). */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define IOC_ALLOC _IOWR('C', 0xe0, unsigned int)
#define IOC_STATE _IOWR('C', 0xe1, unsigned int)
#define IOC_GET   _IOWR('C', 0xe2, unsigned int)

static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }

static int io(int fd, unsigned long req, unsigned long arg, const char *what)
{
    int r = ioctl(fd, req, arg);
    printf("%s(%lu): %d%s%s\n", what, arg, r, r < 0 ? " " : "", r < 0 ? strerror(errno) : "");
    return r;
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: ebcprobe rx|tx <secs> <file>\n"); return 2; }
    setvbuf(stdout, NULL, _IOLBF, 0);
    int tx = !strcmp(argv[1], "tx"), secs = atoi(argv[2]);
    int fd = open("/dev/ebc", O_RDWR);
    if (fd < 0) { printf("open /dev/ebc: %s\n", strerror(errno)); return 1; }
    FILE *f = fopen(argv[3], tx ? "rb" : "wb");
    if (!f) { printf("%s: %s\n", argv[3], strerror(errno)); return 1; }
    io(fd, IOC_ALLOC, tx ? 0 : 2, "alloc");
    io(fd, IOC_STATE, tx ? 0x01 : 0x11, "state init");
    io(fd, IOC_STATE, tx ? 0x02 : 0x12, "state ready");
    io(fd, IOC_STATE, tx ? 0x03 : 0x13, "state running");
    long long t0 = now_ms(), next = t0 + 1000, total = 0, calls = 0;
    int sizes[8] = { 0 }, nsz = 0;
    unsigned char b[4096];
    while (now_ms() - t0 < secs * 1000LL) {
        ssize_t n;
        if (tx) {
            size_t want = fread(b, 1, 180, f);
            if (!want) { rewind(f); continue; }
            n = write(fd, b, want);
        } else {
            n = read(fd, b, sizeof b);
            if (n > 0) fwrite(b, 1, n, f);
        }
        if (n < 0) { printf("%s: %s\n", tx ? "write" : "read", strerror(errno)); usleep(10000); continue; }
        calls++; total += n;
        int k; for (k = 0; k < nsz && sizes[k] != n; k++) ;
        if (k == nsz && nsz < 8) { sizes[nsz++] = n; if (!tx) { printf("read of %zd bytes:", n); for (int i = 0; i < n && i < 64; i++) printf(" %02x", b[i]); printf("\n"); } }
        if (now_ms() >= next) { printf("%llds %s %lld B in %lld calls\n", (now_ms() - t0) / 1000, tx ? "wrote" : "read", total, calls); next += 1000; }
    }
    io(fd, IOC_STATE, tx ? 0x04 : 0x14, "state ending");
    io(fd, IOC_STATE, tx ? 0x00 : 0x10, "state idle");
    io(fd, IOC_ALLOC, tx ? 1 : 3, "free");
    fclose(f);
    printf("total %lld B\n", total);
    return 0;
}
