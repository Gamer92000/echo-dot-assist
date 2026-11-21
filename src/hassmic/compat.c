/* What older bionic lacks, for the Android models whose NDK API level is below the function's (rook: Android 5.1, API 22;
 * getifaddrs, freeifaddrs, if_nameindex and if_freenameindex came with API 24, open_memstream with 23).  Compiled in only below that level, so a build
 * for a newer Android is byte for byte what it was.  Only what hassmic uses: IPv4 addresses with their flags, and a
 * write-only memory stream that is read after fclose(). */
#ifdef __ANDROID__
#include <android/api-level.h>
#if __ANDROID_API__ < 24
#include <dirent.h>
#include <ifaddrs.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include "compat.h"

/* SIOCGIFCONF lists every IPv4 address of every interface that has one, up or not (netaddr.c needs the down ones too) */
struct compat_ifa {
    struct ifaddrs ifa;
    char name[IFNAMSIZ];
    struct sockaddr_storage addr, mask;
};

int getifaddrs(struct ifaddrs **out)
{
    struct ifaddrs *head = NULL, **tail = &head;
    struct ifconf ic; struct ifreq *rq;
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0), cap = 16;
    if (s < 0) return -1;
    for (;;) {
        rq = calloc(cap, sizeof *rq);
        if (!rq) { close(s); return -1; }
        ic.ifc_len = cap * sizeof *rq; ic.ifc_req = rq;
        if (ioctl(s, SIOCGIFCONF, &ic) < 0) { free(rq); close(s); return -1; }
        if (ic.ifc_len < (int)(cap * sizeof *rq)) break;      /* it all fit */
        free(rq); cap *= 2;
    }
    for (int i = 0; i < (int)(ic.ifc_len / sizeof *rq); i++) {
        struct compat_ifa *c = calloc(1, sizeof *c); struct ifreq q;
        if (!c) { freeifaddrs(head); free(rq); close(s); return -1; }
        strncpy(c->name, rq[i].ifr_name, IFNAMSIZ - 1);
        memcpy(&c->addr, &rq[i].ifr_addr, sizeof(struct sockaddr_in));
        c->ifa.ifa_name = c->name; c->ifa.ifa_addr = (struct sockaddr *)&c->addr; 
        memset(&q, 0, sizeof q); strncpy(q.ifr_name, c->name, IFNAMSIZ - 1);
        if (ioctl(s, SIOCGIFFLAGS, &q) == 0) c->ifa.ifa_flags = (unsigned short)q.ifr_flags;
        memset(&q, 0, sizeof q); strncpy(q.ifr_name, c->name, IFNAMSIZ - 1);
        if (ioctl(s, SIOCGIFNETMASK, &q) == 0) {
            memcpy(&c->mask, &q.ifr_netmask, sizeof(struct sockaddr_in)); c->ifa.ifa_netmask = (struct sockaddr *)&c->mask;
        }
        *tail = &c->ifa; tail = &c->ifa.ifa_next;
    }
    free(rq); close(s);
    *out = head;
    return 0;
}

void freeifaddrs(struct ifaddrs *ifa)
{
    while (ifa) { struct ifaddrs *n = ifa->ifa_next; free(ifa); ifa = n; }
}

/* every interface by name and index, up or not: /sys/class/net/<name>/ifindex */
struct if_nameindex *if_nameindex(void)
{
    DIR *d = opendir("/sys/class/net");
    struct dirent *e; struct if_nameindex *v = NULL; int n = 0, cap = 0;
    if (!d) return NULL;
    while ((e = readdir(d))) {
        char path[96], num[16]; int f, k;
        if (e->d_name[0] == '.' || strlen(e->d_name) >= IFNAMSIZ) continue;
        snprintf(path, sizeof path, "/sys/class/net/%s/ifindex", e->d_name);
        if ((f = open(path, O_RDONLY | O_CLOEXEC)) < 0) continue;
        k = read(f, num, sizeof num - 1); close(f);
        if (k <= 0) continue;
        num[k] = 0;
        if (n + 2 > cap) {
            struct if_nameindex *w = realloc(v, (cap = cap ? cap * 2 : 8) * sizeof *v);
            if (!w) { if (v) { v[n].if_index = 0; v[n].if_name = NULL; if_freenameindex(v); } closedir(d); return NULL; }
            v = w;
        }
        v[n].if_index = (unsigned)atoi(num); v[n].if_name = strdup(e->d_name);
        if (v[n].if_name) n++;
    }
    closedir(d);
    if (!v && !(v = malloc(sizeof *v))) return NULL;
    v[n].if_index = 0; v[n].if_name = NULL;
    return v;
}

void if_freenameindex(struct if_nameindex *v)
{
    if (!v) return;
    for (struct if_nameindex *i = v; i->if_index || i->if_name; i++) free(i->if_name);
    free(v);
}

#if __ANDROID_API__ < 23
FILE *funopen(const void *cookie, int (*readfn)(void *, char *, int), int (*writefn)(void *, const char *, int), fpos_t (*seekfn)(void *, fpos_t, int), int (*closefn)(void *));

struct memstream { char **buf; size_t *len; size_t used, cap; char *data; };

static int ms_write(void *cookie, const char *p, int n)
{
    struct memstream *m = cookie;
    if (m->used + n + 1 > m->cap) {
        size_t cap = (m->cap ? m->cap : 256) * 2; char *d;
        while (cap < m->used + n + 1) cap *= 2;
        if (!(d = realloc(m->data, cap))) return -1;
        m->data = d; m->cap = cap;
    }
    memcpy(m->data + m->used, p, n); m->used += n; m->data[m->used] = 0;
    return n;
}

static int ms_close(void *cookie)
{
    struct memstream *m = cookie;
    if (!m->data) m->data = calloc(1, 1);
    *m->buf = m->data; *m->len = m->used;
    free(m);
    return 0;
}

FILE *open_memstream(char **buf, size_t *len)
{
    struct memstream *m = calloc(1, sizeof *m);
    FILE *f;
    if (!m) return NULL;
    m->buf = buf; m->len = len;
    if (!(f = funopen(m, NULL, ms_write, NULL, ms_close))) free(m);
    return f;
}
#endif
#endif
#endif
