#!/bin/sh
# Push update path on the PC: real hassmic-host receiver, the PC's otatool (scripts/otatool.py) as ota-push.sh and
# adb-wifi.sh use it, and this script in the role of the root-side installer loop of main.sh (same commands, temp
# directories, the Echo's otatool: build/otatool-host).
cd "$(dirname "$0")/.."; T=$(mktemp -d); O="python3 scripts/otatool.py"; E=build/otatool-host; fail=0
ok() { if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; fail=1; fi; }
$O keygen $T/k.sec $T/k.pub; $O keygen $T/evil.sec $T/evil.pub
mkdir $T/state
# (root's minute for an answer 5 s here, HASSMIC_OTA_ANSWER_S)
HASSMIC_STATE=$T/state HASSMIC_ADB_OPEN=$T/adb-open HASSMIC_UPDATE_PUB=$T/k.pub HASSMIC_SETTINGS=$T/settings HASSMIC_OTA_ANSWER_S=5 build/hassmic-host -p 16971 -z 0 -o 16972 -L 2>$T/log &
PID=$!; for i in $(seq 1 200); do nc -z 127.0.0.1 16972 && break; sleep 0.1; done      # bound in ~0.3 s alone, later with every test at once
# Self test: a second of audio through the capture loop (the PC build reads a file), then the marker for root's
# installer loop, which makes the running update the factory copy (main.sh).  Before the push below: once a bundle is
# with root's installer and root said OK, this hassmic is about to be replaced, and its self test says nothing (U1)
for i in $(seq 1 40); do [ -f $T/state/ota/healthy ] && break; sleep 0.25; done
[ -f $T/state/ota/healthy ] && grep -q "self test: passed" $T/log; ok $? "self test passed, installer told (state/ota/healthy)"
v=$(make -s version)
[ "$(cat $T/state/ota/healthy)" = "$v" ]; ok $? "it names the build that passed ($(cat $T/state/ota/healthy), the bundle's VERSION of this build: $v)"
# Root's side, main.sh healthy() cut out, factory() stood in for: only the update installed now is promoted, and only
# by its own word.  A "1" (a build's before) or another build's version is dropped: a build held by its wake word
# plugin passed late in the seconds between root's install of the next and its end, and root made that untested
# next one the fallback (a review of 1d704e9, U1)
R=$T/root; mkdir -p $R/in $R/ota/vnew $R/sys; printf '%s\n' "$v" > $R/ota/vnew/VERSION; echo 2001.01.01.000000 > $R/sys/VERSION
ln -s $R/ota/vnew $R/ota/current
sed -n '/^healthy() {/,/^}/p' scripts/system/main.sh > $R/healthy.sh
root_look() {        # $1: what healthy holds (a file to copy, or text), $2 how it is there (file, hardlink, symlink)
    rm -f $R/promoted $R/said $R/in/healthy $R/elsewhere
    if [ -f "$1" ]; then cp "$1" $R/elsewhere; else printf '%s\n' "$1" > $R/elsewhere; fi
    case $2 in hardlink) ln $R/elsewhere $R/in/healthy;; symlink) ln -s $R/elsewhere $R/in/healthy;; *) mv $R/elsewhere $R/in/healthy;; esac
    ( IN=$R/in OTA=$R/ota SYS=$R/sys TAKEN=$R/taken
      say() { echo "$*" >> $R/said; }; factory() { echo "$1" > $R/promoted; echo "OK"; }
      . $R/healthy.sh; healthy )
    [ ! -e $R/in/healthy ] && [ ! -L $R/in/healthy ] && [ ! -e $R/taken.healthy ]     # taken, whatever it said
}
root_look $T/state/ota/healthy && [ "$(cat $R/promoted 2>/dev/null)" = "$v" ]; ok $? "root promotes the installed update on its own word"
root_look 1 && [ ! -f $R/promoted ] && grep -q "another build's" $R/said; ok $? "a \"1\" of a build before names no build: dropped, nothing promoted"
root_look 2001.01.02.000000+other && [ ! -f $R/promoted ]; ok $? "another build's version (the one before, passing late): dropped, nothing promoted"
root_look "$v" hardlink && [ ! -f $R/promoted ] && [ -f $R/elsewhere ]; ok $? "a file with another name too (a hard link): not read, nothing promoted"
root_look "$v" symlink && [ ! -f $R/promoted ] && [ -f $R/elsewhere ]; ok $? "a link: not followed, nothing promoted"
# and before every install: one left by the build that runs (it passed before the bundle came) is that build's, the
# update installed until then; taken after the install, it named another build, and before U1's version it promoted
# the update installed in its place.  (ota_watch needs the Echo; its text is read here)
sed -n '/^ota_watch() {/,/^}/p' scripts/system/main.sh > $R/watch
req=$(grep -n 'IN/request \] || continue' $R/watch | cut -d: -f1); inst=$(grep -n 'otatool install' $R/watch | cut -d: -f1)
set -- $(grep -n '^ *\((cd /; \)\?healthy\( \|$\|)\)' $R/watch | cut -d: -f1)
[ $# = 2 ] && [ "$1" -lt "$req" ] && [ "$2" -gt "$req" ] && [ "$2" -lt "$inst" ]; ok $? "ota_watch takes healthy at every look and, a request there, before it installs (lines $*; request $req, install $inst)"
# the install's call runs in the daemon's folder (cd -P $IN): factory() and root's sysinstall.sh from / all the same,
# as at the top of the look (a review of 0119caf)
line=$(sed -n "${2:-0}p" $R/watch); rm -f $R/cwd; printf '%s\n' "$v" > $R/in/healthy
( cd $R/in; IN=$R/in OTA=$R/ota SYS=$R/sys TAKEN=$R/taken
  say() { :; }; factory() { pwd > $R/cwd; echo "OK"; }
  . $R/healthy.sh; eval "$line" )
[ "$(cat $R/cwd 2>/dev/null)" = / ]; ok $? "the install's healthy runs factory() from /, not from hassmic's folder ($(cat $R/cwd 2>/dev/null); $(echo $line))"
cp $R/ota/vnew/VERSION $R/sys/VERSION
root_look "$v" && [ ! -f $R/promoted ] && [ ! -f $R/said ]; ok $? "the installed update the factory copy already: nothing to do, nothing said"
# The request's nonce, root's side (main.sh ota_nonce, cut out as healthy() above): echoed under root's answer, so that
# hassmic takes only the answer to its own request (a review of 9bbfd66, W3).  Read in hassmic's folder, which may
# hold anything by that name: only a plain file with one name, only 32 hex digits, else "-"
sed -n '/^ota_nonce() {/,/^}/p' scripts/system/main.sh > $R/nonce.sh
nonce_of() {         # $1: what request holds (text), $2 how it is there (file, hardlink, symlink, fifo, dir); prints ota_nonce's word
    mkdir -p $R/ota_in; rm -rf $R/ota_in/request $R/elsewhere $R/taken.request
    case $2 in fifo) mkfifo $R/ota_in/request;; dir) mkdir $R/ota_in/request;;
        *) printf '%s\n' "$1" > $R/elsewhere
           case $2 in hardlink) ln $R/elsewhere $R/ota_in/request;; symlink) ln -s $R/elsewhere $R/ota_in/request;; *) mv $R/elsewhere $R/ota_in/request;; esac;; esac
    ( cd $R/ota_in && timeout 5 sh -c ". $R/nonce.sh; TAKEN=$R/taken; ota_nonce" )
}
h=0123456789abcdef0123456789abcdef
[ "$(nonce_of "1 $h")" = $h ] && [ ! -e $R/ota_in/request ] && [ ! -e $R/taken.request ]; ok $? "root reads the request's nonce, the request taken"
[ "$(nonce_of 1)" = - ]; ok $? "a request without one (a hassmic before the nonce): \"-\""
[ "$(nonce_of "1 ${h}0")" = - ] && [ "$(nonce_of "1 ${h%?}")" = - ] && [ "$(nonce_of "1 $(echo $h | tr a-f A-F)")" = - ]; ok $? "only 32 lower-case hex digits are a nonce"
[ "$(nonce_of "1 $h" hardlink)" = - ] && [ -f $R/elsewhere ]; ok $? "a file with another name too (a hard link): not read"
[ "$(nonce_of "1 $h" symlink)" = - ] && [ -f $R/elsewhere ]; ok $? "a link: not followed"
[ "$(nonce_of x fifo)" = - ] && [ "$(nonce_of x dir)" = - ] && [ ! -e $R/ota_in/request ]; ok $? "a FIFO (that would hold the loop) or a folder: \"-\", gone"
w=$(sed -n '/^ota_watch() {/,/^}/p' scripts/system/main.sh)
echo "$w" | grep -q 'nonce=$(ota_nonce)' && echo "$w" | grep -q 'echo "nonce $nonce" >> $RES'; ok $? "ota_watch echoes it under every answer"
mkdir -p $R/ota_in; rm -rf $R/ota_in/request; printf '1 %s\n' $h > $R/ota_in/request
[ "$(cd $R/ota_in && sh -c ". $R/nonce.sh; TAKEN=$R/nowhere/taken; ota_nonce")" = - ] && [ ! -e $R/ota_in/request ]; ok $? "a request that cannot be taken: \"-\", deleted where it is"
# The request is taken by ota_nonce alone: one hassmic writes after it (the next handoff, once it stopped waiting for
# this one) stays for the next look and its own answer.  ota_watch deleted it with the old answer, and the next push
# heard nothing, its late reader asking for good (a review of 06683b6, X2).  ota_watch's lines from the nonce to the
# bundle's move, run with root's ota_nonce and hassmic's request right behind it
from=$(echo "$w" | grep -n 'nonce=$(ota_nonce)' | cut -d: -f1); to=$(echo "$w" | grep -n 'rm -rf $INB; mkdir' | cut -d: -f1)
h2=fedcba9876543210fedcba9876543210; rm -rf $R/gap $R/gap.nonce; mkdir $R/gap; printf '1 %s\n' $h > $R/gap/request; echo old > $R/gap/result
( cd $R/gap; TAKEN=$R/taken
  eval "$(sed 's/^ota_nonce()/root_nonce()/' $R/nonce.sh)"
  ota_nonce() { root_nonce; printf '1 %s\n' $h2 > request; }
  eval "$(echo "$w" | sed -n "${from},$((to - 1))p")"; echo "$nonce" > $R/gap.nonce )
[ "$(cat $R/gap.nonce)" = $h ] && [ "$(cat $R/gap/request 2>/dev/null)" = "1 $h2" ] && [ ! -e $R/gap/result ]
ok $? "a request written between root's taking the one before and its deleting the old answer stays for the next look ($(echo "$w" | sed -n "${from},$((to - 1))p" | tr -s ' ' | tr '\n' ';'))"
installer() {        # what ota_watch in main.sh does, once ($1 legacy: an older main.sh's answer, no nonce under it;
                     # stale: a root's answer to an earlier request, its nonce another, written first; long: an answer
                     # longer than hassmic reads of it)
    for i in $(seq 1 40); do [ -f $T/state/ota/request ] && break; sleep 0.25; done
    [ -f $T/state/ota/request ] || return
    n=$(cd $T/state/ota && TAKEN=$T/taken && . $R/nonce.sh && ota_nonce)
    rm -f $T/state/ota/request
    if [ "$1" = stale ]; then printf 'FAILED an earlier request'"'"'s answer\nnonce %s\n' $h > $T/state/ota/result; sleep 1.5; fi
    if v=$($E install $T/k.pub $T/state/ota/bundle $T/state/ota/bundle.sig $T/installed 2>&1); then a="OK $v"; else a="FAILED $v"; fi
    if [ "$1" = long ]; then a="FAILED $(printf '%0300d' 0 | tr 0 x)"; fi
    if [ "$1" = legacy ]; then echo "$a"; else printf '%s\nnonce %s\n' "$a" "$n"; fi > $T/state/ota/result.tmp
    mv $T/state/ota/result.tmp $T/state/ota/result
}
printf '#!/bin/sh\necho main\n' > $T/main.sh; cp build/hassmic-host $T/hassmic
$O pack $T/k.sec 9.9.9+test $T/good.bundle $T/main.sh $T/hassmic >/dev/null
installer & r=$($O push 127.0.0.1 16972 $T/good.bundle $T/good.bundle.sig); wait $!
[ "$r" = "OK 9.9.9+test" ] && [ -x $T/installed/hassmic ] && [ "$(cat $T/installed/VERSION)" = 9.9.9+test ]; ok $? "signed bundle is received, installed and reported: $r"
rm -rf $T/installed; installer & r=$($E push 127.0.0.1 16972 $T/good.bundle $T/good.bundle.sig); wait $!
[ "$r" = "OK 9.9.9+test" ] && [ -x $T/installed/hassmic ]; ok $? "the Echo's otatool (C) as the pusher: the same line: $r"
rm -rf $T/installed; installer stale & r=$($O push 127.0.0.1 16972 $T/good.bundle $T/good.bundle.sig); wait $!
[ "$r" = "OK 9.9.9+test" ] && grep -q "an answer of root's to an earlier handoff left alone" $T/log; ok $? "an answer with another request's nonce is not this push's: its own one comes after it: $r"
rm -rf $T/installed; installer legacy & r=$($O push 127.0.0.1 16972 $T/good.bundle $T/good.bundle.sig); wait $!
[ "$r" = "OK 9.9.9+test" ]; ok $? "an older main.sh's answer (no nonce under it: the factory copy's beside the update's hassmic) is taken: $r"
# a first line longer than hassmic reads: the rest of it is not the nonce's line (it was: "another handoff's", and the
# push heard nothing; a review of 06683b6, X3)
n0=$(wc -c < $T/log); rm -rf $T/installed; installer long & r=$($O push 127.0.0.1 16972 $T/good.bundle $T/good.bundle.sig); wait $!
[ "$r" != "${r#FAILED xxxxxxxxxx}" ] && ! tail -c +$((n0 + 1)) $T/log | grep -q "left alone"; ok $? "an answer whose first line is longer than hassmic reads is still this push's: $(echo "$r" | cut -c1-40)..."
# After the minute (ota_handed, read by ota.c's own thread): root took a push late and is slow to answer it, and the
# next push went unanswered too; root answers the first, then takes the next and is slow to delete the old answer
# (between ota_nonce and the rm): the late answer read as the next push's is its own
late_root() {
    for i in $(seq 1 40); do [ -f $T/state/ota/request ] && break; sleep 0.25; done
    a=$(cd $T/state/ota && TAKEN=$T/taken && . $R/nonce.sh && ota_nonce); rm -f $T/state/ota/request
    for i in $(seq 1 240); do [ -f $T/go ] && break; sleep 0.25; done
    printf 'FAILED the first push'"'"'s answer\nnonce %s\n' $a > $T/state/ota/result
    for i in $(seq 1 40); do [ -f $T/state/ota/request ] && break; sleep 0.25; done
    n=$(cd $T/state/ota && TAKEN=$T/taken && . $R/nonce.sh && ota_nonce); rm -f $T/state/ota/request
    sleep 3; rm -f $T/state/ota/result
    v=$($E install $T/k.pub $T/state/ota/bundle $T/state/ota/bundle.sig $T/installed 2>&1) && printf 'OK %s\nnonce %s\n' "$v" $n > $T/state/ota/result.tmp
    mv $T/state/ota/result.tmp $T/state/ota/result
}
rm -rf $T/installed $T/go; late_root & L=$!
r1=$($O push 127.0.0.1 16972 $T/good.bundle $T/good.bundle.sig); r2=$($O push 127.0.0.1 16972 $T/good.bundle $T/good.bundle.sig)
n0=$(wc -c < $T/log); : > $T/go; wait $L
for i in $(seq 1 40); do tail -c +$((n0 + 1)) $T/log | grep -q "answered late: OK" && break; sleep 0.25; done
late=$(tail -c +$((n0 + 1)) $T/log | grep "answered late")
printf "%s\n%s\n" "$r1" "$r2" | grep -c "did not answer" | grep -qx 2 && [ "$late" != "${late#*answered late: OK 9.9.9+test}" ] && ! echo "$late" | grep -q "answered late: FAILED"
ok $? "after the minute too, root's answer to the push before is not the next one's: $(echo $late)"
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
r=$(printf 'HMOTA-FACTORY1 9.9.9+test\n' | timeout 10 nc -N 127.0.0.1 16972 2>/dev/null || true); echo "$r" | grep -q "^FAILED bad request"; ok $? "no approval request any more: $r"
kill $PID
# An update key under which any signature holds (src/include/edkey.h: all zero, small order): the null signature
# (R the identity, s = 0) verifies under it for every bundle, and the Echo takes the key as none
head -c 32 /dev/zero > $T/zero.pub; printf '\001' > $T/null.sig; head -c 63 /dev/zero >> $T/null.sig; mkdir $T/state0
HASSMIC_STATE=$T/state0 HASSMIC_ADB_OPEN=$T/adb-open HASSMIC_UPDATE_PUB=$T/zero.pub HASSMIC_SETTINGS=$T/settings0 build/hassmic-host -p 16973 -z 0 -o 16974 -L 2>$T/log0 &
PID=$!; for i in $(seq 1 200); do nc -z 127.0.0.1 16974 && break; sleep 0.1; done
r=$($O push 127.0.0.1 16974 $T/good.bundle $T/null.sig); echo "$r" | grep -q "^FAILED this device has no update key" && [ ! -f $T/state0/ota/request ] && grep -q "any signature holds" $T/log0
ok $? "an update key of small order is none: the null signature's bundle refused: $r"
kill $PID; rm -rf $T
[ $fail = 0 ] && echo "all good" || { echo FAILED; exit 1; }
