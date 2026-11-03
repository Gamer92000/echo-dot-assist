#!/bin/bash
# scripts/install-boot.sh --uninstall (the Echo Shows: checkers, cronos, crown) against a stand-in adb: hassmic off
# /system, then cache and data wiped in TWRP and read back, then Fire OS booted.  An uninstall without the wipe left
# Amazon's apps without their data and default permissions ("OOBE has stopped" in a loop, cronos 2026-11-03).
cd "$(dirname "$0")/../.."; Q=$(mktemp -d); trap 'rm -rf "$Q"' EXIT; fail=0
ok_() { if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; fail=1; fi; }
mkdir $Q/bin
printf '#!/bin/sh\nexit 0\n' > $Q/bin/sleep
# adb: state in $Q/state, whether hassmic is on /system in $Q/installed (0/1), the read-back after the wipe in $Q/left
cat > $Q/bin/adb << 'EOF'
#!/bin/bash
Q=${0%/bin/adb}
echo "adb $*" >> $Q/log
case "$*" in
get-state) cat $Q/state;;
"reboot recovery") echo recovery > $Q/state;;
reboot) echo "BOOT-OS" >> $Q/log; echo device > $Q/state;;
"shell getprop ro.product.device") echo cronos;;
"shell getprop sys.boot_completed") echo 1;;
*"[ -e /system/hassmic/sysinstall.sh ] && echo yes"*) [ "$(cat $Q/installed)" = 1 ] && echo yes;;
*"[ -e /data/local/tmp/hm-install/ok ] && echo yes"*) [ -n "$SYSINSTALL_FAILS" ] || echo yes;;
"shell twrp wipe"*) exit "$(cat $Q/wipe_exit)";;
*"grep -q ' /data '"*) printf '%b' "$(cat $Q/left)";;
esac
EOF
chmod 755 $Q/bin/*

run() {        # STATE INSTALLED READ-BACK [ARGS...]: the exit status, the calls in $Q/log
    echo "$1" > $Q/state; echo "$2" > $Q/installed; printf '%s' "$3" > $Q/left; echo "${WIPE_EXIT:-0}" > $Q/wipe_exit
    : > $Q/log; shift 3
    PATH=$Q/bin:$PATH DEVICE=cronos SYSINSTALL_FAILS=$SYSINSTALL_FAILS sh scripts/install-boot.sh --uninstall "$@" < /dev/null > $Q/out 2>&1
}
has() { grep -qF -- "$1" $Q/log; }
order() {      # the calls containing each argument, in this order
    local n=0 l
    for s; do l=$(grep -nF -- "$s" $Q/log | head -1 | cut -d: -f1); [ -n "$l" ] && [ "$l" -gt $n ] || return 1; n=$l; done
}

run device 1 'end\n' --yes; r=$?
[ $r = 0 ] && order "sysinstall.sh uninstall" "reboot recovery" "twrp wipe cache && twrp wipe data" "grep -q ' /data '" "BOOT-OS"
ok_ $? "installed: off /system, then TWRP, wipe, read back, reboot"
has "alexa-on.sh"; ok_ $? "installed: Alexa's apps turned on"

run device 0 'end\n' --yes; r=$?
[ $r = 0 ] && ! has "sysinstall.sh uninstall" && order "reboot recovery" "twrp wipe" "BOOT-OS"
ok_ $? "not on /system (an older uninstall): only the wipe"

run recovery 0 'end\n' --yes; r=$?
[ $r = 0 ] && ! has "reboot recovery" && ! has "sysinstall.sh uninstall" && order "twrp wipe" "BOOT-OS"
ok_ $? "in TWRP already: the wipe, no reboot to TWRP"

SYSINSTALL_FAILS=1 run device 1 'end\n' --yes; r=$?
[ $r != 0 ] && has "sysinstall.sh uninstall" && ! has "reboot recovery" && ! has "twrp"
ok_ $? "sysinstall.sh did not finish: stops before TWRP, nothing wiped"

run device 1 'com.amazon.spotlight-2\nend\n' --yes; r=$?
[ $r != 0 ] && has "twrp wipe" && ! grep -qx "BOOT-OS" $Q/log && grep -q "the wipe did not take" $Q/out
ok_ $? "an app left after the wipe: stops in TWRP and says so"

run device 1 'unmounted\n' --yes; r=$?
[ $r != 0 ] && ! grep -qx "BOOT-OS" $Q/log
ok_ $? "/data not mounted: not taken for wiped"

WIPE_EXIT=255 run recovery 0 'end\n' --yes; r=$?
[ $r = 0 ] && grep -qx "BOOT-OS" $Q/log
ok_ $? "adb shell cut by a USB restart, the wipe went on: read back, booted"

run device 1 'end\n'; r=$?
[ $r != 0 ] && ! has "sysinstall.sh uninstall" && ! has "reboot" && ! has "twrp" && grep -q -- "--yes" $Q/out
ok_ $? "not on a terminal, no --yes: nothing changed"

echo nothing > $Q/state; : > $Q/log
PATH=$Q/bin:$PATH DEVICE=cronos sh scripts/install-boot.sh --uninstall --yes < /dev/null > $Q/out 2>&1; r=$?
[ $r != 0 ] && ! has "twrp" && ! has "reboot"
ok_ $? "no Echo on adb: nothing done"
exit $fail
