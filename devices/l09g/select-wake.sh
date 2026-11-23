#!/bin/sh
# Select one installed local model; the change takes effect after restart.
set -eu
base=/data/l09g-assist
case "${1:-}" in
    alexa|okay_nabu|hey_jarvis) id=$1 ;;
    *) echo 'Usage: sh /data/l09g-assist/select-wake.sh alexa|okay_nabu|hey_jarvis' >&2; exit 2 ;;
esac
[ -r "$base/state/mww/$id/manifest.json" ] && [ -r "$base/state/mww/$id/model.tflite" ] || exit 1
printf '%s\n' "$id" > "$base/state/mww_word.new"
mv "$base/state/mww_word.new" "$base/state/mww_word"
for pid in $(pgrep -f '^/data/l09g-assist/hassmic -P ' || true); do
    kill "$pid" 2>/dev/null || true
done
sleep 2
"$base/start-assist.sh"
echo "Selected local wake word: $id"
