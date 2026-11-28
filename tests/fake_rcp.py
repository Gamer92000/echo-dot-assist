#!/usr/bin/env python3
"""A stand-in for Amazon's ot-rcp (an OpenThread RCP speaking Spinel in HDLC-lite on its standard input and output),
for tests/fake_radio.py behind build/rcpbridge-host: it answers PROP_VALUE_GET of PROTOCOL_VERSION and NCP_VERSION as
the Echo 4's does (measured 2026-11-27), "started <pid>" and "ended <pid>" into FAKE_RCP_LOG.  Also the codec the test
speaks: hdlc(), frames(), get()."""
import os, sys


def fcs(data):
    c = 0xffff
    for b in data:
        c ^= b
        for _ in range(8): c = (c >> 1) ^ 0x8408 if c & 1 else c >> 1
    return c ^ 0xffff


def hdlc(payload):
    f = fcs(payload); body = payload + bytes([f & 0xff, f >> 8]); out = bytearray([0x7e])
    for b in body:
        if b in (0x7e, 0x7d, 0x11, 0x13, 0xf8): out += bytes([0x7d, b ^ 0x20])
        else: out.append(b)
    return bytes(out + b"\x7e")


def frames(buf):
    """complete frames out of buf (a bytearray, consumed): their payloads, checked"""
    out = []
    while True:
        try: a = buf.index(0x7e); e = buf.index(0x7e, a + 1)
        except ValueError: return out
        raw = bytes(buf[a + 1:e]); del buf[:e]
        d = bytearray(); esc = False
        for x in raw:
            if esc: d.append(x ^ 0x20); esc = False
            elif x == 0x7d: esc = True
            else: d.append(x)
        if len(d) >= 3 and fcs(bytes(d[:-2])) == d[-2] | d[-1] << 8: out.append(bytes(d[:-2]))


NCP = b"OPENTHREAD/404cfe2ff ;GP712 ; Tue Oct 27 01:25:34 UTC 2026\x00"

def main():
    log = os.environ.get("FAKE_RCP_LOG")
    def say(s):
        if log:
            with open(log, "a") as f: f.write(f"{s} {os.getpid()}\n")
    import signal
    def term(*_):                                                   # rcpbridge ends it so: said once, not in the middle of saying it
        signal.signal(signal.SIGTERM, signal.SIG_IGN); raise SystemExit(0)
    signal.signal(signal.SIGTERM, term)
    say("started")
    buf = bytearray()
    try:
        while True:
            d = os.read(0, 4096)
            if not d: break
            buf += d
            for f in frames(buf):
                if len(f) >= 3 and f[1] == 2:                       # PROP_VALUE_GET
                    prop = f[2]
                    val = {1: b"\x04\x03", 2: NCP}.get(prop)
                    if val is not None: os.write(1, hdlc(bytes([f[0], 6, prop]) + val))      # PROP_VALUE_IS
    finally:
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        say("ended")


if __name__ == "__main__":
    main()
