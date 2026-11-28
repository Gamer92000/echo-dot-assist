#!/bin/sh
# Build and push everything to /data/local/hassmic on the device (adb, root shell).  The model is the one on adb.
# The binaries are built here or this commit's release build (PREBUILT, scripts/lib/build.sh).
set -e
cd "$(dirname "$0")/.."
. scripts/lib/device.sh; device_load adb; device_check_firmware warn
. scripts/lib/build.sh; build_binaries; notice_make
D=/data/local/hassmic
adb shell "mkdir -p $D"
# main.sh runs $D/hassmic in place of an installed copy, until another update is installed than the one it was pushed
# over (its VERSION in hassmic.over, "-": none; main.sh "trial", issue #251).  Decided by what adb prints: older adbd
# (Fire OS 5) answers 0 whatever the command's exit status was.  The mark before the binary: a start in between
# must not find the new binary beside an old mark
inst=$(adb shell "v=\$(cat /data/local/hassmic/ota/current/VERSION 2>/dev/null); echo \"\${v:--}\" > $D/hassmic.over; [ -f /system/hassmic/boot.sh ] && echo installed" | tr -d '\r')
adb push $(ls $OUT/plugrun $OUT/rcpbridge $OUT/otbr-agent $OUT/otbr-agent-csl $OUT/ot-ctl $OUT/zigbeed $OUT/gprcp $OUT/mixcap $OUT/mixplay $OUT/pryon_test 2>/dev/null) $OUT/hassmic $OUT/runas $OUT/NOTICE $DDIR/device.conf scripts/device/*.sh testdata/alexa_espeak.raw $D/ >/dev/null
# the wake word aligners (src/hassmic/wakealign.h)
adb push wakealign/*.wa wakealign/*.was $D/ >/dev/null
[ -n "$ANDROID" ] && adb push scripts/device/wifictl.dex ${SCREEN_BAND:+scripts/device/ringbar.dex} $D/ >/dev/null
[ -f $OUT/libhmui.so ] && adb push $OUT/libhmui.so scripts/device/screen.dex $D/ >/dev/null
adb shell "chmod 755 $D/*; chmod 644 $D/NOTICE"
echo "pushed $DEVICE build to $D"
if [ "$inst" = installed ]; then
    echo "this Echo is installed: $D/hassmic runs in place of the installed hassmic until the next update (or: adb shell rm $D/hassmic)"
fi
