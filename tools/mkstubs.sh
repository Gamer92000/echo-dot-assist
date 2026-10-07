#!/bin/sh
# Writes devices/<codename>/stubs/<lib>.syms: per stock library what our device binaries need of it, in the library's own
# symbol order:  "T name"  a function it defines that they call, or a symbol of theirs it defines as well (old toolchains'
#                          _end, _edata, and main): the linker exports those from the executable;
#                "U name"  a symbol of theirs it refers to (__emutls_get_address): exported the same way.
# `make STUBS=1` builds stand-ins from these lists (same soname, empty functions, the same references) and links against
# them instead of the firmware's: what CI does, where the firmware cannot be.  The binaries come out the same, byte for
# byte (checked below): the linker only records names, and orders the exported ones as the libraries list them.  Run it
# again when the code starts using another function of a stock library; the check then fails until it has.
#   tools/mkstubs.sh [codename...]      default: every model with a device.mk; their firmware must be unpacked
set -e
export LC_ALL=C                         # sort and comm must agree on the order
cd "$(dirname "$0")/.."
DEVS=${*:-$(ls devices/*/device.mk | cut -d/ -f2)}
NDK=${NDK:-$PWD/toolchain/android-ndk-r21e}
BIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
T=$(mktemp -d); trap 'rm -rf $T' EXIT
BINS="hassmic mixcap mixplay latency pryon_test aed_test whisper_test"

# Undefined in our binaries, not from the NDK's own libc/libm/libdl/libOpenSLES (device.mk SYSLIBS): what the stock
# libraries must provide.  Includes the EABI helpers (__aeabi_idiv ...) that the stock libraries export: the linker takes
# them from there before libgcc.
SYSLIB=$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/arm-linux-androideabi/24
$BIN/llvm-nm -D --defined-only $SYSLIB/libc.so $SYSLIB/libm.so $SYSLIB/libdl.so $SYSLIB/libOpenSLES.so 2>/dev/null | awk 'NF == 3 { print $3 }' | sort -u > $T/ndk
for d in $DEVS; do
    make -s DEVICE=$d all > /dev/null
    for b in $BINS; do [ -f build/$d/$b ] && $BIN/llvm-nm -D --undefined-only build/$d/$b | awk '{ print $2 }'; done |
        sort -u | comm -23 - $T/ndk > $T/need
    for b in $BINS; do [ -f build/$d/$b ] && readelf -W --dyn-syms build/$d/$b | awk '$7 != "UND" && $7 != "Ndx" && $8 != "" { print $8 }'; done |
        sort -u > $T/exported
    mkdir -p devices/$d/stubs; rm -f devices/$d/stubs/*.syms
    for l in $(sed -n 's/^LIBS *:= *//p' devices/$d/device.mk); do
        out=devices/$d/stubs/${l%.so}.syms
        readelf -W --dyn-syms firmware/$d/rootfs/system/lib/$l | awk -v need=$T/need -v exported=$T/exported -v lib=$l '
            BEGIN { while ((getline s < need) > 0) n[s] = 1; while ((getline s < exported) > 0) e[s] = 1 }
            $8 == "" || $7 == "Ndx" || seen[$8]++ { next }
            $7 == "UND" { if ($8 in e) print "U", $8; next }
            ($8 in n) && $4 == "OBJECT" { print lib ": data symbol " $8 " imported, stubs only do functions" > "/dev/stderr"; exit 1 }
            ($8 in n) || ($8 in e) { print "T", $8 }' > $out
        echo "$out: $(wc -l < $out) symbols"
    done
done

# The point of it all: the same binaries from stubs as from the firmware.
for d in $DEVS; do
    make -s DEVICE=$d STUBS=1 OUT=$T/st/$d $(for b in $BINS; do [ -f build/$d/$b ] && echo $T/st/$d/$b; done) > /dev/null
    for b in $BINS; do
        [ -f build/$d/$b ] || continue
        cmp -s build/$d/$b $T/st/$d/$b && echo "$d: $b identical" || { echo "$d: $b DIFFERS between firmware and stub link"; exit 1; }
    done
done
