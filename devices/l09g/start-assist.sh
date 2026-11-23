#!/bin/sh
set -eu
base=/data/l09g-assist
export HASSMIC_STATE="$base/state"
export HASSMIC_MODELS="$base/models"
export HASSMIC_WIFI=/run/l09g-assist-wifi
export HASSMIC_MDNS_FILE=/etc/avahi/services/l09g-assist.service
export HASSMIC_ACTION_KEY=164
mkdir -p "$HASSMIC_STATE" "$HASSMIC_MODELS"
if ! pactl list short sinks | awk '{print $2}' | grep -qx l09g_direct; then
    pactl load-module module-alsa-sink device=playback mmap=no rate=48000 format=s16 channels=2 sink_name=l09g_direct >/dev/null
fi
if [ -x "$base/bt-service.sh" ] && ! pgrep -f '^/bin/sh /data/l09g-assist/bt-service.sh$' >/dev/null; then
    "$base/bt-service.sh" >> "$base/bt-service.log" 2>&1 &
fi
if [ -x "$base/wifi-service.sh" ] && ! pgrep -f '^/bin/sh /data/l09g-assist/wifi-service.sh run$' >/dev/null; then
    /bin/sh "$base/wifi-service.sh" run >> "$base/wifi-service.log" 2>&1 &
fi
if [ ! -e "$HASSMIC_MDNS_FILE" ] || grep -q 'mi-smart-speaker-l09g' "$HASSMIC_MDNS_FILE"; then
    "$base/hassmic" -P esphome -n Mi-Smart-Speaker-L09G -z 28928 -S > "$HASSMIC_MDNS_FILE"
else
    echo 'Refusing to overwrite an unrelated Avahi service' >&2
    exit 1
fi
if ! pgrep -f '^/data/l09g-assist/hassmic ' >/dev/null; then
    # A clock request from an earlier boot uses that boot's uptime and must not be replayed.
    rm -f "$HASSMIC_STATE/clock"
    "$base/hassmic" -P esphome -n Mi-Smart-Speaker-L09G -L -B -z 28928 -o 0 >> "$base/hassmic.log" 2>&1 &
    pid=$!
    "$base/clock-watch.sh" "$pid" >> "$base/startup.log" 2>&1 &
fi
