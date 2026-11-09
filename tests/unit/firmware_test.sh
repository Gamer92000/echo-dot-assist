#!/bin/bash
# Which firmware hassmic goes onto (scripts/lib/device.sh firmware_same, firmware_ok, device_check_firmware) and the
# guided setup's question (scripts/lib/setup.sh firmware_accept), against a stand-in adb that answers the real
# scripts/probe.sh with athena's probe.md5, one file changed or none at all.  PR #98: a build of a new line (athena's
# PS75751.7806N, two libraries rebuilt) asks instead of stopping; one with the same libraries goes on where nothing
# build-bound is flashed; the question never waits where no one sees it (setup's task: stdin /dev/null, output in a log).
cd "$(dirname "$0")/../.."; ROOT=$PWD; Q=$(mktemp -d); trap 'rm -rf "$Q"' EXIT; fail=0
ok_() { if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; fail=1; fi; }

# probe.sh writes device-logs/ beside scripts/: a tree of links, so that lands in $Q
mkdir -p $Q/r $Q/bin; ln -s $ROOT/scripts $ROOT/devices $ROOT/keys $Q/r/; cd $Q/r
cat > $Q/bin/adb <<'EOF'
#!/bin/sh
case "$*" in
get-state) echo "$FAKE_STATE";;
"shell getprop ro.product.device") echo athena;;
"shell getprop ro.build.display.id") printf '%s\r\n' "$FAKE_FW";;
push*) echo push >> "$FAKE_CALLS";;
"shell sh /data/local/tmp/probe.sh"*) echo "######## libs"; cat "$FAKE_MD5";;
esac
EOF
chmod +x $Q/bin/adb; export PATH=$Q/bin:$PATH FAKE_CALLS=$Q/calls FAKE_MD5=$Q/md5 FAKE_STATE=device FAKE_FW=
sed 's/^3d687c25/00000000/' devices/athena/probe.md5 > $Q/md5.differ    # libpryon.so rebuilt, as on 7806N
probe() { case $1 in same) cp devices/athena/probe.md5 $Q/md5;; differ) cp $Q/md5.differ $Q/md5;; esac; }

. scripts/lib/device.sh; DEVICE=athena device_load
unset HASSMIC_FIRMWARE_OK

# --- firmware_same: the real probe.sh's verdict
probe same; firmware_same; r=$?; [ $r = 0 ] && [ -z "$PROBE_DIFF" ]; ok_ $? "the same libraries: same"
probe differ; firmware_same; r=$?
[ $r = 1 ] && [ "$(printf '%s\n' "$PROBE_DIFF" | wc -l)" = 1 ] && [[ $PROBE_DIFF == *libpryon.so* ]]
ok_ $? "one library rebuilt: different, named"
FAKE_STATE=offline firmware_same; r=$?; [ $r = 2 ] && [ -z "$PROBE_DIFF" ]; ok_ $? "no Echo on adb: not known (2)"

# --- device_check_firmware: in a subshell each (die exits); stdin /dev/null as under setup's task, cut off after 10 s
check() {     # NAME WANT-EXIT WANT-TEXT FIRMWARE LIBRARIES INSTALL [ARG [STDIN]]
    local name=$1 want=$2 text=$3 r
    FAKE_FW=$4; probe $5; : > $Q/calls
    INSTALL=$6 timeout 10 bash -c '. scripts/lib/device.sh; DEVICE=athena device_load; INSTALL=$1; device_check_firmware $2' \
        _ "$6" "$7" < "${8:-/dev/null}" > $Q/out 2>&1; r=$?
    [ $r = "$want" ] && { [ -z "$text" ] || grep -q -- "$text" $Q/out; }; ok_ $? "$name"
}
check "the pinned build: no probe" 0 "" PS75741.7080N differ running
[ ! -s $Q/calls ]; ok_ $? "the pinned build: the probe not run"
check "another build, the same libraries, installed from the running OS: goes on" 0 "same probed stock files" PS75741.7082N same running
check "another build, the same libraries, twrp-ab: goes on" 0 "same probed stock files" X same twrp-ab
check "another build, the same libraries, a boot image: stops" 1 "reflash that one" X same boot
[ ! -s $Q/calls ]; ok_ $? "a boot image: the probe not run"
check "libraries differ, twrp-ab: stops" 1 "reflash that one" X differ twrp-ab
check "libraries differ, running, no terminal: stops, never waits" 1 "no terminal to ask on" PS75751.7806N differ running
grep -q "#   DIFFERENT: /system/lib/libpryon.so" $Q/out; ok_ $? "the warning names what differs"
HASSMIC_FIRMWARE_OK=1 check "libraries differ, running, HASSMIC_FIRMWARE_OK=1: goes on" 0 "WARNING" PS75751.7806N differ running
check "libraries differ, warn only (deploy.sh): goes on" 0 "^warning: this Echo runs" X differ running warn
FAKE_STATE=offline
check "no probe, running, no terminal: stops" 1 "probe.sh did not run" X differ running
FAKE_STATE=device
if command -v script > /dev/null; then      # the question on a terminal: script(1) gives the shell a pty for stdin
    ask() {   # ANSWER WANT-EXIT
        FAKE_FW=X; probe differ
        printf '%s\n' "$1" | timeout 10 script -qec \
            "bash -c '. scripts/lib/device.sh; DEVICE=athena device_load; INSTALL=running; device_check_firmware'" \
            /dev/null > $Q/out 2>&1
        [ $? = "$2" ] && grep -q "Install anyway" $Q/out; ok_ $? "a terminal, '$1': exit $2"
    }
    ask y 0; ask n 1
    # the end of input (Ctrl-D) under install-system.sh's set -e: the cancel's words, not a silent exit
    probe differ; printf '' | timeout 10 script -qec \
        "bash -c 'set -e; . scripts/lib/device.sh; DEVICE=athena device_load; INSTALL=running; device_check_firmware'" \
        /dev/null > $Q/out 2>&1
    [ $? = 1 ] && grep -q "cancelled" $Q/out; ok_ $? "a terminal, the end of input under set -e: cancelled, said so"
else echo "skip a terminal's answer: no script(1)"; fi

# --- firmware_accept: the guided setup asks itself, in the foreground, and hands the answer to install-system.sh
. scripts/lib/setup.sh > /dev/null 2>&1
prompt() { echo asked >> $Q/asked; printf -v "$1" %s "$TYPED"; }
accept() {    # NAME WANT-EXIT WANT-ASKED WANT-OK FIRMWARE LIBRARIES INSTALL TYPED
    local r asked=0
    : > $Q/asked; FAKE_FW=$5; probe $6; INSTALL=$7 TYPED=$8; unset HASSMIC_FIRMWARE_OK
    firmware_accept > $Q/out 2>&1; r=$?
    [ -s $Q/asked ] && asked=1
    [ "$r/$asked/${HASSMIC_FIRMWARE_OK:-}" = "$2/$3/$4" ]; ok_ $? "setup: $1"
}
DRY=
accept "libraries differ, yes: asked, passed on" 0 1 1 PS75751.7806N differ running yes
grep -q "DIFFERENT: /system/lib/libpryon.so" $Q/out; ok_ $? "setup: the question names what differs"
env | grep -q '^HASSMIC_FIRMWARE_OK=1$'; ok_ $? "setup: the answer exported for install-system.sh"
accept "libraries differ, no: stops" 1 1 "" PS75751.7806N differ running no
accept "the same libraries: not asked" 0 0 "" PS75741.7082N same running yes
accept "the pinned build: not asked" 0 0 "" PS75741.7080N differ running yes
accept "twrp-ab: not asked (its step_build stops on other libraries)" 0 0 "" X differ twrp-ab no
: > $Q/asked; FAKE_FW=X; probe differ; INSTALL=running; HASSMIC_FIRMWARE_OK=1; : > $Q/calls
firmware_accept > /dev/null 2>&1 && [ ! -s $Q/asked ] && [ ! -s $Q/calls ]; ok_ $? "setup: answered before in this run: neither asked nor probed"
unset HASSMIC_FIRMWARE_OK
DRY=1 accept "a dry run: not asked" 0 0 "" X differ running no
exit $fail
