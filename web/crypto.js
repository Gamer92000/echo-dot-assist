// X25519 and keyed BLAKE2b for the settings page.  The page is plain HTTP, where browsers leave crypto.subtle out, so
// both are here: X25519 after TweetNaCl (public domain), BLAKE2b after RFC 7693 on BigInt (the messages are a few
// hundred bytes).  tests/unit/web_crypto_test.mjs checks both against Python's.
'use strict';

const hmcrypto = (() => {
  // ---------------------------------------------------------------- X25519 (field elements: 16 limbs of 16 bits)
  const gf = (init) => { const r = new Float64Array(16); if (init) for (let i = 0; i < 16 && i < init.length; i++) r[i] = init[i]; return r; };
  const _121665 = gf([0xdb41, 1]);

  function car25519(o) {
    let c = 1;
    for (let i = 0; i < 16; i++) { const v = o[i] + c + 65535; c = Math.floor(v / 65536); o[i] = v - c * 65536; }
    o[0] += c - 1 + 37 * (c - 1);
  }
  function sel25519(p, q, b) {
    const c = ~(b - 1);
    for (let i = 0; i < 16; i++) { const t = c & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t; }
  }
  function pack25519(o, n) {
    const m = gf(), t = gf(n);
    car25519(t); car25519(t); car25519(t);
    for (let j = 0; j < 2; j++) {
      m[0] = t[0] - 0xffed;
      for (let i = 1; i < 15; i++) { m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1); m[i - 1] &= 0xffff; }
      m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
      const b = (m[15] >> 16) & 1;
      m[14] &= 0xffff;
      sel25519(t, m, 1 - b);
    }
    for (let i = 0; i < 16; i++) { o[2 * i] = t[i] & 0xff; o[2 * i + 1] = t[i] >> 8; }
  }
  function unpack25519(o, n) { for (let i = 0; i < 16; i++) o[i] = n[2 * i] + (n[2 * i + 1] << 8); o[15] &= 0x7fff; }
  const A = (o, a, b) => { for (let i = 0; i < 16; i++) o[i] = a[i] + b[i]; };
  const Z = (o, a, b) => { for (let i = 0; i < 16; i++) o[i] = a[i] - b[i]; };
  function M(o, a, b) {
    const t = new Float64Array(31);
    for (let i = 0; i < 16; i++) for (let j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (let i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (let i = 0; i < 16; i++) o[i] = t[i];
    car25519(o); car25519(o);
  }
  const S = (o, a) => M(o, a, a);
  function inv25519(o, inp) {
    const c = gf(inp);
    for (let a = 253; a >= 0; a--) { S(c, c); if (a !== 2 && a !== 4) M(c, c, inp); }
    for (let a = 0; a < 16; a++) o[a] = c[a];
  }

  // scalar n (32 bytes), point p (32 bytes) -> 32 bytes
  function x25519(n, p) {
    const z = new Uint8Array(32), x = new Float64Array(80), q = new Uint8Array(32);
    const a = gf(), b = gf(), c = gf(), d = gf(), e = gf(), f = gf();
    z.set(n.subarray(0, 31)); z[31] = (n[31] & 127) | 64; z[0] &= 248;
    unpack25519(x, p);
    for (let i = 0; i < 16; i++) { b[i] = x[i]; d[i] = a[i] = c[i] = 0; }
    a[0] = d[0] = 1;
    for (let i = 254; i >= 0; --i) {
      const r = (z[i >>> 3] >>> (i & 7)) & 1;
      sel25519(a, b, r); sel25519(c, d, r);
      A(e, a, c); Z(a, a, c); A(c, b, d); Z(b, b, d); S(d, e); S(f, a); M(a, c, a); M(c, b, e);
      A(e, a, c); Z(a, a, c); S(b, a); Z(c, d, f); M(a, c, _121665); A(a, a, d); M(c, c, a); M(a, d, f); M(d, b, x); S(b, e);
      sel25519(a, b, r); sel25519(c, d, r);
    }
    for (let i = 0; i < 16; i++) { x[i + 16] = a[i]; x[i + 32] = c[i]; x[i + 48] = b[i]; x[i + 64] = d[i]; }
    const x32 = x.subarray(32), x16 = x.subarray(16);
    inv25519(x32, x32); M(x16, x16, x32); pack25519(q, x16);
    return q;
  }
  const BASE = new Uint8Array(32); BASE[0] = 9;
  const x25519Public = (sk) => x25519(sk, BASE);

  // ---------------------------------------------------------------- BLAKE2b
  const M64 = (1n << 64n) - 1n;
  const IV = [0x6a09e667f3bcc908n, 0xbb67ae8584caa73bn, 0x3c6ef372fe94f82bn, 0xa54ff53a5f1d36f1n,
              0x510e527fade682d1n, 0x9b05688c2b3e6c1fn, 0x1f83d9abfb41bd6bn, 0x5be0cd19137e2179n];
  const SIGMA = [
    [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15], [14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3],
    [11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4], [7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8],
    [9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13], [2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9],
    [12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11], [13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10],
    [6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5], [10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0]];
  const rotr = (x, n) => ((x >> n) | (x << (64n - n))) & M64;

  function compress(h, block, t, last) {
    const v = h.concat(IV), m = new Array(16);
    for (let i = 0; i < 16; i++) {
      let w = 0n;
      for (let k = 7; k >= 0; k--) w = (w << 8n) | BigInt(block[8 * i + k]);
      m[i] = w;
    }
    v[12] ^= t & M64; v[13] ^= t >> 64n;
    if (last) v[14] ^= M64;
    const G = (a, b, c, d, x, y) => {
      v[a] = (v[a] + v[b] + x) & M64; v[d] = rotr(v[d] ^ v[a], 32n);
      v[c] = (v[c] + v[d]) & M64;     v[b] = rotr(v[b] ^ v[c], 24n);
      v[a] = (v[a] + v[b] + y) & M64; v[d] = rotr(v[d] ^ v[a], 16n);
      v[c] = (v[c] + v[d]) & M64;     v[b] = rotr(v[b] ^ v[c], 63n);
    };
    for (let r = 0; r < 12; r++) {
      const s = SIGMA[r % 10];
      G(0, 4, 8, 12, m[s[0]], m[s[1]]); G(1, 5, 9, 13, m[s[2]], m[s[3]]);
      G(2, 6, 10, 14, m[s[4]], m[s[5]]); G(3, 7, 11, 15, m[s[6]], m[s[7]]);
      G(0, 5, 10, 15, m[s[8]], m[s[9]]); G(1, 6, 11, 12, m[s[10]], m[s[11]]);
      G(2, 7, 8, 13, m[s[12]], m[s[13]]); G(3, 4, 9, 14, m[s[14]], m[s[15]]);
    }
    for (let i = 0; i < 8; i++) h[i] ^= v[i] ^ v[i + 8];
  }

  // outlen 1..64, key 0..64 bytes (Uint8Array or null), data Uint8Array
  function blake2b(outlen, key, data) {
    const kl = key ? key.length : 0, h = IV.slice();
    h[0] ^= 0x01010000n ^ (BigInt(kl) << 8n) ^ BigInt(outlen);
    let input = data;
    if (kl) { input = new Uint8Array(128 + data.length); input.set(key); input.set(data, 128); }
    let t = 0n, off = 0;
    while (input.length - off > 128) { t += 128n; compress(h, input.subarray(off, off + 128), t, false); off += 128; }
    const lastBlock = new Uint8Array(128); lastBlock.set(input.subarray(off));
    t += BigInt(input.length - off);
    compress(h, lastBlock, t, true);
    const out = new Uint8Array(outlen);
    for (let i = 0; i < outlen; i++) out[i] = Number((h[i >> 3] >> BigInt(8 * (i & 7))) & 0xffn);
    return out;
  }

  const hex = (b) => Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
  const unhex = (s) => new Uint8Array(s.match(/../g).map((x) => parseInt(x, 16)));
  return { x25519, x25519Public, blake2b, hex, unhex };
})();

if (typeof module !== 'undefined') module.exports = hmcrypto;
