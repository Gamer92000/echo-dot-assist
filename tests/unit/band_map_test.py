#!/usr/bin/env python3
"""The Echo Show's band as the settings page draws it (web/ring.js bandColours) against what ring_band.c sends the
screen for the same design (design_draw, then unroll): which LED lands where (band position p = LED p + 5) and the
blend between them, pixel for pixel across 960, through node.  The two are written apart; this keeps them one.  Then the
page's other band code against that same order: the frame strip's thumbnails (frameCss), the designer's hit test
(bandCellAt), Mirror (left to right), Move/Run (one cell right per step, round to the other end) and Rainbow (from the
left end)."""
import json, os, random, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
W = 960
random.seed(26)
designs = [[[random.choice([0, 255, random.randrange(256)]) for _ in range(3)] for _ in range(12)] for _ in range(6)]
designs.append([[255, 0, 0] if i == 10 else [0, 255, 0] if i == 11 else [0, 0, 0] for i in range(12)])   # the talker's pair

# ring_band.c itself, its static functions reached by including it; the core stubbed as tests/unit/ring_band_test.c does
harness = r"""
#include "ring_band.c"
const struct board board = { .volume_steps = 100 };
int core_muted(void) { return 0; }
int core_dnd(int set) { (void)set; return 0; }
int core_led_brightness(int set) { (void)set; return 100; }
void core_spawn(char *const argv[]) { (void)argv; }
int main(void)
{
    int s = slot_of(P_THINK), c; static struct rgb row[%d]; struct rgb led[LEDS];
    struct rd_frame fr = { .ms = 1000 };
    while (1) {
        for (int i = 0; i < LEDS; i++) for (int k = 0; k < 3; k++) { if (scanf("%%d", &c) != 1) return 0; fr.px[i][k] = (unsigned char)c; }
        des[s] = (struct rd_design){ .leds = LEDS, .nframes = 1, .loop = 0, .f = &fr };
        design_draw(s, P_THINK, led, 0, 0);
        unroll(led, row, %d);
        for (int x = 0; x < %d; x++) printf("%%ld %%ld %%ld\n", lroundf(clamp01(row[x].r) * 255), lroundf(clamp01(row[x].g) * 255), lroundf(clamp01(row[x].b) * 255));
    }
}
""" % (W, W, W)
with tempfile.TemporaryDirectory() as tmp:
    src, exe = os.path.join(tmp, "band_map.c"), os.path.join(tmp, "band_map")
    open(src, "w").write(harness)
    subprocess.run(["cc", "-O2", "-w", f"-I{ROOT}/src/hassmic", src, f"{ROOT}/src/hassmic/ringdesign.c", f"{ROOT}/src/hassmic/statefile.c",
                    "-lm", "-lpthread", "-o", exe], check=True)
    out = subprocess.run([exe], input=" ".join(str(v) for d in designs for c in d for v in c), capture_output=True, text=True, check=True).stdout
c_rows = [list(map(int, l.split())) for l in out.split("\n") if l]

js = r"""
const vm = require('vm'), fs = require('fs');
const ctx = vm.createContext({ console });
vm.runInContext(fs.readFileSync(process.argv[1], 'utf8'), ctx, { filename: 'ring.js' });
vm.runInContext('ring.status = { band: true, band_on: true }', ctx);
const t = JSON.parse(fs.readFileSync(0, 'utf8'));
const fn = (name) => vm.runInContext(name, ctx);           // a const of ring.js's: not a property of the context
const rows = t.designs.map((px) => ctx.bandColours(px, t.w).map((c) => c.map((v) => Math.round(v))));
// the rest in the band's order: what shows at the middle of cell p, for a design and what the designer makes of it
const at = (px, p) => ctx.bandColours(px, t.w)[Math.floor((p + 0.5) * t.w / 12)].map((v) => Math.round(v));
const px = t.designs[0], n = 12, B = vm.runInContext('BAND_BOX', ctx), cell = (B.x1 - B.x0) / n;
const css = ctx.frameCss(px, true), stops = [...css.matchAll(/(rgb\((\d+),(\d+),(\d+)\)|#07090a) [\d.]+%/g)].map((m) => (m[2] ? [+m[2], +m[3], +m[4]] : [0, 0, 0]));
const lit = (c) => c[0] || c[1] || c[2];
const extra = {
  thumbs: Array.from({ length: n }, (_, p) => { const c = px[(p + 5) % n]; return JSON.stringify(stops[2 * p]) === JSON.stringify(lit(c) ? c : [0, 0, 0]); }).every(Boolean),
  hit: Array.from({ length: n }, (_, p) => ctx.bandCellAt(B.x0 + (p + 0.5) * cell, (B.y0 + B.y1) / 2, n) === (p + 5) % n).every(Boolean)
       && ctx.bandCellAt(0.5, 0.3, n) === -1,
  mirror: Array.from({ length: n }, (_, p) => JSON.stringify(at(fn("mirrorPx")(px), p)) === JSON.stringify(at(px, n - 1 - p))).every(Boolean),
  move: Array.from({ length: n }, (_, p) => JSON.stringify(at(fn("rotPx")(px, 1), p)) === JSON.stringify(at(px, (p + n - 1) % n))).every(Boolean),
  rainbow: (() => { const w = ctx.wheelPx(n, true); return JSON.stringify(w[(0 + 5) % n]) === '[255,0,0]' && JSON.stringify(w[(2 + 5) % n]) === '[255,255,0]'; })(),
};
console.log(JSON.stringify({ rows, extra }));
"""
r = subprocess.run(["node", "-e", js, f"{ROOT}/web/ring.js"], input=json.dumps({"designs": designs, "w": W}), capture_output=True, text=True)
if r.returncode: sys.exit(f"node: {r.stderr.strip()}")
res = json.loads(r.stdout)
js_rows = [px for d in res["rows"] for px in d]

bad = [(i // W, i % W, a, b) for i, (a, b) in enumerate(zip(c_rows, js_rows)) if max(abs(x - y) for x, y in zip(a, b)) > 1]
ok = len(c_rows) == len(js_rows) == W * len(designs) and not bad
print(f"{'ok  ' if ok else 'FAIL'} the page's band and ring_band.c's: {len(designs)} designs, {W} pixels each, within 1 of 255"
      + ("" if ok else f" ({len(bad)} pixels apart, first: design {bad[0][0]} x {bad[0][1]}: C {bad[0][2]} page {bad[0][3]})" if bad else f" (rows {len(c_rows)} / {len(js_rows)})"))
mid = js_rows[-W + 440], js_rows[-W + 520]
print(f"{'ok  ' if mid == ([255, 0, 0], [0, 255, 0]) else 'FAIL'} LED 10 left of the middle, 11 right of it ({mid})")
words = {"thumbs": "the frame strip's thumbnails: cell p shows LED p + 5", "hit": "the designer's hit test: cell p paints LED p + 5",
         "mirror": "Mirror: left to right on the band", "move": "Move right / Run: one cell to the right, round to the other end",
         "rainbow": "Rainbow on the band: red at the left end, yellow two cells on"}
for k, v in res["extra"].items(): print(f"{'ok  ' if v else 'FAIL'} {words[k]}")
sys.exit(0 if ok and mid == ([255, 0, 0], [0, 255, 0]) and all(res["extra"].values()) else 1)
