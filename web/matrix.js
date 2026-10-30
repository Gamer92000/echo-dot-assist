// The settings page's matrices (app.js: settings on "Compare & copy", models on "Models"): one row per setting or model,
// one column per Echo.  No Echo is "the" source: any cell can be the value its row takes, so a copy goes whichever way
// is wanted, and a row's direction turns around with a click on the other value.  What is picked is a plan: row -> { from:
// the Echo it comes from, skip: Echos left alone }.  The cells are only drawn from it; hmmatrix.changes() says what
// Apply does.  Keyed by Echo, never by column: columns merge and split as the Echos' values change (groups()), the
// plan stays what was picked.  No DOM here: tests/unit/web_matrix_test.py runs it under node.
'use strict';

const hmmatrix = (() => {
  // Columns: Echos with the same settings as one column, when asked for.  val(row, col): undefined = no such row there
  // (another model), which is no difference: a Dot and an Echo Show set alike are "the same" as far as both go, as the
  // Echos list has always counted it.  That makes "the same" not transitive (A and C differ, both agree with B, which
  // has not got the row), so an Echo joins a group only if it agrees with all of it, and A, B + C come out in the
  // order given, never a group with two values in one cell
  function groups(cols, rows, val, on = true) {
    const out = [];
    const agree = (a, b) => rows.every((r) => { const x = val(r, a), y = val(r, b); return x === undefined || y === undefined || x === y; });
    for (const c of cols) {
      const g = on ? out.find((x) => x.cols.every((o) => agree(c, o))) : null;
      if (g) g.cols.push(c); else out.push({ key: c.key, cols: [c] });
    }
    return out;
  }

  const distinct = (row, cols, val) => new Set(cols.map((c) => val(row, c)).filter((v) => v !== undefined)).size;

  // Values of a row by how many Echos have them, most first (ties: first column first): 0 is what most have, a cell's
  // tint tells the others apart
  function rank(row, cols, val) {
    const n = new Map();
    for (const c of cols) { const v = val(row, c); if (v !== undefined) n.set(v, (n.get(v) || 0) + 1); }
    return new Map([...n].sort((a, b) => b[1] - a[1]).map(([v], i) => [v, i]));
  }

  // A click on a value: that value for the row.  On the value already picked: the row as it was.  Skips stay when the
  // direction turns, but never on the Echo that is now the source
  function pick(plan, row, key) {
    const p = plan.get(row);
    if (p && p.from === key) { plan.delete(row); return; }
    const skip = p ? p.skip : new Set();
    skip.delete(key);
    plan.set(row, { from: key, skip });
  }

  // Leave these Echos out of a row, or put them back (a group's cell is all of its Echos)
  function skip(plan, row, keys) {
    const p = plan.get(row); if (!p) return;
    const left = keys.every((k) => p.skip.has(k));
    for (const k of keys) { if (left) p.skip.delete(k); else p.skip.add(k); }
  }

  // Would this Echo change, were the row to take the value of `from`?  takes(row, col): it can have the row at all
  const wouldChange = (row, from, c, val, takes) => c !== from && takes(row, c) && val(row, c) !== val(row, from);

  // Every row these Echos (a column, grouped or not) have and another would change on, from them: each row from the
  // first of them that has it.  All of them from there already: back to none.  locked: Echos left alone: a row only they
  // differ on changes nothing
  function column(plan, rows, group, cols, val, takes, locked = new Set()) {
    const from = (r) => group.find((c) => val(r, c) !== undefined);
    const mine = rows.filter((r) => from(r) && cols.some((c) => !locked.has(c.key) && wouldChange(r, from(r), c, val, takes)));
    const all = mine.length && mine.every((r) => (plan.get(r) || {}).from === from(r).key);
    for (const r of mine) {
      if (all) plan.delete(r);
      else if ((plan.get(r) || {}).from !== from(r).key) pick(plan, r, from(r).key);
    }
  }

  // What Apply does: [{ row, from, to, value }], from and to columns.  A plan outlives a refresh, so what no longer holds
  // (the Echo gone, the row gone there, it has the value by now) simply is no change.  locked: Echos left alone altogether
  function changes(plan, cols, val, takes, locked = new Set()) {
    const out = [], by = new Map(cols.map((c) => [c.key, c]));
    for (const [row, p] of plan) {
      const from = by.get(p.from), value = from ? val(row, from) : undefined;
      if (value === undefined) continue;
      for (const c of cols) if (!locked.has(c.key) && !p.skip.has(c.key) && wouldChange(row, from, c, val, takes)) out.push({ row, from, to: c, value });
    }
    return out;
  }

  return { groups, distinct, rank, pick, skip, column, changes };
})();

if (typeof module !== 'undefined') module.exports = hmmatrix;

// ---------------------------------------------------------------- the table

// The table both matrices draw.  groups: its columns (groups()), rows: { id, label, title, sub, rank } and { head } between
// them, val and takes as above, text(id, the Echo that has it, value): what a cell says, m: { plan, locked }, busy: no
// clicks, redraw(): after one.  none: the title of a cell whose Echo cannot have the row, missing: what one that could says
function matrixTable({ groups, rows, cols, val, takes, text, m, busy, redraw, none = 'Not on this model', missing = '—' }) {
  const dense = groups.length > 6;              // 15 Echos at full width: seven on a big screen, the rest a long way off
  const n = groups.length + 1;
  const useLabel = (g) => (g.cols.length > 1 ? 'these Echos' : g.cols[0].name);
  const head = (g) => {
    const keys = g.cols.map((c) => c.key), locked = keys.some((k) => m.locked.has(k)), me = keys.includes('');    // some: locked, then grouped
    const lock = cols.length > 2 || locked ? h('button', { type: 'button', class: 'icon-btn mx-lock', 'aria-pressed': String(locked), 'data-k': 'lock ' + g.key, disabled: busy,
      title: locked ? 'Takes changes again' : 'Leave as it is: takes no changes', 'aria-label': `Leave ${useLabel(g)} as it is`,
      onclick: () => { for (const k of keys) { if (locked) m.locked.delete(k); else m.locked.add(k); } redraw(); } }, icon(locked ? 'lock' : 'unlock')) : null;
    const use = h('button', { type: 'button', class: 'small', 'data-k': 'use ' + g.key, disabled: busy,
      title: `Every row with a difference takes the value of ${useLabel(g)}`,
      onclick: () => { hmmatrix.column(m.plan, rows.filter((r) => r.id !== undefined).map((r) => r.id), g.cols, cols, val, takes, m.locked); redraw(); } }, 'Use all');
    return h('th', { scope: 'col', class: 'mx-h' + (locked ? ' locked' : '') },
      g.cols.length > 1 ? [h('div', { class: 'mx-name' }, nameList(g.cols.map((c) => c.name))), h('div', { class: 'mx-sub' + (me ? ' me' : '') }, `${g.cols.length} identical Echos${me ? ', this one too' : ''}`)]
        : [h('div', { class: 'mx-name' }, g.cols[0].name), h('div', { class: 'mx-sub' + (me ? ' me' : '') }, g.cols[0].sub)],
      h('div', { class: 'mx-acts' }, use, lock));
  };
  const cell = (r, g) => {
    const has = g.cols.find((c) => val(r.id, c) !== undefined), v = has ? val(r.id, has) : undefined;
    const p = m.plan.get(r.id), keys = g.cols.map((c) => c.key), byKey = new Map(cols.map((c) => [c.key, c]));
    const from = p ? byKey.get(p.from) : null, isFrom = !!(from && keys.includes(from.key));
    const hit = from && !isFrom && val(r.id, from) !== undefined && g.cols.some((c) => takes(r.id, c) && val(r.id, c) !== val(r.id, from));
    const out = hit && keys.every((k) => p.skip.has(k) || m.locked.has(k));
    if (v === undefined && !g.cols.some((c) => takes(r.id, c))) return h('td', { class: 'mx-c none', title: none }, h('span', { class: 'mx-none' }, '—'));
    const tint = v === undefined ? '' : ` v${Math.min(r.rank.get(v) || 0, 2)}`;
    const now = v === undefined ? missing : text(r.id, has, v);
    const to = from ? text(r.id, from, val(r.id, from)) : '';
    const pickIt = () => { hmmatrix.pick(m.plan, r.id, isFrom ? from.key : has.key); redraw(); };   // a group's source may be any of it
    const main = hit && !out
      ? h('button', { type: 'button', class: 'mx-v tgt', 'data-k': `v ${r.id} ${g.key}`, disabled: busy || v === undefined, title: 'This one instead', 'aria-label': `${g.cols[0].name}: ${r.label} changes from ${now} to ${to}. Use ${now} instead`,
        onclick: pickIt }, h('span', { class: 'old' }, now), h('span', { class: 'new' }, to))
      : h('button', { type: 'button', class: 'mx-v' + (isFrom ? ' src' : '') + (out ? ' kept' : '') + tint, 'data-k': `v ${r.id} ${g.key}`, disabled: busy || v === undefined, 'aria-pressed': String(isFrom),
        title: isFrom ? 'Copied from here. Click to leave this row as it is' : 'Use this value', 'aria-label': `${g.cols[0].name}: ${r.label} is ${now}. ${isFrom ? 'Copied from here' : 'Use this value'}`,
        onclick: pickIt }, isFrom ? icon('arrow') : null, h('span', {}, now));
    const skipper = hit && (cols.length > 2 || out) && !keys.every((k) => m.locked.has(k)) ? h('button', { type: 'button', class: 'mx-skip', 'data-k': `s ${r.id} ${g.key}`, disabled: busy,
      title: out ? 'Change this one too' : 'Leave this one as it is', 'aria-label': `${out ? 'Change' : 'Leave'} ${g.cols[0].name}`,
      onclick: () => { hmmatrix.skip(m.plan, r.id, keys); redraw(); } }, icon(out ? 'plus' : 'x')) : null;
    return h('td', { class: 'mx-c' }, main, skipper);
  };
  return h('table', { class: 'mx' + (dense ? ' dense' : '') },
    h('thead', {}, h('tr', {}, h('th', { class: 'mx-corner', scope: 'col' }, h('span', { class: 'sr' }, 'Setting')), groups.map(head))),
    h('tbody', {}, rows.map((r) => (r.head !== undefined
      ? h('tr', { class: 'mx-grp' }, h('th', { scope: 'colgroup', colspan: String(n) }, h('span', { class: 'mx-grp-l' }, r.head)))
      : h('tr', { class: m.plan.has(r.id) ? 'on' : '' }, h('th', { scope: 'row', title: r.title || null }, h('div', { class: 'mx-row' }, r.label), r.sub ? h('div', { class: 'mx-sub' }, r.sub) : null),
        groups.map((g) => cell(r, g)))))));
}
