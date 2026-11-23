#!/bin/sh
# Runs alongside hassmic; the switch controls pairing, not adapter power.
set -u
base=/data/l09g-assist
state="$base/state/bluetooth.enabled"
bluez=/org/bluez/hci0
set_property() {
    dbus-send --system --print-reply --reply-timeout=3000 --dest=org.bluez "$bluez" \
        org.freedesktop.DBus.Properties.Set string:org.bluez.Adapter1 "string:$1" "$2" >/dev/null 2>&1
}
if [ ! -e "$state" ]; then printf '0\n' > "$state"; fi
if ! pgrep -f '^/data/l09g-assist/bt-agent$' >/dev/null; then
    "$base/bt-agent" >> "$base/bt-service.log" 2>&1 &
fi
last=unknown
while :; do
    if ! pgrep -f '^/data/l09g-assist/bt-agent$' >/dev/null; then
        "$base/bt-agent" >> "$base/bt-service.log" 2>&1 &
    fi
    requested=$(cat "$state" 2>/dev/null || echo 0)
    case "$requested" in 1|0) : ;; *) requested=0 ;; esac
    if [ "$requested" != "$last" ]; then
        pactl set-default-sink l09g_direct >/dev/null 2>&1 || true
        pactl list short modules | grep -q 'module-bluetooth-discover' || pactl load-module module-bluetooth-discover >/dev/null 2>&1 || true
        pactl list short modules | grep -q 'module-bluetooth-policy' || pactl load-module module-bluetooth-policy >/dev/null 2>&1 || true
        if [ "$requested" = 1 ]; then
            if set_property Powered variant:boolean:true &&
               set_property Pairable variant:boolean:true &&
               set_property PairableTimeout variant:uint32:0 &&
               set_property DiscoverableTimeout variant:uint32:0 &&
               set_property Discoverable variant:boolean:true; then
                echo 'Bluetooth pairing enabled.'
                last=1
            fi
        else
            if set_property Powered variant:boolean:true &&
               set_property Discoverable variant:boolean:false &&
               set_property Pairable variant:boolean:false; then
                echo 'Bluetooth pairing disabled; paired devices can reconnect.'
                last=0
            fi
        fi
    fi
    sleep 2
done
