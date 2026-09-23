#!/bin/sh
# SBC decoder against BlueZ's libsbc: every mode sbcenc offers, on a synthetic stereo signal (sweeps, noise, clipping).
set -e
d=build/sbc; mkdir -p $d
python3 - $d/in.au <<'PY'
import math, random, struct, sys
r = 44100; n = r * 3; random.seed(1); out = bytearray(struct.pack('>6I', 0x2e736e64, 24, 0xffffffff, 3, r, 2))
for i in range(n):
    t = i / r
    l = 20000 * math.sin(2 * math.pi * (100 + 4000 * t) * t) + random.uniform(-3000, 3000)
    rr = 14000 * math.sin(2 * math.pi * 440 * t) * (1 if t < 2 else 3) + 6000 * math.sin(2 * math.pi * 9000 * t)
    out += struct.pack('>hh', *(max(-32768, min(32767, int(v))) for v in (l, rr)))
open(sys.argv[1], 'wb').write(out)
open(sys.argv[1].replace('in', 'mono'), 'wb').write(struct.pack('>6I', 0x2e736e64, 24, 0xffffffff, 3, 48000, 1) + b''.join(out[24 + 4 * i:26 + 4 * i] for i in range(n)))
PY
cc -O2 -Wall -Isrc/hassmic tests/unit/sbc_test.c src/hassmic/sbc.c -lm -o build/sbc_test
fail=0
for opt in "-s 8 -b 53 -j" "-s 8 -b 35" "-s 4 -b 20 -j -S" "-s 8 -b 31 -d -B 4" "-s 4 -b 12 -B 8" "-s 8 -b 2 -j" "-s 8 -b 250 -B 16"; do
    f=$d/$(echo "$opt" | tr -d ' -').sbc
    sbcenc $opt $d/in.au > $f 2>/dev/null
    sbcdec -f $f.au $f >/dev/null 2>&1
    build/sbc_test $f $f.au || fail=1
done
sbcenc -s 8 -b 31 $d/mono.au > $d/mono.sbc 2>/dev/null; sbcdec -f $d/mono.sbc.au $d/mono.sbc >/dev/null 2>&1
build/sbc_test $d/mono.sbc $d/mono.sbc.au || fail=1
exit $fail
