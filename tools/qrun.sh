#!/bin/sh
# Run an armv7 bionic binary on the PC against the extracted rootfs.
# New PID namespace is required: bionic mutexes store a 16-bit owner tid and deadlock when pid > 65535.
# Usage: tools/qrun.sh [-t seconds] <binary> [args...]   (paths inside args are resolved inside the rootfs first)
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T=120
[ "$1" = "-t" ] && { T=$2; shift 2; }
exec timeout -s KILL "$T" unshare --user --map-root-user --pid --fork \
    qemu-arm -L "$ROOT/firmware/rootfs" -E LD_LIBRARY_PATH=/system/lib:/system/vendor/lib "$@"
