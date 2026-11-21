#!/system/bin/sh
# Phase 3: record stock state before changing anything.  Output goes to stdout; run via scripts/probe.sh on the PC.
# Arguments: stock files (relative to /system) to checksum.
D=/data/local/hassmic
# A model whose Android has no toybox (rook, Android 5.1) ships ours (tools/toybox.sh): the applets the system lacks go on
# PATH, after the system's own, so nothing that exists changes
if [ -x $D/toybox ]; then
    TB=/data/local/hassmic/tb; mkdir -p $TB
    for a in $($D/toybox); do [ -e /system/bin/$a ] || [ -e /system/xbin/$a ] || ln -sf $D/toybox $TB/$a; done
    PATH=$PATH:$TB
fi
md5sum() { if [ -x /system/bin/md5 ] && ! type md5sum >/dev/null 2>&1; then md5 "$@"; else command md5sum "$@"; fi; }
sec() { echo; echo "######## $*"; }
sec build;      getprop ro.product.device; getprop ro.build.display.id; getprop ro.build.fingerprint; getprop ro.build.version.number; uname -a
sec selinux;    getenforce 2>&1; id
sec services;   getprop | grep init.svc
sec props;      getprop | grep -iE "puffin|audio|mixer|oobe|wha|ota"
sec processes;  ps | grep -iE "mixer|puffin|shmd|ledcontroller|uxeventd|dacd|otad|BTSink|quantum"
sec mixer dirs; ls -la /data/mixer_streams /data/mixer_meta 2>&1
sec streams;    for f in /data/mixer_streams/*; do echo "--- $f"; dd if="$f" bs=600 count=1 2>/dev/null; echo; done    # the image's head has no -c
sec alsa;       cat /proc/asound/cards /proc/asound/pcm 2>&1
sec volume;     for p in MainVolume TTSVolume Mute AllowMic SilenceMic LPMState; do echo "$p = $(audio_manager_get_prop $p 2>&1 | tail -1)"; done
sec privacy;    cat /sys/devices/platform/gpio-privacy/state 2>&1
sec leds;       ledctrl -g 2>&1
sec firewall;   iptables -S INPUT 2>&1
sec wifi;       wpa_cli -i wlan0 -p /data/misc/wifi/sockets status 2>&1 | grep -vE "^(bssid|address|uuid)"
sec libs;       for f in "$@"; do md5sum /system/$f; done
sec model;      ls -la /system/local/models/keyword/*/*/ /data/avs/ 2>&1 | head -40
