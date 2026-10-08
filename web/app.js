// The settings page (web.c serves it).  One key pair per browser, kept in localStorage; each Echo approves it once
// (action button), then every request is signed: X-HM-Mac = BLAKE2b-128 keyed with K over "METHOD\nPATH\nCTR\nBODY",
// K = BLAKE2b-256 keyed with X25519(our key, the Echo's) over "hassmic web 1" + Echo key + our key.  The answers carry
// X-HM-Mac too, over "RESP\nCTR\nBODY": what the page carries from one Echo to another (settings, models) is what it sent.
'use strict';

const { x25519, x25519Public, blake2b, seal, hex, unhex } = hmcrypto;
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
  // body: text or bytes, or a function of the counter that returns them (what it seals is bound to this request).
  // Returns { bytes, text(), json() } once the answer's signature checks out.  Errors: 'login' (not approved here), 'old'
  // (an Echo whose answers are not signed yet: update it), 'forged', or the Echo's own words.
  async call(method, path, body = '') {
    const ctr = String(this.nextCtr()), raw = typeof body === 'function' ? body(ctr) : body;
    const b = typeof raw === 'string' ? enc.encode(raw) : raw;
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
  talk: '<path d="M3 5h12v8H8l-5 4z"/><path d="M15 9h6v8l-4-3h-6v-1"/>',
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
  lock: '<rect x="5" y="11" width="14" height="10" rx="2"/><path d="M8 11V8a4 4 0 0 1 8 0v3"/>',
  x: '<path d="M6 6l12 12M18 6L6 18"/>',
  refresh: '<path d="M20 12a8 8 0 0 1-14.2 5"/><path d="M4 12a8 8 0 0 1 14.2-5"/><path d="M18.5 3v4h-4M5.5 21v-4h4"/>',
  ear: '<path d="M6.5 9a5.5 5.5 0 0 1 11 0c0 3.2-3.5 4.3-3.5 7.5a3 3 0 0 1-5.6 1.5"/><path d="M9.5 9a2.5 2.5 0 0 1 5 0c0 1.2-1 1.8-1.6 2.4"/>',
  trash: '<path d="M4 7h16M10 11v6M14 11v6"/><path d="M6 7l1 13h10l1-13M9 7V4h6v3"/>',
  down: '<path d="M12 4v11M7 10l5 5 5-5M5 20h14"/>',
  up: '<path d="M12 20V9M7 14l5-5 5 5M5 4h14"/>',
  dot: '<circle cx="12" cy="12" r="2.5"/>',
  eye: '<path d="M2 12s3.5-7 10-7 10 7 10 7-3.5 7-10 7S2 12 2 12z"/><circle cx="12" cy="12" r="3"/>',
};
const icon = (n) => h('span', { html: `<svg class="i" viewBox="0 0 24 24" aria-hidden="true">${ICONS[n] || ''}</svg>`, style: 'display:inline-flex' });

function browserLabel() { return (navigator.userAgentData && navigator.userAgentData.platform) || navigator.platform || 'browser'; }

// In place of the browser's confirm() and prompt(): the page's own dialog.  title, body (nodes or text), ok (the button
// that goes ahead), danger (it deletes or cuts something off), input (a text field: { label, value, max }), check (a
// value from the field: an error text, or null).  Resolves to true (or the field's text) when gone ahead, null otherwise.
function ask({ title, body = [], ok = 'OK', danger = false, input = null, check = null }) {
  return new Promise((done) => {
    let result = null;
    const field = input ? h('input', { type: 'text', id: 'ask-field', value: input.value || '', maxlength: String(input.max || 200), autocomplete: 'off' }) : null;
    const msg = h('p', { class: 'help ask-msg' });
    if (field) field.oninput = () => { msg.textContent = ''; };
    const go = h('button', { type: 'submit', class: danger ? 'danger' : 'primary' }, ok);
    const cancel = h('button', { type: 'button', onclick: () => dlg.close() }, 'Cancel');
    const close = h('button', { type: 'button', class: 'icon-btn', title: 'Close', 'aria-label': 'Close', onclick: () => dlg.close() }, icon('x'));
    const form = h('form', { method: 'dialog', class: 'ask-form' },
      h('div', { class: 'modal-body' }, ...[].concat(body).map((b) => typeof b === 'string' ? h('p', {}, b) : b),
        field ? h('div', { class: 'field ask-field' }, h('label', { for: 'ask-field' }, input.label), field, msg) : null),
      h('div', { class: 'modal-foot' }, cancel, go));
    const dlg = h('dialog', { class: 'modal ask', 'aria-labelledby': 'ask-title' }, h('div', { class: 'modal-head' }, h('h2', { id: 'ask-title' }, title), close), form);
    form.onsubmit = (e) => {
      e.preventDefault();
      if (field) {
        const v = field.value.trim(), bad = check ? check(v) : (v ? null : 'Type something first.');
        if (bad) { msg.textContent = bad; field.focus(); return; }
        result = v;
      } else result = true;
      dlg.close();
    };
    dlg.onclose = () => { dlg.remove(); done(result); };
    document.body.append(dlg);
    dlg.showModal();
    if (field) { field.focus(); field.select(); } else cancel.focus();      // the safe button first: Enter does not go ahead by accident
  });
}

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
  { id: 'Wake word', icon: 'ear', intro: 'Which engine listens for the wake word. Home Assistant picks the wake word itself, from the engine\'s list (the satellite\'s "Wake word" select).' },
  { id: 'Sound', icon: 'speaker', intro: 'Amazon\'s own equalizer, applied to everything the Echo plays: replies, timers, music, Bluetooth.' },
  { id: 'Lights', icon: 'sun', intro: 'The light ring. It shows listening, thinking, speaking, errors and mute whatever you set here.' },
  { id: 'Features', icon: 'sparkle', intro: 'Optional extras. While a feature is on, its entities are in Home Assistant; switched off, they are removed there. Home Assistant reconnects for a moment when you switch one.' },
  { id: 'Music', icon: 'music', intro: 'Music Assistant plays on the Echo as a Sendspin player.' },
  { id: 'Bluetooth', icon: 'bt', intro: 'For phones playing music on the Echo.' },
  { id: 'Echos', icon: 'net', intro: '' },
  { id: 'System', icon: 'sliders', intro: 'This Echo\'s name and network, updates, backups and troubleshooting.' },
];

// help: what it does, in the user's terms.  ha: also in Home Assistant (true), or for features the entities it adds.
// icon: features.  parent: shown inside that feature's card while it is on.  tag: an extra badge.
const HELP = {
  mic_level: { help: 'How loud your voice reaches speech-to-text. The Echo adjusts its gain to hold speech at this level. Raise it if quiet commands get misheard or cut off; lower it if loud speech comes out distorted. Default −26 dBFS.' },
  noise_reduction: { help: 'Takes steady background noise (fans, traffic, the dishwasher) out of what speech-to-text hears, by up to 6, 9 or 12 dB. The wake word always gets the untouched sound. Try it if commands fail in a noisy room.' },
  wake_sound: { ha: true, help: 'A tone when the Echo starts listening. Turning it off also silences the Echo\'s other local sounds.' },
  mute: { ha: true, help: 'Stops listening for the wake word, as the switch in Home Assistant does. The mic-off button on the Echo is a separate, hardware mute: only the button can lift it.' },
  do_not_disturb: { ha: true, help: 'Drops announcements from Home Assistant while on. The ring pulses purple when you switch it on.' },
  timer_ring: { step: 10, help: 'How long a finished timer rings before it gives up. All the way left: until someone stops it ("<wake word>, stop", the action button, or the media player\'s stop in Home Assistant; the wake word alone only pauses it while you talk). Home Assistant\'s "Timer ringing" stays on just as long, so an automation can tell when nobody heard it. Default 1 min.' },
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
  wake_engine: { help: 'Amazon\'s engine is the one the Echo was built around. microWakeWord, the engine of ESPHome\'s voice satellites, takes wake words anyone can train, at the cost of hearing them much less reliably. Home Assistant: the Echo streams its microphone and your Home Assistant listens, with openWakeWord for instance.',
    // the warning, a first model, or Home Assistant's side of it, before the switch; from or to Home Assistant the satellite restarts
    before: (c) => c === 'microwakeword' ? mwwSwitchDialog() : c === 'homeassistant' ? haSwitchDialog() : true,
    after: async (c, was) => { if (c === 'homeassistant' || was === 'homeassistant') await restarted(); },
    modes: {
      amazon: { title: 'Amazon\'s engine', tag: 'Default', sub: 'Pryon, with Amazon\'s models: what a stock Echo runs' },
      homeassistant: { title: 'Home Assistant', sub: 'The Echo streams its microphone, your Home Assistant listens (openWakeWord, for instance)' },
      microwakeword: { title: 'microWakeWord', tag: 'Experimental', sub: 'The open engine of ESPHome\'s voice satellites, with the models you add below' },
    },
    // one row per question, one cell per choice: '+' gives, '-' costs, '' neither
    compare: [
      ['How well it hears', {
        amazon: ['+', 'Best: Amazon trained its models on vast amounts of real speech, for this very microphone array and its audio front end. Few false wakes from TV and conversation'],
        microwakeword: ['-', 'Significantly worse: expect more missed wake words, above all from across the room or over music, and more false wakes. Its small models were trained mostly on synthetic speech, never with this Echo\'s microphones and audio front end'],
        homeassistant: ['-', 'Expect it to be less reliable than Amazon\'s engine: openWakeWord\'s models, like microWakeWord\'s, are trained mostly on synthetic speech'] }],
      ['Wake words', {
        amazon: ['-', 'Amazon\'s only, no new ones: Alexa, and Echo, Computer, Amazon, Ziggy in many languages from Amazon on this page'],
        microwakeword: ['+', 'Any someone trained a model for: “Okay Nabu”, “Hey Jarvis”, “Hey Mycroft”, or your own from microWakeWord\'s training notebook. The same model files ESPHome devices (Home Assistant Voice PE) use. Amazon\'s, also those downloaded from Amazon, are not offered (its “Alexa” is a model of its own)'],
        homeassistant: ['+', 'Any your Home Assistant knows, openWakeWord models of your own included: the same as your other satellites, set up in one place'] }],
      ['“<wake word>, stop”', {
        amazon: ['+', 'Ends a ringing timer or a reply'],
        microwakeword: ['-', 'No: a ringing timer or a reply stops with the action button or Home Assistant only'],
        homeassistant: ['-', 'No'] }],
      ['While the Echo plays', {
        amazon: ['+', 'Listens harder while it plays music, rings or speaks, so you can talk over it'],
        microwakeword: ['-', 'No extra sensitivity: talking over it works less often'],
        homeassistant: ['-', 'No extra sensitivity'] }],
      ['Several Echos', {
        amazon: ['+', 'Wake word arbitration scores with the energies of Amazon\'s own audio front end, as stock Echos did'],
        microwakeword: ['-', 'Arbitration scores from the audio level instead: the one that answers is a rougher guess'],
        homeassistant: ['-', 'No arbitration between Echos: Home Assistant lets the first satellite that heard it answer'] }],
      ['Where it runs', {
        amazon: ['+', 'On the Echo'],
        microwakeword: ['+', 'On the Echo'],
        homeassistant: ['-', 'On your Home Assistant\'s hardware: the microphone streams there all the time, about 256 kbit/s on your network, and nothing hears the wake word while Home Assistant or the network is down'] }],
      ['Sound and whisper detection', {
        amazon: ['+', 'Work'],
        microwakeword: ['+', 'Keep working: they do not depend on the wake word engine'],
        homeassistant: ['+', 'Sound detection keeps working'] }],
      ['Setting up', {
        amazon: ['+', 'Nothing to do'],
        microwakeword: ['', 'Add a model below (asked when you pick it)'],
        homeassistant: ['-', 'Needs setting up in Home Assistant (shown when you pick it); switching restarts the satellite for a few seconds'] }],
    ] },
  arbitration_mode: { parent: 'arbitration', help: 'How the Echos agree on who answers. Use the same on every Echo: Echos in one mode do not settle wake words with Echos in the other.',
    modes: {
      hassmic: { title: 'Echo network', tag: 'Default', sub: 'hassmic\'s own protocol, between your Echos' },
      kiosk: { title: 'Kiosk Satellite', sub: 'Kiosk Satellite\'s protocol: tablets and Echos together' },
    },
    compare: [
      ['Who takes part', {
        hassmic: ['-', 'Only Echos running hassmic: tablets and other satellites are left to Home Assistant, where the first device to wake up wins'],
        kiosk: ['+', 'Kiosk Satellite tablets too, when they listen for the same wake word (“Alexa”), and Echos in this mode'] }],
      ['Protection', {
        hassmic: ['+', 'Only your Echos: claims are signed with the network\'s key, so nothing else on the network can silence an Echo'],
        kiosk: ['-', 'None: any device on your network can claim every wake word and keep this Echo silent'] }],
      ['Who answers', {
        hassmic: ['+', 'The one that heard you best, scored with the wake word energies of Amazon\'s own audio front end, as stock Echos did. The Echo you are talking to, or one that is ringing, keeps the next wake word'],
        kiosk: ['-', 'The one that heard the wake word loudest over its room. No preference for the device you are talking to: a louder one can take the next wake word in the middle of a conversation. The Echo\'s loudness is not yet calibrated against a tablet\'s microphone: an offset (below) evens it out by ear'] }],
      ['Speed', {
        hassmic: ['+', 'Decided within 0.2 s; with no other Echo around there is no wait at all'],
        kiosk: ['-', 'Every wake word waits the full window (below), even with no other device around'] }],
      ['Lost on Wi-Fi', {
        hassmic: ['+', 'The winner says it answers, so an Echo that heard the wake word late stays quiet too'],
        kiosk: ['-', 'A lost claim means two devices answer; Home Assistant then lets only the first through'] }],
      ['Firewall', {
        hassmic: ['+', 'Nothing to open'],
        kiosk: ['-', 'Opens UDP port 2330 in the Echo\'s firewall, outside the range it otherwise allows'] }],
    ] },
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
  drop_in: { icon: 'talk', ha: ['Drop In', 'Drop In with', 'End Drop In', 'the action drop_in'], help: 'Talk between two Echos, as Alexa\'s Drop In: one Echo calls another of your Echos (the ones listed under Echos below), and both hear each other. Start it from Home Assistant (the action drop_in, or by voice with the "Drop In" blueprint: "drop in kitchen") or with the Drop In button of an Echo below. End it with the action button, "<wake word>, stop" or "hang up", on either Echo. Off: this Echo neither calls nor can be called.',
    note: 'An Echo that is called opens its microphone at once, unless it is set to answer with the button below. Only your own Echos can call it (the Echo network\'s key), never with do not disturb on or the microphones off. The ring shows green while a call runs.' },
  drop_in_answer: { parent: 'drop_in', help: 'At once: a call connects straight away, with a chime, as Alexa\'s Drop In does. After the action button: the Echo rings for 30 s, and only a press of its action button connects; nobody can listen into a room where nobody is.' },
  bluetooth_speaker: { icon: 'box', ha: ['Bluetooth speaker search', 'Play on Bluetooth speaker', 'Bluetooth speaker', 'Bluetooth speaker delay'], help: 'Plays everything (replies, timers, music) on a Bluetooth speaker instead of the Echo\'s own. Put the speaker in pairing mode near the Echo and switch on "Bluetooth speaker search" in Home Assistant: the Echo pairs with the strongest one it hears.' },
};
const CHOICE_NAMES = {
  wake_engine: { amazon: 'Amazon', microwakeword: 'microWakeWord', homeassistant: 'Home Assistant' },
  arbitration_mode: { hassmic: 'Echo network', kiosk: 'Kiosk Satellite' },
  noise_reduction: { off: 'Off', low: 'Low', medium: 'Medium', high: 'High' },
  online_updates: { off: 'Off', beta: 'Beta', release: 'Release' },
  drop_in_answer: { auto: 'At once', ask: 'After the action button' },
  bluetooth_announcement_language: { en: 'English', de: 'Deutsch', fr: 'Français', es: 'Español', it: 'Italiano', pt: 'Português', nl: 'Nederlands',
    sv: 'Svenska', da: 'Dansk', nb: 'Norsk', fi: 'Suomi', pl: 'Polski' },
};
const settingValueIndex = (name) => { const s = state.settings.find((x) => x.name === name); return s ? s.value : -1; };
const settingValue = (name) => { const s = state.settings.find((x) => x.name === name); return s ? (s.type === 'choice' ? s.choices[s.value] : s.value) : undefined; };
const label = (s) => { const t = s.label.replace(/ \(experimental\)$/, '').replace(/^Equalizer /, '').replace(' with other Echos', ''); return t[0].toUpperCase() + t.slice(1); };
const fmt = (s, v) => {
  if (s.name === 'timer_ring') return !v ? 'until stopped' : v < 60 ? `${v} s` : `${Math.floor(v / 60)} min${v % 60 ? ` ${v % 60} s` : ''}`;
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
  markNav();
  if (mwwBox) getMww().catch(() => {});                                  // the engine and its models, as Home Assistant may have picked
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
const compareOpen = new Set();          // settings whose pros and cons are open
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
  if (modes) {                          // choices worth weighing: a small card each, what each gives and costs one click away
    const cards = s.choices.map((c) => {
      const { before, after } = HELP[s.name] || {}, m = modes[c];          // a dialog of the page's own before a choice counts
      const pick = async () => {
        const was = s.choices[settingValueIndex(s.name)];
        if (was === c || (before && !(await before(c)))) return;
        await set(s.name, c);
        if (after && settingValue(s.name) === c) await after(c, was);
      };
      return h('div', { class: 'mode', role: 'radio', tabindex: '0', 'aria-checked': 'false', onclick: pick,
        onkeydown: (e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); pick(); } } },
        h('span', { class: 'radio' }),
        h('div', { class: 'mode-body' },
          h('div', { class: 'mode-head' }, h('b', {}, m.title), m.tag ? h('span', { class: 'badge' + (m.tag === 'Experimental' ? ' warn' : '') }, m.tag) : null),
          h('p', { class: 'mode-sub' }, m.sub)));
    });
    // what each gives and costs, question by question: a table, one column per choice (on a phone, one block per
    // question); open stays open over rebuilds
    const mark = { '+': ['pro', 'plus', 'Gives: '], '-': ['con', 'minus', 'Costs: '], '': ['mid', 'dot', ''] };
    const cols = s.choices.map((c) => h('th', { scope: 'col' }, modes[c].title));
    const rows = (HELP[s.name].compare || []).map(([q, by]) => h('tr', {}, h('th', { scope: 'row' }, q), s.choices.map((c, i) => {
      const [k, t] = by[c] || ['', '—'], [cls, ic, sr] = mark[k];
      return h('td', { class: cls, 'data-col': i, 'data-mode': modes[c].title }, h('div', { class: 'cell' }, icon(ic), h('span', {}, h('span', { class: 'sr' }, sr), t)));
    })));
    const table = h('table', { class: 'cmp-table' }, h('thead', {}, h('tr', {}, h('td'), cols)), h('tbody', {}, rows));
    const more = h('details', { class: 'more compare', open: compareOpen.has(s.name),
      ontoggle: () => { if (more.open) compareOpen.add(s.name); else compareOpen.delete(s.name); } },
      h('summary', {}, 'Compare: what each one gives and what it costs'), h('div', { class: 'cmp-wrap' }, table));
    return { el: h('div', { class: 'mode-pick' }, h('div', { class: 'modes' + (cards.length > 2 ? ' modes-3' : ''), role: 'radiogroup', 'aria-label': s.label }, cards), more),
             update: (x) => {
               cards.forEach((b, i) => { const on = i === x.value; b.classList.toggle('on', on); b.setAttribute('aria-checked', String(on)); cols[i].classList.toggle('on', on); });
               table.querySelectorAll('td[data-col]').forEach((td) => td.classList.toggle('on', +td.dataset.col === x.value));
             } };
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
    } else if (sec.id === 'Wake word') {
      el.append(h('div', { class: 'card' }, list.map(settingRow)));
      buildWake(el);
    } else if (sec.id === 'Echos') {
      buildEchos(el, state.settings.find((s) => s.name === 'arbitration'));
    } else if (sec.id === 'System') {
      buildSystem(el, list);
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
  renderTop(); renderAttention(); renderNetwork(); renderDevices(); renderSystem(); renderWake();
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

// The section being read: the last one whose top has passed below the header (an observer marked whichever entered
// last, so a tall section above the one in view, or several at once on load, lit the wrong link); the last one at the
// very bottom of the page, where a short one never gets that far up.  Not while the page is hidden (first load: every
// top is 0 and the empty page is "at its end", which lit the last one)
function markNav() {
  const secs = [...document.querySelectorAll('.section')], links = [...$('nav').querySelectorAll('a')];
  if (!secs.length || $('app').classList.contains('hidden')) return;
  const atEnd = window.scrollY > 0 && window.innerHeight + window.scrollY >= document.documentElement.scrollHeight - 4;
  let cur = secs[0];
  for (const s of secs) if (s.getBoundingClientRect().top <= 120) cur = s;
  if (atEnd) cur = secs[secs.length - 1];
  links.forEach((a) => a.classList.toggle('cur', a.dataset.sec === cur.id.slice(4)));
}
function watchNav() {
  if (!watchNav.on) { watchNav.on = true; window.addEventListener('scroll', () => requestAnimationFrame(markNav), { passive: true }); window.addEventListener('resize', markNav); }
  markNav();
}

// ---------------------------------------------------------------- Wake word: microWakeWord's models (mww_store.c)

// ESPHome's collection (github.com/esphome/micro-wake-word-models): the browser fetches them and hands them to the Echo
const MWW_REPO = 'https://raw.githubusercontent.com/esphome/micro-wake-word-models/main/models/v2/';
const MWW_OFFICIAL = [['okay_nabu', 'Okay Nabu'], ['hey_jarvis', 'Hey Jarvis'], ['hey_mycroft', 'Hey Mycroft'], ['alexa', 'Alexa'],
  ['hey_home_assistant', 'Hey Home Assistant', 'experiments/'], ['okay_computer', 'Okay Computer', 'experiments/']];
let mwwBox, mwwWarn, mww = null, mwwBusy = '';
const mwwOpen = new Set();                      // models whose "Tune" is open: still open after the next refresh
const mwwId = (file) => file.replace(/\.(tflite|json)$/i, '').toLowerCase().replace(/[^a-z0-9_-]+/g, '_').replace(/^[^a-z0-9]+/, '').slice(0, 40);

async function getMww() { mww = (await echo.call('GET', '/api/mww')).json(); renderWake(); }

function buildWake(el) {
  mwwWarn = h('div');
  mwwBox = h('div', { class: 'card' });
  davsBox = h('div', { class: 'card' });          // more of Amazon's wake words: where one looks for them
  el.append(mwwWarn, mwwBox, davsBox);
}

async function mwwAdd(id, model, manifest) {
  const j = manifest ? enc.encode(manifest) : new Uint8Array(0);
  await echo.call('POST', `/api/mww/add/${id}`, cat(cat(enc.encode(`${j.length}\n`), j), model));
}

async function mwwDo(what, fn) {
  mwwBusy = what; renderWake();
  try { await fn(); } catch (e) { toast(errText(e)); }
  mwwBusy = ''; await getMww().catch(() => {}); await load().catch(() => {});
}

// files picked: a .tflite each, with the .json of the same name if there is one
// files picked: a .tflite each, with the .json of the same name if there is one.  Map id -> { tflite, json }; throws
// when a manifest came without its model
function mwwGroups(list) {
  const groups = new Map();
  for (const f of list) { const id = mwwId(f.name), g = groups.get(id) || {}; g[/\.json$/i.test(f.name) ? 'json' : 'tflite'] = f; groups.set(id, g); }
  const bad = [...groups].filter(([id, g]) => !g.tflite || !id).map(([id, g]) => (g.json || {}).name || id);
  if (bad.length) throw new Error(`No .tflite for ${bad.join(', ')}: pick the model file together with its manifest`);
  if (!groups.size) throw new Error('Pick a .tflite file');
  return groups;
}

// adds them; the ids that came without a manifest (microWakeWord's defaults then)
async function mwwUpload(groups) {
  for (const [id, g] of groups) await mwwAdd(id, new Uint8Array(await g.tflite.arrayBuffer()), g.json ? await g.json.text() : null);
  return [...groups].filter(([, g]) => !g.json).map(([id]) => id);
}

// one of ESPHome's: the browser fetches it from GitHub, the Echo gets it from here
async function mwwFetch(id, name, dir) {
  const get = async (ext) => { const r = await fetch(MWW_REPO + (dir || '') + id + ext); if (!r.ok) throw new Error(`GitHub: ${r.status}`); return r; };
  let json, model;
  try { json = await (await get('.json')).text(); model = new Uint8Array(await (await get('.tflite')).arrayBuffer()); }
  catch (e) { throw new Error(`Cannot fetch ${name} from GitHub (this browser needs the internet): ${e.message}`); }
  await mwwAdd(id, model, json);
}

async function mwwFiles(list) {
  let groups;
  try { groups = mwwGroups(list); } catch (e) { toast(e.message); return; }
  const have = new Set(((mww || {}).models || []).map((m) => m.id)), again = [...groups.keys()].filter((id) => have.has(id));
  if (again.length && !(await ask({ title: `Replace ${nameList(again)}?`, ok: 'Replace', body: `This Echo has ${again.length > 1 ? 'these models' : 'this model'} already. The file${again.length > 1 ? 's' : ''} you picked take${again.length > 1 ? '' : 's'} ${again.length > 1 ? 'their' : 'its'} place, with ${again.length > 1 ? 'their' : 'its'} own name and threshold.` }))) return;
  mwwDo('Adding…', async () => {
    const lone = await mwwUpload(groups);
    toast(lone.length ? `Added. ${nameList(lone)} came without a manifest (.json): threshold and name are microWakeWord's defaults, change them below.` : 'Added');
  });
}

function mwwOfficial(id, name, dir) {
  mwwDo(`Fetching ${name} from GitHub…`, async () => { await mwwFetch(id, name, dir); toast(`${name} added`); });
}

const mwwLosses = () => [callout('warn', h('p', {}, h('b', {}, 'It hears the wake word significantly worse than Amazon\'s engine. '),
    'Expect more missed wake words, above all from across the room or over music, and more false wakes from TV and conversation.')),
  h('p', { class: 'ask-sub' }, 'While it is on, you also lose:'),
  h('ul', { class: 'pc' }, ['\u201c<wake word>, stop\u201d to end a ringing timer or a reply',
    'The extra sensitivity while the Echo plays music, rings or speaks',
    'Amazon\'s wake words, also those downloaded from Amazon',
    'The audio front end\'s scores in wake word arbitration: with several Echos, the one that answers is a rougher guess']
    .map((t) => h('li', { class: 'con' }, icon('minus'), h('span', {}, t)))),
  h('p', { class: 'help' }, 'Sound detection and whisper detection keep working. You can switch back at any time.'
    + (settingValue('wake_engine') === 'homeassistant' ? ' Leaving Home Assistant\'s wake word restarts the satellite for a few seconds.' : ''))];

// What Home Assistant needs before it hears the wake word in the Echo's stream (its docs, 2026-10: add-ons are "Apps")
const haSteps = () => h('ol', { class: 'ha-steps' },
  h('li', {}, h('b', {}, 'A wake word engine in Home Assistant. '), 'Settings \u203a Apps (Add-ons on older versions) \u203a ', h('b', {}, 'openWakeWord'),
    ' \u203a Install, then Start. Settings \u203a Devices & services then shows it as discovered: Configure, Submit. Home Assistant in a container? Run ',
    h('code', {}, 'wyoming-openwakeword'), ' beside it and add it with the Wyoming Protocol integration (port 10400).'),
  h('li', {}, h('b', {}, 'Your own wake words (optional). '), 'Put the ', h('code', {}, '.tflite'), ' file into ', h('code', {}, '/share/openwakeword'),
    ' (with the Samba app, for instance). The app takes ', h('code', {}, '.tflite'), ' only: an ', h('code', {}, '.onnx'),
    ' model, as EchoMuse\'s Forge makes them, has to be converted to ', h('code', {}, '.tflite'), ' first.'),
  h('li', {}, h('b', {}, 'The assistant. '), 'Settings \u203a Voice assistants: open the assistant this Echo uses, then \u22ee (top right) \u203a ',
    h('b', {}, 'Add streaming wake word'), '. Under \u201cStreaming wake word engine\u201d pick openwakeword and the wake word, then Update.'),
  h('li', {}, h('b', {}, 'This Echo. '), 'On its device page (Settings \u203a Devices & services \u203a ESPHome), pick that assistant as its ', h('b', {}, 'Assistant'), '.'));

// the satellite restarts (the wake word moved to or from Home Assistant): this page's Echo answers again within seconds
async function restarted() {
  toast('The satellite restarts, back in a few seconds\u2026');
  await waitBack(echo);
  await load().catch(() => {});
}

function haSwitchDialog() {
  const fromMww = settingValue('wake_engine') === 'microwakeword';
  return ask({ title: 'Let Home Assistant listen?', ok: 'Stream to Home Assistant',
    body: [callout('info', h('p', {}, h('b', {}, 'The Echo stops listening for the wake word itself and streams its microphone to Home Assistant all the time. '),
        'Home Assistant needs a wake word engine for that; without one nothing hears the wake word. Set it up there, before or after switching:')),
      haSteps(),
      h('p', { class: 'ask-sub' }, 'Compared with Amazon\'s engine, you lose:'),
      h('ul', { class: 'pc' }, ['Likely some reliability: openWakeWord\'s models are trained mostly on synthetic speech',
        'Wake word arbitration between Echos: Home Assistant lets the first satellite that heard it answer',
        '\u201c<wake word>, stop\u201d, and the extra sensitivity while the Echo plays music, rings or speaks',
        'The wake word while Home Assistant or the network is down']
        .map((t) => h('li', { class: 'con' }, icon('minus'), h('span', {}, t)))),
      h('p', { class: 'help' }, 'The satellite restarts for a few seconds.' + (fromMww ? ' microWakeWord\'s models stay on the Echo for when you switch back.' : ''))] });
}

// Before the engine becomes microWakeWord: what it costs, and, on an Echo without a model yet, the wake word to start
// with (one of ESPHome's, or files of your own), added before the switch: the Echo refuses microWakeWord without one.
// Resolves true once the switch may go ahead.
async function mwwSwitchDialog() {
  try { await getMww(); } catch (e) { toast(errText(e)); return false; }
  const need = !mww.models.length;
  return new Promise((done) => {
    let ok = false, busy = false, choice = null;
    const msg = h('p', { class: 'help ask-msg', role: 'alert' });
    const go = h('button', { type: 'submit', class: 'primary', disabled: need }, 'Switch to microWakeWord');
    const cancel = h('button', { type: 'button', onclick: () => dlg.close() }, 'Cancel');
    const close = h('button', { type: 'button', class: 'icon-btn', title: 'Close', 'aria-label': 'Close', onclick: () => dlg.close() }, icon('x'));
    const file = h('input', { type: 'file', accept: '.tflite,.json', multiple: true, class: 'hidden' });
    const fileName = h('span', { class: 'mww-pick-sub' }, 'A .tflite with its .json manifest');
    const opts = [...MWW_OFFICIAL.map(([id, name, dir]) => ({ key: id, name, sub: dir ? 'experiment' : 'from ESPHome\'s collection', id, dir })),
      { key: 'files', name: 'Files of your own…', sub: null }];
    const rows = opts.map((o) => {
      const row = h('label', { class: 'mww-pick' }, h('input', { type: 'radio', name: 'mww-first', value: o.key }),
        h('span', { class: 'mww-pick-t' }, o.name, o.sub ? h('span', { class: 'mww-pick-sub' + (o.sub === 'experiment' ? ' warn' : '') }, o.sub) : fileName));
      if (o.key === 'files') row.firstChild.onclick = () => file.click();      // also when it is ticked already: other files
      row.firstChild.onchange = () => {
        msg.textContent = '';
        if (o.key === 'files') { if (!(choice && choice.key === 'files')) { choice = null; go.disabled = true; } return; }
        choice = o; go.disabled = false;
      };
      return row;
    });
    file.onchange = () => {
      msg.textContent = '';
      try { const g = mwwGroups([...file.files]); choice = { key: 'files', groups: g }; fileName.textContent = [...g.keys()].join(', '); go.disabled = false; }
      catch (e) { choice = null; go.disabled = true; msg.textContent = e.message; }
    };
    const pickBox = need ? [h('p', { class: 'ask-sub' }, 'This Echo has no microWakeWord model yet. Pick the wake word to start with:'),
      h('div', { class: 'mww-picks', role: 'radiogroup' }, rows), file,
      h('p', { class: 'help' }, 'ESPHome\'s models come from GitHub through this browser. More, and settings per model, under \u201cmicroWakeWord models\u201d once it is on.')] : [];
    const form = h('form', { method: 'dialog', class: 'ask-form' }, h('div', { class: 'modal-body' }, ...mwwLosses(), ...pickBox, msg), h('div', { class: 'modal-foot' }, cancel, go));
    const dlg = h('dialog', { class: 'modal ask', 'aria-labelledby': 'mww-sw-title' }, h('div', { class: 'modal-head' }, h('h2', { id: 'mww-sw-title' }, 'Switch to microWakeWord?'), close), form);
    form.onsubmit = async (e) => {
      e.preventDefault();
      if (busy) return;
      if (need) {
        if (!choice) { msg.textContent = 'Pick a wake word first.'; return; }
        busy = true; go.disabled = cancel.disabled = true; rows.forEach((r) => { r.firstChild.disabled = true; });
        go.replaceChildren(h('span', { class: 'spin' }), choice.key === 'files' ? 'Adding the model…' : `Fetching ${choice.name}…`);
        try {
          if (choice.key === 'files') { const lone = await mwwUpload(choice.groups); if (lone.length) toast(`${nameList(lone)} came without a manifest: microWakeWord's default threshold, change it under \u201cTune\u201d`); }
          else await mwwFetch(choice.id, choice.name, choice.dir);
        } catch (err) {
          busy = false; go.disabled = cancel.disabled = false; rows.forEach((r) => { r.firstChild.disabled = false; });
          go.textContent = 'Switch to microWakeWord'; msg.textContent = errText(err); return;
        }
      }
      ok = true; dlg.close();
    };
    dlg.oncancel = (e) => { if (busy) e.preventDefault(); };            // Escape: not while a model is on its way
    dlg.onclose = () => { dlg.remove(); done(ok); };
    document.body.append(dlg);
    dlg.showModal(); cancel.focus();
  });
}

async function mwwSave(m) {
  try {
    const [man, model] = await Promise.all([echo.call('GET', `/api/mww/file/${m.id}/manifest.json`), echo.call('GET', `/api/mww/file/${m.id}/model.tflite`)]);
    const j = man.json(); j.model = `${m.id}.tflite`;               // as ESPHome expects them: side by side, named alike
    for (const [name, data, type] of [[`${m.id}.json`, JSON.stringify(j, null, 2), 'application/json'], [`${m.id}.tflite`, model.bytes, 'application/octet-stream']]) {
      const a = h('a', { href: URL.createObjectURL(new Blob([data], { type })), download: name });
      document.body.append(a); a.click(); a.remove(); setTimeout(() => URL.revokeObjectURL(a.href), 5000);
    }
  } catch (e) { toast(errText(e)); }
}

function mwwTune(m) {
  const row = (label, min, max, step, value, show, key, help) => {
    const r = h('input', { type: 'range', min: String(min), max: String(max), step: String(step), value: String(value), 'aria-label': label });
    const out = h('output', {}, show(value));
    const paint = () => { r.style.setProperty('--p', `${((r.value - min) * 100) / (max - min)}%`); out.textContent = show(+r.value); };
    r.oninput = paint; r.onchange = () => mwwDo('Saving…', () => echo.call('POST', `/api/mww/edit/${m.id}`, `${key}=${r.value}\n`)); paint();
    return h('div', { class: 'mww-tune' }, h('div', { class: 'row-label' }, label), h('p', { class: 'help' }, help), h('div', { class: 'slider' }, r, out));
  };
  const d = h('details', { class: 'mww-more', ontoggle: () => { if (d.open) mwwOpen.add(m.id); else mwwOpen.delete(m.id); } }, h('summary', {}, 'Tune'),
    row('Threshold', 0.5, 0.99, 0.01, m.cutoff, (v) => v.toFixed(2), 'cutoff',
      'How sure the model must be. Lower: it hears you more often, and wakes by mistake more often. The model\'s author set it for a Voice PE; on the Echo try 0.05 lower if it misses you.'),
    row('Window', 1, 20, 1, m.window, (v) => `${v * 30} ms`, 'window',
      'How long it must stay that sure. Longer: fewer false wakes from short sounds, slower to answer.'));
  d.open = mwwOpen.has(m.id);
  return d;
}

function renderWake() {
  if (!mwwBox) return;
  const eng = settingValue('wake_engine'), on = eng === 'microwakeword', ms = (mww || {}).models || [];
  mwwWarn.replaceChildren(...(on ? [callout('warn', h('p', {}, h('b', {}, 'microWakeWord is listening for the wake word. '),
    'It detects significantly worse than Amazon\'s engine, and \u201c<wake word>, stop\u201d, the extra sensitivity while the Echo plays and the front end\'s arbitration scores are off. Switch back above at any time.'))]
    : eng === 'homeassistant' ? [h('div', { class: 'card' }, cardHead('Home Assistant listens', 'The Echo streams its microphone to Home Assistant, which listens for the wake word. What it needs there:', 'ha'),
      h('div', { class: 'card-body' }, haSteps()))] : []));
  mwwBox.classList.toggle('hidden', !on);                     // the models are microWakeWord's business: shown while it is on
  if (!on) return;
  if (mwwBox.contains(document.activeElement) && document.activeElement.type === 'range') return;     // not under a dragging thumb
  const file = h('input', { type: 'file', accept: '.tflite,.json', multiple: true, class: 'hidden', onchange: () => { mwwFiles([...file.files]); file.value = ''; } });
  const head = cardHead('microWakeWord models', ['Wake word models for microWakeWord: a ', h('code', {}, '.tflite'), ' file with its ', h('code', {}, '.json'),
    ' manifest, as ESPHome uses them. Home Assistant\'s wake word select offers them; pick one there or with \u201cUse\u201d. ',
    'The Echos section copies them to your other Echos.'], 'ear');
  const rows = ms.map((m) => {
    const active = on && mww.active === m.id, busy = !!mwwBusy;
    const rename = async () => {
      const n = await ask({ title: `Rename ${m.name}`, ok: 'Rename', body: 'The name Home Assistant shows in the satellite\'s wake word select and in the pipeline\'s runs. Echos in wake word arbitration only compete for wake words of the same name.',
        input: { label: 'Name', value: m.name, max: 63 }, check: (v) => !v ? 'Give it a name.' : enc.encode(v).length > 63 ? 'At most 63 bytes.' : null });
      if (n && n !== m.name) mwwDo('Saving…', () => echo.call('POST', `/api/mww/edit/${m.id}`, `name=${n}\n`));
    };
    const del = async () => {
      const last = on && ms.length === 1;
      if (await ask({ title: `Delete ${m.name}?`, ok: 'Delete', danger: true,
        body: last ? [callout('info', h('p', {}, 'It is the only model, so the Echo goes back to Amazon\'s engine.'))] : 'It goes from this Echo; other Echos keep their copy.' }))
        mwwDo('Deleting…', () => echo.call('POST', `/api/mww/delete/${m.id}`));
    };
    const meta = [m.langs ? m.langs.split(',').join(', ') : null, m.author || null, mb(m.size), `threshold ${m.cutoff.toFixed(2)}`].filter(Boolean).join(' · ');
    return h('div', { class: 'row mww-row' },
      h('div', {}, h('div', { class: 'row-label' }, m.name, active ? h('span', { class: 'badge ok' }, icon('check'), 'Listening for it') : null, h('code', { class: 'mww-id' }, m.id)),
        h('p', { class: 'help' }, meta), mwwTune(m)),
      h('div', { class: 'ctl btns' },
        on && !active ? h('button', { class: 'small', disabled: busy, onclick: () => mwwDo('Switching…', () => echo.call('POST', `/api/mww/use/${m.id}`)) }, 'Use') : null,
        h('button', { class: 'small', disabled: busy, title: 'Rename', onclick: rename }, icon('pen'), 'Rename'),
        h('button', { class: 'small', disabled: busy, title: 'Save the .tflite and .json', onclick: () => mwwSave(m) }, icon('down')),
        h('button', { class: 'small danger', disabled: busy, title: 'Delete', onclick: del }, icon('trash'))));
  });
  const have = new Set(ms.map((m) => m.id));
  const official = h('div', { class: 'chips' }, MWW_OFFICIAL.map(([id, name, dir]) => h('button', { type: 'button', class: 'chip' + (have.has(id) ? ' on' : ''),
    disabled: have.has(id) || !!mwwBusy || ms.length >= 16, onclick: () => mwwOfficial(id, name, dir) }, h('span', { class: 'tick' }, icon(have.has(id) ? 'check' : 'plus')), name,
    dir ? h('span', { class: 'chip-n warn' }, 'experiment') : null)));
  mwwBox.replaceChildren(head,
    ms.length ? h('div', { class: 'clients' }, rows) : empty('ear', 'No microWakeWord model on this Echo yet. Add one from ESPHome\'s collection or from your files.'),
    h('div', { class: 'card-body mww-add' }, h('p', { class: 'help' }, 'Add from ESPHome\'s collection on GitHub (your browser downloads them for the Echo):'), official),
    h('div', { class: 'card-foot' }, mwwBusy ? h('div', { class: 'busy grow' }, h('span', { class: 'spin' }), mwwBusy) : h('span', { class: 'help grow' }, `${ms.length} of 16 models`),
      h('button', { disabled: !!mwwBusy || ms.length >= 16, onclick: () => file.click() }, icon('up'), 'Add from files…'), file));
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
  modelBox = h('div', { class: 'card' });
  el.append(syncBox, modelBox);
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
        : a && a.mode === 'kiosk' ? h('span', { class: 'badge' }, 'Kiosk Satellite mode') : null,
        state.dropin && state.dropin.state !== 'idle' ? h('span', { class: 'badge ok' }, icon('talk'), `Drop In ${DROPIN_WORDS[state.dropin.state]} ${state.dropin.peer}`) : null),
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
        h('div', { class: 'echo-meta' }, meta), login, dropButton(r, name)),
      h('a', { class: 'echo-go', href: base + '/', title: `${name}: its settings page`, 'aria-label': `Open the settings page of ${name}` }, icon('ext'))));
  }
  if (!others.size) devBox.append(h('div', { class: 'echo ghost' }, h('span', { class: 'mini-puck' }),
    h('div', { class: 'echo-body' }, h('div', { class: 'echo-name' }, 'No other Echo yet'),
      h('div', { class: 'help' }, 'Echos running hassmic on the same network show up here by themselves within a minute. Then this section copies settings and models to them.'))));
  syncBox.classList.toggle('hidden', !others.size); modelBox.classList.toggle('hidden', !others.size);
  renderSync(); renderDavs(); renderModels();
}

// Drop In (dropin.c) on a member of the network: from this Echo, which needs no login on the other one
const DROPIN_WORDS = { calling: 'calling', ringing: 'ringing from', connected: 'with' };
function dropButton(r, name) {
  const dr = state.dropin;
  if (!dr || !r || !r.member || !settingValue('drop_in')) return null;
  const mine = dr.state !== 'idle' && dr.peer === r.node, busy = dr.state !== 'idle' && !mine;
  return h('div', { class: 'echo-acts' }, h('button', { class: 'small' + (mine ? '' : ' primary'), 'data-k': 'drop ' + r.node, disabled: busy,
    title: mine ? `End the Drop In with ${name}` : busy ? 'This Echo is in another Drop In' : `Talk to ${name}: it connects at once, or rings until its action button`,
    onclick: () => dropIn(mine ? 'end' : r.node, name) }, icon('talk'), mine ? (dr.state === 'connected' ? 'End Drop In' : 'Cancel') : 'Drop In'));
}
async function dropIn(what, name) {
  try { await echo.call('POST', '/api/dropin', what); toast(what === 'end' ? `Drop In with ${name} ended` : `Dropping in on ${name}…`); }
  catch (e) { toast(`Drop In: ${errText(e)}`); }
  for (let i = 0; i < 6; i++) { await new Promise((ok) => setTimeout(ok, 500)); await load().catch(() => {}); renderDevices(); if (!state.dropin || state.dropin.state !== 'calling') break; }
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
  davsBox.append(cardHead('Download from Amazon', 'More wake words and Amazon\'s newer models, straight from Amazon onto this Echo. Once here, the Echos section copies them to your other Echos.', 'ext'));
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
    if (!(await ask({ title: 'Sign out of Amazon?', ok: 'Sign out', danger: true, body: 'This Echo leaves your Alexa app; downloading again needs a new code.' }))) return;
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
  if (a.kind === 'mww') return `microWakeWord model ${a.name}`;
  if (a.kind === 'sound') return 'Sound detection, newer model';
  if (a.kind === 'whisper') return 'Whisper detection';
  const [w, ...loc] = a.name.split('-');
  return `Wake word ${w[0].toUpperCase() + w.slice(1)}${loc.length ? ` (${loc.join('-')})` : ''}`;
}
const mb = (n) => n >= 1048576 ? `${(n / 1048576).toFixed(1)} MB` : `${Math.max(1, Math.round(n / 1024))} KB`;

function renderModels() { if (modelBox) keepFocus(modelBox, drawModels); }
function drawModels() {
  modelBox.innerHTML = '';
  modelBox.append(cardHead('Copy models', 'Amazon\'s extra models from scripts/artifacts.sh (more wake words, whisper detection, the newer sound detection model) and microWakeWord\'s models, Echo to Echo instead of adding them on each. A model is tried on the receiving Echo first; each Echo that gets one of Amazon\'s restarts its satellite once, for a few seconds.', 'box'));
  if (!myArts) { modelBox.append(empty('warn', 'This Echo cannot list its models.')); return; }
  const echos = [{ name: echo.hello.name, key: '', e: echo, arts: myArts }, ...ready().filter((d) => d.arts).map((d) => ({ name: d.echo.hello.name, key: d.echo.base, e: d.echo, arts: d.arts }))];
  const all = new Map();
  for (const x of echos) for (const a of x.arts.artifacts) if (!all.has(a.id)) all.set(a.id, a);
  if (!all.size) { modelBox.append(empty('box', 'None of these Echos has any of Amazon\'s extra models or a microWakeWord model yet: download or add them first, then copy from here.')); return; }
  // where each model comes from: this Echo if it has it, else the first that does
  const source = (id) => echos.find((x) => x.arts.artifacts.some((a) => a.id === id));
  const theirs = (x, id) => x.arts.artifacts.find((a) => a.id === id);
  const needs = (x, id) => { const s = source(id), t = theirs(x, id); return x !== s && (!t || t.digest !== theirs(s, id).digest); };
  // a model every Echo listed has already, the same version everywhere, has nothing to copy: left out
  const ids = [...all.keys()].filter((id) => echos.some((x) => needs(x, id))).sort((p, q) => artLabel(all.get(p)).localeCompare(artLabel(all.get(q))));
  const everywhere = all.size - ids.length;
  if (!ids.length) {
    modelBox.append(empty('check', echos.length > 1 ? `Every Echo here has all ${plural(all.size, 'model')}, the same version: nothing to copy.`
      : `This Echo's ${plural(all.size, 'model')}: log in to another Echo above to copy them to it.`, echos.length > 1));
    return;
  }
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
  if (everywhere) list.append(h('p', { class: 'help copy-same' }, `${plural(everywhere, 'more model')} ${everywhere === 1 ? 'is' : 'are'} on every Echo here already, the same version.`));
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
  const total = jobs.reduce((n, j) => n + j.art.size, 0), notes = [], got = new Map();   // got: Echos that install and restart
  let done = 0, installed = 0;                                                            // microWakeWord's: in place at commit
  copying = { text: 'Starting…', done: 0, total }; renderModels();
  for (const j of jobs) {
    try { await transfer(j, done, total); if (j.art.kind !== 'mww') got.set(j.to.key, j.to); else installed++; }
    catch (e) { notes.push(`${artLabel(j.art)} → ${j.to.name}: ${errText(e).replace(/^Error: /, '')}`); }
    done += j.art.size;
  }
  for (const x of got.values()) {
    copying = { text: `Installing on ${x.name}; its satellite restarts…`, done: total, total }; renderModels();
    try { await x.e.call('POST', '/api/artifact/install'); await waitBack(x.e); }
    catch (e) { notes.push(`${x.name}: ${errText(e).replace(/^Error: /, '')}`); }
  }
  copying = null; mpick.rows.clear();
  toast(notes.length ? notes.join(' · ') : `Copied and installed: ${plural(installed + jobs.filter((j) => j.art.kind !== 'mww').length, 'model')}`);
  if (got.has('')) { setTimeout(() => location.reload(), 500); return; }       // this Echo restarted: start the page over
  if (mwwBox) getMww().catch(() => {});
  await refreshDevices();
}

// ---------------------------------------------------------------- System: settings file, debug access, browsers

let adbBox, clientsBox, importMsg, nameCtl, wifiCtl;

// The name: the model and the end of its MAC address (main.c name_make), never set here.  What people call it is the name
// given in Home Assistant when the Echo was added.
function nameRow() {
  const now = h('div', { class: 'row-val' });
  nameCtl = { paint: () => {
    now.replaceChildren(echo.hello.name, ' ', h('code', { title: 'Node name: ESPHome device name, host name' }, echo.hello.node));
  } };
  return infoRow('Name', 'Made from the model and the end of its MAC address, so no two Echos share it and a reset keeps it. Name it in Home Assistant: on the device\'s page there (the pencil), which is where everything else picks names up from.',
    null, null, now);
}

// A dialog of the page's own, as the Wi-Fi one: title, a body that scrolls, a foot for its buttons.  cls names it for
// its size (logs).  onclose: when it is gone.
function modal(title, cls) {
  const t = h('h2', { id: 'm-title-' + cls }, title), body = h('div', { class: 'modal-body' }), foot = h('div', { class: 'modal-foot' });
  const m = { body, foot, onclose: null };
  const close = h('button', { type: 'button', class: 'icon-btn', title: 'Close', 'aria-label': 'Close', onclick: () => m.dlg.close() }, icon('x'));
  m.head = h('div', { class: 'modal-head' }, t, close);
  m.dlg = h('dialog', { class: 'modal ' + cls, 'aria-labelledby': 'm-title-' + cls }, m.head, body, foot);
  m.dlg.onclose = () => { m.dlg.remove(); if (m.onclose) m.onclose(); };
  m.show = () => { document.body.append(m.dlg); m.dlg.showModal(); };
  return m;
}

// A row of the System cards: what it is (and what it is now), what it does, its buttons; extra (a form, the log) under it all
function infoRow(title, help, ctl, extra, val) {
  return h('div', { class: 'row stack' },
    h('div', {}, h('div', { class: 'row-label' }, title), val || null, help ? h('p', { class: 'help' }, help) : null),
    h('div', { class: 'ctl btns' }, ctl), extra || null);
}

// ---------------------------------------------------------------- Wi-Fi (wifi.c; root's scripts/device/wifi.sh scans and switches)

// Why a switch did not work, from wifi.sh's word for how far the Echo got
const WIFI_WHY = {
  notfound: (n) => `The Echo could not find ${n}: out of reach, or the name is not exactly right (capitals count).`,
  noassoc: (n) => `${n} did not let the Echo in. Routers that take only WPA3, or only devices they know, refuse it.`,
  wrongkey: () => 'The password was not accepted.',
  noaddress: (n) => `The Echo got on ${n}, but no address from it (no answer from its DHCP server).`,
  nogateway: (n) => `The Echo got an address on ${n}, but the router there did not answer.`,
  bad: () => 'The Echo\'s Wi-Fi did not take the new network\'s settings.',
};
// what the Echo cannot join (its wpa_supplicant takes WPA2 with a password, or open networks)
const WIFI_NO = { sae: 'WPA3 only: the Echo cannot join it', wep: 'WEP: too old for the Echo', eap: 'Enterprise login: not supported', owe: 'Enhanced Open: not supported' };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
// signal strength as the Wi-Fi fan: the dot and up to three arcs
const wifiSig = (dbm) => {
  const n = dbm >= -55 ? 4 : dbm >= -65 ? 3 : dbm >= -75 ? 2 : 1, on = (i) => (i <= n ? 'on' : 'off');
  return h('span', { class: 'wsig', title: `${dbm} dBm`, role: 'img', 'aria-label': `Signal ${n} of 4`,
    html: `<svg viewBox="0 0 24 24" aria-hidden="true"><path class="${on(4)}" d="M2.5 9.5a13.5 13.5 0 0 1 19 0"/><path class="${on(3)}" d="M5.6 12.8a9 9 0 0 1 12.8 0"/>`
      + `<path class="${on(2)}" d="M8.7 16.1a4.6 4.6 0 0 1 6.6 0"/><circle class="on" cx="12" cy="19.2" r="1.3"/></svg>` });
};
// as wifi.c takes it: 8 to 63 printable ASCII characters, or the PSK as 64 hex digits; empty for an open network
const wifiPassProblem = (p) => !p || /^[0-9a-fA-F]{64}$/.test(p) ? null
  : !/^[\x20-\x7e]*$/.test(p) ? 'A Wi-Fi password has only the characters of an English keyboard.'
  : p.length < 8 || p.length > 63 ? 'A Wi-Fi password has 8 to 63 characters.' : null;

function wifiRow() {
  const now = h('div', { class: 'row-val' });
  wifiCtl = { paint: () => {
    const w = state.wifi, ip = (w && w.ip) || location.hostname;
    now.replaceChildren(...(w ? [w.ssid, h('span', { class: 'row-val-sub' }, ip)]
      : [h('span', { class: 'row-val-sub' }, `Network not known yet: it shows once the Echo has looked for networks · ${ip}`)]));
  } };
  return infoRow('Wi-Fi', 'A switch tries the new network first and keeps it only once the Echo is on it; if the Echo does not get on it within about a minute, it goes back to this one by itself.',
    h('button', { onclick: wifiDialog }, icon('wifi'), 'Switch network…'), null, now);
}

// The dialog: the password first if you like, then a network from the scan or one typed by hand, a last look at what
// the switch costs (most likely a new address), then the switch, followed from here as long as the Echo answers here.
function wifiDialog() {
  const d = { w: null, scanning: false, scanErr: '', busy: false, closed: false, net: null };
  const title = h('h2', { id: 'wifi-title' }), body = h('div', { class: 'modal-body' }), foot = h('div', { class: 'modal-foot' });
  const dock = h('div', { class: 'modal-dock' });     // below what scrolls: the password, in reach however long the list
  const close = h('button', { type: 'button', class: 'icon-btn', title: 'Close', 'aria-label': 'Close', onclick: () => dlg.close() }, icon('x'));
  const dlg = h('dialog', { class: 'modal', 'aria-labelledby': 'wifi-title' }, h('div', { class: 'modal-head' }, title, close), body, dock, foot);
  dlg.oncancel = (e) => { if (d.busy) e.preventDefault(); };          // Escape: not while a switch runs
  dlg.onclose = () => { d.closed = true; dlg.remove(); load().catch(() => {}); };

  const pass = h('input', { type: 'password', id: 'wifi-pass', autocomplete: 'off', spellcheck: 'false', maxlength: '64' });
  const eye = h('button', { type: 'button', class: 'icon-btn', title: 'Show the password', 'aria-label': 'Show the password', 'aria-pressed': 'false' }, icon('eye'));
  eye.onclick = () => { const show = pass.type === 'password'; pass.type = show ? 'text' : 'password'; eye.setAttribute('aria-pressed', String(show)); };
  const passMsg = h('p', { class: 'help' });
  const passField = h('div', { class: 'field wifi-pass' }, h('label', { for: 'wifi-pass' }, 'Password'), h('div', { class: 'pass-wrap' }, pass, eye), passMsg);
  const list = h('div', { class: 'net-list', role: 'list' }), scanInfo = h('span', { class: 'net-count' });
  const rescan = h('button', { type: 'button', class: 'icon-btn', title: 'Scan again', 'aria-label': 'Scan again', onclick: () => scan() }, icon('refresh'));
  const manual = h('input', { type: 'text', id: 'wifi-ssid', maxlength: '32', autocomplete: 'off', spellcheck: 'false', placeholder: 'Network name', 'aria-label': 'Network name' });
  const manualGo = h('button', { type: 'button', class: 'primary' }, 'Next');
  const newAddress = (n) => callout('info', h('p', {}, h('b', {}, 'The Echo will most likely get a new IP address'), ` on ${n || 'the other network'}, and this page cannot follow it there. `,
    'Find the new address in Home Assistant afterwards: on the Echo\'s device page, under ', h('b', {}, 'Web UI address'), '. ',
    'At the new address this browser needs the action button once more.'));
  let repaint = () => {};
  pass.oninput = () => repaint();
  manual.oninput = () => { manualGo.disabled = !manual.value || enc.encode(manual.value).length > 32; };
  manual.onkeydown = (e) => { if (e.key === 'Enter' && !manualGo.disabled) manualGo.click(); };
  manualGo.onclick = () => confirmView({ ssid: manual.value, hex: hex(enc.encode(manual.value)), sec: null });
  manualGo.disabled = true;

  function paintList() {
    const w = d.w, here = w && w.current && w.current.hex, nets = (w && w.scan && w.scan.networks) || [];
    const rows = nets.map((n) => {
      const no = WIFI_NO[n.sec], cur = n.hex === here;
      return h('button', { type: 'button', role: 'listitem', class: 'net-row' + (cur ? ' here' : '') + (no ? ' no' : ''), disabled: !!no || cur,
        onclick: () => confirmView(n) },
        wifiSig(n.signal),
        h('span', { class: 'grow' }, h('span', { class: 'net-name' }, n.ssid),
          h('span', { class: 'net-sub' }, no || `${n.band === 'both' ? '2.4 and 5' : n.band} GHz${n.sec === 'open' ? ' · open' : ''}`)),
        cur ? h('span', { class: 'net-end ok' }, icon('check'), 'Connected') : n.sec === 'psk' ? h('span', { class: 'net-end', title: 'Needs a password' }, icon('lock')) : null);
    });
    if (!nets.length) rows.push(h('div', { class: 'net-empty' }, d.scanning ? [h('span', { class: 'spin' }), 'Looking for networks…'] : d.scanErr ? '' : 'No networks seen.'));
    // a network that is hidden, or out of reach right now: typed by hand
    rows.push(d.other ? h('div', { class: 'net-row net-form' }, manual, manualGo)
      : h('button', { type: 'button', class: 'net-row net-other', onclick: () => { d.other = true; paintList(); manual.focus(); } },
        h('span', { class: 'wsig' }, icon('plus')), h('span', { class: 'grow' }, h('span', { class: 'net-name' }, 'Other network…'),
          h('span', { class: 'net-sub' }, 'Hidden, or not in reach right now'))));
    list.replaceChildren(...rows);
    scanInfo.replaceChildren(d.scanErr ? h('span', { class: 'f-bad' }, d.scanErr) : nets.length ? String(nets.length) : '');
    rescan.disabled = d.scanning; rescan.classList.toggle('spinning', d.scanning);
  }

  async function scan() {
    if (d.scanning) return;
    d.scanning = true; d.scanErr = ''; paintList();
    try {
      let id = null;
      for (let i = 0; id === null; i++) {          // 409: a request waits, or a switch runs: wait for it
        try { id = (await echo.call('POST', '/api/wifi/scan')).json().id; } catch (e) { if (!/switching|not taken/.test(e.message) || i >= 15) throw e; await sleep(2000); }
      }
      for (let i = 0; i < 20 && !d.closed; i++) {
        await sleep(1500);
        d.w = (await echo.call('GET', '/api/wifi')).json();
        if (d.w.pending && d.w.pending.stale) throw new Error('The Echo\'s system side did not take the request: is it installed with this version (scripts/install-system.sh, or an update)?');
        if (d.w.scan && d.w.scan.id === id) break;
        if (i === 19) throw new Error('No scan came back');
      }
    } catch (e) { d.scanErr = e.message.startsWith('The Echo') || e.message === 'No scan came back' ? e.message : errText(e); }
    d.scanning = false;
    if (!d.closed) paintList();
  }

  // only what is wrong with the password, nothing while it is fine
  const passCheck = () => { const bad = wifiPassProblem(pass.value); passMsg.textContent = bad || ''; passMsg.classList.toggle('f-bad', !!bad); };

  function pickView() {
    d.net = null; repaint = passCheck;
    title.textContent = 'Switch Wi-Fi network';
    body.replaceChildren(
      h('div', { class: 'net-head' }, h('span', { class: 'net-label' }, 'Networks', scanInfo), rescan),
      list);
    dock.replaceChildren(passField);
    foot.replaceChildren(h('button', { type: 'button', onclick: () => dlg.close() }, 'Cancel'));
    repaint(); paintList();
  }

  function confirmView(net) {
    d.net = net;
    const open = net.sec === 'open', go = h('button', { type: 'button', class: 'primary', onclick: () => run(net) }, 'Switch now');
    repaint = () => {
      const bad = wifiPassProblem(pass.value), need = net.sec === 'psk' && !pass.value;
      passMsg.textContent = bad || (need ? 'This network needs its password.' : !pass.value && !net.sec ? 'Without a password the Echo joins it as an open network.' : '');
      passMsg.classList.toggle('f-bad', !!(bad || need));
      go.disabled = !!(bad || need);
    };
    title.textContent = `Switch to ${net.ssid}?`;
    dock.replaceChildren();
    body.replaceChildren(
      newAddress(net.ssid),
      open ? null : passField,
      h('p', { class: 'help' }, 'If the Echo does not get on it within about a minute, it goes back to the network it is on now, and this page tells you why. Once it works, the Echo forgets the networks it knew before.'));
    foot.replaceChildren(h('button', { type: 'button', onclick: pickView }, 'Back'), go);
    repaint();
    if (!open && !pass.value) pass.focus();
  }

  const step = h('div', { class: 'wifi-step' });
  function workView(net) {
    title.textContent = `Switching to ${net.ssid}`;
    close.classList.add('hidden'); dock.replaceChildren();
    body.replaceChildren(step, newAddress(net.ssid));
    foot.replaceChildren(h('span', { class: 'help' }, 'Keep this open: it shows what happened if the Echo comes back here.'));
  }
  const setStep = (text) => step.replaceChildren(h('span', { class: 'spin' }), h('span', {}, text));

  function doneView(kind, net, r) {
    d.busy = false; close.classList.remove('hidden');
    const again = h('button', { type: 'button', onclick: () => { pickView(); scan(); } }, 'Try again');
    const done = h('button', { type: 'button', class: 'primary', onclick: () => dlg.close() }, 'Close');
    if (kind === 'ok') {
      const moved = r.ip && r.ip !== location.hostname, url = `http://${r.ip}${location.port ? ':' + location.port : ''}/`;
      title.textContent = `On ${r.ssid || net.ssid}`;
      body.replaceChildren(callout('ok', h('p', {}, `The Echo is on ${r.ssid || net.ssid} now, at ${r.ip || 'a new address'}.`),
        moved ? h('p', {}, h('a', { href: url }, 'Open its settings page there'), ' (the action button approves this browser there once more).') : null,
        r.saved ? null : h('p', {}, 'It could not save the network, though: after a restart it goes back to the one before.')),
        h('p', { class: 'help' }, 'Home Assistant finds it again on its own when it can reach that network; otherwise enter the new address in the ESPHome integration.'));
      foot.replaceChildren(done);
    } else if (kind === 'failed') {
      title.textContent = `Not switched to ${net.ssid}`;
      const why = (WIFI_WHY[r.reason] || (() => 'It did not work.'))(r.ssid || net.ssid);
      body.replaceChildren(callout('bad', h('p', {}, why),
        h('p', {}, r.state === 'interrupted' ? 'The switch was cut short (did the Echo restart?). It is on its saved network.'
          : r.back ? `The Echo is back on ${r.back}.` : 'The Echo is on no network right now; a restart brings it back on its saved one.')));
      foot.replaceChildren(again, done);
    } else if (kind === 'lost') {
      title.textContent = `Most likely on ${net.ssid}`;
      body.replaceChildren(callout('info', h('p', {}, `The Echo has not come back to this address, so it is most likely on ${net.ssid} now, with a new address. `,
        'Find it in Home Assistant: on the Echo\'s device page, under ', h('b', {}, 'Web UI address'), '. ',
        'If Home Assistant cannot reach that network, it shows the Echo as unavailable: enter the new address in the ESPHome integration.')),
        h('p', { class: 'help' }, 'If it could not get on the network, it would have come back here within two minutes.'));
      foot.replaceChildren(done);
    } else {
      title.textContent = 'Not switched';
      body.replaceChildren(callout('bad', h('p', {}, r)));
      foot.replaceChildren(again, done);
    }
  }

  async function run(net) {
    const p = net.sec === 'open' ? '' : pass.value;
    d.busy = true; workView(net); setStep('Asking the Echo…');
    let id;
    for (let i = 0; !id; i++) {                     // 409: a scan still runs or waits (picked while it looked): wait for it
      try {
        id = (await echo.call('POST', '/api/wifi/join', (ctr) => {
          if (!p) return `${net.hex} -`;
          const b = new Uint8Array(64); b.set(enc.encode(p));        // padded: the length of the password stays inside
          return `${net.hex} ${hex(seal(echo.k, ctr, b))}`;
        })).json().id;
      } catch (e) {
        if (!/not taken/.test(e.message) || i >= 15 || d.closed) { doneView('error', net, errText(e).replace(/^Error: /, '')); return; }
        setStep('Waiting for the scan to finish…');
        await sleep(2000);
      }
    }
    setStep(`Waiting for the Echo to start…`);
    // wifi.sh decides within a minute and needs up to 45 s more to get back: an Echo that does not answer here after
    // that is on the new network
    const t0 = Date.now();
    while (Date.now() - t0 < 150000 && !d.closed) {
      await sleep(2000);
      let w;
      try { w = (await echo.call('GET', '/api/wifi')).json(); } catch (e) {
        if (e.message === 'login' || e.message === 'forged') { doneView('error', net, errText(e)); return; }
        setStep(`Joining ${net.ssid}… The Echo does not answer here any more: that is expected once it is on the new network. If it cannot get on it, it comes back here.`);
        continue;
      }
      const r = w.result && w.result.id === id ? w.result : null;
      if (w.pending && w.pending.stale) { doneView('error', net, 'The Echo\'s system side did not take the request: is it installed with this version (scripts/install-system.sh, or an update)?'); return; }
      if (r && r.state !== 'switching') { doneView(r.state === 'ok' ? 'ok' : 'failed', net, r); return; }
      if (r) setStep(`Joining ${net.ssid}…`);
    }
    if (!d.closed) doneView('lost', net);
  }

  document.body.append(dlg);
  dlg.showModal();
  pickView();
  echo.call('GET', '/api/wifi').then((r) => { d.w = r.json(); paintList(); }).catch(() => {}).finally(() => scan());
}

function buildSystem(el, list) {
  el.append(h('div', { class: 'card' }, nameRow(), state.wifi !== undefined ? wifiRow() : null, list.map(settingRow)));
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
  adbBox = h('div', { class: 'btns' });
  el.append(h('div', { class: 'card' },
    infoRow('Settings file', ['All settings except what belongs to this one Echo (its name, keys, pairings), as a text file. Import it on another Echo, keep it as a backup, or give it to ', h('code', {}, 'scripts/setup.sh --preset'), ' so the next Echo you install starts with these settings.'],
      [h('button', { onclick: exportFile }, icon('down'), 'Export'), h('button', { onclick: () => file.click() }, icon('up'), 'Import…'), file], h('div', { class: 'row-x' }, importMsg)),
    logRow(),
    infoRow('Debug access', 'Opens adb over Wi-Fi for 30 minutes: a root shell on this Echo for anyone on your network while it is open. Only for troubleshooting. Opening needs a press of the action button, even from an approved browser.', adbBox),
    infoRow('Factory reset', 'Forgets everything this Echo was told: settings, name, Home Assistant\'s key, approved browsers, Bluetooth pairings, and its Wi-Fi networks. It then waits to be set up again, like after the install: the Home Assistant app finds it over Bluetooth. The same as holding the action button for 10 seconds.',
      h('button', { class: 'danger', onclick: factoryReset }, 'Factory reset…'))));

  clientsBox = h('div');
  el.append(h('div', { class: 'card' },
    infoRow('Approved browsers', 'Browsers that may change settings on this Echo. Revoke one you no longer use.', null),
    h('div', { class: 'clients' }, clientsBox)));
}

// Factory reset (main.c core_reset): root wipes state/ and the Wi-Fi networks, the Echo leaves the network
async function factoryReset() {
  if (!await ask({ title: 'Factory reset?', ok: 'Reset', danger: true, body: [
    `${echo.hello.name} forgets its settings, its name, Home Assistant's key, the browsers approved here, its Bluetooth pairings and its Wi-Fi networks, and leaves the network.`,
    'Then it waits with the orange setup spinner on the ring: set it up again with the Home Assistant app (Settings, Devices, Add device), which finds it over Bluetooth. Home Assistant has to add it again too.'] })) return;
  try { await echo.call('POST', '/api/reset', 'reset'); toast(`${echo.hello.name} is resetting and leaves the network: set it up again with the Home Assistant app`); }
  catch (e) { toast(errText(e)); }
}

function renderSystem() {
  if (!adbBox) return;
  if (nameCtl) nameCtl.paint();
  if (wifiCtl) wifiCtl.paint();
  const s = state.adb;
  const status = s.waiting ? h('span', { class: 'badge acc' }, 'Waiting for the action button…')
    : s.open ? h('span', { class: 'badge bad' }, 'Open: closes by itself after 30 min') : h('span', { class: 'badge' }, 'Closed');
  adbBox.replaceChildren(status,
    s.open ? h('button', { onclick: adbClose }, 'Close now') : h('button', { class: 'danger', disabled: s.waiting, onclick: adbOpen }, icon('term'), 'Open'));

  clientsBox.innerHTML = '';
  for (const k of state.clients) {
    const b = h('button', { class: 'small' + (k.me ? '' : ' danger') }, k.me ? 'Log out' : 'Revoke');
    b.onclick = async () => {
      if (!(await ask(k.me ? { title: 'Log out?', ok: 'Log out', danger: true, body: 'This browser leaves this Echo. Logging in again needs the action button.' }
        : { title: `Revoke ${k.label}?`, ok: 'Revoke', danger: true, body: 'That browser can no longer change settings here. Logging in again needs the action button.' }))) return;
      try { await echo.call('POST', '/api/revoke', k.pub); } catch (e) { toast(errText(e)); return; }
      if (k.me) showLogin(); else load();
    };
    clientsBox.append(h('div', { class: 'row client' }, h('div', { class: 'row-label' }, icon('key'), k.label, k.me ? h('span', { class: 'badge acc' }, 'This browser') : null), h('div', { class: 'ctl' }, b)));
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

// Loaded on demand only: up to 2 MB, and the Echo reads it from flash.  Part 1 is the rotated older part (main.sh keeps one).
// Shown in a dialog of its own, as big as the window allows: log lines are long.
function logRow() {
  return infoRow('Log', 'What hassmic, the firewall and updates wrote, newest at the bottom: the first place to look when something does not work, and what to attach to a bug report. It travels unencrypted, like everything on this page, so someone watching your network can read it: names, addresses and when the Echo was spoken to.',
    h('button', { onclick: logDialog }, icon('file'), 'Show log…'));
}

function logDialog() {
  const parts = [null, null];           // [boot.log, boot.log.1] as text once fetched
  const m = modal('Log', 'logs');
  const pre = h('pre', { class: 'log', tabindex: '0', 'aria-label': 'Log' });
  const filter = h('input', { type: 'search', placeholder: 'Filter', 'aria-label': 'Show only lines with this text' });
  const older = h('input', { type: 'checkbox' });
  const olderBox = h('label', { class: 'log-older hidden' }, older, 'With the older part');
  const info = h('p', { class: 'help log-info grow' });
  const refresh = h('button', { type: 'button', class: 'icon-btn', title: 'Load again', 'aria-label': 'Load again' }, icon('refresh'));
  const save = h('button', { type: 'button', disabled: true }, icon('down'), 'Download');
  const text = () => (older.checked && parts[1] ? parts[1] : '') + (parts[0] || '');
  const paint = (toEnd) => {
    const q = filter.value.trim().toLowerCase(), all = text().split('\n');
    if (all[all.length - 1] === '') all.pop();
    const lines = q ? all.filter((l) => l.toLowerCase().includes(q)) : all;
    const end = toEnd || pre.scrollTop + pre.clientHeight >= pre.scrollHeight - 20;    // stay at the end if there already
    pre.replaceChildren(...lines.map(logLine));
    if (end) pre.scrollTop = pre.scrollHeight;
    if (!all.length) { info.textContent = 'The log is empty.'; return; }
    info.textContent = (q ? `${lines.length} of ${all.length} lines` : `${all.length} lines`)
      + (parts[1] === '' ? '' : older.checked ? ', older part included' : '') + '. Times in this browser\'s time zone; "boot+" is seconds since the Echo started, before it had the time from Home Assistant. Secrets (the Sendspin pairing token) are blanked.';
  };
  const fetchPart = async (i) => { parts[i] = (await echo.call('GET', `/api/log/${i}`)).text(); };
  const load = async () => {
    refresh.disabled = true; refresh.classList.add('spinning');
    try {
      await fetchPart(0);
      if (older.checked || parts[1] === null) await fetchPart(1);
      olderBox.classList.toggle('hidden', !parts[1]);
      save.disabled = false;
      paint(true);
    } catch (e) { toast(errText(e)); if (parts[0] === null) m.dlg.close(); }
    refresh.disabled = false; refresh.classList.remove('spinning');
  };
  refresh.onclick = load;
  filter.oninput = () => paint(true);
  older.onchange = () => paint(false);
  save.onclick = () => {
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([text()], { type: 'text/plain' }));
    a.download = `hassmic-${echo.hello.name.toLowerCase().replace(/[^a-z0-9]+/g, '-')}-boot.log`; a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  };
  m.head.insertBefore(refresh, m.head.lastChild);
  pre.append(h('span', { class: 'log-wait' }, h('span', { class: 'spin' }), 'Loading…'));
  m.body.append(h('div', { class: 'btns log-tools' }, filter, olderBox), pre);
  m.foot.append(info, save, h('button', { type: 'button', class: 'primary', onclick: () => m.dlg.close() }, 'Close'));
  m.show();
  load();
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
