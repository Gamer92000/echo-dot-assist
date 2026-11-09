/* Where the replay after a wake word starts: see wordtail.h. */
#include "wordtail.h"
#include <math.h>

#define GAP_DB 9.0f                     /* this far under the word's loudest: not the word */
#define OVER_DB 12.0f                   /* where the replay starts inside the pause */
#define PAUSE 10                        /* frames: 100 ms */

/* the 30 ms mean around frame i, in dB; 0 when frame i + 1 has not come yet (and more will) */
static int smooth(const float *db, int n, int i, float *out)
{
    if (i + 1 >= n && n < WORDTAIL_FRAMES) return 0;
    float p = 0; int k = 0;
    for (int j = i - 1; j <= i + 1; j++) if (j >= 0 && j < n) { p += powf(10.0f, db[j] / 10.0f); k++; }
    *out = 10.0f * log10f(p / k + 1e-12f);
    return 1;
}

/* a4fe0bd's rule on the raw frames: up to the first 12 dB under the keyword, or the lowest before a 3 dB rise, at
 * most 200 ms and never past the live block, which without arbitration came 120-180 ms after the end: so 120 ms
 * here.  What a fallback may leave out at most */
static int first_version(const float *db, int n, float keyword_db)
{
    float low = 1e9f; int low_at = 0;
    for (int i = 0; i < n && i < 12; i++) {
        if (db[i] < keyword_db - 12) return i;
        if (db[i] > low + 3) break;
        if (db[i] < low) { low = db[i]; low_at = i; }
    }
    return low_at;
}

int wordtail_cut(const float *db, int n, float keyword_db, int pause_ok, const char **why)
{
    float top = keyword_db, run_low = 0, first_low = 0, s;
    int run = -1, run_at = 0, over_at = -1, first_at = -1, first_start = -1, runs = 0;
    if (n > WORDTAIL_FRAMES) n = WORDTAIL_FRAMES;
    for (int i = 0; i < n; i++) {
        if (!smooth(db, n, i, &s)) return -1;
        if (s > top - GAP_DB) {                     /* the word, or the next one */
            if (s > top) top = s;
            run = -1;
            continue;
        }
        if (run < 0) { run = i; run_low = s; run_at = i; over_at = -1; if (!runs++) first_start = i; }
        if (s < run_low) { run_low = s; run_at = i; }
        if (runs == 1 && (first_at < 0 || s < first_low)) { first_low = s; first_at = i; }
        if (over_at < 0 && s <= top - OVER_DB) over_at = i;
        if (pause_ok && i - run + 1 >= PAUSE) { *why = "a pause"; return over_at >= 0 ? over_at : run_at; }
    }
    if (n < WORDTAIL_FRAMES) return -1;
    int cut = first_at >= 0 && first_start < WORDTAIL_EARLY ? first_at : 0, old = first_version(db, n, keyword_db);
    if (old < cut) cut = old;
    *why = cut ? "no pause: an early dip" : "no pause: nothing";
    return cut;
}
