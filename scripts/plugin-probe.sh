#!/bin/sh
# The plugins' sandbox on the Echo on adb (docs/plugins.md 6): the launcher as it is installed (root) starts
# src/tools/plugprobe.c as it starts a plugin, and the probe makes every call the filter has a rule about, both ways.
# Prints what the launcher found (the kernel's filter, the SELinux domain, the CPU groups), one line per check that
# is wrong, and the count; the whole table goes to device-logs/.  Nothing of an installed hassmic is touched: two
# files in /data/local/tmp, removed afterwards.  The CPU groups the launcher makes under /dev/cpuctl stay.
#
# The probe is built as a plugin is: a static musl program (zig, or PLUGIN_CC; never the NDK: a static bionic
# program does not start in the sandbox, which is the first thing this would show).
set -e
cd "$(dirname "$0")/.."
. scripts/lib/device.sh; device_load adb
[ "$(adb get-state 2>/dev/null)" = device ] || die "no Echo running Fire OS on adb"
make -s DEVICE=$DEVICE $OUT/plugrun build/plugins/plugprobe
T=/data/local/tmp/hassmic-probe
LOG=device-logs/plugin-probe-$DEVICE-$(date +%Y%m%d-%H%M%S).txt
mkdir -p device-logs
adb shell "mkdir -p $T"
adb push $OUT/plugrun build/plugins/plugprobe $T/ >/dev/null
# -T: the launcher by itself, one program, its stderr and exit status passed on
adb shell "chmod 755 $T/plugrun $T/plugprobe; $T/plugrun -T $T/plugprobe 2>&1; echo \"exit \$?\"" | tr -d '\r' > "$LOG" || true
adb shell "rm -f $T/plugrun $T/plugprobe; rmdir $T" || true
grep -v '^ok ' "$LOG"
echo "the whole table: $LOG"
grep -q '^exit 0$' "$LOG"
