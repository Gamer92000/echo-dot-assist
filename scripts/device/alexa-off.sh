#!/system/bin/sh
# Stop the Alexa client and OTA until next reboot.  Leaves mixer, shmd, ledcontroller, Wi-Fi untouched.
# ledcontroller sets PUFFIN_START=1 after the boot animation, which starts these four services.
setprop com.amazon.puffin.PUFFIN_START 0
for s in puffin puffinmrmd dacd smarthomed otad ace_otad update_engine; do stop $s; done
# Amazon's Bluetooth stack (speaker mode, pairing through the Alexa app).  hassmic drives the radio itself, for Home
# Assistant's Bluetooth proxy and as a Bluetooth speaker, and /dev/stpbt does not keep a second user out: it has to go.
stop btmanagerd
# Setup mode (unregistered device): oobed advertises for the Alexa app and keeps the orange setup animation on the ring.
for s in oobed_on_boot oobed_on_press oobed_on_no_nw; do stop $s; done
# uxeventd starts the orange `setup-mode` spinner; `ledctrl -c` does not clear it and `-g` does not list it. Unset by name.
ledctrl -c >/dev/null; ledctrl -u setup-mode >/dev/null
# oobed leaves a Wi-Fi Direct group up for the Alexa app (p2p-p2p0-0 + dnsmasq).  wpa_supplicant itself must stay: it also runs wlan0.
wpa_cli -p /data/misc/wifi/sockets -i p2p0 p2p_group_remove p2p-p2p0-0 >/dev/null 2>&1
sleep 1
getprop | grep -E "init.svc.(puffin|puffinmrmd|dacd|smarthomed|otad|ace_otad|mixer|shmd|ledcontroller)\]"
