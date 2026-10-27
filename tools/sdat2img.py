#!/usr/bin/env python3
"""sdat2img: a block OTA's system.new.dat back into the ext4 image, in the standard library only.

  sdat2img.py system.transfer.list system.new.dat system.img

checkers' firmware (and other non-A/B Fire OS updates) carries the system partition as system.new.dat, the "new"
blocks of system.transfer.list (version 2 to 4) in their order.  The image is as large as the highest block any
command names; erase/zero ranges stay holes (zeros).  Brotli-packed .dat.br is not handled (none of ours is).
"""
import sys

BLOCK = 4096


def ranges(s):
    v = [int(x) for x in s.split(",")]
    assert v[0] == len(v) - 1 and v[0] % 2 == 0, "bad range set " + s
    return [(v[1 + 2 * i], v[2 + 2 * i]) for i in range(v[0] // 2)]


def main(tl, dat, out):
    lines = open(tl).read().split("\n")
    version = int(lines[0])
    assert 1 <= version <= 4, "transfer list version %d" % version
    cmds = [l.split(" ", 1) for l in lines[4 if version >= 2 else 2:] if l.strip()]
    end = max(e for c, a in cmds if c in ("new", "erase", "zero") for _, e in ranges(a))
    written = 0
    with open(out, "wb") as o, open(dat, "rb") as d:
        o.truncate(end * BLOCK)
        for c, a in cmds:
            if c != "new":
                assert c in ("erase", "zero"), "a full OTA has only new/erase/zero, not " + c
                continue
            for s, e in ranges(a):
                o.seek(s * BLOCK)
                n = (e - s) * BLOCK
                while n:
                    b = d.read(min(n, 1 << 24))
                    assert b, "system.new.dat ends early"
                    o.write(b)
                    n -= len(b)
                    written += len(b)
        assert not d.read(1), "system.new.dat is longer than the list says"
    print("%s: %d blocks, %d of them data" % (out, end, written // BLOCK))


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
