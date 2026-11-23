#!/bin/sh
# Update an installed Echo over Wi-Fi: build, pack, sign with secrets/update.key, push.  No adb, no TWRP.
#   [DEVICE=<codename>] scripts/ota-push.sh [host]              host defaults to the one used last time (secrets/ota.host)
# The push installs the update beside the factory copy on the system partition and restarts hassmic with it.  If it
# does not come up, the Echo goes back to the factory copy by itself; once it has passed its self test (started, wake
# word engine loaded, a second of microphone audio), the Echo writes it over the factory copy, bootstrap included
# (boot.sh, otatool, hassmic.rc; scripts/system/sysinstall.sh): the new fallback.  Echos installed before this existed
# get there the same way: the pushed update brings the code that does the writing.
# No adb here, so the model comes from DEVICE (default donut).  The Echo refuses a bundle built for another model once it
# runs a main.sh that checks (devices/README.md).
# The Echo accepts the bundle only if it verifies against the public key that scripts/install-system.sh put on its
# read-only system partition.  Lose secrets/update.key and updates need one more trip through install-system.sh.
#   scripts/ota-push.sh --plugin <file.hmp> [--plugin ...] [host]
# also sends listed plugins' packages (as the plugin index publishes them: its site's packages/<id>-<version>.hmp)
# once the pushed hassmic is up: an Echo without internet gets its plugins' updates so (no bundle carries plugins).
# The Echo takes one only as an update of a plugin the owner accepted there:
# it has the index key's listing, the author key that is pinned, and asks for nothing beyond what was accepted (web.c
# web_plugin_push); anything else it refuses and says why, and the settings page is where that is installed.  The
# push itself is answered with secrets/update.key, as adb over Wi-Fi is (ota.c "HMOTA-PLUG1").
set -e
PLUGINS=
while [ "$1" = --plugin ]; do
    [ -f "$2" ] || { echo "ota-push: --plugin: no such file: $2"; exit 1; }
    case "$2" in *[[:space:]]*) echo "ota-push: --plugin: a path without spaces, please: $2"; exit 1 ;; esac
    PLUGINS="$PLUGINS $(cd "$(dirname "$2")" && pwd)/$(basename "$2")"; shift 2
done
cd "$(dirname "$0")/.."
. scripts/lib/device.sh; device_load
HOST=${1:-$(cat secrets/ota.host 2>/dev/null || true)}
[ -n "$HOST" ] || { echo "usage: scripts/ota-push.sh [--plugin <file.hmp>]... <echo-ip-or-hostname>"; exit 1; }
[ -f secrets/update.key ] || { echo "secrets/update.key missing: this key pair is created by scripts/install-system.sh"; exit 1; }
PORT=${OTA_PORT:-28929}

. scripts/lib/build.sh; build_binaries    # built here or this commit's release build (PREBUILT)
VERSION=$(build_version)                    # what hassmic will report: the commit's time and id, or the release's
scripts/bundle.sh secrets/update.key "$VERSION"
echo "pushing $DEVICE build to $HOST ..."
python3 scripts/otatool.py push "$HOST" $PORT $OUT/hassmic.bundle $OUT/hassmic.bundle.sig
mkdir -p secrets; echo "$HOST" > secrets/ota.host

# root restarts hassmic with the new build 2 s after it answered; the package goes to the new one (the old one may
# lack a scope version the package requires), so wait that out, then for the new one's push port (up to a minute:
# the Echo's own start)
plugins_failed=0
if [ -n "$PLUGINS" ]; then
    sleep 6
    for p in $PLUGINS; do
        echo "pushing $(basename "$p") ..."
        python3 scripts/otatool.py plugin "$HOST" $PORT secrets/update.key "$p" 60 || plugins_failed=1
    done
fi

echo
echo "$VERSION runs on $HOST now. Once it has passed its self test (seconds) it is also what the Echo falls back to."
[ $plugins_failed = 0 ] || { echo "A plugin was not installed (the Echo's answer above says why): the settings page's Plugins page installs it."; exit 1; }
