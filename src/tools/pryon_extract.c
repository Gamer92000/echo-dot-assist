/* Offline native Pryon activation extraction. Vendor files never changed.
 * The Thumb observation trampoline is confined to this process's private pages.
 * Supported libpryon build is pinned by full SHA256 and instruction fingerprint.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <stdatomic.h>
#include "pryon_api.h"
#include "hash.h"

extern void pryon_capture_stub(void);
extern unsigned char pryon_capture_resume[];
static int output_fd = -1;
static _Atomic int enabled, failed;
static uint32_t cycles;
static uintptr_t library_base;

static int write_all(int fd, const void *data, size_t n)
{
    const unsigned char *p = data;
    while (n) { ssize_t k = write(fd, p, n); if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1; p += k; n -= (size_t)k; }
    return 0;
}

/* Called with every general/VFP register and the original argument stack saved. */
void pryon_capture_enter(const uint32_t *r, const uint32_t *stack)
{
    if (!atomic_load(&enabled) || r[3] != 100 || stack[11] != 3) return;
    if (r[2] != 25 || stack[0] != 100 || !r[1]) { atomic_store(&failed, 1); return; }
    if (write_all(output_fd, (const void *)(uintptr_t)r[1], 25 * 100 * sizeof(float)))
        atomic_store(&failed, 1);
    cycles++;
}

static int library(struct dl_phdr_info *info, size_t n, void *arg)
{
    (void)n; (void)arg;
    if (!info->dlpi_name) return 0;  /* bionic's main-program entry can be unnamed. */
    const char *name = strrchr(info->dlpi_name, '/'); name = name ? name + 1 : info->dlpi_name;
    if (strcmp(name, "libpryon.so")) return 0;
    library_base = info->dlpi_addr; return 1;
}

static int guard_library(void)
{
    static const unsigned char expected[32] = {
        0x65,0xdf,0x80,0x1e,0x37,0xad,0x0b,0xbd,0xfa,0xf8,0x92,0x21,0x74,0xbc,0xeb,0x08,
        0x4f,0xae,0x6b,0xa7,0x1c,0x50,0x76,0xb1,0xe2,0x3a,0x00,0x05,0x23,0xf9,0x70,0x27 };
    FILE *f = fopen("/system/lib/libpryon.so", "rb");
    if (!f) return -1;
    struct sha256 c; sha256_init(&c); unsigned char b[16384], digest[32]; size_t n;
    while ((n = fread(b, 1, sizeof b, f))) sha256_update(&c, b, n);
    int error = ferror(f); fclose(f); sha256_final(&c, digest);
    return error || memcmp(digest, expected, 32) ? -1 : 0;
}

static int observe(void)
{
    static const unsigned char original[8] = { 0xf0,0xb5,0x03,0xaf,0x2d,0xe9,0x00,0x0f };
    if (guard_library()) { fprintf(stderr, "Unsupported Pryon library hash\n"); return -1; }
    dl_iterate_phdr(library, NULL);
    if (!library_base) return -1;
    unsigned char *target = (unsigned char *)(library_base + 0x123897c);
    unsigned char *resume = (unsigned char *)((uintptr_t)pryon_capture_resume & ~(uintptr_t)1);
    if (memcmp(target, original, 8)) return -1;
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return -1;
    uintptr_t mask = ~((uintptr_t)page_size - 1);
    if (mprotect((void *)((uintptr_t)resume & mask), page_size, PROT_READ|PROT_WRITE|PROT_EXEC)) return -1;
    memcpy(resume, original, 8);
    /* Thumb LDR.W pc,[pc,#0], followed by the destination. No scratch register. */
    const unsigned char jump[4] = { 0xdf,0xf8,0x00,0xf0 };
    memcpy(resume + 8, jump, 4);
    uint32_t back = (uint32_t)(uintptr_t)(target + 8) | 1;
    memcpy(resume + 12, &back, 4);
    __builtin___clear_cache((char *)resume, (char *)resume + 16);
    if (mprotect((void *)((uintptr_t)resume & mask), page_size, PROT_READ|PROT_EXEC)) return -1;
    if (mprotect((void *)((uintptr_t)target & mask), page_size, PROT_READ|PROT_WRITE|PROT_EXEC)) return -1;
    uint32_t enter = (uint32_t)(uintptr_t)pryon_capture_stub | 1;
    memcpy(target, jump, 4); memcpy(target + 4, &enter, 4);
    __builtin___clear_cache((char *)target, (char *)target + 8);
    return mprotect((void *)((uintptr_t)target & mask), page_size, PROT_READ|PROT_EXEC);
}

static void log_line(int level, const char *tag, const char *message)
{
    if (level <= 3) fprintf(stderr, "pryon %s: %s\n", tag ? tag : "", message ? message : "");
}

static int push(const int16_t *pcm, size_t n, uint64_t *sample)
{
    int rc = PryonDecoder_PushAudioEventSamples("extract-dec", *sample, pcm, n);
    *sample += n;
    usleep((useconds_t)(n * 1000000 / 16000));
    return rc;
}

int main(int argc, char **argv)
{
    if (argc != 4) { fprintf(stderr, "usage: pryon-extract MANIFEST PADDED_RAW NEW_FEATURE_FILE\n"); return 2; }
    FILE *in = fopen(argv[2], "rb"); if (!in) { perror("audio"); return 1; }
    output_fd = open(argv[3], O_WRONLY|O_CREAT|O_EXCL, 0600);
    if (output_fd < 0) { perror("features"); fclose(in); return 1; }
    uint32_t header[4] = { 0x46595250, 1, 25, 100 };
    if (write_all(output_fd, header, sizeof header) || observe()) {
        fprintf(stderr, "Cannot initialize guarded activation observer\n"); return 1;
    }
    PryonApi_SetLoggingCallback(log_line);
    if (PryonModelSet_New("extract-ms", argv[1], "")) return 1;
    PryonMultichannelAudioFormat fmt;
    PryonDecoder_NewPryonMultichannelAudioFormat_Default(&fmt);
    if (PryonDecoder_NewSpotterAudioDecoder("extract-dec", "extract-ms", "pryon", fmt, "{}")) return 1;
    atomic_store(&enabled, 1);
    int16_t pcm[800]; uint64_t samples = 0; size_t n; int rc = 0;
    while ((n = fread(pcm, sizeof *pcm, 800, in))) if ((rc = push(pcm, n, &samples))) break;
    if (ferror(in)) rc = 1;
    fclose(in);
    PryonDecoder_BacklogWait("extract-dec", -1);
    sleep(1);
    PryonDecoder_Delete("extract-dec");
    PryonModelSet_Delete("extract-ms");
    atomic_store(&enabled, 0);
    if (fsync(output_fd) || close(output_fd)) rc = 1;
    fprintf(stderr, "samples=%llu cycles=%u rows=25 columns=100 capture_error=%d\n",
            (unsigned long long)samples, cycles, atomic_load(&failed));
    return rc || atomic_load(&failed) || !cycles ? 1 : 0;
}
