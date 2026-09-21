#!/bin/sh
# Record N seconds of processed mic audio (default 10) into device-logs/*.wav.  Run alexa-off.sh first.
# With -p FILE.wav, plays that file through the mixer at the same time: the AEC test.
cd "$(dirname "$0")/.."
PLAY=""; [ "$1" = "-p" ] && { PLAY=$2; shift 2; }
SECS=${1:-10}; D=/data/local/hassmic
AS="$D/runas puffin aipc,audio,system,inet,shell"   # AIPC refuses uid 0
mkdir -p device-logs; OUT=device-logs/micAsr-$(date +%H%M%S).wav
if [ -n "$PLAY" ]; then
    ffmpeg -loglevel error -y -i "$PLAY" -ac 1 -ar 22050 -f s16le /tmp/hassmic_play.raw
    adb push /tmp/hassmic_play.raw $D/play.raw >/dev/null
    adb shell "$AS $D/mixplay -r 22050 < $D/play.raw" &
fi
adb exec-out "$AS $D/mixcap -s $SECS 2>/dev/null" | ffmpeg -loglevel error -y -f s16le -ar 16000 -ac 1 -i - "$OUT"
wait
echo "saved $OUT"
ffmpeg -hide_banner -i "$OUT" -af volumedetect -f null - 2>&1 | grep -E "mean_volume|max_volume"
