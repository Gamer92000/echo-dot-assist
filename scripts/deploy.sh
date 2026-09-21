#!/bin/sh
# Build and push everything to /data/local/hassmic on the device (adb, root shell).
set -e
cd "$(dirname "$0")/.."
make -s all
D=/data/local/hassmic
adb shell "mkdir -p $D"
adb push build/mixcap build/mixplay build/pryon_test build/hassmic build/runas scripts/device/*.sh scripts/device/hassmic.service testdata/alexa_espeak.raw $D/ >/dev/null
adb shell "chmod 755 $D/*"
echo "pushed to $D"
