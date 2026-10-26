// The light ring's designs (src/hassmic/ringdesign.c, ring.c; issue #38): up to 100 animations drawn here, and for each
// of the Echo's own patterns which of them shows in place of Amazon's.  Designs are kept as drawn; the Echo turns
// listening's towards the talker itself when it installs them.  Nothing reaches the ring before "Apply": root then
// writes the set to the system partition and restarts the ring's daemon (a second without light).
// Colours here are as seen; the Echo maps them to its LEDs' duty (gamma 2.2), and Amazon's files the other way round
// when one is taken as a start.  Uses app.js: h, icon, echo, modal, ask, toast, errText.
'use strict';

// ring.c's slots: what each is, in the user's words, in the order the page lists them
const RING_PLACES = [
  { id: 'listen', group: 'Voice', label: 'Listening', help: 'From the wake word on, while you speak. Turned towards whoever speaks (the arrow in the designer).' },
  { id: 'end', group: 'Voice', label: 'Listening ends', help: 'Once you stopped speaking. Turned towards the talker too.' },
  { id: 'think', group: 'Voice', label: 'Thinking', help: 'While your voice assistant works out the answer.' },
  { id: 'talk', group: 'Voice', label: 'Speaking', help: 'While the reply plays.' },
  { id: 'error', group: 'Voice', label: 'Not understood', help: 'Nothing was said after the wake word, or the command failed.' },
  { id: 'timer', group: 'Timers', label: 'Timer ringing' },
  { id: 'timer_short', group: 'Timers', label: 'Timer behind a reply', help: 'A short flash when a ringing timer goes behind a conversation.' },
  { id: 'volume', group: 'Volume', label: 'Volume', help: 'Amazon\'s volume steps in the design\'s colours: each lit LED takes its colour from the design\'s first frame.' },
  { id: 'assist_volume', group: 'Volume', label: 'Voice reply volume', help: 'The same for the replies\' own volume (separate volumes); Amazon\'s steps in cyan otherwise.' },
  { id: 'mute', group: 'Status', label: 'Microphones off', help: 'The microphone button, or the mute switch in Home Assistant.' },
  { id: 'unmute', group: 'Status', label: 'Microphones on', help: 'Once, when the microphones are switched back on.' },
  { id: 'dnd', group: 'Status', label: 'Do not disturb', help: 'Once, when do not disturb is switched on.' },
  { id: 'identify', group: 'Status', label: 'Identify', help: 'For 10 s, to tell which Echo this is (settings page, Home Assistant).' },
  { id: 'bt_pairing', group: 'Status', label: 'Bluetooth pairing', help: 'While phones can pair.' },
  { id: 'call_out', group: 'Drop In', label: 'Calling' },
  { id: 'call_in', group: 'Drop In', label: 'Ringing' },
  { id: 'call_live', group: 'Drop In', label: 'In a call' },
  { id: 'call_end', group: 'Drop In', label: 'Call over' },
  { id: 'setup', group: 'Setup', label: 'Not set up yet', help: 'Until Home Assistant has adopted the Echo.' },
  { id: 'reset', group: 'Setup', label: 'Factory reset', help: 'While the action button is held for a reset.' },
  { id: 'login', group: 'Setup', label: 'Page login', help: 'While a browser waits for the action button to log in to this page.' },
];
const RING_TALKER = 10.5;              // where stock's listening ring points at turn 0: its light-blue pair, LEDs 10 and 11
const RING_PRESETS = ['#000000', '#ffffff', '#0000ff', '#00ffff', '#ff0000', '#ff7a00', '#ffd400', '#00e040', '#00c8a0', '#7a3cff', '#ff2fb4', '#ff8080'];

let ringBox = null;
const ring = { status: null, designs: new Map(), packs: new Map(), stock: new Map(), loading: null, next: null, poll: 0, listeners: new Set(), view: 0,
               home: new Map(), unfixed: new Set() };
// One request at a time: the Echo takes a counter only when it is above the last one it saw, and two in flight can
// arrive the other way round.  Each given up on after RING_WAIT (none of them waits for anything on the Echo; the
// longest, every design, is 4 MB at most): one that hangs would hold every later one, the whole section dead
const RING_WAIT = 20000;
let ringQ = Promise.resolve();
function rcall(method, path, body) {
  const r = ringQ.then(() => echo.call(method, path, body, within(RING_WAIT)).catch((e) => {
    throw e.name === 'AbortError' ? new Error(`no answer from the Echo in ${RING_WAIT / 1000} s`) : e;
  }));
  ringQ = r.catch(() => {});
  return r;
}

// ---------------------------------------------------------------- designs as text (ringdesign.h) and as objects

const hexRgb = (s) => (s.length === 3 ? [...s].map((c) => parseInt(c + c, 16)) : [0, 2, 4].map((i) => parseInt(s.slice(i, i + 2), 16)));
const rgbHex = (c) => c.map((v) => v.toString(16).padStart(2, '0')).join('').toUpperCase();
const cssRgb = (c) => `rgb(${c[0]},${c[1]},${c[2]})`;
const black = (n) => Array.from({ length: n }, () => [0, 0, 0]);
const copyDesign = (d) => ({ ...d, frames: d.frames.map((f) => ({ ms: f.ms, px: f.px.map((c) => c.slice()) })) });

function designParse(text) {
  const d = { name: 'Design', leds: 12, loop: 0, frames: [] };
  for (const raw of text.split('\n')) {
    const l = raw.replace(/\r$/, '');
    if (!l || l[0] === '#') continue;
    if (l.startsWith('name ')) d.name = l.slice(5);
    else if (l.startsWith('leds ')) d.leds = +l.slice(5);
    else if (l.startsWith('loop ')) d.loop = l.slice(5) === 'once' ? -1 : +l.slice(5);
    else { const m = /^(\d+):([0-9A-Fa-f,]+)$/.exec(l); if (m) d.frames.push({ ms: +m[1], px: m[2].split(',').map(hexRgb) }); }
  }
  return d;
}

function designText(d) {
  return `name ${d.name}\nleds ${d.leds}\nloop ${d.loop < 0 ? 'once' : d.loop}\n` +
    d.frames.map((f) => `${f.ms}:${f.px.map(rgbHex).join(',')}\n`).join('');
}

// Amazon's file (the driver's duty, 3 or 6 digits, "loop" where its loop starts) as a design: colours as seen, frames
// that repeat merged, a black loop (it finishes) as "once", no loop (it repeats) as all of it repeating.  cut: how many
// frames it had where a design holds fewer (the first frames_max kept), else 0
function stockDesign(text, leds, name) {
  const seen = (v) => Math.round(255 * Math.pow(v / 255, 1 / 2.2));
  let frames = [], loopAt = -1;
  for (const raw of text.split('\n')) {
    const l = raw.trim();
    if (l === 'loop') { loopAt = frames.length; continue; }
    const m = /^(\d+):([0-9A-Fa-f,]+)$/.exec(l);
    if (!m) continue;
    const px = m[2].split(',').slice(0, leds).map((p) => hexRgb(p).map(seen));
    while (px.length < leds) px.push([0, 0, 0]);
    frames.push({ ms: Math.max(1, Math.min(60000, +m[1])), px });
  }
  let from = loopAt < 0 ? 0 : loopAt, loop = from;
  if (loopAt >= 0 && frames.slice(loopAt).every((f) => f.px.every((c) => !c[0] && !c[1] && !c[2]))) { frames = frames.slice(0, loopAt); from = loop = -1; }
  const merged = [];
  frames.forEach((f, i) => {
    const prev = merged[merged.length - 1];
    if (prev && i !== from && rgbKey(prev.px) === rgbKey(f.px) && prev.ms + f.ms <= 60000) prev.ms += f.ms;
    else { if (i === from) loop = merged.length; merged.push(f); }
  });
  const max = (ring.status && ring.status.frames_max) || 1000;
  const cut = merged.length > max ? merged.length : 0;
  const d = { name, leds, loop: loop >= max ? 0 : loop, frames: merged.length ? merged.slice(0, max) : [{ ms: 1000, px: black(leds) }] };
  return { d, cut };
}
const rgbKey = (px) => px.map(rgbHex).join();
// a text field cut to what the Echo takes, in bytes (fitBytes, app.js)
const fitInput = (el, max) => { const v = fitBytes(el.value, max); if (v !== el.value) el.value = v; return v; };

// A colour with its hue turned by deg in OKLCH, a space made so that equal steps look equal: how light it looks and how
// colourful stay, only the hue moves (in HSV pure blue turned to yellow came out several times as bright).  Where no
// colour of that lightness has the chroma (a dark yellow), as much as fits.  Greys stay as they are
const lin = (v) => ((v /= 255) <= 0.04045 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4);
const unlin = (v) => 255 * (v <= 0.0031308 ? 12.92 * v : 1.055 * v ** (1 / 2.4) - 0.055);
function toOklab(c) {
  const [r, g, b] = c.map(lin);
  const l = Math.cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
  const m = Math.cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
  const s = Math.cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
  return [0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s, 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
          0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s];
}
function fromOklab([L, A, B]) {                     // linear RGB, maybe out of range
  const l = (L + 0.3963377774 * A + 0.2158037573 * B) ** 3, m = (L - 0.1055613458 * A - 0.0638541728 * B) ** 3;
  const s = (L - 0.0894841775 * A - 1.2914855480 * B) ** 3;
  return [4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s, -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
          -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s];
}
function turnHue(c, deg) {
  if (Math.max(...c) - Math.min(...c) < 3) return c.slice();
  const [L, A, B] = toOklab(c), C = Math.hypot(A, B), hh = Math.atan2(B, A) + (deg * Math.PI) / 180;
  const at = (k) => fromOklab([L, Math.cos(hh) * C * k, Math.sin(hh) * C * k]);
  const fits = (rgb) => rgb.every((v) => v >= -1e-4 && v <= 1 + 1e-4);
  let lo = 0, hi = 1;
  if (!fits(at(1))) for (let i = 0; i < 18; i++) { const mid = (lo + hi) / 2; if (fits(at(mid))) lo = mid; else hi = mid; }
  else lo = 1;
  return at(lo).map((v) => Math.max(0, Math.min(255, Math.round(unlin(Math.max(0, Math.min(1, v)))))));
}

// ---------------------------------------------------------------- the ring drawn: an Echo seen from above

// As the Echo looks: a thin band of light near the edge of its dark top, twelve LEDs under one diffuser, each a spot
// that runs a little into its neighbours, with a glow round what is lit.  LED 0 at the top, then clockwise, as ring.c
// counts them.  Drawn once per frame and size and kept: a design repeats its frames, and the page plays dozens
const RING_R = 0.8, RING_W = 0.12, RING_SIGMA = 0.3;          // band radius and width as a share of half the size; spot width in LEDs
const RING_OFF = [38, 44, 51];
function ledAngle(i, n) { return -Math.PI / 2 + (i * 2 * Math.PI) / n; }
const lit = (c) => c && (c[0] || c[1] || c[2]);
// el.append for what h() takes: arrays flattened, null and false left out (append itself would write them as text)
const fill = (el, ...kids) => el.append(...kids.flat(Infinity).filter((x) => x != null && x !== false));

// The light at a point of the ring (u in LEDs from LED 0): every LED's colour, weighted by how far it is
function lightAt(px, u) {
  const n = px.length, out = [0, 0, 0];
  for (let i = 0; i < n; i++) {
    const c = px[i]; if (!lit(c)) continue;
    let d = Math.abs(u - i) % n; if (d > n / 2) d = n - d;
    if (d > 1.5) continue;
    const w = Math.exp(-(d * d) / (2 * RING_SIGMA * RING_SIGMA));
    out[0] += c[0] * w; out[1] += c[1] * w; out[2] += c[2] * w;
  }
  return out;
}

// Canvases drawn once and kept by key, up to a budget of pixels (4 bytes each), the least recently used dropped first.
// A count would not do: the designer's ring at 3x is 1080 px square, 4.7 MB, a tile's 70 kB
function pixelCache(budget) {
  const m = new Map(); let used = 0;
  return {
    get(k) { const v = m.get(k); if (v) { m.delete(k); m.set(k, v); } return v; },
    set(k, v) {
      m.set(k, v); used += v.width * v.height;
      for (const [ok, ov] of m) { if (used <= budget || ok === k) break; m.delete(ok); used -= ov.width * ov.height; }
    },
  };
}
const ringCache = pixelCache(16e6);       // the designer's 13 frames at 3x, or 230 tiles
const canFilter = (() => { try { return 'filter' in document.createElement('canvas').getContext('2d'); } catch (e) { return false; } })();
function ringImage(px, s, talker) {
  const key = `${s}|${talker}|${px.map((c) => (lit(c) ? rgbHex(c) : '0')).join()}`;
  let img = ringCache.get(key);
  if (img) return img;
  const n = px.length || 12, c = s / 2, r = c * RING_R, w = c * RING_W, slices = Math.max(72, Math.min(240, Math.round(s * 1.2)));
  img = document.createElement('canvas'); img.width = img.height = s;
  const x = img.getContext('2d');
  // the top: dark, a touch lighter in the middle, a rim
  const g = x.createRadialGradient(c, c * 0.92, c * 0.1, c, c, c);
  g.addColorStop(0, '#2c3238'); g.addColorStop(0.75, '#1d2227'); g.addColorStop(1, '#14181b');
  x.beginPath(); x.arc(c, c, c * 0.985, 0, 2 * Math.PI); x.fillStyle = g; x.fill();
  x.lineWidth = Math.max(1, c * 0.02); x.strokeStyle = 'rgba(255,255,255,.07)'; x.stroke();
  // the band: a groove, the diffuser where it is dark
  x.beginPath(); x.arc(c, c, r, 0, 2 * Math.PI); x.lineWidth = w * 1.5; x.strokeStyle = '#101316'; x.stroke();
  x.beginPath(); x.arc(c, c, r, 0, 2 * Math.PI); x.lineWidth = w; x.strokeStyle = cssRgb(RING_OFF); x.stroke();
  // the light: a gradient round the band (slices where the browser has no conic gradient), added onto it
  const light = document.createElement('canvas'); light.width = light.height = s;
  const l = light.getContext('2d'); l.lineWidth = w; l.lineCap = 'butt';
  const cols = Array.from({ length: slices }, (_, k) => lightAt(px, (k * n) / slices));
  const any = cols.some((col) => col[0] + col[1] + col[2] >= 3);
  const css = (col) => `rgb(${Math.min(255, Math.round(col[0]))},${Math.min(255, Math.round(col[1]))},${Math.min(255, Math.round(col[2]))})`;
  if (any && l.createConicGradient) {
    const grad = l.createConicGradient(-Math.PI / 2, c, c);
    cols.forEach((col, k) => grad.addColorStop(k / slices, css(col)));
    grad.addColorStop(1, css(cols[0]));
    l.beginPath(); l.arc(c, c, r, 0, 2 * Math.PI); l.strokeStyle = grad; l.stroke();
  } else if (any) {
    cols.forEach((col, k) => {
      if (col[0] + col[1] + col[2] < 3) return;
      const a0 = -Math.PI / 2 + ((k - 0.5) * 2 * Math.PI) / slices;
      l.beginPath(); l.arc(c, c, r, a0, a0 + (2 * Math.PI) / slices + 0.01); l.strokeStyle = css(col); l.stroke();
    });
  }
  if (any) {
    if (canFilter) {                    // the glow: the light blurred, under it
      x.save(); x.filter = `blur(${Math.max(1.5, c * 0.07)}px)`; x.globalCompositeOperation = 'lighter'; x.globalAlpha = 0.85;
      x.drawImage(light, 0, 0); x.restore();
    }
    x.save(); x.globalCompositeOperation = 'lighter'; x.drawImage(light, 0, 0); x.restore();
  }
  if (talker != null) {                 // where the talker is: an arrow outside the band
    const a = ledAngle(talker, n);
    x.save(); x.translate(c + Math.cos(a) * c * 0.985, c + Math.sin(a) * c * 0.985); x.rotate(a + Math.PI / 2);
    x.beginPath(); x.moveTo(0, c * 0.05); x.lineTo(-c * 0.065, -c * 0.06); x.lineTo(c * 0.065, -c * 0.06); x.closePath();
    x.fillStyle = '#f2c94c'; x.fill(); x.restore();
  }
  ringCache.set(key, img);
  return img;
}

function drawRing(cv, px, opts = {}) {
  const x = cv.getContext('2d');
  x.clearRect(0, 0, cv.width, cv.height);
  x.drawImage(ringImage(px, cv.width, opts.talker == null ? null : opts.talker), 0, 0);
}

function sizedCanvas(css, cls) {
  const dpr = Math.min(3, window.devicePixelRatio || 1);
  const cv = h('canvas', { width: String(Math.round(css * dpr)), height: String(Math.round(css * dpr)), 'aria-hidden': 'true', class: cls || null });
  cv.style.width = cv.style.height = css + 'px';
  return cv;
}

// A frame as a CSS background, for the frame strip (hundreds of them: no canvas each): each LED a spot of its colour,
// running into the next as on the ring
function frameCss(px) {
  const n = px.length, step = 360 / n, half = step * 0.32;
  const col = (c) => (lit(c) ? cssRgb(c.map((v, k) => Math.min(255, v + RING_OFF[k]))) : cssRgb(RING_OFF));
  const mid = (a, b) => cssRgb([0, 1, 2].map((k) => Math.round(((lit(a) ? a[k] + RING_OFF[k] : RING_OFF[k]) + (lit(b) ? b[k] + RING_OFF[k] : RING_OFF[k])) / 2)));
  const stops = [`${mid(px[n - 1], px[0])} 0deg`];
  px.forEach((c, i) => { const a = step / 2 + i * step; stops.push(`${col(c)} ${a - half}deg`, `${col(c)} ${a + half}deg`); });
  stops.push(`${mid(px[n - 1], px[0])} 360deg`);
  return `conic-gradient(from ${-step / 2}deg, ${stops.join(',')})`;
}

// Which frame shows t ms after it started (-1: none, it is over): its frames up to the loop once, then the loop again
// and again; played once: then dark.  A preview starts over now and then, so that the opening shows again
function frameIndexAt(d, t) {
  const fr = d.frames;
  if (!fr.length) return -1;
  const lp = d.loop < 0 ? fr.length : d.loop;
  const intro = fr.slice(0, lp).reduce((a, f) => a + f.ms, 0), loopMs = fr.slice(lp).reduce((a, f) => a + f.ms, 0);
  const cycle = d.loop < 0 ? intro + 1200 : intro + loopMs * Math.max(1, Math.ceil(4000 / Math.max(1, loopMs)));
  t %= cycle;
  if (t < intro) { for (let i = 0, a = 0; i < lp; i++) { a += fr[i].ms; if (t < a) return i; } }
  else if (d.loop >= 0 && loopMs) {
    let u = (t - intro) % loopMs;
    for (let i = lp; i < fr.length; i++) { if (u < fr[i].ms) return i; u -= fr[i].ms; }
  }
  return -1;
}
// ... and what it shows then, turned by turn LEDs (LED i shows what LED i - turn did, as ring.c)
function frameAt(d, t, turn = 0) {
  const i = frameIndexAt(d, t);
  return i < 0 ? black(d.leds) : turned(d.frames[i].px, turn);
}
function turned(px, turn) {
  if (!turn) return px;
  const n = px.length, out = new Array(n);
  px.forEach((c, i) => { out[(i + turn) % n] = c; });
  return out;
}
const totalMs = (d) => d.frames.reduce((a, f) => a + f.ms, 0);
const seconds = (ms) => `${(ms / 1000).toFixed(ms < 10000 ? 1 : 0)} s`;

// Every playing ring on the page, on one clock: 25 frames a second, only those on screen, still with reduced motion.
// get() -> { d, turn, talker, still } or null (dark); still: a frame instead of the design playing
const players = new Set();
let playing = false;
function play(cv, get) {
  const p = { cv, get, t0: performance.now(), born: performance.now() };
  players.add(p);
  if (!playing) { playing = true; requestAnimationFrame(tickPlayers); }
  return p;
}
let lastPaint = 0;
function tickPlayers(now) {
  for (const p of players) if (!p.cv.isConnected && (p.seen || now - p.born > 2000)) players.delete(p);    // its card redrawn, its dialog closed
  if (!players.size) { playing = false; return; }
  requestAnimationFrame(tickPlayers);
  if (now - lastPaint < 40) return;
  lastPaint = now;
  const reduced = window.matchMedia && matchMedia('(prefers-reduced-motion: reduce)').matches;
  for (const p of players) {
    if (!p.cv.isConnected) continue;
    p.seen = true;
    const r = p.cv.getBoundingClientRect();
    if (!r.width || r.bottom < 0 || r.top > innerHeight) continue;
    const g = p.get();
    if (!g) { drawRing(p.cv, black(12)); continue; }
    const at = g.still ? null : reduced ? (g.d.loop > 0 ? g.d.frames.slice(0, g.d.loop).reduce((a, f) => a + f.ms, 0) : 0) : now - p.t0;
    drawRing(p.cv, g.still ? turned(g.still, g.turn || 0) : frameAt(g.d, at, g.turn || 0), { talker: g.talker });
  }
}

// ---------------------------------------------------------------- the Echo's side

// Everything again from the Echo, after a change.  One load at a time; asked while one runs (a change made after it
// started, which it may not have seen) one more follows it, and the caller waits for that one
function ringLoad() {
  if (ring.loading) {
    return ring.next || (ring.next = ring.loading.catch(() => {}).then(() => { ring.next = null; return ringLoad(); }));
  }
  ring.loading = ringLoadNow().finally(() => { ring.loading = null; });
  return ring.loading;
}
async function ringLoadNow() {
  await ringFetch();
  // packs of older builds made folders: their designs in them, none in two (then fetched again)
  let changed = false;
  try { changed = await fixPacks(); } catch (e) { toast('Converting the packs: ' + errText(e)); }
  if (changed) await ringFetch();
  ring.home = new Map();                    // design id -> the pack it is in (none: the top level)
  for (const [pid, p] of ring.packs) for (const did of p.has) ring.home.set(did, pid);
  ringRender();
  for (const f of ring.listeners) f();
}
async function ringFetch() {
  const [st, lib, packs] = await Promise.all([rcall('GET', '/api/ring'), rcall('GET', '/api/ring/designs'), rcall('GET', '/api/ring/packs')]);
  ring.status = st.json();
  ring.designs = new Map();
  const parts = ('\n' + lib.text()).split(/\ndesign (\d+)\n/);       // '', id, text, id, text, ...
  for (let i = 1; i + 1 < parts.length; i += 2) ring.designs.set(+parts[i], designParse(parts[i + 1]));
  ring.packs = new Map();
  const pk = ('\n' + packs.text()).split(/\npack (\d+)\n/);
  for (let i = 1; i + 1 < pk.length; i += 2) ring.packs.set(+pk[i], packParse(pk[i + 1]));
}

// While root installs, asked again every 1.5 s, a request that failed included: one lost answer must not leave the
// bar on "Installing…"
async function ringStatus() {
  try {
    ring.status = (await rcall('GET', '/api/ring')).json();
    ringRender();
  } finally {
    clearTimeout(ring.poll);
    if (ring.status && ring.status.state === 'installing') ring.poll = setTimeout(() => ringStatus().catch(() => {}), 1500);
  }
}

// Amazon's animation for a slot, once fetched: .d (and .steps for a volume: each step's lit frame; .cut: how many
// frames it had where it was shortened).  One that failed is asked again, after 10 s: the players ask for it 25 times
// a second, and a slot whose file fails each time must not keep the queue busy
function stockOf(slot) {
  if (!ring.stock.has(slot)) {
    const leds = (ring.status && ring.status.leds) || 12;
    const p = rcall('GET', '/api/ring/stock/' + slot).then((r) => {
      if (!isVolume(slot)) { const s = stockDesign(r.text(), leds, 'Amazon\'s'); p.cut = s.cut; return s.d; }
      p.steps = r.text().split(/^step \d+\n/m).slice(1).map((t) => stockDesign(t, leds, '').d.frames[0].px);
      return volumeDemo(p.steps, null, leds);
    }).catch(() => { setTimeout(() => { if (ring.stock.get(slot) === p) ring.stock.delete(slot); }, 10000); return null; });
    p.then((d) => { p.d = d; });
    ring.stock.set(slot, p);
  }
  return ring.stock.get(slot);
}
// ... as the start of a design; shortened to what a design holds, it says so
async function stockStart(slot) {
  const p = stockOf(slot), d = await p;
  if (d && p.cut) toast(`Amazon's has ${p.cut} frames, a design ${d.frames.length} at most: the first ${d.frames.length} are taken`);
  return d;
}

const kindOf = (slot) => { const s = ring.status && ring.status.slots.find((x) => x.id === slot); return s ? s.kind : ''; };
const isVolume = (slot) => kindOf(slot) === 'volume' || kindOf(slot) === 'assist';

// What the ring shows for a volume: Amazon's steps going up and back down, in their own colours or in a design's
// (ring.c tinted: each LED the design's first frame there, as bright as the step has it)
function volumeDemo(steps, d, leds) {
  const tint = (px) => (d ? px.map((c, i) => { const v = Math.max(...c) / 255; return d.frames[0].px[i % d.leds].map((x) => Math.round(x * v)); }) : px);
  const fr = steps.map((px, i) => ({ ms: i === steps.length - 1 ? 900 : 220, px: tint(px) }));
  for (let i = steps.length - 2; i >= 0; i--) fr.push({ ms: 140, px: tint(steps[i]) });
  fr.push({ ms: 600, px: black(leds) });
  return { name: '', leds, loop: 0, frames: fr };
}

// What plays for slot s with design d (null: Amazon's); a volume takes the design's colours through the steps
function shownFor(slot, d) {
  const st = stockOf(slot);
  if (!d) return st.d || null;
  return isVolume(slot) ? (st.steps ? volumeDemo(st.steps, d, d.leds) : null) : d;
}

async function ringDo(what, fn) {
  try { await fn(); if (what) toast(what); } catch (e) { toast(errText(e)); }
  await ringLoad().catch((e) => toast(errText(e)));
}

async function assignAll(lines) { if (lines.length) await rcall('POST', '/api/ring/assign', lines.join('\n') + '\n'); }

// ---------------------------------------------------------------- the cards: which design shows for what, the library

function buildRing(el) {
  if (!ringBox) {
    ringBox = h('div', { class: 'ringd' });
    ringLoad().catch((e) => { ringBox.replaceChildren(h('div', { class: 'card card-pad' }, h('p', { class: 'help' }, 'Light ring designs: ' + errText(e)))); });
  }
  el.append(ringBox);
}

const placeOf = (id) => RING_PLACES.find((p) => p.id === id) || { id, group: 'Other', label: id };
const slotsOf = () => (ring.status ? ring.status.slots.filter((s) => s.available) : []);
const usedFor = (did) => slotsOf().filter((s) => s.design === did).map((s) => placeOf(s.id).label);

function ringRender() {
  if (!ringBox || !ring.status) return;
  if (!ring.status.slots.length) { ringBox.replaceChildren(); return; }   // no animations to design: the Echo Show's band
  if (!ring.status.installed) {
    ringBox.replaceChildren(h('div', { class: 'card' }, cardHead('Light ring animations', null, 'sun'),
      h('div', { class: 'callout info ringd-callout' }, icon('info'),
        h('p', {}, 'The ring\'s own animations are not installed on this Echo, so your designs cannot show either. The log says why (look for "light ring").'))));
    return;
  }
  ringBox.replaceChildren(actionsCard(ring.status), designsCard(ring.status));
}

function applyBar(st) {
  const [cls, text, help] = {
    applied: ['ok', 'On the ring', ''],
    pending: ['warn', 'Not on the ring yet', 'Apply writes them to the Echo and restarts its ring: a second without light.'],
    installing: ['', 'Installing…', 'The ring goes dark for a second.'],
    failed: ['bad', 'Not installed', st.error],
  }[st.state] || ['', st.state, ''];
  const btn = h('button', { class: 'primary', disabled: st.state === 'installing' || st.state === 'applied',
    onclick: async () => {
      btn.disabled = true;
      try { await rcall('POST', '/api/ring/apply'); } catch (e) { toast(errText(e)); }
      await ringStatus().catch(() => {});
    } }, 'Apply');
  return h('div', { class: 'ringd-apply ' + st.state }, h('span', { class: 'badge ' + cls }, text), h('span', { class: 'help' }, help), btn);
}

// What shows for each of the Echo's animations: a tile each, playing what the ring shows there; a click picks
function actionsCard(st) {
  const slots = slotsOf();
  const groups = [...new Set(slots.map((s) => placeOf(s.id).group))];
  const designOf = (s) => (s.design && ring.designs.get(s.design)) || null;
  const tile = (s) => {
    const p = placeOf(s.id), cv = sizedCanvas(72), d = designOf(s);
    const talker = s.id === 'listen' || s.id === 'end' ? RING_TALKER : null;
    play(cv, () => { const x = shownFor(s.id, designOf(s)); return x ? { d: x, talker } : null; });
    const changed = s.applied !== s.design;
    return h('button', { class: 'ringd-act' + (d ? ' custom' : ''), 'aria-haspopup': 'dialog', title: p.help || '',
      onclick: () => pickDesign({ slot: s.id, current: s.design, onPick: (v) => { if (v !== s.design) ringDo(null, () => assignAll([`${s.id} ${v}`])); } }) },
      h('span', { class: 'ringd-art' }, cv), h('span', { class: 'ringd-act-label' }, p.label),
      h('span', { class: 'ringd-act-pick' }, d ? d.name : 'Amazon\'s'),
      changed ? h('span', { class: 'ringd-dot', title: 'Changed: on the ring once you apply' }) : null);
  };
  return h('div', { class: 'card ringd-actions' },
    cardHead('Light ring animations', 'What the ring shows for each of the Echo\'s own states. Click one to pick Amazon\'s or one of your designs; a design can show for as many as you like.', 'sun',
      h('button', { class: 'small', title: 'Every animation as it is now, as a theme pack', onclick: saveRingAsPack }, icon('plus'), 'Save as pack')),
    h('div', { class: 'ringd-groups' }, groups.map((g) => h('div', { class: 'ringd-group' }, h('div', { class: 'ringd-group-head' }, g),
      h('div', { class: 'ringd-acts' }, slots.filter((s) => placeOf(s.id).group === g).map(tile))))),
    applyBar(st));
}

// Picking what one animation shows: Amazon's or a design, each playing as it would there.  current: a design id, 0 for
// Amazon's, null for "not in this pack" (with notInPack).  sections: [{ title, ids, sub }] (default: the top level,
// then each pack); onPick(value, section)
function pickDesign({ slot, current, onPick, notInPack = false, sections = null }) {
  const p = placeOf(slot), m = modal(p.label, 'ringd-choose');
  const talker = slot === 'listen' || slot === 'end' ? RING_TALKER : null;
  const opt = (v, name, get, sub, sec) => {
    const cv = sizedCanvas(76); if (get) play(cv, get); else drawRing(cv, black(12));
    return h('button', { class: 'ringd-opt' + (current === v && !(sec && sec.other) ? ' on' : ''), 'aria-pressed': String(current === v),
      onclick: () => { m.dlg.close(); onPick(v, sec); } },
      h('span', { class: 'ringd-art' + (v === null ? ' none' : '') }, cv), h('span', { class: 'ringd-name' }, name), sub ? h('span', { class: 'ringd-sub' }, sub) : null);
  };
  const dOpt = (sec) => (id) => { const d = ring.designs.get(id); return opt(id, d.name, () => { const x = shownFor(slot, d); return x && { d: x, talker }; },
    sec.sub || (isVolume(slot) ? 'its first frame\'s colours' : `${d.frames.length} frame${d.frames.length === 1 ? '' : 's'}`), sec); };
  sections = (sections || [{ title: 'Designs', ids: [...ring.designs.keys()].filter((id) => !ring.home.has(id)) },
    ...[...ring.packs].map(([pid, pk]) => ({ title: pk.name, ids: pk.has.filter((id) => ring.designs.has(id)), pack: pid }))]).filter((x) => x.ids.length);
  fill(m.body, p.help ? h('p', { class: 'help' }, p.help) : null,
    h('div', { class: 'ringd-opts' },
      notInPack ? opt(null, 'Not in this pack', null, 'stays as it is') : null,
      opt(0, 'Amazon\'s', () => { const d = shownFor(slot, null); return d && { d, talker }; }, 'as stock')),
    sections.map((sec) => [h('div', { class: 'sub-head ringd-opts-head' }, sec.pack ? [icon('box'), ' ', sec.title] : sec.title, sec.note ? h('span', { class: 'ringd-opts-note' }, ' · ' + sec.note) : null),
      h('div', { class: 'ringd-opts' }, sec.ids.map(dOpt(sec)))]),
    ring.designs.size ? null : h('p', { class: 'help' }, 'No designs yet: make one under "Designs".'));
  const recolour = isVolume(slot) || notInPack ? null : h('button', { title: 'A copy of Amazon\'s animation to give other colours', onclick: async () => {
    m.dlg.close();
    const d = await stockStart(slot);
    if (d) designer(0, { ...copyDesign(d), name: p.label + ' in colour' }, { useFor: slot, hue: true });
  } }, glyph('<circle cx="12" cy="12" r="8"/><path d="M12 4a8 8 0 0 1 0 16z" fill="currentColor"/>'), 'Recolour Amazon\'s…');
  m.foot.append(...[recolour, h('span', { class: 'grow' }), h('button', { onclick: () => m.dlg.close() }, 'Cancel')].filter(Boolean));
  m.show();
}

const glyph = (d) => h('span', { html: `<svg class="i" viewBox="0 0 24 24" aria-hidden="true">${d}</svg>`, style: 'display:inline-flex' });

function blankDesign(name) {
  const n = (ring.status && ring.status.leds) || 12;
  return { name: name || 'Design', leds: n, loop: 0, frames: [{ ms: 500, px: black(n) }] };
}

// Which animations a design shows for: any number of them at once
function useFor(id, d) {
  const m = modal('Use "' + d.name + '" for', 'ringd-use');
  const slots = slotsOf(), boxes = new Map();
  const groups = [...new Set(slots.map((s) => placeOf(s.id).group))];
  fill(m.body, h('p', { class: 'help' }, 'Ticked: the ring shows this design there. An animation can show one design; ticking one that shows another replaces it.'),
    groups.map((g) => h('div', { class: 'ringd-use-group' }, h('div', { class: 'sub-head' }, g), slots.filter((s) => placeOf(s.id).group === g).map((s) => {
      const cb = h('input', { type: 'checkbox', checked: s.design === id });
      boxes.set(s.id, cb);
      const other = s.design && s.design !== id && ring.designs.get(s.design);
      return h('label', { class: 'ringd-use-row' }, cb, h('span', {}, placeOf(s.id).label),
        h('span', { class: 'ringd-sub' }, other ? 'now: ' + other.name : s.design === id ? '' : 'now: Amazon\'s'));
    }))));
  m.foot.append(h('button', { onclick: () => m.dlg.close() }, 'Cancel'),
    h('button', { class: 'primary', onclick: async () => {
      const lines = [];
      for (const s of slots) {
        const on = boxes.get(s.id).checked;
        if (on && s.design !== id) lines.push(`${s.id} ${id}`);
        else if (!on && s.design === id) lines.push(`${s.id} 0`);
      }
      m.dlg.close();
      await ringDo(lines.length ? 'Saved. Apply to put it on the ring.' : null, () => assignAll(lines));
    } }, 'Save'));
  m.show();
}

function newDialog(packId = 0) {
  const m = modal('New design', 'ringd-choose');
  const opts = packId ? { onSaved: (nid) => addToPack(packId, [nid]) } : {};
  const start = async (slot) => {
    m.dlg.close();
    if (!slot) return designer(0, blankDesign('Design ' + (ring.designs.size + 1)), opts);
    const d = await stockStart(slot);
    designer(0, d ? { ...copyDesign(d), name: 'My ' + placeOf(slot).label.toLowerCase() } : blankDesign(), opts);
  };
  const opt = (slot) => {
    const cv = sizedCanvas(76);
    play(cv, () => { const d = stockOf(slot).d; return d && { d }; });
    return h('button', { class: 'ringd-opt', onclick: () => start(slot) }, h('span', { class: 'ringd-art' }, cv), h('span', { class: 'ringd-name' }, placeOf(slot).label), h('span', { class: 'ringd-sub' }, 'Amazon\'s'));
  };
  fill(m.body, h('p', { class: 'help' }, 'Start from an empty ring, or from one of Amazon\'s animations to change it. Either way it is a design of its own: pick where it shows afterwards.'),
    h('div', { class: 'ringd-opts' },
      h('button', { class: 'ringd-opt', onclick: () => start(null) }, h('span', { class: 'ringd-art' }, h('span', { class: 'ringd-plus' }, icon('plus'))), h('span', { class: 'ringd-name' }, 'Empty ring')),
      slotsOf().filter((s) => !isVolume(s.id)).map((s) => opt(s.id))));
  m.foot.append(h('button', { onclick: () => m.dlg.close() }, 'Cancel'));
  m.show();
}

// ---------------------------------------------------------------- the designer

// Three places, one job each.  The stage draws, and plays: one ring for both, the frame being drawn or the design
// playing (touching it pauses there).  The panel beside it changes what is drawn: the colour to paint with, and
// adjustments of this frame or of all of them, by one switch for all.  The timeline below is time: playing, how it
// repeats, the frames and what makes or moves them.  The foot keeps the design: delete, download, save.
// opts.useFor: a slot to use the design for once saved (ticked in the foot, untickable); opts.hue: start at the hue,
// for all frames
const SVG_NS = 'http://www.w3.org/2000/svg';
const svgEl = (t, a) => { const e = document.createElementNS(SVG_NS, t); for (const k in a) e.setAttribute(k, a[k]); return e; };

function designer(id, start, opts = {}) {
  const n = start.leds;
  const ed = { id, d: copyDesign(start), cur: 0, colour: [0, 0, 255], tool: 'paint', undo: [], redo: [], dirty: !id,
               turn: 0, talker: true, recent: [], playing: false, all: !!opts.hue, playAt: -1 };
  const m = modal('', 'ringd-dlg');

  // ---- history: each change one undo step (a stroke of the brush is one, a run of hue drags is one)
  const snap = () => JSON.stringify({ frames: ed.d.frames, loop: ed.d.loop, cur: ed.cur });
  const restore = (x) => { const o = JSON.parse(x); ed.d.frames = o.frames; ed.d.loop = o.loop; ed.cur = Math.min(o.cur, o.frames.length - 1); };
  const keep = () => { ed.undo.push(snap()); if (ed.undo.length > 80) ed.undo.shift(); ed.redo = []; ed.dirty = true; };
  const change = (fn) => { hueEnd(); keep(); fn(); paint(); };
  const undo = () => { hueEnd(); if (!ed.undo.length) return; ed.redo.push(snap()); restore(ed.undo.pop()); ed.dirty = true; paint(); };
  const redo = () => { hueEnd(); if (!ed.redo.length) return; ed.undo.push(snap()); restore(ed.redo.pop()); ed.dirty = true; paint(); };
  const frame = () => ed.d.frames[ed.cur];
  const glyphBtn = (cls, label, title, fn, ic) => h('button', { type: 'button', class: cls, title, 'aria-label': title || label, onclick: fn }, ic || null, label ? h('span', {}, label) : null);
  const G = {
    undo: '<path d="M9 14L4 9l5-5"/><path d="M4 9h10a6 6 0 0 1 0 12h-3"/>', redo: '<path d="M15 14l5-5-5-5"/><path d="M20 9H10a6 6 0 0 0 0 12h3"/>',
    left: '<path d="M4 12a8 8 0 1 0 2.3-5.6"/><path d="M4 4v4h4"/>', right: '<path d="M20 12a8 8 0 1 1-2.3-5.6"/><path d="M20 4v4h-4"/>',
    mirror: '<path d="M12 3v18"/><path d="M8 7l-4 5 4 5M16 7l4 5-4 5"/>', darker: '<circle cx="12" cy="12" r="7"/><path d="M12 5a7 7 0 0 0 0 14z" fill="currentColor"/>',
    rainbow: '<path d="M3 17a9 9 0 0 1 18 0"/><path d="M7 17a5 5 0 0 1 10 0"/>', play: '<path d="M7 4l13 8-13 8z" fill="currentColor"/>',
    pause: '<path d="M8 5v14M16 5v14"/>', prev: '<path d="M15 6l-6 6 6 6"/>', next: '<path d="M9 6l6 6-6 6"/>', copy: '<rect x="8" y="8" width="12" height="12" rx="2"/><path d="M16 8V5a1 1 0 0 0-1-1H5a1 1 0 0 0-1 1v10a1 1 0 0 0 1 1h3"/>',
    fill: '<circle cx="12" cy="12" r="8"/><circle cx="12" cy="12" r="4" fill="currentColor"/>', talker: '<path d="M12 3l5 8H7z" fill="currentColor"/><path d="M12 13v8"/>',
  };
  const ic = (k) => glyph(G[k]);

  // ---- the stage: the ring, a target per LED over it, the tools on its edge
  const edCv = sizedCanvas(360, 'ringd-paintcv');
  const svg = svgEl('svg', { viewBox: '-100 -100 200 200', class: 'ringd-canvas', role: 'img', 'aria-label': 'The ring: click or drag over its LEDs to paint them' });
  const pt = (r, a) => `${(Math.cos(a) * r).toFixed(2)} ${(Math.sin(a) * r).toFixed(2)}`;
  const R0 = 100 * (RING_R - RING_W * 1.1), R1 = 100 * (RING_R + RING_W * 1.1), half = Math.PI / n - 0.03;
  for (let i = 0; i < n; i++) {
    const a = ledAngle(i, n);
    svg.append(svgEl('path', { d: `M${pt(R1, a - half)} A${R1} ${R1} 0 0 1 ${pt(R1, a + half)} L${pt(R0, a + half)} A${R0} ${R0} 0 0 0 ${pt(R0, a - half)} Z`, class: 'ringd-led' }));
  }
  const ledAt = (e) => {
    const r = svg.getBoundingClientRect(), x = ((e.clientX - r.left) / r.width) * 200 - 100, y = ((e.clientY - r.top) / r.height) * 200 - 100;
    const d = Math.hypot(x, y);
    if (d < R0 - 14 || d > 100) return -1;
    let a = Math.atan2(y, x) + Math.PI / 2; if (a < 0) a += 2 * Math.PI;
    return Math.round((a * n) / (2 * Math.PI)) % n;
  };
  // the stage shows the design turned towards the talker (ed.turn): what is under the pointer is the design's LED
  // that many before it, as the Echo turns them
  const shownLed = (e) => { const j = ledAt(e); return j < 0 ? -1 : (j - ed.turn + n) % n; };
  let stroke = null;
  const apply = (i) => {
    const f = frame();
    if (ed.tool === 'pick') { setColour(f.px[i]); setTool('paint'); return; }
    const c = ed.tool === 'erase' ? [0, 0, 0] : ed.colour;
    if (ed.tool === 'fill') { f.px = f.px.map(() => c.slice()); return; }
    f.px[i] = c.slice();
  };
  svg.onpointerdown = (e) => {
    if (ed.playing) pause();                // drawing stops the playing, at the frame it showed
    const i = shownLed(e); if (i < 0) return;
    e.preventDefault(); try { svg.setPointerCapture(e.pointerId); } catch (x) { /* a pointer the browser no longer has */ }
    if (ed.tool === 'pick') { apply(i); return; }
    hueEnd(); keep(); stroke = new Set([i]); apply(i); remember(); paint();
  };
  svg.onpointermove = (e) => { if (!stroke || ed.tool === 'fill') return; const i = shownLed(e); if (i >= 0 && !stroke.has(i)) { stroke.add(i); apply(i); paint(); } };
  svg.onpointerup = svg.onpointercancel = () => { stroke = null; };
  const tools = [['paint', 'Paint', 'Paint LEDs (click or drag)', icon('pen')], ['fill', 'Fill', 'Fill the ring', ic('fill')],
    ['erase', 'Off', 'Switch LEDs off', icon('x')], ['pick', 'Pick', 'Take an LED\'s colour', icon('eye')]].map(([t, label, title, i]) =>
    h('button', { type: 'button', 'data-t': t, title, onclick: () => setTool(t) }, i, h('span', {}, label)));
  const setTool = (t) => { ed.tool = t; paintTools(); };
  const undoBtn = glyphBtn('ringd-onstage', null, 'Undo (Ctrl+Z)', undo, ic('undo'));
  const redoBtn = glyphBtn('ringd-onstage', null, 'Redo (Ctrl+Y)', redo, ic('redo'));
  const where = h('span', { class: 'ringd-where' });
  // The talker: the yellow arrow outside the ring, dragged round it (from LED to LED) or turned with the arrow keys.
  // The stage shows the design turned towards it, as the Echo turns listening; drawing on it turned still lands on the
  // design's own LEDs (shownLed)
  const arrowG = svgEl('g', { class: 'ringd-arrow', tabindex: '0', role: 'slider', 'aria-label': 'Where the talker is: drag it round the ring',
    'aria-valuemin': '0', 'aria-valuemax': String(n - 1) });
  const arrowTip = svgEl('title', {}); arrowTip.textContent = 'The talker: drag it round the ring, the design turns to them as on the Echo';
  arrowG.append(arrowTip, svgEl('circle', { r: 13, class: 'ringd-arrow-hit' }), svgEl('path', { d: 'M0 4 L-8 -10 L8 -10 Z', class: 'ringd-arrow-tri' }));
  svg.append(arrowG);
  const setTurn = (t) => { t = ((t % n) + n) % n; if (t !== ed.turn) { ed.turn = t; paint(); } };
  let dragging = false;
  arrowG.addEventListener('pointerdown', (e) => {
    e.stopPropagation(); e.preventDefault(); dragging = true; arrowG.classList.add('drag');
    try { arrowG.setPointerCapture(e.pointerId); } catch (x) { /* a pointer the browser no longer has */ }
  });
  arrowG.addEventListener('pointermove', (e) => {
    if (!dragging) return;
    const r = svg.getBoundingClientRect();
    let a = Math.atan2(e.clientY - r.top - r.height / 2, e.clientX - r.left - r.width / 2) + Math.PI / 2; if (a < 0) a += 2 * Math.PI;
    setTurn(Math.round((a * n) / (2 * Math.PI) - RING_TALKER));
  });
  const drop = () => { dragging = false; arrowG.classList.remove('drag'); };
  arrowG.addEventListener('pointerup', drop); arrowG.addEventListener('pointercancel', drop);
  arrowG.addEventListener('keydown', (e) => {
    const k = { ArrowRight: 1, ArrowDown: 1, ArrowLeft: -1, ArrowUp: -1 }[e.key];
    if (k) { e.preventDefault(); setTurn(ed.turn + k); } else if (e.key === 'Home') { e.preventDefault(); setTurn(0); }
  });
  const placeArrow = () => {
    const a = ledAngle(RING_TALKER + ed.turn, n);
    arrowG.setAttribute('transform', `translate(${pt(97, a)}) rotate(${(a * 180) / Math.PI + 90})`);
    arrowG.setAttribute('aria-valuenow', String(ed.turn));
    arrowG.classList.toggle('hidden', !ed.talker);
  };
  const turnOut = h('span', { class: 'ringd-turned' });
  const turnReset = glyphBtn('ringd-onstage', null, 'Back as drawn', () => setTurn(0), ic('undo'));
  const talkerBtn = glyphBtn('ringd-onstage ringd-talkerbtn', 'Talker', 'Show the talker: drag the yellow arrow round the ring, the design turns to them as the Echo turns listening',
    () => { ed.talker = !ed.talker; paint(); }, ic('talker'));
  const talkBox = h('div', { class: 'ringd-talk' }, talkerBtn, turnOut, turnReset);

  // ---- the colour to paint with
  const picker = h('input', { type: 'color', value: '#0000ff', class: 'ringd-well', 'aria-label': 'Any colour', title: 'Any colour',
    oninput: () => { setColour(hexRgb(picker.value.slice(1)), true); if (ed.tool === 'erase' || ed.tool === 'pick') setTool('paint'); }, onchange: () => remember() });
  const recentBox = h('div', { class: 'ringd-recent-sw' });
  const swatches = [];
  const swatchBtn = (hx) => {
    const b = h('button', { type: 'button', class: 'ringd-sw', style: `--c:${hx}`, title: hx, 'aria-label': 'Colour ' + hx,
      onclick: () => { setColour(hexRgb(hx.slice(1))); if (ed.tool === 'erase' || ed.tool === 'pick') setTool('paint'); } });
    b.dataset.hex = hx.slice(1).toUpperCase(); swatches.push(b);
    return b;
  };
  const setColour = (c, keepPicker) => { ed.colour = c.slice(); if (!keepPicker) picker.value = '#' + rgbHex(c).toLowerCase(); paintTools(); };
  const remember = () => {
    const k = rgbHex(ed.colour);
    if (RING_PRESETS.includes('#' + k.toLowerCase()) || ed.tool !== 'paint' && ed.tool !== 'fill') return;
    ed.recent = [k, ...ed.recent.filter((x) => x !== k)].slice(0, 7);
    recentBox.replaceChildren(...ed.recent.map((x) => swatchBtn('#' + x)));
    recentRow.classList.remove('hidden');
  };
  const recentRow = h('div', { class: 'ringd-recent hidden' }, h('span', { class: 'ringd-lbl' }, 'Recent'), recentBox);
  const paintTools = () => {
    tools.forEach((b) => { const on = b.dataset.t === ed.tool; b.classList.toggle('on', on); b.setAttribute('aria-pressed', String(on)); });
    const k = rgbHex(ed.colour);
    swatches.forEach((b) => b.classList.toggle('on', b.isConnected && b.dataset.hex === k));
    svg.dataset.tool = ed.tool;
  };

  // ---- adjustments: of this frame or of all frames, one switch for every one of them
  const each = (fn) => ed.d.frames.forEach((f, i) => { if (ed.all || i === ed.cur) fn(f, i); });
  const adjust = (fn) => change(() => each(fn));
  const rot = (k) => adjust((f) => { const p = f.px; f.px = p.map((_, i) => p[(i - k + n) % n]); });
  const wheel = (i) => {
    const h6 = ((i % n) / n) * 6, f = h6 - Math.floor(h6);
    return [[1, f, 0], [1 - f, 1, 0], [0, 1, f], [0, 1 - f, 1], [f, 0, 1], [1, 0, 1 - f]][Math.floor(h6) % 6].map((x) => Math.round(x * 255));
  };
  let hueBase = null;
  const hueIn = h('input', { type: 'range', min: '-180', max: '180', step: '1', value: '0', class: 'ringd-hue', 'aria-label': 'Hue',
    oninput: () => {
      if (!hueBase) { hueBase = snap(); ed.undo.push(hueBase); if (ed.undo.length > 80) ed.undo.shift(); ed.redo = []; }
      const deg = +hueIn.value, base = JSON.parse(hueBase).frames;
      each((f, i) => { f.px = base[i].px.map((c) => turnHue(c, deg)); });
      ed.dirty = true; hueOut.textContent = (deg > 0 ? '+' : deg < 0 ? '−' : '±') + Math.abs(deg) + '°'; paint();
    } });
  const hueOut = h('output', {}, '±0°');
  function hueEnd() { hueBase = null; hueIn.value = '0'; hueOut.textContent = '±0°'; }
  const scopeBtns = [['frame', 'This frame'], ['all', 'All frames']].map(([k, t]) =>
    h('button', { type: 'button', 'data-k': k, onclick: () => { hueEnd(); ed.all = k === 'all'; paint(); } }, t));
  const adj = (label, title, fn, i, cls) => glyphBtn('ringd-ghost' + (cls ? ' ' + cls : ''), label, title, fn, i);
  const hueRow = h('div', { class: 'ringd-hue-row' + (opts.hue ? ' hl' : ''), title: 'Turns every colour round the colour wheel; how light and how colourful each looks stays' },
    h('span', { class: 'ringd-lbl' }, 'Hue'), hueIn, hueOut);

  // ---- time: playing, how it repeats, the frames
  const player = play(edCv, () => ({ d: ed.d, turn: ed.turn, talker: null,         /* the arrow is the stage's own (arrowG) */
                                     still: ed.playing ? null : frame().px }));
  const playBtn = h('button', { type: 'button', class: 'ringd-play', onclick: () => (ed.playing ? pause() : resume()) });
  const resume = () => { ed.playing = true; player.t0 = performance.now(); tickHead(); paint(); };
  function pause() { ed.playing = false; if (ed.playAt >= 0) ed.cur = ed.playAt; ed.playAt = -1; paint(); }
  const tickHead = () => {                 // the frame playing, marked in the strip
    if (!ed.playing || !m.dlg.isConnected) return;
    const i = frameIndexAt(ed.d, performance.now() - player.t0);
    if (i !== ed.playAt) {
      const was = strip.children[ed.playAt]; if (was) was.classList.remove('head');
      ed.playAt = i;
      const now = strip.children[i]; if (now) { now.classList.add('head'); now.scrollIntoView({ block: 'nearest', inline: 'nearest' }); }
      where.textContent = i < 0 ? 'Over' : `Playing ${i + 1} / ${ed.d.frames.length}`;
    }
    requestAnimationFrame(tickHead);
  };
  const modes = [['all', 'Repeat', 'All frames again and again, as long as the Echo is in that state'],
    ['intro', 'Loop', 'The frames before the loop once, then from the loop on again and again'],
    ['once', 'Once', 'All frames once, then dark, even while the Echo is still in that state']].map(([k, t, title]) =>
    h('button', { type: 'button', 'data-m': k, title, onclick: () => change(() => {
      ed.d.loop = k === 'once' ? -1 : k === 'all' ? 0 : ed.d.loop > 0 ? ed.d.loop : Math.min(Math.max(1, ed.cur), ed.d.frames.length - 1); }) }, t));
  const loopHere = h('button', { type: 'button', class: 'ringd-ghost small-g', title: 'The loop starts at the selected frame',
    onclick: () => change(() => { ed.d.loop = ed.cur; }) }, '↻ Loop from here');
  const summary = h('span', { class: 'ringd-summary' });
  const strip = h('div', { class: 'ringd-strip', role: 'listbox', 'aria-label': 'Frames' });
  const max = ring.status.frames_max || 1000;
  const room = (k) => { if (ed.d.frames.length + k > max) { toast(`A design holds ${max} frames at most`); return false; } return true; };
  const insert = (add) => { ed.d.frames.splice(ed.cur + 1, 0, ...add); if (ed.d.loop > ed.cur) ed.d.loop += add.length; };
  const addAfter = (f) => { if (room(1)) change(() => { insert([f]); ed.cur++; }); };
  const del = () => {
    if (ed.d.frames.length < 2) return;
    change(() => { ed.d.frames.splice(ed.cur, 1); if (ed.d.loop > ed.cur || ed.d.loop >= ed.d.frames.length) ed.d.loop = Math.max(0, ed.d.loop - 1); ed.cur = Math.min(ed.cur, ed.d.frames.length - 1); });
  };
  // the loop's start goes with its frame (the strip marks the frame), unless that would make it 0: all repeating
  const move = (k) => {
    const j = ed.cur + k; if (j < 0 || j >= ed.d.frames.length) return;
    change(() => {
      const fr = ed.d.frames, lp = ed.d.loop; [fr[ed.cur], fr[j]] = [fr[j], fr[ed.cur]];
      if (lp > 0 && lp === ed.cur && j > 0) ed.d.loop = j; else if (lp > 0 && lp === j && ed.cur > 0) ed.d.loop = ed.cur;
      ed.cur = j;
    });
  };
  const spin = () => { if (room(n - 1)) change(() => {
    const f = frame(); insert(Array.from({ length: n - 1 }, (_, k) => ({ ms: f.ms, px: f.px.map((_, i) => f.px[(i - k - 1 + n * 2) % n].slice()) }))); }); };
  const fade = () => {                      // 6 frames blending this one into the next
    const k = 6, a = frame(), b = ed.d.frames[(ed.cur + 1) % ed.d.frames.length];
    if (room(k)) change(() => insert(Array.from({ length: k }, (_, x) => ({ ms: Math.max(1, Math.round(a.ms / (k + 1))),
      px: a.px.map((c, i) => c.map((v, ch) => Math.round(v + ((b.px[i][ch] - v) * (x + 1)) / (k + 1)))) }))));
  };
  const breathe = () => {                   // 11 frames dimming to 15 % and back
    const k = 6, a = frame();
    if (room(2 * k - 1)) change(() => insert(Array.from({ length: 2 * k - 1 }, (_, x) => { const v = 0.5 + 0.5 * Math.cos((Math.PI * (x + 1)) / k);
      return { ms: a.ms, px: a.px.map((c) => c.map((y) => Math.round(y * (0.15 + 0.85 * v)))) }; })));
  };
  const msIn = h('input', { type: 'number', min: '1', max: '60000', class: 'ringd-num-in', 'aria-label': 'Milliseconds this frame shows',
    onchange: () => { const v = Math.max(1, Math.min(60000, Math.round(+msIn.value) || 1)); change(() => { frame().ms = v; }); } });
  const frameLbl = h('span', { class: 'ringd-framelbl' });

  // ---- the foot: the design itself
  const name = h('input', { type: 'text', value: ed.d.name, maxlength: '47', 'aria-label': 'Name', id: 'ringd-name', class: 'ringd-title',
    oninput: () => { ed.d.name = fitInput(name, 47); ed.dirty = true; } });
  const uses = h('span', { class: 'ringd-uses-line' });
  const useCb = opts.useFor ? h('input', { type: 'checkbox', checked: true }) : null;
  const useRow = useCb ? h('label', { class: 'ringd-check ringd-usefor' }, useCb, 'Use it for ' + placeOf(opts.useFor).label) : null;
  // one save at a time: a second click while the first is on its way (a new design: id still 0) would make two
  let saving = null;
  const save = (asNew) => saving || (saving = saveNow(asNew).finally(() => { saving = null; }));
  const saveNow = async (asNew) => {
    ed.d.name = name.value.trim() || 'Design';
    try {
      const r = (await rcall('POST', '/api/ring/design/' + (asNew ? 0 : ed.id), designText(ed.d))).json();
      ed.id = r.id; ed.dirty = false;             // saved: a failing assign below must not make the next save a second copy
      delBtn.classList.remove('hidden');
      if (useCb && useCb.checked) { await rcall('POST', '/api/ring/assign', `${opts.useFor} ${r.id}\n`); useRow.remove(); }
      const live = (!asNew && usedFor(r.id).length) || (useCb && useCb.checked);
      toast(asNew ? 'Saved as a new design' : live ? 'Saved. Apply to see it on the ring.' : 'Saved');
      if (opts.onSaved) opts.onSaved(r.id);
    } catch (e) { toast(errText(e)); return false; }
    ringLoad().then(paint).catch(() => {});
    return true;
  };
  const delBtn = h('button', { type: 'button', class: 'danger' + (ed.id ? '' : ' hidden'), onclick: async () => {
    const u = usedFor(ed.id), pk = ring.home.has(ed.id) ? [`"${ring.packs.get(ring.home.get(ed.id)).name}"`] : [];
    if (!(await ask({ title: 'Delete this design?', danger: true, ok: 'Delete',
      body: [`"${ed.d.name}" is gone for good.` + (u.length ? ` ${u.join(', ')} go${u.length === 1 ? 'es' : ''} back to Amazon's.` : '')
        + (pk.length ? ` It leaves the pack${pk.length === 1 ? '' : 's'} ${pk.join(', ')}.` : '')] }))) return;
    ed.dirty = false; m.dlg.close();
    ringDo('Deleted', () => rcall('POST', '/api/ring/delete/' + ed.id));
  } }, icon('trash'), 'Delete');
  const download = h('button', { type: 'button', title: 'Save it as a file, to upload on another Echo', onclick: () => {
    const a = h('a', { href: URL.createObjectURL(new Blob([designText(ed.d)], { type: 'text/plain' })), download: (name.value.trim() || 'design').replace(/[^\w.-]+/g, '_') + '.txt' });
    document.body.append(a); a.click(); a.remove(); setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  } }, icon('down'), 'Download');

  function paint() {
    const f = frame();
    if (!ed.playing) drawRing(edCv, turned(f.px, ed.turn));
    placeArrow();
    const loopAt = ed.d.loop;
    strip.replaceChildren(...ed.d.frames.map((fr, i) => h('button', {
      type: 'button', class: 'ringd-fr' + (i === ed.cur ? ' on' : '') + (i === loopAt && loopAt > 0 ? ' loopstart' : '') + (loopAt >= 0 && i >= loopAt ? ' inloop' : '')
        + (ed.playing && i === ed.playAt ? ' head' : ''),
      role: 'option', 'aria-selected': String(i === ed.cur), title: `Frame ${i + 1}: ${fr.ms} ms`, onclick: () => { if (ed.playing) pause(); ed.cur = i; paint(); } },
      h('span', { class: 'ringd-mini', style: `--ring-bg:${frameCss(fr.px)}` }), h('span', { class: 'ringd-fr-no' }, String(i + 1)), h('span', { class: 'ringd-ms' }, `${fr.ms}`))));
    const on = strip.children[ed.cur]; if (on && on.scrollIntoView && !ed.playing) on.scrollIntoView({ block: 'nearest', inline: 'nearest' });
    if (document.activeElement !== msIn) msIn.value = String(f.ms);
    const mode = ed.d.loop < 0 ? 'once' : ed.d.loop === 0 ? 'all' : 'intro';
    modes.forEach((b) => { const o = b.dataset.m === mode; b.classList.toggle('on', o); b.setAttribute('aria-pressed', String(o)); });
    loopHere.classList.toggle('hidden', mode !== 'intro');
    loopHere.disabled = ed.cur === 0 || ed.cur === ed.d.loop;
    const nf = ed.d.frames.length, tot = seconds(totalMs(ed.d));
    summary.textContent = `${nf} frame${nf === 1 ? '' : 's'} · ${tot} · ` + (mode === 'once' ? 'plays once, then dark'
      : mode === 'all' ? 'repeats' : `${ed.d.loop === 1 ? 'frame 1' : 'frames 1–' + ed.d.loop} once, then ${ed.d.loop + 1 === nf ? 'frame ' + nf : (ed.d.loop + 1) + '–' + nf} repeat`);
    frameLbl.textContent = `Frame ${ed.cur + 1}`;
    if (!ed.playing) where.textContent = `Frame ${ed.cur + 1} / ${nf}`;
    playBtn.replaceChildren(ed.playing ? ic('pause') : ic('play'), h('span', {}, ed.playing ? 'Pause' : 'Play'));
    playBtn.classList.toggle('on', ed.playing);
    m.dlg.classList.toggle('playing', ed.playing);
    talkerBtn.classList.toggle('on', ed.talker); talkerBtn.setAttribute('aria-pressed', String(ed.talker));
    turnOut.textContent = !ed.talker ? '' : ed.turn ? `turned ${ed.turn} LED${ed.turn === 1 ? '' : 's'}` : 'drag the arrow';
    turnReset.classList.toggle('hidden', !ed.turn);
    scopeBtns.forEach((b) => { const o = (b.dataset.k === 'all') === ed.all; b.classList.toggle('on', o); b.setAttribute('aria-pressed', String(o)); });
    const u = ed.id ? usedFor(ed.id) : [];
    uses.textContent = u.length ? 'Shows for ' + u.join(', ') : '';
    undoBtn.disabled = !ed.undo.length; redoBtn.disabled = !ed.redo.length;
    paintTools();
  }

  // ---- put together
  const sect = (title, extra, ...kids) => h('section', { class: 'ringd-sect' }, h('div', { class: 'ringd-sect-head' }, h('h3', {}, title), extra || null), ...kids);
  m.head.querySelector('h2').replaceChildren(name);      // the dialog's title is the design's name, edited in place
  fill(m.body,
    h('div', { class: 'ringd-ed' },
      h('div', { class: 'ringd-stage' },
        h('div', { class: 'ringd-dock', role: 'group', 'aria-label': 'Tool' }, tools),
        h('div', { class: 'ringd-stage-top' }, where, h('span', { class: 'grow' }), undoBtn, redoBtn),
        h('div', { class: 'ringd-paint' }, edCv, svg),
        talkBox),
      h('aside', { class: 'ringd-panel' },
        sect('Colour', null,
          h('div', { class: 'ringd-palette' }, h('label', { class: 'ringd-wellwrap', title: 'Any colour' }, picker), RING_PRESETS.map(swatchBtn)),
          recentRow),
        sect('Adjust', h('div', { class: 'seg ringd-scope', role: 'group', 'aria-label': 'Adjust' }, scopeBtns),
          h('div', { class: 'ringd-grid2' },
            adj('Turn left', 'Turn one LED anticlockwise', () => rot(-1), ic('left')),
            adj('Turn right', 'Turn one LED clockwise', () => rot(1), ic('right')),
            adj('Darker', 'Every LED darker', () => adjust((f) => { f.px = f.px.map((c) => c.map((v) => Math.round(v * 0.75))); }), ic('darker')),
            adj('Brighter', 'Every LED brighter', () => adjust((f) => { f.px = f.px.map((c) => c.map((v) => Math.min(255, Math.round(v * 1.33)))); }), icon('sun')),
            adj('Mirror', 'Mirrored through the talker', () => adjust((f) => { const p = f.px, ax = Math.round(RING_TALKER * 2); f.px = p.map((_, i) => p[((ax - i) % n + n) % n]); }), ic('mirror')),
            adj('Rainbow', 'A colour wheel round the ring', () => adjust((f) => { f.px = f.px.map((_, i) => wheel(i)); }), ic('rainbow')),
            adj('Clear', 'Every LED off', () => adjust((f) => { f.px = black(n); }), icon('x'), 'wide')),
          hueRow))),
    h('div', { class: 'ringd-timeline' },
      h('div', { class: 'ringd-tl-bar' }, playBtn,
        h('div', { class: 'seg ringd-modes', role: 'group', 'aria-label': 'How it repeats' }, modes), loopHere,
        h('span', { class: 'grow' }), summary),
      strip,
      h('div', { class: 'ringd-tl-bar ringd-tl-frame' },
        frameLbl, h('label', { class: 'ringd-inline' }, msIn, 'ms'),
        h('button', { type: 'button', class: 'link', title: 'Every frame shows this long', onclick: () => { const v = frame().ms; change(() => ed.d.frames.forEach((x) => { x.ms = v; })); } }, 'for all'),
        h('span', { class: 'ringd-sep' }),
        adj('Duplicate', 'A copy of this frame after it', () => addAfter({ ms: frame().ms, px: frame().px.map((c) => c.slice()) }), ic('copy')),
        adj('Empty', 'A dark frame after this one', () => addAfter({ ms: frame().ms, px: black(n) }), icon('plus')),
        adj(null, 'Move this frame earlier', () => move(-1), ic('prev')),
        adj(null, 'Move this frame later', () => move(1), ic('next')),
        adj(null, 'Delete this frame', del, icon('trash'), 'danger'),
        h('span', { class: 'grow' }),
        h('span', { class: 'ringd-lbl' }, 'Make from it'),
        adj('Spin', `${n - 1} frames after it, each turned one LED further`, spin),
        adj('Fade', '6 frames blending it into the next', fade),
        adj('Breathe', '11 frames dimming and coming back', breathe))));
  m.foot.append(...[delBtn, download, uses, useRow, h('span', { class: 'grow' })].filter(Boolean),
    h('button', { type: 'button', onclick: () => save(true), title: 'A design of its own; the one saved before stays as it is' }, 'Save as new'),
    h('button', { type: 'button', class: 'primary', onclick: async () => { if (await save(false)) m.dlg.close(); } }, 'Save'));

  // Closing with changes not saved asks first.  Keys: space plays and pauses, arrows step through the frames
  const leave = async () => {
    if (ed.dirty && !(await ask({ title: 'Close without saving?', body: ['What you drew since the last save is lost.'], ok: 'Close', danger: true }))) return;
    ed.dirty = false; m.dlg.close();
  };
  m.dlg.addEventListener('cancel', (e) => { if (ed.dirty) { e.preventDefault(); leave(); } });
  m.head.querySelector('.icon-btn').onclick = leave;
  m.dlg.addEventListener('keydown', (e) => {
    const t = e.target, typing = /^(INPUT|SELECT|TEXTAREA)$/.test(t.tagName) && t.type !== 'range' && t.type !== 'checkbox';
    if (typing || t.type === 'range' || t.closest('.ringd-arrow')) return;
    const k = e.key.toLowerCase();
    if ((e.ctrlKey || e.metaKey) && k === 'z') { e.preventDefault(); e.shiftKey ? redo() : undo(); }
    else if ((e.ctrlKey || e.metaKey) && k === 'y') { e.preventDefault(); redo(); }
    else if (k === ' ' && t.tagName !== 'BUTTON') { e.preventDefault(); ed.playing ? pause() : resume(); }
    else if (t.closest('.seg')) return;
    else if (e.key === 'ArrowLeft') { e.preventDefault(); if (ed.playing) pause(); ed.cur = Math.max(0, ed.cur - 1); paint(); }
    else if (e.key === 'ArrowRight') { e.preventDefault(); if (ed.playing) pause(); ed.cur = Math.min(ed.d.frames.length - 1, ed.cur + 1); paint(); }
  });
  paint();
  m.show();
  if (opts.hue) hueIn.focus(); else name.blur();
}

// ---------------------------------------------------------------- copied to other Echos (app.js "Copy settings")

// What the copy compares on each Echo, besides its settings: for each animation of the ring the design it shows
// ("ring:<slot>" = the design's mark, "stock" for Amazon's): that is a setting.  The designs and theme packs themselves
// are not; they go with the models (app.js "Copy models", from byMark and packs).  A design is known by what it is, its text as the Echo keeps
// it, name included: ids are each Echo's own.  null: an Echo without designs (older hassmic, or the ring's own
// animations not installed)
const designMark = (text) => hex(blake2b(8, null, enc.encode(text)));

async function ringSnapshot(e) {
  let st, lib;
  try { st = (await e.call('GET', '/api/ring')).json(); lib = (await e.call('GET', '/api/ring/designs')).text(); } catch (x) { return null; }
  if (!st.installed || !st.slots.length) return null;
  const snap = { byMark: new Map(), byId: new Map(), values: new Map() };
  const parts = ('\n' + lib).split(/\ndesign (\d+)\n/);
  for (let i = 1; i + 1 < parts.length; i += 2) {
    const text = parts[i + 1].replace(/\n+$/, '\n'), mark = designMark(text), d = designParse(text);
    snap.byId.set(+parts[i], mark);
    if (!snap.byMark.has(mark)) snap.byMark.set(mark, { id: +parts[i], name: d.name, text });
  }
  for (const s of st.slots) if (s.available) snap.values.set('ring:' + s.id, s.design ? snap.byId.get(s.design) || 'stock' : 'stock');
  // its theme packs, each as it travels (its designs inside), known by that text; null: an Echo without packs
  snap.packs = new Map(); snap.inPack = new Set();         // inPack: marks of designs in a pack (they travel in it)
  const designs = new Map([...snap.byId].map(([id, mk]) => [id, designParse(snap.byMark.get(mk).text)]));
  snap.toId = new Map([...snap.byMark].map(([mk, x]) => [mk, x.id]));
  try {
    const t = (await e.call('GET', '/api/ring/packs')).text(), pk = ('\n' + t).split(/\npack (\d+)\n/);
    for (let i = 1; i + 1 < pk.length; i += 2) {
      const p = packParse(pk[i + 1]), carried = packCarryText(p.legacy ? p : packCarry(p, designs));
      snap.packs.set(designMark(carried), { name: p.name, text: carried });
      for (const did of p.has) snap.inPack.add(snap.byId.get(did));
    }
  } catch (x) { snap.packs = null; }
  return snap;
}

// The rows' words: which group, what the row is, what a value is (marks resolved through every Echo's library)
function ringRowMeta(n, libs) {
  const name = (mark) => { for (const l of libs) if (l && l.byMark.has(mark)) return l.byMark.get(mark).name; return 'a design'; };
  if (n.startsWith('ring:')) {
    return { group: 'Lights', label: 'Ring: ' + placeOf(n.slice(5)).label,
             pretty: (v) => (v === 'stock' ? 'Amazon\'s' : `"${name(v)}"`) };
  }
  return null;
}

// Copies the ticked ring rows of `mine` (this Echo's snapshot) to one Echo: the designs they show that it lacks first
// (uploaded as new ones), then the animations, then Apply there.  Returns notes for what did not work
async function ringCopy(e, names, mineSnap, theirs) {
  const notes = [], have = new Map(theirs ? [...theirs.byMark].map(([m, x]) => [m, x.id]) : []);
  const ensure = async (mark) => {
    if (have.has(mark)) return have.get(mark);
    const src = mineSnap.byMark.get(mark);
    if (!src) throw new Error('design not found here');
    const id = (await e.call('POST', '/api/ring/design/0', src.text)).json().id;
    have.set(mark, id);
    return id;
  };
  const lines = [];
  for (const n of names) {
    try {
      if (n.startsWith('ring:')) {
        const v = mineSnap.values.get(n);
        lines.push(`${n.slice(5)} ${v === 'stock' ? 0 : await ensure(v)}`);
      }
    } catch (x) { notes.push(errText(x)); }
  }
  if (lines.length) {
    try { await e.call('POST', '/api/ring/assign', lines.join('\n') + '\n'); await e.call('POST', '/api/ring/apply'); }
    catch (x) { notes.push('ring: ' + errText(x)); }
  }
  return notes;
}


// ---------------------------------------------------------------- designs in folders: theme packs

// A theme pack is a folder of the library (ringdesign.h, version 2): its designs (has) and which of them each of its
// animations shows: { name, author, about, shot, has: [id], uses: [[slot, id]] }, id 0 for Amazon's.  A design is in
// one pack at most (the Echo refuses another).  Shared (a .ringpack file, a copy to another Echo) a pack carries its
// designs (version 1): { ..., designs: [design], uses: [[slot, n]] }, n the 1-based design.
const PACK_SHOTS = [['halo', 'Halo'], ['lineup', 'Line-up'], ['sequence', 'Sequence'], ['trail', 'Trail']];
const SHOT_OF = { hero: 'halo', grid: 'lineup', glow: 'halo', film: 'sequence', rings: 'lineup', matrix: 'sequence' };   // earlier styles' ids, kept in packs
const designMarkOf = (d) => designMark(designText(d));

function packParse(text) {
  const p = { name: 'Pack', author: '', about: '', shot: 'halo', has: [], uses: [] };
  const lines = text.replace(/\r/g, '').split('\n');
  p.legacy = lines[0] === 'hassmic ring pack 1';
  if (p.legacy) p.designs = [];
  for (let i = 1; i < lines.length; i++) {
    const l = lines[i];
    if (l.startsWith('name ')) p.name = l.slice(5);
    else if (l.startsWith('author ')) p.author = l.slice(7);
    else if (l.startsWith('about ')) p.about = l.slice(6);
    else if (l.startsWith('shot ')) p.shot = l.slice(5);
    else if (l.startsWith('has ')) p.has.push(+l.slice(4));
    else if (l === 'design' && p.legacy) { const j = lines.indexOf('end', i + 1); p.designs.push(designParse(lines.slice(i + 1, j < 0 ? lines.length : j).join('\n'))); i = j < 0 ? lines.length : j; }
    else if (l.startsWith('use ')) { const [, slot, w] = l.split(' '); p.uses.push([slot, w === 'stock' ? 0 : +w]); }
  }
  return p;
}

const packHead = (p) => [['name', p.name], ['author', p.author], ['about', p.about], ['shot', p.shot]].filter(([, v]) => v).map(([k, v]) => `${k} ${v}\n`).join('');
// as the Echo keeps it
const packText = (p) => 'hassmic ring pack 2\n' + packHead(p) + p.has.map((id) => `has ${id}\n`).join('') + p.uses.map(([sl, id]) => `use ${sl} ${id || 'stock'}\n`).join('');
// with its designs, to share: byId is the library it names (id -> design)
function packCarry(p, byId) {
  const designs = [], at = new Map();
  for (const id of p.has) { const d = byId.get(id); if (d) { designs.push(d); at.set(id, designs.length); } }
  return { ...p, legacy: true, designs, uses: p.uses.filter(([, id]) => !id || at.has(id)).map(([sl, id]) => [sl, id ? at.get(id) : 0]) };
}
const packCarryText = (c) => 'hassmic ring pack 1\n' + packHead(c) + c.designs.map((d) => `design\n${designText(d)}end\n`).join('')
  + c.uses.map(([sl, n]) => `use ${sl} ${n || 'stock'}\n`).join('');

// What the Echo refuses of a design or a pack (ringdesign.c), in words, or null: told before anything is sent, since
// an import refused half way leaves designs behind.  Fields in bytes of UTF-8, no control characters
const fieldBad = (v, max) => enc.encode(v).length > max || /[\x00-\x1f\x7f]/.test(v);
function designBad(d) {
  const max = (ring.status && ring.status.frames_max) || 1000;
  if (fieldBad(d.name, 47)) return `"${d.name}": a design's name is 47 bytes at most`;
  if (!d.frames.length || d.frames.length > max) return `"${d.name}": 1 to ${max} frames`;
  if (!(d.leds >= 1 && d.leds <= 24) || d.loop >= d.frames.length
      || d.frames.some((f) => f.px.length !== d.leds || !(f.ms >= 1 && f.ms <= 60000))) return `"${d.name}": not a design the Echo takes`;
  return null;
}
function packBad(p) {
  if (!p.name || fieldBad(p.name, 47)) return 'a pack\'s name is 1 to 47 bytes';
  if (fieldBad(p.author || '', 47)) return 'a pack\'s author is 47 bytes at most';
  if (fieldBad(p.about || '', 199)) return 'a pack\'s "about" is 199 bytes at most';
  if (p.shot && !/^[a-z]{1,15}$/.test(p.shot)) return 'not a packshot style';
  return null;
}

// designs uploaded for a pack the Echo then did not take: deleted again, or each try leaves copies in the library
async function dropDesigns(ids, post = (path) => rcall('POST', path)) {
  for (const id of ids) try { await post('/api/ring/delete/' + id, ''); } catch (e) { /* the Echo away: nothing more to do here */ }
}

// A pack that carries its designs into an Echo: its designs uploaded there as designs of their own, then the pack as
// a folder of them (as pack id, or a new one); refused, none of it stays.  post(path, body) talks to that Echo
async function importPack(c, post, id = 0) {
  const bad = packBad(c) || c.designs.map(designBad).find(Boolean);
  if (bad) throw new Error(bad);
  const ids = [];
  try {
    for (const d of c.designs) ids.push((await post('/api/ring/design/0', designText(d))).json().id);
    const p = { ...c, legacy: false, has: ids, uses: c.uses.filter(([, n]) => !n || ids[n - 1]).map(([sl, n]) => [sl, n ? ids[n - 1] : 0]) };
    return (await post('/api/ring/pack/' + id, packText(p))).json().id;
  } catch (e) { await dropDesigns(ids, post); throw e; }
}

// Packs of older builds made folders: designs inside one converted into the library; one naming designs without
// holding them gets them; a design two packs had stays with the first, the second gets a copy.  true if anything
// changed.  A pack the Echo refuses is left as it is and not tried again this session (each load would)
async function fixPacks() {
  const post = (path, body) => rcall('POST', path, body), claimed = new Map();
  let changed = false;
  for (const [pid, p] of ring.packs) if (!p.legacy) for (const did of p.has) if (!claimed.has(did)) claimed.set(did, pid);
  for (const [pid, p] of ring.packs) {
    if (ring.unfixed.has(pid)) continue;
    const copies = [];
    try {
      if (p.legacy) { await importPack(p, post, pid); changed = true; continue; }
      const want = [...new Set([...p.has, ...p.uses.map(([, x]) => x).filter(Boolean)])].filter((x) => ring.designs.has(x));
      const remap = new Map();
      for (const did of want) {
        if (claimed.has(did) && claimed.get(did) !== pid) {
          const nid = (await post('/api/ring/design/0', designText(ring.designs.get(did)))).json().id;
          copies.push(nid); remap.set(did, nid);
        } else claimed.set(did, pid);
      }
      const has = want.map((x) => remap.get(x) || x);
      if (remap.size || has.length !== p.has.length || has.some((x, i) => x !== p.has[i])) {
        await post('/api/ring/pack/' + pid, packText({ ...p, has, uses: p.uses.filter(([, x]) => !x || ring.designs.has(x)).map(([sl, x]) => [sl, remap.get(x) || x]) }));
        changed = true;
      }
    } catch (e) {
      ring.unfixed.add(pid);
      await dropDesigns(copies);
      toast(`Pack "${p.name}" from an older version: ${errText(e).replace(/^Error: /, '')}`);
    }
  }
  return changed;
}

const savePack = (id, p) => rcall('POST', '/api/ring/pack/' + id, packText(p)).then((r) => r.json().id);
// a pack saved with designs copied in for it: not saved, the copies go again
const savePackWith = (id, p, copies) => savePack(id, p).catch(async (e) => { await dropDesigns(copies); throw e; });

// designs into a pack: out of the packs they were in (whose animations showing them go back to not in it), each of
// those saved once with all that left it
async function addToPack(pid, ids) {
  const leave = new Map();
  for (const did of ids) {
    const from = ring.home.get(did);
    if (from && from !== pid) leave.set(from, [...(leave.get(from) || []), did]);
  }
  for (const [from, out] of leave) {
    const q = ring.packs.get(from);
    await savePack(from, { ...q, has: q.has.filter((x) => !out.includes(x)), uses: q.uses.filter(([, x]) => !out.includes(x)) });
  }
  const p = ring.packs.get(pid);
  await savePack(pid, { ...p, has: [...new Set([...p.has, ...ids])] });
  await ringLoad();
}

// what a pack does, in words
function packSays(p) {
  const withD = p.uses.filter(([, id]) => id).length, stock = p.uses.length - withD, n = p.has.length;
  return [`${n} design${n === 1 ? '' : 's'}`, withD ? `${withD} animation${withD === 1 ? '' : 's'}` : '', stock ? `${stock} as Amazon's` : ''].filter(Boolean).join(' · ');
}

// ---- the packshot: drawn from the pack itself, in one of PACK_SHOTS' styles, 16:10.  Its animations on Echo Dots
// (three-quarter view, or the top seen from above where many are small) on near black, each named for what it shows,
// their light falling on the wall, the fabric and the table in its own hues, a fine grain, the name

// a design's brightest frame: what a still picture of it shows
function brightest(d) {
  let best = d.frames[0], v = -1;
  for (const f of d.frames) { const t = f.px.reduce((a, c) => a + c[0] + c[1] + c[2], 0); if (t > v) { v = t; best = f; } }
  return best.px;
}

// the band of light of one frame, radius r, width w, on a transparent canvas: each LED a spot running into the next,
// as drawRing's, without the Echo round it.  k < 1: seen from the side (tiltLayer), hgt > 0: down that height
// (bandLayer); each kept in the cache as a layer of its own
const lightCache = pixelCache(6e6);
function lightLayer(px, r, w, k = 1, hgt = 0) {
  hgt = Math.round(hgt); r = Math.round(r); w = Math.round(w * 2) / 2;     // drawn at what the key says: a kept layer fits every r it is found for
  const key = `${r}|${w}|${k}|${hgt}|${px.map((c) => (lit(c) ? rgbHex(c) : '0')).join()}`;
  let cv = lightCache.get(key);
  if (cv) return cv;
  if (hgt >= 1) cv = bandLayer(lightLayer(px, r, w, k), hgt);
  else if (k !== 1) cv = tiltLayer(lightLayer(px, r, w), k);
  if (cv) { lightCache.set(key, cv); return cv; }
  const n = px.length || 12, size = Math.ceil(2 * r + 2 * w + 4), c = size / 2, slices = 180;
  cv = document.createElement('canvas'); cv.width = cv.height = size;
  const x = cv.getContext('2d');
  const stop = (u) => {
    const col = lightAt(px, u), m = Math.max(col[0], col[1], col[2]);
    if (m < 2) return 'rgba(0,0,0,0)';
    return `rgba(${Math.round((col[0] / m) * 255)},${Math.round((col[1] / m) * 255)},${Math.round((col[2] / m) * 255)},${Math.min(1, m / 255).toFixed(3)})`;
  };
  if (x.createConicGradient) {
    const g = x.createConicGradient(-Math.PI / 2, c, c);
    for (let k = 0; k <= slices; k++) g.addColorStop(k / slices, stop(((k % slices) * n) / slices));
    x.beginPath(); x.arc(c, c, r, 0, 2 * Math.PI); x.lineWidth = w; x.strokeStyle = g; x.stroke();
  } else {
    x.lineWidth = w;
    for (let k = 0; k < slices; k++) { const a0 = -Math.PI / 2 + (k * 2 * Math.PI) / slices; x.beginPath(); x.arc(c, c, r, a0, a0 + (2 * Math.PI) / slices + 0.01); x.strokeStyle = stop((k * n) / slices); x.stroke(); }
  }
  lightCache.set(key, cv);
  return cv;
}

// a light layer seen from the side: squashed to k of its height (an Echo's top in three-quarter view)
function tiltLayer(L, k) {
  const cv = document.createElement('canvas'); cv.width = L.width; cv.height = Math.max(1, Math.round(L.height * k));
  cv.getContext('2d').drawImage(L, 0, 0, cv.width, cv.height);
  return cv;
}

// a tilted layer drawn down a height of hgt: the ring seen as a band round the Echo's side, not a line on its top
function bandLayer(L, hgt) {
  const cv = document.createElement('canvas'); cv.width = L.width; cv.height = L.height + hgt;
  const g = cv.getContext('2d');
  for (let i = 0; i <= hgt; i++) g.drawImage(L, 0, i);
  return cv;
}

// light drawn onto a packshot: the band, its bloom wide and close, and a whiter core where it is bright; k < 1 tilts it,
// hgt > 0 runs it down that many pixels from cy
function drawLight(x, px, cx, cy, r, w, alpha = 1, k = 1, hgt = 0) {
  const L = lightLayer(px, r, w, k, hgt), at = (c) => x.drawImage(c, cx - c.width / 2, cy + Math.round(hgt) / 2 - c.height / 2);
  x.save(); x.globalCompositeOperation = 'lighter'; x.globalAlpha = alpha;
  if (canFilter) {
    x.filter = `blur(${Math.max(2, r * 0.22)}px)`; x.globalAlpha = alpha * 0.55; at(L);
    x.filter = `blur(${Math.max(1, r * 0.06)}px)`; x.globalAlpha = alpha * 0.9; at(L);
    x.filter = 'none';
  }
  x.globalAlpha = alpha; at(L);
  const core = lightLayer(px.map((c) => (lit(c) ? c.map((v) => Math.min(255, v + 120)) : c)), r, w * 0.35, k, hgt);   // whiter where bright
  x.globalAlpha = alpha * 0.35; at(core);
  x.restore();
}

let grain = null;                           // a fine noise, laid over every packshot
function grainPattern(x) {
  if (!grain) {
    grain = document.createElement('canvas'); grain.width = grain.height = 160;
    const g = grain.getContext('2d'), img = g.createImageData(160, 160);
    for (let i = 0; i < img.data.length; i += 4) { const v = Math.random() * 255; img.data[i] = img.data[i + 1] = img.data[i + 2] = v; img.data[i + 3] = 255; }
    g.putImageData(img, 0, 0);
  }
  return x.createPattern(grain, 'repeat');
}

// what a packshot shows first, of the animations a pack sets: the ones seen most
const SHOT_ORDER = ['listen', 'think', 'talk', 'error', 'timer', 'mute', 'volume', 'dnd', 'unmute', 'call_in', 'call_live', 'identify',
  'bt_pairing', 'call_out', 'call_end', 'setup', 'reset', 'login', 'end', 'timer_short', 'assist_volume'];
const SHOT_FONT = '"Inter", "SF Pro Display", -apple-system, "Segoe UI Variable Display", "Segoe UI", Roboto, "Helvetica Neue", system-ui, sans-serif';

// a design at a share of its run (0..1), by time
function frameAtShare(d, t) {
  const total = d.frames.reduce((a, f) => a + f.ms, 0) || 1;
  let at = 0;
  for (const f of d.frames) { at += f.ms; if (at >= t * total) return f.px; }
  return d.frames[d.frames.length - 1].px;
}

// p: { name, author, about, uses: [[slot, design]] } (null: Amazon's), designs: its designs
function drawPackshot(cv, p, style) {
  style = SHOT_OF[style] || style;
  const x = cv.getContext('2d'), W = cv.width, H = cv.height, u = H / 100;
  const tiny = (parseFloat(cv.style.width) || W) * 0.625 < 160;      // a tile, its name beside it: the light alone
  const used = p.uses.filter(([, d]) => d), designs = p.designs && p.designs.length ? p.designs : [...new Set(used.map(([, d]) => d))];
  // the animations shown, most seen first, each design once with all it is for
  const shown = [], seen = new Map(), place = (sl) => (RING_PLACES.find((q) => q.id === sl) || { label: sl }).label;
  for (const sl of [...new Set([...SHOT_ORDER, ...used.map(([s]) => s)])]) {
    const e = used.find(([s]) => s === sl);
    if (!e) continue;
    if (seen.has(e[1])) seen.get(e[1]).label += ' · ' + place(sl);
    else { const o = { slot: sl, label: place(sl), d: e[1] }; seen.set(e[1], o); shown.push(o); }
  }
  if (!shown.length) for (const d of designs) shown.push({ slot: '', label: d.name, d });
  const main = shown.length ? shown[0].d : null, mainPx = main ? brightest(main) : black(12);
  const rgba = (c, a) => `rgba(${c[0]},${c[1]},${c[2]},${a.toFixed(3)})`;
  const full = (c) => { const m = Math.max(...c) || 1; return c.map((v) => Math.round((v / m) * 255)); };
  const still = !main || new Set(main.frames.map((f) => f.px.map(rgbHex).join())).size < 4;     // a design that hardly moves

  const ground = () => {                      // near black, darker to the edges; the colour comes from the light itself
    x.fillStyle = '#060708'; x.fillRect(0, 0, W, H);
    const v = x.createRadialGradient(W / 2, H / 2, H * 0.4, W / 2, H / 2, W * 0.75); v.addColorStop(0, 'rgba(0,0,0,0)'); v.addColorStop(1, 'rgba(0,0,0,.55)');
    x.fillStyle = v; x.fillRect(0, 0, W, H);
  };
  // the light a ring throws round it: each LED its own colour from where it sits on an ellipse rx by ry, out to reach
  // (sq < 1: onto a surface seen flat, the table).  Each hue stays itself where a mean of them would turn to mud
  const spill = (px, cx, cy, rx, ry, reach, a, sq = 1) => {
    const on = px.filter(lit).length;
    if (!on) return;
    x.save(); x.globalCompositeOperation = 'lighter'; x.translate(cx, cy); x.scale(1, sq);
    px.forEach((c, i) => {
      if (!lit(c)) return;
      const t = ledAngle(i, px.length), lx = rx * Math.cos(t), ly = (ry * Math.sin(t)) / sq, f = full(c);
      const m = (Math.max(...c) / 255) * a * Math.min(1, 3 / Math.sqrt(on)), g = x.createRadialGradient(lx, ly, 0, lx, ly, reach);
      g.addColorStop(0, rgba(f, m)); g.addColorStop(0.35, rgba(f, m * 0.4)); g.addColorStop(1, rgba(f, 0));
      x.fillStyle = g; x.fillRect(lx - reach, ly - reach, 2 * reach, 2 * reach);
    });
    x.restore();
  };
  // an Echo Dot (3rd gen.) seen from a little above (top centre cx, cy; radius r): 99 mm across, 43 mm high.  A smooth
  // dark cap, the ring a narrow band round its foot (its back hidden behind the cap), then the fabric, upright from just
  // under the ring and tucked in round the foot.  Cap and fabric are stacks of level slices, each as wide as the
  // profile there.  The light falls on the wall behind, the fabric under the ring and the table
  const K = 0.36, DOT_H = 0.8, FOOT = DOT_H + K;     // tilt (sin of the view's angle); height and lowest point, in r
  const echo = (cx, cy, r, px, wall = 0.08) => {
    const hb = r * DOT_H, base = cy + hb, rc = r * 0.88, cs = r * 0.92, b0 = r * 0.045, bh = r * 0.055, f0 = b0 + bh;
    const corner = (c, d) => c * (1 - Math.sqrt(Math.max(0, 1 - (d / c) ** 2)));     // inset of a round corner of radius c, d from its end
    const tx = r - cs, ty = r * 0.2, bx = r * 0.2, by = r * 0.34, rb = r - bx;
    const fabric = (y) => r - (y < f0 + ty ? corner(ty, f0 + ty - y) * (tx / ty) : y > hb - by ? corner(by, y - hb + by) * (bx / by) : 0);
    const slices = (y0, y1, wd) => { const n = Math.max(6, Math.ceil((y1 - y0) / 1.2)); x.beginPath(); for (let k = 0; k <= n; k++) { const y = y0 + ((y1 - y0) * k) / n, w = wd(y); x.moveTo(cx + w, cy + y); x.ellipse(cx, cy + y, w, w * K, 0, 0, 2 * Math.PI); } };
    spill(px, cx, cy + b0, cs * 0.95, cs * K * 0.95, r * 1.7, wall);
    spill(px, cx, base, r, r * K, r * 1.3, 0.16, 0.34);
    x.save(); x.translate(cx, base); x.scale(1, K);
    const sh = x.createRadialGradient(0, 0, rb * 0.7, 0, 0, r * 1.15); sh.addColorStop(0, 'rgba(0,0,0,.85)'); sh.addColorStop(1, 'rgba(0,0,0,0)');
    x.fillStyle = sh; x.fillRect(-r * 1.3, -r * 1.3, r * 2.6, r * 2.6); x.restore();
    // the fabric: charcoal, a weave, lit just under the ring and darker to the foot
    x.save();
    slices(f0, hb, fabric);
    const side = x.createLinearGradient(cx - r, 0, cx + r, 0);
    side.addColorStop(0, '#08090a'); side.addColorStop(0.3, '#1d1f22'); side.addColorStop(0.6, '#15171a'); side.addColorStop(1, '#070809');
    x.fillStyle = side; x.fill(); x.clip();
    x.globalAlpha = 0.12; x.globalCompositeOperation = 'overlay'; x.fillStyle = grainPattern(x); x.fillRect(cx - r, cy, 2 * r, hb + r * K);
    x.globalAlpha = 1; x.globalCompositeOperation = 'source-over';
    spill(px, cx, cy + f0, cs, cs * K, r * 0.45, 0.45);
    const fall = x.createLinearGradient(0, cy, 0, base + rb * K), at = (y) => Math.min(1, y / (hb + rb * K));
    fall.addColorStop(at(f0), 'rgba(0,0,0,.25)'); fall.addColorStop(at(f0 + ty), 'rgba(255,255,255,.04)');
    fall.addColorStop(at(hb - by), 'rgba(0,0,0,.1)'); fall.addColorStop(1, 'rgba(0,0,0,.8)');
    x.fillStyle = fall; x.fillRect(cx - r, cy, 2 * r, hb + r * K);
    x.restore();
    // the cap: a rim rounding down to the ring, the top faintly domed and catching the light at the back
    slices(0, b0, (y) => cs - corner(cs - rc, Math.max(0, b0 - y) * ((cs - rc) / b0)));
    x.fillStyle = '#0b0c0e'; x.fill();
    const top = x.createRadialGradient(cx - rc * 0.2, cy - rc * K * 0.55, 0, cx, cy, rc * 1.05);
    top.addColorStop(0, '#26282c'); top.addColorStop(0.6, '#16171a'); top.addColorStop(1, '#0e0f11');
    x.beginPath(); x.ellipse(cx, cy, rc, rc * K, 0, 0, 2 * Math.PI); x.fillStyle = top; x.fill();
    x.lineWidth = Math.max(1, r * 0.008); x.strokeStyle = 'rgba(255,255,255,.08)'; x.stroke();
    buttons(cx, cy, rc, K);
    // the ring, its back behind the cap
    x.save();
    x.beginPath(); x.rect(cx - 3 * r, cy - 3 * r, 6 * r, 6 * r); x.ellipse(cx, cy, rc, rc * K, 0, 0, 2 * Math.PI); x.clip('evenodd');
    drawLight(x, px, cx, cy + b0, cs - r * 0.02, r * 0.06, 1, K, bh);
    x.restore();
  };
  // the Dot's four buttons on a cap of radius rc (k: its tilt): + at the back, the action dot right, - at the front,
  // the microphone left, and the microphones' pinholes between them
  const buttons = (cx, cy, rc, k) => {
    const d = rc * 0.48, s = rc * 0.12;
    if (s < 2) return;
    x.save(); x.translate(cx, cy); x.scale(1, k);
    const lw = Math.max(1, s * 0.09);
    x.lineWidth = lw; x.strokeStyle = 'rgba(255,255,255,.1)'; x.fillStyle = 'rgba(255,255,255,.035)';
    for (const [bx, by] of [[0, -d], [d, 0], [0, d], [-d, 0]]) { x.beginPath(); x.arc(bx, by, s, 0, 2 * Math.PI); x.fill(); x.stroke(); }
    if (s > 4) {
      x.strokeStyle = 'rgba(255,255,255,.22)'; x.lineWidth = lw * 1.4; x.lineCap = 'round';
      const g = s * 0.38;
      x.beginPath(); x.moveTo(-g, -d); x.lineTo(g, -d); x.moveTo(0, -d - g); x.lineTo(0, -d + g); x.moveTo(-g, d); x.lineTo(g, d);
      x.moveTo(-d, -g * 0.75); x.lineTo(-d, -g * 0.05); x.moveTo(-d, g * 0.55); x.lineTo(-d, g * 0.85); x.stroke();     // the microphone
      x.beginPath(); x.arc(-d, -g * 0.05, g * 0.55, 0.15 * Math.PI, 0.85 * Math.PI); x.stroke();
      x.beginPath(); x.arc(d, 0, s * 0.14, 0, 2 * Math.PI); x.fillStyle = 'rgba(255,255,255,.3)'; x.fill();
    }
    x.fillStyle = 'rgba(0,0,0,.6)';
    for (let i = 0; i < 4; i++) { const t = Math.PI / 4 + (i * Math.PI) / 2; x.beginPath(); x.arc(d * 1.05 * Math.cos(t), d * 1.05 * Math.sin(t), Math.max(0.6, s * 0.1), 0, 2 * Math.PI); x.fill(); }
    x.restore();
  };
  const puck = (cx, cy, r) => {               // the Echo seen from above under a small ring: fabric round the cap, the ring between
    const R = r * 1.22, g = x.createRadialGradient(cx - R * 0.3, cy - R * 0.35, 0, cx, cy, R);
    g.addColorStop(0, '#1c1e21'); g.addColorStop(1, '#0d0e10');
    x.beginPath(); x.arc(cx, cy, R, 0, 2 * Math.PI); x.fillStyle = g; x.fill();
    const c = x.createRadialGradient(cx - r * 0.3, cy - r * 0.35, 0, cx, cy, r);
    c.addColorStop(0, '#1a1c1f'); c.addColorStop(1, '#0b0c0e');
    x.beginPath(); x.arc(cx, cy, r * 0.94, 0, 2 * Math.PI); x.fillStyle = c; x.fill();
    buttons(cx, cy, r * 0.94, 1);
  };
  const font = (px, wgt) => `${wgt} ${Math.round(px)}px ${SHOT_FONT}`;
  const track = (em, fs) => { if ('letterSpacing' in x) x.letterSpacing = `${(em * fs).toFixed(2)}px`; };
  const caps = (t, X, Y, fs, a = 0.55, align = 'left', maxw = W, n = 1, mid = false) => {    // small spaced capitals, n lines, Y the first's or (mid) their middle
    if (tiny) return;
    x.font = font(fs, 600); track(0.14, fs); x.textAlign = align; x.textBaseline = 'alphabetic'; x.fillStyle = `rgba(255,255,255,${a})`;
    const out = []; let cur = '';
    for (const wd of t.toUpperCase().split(/\s+/)) { const s = cur ? cur + ' ' + wd : wd; if (x.measureText(s).width <= maxw || !cur) cur = s; else { out.push(cur); cur = wd; } }
    if (cur) out.push(cur);
    if (out.length > n) { out.length = n; out[n - 1] += ' …'; }
    out.forEach((l, i) => {
      while (l.length > 3 && x.measureText(l).width > maxw) l = l.slice(0, -3) + ' …';
      x.fillText(l.replace(/ ·( …)?$/, '$1'), X, Y + (i - (mid ? (out.length - 1) / 2 : 0)) * fs * 1.45);
    });
    track(0, fs);
  };
  // the name: light and large, two lines at most, shrunk to fit; returns where it ends
  const title = (X, top, maxw, size, wgt = 300) => {
    if (tiny) return top;
    let fs = size * u, lines = [];
    const fit = () => {
      x.font = font(fs, wgt); track(-0.025, fs); lines = []; let cur = '';
      for (const wd of p.name.split(/\s+/)) { const t = cur ? cur + ' ' + wd : wd; if (x.measureText(t).width <= maxw || !cur) cur = t; else { lines.push(cur); cur = wd; } }
      if (cur) lines.push(cur);
    };
    fit();
    while ((lines.length > 2 || lines.some((l) => x.measureText(l).width > maxw)) && fs > 4 * u) { fs *= 0.92; fit(); }
    x.textAlign = 'left'; x.textBaseline = 'alphabetic'; x.fillStyle = '#fff';
    let y = top + fs * 0.8;
    for (const l of lines) { x.fillText(l, X, y); y += fs * 1.04; }
    track(0, fs);
    return y - fs * 1.04 + fs * 0.25;
  };
  // a hairline in the pack's own colours, round the main design's ring
  const accent = (X, Y, w) => {
    if (tiny) return;
    const c = mainPx.filter(lit).map(full), g = x.createLinearGradient(X, 0, X + w, 0);
    (c.length ? c : [[255, 255, 255]]).forEach((v, i, a) => g.addColorStop(a.length > 1 ? i / (a.length - 1) : 0, rgba(v, 1)));
    x.fillStyle = g; x.fillRect(X, Y, w, Math.max(1, u * 0.5));
  };
  const lines = (t, X, Y, maxw, fs, n = 2, a = 0.6) => {     // a few lines of plain words
    x.font = font(fs, 400); x.textAlign = 'left'; x.fillStyle = `rgba(255,255,255,${a})`;
    const out = []; let cur = '';
    for (const wd of t.split(/\s+/)) { const s = cur ? cur + ' ' + wd : wd; if (x.measureText(s).width <= maxw || !cur) cur = s; else { out.push(cur); cur = wd; } }
    if (cur) out.push(cur);
    if (out.length > n) { out.length = n; out[n - 1] = out[n - 1].replace(/\s*\S*$/, '') + '…'; }
    out.forEach((l, i) => x.fillText(l, X, Y + i * fs * 1.35));
  };
  const n = used.length, amazon = p.uses.length - n;
  const by = p.author ? 'by ' + p.author : 'Theme pack';
  const counts = `${n} animation${n === 1 ? '' : 's'}${amazon ? ` · ${amazon} Amazon's` : ''}`;
  const head = () => {                        // the name left, author and counts right, apart however long either is
    x.font = font(2.9 * u, 600); track(0.14, 2.9 * u);
    const cw = Math.min(W * 0.4, Math.max(...[by, counts].map((t) => x.measureText(t.toUpperCase()).width)));
    track(0, 2.9 * u);
    const end = title(6 * u, 6 * u, W - 17 * u - cw, 9, 400);
    caps(by, W - 6 * u, 6 * u + 3 * u, 2.9 * u, 0.5, 'right', cw);
    caps(counts, W - 6 * u, 6 * u + 7.4 * u, 2.9 * u, 0.5, 'right', cw);
    return tiny ? 0 : Math.max(end, 17 * u);
  };
  ground();

  if (style === 'lineup') {                 // the pack's animations as a row of Echos, each with what it is for
    const end = head();
    accent(6 * u, end + 2 * u, W - 12 * u);
    const list = shown.slice(0, 8), k = list.length || 1, nc = k <= 4 ? k : Math.ceil(k / 2), nr = Math.ceil(k / nc);
    const top = tiny ? 4 * u : end + 5 * u, cw = (W - 12 * u) / nc, ch = (H - top - (tiny ? 4 : 3) * u) / nr;
    const r = Math.min(cw * 0.34, ch * (tiny ? 0.4 : 0.3));
    list.forEach((s, i) => {
      const cx = 6 * u + cw * ((i % nc) + 0.5), cy = top + ch * Math.floor(i / nc) + (tiny ? (ch - r * FOOT) / 2 + r * K : ch * 0.32);
      echo(cx, cy, r, brightest(s.d), 0.05);
      if (!tiny) caps(s.label, cx, cy + r * FOOT + 5 * u, 3.1 * u, 0.62, 'center', cw - 3 * u, 2);
    });
  } else if (style === 'sequence') {        // a row per animation: how it runs, left to right
    const end = head();
    const list = shown.slice(0, 4), top = tiny ? 4 * u : end + 3 * u, rh = (H - top - 4 * u) / Math.max(1, list.length);
    const lw = tiny ? 0 : W * 0.2, steps = 6, x0 = 6 * u + lw, cw = (W - 6 * u - x0) / steps, r = Math.min(cw * 0.36, rh * 0.36);
    list.forEach((s, j) => {
      const cy = top + rh * (j + 0.5);
      if (!tiny) caps(s.label, 6 * u, cy + 1 * u, 3.1 * u, 0.62, 'left', lw - 3 * u, 2, true);
      x.fillStyle = 'rgba(255,255,255,.07)'; x.fillRect(x0 + cw * 0.5, cy - u * 0.1, cw * (steps - 1), Math.max(1, u * 0.2));   // its time line
      for (let i = 0; i < steps; i++) { const cx = x0 + cw * (i + 0.5), f = frameAtShare(s.d, (i + 0.5) / steps); spill(f, cx, cy, r, r, r * 1.6, 0.06); puck(cx, cy, r); drawLight(x, f, cx, cy, r, Math.max(1.2, r * 0.16), 1); }
    });
  } else if (style === 'trail') {           // a long exposure: the main design's frames rising off the Echo, the oldest highest
    const cx = tiny ? W * 0.5 : W * 0.68, r = H * (tiny ? 0.26 : 0.23), cy = H * 0.89 - r * FOOT;
    const fr = still ? designs.map((d) => brightest(d)).reverse() : main.frames.map((f) => f.px), k = Math.min(10, Math.max(1, fr.length));
    const pick = (i) => (fr.length ? fr[fr.length - 1 - Math.floor((i * fr.length) / k)] : mainPx), step = (cy - H * 0.12) / Math.max(1, k - 1);
    for (let i = k - 1; i > 0; i--) {
      const t = i / k;
      drawLight(x, pick(i), cx, cy - i * step, r * 0.88 * (1 + 0.07 * i), Math.max(1.2, r * 0.07 * (1 - t * 0.5)), 0.85 * Math.pow(1 - t, 1.4), K);
    }
    echo(cx, cy, r, pick(0));
    const shade = x.createLinearGradient(0, 0, W * 0.5, 0); shade.addColorStop(0, 'rgba(6,7,8,.75)'); shade.addColorStop(1, 'rgba(6,7,8,0)');
    x.fillStyle = shade; x.fillRect(0, 0, W * 0.5, H);
    if (!tiny) caps(by, 7 * u, 12 * u, 2.9 * u, 0.55, 'left', W * 0.4);
    const end = title(7 * u, tiny ? 9 * u : 16 * u, W * 0.4, 12.5);
    accent(7 * u, end + 2.5 * u, 12 * u);
    if (!tiny && p.about) lines(p.about, 7 * u, end + 9 * u, W * 0.34, 3.3 * u, 3);
    if (!tiny) caps(still ? counts : `${shown[0].label} · ${main.frames.length} frames`, 7 * u, H - 8 * u, 3 * u, 0.5, 'left', W * 0.4);
  } else {                                  // halo: the main design on an Echo, the next ones small beside the words
    const cx = tiny ? W * 0.62 : W * 0.7, r = tiny ? H * 0.34 : H * 0.29, cy = tiny ? H * 0.5 - r * (FOOT - K) / 2 : H * 0.4;
    echo(cx, cy, r, mainPx);
    if (!tiny && shown.length) caps(shown[0].label, cx, H - 7 * u, 3.1 * u, 0.55, 'center', r * 2.6);
    if (!tiny) caps(by, 7 * u, 12 * u, 2.9 * u, 0.55, 'left', W * 0.4);
    const end = title(7 * u, tiny ? 9 * u : 16 * u, W * 0.42, 12.5);
    accent(7 * u, end + 2.5 * u, 12 * u);
    if (!tiny && p.about) lines(p.about, 7 * u, end + 9 * u, W * 0.36, 3.3 * u, 2);
    const minis = shown.slice(1, 4), mr = tiny ? 7.5 * u : 4.6 * u, gap = W * 0.42 / 3;
    minis.forEach((s, i) => {
      const mx = tiny ? 20 * u : 7 * u + gap * (i + 0.5), my = tiny ? H * (0.25 + 0.25 * i) : H - 21 * u, px = brightest(s.d);
      spill(px, mx, my, mr, mr, mr * 1.8, 0.07); puck(mx, my, mr); drawLight(x, px, mx, my, mr, Math.max(1.2, mr * 0.16), 1);
      if (!tiny) caps(s.label, mx, my + mr * 1.2 + 5.5 * u, 2.4 * u, 0.55, 'center', gap, 2);
    });
  }
  x.save(); x.globalAlpha = 0.045; x.globalCompositeOperation = 'overlay'; x.fillStyle = grainPattern(x); x.fillRect(0, 0, W, H); x.restore();
}
// a pack of this Echo's library, with its designs, for drawing
const packDrawn = (p) => (p.legacy ? { ...p, uses: p.uses.map(([sl, n]) => [sl, n ? p.designs[n - 1] || null : null]) }
  : { ...p, designs: p.has.map((id) => ring.designs.get(id)).filter(Boolean), uses: p.uses.map(([sl, id]) => [sl, id ? ring.designs.get(id) || null : null]) });

function packCanvas(css) {
  const dpr = Math.min(2, window.devicePixelRatio || 1), cv = h('canvas', { width: String(Math.round(css * dpr)), height: String(Math.round(css * 0.625 * dpr)), 'aria-hidden': 'true' });
  cv.style.width = css + 'px'; cv.style.aspectRatio = '16 / 10';
  return cv;
}

const fileName = (name, ext) => (name.trim() || 'pack').replace(/[^\w.-]+/g, '_') + ext;
function saveFile(name, data, type) {
  const a = h('a', { href: URL.createObjectURL(data instanceof Blob ? data : new Blob([data], { type })), download: name });
  document.body.append(a); a.click(); a.remove(); setTimeout(() => URL.revokeObjectURL(a.href), 1000);
}
const exportPack = (p) => saveFile(fileName(p.name, '.ringpack'), packCarryText(packCarry(p, ring.designs)), 'text/plain');

// ---- the Designs card: the packs as folders and the designs at the top level; or one pack opened

function designsCard(st) {
  if (ring.view && !ring.packs.has(ring.view)) ring.view = 0;
  const file = h('input', { type: 'file', accept: '.ringpack,.txt,text/plain', multiple: true, class: 'hidden', onchange: async () => {
    const list = [...file.files]; file.value = '';
    await ringDo(null, async () => {
      for (const f of list) {
        const text = await f.text();
        try {
          if (text.startsWith('hassmic ring pack 1')) {
            const c = packParse(text);
            await importPack(c, (path, body) => rcall('POST', path, body));
            toast(`Pack "${c.name}" added, with its ${c.designs.length} design${c.designs.length === 1 ? '' : 's'}`);
          } else {
            const d = designParse(text);
            if (!d.frames.length) { toast(`${f.name}: neither a design nor a pack`); continue; }
            const nid = (await rcall('POST', '/api/ring/design/0', designText(d))).json().id;
            if (ring.view) await addToPack(ring.view, [nid]);
          }
        } catch (e) { toast(`${f.name}: ${errText(e).replace(/^Error: /, '')}`); }
      }
    });
  } });
  const head = cardHead('Designs', `Your animations for the ring, ${ring.designs.size} of ${st.max}, and theme packs: folders of designs with which animation shows each, used at once and shared as one.`, 'sparkle',
    h('div', { class: 'ringd-headbtns' },
      h('button', { class: 'small', onclick: newPackDialog, disabled: ring.packs.size >= (st.packs_max || 50) }, icon('plus'), 'New pack'),
      h('button', { class: 'small', onclick: () => file.click(), title: 'A design (.txt) or a theme pack (.ringpack)' }, icon('up'), 'Import')));
  const crumbs = h('nav', { class: 'ringd-crumbs', 'aria-label': 'Where' },
    ring.view ? h('button', { class: 'link', onclick: () => { ring.view = 0; ringRender(); } }, 'All designs') : h('span', { class: 'ringd-crumb-here' }, 'All designs'),
    ring.view ? [h('span', { class: 'ringd-crumb-sep' }, '›'), h('span', { class: 'ringd-crumb-here' }, icon('box'), ring.packs.get(ring.view).name)] : null);
  const body = ring.view ? packFolder(ring.view, st) : topLevel(st);
  return h('div', { class: 'card ringd-designs' }, head, file, crumbs, body,
    h('div', { class: 'card-foot' }, h('span', { class: 'help' }, `${Math.max(1, Math.round(st.library_bytes / 1024))} kB of ${st.library_max >> 20} MB used`)));
}

// a design's tile: playing, what it shows for on the ring (and in the pack open), edit, use for, move
function designTile(id, d, inPack) {
  const cv = sizedCanvas(88), uses = usedFor(id);
  const forPack = inPack ? inPack.uses.filter(([, x]) => x === id).map(([sl]) => placeOf(sl).label) : [];
  play(cv, () => { const x = ring.designs.get(id); return x && { d: x }; });
  return h('div', { class: 'ringd-tile' },
    h('button', { class: 'ringd-tile-main', title: 'Edit "' + d.name + '"', onclick: () => designer(id, d) },
      h('span', { class: 'ringd-tile-art' }, cv, h('span', { class: 'ringd-tile-edit' }, icon('pen'))),
      h('span', { class: 'ringd-name' }, d.name),
      h('span', { class: 'ringd-sub' }, `${d.frames.length} frame${d.frames.length === 1 ? '' : 's'} · ${seconds(totalMs(d))}`)),
    h('div', { class: 'ringd-tile-foot' },
      h('span', { class: 'ringd-uses' + (uses.length || forPack.length ? '' : ' none'), title: [...forPack, ...uses].join(', ') },
        forPack.length ? 'In the pack: ' + forPack.join(', ') : uses.length ? 'On the ring: ' + uses.join(', ') : 'Not on the ring'),
      h('span', { class: 'ringd-tile-acts' },
        h('button', { class: 'link', onclick: () => useFor(id, d) }, 'Use for…'),
        h('button', { class: 'link', onclick: () => moveDialog(id, d) }, 'Move…'))));
}

const newDesignTile = (packId) => h('button', { class: 'ringd-tile ringd-new', onclick: () => newDialog(packId), disabled: ring.designs.size >= (ring.status.max || 100) },
  h('span', { class: 'ringd-plus' }, icon('plus')), h('span', { class: 'ringd-name' }, 'New design'), h('span', { class: 'ringd-sub' }, packId ? 'in this pack' : 'empty, or from Amazon\'s'));

function packTile(pid, p) {
  const cv = packCanvas(232); drawPackshot(cv, packDrawn(p), p.shot);
  const open = () => { ring.view = pid; ringRender(); };
  return h('div', { class: 'ringd-pack' },
    h('button', { class: 'ringd-pack-shot', title: 'Open "' + p.name + '"', onclick: open }, cv, h('span', { class: 'ringd-folder-tab' }, icon('box'), 'Pack')),
    h('div', { class: 'ringd-pack-text' }, h('span', { class: 'ringd-name' }, p.name),
      h('span', { class: 'ringd-sub' }, [p.author ? 'by ' + p.author : '', packSays(p)].filter(Boolean).join(' · '))),
    h('div', { class: 'ringd-tile-foot' }, h('button', { class: 'link', onclick: open }, 'Open'), h('button', { class: 'link', onclick: () => usePack(pid, p) }, 'Use')));
}

function topLevel(st) {
  const loose = [...ring.designs].filter(([id]) => !ring.home.has(id));
  return [ring.packs.size ? [h('div', { class: 'ringd-sect-title' }, 'Theme packs'), h('div', { class: 'ringd-packs' }, [...ring.packs].map(([pid, p]) => packTile(pid, p)))] : null,
    h('div', { class: 'ringd-sect-title' }, ring.packs.size ? 'Designs in no pack' : 'Designs'),
    h('div', { class: 'ringd-grid' }, newDesignTile(0), loose.map(([id, d]) => designTile(id, d, null)))];
}

// One pack opened: its packshot and words, what it does, its animations (a click picks one of its designs, Amazon's,
// or a design from elsewhere copied in), and its designs
function packFolder(pid, st) {
  const p = ring.packs.get(pid), cv = packCanvas(300);
  drawPackshot(cv, packDrawn(p), p.shot);
  const save = async (q) => { try { await savePack(pid, q); } catch (e) { toast(errText(e)); } await ringLoad(); };
  const slots = slotsOf(), groups = [...new Set(slots.map((s) => placeOf(s.id).group))];
  const tile = (s) => {
    const pl = placeOf(s.id), c2 = sizedCanvas(56), talker = s.id === 'listen' || s.id === 'end' ? RING_TALKER : null;
    const use = p.uses.find(([sl]) => sl === s.id), v = use ? use[1] : undefined, d = v ? ring.designs.get(v) : null;
    play(c2, () => { if (v === undefined) return null; const x = shownFor(s.id, d); return x ? { d: x, talker } : null; });
    const pickBtn = h('button', { type: 'button', class: 'ringd-act ringd-pact' + (d ? ' custom' : '') + (v === undefined ? ' out' : ''), title: pl.help || '',
      onclick: () => pickDesign({ slot: s.id, current: v === undefined ? null : v, notInPack: true,
        sections: [{ title: 'In this pack', ids: p.has.filter((x) => ring.designs.has(x)) },
          { title: 'Elsewhere', note: 'a copy comes into this pack', ids: [...ring.designs.keys()].filter((x) => !p.has.includes(x)), other: true, sub: 'copied in' }],
        onPick: async (val, sec) => {
          const other = sec && sec.other, uses = p.uses.filter(([sl]) => sl !== s.id);
          try {
            const id = other ? (await rcall('POST', '/api/ring/design/0', designText(ring.designs.get(val)))).json().id : val;
            if (id !== null) uses.push([s.id, id]);
            await savePackWith(pid, { ...p, has: other ? [...p.has, id] : p.has, uses }, other ? [id] : []);
          } catch (e) { toast(errText(e)); }
          await ringLoad();
        } }) },
      h('span', { class: 'ringd-art' }, c2), h('span', { class: 'ringd-act-label' }, pl.label),
      h('span', { class: 'ringd-act-pick' }, v === undefined ? 'Not in the pack' : d ? d.name : 'Amazon\'s'));
    // the design it shows, opened in the designer: designs an animation shows are not listed again below
    return h('div', { class: 'ringd-pact-wrap' }, pickBtn,
      d ? h('button', { type: 'button', class: 'ringd-pact-edit', title: `Edit "${d.name}"`, 'aria-label': `Edit "${d.name}"`, onclick: () => designer(v, d) }, icon('pen')) : null);
  };
  // below the animations only the pack's designs none of them shows: each design once on the page
  const designs = p.has.filter((x) => ring.designs.has(x) && !p.uses.some(([, y]) => y === x));
  return h('div', { class: 'ringd-folder' },
    h('div', { class: 'ringd-folder-head' },
      h('div', { class: 'ringd-shotframe' }, cv),
      h('div', { class: 'ringd-folder-text' },
        h('h3', {}, p.name), p.author ? h('div', { class: 'ringd-sub' }, 'by ' + p.author) : null,
        p.about ? h('p', { class: 'help' }, p.about) : null,
        h('div', { class: 'ringd-sub' }, packSays(p)),
        h('div', { class: 'ringd-folder-acts' },
          h('button', { class: 'primary', onclick: () => usePack(pid, p), disabled: !p.uses.length, title: 'Put its animations on the ring' },
            glyph('<path d="M7 4l13 8-13 8z" fill="currentColor"/>'), 'Use'),
          h('button', { onclick: () => packDetails(pid, p), title: 'Name, author, about and packshot' }, icon('pen'), 'Details'),
          h('button', { onclick: () => exportPack(p), title: 'Save it as a .ringpack file, its designs inside' }, icon('down'), 'Export'),
          h('button', { class: 'danger square', onclick: () => deletePack(pid, p), title: 'Delete the pack', 'aria-label': 'Delete the pack' }, icon('trash'))))),
    h('div', { class: 'ringd-sect-title' }, 'Its animations',
      h('span', { class: 'ringd-sect-acts' },
        h('button', { class: 'link', title: 'Every animation as the ring shows it now; designs from elsewhere copied in', onclick: () => fillFromRing(pid) }, 'Fill from the ring'),
        h('button', { class: 'link', onclick: () => save({ ...p, uses: [] }) }, 'Empty'))),
    h('div', { class: 'ringd-groups' }, groups.map((g) => h('div', { class: 'ringd-group' }, h('div', { class: 'ringd-group-head' }, g),
      h('div', { class: 'ringd-acts' }, slots.filter((s) => placeOf(s.id).group === g).map(tile))))),
    h('div', { class: 'ringd-sect-title' }, designs.length ? `Its other designs (${designs.length}), not shown for any animation` : 'Its other designs'),
    h('div', { class: 'ringd-grid' }, newDesignTile(pid), designs.map((id) => designTile(id, ring.designs.get(id), p))));
}

// every animation of a pack as the ring shows it now: designs in the pack as they are, from elsewhere copied in
async function fillFromRing(pid) {
  const p = ring.packs.get(pid), has = [...p.has], uses = [], copies = [];
  try {
    for (const s of slotsOf()) {
      let id = s.design || 0;
      if (id && !has.includes(id)) { id = (await rcall('POST', '/api/ring/design/0', designText(ring.designs.get(id)))).json().id; has.push(id); copies.push(id); }
      uses.push([s.id, id]);
    }
    await savePack(pid, { ...p, has, uses });
  } catch (e) { await dropDesigns(copies); toast(errText(e)); }
  await ringLoad();
}

// "Save as pack": what the ring shows now as a new pack.  Designs at the top level move into it; those in other packs
// come as copies (a design is in one pack)
async function saveRingAsPack() {
  const name = await ask({ title: 'Save as a theme pack', body: ['Every animation as the ring shows it now. Its designs at the top level move into the pack; those in other packs are copied.'],
    ok: 'Save', input: { label: 'Name', value: 'My look', max: 47 } });
  if (!name) return;
  const copies = new Map();                 // design in another pack -> its copy for this one (one per design)
  try {
    const has = [], uses = [];
    for (const s of slotsOf()) {
      let id = s.design || 0;
      if (id && ring.home.has(id)) {
        if (!copies.has(id)) copies.set(id, (await rcall('POST', '/api/ring/design/0', designText(ring.designs.get(id)))).json().id);
        id = copies.get(id);
      }
      if (id && !has.includes(id)) has.push(id);
      uses.push([s.id, id]);
    }
    ring.view = await savePack(0, { name, author: '', about: '', shot: 'halo', has, uses });
  } catch (e) { await dropDesigns([...copies.values()]); toast(errText(e)); }
  await ringLoad();
  if (ringBox) ringBox.querySelector('.ringd-designs').scrollIntoView({ behavior: 'smooth', block: 'start' });
}

function newPackDialog() {
  ask({ title: 'New theme pack', body: ['A folder for designs that belong together: pick which of them each animation shows, then use them all at once.'], ok: 'Create',
    input: { label: 'Name', value: '', max: 47 } }).then(async (name) => {
    if (!name) return;
    try { ring.view = await savePack(0, { name, author: '', about: '', shot: 'halo', has: [], uses: [] }); } catch (e) { toast(errText(e)); }
    await ringLoad();
  });
}

// A design to another place: the top level or a pack (out of the one it was in: its animations there showing it lose it)
function moveDialog(id, d) {
  const from = ring.home.get(id) || 0, m = modal(`Move "${d.name}" to`, 'ringd-use');
  const go = async (to) => {
    m.dlg.close();
    if (to === from) return;
    try {
      if (from) { const q = ring.packs.get(from); await savePack(from, { ...q, has: q.has.filter((x) => x !== id), uses: q.uses.filter(([, x]) => x !== id) }); }
      if (to) { const q = ring.packs.get(to); await savePack(to, { ...q, has: [...q.has, id] }); }
    } catch (e) { toast(errText(e)); }
    await ringLoad();
  };
  const fromUses = from ? ring.packs.get(from).uses.filter(([, x]) => x === id).map(([sl]) => placeOf(sl).label) : [];
  fill(m.body, fromUses.length ? h('p', { class: 'help' }, `In "${ring.packs.get(from).name}" it shows for ${fromUses.join(', ')}: moved out, those are no longer in that pack.`) : null,
    h('div', { class: 'ringd-moveto' },
      h('button', { class: from === 0 ? 'on' : '', onclick: () => go(0) }, icon('sparkle'), 'No pack (the top level)'),
      [...ring.packs].map(([pid, p]) => h('button', { class: from === pid ? 'on' : '', onclick: () => go(pid) }, icon('box'), p.name, h('span', { class: 'ringd-sub' }, packSays(p))))));
  m.foot.append(h('button', { onclick: () => m.dlg.close() }, 'Cancel'));
  m.show();
}

// A pack's words and packshot style, the packshot drawn as it goes
function packDetails(pid, p) {
  const st = { name: p.name, author: p.author, about: p.about, shot: SHOT_OF[p.shot] || p.shot || 'halo' }, m = modal('Pack details', 'ringd-details');
  const shot = packCanvas(420);
  const styles = PACK_SHOTS.map(([k, t]) => { const cv = packCanvas(90); return { k, cv, b: h('button', { type: 'button', class: 'ringd-shotpick', title: t, onclick: () => { st.shot = k; redraw(); } }, cv, h('span', {}, t)) }; });
  const field = (label, el) => h('div', { class: 'field' }, h('label', {}, label), el);
  const name = h('input', { type: 'text', value: st.name, maxlength: '47', oninput: () => { st.name = fitInput(name, 47); redraw(); } });
  const author = h('input', { type: 'text', value: st.author, maxlength: '47', placeholder: 'optional', oninput: () => { st.author = fitInput(author, 47); redraw(); } });
  const about = h('textarea', { rows: '2', maxlength: '199', placeholder: 'What it is like (optional)', oninput: () => { st.about = fitInput(about, 199).replace(/\n/g, ' '); } });
  about.value = st.about;
  const redraw = () => { const dp = packDrawn({ ...p, ...st }); drawPackshot(shot, dp, st.shot); styles.forEach((x) => { drawPackshot(x.cv, dp, x.k); x.b.classList.toggle('on', x.k === st.shot); }); };
  fill(m.body, h('div', { class: 'ringd-shotframe' }, shot), h('div', { class: 'ringd-shotpicks' }, styles.map((x) => x.b)),
    h('div', { class: 'ringd-details-fields' }, field('Name', name), field('Author', author), field('About', about)));
  m.foot.append(h('button', { onclick: () => shot.toBlob((b) => b && saveFile(fileName(st.name, '.png'), b)) }, icon('down'), 'Packshot as a picture'), h('span', { class: 'grow' }),
    h('button', { onclick: () => m.dlg.close() }, 'Cancel'),
    h('button', { class: 'primary', onclick: async () => {
      if (!st.name.trim()) { toast('A pack needs a name'); return; }
      try { await savePack(pid, { ...p, name: st.name.trim(), author: st.author.trim(), about: st.about.trim(), shot: st.shot }); m.dlg.close(); } catch (e) { toast(errText(e)); return; }
      ringLoad().catch(() => {});
    } }, 'Save'));
  m.show(); redraw();
}

// Deleting a pack: its designs go to the top level, or go with it (those on the ring then show Amazon's there)
function deletePack(pid, p) {
  const m = modal(`Delete "${p.name}"?`, 'ringd-use'), designs = p.has.filter((x) => ring.designs.has(x));
  let withDesigns = false;
  const opts = [['keep', `Keep its ${designs.length === 1 ? 'design' : designs.length + ' designs'}: they go to the top level`], ['all', 'Delete its designs too']].map(([k, t]) =>
    h('label', { class: 'ringd-use-row' }, h('input', { type: 'radio', name: 'ringd-del', checked: k === 'keep', onchange: () => { withDesigns = k === 'all'; } }), h('span', {}, t)));
  const onRing = designs.filter((x) => slotsOf().some((s) => s.design === x));
  fill(m.body, h('p', {}, 'The pack is gone for good.'), designs.length ? opts : null,
    onRing.length ? h('p', { class: 'help' }, `${onRing.length === 1 ? 'One of them is' : onRing.length + ' of them are'} on the ring now: deleted, those animations go back to Amazon's.`) : null);
  m.foot.append(h('button', { onclick: () => m.dlg.close() }, 'Cancel'), h('button', { class: 'danger', onclick: () => {
    m.dlg.close(); ring.view = 0;
    ringDo('Deleted', async () => {
      await rcall('POST', '/api/ring/pack-delete/' + pid);
      if (withDesigns) for (const did of designs) await rcall('POST', '/api/ring/delete/' + did);
    });
  } }, icon('trash'), 'Delete'));
  m.show();
}

// Using a pack: what it sets shown first; then its animations set (the rest as they are, or back to Amazon's) and
// applied
function usePack(id, p) {
  const avail = new Set(slotsOf().map((s) => s.id)), here = p.uses.filter(([sl]) => avail.has(sl)), away = p.uses.filter(([sl]) => !avail.has(sl));
  const m = modal(`Use "${p.name}"`, 'ringd-use');
  let all = false;
  const scope = [['only', 'Only its animations'], ['all', 'All animations']].map(([k, t]) =>
    h('button', { type: 'button', 'data-k': k, onclick: () => { all = k === 'all'; paintScope(); } }, t));
  const scopeHelp = h('p', { class: 'help' });
  const paintScope = () => {
    scope.forEach((b) => b.classList.toggle('on', (b.dataset.k === 'all') === all));
    scopeHelp.textContent = all ? 'Every animation not in the pack goes back to Amazon\'s.' : 'The animations the pack does not have stay as they are.';
  };
  paintScope();
  const designed = here.filter(([, x]) => x), stock = here.length - designed.length;
  const cv = packCanvas(440); drawPackshot(cv, packDrawn(p), p.shot);
  fill(m.body, h('div', { class: 'ringd-use-shot' }, cv),
    h('p', {}, designed.length ? `Its designs for ${designed.map(([sl]) => placeOf(sl).label).join(', ')}` : 'No designs', stock ? `; Amazon's for ${stock} other${stock === 1 ? '' : 's'}.` : '.'),
    away.length ? h('p', { class: 'help' }, `Not on this Echo, left out: ${away.map(([sl]) => placeOf(sl).label).join(', ')}.`) : null,
    h('div', { class: 'seg', role: 'group' }, scope), scopeHelp);
  const go = h('button', { class: 'primary', disabled: !here.length, onclick: async () => {
    go.disabled = true;
    try {
      const lines = here.map(([sl, x]) => `${sl} ${x}`);
      if (all) for (const sl of avail) if (!here.some(([y]) => y === sl)) lines.push(`${sl} 0`);
      await rcall('POST', '/api/ring/assign', lines.join('\n') + '\n');
      await rcall('POST', '/api/ring/apply');
      m.dlg.close();
      toast(`"${p.name}" goes on the ring now`);
    } catch (e) { toast(errText(e)); go.disabled = false; return; }
    await ringLoad().catch(() => {});
    ringStatus().catch(() => {});
  } }, 'Use and apply');
  m.foot.append(h('button', { onclick: () => m.dlg.close() }, 'Cancel'), go);
  m.show();
}
