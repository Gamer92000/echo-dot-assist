/* The C client's put() itself (sdk/hmplugin.c, included here: it is static), as built against the demo's later SDK
 * (tests/unit/scopes_demo.py): pm_fit rewrites a packet in place for the one send it is for, so put() refuses a
 * buffer that went once (one encoded buffer, one send; finding 7 of a review of 4c55b54); a value of a later version
 * than spoken goes as its fallback (the demo's MUSIC_BUFFERING=4@2>PLAYING); and what is not of what is spoken is
 * refused HM_ERR_NOT_SPOKEN, apart from what is wrong (HM_ERR_BAD), through ask() too (finding 1 of a review of
 * 61e3d8e).  The control socket is fd 3 (CTL), here one end of a socketpair */
#include "hmplugin.c"
#include <fcntl.h>

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s (line %d)\n", what, __LINE__); fails++; } else printf("ok   %s\n", what); } while (0)

static int waiting(int fd) { uint8_t b[64]; return recv(fd, b, sizeof b, MSG_DONTWAIT) > 0; }
/* the state of the MUSIC_STATE waiting on FD, -1 none */
static int got(int fd)
{
    uint8_t b[64]; long n = recv(fd, b, sizeof b, MSG_DONTWAIT); struct pm_hdr h; const uint8_t *body; size_t bl; struct pm_music_state m;
    if (n <= 0 || pm_head(b, (size_t)n, &h, &body, &bl) || h.type != PM_MUSIC_STATE || pm_get_music_state(body, bl, &m)) return -1;
    return m.state;
}

int main(void)
{
    int sv[2]; uint8_t b[64]; struct pm_w w; struct pm_music_state m = { HM_MUSIC_PLAYING, pm_s("t"), pm_s("a") };
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv)) return 1;
    if (sv[0] == CTL) sv[0] = fcntl(sv[0], F_DUPFD, 10);         /* (the test's end never fd 3, which CTL takes) */
    if (sv[0] < 0 || dup2(sv[1], CTL) != CTL) return 1;
    ver[PM_SCOPE_CORE] = 1; ver[PM_SCOPE_AUDIO_MUSIC] = 1;
    pm_winit(&w, b, sizeof b); pm_put_music_state(&w, 0, &m);
    CHECK(put(&w) == 0 && waiting(sv[0]), "a packet encoded for this send: sent");
    CHECK(put(&w) == -1 && !waiting(sv[0]), "the same buffer once more: refused, not sent");
    m.state = PM_MUSIC_BUFFERING; pm_winit(&w, b, sizeof b); pm_put_music_state(&w, 0, &m);
    CHECK(put(&w) == 0 && got(sv[0]) == PM_MUSIC_PLAYING, "MUSIC_BUFFERING (audio.music 2, its fallback PLAYING) at audio.music 1: sent as PLAYING");
    ver[PM_SCOPE_AUDIO_MUSIC] = 2; pm_winit(&w, b, sizeof b); pm_put_music_state(&w, 0, &m);
    CHECK(put(&w) == 0 && got(sv[0]) == PM_MUSIC_BUFFERING, "... at audio.music 2: sent as it is");
    m.state = 9; pm_winit(&w, b, sizeof b); pm_put_music_state(&w, 0, &m);
    CHECK(put(&w) == -1 && put_why == HM_ERR_BAD && !waiting(sv[0]), "a state no version has: not sent, HM_ERR_BAD");
    { struct pm_store_get g = { pm_s("k") }; int why = 0;
      pm_winit(&w, b, sizeof b); pm_put_store_get(&w, 7, &g);
      CHECK(put(&w) == -1 && put_why == HM_ERR_NOT_SPOKEN && !waiting(sv[0]), "STORE_GET where storage is not spoken: not sent, HM_ERR_NOT_SPOKEN");
      pm_winit(&w, b, sizeof b); pm_put_store_get(&w, 8, &g);
      CHECK(ask(&w, 8, NULL, &why, NULL, NULL) == -1 && why == HM_ERR_NOT_SPOKEN && !waiting(sv[0]), "... a request's *why through ask() (no wait for an answer that cannot come)");
      CHECK(HM_ERR_NOT_SPOKEN == 0xffff && HM_ERR_NOT_SPOKEN > HM_ERR_UNSET, "(the client's own code, apart from every ERR_* of the wire)"); }
    /* the demo's BUTTON_KEY_MIC=4@2>- (BUTTON, marked drop): the only kind of packet pm_fit leaves unsent.  A plugin
     * sends no BUTTON; put() holds the branch all the same: not sent, HM_ERR_NOT_SPOKEN */
    { struct pm_button bt = { PM_BUTTON_KEY_MIC, PM_BUTTON_HOW_PRESS };
      ver[PM_SCOPE_BUTTON] = 1; pm_winit(&w, b, sizeof b); pm_put_button(&w, 0, &bt);
      CHECK(pm_droppable(PM_BUTTON) && !pm_droppable(PM_MUSIC_STATE) && put(&w) == -1 && put_why == HM_ERR_NOT_SPOKEN && !waiting(sv[0]), "a packet pm_fit leaves unsent (BUTTON_KEY_MIC at button 1): not sent, HM_ERR_NOT_SPOKEN"); }
    printf("hmplugin put: %s\n", fails ? "FAILED" : "all passed");
    return fails != 0;
}
