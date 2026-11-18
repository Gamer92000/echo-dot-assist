/*
 * The country Home Assistant's home is in.  Home Assistant tells devices its zone only as a rule (clock.c, tz.c), but
 * any device may read any entity's state and attributes, zone.home's latitude and longitude among them (the location
 * set when Home Assistant was set up: its onboarding offers the place it found the house at).  The country decides the
 * zone's name among those of the rule, and the name the language (main.c speech_locale).
 *
 * The borders: Natural Earth's countries at 1:10m (public domain), cut to what a country lookup needs by
 * tools/geo/build.py into geo/borders.bin (zlib'd, built in; geo/README.md has its size and self-check): land borders
 * and enclaves' to ~500 m, coasts near another country as finely, other coasts to ~10 km, islands under ~25 km² with
 * no land border left out but each country's largest.  Near a border the other country is named within GEO_BORDER_KM
 * (clock.c lets the Wi-Fi's country decide there).  ~10 ms on an Echo Dot 2 (2026-11-18), ~1.5 ms on a PC.  tools/geo/geo.py is the format and this lookup in Python (tests/unit/geo_ref.py
 * holds the two to each other).
 *
 * A point in several outer rings is an enclave's (San Marino in Italy): the smallest ring by its box.  Holes (the
 * enclave's outline as the surrounding country's) count only for what is near.  In none: the nearest ring within
 * GEO_NEAR_MAX_KM.  Distances flat around the point's latitude: kilometres matter here, not more.
 * The data is our own, built in; every read is still bounded.
 */
#include "geo.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

extern const unsigned geo_borders_len, geo_borders_raw;
extern const unsigned char geo_borders[];

struct ring { char cc[3]; int hole; int32_t x0, y0, x1, y1; uint32_t n, bytes; const uint8_t *data; };

/* the borders inflated: inflate() itself, not uncompress(), which the stub libraries (devices/<codename>/stubs) do
 * not list */
static uint8_t *inflated(uLong *len)
{
    z_stream z; uint8_t *out = malloc(geo_borders_raw ? geo_borders_raw : 1); int r;
    memset(&z, 0, sizeof z);
    if (!out || inflateInit2(&z, 15) != Z_OK) { free(out); return NULL; }
    z.next_in = (Bytef *)geo_borders; z.avail_in = geo_borders_len; z.next_out = out; z.avail_out = geo_borders_raw;
    r = inflate(&z, Z_FINISH); *len = z.total_out; inflateEnd(&z);
    if (r != Z_STREAM_END) { free(out); return NULL; }
    return out;
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* the ring's points into pts (n of them); 0 if its data is not what it says */
static int points(const struct ring *r, int32_t *pts)
{
    const uint8_t *p = r->data, *end = r->data + r->bytes; int64_t x = 0, y = 0;
    for (uint32_t i = 0; i < r->n; i++)
        for (int k = 0; k < 2; k++) {
            uint64_t v = 0; int s = 0;
            do { if (p >= end || s > 35) return 0; v |= (uint64_t)(*p & 127) << s; s += 7; } while (*p++ & 128);
            int64_t d = (v & 1) ? -(int64_t)(v >> 1) - 1 : (int64_t)(v >> 1);
            if (k) y += d; else x += d;
            if (x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX) return 0;
            pts[2 * i + k] = (int32_t)(k ? y : x);
        }
    return p == end;
}

static int inside(const int32_t *pts, uint32_t n, int32_t x, int32_t y)
{
    int c = 0;
    for (uint32_t i = 0, j = n - 1; i < n; j = i++) {
        double x1 = pts[2 * i], y1 = pts[2 * i + 1], x2 = pts[2 * j], y2 = pts[2 * j + 1];
        if ((y1 > y) != (y2 > y) && x < (x2 - x1) * (y - y1) / (y2 - y1) + x1) c = !c;
    }
    return c;
}

/* km from (x, y) to the ring's edge, flat around the point's latitude */
static double ring_km(const int32_t *pts, uint32_t n, int32_t x, int32_t y, double cx, double k)
{
    double best = 1e18, px = x * cx, py = y;
    for (uint32_t i = 0, j = n - 1; i < n; j = i++) {
        double ax = pts[2 * j] * cx, ay = pts[2 * j + 1], bx = pts[2 * i] * cx, by = pts[2 * i + 1];
        double dx = bx - ax, dy = by - ay, L = dx * dx + dy * dy;
        double t = L == 0 ? 0 : ((px - ax) * dx + (py - ay) * dy) / L;
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        double d = hypot(px - ax - t * dx, py - ay - t * dy);
        if (d < best) best = d;
    }
    return best * k;
}

int32_t geo_unit(double deg, uint32_t scale) { return (int32_t)lrint(deg * scale); }   /* half to even, as Python's round() */

int geo_country(double lat, double lon, char cc[3], char near[][3], int *nnear)
{
    *nnear = 0; cc[0] = 0;
    if (!(lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180)) return 0;   /* NaN too */
    uLong rawlen = 0; uint8_t *raw = inflated(&rawlen);
    if (!raw || rawlen < 16 || memcmp(raw, "HMGEO1\n", 8)) { free(raw); return 0; }
    uint32_t scale = le32(raw + 8), nr = le32(raw + 12), at = 16, maxn = 0;
    /* a scale lrint() keeps in a 32-bit long at 180°, and a ring count the file can hold */
    struct ring *rings = scale && scale <= 1000000 && nr <= rawlen / 28 ? calloc(nr ? nr : 1, sizeof *rings) : NULL;
    for (uint32_t i = 0; rings && i < nr; i++) {           /* the index: each ring's box and where its points are */
        struct ring *r = &rings[i];
        if (rawlen - at < 28) { nr = i; break; }
        memcpy(r->cc, raw + at, 2); r->cc[2] = 0; r->hole = raw[at + 2] & 1;
        r->x0 = (int32_t)le32(raw + at + 4); r->y0 = (int32_t)le32(raw + at + 8); r->x1 = (int32_t)le32(raw + at + 12); r->y1 = (int32_t)le32(raw + at + 16);
        r->n = le32(raw + at + 20); r->bytes = le32(raw + at + 24); at += 28;
        if (r->bytes > rawlen - at || r->n < 3 || r->n > r->bytes / 2) { nr = i; break; }
        r->data = raw + at; at += r->bytes;
        if (r->n > maxn) maxn = r->n;
    }
    int32_t *pts = rings ? malloc((size_t)maxn * 2 * sizeof *pts + 1) : NULL;
    int found = 0;
    if (pts) {
        int32_t x = geo_unit(lon, scale), y = geo_unit(lat, scale);
        double cx = cos(lat * (M_PI / 180)), k = 111.195 / scale, ex = cx > 0.01 ? cx : 0.01;
        int64_t best_area = -1; const struct ring *best = NULL;
        for (uint32_t i = 0; i < nr; i++) {
            const struct ring *r = &rings[i];
            if (r->hole || x < r->x0 || x > r->x1 || y < r->y0 || y > r->y1 || !points(r, pts) || !inside(pts, r->n, x, y)) continue;
            int64_t a = ((int64_t)r->x1 - r->x0) * ((int64_t)r->y1 - r->y0);
            if (!best || a < best_area) { best = r; best_area = a; }
        }
        /* the others near it; in none, the nearest within reach */
        double reach = best ? GEO_BORDER_KM : GEO_NEAR_MAX_KM, rx = reach / k / ex, ry = reach / k, dist[GEO_SEEN_MAX];
        char seen[GEO_SEEN_MAX][3]; int ns = 0;
        for (uint32_t i = 0; i < nr; i++) {
            const struct ring *r = &rings[i];
            if (x < r->x0 - rx || x > r->x1 + rx || y < r->y0 - ry || y > r->y1 + ry || !points(r, pts)) continue;
            double d = ring_km(pts, r->n, x, y, cx, k);
            if (d > reach) continue;
            int s = 0;
            while (s < ns && strcmp(seen[s], r->cc)) s++;
            if (s == ns) { if (ns == GEO_SEEN_MAX) continue; memcpy(seen[ns], r->cc, 3); dist[ns++] = d; }
            else if (d < dist[s]) dist[s] = d;
        }
        const char *own = best ? best->cc : NULL;
        if (!own) {                                         /* the nearest; of two as near, the first by name */
            int m = -1;
            for (int s = 0; s < ns; s++) if (m < 0 || dist[s] < dist[m] || (dist[s] == dist[m] && strcmp(seen[s], seen[m]) < 0)) m = s;
            if (m >= 0) own = seen[m];
        }
        if (own) {
            found = 1; memcpy(cc, own, 3);
            char all[GEO_SEEN_MAX][3]; int na = 0;     /* sorted by name, the first GEO_NEAR_MAX: as geo.py */
            for (int s = 0; s < ns; s++)
                if (strcmp(seen[s], cc) && dist[s] <= GEO_BORDER_KM) {
                    int j = na++;
                    while (j > 0 && strcmp(all[j - 1], seen[s]) > 0) { memcpy(all[j], all[j - 1], 3); j--; }
                    memcpy(all[j], seen[s], 3);
                }
            for (int j = 0; j < na && j < GEO_NEAR_MAX; j++) memcpy(near[(*nnear)++], all[j], 3);
        }
    }
    free(pts); free(rings); free(raw);
    return found;
}
