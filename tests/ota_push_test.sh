#!/bin/sh
# Push update path on the PC: real hassmic-host receiver, real otatool, and this script in the role of the root-side
# installer loop of main.sh (same commands, temp directories).
cd "$(dirname "$0")/.."; T=$(mktemp -d); O=build/otatool-host; fail=0
ok() { if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; fail=1; fi; }
$O keygen $T/k.sec $T/k.pub; $O keygen $T/evil.sec $T/evil.pub
mkdir $T/state
HASSMIC_STATE=$T/state HASSMIC_UPDATE_PUB=$T/k.pub HASSMIC_SETTINGS=$T/settings build/hassmic-host -p 16971 -z 0 -o 16972 -L 2>$T/log &
PID=$!; sleep 0.7
installer() {        # what ota_watch in main.sh does, once
    for i in $(seq 1 40); do [ -f $T/state/ota/request ] && break; sleep 0.25; done
    [ -f $T/state/ota/request ] || return
    rm -f $T/state/ota/request
    if v=$($O install $T/k.pub $T/state/ota/bundle $T/state/ota/bundle.sig $T/installed 2>&1); then echo "OK $v" > $T/state/ota/result; else echo "FAILED $v" > $T/state/ota/result; fi
}
printf '#!/bin/sh\necho main\n' > $T/main.sh; cp build/hassmic-host $T/hassmic
$O pack $T/k.sec 9.9.9+test $T/good.bundle $T/main.sh $T/hassmic >/dev/null
installer & r=$($O push 127.0.0.1 16972 $T/good.bundle $T/good.bundle.sig); wait $!
[ "$r" = "OK 9.9.9+test" ] && [ -x $T/installed/hassmic ] && [ "$(cat $T/installed/VERSION)" = 9.9.9+test ]; ok $? "signed bundle is received, installed and reported: $r"
$O pack $T/evil.sec 6.6.6 $T/evil.bundle $T/main.sh >/dev/null
r=$($O push 127.0.0.1 16972 $T/evil.bundle $T/evil.bundle.sig); echo "$r" | grep -q "^FAILED signature" && [ ! -f $T/state/ota/request ]; ok $? "bundle from another key is refused before it reaches the installer: $r"
cp $T/good.bundle $T/bad.bundle; printf X | dd of=$T/bad.bundle bs=1 seek=30 conv=notrunc 2>/dev/null
r=$($O push 127.0.0.1 16972 $T/bad.bundle $T/good.bundle.sig); echo "$r" | grep -q "^FAILED signature"; ok $? "tampered bundle is refused: $r"
r=$(printf 'GET / HTTP/1.0\r\n\r\n' | timeout 3 nc -q1 127.0.0.1 16972 2>/dev/null || true); echo "$r" | grep -q "^FAILED bad request"; ok $? "garbage on the port is refused: $r"
kill $PID; rm -rf $T
[ $fail = 0 ] && echo "all good" || { echo FAILED; exit 1; }
