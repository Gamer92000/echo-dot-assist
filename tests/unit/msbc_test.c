/* mSBC (sbc.c msbc_*) against BlueZ's libsbc, from sbc_ref.sh:
 *   msbc_test dec in.msbc ref.au   decodes libsbc's frames (sbcenc -m) with ours, compared with sbcdec -m's decoding
 *   msbc_test enc in.au out.msbc   encodes a 16 kHz mono .au with ours, for sbcdec -m (which checks the CRC)
 *   msbc_test crc in.msbc          every frame with one bit flipped (header, scale factors) is refused, whole ones not */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    size_t n, m; unsigned char *in; int16_t pcm[MSBC_SAMPLES];
    if (argc < 3 || !(in = slurp(argv[2], &n))) { fprintf(stderr, "usage: msbc_test dec|enc|crc ...\n"); return 2; }
    if (!strcmp(argv[1], "dec") && argc == 4) {
        unsigned char *au = slurp(argv[3], &m); struct sbc s; size_t off = 0, got = 0; double err = 0, sig = 0; long maxd = 0;
        if (!au || m < 24) return 2;
        size_t data = (size_t)au[4] << 24 | au[5] << 16 | au[6] << 8 | au[7];
        sbc_init(&s);
        for (size_t len; off < n && (len = msbc_decode(&s, in + off, n - off, pcm)); off += len)
            for (unsigned i = 0; i < MSBC_SAMPLES && data + 2 * (got + 1) <= m; i++, got++) {
                long ref = (int16_t)(au[data + 2 * got] << 8 | au[data + 2 * got + 1]), d = pcm[i] - ref;
                err += (double)d * d; sig += (double)ref * ref; if (labs(d) > maxd) maxd = labs(d);
            }
        double snr = 10 * log10(sig / (err ? err : 1));
        int ok = off == n && got && got * 2 == m - data && snr > 60;
        printf("%s mSBC decoder: %zu samples, max diff %ld, SNR against libsbc %.1f dB\n", ok ? "ok  " : "FAIL", got, maxd, snr);
        return !ok;
    }
    if (!strcmp(argv[1], "enc") && argc == 4) {
        FILE *out = fopen(argv[3], "wb"); struct sbc_enc e; uint8_t frame[MSBC_FRAME]; size_t data;
        if (!out || n < 24 || sbc_enc_init(&e, 16000, 26)) return 2;
        data = (size_t)in[4] << 24 | in[5] << 16 | in[6] << 8 | in[7];
        for (size_t off = data; off + 2 * MSBC_SAMPLES <= n; off += 2 * MSBC_SAMPLES) {
            for (int i = 0; i < MSBC_SAMPLES; i++) pcm[i] = (int16_t)(in[off + 2 * i] << 8 | in[off + 2 * i + 1]);
            if (msbc_encode(&e, pcm, frame) != MSBC_FRAME || fwrite(frame, 1, MSBC_FRAME, out) != MSBC_FRAME) return 1;
        }
        return fclose(out) != 0;
    }
    if (!strcmp(argv[1], "crc")) {
        struct sbc s; long whole = 0, refused = 0, frames = 0;
        for (size_t off = 0; off + MSBC_FRAME <= n; off += MSBC_FRAME, frames++) {
            sbc_init(&s); whole += msbc_decode(&s, in + off, MSBC_FRAME, pcm) == MSBC_FRAME;
            unsigned char f[MSBC_FRAME]; memcpy(f, in + off, MSBC_FRAME);
            int bit = 8 + (int)(frames * 7 % 56);                  /* bytes 1..7: reserved, CRC, scale factors */
            f[bit >> 3] ^= 0x80 >> (bit & 7);
            sbc_init(&s); refused += msbc_decode(&s, f, MSBC_FRAME, pcm) == 0;
        }
        int ok = frames && whole == frames && refused == frames;
        printf("%s mSBC CRC: %ld of %ld frames whole, %ld of %ld with a bit flipped refused\n", ok ? "ok  " : "FAIL", whole, frames, refused, frames);
        return !ok;
    }
    return 2;
}
