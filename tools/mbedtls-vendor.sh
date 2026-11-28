#!/bin/sh
# Puts the parts of an mbedTLS release that hassmic builds into src/third_party/mbedtls/ (issue #236): the library's
# .c files that hassmic_config.h leaves any code in, and every header they or tls.c include, as the release has
# them.  Nothing else of the release (its tests, programs, 3rdparty/), and nothing changed.
#
#   tools/mbedtls-vendor.sh <mbedtls-X.Y.Z.tar.bz2> <its SHA-256, from the release's page>
#
# Run it again for a new release (and after a change of hassmic_config.h that turns on more), then make unit-http and
# the HTTPS tests (tests/run.py esphome ma-api update davs plugins-fetch).  Needs a C compiler for the PC: what the
# config keeps is found by compiling.
set -eu
tar_=$1; sum=$2
root=$(cd "$(dirname "$0")/.." && pwd)
dst=$root/src/third_party/mbedtls
echo "$sum  $tar_" | sha256sum -c - >/dev/null || { echo "mbedtls-vendor: $tar_ is not the release with SHA-256 $sum" >&2; exit 1; }
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
tar -xjf "$tar_" -C "$tmp"
src=$(echo "$tmp"/mbedtls-*); ver=${src##*/mbedtls-}
mkdir -p "$tmp/obj"
keep=""
for f in "$src"/library/*.c; do
    b=$(basename "$f" .c)
    cc -O1 -c -MD -MF "$tmp/obj/$b.d" -I"$src/include" -I"$src/library" -I"$dst" -DMBEDTLS_CONFIG_FILE='"hassmic_config.h"' "$f" -o "$tmp/obj/$b.o"
    # a file whose code the config takes out compiles to nothing
    if nm --defined-only "$tmp/obj/$b.o" | grep -q ' [TDRB] '; then keep="$keep $b"; else rm "$tmp/obj/$b.d"; fi
done
# tls.c's own includes too
printf '#include "mbedtls/error.h"\n#include "mbedtls/ssl.h"\n#include "mbedtls/x509_crt.h"\n#include "psa/crypto.h"\n' > "$tmp/tls_inc.c"
cc -E -MD -MF "$tmp/obj/tls_inc.d" -I"$src/include" -I"$dst" -DMBEDTLS_CONFIG_FILE='"hassmic_config.h"' "$tmp/tls_inc.c" -o /dev/null
hdrs=$(cat "$tmp"/obj/*.d | tr ' \\' '\n\n' | grep -F "$src/" | grep '\.h$' | sed "s|^$src/||" | sort -u)
rm -rf "$dst/include" "$dst/library"
for b in $keep; do mkdir -p "$dst/library"; cp "$src/library/$b.c" "$dst/library/"; done
for h in $hdrs; do mkdir -p "$dst/$(dirname "$h")"; cp "$src/$h" "$dst/$h"; done
cp "$src/LICENSE" "$dst/LICENSE"
echo "$ver $sum" > "$dst/VERSION"
echo "mbedtls-vendor: mbedTLS $ver, $(echo $keep | wc -w) library files, $(echo "$hdrs" | wc -l) headers into $dst"
