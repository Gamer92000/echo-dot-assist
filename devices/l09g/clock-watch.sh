#!/bin/sh
# Apply fresh time offered by the paired, encrypted Home Assistant connection.
set -u
pid=$1
state=/data/l09g-assist/state
while kill -0 "$pid" 2>/dev/null; do
    if [ -f "$state/clock" ] && [ ! -L "$state/clock" ] && mv "$state/clock" "$state/.clock-processing" 2>/dev/null; then
        read -r epoch reference extra < "$state/.clock-processing" || true
        rm -f "$state/.clock-processing"
        case "${epoch:-}" in ''|*[!0-9]*) epoch=0;; esac
        case "${reference:-}" in ''|*[!0-9.]*) reference=0;; esac
        reference=${reference%%.*}
        case "$reference" in ''|*[!0-9]*) reference=0;; esac
        read -r uptime rest < /proc/uptime
        uptime=${uptime%%.*}
        elapsed=$((uptime - reference))
        if [ "${#epoch}" -eq 10 ] && [ "$epoch" -ge 1767225600 ] && [ "$elapsed" -ge 0 ] && [ "$elapsed" -le 600 ]; then
            date -u -s "@$((epoch + elapsed))" >/dev/null 2>&1 && echo 'Clock synchronized from the paired server.'
        fi
    fi
    sleep 5
done
