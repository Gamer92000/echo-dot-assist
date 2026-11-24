#!/usr/bin/env python3
"""web/crypto.js (the settings page's X25519, BLAKE2b and seal) against Python's: random keys and messages of every
length around the block size, through node."""
import hashlib, json, os, subprocess, sys
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat, PrivateFormat, NoEncryption

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
cases = {"x": [], "b": [], "s": [], "s2": []}
for _ in range(20):
    a, b = X25519PrivateKey.generate(), X25519PrivateKey.generate()
    ask = a.private_bytes(Encoding.Raw, PrivateFormat.Raw, NoEncryption())
    bpk = b.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
    cases["x"].append([ask.hex(), bpk.hex(), a.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw).hex(), a.exchange(X25519PublicKey.from_public_bytes(bpk)).hex()])
for n in list(range(0, 260)) + [1000, 4096]:
    data, key, out = os.urandom(n), os.urandom(n % 65), 16 + n % 49
    cases["b"].append([data.hex(), key.hex(), out, hashlib.blake2b(data, key=key, digest_size=out).hexdigest()])

def seal(k, ctr, data):
    """web.c unseal's other half, from its description"""
    ek = hashlib.blake2b(b"hassmic seal 1", key=k, digest_size=32).digest()
    ks = b"".join(hashlib.blake2b(f"{ctr}\n".encode() + bytes([i]), key=ek, digest_size=64).digest() for i in range((len(data) + 63) // 64))
    return bytes(a ^ b for a, b in zip(data, ks))
for n in [0, 1, 8, 63, 64, 65, 200]:
    k, data, ctr = os.urandom(32), os.urandom(n), 1700000000000000 + n
    cases["s"].append([k.hex(), ctr, data.hex(), seal(k, ctr, data).hex()])

def seal2(k, ctr, data, purpose, nonce=b""):
    """web.c seal2 (any length: a four-byte block number; the Echo's answers under a nonce of their own), from its
    description"""
    ek = hashlib.blake2b(purpose.encode(), key=k, digest_size=32).digest()
    ks = b"".join(hashlib.blake2b(f"{ctr}\n".encode() + nonce + i.to_bytes(4, "big"), key=ek, digest_size=64).digest() for i in range((len(data) + 63) // 64))
    return bytes(a ^ b for a, b in zip(data, ks))
for n, purpose, nonce in ((0, "hassmic seal 2", b""), (65, "hassmic seal 2", b""), (64 * 256 + 7, "hassmic seal 2", b""), (64 * 300, "hassmic seal back 3", os.urandom(16)), (5, "hassmic seal back 3", os.urandom(16))):
    k, data, ctr = os.urandom(32), os.urandom(n), 1700000000000000 + n
    cases["s2"].append([k.hex(), ctr, data.hex(), purpose, nonce.hex(), seal2(k, ctr, data, purpose, nonce).hex()])

js = """
const c = require(process.argv[1]); const t = JSON.parse(require('fs').readFileSync(0, 'utf8')); let bad = 0;
for (const [sk, pk, pub, shared] of t.x) {
  if (c.hex(c.x25519Public(c.unhex(sk))) !== pub) { bad++; console.log('public key', sk); }
  if (c.hex(c.x25519(c.unhex(sk), c.unhex(pk))) !== shared) { bad++; console.log('shared', sk); }
}
for (const [d, k, n, want] of t.b) {
  const got = c.hex(c.blake2b(n, k ? c.unhex(k) : null, d ? c.unhex(d) : new Uint8Array(0)));
  if (got !== want) { bad++; console.log('blake2b', d.length / 2, k.length / 2, n); }
}
for (const [k, ctr, d, want] of t.s) {
  if (c.hex(c.seal(c.unhex(k), ctr, d ? c.unhex(d) : new Uint8Array(0))) !== want) { bad++; console.log('seal', d.length / 2); }
}
for (const [k, ctr, d, purpose, nonce, want] of t.s2) {
  if (c.hex(c.seal2(c.unhex(k), ctr, d ? c.unhex(d) : new Uint8Array(0), purpose, nonce ? c.unhex(nonce) : undefined)) !== want) { bad++; console.log('seal2', d.length / 2, purpose); }
}
console.log(bad ? 'FAIL ' + bad : 'ok   ' + t.x.length + ' X25519, ' + t.b.length + ' BLAKE2b, ' + t.s.length + ' seal, ' + t.s2.length + ' seal2 (past 256 blocks)'); process.exit(bad ? 1 : 0);
"""
r = subprocess.run(["node", "-e", js, os.path.join(ROOT, "web/crypto.js")], input=json.dumps(cases), text=True)
sys.exit(r.returncode)
