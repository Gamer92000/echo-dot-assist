#!/bin/sh
# Update an installed Echo over Wi-Fi: build, pack, sign with secrets/update.key, push.  No adb, no TWRP.
#   scripts/ota-push.sh [host]      host defaults to the one used last time (secrets/ota.host)
# The Echo accepts the bundle only if it verifies against the public key that scripts/install-system.sh put on its
# read-only system partition.  Lose secrets/update.key and updates need one more trip through install-system.sh.
set -e
cd "$(dirname "$0")/.."
HOST=${1:-$(cat secrets/ota.host 2>/dev/null || true)}
[ -n "$HOST" ] || { echo "usage: scripts/ota-push.sh <echo-ip-or-hostname>"; exit 1; }
[ -f secrets/update.key ] || { echo "secrets/update.key missing: this key pair is created by scripts/install-system.sh"; exit 1; }
make -s all build/otatool-host
VERSION="$(sed -n 's/^#define VERSION "\(.*\)"/\1/p' src/hassmic/core.h)+$(git describe --always --dirty 2>/dev/null || echo nogit)"
build/otatool-host pack secrets/update.key "$VERSION" build/hassmic.bundle \
    build/hassmic build/runas build/latency build/mixcap build/mixplay \
    scripts/system/main.sh scripts/device/lockdown.sh scripts/device/alexa-off.sh scripts/device/alexa-on.sh
echo "pushing to $HOST ..."
build/otatool-host push "$HOST" "${OTA_PORT:-28929}" build/hassmic.bundle build/hassmic.bundle.sig
mkdir -p secrets; echo "$HOST" > secrets/ota.host
