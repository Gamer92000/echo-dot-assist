/* Exercise the actual renderer with a tiny DOM stand-in; no browser dependency. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const app = fs.readFileSync('web/app.js', 'utf8');
const render = app.slice(app.indexOf('function renderEnroll()'), app.indexOf('// ---------------------------------------------------------------- Echos:'));
const h = (...args) => args;
const box = { classList: { toggle() {} }, replaceChildren(...nodes) { this.nodes = nodes; } };
const ctx = vm.createContext({ enrollBox: box, enrollBusy: false, h, callout: h, cardHead: h });
vm.runInContext(app.match(/^const ENROLL_ACTIVE = .*;$/m)[0] + '\n' + render, ctx);
for (const [state, error] of [['failed', 'Training failed.'], ['unresolved', 'The set was installed, but selecting it failed.'], ['unresolved', 'Installation is unconfirmed.']]) {
  ctx.enroll = { available: true, session: { state, error } };
  ctx.renderEnroll();
  const text = JSON.stringify(box.nodes);
  assert(text.includes(error));
  assert(!text.includes('Nothing was installed'));
  assert(text.includes(state === 'unresolved' ? 'Clear pending request' : 'Try again'));
}
console.log('enroll UI: known and unresolved outcomes rendered accurately');
