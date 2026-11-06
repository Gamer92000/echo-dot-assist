#!/bin/sh
# Packs and signs an update bundle from a build of one model: what scripts/ota-push.sh pushes and CI publishes.
#   [DEVICE=<codename>] scripts/bundle.sh KEY VERSION       -> build/<codename>/hassmic.bundle and hassmic.bundle.sig
# Needs that model's binaries in build/<codename>/ first: `make all`, or a release's (scripts/lib/build.sh).
# RELEASE_PUB rides along as release.pub (keys/release.pub, or a fork's own: scripts/lib/device.sh release_load): the
# key of the releases, which the Echo then accepts for the updates hassmic downloads itself, once a channel is picked on
# the settings page (scripts/system/main.sh).
set -e
cd "$(dirname "$0")/.."
. scripts/lib/device.sh; device_load
OUT=${BUNDLE_FROM:-$OUT}            # scripts/content-id.sh: packs from a build of its own
[ $# = 2 ] || die "usage: [DEVICE=<codename>] scripts/bundle.sh KEY VERSION"
python3 scripts/otatool.py pack "$1" "$2" $OUT/hassmic.bundle \
    $OUT/hassmic $OUT/runas $OUT/otatool $(ls $OUT/latency $OUT/mixcap $OUT/mixplay $OUT/pryon_test 2>/dev/null) $DDIR/device.conf:644 \
    $(for k in $OUT/*.ko; do [ -f "$k" ] && echo "$k:644"; done) $RELEASE_PUB:644 \
    $(for k in wakealign/*.wa wakealign/*.was; do echo "$k:644"; done) \
    scripts/system/main.sh scripts/system/boot.sh scripts/system/sysinstall.sh $DDIR/hassmic.rc:644 \
    scripts/device/lockdown.sh scripts/device/alexa-off.sh scripts/device/alexa-on.sh scripts/device/artifact-install.sh scripts/device/apps.sh scripts/device/wifi.sh \
    $([ -z "$HOME_APK" ] || echo scripts/device/$HOME_APK:644) \
    $([ -n "$ANDROID" ] && echo scripts/device/wifictl.dex:644) $([ -n "$SCREEN_BAND" ] && echo scripts/device/ringbar.dex:644) \
    $([ ! -f $OUT/libhmui.so ] || echo $OUT/libhmui.so:644 scripts/device/screen.dex:644) $([ ! -f $OUT/libhmcam.so ] || echo $OUT/libhmcam.so:644)
# wifictl: WifiService's client (wifi.sh), Android models only (device.conf ANDROID); ringbar: the light band's renderer
# (ring_band.c, SCREEN_BAND); HOME_APK: the home screen (sysinstall.sh); libhmui.so + screen.dex: the screen's UI
# (screen.c), where the model has one (device.mk SCREEN)
