#!/system/bin/sh
# Phase 3: record stock state before changing anything.  Output goes to stdout; run via scripts/probe.sh on the PC.
sec() { echo; echo "######## $*"; }
sec build;      getprop ro.build.fingerprint; getprop ro.build.version.number; uname -a
sec selinux;    getenforce 2>&1; id
sec services;   getprop | grep init.svc
sec props;      getprop | grep -iE "puffin|audio|mixer|oobe|wha|ota"
sec processes;  ps | grep -iE "mixer|puffin|shmd|ledcontroller|uxeventd|dacd|otad|BTSink|quantum"
sec mixer dirs; ls -la /data/mixer_streams /data/mixer_meta 2>&1
sec streams;    for f in /data/mixer_streams/*; do echo "--- $f"; cat "$f" 2>/dev/null | head -c 600; echo; done
sec alsa;       cat /proc/asound/cards /proc/asound/pcm 2>&1
sec volume;     for p in MainVolume TTSVolume Mute AllowMic SilenceMic LPMState; do echo "$p = $(audio_manager_get_prop $p 2>&1 | tail -1)"; done
sec privacy;    cat /sys/devices/platform/gpio-privacy/state 2>&1
sec leds;       ledctrl -g 2>&1
sec firewall;   iptables -S INPUT 2>&1
sec wifi;       wpa_cli -i wlan0 -p /data/misc/wifi/sockets status 2>&1 | grep -vE "^(bssid|address|uuid)"
sec libs;       md5sum /system/lib/libmixerAPI.so /system/lib/libpryon.so /system/lib/libasp.so /system/bin/mixer /system/bin/PuffinApp
sec model;      ls -la /system/local/models/keyword/*/*/ /data/avs/ 2>&1 | head -40
