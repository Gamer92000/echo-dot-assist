/* Lyrion Music Server player (SlimProto, as Squeezelite speaks it): the Echo in Lyrion's list of players.  Beside
 * Sendspin; off unless the setting "lyrion" is on.  See lyrion.c. */
#ifndef LYRION_H
#define LYRION_H
#include <stddef.h>

void   lyrion_start(void);                      /* the threads; they idle while the setting is off */
int    lyrion_enable(int set);                  /* the setting "lyrion" (settings.c keeps it): -1 reads */
/* the page's server: "host" or "host:port" (port: Lyrion's web port, 9000 by default; the player's own is always
 * 3483), "" finds it on the network.  0 saved, -1 not a host name or address */
int    lyrion_set_server(const char *host);
size_t lyrion_status_json(char *o, size_t cap); /* {"on","state","server","name","version","given","playing"}: the page's row */
/* What the Echo's own controls ask of Lyrion (its JSON-RPC, as any remote does), only while a track is loaded here: 1 =
 * taken.  None of them blocks: the request goes from a thread of its own */
int    lyrion_button(void);                     /* action button: play / pause */
int    lyrion_skip(int dir);                    /* pressed twice (+1: next) or three times (-1: previous) */
int    lyrion_stop(void);                       /* "<wake word>, stop": pause it if it plays.  1 = paused */
void   lyrion_pause(void);                      /* another source started: pause it if it plays */
void   lyrion_volume_changed(int percent);      /* volume changed on the Echo: tell Lyrion */
/* Lyrion's volume and its gain (audg, 16.16) both ways, for players of SqueezePlay's kind (Squeezelite's, ours):
 * Slim::Player::SqueezePlay getVolumeParameters, -74 dB over 0..100 in two lines with -37 dB at 25 (the low quarter
 * steeper), Squeezebox2 getVolume, and dBToFixed's rounding to 1/256 above -30 dB, which gives 41 and 42, 44 and 45, 47
 * and 48 one gain each: audg's old-style gain (Squeezebox2's volume_map) tells those apart.  Every volume comes back as
 * Lyrion had it (tests/unit/lyrion_test.c); a gain of another Lyrion's sums the nearest */
#include <math.h>
#include <stdint.h>
static inline double lyrion_db_of_volume(int v) { return v > 25 ? (v - 100) * 37.0 / 75 : v * 37.0 / 25 - 74; }
static inline double lyrion_gain_of_volume(int v) { return v <= 0 ? 0 : pow(10, lyrion_db_of_volume(v) / 20); }
static inline uint32_t lyrion_gain16(int v)             /* what Lyrion sends for v, to the bit */
{
    double db = lyrion_db_of_volume(v), m = pow(10, db / 20);
    return v <= 0 ? 0 : db >= -30 && db <= 0 ? (uint32_t)(m * 256 + 0.5) * 256 : (uint32_t)(m * 65536 + 0.5);
}
static inline int lyrion_volume_of_gain(uint32_t g16, uint32_t old)    /* 0: none */
{
    static const uint8_t old_map[101] = { 0, 1, 1, 1, 2, 2, 2, 3, 3, 4, 5, 5, 6, 6, 7, 8, 9, 9, 10, 11, 12, 13, 14, 15, 16, 16,
        17, 18, 19, 20, 22, 23, 24, 25, 26, 27, 28, 29, 30, 32, 33, 34, 35, 37, 38, 39, 40, 42, 43, 44, 46, 47, 48, 50, 51,
        53, 54, 56, 57, 59, 60, 61, 63, 65, 66, 68, 69, 71, 72, 74, 75, 77, 79, 80, 82, 84, 85, 87, 89, 90, 92, 94, 96, 97,
        99, 101, 103, 104, 106, 108, 110, 112, 113, 115, 117, 119, 121, 123, 125, 127, 128 };
    int best = 0;
    if (!g16) return 0;
    for (int v = 1; v <= 100; v++) if (lyrion_gain16(v) == g16 && (!best || old == old_map[v])) best = v;
    if (best) return best;
    double db = 20 * log10((double)g16 / 65536), v = db >= -37 ? 100 + db * 75 / 37 : (db + 74) * 25 / 37;
    long r = lround(v);
    return r < 1 ? 1 : r > 100 ? 100 : (int)r;
}
#endif
