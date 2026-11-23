/* The launcher protocol (docs/plugins.md 2.2): what hassmic (the broker, plugin.c) and root's launcher
 * (src/tools/plugrun.c) say to each other over the socketpair hassmic finds as descriptor HASSMIC_PLUGIN_FD.
 * SOCK_SEQPACKET, one struct plugrun_msg per packet, host byte order (both ends are one machine); a packet of another
 * size, an unknown type or a slot out of range closes the socket, and with it ends every plugin.
 * The launcher has no policy: whatever it is asked to start, it starts as a plugin user under the filter and the
 * limits.  What may run is hassmic's to decide (signatures, what the owner accepted). */
#ifndef PLUGRUN_H
#define PLUGRUN_H
#include <stdint.h>

/* 2: PLUGRUN_USAGE and PLUGRUN_USED.  3: SPAWN's d, its flags (PLUGRUN_SPAWN_DEVELOP), which a launcher before
 * reads nothing of (a plugin with the development flag starts without it there, said in the log).  A version
 * otherwise only ever adds messages.  Launcher and hassmic come in one bundle
 * and main.sh starts both from one folder, so the two differ only when one was pushed by hand:
 *  - an older launcher (1) under this hassmic: plugins run; hassmic never sends it PLUGRUN_USAGE (an unknown type
 *    ends the launcher and every plugin), so there is no CPU watchdog, and the log says so once;
 *  - this launcher under a hassmic of version 1: that hassmic takes only its own version in READY and runs no
 *    plugin ("the plugins' launcher did not answer"); a wake word engine's plugin counts as missing there, and
 *    Amazon's wake word listens. */
#define PLUGRUN_VERSION 3
#define PLUGRUN_SLOTS   8               /* users 6900..6907, one per slot */
#define PLUGRUN_UID     6900
#define PLUGRUN_FD_ENV  "HASSMIC_PLUGIN_FD"

enum {
    PLUGRUN_READY = 1,      /* launcher -> hassmic, once: a = PLUGRUN_VERSION, b = sandbox ok (the self test: a child
                             * under the filter got EPERM from open and socket), c = PLUGRUN_DOMAIN_*, d = CPU group ok */
    PLUGRUN_SPAWN,          /* hassmic -> launcher: slot, a = memory MB (clamped 8..96), b = threads (1..16),
                             * c = PLUGRUN_CPU_*, d = PLUGRUN_SPAWN_* flags (version 3; bits it does not know: none);
                             * three descriptors (SCM_RIGHTS, in this order): the program (read-only), the plugin's
                             * control socket, its stderr pipe */
    PLUGRUN_SPAWNED,        /* launcher -> hassmic: slot, a = pid */
    PLUGRUN_FAILED,         /* launcher -> hassmic: slot, a = errno, b = the step of 2.3 that failed (0: before the fork) */
    PLUGRUN_KILL,           /* hassmic -> launcher: slot (SIGTERM, SIGKILL after 2 s) */
    PLUGRUN_EXITED,         /* launcher -> hassmic: slot, a = wait status, b = user ms, c = system ms, d = peak RSS kB */
    /* The CPU watchdog's eyes (2.6).  hassmic cannot read another user's /proc/<pid>/stat on Android (hidepid);
     * root, the plugins' parent, can.  The launcher reads and reports, nothing more: what is too much, over which
     * time, and what follows are hassmic's (the launcher has no policy).
     * Asked for, not sent every 10 s by itself: the window is hassmic's to choose (a minute on an Echo, 2 s in the
     * tests) and a timer of the launcher's own would be a second copy of it; nothing is read for a plugin nobody
     * watches (the wake engine's, on most Echos the only one: no wakeup, no /proc read, ever); and the answer's two
     * numbers are read at one moment for exactly the asker.  The price is one more message root takes from hassmic:
     * a type and a slot, checked as KILL's are, answered with one read of the launcher's own child's file. */
    PLUGRUN_USAGE,          /* hassmic -> launcher: slot; no descriptors */
    PLUGRUN_USED,           /* launcher -> hassmic: slot, a = pid (0: nothing runs in the slot), b = its CPU time so
                             * far in ms, user and system, all threads living and gone (/proc/<pid>/stat), c = the
                             * launcher's CLOCK_MONOTONIC in ms at that reading, both modulo 2^32; d = 1 read, 0 not */
};
enum { PLUGRUN_DOMAIN_NONE, PLUGRUN_DOMAIN_HASSMIC, PLUGRUN_DOMAIN_PLUGIN };   /* stays in su / u:r:hassmic:s0 / u:r:hassmic_plugin:s0 */
enum { PLUGRUN_CPU_ORDINARY, PLUGRUN_CPU_WAKE };    /* the plugins' shared group / a group of its own, a thread's full weight */
/* The development flag (docs/plugins.md 2.2, 5.8): a call the filter refuses raises SIGSYS (SECCOMP_RET_TRAP) instead
 * of answering EPERM, so an author sees which call his runtime makes; the SDK's client names it in its last line.
 * It widens nothing: the call is not made either way, and a plugin that catches SIGSYS gets what EPERM gives (the
 * calls above 386 still answer ENOSYS: musl's and Go's fallbacks).  The owner's, per plugin, on its card */
#define PLUGRUN_SPAWN_DEVELOP 1

struct plugrun_msg { uint32_t type, slot, a, b, c, d; };

/* what a plugin finds: 0, 1 /dev/null; 2 its stderr pipe; 3 the control socket; nothing else */
#define PLUGIN_FD_CONTROL 3
#endif
