#!/bin/sh
# Kiosk Satellite on an installed Echo Show, for one that went through the guided setup without it: the newest release and
# Simple Keyboard (its setup has text fields), both checked (scripts/lib/kiosk.sh).  The Echo opens the app by itself.
#   scripts/kiosk-install.sh             over the adb that is connected (USB)
#   scripts/kiosk-install.sh <echo-ip>   over Wi-Fi: opens adb for 30 minutes with secrets/update.key (scripts/adb-wifi.sh)
#   scripts/kiosk-install.sh --uninstall [<echo-ip>]   takes the app off again (kiosk_remove); the keyboard stays
set -e
cd "$(dirname "$0")/.."
. scripts/lib/device.sh
. scripts/lib/kiosk.sh
UNINSTALL=
[ "$1" != --uninstall ] || { UNINSTALL=1; shift; }
HOST=$1
if [ -n "$HOST" ]; then scripts/adb-wifi.sh "$HOST"; export ANDROID_SERIAL=$HOST:5555; fi
[ "$(adb get-state 2>/dev/null)" = device ] ||
    die "no Echo on adb: connect USB, or give its address (scripts/kiosk-install.sh <echo-ip>)"
device_load adb
[ -n "$KIOSK_PACKAGE" ] || die "$DEVICE has no screen for Kiosk Satellite"
if [ -n "$UNINSTALL" ]; then
    echo "Kiosk Satellite, uninstalling ..."
    kiosk_remove || die "Kiosk Satellite was not uninstalled"
    echo "Done. Its ports (2324, 6053) close within seconds; hassmic's home screen shows again."
    exit 0
fi
mkdir -p "$OUT"
echo "Simple Keyboard ..."
keyboard_get || echo "the keyboard was not installed; the app's setup can still be done from a PC" >&2
echo "Kiosk Satellite, newest release ..."
kiosk_get || die "Kiosk Satellite was not installed"
ip=${HOST:-$(adb shell ifconfig ${WLAN:-wlan0} | sed -n 's/.*inet addr:\([0-9.]*\).*/\1/p')}
echo
echo "Installed. The Echo opens it by itself within a minute."
echo "Do its setup from this PC, where you can paste the Home Assistant token: http://${ip:-<echo-ip>}:2324"
echo "Leave its voice assistant off: hassmic is the satellite."
