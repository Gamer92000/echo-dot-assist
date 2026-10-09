#!/bin/sh
# Push secrets/wifi.conf (line 1 SSID, line 2 passphrase; git-ignored) and join.  Needs lockdown.sh applied on the device.
# The Dots: through their stock wpa_supplicant (scripts/device/wifi-join.sh).  checkers: through WifiService, with
# scripts/device/wifictl.dex — the framework owns the networks there, and it runs DHCP itself once its connect lands.
cd "$(dirname "$0")/.."
. scripts/lib/device.sh; device_load adb
F=secrets/wifi.conf; [ -f $F ] || { echo "create $F: line 1 SSID, line 2 passphrase"; exit 1; }
D=/data/local/hassmic

if [ "$INSTALL" = boot ]; then
    ssid=$(sed -n 1p $F); pass=$(sed -n 2p $F)
    hex() { printf %s "$1" | od -An -tx1 | tr -d " \n"; }       # wifictl takes the SSID and passphrase as hex:
    adb push scripts/device/wifictl.dex $D/ >/dev/null          # nothing of either passes a shell
    W() { adb shell "CLASSPATH=$D/wifictl.dex app_process / Wifictl $*" | tr -d '\r'; }
    W wifi-on > /dev/null
    n=$(W add "$(hex "$ssid")" "$(hex "$pass")" | tail -1)
    case $n in ''|*[!0-9]*) echo "WifiService did not add the network:"; W add "$(hex "$ssid")" "$(hex "$pass")"; exit 1;; esac
    W select "$n" > /dev/null
    echo "joining $ssid ..."
    t=0 ip=
    while [ $t -lt 60 ]; do
        ip=$(adb shell ifconfig $WLAN | tr -d '\r' | sed -n 's/.*inet addr:\([0-9.]*\).*/\1/p')
        [ -n "$ip" ] && break
        sleep 2; t=$((t + 2))
    done
    [ -n "$ip" ] || { echo "no address after 60 s"; W status; exit 1; }
    W status | grep -E '^(wpa_state|ssid|ip_address)='
    exit 0
fi

adb push scripts/device/wifi-join.sh $D/ >/dev/null && adb push $F $D/wifi.conf >/dev/null
adb shell "chmod 600 $D/wifi.conf; sh $D/wifi-join.sh $D/wifi.conf; rm -f $D/wifi.conf"
