#!/bin/sh
# Pairable and Discoverable control only new pairings; the adapter remains on.
state=/data/lx05-assist/state/bluetooth.pairing
set_property() {
    dbus-send --system --print-reply --reply-timeout=2000 --dest=org.bluez /org/bluez/hci0 \
        org.freedesktop.DBus.Properties.Set string:org.bluez.Adapter1 "string:$1" "$2" >/dev/null 2>&1
}
last=unknown
refresh=0
while :; do
    requested=$(cat "$state" 2>/dev/null || echo 0)
    case "$requested" in 0|1) :;; *) requested=0;; esac
    if [ "$requested" != "$last" ]; then
        if [ "$requested" = 1 ]; then
            if set_property Pairable variant:boolean:true && set_property Discoverable variant:boolean:true; then last=1; fi
        else
            if set_property Discoverable variant:boolean:false && set_property Pairable variant:boolean:false; then last=0; fi
        fi
    fi
    refresh=$((refresh + 1))
    if [ "$refresh" -ge 10 ]; then last=unknown; refresh=0; fi
    sleep 1
done
