/* scoaudio.c's PCM devices by name (make unit-scoaudio): the names the boards give, found in each measured Echo's
 * /proc/asound/pcm as it was read off it (testdata/hfp/asound-pcm-<codename>.txt), at the numbers the probes used */
#include "../../src/hassmic/scoaudio.c"
#include <stdio.h>

const struct board board = { .sco_msbc_chip = 1 };     /* (pcm_find reads no board field) */
int audio_sco_native(void) { return 0; }
int hci_cmd(unsigned op, const void *par, unsigned n) { (void)op; (void)par; (void)n; return 0; }
int hci_write(const void *b, size_t n) { (void)b; (void)n; return 0; }

static int fails;
static void at(const char *file, const char *name, int want)
{
    setenv("HASSMIC_ASOUND_PCM", file, 1);
    int got = pcm_find(name);
    printf("%s %s: %s is %d (%d)\n", got == want ? "ok  " : "FAIL", file, name, got, want);
    fails += got != want;
}

int main(void)
{
    at("testdata/hfp/asound-pcm-donut.txt", "MultiMedia2_PLayback", 8);
    at("testdata/hfp/asound-pcm-donut.txt", "VOIP_Call_BT_Capture", 9);
    at("testdata/hfp/asound-pcm-cannoli.txt", "DL6 Playback", 3);
    at("testdata/hfp/asound-pcm-cannoli.txt", "UL5 Capture", 7);
    at("testdata/hfp/asound-pcm-checkers.txt", "VOIP_Call_BT_Playback", 10);
    at("testdata/hfp/asound-pcm-checkers.txt", "VOIP_Call_BT_Capture", 11);
    at("testdata/hfp/asound-pcm-checkers.txt", "VOIP_Call_BT", -1);          /* a name's start is no name */
    at("testdata/hfp/asound-pcm-cannoli.txt", "UL5", -1);
    /* the phone's link accepted: the bytes scoprobe's "msbc -U 16" accepted on the Echo Dot 3, Dot 5, Show 5
     * (2026-11-25), and the old ones */
    {
        static const unsigned char enh[57] = {
            0x40, 0x1f, 0, 0, 0x40, 0x1f, 0, 0, 0x05, 0, 0, 0, 0, 0x05, 0, 0, 0, 0, 60, 0, 60, 0,
            0x00, 0x7d, 0, 0, 0x00, 0x7d, 0, 0, 0x04, 0, 0, 0, 0, 0x04, 0, 0, 0, 0, 16, 0, 16, 0,
            2, 2, 0, 0, 1, 1, 16, 16, 13, 0, 0x80, 0x03, 2 };
        static const unsigned char cvsd[15] = { 0x40, 0x1f, 0, 0, 0x40, 0x1f, 0, 0, 0xff, 0xff, 0x60, 0, 0xff, 0x3f, 0 };
        static const unsigned char transp[15] = { 0x40, 0x1f, 0, 0, 0x40, 0x1f, 0, 0, 0xff, 0xff, 0x63, 0, 0xff, 0x88, 0x03 };
        unsigned char p[64]; int n; unsigned op;
        kind = SCO_PCM;
        op = sco_accept_params(SCO_MSBC, 1, p, &n);
        printf("%s pcm, mSBC: Enhanced Accept, the measured parameters\n", op == 0x043e && n == 57 && !memcmp(p, enh, 57) ? "ok  " : "FAIL"); fails += !(op == 0x043e && n == 57 && !memcmp(p, enh, 57));
        op = sco_accept_params(SCO_MSBC, 0, p, &n);
        printf("%s pcm, mSBC asked on a plain SCO link: CVSD\n", op == 0x0429 && n == 15 && !memcmp(p, cvsd, 15) ? "ok  " : "FAIL"); fails += !(op == 0x0429 && n == 15 && !memcmp(p, cvsd, 15));
        op = sco_accept_params(SCO_CVSD, 1, p, &n);
        printf("%s pcm, CVSD: the plain accept\n", op == 0x0429 && n == 15 && !memcmp(p, cvsd, 15) ? "ok  " : "FAIL"); fails += !(op == 0x0429 && n == 15 && !memcmp(p, cvsd, 15));
        kind = SCO_EBC;
        op = sco_accept_params(SCO_MSBC, 1, p, &n);
        printf("%s ebc, mSBC coded here: transparent, EV3 or 2-EV3\n", op == 0x0429 && n == 15 && !memcmp(p, transp, 15) ? "ok  " : "FAIL"); fails += !(op == 0x0429 && n == 15 && !memcmp(p, transp, 15));
        printf("%s wideband: ebc, and pcm where the board says the chip codes it\n", sco_wideband() ? "ok  " : "FAIL"); fails += !sco_wideband();
    }
    printf("%s\n", fails ? "FAILED" : "all passed");
    return fails != 0;
}
