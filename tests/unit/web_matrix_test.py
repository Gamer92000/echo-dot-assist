#!/usr/bin/env python3
"""web/matrix.js (the settings page's "Compare & copy" and model matrices), through node: which Echos count as the same
(another model's missing rows are no difference, and a group never holds two values), and what a plan of picked
cells writes: either way along a row, turned around, Echos left out by the cell or by the column, what no longer holds
after a refresh, models going to an Echo that has none yet."""
import json, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
JS = """
const m = require(process.argv[1]);
const T = {};                                     // Echo -> { row: value }; absent: not on that model
const cols = (t) => Object.keys(t).map((key) => ({ key }));
const val = (t) => (r, c) => t[c.key][r];
const takes = (t) => (r, c) => t[c.key][r] !== undefined;
const out = {};
const ch = (t, plan, locked, tk = takes(t)) => m.changes(plan, cols(t), val(t), tk, new Set(locked || [])).map((c) => [c.row, c.from.key, c.to.key, c.value]);
const grp = (t, on) => m.groups(cols(t), [...new Set(Object.values(t).flatMap(Object.keys))], val(t), on).map((g) => g.cols.map((c) => c.key));

{ const t = { a: { x: 1, y: 2 }, b: { x: 1, y: 2 }, c: { x: 9, y: 2 } };
  out.same = grp(t); out.off = grp(t, false); }
{ const t = { dot: { x: 1 }, show: { x: 1, band: 10 }, dot2: { x: 1 } };          // another model's row: no difference
  out.models = grp(t); }
{ const t = { a: { x: 1 }, b: { y: 1 }, c: { x: 2 } };                             // a~b, b~c, not a~c
  out.chain = grp(t); }
{ const t = { a: { x: 1, y: 5 }, b: { x: 2, y: 5 }, c: { x: 2 } }, p = new Map();
  m.pick(p, 'x', 'a'); out.down = ch(t, p);
  m.pick(p, 'x', 'b'); out.up = ch(t, p);                                          // the other value: turned around
  m.pick(p, 'x', 'b'); out.undo = [p.size, ch(t, p)];                              // the same value again: as it was
  m.pick(p, 'x', 'a'); m.skip(p, 'x', ['c']); out.skip = ch(t, p);
  m.pick(p, 'x', 'c'); out.skipSource = ch(t, p);                                  // c left out, then the source
  m.skip(p, 'x', ['a']); m.skip(p, 'x', ['a']); out.skipBack = ch(t, p);
  p.clear(); m.pick(p, 'x', 'a'); out.locked = ch(t, p, ['b']);
  out.noRow = (p.clear(), m.pick(p, 'y', 'a'), ch(t, p));                          // c has not got it, b has the value
}
{ const t = { a: { x: 1, y: 1, z: 7 }, b: { x: 2, y: 1 }, c: { x: 3, z: 8 } }, p = new Map(), rows = ['x', 'y', 'z'];
  const c = cols(t), by = (k) => c.find((x) => x.key === k);
  m.column(p, rows, [by('a')], c, val(t), takes(t)); out.colAll = [[...p.keys()], ch(t, p)];
  m.column(p, rows, [by('a')], c, val(t), takes(t)); out.colOff = p.size;          // again: back to none
  m.pick(p, 'x', 'b'); m.column(p, rows, [by('a')], c, val(t), takes(t)); out.colOver = ch(t, p); }
{ const t = { dot: { x: 1 }, show: { x: 1, band: 10 }, show2: { x: 2, band: 20 } }, p = new Map();  // a grouped column of two models
  const c = cols(t); m.column(p, ['x', 'band'], [c[0], c[1]], c, val(t), takes(t)); out.colMixed = ch(t, p); }
{ const t = { a: { x: 1, y: 1 }, b: { x: 2, y: 1 }, c: { x: 2, y: 2 } }, p = new Map(), c = cols(t);   // only c differs on y, and c is left alone
  m.column(p, ['x', 'y'], [c[0]], c, val(t), takes(t), new Set(['c'])); out.colLocked = [...p.keys()]; }
{ const t = { a: { x: 1 }, b: { x: 2 } }, p = new Map(); m.pick(p, 'x', 'a');
  t.b.x = 1; out.settled = ch(t, p);                                               // the same by the next refresh
  t.b.x = 2; delete t.a.x; out.gone = ch(t, p);                                    // the source lost the row
  const q = new Map(); m.pick(q, 'x', 'gone'); out.goneEcho = ch(t, q); }
{ const t = { a: { w: 'd1', v: 'd2' }, b: { w: 'd1' }, c: {} }, p = new Map();     // models: an Echo without one takes it
  const any = () => true; m.pick(p, 'v', 'a');
  out.model = ch(t, p, [], any);
  const no = (r, c) => c.key !== 'c'; out.modelNo = ch(t, p, [], no); }
{ const t = { a: { x: 1 }, b: { x: 2 }, c: { x: 2 }, d: { x: 2 }, e: { x: 3 } };
  out.rank = [...m.rank('x', cols(t), val(t))]; out.distinct = m.distinct('x', cols(t), val(t)); }
console.log(JSON.stringify(out));
"""
bad = n = 0


def check(cond, what):
    global bad, n
    n += 1
    if not cond:
        bad += 1
        print("FAIL", what)


r = subprocess.run(["node", "-e", JS, os.path.join(ROOT, "web/matrix.js")], capture_output=True, text=True)
if r.returncode:
    print(r.stderr)
    sys.exit(1)
o = json.loads(r.stdout)

check(o["same"] == [["a", "b"], ["c"]], f"Echos with the same settings make one column, the one that differs its own: {o['same']}")
check(o["off"] == [["a"], ["b"], ["c"]], "grouping off: a column each")
check(o["models"] == [["dot", "show", "dot2"]], f"a row another model has not got is no difference: {o['models']}")
check(o["chain"] == [["a", "b"], ["c"]], f"a group never holds two values, even where each agrees with the one between: {o['chain']}")

check(o["down"] == [["x", "a", "b", 1], ["x", "a", "c", 1]], f"a value picked goes to the other Echos of its row: {o['down']}")
check(o["up"] == [["x", "b", "a", 2]], f"the other value picked turns the row around, and the Echo that has it already is no change: {o['up']}")
check(o["undo"] == [0, []], "the picked value again: the row as it was")
check(o["skip"] == [["x", "a", "b", 1]], f"an Echo left out of a row: {o['skip']}")
check(o["skipSource"] == [["x", "c", "a", 2]], f"left out, then picked as the source: the source counts, the skip is gone: {o['skipSource']}")
check(o["skipBack"] == [["x", "c", "a", 2]], "left out and put back")
check(o["locked"] == [["x", "a", "c", 1]], f"an Echo left alone by its column: {o['locked']}")
check(o["noRow"] == [], f"neither the Echo with the value nor the one without the row: {o['noRow']}")

check(o["colAll"][0] == ["x", "z"], f"a whole column: the rows that differ, none that would change nothing: {o['colAll'][0]}")
check(["x", "a", "b", 1] in o["colAll"][1] and ["x", "a", "c", 1] in o["colAll"][1] and ["z", "a", "c", 7] in o["colAll"][1] and len(o["colAll"][1]) == 3,
      f"... to every Echo that has them: {o['colAll'][1]}")
check(o["colOff"] == 0, "the same column again: back to none")
check(all(c[1] == "a" for c in o["colOver"]) and len(o["colOver"]) == 3, f"a column over a row picked from elsewhere: {o['colOver']}")

check(o["colMixed"] == [["x", "dot", "show2", 1], ["band", "show", "show2", 10]], f"a grouped column: each row from the Echo in it that has it: {o['colMixed']}")

check(o["colLocked"] == ["x"], f"a whole column skips a row only a locked Echo differs on: {o['colLocked']}")

check(o["settled"] == [] and o["gone"] == [] and o["goneEcho"] == [], "what no longer holds after a refresh is no change")
check(o["model"] == [["v", "a", "b", "d2"], ["v", "a", "c", "d2"]], f"a model goes to an Echo without one: {o['model']}")
check(o["modelNo"] == [["v", "a", "b", "d2"]], f"... where it can take it: {o['modelNo']}")
check(o["rank"] == [[2, 0], [1, 1], [3, 2]], f"values by how many have them: {o['rank']}")
check(o["distinct"] == 3, "distinct values of a row")

print(f"web matrix: {n - bad}/{n} ok")
sys.exit(1 if bad else 0)
