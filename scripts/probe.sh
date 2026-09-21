#!/bin/sh
# Run the stock-state probe and compare the audio libraries with the analysed firmware.
cd "$(dirname "$0")/.."
mkdir -p device-logs
OUT=device-logs/probe-$(date +%Y%m%d-%H%M%S).txt
adb push scripts/device/probe.sh /data/local/tmp/ >/dev/null && adb shell sh /data/local/tmp/probe.sh > "$OUT"
echo "saved $OUT"
echo "library check against firmware/rootfs (no output below = identical):"
for f in lib/libmixerAPI.so lib/libpryon.so lib/libasp.so bin/mixer bin/PuffinApp; do
    want=$(md5sum firmware/rootfs/system/$f | cut -d' ' -f1)
    grep -q "$want" "$OUT" || echo "  DIFFERENT: /system/$f"
done
