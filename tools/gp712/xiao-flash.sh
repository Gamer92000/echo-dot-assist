#!/bin/bash
# The XIAO nRF52840's firmware (tools/gp712/xiao-build.sh) onto it over its USB port, no button: our firmware's shell
# command "bootloader" (xiao-module/), or Seeed's Arduino firmware's 1200 baud open of its port, resets it into the
# bootloader's serial DFU (Adafruit's nRF52 bootloader: USB 2886:0045), which adafruit-nrfutil speaks.  The double
# press of the reset button, the bootloader's own way in, did not work on the unit here (2026-11-28); a firmware
# without the command but with Zephyr's devmem shell gets there by hand: devmem 0x4000051C 32 0x4E (GPREGRET), then
# devmem 0xE000ED0C 32 0x05FA0004 (SYSRESETREQ).
#   tools/gp712/xiao-flash.sh [build dir]
# adafruit-nrfutil in toolchain/zephyr/.venv (uv pip install adafruit-nrfutil==0.5.3.post16).
set -euo pipefail
top=$(cd "$(dirname "$0")/../.." && pwd)
out=${1:-$top/build/xiao-ncs}
nrfutil=$top/toolchain/zephyr/.venv/bin/adafruit-nrfutil
[ -x "$nrfutil" ] || { echo "xiao-flash: no $nrfutil (see this script's head)" >&2; exit 1; }
port() { ls /dev/serial/by-id/ 2>/dev/null | grep -E "$1" | head -1 || true; }
boot='Seeed_XIAO_nRF52840'                     # the bootloader's own port (the Arduino firmware's is "Arduino_Seeed_...")
p=$(port 'Zephyr_Project_CDC_ACM|Zephyr_Project_Thread_CLI|Arduino_Seeed_XIAO')
if [ -n "$p" ]; then
    python3 - "/dev/serial/by-id/$p" <<'EOF'
import os, sys, termios, time
fd = os.open(sys.argv[1], os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
a = termios.tcgetattr(fd)
if "Arduino" in sys.argv[1]:
    a[4] = a[5] = termios.B1200                 # Seeed's firmware: a 1200 baud open is the way in
    termios.tcsetattr(fd, termios.TCSANOW, a)
else:
    os.write(fd, b"\r\nbootloader\r\n")         # ours: the shell command
time.sleep(0.5)
os.close(fd)
EOF
fi
for _ in $(seq 1 40); do b=$(port "^usb-$boot"); [ -n "$b" ] && break; sleep 0.5; done
[ -n "${b:-}" ] || { echo "xiao-flash: the bootloader's port did not come (press reset twice, quickly)" >&2; exit 1; }
pkg=$out/xiao-dfu.zip
"$nrfutil" dfu genpkg --dev-type 0x0052 --application "$out/zephyr/zephyr.hex" "$pkg" > /dev/null
"$nrfutil" dfu serial --package "$pkg" -p "/dev/serial/by-id/$b" -b 115200 --singlebank
