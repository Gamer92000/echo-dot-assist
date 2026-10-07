#!/system/bin/sh
# Stop the Alexa client and OTA until next reboot.  Leaves mixer, shmd, ledcontroller, Wi-Fi untouched.
# Which services that are: device.conf of this model, next to this script.
. "${0%/*}/device.conf" || exit 1
# ledcontroller sets ALEXA_PROP=1 after the boot animation, which starts the Alexa services.
[ -n "$ALEXA_PROP" ] && setprop $ALEXA_PROP 0
for s in $ALEXA_SERVICES $UPDATE_SERVICES $UPDATE_ONDEMAND; do stop $s; done
# Where Alexa is Android apps (checkers): the framework starts them, and restarts a persistent one that is only stopped,
# so they are disabled.  That outlives a reboot, so what was disabled here is written down (DISABLED): alexa-on.sh, and
# boot.sh when hassmic.conf is gone, turn exactly those back on.  pm is slow (the Java runtime, ~1 s a call), and this
# runs after every restart of hassmic: only what is not disabled yet.
DISABLED=/data/local/hassmic/alexa-disabled
PKGS="$ALEXA_PACKAGES $SETUP_PACKAGES $UPDATE_PACKAGES"
if [ -n "$(echo $PKGS)" ] && command -v pm > /dev/null; then
    NL='
'
    off=$(pm list packages -d 2>/dev/null | tr -d '\r')
    for p in $PKGS; do
        case "$NL$off$NL" in *"${NL}package:$p$NL"*) continue;; esac
        pm list packages "$p" 2>/dev/null | grep -qx "package:$p" || continue        # not on this firmware
        if pm disable "$p" > /dev/null 2>&1; then
            grep -qx "$p" $DISABLED 2>/dev/null || echo "$p" >> $DISABLED
        else echo "alexa-off: $p could not be disabled"; fi
    done
fi
if [ -n "$BT_SERVICE" ]; then
    # Amazon's Bluetooth stack (speaker mode, pairing through the Alexa app).  hassmic drives the radio itself, for Home
    # Assistant's Bluetooth proxy and as a Bluetooth speaker, and /dev/stpbt does not keep a second user out: it has to go.
    stop $BT_SERVICE
    # Its AIPC service directory outlives it and stays its own (0710): hassmic answers the mixer on that service in its
    # place when it plays to a Bluetooth speaker (btout.c) and could neither remove nor reuse it.
    i=0; while [ "$(getprop init.svc.$BT_SERVICE)" = running ] && [ $i -lt 20 ]; do sleep 0.25; i=$((i + 1)); done
    rm -rf /dev/aipc/0
fi
# Setup mode (unregistered device): oobed advertises for the Alexa app and keeps the orange setup animation on the ring.
for s in $SETUP_SERVICES; do stop $s; done
# uxeventd starts the orange `setup-mode` spinner; `ledctrl -c` does not clear it and `-g` does not list it. Unset by name.
command -v ledctrl > /dev/null && { ledctrl -c >/dev/null; ledctrl -u setup-mode >/dev/null; }
# oobed leaves a Wi-Fi Direct group up for the Alexa app (p2p-p2p0-0 + dnsmasq).  wpa_supplicant itself must stay: it also runs wlan0.
wpa_cli -p $WPA_SOCKETS -i p2p0 p2p_group_remove p2p-p2p0-0 >/dev/null 2>&1
sleep 1
getprop | grep -E "init.svc.($(echo $ALEXA_SERVICES $UPDATE_SERVICES $UPDATE_ONDEMAND mixer shmd ledcontroller | tr ' ' '|'))\]"
