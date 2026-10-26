#!/bin/sh
# Run an armv7 bionic binary on the PC against the extracted rootfs.
# New PID namespace is required: bionic mutexes store a 16-bit owner tid and deadlock when pid > 65535.
# Usage: [DEVICE=<codename>] tools/qrun.sh [-t seconds] <binary> [args...]   (paths inside args are resolved inside the
# rootfs of that model's firmware, firmware/$DEVICE/rootfs, default donut)
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T=120
[ "$1" = "-t" ] && { T=$2; shift 2; }
QEMU=$(command -v qemu-arm || true)
[ -n "$QEMU" ] || [ ! -x "$ROOT/toolchain/qemu-arm" ] || QEMU="$ROOT/toolchain/qemu-arm"   # the models' setup.sh downloads one
[ -n "$QEMU" ] || { echo "qrun.sh: no qemu-arm (the system's, or toolchain/qemu-arm)" >&2; exit 1; }
NS="unshare --user --map-root-user --pid --fork"
# Ubuntu 24.04 and later refuse users a user namespace (AppArmor: kernel.apparmor_restrict_unprivileged_userns=1;
# "write failed /proc/self/uid_map: Operation not permitted" on Ubuntu 26.04, 2026-10-26).  Without one the PIDs are the
# PC's: fine while they are well below 65536 (a run takes a few; they start low after a boot, pid_max is 4194304 there).
if ! $NS true 2> /dev/null; then
    last=$(cat /proc/sys/kernel/ns_last_pid 2> /dev/null || echo 99999)
    if [ "$last" -lt 60000 ]; then
        NS=
        echo "qrun.sh: no user namespace allowed here; process ids are at $last, below bionic's 65536: qemu runs without one" >&2
    else
        echo "qrun.sh: no user namespace allowed here (Ubuntu: AppArmor), and process ids are at $last: bionic would hang above 65535." >&2
        echo "  Allow namespaces until the next boot: sudo sysctl kernel.apparmor_restrict_unprivileged_userns=0" >&2
        echo "  or reboot the PC (ids start low again) and run this soon after." >&2
        exit 1
    fi
fi
exec timeout -s KILL "$T" $NS \
    "$QEMU" -L "$ROOT/firmware/${DEVICE:-donut}/rootfs" -E LD_LIBRARY_PATH=/system/lib:/system/vendor/lib "$@"
