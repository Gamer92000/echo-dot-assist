#!/bin/sh
# Build and push everything to /data/local/hassmic on the device (adb, root shell).  The model is the one on adb.
# The binaries are built here or this commit's release build (PREBUILT, scripts/lib/build.sh).
set -e
cd "$(dirname "$0")/.."
. scripts/lib/device.sh; device_load adb; device_check_firmware warn
. scripts/lib/build.sh; build_binaries; notice_make
D=/data/local/hassmic
adb shell "mkdir -p $D"
adb push $(ls $OUT/plugrun $OUT/rcpbridge $OUT/otbr-agent $OUT/otbr-agent-csl $OUT/ot-ctl $OUT/zigbeed $OUT/gprcp $OUT/mixcap $OUT/mixplay $OUT/pryon_test 2>/dev/null) $OUT/hassmic $OUT/runas $OUT/NOTICE $DDIR/device.conf scripts/device/*.sh testdata/alexa_espeak.raw $D/ >/dev/null
# the wake word aligners (src/hassmic/wakealign.h)
adb push wakealign/*.wa wakealign/*.was $D/ >/dev/null
[ -n "$ANDROID" ] && adb push scripts/device/wifictl.dex ${SCREEN_BAND:+scripts/device/ringbar.dex} $D/ >/dev/null
[ -f $OUT/libhmui.so ] && adb push $OUT/libhmui.so scripts/device/screen.dex $D/ >/dev/null
adb shell "chmod 755 $D/*; chmod 644 $D/NOTICE"
echo "pushed $DEVICE build to $D"
# main.sh runs $D/hassmic in place of an installed copy until an update or install-system.sh replaces it (issue #251)
adb shell "[ -f /system/hassmic/boot.sh ]" 2>/dev/null &&
    echo "this Echo is installed: $D/hassmic now runs in place of the installed hassmic, until the next update or install (or: adb shell rm $D/hassmic)"
