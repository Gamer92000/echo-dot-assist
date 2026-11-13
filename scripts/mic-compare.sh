#!/bin/sh
# Records what the microphones deliver (micRaw) and what Amazon's front end makes of it (micAsr, the stream the wake
# word and the voice pipeline get) at the same time on an installed Echo, without stopping hassmic, and compares speech
# against noise in both (tools/mic-compare.py).  This is how the missing listening mode was found (PLAN.md, 2026-09-30):
# the front end took a quiet sentence away after 1.5 s that the microphones still had.
#   scripts/mic-compare.sh [-l] [seconds]      default 15 s; say one sentence when it prints GO, then stay silent
#   -l   put the front end into listening mode for the recording, as hassmic does while a command is spoken
# micAsr comes from hassmic's own dump (SIGTTIN -> state/capture.raw): the mixer gives that stream to one client only.
# micRaw can be read beside it (one channel, whatever is asked for).  Results in device-logs/mic-compare-<time>/.
cd "$(dirname "$0")/.."
. scripts/lib/device.sh; device_load adb
LISTEN=0; [ "$1" = "-l" ] && { LISTEN=1; shift; }
SECS=${1:-15}; B=/data/local/hassmic
[ "$SECS" -ge 6 ] || die "at least 6 seconds"
adb shell pidof hassmic > /dev/null || die "hassmic is not running on the Echo"
# mixcap and runas: next to the running build (push update), else the factory copy, else a trial deploy
T=$(adb shell "for d in $B/ota/current /system/hassmic $B; do [ -x \$d/mixcap ] && [ -x \$d/runas ] && { echo \$d; break; }; done" | tr -d '\r')
[ -n "$T" ] || die "no mixcap on the Echo"
AS="$T/runas $DAEMON_USER $DAEMON_GROUPS"   # AIPC refuses uid 0
lasp() { adb shell "lipc-set-prop -i com.doppler.lasp LASP_CMD_SET_LISTENING_MODE $1"; }
OUT=device-logs/mic-compare-$(date +%H%M%S); mkdir -p "$OUT"
lasp 0
# A freshly opened stream starts with a backlog block: micRaw starts first and runs 3 s longer, the analysis lines them up
adb exec-out "$AS $T/mixcap -t micRaw -s $((SECS + 3)) 2>/dev/null" > "$OUT/micRaw.raw" &
sleep 1
adb shell 'kill -TTIN $(pidof hassmic)'
echo ">>> recording $SECS s: stay silent until GO"
sleep 2
[ $LISTEN = 1 ] && lasp 1
sleep 1
echo ">>> GO: one sentence, then silence"
sleep $((SECS - 3))
adb shell 'kill -TTIN $(pidof hassmic)'; lasp 0
wait
adb pull $B/state/capture.raw "$OUT/micAsr.raw" > /dev/null 2>&1
echo "listening mode: $LISTEN" > "$OUT/info.txt"
${PYTHON:-python3} tools/mic-compare.py "$OUT"
