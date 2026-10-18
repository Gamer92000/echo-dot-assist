#!/bin/sh
# Push update path on the PC: real hassmic-host receiver, the PC's otatool (scripts/otatool.py) as ota-push.sh and
# adb-wifi.sh use it, and this script in the role of the root-side installer loop of main.sh (same commands, temp
# directories, the Echo's otatool: build/otatool-host).
cd "$(dirname "$0")/.."; T=$(mktemp -d); O="python3 scripts/otatool.py"; E=build/otatool-host; fail=0
ok() { if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; fail=1; fi; }
$O keygen $T/k.sec $T/k.pub; $O keygen $T/evil.sec $T/evil.pub
mkdir $T/state
HASSMIC_STATE=$T/state HASSMIC_ADB_OPEN=$T/adb-open HASSMIC_UPDATE_PUB=$T/k.pub HASSMIC_SETTINGS=$T/settings build/hassmic-host -p 16971 -z 0 -o 16972 -L 2>$T/log &
PID=$!; for i in $(seq 1 200); do nc -z 127.0.0.1 16972 && break; sleep 0.1; done      # bound in ~0.3 s alone, later with every test at once
installer() {        # what ota_watch in main.sh does, once
    for i in $(seq 1 40); do [ -f $T/state/ota/request ] && break; sleep 0.25; done
    [ -f $T/state/ota/request ] || return
    rm -f $T/state/ota/request
    if v=$($E install $T/k.pub $T/state/ota/bundle $T/state/ota/bundle.sig $T/installed 2>&1); then echo "OK $v" > $T/state/ota/result; else echo "FAILED $v" > $T/state/ota/result; fi
}
printf '#!/bin/sh\necho main\n' > $T/main.sh; cp build/hassmic-host $T/hassmic
$O pack $T/k.sec 9.9.9+test $T/good.bundle $T/main.sh $T/hassmic >/dev/null
installer & r=$($O push 127.0.0.1 16972 $T/good.bundle $T/good.bundle.sig); wait $!
[ "$r" = "OK 9.9.9+test" ] && [ -x $T/installed/hassmic ] && [ "$(cat $T/installed/VERSION)" = 9.9.9+test ]; ok $? "signed bundle is received, installed and reported: $r"
$O pack $T/evil.sec 6.6.6 $T/evil.bundle $T/main.sh >/dev/null
r=$($O push 127.0.0.1 16972 $T/evil.bundle $T/evil.bundle.sig); echo "$r" | grep -q "^FAILED signature" && [ ! -f $T/state/ota/request ]; ok $? "bundle from another key is refused before it reaches the installer: $r"
cp $T/good.bundle $T/bad.bundle; printf X | dd of=$T/bad.bundle bs=1 seek=30 conv=notrunc 2>/dev/null
r=$($O push 127.0.0.1 16972 $T/bad.bundle $T/good.bundle.sig); echo "$r" | grep -q "^FAILED signature"; ok $? "tampered bundle is refused: $r"
# One line only: the Echo reads a line, answers and closes, and a close with bytes left unread sends a reset, which
# throws away the answer if nc has not read it yet (under load it had not).  nc -N quits once the Echo closed.
r=$(printf 'GET / HTTP/1.0\n' | timeout 10 nc -N 127.0.0.1 16972 2>/dev/null || true); echo "$r" | grep -q "^FAILED bad request"; ok $? "garbage on the port is refused: $r"

# adb over Wi-Fi with the update key.  This script is the firewall watcher: it takes the request and answers with adb-open.
watcher() {
    for i in $(seq 1 40); do [ -f $T/state/adb-request ] && break; sleep 0.25; done
    [ "$(cat $T/state/adb-request 2>/dev/null)" = 1 ] && : > $T/adb-open; rm -f $T/state/adb-request
}
watcher & r=$($O adb 127.0.0.1 16972 $T/k.sec); wait $!
[ "$r" = "OK adb over Wi-Fi open for 30 min" ] && [ -f $T/adb-open ]; ok $? "update key opens adb over Wi-Fi: $r"
rm -f $T/adb-open
r=$($O adb 127.0.0.1 16972 $T/evil.sec); echo "$r" | grep -q "^FAILED signature" && [ ! -f $T/state/adb-request ]; ok $? "another key does not, and asks for nothing: $r"
# a recorded answer does not work again: the challenge differs each time
n1=$(printf 'HMOTA-ADB1\n' | timeout 3 nc -q2 127.0.0.1 16972 | head -1); n2=$(printf 'HMOTA-ADB1\n' | timeout 3 nc -q2 127.0.0.1 16972 | head -1)
echo "$n1" | grep -qE '^NONCE [0-9a-f]{64}$' && [ "$n1" != "$n2" ]; ok $? "a new challenge every time"
# Self test: a second of audio through the capture loop (the PC build reads a file), then the marker for root's
# installer loop, which makes the running update the factory copy (main.sh)
for i in $(seq 1 40); do [ -f $T/state/ota/healthy ] && break; sleep 0.25; done
[ -f $T/state/ota/healthy ] && grep -q "self test: passed" $T/log; ok $? "self test passed, installer told (state/ota/healthy)"
r=$(printf 'HMOTA-FACTORY1 9.9.9+test\n' | timeout 10 nc -N 127.0.0.1 16972 2>/dev/null || true); echo "$r" | grep -q "^FAILED bad request"; ok $? "no approval request any more: $r"
kill $PID; rm -rf $T
[ $fail = 0 ] && echo "all good" || { echo FAILED; exit 1; }
