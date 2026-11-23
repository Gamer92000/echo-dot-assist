#!/bin/sh
base=/data/lx05-assist
export HASSMIC_STATE="$base/state"
export HASSMIC_MODELS="$base/models"
export HASSMIC_WIFI="$base/wifi"
export HASSMIC_ACTION_KEY=28
# This port's tested network guard and Wi-Fi helper support a single /24 LAN.
# Set LX05_LAN_PREFIX (for example 10.42.0.) in this private local file.
[ -r "$base/network.conf" ] || { echo 'Missing network.conf' >&2; exit 1; }
. "$base/network.conf"
printf '%s\n' "${LX05_LAN_PREFIX:-}" | grep -Eq '^([0-9]{1,3}\.){3}$' || exit 1
export LX05_LAN_PREFIX
export HASSMIC_ARB_ADDR="${LX05_LAN_PREFIX}255"

# Wait for the guarded Wi-Fi connection. Do not launch network services if the
# policy rule disappeared; the unreachable default must still be present.
tries=0
while [ "$tries" -lt 60 ]; do
    if ip -4 addr show wlan0 2>/dev/null | awk -v p="$LX05_LAN_PREFIX" '$1 == "inet" && index($2, p) == 1 {found=1} END {exit !found}' &&
       ip rule show | grep -q '100:.*lookup 123' &&
       ip route show table 123 | grep -q '^unreachable default'; then
        break
    fi
    tries=$((tries + 1))
    sleep 1
done
[ "$tries" -lt 60 ] || { echo 'LX05 Assist: guarded network unavailable' >> /tmp/lx05-start.log; exit 1; }
ip route replace 224.0.0.251/32 dev wlan0 table 123 || exit 1
/etc/init.d/led stop >/dev/null 2>&1
"$base/mdns" >> /tmp/lx05-mdns.log 2>&1 &
"$base/wifi-service.sh" >> /tmp/lx05-wifi.log 2>&1 &
rm -f "$HASSMIC_STATE/clock"
"$base/clock-watch.sh" "$$" >> /tmp/lx05-start.log 2>&1 &
exec "$base/lib/ld-musl-armhf.so.1" --library-path "$base/lib:/lib:/usr/lib" \
    "$base/hassmic" -P esphome -n Xiaomi-LX05 -b /dev/input/event0 -L -B -o 0 \
    >> /tmp/lx05-assist.log 2>&1
