/* Apps of the owner's on an Echo with Android (the Shows): which of them may reach the internet.
 *
 * The egress lock (scripts/device/lockdown.sh) lets out hassmic's own sockets and nothing else of the Echo's: an app the
 * owner installed (a video client the screen app starts, docs/screen-apps.md "Other apps") fetches nothing until the
 * owner allows it here, on the settings page.  hassmic keeps the list in state/apps ("net <package>" lines); root's
 * firewall reads it, resolves each name to the app's uid and lets that uid out, refusing anything that is not an app
 * the owner installed (Amazon's own, an updated system app, a shared system uid).
 *
 * Every function: any thread; apps has its own lock and calls nothing that takes core_lock. */
#ifndef APPS_H
#define APPS_H
#include <stddef.h>

/* malloc'd JSON: {"apps":[{"package","net","lan"}...]}: every app installed in /data/app that is no system app's update,
 * and whether it may reach the internet; NULL where there is no Android (packages.xml) or no memory */
char *apps_json(size_t *len);
/* net 1: the package may reach the internet; 0 ok, -1 not an installed app of the owner's, -2 not written */
int   apps_set_net(const char *pkg, int on);
/* lan 1: the ports the package listens on may be reached from the home network (private and link-local addresses:
 * root's firewall, lockdown.sh hassmic_in; issue #109: a DLNA renderer, a cast receiver); 0 ok, as apps_set_net */
int   apps_set_lan(const char *pkg, int on);

/* An APK in pieces (state/apps-staged.apk), each at most APPS_CHUNK, in order: offset 0 starts anew.  0 ok, -1 not
 * at the end of what is staged (409), -2 too big, -3 not written */
#define APPS_CHUNK   262144
#define APPS_APK_MAX (200L << 20)
int   apps_chunk(long off, const void *data, size_t n);
/* the staged APK checked (size, BLAKE2b-256 of the whole file, hex) and handed to root's apps.sh: 0 queued, -1 not
 * what was staged, -2 busy, -3 not written */
int   apps_install(long size, const char *hex);
/* an app of the owner's removed by root's apps.sh (never the screen app: root checks that); 0 queued, -1 not an app of
 * the owner's, -2 busy, -3 not written */
int   apps_uninstall(const char *pkg);
#endif
