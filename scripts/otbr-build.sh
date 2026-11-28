#!/bin/bash
# Our own Thread border router for the Echos with an 802.15.4 radio (device.mk THREAD := 1; the Echo 4: a Qorvo GP712
# behind Amazon's ot-rcp, docs/re-thread.md): OpenThread's ot-br-posix built with the NDK for the model's target,
# libc only.  Usage: scripts/otbr-build.sh <out dir> <clang target> [<ndk>]
#   writes <out>/otbr-agent and <out>/ot-ctl (stripped); sources fetched once into toolchain/src/otbr/ by checksum.
#
# Pinned sources: ot-br-posix's main of 2026-11-26 and the submodule commits it names (GitHub's archives, which do not
# carry submodules, so each is fetched by itself and put where the submodule goes).  A new pin: change a line, run it,
# put the new checksum in.  ~14 MB of archives, ~3 min of build.
#
# How it differs from a plain build of that tree, and why:
# - Amazon's RCP (OPENTHREAD/404cfe2ff, Qorvo's fork for the GP712, RCP API 11) has no transmit security and no timed
#   transmit (RADIO_CAPS 0x1f: ack timeout, energy scan, retries, CSMA, sleep to tx), which a Thread 1.2+ host asks
#   of every RCP.  The host does the MAC security itself where the radio does not (sub_mac.cpp ShouldHandle
#   kCapTransmitSec), as a Thread 1.1 RCP always had it; what needs the radio's own clock is switched off: the CSL
#   transmitter (frames to a CSL child at its window) and the link metrics subject (enhanced-ACK probing).  main.sh
#   gives the radio URL skip-rcp-compatibility-check, which only leaves out that capability check (radio.cpp).
# - Android's tun is /dev/tun (OpenThread's posix default /dev/net/tun); the daemon's socket and settings live in
#   /data/local/hassmic/thread (no /run, no /var on Android); the build date is read from ro.build.date at run time
#   (ANDROID_NDK_ENABLE, instance_api.cpp), so nothing of the build's clock goes in.
# - __FILE__ is the file's name alone (clang 9 has no -ffile-prefix-map): log lines name sources by their path, and
#   the binary must be the same whatever folder it was built in (reproducible builds, CI against local).
# - No D-Bus, no avahi: OpenThread's own mDNS (ot-br-posix's default since 2025), the REST server for Home
#   Assistant's OTBR integration on a unix socket main.sh names (hassmic forwards it, radio.c), no web GUI, no
#   firewall helper (ipset).
# - Thread 1.3/1.4's border router features are in (PLAN.md "Thread 1.4", phase 3), main.sh decides at run time:
#   NAT64 (OpenThread's translator: IPv4 out of ot0 from OPENTHREAD_POSIX_CONFIG_NAT64_CIDR, the Echo's MASQUERADE
#   only while the owner's setting thread_nat64 is on), upstream DNS for Thread devices (the resolver reads a file of
#   ours: Android has no /etc/resolv.conf), TREL (Thread over the Wi-Fi between border routers; its UDP port fixed,
#   28936, in the range lockdown.sh admits), the backbone router (multicast across the home network: the kernel's
#   IPv6 multicast routing), DHCPv6 prefix delegation (setting thread_pd), the ephemeral key (ePSKc, the page's
#   "Share the network" through REST).
set -euo pipefail
out=$1; target=$2
top=$(cd "$(dirname "$0")/.." && pwd)
ndk=${3:-$top/toolchain/android-ndk-r21e}
src=$top/toolchain/src/otbr
mkdir -p "$src" "$out"; out=$(cd "$out" && pwd)

# name  where it goes in the tree  archive  sha256
SOURCES="
ot-br-posix  .                                                   openthread/ot-br-posix/archive/7d52ce8fd6bfccb121ba70cb1ce11451048c165c   a5ac25fddfbfa175b6c4da283bd275bd7f6ddd584da064c20ba860ed69398136
openthread   third_party/openthread/repo                         openthread/openthread/archive/27ded13010ef7ce8c47560108f1e7c27ecde1d26    709e6ee5b3a2304162204ab0b990799a4b5b74b68a2a41cc01a92895e4c280e4
mbedtls      third_party/openthread/repo/third_party/mbedtls/repo Mbed-TLS/mbedtls/archive/82e69999f91126589a28b70dd75b69fbcabe2711        49562d7a841503708ab8a8d2ef4939e655dfa3d2363ab8400c2f426684f60492
framework    third_party/openthread/repo/third_party/mbedtls/repo/framework Mbed-TLS/mbedtls-framework/archive/ced31491c5ec907a6be533512f19dc1779230ac1 66c5bea27c94cb0e6838c741593e1832497f386b639d64f0a3c3dbf1d71f9055
cjson        third_party/cJSON/repo                              DaveGamble/cJSON/archive/cf97c6f066d81fdbba4ef722cfd327bbbba2365c        1db000953ca1baf2caa3c0aac2a1d713158ff545fa7f5da22b8b965fb7bddc62
httplib      third_party/cpp-httplib/repo                        yhirose/cpp-httplib/archive/03cf43ebaa55f27a2778bed870ea3549f7e84e2c     6e049be7ab3ba6ae95067b70aa0b53d442f92935fde160aad521da3459c969d9
"
# the tree's name: the sources and the patches below (one more patch: PATCHES one up, so a tree built before is not taken)
PATCHES=3
pin=$(echo "$SOURCES $PATCHES" | sha256sum | cut -c1-16)
tree=$src/tree-$pin
if [ ! -f "$tree/.complete" ]; then
    rm -rf "$tree"; mkdir -p "$tree"
    echo "$SOURCES" | while read -r name dir path sum; do
        [ -n "$name" ] || continue
        a=$src/$name-${path##*/}.tar.gz
        if [ ! -f "$a" ] || [ "$(sha256sum "$a" | cut -c1-64)" != "$sum" ]; then
            curl -fsSL --retry 3 -o "$a.part" "https://github.com/$path.tar.gz"
            [ "$(sha256sum "$a.part" | cut -c1-64)" = "$sum" ] || { echo "otbr-build: $name: checksum differs" >&2; rm -f "$a.part"; exit 1; }
            mv "$a.part" "$a"
        fi
        mkdir -p "$tree/$dir"
        tar -xzf "$a" -C "$tree/$dir" --strip-components=1
    done
    # the link metrics subject: ot-br-posix forces it on for every Thread version but 1.1 (see the top)
    sed -i 's/set(OT_LINK_METRICS_SUBJECT ON CACHE STRING "enable link metrics subject" FORCE)/set(OT_LINK_METRICS_SUBJECT OFF CACHE STRING "enable link metrics subject" FORCE)/' \
        "$tree/third_party/openthread/CMakeLists.txt"
    grep -q 'OT_LINK_METRICS_SUBJECT OFF' "$tree/third_party/openthread/CMakeLists.txt" || { echo "otbr-build: the link metrics patch no longer applies" >&2; exit 1; }
    # the upstream DNS servers: a file main.sh writes (Android's /etc is the system partition's, with no resolv.conf)
    # (root writes it in its own folder: not in the radio user's, where a link could point root elsewhere)
    r=$tree/third_party/openthread/repo/src/posix/platform/resolver.cpp
    sed -i 's|kResolvConfFullPath\[\] = "/etc/resolv.conf"|kResolvConfFullPath[] = "/data/local/hassmic/thread-resolv.conf"|' "$r"
    grep -q '/data/local/hassmic/thread-resolv.conf' "$r" || { echo "otbr-build: the resolv.conf patch no longer applies" >&2; exit 1; }
    touch "$tree/.complete"
fi

case $target in
armv7a-*) abi=armeabi-v7a;;
aarch64-*) abi=arm64-v8a;;
*) echo "otbr-build: no Android ABI for $target" >&2; exit 1;;
esac
api=${target##*androideabi}; api=${api##*android}

# bionic has forkpty and the threads in libc; ot-br-posix's link line names -lutil and -lpthread all the same
empty=$out/otbr-empty
mkdir -p "$empty"
for l in util pthread; do [ -f "$empty/lib$l.a" ] || "$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-ar" rc "$empty/lib$l.a"; done

flags='-D__FILE__=__FILE_NAME__ -Wno-builtin-macro-redefined -DOPENTHREAD_CONFIG_ANDROID_NDK_ENABLE=1'
flags="$flags -DOPENTHREAD_POSIX_CONFIG_DAEMON_SOCKET_BASENAME=\\\"/data/local/hassmic/thread/openthread-%s\\\""
# the factory and product configuration files (power tables, region): none; a path on the Echo, not the build's
flags="$flags -DOPENTHREAD_POSIX_CONFIG_FACTORY_CONFIG_FILE=\\\"/data/local/hassmic/thread/openthread.conf\\\""
flags="$flags -DOPENTHREAD_POSIX_CONFIG_PRODUCT_CONFIG_FILE=\\\"/data/local/hassmic/thread/openthread.conf\\\""
# REST answers name no origin a browser has (upstream sends Access-Control-Allow-Origin: * on every answer): a web
# page in a browser on Home Assistant's machine must not read the network key through it, nor PUT or DELETE past the
# preflight.  python-otbr-api (Home Assistant) sends no Origin.  hassmic also drops a request that comes from a
# browser at all (radio.c browser_said)
flags="$flags -DOTBR_REST_ACCESS_CONTROL_ALLOW_ORIGIN=\\\"http://invalid.invalid\\\""
flags="$flags -DOPENTHREAD_POSIX_TUN_DEVICE=\\\"/dev/tun\\\""
# OTBR_CSL=1 (a test build, PLAN.md "Thread 1.4" 2b.3): the CSL transmitter on, for gprcp's timed transmit on the
# chip's clock (src/gprcp/timed.c, hassmic_gptime).  Its frames are handed to the RCP 4 ms ahead of their time (2 ms by
# default): staging and ScheduleTimedTx take up to 1.4 ms on the chip, Spinel's way through rcpbridge comes before
if [ "${OTBR_CSL:-0}" = 1 ]; then
    flags="$flags -DOPENTHREAD_CONFIG_MAC_CSL_TRANSMITTER_ENABLE=1 -DOPENTHREAD_CONFIG_MAC_CSL_REQUEST_AHEAD_US=4000"
else
    flags="$flags -DOPENTHREAD_CONFIG_MAC_CSL_TRANSMITTER_ENABLE=0"
fi
flags="$flags -DOPENTHREAD_POSIX_CONFIG_TREL_UDP_PORT=28936"
# the border agent (a phone's Thread app, commissioners) on a port of the range lockdown.sh admits: OpenThread's
# default is an ephemeral one, above 32767, which the Echo's INPUT drops; its ephemeral key (ePSKc) on 28938 (radio.c)
flags="$flags -DOPENTHREAD_CONFIG_BORDER_AGENT_UDP_PORT=28937"
b=$out/otbr-build-$pin           # one per tree: cmake binds a build folder to its sources
mkdir -p "$b"
( cd "$b"
  PKG_CONFIG_LIBDIR=/nonexistent PKG_CONFIG_PATH= cmake -GNinja -Wno-dev -Wno-deprecated -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" -DANDROID_ABI=$abi -DANDROID_PLATFORM=android-$api \
    -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    "-DCMAKE_C_FLAGS=$flags" "-DCMAKE_CXX_FLAGS=$flags" "-DCMAKE_EXE_LINKER_FLAGS=-L$empty" \
    -DOTBR_VERSION=ot-br-posix-7d52ce8 -DOT_PACKAGE_VERSION=openthread-27ded13 \
    '-DOT_POSIX_SETTINGS_PATH="/data/local/hassmic/thread"' \
    -DOTBR_VENDOR_NAME=hassmic -DOTBR_PRODUCT_NAME=Echo \
    -DOTBR_REST=ON -DOTBR_BACKBONE_ROUTER=ON -DOTBR_TREL=ON -DOTBR_WEB=OFF -DOTBR_DBUS=OFF -DOTBR_NOTIFY_UPSTART=OFF \
    -DOT_FIREWALL=OFF -DOTBR_NAT64=ON -DOTBR_DNS_UPSTREAM_QUERY=ON -DOTBR_DHCP6_PD=ON -DOTBR_EPSKC=ON \
    -DOTBR_POWER_CALIBRATION=OFF -DOTBR_SRP_SERVER_AUTO_ENABLE=ON \
    "$tree" > cmake.log 2>&1 || { tail -30 cmake.log >&2; exit 1; }
  ninja otbr-agent ot-ctl > ninja.log 2>&1 || { grep -E 'error|FAILED' ninja.log | head -30 >&2; exit 1; } )
strip=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip
"$strip" -o "$out/otbr-agent" "$b/src/agent/otbr-agent"
"$strip" -o "$out/ot-ctl" "$b/third_party/openthread/repo/src/posix/ot-ctl"
echo "otbr-build: $out/otbr-agent $(stat -c%s "$out/otbr-agent") bytes, $out/ot-ctl"
