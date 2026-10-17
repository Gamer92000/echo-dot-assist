#!/bin/sh
# What a model's update bundle holds, apart from its version: CI publishes it beside each bundle and publishes a beta only
# when it differs from the newest release's, so a commit that changes only CI or the docs offers Echos nothing.
#   [DEVICE=<codename>] scripts/content-id.sh [make variables, e.g. STUBS=1]     -> prints a sha256
# Needs that model's build in build/<codename>/ (make all).  hassmic is the only file with the version in it (VERSION,
# BUILD, BUILD_TIME: string literals), so it is built again with those fixed, beside copies of the rest, and that is packed
# with version 0.  Packing is deterministic (version line, then each file's name, mode, size and bytes) and builds repeat
# byte for byte; the key is a throwaway, since only the bundle is hashed, not its signature.
set -e
cd "$(dirname "$0")/.."
. scripts/lib/device.sh; device_load
N=build/$DEVICE-content
rm -rf $N; mkdir -p $N
find $OUT -maxdepth 1 -type f ! -name 'hassmic*' -exec cp -p {} $N/ \;
make -s DEVICE=$DEVICE RELEASE=1 OUT=$N VERSION=0 BUILD=0 BUILD_TIME=0 "$@" $N/hassmic >&2
python3 scripts/otatool.py keygen $N/throwaway.key $N/throwaway.pub > /dev/null
BUNDLE_FROM=$N scripts/bundle.sh $N/throwaway.key 0 > /dev/null
sha256sum $N/hassmic.bundle | cut -d' ' -f1
