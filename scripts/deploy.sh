#!/bin/sh
# Build and push everything to /data/local/hassmic on the device (adb, root shell).  The model is the one on adb.
# The binaries are built here or this commit's release build (PREBUILT, scripts/lib/build.sh).
set -e
cd "$(dirname "$0")/.."
. scripts/lib/device.sh; device_load adb; device_check_firmware warn
. scripts/lib/build.sh; build_binaries
D=/data/local/hassmic
adb shell "mkdir -p $D"
adb push $(ls $OUT/mixcap $OUT/mixplay $OUT/pryon_test 2>/dev/null) $OUT/hassmic $OUT/runas $DDIR/device.conf scripts/device/*.sh testdata/alexa_espeak.raw $D/ >/dev/null
[ "$INSTALL" = boot ] && adb push scripts/device/wifictl.dex $D/ >/dev/null
adb shell "chmod 755 $D/*"
echo "pushed $DEVICE build to $D"
