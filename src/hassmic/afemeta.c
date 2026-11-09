/* The front end's record in the micAsr samples' lowest bits: see afemeta.h; bit positions from docs/re-vad.md. */
#include "afemeta.h"

static unsigned bits(const int16_t *s, int at, int n)  /* most significant first */
{
    unsigned v = 0;
    while (n--) v = v << 1 | (unsigned)(s[at++] & 1);
    return v;
}

int afemeta_frame(const int16_t *s, struct afemeta *m)
{
    unsigned ones = 0;
    if (bits(s, 0, 8) != 0xa5 || bits(s, 8, 8) != 0x63 || bits(s, 16, 7) != 0x68) return 0;
    for (int i = 0; i < 120; i++) ones += s[i] & 1;
    if ((ones & 0xff) != bits(s, 120, 8)) return 0;
    m->rms_db = (int)bits(s, 54, 8) - 256;
    m->playback = (int)bits(s, 67, 1);
    m->muted = (int)bits(s, 75, 1);
    m->vad = (int)bits(s, 113, 2);
    return 1;
}
