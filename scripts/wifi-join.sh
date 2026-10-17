#!/bin/sh
# Push secrets/wifi.conf (line 1 SSID, line 2 passphrase; git-ignored) and join.  Needs lockdown.sh applied on the device.
cd "$(dirname "$0")/.."
F=secrets/wifi.conf; [ -f $F ] || { echo "create $F: line 1 SSID, line 2 passphrase"; exit 1; }
D=/data/local/hassmic
adb push scripts/device/wifi-join.sh $D/ >/dev/null && adb push $F $D/wifi.conf >/dev/null || exit 1
# the join's own exit status, not rm's: a failed join has to fail setup's "Joining Wi-Fi" step
adb shell "chmod 600 $D/wifi.conf; sh $D/wifi-join.sh $D/wifi.conf; r=\$?; rm -f $D/wifi.conf; exit \$r"
