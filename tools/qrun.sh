#!/bin/sh
# Run an armv7 bionic binary on the PC against the extracted rootfs.
# New PID namespace is required: bionic mutexes store a 16-bit owner tid and deadlock when pid > 65535.
# Usage: [DEVICE=<codename>] tools/qrun.sh [-t seconds] <binary> [args...]   (paths inside args are resolved inside the
# rootfs of that model's firmware, firmware/$DEVICE/rootfs, default donut)
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T=120
[ "$1" = "-t" ] && { T=$2; shift 2; }
QEMU=$(command -v qemu-arm || true)
[ -x "$ROOT/toolchain/qemu-arm" ] && QEMU="$ROOT/toolchain/qemu-arm"     # devices/checkers/setup.sh downloads one
[ -n "$QEMU" ] || { echo "qrun.sh: no qemu-arm (the system's, or toolchain/qemu-arm)" >&2; exit 1; }
exec timeout -s KILL "$T" unshare --user --map-root-user --pid --fork \
    "$QEMU" -L "$ROOT/firmware/${DEVICE:-donut}/rootfs" -E LD_LIBRARY_PATH=/system/lib:/system/vendor/lib "$@"
