/* Declarations for compat.c: what bionic's headers hide below the function's API level (older Android models) */
#ifndef HASSMIC_COMPAT_H
#define HASSMIC_COMPAT_H
#ifdef __ANDROID__
#include <android/api-level.h>
#include <stdio.h>
#if __ANDROID_API__ < 24
struct ifaddrs;
int getifaddrs(struct ifaddrs **out);
void freeifaddrs(struct ifaddrs *ifa);
struct if_nameindex;
struct if_nameindex *if_nameindex(void);
void if_freenameindex(struct if_nameindex *v);
#endif
#if __ANDROID_API__ < 23
FILE *open_memstream(char **buf, size_t *len);
#endif
#endif
#endif
