#!/bin/sh
# Restore the stock startup on next reboot, without changing the running watchdog.
set -eu
names='S51harman-audio-server S51stereo-harman-audio-server S96stereoctl.sh S97okcall.sh S97voice-agent.sh S99btctl.sh'
marker='# l09g-assist: disabled stock service'
for name in $names; do
    upper=/data/overlay/upper/etc/init.d/$name
    if [ -e "$upper" ] && ! grep -qF "$marker" "$upper"; then
        echo "Refusing to remove an unrelated customization: $upper" >&2
        exit 1
    fi
done
for name in $names; do
    upper=/data/overlay/upper/etc/init.d/$name
    # Remove only our upper-layer file: unlinking the merged path makes an
    # overlay whiteout, which would keep the original hidden after reboot.
    if [ -e "$upper" ]; then rm "$upper"; fi
done
service=/etc/avahi/services/l09g-assist.service
if [ -f "$service" ] && grep -q 'mi-smart-speaker-l09g' "$service"; then
    rm "$service"
fi
echo 'Stock startup restored. Reboot to start the original services.'
