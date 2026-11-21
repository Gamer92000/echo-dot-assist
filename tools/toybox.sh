#!/bin/sh
# Cross-builds the toybox that models without one of their own ship (device.mk: TOYBOX := 1; rook: Android 5.1's toolbox
# has no sed, tr, cut, head, tail, wc, printf, pkill, pidof, timeout, stat, sort or od, which main.sh and the scripts use).
#   tools/toybox.sh OUT-FILE NDK-CLANG [strip]
# toybox 0.8.4 (0BSD): the newest that NDK r21e builds (0.8.11 wants getentropy, API 28).  Static, the applets of
# tools/toybox.config only.  The source tarball is downloaded into build/toybox/ and checked against the digest below.
set -e
cd "$(dirname "$0")/.."
OUT=$1 CC=$2 STRIP=${3:-}
VER=0.8.4
URL=https://landley.net/toybox/downloads/toybox-$VER.tar.gz
SHA256=cb2a565a8d30015d08d73628795dca51a85b99b149aeabbbecd9e8dbdbd8fddc
[ -n "$OUT" ] && [ -n "$CC" ] || { echo "usage: tools/toybox.sh OUT-FILE NDK-CLANG [strip]" >&2; exit 2; }
W=build/toybox; mkdir -p $W
[ -f $W/toybox-$VER.tar.gz ] || curl -fsSL -o $W/toybox-$VER.tar.gz $URL
[ "$(sha256sum < $W/toybox-$VER.tar.gz | cut -c1-64)" = $SHA256 ] || { echo "toybox-$VER.tar.gz: wrong digest, deleted" >&2; rm -f $W/toybox-$VER.tar.gz; exit 1; }
rm -rf $W/src; mkdir -p $W/src; tar xzf $W/toybox-$VER.tar.gz -C $W/src --strip-components=1
cp tools/toybox.config $W/src/.config
make -C $W/src CROSS_COMPILE= CC="$CC" HOSTCC=cc CFLAGS=-Os LDFLAGS=-static toybox > $W/build.log 2>&1 || { tail -20 $W/build.log >&2; exit 1; }
cp $W/src/toybox $OUT.tmp; chmod u+w $OUT.tmp
[ -z "$STRIP" ] || "$STRIP" $OUT.tmp
mv $OUT.tmp $OUT
