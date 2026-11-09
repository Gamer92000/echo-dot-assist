// How loud a sound is, and the same sound at another loudness: the settings page's timer sound upload (app.js), which
// is held to Alexa's timer tone.  Loudness as BS.1770-4 integrates it (K-weighting, 400 ms blocks every 100 ms, the
// gates at -70 LUFS and 10 LU under the mean), on mono at 48 kHz: what the page makes of every upload, so the filters'
// coefficients are the standard's own for that rate.  A level raised past where its peaks fit goes through a limiter
// that looks 5 ms ahead: the peaks are held under the ceiling, the rest keeps its shape.  A ring plays its sound over and
// over, so that is how ringLufs measures it, for 3 s at least: a clip under 400 ms has no block of its own.  tests/unit/web_loudness_test.py
// holds it to a Python implementation of the standard, which ffmpeg's ebur128 agrees with on Amazon's tones.
'use strict';

const hmloud = (() => {
  const RATE = 48000;

  function biquad(x, b0, b1, b2, a1, a2) {
    const y = new Float64Array(x.length);
    let x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (let i = 0; i < x.length; i++) {
      const v = x[i], o = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
      x2 = x1; x1 = v; y2 = y1; y1 = o; y[i] = o;
    }
    return y;
  }

  // integrated loudness of x (floats, full scale 1) in LUFS; -Infinity when nothing passes the gates (silence)
  function lufs(x) {
    let y = biquad(x, 1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585);   // the head's shelf
    y = biquad(y, 1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621);                                                  // RLB high-pass
    const c = new Float64Array(y.length + 1);
    for (let i = 0; i < y.length; i++) c[i + 1] = c[i] + y[i] * y[i];
    const blk = RATE * 4 / 10, step = RATE / 10, z = [];
    for (let i = 0; i + blk <= y.length; i += step) z.push((c[i + blk] - c[i]) / blk);
    const L = (m) => m > 0 ? -0.691 + 10 * Math.log10(m) : -Infinity;
    const mean = (a) => a.reduce((s, v) => s + v, 0) / a.length;
    let g = z.filter((v) => L(v) > -70);
    if (!g.length) return -Infinity;
    const rel = L(mean(g)) - 10;
    g = g.filter((v) => L(v) > rel);
    return L(mean(g));
  }

  function looped(x, n) {
    if (!x.length || x.length >= n) return x;
    const out = new Float32Array(Math.ceil(n / x.length) * x.length);
    for (let o = 0; o < out.length; o += x.length) out.set(x, o);
    return out;
  }
  const ringLufs = (x) => lufs(looped(x, 3 * RATE));

  function peak(x) { let p = 0; for (let i = 0; i < x.length; i++) p = Math.max(p, Math.abs(x[i])); return p; }

  // x times gain, peaks over ceiling taken down: the gain each sample needs, its lowest over the next 5 ms (so the
  // gain is down before a peak comes), averaged over 5 ms (no step: it would click) and let back up over ~80 ms.
  // Never above what any sample needs: each average is of minimums that already cover it
  function limit(x, gain, ceiling) {
    const n = x.length, L = RATE / 200, need = new Float64Array(n), min = new Float64Array(n), out = new Float32Array(n);
    for (let i = 0; i < n; i++) { const a = Math.abs(x[i] * gain); need[i] = a > ceiling ? ceiling / a : 1; }
    const q = []; let h = 0;                                    // indexes of rising needs: the window's minimum first
    for (let i = n - 1; i >= 0; i--) {                          // min[i] = lowest of need[i .. i+L-1]
      while (q.length > h && need[q[q.length - 1]] >= need[i]) q.pop();
      q.push(i);
      if (q[h] >= i + L) h++;
      min[i] = need[q[h]];
      if (h > 4096) { q.splice(0, h); h = 0; }
    }
    const first = n ? min[0] : 1;                               // before the start: as low as its first 5 ms need
    let sum = L * first, g = 1; const k = 1 - Math.exp(-1 / (0.08 * RATE));
    for (let i = 0; i < n; i++) {
      sum += min[i] - (i >= L ? min[i - L] : first);           // the last L minimums
      g = Math.min(sum / L, g + (1 - g) * k);
      out[i] = Math.max(-1, Math.min(1, x[i] * gain * g));
    }
    return out;
  }

  // x brought to target LUFS (loop: measured as ringLufs does): { out, lufs (as measured after), gain_db (what was applied before the limiter), limited
  // (peaks were held), short_db (how far it stays under where maxGainDb is not enough) }; null for silence.  The
  // limiter only takes loudness away, so where it has to work the gain is searched for between the plain difference
  // (too little) and the most allowed: a secant step and a halving by turns (a secant alone can creep along one side)
  function match(x, target, { ceiling = 0.891, maxGainDb = 24, loop = false } = {}) {      // ceiling: -1 dBFS
    const measure = loop ? ringLufs : lufs, l0 = measure(x);
    if (!isFinite(l0)) return null;
    const at = (db) => { const out = limit(x, 10 ** (db / 20), ceiling); return { db, out, l: measure(out) }; };
    let a = at(Math.min(target - l0, maxGainDb)), best = a;
    if (target - a.l >= 0.1 && a.db < maxGainDb) {
      let b = at(maxGainDb);
      best = b;
      if (b.l > target) {
        for (let k = 0; k < 16 && target - a.l >= 0.1 && b.l - target >= 0.1; k++) {
          let d = k % 2 ? (a.db + b.db) / 2 : a.db + (target - a.l) * (b.db - a.db) / (b.l - a.l);
          if (!(d > a.db && d < b.db)) d = (a.db + b.db) / 2;
          const c = at(d);
          if (c.l < target) a = c; else b = c;
        }
        best = target - a.l < b.l - target ? a : b;
      }
    }
    return { out: best.out, lufs: best.l, gain_db: best.db, limited: peak(x) * 10 ** (best.db / 20) > ceiling, short_db: Math.max(0, target - best.l) };
  }

  return { lufs, ringLufs, peak, limit, match, RATE };
})();

if (typeof module !== 'undefined') module.exports = hmloud;
