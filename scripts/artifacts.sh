#!/usr/bin/env bash
# Amazon's artifacts (DAVS) for an installed Echo: other wake words ("Echo", "Computer", "Amazon", "Ziggy", or "Alexa" in another
# language), the whisper detection model (docs/re-whisper.md) and Amazon's newest sound detection model (Alexa Guard's,
# in place of the firmware's, docs/re-aed.md), all installed on the Echo.
# All are Amazon's (DAVS) and the same for every Echo, so ones fetched before (device-logs/models/, git-ignored) are only
# copied over.  A menu with a list of ticks per kind, everything new ticked; then one run does the rest.  Downloading
# needs the Echo registered to an Amazon account once: it runs stock Alexa with the updaters cut off
# (MODE=stock-online) until then, and everything is undone afterwards (registration, the Wi-Fi the Alexa app added,
# the mode).  Home Assistant then offers every installed wake word in the Echo's wake word select.  Stopped halfway
# (Ctrl-C), a new run finds the Echo in stock-online mode and goes on there.
# Not the way meant any more: the Echo's settings page downloads from Amazon itself ("Download from Amazon", every
# model, no Alexa app, no stock Echo online, nothing to undo).  Kept for models already on the PC and as a fallback.
#   scripts/artifacts.sh [echo-ip]      without an address: the Echo on adb (USB, or ANDROID_SERIAL)
cd "$(dirname "$0")/.."
. scripts/lib/device.sh
. scripts/lib/setup.sh
. scripts/lib/artifacts.sh

case $1 in -h|--help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0;; esac
[ -n "$1" ] && { export ANDROID_SERIAL=$1; [[ $1 == *:* ]] || ANDROID_SERIAL=$1:5555; }

trap '[ -n "$TTY" ] && printf "\e[?25h\e[?7h"' EXIT
trap '[ -n "$TASK_PID" ] && kill $TASK_PID 2>/dev/null; rm -rf "$TMP"; _clr; printf "\n  %sStopped. Run scripts/artifacts.sh again to go on; leave the Echo registered until then (deregistered as a stock Echo, it resets itself to factory settings).%s\n" "$DIM" "$N"; exit 130' INT TERM
LOG=build/artifacts.log; mkdir -p build; [ -s $LOG ] && mv $LOG $LOG.1; : > $LOG   # the run before kept: it may be the one that failed
MODEL_NAME="Artifacts"

# the old way, said before anything happens: the next screens clear this one, so it waits for Enter
w=78
printf '\n  %s%s%s\n' "$YEL$B" "$(_rep ━ $w)" "$N"
for l in \
    "THIS SCRIPT IS NOT THE WAY TO DOWNLOAD FROM AMAZON ANY MORE" \
    "" \
    "Use the Echo's settings page instead:  http://<echo-ip>:28931/  →  \"Download from Amazon\"" \
    "Every model signs in there itself: a code to enter on Amazon's site, no Alexa app," \
    "no stock Echo online, nothing to undo. The Echos section copies models to other Echos." \
    "" \
    "This script runs the Echo as a stock Alexa online and needs it registered in the" \
    "Alexa app. Deregistered too early (while it is a stock Echo), it resets itself to" \
    "factory settings. Use it only for models already on this PC or if the page fails."; do
    printf '  %s┃%s %s\n' "$YEL$B" "$N" "$l"
done
printf '  %s%s%s\n' "$YEL$B" "$(_rep ━ $w)" "$N"
printf '\n  %sEnter to go on anyway, Ctrl-C to stop.%s ' "$DIM" "$N"
read -r _ < /dev/tty || exit 1
artifacts_run
