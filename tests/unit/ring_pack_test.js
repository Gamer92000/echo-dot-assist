#!/usr/bin/env node
// web/ring.js's theme packs without a browser: the pack as the Echo keeps it and as it travels (its designs inside),
// read back as written; an import the Echo refuses half way leaves none of its designs behind, and one it would refuse
// is not sent at all; the canvas cache drops the least recently used past its pixels.  ring.js runs in a vm context
// with app.js's few helpers it needs stubbed
'use strict';
const fs = require('fs'), path = require('path'), vm = require('vm');

const enc = new TextEncoder();
const ctx = {
  console, enc,
  fitBytes: (v, max) => { while (enc.encode(v).length > max) v = [...v].slice(0, -1).join(''); return v; },
  toast: () => {}, errText: (e) => 'Error: ' + e.message,
};
vm.createContext(ctx);
const src = fs.readFileSync(path.join(__dirname, '../../web/ring.js'), 'utf8');
vm.runInContext(src + '\n;globalThis.__t = { packParse, packText, packHead, packCarry, packCarryText, importPack, designText, designParse, pixelCache };', ctx);
const t = ctx.__t;

let bad = 0, n = 0;
const ok = (c, what) => { n++; if (!c) { bad++; console.log('FAIL', what); } };
const same = (a, b, what) => ok(JSON.stringify(a) === JSON.stringify(b), `${what}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`);
const rejects = async (p) => { try { await p; return null; } catch (e) { return e; } };

const design = (name, k) => ({ name, leds: 12, loop: k % 2 ? -1 : 0,
  frames: Array.from({ length: 1 + k }, (_, f) => ({ ms: 100 + f, px: Array.from({ length: 12 }, (_, i) => [(i * 20 + f) & 255, k * 30, 255 - i]) })) });

(async () => {
  // as the Echo keeps it: written, read, written again the same; empty fields left out
  const p = { name: 'Aurora ä', author: 'Me', about: '', shot: 'trail', has: [5, 7], uses: [['listen', 5], ['think', 0], ['talk', 7]] };
  const text = t.packText(p), back = t.packParse(text);
  ok(text.startsWith('hassmic ring pack 2\nname Aurora ä\nauthor Me\nshot trail\nhas 5\n'), 'packText: ' + text);
  ok(!/^about/m.test(text), 'packHead: empty about left out');
  same([back.name, back.author, back.about, back.shot, back.has, back.uses, back.legacy], [p.name, p.author, '', p.shot, p.has, p.uses, false], 'packParse(packText)');
  same(t.packText(back), text, 'packText(packParse(packText))');
  ok(t.packHead({ name: 'X', author: '', about: '', shot: '' }) === 'name X\n', 'packHead');

  // as it travels: its designs inside, uses by number; a use of a design not in it dropped
  const lib = new Map([[5, design('Five', 2)], [7, design('Seven', 1)], [9, design('Nine', 0)]]);
  const c = t.packCarry({ ...p, uses: [...p.uses, ['error', 9]] }, lib);
  same(c.uses, [['listen', 1], ['think', 0], ['talk', 2]], 'packCarry uses');
  const ct = t.packCarryText(c), cb = t.packParse(ct);
  ok(cb.legacy && cb.designs.length === 2, 'packCarryText read back: legacy with 2 designs');
  same(cb.designs.map(t.designText), [lib.get(5), lib.get(7)].map(t.designText), 'carried designs');
  same([cb.name, cb.author, cb.shot, cb.uses], [p.name, p.author, p.shot, c.uses], 'carried pack');

  // into an Echo: designs first, then the pack naming them
  const echo = (failPack) => {
    const calls = []; let next = 11;
    const post = async (where, body) => {
      calls.push([where, body]);
      if (where.startsWith('/api/ring/pack/') && failPack) throw new Error('refused');
      return { json: () => ({ id: where.startsWith('/api/ring/design/') ? next++ : 3 }) };
    };
    return { calls, post };
  };
  let e = echo(false);
  ok(await t.importPack(cb, e.post) === 3, 'importPack: the pack\'s id');
  same(e.calls.map(([w]) => w), ['/api/ring/design/0', '/api/ring/design/0', '/api/ring/pack/0'], 'importPack posts');
  ok(/^has 11\nhas 12\nuse listen 11\nuse think stock\nuse talk 12\n/m.test(e.calls[2][1]), 'importPack pack text: ' + e.calls[2][1]);

  // the pack refused: the designs uploaded for it deleted again
  e = echo(true);
  ok((await rejects(t.importPack(cb, e.post))) !== null, 'importPack: a refused pack fails');
  same(e.calls.map(([w]) => w), ['/api/ring/design/0', '/api/ring/design/0', '/api/ring/pack/0', '/api/ring/delete/11', '/api/ring/delete/12'], 'importPack rolls back');

  // what the Echo would refuse is not sent: a name of 48 bytes (24 two-byte letters too), an about of 200
  for (const [what, x] of [['48-byte design name', { ...cb, designs: [design('x'.repeat(48), 1)] }],
    ['48-byte design name in umlauts', { ...cb, designs: [cb.designs[0], design('ä'.repeat(24), 1)] }],
    ['48-byte pack name', { ...cb, name: 'p'.repeat(48) }], ['200-byte about', { ...cb, about: 'a'.repeat(200) }],
    ['a control character', { ...cb, author: 'a\tb' }], ['no frames', { ...cb, designs: [{ ...design('E', 0), frames: [] }] }]]) {
    e = echo(false);
    ok((await rejects(t.importPack(x, e.post))) !== null && !e.calls.length, what + ': refused before any request');
  }
  e = echo(false);
  ok(await t.importPack({ ...cb, designs: [design('x'.repeat(47), 1)], uses: [['listen', 1]] }, e.post) === 3, '47 bytes taken');

  // the cache: past its pixels the least recently used goes
  const pc = t.pixelCache(300), cv = (w) => ({ width: w, height: 10 });
  pc.set('a', cv(10)); pc.set('b', cv(10)); pc.set('c', cv(10));
  pc.get('a'); pc.set('d', cv(10));
  ok(pc.get('a') && !pc.get('b') && pc.get('c') && pc.get('d'), 'pixelCache: b, least recently used, dropped');
  pc.set('big', cv(50));
  ok(pc.get('big') && !pc.get('a') && !pc.get('c') && !pc.get('d'), 'pixelCache: one past the budget alone stays');

  console.log(bad ? `FAIL ${bad} of ${n}` : `ok   ${n} ring pack checks`);
  process.exit(bad ? 1 : 0);
})();
