#!/bin/sh
# Run an armv7 bionic binary on the PC against the extracted rootfs.
# New PID namespace is required: bionic mutexes store a 16-bit owner tid and deadlock when pid > 65535.
# Usage: [DEVICE=<codename>] tools/qrun.sh [-t seconds] <binary> [args...]   (paths inside args are resolved inside the
# rootfs of that model's firmware, firmware/$DEVICE/rootfs, default donut)
#        tools/qrun.sh --userns   the sysctl and value that let users make namespaces here, if one is known (the setup)
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# What refuses users a namespace: Ubuntu 24.04 and later by AppArmor ("write failed /proc/self/uid_map: Operation not
# permitted", Ubuntu 26.04, 2026-10-26), older Debian kernels by their own switch.  Without the namespace the PIDs are
# the PC's (pid_max 4194304 on Ubuntu), and a run that gets one above 65535 hangs until the timeout: no run without it
knob() {
    if [ "$(cat /proc/sys/kernel/apparmor_restrict_unprivileged_userns 2> /dev/null)" = 1 ]; then
        echo kernel.apparmor_restrict_unprivileged_userns 0
    elif [ "$(cat /proc/sys/kernel/unprivileged_userns_clone 2> /dev/null)" = 0 ]; then
        echo kernel.unprivileged_userns_clone 1
    fi
}
[ "$1" = "--userns" ] && { knob; exit 0; }
T=120
[ "$1" = "-t" ] && { T=$2; shift 2; }
QEMU=$(command -v qemu-arm || true)
[ -n "$QEMU" ] || [ ! -x "$ROOT/toolchain/qemu-arm" ] || QEMU="$ROOT/toolchain/qemu-arm"   # the Shows' setup.sh downloads one
[ -n "$QEMU" ] || { echo "qrun.sh: no qemu-arm (the system's, or toolchain/qemu-arm)" >&2; exit 1; }
if ! unshare --user --map-root-user --pid --fork true 2> /dev/null; then
    set -- $(knob)
    echo "qrun.sh: this PC does not let users make namespaces, and bionic hangs without one (process ids over 65535)." >&2
    [ -n "$1" ] && echo "qrun.sh: allow them until the next boot with: sudo sysctl $1=$2" >&2
    exit 1
fi
exec timeout -s KILL "$T" unshare --user --map-root-user --pid --fork \
    "$QEMU" -L "$ROOT/firmware/${DEVICE:-donut}/rootfs" -E LD_LIBRARY_PATH=/system/lib:/system/vendor/lib "$@"
