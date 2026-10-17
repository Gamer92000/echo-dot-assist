#!/bin/sh
# Open adb over Wi-Fi on an installed Echo for 30 minutes, with the key that signs its updates (secrets/update.key)
# instead of the settings page (http://<echo-ip>:28931/, with a press of the action button): for an Echo that is not adopted (yet), has lost its key, runs Wyoming, or
# whose Home Assistant is away.  Needs hassmic running (its push port, TCP 28929), as for scripts/ota-push.sh.
#   scripts/adb-wifi.sh [host]      host defaults to the one ota-push.sh used last (secrets/ota.host)
set -e
cd "$(dirname "$0")/.."
HOST=${1:-$(cat secrets/ota.host 2>/dev/null || true)}
[ -n "$HOST" ] || { echo "usage: scripts/adb-wifi.sh <echo-ip>"; exit 1; }
[ -f secrets/update.key ] || { echo "secrets/update.key missing: without it only USB or Home Assistant's switch open adb"; exit 1; }
python3 scripts/otatool.py adb "$HOST" "${OTA_PORT:-28929}" secrets/update.key
adb connect "$HOST:5555"
