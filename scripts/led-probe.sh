#!/bin/sh
# Measures the light ring's driver on an Echo over adb, for a live ring (scripts/device/led-probe.sh says what each stage
# does and what it may touch).  Run them in order and look at each output before the next:
#   scripts/led-probe.sh read     only reads (the default)
#   scripts/led-probe.sh stock    stock's patterns and brightness through ledctrl, put back afterwards
#   scripts/led-probe.sh write    the frame file written with stock's own values, put back afterwards
# Output in device-logs/led-probe-<model>-<stage>-<time>.txt; read also copies ledcontroller, its LED library and
# configs to device-logs/led-probe-<model>/ (Amazon's: git-ignored there) to be read on the PC.
cd "$(dirname "$0")/.."
. scripts/lib/device.sh; device_load adb
STAGE=${1:-read}
case $STAGE in read|stock|write) ;; *) die "stage: read, stock or write";; esac
[ "$(adb shell id -u | tr -d '\r')" = 0 ] || die "adb shell is not root"
mkdir -p device-logs
LOG=device-logs/led-probe-$DEVICE-$STAGE-$(date +%Y%m%d-%H%M%S).txt
adb push scripts/device/led-probe.sh /data/local/tmp/led-probe.sh > /dev/null || die "push failed"
adb shell sh /data/local/tmp/led-probe.sh $STAGE 2>&1 | tr -d '\r' | tee "$LOG"
if [ $STAGE = read ]; then
    P=device-logs/led-probe-$DEVICE; mkdir -p $P
    for f in /system/bin/ledcontroller /system/bin/ledctrl $(adb shell 'ls /system/lib/*led* /system/lib/hw/*led* 2>/dev/null' | tr -d '\r') \
             /system/etc/led_mute_ring_config.json /system/etc/ledlipcconfig.json $(adb shell 'ls /data/ledcontroller/* 2>/dev/null' | tr -d '\r'); do
        adb pull "$f" $P/ > /dev/null 2>&1 && echo "copied $f"
    done
fi
echo "saved: $LOG"
