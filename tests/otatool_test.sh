#!/bin/sh
# The PC's otatool (scripts/otatool.py) against the Echo's (src/tools/otatool.c, as build/otatool-host): the Echo only
# installs what its C copy verifies, so the two must agree on keys, signatures and the bundle format.  EdDSA signatures
# are deterministic: the same key and bundle give the same bytes from both.
cd "$(dirname "$0")/.."; T=$(mktemp -d); C=build/otatool-host; PY="python3 scripts/otatool.py"; fail=0
ok() { if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; fail=1; fi; }
make -s $C || exit 1
printf 'one\n' > $T/a.txt; head -c 300000 /dev/urandom > $T/blob; cp $C $T/tool

$C keygen $T/c.sec $T/c.pub; $PY keygen $T/p.sec $T/p.pub
[ "$(stat -c %a $T/p.sec)" = 600 ] && [ "$(stat -c %s $T/p.sec)" = 64 ] && [ "$(stat -c %s $T/p.pub)" = 32 ]; ok $? "keygen: 64-byte secret (0600), 32-byte public"
$C pack $T/c.sec 1.2.3+x $T/c.bundle $T/tool $T/a.txt:644 $T/blob:600 >/dev/null
$PY pack $T/c.sec 1.2.3+x $T/p.bundle $T/tool $T/a.txt:644 $T/blob:600 >/dev/null
cmp -s $T/c.bundle $T/p.bundle && cmp -s $T/c.bundle.sig $T/p.bundle.sig; ok $? "pack with C's key: bundle and signature byte for byte the same"
$PY pack $T/p.sec 4.5.6 $T/pp.bundle $T/tool $T/a.txt:644 >/dev/null
[ "$($C verify $T/p.pub $T/pp.bundle $T/pp.bundle.sig)" = 4.5.6 ]; ok $? "C verifies Python's key and signature"
[ "$($PY verify $T/c.pub $T/c.bundle $T/c.bundle.sig)" = 1.2.3+x ]; ok $? "Python verifies C's"
$C install $T/c.pub $T/p.bundle $T/p.bundle.sig $T/by-c >/dev/null && $PY install $T/c.pub $T/c.bundle $T/c.bundle.sig $T/by-py >/dev/null &&
    diff -r $T/by-c $T/by-py && [ "$(stat -c %a $T/by-py/tool $T/by-py/a.txt $T/by-py/blob $T/by-py/VERSION | tr '\n' ' ')" = "755 644 600 644 " ] &&
    [ "$(stat -c %a $T/by-c/blob)" = 600 ] && cmp -s $T/blob $T/by-py/blob; ok $? "install: the same files, contents and modes from both"
! $PY verify $T/p.pub $T/c.bundle $T/c.bundle.sig 2>/dev/null; ok $? "Python refuses another key's signature"
cp $T/c.bundle $T/bad.bundle; printf X | dd of=$T/bad.bundle bs=1 seek=40 conv=notrunc 2>/dev/null
! $PY verify $T/c.pub $T/bad.bundle $T/c.bundle.sig 2>/dev/null && ! $C verify $T/c.pub $T/bad.bundle $T/c.bundle.sig 2>/dev/null; ok $? "both refuse a tampered bundle"
! $PY install $T/c.pub $T/c.bundle $T/c.bundle.sig $T/by-py 2>/dev/null; ok $? "install refuses a DESTDIR that exists"
! $PY pack $T/c.sec 1 $T/x.bundle "$T/.hidden" 2>/dev/null; ok $? "pack refuses a name the Echo would refuse"
# a file whose first bytes are whitespace (the random blob above starts with one in 2 % of runs)
printf '\n \tws\n' > $T/ws; $PY pack $T/c.sec 1 $T/ws.bundle $T/ws:644 $T/a.txt:644 >/dev/null
$C install $T/c.pub $T/ws.bundle $T/ws.bundle.sig $T/ws-c >/dev/null && cmp -s $T/ws $T/ws-c/ws && cmp -s $T/a.txt $T/ws-c/a.txt
ok $? "install: a file starting with whitespace"
rm -rf $T
[ $fail = 0 ] && echo "all good" || { echo FAILED; exit 1; }
