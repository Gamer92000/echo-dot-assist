// Echo Local's sidebar panel (/echo-local, registered in __init__.py): every Echo Show's screen, set up graphically.
//
//   - left: the Echos (config entries of echo_local)
//   - "Screen": the home app's options (kiosk.py, panel.py), which the options flow also sets: the home app (any
//     package; the settings page's Screen page sets the same), Home Assistant in it or not, its screens, its settings
//
// A home screen app arranges its Home itself: Echo-Wall has an integration of its own with its own panel
// (Nt-f/Echo-Wall).  Only Home Assistant's websocket (this.hass.connection).  Plain custom element, no build step.
'use strict';

// the home app's pages, as the drawer names them (kiosk.py screens()); Music is the drawer's own (the Echo's Music
// Assistant player), no page of the app's
const KIND_LABEL = { wall: 'Home (the home app’s own)', dashboard: 'A Home Assistant dashboard', clock: 'Clock (the home app’s own)', custom: 'A web page of your own' };
const WALL = 'io.github.nt_f.echowall';
const KNOWN_APPS = [['', 'As the Echo has it (its settings page\'s Screen page)'], ['io.github.nt_f.echowall', 'Echo-Wall'], ['io.github.nt_f.echoscreen', 'Echo-Screen'], ['me.jxl.kiosk_satellite', 'Kiosk Satellite']];

const h = (tag, props, ...kids) => {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(props || {})) {
    if (v == null || v === false) continue;
    if (k === 'class') e.className = v;
    else if (k === 'style') e.style.cssText = v;
    else if (k.startsWith('on')) e.addEventListener(k.slice(2), v);
    else if (k in e && typeof v !== 'string') e[k] = v;
    else e.setAttribute(k, v === true ? '' : v);
  }
  for (const c of kids.flat(Infinity)) if (c != null && c !== false) e.append(c.nodeType ? c : document.createTextNode(String(c)));
  return e;
};
const clone = (x) => JSON.parse(JSON.stringify(x == null ? {} : x));
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);
const ago = (t) => {
  if (!t) return '';
  const s = Math.round(Date.now() / 1000 - t);
  return s < 60 ? 'just now' : s < 3600 ? `${Math.round(s / 60)} min ago` : s < 86400 ? `${Math.round(s / 3600)} h ago` : new Date(t * 1000).toLocaleDateString();
};
const domainOf = (id) => String(id).split('.')[0];
const CSS = `
:host { display: block; height: 100%; background: var(--primary-background-color); color: var(--primary-text-color);
  font-family: var(--ha-font-family-body, Roboto, sans-serif); --r: var(--ha-card-border-radius, 12px);
  --line: var(--divider-color, rgba(127,127,127,.25)); --soft: var(--secondary-background-color, rgba(127,127,127,.08));
  --accent: var(--primary-color, #03a9f4); --muted: var(--secondary-text-color, #888); }
* { box-sizing: border-box; }
header { height: var(--header-height, 56px); display: flex; align-items: center; gap: 8px; padding: 0 12px;
  background: var(--app-header-background-color, var(--primary-color)); color: var(--app-header-text-color, #fff);
  border-bottom: var(--app-header-border-bottom, none); position: sticky; top: 0; z-index: 3; }
header h1 { font-size: 20px; font-weight: 400; margin: 0 0 0 4px; flex: 1; }
header .menu { display: none; }
:host([narrow]) header .menu { display: inline-flex; }
.iconbtn { background: none; border: 0; color: inherit; width: 40px; height: 40px; border-radius: 50%; cursor: pointer; font-size: 22px; }
.iconbtn:hover { background: rgba(127,127,127,.15); }
.wrap { display: grid; grid-template-columns: 260px 1fr; min-height: calc(100vh - var(--header-height, 56px)); }   /* (the Echos' list down to the bottom on a short tab) */
:host([narrow]) .wrap { grid-template-columns: 1fr; }
nav { border-right: 1px solid var(--line); padding: 12px 8px; background: var(--card-background-color); }
:host([narrow]) nav { border-right: 0; border-bottom: 1px solid var(--line); display: flex; overflow-x: auto; gap: 6px; padding: 8px; }
nav .lbl { font-size: 12px; text-transform: uppercase; letter-spacing: .06em; color: var(--muted); margin: 6px 10px 8px; }
:host([narrow]) nav .lbl { display: none; }
.echo { display: flex; gap: 10px; align-items: center; width: 100%; text-align: left; padding: 10px; border-radius: 10px; border: 0;
  background: none; color: inherit; cursor: pointer; font: inherit; }
:host([narrow]) .echo { width: auto; flex: none; border: 1px solid var(--line); }
.echo:hover { background: var(--soft); }
.echo.sel { background: color-mix(in srgb, var(--accent) 16%, transparent); }
.echo .dev { width: 36px; height: 24px; border-radius: 5px; border: 2px solid currentColor; opacity: .7; flex: none; position: relative; }
.echo .dev.on::after { content: ''; position: absolute; inset: 3px; border-radius: 2px; background: var(--accent); opacity: .7; }
.echo .t { font-weight: 500; } .echo .s { font-size: 12px; color: var(--muted); }
main { padding: 16px 20px 40px; min-width: 0; }
:host([narrow]) main { padding: 12px 10px 32px; }
:host([narrow]) .card { padding: 4px 12px 12px; }
:host([narrow]) .item.app { flex-wrap: wrap; }
:host([narrow]) .item.app .nm { flex-basis: 100%; }
.tabs { overflow-x: auto; }
.tabs { display: flex; gap: 4px; border-bottom: 1px solid var(--line); margin-bottom: 16px; }
.tab { background: none; border: 0; border-bottom: 2px solid transparent; padding: 10px 14px; font: inherit; color: var(--muted); cursor: pointer; }
.tab.sel { color: var(--accent); border-bottom-color: var(--accent); }
.apps { display: flex; gap: 8px; flex-wrap: wrap; margin-bottom: 12px; }
.chip { border: 1px solid var(--line); background: var(--card-background-color); color: inherit; border-radius: 18px; padding: 6px 14px;
  font: inherit; font-size: 14px; cursor: pointer; display: inline-flex; gap: 6px; align-items: center; }
.chip.sel { border-color: var(--accent); background: color-mix(in srgb, var(--accent) 14%, transparent); }
.chip .dot { width: 8px; height: 8px; border-radius: 50%; background: var(--success-color, #4caf50); }
.chip .dot.none { background: var(--line); }
.bar { display: flex; gap: 8px; flex-wrap: wrap; align-items: center; margin-bottom: 14px; }
.bar .grow { flex: 1; }
.status { font-size: 13px; color: var(--muted); }
.status.dirty { color: var(--warning-color, #ff9800); }
.btn { border: 1px solid var(--line); background: var(--card-background-color); color: inherit; border-radius: 8px; padding: 8px 14px;
  font: inherit; font-size: 14px; cursor: pointer; }
.btn:hover:not(:disabled) { background: var(--soft); }
.btn:disabled { opacity: .45; cursor: default; }
.btn.primary { background: var(--accent); border-color: var(--accent); color: var(--text-primary-color, #fff); }
.btn.danger { color: var(--error-color, #db4437); }
.btn.small { padding: 4px 10px; font-size: 13px; }
.grid { display: grid; grid-template-columns: minmax(320px, 1fr) minmax(320px, 1.25fr); gap: 16px; align-items: start; }
@media (max-width: 1100px) { .grid { grid-template-columns: 1fr; } }
.grid > * { min-width: 0; }                         /* (a card no wider than the column: its inputs give way) */
.card { background: var(--card-background-color); border-radius: var(--r); border: 1px solid var(--line); padding: 4px 16px 12px;
  box-shadow: var(--ha-card-box-shadow, none); }
.card h2 { font-size: 16px; font-weight: 500; margin: 14px 0 4px; }
.card .about { font-size: 13px; color: var(--muted); margin-bottom: 6px; }
.row { padding: 12px 0; border-top: 1px solid var(--line); }
.row:first-of-type { border-top: 0; }
.row .head { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; }   /* (narrow: the control under its name) */
.row .name { flex: 1; font-size: 15px; }
.row .help { font-size: 12px; color: var(--muted); margin-top: 3px; }
.seg { display: inline-flex; border: 1px solid var(--line); border-radius: 8px; overflow: hidden; }
.seg button { border: 0; background: none; color: inherit; padding: 6px 12px; font: inherit; font-size: 13px; cursor: pointer; }
.seg button + button { border-left: 1px solid var(--line); }
.seg button.sel { background: var(--accent); color: var(--text-primary-color, #fff); }
input[type=text], input[type=number], input[type=url], select, textarea { font: inherit; font-size: 14px; color: inherit;
  background: var(--soft); border: 1px solid var(--line); border-radius: 8px; padding: 7px 10px; }
input[type=number] { width: 110px; }
select, input[type=text], input[type=url] { max-width: 100%; }
textarea { width: 100%; min-height: 90px; font-family: var(--code-font-family, monospace); font-size: 13px; }
.switch { position: relative; width: 40px; height: 22px; flex: none; }
.switch input { opacity: 0; width: 0; height: 0; }
.switch span { position: absolute; inset: 0; border-radius: 11px; background: var(--line); cursor: pointer; transition: .15s; }
.switch span::after { content: ''; position: absolute; width: 16px; height: 16px; left: 3px; top: 3px; border-radius: 50%; background: #fff; transition: .15s; }
.switch input:checked + span { background: var(--accent); }
.switch input:checked + span::after { transform: translateX(18px); }
.list { margin-top: 8px; border: 1px solid var(--line); border-radius: 10px; overflow: hidden; }
.item { display: flex; align-items: center; gap: 8px; padding: 6px 8px; background: var(--card-background-color); border-top: 1px solid var(--line); }
.item:first-child { border-top: 0; }
.item.drag { opacity: .4; }
.item.over { box-shadow: inset 0 2px 0 var(--accent); }
.item .grip { cursor: grab; color: var(--muted); user-select: none; padding: 0 4px; }
.item .nm { flex: 1; min-width: 0; }
.item.page.off .nm { opacity: .55; }
.pagename { font: inherit; border: 1px solid var(--line); border-radius: 6px; padding: 4px 8px; background: transparent; color: inherit; width: 100%; box-sizing: border-box; }
.addpage { display: flex; gap: 8px; margin-top: 8px; }
.addpage .pagename { flex: 1; }
.appicon { width: 36px; height: 36px; border-radius: 9px; flex: none; }
.appicon.none { background: var(--secondary-background-color, #e6e6e6); }
.item .nm div { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.item .id { font-size: 12px; color: var(--muted); }
.item.unknown .nm div:first-child { color: var(--warning-color, #ff9800); }
.mini { background: none; border: 0; color: var(--muted); cursor: pointer; font-size: 16px; width: 28px; height: 28px; border-radius: 6px; }
.mini:hover { background: var(--soft); color: inherit; }
.mini:disabled { opacity: .3; }
.add { display: flex; gap: 6px; margin-top: 8px; }
.add input { flex: 1; }
.empty { padding: 10px; font-size: 13px; color: var(--muted); }
details.room { border: 1px solid var(--line); border-radius: 10px; margin-top: 8px; }
details.room summary { padding: 8px 12px; cursor: pointer; display: flex; gap: 8px; align-items: center; }
details.room summary .n { flex: 1; } details.room .list { margin: 0 8px 8px; }
.preview { position: sticky; top: calc(var(--header-height, 56px) + 12px); }
.screen { position: relative; border-radius: 14px; background: #111; padding: 10px; box-shadow: 0 6px 24px rgba(0,0,0,.25); }
.screen .vp { position: relative; overflow: hidden; border-radius: 4px; background: #000; }
.screen iframe { border: 0; transform-origin: 0 0; position: absolute; left: 0; top: 0; }
.screen .band { height: 4px; margin: 6px 30% 0; border-radius: 2px; background: color-mix(in srgb, var(--accent) 60%, #000); opacity: .6; }
.sketch { position: absolute; inset: 0; padding: 4%; display: grid; grid-template-columns: repeat(4, 1fr); grid-auto-rows: 22%; gap: 3%;
  background: linear-gradient(135deg, #3a2a1a, #120d08); }
.sketch.dusk { background: linear-gradient(135deg, #2a2848, #0b0a18); } .sketch.light { background: linear-gradient(135deg, #f2eee8, #cfc8bd); }
.sketch.photo { background: linear-gradient(135deg, #4a6a7a, #1b2a33 60%, #2f3a24); }
.sketch .w { border-radius: 12px; background: rgba(255,255,255,.14); backdrop-filter: blur(4px); color: #fff; font-size: 11px; padding: 8px;
  overflow: hidden; display: flex; flex-direction: column; justify-content: flex-end; }
.sketch.light .w { background: rgba(255,255,255,.6); color: #222; }
.sketch .w.big { grid-column: span 2; grid-row: span 2; font-size: 14px; }
.sketch .w .k { opacity: .7; font-size: 10px; }
.pbar { display: flex; gap: 8px; align-items: center; margin-top: 10px; flex-wrap: wrap; }
.note { font-size: 12px; color: var(--muted); }
.banner { border: 1px solid var(--warning-color, #ff9800); background: color-mix(in srgb, var(--warning-color, #ff9800) 12%, transparent);
  border-radius: 10px; padding: 10px 12px; display: flex; gap: 10px; align-items: center; margin-bottom: 12px; font-size: 14px; }
.banner .grow { flex: 1; }
.menuwrap { position: relative; display: inline-block; }
.pop { position: absolute; top: calc(100% + 4px); left: 0; z-index: 5; min-width: 260px; background: var(--card-background-color);
  border: 1px solid var(--line); border-radius: 10px; box-shadow: 0 8px 24px rgba(0,0,0,.2); padding: 6px; }
.pop button { display: block; width: 100%; text-align: left; border: 0; background: none; color: inherit; font: inherit; font-size: 14px;
  padding: 8px 10px; border-radius: 6px; cursor: pointer; }
.pop button:hover { background: var(--soft); }
.pop .sep { height: 1px; background: var(--line); margin: 4px 0; }
.pop .h { font-size: 12px; color: var(--muted); padding: 6px 10px 2px; }
.toast { position: fixed; left: 50%; bottom: 24px; transform: translateX(-50%); background: var(--primary-text-color); color: var(--card-background-color, #fff);
  padding: 10px 16px; border-radius: 8px; z-index: 10; font-size: 14px; max-width: 90vw; }
.screens .item .nm { display: flex; align-items: center; gap: 8px; }
.kv { display: grid; grid-template-columns: 1fr; gap: 6px; margin-top: 8px; }
.kv input { width: 100%; }
.pushst { font-size: 13px; padding: 10px 12px; border-radius: 10px; margin-bottom: 12px; display: flex; gap: 10px; align-items: center;
  background: var(--soft); }
.pushst.bad { background: color-mix(in srgb, var(--error-color, #db4437) 12%, transparent); color: var(--error-color, #db4437); }
.pushst .grow { flex: 1; }
.app .nm div:first-child { font-family: var(--code-font-family, monospace); font-size: 14px; }
.prog { height: 6px; border-radius: 3px; background: var(--line); overflow: hidden; margin-top: 8px; }
.prog > div { height: 100%; background: var(--accent); transition: width .2s; }
.inst { margin-top: 10px; font-size: 14px; }
.inst.bad { color: var(--error-color, #db4437); }
.inst.good { color: var(--success-color, #4caf50); }
`;


class EchoLocalPanel extends HTMLElement {
  constructor() {
    super();
    this.attachShadow({ mode: 'open' });
    this.state = { echos: [], loaded: false, error: '' };
    this.echo = null;                 // the Echo's node
    this.tab = 'screen';
    this.kiosk = null;                // { saved, draft } of the selected Echo
    this.apps = null;
  }

  set hass(h) { const first = !this._hass; this._hass = h; if (first) this.load(); if (this._wall) this._wall.hass = h; }
  get hass() { return this._hass; }
  set narrow(n) { this.toggleAttribute('narrow', !!n); this._narrow = !!n; if (this._wall) this._wall.narrow = !!n; }

  connectedCallback() { this.render(); }
  disconnectedCallback() { if (this._wall && this._wall.release) this._wall.release(); this._wall = null; }

  ws(msg) { return this._hass.connection.sendMessagePromise(msg); }
  echoInfo(node = this.echo) { return this.state.echos.find((e) => e.echo === node); }
  toast(t) { const d = h('div', { class: 'toast' }, t); this.shadowRoot.append(d); setTimeout(() => d.remove(), 3500); }

  async load() {
    try {
      const r = await this.ws({ type: 'echo_local/list' });
      this.state = { echos: r.echos, loaded: true, error: '' };
      if (!this.echo) this.echo = (r.echos[0] || {}).echo || null;
    } catch (e) {
      this.state = { ...this.state, loaded: true, error: (e && e.message) || String(e) };
    }
    this.render();
    if (this.echo) this.select(this.echo);
  }

  async refreshList() {
    try { const r = await this.ws({ type: 'echo_local/list' }); this.state.echos = r.echos; this.renderNav(); } catch (e) { /* the next save tells */ }
  }

  select(node) {
    this.echo = node;
    this.kiosk = null;
    this.apps = null;
    this.loadKiosk();
    this.render();
  }

  // ---------------------------------------------------------------- the screen's options

  async loadKiosk() {
    try {
      const r = await this.ws({ type: 'echo_local/kiosk/get', echo: this.echo });
      this.kiosk = { saved: clone(r.options), draft: clone(r.options), kinds: r.kinds, url: r.ha_url, push: r.push };
    } catch (e) { this.kiosk = { error: (e && e.message) || String(e) }; }
    this.render();
  }

  async saveKiosk() {
    const k = this.kiosk;
    const pick = ['kiosk', 'kiosk_screens', 'kiosk_default', 'kiosk_dashboard', 'kiosk_custom_name', 'kiosk_custom_url',
      'kiosk_return', 'kiosk_admin', 'kiosk_ha_url', 'kiosk_app', 'kiosk_app_settings'];
    const opts = {}; for (const p of pick) if (p in k.draft) opts[p] = k.draft[p];
    try {
      const r = await this.ws({ type: 'echo_local/kiosk/set', echo: this.echo, options: opts });
      k.saved = clone(r.options); k.draft = clone(r.options);
      this.toast('Saved: the Echo gets it in a moment');
      setTimeout(() => { if (this.tab === 'screen') this.loadKiosk(); }, 4000);   // the entry reloads, then pushes: its outcome
      this.refreshList();
    } catch (e) { this.toast('Not saved: ' + ((e && e.message) || e)); }
    this.render();
  }

  // ---------------------------------------------------------------- drawing


  // ---------------------------------------------------------------- drawing

  render() {
    const root = this.shadowRoot;
    root.textContent = '';
    root.append(h('style', null, CSS));
    root.append(h('header', null,
      h('button', { class: 'iconbtn menu', title: 'Menu', onclick: () => this.dispatchEvent(new Event('hass-toggle-menu', { bubbles: true, composed: true })) }, '☰'),
      h('h1', null, 'Echo Local')));
    if (!this.state.loaded) { root.append(h('main', null, h('div', { class: 'empty' }, 'Loading…'))); return; }
    if (this.state.error) { root.append(h('main', null, h('div', { class: 'banner' }, 'Echo Local did not answer: ' + this.state.error))); return; }
    root.append(h('div', { class: 'wrap' }, h('nav'), h('main')));
    this.renderNav();
    this.renderMain();
  }

  renderNav() {
    const nav = this.shadowRoot.querySelector('nav'); if (!nav) return;
    nav.textContent = '';
    nav.append(h('div', { class: 'lbl' }, 'Echos'));
    for (const e of this.state.echos) {
      const app = (KNOWN_APPS.find((a) => a[0] === e.app) || [e.app, e.app])[1];
      nav.append(h('button', { class: 'echo' + (e.echo === this.echo ? ' sel' : ''), onclick: () => this.select(e.echo) },
        h('span', { class: 'dev' + (e.kiosk ? ' on' : '') }),
        h('span', null, h('div', { class: 't' }, e.title), h('div', { class: 's' }, (e.kiosk ? 'Screen on' : 'Screen off') + (e.kiosk && app ? ' · ' + app : '')))));
    }
    if (!this.state.echos.length) nav.append(h('div', { class: 'empty' }, 'No Echo added yet: Settings › Devices & services › Add integration › Echo Local.'));
  }

  renderMain() {
    const main = this.shadowRoot.querySelector('main'); if (!main) return;
    main.textContent = '';
    if (!this.echoInfo()) { main.append(h('div', { class: 'empty' }, 'Add an Echo to Echo Local first.')); return; }
    const hasHome = this.echoInfo().app === WALL;
    if (this.tab === 'home' && !hasHome) this.tab = 'screen';
    main.append(h('div', { class: 'tabs' },
      h('button', { class: 'tab' + (this.tab === 'screen' ? ' sel' : ''), onclick: () => { this.tab = 'screen'; if (!this.kiosk) this.loadKiosk(); this.render(); } }, 'Screen'),
      hasHome && h('button', { class: 'tab' + (this.tab === 'home' ? ' sel' : ''), onclick: () => { this.tab = 'home'; this.render(); } }, 'Home')));
    main.append(this.tab === 'home' ? this.homeTab() : this.screenTab());
  }

  // ---------------------------------------------------------------- the Home tab: Echo-Wall's panel, for this Echo

  // Echo-Wall's integration (Nt-f/Echo-Wall) registers its panel; beside Echo Local it has no sidebar entry and lives
  // here.  Its element is loaded from that panel's module and kept across redraws (its drafts and preview with it),
  // one for every Echo: it shows the device that logged in as this Echo's user.
  wallModule() {
    const p = this._hass && this._hass.panels && this._hass.panels['echo-wall'];
    return p && p.config && p.config._panel_custom && p.config._panel_custom.module_url;
  }

  homeTab() {
    const url = this.wallModule();
    if (!url) {
      return h('div', { class: 'card' }, h('h2', null, 'Echo-Wall’s Home'),
        h('div', { class: 'about' }, 'Echo-Wall keeps its Home (tiles, rooms, wallpaper) in an integration of its own. Add it in HACS as a custom repository (Nt-f/Echo-Wall, type Integration), restart Home Assistant, then Settings › Devices & services › Add integration › Echo-Wall. Its Home is arranged here afterwards.'),
        h('a', { href: 'https://my.home-assistant.io/redirect/hacs_repository/?owner=Nt-f&repository=Echo-Wall&category=integration', target: '_blank', rel: 'noreferrer' }, 'Open it in HACS →'));
    }
    const box = h('div', { class: 'hometab' });
    const embed = { user: this.echoInfo().user || '' };
    const put = () => {
      if (!this._wall) { this._wall = document.createElement('echo-wall-panel'); this._wall.narrow = !!this._narrow; }
      this._wall.embed = embed;
      if (this._wall.hass !== this._hass) this._wall.hass = this._hass;
      box.append(this._wall);
    };
    if (customElements.get('echo-wall-panel')) put();
    else {
      box.append(h('div', { class: 'empty' }, 'Loading Echo-Wall…'));
      import(url).then(() => customElements.whenDefined('echo-wall-panel')).then(() => { box.textContent = ''; put(); })
        .catch((e) => { box.textContent = ''; box.append(h('div', { class: 'banner' }, 'Echo-Wall’s panel did not load: ' + ((e && e.message) || e))); });
    }
    return box;
  }

  switch(on, cb) {
    const i = h('input', { type: 'checkbox', checked: on });
    i.addEventListener('change', () => cb(i.checked));
    return h('label', { class: 'switch' }, i, h('span'));
  }

  screenTab() {
    const k = this.kiosk;
    if (!k) return h('div', { class: 'empty' }, 'Loading…');
    if (k.error) return h('div', { class: 'banner' }, k.error);
    const o = k.draft;
    const dirty = !same(k.saved, k.draft);
    const touch = () => { bar.querySelector('.primary').disabled = same(k.saved, k.draft); bar.querySelector('.status').textContent = same(k.saved, k.draft) ? 'Saved' : 'Unsaved changes'; };
    const bar = h('div', { class: 'bar' },
      h('button', { class: 'btn primary', disabled: !dirty, onclick: () => this.saveKiosk() }, 'Save'),
      h('button', { class: 'btn', onclick: () => { k.draft = clone(k.saved); this.render(); } }, 'Revert'),
      h('span', { class: 'status' + (dirty ? ' dirty' : '') }, dirty ? 'Unsaved changes' : 'Saved'),
      h('span', { class: 'grow' }), h('span', { class: 'note' }, 'The Echo reaches Home Assistant at ' + k.url));
    const p = k.push || {};
    const pst = h('div', { class: 'pushst' + (p.at && !p.ok ? ' bad' : '') },
      h('span', { class: 'grow' }, !p.at ? 'Not sent to the Echo since Home Assistant started (it keeps what it had).'
        : p.ok ? `Sent to the Echo ${ago(p.at)}` + (p.kiosk ? `: ${p.screens} page${p.screens === 1 ? '' : 's'} and a fresh login.` : ': the home app not connected.')
        : `Not sent to the Echo (${ago(p.at)}): ${p.error}`),
      h('button', { class: 'btn small', onclick: async (ev) => {
        ev.target.disabled = true;
        try { const r = await this.ws({ type: 'echo_local/kiosk/push', echo: this.echo }); k.push = r.push; this.toast('Sent to the Echo'); }
        catch (e) { this.toast('Not sent: ' + ((e && e.message) || e)); await this.loadKiosk(); return; }
        this.render();
      } }, 'Send to the Echo again'));
    const row = (name, ctl, help) => h('div', { class: 'row' }, h('div', { class: 'head' }, h('span', { class: 'name' }, name), ctl), help && h('div', { class: 'help' }, help));
    const text = (key, ph, type) => { const i = h('input', { type: type || 'text', placeholder: ph || '', style: 'min-width:min(240px, 100%)' }); i.value = o[key] || ''; i.addEventListener('input', () => { o[key] = i.value; touch(); }); return i; };
    const sw = (key) => this.switch(!!o[key], (on) => { o[key] = on; touch(); });

    // (an entry from before keeps kinds there are none of any more: "music", now the drawer's own)
    const scr = (o.kiosk_screens || ['wall', 'dashboard']).filter((x) => k.kinds.includes(x));
    if (scr.length !== (o.kiosk_screens || []).length && o.kiosk_screens) { o.kiosk_screens = scr.slice(); k.saved.kiosk_screens = scr.slice(); }
    if (o.kiosk_default && !k.kinds.includes(o.kiosk_default)) { o.kiosk_default = scr[0] || 'wall'; k.saved.kiosk_default = o.kiosk_default; }
    const screens = h('div', { class: 'list screens' });
    const drawScreens = () => {
      screens.textContent = '';
      const order = [...scr, ...k.kinds.filter((x) => !scr.includes(x))];
      order.forEach((kind) => {
        const on = scr.includes(kind), i = scr.indexOf(kind);
        const it = h('div', { class: 'item' },
          h('div', { class: 'nm' }, this.switch(on, (v) => { if (v) scr.push(kind); else scr.splice(scr.indexOf(kind), 1); o.kiosk_screens = scr.slice(); touch(); drawScreens(); showFields(); }),
            h('span', null, KIND_LABEL[kind] || kind)),
          on && h('button', { class: 'mini', disabled: i === 0, onclick: () => { scr.splice(i - 1, 0, scr.splice(i, 1)[0]); o.kiosk_screens = scr.slice(); touch(); drawScreens(); } }, '↑'),
          on && h('button', { class: 'mini', disabled: i === scr.length - 1, onclick: () => { scr.splice(i + 1, 0, scr.splice(i, 1)[0]); o.kiosk_screens = scr.slice(); touch(); drawScreens(); } }, '↓'));
        screens.append(it);
      });
    };
    // a page's own fields only while it is on
    const dashRow = () => row('Dashboard', text('kiosk_dashboard', 'lovelace'), 'Its path, as in its address: lovelace is Overview.');
    const fields = h('div');
    const showFields = () => {
      fields.textContent = '';
      if (scr.includes('dashboard')) fields.append(dashRow());
      if (scr.includes('custom')) fields.append(row('Web page’s name', text('kiosk_custom_name', 'Page')), row('Web page’s address', text('kiosk_custom_url', 'http://…', 'url')));
    };
    drawScreens(); showFields();
    const def = h('select', { onchange: () => { o.kiosk_default = def.value; touch(); } }, k.kinds.map((x) => h('option', { value: x, selected: (o.kiosk_default || 'wall') === x }, KIND_LABEL[x] || x)));
    const ret = h('input', { type: 'number', min: 0, max: 3600, step: 10 }); ret.value = o.kiosk_return ?? 120;
    ret.addEventListener('input', () => { if (ret.checkValidity() && ret.value !== '') { o.kiosk_return = +ret.value; touch(); } });
    // the home apps known by name, then every app installed on this Echo (any app may be the home app: one not made
    // for hassmic is started as the launcher starts it, logs in to Home Assistant by itself, and gets the words and
    // alarms drawn over it by hassmic), then one typed in
    const installed = ((this.apps && this.apps.apps) || []).map((a) => a.package).filter((p) => !KNOWN_APPS.some(([v]) => v === p)).sort();
    const offered = [...KNOWN_APPS, ...installed.map((p) => [p, `${p} (installed)`])];
    // Echo-Wall keeps its Home's layout in an integration of its own (Nt-f/Echo-Wall, sidebar panel /echo-wall):
    // a way there, or to HACS for it when Home Assistant has no such panel yet
    const hasWall = !!this.wallModule();
    const wallRow = row('Its Home', hasWall
      ? h('a', { href: '#', onclick: (ev) => { ev.preventDefault(); if (this.echoInfo().app !== WALL) { this.toast('Save first: Echo-Wall as the app'); return; } this.tab = 'home'; this.render(); } }, 'Arrange it in the Home tab →')
      : h('a', { href: 'https://my.home-assistant.io/redirect/hacs_repository/?owner=Nt-f&repository=Echo-Wall&category=integration', target: '_blank', rel: 'noreferrer' }, 'Add Echo-Wall’s integration from HACS →'),
      hasWall ? 'Tiles, rooms and wallpaper: Echo-Wall’s own integration, in its Home tab.' : 'Its layout lives in an integration of its own: in HACS, a custom repository (Nt-f/Echo-Wall, Integration), then Settings › Devices › Add integration › Echo-Wall.');
    wallRow.style.display = (o.kiosk_app || '') === WALL ? '' : 'none';
    const appSel = h('select', { onchange: () => { o.kiosk_app = appSel.value === '*' ? (o.kiosk_app || '') : appSel.value; appOwn.style.display = appSel.value === '*' ? '' : 'none'; wallRow.style.display = appSel.value === WALL ? '' : 'none'; touch(); } },
      offered.map(([v, l]) => h('option', { value: v, selected: (o.kiosk_app || '') === v }, l)),
      h('option', { value: '*', selected: !offered.some(([v]) => v === (o.kiosk_app || '')) }, 'Another app (its package)…'));
    const appOwn = text('kiosk_app', 'org.example.screen');
    appOwn.style.display = offered.some(([v]) => v === (o.kiosk_app || '')) ? 'none' : '';
    const settings = h('textarea', { spellcheck: false, placeholder: 'immich.url=http://192.168.1.9:2283\nimmich.key=…' });
    settings.value = o.kiosk_app_settings || '';
    settings.addEventListener('input', () => { o.kiosk_app_settings = settings.value; touch(); });

    return h('div', null, bar, pst, h('div', { class: 'grid' },
      h('div', { class: 'card' }, h('h2', null, 'Home app'),
        h('div', { class: 'about' }, 'The app the Echo opens and comes back to: Echo-Wall, Echo-Screen, Kiosk Satellite or any app installed on the Echo (docs/screen-apps.md). Pull down from the top edge, over any app, for the Echo’s drawer: Home (this app), Face, Music (the Echo’s own Music Assistant player), the pages below and your other apps.'),
        row('App', h('span', null, appSel, ' ', appOwn)),
        wallRow,
        row('Connect it to Home Assistant', sw('kiosk'), 'Gives it a Home Assistant login of this Echo’s own and the pages below. Off: it gets neither (an app not made for hassmic logs in by itself).'),
        h('div', { class: 'row' }, h('div', { class: 'head' }, h('span', { class: 'name' }, 'Its own settings')), h('div', { class: 'help' }, 'One key=value a line, as the app documents them; sent sealed with the Echo’s login.'), settings),
        row('The Echo’s login is an administrator', sw('kiosk_admin'), 'Lets Home Assistant’s settings be changed from the Echo. Off is safer.'),
        row('Home Assistant’s address for the Echo', text('kiosk_ha_url', k.url), 'Empty: the one found. The Echo reaches local addresses only.')),
      h('div', { class: 'card' }, h('h2', null, 'Pages in the drawer'),
        h('div', { class: 'about' }, 'The home app’s pages: the drawer offers them in its row of apps, in this order, before your other apps. Music (in the drawer’s top row) is the Echo’s own player (its Music Assistant token goes on the Echo’s settings page), not a page here.'),
        screens,
        fields,
        row('Opens on', def, 'Where the home app starts.'),
        row('Back to it after', h('span', null, ret, ' ', h('span', { class: 'note' }, 's untouched')), '0: stays where it was left.'))));
  }
}

customElements.get('echo-local-panel') || customElements.define('echo-local-panel', EchoLocalPanel);
