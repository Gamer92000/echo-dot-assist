/*
 * Home Assistant's time zone, as a name Android knows.
 *
 * Home Assistant says its zone with the time (GetTimeResponse field 2, aioesphomeapi's timezone.py), but as a POSIX TZ
 * string ("CET-1CEST,M3.5.0,M10.5.0/3"), the rule its zone follows from now on: the name (Europe/Berlin) stays in
 * Home Assistant.  bionic takes such a string in persist.sys.timezone as it is (tzcode's tzparse when no zone has that
 * name), but Android's Java side (Echo Show screens, AlarmManager) only names: it falls back to GMT.  So the Echo looks
 * for a zone with that rule in its own time zone data, bionic's one-file tzdata: a header ("tzdata2016j\0", then the
 * offsets of the index, the data and zone.tab, big-endian), an index of 52-byte entries (40-byte name, start, length,
 * unused) and every zone's TZif file as zic wrote it, which since TZif version 2 ends in "\n<POSIX TZ string>\n".
 *
 * Many zones share a rule (Europe/Amsterdam, Europe/Berlin, Europe/Paris...): any of them shows the right time from
 * now on, which is all the Echo needs; only the name differs.  The zone already set is kept when it is one of them,
 * else the first of the most usual area among those zone.tab lists (the file's last part): the data holds old names as
 * zones of their own (Australia/ACT, Asia/Calcutta, US/Eastern; Dot 3's tzdata2025a: 596 zones, all with the string).  A rule the Echo's (older) data does not have, as a zone that changed it
 * since the firmware was built (Mexico's 2022 end of summer time), finds a zone with that rule elsewhere, or none:
 * then the string itself goes to root, which native code (and the logs' local time, if any) still gets right.
 */
#include "tz.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NAME_LEN   40
#define ENTRY_LEN  52
#define FOOTER_MAX 128
#define ZONETAB_MAX (1 << 20)               /* 17 kB in tzdata2025a */

static uint32_t be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int area(const char *n)
{
    static const char *const areas[] = { "Europe/", "America/", "Asia/", "Australia/", "Africa/", "Pacific/", "Atlantic/",
                                         "Indian/", "Antarctica/" };
    for (size_t i = 0; i < sizeof areas / sizeof *areas; i++) if (!strncmp(n, areas[i], strlen(areas[i]))) return (int)i;
    return strchr(n, '/') && strncmp(n, "Etc/", 4) ? 20 : 30;
}

/* zone.tab lists it: "CC<tab>coordinates<tab>name[<tab>comment]" */
static int listed(const char *tab, const char *n)
{
    size_t l = strlen(n);
    for (const char *p = tab; (p = strstr(p, n)); p += l)
        if (p > tab && p[-1] == '\t' && (p[l] == '\t' || p[l] == '\n' || !p[l])) return 1;
    return 0;
}

/* lower is more usual: a name zone.tab lists, then by area; names without an area (CET, EST5EDT, Poland) and Etc/ last */
static int rank(const char *tab, const char *n) { return area(n) + (tab && !listed(tab, n) ? 40 : 0); }

/* the POSIX TZ string at the end of the TZif data at off..off+len, into f; 0 if it has none (TZif version 1) */
static int footer(FILE *fp, long off, uint32_t len, char *f)
{
    unsigned char b[FOOTER_MAX + 2]; size_t n = len < sizeof b ? len : sizeof b, i;
    if (len < 2 || fseek(fp, off + (long)(len - n), SEEK_SET) || fread(b, 1, n, fp) != n || b[n - 1] != '\n') return 0;
    for (i = n - 1; i > 0 && b[i - 1] != '\n'; i--) ;
    if (!i) return 0;                                  /* no newline before it within reach: not a footer of ours */
    memcpy(f, b + i, n - 1 - i); f[n - 1 - i] = 0;
    return 1;
}

int tz_lookup(const char *path, const char *posix, const char *cur, char *out, size_t cap)
{
    unsigned char h[24], e[ENTRY_LEN];
    char f[FOOTER_MAX + 1], best[NAME_LEN + 1] = "", name[NAME_LEN + 1];
    int best_rank = 999, found = 0;
    char *tab = NULL;
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    if (fread(h, 1, sizeof h, fp) != sizeof h || memcmp(h, "tzdata", 6) || h[11]) { fclose(fp); return 0; }
    uint32_t idx = be32(h + 12), data = be32(h + 16), zt = be32(h + 20);
    if (idx > data || data > zt || (data - idx) % ENTRY_LEN) { fclose(fp); return 0; }
    long size = !fseek(fp, 0, SEEK_END) ? ftell(fp) : -1;
    unsigned long long tl = size > 0 && (unsigned long long)size > zt ? (unsigned long long)size - zt : 0;   /* 32-bit long */
    if (tl && tl <= ZONETAB_MAX && zt <= 0x7fffffffu && !fseek(fp, (long)zt, SEEK_SET) && (tab = malloc((size_t)tl + 1))) {
        size_t n = fread(tab, 1, (size_t)tl, fp);
        tab[n] = 0;
        if (!n) { free(tab); tab = NULL; }
    }                                                   /* none (or absurd): area order alone */
    for (uint32_t at = idx; at < data; at += ENTRY_LEN) {
        if (fseek(fp, (long)at, SEEK_SET) || fread(e, 1, sizeof e, fp) != sizeof e) break;
        memcpy(name, e, NAME_LEN); name[NAME_LEN] = 0;
        uint32_t start = be32(e + NAME_LEN), len = be32(e + NAME_LEN + 4);
        if (!name[0] || start > zt - data || len > zt - data - start) continue;
        if (!footer(fp, (long)(data + start), len, f) || strcmp(f, posix)) continue;
        found = 1;
        if (cur && !strcmp(name, cur)) { snprintf(best, sizeof best, "%s", cur); break; }
        int r = rank(tab, name);
        if (r < best_rank) { best_rank = r; snprintf(best, sizeof best, "%s", name); }
    }
    fclose(fp); free(tab);
    if (!found || strlen(best) >= cap) return 0;
    memcpy(out, best, strlen(best) + 1);
    return 1;
}
