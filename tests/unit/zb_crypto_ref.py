#!/usr/bin/env python3
"""Reference cases for tests/unit/zb_crypto_test.c: CCM with a 4-byte tag (Python's cryptography: CCM* at level 5 is
plain CCM) on random keys, nonces, headers and payloads of every length Zigbee uses, and HMAC-MMO (Zigbee B.1.4) built
here from AES (the MMO hash written anew, so the C one is checked by an implementation of its own)."""
import os, random
from cryptography.hazmat.primitives.ciphers.aead import AESCCM
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

def aes(k, b):
    e = Cipher(algorithms.AES(k), modes.ECB()).encryptor(); return e.update(b) + e.finalize()

def mmo(m):
    bits = len(m) * 8
    p = m + b"\x80"
    while (len(p) + 2) % 16: p += b"\0"
    p += bits.to_bytes(2, "big")
    h = bytes(16)
    for i in range(0, len(p), 16):
        blk = p[i:i + 16]; h = bytes(x ^ y for x, y in zip(aes(h, blk), blk))
    return h

def hmac(k, b):
    inner = mmo(bytes(x ^ 0x36 for x in k) + bytes([b]))
    return mmo(bytes(x ^ 0x5c for x in k) + inner)

r = random.Random(7)
for i in range(300):
    k = bytes(r.randrange(256) for _ in range(16)); n = bytes(r.randrange(256) for _ in range(13))
    a = bytes(r.randrange(256) for _ in range(r.choice([0, 1, 8, 14, 15, 16, 30, 40])))
    m = bytes(r.randrange(256) for _ in range(r.choice([0, 1, 15, 16, 17, 31, 33, 60, 90])))
    c = AESCCM(k, tag_length=4).encrypt(n, m, a)
    print("ccm", k.hex(), n.hex(), a.hex() or "-", m.hex() or "-", c.hex())
tc = b"ZigBeeAlliance09"
for b in (0, 2, 3):
    print("hmac", tc.hex(), f"{b:02x}", hmac(tc, b).hex())
for i in range(20):
    k = bytes(r.randrange(256) for _ in range(16)); b = r.randrange(256)
    print("hmac", k.hex(), f"{b:02x}", hmac(k, b).hex())
