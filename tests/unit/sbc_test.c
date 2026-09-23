/* Decodes an SBC file with sbc.c and compares it with libsbc's decoding of the same file (an .au from sbcdec -f). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "sbc.h"

static unsigned char *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb"); unsigned char *b = NULL; size_t cap = 0; *n = 0;
    if (!f) return NULL;
    for (size_t r; (b = realloc(b, cap += 1 << 16)) && (r = fread(b + *n, 1, cap - *n, f)) > 0; *n += r) ;
    fclose(f);
    return b;
}

int main(int argc, char **argv)
{
    size_t n, m, off = 0, got = 0; unsigned char *in, *au; struct sbc s; int16_t pcm[256];
    if (argc != 3 || !(in = slurp(argv[1], &n)) || !(au = slurp(argv[2], &m)) || m < 24) { fprintf(stderr, "usage: sbc_test file.sbc ref.au\n"); return 2; }
    size_t data = (size_t)au[4] << 24 | au[5] << 16 | au[6] << 8 | au[7];      /* .au: big-endian s16 after the header */
    double err = 0, sig = 0; long maxd = 0;
    sbc_init(&s);
    for (size_t len; off < n && (len = sbc_decode(&s, in + off, n - off, pcm, (unsigned[]){ 0 })); off += len) {
        unsigned k = (in[off + 1] >> 4 & 3) * 4 + 4; k *= in[off + 1] & 1 ? 8 : 4; k *= s.channels;
        for (unsigned i = 0; i < k && data + 2 * (got + 1) <= m; i++, got++) {
            long ref = (int16_t)(au[data + 2 * got] << 8 | au[data + 2 * got + 1]), d = pcm[i] - ref;
            err += (double)d * d; sig += (double)ref * ref; if (labs(d) > maxd) maxd = labs(d);
        }
    }
    double snr = 10 * log10(sig / (err ? err : 1));
    int ok = off == n && got && got * 2 == m - data && snr > 60;
    printf("%s %s: %zu samples, max diff %ld, SNR against libsbc %.1f dB\n", ok ? "ok  " : "FAIL", argv[1], got, maxd, snr);
    return !ok;
}
