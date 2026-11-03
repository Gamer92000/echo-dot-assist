#!/bin/bash
# The Echo Shows' wipe in the guided setup, the real wipe_data, wipe_twrp, step_system of each model and the rooted menu
# of scripts/setup.sh, with stand-ins for adb and the screen: nothing here needs an Echo, and a wipe that did not take
# shows only long after, as crashes on the Echo (PR #92).
cd "$(dirname "$0")/../.."; Q=$(mktemp -d); trap 'rm -rf "$Q"' EXIT; fail=0
ok_() { if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; fail=1; fi; }
. scripts/lib/setup.sh > /dev/null 2>&1
SYSTEM_DEV=/dev/block/platform/mtk-msdc.0/11230000.MSDC0/by-name/system
LOG=/dev/null FW=x OUT=$Q FIRMWARE_BUILD=good FIRMWARE_ID=FW SYSTEM_IMG_SHA256=abc DRY=
eval "$(grep -E '^(done_ids|mark_done)\(\)' scripts/setup.sh)"
STATE=$Q/state
sleep() { :; }

# --- wipe_twrp: answers to the read-back in numbered files ($(ashell) is a subshell, a shifted array would not last)
adb() {
    case "$*" in
    shell\ twrp*) return $ADB_EXIT;;
    *grep\ -q*) local n; n=$(cat $Q/n); echo $((n + 1)) > $Q/n; [ -f $Q/a$n ] && printf '%b' "$(cat $Q/a$n)";;
    esac
}
check() {      # NAME WANTED-STATUS WIPE-EXIT ANSWER...
    local name=$1 want=$2 i=0 a; ADB_EXIT=$3; shift 3
    command rm -f $Q/a*; echo 0 > $Q/n
    for a; do printf '%s' "$a" > $Q/a$i; i=$((i + 1)); done
    wipe_twrp > /dev/null 2>&1; [ $? = "$want" ]; ok_ $? "wipe_twrp: $name"
}
check "nothing left" 0 0 'end\n'
check "an app left" 1 0 'com.amazon.spotlight-2\nend\n'
check "/data not mounted" 1 0 'unmounted\n'
check "adb away, then back" 0 0 '' '' 'end\r\n'
check "adb never back" 1 0
check "USB restart ends adb shell, the wipe went on" 0 255 '' 'com.amazon.spotlight-2\nend\n' 'end\n'
check "twrp fails, the apps stay" 1 1 'com.amazon.spotlight-2\nend\n'

# --- step_system of each model: wipes, writes and whether the wipe is marked
for model in crown checkers cronos; do (
    eval "$(sed -n '/^step_system() {/,/^}/p' devices/$model/setup.sh)"
    ok() { :; }; info() { :; }; warn() { :; }; fail() { :; }; twrp_screen() { :; }; system_img() { :; }
    wait_adb() { :; }; adb_is() { [ "$1" = "$AT" ]; }; todo() { :; }; rm() { :; }; stat() { echo 7; }
    twrp_system_build() { echo "$SYS"; }
    done_ids() { DONE_IDS="$DONE_IDS $1"; }
    ashell() { case "$1" in getprop) echo "$SYS";; blockdev) echo 7;; esac; }
    task() {
        local label=$1; shift
        case "$label" in "Checking the system partition"*) return $DIGEST;; esac
        case "$1" in wipe_twrp) WIPES=$((WIPES + 1)); return $WIPE_EXIT;; write_system) WRITES=$((WRITES + 1)); return $WRITE_EXIT;; esac
    }
    run() {    # NAME AT SYSTEM-BUILD DIGEST-EXIT DONE-IDS WIPES WRITES WIPED-MARK
        AT=$2 SYS=$3 DIGEST=$4 DONE_IDS=$5 WIPES=0 WRITES=0 WIPED= WIPE_EXIT=0 WRITE_EXIT=0
        step_system > /dev/null 2>&1; r=$?
        m=0; [[ " $DONE_IDS " == *" wiped "* ]] && m=1
        [ "$r/$WIPES/$WRITES/$m" = "0/$6/$7/$8" ]; ok_ $? "$model: $1"
    }
    run "wrong build: wiped and written" recovery bad 0 "" 1 1 1
    run "right build, booted: untouched" device good 0 "" 0 0 0
    run "right build, TWRP: untouched" recovery good 0 "" 0 0 0
    run "a write cut short: wiped, completed" recovery good 1 "" 1 1 1
    run "from scratch, right build, TWRP: wiped, not written" recovery good 0 "wipe" 1 0 1
    run "from scratch, right build, booted: wiped, not written" device good 0 "wipe" 1 0 1
    run "from scratch, wiped before: untouched" recovery good 0 "wipe wiped" 0 0 1
    run "from scratch, wrong build: wiped and written" recovery bad 0 "wipe" 1 1 1
    run "wrong build, wiped by an earlier run: wiped again" recovery bad 0 "wipe wiped" 1 1 1

    # a retry in the same run: the write fails, the wipe is not repeated; the failed wipe is
    AT=recovery SYS=bad DIGEST=0 DONE_IDS= WIPES=0 WRITES=0 WIPED= WIPE_EXIT=0 WRITE_EXIT=1
    step_system > /dev/null 2>&1; WRITE_EXIT=0; step_system > /dev/null 2>&1; r=$?
    [ "$r/$WIPES/$WRITES" = 0/1/2 ]; ok_ $? "$model: the write tried again, no second wipe"
    WIPED= WIPES=0 WIPE_EXIT=1; step_system > /dev/null 2>&1; r=$?; WIPE_EXIT=0
    [ "$r/$WIPES" = 1/1 ] && [ -z "$WIPED" ]; ok_ $? "$model: a failed wipe stops the step"
    exit $fail
) || fail=1; done

# --- the rooted menu: "wipe" only after the warning, which a non-empty progress file skips
menu_block=$(sed -n '/^# --- an Echo rooted already/,/^# no udev rules/p' scripts/setup.sh)
[ -n "$menu_block" ]; ok_ $? "the rooted menu is found in scripts/setup.sh"
header() { :; }; say() { :; }; ok() { :; }; info() { :; }
warn() { echo "$*" >> $Q/warned; }
adb_is() { [ "$1" = device ]; }
ashell() { case "$1" in id) echo 0;; getprop) echo "$FWID";; esac; }
menu() { shift; echo "$*" > $Q/menu; printf -v a %s "$CHOICE"; }
prompt() { printf -v a %s "$TYPED"; }
rooted() {     # CHOICE TYPED ECHO-FIRMWARE MODEL-HAS-FIRMWARE-STEP
    CHOICE=$1 TYPED=$2 FWID=$3
    command rm -f $STATE $Q/warned $Q/menu; : > $STATE; : > $Q/warned; : > $Q/menu
    ( DRY= DONE_IDS= ROOTED_SKIP="usb unlock root" MODEL_NAME=Echo ROOTED_NOTE= SCRATCH= STEP_LIST=(x) n=1 B= N=
      if [ "$4" = 1 ]; then step_system() { :; }; else unset -f step_system step_flash; fi
      eval "$menu_block" )
}
rooted 1 yes FW 1
grep -q "wipes the Echo" $Q/warned && [ "$(cat $STATE)" = wipe ] && grep -q "unlock, wipe and reflash" $Q/menu; ok_ $? "from scratch: warned, then marked"
rooted 1 no FW 1
[ ! -s $STATE ]; ok_ $? "from scratch, not confirmed: nothing marked"
rooted 0 x FW 1
grep -q rooted $STATE && ! grep -q wipe $STATE && [ ! -s $Q/warned ]; ok_ $? "kept as it is: marked rooted, no wipe"
rooted 0 yes other 1
grep -q "reflashes and wipes it" $Q/warned; ok_ $? "other firmware: says it is wiped"
rooted 1 yes FW 0
grep -q "nothing is wiped" $Q/menu && ! grep -q "reflash\|firmware" $Q/menu; ok_ $? "no firmware step (athena): the menu promises no wipe"
rooted 0 yes other 0
grep -q "no step here changes its firmware" $Q/warned; ok_ $? "no firmware step (athena): the note says so"
exit $fail
