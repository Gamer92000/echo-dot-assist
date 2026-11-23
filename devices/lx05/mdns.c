/* Minimal LAN-only mDNS announcements for the LX05 ESPHome endpoint. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static unsigned char buf[1024];
static size_t n;
static char mac_txt[17];
static int read_mac(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;
    struct ifreq req = {0};
    strncpy(req.ifr_name, "wlan0", IFNAMSIZ - 1);
    int ok = ioctl(fd, SIOCGIFHWADDR, &req) == 0;
    close(fd);
    if (!ok) return 0;
    const unsigned char *m = (const unsigned char *)req.ifr_hwaddr.sa_data;
    snprintf(mac_txt, sizeof mac_txt, "mac=%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return 1;
}
static void b8(unsigned x) { if (n < sizeof buf) buf[n++] = x; }
static void b16(unsigned x) { b8(x >> 8); b8(x); }
static void b32(uint32_t x) { b16(x >> 16); b16(x); }
static void name(const char *s) {
    while (*s) { const char *p = strchr(s, '.'); size_t k = p ? (size_t)(p-s) : strlen(s); b8(k); while (k--) b8(*s++); if (*s == '.') s++; }
    b8(0);
}
static size_t record(const char *owner, unsigned type, unsigned cls) {
    name(owner); b16(type); b16(cls); b32(120); size_t at = n; b16(0); return at;
}
static void end(size_t at) { unsigned l = n-at-2; buf[at] = l >> 8; buf[at+1] = l; }
static void txt(const char *s) { size_t k = strlen(s); b8(k); while (k--) b8(*s++); }
static int ipaddr(struct in_addr *a) {
    struct ifaddrs *list, *p; int ok = 0;
    if (getifaddrs(&list)) return 0;
    for (p = list; p; p = p->ifa_next) if (p->ifa_addr && p->ifa_addr->sa_family == AF_INET && !strcmp(p->ifa_name, "wlan0")) {
        *a = ((struct sockaddr_in *)p->ifa_addr)->sin_addr; ok = 1; break;
    }
    freeifaddrs(list); return ok;
}
static void announce(int s, struct sockaddr_in *dst) {
    struct in_addr a; if (!ipaddr(&a)) return;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, &a, sizeof a);
    n=0; b16(0); b16(0x8400); b16(0); b16(7); b16(0); b16(0);
    size_t r = record("_esphomelib._tcp.local", 12, 1); name("xiaomi-lx05._esphomelib._tcp.local"); end(r);
    r = record("xiaomi-lx05._esphomelib._tcp.local", 33, 0x8001); b16(0); b16(0); b16(26053); name("xiaomi-lx05.local"); end(r);
    r = record("xiaomi-lx05._esphomelib._tcp.local", 16, 0x8001);
    if (read_mac()) txt(mac_txt); txt("friendly_name=Xiaomi-LX05"); txt("version=2025.5.0"); txt("platform=hassmic"); txt("network=wifi");
    txt("api_encryption_supported=Noise_NNpsk0_25519_ChaChaPoly_SHA256"); end(r);
    r = record("_sendspin._tcp.local", 12, 1); name("xiaomi-lx05._sendspin._tcp.local"); end(r);
    r = record("xiaomi-lx05._sendspin._tcp.local", 33, 0x8001); b16(0); b16(0); b16(28928); name("xiaomi-lx05.local"); end(r);
    r = record("xiaomi-lx05._sendspin._tcp.local", 16, 0x8001); txt("path=/sendspin"); txt("name=Xiaomi-LX05"); end(r);
    r = record("xiaomi-lx05.local", 1, 0x8001); const unsigned char *q=(const unsigned char *)&a.s_addr; for (int i=0;i<4;i++) b8(q[i]); end(r);
    sendto(s, buf, n, 0, (struct sockaddr *)dst, sizeof *dst);
}
int main(void) {
    int s=socket(AF_INET,SOCK_DGRAM,0), one=1; if (s<0) return 1;
    setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one);
    struct sockaddr_in bindaddr={.sin_family=AF_INET,.sin_port=htons(5353),.sin_addr.s_addr=htonl(INADDR_ANY)};
    if (bind(s,(struct sockaddr *)&bindaddr,sizeof bindaddr)) return 2;
    struct ip_mreq group={.imr_multiaddr.s_addr=inet_addr("224.0.0.251"),.imr_interface.s_addr=htonl(INADDR_ANY)};
    if (setsockopt(s,IPPROTO_IP,IP_ADD_MEMBERSHIP,&group,sizeof group)) return 3;
    unsigned char ttl=255; setsockopt(s,IPPROTO_IP,IP_MULTICAST_TTL,&ttl,sizeof ttl);
    struct sockaddr_in dst={.sin_family=AF_INET,.sin_port=htons(5353),.sin_addr.s_addr=inet_addr("224.0.0.251")};
    time_t last=0;
    for (;;) {
        time_t now=time(NULL); if (now-last>=60 || now<last) { announce(s,&dst); last=now; }
        fd_set f; FD_ZERO(&f); FD_SET(s,&f); struct timeval t={.tv_sec=5};
        if (select(s+1,&f,NULL,NULL,&t)>0) {
            unsigned char query[1500]; ssize_t len=recv(s,query,sizeof query,0);
            if (len>12 && !(query[2]&0x80) && (memmem(query+12,len-12,"_esphomelib",11) || memmem(query+12,len-12,"_sendspin",9) || memmem(query+12,len-12,"xiaomi-lx05",11))) announce(s,&dst);
        }
    }
}
