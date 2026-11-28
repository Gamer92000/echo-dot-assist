#!/bin/sh
# The port's update bundle for an OS builder's --port (laser-os-builder: build/<codename>/hassmic.bundle, which it bakes
# into the system image as the factory copy), signed with secrets/update.key; the key pair is made here when there is
# none.  The binaries come as ota-push.sh takes them (scripts/lib/build.sh build_binaries): this commit's release build
# from GitHub where there is no Android NDK in toolchain/ (the Echo 4's build also needs cmake, ninja, zig and the
# kernel tools), else built here; PREBUILT=1 always the release build, PREBUILT=0 always built here.
#   DEVICE=<codename> scripts/port-bundle.sh
set -e
cd "$(dirname "$0")/.."
. scripts/lib/device.sh; device_load
[ $# = 0 ] || die "usage: DEVICE=<codename> scripts/port-bundle.sh"
mkdir -p secrets
[ -f secrets/update.key ] || {
    python3 scripts/otatool.py keygen secrets/update.key secrets/update.pub
    echo "new update signing key: secrets/update.key (keep it, back it up)"
}
[ -f secrets/update.pub ] || die "secrets/update.key without secrets/update.pub: the OS builder bakes the .pub in"
. scripts/lib/build.sh; build_binaries
VERSION=$(build_version)
scripts/bundle.sh secrets/update.key "$VERSION"
echo "$OUT/hassmic.bundle: $DEVICE, $VERSION"
