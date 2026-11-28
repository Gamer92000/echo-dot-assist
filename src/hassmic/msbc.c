/* mSBC's framing on the air: see msbc.h.  The H2 header's sequence is written in order and not held against what comes
 * in (a frame out of turn still decodes; PipeWire and BlueZ do the same). */
#include "msbc.h"
#include <string.h>

static const uint8_t h2[4] = { 0x08, 0x38, 0xc8, 0xf8 };

void msbc_tx_init(struct msbc_tx *t) { memset(t, 0, sizeof *t); sbc_enc_init(&t->e, 16000, 26); }

void msbc_tx(struct msbc_tx *t, const int16_t *pcm, uint8_t *out)
{
    out[0] = 0x01; out[1] = h2[t->seq++ & 3];
    msbc_encode(&t->e, pcm, out + 2);
    out[MSBC_PACKET - 1] = 0;
}

void msbc_rx_init(struct msbc_rx *r) { memset(r, 0, sizeof *r); sbc_init(&r->s); }

static int at_frame(const uint8_t *b)
{
    return b[0] == 0x01 && (b[1] == h2[0] || b[1] == h2[1] || b[1] == h2[2] || b[1] == h2[3]) && b[2] == 0xad;
}

static size_t lost(struct msbc_rx *r, int16_t *out)
{
    for (int i = 0; i < MSBC_SAMPLES; i++) r->last[i] /= 2;
    memcpy(out, r->last, sizeof r->last);
    r->lost++;
    return MSBC_SAMPLES;
}

size_t msbc_rx(struct msbc_rx *r, const uint8_t *p, size_t n, int16_t *out)
{
    size_t w = 0;
    while (n) {
        size_t k = sizeof r->b - r->n < n ? sizeof r->b - r->n : n;
        memcpy(r->b + r->n, p, k); r->n += k; p += k; n -= k;
        while (r->n >= MSBC_PACKET) {
            size_t drop;
            if (at_frame(r->b)) {
                if (msbc_decode(&r->s, r->b + 2, MSBC_FRAME, out + w) == MSBC_FRAME) {
                    memcpy(r->last, out + w, sizeof r->last); w += MSBC_SAMPLES; r->frames++;
                } else w += lost(r, out + w);
                drop = MSBC_PACKET; r->skipped = 0;
            } else {                                                /* not at a frame: up to the next header */
                drop = 1;
                while (drop + 3 <= r->n && !at_frame(r->b + drop)) drop++;
                for (r->skipped += drop; r->skipped >= MSBC_PACKET; r->skipped -= MSBC_PACKET) w += lost(r, out + w);
            }
            memmove(r->b, r->b + drop, r->n - drop); r->n -= drop;
        }
    }
    return w;
}
