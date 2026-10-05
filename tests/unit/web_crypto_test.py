#!/usr/bin/env python3
"""web/crypto.js (the settings page's X25519 and BLAKE2b) against Python's: random keys and messages of every length
around the block size, through node."""
import hashlib, json, os, subprocess, sys
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat, PrivateFormat, NoEncryption

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
cases = {"x": [], "b": []}
for _ in range(20):
    a, b = X25519PrivateKey.generate(), X25519PrivateKey.generate()
    ask = a.private_bytes(Encoding.Raw, PrivateFormat.Raw, NoEncryption())
    bpk = b.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
    cases["x"].append([ask.hex(), bpk.hex(), a.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw).hex(), a.exchange(X25519PublicKey.from_public_bytes(bpk)).hex()])
for n in list(range(0, 260)) + [1000, 4096]:
    data, key, out = os.urandom(n), os.urandom(n % 65), 16 + n % 49
    cases["b"].append([data.hex(), key.hex(), out, hashlib.blake2b(data, key=key, digest_size=out).hexdigest()])

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
console.log(bad ? 'FAIL ' + bad : 'ok   ' + t.x.length + ' X25519, ' + t.b.length + ' BLAKE2b'); process.exit(bad ? 1 : 0);
"""
r = subprocess.run(["node", "-e", js, os.path.join(ROOT, "web/crypto.js")], input=json.dumps(cases), text=True)
sys.exit(r.returncode)
