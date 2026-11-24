#!/bin/bash
# The guided setup's fixed address (issue #191): the checks against the settings page's own cases (web/app.js
# addrProblem, wifi.c addr_check, wifi.sh fixed_ok refuse the same), the mask in its three spellings, and fixed_set
# with stand-ins for adb and curl: the request root takes, this PC asking the new address while root waits, the
# outcome said.  Nothing here needs an Echo.
cd "$(dirname "$0")/../.."; Q=$(mktemp -d); trap 'rm -rf "$Q"' EXIT; fail=0
ok_() { if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; fail=1; fi; }
. scripts/lib/setup.sh > /dev/null 2>&1
LOG=/dev/null OUT=$Q DRY= DAEMON_USER=puffin TTY= FIXED_WAIT=2
sleep() { :; }

# --- the checks: nothing for a good one, the page's words for the rest
good() { [ -z "$(addr_problem "$@")" ]; ok_ $? "accepted: $*"; }
bad() { local w=$1; shift; [[ "$(addr_problem "$@")" == *"$w"* ]]; ok_ $? "refused ($w): $*"; }
good 192.168.1.50 255.255.255.0 192.168.1.1 192.168.1.1
good 192.168.103.250 255.255.252.0 192.168.100.1 192.168.100.1 8.8.8.8
good 10.0.0.7 /8 10.0.0.1 10.0.0.1
bad "router is not in" 192.168.1.50 24 10.0.0.1 192.168.1.1
bad "network's own" 192.168.1.0 24 192.168.1.1 192.168.1.1
bad "network's own" 192.168.1.255 24 192.168.1.1 192.168.1.1
bad "subnet mask" 192.168.1.50 31 192.168.1.1 192.168.1.1
bad "subnet mask" 192.168.1.50 255.0.255.0 192.168.1.1 192.168.1.1
bad "not one a device" 192.168.01.50 24 192.168.1.1 192.168.1.1
bad "not one a device" 127.0.0.5 8 127.0.0.1 192.168.1.1
bad "not one a device" 192.168.1.256 24 192.168.1.1 192.168.1.1
bad "router's own" 192.168.1.1 24 192.168.1.1 192.168.1.1
bad "DNS" 192.168.1.50 24 192.168.1.1 224.0.0.1
bad "DNS" 192.168.1.50 24 192.168.1.1 192.168.1.1 0.0.0.0
bad "DNS" 192.168.1.50 24 192.168.1.1 192.168.1.1 nope
[ "$(prefix_of 255.255.252.0) $(prefix_of /22) $(prefix_of 22) $(mask_dotted 22)" = "22 22 22 255.255.252.0" ]; ok_ $? "the mask as 255.255.252.0, /22 or 22"
[ "$(prefix_of 08) $(prefix_of /09)" = "8 9" ] && ! prefix_of 07 > /dev/null; ok_ $? "a prefix with a leading zero: base 10, said without it (bash's octal)"
good 10.0.0.7 08 10.0.0.1 10.0.0.1

# --- fixed_set: root's answers in turn from a file, the commands and curl's asks written down
curl() { echo "curl $*" >> $Q/calls; }
run() {      # FIXED, then root's answers to the setup's looks ("ID" stands for the request's id)
    FIXED=$1; shift; : > $Q/calls; echo 0 > $Q/n; printf '%s\n' "$@" > $Q/answers
    fixed_set > $Q/out 2>&1; RC=$?
    ID=$(sed -n "s/.*echo 'address \([0-9]*\) fixed.*/\1/p" $Q/calls | head -1)
}
# root's answers name the request's id: fixed_set makes it, so the stand-in fills it in as it answers
adb() {
    echo "adb $*" >> $Q/calls
    case "$*" in
    *address-result*) local n id; n=$(cat $Q/n); echo $((n + 1)) > $Q/n
        id=$(sed -n "s/.*echo 'address \([0-9]*\) fixed.*/\1/p" $Q/calls | head -1)
        sed -n "$((n + 1))p" $Q/answers | sed "s/ID/$id/";;
    esac
}
run "192.168.1.50 24 192.168.1.1 192.168.1.1 -" "" "ID applying 192.168.1.50" "ID waiting 192.168.1.50" "ID waiting 192.168.1.50" "ID ok 192.168.1.50"
grep -q "echo 'address $ID fixed 192.168.1.50 24 192.168.1.1 192.168.1.1 -' > /data/local/hassmic/state/wifi-request" $Q/calls &&
    grep -q "chown puffin /data/local/hassmic/state/wifi-request && chmod 600" $Q/calls; ok_ $? "the request for root, hassmic's own file as the page's"
[ "$(grep -c 'curl .*http://192.168.1.50:28931/api/hello' $Q/calls)" = 2 ]; ok_ $? "this PC asks the new address while root waits (twice), not before"
[ $RC = 0 ] && grep -q "Fixed address: 192.168.1.50" $Q/out; ok_ $? "kept: said so"
run "192.168.1.60 24 192.168.1.1 192.168.1.1 -" "ID waiting 192.168.1.60" "ID failed unreached 192.168.1.20"
grep -q "Nothing reached the Echo at 192.168.1.60" $Q/out && grep -q "Network address" $Q/out; ok_ $? "not reached: said so, and where to try again"
run "192.168.1.99 24 192.168.1.1 192.168.1.1 -" "ID failed inuse 192.168.1.20"
grep -q "Another device answers at 192.168.1.99" $Q/out && ! grep -q curl $Q/calls; ok_ $? "taken by another device: said so, nothing asked of the address"
run "" ; [ $RC = 0 ] && [ ! -s $Q/calls ]; ok_ $? "none asked for: nothing done"
ANDROID_SERIAL=192.168.1.20:5555 run "192.168.1.50 24 192.168.1.1 192.168.1.1 -" "ID ok 192.168.1.50"
[ $RC = 0 ] && [ ! -s $Q/calls ] && grep -q "settings page" $Q/out; ok_ $? "over adb on Wi-Fi: nothing asked, the settings page named"
run "192.168.1.50 24 192.168.1.1 192.168.1.1 -" "ID waiting 192.168.1.50"
[ $RC = 0 ] && grep -q "No word from the Echo" $Q/out; ok_ $? "no answer in time: not called a failure, the page named"
run "192.168.1.50 24 192.168.1.1 192.168.1.1 -" "987 ok 192.168.1.50" "ID ok 192.168.1.50"
[ $RC = 0 ] && [ "$(cat $Q/n)" = 2 ]; ok_ $? "an answer to another request is not this one's"
exit $fail
