// The settings page (web.c serves it).  One key pair per browser, kept in localStorage; each Echo approves it once
// (action button), then every request is signed: X-HM-Mac = BLAKE2b-128 keyed with K over "METHOD\nPATH\nCTR\nBODY",
// K = BLAKE2b-256 keyed with X25519(our key, the Echo's) over "hassmic web 1" + Echo key + our key.
'use strict';

const { x25519, x25519Public, blake2b, hex, unhex } = hmcrypto;
const enc = new TextEncoder();
const $ = (id) => document.getElementById(id);

function store(k, v) { try { if (v === undefined) return JSON.parse(localStorage.getItem(k)); localStorage.setItem(k, JSON.stringify(v)); } catch (e) { return null; } }

function myKey() {
  let sk = store('hm.sk');
  if (!sk) { const b = new Uint8Array(32); crypto.getRandomValues(b); sk = hex(b); store('hm.sk', sk); }
  const s = unhex(sk);
  return { sk: s, pub: x25519Public(s) };
}
const me = myKey();

// An Echo the page talks to: base URL ('' = the one that served the page) and its key
class Echo {
  constructor(base, hello) {
    this.base = base; this.hello = hello;
    const shared = x25519(me.sk, unhex(hello.pub));
    const msg = new Uint8Array(13 + 64);
    msg.set(enc.encode('hassmic web 1')); msg.set(unhex(hello.pub), 13); msg.set(me.pub, 45);
    this.k = blake2b(32, shared, msg);
  }
  nextCtr() {
    const key = 'hm.ctr.' + this.hello.pub, last = store(key) || 0;
    const c = Math.max(Date.now() * 1000, last + 1);
    store(key, c);
    return c;
  }
  async call(method, path, body = '') {
    const ctr = String(this.nextCtr());
    const mac = blake2b(16, this.k, enc.encode(`${method}\n${path}\n${ctr}\n${body}`));
    const r = await fetch(this.base + path, { method, body: method === 'POST' ? body : undefined,
      headers: { 'X-HM-Pub': hex(me.pub), 'X-HM-Ctr': ctr, 'X-HM-Mac': hex(mac) } });
    if (r.status === 401) throw new Error('login');
    if (!r.ok) throw new Error(`${r.status}`);
    return r;
  }
  async login() {
    const label = (navigator.userAgentData && navigator.userAgentData.platform) || navigator.platform || 'browser';
    const r = await fetch(this.base + '/api/login', { method: 'POST', body: `${hex(me.pub)} ${label}` });
    return (await r.json()).login;
  }
}

function toast(text) {
  const t = $('toast'); t.textContent = text; t.classList.add('show');
  clearTimeout(toast.t); toast.t = setTimeout(() => t.classList.remove('show'), 2500);
}

let echo, state;

async function start() {
  const hello = await (await fetch('/api/hello')).json();
  echo = new Echo('', hello);
  $('name').textContent = hello.name;
  $('ident').textContent = `${hello.model} · ${hello.node} · ${hello.version}`;
  document.title = `${hello.name} · hassmic`;
  try { await load(); } catch (e) { if (e.message === 'login') showLogin(); else toast('Error: ' + e.message); }
}

function showLogin() { $('login').classList.remove('hidden'); $('app').classList.add('hidden'); }

$('ask').onclick = async () => {
  const msg = $('loginmsg');
  msg.textContent = 'Asked. Press the action button on the Echo now.';
  for (let i = 0; i < 70; i++) {
    let s;
    try { s = await echo.login(); } catch (e) { msg.textContent = 'The Echo did not answer.'; return; }
    if (s === 'approved') { $('login').classList.add('hidden'); await load(); toast('Logged in'); return; }
    if (s === 'refused') { msg.textContent = 'Refused: not approved within a minute, or another browser asked at the same time. Ask again.'; return; }
    await new Promise((r) => setTimeout(r, 1000));
  }
  msg.textContent = 'No answer from the button. Ask again.';
};

async function load() {
  state = await (await echo.call('GET', '/api/state')).json();
  render();
  $('app').classList.remove('hidden');
  if (!load.devices) { load.devices = true; refreshDevices().catch(() => {}); }
}

async function set(name, value) {
  try {
    const r = await (await echo.call('POST', '/api/set', `${name}=${value}\n`)).json();
    if (r.errors) toast(r.errors.trim()); else toast('Saved');
  } catch (e) { toast(e.message === 'login' ? 'Not logged in any more' : 'Error: ' + e.message); }
  await load();
}

function control(s) {
  const wrap = document.createElement('div'); wrap.className = 'ctl';
  if (s.type === 'bool') {
    const l = document.createElement('label'); l.className = 'switch';
    const i = document.createElement('input'); i.type = 'checkbox'; i.checked = !!s.value; i.id = 's-' + s.name;
    i.onchange = () => set(s.name, i.checked ? 'on' : 'off');
    l.append(i, document.createElement('span')); wrap.append(l);
  } else if (s.type === 'int') {
    const r = document.createElement('input'); r.type = 'range'; r.min = s.min; r.max = s.max; r.value = s.value; r.id = 's-' + s.name;
    const n = document.createElement('input'); n.type = 'number'; n.min = s.min; n.max = s.max; n.value = s.value;
    r.oninput = () => { n.value = r.value; };
    r.onchange = () => set(s.name, r.value);
    n.onchange = () => set(s.name, n.value);
    wrap.append(r, n);
    if (s.unit) { const u = document.createElement('span'); u.className = 'sub'; u.textContent = s.unit; wrap.append(u); }
  } else {
    const sel = document.createElement('select'); sel.id = 's-' + s.name;
    s.choices.forEach((c, i) => { const o = document.createElement('option'); o.value = c; o.textContent = c; if (i === s.value) o.selected = true; sel.append(o); });
    sel.onchange = () => set(s.name, sel.value);
    wrap.append(sel);
  }
  return wrap;
}

function render() {
  const d = state.diag, diag = [d.soc_temp !== null ? `SoC ${d.soc_temp} °C` : '', d.cpu !== null ? `CPU ${d.cpu} %` : ''].filter(Boolean).join(' · ');
  $('ha').innerHTML = `<span class="dot ${state.ha ? 'ok' : 'bad'}"></span>Home Assistant ${state.ha ? 'connected' : 'not connected'}` +
    (diag ? `<br>${diag}` : '');
  const w = $('warnings'); w.innerHTML = '';
  for (const text of state.warnings) { const d = document.createElement('div'); d.className = 'warn'; d.textContent = text; w.append(d); }

  const groups = $('groups'); groups.innerHTML = '';
  const by = new Map();
  for (const s of state.settings) { if (!by.has(s.group)) by.set(s.group, []); by.get(s.group).push(s); }
  for (const [g, list] of by) {
    const card = document.createElement('section'); card.className = 'card';
    const h = document.createElement('h2'); h.textContent = g; card.append(h);
    if (g === 'Features') {
      const p = document.createElement('p'); p.className = 'sub';
      p.textContent = 'A feature that is on has its entities in Home Assistant; off, they are gone. Switching one makes Home Assistant reconnect for a moment.';
      card.append(p);
    }
    for (const s of list) {
      const row = document.createElement('div'); row.className = 'row';
      const l = document.createElement('label'); l.textContent = s.label; l.htmlFor = 's-' + s.name;
      row.append(l, control(s)); card.append(row);
    }
    groups.append(card);
  }

  renderArb();

  const a = $('adb');
  a.textContent = state.adb.waiting ? 'Waiting for the action button…' : state.adb.open ? 'Open: anyone on the network has a root shell (closes by itself after 30 min)' : 'Closed';
  $('adbopen').disabled = state.adb.open || state.adb.waiting; $('adbclose').disabled = !state.adb.open;

  const c = $('clients'); c.innerHTML = '';
  for (const k of state.clients) {
    const row = document.createElement('div'); row.className = 'row';
    const l = document.createElement('label'); l.textContent = `${k.label}${k.me ? ' (this browser)' : ''}`;
    const b = document.createElement('button'); b.textContent = 'Revoke';
    b.onclick = async () => {
      if (!confirm(k.me ? 'Log this browser out of this Echo?' : `Revoke ${k.label}?`)) return;
      await echo.call('POST', '/api/revoke', k.pub);
      if (k.me) showLogin(); else load();
    };
    row.append(l, b); c.append(row);
  }
}

// ---------------------------------------------------------------- arbitration

const escape = (t) => String(t).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]);
const pageOf = (ip) => `http://${ip}:${location.port || 80}/`;

function renderArb() {
  const a = state.arbitration, card = $('arbcard');
  if (!a) { card.classList.add('hidden'); return; }
  card.classList.remove('hidden');
  let h = '';
  if (!a.joining) h += '<p>Off: this Echo answers every wake word it hears, whatever other Echos do.</p>';
  else h += `<p>${a.network ? `Network <code>${a.network}</code>` : 'Looking for a network'}${a.pairing ? '<span class="tag warn2">pairing</span>' : ''}</p>`;
  if (a.joining && state.ha && !a.handoff_entity)
    h += '<div class="warn">Home Assistant does not show this Echo\'s "Arbitration handoff" entity under the name it expects (renamed in Home Assistant, or the entity disabled). Other Echos can then hand it their network only through "Allow the device to perform Home Assistant actions", or the volume keys: hold Volume up and Volume down 2 s on this Echo, then on one in the network.</div>';
  if (a.members.length) {
    h += '<div class="sub">In the network with it:</div><ul class="plain">';
    for (const m of a.members) h += `<li>${escape(m.node)} ${m.ip ? `<a href="${pageOf(m.ip)}">${m.ip}</a>` : ''}</li>`;
    h += '</ul>';
  } else if (a.joining && a.network) h += '<p class="sub">No other Echo in its network right now.</p>';
  const why = {
    none: (o) => `in no network${o.for_s > 60 ? ` for ${Math.round(o.for_s / 60)} min: it did not get the key` : ' yet'}`,
    younger: () => 'in another network (younger): it should join this one',
    older: () => 'in another network (older): this Echo should join it',
  };
  const others = a.others || [];
  if (others.length) {
    h += '<div class="sub">Echos outside it:</div><ul class="plain">';
    for (const o of others) {
      const stuck = o.state === 'none' ? o.for_s > 60 : true;
      h += `<li>${escape(o.node)} ${o.ip ? `<a href="${pageOf(o.ip)}">${o.ip}</a>` : ''}: ${why[o.state](o)}` +
           `${o.network ? ` <code>${o.network}</code>` : ''}${stuck ? '<span class="tag warn2">not joined</span>' : ''}</li>`;
    }
    h += '</ul>';
    const nets = new Set(others.filter((o) => o.network).map((o) => o.network));
    if (nets.size) h += `<div class="warn">${nets.size === 1 ? 'A second network' : nets.size + ' other networks'} beside this one: the Echos in it do not settle wake words with these. They merge by themselves when Home Assistant can carry the key; otherwise pair them with the volume keys (new Echo first, then one in this network).</div>`;
  }
  $('arb').innerHTML = h;
}

// ---------------------------------------------------------------- other Echos

const others = new Map();                // base URL -> { echo, hello, logged, diff, error }
async function exportOf(e) { return (await (await e.call('GET', '/api/export')).text()); }
const parse = (text) => new Map(text.split('\n').filter((l) => l.includes('=') && !l.startsWith('#')).map((l) => l.split('=', 2)));

async function refreshDevices() {
  const ips = new Set(store('hm.extra') || []);
  const a = state.arbitration;
  if (a) for (const m of [...a.members, ...(a.others || [])]) if (m.ip) ips.add(m.ip);
  const mine = parse(await exportOf(echo));
  for (const ip of ips) {
    const base = ip.includes(':') ? `http://${ip}` : `http://${ip}:${location.port || 80}`;     // host:port when added so
    if (base === location.origin) continue;
    let d = others.get(base);
    try {
      if (!d) {
        const hello = await (await fetch(base + '/api/hello')).json();
        if (hello.pub === echo.hello.pub) continue;            // this one, under another address
        d = { echo: new Echo(base, hello) }; others.set(base, d);
      }
      try {
        const theirs = parse(await exportOf(d.echo));
        d.logged = true; d.diff = [...mine].filter(([k, v]) => theirs.has(k) && theirs.get(k) !== v).map(([k]) => k);
      } catch (e) { d.logged = false; if (e.message !== 'login') throw e; }
      d.error = null;
    } catch (e) { if (d) d.error = 'not reachable'; else others.set(base, { error: 'not reachable', ip }); }
  }
  renderDevices();
}

function renderDevices() {
  const box = $('devices'); box.innerHTML = '';
  const self = document.createElement('div'); self.className = 'row';
  self.innerHTML = `<label>${escape(echo.hello.name)} <span class="tag">this Echo</span></label>`;
  box.append(self);
  for (const [base, d] of others) {
    const row = document.createElement('div'); row.className = 'row';
    const l = document.createElement('label');
    const name = d.echo ? d.echo.hello.name : d.ip;
    l.innerHTML = `<a href="${base}/">${escape(name)}</a> <span class="sub">${escape(base.replace('http://', ''))}</span>` +
      (d.error ? `<span class="tag warn2">${d.error}</span>` : !d.logged ? '<span class="tag">not logged in</span>'
        : d.diff.length ? `<span class="tag warn2" title="${escape(d.diff.join(', '))}">${d.diff.length} setting${d.diff.length > 1 ? 's' : ''} differ</span>`
        : '<span class="tag">same settings</span>');
    const ctl = document.createElement('div'); ctl.className = 'ctl';
    if (d.echo && !d.logged) {
      const b = document.createElement('button'); b.textContent = 'Log in';
      b.onclick = () => loginOther(d, b); ctl.append(b);
    } else if (d.echo && d.diff.length) {
      const b = document.createElement('button'); b.textContent = 'Make like this Echo';
      b.onclick = () => applyTo([d]); ctl.append(b);
    }
    row.append(l, ctl); box.append(row);
  }
}

async function loginOther(d, b) {
  b.disabled = true; toast(`Press the action button on ${d.echo.hello.name}`);
  for (let i = 0; i < 70; i++) {
    const s = await d.echo.login();
    if (s === 'approved') { await refreshDevices(); toast(`Logged in to ${d.echo.hello.name}`); return; }
    if (s === 'refused') break;
    await new Promise((r) => setTimeout(r, 1000));
  }
  b.disabled = false; toast(`${d.echo.hello.name}: not approved`);
}

async function applyTo(list) {
  const text = await exportOf(echo);
  const notes = [];
  for (const d of list) {
    try {
      const r = await (await d.echo.call('POST', '/api/set', text)).json();
      // what that model has not got (Bluetooth, Wi-Fi motion) is no error worth showing
      const errs = r.errors.split('\n').filter((x) => x && !x.includes('no such setting'));
      if (errs.length) notes.push(`${d.echo.hello.name}: ${errs.join('; ')}`);
    } catch (e) { notes.push(`${d.echo.hello.name}: ${e.message === 'login' ? 'not logged in' : e.message}`); }
  }
  toast(notes.length ? notes.join(' · ') : 'Copied');
  await refreshDevices();
}

$('applyall').onclick = () => applyTo([...others.values()].filter((d) => d.echo && d.logged && d.diff.length));
$('add').onclick = () => {
  const ip = $('addip').value.trim();
  if (!/^[0-9a-zA-Z.:-]+$/.test(ip)) { toast('Not an address'); return; }
  const l = new Set(store('hm.extra') || []); l.add(ip); store('hm.extra', [...l]); $('addip').value = '';
  refreshDevices();
};

$('adbopen').onclick = async () => {
  try {
    const r = await (await echo.call('POST', '/api/adb', 'on')).json();
    if (r.adb === 'busy') { toast('A login waits for the button first'); return; }
    toast('Press the action button on the Echo');
    for (let i = 0; i < 65; i++) {
      await new Promise((res) => setTimeout(res, 1000));
      await load();
      if (!state.adb.waiting) break;
    }
  } catch (e) { toast('Error: ' + e.message); }
};
$('adbclose').onclick = async () => { await echo.call('POST', '/api/adb', 'off'); await load(); };

$('export').onclick = async () => {
  try {
    const text = await (await echo.call('GET', '/api/export')).text();
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([text], { type: 'text/plain' }));
    a.download = 'hassmic-settings.conf'; a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  } catch (e) { toast('Error: ' + e.message); }
};
$('importbtn').onclick = () => $('import').click();
$('import').onchange = async () => {
  const f = $('import').files[0]; if (!f) return;
  const text = await f.text();
  try {
    const r = await (await echo.call('POST', '/api/set', text)).json();
    $('importmsg').textContent = `${r.applied} applied` + (r.errors ? `\nNot applied:\n${r.errors}` : '');
    await load();
  } catch (e) { toast('Error: ' + e.message); }
  $('import').value = '';
};

start();
