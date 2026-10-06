// The settings page (web.c serves it).  One key pair per browser, kept in localStorage; each Echo approves it once
// (action button), then every request is signed: X-HM-Mac = BLAKE2b-128 keyed with K over "METHOD\nPATH\nCTR\nBODY",
// K = BLAKE2b-256 keyed with X25519(our key, the Echo's) over "hassmic web 1" + Echo key + our key.  The answers carry
// X-HM-Mac too, over "RESP\nCTR\nBODY": what the page carries from one Echo to another (settings, models) is what it sent.
'use strict';

const { x25519, x25519Public, blake2b, hex, unhex } = hmcrypto;
const enc = new TextEncoder(), dec = new TextDecoder();
const cat = (a, b) => { const o = new Uint8Array(a.length + b.length); o.set(a); o.set(b, a.length); return o; };
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
  // body: text or bytes.  Returns { bytes, text(), json() } once the answer's signature checks out.  Errors: 'login'
  // (not approved here), 'old' (an Echo whose answers are not signed yet: update it), 'forged', or the Echo's own words.
  async call(method, path, body = '') {
    const ctr = String(this.nextCtr()), b = typeof body === 'string' ? enc.encode(body) : body;
    const mac = blake2b(16, this.k, cat(enc.encode(`${method}\n${path}\n${ctr}\n`), b));
    const r = await fetch(this.base + path, { method, body: method === 'POST' ? b : undefined,
      headers: { 'X-HM-Pub': hex(me.pub), 'X-HM-Ctr': ctr, 'X-HM-Mac': hex(mac) } });
    if (r.status === 401) throw new Error('login');
    const bytes = new Uint8Array(await r.arrayBuffer()), got = r.headers.get('X-HM-Mac');
    if (!r.ok) { let m = `${r.status}`; try { m = JSON.parse(dec.decode(bytes)).error || m; } catch (e) { /* not JSON */ } throw new Error(m); }
    if (!got) throw new Error('old');
    if (got !== hex(blake2b(16, this.k, cat(enc.encode(`RESP\n${ctr}\n`), bytes)))) throw new Error('forged');
    return { bytes, text: () => dec.decode(bytes), json: () => JSON.parse(dec.decode(bytes)) };
  }
  async login() {
    const r = await fetch(this.base + '/api/login', { method: 'POST', body: `${hex(me.pub)} ${browserLabel()}` });
    return (await r.json()).login;
  }
  // Echos of one network trust each other: let in here because `via` (an Echo this browser is approved on) vouches
  async nonce() { return (await (await fetch(this.base + '/api/vouch/nonce', { method: 'POST', body: hex(me.pub) })).json()).nonce; }
  async voucherLogin(nonce, v) {
    const body = `${hex(me.pub)} ${nonce} ${v.voucher} ${hex(enc.encode(v.via))} ${browserLabel()}`;
    return (await (await fetch(this.base + '/api/vouch/login', { method: 'POST', body })).json()).login;
  }
  async through(via) {
    const n = await this.nonce();
    const v = (await via.call('POST', '/api/vouch/issue', `${this.hello.pub} ${hex(me.pub)} ${n}`)).json();
    return this.voucherLogin(n, v);
  }
}

// ---------------------------------------------------------------- DOM helpers

function h(tag, props, ...kids) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(props || {})) {
    if (v == null || v === false) continue;
    if (k === 'class') e.className = v;
    else if (k === 'html') e.innerHTML = v;
    else if (k.startsWith('on')) e[k] = v;
    else if (typeof v !== 'string' && k in e) e[k] = v;
    else e.setAttribute(k, v === true ? '' : v);
  }
  for (const c of kids.flat(Infinity)) if (c != null && c !== false) e.append(c instanceof Node ? c : document.createTextNode(String(c)));
  return e;
}

const ICONS = {
  mic: '<path d="M12 3a3 3 0 0 0-3 3v6a3 3 0 0 0 6 0V6a3 3 0 0 0-3-3z"/><path d="M5 11a7 7 0 0 0 14 0M12 18v3"/>',
  speaker: '<path d="M4 9v6h4l5 4V5L8 9H4z"/><path d="M16.5 8.5a5 5 0 0 1 0 7M19 6a8.5 8.5 0 0 1 0 12"/>',
  sun: '<circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/>',
  sparkle: '<path d="M12 3l1.8 4.7 4.7 1.8-4.7 1.8L12 16l-1.8-4.7-4.7-1.8 4.7-1.8z"/><path d="M19 15l.8 2.2 2.2.8-2.2.8L19 21l-.8-2.2-2.2-.8 2.2-.8z"/>',
  music: '<path d="M9 18V5l11-2v13"/><circle cx="6" cy="18" r="3"/><circle cx="17" cy="16" r="3"/>',
  bt: '<path d="M7 7l10 10-5 5V2l5 5L7 17"/>',
  net: '<circle cx="12" cy="12" r="2"/><path d="M16.2 7.8a6 6 0 0 1 0 8.4M7.8 16.2a6 6 0 0 1 0-8.4M19 5a10 10 0 0 1 0 14M5 19A10 10 0 0 1 5 5"/>',
  sliders: '<path d="M4 6h9M17 6h3M4 12h3M11 12h9M4 18h11M19 18h1"/><circle cx="15" cy="6" r="2"/><circle cx="9" cy="12" r="2"/><circle cx="17" cy="18" r="2"/>',
  warn: '<path d="M12 3.5l9.5 17h-19z"/><path d="M12 10v4.5M12 17.5v.01"/>',
  info: '<circle cx="12" cy="12" r="9"/><path d="M12 11v5M12 8v.01"/>',
  ha: '<path d="M3 11.5L12 4l9 7.5"/><path d="M5.5 9.5V20h13V9.5"/><circle cx="12" cy="14" r="2"/>',
  wave: '<path d="M3 12h2M7 8v8M11 5v14M15 9v6M19 7v10"/>',
  whisper: '<path d="M4 5h16v11H9l-5 4z"/><path d="M8.5 10.5h.01M12 10.5h.01M15.5 10.5h.01"/>',
  wifi: '<path d="M2 9a15 15 0 0 1 20 0M5 12.5a10 10 0 0 1 14 0M8.5 16a5 5 0 0 1 7 0"/><path d="M12 19.5v.01"/>',
  phone: '<rect x="7" y="2.5" width="10" height="19" rx="2"/><path d="M11 18h2"/>',
  box: '<rect x="5" y="2.5" width="14" height="19" rx="2"/><circle cx="12" cy="14" r="3.5"/><path d="M12 7v.01"/>',
  term: '<path d="M4 6l6 6-6 6M12 18h8"/>',
  file: '<path d="M14 3H6v18h12V7z"/><path d="M14 3v4h4M9 13h6M9 17h6"/>',
  key: '<circle cx="8" cy="15" r="4"/><path d="M11 12l9-9M16 7l3 3"/>',
  check: '<path d="M5 12.5l4.5 4.5L19 7.5"/>',
  arrow: '<path d="M5 12h14M13 6l6 6-6 6"/>',
  ext: '<path d="M14 4h6v6M20 4l-9 9"/><path d="M18 14v5a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V7a1 1 0 0 1 1-1h5"/>',
  plus: '<path d="M12 5v14M5 12h14"/>',
  minus: '<path d="M6 12h12"/>',
  pen: '<path d="M4 20h4L19 9l-4-4L4 16z"/><path d="M14 6l4 4"/>',
  link: '<path d="M10 14a4 4 0 0 0 5.7 0l3-3a4 4 0 0 0-5.7-5.7l-1 1"/><path d="M14 10a4 4 0 0 0-5.7 0l-3 3a4 4 0 0 0 5.7 5.7l1-1"/>',
};
const icon = (n) => h('span', { html: `<svg class="i" viewBox="0 0 24 24" aria-hidden="true">${ICONS[n] || ''}</svg>`, style: 'display:inline-flex' });

function browserLabel() { return (navigator.userAgentData && navigator.userAgentData.platform) || navigator.platform || 'browser'; }

function toast(text) {
  const t = $('toast'); t.textContent = text; t.classList.add('show');
  clearTimeout(toast.t); toast.t = setTimeout(() => t.classList.remove('show'), 3200);
}
const errText = (e) => e.message === 'login' ? 'Not logged in any more: reload the page'
  : e.message === 'forged' ? 'An answer did not carry this Echo\'s signature: someone on the network may be interfering'
  : e.message === 'old' ? 'That Echo runs an older hassmic: update it first' : 'Error: ' + e.message;

// ---------------------------------------------------------------- what the settings are

// Page sections, in this order.  Settings come from the Echo (settings.c) with their group; the words are here.
const SECTIONS = [
  { id: 'Voice', icon: 'mic', intro: 'How the Echo hears you, and how it sounds when it does.' },
  { id: 'Sound', icon: 'speaker', intro: 'Amazon\'s own equalizer, applied to everything the Echo plays: replies, timers, music, Bluetooth.' },
  { id: 'Lights', icon: 'sun', intro: 'The light ring. It shows listening, thinking, speaking, errors and mute whatever you set here.' },
  { id: 'Features', icon: 'sparkle', intro: 'Optional extras. While a feature is on, its entities are in Home Assistant; switched off, they are removed there. Home Assistant reconnects for a moment when you switch one.' },
  { id: 'Music', icon: 'music', intro: 'Music Assistant plays on the Echo as a Sendspin player.' },
  { id: 'Bluetooth', icon: 'bt', intro: 'For phones playing music on the Echo.' },
  { id: 'Echos', icon: 'net', intro: '' },
  { id: 'System', icon: 'sliders', intro: 'Updates, backups of these settings, access.' },
];

// help: what it does, in the user's terms.  ha: also in Home Assistant (true), or for features the entities it adds.
// icon: features.  parent: shown inside that feature's card while it is on.  tag: an extra badge.
const HELP = {
  mic_level: { help: 'How loud your voice reaches speech-to-text. The Echo adjusts its gain to hold speech at this level. Raise it if quiet commands get misheard or cut off; lower it if loud speech comes out distorted. Default −26 dBFS.' },
  noise_reduction: { help: 'Takes steady background noise (fans, traffic, the dishwasher) out of what speech-to-text hears, by up to 6, 9 or 12 dB. The wake word always gets the untouched sound. Try it if commands fail in a noisy room.' },
  wake_sound: { ha: true, help: 'A tone when the Echo starts listening. Turning it off also silences the Echo\'s other local sounds.' },
  mute: { ha: true, help: 'Stops listening for the wake word, as the switch in Home Assistant does. The mic-off button on the Echo is a separate, hardware mute: only the button can lift it.' },
  do_not_disturb: { ha: true, help: 'Drops announcements from Home Assistant while on. The ring pulses purple when you switch it on.' },
  equalizer_bass: { ha: true }, equalizer_mid: { ha: true }, equalizer_treble: { ha: true },
  led_auto_brightness: { ha: true, help: 'The ring dims and brightens with the room, from the Echo\'s light sensor and Amazon\'s own curve, as on a stock Echo.' },
  led_brightness: { ha: true, help: 'A fixed brightness for the ring. Setting one turns auto brightness off.' },
  bluetooth_announcements: { help: 'Says "Connected to …" and "Disconnected from …" when a phone connects, in your voice assistant\'s voice.' },
  bluetooth_announcement_language: { help: 'The language of those sentences. Home Assistant does not tell the Echo which language your voice assistant speaks, so pick the one that matches its voice.' },
  sendspin_unpaired: { help: 'Lets any Music Assistant server on the network play here without pairing. Off: only a server given this Echo\'s "Sendspin pairing token" (a diagnostic entity in Home Assistant) may play.' },
  online_updates: { help: 'Where new versions come from. Release: tested releases only. Beta: every new build, plus releases. Off: nothing is fetched. The Echo\'s "Firmware" entity in Home Assistant then shows what is new and installs it; every update is signature-checked, and the Echo falls back by itself if a new version does not stay up.' },

  arbitration: { icon: 'net', ha: ['Arbitration peers'], help: 'When several Echos hear the wake word, only the one that heard you best answers; the others stay silent and dark. Only devices listening for the same wake word compete. Off: this Echo answers every wake word it hears.',
    status: () => {
      const a = state.arbitration; if (!a || !a.arbitrates || a.mode !== 'kiosk') return null;
      if (!a.kiosk_port) return h('p', { class: 'f-bad' }, icon('warn'), h('span', {}, 'Port 2330 could not be opened, so this Echo answers every wake word. The Echo\'s log (boot.log) says why.'));
      return h('p', { class: 'f-model' }, a.kiosk_heard_s == null ? 'No claim from another Kiosk Satellite device heard yet.'
        : `Last claim from another Kiosk Satellite device: ${a.kiosk_heard_s < 90 ? a.kiosk_heard_s + ' s' : Math.round(a.kiosk_heard_s / 60) + ' min'} ago.`);
    } },
  arbitration_mode: { parent: 'arbitration', help: 'How the Echos agree on who answers. Use the same on every Echo: Echos in one mode do not settle wake words with Echos in the other.',
    modes: {
      hassmic: { title: 'Echo network', tag: 'Default', sub: 'hassmic\'s own protocol, between your Echos',
        pros: ['Only your Echos take part: claims are signed with the network\'s key, so nothing else on the network can silence an Echo',
          'The Echo you are talking to, or one that is ringing, keeps the next wake word',
          'Scores with the wake word energies of Amazon\'s own audio front end, as stock Echos did',
          'Decided within 0.2 s, and with no other Echo around there is no wait at all',
          'The winner says it answers, so an Echo that heard the wake word late stays quiet too'],
        cons: ['Only Echos running hassmic: tablets and other satellites are left to Home Assistant, where the first device to wake up wins'] },
      kiosk: { title: 'Kiosk Satellite', sub: 'Kiosk Satellite\'s protocol: tablets and Echos together',
        pros: ['Settles wake words with Kiosk Satellite tablets too, when they listen for the same wake word (\u201cAlexa\u201d), and with Echos in this mode',
          'Simple: the device that heard the wake word loudest over its room answers'],
        cons: ['No protection: any device on your network can claim every wake word and keep this Echo silent',
          'No preference for the device you are talking to: a louder one can take the next wake word in the middle of a conversation',
          'Every wake word waits the full window (below), even with no other device around',
          'A claim lost on Wi-Fi means two devices answer; Home Assistant then lets only the first through',
          'Opens UDP port 2330 in the Echo\'s firewall, outside the range it otherwise allows',
          'The Echo\'s loudness is not yet calibrated against a tablet\'s microphone: an offset (below) evens it out by ear'] },
    } },
  arbitration_offset: { parent: 'arbitration', when: () => settingValue('arbitration_mode') === 'kiosk',
    help: 'Evens out the Echo\'s loudness against your tablets\' microphones, which hear differently. If a tablet answers when you spoke to the Echo, raise it; if the Echo answers when you faced a tablet, lower it. Only counts against devices in Kiosk Satellite mode with a different offset: set the same on every Echo. Default 0 dB.' },
  arbitration_window: { parent: 'arbitration', step: 50, when: () => settingValue('arbitration_mode') === 'kiosk',
    help: 'How long the Echo waits for other devices\' claims before it answers, as Kiosk Satellite\'s own setting. Longer catches devices that report late, at the cost of a slower answer. Use the same as on your tablets; Kiosk Satellite\'s default is 400 ms.' },
  sound_detection: { icon: 'wave', ha: ['Sound (event)'], help: 'Amazon\'s Alexa Guard model listens for smoke and CO alarms, breaking glass, barking, a crying baby, snoring, coughing, running water and beeping appliances, and Home Assistant gets each as a "Sound" event for automations.',
    note: 'Less reliable than on a stock Echo, which double-checks every hit in Amazon\'s cloud. Treat events as hints, never as a replacement for a smoke detector.',
    status: () => {
      const s = state.sound; if (!s) return null;
      if (s.failed) return h('p', { class: 'f-bad' }, icon('warn'), h('span', {}, 'Could not start: the model did not load, so it was switched off. The Echo\'s log (boot.log) says why.'));
      return h('p', { class: 'f-model' }, s.model === 'newer' ? 'Model: Amazon\'s newer one, installed with scripts/artifacts.sh.'
        : 'Model: the one in the Echo\'s firmware. scripts/artifacts.sh can install Amazon\'s newer one (retrained, same sounds).');
    } },
  whisper_detection: { icon: 'whisper', ha: ['Last request whispered'], help: 'Tells whether your last request was whispered, before speech-to-text finishes. Use it in your conversation agent\'s prompt to have it answer quietly.' },
  wifi_motion: { icon: 'wifi', tag: 'Experimental', ha: ['Wi-Fi motion', 'Wi-Fi motion sensitivity'], help: 'A motion sensor without extra hardware: someone walking between the Echo and your Wi-Fi router changes how the Echo receives the router. It shows motion, not presence, sees best along that path, and other Wi-Fi traffic can set it off.' },
  wifi_motion_sensitivity: { parent: 'wifi_motion', help: 'How much the signal has to change to count as motion. Higher catches more, with more false alarms.' },
  bluetooth_audio: { icon: 'phone', ha: ['Bluetooth pairing'], help: 'Phones can pair with the Echo and play music on it (SBC, AAC, aptX). Start pairing with the "Bluetooth pairing" switch in Home Assistant; the ring shows a blue chaser meanwhile. Phones paired before still connect while this is off.' },
  bluetooth_speaker_delay: { parent: 'bluetooth_speaker', step: 10, help: 'How much later the speaker plays than the Echo would, so Music Assistant keeps it in step with your other players. Raise it if this speaker lags behind them, lower it if it runs ahead. Each speaker differs; the Echo keeps one value.' },
  bluetooth_speaker: { icon: 'box', ha: ['Bluetooth speaker search', 'Play on Bluetooth speaker', 'Bluetooth speaker', 'Bluetooth speaker delay'], help: 'Plays everything (replies, timers, music) on a Bluetooth speaker instead of the Echo\'s own. Put the speaker in pairing mode near the Echo and switch on "Bluetooth speaker search" in Home Assistant: the Echo pairs with the strongest one it hears.' },
};
const CHOICE_NAMES = {
  arbitration_mode: { hassmic: 'Echo network', kiosk: 'Kiosk Satellite' },
  noise_reduction: { off: 'Off', low: 'Low', medium: 'Medium', high: 'High' },
  online_updates: { off: 'Off', beta: 'Beta', release: 'Release' },
  bluetooth_announcement_language: { en: 'English', de: 'Deutsch', fr: 'Français', es: 'Español', it: 'Italiano', pt: 'Português', nl: 'Nederlands',
    sv: 'Svenska', da: 'Dansk', nb: 'Norsk', fi: 'Suomi', pl: 'Polski' },
};
const settingValue = (name) => { const s = state.settings.find((x) => x.name === name); return s ? (s.type === 'choice' ? s.choices[s.value] : s.value) : undefined; };
const label = (s) => { const t = s.label.replace(/ \(experimental\)$/, '').replace(/^Equalizer /, '').replace(' with other Echos', ''); return t[0].toUpperCase() + t.slice(1); };
const fmt = (s, v) => {
  if (s.unit === 'dB') return `${v > 0 ? '+' : v < 0 ? '−' : ''}${Math.abs(v)} dB`;
  if (s.unit === 'dBFS') return `${v < 0 ? '−' : ''}${Math.abs(v)} dBFS`;
  return s.unit ? `${v} ${s.unit}` : String(v);
};

// ---------------------------------------------------------------- start, login

let echo, state;

async function start() {
  const hello = await (await fetch('/api/hello')).json();
  echo = new Echo('', hello);
  renderThrough(hello.members || []);
  showName(hello);
  try { await load(); } catch (e) { if (e.message === 'login') showLogin(); else toast('Error: ' + e.message); }
}

function showName(d) {
  $('name').textContent = d.name; $('lname').textContent = d.name;
  $('ident').textContent = `${d.model} · ${d.node} · ${d.version}`;
  document.title = `${d.name} · settings`;
}

function showLogin() { $('login').classList.remove('hidden'); $('app').classList.add('hidden'); }

// The login page: an Echo of this one's network that this browser is approved on can vouch for it.  Its page opens in a
// small window, asks you, and sends the voucher back here (postMessage).
function renderThrough(members) {
  const box = $('through'); box.innerHTML = '';
  if (!members.length) return;
  box.append(h('p', { class: 'or' }, h('span', {}, 'or, if this browser is logged in on another of your Echos')));
  for (const m of members) box.append(h('button', { class: 'big', style: 'margin-top:8px', onclick: () => askThrough(m) }, `Log in through ${m.node}`));
}

async function askThrough(m) {
  const msg = $('loginmsg'), base = baseOf(m.ip);
  let nonce;
  try { nonce = await echo.nonce(); } catch (e) { msg.textContent = 'This Echo did not answer.'; return; }
  const req = { target: echo.hello.pub, pub: hex(me.pub), nonce, origin: location.origin, name: echo.hello.name };
  const w = window.open(`${base}/#vouch=${encodeURIComponent(JSON.stringify(req))}`, 'hm-vouch', 'width=480,height=640');
  if (!w) { msg.textContent = 'The browser blocked the window: allow pop-ups for this page and try again.'; return; }
  msg.className = 'login-msg'; msg.textContent = `Confirm in the window of ${m.node}…`;
  const onMsg = async (ev) => {
    if (ev.origin !== base || !ev.data || ev.data.type !== 'hm-voucher' || ev.data.nonce !== nonce) return;
    window.removeEventListener('message', onMsg);
    const r = await echo.voucherLogin(nonce, ev.data).catch(() => 'refused');
    if (r === 'approved') { $('login').classList.add('hidden'); await load(); toast(`Logged in through ${ev.data.via}`); }
    else { msg.textContent = 'Not let in: the two Echos are not in the same network any more.'; msg.classList.add('bad'); }
  };
  window.addEventListener('message', onMsg);
}

// This page was opened by another Echo's login page (#vouch=...): once logged in here, ask, check that the asker is an
// Echo of our network with the key it named, then vouch for that browser and hand the voucher back.
async function answerVouch() {
  const m = location.hash.match(/^#vouch=(.*)$/);
  if (!m) return;
  history.replaceState(null, '', location.pathname);
  let q; try { q = JSON.parse(decodeURIComponent(m[1])); } catch (e) { return; }
  const host = (() => { try { return new URL(q.origin).hostname; } catch (e) { return ''; } })();
  const members = (state.arbitration && state.arbitration.members) || [];
  let ok = members.some((x) => x.ip === host);
  if (ok) { try { ok = (await (await fetch(q.origin + '/api/hello')).json()).pub === q.target; } catch (e) { ok = false; } }
  const box = h('div', { class: 'callout ' + (ok ? 'info' : 'bad') }, icon(ok ? 'key' : 'warn'));
  $('attention').prepend(box);
  if (!ok || !window.opener) { box.append(h('p', {}, `A page at ${q.origin} asked to be let in through this Echo, but it is not one of the Echos in this one's network. Nothing was sent.`)); return; }
  const allow = h('button', { class: 'primary small' }, 'Allow'), no = h('button', { class: 'small' }, 'No');
  box.append(h('div', {}, h('p', {}, `Log this browser in on ${q.name || host} (${host}) through ${echo.hello.name}?`),
    h('div', { class: 'btns', style: 'margin-top:8px' }, allow, no)));
  no.onclick = () => window.close();
  allow.onclick = async () => {
    try {
      const v = (await echo.call('POST', '/api/vouch/issue', `${q.target} ${q.pub} ${q.nonce}`)).json();
      window.opener.postMessage({ type: 'hm-voucher', nonce: q.nonce, voucher: v.voucher, via: v.via }, q.origin);
      setTimeout(() => window.close(), 300);
    } catch (e) { toast(errText(e)); }
  };
}

$('ask').onclick = async () => {
  const msg = $('loginmsg'), art = $('art'), b = $('ask');
  msg.className = 'login-msg'; msg.textContent = 'Waiting… press the action button on the Echo now.';
  art.classList.add('wait'); b.disabled = true;
  const done = (text, bad) => { art.classList.remove('wait'); b.disabled = false; msg.textContent = text; if (bad) msg.classList.add('bad'); };
  for (let i = 0; i < 70; i++) {
    let s;
    try { s = await echo.login(); } catch (e) { done('The Echo did not answer. Is it still on the network?', true); return; }
    if (s === 'approved') { art.classList.remove('wait'); $('login').classList.add('hidden'); await load(); toast('Logged in'); return; }
    if (s === 'refused') { done('Not approved: the button was not pressed within a minute, or another browser asked at the same time. Try again.', true); return; }
    await new Promise((r) => setTimeout(r, 1000));
  }
  done('No button press came. Try again.', true);
};

// Identify (main.c core_identify): rainbow ring for 10 s and stock's setup sound, on this Echo or another logged-in one
async function identify(e, name) {
  try { await e.call('POST', '/api/identify'); toast(`${name}: rainbow on the ring for 10 seconds`); } catch (err) { toast(`${name}: ${errText(err)}`); }
}
$('identify').onclick = () => identify(echo, echo.hello.name);

// ---------------------------------------------------------------- state

async function load() {
  state = await (await echo.call('GET', '/api/state')).json();
  // updated meanwhile (online update, push): this page is the old version's, its header the old number
  if (state.device.version !== echo.hello.version) { location.reload(); return; }
  if (state.device.name !== echo.hello.name || state.device.node !== echo.hello.node) {      // renamed (hassmic.conf NAME)
    echo.hello.name = state.device.name; echo.hello.node = state.device.node; showName(echo.hello);
  }
  render();
  $('app').classList.remove('hidden');
  if (load.started && unseen()) refreshDevices().catch(() => {});        // a new Echo in the beacons: show it now, not next round
  else if (load.started && !devRun) refreshMine().catch(() => {});       // a setting changed here or from HA: copy offers the new value
  if (!load.started) {
    load.started = true;
    answerVouch();
    refreshDevices().catch(() => {});
    getDavs().then(() => { renderDavs(); davsPoll(); }).catch(() => {});
    setInterval(() => { if (!document.hidden) load().catch(() => {}); }, 15000);
    setInterval(() => { if (!document.hidden) refreshDevices().catch(() => {}); }, 30000);
  }
}

async function set(name, value) {
  try {
    const r = await (await echo.call('POST', '/api/set', `${name}=${value}\n`)).json();
    if (r.errors) toast(r.errors.trim());
    else {
      const s = state.settings.find((x) => x.name === name);
      if (s && s.feature) toast('Saved. Home Assistant reconnects to update its entities.');
      flash(name);
    }
  } catch (e) { toast(errText(e)); }
  await load();
  // once more shortly: what the change set off (a feature that cannot start switches itself back off) shows without the 15 s wait
  clearTimeout(set.again); set.again = setTimeout(() => load().catch(() => {}), 1500);
}

function flash(name) {
  const c = controls.get(name);
  if (!c || !c.saved) return;
  c.saved.classList.add('show'); clearTimeout(c.saved.t); c.saved.t = setTimeout(() => c.saved.classList.remove('show'), 1600);
}

// ---------------------------------------------------------------- controls (built once, updated in place)

const controls = new Map();             // setting name -> { update(s), saved, root }
let built = '';

function control(s) {
  const id = 's-' + s.name;
  if (s.type === 'bool') {
    const i = h('input', { type: 'checkbox', id, role: 'switch', onchange: () => set(s.name, i.checked ? 'on' : 'off') });
    return { el: h('label', { class: 'switch' }, i, h('span')), update: (x) => { i.checked = !!x.value; } };
  }
  if (s.type === 'int') {
    const r = h('input', { type: 'range', id, min: String(s.min), max: String(s.max), step: String((HELP[s.name] || {}).step || 1) });
    const out = h('output', { for: id });
    const paint = () => { r.style.setProperty('--p', `${((r.value - s.min) * 100) / (s.max - s.min)}%`); out.textContent = fmt(s, +r.value); };
    r.oninput = paint; r.onchange = () => set(s.name, r.value);
    return { el: h('div', { class: 'slider' }, r, out), update: (x) => { if (!r.matches(':active')) { r.value = x.value; paint(); } } };     // not while dragging
  }
  const names = CHOICE_NAMES[s.name] || {}, modes = (HELP[s.name] || {}).modes;
  if (modes) {                          // choices worth weighing: a card each, with what it gives and what it costs
    const cards = s.choices.map((c) => {
      const m = modes[c], pick = () => set(s.name, c);
      return h('div', { class: 'mode', role: 'radio', tabindex: '0', 'aria-checked': 'false', onclick: pick,
        onkeydown: (e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); pick(); } } },
        h('div', { class: 'mode-head' }, h('span', { class: 'radio' }), h('b', {}, m.title), m.tag ? h('span', { class: 'badge' }, m.tag) : null),
        h('p', { class: 'mode-sub' }, m.sub),
        h('ul', { class: 'pc' }, m.pros.map((t) => h('li', { class: 'pro' }, icon('plus'), h('span', {}, t))),
          m.cons.map((t) => h('li', { class: 'con' }, icon('minus'), h('span', {}, t)))));
    });
    return { el: h('div', { class: 'modes', role: 'radiogroup', 'aria-label': s.label }, cards),
             update: (x) => cards.forEach((b, i) => { b.classList.toggle('on', i === x.value); b.setAttribute('aria-checked', String(i === x.value)); }) };
  }
  if (s.choices.length <= 4) {
    const btns = s.choices.map((c) => h('button', { type: 'button', onclick: () => set(s.name, c) }, names[c] || c));
    return { el: h('div', { class: 'seg', role: 'group', 'aria-label': s.label }, btns),
             update: (x) => btns.forEach((b, i) => { b.classList.toggle('on', i === x.value); b.setAttribute('aria-pressed', String(i === x.value)); }) };
  }
  const sel = h('select', { id, onchange: () => set(s.name, sel.value) }, s.choices.map((c) => h('option', { value: c }, names[c] || c)));
  return { el: sel, update: (x) => { sel.value = s.choices[x.value]; } };
}

function haBadge() { return h('span', { class: 'badge ha', title: 'Also in Home Assistant' }, icon('ha'), 'Home Assistant'); }

function settingRow(s) {
  const info = HELP[s.name] || {}, c = control(s), saved = h('span', { class: 'saved' }, 'Saved');
  const wide = s.type === 'int' || !!info.modes, stack = s.type === 'choice';      // stack: control under the text on a phone
  const row = h('div', { class: 'row' + (wide ? ' wide' : '') + (stack ? ' stack' : '') },
    h('div', {},
      h('label', { class: 'row-label', for: 's-' + s.name }, label(s), info.ha === true ? haBadge() : null, saved),
      info.help ? h('p', { class: 'help' }, info.help) : null),
    h('div', { class: 'ctl' }, c.el));
  controls.set(s.name, { update: c.update, saved, root: row });
  return row;
}

function featureCard(s, children) {
  const info = HELP[s.name] || {}, c = control(s), saved = h('span', { class: 'saved' }, 'Saved');
  const kids = children.map((k) => { const r = settingRow(k); r.classList.add('f-child'); return r; });
  const status = h('div');
  const card = h('article', { class: 'feature' },
    h('div', { class: 'f-head' }, h('span', { class: 'f-icon' }, icon(info.icon || 'sparkle')),
      h('label', { class: 'f-title', for: 's-' + s.name }, label(s), info.tag ? h('span', { class: 'badge warn' }, info.tag) : null, saved), c.el),
    info.help ? h('p', { class: 'f-help' }, info.help) : null,
    info.note ? h('p', { class: 'f-note' }, info.note) : null,
    status,
    kids,
    info.ha ? h('div', { class: 'f-ha' }, icon('ha'), h('span', {}, 'In Home Assistant while on: ', info.ha.map((n, i) => [i ? ', ' : '', h('b', {}, n)]))) : null);
  controls.set(s.name, { saved, root: card, update: (x) => {
    c.update(x); card.classList.toggle('on', !!x.value);
    kids.forEach((k, i) => { const w = (HELP[children[i].name] || {}).when; k.classList.toggle('hidden', !x.value || !!(w && !w())); });
    if (info.status) status.replaceChildren(...[info.status()].filter(Boolean));
  } });
  return card;
}

function sectionShell(sec) {
  return h('section', { class: 'section', id: 'sec-' + sec.id },
    h('div', { class: 'sec-head' }, h('span', { class: 'ic' }, icon(sec.icon)), h('h2', {}, sec.title || sec.id)),
    sec.intro ? h('p', { class: 'intro' }, sec.intro) : null);
}

function build() {
  controls.clear();
  const root = $('sections'), nav = $('nav');
  root.innerHTML = ''; nav.innerHTML = '';
  const by = new Map();
  for (const s of state.settings) { if (!by.has(s.group)) by.set(s.group, []); by.get(s.group).push(s); }
  const order = [...SECTIONS, ...[...by.keys()].filter((g) => !SECTIONS.some((x) => x.id === g)).map((id) => ({ id, icon: 'sliders' }))];
  for (const sec of order) {
    const list = (by.get(sec.id) || []).filter((s) => s.name !== 'arbitration');
    if (!list.length && sec.id !== 'Echos' && sec.id !== 'System') continue;
    const el = sectionShell(sec);
    if (sec.id === 'Features') {
      const grid = h('div', { class: 'features' });
      for (const s of list.filter((x) => x.feature)) grid.append(featureCard(s, list.filter((k) => (HELP[k.name] || {}).parent === s.name)));
      el.append(grid);
      const loose = list.filter((x) => !x.feature && !(HELP[x.name] || {}).parent);
      if (loose.length) el.append(h('div', { class: 'card' }, loose.map(settingRow)));
    } else if (sec.id === 'Sound') {
      const eq = list.filter((s) => s.name.startsWith('equalizer_')), rest = list.filter((s) => !eq.includes(s));
      if (eq.length) el.append(h('div', { class: 'card' }, h('div', { class: 'eq' }, eq.map(settingRow)),
        h('div', { class: 'card-foot eq-foot' }, h('span', { class: 'help' }, haBadge(), ' also as number entities'),
          h('button', { class: 'small', onclick: async () => { for (const s of eq) await set(s.name, 0); } }, 'Flat'))));
      if (rest.length) el.append(h('div', { class: 'card' }, rest.map(settingRow)));
    } else if (sec.id === 'Echos') {
      buildEchos(el, state.settings.find((s) => s.name === 'arbitration'));
    } else if (sec.id === 'System') {
      if (list.length) el.append(h('div', { class: 'card' }, list.map(settingRow)));
      buildSystem(el);
    } else el.append(h('div', { class: 'card' }, list.map(settingRow)));
    root.append(el);
    nav.append(h('a', { href: '#sec-' + sec.id, 'data-sec': sec.id }, icon(sec.icon), sec.title || sec.id));
  }
  $('foot').textContent = `hassmic ${state.device.version} · This page is plain HTTP, so it never shows keys or tokens; every change is signed by this browser.`;
  watchNav();
}

function render() {
  const sig = state.settings.map((s) => s.name).join() + '|' + !!state.arbitration;
  if (sig !== built) { built = sig; build(); }
  for (const s of state.settings) { const c = controls.get(s.name); if (c) c.update(s); }
  renderTop(); renderAttention(); renderNetwork(); renderDevices(); renderSystem();
}

function renderTop() {
  const d = state.diag, p = $('pills'); p.innerHTML = '';
  p.append(h('span', { class: 'pill ' + (state.ha ? 'ok' : 'bad') }, h('span', { class: 'd' }), state.ha ? 'Home Assistant connected' : 'Home Assistant not connected'));
  if (d.soc_temp !== null) p.append(h('span', { class: 'pill', title: 'Chip temperature' }, `${Math.round(d.soc_temp)} °C`));
  if (d.cpu !== null) p.append(h('span', { class: 'pill', title: 'CPU usage' }, `CPU ${d.cpu} %`));
}

function renderAttention() {
  const box = $('attention'); box.innerHTML = '';
  for (const text of state.warnings) {
    const ha = /Home Assistant is not connected/.test(text);
    box.append(h('div', { class: 'callout' + (ha ? ' bad' : '') }, icon('warn'),
      h('p', {}, ha ? 'Home Assistant is not connected. The Echo cannot answer requests until it is; check that the ESPHome integration has this device and that both are on the same network.' : text)));
  }
}

function watchNav() {
  if (watchNav.o) watchNav.o.disconnect();
  const links = [...$('nav').querySelectorAll('a')];
  const o = new IntersectionObserver((es) => {
    for (const e of es) if (e.isIntersecting) links.forEach((a) => a.classList.toggle('cur', a.dataset.sec === e.target.id.slice(4)));
  }, { rootMargin: '-80px 0px -65% 0px' });
  document.querySelectorAll('.section').forEach((s) => o.observe(s));
  watchNav.o = o;
}

// ---------------------------------------------------------------- Echos: the network, arbitration, settings sync

const escape = (t) => String(t).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]);
const others = new Map();                // base URL -> { echo, hello, logged, diff, error }
const baseOf = (ip) => ip.includes(':') ? `http://${ip}` : `http://${ip}:${location.port || 80}`;   // host:port when added so
const plural = (n, one, many = one + 's') => `${n} ${n === 1 ? one : many}`;
const nameList = (l) => l.length <= 2 ? l.join(' and ') : `${l.slice(0, -1).join(', ')} and ${l[l.length - 1]}`;
let netHead, netWarn, devBox, syncBox, modelBox;

function cardHead(title, help, ic, extra) {
  return h('div', { class: 'card-head' }, ic ? h('span', { class: 'ic' }, icon(ic)) : null,
    h('div', { class: 'grow' }, h('h3', {}, title), help ? h('p', { class: 'help' }, help) : null), extra || null);
}
function empty(ic, text, ok) { return h('div', { class: 'empty' + (ok ? ' ok' : '') }, h('span', { class: 'ic' }, icon(ic)), h('div', {}, text)); }

function buildEchos(el, arbSetting) {
  el.append(h('p', { class: 'intro' }, 'Your Echos find each other on the local network and share a key, so nothing else on the network can join them or silence them. This always runs: it is also how this page finds your other Echos to copy settings and models to.'));
  if (arbSetting) {
    const c = featureCard(arbSetting, state.settings.filter((k) => (HELP[k.name] || {}).parent === 'arbitration'));
    c.classList.add('solo'); el.append(c);
  }
  netHead = h('div', { class: 'net-line' });
  netWarn = h('div', { class: 'net-warn' });
  devBox = h('div', { class: 'echos' });
  scanMark = h('span', { class: 'scan hidden', title: 'Asking your Echos' }, h('span', { class: 'spin' }), 'Checking…');
  const input = h('input', { type: 'text', placeholder: 'IP address', 'aria-label': 'IP address of another Echo' });
  const add = () => {
    const ip = input.value.trim();
    if (!/^[0-9a-zA-Z.:-]+$/.test(ip)) { toast('That is not an address'); return; }
    const l = new Set(store('hm.extra') || []); l.add(ip); store('hm.extra', [...l]); input.value = '';
    toast('Looking for it…'); refreshDevices().catch(() => {});
  };
  input.onkeydown = (e) => { if (e.key === 'Enter') add(); };
  el.append(h('div', { class: 'card' },
    h('div', { class: 'card-head' }, h('div', { class: 'grow' }, h('h3', {}, 'Your Echos'), netHead), scanMark),
    netWarn, devBox,
    h('div', { class: 'card-foot' }, h('span', { class: 'help grow add-help' }, 'One missing? Echos on another subnet do not show up by themselves.'),
      input, h('button', { onclick: add }, icon('plus'), 'Add'))));
  syncBox = h('div', { class: 'card' });
  davsBox = h('div', { class: 'card' });
  modelBox = h('div', { class: 'card' });
  el.append(syncBox, davsBox, modelBox);
}

function renderNetwork() {
  if (!netHead) return;
  const a = state.arbitration; netHead.innerHTML = ''; netWarn.innerHTML = '';
  if (!a) { netHead.append('The Echo network is switched off on this Echo (started with -a 0).'); return; }
  const n = a.members.length + 1;
  netHead.append(...[h('span', { class: 'dot' + (a.network && n > 1 ? ' ok' : '') }),
    a.network ? (n > 1 ? `${n} Echos share a network` : 'Only this Echo in its network') : 'Looking for other Echos…',
    a.network ? h('code', { title: 'Network id' }, a.network) : null,
    a.pairing ? h('span', { class: 'badge acc' }, 'Pairing with the volume keys') : null].filter(Boolean));
  const out = a.others || [];
  const stuck = out.filter((o) => o.state === 'none' && o.for_s > 60);
  const nets = new Set(out.filter((o) => o.network).map((o) => o.network));
  const warn = (text) => netWarn.append(h('div', { class: 'callout' }, icon('warn'), h('p', {}, text)));
  // Same 60 s as the hint arb.c logs: the tags take a read, a scan and a read again, the action 30 s
  const waiting = out.filter((o) => o.network && o.state === 'older' && o.for_s > 60).map((o) => o.node);
  if (state.ha && waiting.length)
    warn(`This Echo has not got the key from ${nameList(waiting)} for over a minute. Home Assistant has not confirmed the Echos' tags: its Tags integration (part of the default configuration) may be off, or one of them is not adopted there. Tick "Allow the device to perform Home Assistant actions" in ${waiting.length > 1 ? 'their' : 'its'} ESPHome options, or pair with the volume keys.`);
  if (nets.size) warn(`${nets.size === 1 ? 'A second network' : nets.size + ' other networks'} beside this one: its Echos do not settle wake words with these. They merge by themselves when Home Assistant can carry the key; if they do not, pair them with the volume keys.`);
  const myMode = a.arbitrates ? a.mode || 'hassmic' : null;       // older Echos say nothing: they are in ours
  const odd = a.members.filter((m) => myMode && m.arbitrates && (m.mode || 'hassmic') !== myMode).map((m) => m.node);
  if (odd.length) warn(`${nameList(odd)} ${odd.length > 1 ? 'use' : 'uses'} the other arbitration mode (${myMode === 'kiosk' ? 'Echo network' : 'Kiosk Satellite'}): this Echo and ${odd.length > 1 ? 'they' : 'it'} do not settle wake words with each other, and both may answer. Set the same mode on every Echo.`);
  if (stuck.length) warn(`${nameList(stuck.map((o) => o.node))} ${stuck.length > 1 ? 'have' : 'has'} not got the key for over a minute. Usually Home Assistant has not confirmed ${stuck.length > 1 ? 'their' : 'its'} tag: its Tags integration (part of the default configuration) is off, or ${stuck.length > 1 ? 'they are' : 'it is'} not adopted there. Pair with the volume keys instead.`);
  const more = h('details', { class: 'more', open: renderNetwork.open, ontoggle: () => { renderNetwork.open = more.open; } });
  netWarn.append(more);
  more.append(h('summary', {}, 'How Echos get the key'),
    h('ul', {},
      h('li', {}, 'Through Home Assistant, by themselves: each Echo reports a tag named after its key as scanned (Settings › Tags lists one "Tag hassmic_…" per Echo), and an Echo hands the key only to one whose tag Home Assistant confirms. Nothing to set up; it needs the Tags integration, part of the default configuration.'),
      h('li', {}, 'Or, without tags: tick "Allow the device to perform Home Assistant actions" in each Echo\'s ESPHome options.'),
      h('li', {}, 'Without Home Assistant: hold Volume up and Volume down together for 2 s on the new Echo, then on one already in. A tap sounds; the Bluetooth "connected" sound when it worked.')));
}

async function exportOf(e) { return (await e.call('GET', '/api/export')).text(); }
const parse = (text) => new Map(text.split('\n').filter((l) => l.includes('=') && !l.startsWith('#')).map((l) => { const i = l.indexOf('='); return [l.slice(0, i), l.slice(i + 1)]; }));
let mine = new Map(), myArts = null, scanMark = null, devRun = null, devNext = null;
const selfAt = new Set();                // this Echo's own other addresses
const within = (ms) => { const c = new AbortController(); setTimeout(() => c.abort(), ms); return c.signal; };

// Addresses to look at: the ones added by hand, and every Echo this one hears beacons from
function knownIps() {
  const ips = new Set(store('hm.extra') || []);
  const a = state.arbitration;
  if (a) for (const m of [...a.members, ...(a.others || [])]) if (m.ip) ips.add(m.ip);
  return ips;
}
const unseen = () => [...knownIps()].some((ip) => { const b = baseOf(ip); return b !== location.origin && !selfAt.has(b) && !others.has(b); });

// One round at a time; a call during a round gets one more after it, so what a login or a copy changed shows
function refreshDevices() {
  if (devRun) return devNext || (devNext = devRun.catch(() => {}).then(() => { devNext = null; return refreshDevices(); }));
  devRun = scanDevices().finally(() => { devRun = null; renderDevices(); });
  renderDevices();
  return devRun;
}

// All Echos at once, each shown as soon as it answers: one at a time, a single dead address held the whole list back
async function scanDevices() {
  mine = parse(await exportOf(echo));
  try { myArts = (await echo.call('GET', '/api/artifacts')).json(); } catch (e) { myArts = null; }
  const ips = knownIps(), jobs = [];
  for (const [base, d] of others) if (!ips.has(d.ip)) others.delete(base);     // moved, or added by hand and removed
  for (const ip of ips) {
    const base = baseOf(ip);
    if (base === location.origin || selfAt.has(base)) continue;
    if (!others.has(base)) others.set(base, { ip });
    jobs.push(probe(base).finally(renderDevices));
  }
  renderDevices();
  await Promise.all(jobs);
}

async function probe(base) {
  const d = others.get(base);
  d.looking = true;
  try {
    if (!d.echo) {                       // also after a miss: an Echo that was off or rebooting comes back by itself
      const hello = await (await fetch(base + '/api/hello', { signal: within(5000) })).json();
      if (hello.pub === echo.hello.pub) { others.delete(base); selfAt.add(base); return; }
      d.echo = new Echo(base, hello);
    }
    d.error = null;
    const read = async () => {
      d.values = parse(await exportOf(d.echo)); d.logged = true;
      rediff(d);
      try { d.arts = (await d.echo.call('GET', '/api/artifacts')).json(); } catch (e) { d.arts = null; }
    };
    try { await read(); } catch (e) {
      d.logged = false;
      if (e.message === 'old') d.error = 'Older hassmic: update it';
      else if (e.message !== 'login') throw e;
      // a member of this Echo's network: this Echo vouches for us there, no button needed (tried once per page)
      else if ((arbOf(d.ip) || {}).member && !d.vouchTried) {
        d.vouchTried = true;
        if (await d.echo.through(echo).catch(() => 'refused') === 'approved') await read().catch(() => {});
      }
    }
  } catch (e) { d.error = 'Not reachable'; }
  finally { d.looking = false; }
}

const rediff = (d) => { d.diff = [...mine].filter(([k, v]) => d.values.has(k) && d.values.get(k) !== v).map(([k]) => k); };

// This Echo's values only, between rounds: the other Echos are read every 30 s, but what is copied is what this one has now
async function refreshMine() {
  const now = parse(await exportOf(echo));
  if (devRun || (now.size === mine.size && [...now].every(([k, v]) => mine.get(k) === v))) return;
  mine = now;
  for (const d of others.values()) if (d.values) rediff(d);
  renderDevices();
}

function arbOf(ip) {
  const a = state.arbitration; if (!a) return null;
  const host = ip.split(':')[0];
  const m = a.members.find((x) => x.ip === host); if (m) return { member: true, ...m };
  const o = (a.others || []).find((x) => x.ip === host); return o ? { member: false, ...o } : null;
}

function netBadge(r) {
  if (!r) return null;
  if (r.member) return h('span', { class: 'badge ok' }, 'In the network');
  if (r.state === 'none') return r.for_s > 60 ? h('span', { class: 'badge warn', title: 'It has not been handed the key' }, 'No key yet') : h('span', { class: 'badge' }, 'Joining…');
  if (r.state === 'younger') return h('span', { class: 'badge warn', title: 'It should move into this network' }, 'Other network');
  return h('span', { class: 'badge warn', title: 'This Echo should move into that network' }, 'Older network');
}

const ready = () => [...others.values()].filter((d) => d.echo && d.logged && !d.error);
const shortAddr = (base) => base.replace('http://', '').replace(`:${location.port || 80}`, '');

function renderDevices() { if (devBox) keepFocus(devBox, drawDevices); }
function drawDevices() {
  scanMark.classList.toggle('hidden', !devRun);
  devBox.innerHTML = '';
  const a = state.arbitration;
  devBox.append(h('div', { class: 'echo me' }, h('span', { class: 'mini-puck on' }),
    h('div', { class: 'echo-body' }, h('div', { class: 'echo-name' }, echo.hello.name), h('div', { class: 'echo-addr' }, location.host),
      h('div', { class: 'echo-meta' }, h('span', { class: 'badge acc' }, 'This Echo'), a && !a.arbitrates ? h('span', { class: 'badge' }, 'Arbitration off')
        : a && a.mode === 'kiosk' ? h('span', { class: 'badge' }, 'Kiosk Satellite mode') : null),
      h('div', { class: 'echo-acts' }, idButton(echo, echo.hello.name)))));
  for (const [base, d] of others) {
    const r = arbOf(d.ip), name = d.echo ? d.echo.hello.name : d.ip;
    const meta = [];
    if (!d.echo && d.looking) meta.push(h('span', { class: 'badge' }, h('span', { class: 'spin' }), 'Looking…'));
    else if (d.error) meta.push(h('span', { class: 'badge bad' }, d.error));
    else if (!d.logged) meta.push(h('span', { class: 'badge' }, 'Not logged in'));
    else if (d.diff.length) meta.push(h('span', { class: 'badge warn' }, `${plural(d.diff.length, 'setting')} differ${d.diff.length === 1 ? 's' : ''}`));
    else meta.push(h('span', { class: 'badge ok' }, icon('check'), 'Same settings'));
    meta.push(netBadge(r));
    if (r && r.member && r.arbitrates === false) meta.push(h('span', { class: 'badge', title: 'It answers every wake word itself' }, 'Arbitration off'));
    else if (r && r.member && r.mode === 'kiosk') meta.push(h('span', { class: 'badge', title: 'It settles wake words the Kiosk Satellite way' }, 'Kiosk Satellite mode'));
    const login = d.echo && d.logged && !d.error ? h('div', { class: 'echo-acts' }, idButton(d.echo, name))
      : d.echo && !d.logged && !d.error ? h('button', { class: 'small primary', disabled: !!d.asking, onclick: () => loginOther(d) }, icon('key'), d.asking ? 'Press its button…' : 'Log in') : null;
    devBox.append(h('div', { class: 'echo' }, h('span', { class: 'mini-puck' + (r && r.member ? ' on' : '') }),
      h('div', { class: 'echo-body' }, h('div', { class: 'echo-name' }, name), h('div', { class: 'echo-addr' }, shortAddr(base)),
        h('div', { class: 'echo-meta' }, meta), login),
      h('a', { class: 'echo-go', href: base + '/', title: `${name}: its settings page`, 'aria-label': `Open the settings page of ${name}` }, icon('ext'))));
  }
  renderSync(); renderDavs(); renderModels();
}

// The list is redrawn every 15 s and on every answer: the wait is kept on d, so a redraw neither resets the button nor
// lets a second click start a second wait
const idButton = (e, name) => h('button', { class: 'small', 'data-k': 'id ' + e.base, title: `Rainbow on ${name}'s ring for 10 s and a sound`,
  onclick: () => identify(e, name) }, icon('sparkle'), 'Identify');

async function loginOther(d) {
  d.asking = true; renderDevices();
  toast(`Press the action button on ${d.echo.hello.name}`);
  try {
    for (let i = 0; i < 70; i++) {
      let s;
      try { s = await d.echo.login(); } catch (e) { break; }
      if (s === 'approved') { d.asking = false; await refreshDevices(); toast(`Logged in to ${d.echo.hello.name}`); return; }
      if (s === 'refused') break;
      await new Promise((r) => setTimeout(r, 1000));
    }
    toast(`${d.echo.hello.name}: not approved`);
  } finally { d.asking = false; renderDevices(); }
}

// The cards below are redrawn whole (every 15 s, on every answer of another Echo, on a tick that changes what is listed):
// what had the keyboard focus gets it back, found by its data-k, or keyboard users start over at the top each time
function keepFocus(box, draw) {
  const a = document.activeElement, k = a && box.contains(a) ? a.dataset.k : null;
  draw();
  if (!k) return;
  const e = [...box.querySelectorAll('[data-k]')].find((x) => x.dataset.k === k);
  if (e && !e.disabled) e.focus();
}

// Echos to pick as targets, as toggle chips
function chips(list) {
  return h('div', { class: 'chips', role: 'group' }, list.map((x) => h('button', { type: 'button', class: 'chip' + (x.on ? ' on' : ''), 'aria-pressed': String(x.on),
    disabled: x.disabled, 'data-k': x.k, onclick: x.toggle }, h('span', { class: 'tick' }, icon('check')), x.name, x.note ? h('span', { class: 'chip-n ' + (x.cls || '') }, x.note) : null)));
}

// ---------------------------------------------------------------- copy settings: which ones, to which Echos

// What is ticked: the user's own ticks (name or Echo -> true/false), else the default: every listed setting, Echos where
// something differs.  Only settings that differ on a picked Echo are listed: copying the rest would change nothing.
const pick = { rows: new Map(), to: new Map() };
const ticked = (m, k, dflt) => (m.has(k) ? m.get(k) : dflt);
let syncing = false;

function pretty(name, raw) {
  if (raw === undefined) return '—';
  const s = state.settings.find((x) => x.name === name);
  if (!s) return raw;
  if (s.type === 'bool') return raw === 'on' ? 'On' : 'Off';
  if (s.type === 'choice') return (CHOICE_NAMES[name] || {})[raw] || raw;
  return fmt(s, +raw);
}

function checkbox(on, onchange, labelText, k) {
  const i = h('input', { type: 'checkbox', checked: on, 'aria-label': labelText, 'data-k': k }); i.onchange = () => onchange(i.checked); return i;
}

function renderSync() { if (syncBox) keepFocus(syncBox, drawSync); }
function drawSync() {
  syncBox.innerHTML = '';
  const targets = ready(), names = [...mine.keys()];
  syncBox.append(cardHead('Copy settings', `From ${echo.hello.name} to your other Echos. Only what differs is listed; a setting an Echo does not have (no Bluetooth, no Wi-Fi motion) is left out there.`, 'sliders'));
  if (!targets.length) {
    syncBox.append(empty('net', others.size ? 'Log in to another Echo above (its own action button, once) to copy settings to it.'
      : 'No other Echo found yet. Echos running hassmic on the same network show up above by themselves within a minute.'));
    return;
  }
  const differs = (n, d) => d.values.has(n) && d.values.get(n) !== mine.get(n);
  const toOn = (d) => ticked(pick.to, d.echo.base, d.diff.length > 0);
  const sel = targets.filter(toOn);
  const waiting = [...others.values()].filter((d) => d.echo && !d.logged && !d.error).length;
  syncBox.append(h('div', { class: 'pickbar' }, h('span', { class: 'pick-l' }, 'Copy to'),
    chips(targets.map((d) => ({ name: d.echo.hello.name, on: toOn(d), disabled: syncing, k: 'to ' + d.echo.base,
      note: d.diff.length ? String(d.diff.length) : 'same', cls: d.diff.length ? 'warn' : 'ok',
      toggle: () => { pick.to.set(d.echo.base, !toOn(d)); renderSync(); } }))),
    waiting ? h('span', { class: 'pick-hint' }, `${plural(waiting, 'more Echo')} after logging in`) : null));
  const listed = names.filter((n) => sel.some((d) => differs(n, d)));
  if (!sel.length) { syncBox.append(empty('info', 'Pick the Echos to copy to.')); return; }
  if (!listed.length) {
    syncBox.append(empty('check', h('span', {}, h('b', {}, nameList(sel.map((d) => d.echo.hello.name))), ` ${sel.length > 1 ? 'have' : 'has'} the same settings as this Echo.`), true));
    return;
  }
  const rowOn = (n) => ticked(pick.rows, n, true);
  const setting = (n) => state.settings.find((x) => x.name === n);
  const groups = new Map();
  for (const n of listed) { const g = (setting(n) || {}).group || 'Other'; if (!groups.has(g)) groups.set(g, []); groups.get(g).push(n); }
  const order = [...groups.keys()].sort((p, q) => ((SECTIONS.findIndex((x) => x.id === p) + 99) % 99) - ((SECTIONS.findIndex((x) => x.id === q) + 99) % 99));
  const list = h('div', { class: 'changes' }), rows = [];
  for (const g of order) {
    list.append(h('div', { class: 'grp' }, g));
    for (const n of groups.get(g)) {
      const s = setting(n), to = mine.get(n);
      const lines = sel.map((d) => {
        const v = d.values.get(n), who = h('span', { class: 'ch-echo', title: d.echo.hello.name }, d.echo.hello.name);
        if (v === undefined) return h('div', { class: 'ch-line same' }, who, 'not on that model');
        if (v === to) return h('div', { class: 'ch-line same' }, who, icon('check'), 'already ', pretty(n, v));
        return h('div', { class: 'ch-line' }, who, h('span', { class: 'old' }, pretty(n, v)), icon('arrow'), h('span', { class: 'new' }, pretty(n, to)));
      });
      const cb = checkbox(rowOn(n), (on) => { pick.rows.set(n, on); row.classList.toggle('on', on); row.classList.toggle('off', !on); foot(); }, s ? s.label : n, 'row ' + n);
      cb.disabled = syncing;
      const row = h('label', { class: 'change ' + (rowOn(n) ? 'on' : 'off') }, cb, h('div', { class: 'ch-name' }, s ? label(s) : n), h('div', { class: 'ch-to' }, lines));
      rows.push(row); list.append(row);
    }
  }
  syncBox.append(list);
  const same = names.filter((n) => !listed.includes(n)).length;
  const footBox = h('div', { class: 'card-foot sync-foot' });
  const foot = () => {
    const chosen = listed.filter(rowOn), changes = chosen.reduce((k, n) => k + sel.filter((d) => differs(n, d)).length, 0);
    const go = h('button', { class: 'primary', 'data-k': 'go', disabled: syncing || !chosen.length, onclick: () => copySettings(chosen, sel) },
      syncing ? [h('span', { class: 'spin' }), 'Copying…'] : `Copy to ${sel.length === 1 ? sel[0].echo.hello.name : plural(sel.length, 'Echo')}`);
    footBox.replaceChildren(
      h('div', { class: 'count grow' }, h('b', {}, plural(chosen.length, 'setting')), ` of ${listed.length}`, changes !== chosen.length ? `, ${plural(changes, 'change')}` : '',
        same ? ` · ${same} already match` : '', ' ',
        h('button', { class: 'link', disabled: syncing, 'data-k': 'all', onclick: () => { listed.forEach((n) => pick.rows.set(n, true)); renderSync(); } }, 'All'),
        h('button', { class: 'link', disabled: syncing, 'data-k': 'none', onclick: () => { listed.forEach((n) => pick.rows.set(n, false)); renderSync(); } }, 'None')),
      go);
  };
  foot();
  syncBox.append(footBox);
}

async function copySettings(names, list) {
  syncing = true; renderSync();
  const text = names.filter((n) => mine.has(n)).map((n) => `${n}=${mine.get(n)}\n`).join('');
  const notes = [];
  for (const d of list) {
    try {
      const r = (await d.echo.call('POST', '/api/set', text)).json();
      // what that model has not got (Bluetooth, Wi-Fi motion) is no error worth showing
      const errs = r.errors.split('\n').filter((x) => x && !x.includes('no such setting'));
      if (errs.length) notes.push(`${d.echo.hello.name}: ${errs.join('; ')}`);
    } catch (e) { notes.push(`${d.echo.hello.name}: ${errText(e)}`); }
  }
  toast(notes.length ? notes.join(' · ') : `Copied to ${nameList(list.map((d) => d.echo.hello.name))}`);
  pick.rows.clear(); pick.to.clear(); syncing = false;
  await refreshDevices();
}

// ---------------------------------------------------------------- download Amazon's models on this Echo

// What Amazon hands out (scripts/lib/artifacts.sh's lists): wake words per language, whisper (one model, whatever the
// language), and the newer sound detection model (kept by region, not language).
const WW_KEYS = ['alexa', 'echo', 'computer', 'amazon', 'ziggy'];
const LOCALES = ['de-DE', 'en-US', 'en-GB', 'fr-FR', 'it-IT', 'es-ES', 'ja-JP', 'pt-BR', 'en-CA', 'fr-CA', 'en-AU', 'en-IN', 'es-MX'];
const WW_NAMES = { alexa: 'Alexa', echo: 'Echo', computer: 'Computer', amazon: 'Amazon', ziggy: 'Ziggy' };
// The Amazon sites with Alexa (davs.c has the same list).  An account lives in one region; any site of it takes the code.
const AMAZON_SITES = [
  ['North and South America', ['com', 'ca', 'com.mx', 'com.br']],
  ['Europe', ['co.uk', 'de', 'fr', 'it', 'es']],
  ['Asia and Pacific', ['co.jp', 'com.au', 'in']],
];
const SITE_OF = { 'en-GB': 'co.uk', 'en-IE': 'co.uk', 'en-CA': 'ca', 'fr-CA': 'ca', 'es-MX': 'com.mx', 'pt-BR': 'com.br', 'en-AU': 'com.au',
  'en-IN': 'in', de: 'de', fr: 'fr', it: 'it', es: 'es', pt: 'es', ja: 'co.jp' };
const defaultSite = () => { const l = navigator.language || ''; return store('hm.amazon') || SITE_OF[l] || SITE_OF[l.split('-')[0]] || 'com'; };
const davsLoc = () => dv.locale || store('hm.davsLocale') || LOCALES.find((l) => l === (navigator.language || '').replace('_', '-')) || 'de-DE';

let davsBox, davs = null, davsT = null, davsKey = '', davsUntil = 0, davsTotal = 0, davsClock = null;
// the user's picks, a run in progress (one row per pick), and the outcome of the last one (kept over the reload that
// follows an install, in sessionStorage)
const dv = { locale: null, ticks: new Map(), job: null, last: null };
try { dv.last = JSON.parse(sessionStorage.getItem('hm.davsResult')); sessionStorage.removeItem('hm.davsResult'); } catch (e) { /* none */ }

async function getDavs() {
  try { davs = await (await echo.call('GET', '/api/davs')).json(); } catch (e) { davs = null; }
  if (davs && davs.state === 'waiting') { davsUntil = Date.now() + davs.left_s * 1000; davsTotal = Math.max(davsTotal, davs.left_s); }
  else davsTotal = 0;
  return davs;
}

function davsCatalog() {
  const loc = davsLoc();
  return [
    ...WW_KEYS.map((k) => ({ key: k, locale: loc, id: `wake:${k}-${loc}`, group: 'Wake words', name: `${WW_NAMES[k]} (${loc})` })),
    { key: 'whisper', locale: 'en-US', id: 'whisper', group: 'Models', name: 'Whisper detection', note: 'Tells Home Assistant when a request was whispered' },
    { key: 'aed', locale: loc, id: 'sound', group: 'Models', name: 'Sound detection, newer model', note: 'Replaces the one in the firmware' },
  ];
}
const callout = (kind, ...kids) => h('div', { class: 'callout ' + kind }, icon(kind === 'ok' ? 'check' : kind === 'info' ? 'info' : 'warn'), h('div', { class: 'grow' }, ...kids));
const stepper = (cur) => h('ol', { class: 'stepper' }, ['Sign in', 'Enter the code', 'Pick and download'].map((t, i) =>
  h('li', { class: i + 1 < cur ? 'done' : i + 1 === cur ? 'cur' : '' }, h('span', { class: 'st-l' }, t))));

function renderDavs(force) { if (davsBox) keepFocus(davsBox, () => drawDavs(force)); }
function drawDavs(force) {
  // rebuilt only when something shown changed: a poll that finds all as it was leaves an open drop-down alone
  const key = JSON.stringify([davs && { ...davs, left_s: 0 }, dv, [...dv.ticks], myArts && myArts.artifacts.map((a) => a.id)]);
  if (!force && key === davsKey) return;
  davsKey = key;
  davsBox.innerHTML = '';
  davsBox.append(cardHead('Download from Amazon', 'More wake words and Amazon\'s newer models, straight from Amazon onto this Echo. Once here, they can be copied to your other Echos below.', 'ext'));
  if (!davs) { davsBox.append(empty('warn', 'This Echo did not say where its Amazon sign-in stands: it may be restarting.')); return; }
  const body = h('div', { class: 'card-body' });
  davsBox.append(body);
  if (!davs.dha) {
    body.append(callout('info', h('p', {}, 'This model cannot sign in to Amazon itself: the Echo Dot 3 proves who it is in a way not worked out yet. Download on another Echo and copy the models here, or use scripts/artifacts.sh.')));
    return;
  }
  if (davs.state === 'none') davsSignIn(body);
  else if (davs.state === 'waiting') davsCode(body);
  else davsPicker(body);
  davsTick();
}

function davsSignIn(body) {
  const site = h('select', { id: 'davs-site' }, AMAZON_SITES.map(([region, list]) => h('optgroup', { label: region },
    list.map((s) => h('option', { value: s, selected: s === defaultSite() }, `amazon.${s}`)))));
  site.onchange = () => store('hm.amazon', site.value);
  const go = h('button', { class: 'primary', onclick: async () => {
    go.disabled = true;
    try { await echo.call('POST', '/api/davs/login', site.value); } catch (e) { toast(errText(e)); }
    await getDavs(); renderDavs(true); davsPoll();
  } }, icon('key'), 'Get a sign-in code');
  body.append(...[stepper(1),
    davs.error ? callout('bad', h('p', {}, davs.error)) : null,
    h('div', { class: 'field' }, h('label', { for: 'davs-site' }, 'Your Amazon'), site,
      h('p', { class: 'help' }, 'The site you shop on. Any site in your account\'s region works.'))].filter(Boolean));
  davsBox.append(h('div', { class: 'card-foot' },
    h('span', { class: 'help grow' }, 'This Echo then shows up in your Alexa app, as Echos do, until you sign it out here.'), go));
}

function davsCode(body) {
  const site = davs.url.replace(/^https:\/\/www\./, '').replace(/\/code$/, '');
  const cancel = h('button', { onclick: async () => {
    cancel.disabled = true;
    try { await echo.call('POST', '/api/davs/cancel'); } catch (e) { toast(errText(e)); }
    await getDavs(); renderDavs(true);
  } }, 'Cancel');
  const code = davs.code;   // none yet: the Echo is still asking Amazon for one
  body.append(stepper(2),
    h('div', { class: 'davs-code' + (code ? '' : ' pending') },
      h('div', { class: 'code', title: 'The code to enter on Amazon' }, code || '······'),
      h('div', { class: 'davs-code-r' },
        h('p', {}, 'Enter this code on ', h('b', {}, `${site}/code`), ', signed in to your Amazon account. This page carries on by itself.'),
        h('div', { class: 'btns' }, code ? h('a', { class: 'btn primary', href: davs.url, target: '_blank', rel: 'noopener' }, `Open ${site}/code`, icon('ext')) : null, cancel))),
    h('div', { class: 'davs-wait' }, h('span', { class: 'spin' }), code ? 'Waiting for the code' : 'Getting a code from Amazon…', h('span', { class: 'grow' }),
      code ? [h('span', { class: 'davs-left' }), ' left'] : null),
    h('div', { class: 'progress thin' }, h('div', { class: 'davs-bar' })));
}

// the countdown, in place: a rebuild every second would be the flicker this card used to have
function davsTick() {
  const el = davsBox && davsBox.querySelector('.davs-left');
  if (!el) return;
  const s = Math.max(0, Math.round((davsUntil - Date.now()) / 1000));
  el.textContent = `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
  const bar = davsBox.querySelector('.davs-bar');
  if (bar) bar.style.width = `${davsTotal ? (100 * s) / davsTotal : 0}%`;
}

function davsLast() {
  const r = dv.last;
  if (!r) return null;
  // what root's installer said (artifacts.c "result"): "OK <id> FAILED <id>: why ..."
  const rootBad = r.installed ? ((myArts && myArts.result) || '').split(/ (?=OK |FAILED )/).filter((l) => l.startsWith('FAILED ')).map((l) => l.slice(7)) : [];
  const bad = [...r.bad.map(([n, why]) => `${n}: ${why}`), ...rootBad, ...(r.install ? [`Installing: ${r.install}`] : [])];
  const dismiss = h('button', { class: 'link', onclick: () => { dv.last = null; renderDavs(); } }, 'Dismiss');
  return callout(bad.length ? (r.ok.length ? 'warn' : 'bad') : 'ok',
    r.ok.length ? h('p', {}, `${r.installed ? 'Installed' : 'Downloaded'}: ${nameList(r.ok)}.`) : null,
    bad.length ? h('p', {}, r.ok.length ? 'Not this time:' : 'Nothing arrived:') : null,
    bad.length ? h('ul', {}, bad.map((b) => h('li', {}, b))) : null, dismiss);
}

function davsPicker(body) {
  const job = dv.job, has = (id) => !!(myArts && myArts.artifacts.some((a) => a.id === id));
  const out = h('button', { class: 'small', disabled: !!job, onclick: async () => {
    if (!confirm('Sign this Echo out of Amazon? It leaves your Alexa app; downloading again needs a new code.')) return;
    try { await echo.call('POST', '/api/davs/logout'); } catch (e) { toast(errText(e)); }
    await getDavs(); renderDavs(true);
  } }, 'Sign out');
  body.append(h('div', { class: 'davs-acct' }, h('span', { class: 'ic' }, icon('check')),
    h('div', { class: 'grow' }, h('b', {}, davs.device || 'Signed in'),
      h('div', { class: 'help' }, `Signed in to amazon.${davs.domain}; the Alexa app lists this Echo under that name.`)), out));
  const last = davsLast();
  if (last) body.append(last);
  else if (davs.error && !job) body.append(callout('bad', h('p', {}, davs.error)));
  const lang = h('select', { id: 'davs-lang', 'data-k': 'lang', disabled: !!job, onchange: () => { dv.locale = lang.value; store('hm.davsLocale', lang.value); renderDavs(); } },
    LOCALES.map((l) => h('option', { value: l, selected: l === davsLoc() }, l)));
  davsBox.append(h('div', { class: 'pickbar' }, h('label', { class: 'pick-l', for: 'davs-lang' }, 'Language'), lang,
    h('span', { class: 'pick-hint' }, 'Which wake words exist depends on it')));

  const list = h('div', { class: 'changes' });
  let group = '';
  for (const c of davsCatalog()) {
    if (c.group !== group) { group = c.group; list.append(h('div', { class: 'grp' }, group)); }
    const it = job && job.items.find((x) => x.id === c.id), on = !!dv.ticks.get(c.id);
    const cb = checkbox(on, (v) => { dv.ticks.set(c.id, v); dv.last = null; renderDavs(); }, c.name, 'art ' + c.id);
    cb.disabled = !!job;
    let st = has(c.id) ? h('span', { class: 'badge ok' }, icon('check'), 'Installed') : null;
    if (it) st = it.status === 'run' ? [h('span', { class: 'spin' }), it.pct ? `${it.pct} %` : 'Asking Amazon…']
      : it.status === 'ok' ? h('span', { class: 'badge ok' }, icon('check'), 'Downloaded')
      : it.status === 'bad' ? h('span', { class: 'badge bad' }, 'Failed') : h('span', { class: 'davs-none' }, 'Waiting');
    list.append(h('label', { class: `change davs-row ${on ? 'on' : 'off'}${job ? ' idle' : ''}` }, cb,
      h('div', { class: 'ch-name' }, c.name, c.note ? h('div', { class: 'ch-sub' }, c.note) : null,
        it && it.why ? h('div', { class: 'ch-sub bad' }, it.why) : null),
      h('div', { class: 'davs-st' }, st)));
  }
  davsBox.append(list);

  const n = davsCatalog().filter((c) => dv.ticks.get(c.id)).length;
  const now = job && job.items.find((x) => x.status === 'run'), elsewhere = !job && davs.state === 'busy';   // another browser's run
  davsBox.append(h('div', { class: 'card-foot sync-foot' },
    h('div', { class: 'count grow' }, job ? (job.phase === 'install' ? 'Installing: the satellite restarts for a few seconds…' : `Downloading ${now ? now.name : ''}…`)
      : elsewhere ? `Another browser is downloading ${davs.busy} (${davs.progress} %)`
      : n ? [h('b', {}, plural(n, 'download')), ', then the satellite restarts once to install them'] : 'Tick what to download'),
    h('button', { class: 'primary', disabled: !!job || elsewhere || !n, onclick: downloadAmazon },
      job ? [h('span', { class: 'spin' }), job.phase === 'install' ? 'Installing…' : 'Downloading…'] : 'Download and install')));
}

async function downloadAmazon() {
  const picks = davsCatalog().filter((c) => dv.ticks.get(c.id));
  if (!picks.length) return;
  dv.last = null;
  dv.job = { phase: 'download', items: picks.map((c) => ({ id: c.id, name: c.name, status: 'wait', pct: 0, why: '' })) };
  renderDavs();
  try {
    for (const [i, p] of picks.entries()) {
      const it = dv.job.items[i];
      it.status = 'run'; renderDavs();
      try { await echo.call('POST', '/api/davs/fetch', `${p.key} ${p.locale}`); }
      catch (e) { it.status = 'bad'; it.why = errText(e).replace(/^Error: /, ''); renderDavs(); continue; }
      for (let missed = 0; ;) {
        await new Promise((r) => setTimeout(r, 1000));
        const st = await getDavs();
        if (!st) {   // one lost answer (Wi-Fi) is no end; a minute of them is
          if (++missed < 60) continue;
          it.status = 'bad'; it.why = 'this Echo stopped answering'; break;
        }
        missed = 0;
        if (st.state === 'busy') { it.pct = st.progress || 0; renderDavs(); continue; }
        if (st.done) it.status = 'ok';            // each pick's own end: "done" only tells of the last one
        else { it.status = 'bad'; it.why = st.error || 'it did not arrive'; }
        break;
      }
      renderDavs();
    }
    const items = dv.job.items, ok = items.filter((x) => x.status === 'ok');
    const result = { ok: ok.map((x) => x.name), bad: items.filter((x) => x.status === 'bad').map((x) => [x.name, x.why]), installed: false };
    if (!ok.length) { dv.last = result; return; }
    dv.job.phase = 'install'; renderDavs();
    try { await echo.call('POST', '/api/artifact/install'); await waitBack(echo); }
    catch (e) { result.install = errText(e).replace(/^Error: /, ''); dv.last = result; return; }
    result.installed = true;
    dv.ticks.clear();
    // the satellite restarted with the new models: the page starts over (wake word lists), and shows the outcome then
    try { sessionStorage.setItem('hm.davsResult', JSON.stringify(result)); } catch (e) { dv.last = result; return; }
    location.reload();
  } finally {
    dv.job = null; renderDavs();
  }
}

function davsPoll() {
  clearTimeout(davsT);
  if (!davsClock) davsClock = setInterval(davsTick, 1000);
  const quick = davs && (davs.state === 'waiting' || davs.state === 'busy');
  davsT = setTimeout(async () => {
    if (!document.hidden && !dv.job) { await getDavs(); renderDavs(); }   // a run polls by itself
    davsPoll();
  }, quick ? 2000 : 15000);
}

// ---------------------------------------------------------------- copy models (Amazon's artifacts) between Echos

const CHUNK = 192 * 1024;
const mpick = { rows: new Map(), to: new Map() };    // the user's ticks; default: no model, every Echo ('' = this one)
const mOn = (id) => ticked(mpick.rows, id, false), xOn = (x) => ticked(mpick.to, x.key, true);
let copying = null;                             // { text, done, total } while models travel

function artLabel(a) {
  if (a.kind === 'sound') return 'Sound detection, newer model';
  if (a.kind === 'whisper') return 'Whisper detection';
  const [w, ...loc] = a.name.split('-');
  return `Wake word ${w[0].toUpperCase() + w.slice(1)}${loc.length ? ` (${loc.join('-')})` : ''}`;
}
const mb = (n) => n >= 1048576 ? `${(n / 1048576).toFixed(1)} MB` : `${Math.max(1, Math.round(n / 1024))} KB`;

function renderModels() { if (modelBox) keepFocus(modelBox, drawModels); }
function drawModels() {
  modelBox.innerHTML = '';
  modelBox.append(cardHead('Copy models', 'Amazon\'s extra models from scripts/artifacts.sh (more wake words, whisper detection, the newer sound detection model), Echo to Echo instead of running the script on each. A wake word set is tried on the receiving Echo first; each Echo that gets something restarts its satellite once, for a few seconds.', 'box'));
  if (!myArts) { modelBox.append(empty('warn', 'This Echo cannot list its models.')); return; }
  const echos = [{ name: echo.hello.name, key: '', e: echo, arts: myArts }, ...ready().filter((d) => d.arts).map((d) => ({ name: d.echo.hello.name, key: d.echo.base, e: d.echo, arts: d.arts }))];
  const all = new Map();
  for (const x of echos) for (const a of x.arts.artifacts) if (!all.has(a.id)) all.set(a.id, a);
  if (!all.size) { modelBox.append(empty('box', 'None of these Echos has any of Amazon\'s extra models yet: download them above, or run scripts/artifacts.sh, then copy from here.')); return; }
  // where each model comes from: this Echo if it has it, else the first that does
  const source = (id) => echos.find((x) => x.arts.artifacts.some((a) => a.id === id));
  const theirs = (x, id) => x.arts.artifacts.find((a) => a.id === id);
  const needs = (x, id) => { const s = source(id), t = theirs(x, id); return x !== s && (!t || t.digest !== theirs(s, id).digest); };
  const ids = [...all.keys()].sort((p, q) => artLabel(all.get(p)).localeCompare(artLabel(all.get(q))));
  if (echos.length > 1) modelBox.append(h('div', { class: 'pickbar' }, h('span', { class: 'pick-l' }, 'Install on'),
    chips(echos.map((x) => ({ name: x.name + (x.key ? '' : ' (this one)'), on: xOn(x), disabled: !!copying, k: 'to ' + x.key,
      toggle: () => { mpick.to.set(x.key, !xOn(x)); renderModels(); } })))));
  else modelBox.append(h('div', { class: 'pickbar' }, h('span', { class: 'pick-hint' }, 'Log in to another Echo above to copy these to it.')));
  const list = h('div', { class: 'changes' });
  for (const id of ids) {
    const a = all.get(id), src = source(id), wanted = echos.filter((x) => xOn(x) && needs(x, id));
    const lines = echos.map((x) => {
      const t = theirs(x, id), who = h('span', { class: 'ch-echo', title: x.name }, x.name);
      if (x === src) return h('div', { class: 'ch-line same' }, who, icon('check'), 'has it, copied from here');
      if (t && t.digest === theirs(src, id).digest) return h('div', { class: 'ch-line same' }, who, icon('check'), 'has it');
      if (!xOn(x)) return h('div', { class: 'ch-line same' }, who, t ? 'another version, left alone' : 'not there, left alone');
      return h('div', { class: 'ch-line' + (t ? ' warn' : '') }, who, icon('arrow'), h('span', { class: 'new' }, t ? 'replace its other version' : 'install'));
    });
    const idle = !wanted.length;
    const cb = checkbox(!idle && mOn(id), (on) => { mpick.rows.set(id, on); renderModels(); }, artLabel(a), 'art ' + id);
    cb.disabled = idle || !!copying;
    list.append(h('label', { class: 'change ' + (idle ? 'idle off' : mOn(id) ? 'on' : 'off') }, cb,
      h('div', { class: 'ch-name' }, artLabel(a), h('div', { class: 'ch-sub' }, mb(theirs(src, id).size))), h('div', { class: 'ch-to' }, lines)));
  }
  modelBox.append(list);
  const jobs = [];
  for (const id of ids.filter(mOn)) for (const x of echos) if (xOn(x) && needs(x, id)) jobs.push({ id, to: x, from: source(id), art: theirs(source(id), id) });
  const bytes = jobs.reduce((n, j) => n + j.art.size, 0);
  const go = h('button', { class: 'primary', 'data-k': 'go', disabled: !!copying || !jobs.length, onclick: () => copyModels(jobs) },
    copying ? [h('span', { class: 'spin' }), 'Copying…'] : jobs.length ? `Copy ${plural(jobs.length, 'model')} (${mb(bytes)})` : 'Copy');
  modelBox.append(h('div', { class: 'card-foot sync-foot' },
    copying ? h('div', { class: 'busy' }, copying.text) : null,
    copying ? h('div', { class: 'progress', role: 'progressbar', 'aria-valuemin': '0', 'aria-valuemax': String(copying.total), 'aria-valuenow': String(copying.done) },
      h('div', { style: `width:${copying.total ? (100 * copying.done) / copying.total : 0}%` })) : null,
    h('div', { class: 'count grow' }, jobs.length ? h('b', {}, `${plural(jobs.length, 'copy', 'copies')}`) : 'Tick the models to copy', jobs.length ? `, ${mb(bytes)}` : ''),
    go));
}

async function transfer(job, done0, total) {
  const { art, from, to } = job;
  const spec = `${art.id} ${art.digest}\n` + art.files.map((f) => `${f.name} ${f.size}\n`).join('');
  await to.e.call('POST', '/api/artifact/begin', spec);
  let done = 0;
  for (const f of art.files) {
    for (let off = 0; off < f.size; off += CHUNK) {
      const len = Math.min(CHUNK, f.size - off);
      const r = await from.e.call('GET', `/api/artifact/read/${art.id}/${encodeURIComponent(f.name)}/${off}/${len}`);
      if (r.bytes.length !== len) throw new Error(`${from.name} sent less of ${f.name} than asked`);
      await to.e.call('POST', `/api/artifact/chunk/${art.id}/${encodeURIComponent(f.name)}/${off}`, r.bytes);
      done += len;
      copying = { text: `${artLabel(art)}: ${from.name} → ${to.name}, ${mb(done)} of ${mb(art.size)}`, done: done0 + done, total };
      renderModels();
    }
  }
  await to.e.call('POST', `/api/artifact/commit/${art.id}`);
}

async function waitBack(e) {             // an Echo restarting its satellite: its page answers again within seconds
  await new Promise((r) => setTimeout(r, 3000));
  for (let i = 0; i < 40; i++) {
    try { const r = await fetch(e.base + '/api/hello'); if (r.ok) return true; } catch (err) { /* still restarting */ }
    await new Promise((r) => setTimeout(r, 1000));
  }
  return false;
}

async function copyModels(jobs) {
  const total = jobs.reduce((n, j) => n + j.art.size, 0), notes = [], got = new Map();
  let done = 0;
  copying = { text: 'Starting…', done: 0, total }; renderModels();
  for (const j of jobs) {
    try { await transfer(j, done, total); got.set(j.to.key, j.to); }
    catch (e) { notes.push(`${artLabel(j.art)} → ${j.to.name}: ${errText(e).replace(/^Error: /, '')}`); }
    done += j.art.size;
  }
  for (const x of got.values()) {
    copying = { text: `Installing on ${x.name}; its satellite restarts…`, done: total, total }; renderModels();
    try { await x.e.call('POST', '/api/artifact/install'); await waitBack(x.e); }
    catch (e) { notes.push(`${x.name}: ${errText(e).replace(/^Error: /, '')}`); }
  }
  copying = null; mpick.rows.clear();
  toast(notes.length ? notes.join(' · ') : `Copied and installed on ${got.size} Echo${got.size === 1 ? '' : 's'}`);
  if (got.has('')) { setTimeout(() => location.reload(), 500); return; }       // this Echo restarted: start the page over
  await refreshDevices();
}

// ---------------------------------------------------------------- System: settings file, debug access, browsers

let adbBox, clientsBox, importMsg, nameCtl;

// The node name a name makes, as main.c node_of: ASCII letters and digits lower-cased, Latin-1 letters spelled out the
// German way ("Küchen Echo" -> "kuechen-echo"), anything else one dash between words
const LATIN1 = ['a', 'a', 'a', 'a', 'ae', 'a', 'ae', 'c', 'e', 'e', 'e', 'e', 'i', 'i', 'i', 'i',
  'd', 'n', 'o', 'o', 'o', 'o', 'oe', null, 'o', 'u', 'u', 'u', 'ue', 'y', 'th', 'ss'];
function nodeOf(name) {
  const b = enc.encode(name); let n = '', dash = false;
  for (let i = 0; i < b.length; i++) {
    const c = b[i]; let add = null;
    if ((c >= 48 && c <= 57) || (c >= 65 && c <= 90) || (c >= 97 && c <= 122)) add = String.fromCharCode(c).toLowerCase();
    else if (c === 0xC3 && i + 1 < b.length) { const d = b[++i]; add = d === 0xBF ? 'y' : d === 0xB7 ? null : d >= 0x80 && d <= 0xBF ? LATIN1[(d - 0x80) & 0x1F] : null; }
    else while (i + 1 < b.length && (b[i + 1] & 0xC0) === 0x80) i++;
    if (!add) { dash = n.length > 0; continue; }
    if (dash && n.length < 63) n += '-';
    dash = false;
    n = (n + add).slice(0, 63);
  }
  return n || 'echo';
}
// what main.c name_ok takes: it goes unescaped into the avahi service file
const nameProblem = (t) => !t ? 'Give it a name.' : enc.encode(t).length > 48 ? 'At most 48 characters.'
  : /[\x00-\x1f\x7f<>&"'\\]/.test(t) ? 'Without < > & " \' or \\.' : null;

// Rename: the display name alone, or the node name with it (what that costs is spelled out, and ticked off, first)
function nameCard() {
  const input = h('input', { type: 'text', id: 'ren-name', maxlength: '48', autocomplete: 'off' });
  const node = h('input', { type: 'checkbox', id: 'ren-node' }), sure = h('input', { type: 'checkbox', id: 'ren-sure' });
  const msg = h('p', { class: 'help' }), costs = h('div'), go = h('button', { class: 'primary' }, 'Rename');
  const c = { input, dirty: false, busy: false };
  const paint = () => {
    const cur = echo.hello.name, curNode = echo.hello.node, t = input.value.trim(), want = nodeOf(t), bad = nameProblem(t);
    const a = state.arbitration, taken = node.checked && want !== curNode && a && [...a.members, ...(a.others || [])].find((m) => m.node === want);
    if (!c.dirty && document.activeElement !== input) input.value = cur;
    msg.replaceChildren(...(bad && t !== cur ? [h('span', { class: 'f-bad' }, bad)]
      : taken ? [h('span', { class: 'f-bad' }, `${taken.node} is another Echo's node name already: Home Assistant would mix the two up. Pick another name.`)]
      : node.checked ? ['Node name: ', h('code', {}, curNode), want === curNode ? ' (stays)' : [' ', icon('arrow'), ' ', h('code', {}, want)]]
      : ['Node name stays ', h('code', {}, curNode), '.']));
    costs.replaceChildren();
    if (node.checked && want !== curNode && !bad) costs.append(callout('warn',
      h('p', {}, h('b', {}, 'Changing the node name has costs:')),
      h('ul', {},
        h('li', {}, 'Home Assistant keeps the device (it knows it by its MAC address) and takes the new name, but its entity ids keep the old one (', h('code', {}, `sensor.${curNode.replace(/-/g, '_')}_…`), ') unless you rename them there.'),
        h('li', {}, 'Its Home Assistant action becomes ', h('code', {}, `esphome.${want.replace(/-/g, '_')}_arbitration_key`), '.'),
        h('li', {}, 'The host name becomes ', h('code', {}, `${want}.local`), ': bookmarks or anything else that reaches the Echo by name need the new one. By IP address nothing changes.')),
      h('label', { class: 'ren-sure' }, sure, ' Change the node name anyway')));
    const changes = t !== cur || (node.checked && want !== curNode);
    go.disabled = c.busy || !!bad || !!taken || !changes || (node.checked && want !== curNode && !sure.checked);
    go.replaceChildren(...(c.busy ? [h('span', { class: 'spin' }), 'Restarting…'] : ['Rename']));
  };
  input.oninput = () => { c.dirty = true; paint(); };
  node.onchange = () => { sure.checked = false; paint(); };
  sure.onchange = paint;
  go.onclick = async () => {
    const t = input.value.trim(), withNode = node.checked && nodeOf(t) !== echo.hello.node;
    c.busy = true; paint();
    try {
      await echo.call('POST', '/api/name', `${withNode ? 1 : 0} ${t}`);
      toast('Renamed: the satellite restarts for a few seconds');
      await waitBack(echo);
      location.reload();
    } catch (e) { toast(errText(e)); c.busy = false; paint(); }
  };
  c.paint = paint;
  nameCtl = c;
  return h('div', { class: 'card' },
    cardHead('Name', 'What this Echo is called: in Home Assistant (unless you renamed the device there; yours stays), on Bluetooth, in Music Assistant and on these pages. The Alexa app keeps the name it got when this Echo signed in to Amazon. The satellite restarts once, for a few seconds.', 'pen'),
    h('div', { class: 'card-body' },
      h('div', { class: 'field' }, h('label', { for: 'ren-name' }, 'Name'), input, msg),
      h('label', { class: 'ren-node' }, node, ' Also change the node name (ESPHome device name, host name, entity ids of new entities)'),
      costs),
    h('div', { class: 'card-foot' }, h('span', { class: 'grow' }), go));
}

function buildSystem(el) {
  el.append(nameCard());
  const file = h('input', { type: 'file', accept: '.conf,.txt,text/plain', class: 'hidden' });
  importMsg = h('pre', { class: 'help import-msg' });
  file.onchange = async () => {
    const f = file.files[0]; if (!f) return;
    try {
      const r = await (await echo.call('POST', '/api/set', await f.text())).json();
      importMsg.textContent = `${r.applied} setting${r.applied === 1 ? '' : 's'} applied.` + (r.errors ? `\nNot applied:\n${r.errors}` : '');
      await load();
    } catch (e) { toast(errText(e)); }
    file.value = '';
  };
  el.append(h('div', { class: 'card' },
    cardHead('Settings file', ['All settings except what belongs to this one Echo (its name, keys, pairings), as a text file. Import it on another Echo, keep it as a backup, or give it to ', h('code', {}, 'scripts/setup.sh --preset'), ' so the next Echo you install starts with these settings.'], 'file'),
    h('div', { class: 'card-body' }, h('div', { class: 'btns' }, h('button', { onclick: exportFile }, 'Export'), h('button', { onclick: () => file.click() }, 'Import…'), file),
      importMsg)));

  adbBox = h('div');
  el.append(h('div', { class: 'card' },
    cardHead('Debug access', 'Opens adb over Wi-Fi for 30 minutes: a root shell on this Echo for anyone on your network while it is open. Only for troubleshooting. Opening needs a press of the action button, even from an approved browser.', 'term'),
    h('div', { class: 'card-body' }, adbBox)));

  el.append(logCard());

  clientsBox = h('div');
  el.append(h('div', { class: 'card' },
    cardHead('Approved browsers', 'Browsers that may change settings on this Echo. Revoke one you no longer use.', 'key'),
    h('div', { class: 'clients' }, clientsBox)));
}

function renderSystem() {
  if (!adbBox) return;
  if (nameCtl) nameCtl.paint();
  const s = state.adb; adbBox.innerHTML = '';
  const status = s.waiting ? h('span', { class: 'badge acc' }, 'Waiting for the action button…')
    : s.open ? h('span', { class: 'badge bad' }, 'Open: closes by itself after 30 min') : h('span', { class: 'badge' }, 'Closed');
  adbBox.append(h('div', { class: 'btns' }, status, h('span', { class: 'grow' }),
    s.open ? h('button', { onclick: adbClose }, 'Close now') : h('button', { class: 'danger', disabled: s.waiting, onclick: adbOpen }, 'Open debug access')));

  clientsBox.innerHTML = '';
  for (const k of state.clients) {
    const b = h('button', { class: 'small' + (k.me ? '' : ' danger') }, k.me ? 'Log out' : 'Revoke');
    b.onclick = async () => {
      if (!confirm(k.me ? 'Log this browser out of this Echo? You will need the action button to log in again.' : `Revoke "${k.label}"? It will need the action button to log in again.`)) return;
      try { await echo.call('POST', '/api/revoke', k.pub); } catch (e) { toast(errText(e)); return; }
      if (k.me) showLogin(); else load();
    };
    clientsBox.append(h('div', { class: 'row' }, h('div', { class: 'row-label' }, k.label, k.me ? h('span', { class: 'badge acc' }, 'This browser') : null), h('div', { class: 'ctl' }, b)));
  }
}

// ---------------------------------------------------------------- the log (boot.log: hassmic, the firewall, updates)

// Each line starts with when (clock.c): "2026-10-06 15:18:02.417Z" (UTC, shown here in the browser's zone) once the
// Echo has the time from Home Assistant, "boot+29935.512" (seconds since boot) before.  Lines of older versions have none.
const LOG_UTC = /^(\d{4})-(\d\d)-(\d\d) (\d\d):(\d\d):(\d\d)(\.\d{3})?Z /, LOG_BOOT = /^boot\+\d+(\.\d+)? /;
const pad2 = (n) => String(n).padStart(2, '0');
function logLine(l) {
  let ts = '', rest = l; const m = LOG_UTC.exec(l);
  if (m) {
    const d = new Date(Date.UTC(+m[1], m[2] - 1, +m[3], +m[4], +m[5], +m[6]));
    ts = `${d.getFullYear()}-${pad2(d.getMonth() + 1)}-${pad2(d.getDate())} ${pad2(d.getHours())}:${pad2(d.getMinutes())}:${pad2(d.getSeconds())}${m[7] || ''}`;
    rest = l.slice(m[0].length);
  } else { const b = LOG_BOOT.exec(l); if (b) { ts = b[0].trim(); rest = l.slice(b[0].length); } }
  const cls = /^!!|\b(error|failed|cannot|refused)\b/i.test(rest) ? 'bad' : /^==/.test(rest) ? 'mark' : null;
  return h('span', { class: cls }, ts ? h('span', { class: 'ts', title: m ? l.slice(0, m[0].length - 1) + ' (UTC)' : 'Seconds since the Echo started: it did not have the time from Home Assistant yet' }, ts + ' ') : null, rest + '\n');
}

// Loaded on demand only: up to 2 MB, and the Echo reads it from flash.  Part 1 is the rotated older part (main.sh keeps one)
function logCard() {
  const parts = [null, null];           // [boot.log, boot.log.1] as text once fetched
  const pre = h('pre', { class: 'log hidden', tabindex: '0', 'aria-label': 'Log' });
  const filter = h('input', { type: 'search', placeholder: 'Filter', 'aria-label': 'Show only lines with this text', class: 'hidden' });
  const older = h('input', { type: 'checkbox' });
  const olderBox = h('label', { class: 'log-older hidden' }, older, 'With the older part');
  const info = h('p', { class: 'help log-info' });
  const show = h('button', { class: 'primary' }, 'Show log');
  const save = h('button', { class: 'hidden' }, 'Download');
  const text = () => (older.checked && parts[1] ? parts[1] : '') + (parts[0] || '');
  const paint = (toEnd) => {
    const q = filter.value.trim().toLowerCase(), all = text().split('\n');
    if (all[all.length - 1] === '') all.pop();
    const lines = q ? all.filter((l) => l.toLowerCase().includes(q)) : all;
    const end = toEnd || pre.scrollTop + pre.clientHeight >= pre.scrollHeight - 20;    // stay at the end if there already
    pre.replaceChildren(...lines.map(logLine));
    if (end) pre.scrollTop = pre.scrollHeight;
    pre.classList.toggle('hidden', !lines.length);
    if (!all.length) { info.textContent = 'The log is empty.'; return; }
    info.textContent = (q ? `${lines.length} of ${all.length} lines` : `${all.length} lines`)
      + (parts[1] === '' ? '' : older.checked ? ', older part included' : '') + '. Times in this browser\'s time zone; "boot+" is seconds since the Echo started, before it had the time from Home Assistant. Secrets (the Sendspin pairing token) are blanked.';
  };
  const fetchPart = async (i) => { parts[i] = (await echo.call('GET', `/api/log/${i}`)).text(); };
  show.onclick = async () => {
    show.disabled = true;
    try {
      await fetchPart(0);
      if (older.checked || parts[1] === null) await fetchPart(1);
      [pre, filter, save].forEach((x) => x.classList.remove('hidden'));
      olderBox.classList.toggle('hidden', !parts[1]);
      show.textContent = 'Refresh';
      paint(true);
    } catch (e) { toast(errText(e)); }
    show.disabled = false;
  };
  filter.oninput = () => paint(true);
  older.onchange = () => paint(false);
  save.onclick = () => {
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([text()], { type: 'text/plain' }));
    a.download = `hassmic-${echo.hello.name.toLowerCase().replace(/[^a-z0-9]+/g, '-')}-boot.log`; a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  };
  return h('div', { class: 'card' },
    cardHead('Log', 'What hassmic, the firewall and updates wrote, newest at the bottom: the first place to look when something does not work, and what to attach to a bug report. It travels unencrypted, like everything on this page, so someone watching your network can read it: names, addresses and when the Echo was spoken to.', 'file'),
    h('div', { class: 'card-body' }, h('div', { class: 'btns' }, show, save, filter, olderBox), info, pre));
}

async function adbOpen() {
  try {
    const r = await (await echo.call('POST', '/api/adb', 'on')).json();
    if (r.adb === 'busy') { toast('A login is waiting for the button first'); return; }
    toast('Press the action button on the Echo');
    for (let i = 0; i < 65; i++) {
      await new Promise((res) => setTimeout(res, 1000));
      await load();
      if (!state.adb.waiting) break;
    }
  } catch (e) { toast(errText(e)); }
}
async function adbClose() { try { await echo.call('POST', '/api/adb', 'off'); } catch (e) { toast(errText(e)); } await load(); }

async function exportFile() {
  try {
    const text = await exportOf(echo);
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([text], { type: 'text/plain' }));
    a.download = 'hassmic-settings.conf'; a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  } catch (e) { toast(errText(e)); }
}

start();
