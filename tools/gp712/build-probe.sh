#!/bin/bash
# tools/gp712/gpprobe: Qorvo's public GP712 OpenThread library (Qorvo/qpg-openthread, its EULA: use with Qorvo silicon,
# which the Echo 4's GP712 is; fetched here by checksum, never in git) with the one byte Amazon's older driver needs
# (gpMacDispatcher API 2.8.0.0 offered as 2.7.0.0: docs/re-gp712.md §0), linked into gpprobe.c.  The library is the
# Raspberry Pi's (hard float, glibc names): a static musl binary by zig, as the plugins are.
#   tools/gp712/build-probe.sh [out dir]      (default build/gp712)
set -euo pipefail
top=$(cd "$(dirname "$0")/../.." && pwd)
out=${1:-$top/build/gp712}; src=$top/toolchain/src/gp712
mkdir -p "$out" "$src"
QPG=Qorvo/qpg-openthread/2762d55db1d218be57f251a8c8eafbb1938c36a7
OTQ=openthread/ot-qorvo/ce0fd368ef6b0a8122433588a17ec8f5258d647a
# file  from  sha256
FILES="
libQorvoGP712_ftd.a $QPG/gp712/lib/libQorvoGP712_ftd.a 43b4e631f1ffb8868e930a5ca4b072c05f7968eb610240113c749b9e5362e8f8
radio_qorvo.h       $OTQ/src/gp712/radio_qorvo.h       -
platform_qorvo.h    $OTQ/src/gp712/platform_qorvo.h    -
alarm_qorvo.h       $OTQ/src/gp712/alarm_qorvo.h       -
random_qorvo.h      $OTQ/src/gp712/random_qorvo.h      -
"
echo "$FILES" | while read -r f from sum; do
    [ -n "$f" ] || continue
    [ -f "$src/$f" ] || curl -fsSL --retry 3 -o "$src/$f" "https://raw.githubusercontent.com/$from"
    [ "$sum" = - ] || [ "$(sha256sum "$src/$f" | cut -c1-64)" = "$sum" ] || { echo "build-probe: $f: checksum differs" >&2; exit 1; }
done
touch "$src/openthread-core-config.h"           # the platform headers name it; nothing of it is needed
w=$out/lib; rm -rf "$w"; mkdir -p "$w"
( cd "$w" && ar x "$src/libQorvoGP712_ftd.a"
  o=gpMacDispatcher_wrapper_client.o
  [ "$(xxd -s 0x8318 -l 4 -p $o)" = 02080000 ] || { echo "build-probe: the version bytes are not where docs/re-gp712.md says" >&2; exit 1; }
  printf '\x07' | dd of=$o bs=1 seek=$((0x8319)) conv=notrunc 2>/dev/null
  llvm-ar rcs ../libQorvoGP712_27.a $(ar t "$src/libQorvoGP712_ftd.a") )
zig cc -target arm-linux-musleabihf -mcpu=cortex_a7 -O1 -static -I"$src" -I"$top/toolchain/src/openthread/include" \
    "$top/tools/gp712/gpprobe.c" "$out/libQorvoGP712_27.a" -o "$out/gpprobe"
# gptimed: timed transmit on the chip's clock (needs hassmic_gptime loaded); the library's DataRequest wrapped
zig cc -target arm-linux-musleabihf -mcpu=cortex_a7 -O1 -static -I"$src" -I"$top/toolchain/src/openthread/include" \
    -Wl,--wrap=gpMacDispatcher_DataRequest "$top/tools/gp712/gptimed.c" "$out/libQorvoGP712_27.a" -o "$out/gptimed"
echo "build-probe: $out/gpprobe, $out/gptimed (adb push to /data/local/tmp; radio off; run as root)"
