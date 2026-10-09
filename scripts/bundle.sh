#!/bin/sh
# Packs and signs an update bundle from a build of one model: what scripts/ota-push.sh pushes and CI publishes.
#   [DEVICE=<codename>] scripts/bundle.sh KEY VERSION       -> build/<codename>/hassmic.bundle and hassmic.bundle.sig
# Needs that model's binaries in build/<codename>/ first: `make all`, or a release's (scripts/lib/build.sh).
# keys/release.pub rides along: the key of the project's releases, which the Echo then accepts for the updates hassmic
# downloads itself, once a channel is picked on the settings page (scripts/system/main.sh).
set -e
cd "$(dirname "$0")/.."
. scripts/lib/device.sh; device_load
OUT=${BUNDLE_FROM:-$OUT}            # scripts/content-id.sh: packs from a build of its own
[ $# = 2 ] || die "usage: [DEVICE=<codename>] scripts/bundle.sh KEY VERSION"
python3 scripts/otatool.py pack "$1" "$2" $OUT/hassmic.bundle \
    $OUT/hassmic $OUT/runas $OUT/otatool $(ls $OUT/latency $OUT/mixcap $OUT/mixplay $OUT/pryon_test 2>/dev/null) $DDIR/device.conf:644 \
    $(for k in $OUT/*.ko; do [ -f "$k" ] && echo "$k:644"; done) keys/release.pub:644 \
    scripts/system/main.sh scripts/system/boot.sh scripts/system/sysinstall.sh $DDIR/hassmic.rc:644 \
    scripts/device/lockdown.sh scripts/device/alexa-off.sh scripts/device/alexa-on.sh scripts/device/artifact-install.sh scripts/device/wifi.sh \
    $([ "$INSTALL" != boot ] || echo scripts/device/wifictl.dex:644)    # WifiService's client (wifi.sh), INSTALL=boot only
