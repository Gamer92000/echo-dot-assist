/* textdraw.c on the PC: what was said and the reply as the Echo Show's text layer draws them.  Needs a font: the
 * firmware's (HASSMIC_FONTS=firmware/cronos/rootfs/system/fonts, as make does when it is unpacked), else DejaVu, else
 * the test is skipped.  Checks the band's rows stay clear, the reply's white and the heard text's grey are there, an
 * empty request draws nothing, a reply too long for the screen ends inside it, and broken UTF-8 draws without a crash;
 * build/textdraw_<n>.ppm shows each case over a dark grey screen, for the eye. */
#include "textdraw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }

#define W 960
#define H 480                                   /* the Echo Show 5's screen */
#define KEEP 20                                 /* the band, as the first cronos had it */
static uint32_t px[W * H];

static void ppm(const char *name)
{
    FILE *f = fopen(name, "wb");
    if (!f) return;
    fprintf(f, "P6 %d %d 255\n", W, H);
    for (int i = 0; i < W * H; i++) {           /* over a dark grey screen, as a dashboard might be */
        float a = (px[i] >> 24) / 255.f;
        unsigned char c[3] = { (unsigned char)((px[i] >> 16 & 255) * a + 40 * (1 - a)),
                               (unsigned char)((px[i] >> 8 & 255) * a + 40 * (1 - a)),
                               (unsigned char)((px[i] & 255) * a + 40 * (1 - a)) };
        fwrite(c, 1, 3, f);
    }
    fclose(f);
}

/* rows [y0, y1) have a pixel close to rgb, fully covered */
static int has(int y0, int y1, uint32_t rgb)
{
    for (int y = y0; y < y1; y++)
        for (int x = 0; x < W; x++) {
            uint32_t p = px[y * W + x];
            if (p >> 24 != 255) continue;
            int d = abs((int)(p >> 16 & 255) - (int)(rgb >> 16 & 255)) + abs((int)(p >> 8 & 255) - (int)(rgb >> 8 & 255)) +
                    abs((int)(p & 255) - (int)(rgb & 255));
            if (d < 12) return 1;
        }
    return 0;
}

static int clear(int y0, int y1)
{
    for (int y = y0; y < y1; y++) for (int x = 0; x < W; x++) if (px[y * W + x]) return 0;
    return 1;
}

/* the first and last rows with words on them (bright and covered: not the shade) */
static int text_row(int last)
{
    for (int k = 0; k < H; k++) {
        int y = last ? H - 1 - k : k;
        for (int x = 0; x < W; x++) { uint32_t p = px[y * W + x]; if (p >> 24 > 200 && (p >> 8 & 255) > 120) return y; }
    }
    return last ? -1 : H;
}

int main(void)
{
    if (!getenv("HASSMIC_FONT") && !getenv("HASSMIC_FONTS")) {
        static const char *dejavu = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
        FILE *f = fopen(dejavu, "rb");
        if (f) { fclose(f); setenv("HASSMIC_FONT", dejavu, 1); }
    }
    if (textdraw_init() < 0) { printf("textdraw: no font (HASSMIC_FONTS, HASSMIC_FONT or DejaVu), skipped\n"); return 0; }

    textdraw_render(px, W, H, KEEP, "Wie wird das Wetter morgen?", "Morgen wird es sonnig bei bis zu 18 Grad.");
    ppm("build/textdraw_1.ppm");
    check(clear(H - KEEP, H), "the band's rows stay clear");
    check(has(0, H - KEEP, 0xffffff), "the reply in white");
    check(has(0, H - KEEP, 0xb4bcc8), "the heard text in grey");
    check(has(0, H - KEEP, 0x00caff), "the cyan bar beside the reply");
    int top_short = text_row(0), foot_short = text_row(1);
    check(top_short > H / 4 && abs((top_short + foot_short) / 2 - (H - KEEP) / 2) < H / 10, "a short request in the middle");
    check(foot_short - top_short > H / 6, "large: the words of a short request take a sixth of the screen or more");

    textdraw_render(px, W, H, KEEP, "", "");
    check(clear(0, H), "nothing said, nothing drawn");

    textdraw_render(px, W, H, KEEP, "Was steht heute an?", NULL);
    ppm("build/textdraw_2.ppm");
    check(has(0, H - KEEP, 0xb4bcc8) && !has(0, H - KEEP, 0x00caff), "heard alone: grey text, no reply bar");

    char longer[4096] = "";
    for (int i = 0; i < 30; i++) strcat(longer, "Heute stehen drei Termine im Kalender, der erste um neun Uhr im Büro. ");
    textdraw_render(px, W, H, KEEP, "Was steht heute im Kalender und was muss ich mitbringen, damit nichts fehlt?", longer);
    ppm("build/textdraw_3.ppm");
    check(clear(H - KEEP, H), "a reply too long for the screen: the band's rows still clear");
    check(has(0, H - KEEP, 0xffffff), "a reply too long for the screen: drawn, cut");
    check(text_row(0) < top_short && text_row(1) > foot_short, "a long reply takes more of the screen than a short one");

    textdraw_render(px, W, H, KEEP, "Stra\xc3\x9f" "e \xff\xfe kaputt \xe2\x82", "Aufw\xc3\xa4rts \xe2\x80\x94 ok");
    ppm("build/textdraw_4.ppm");
    check(has(0, H - KEEP, 0xffffff), "broken UTF-8: drawn without a crash");

    char word[600]; memset(word, 'W', sizeof word - 1); word[sizeof word - 1] = 0;
    textdraw_render(px, W, H, KEEP, "", word);
    check(clear(H - KEEP, H) && has(0, H - KEEP, 0xffffff), "one word longer than the screen: broken, cut");

    struct textdraw_timer one[] = { { "Pizza", 299, 600, 1 } };
    textdraw_timers(px, W, H, KEEP, one, 1, 0);
    ppm("build/textdraw_5.ppm");
    int t0 = text_row(0), t1 = text_row(1);
    check(clear(H - KEEP, H) && has(0, H - KEEP, 0xffffff) && has(0, H - KEEP, 0x00caff), "a timer: its time in white, its bar in cyan");
    check(t1 - t0 > H / 4 && abs((t0 + t1) / 2 - (H - KEEP) / 2) < H / 8, "a timer: large, in the middle");
    struct textdraw_timer three[] = { { "Tee", 45, 180, 1 }, { "", 3725, 7200, 1 }, { "Kuchen", 1200, 2400, 0 } };
    textdraw_timers(px, W, H, KEEP, three, 3, 0);
    ppm("build/textdraw_6.ppm");
    check(clear(H - KEEP, H) && has(0, H - KEEP, 0x8a919c), "three timers, one paused: drawn, the paused one grey");
    struct textdraw_timer rings[] = { { "Pizza", 0, 600, 1 } };
    textdraw_timers(px, W, H, KEEP, rings, 1, 1);
    ppm("build/textdraw_7.ppm");
    check(has(0, H - KEEP, 0xff9f0a) && !has(0, H - KEEP, 0x00caff), "a timer ringing: orange, no cyan");
    textdraw_timers(px, W, H, KEEP, one, 0, 0);
    check(clear(0, H), "no timers, nothing drawn");

    if (fails) { printf("FAILED: %d\n", fails); return 1; }
    printf("textdraw: all passed (build/textdraw_*.ppm)\n");
    return 0;
}
