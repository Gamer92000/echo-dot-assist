// echo-local-alarms-card: the alarms of an Echo running hassmic (custom_components/echo_local), as the Alexa app shows them:
// a row per alarm with its switch, tap one to change its time, days and label, "Add alarm", and Snooze / Stop while
// one rings.  Plain custom elements, no build step: the integration serves this file and loads it on every dashboard.
//
//   type: custom:echo-local-alarms-card
//   device_id: <the Echo's device>     # optional with one Echo
//   title: Bedroom alarms              # optional
//
// Written for old browsers too (no ?? or ?.): a wall tablet's or an old phone app's engine would refuse the whole
// file over one of them, and Home Assistant then says only "Custom element not found".

const DAYS = [['mon', 'Mon'], ['tue', 'Tue'], ['wed', 'Wed'], ['thu', 'Thu'], ['fri', 'Fri'], ['sat', 'Sat'], ['sun', 'Sun']];
const PRESETS = [['Once', []], ['Weekdays', ['mon', 'tue', 'wed', 'thu', 'fri']], ['Weekends', ['sat', 'sun']], ['Every day', DAYS.map((d) => d[0])]];
const esc = (s) => String(s === undefined || s === null ? '' : s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
const pad = (n) => String(n).padStart(2, '0');

function clock(h, m, h24) {
  return h24 ? { t: `${pad(h)}:${pad(m)}`, ap: '' } : { t: `${h % 12 || 12}:${pad(m)}`, ap: h < 12 ? 'AM' : 'PM' };
}

function daysText(days) {
  const k = days.join();
  if (!days.length) return 'Once';
  if (k === 'mon,tue,wed,thu,fri') return 'Weekdays';
  if (k === 'sat,sun') return 'Weekends';
  if (days.length === 7) return 'Every day';
  return days.map((d) => DAYS.find((x) => x[0] === d)[1]).join(', ');
}

function untilText(iso) {
  if (!iso) return '';
  const s = (new Date(iso) - Date.now()) / 1000;
  if (s < 0) return '';
  const mins = Math.round(s / 60), h = Math.floor(mins / 60), m = mins % 60;
  return h >= 24 ? `in ${Math.round(h / 24)} day${Math.round(h / 24) === 1 ? '' : 's'}` : h ? `in ${h} h${m ? ` ${m} min` : ''}` : `in ${m} min`;
}

class EchoLocalAlarmsCard extends HTMLElement {
  setConfig(config) {
    this._config = { ...(config || {}) };
    this._edit = null;                                         // null, or { id: entity_id | 'new', time, days, label }
    this._sig = '';
    if (!this.shadowRoot) this.attachShadow({ mode: 'open' });
    this._draw();
  }

  set hass(hass) { this._hass = hass; this._draw(); }

  getCardSize() { return 2 + this._alarms().length; }

  static getConfigElement() { return document.createElement('echo-local-alarms-card-editor'); }

  static getStubConfig(hass) {
    const e = Object.values(hass.entities || {}).find((x) => x.platform === 'echo_local' && x.device_id);
    return e ? { device_id: e.device_id } : {};
  }

  // the Echo's alarm switches (switch.py): this device's, or every Echo's
  _alarms() {
    const h = this._hass;
    if (!h) return [];
    const dev = this._config.device_id;
    return Object.values(h.entities || {})
      .filter((e) => e.platform === 'echo_local' && e.entity_id.startsWith('switch.') && (!dev || e.device_id === dev))
      .map((e) => h.states[e.entity_id]).filter((s) => s && s.attributes.alarm_id !== undefined)
      .sort((a, b) => (a.attributes.time || '').localeCompare(b.attributes.time || ''));
  }

  _deviceId(alarms) {
    if (this._config.device_id) return this._config.device_id;
    const e = alarms[0] && this._hass.entities[alarms[0].entity_id];
    return e ? e.device_id : undefined;
  }

  // this Echo's devices: hassmic's and the esphome integration's (Home Assistant keeps them apart; the same MAC)
  _twins(dev) {
    const d = this._hass.devices || {}, me = d[dev];
    if (!me) return new Set(dev ? [dev] : []);
    const macs = new Set((me.connections || []).filter((c) => c[0] === 'mac').map((c) => c[1]));
    return new Set(Object.values(d).filter((x) => x.id === dev || (x.connections || []).some((c) => c[0] === 'mac' && macs.has(c[1]))).map((x) => x.id));
  }

  // ringing: "Alarm ringing" of the esphome integration (at once), else the switches' last poll
  _ringing(alarms) {
    const h = this._hass, twins = this._twins(this._deviceId(alarms));
    const live = Object.values(h.entities || {}).find((e) => twins.has(e.device_id) && /alarm_ringing$/.test(e.entity_id));
    const st = live && h.states[live.entity_id];
    return live ? !!st && st.state === 'on' : alarms.some((a) => a.attributes.ringing);
  }

  _call(service, data, target) {
    return this._hass.callService('echo_local', service, data, target).catch((e) => { this._error = e.message || String(e); this._sig = ''; this._draw(); });
  }

  _draw() {
    if (!this.shadowRoot || !this._hass || !this._config) return;
    const alarms = this._alarms(), ringing = this._ringing(alarms);
    const sig = JSON.stringify([alarms.map((a) => [a.entity_id, a.state, a.attributes]), ringing, this._edit, this._error]);
    if (sig === this._sig) return;                             // nothing new: inputs being typed in stay as they are
    this._sig = sig;
    const h24 = alarms.length ? alarms[0].attributes.h24 : !(this._hass.locale && this._hass.locale.time_format === '12');
    const next = alarms.filter((a) => a.state === 'on' && a.attributes.next_ring).sort((a, b) => a.attributes.next_ring.localeCompare(b.attributes.next_ring))[0];
    const dv = (this._hass.devices || {})[this._deviceId(alarms)] || {};
    const title = this._config.title || dv.name_by_user || dv.name || 'Alarms';
    const row = (a) => {
      const at = a.attributes, c = clock(at.hour, at.minute, at.h24), on = a.state === 'on';
      if (this._edit && this._edit.id === a.entity_id) return this._editor(h24);
      const sub = at.snoozed_until ? `Snoozed until ${new Date(at.snoozed_until).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit', hour12: !at.h24 })}`
        : on ? `${daysText(at.days)}${at.next_ring ? ' · ' + untilText(at.next_ring) : ''}` : 'Off';
      return `<div class="row ${on ? '' : 'off'}" data-edit="${esc(a.entity_id)}">
        <div class="time">${esc(c.t)}<span class="ap">${esc(c.ap)}</span></div>
        <div class="what"><div class="label">${esc(at.label || (at.repeats ? 'Alarm' : 'Once'))}</div><div class="sub">${esc(sub)}</div></div>
        <label class="sw" title="${on ? 'On' : 'Off'}"><input type="checkbox" data-toggle="${esc(a.entity_id)}" ${on ? 'checked' : ''}><span></span></label>
      </div>`;
    };
    this.shadowRoot.innerHTML = `<style>${CSS}</style><ha-card>
      <div class="head"><div class="title">${esc(title)}</div>
        <div class="next">${next ? `Next: ${esc(next.attributes.description)} · ${esc(untilText(next.attributes.next_ring))}` : alarms.length ? 'No alarm on' : ''}</div></div>
      ${ringing ? `<div class="ring"><span>Ringing</span><button data-act="snooze">Snooze</button><button class="stop" data-act="stop">Stop</button></div>` : ''}
      ${this._error ? `<div class="err">${esc(this._error)}</div>` : ''}
      <div class="list">${alarms.map(row).join('') || (this._edit && this._edit.id === 'new' ? '' : '<div class="empty">No alarms. Add one, or say “Alexa, wake me up at 7”.</div>')}</div>
      ${this._edit && this._edit.id === 'new' ? this._editor(h24) : `<div class="foot"><button data-act="add">+ Add alarm</button></div>`}
    </ha-card>`;
    this._wire(alarms);
  }

  _editor(h24) {
    const e = this._edit;
    return `<div class="edit">
      <input type="time" class="t" value="${esc(e.time)}" required>
      <div class="days">${DAYS.map(([k, n]) => `<button type="button" class="day ${e.days.includes(k) ? 'on' : ''}" data-day="${k}">${n}</button>`).join('')}</div>
      <div class="presets">${PRESETS.map(([n], i) => `<button type="button" class="preset" data-preset="${i}">${n}</button>`).join('')}</div>
      <input type="text" class="l" maxlength="47" placeholder="Label (Work, Gym, …)" value="${esc(e.label)}">
      <div class="btns">${e.id !== 'new' ? '<button class="del" data-act="delete">Delete</button>' : ''}<span class="grow"></span>
        <button data-act="cancel">Cancel</button><button class="primary" data-act="save">Save</button></div>
    </div>`;
  }

  _wire(alarms) {
    const r = this.shadowRoot, dev = this._deviceId(alarms);
    r.querySelectorAll('[data-toggle]').forEach((i) => {
      i.addEventListener('click', (ev) => ev.stopPropagation());
      i.addEventListener('change', () => this._hass.callService('switch', i.checked ? 'turn_on' : 'turn_off', {}, { entity_id: i.dataset.toggle }));
    });
    r.querySelectorAll('[data-edit]').forEach((row) => row.addEventListener('click', () => {
      const a = this._hass.states[row.dataset.edit].attributes;
      this._edit = { id: row.dataset.edit, time: a.time, days: [...a.days], label: a.label || '' }; this._error = ''; this._draw();
    }));
    const keep = () => {                                        // what is typed survives a redraw (a day ticked)
      const t = r.querySelector('.edit .t'), l = r.querySelector('.edit .l');
      if (t) this._edit.time = t.value;
      if (l) this._edit.label = l.value;
    };
    r.querySelectorAll('[data-day]').forEach((b) => b.addEventListener('click', () => {
      keep(); const d = this._edit.days, k = b.dataset.day;
      this._edit.days = d.includes(k) ? d.filter((x) => x !== k) : DAYS.map((x) => x[0]).filter((x) => x === k || d.includes(x));
      this._draw();
    }));
    r.querySelectorAll('[data-preset]').forEach((b) => b.addEventListener('click', () => { keep(); this._edit.days = [...PRESETS[b.dataset.preset][1]]; this._draw(); }));
    r.querySelectorAll('[data-act]').forEach((b) => b.addEventListener('click', async () => {
      const act = b.dataset.act, devData = dev ? { device_id: dev } : {};
      if (act === 'add') { this._edit = { id: 'new', time: '07:00', days: [], label: '' }; this._error = ''; return this._draw(); }
      if (act === 'cancel') { this._edit = null; return this._draw(); }
      if (act === 'snooze' || act === 'stop') return this._call(act, devData);
      if (act === 'delete') { const id = this._edit.id; this._edit = null; this._draw(); return this._call('delete_alarm', {}, { entity_id: id }); }
      if (act === 'save') {
        keep();
        const e = this._edit;
        if (!/^\d\d:\d\d/.test(e.time)) { this._error = 'Pick a time'; this._sig = ''; return this._draw(); }
        const data = { time: e.time.slice(0, 5) + ':00', days: e.days, label: e.label.trim() };
        this._edit = null; this._error = ''; this._draw();
        return e.id === 'new' ? this._call('add_alarm', { ...devData, ...data }) : this._call('edit_alarm', data, { entity_id: e.id });
      }
    }));
  }
}

const CSS = `
ha-card { padding: 16px 0 8px; }
.head { padding: 0 16px 8px; }
.title { font-size: 20px; font-weight: 500; color: var(--primary-text-color); }
.next, .sub, .empty { color: var(--secondary-text-color); font-size: 14px; }
.ring { display: flex; align-items: center; gap: 8px; margin: 4px 16px 8px; padding: 10px 12px; border-radius: 12px;
  background: color-mix(in srgb, var(--primary-color) 18%, transparent); font-weight: 500; color: var(--primary-text-color); }
.ring span { flex: 1; }
.err { margin: 4px 16px; color: var(--error-color); font-size: 14px; }
.row { display: flex; align-items: center; gap: 12px; padding: 10px 16px; cursor: pointer; border-top: 1px solid var(--divider-color); }
.row:hover { background: color-mix(in srgb, var(--primary-text-color) 4%, transparent); }
.row.off .time, .row.off .label { opacity: .45; }
.time { font-size: 30px; font-weight: 300; font-variant-numeric: tabular-nums; min-width: 4.4em; color: var(--primary-text-color); }
.ap { font-size: 14px; font-weight: 500; margin-left: 4px; color: var(--secondary-text-color); }
.what { flex: 1; min-width: 0; }
.label { font-weight: 500; color: var(--primary-text-color); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.empty { padding: 12px 16px; border-top: 1px solid var(--divider-color); }
.foot { padding: 8px 16px 4px; }
button { font: inherit; font-size: 14px; border-radius: 18px; padding: 6px 14px; cursor: pointer;
  border: 1px solid var(--divider-color); background: none; color: var(--primary-color); }
button.primary, .ring button { background: var(--primary-color); color: var(--text-primary-color, #fff); border-color: var(--primary-color); }
.ring button.stop { background: var(--error-color); border-color: var(--error-color); }
button.del { color: var(--error-color); }
.sw { position: relative; width: 40px; height: 22px; flex: none; }
.sw input { position: absolute; inset: 0; opacity: 0; margin: 0; cursor: pointer; z-index: 1; }
.sw span { position: absolute; inset: 0; border-radius: 11px; background: var(--switch-unchecked-track-color, #9e9e9e); transition: background .15s; }
.sw span::after { content: ''; position: absolute; top: 3px; left: 3px; width: 16px; height: 16px; border-radius: 50%; background: #fff; transition: transform .15s; }
.sw input:checked + span { background: var(--switch-checked-track-color, var(--primary-color)); }
.sw input:checked + span::after { transform: translateX(18px); }
.edit { display: grid; gap: 10px; padding: 12px 16px; border-top: 1px solid var(--divider-color); background: var(--secondary-background-color); }
.edit input { font: inherit; color: var(--primary-text-color); background: var(--card-background-color); border: 1px solid var(--divider-color); border-radius: 8px; padding: 8px 10px; }
.edit input.t { font-size: 26px; width: max-content; }
.days, .presets { display: flex; flex-wrap: wrap; gap: 6px; }
.day { min-width: 48px; color: var(--primary-text-color); }
.day.on { background: var(--primary-color); color: var(--text-primary-color, #fff); border-color: var(--primary-color); }
.preset { font-size: 13px; padding: 4px 10px; }
.btns { display: flex; gap: 8px; align-items: center; }
.grow { flex: 1; }
`;

class EchoLocalAlarmsCardEditor extends HTMLElement {
  setConfig(config) { this._config = { ...config }; this._render(); }
  set hass(hass) { this._hass = hass; this._render(); }
  _render() {
    if (!this._hass || !this._config) return;
    if (!this._form) {
      this._form = document.createElement('ha-form');
      this._form.schema = [{ name: 'device_id', selector: { device: { integration: 'echo_local' } } }, { name: 'title', selector: { text: {} } }];
      this._form.computeLabel = (s) => (s.name === 'device_id' ? 'Echo' : 'Title (optional)');
      this._form.addEventListener('value-changed', (ev) => {
        this._config = ev.detail.value;
        this.dispatchEvent(new CustomEvent('config-changed', { detail: { config: this._config }, bubbles: true, composed: true }));
      });
      this.appendChild(this._form);
    }
    this._form.hass = this._hass;
    this._form.data = this._config;
  }
}

// Defined once Home Assistant's own root element is, through whatever window.customElements is by then: where the
// browser has no scoped registries (Firefox 157), Home Assistant's frontend puts a polyfill in its place, and a card
// that defined itself before that (a reload: this file from memory, evaluated ahead of HA's core) is in the browser's
// own registry, which Home Assistant no longer asks: "Custom element doesn't exist" on every reload, the first load fine.
customElements.whenDefined('home-assistant').then(() => {
  if (!window.customElements.get('echo-local-alarms-card')) window.customElements.define('echo-local-alarms-card', EchoLocalAlarmsCard);
  if (!window.customElements.get('echo-local-alarms-card-editor')) window.customElements.define('echo-local-alarms-card-editor', EchoLocalAlarmsCardEditor);
});
window.customCards = window.customCards || [];
if (!window.customCards.some((c) => c.type === 'echo-local-alarms-card'))
  window.customCards.push({ type: 'echo-local-alarms-card', name: 'Echo alarms', description: 'The alarms of an Echo running hassmic: switch, change, add, snooze.', preview: false });
