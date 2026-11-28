#!/bin/bash
# The XIAO nRF52840's firmware for the CSL tests (xiao-csl.conf says what it is): nRF Connect SDK v3.4.2's OpenThread
# CLI sample (nrf/samples/openthread/cli), OpenThread from its sources, with xiao-module/ (shell commands
# "bootloader" and "lastfatal", the crystal held).  Not upstream Zephyr: its 802.15.4 driver is built on the open
# source service layer, without delayed receive, CSMA or timestamps (nrf_802154 driver/CMakeLists.txt,
# SL_OPENSOURCE), and as a sleepy device OpenThread's software CSMA then asserts in the driver
# (nrf_802154_trx_standalone_cca), the CPU halted, USB with it (2026-11-28); NCS links Nordic's own service layer
# (nrfxlib's libnrf-802154-sl.a).  toolchain/ncs: west init -m https://github.com/nrfconnect/sdk-nrf --mr v3.4.2,
# west update --narrow -o=--depth=1 zephyr nrfxlib openthread hal_nordic mbedtls oberon-psa-crypto bsim, then
# cmsis_6, segger and picolibc at the manifest's commits (west list -f '{path} {revision} {url}'; west update
# refuses projects resolved through imports by name); Zephyr's SDK 1.0.1 and west from toolchain/zephyr
# (its .venv).  Writes <out>/zephyr/zephyr.hex: tools/gp712/xiao-flash.sh puts it on over the USB port.
#   tools/gp712/xiao-build.sh [out dir]
set -euo pipefail
top=$(cd "$(dirname "$0")/../.." && pwd)
z=$top/toolchain/zephyr
ncs=$top/toolchain/ncs
out=${1:-$top/build/xiao-ncs}
[ -x "$z/.venv/bin/west" ] && [ -d "$z/zephyr-sdk-1.0.1/gnu/arm-zephyr-eabi" ] && [ -d "$ncs/nrf" ] && [ -d "$ncs/nrfxlib" ] ||
    { echo "xiao-build: toolchain/zephyr or toolchain/ncs is not set up (see this script's head)" >&2; exit 1; }
export ZEPHYR_SDK_INSTALL_DIR=$z/zephyr-sdk-1.0.1 ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export PATH=$z/.venv/bin:$PATH
cd "$ncs"
west build -p always --no-sysbuild -b xiao_ble -d "$out" nrf/samples/openthread/cli -- \
    "-DEXTRA_CONF_FILE=$top/tools/gp712/xiao-csl.conf" "-DZEPHYR_EXTRA_MODULES=$top/tools/gp712/xiao-module" \
    > "$out.log" 2>&1 || { tail -30 "$out.log" >&2; exit 1; }
ls -la "$out/zephyr/zephyr.hex"
