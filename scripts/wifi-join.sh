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
    hex() { printf %s "$1" | od -An -tx1 | tr -d " \n"; }       # wifictl takes the SSID as hex and the PSK itself
    # (64 hex digits, PBKDF2 of passphrase and SSID, as wifi.c makes it): nothing of either passes a shell
    if [ -z "$pass" ]; then psk=-
    else
        [ ${#pass} -ge 8 ] && [ ${#pass} -le 63 ] || { echo "the passphrase in $F has ${#pass} characters; WPA2 wants 8 to 63"; exit 1; }
        psk=$(printf '%s\n%s\n' "$ssid" "$pass" | python3 -c 'import sys, hashlib
s, p = sys.stdin.read().split("\n")[:2]
print(hashlib.pbkdf2_hmac("sha1", p.encode(), s.encode(), 4096, 32).hex())') || exit 1
    fi
    adb push scripts/device/wifictl.dex $D/ >/dev/null
    W() { adb shell "CLASSPATH=$D/wifictl.dex app_process / Wifictl $*" | tr -d '\r'; }
    W wifi-on > /dev/null
    # the radio takes a moment to come up, and WifiService adds no network before it has
    t=0
    while :; do
        out=$(W add "$(hex "$ssid")" $psk); n=$(printf '%s\n' "$out" | tail -1)
        case $n in ''|*[!0-9]*) ;; *) break;; esac
        [ $t -ge 30 ] && { echo "WifiService did not add the network:"; printf '%s\n' "$out"; exit 1; }
        sleep 2; t=$((t + 2))
    done
    [ "$(W select "$n" | tail -1)" = OK ] || { echo "WifiService did not switch to network $n"; W status; exit 1; }
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
