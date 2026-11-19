#!/usr/bin/env python3
"""Run the login peer renderer from web/app.js in node with a small DOM and network stand-in."""
import os
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
JS = r"""
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(process.argv[1], 'utf8');
class Element {
  constructor(tag, props, children) { this.tag = tag; this.props = props; this.children = children; this.textContent = children.filter((x) => typeof x === 'string').join(''); }
  set innerHTML(value) { this.children = []; }
  append(...children) { this.children.push(...children); }
  contains(child) { return this.children.includes(child); }
}
const box = new Element('div', {}, []), requests = [], clicks = [];
let replies = {};
const context = {
  $: () => box,
  h: (tag, props, ...children) => new Element(tag, props, children),
  baseOf: (ip) => `http://${ip}:28931`, within: () => 'timeout',
  askThrough: (member) => clicks.push(member),
  fetch: async (url, options) => {
    requests.push([url, options]);
    const reply = replies[url];
    if (reply instanceof Error) throw reply;
    if (typeof reply === 'function') return reply();
    return { ok: reply !== undefined, json: async () => reply };
  },
};
vm.createContext(context);
vm.runInContext(source.slice(source.indexOf('async function renderThrough('), source.indexOf('\nasync function askThrough(')), context);
context.echo = { hello: { node: 'self-node', name: 'This Echo' } };
context.others = new Map([
  ['one', { echo: { hello: { node: 'peer-node', name: 'Echo Séjour <&>' } } }],
  ['offline', { ip: '192.0.2.3' }],
]);
vm.runInContext(source.slice(source.indexOf('function peerDisplayName('), source.indexOf('\nfunction renderDevices(')), context);
assert.equal(context.peerDisplayName('self-node'), 'This Echo');
assert.equal(context.peerDisplayName('peer-node'), 'Echo Séjour <&>');
assert.equal(context.peerDisplayName('unknown'), 'unknown');
context.others.get('one').echo.hello.name = 'Echo renamed';
assert.equal(context.peerDisplayName('peer-node'), 'Echo renamed');
assert.equal(context.others.get('one').echo.hello.node, 'peer-node');
const peer = (node, ip) => ({ node, ip });
const url = (ip) => `http://${ip}:28931/api/hello`;
const buttons = () => box.children.filter((x) => x.tag === 'button');
(async () => {
  const members = [peer('echo-dot-3-123456', '192.0.2.1'), peer('echo-dot-3-abcdef', '192.0.2.2')];
  replies[url('192.0.2.1')] = { node: members[0].node, name: 'Echo Séjour <&>' };
  replies[url('192.0.2.2')] = { node: members[1].node, name: 'Echo Cuisine' };
  await context.renderThrough(members);
  assert.deepEqual(buttons().map((x) => x.textContent), ['Log in through Echo Séjour <&>', 'Log in through Echo Cuisine']);
  assert.equal(requests.length, 2);
  assert.equal(requests[0][1].signal, 'timeout');
  buttons()[0].props.onclick();
  assert.equal(clicks[0].name, 'Echo Séjour <&>');
  assert.equal(clicks[0].node, members[0].node);
  assert.equal(members[0].name, undefined, 'discovery identities are not mutated');

  for (const reply of [new Error('offline'), undefined, { node: 'different-node', name: 'Wrong Echo' },
                       { node: members[0].node, name: '' }, { node: members[0].node, name: 42 }]) {
    replies[url('192.0.2.1')] = reply;
    await context.renderThrough([members[0]]);
    assert.equal(buttons()[0].textContent, `Log in through ${members[0].node}`);
    buttons()[0].props.onclick();
    assert.equal(clicks.at(-1).name, undefined);
  }

  let complete;
  replies[url('192.0.2.1')] = () => new Promise((resolve) => { complete = resolve; });
  const pending = context.renderThrough([members[0]]), stale = buttons()[0];
  await context.renderThrough([]);
  complete({ ok: true, json: async () => ({ node: members[0].node, name: 'Too late' }) });
  await pending;
  assert.equal(box.children.length, 0, 'a stale reply must not restore a removed peer');
  assert.equal(stale.textContent, `Log in through ${members[0].node}`);
  console.log('web login: friendly names, safe text, node checks, fallback and stale replies ok');
})().catch((err) => { console.error(err); process.exitCode = 1; });
"""

subprocess.run(["node", "-e", JS, os.path.join(ROOT, "web", "app.js")], check=True)