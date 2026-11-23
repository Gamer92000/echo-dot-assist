#!/bin/sh
# Run as root on an L09G with SpeakerLab's /data overlay active.
# The original scripts stay in /rom; only our marked overlay files are changed.
set -eu
names='S51harman-audio-server S51stereo-harman-audio-server S96stereoctl.sh S97okcall.sh S97voice-agent.sh S99btctl.sh'
marker='# l09g-assist: disabled stock service'
for name in $names; do
    upper=/data/overlay/upper/etc/init.d/$name
    if [ -e "$upper" ] && ! grep -qF "$marker" "$upper"; then
        echo "Refusing to overwrite existing customization: $upper" >&2
        exit 1
    fi
    [ -r "/rom/etc/init.d/$name" ] || { echo "Missing stock script: $name" >&2; exit 1; }
done
pgrep -x voice-agent >/dev/null && { echo 'Stop voice-agent and start watchdog first' >&2; exit 1; }
pgrep -f '^/sbin/watchdog -T 60 -t 5 /dev/watchdog$' >/dev/null || { echo 'Watchdog keeper not running' >&2; exit 1; }
for name in $names; do
    dest=/etc/init.d/$name
    tmp=/etc/init.d/.l09g-assist-$name.tmp
    if [ "$name" = S97voice-agent.sh ]; then
        printf '%s\n' '#!/bin/sh' "$marker" 'case "$1" in' '  start)' "    pgrep -f '^/sbin/watchdog -T 60 -t 5 /dev/watchdog$' >/dev/null || /sbin/watchdog -T 60 -t 5 /dev/watchdog" '    if [ -x /data/l09g-assist/start-assist.sh ]; then' '      /data/l09g-assist/start-assist.sh >/data/l09g-assist/startup.log 2>&1 &' '    fi' '    ;;' '  stop) : ;;' 'esac' > "$tmp"
    else
        printf '%s\n' '#!/bin/sh' "$marker" 'exit 0' > "$tmp"
    fi
    chmod 755 "$tmp"
    mv "$tmp" "$dest"
done
echo 'Stock voice, call, stereo and Harman services disabled on boot.'
