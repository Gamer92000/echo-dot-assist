// The Thread and Zigbee radio (radio.c, docs/re-thread.md, docs/zigbee.md): the Echo 4's 802.15.4 radio as Home
// Assistant's.  The settings (radio, thread_role, zigbee_role) are rows of their own; this card shows what runs from
// GET /api/radio: the Thread network the border router or router is on (never its key), the Zigbee network zigbeed
// runs or joined, and what to give Home Assistant or another host.  Its actions (POST /api/radio): Thread's new
// network, joining one by its dataset (sealed as a Wi-Fi password: it holds the network key), forgetting it; Zigbee's
// permit joining and a router's leave; the one address besides Home Assistant's that may connect.

let radioCtl = null, radioTimer = null;

const RADIO_ROLE = {
  border_router: 'Border router',
  router: 'Router',
  remote: 'Remote radio',
};
const THREAD_STATE = { disabled: 'Off', detached: 'Looking for its network', child: 'Joined (as a child)', router: 'Router', leader: 'Leader', unknown: '—' };

function radioHost() { try { return new URL(echo.base || location.href).hostname || location.hostname; } catch (e) { return location.hostname; } }

// what to give whoever uses the radio, for the mode and role it is in
function radioUse(r) {
  const ip = radioHost(), code = (t) => h('code', { class: 'sel' }, t);
  if (r.mode === 'thread' && r.thread_role === 'border_router') return [
    h('p', {}, 'In Home Assistant: Settings → Devices & services → Add integration → ', h('b', {}, 'OpenThread Border Router'), ', with the URL ', code(`http://${ip}:${r.rest_port}`), '. It shows up in the Thread integration then, and Matter devices on Thread commission through it.'),
    h('p', {}, 'Only Home Assistant (the address its ESPHome connection to this Echo comes from) and the address under "Also let in" may use that URL: whoever reaches it can read the network\'s key.')];
  if (r.mode === 'thread' && r.thread_role === 'router') return [
    h('p', {}, 'A router extends a Thread network another border router runs (Home Assistant\'s, an Apple TV, a Google Nest Hub): it routes for the devices near it, and nothing else. Give it that network\'s dataset with "Join a network…": in Home Assistant, Settings → Devices & services → Thread → Configure, the network\'s ⋮ → the dataset (TLV).')];
  if (r.mode === 'zigbee' && r.zigbee_role === 'coordinator') return [
    h('p', {}, 'Home Assistant finds it by itself: Settings → Devices & services shows a discovered ', h('b', {}, 'Zigbee Home Automation'), ' (ZHA) for it; or add ZHA by hand, pick "Enter manually", radio type ', h('b', {}, 'deCONZ'), ' (ConBee, ConBee II, RaspBee II), and as its serial port ', code(`socket://${ip}:${r.zb_port}`), '. The Echo answers as a ConBee II does.'),
    h('p', {}, 'For Zigbee2MQTT, in its configuration:'),
    h('pre', { class: 'sel' }, `serial:\n  port: tcp://${ip}:${r.zb_port}\n  adapter: deconz`),
    h('p', {}, 'One host at a time: ZHA or Zigbee2MQTT. The network keeps running when neither is connected, and devices stay joined over restarts of either. Zigbee2MQTT on another machine than Home Assistant: name that machine\'s address under "Also let in".')];
  if (r.mode === 'zigbee' && r.zigbee_role === 'router') return [
    h('p', {}, 'A router extends a Zigbee network another coordinator runs (ZHA\'s, Zigbee2MQTT\'s, a Hue bridge\'s): it relays for the devices near it and takes new ones that join through it. It joins the first network that lets it: open joining there ("Add device" in ZHA, "Permit join" in Zigbee2MQTT) within a minute of picking Router, and it shows up there as "hassmic Echo 4 Zigbee router". It stays on that network over restarts; "Leave" makes it look for one again.')];
  if (r.mode === 'zigbee') return [
    h('p', {}, 'For Zigbee2MQTT (2.1.3 or later; its zoh adapter, which runs the Zigbee stack in Zigbee2MQTT itself and is still experimental there), in its configuration:'),
    h('pre', { class: 'sel' }, `serial:\n  port: tcp://${ip}:${r.rcp_port}\n  adapter: zoh`),
    h('p', {}, 'For ZHA, pick Coordinator instead. The radio is one host\'s at a time: a second is turned away while the first has it. Zigbee2MQTT on another machine than Home Assistant: name that machine\'s address under "Also let in".')];
  if (r.mode === 'thread' && r.thread_role === 'remote') return [
    h('p', {}, 'The radio itself, for an OpenThread border router of your own (Spinel over HDLC, as a USB stick\'s), at ', code(`${ip}:${r.rcp_port}`), '. Home Assistant\'s OpenThread Border Router add-on does not take it: Amazon\'s radio firmware lacks two radio features (transmit security and timing) every current border router asks for, and the add-on has no way to skip that check. With otbr-agent of your own, through socat, for example:'),
    h('pre', { class: 'sel' }, `socat pty,link=/dev/ttyEcho,raw tcp:${ip}:${r.rcp_port} &\notbr-agent ... 'spinel+hdlc+uart:///dev/ttyEcho?skip-rcp-compatibility-check'`),
    h('p', {}, 'A border router here on the Echo is the better way: the radio\'s timing stays on the Echo, and Wi-Fi\'s delays do not reach it.')];
  return [h('p', {}, 'The radio is off: Amazon\'s Thread border router and Zigbee hub are stopped, and nothing of ours uses it.')];
}

function radioRows(r) {
  const rows = [], t = r.thread;
  if (r.mode === 'thread' && r.thread_role !== 'remote') {
    if (!t || !t.up) rows.push(infoRow('Thread', 'Starting…', 'The border router starts within a few seconds of the switch, and again within 10 s if it ever ends.', null));
    else {
      const st = THREAD_STATE[t.state] || t.state;
      rows.push(infoRow('Thread', st, 'What the Echo is on its Thread network. A leader runs the network; a router routes for its devices; a border router also connects it with your home network.', null));
      if (t.network) rows.push(infoRow('Network', t.network, 'The Thread network\'s name, channel and extended PAN ID: the same on every device of that network.', null, null,
        h('span', { class: 'row-val' }, `channel ${t.channel >= 0 ? t.channel : '?'} · ${t.extpanid || ''}`)));
      if (t.routers >= 0) rows.push(infoRow('Routers', `${t.routers} on the network`, 'How many routers the network has, this Echo included.', null));
    }
    rows.push(infoRow('Change network', 'A new one, or one you have', 'New network: a Thread network of its own, with a random key; Home Assistant\'s OpenThread Border Router integration then offers to make it the preferred one. Join: the network of a dataset you have (Home Assistant shows it for each network in its Thread settings). Leave: the Echo forgets its network.', null,
      h('div', { class: 'row-x btns' }, h('button', { onclick: radioForm }, 'New network…'), h('button', { onclick: radioJoin }, 'Join a network…'),
        t && t.network ? h('button', { class: 'danger', onclick: radioLeave }, 'Leave…') : null)));
    if (r.thread_role === 'border_router' && t && t.up && t.network)
      rows.push(infoRow('Share the network', r.share_left ? `a code is open, ${r.share_left} s left` : 'with a phone\'s app, by a code', 'A code of 9 digits, good for 2 minutes, that lets a phone\'s Thread app (Google Home, Apple Home and others that ask for a border router\'s code) take this Thread network, so that devices it adds join it. The network\'s key itself never leaves the Echo for it. Nobody else should see the code while it is open.', null,
        h('div', { class: 'row-x btns' }, h('button', { onclick: radioShare }, r.share_left ? 'Show a new code…' : 'Share…'),
          r.share_left ? h('button', { onclick: radioUnshare }, 'End the code') : null)));
  }
  if (r.mode === 'zigbee' && r.zigbee_role !== 'remote') {
    const z = r.zigbee;
    const ZST = { connected: 'On its network', joining: 'Looking for a network to join', offline: 'Waiting for its host to start the network' };
    if (!z || !z.up) rows.push(infoRow('Zigbee', 'Starting…', 'The Zigbee stack starts within a few seconds of the switch, and again within 10 s if it ever ends.', null));
    else {
      rows.push(infoRow('Zigbee', ZST[z.state] || z.state, r.zigbee_role === 'coordinator' ? 'The coordinator runs the network: the first time, the host (ZHA or Zigbee2MQTT) starts it; after that it runs whether a host is connected or not.' : 'A router joins the network you chose below (or the first one that lets it), and stays on it.', null));
      if (z.state === 'connected') {
        rows.push(infoRow('Network', `channel ${z.channel}, PAN ${z.pan.toString(16).padStart(4, '0')}`, 'The Zigbee network\'s channel, PAN ID and extended PAN ID: the same on every device of that network.', null, null,
          h('span', { class: 'row-val' }, z.epan)));
        rows.push(infoRow(r.zigbee_role === 'coordinator' ? 'Devices' : 'Near it', r.zigbee_role === 'coordinator' ? `${z.devices} known, ${z.children} its own children` : `${z.neighbors} routers heard, ${z.children} children`,
          'Children: devices that sleep (sensors, remotes) and talk to the network through this Echo.', null));
      }
      if (r.zigbee_role === 'coordinator') rows.push(infoRow('Host', z.host ? `connected${r.zb_holder ? ' (' + r.zb_holder + ')' : ''}` : 'none connected', 'ZHA or Zigbee2MQTT, connected to the coordinator now.', null));
      if (r.zigbee_role === 'coordinator' && z.state === 'connected')
        rows.push(infoRow('Network key', z.rotating ? 'a new one on its way: the switch in a few seconds' : `number ${z.key_seq}`, 'The key every device of the network holds. A new one goes to every device (sealed with its own key where it has one), and the network switches to it 20 s later; a sleepy device that missed it gets it when it next asks.', null,
          h('div', { class: 'row-x btns' }, h('button', { onclick: radioRotate, disabled: !!z.rotating }, 'New network key…'))));
      if (r.zigbee_role === 'router') rows.push(...radioNetworks(z));
      if (r.zigbee_role === 'router' && z.state === 'connected')
        rows.push(infoRow('Green Power', `${z.gp || 0} switch${z.gp === 1 ? '' : 'es'} paired${z.gp_commissioning ? ', taking new ones' : ''}`, 'Battery-less switches (Friends of Hue, the Hue Tap) in this Echo\'s range: it passes what they send on to Home Assistant (a Green Power proxy). Pair one with "Add device" in ZHA open, then press it.', null));
      if (r.zigbee_role === 'router' && z.state === 'connected') rows.push(infoRow('Trust center', z.key_confirmed ? 'a link key of its own, confirmed' : 'the network\'s common key', 'Zigbee 3.0 coordinators give each device a key of its own once it joined; older ones keep the common one.', null));
      if (z.state === 'connected') rows.push(infoRow('Joining', z.permit ? `open, ${z.permit} s left` : 'closed', 'New devices join while this is open: through this Echo, and (from the coordinator) anywhere in the network. Home Assistant opens it with "Add device" too.', null,
        h('div', { class: 'row-x btns' },
          h('button', { onclick: () => radioPermit(z.permit ? 0 : 180) }, z.permit ? 'Close joining' : 'Allow joining (3 min)'),
          r.zigbee_role === 'router' ? h('button', { class: 'danger', onclick: radioZbLeave }, 'Leave the network…') : null)));
    }
  }
  if (r.mode !== 'off' && r.program) {
    const PROG = { gprcp: 'ours (Thread 1.4)', amazon: 'Amazon\'s', fallback: 'Amazon\'s: ours kept failing, until the next restart' };
    rows.push(infoRow('Radio program', PROG[r.program] || r.program, 'What drives the radio now (the setting "Radio program" above chooses).', null));
  }
  if (r.mode === 'thread' && settingValue('thread_csl'))
    rows.push(infoRow('Sleepy devices (CSL)', r.csl === 'on' ? 'on' : r.csl === 'unavailable' ? `not available: ${r.csl_why || 'see the log'}` : 'starting',
      'Whether the border router sends to devices that sleep on a schedule (CSL): it needs the Echo\'s own kernel module and our radio program (the setting above).', null));
  if (r.mode !== 'off') {
    const inp = h('input', { type: 'text', inputmode: 'decimal', placeholder: 'no other', value: r.allow || '', size: 15, 'aria-label': 'Also let in' });
    rows.push(infoRow('Also let in', r.allow ? r.allow : 'Home Assistant only', 'Who may connect to the radio and to the border router\'s API: Home Assistant (the address its ESPHome connection comes from) and this one address, for a host on another machine (Zigbee2MQTT, a border router of your own). Empty: Home Assistant only.', [inp,
      h('button', { onclick: async () => {
        try { radioShow((await echo.call('POST', '/api/radio', `allow ${inp.value.trim() || '-'}`)).json()); toast('Saved'); } catch (e) { toast(errText(e)); }
      } }, 'Save')]));
    if (r.rcp) rows.push(infoRow('In use by', r.holder || 'nobody', 'The host the radio is with now: one at a time.', null));
  }
  return rows;
}

/* the networks a router heard: which one it joins */
function radioNetworks(z) {
  const nets = (z.networks || []).slice().sort((a, b) => b.lqi - a.lqi);
  const pref = z.prefer && !/^0+$/.test(z.prefer) ? z.prefer : null;
  const rows = [infoRow('Joins', pref ? `only ${pref}` : 'the first network open for joining', 'Which Zigbee network this router joins: one you choose from those it heard, or the first that lets it (open for joining: "Add device" in ZHA or Zigbee2MQTT). Choosing another than the one it is on makes it leave that one.', null,
    pref ? h('div', { class: 'row-x btns' }, h('button', { onclick: () => radioPrefer('any') }, 'Any network')) : null)];
  for (const n of nets) {
    const here = z.state === 'connected' && n.epan === z.epan;
    rows.push(infoRow(n.epan, `channel ${n.channel}, ${n.permit ? 'open for joining' : 'closed'}${n.room ? '' : ', no room'}`, 'A network this Echo heard (its extended PAN ID; the same one ZHA or Zigbee2MQTT shows for its network).', null,
      h('span', { class: 'row-val' }, here ? 'on it' : `signal ${n.lqi}`),
      here || n.epan === pref ? null : h('div', { class: 'row-x btns' }, h('button', { onclick: () => radioPrefer(n.epan) }, 'Join this one'))));
  }
  if (!nets.length) rows.push(infoRow('Networks heard', 'none yet', 'The networks heard while it looked for one to join; a router on a network does not look.', null));
  return rows;
}

async function radioPrefer(epan) {
  try { await echo.call('POST', '/api/radio', `zb_prefer ${epan}`); toast(epan === 'any' ? 'It joins the first network that lets it' : 'It joins that network once it is open for joining'); } catch (e) { toast(errText(e)); }
  radioRefresh();
}

async function radioRotate() {
  const m = modal('A new Zigbee network key', 'radio');
  m.body.append(h('p', {}, 'Every device gets the new key, and 20 s later the network switches to it. Devices that sleep get it at their next wake.'),
    h('p', {}, 'ZHA keeps a backup of the network: make a new one afterwards (Settings → Devices & services → Zigbee → Configure → Create backup). Until then, a restart of Home Assistant reports that the radio\'s settings changed: keep the radio\'s settings there, never restore the old backup (its key is no longer the network\'s).'));
  const go = h('button', { class: 'primary', onclick: async () => {
    go.disabled = true;
    try { await echo.call('POST', '/api/radio', 'zb_rotate'); m.dlg.close(); toast('The new key is on its way'); } catch (e) { toast(errText(e)); go.disabled = false; }
    radioRefresh();
  } }, 'New key');
  m.foot.append(h('button', { onclick: () => m.dlg.close() }, 'Cancel'), go);
  m.show();
}

function radioShow(r) { if (r) state.radio = r; if (radioCtl) radioCtl.paint(); }
async function radioRefresh() { try { radioShow((await echo.call('GET', '/api/radio')).json()); } catch (e) { /* the page's own error shows */ } }

function buildRadio(el, list) {
  const card = h('div'), use = h('div', { class: 'row-t help radio-use' });
  radioCtl = { paint: () => { const r = state.radio; if (!r) return; card.replaceChildren(...radioRows(r)); use.replaceChildren(...radioUse(r)); } };
  el.append(flow([titled('Radio', list.map(settingRow)), titled('On the Echo', card), titled('How to use it', h('div', { class: 'row' }, use))]));
  radioRefresh();
  // while the page shows: the border router's state moves on its own (starting, joining, becoming leader)
  if (!radioTimer) radioTimer = setInterval(() => { if (location.hash.startsWith('#/radio') && !document.hidden) radioRefresh(); }, 4000);
}

async function radioForm() {
  const m = modal('New Thread network', 'radio');
  const name = h('input', { type: 'text', maxlength: 16, value: 'hassmic', 'aria-label': 'Network name' });
  m.body.append(h('p', {}, 'The Echo forgets the network it is on (if any) and starts a new one: random key, channel, PAN ID. Devices on the old one stay there.'),
    h('label', {}, 'Name ', name));
  const go = h('button', { class: 'primary', onclick: async () => {
    go.disabled = true;
    try { await echo.call('POST', '/api/radio', `form ${hex(new TextEncoder().encode(name.value.trim() || 'hassmic'))}`); m.dlg.close(); toast('Started: the Echo becomes the new network\'s leader in a few seconds'); }
    catch (e) { toast(errText(e)); go.disabled = false; }
    radioRefresh();
  } }, 'Start it');
  m.foot.append(h('button', { onclick: () => m.dlg.close() }, 'Cancel'), go);
  m.show();
}

async function radioJoin() {
  const m = modal('Join a Thread network', 'radio');
  const tlv = h('textarea', { rows: 4, spellcheck: 'false', placeholder: '0E080000000000010000000300000F35060004001FFFE0...', 'aria-label': 'Dataset' });
  m.body.append(h('p', {}, 'The network\'s active operational dataset, as hex (TLVs). In Home Assistant: Settings → Devices & services → Thread → Configure, the network\'s ⋮. It holds the network\'s key: it goes to the Echo sealed, and the Echo never shows it again.'), tlv);
  const go = h('button', { class: 'primary', onclick: async () => {
    const t = tlv.value.replace(/\s+/g, '');
    if (!/^([0-9a-fA-F]{2}){2,254}$/.test(t)) { toast('That is not a dataset: hex digits, two per byte'); return; }
    go.disabled = true;
    try {
      await echo.call('POST', '/api/radio', (ctr) => `join ${hex(seal(echo.k, ctr, hmcrypto.unhex(t.toLowerCase())))}`);
      m.dlg.close(); toast('Joining: the Echo finds the network within a minute when one of its routers is in reach');
    } catch (e) { toast(errText(e)); go.disabled = false; }
    tlv.value = '';
    radioRefresh();
  } }, 'Join');
  m.foot.append(h('button', { onclick: () => m.dlg.close() }, 'Cancel'), go);
  m.show();
}

async function radioLeave() {
  const m = modal('Leave the Thread network', 'radio');
  m.body.append(h('p', {}, 'The Echo forgets its Thread network. If it is the network\'s only border router, Home Assistant loses its way to the devices on it until another one takes over.'));
  const go = h('button', { class: 'danger', onclick: async () => {
    go.disabled = true;
    try { await echo.call('POST', '/api/radio', 'leave'); m.dlg.close(); toast('Forgotten'); } catch (e) { toast(errText(e)); go.disabled = false; }
    radioRefresh();
  } }, 'Leave');
  m.foot.append(h('button', { onclick: () => m.dlg.close() }, 'Cancel'), go);
  m.show();
}

async function radioShare() {
  let res;
  try { res = await echo.call('POST', '/api/radio', 'share'); } catch (e) { toast(errText(e)); return; }
  const j = res.json().share;
  if (!j) { toast('The border router gave no code'); radioRefresh(); return; }
  // sealed for this browser (web.c radio_api): opened under the counter of this request and the answer's nonce
  const code = new TextDecoder().decode(seal2(echo.k, res.ctr, unhex(j.sealed), 'hassmic seal back 3', unhex(j.nonce)));
  const m = modal('Share the Thread network', 'radio');
  const left = h('p', {}, '');
  const end = Date.now() + j.seconds * 1000;
  const tick = () => { const s = Math.max(0, Math.round((end - Date.now()) / 1000)); left.textContent = s ? `Open for ${s} s more.` : 'The code has ended.'; if (!s) clearInterval(iv); };
  const iv = setInterval(tick, 1000); tick();
  m.body.append(h('p', {}, 'In your phone\'s app, choose this border router ("hassmic Echo") and enter:'),
    h('p', { class: 'big-code sel' }, `${code.slice(0, 3)} ${code.slice(3, 6)} ${code.slice(6)}`), left);
  m.foot.append(h('button', { onclick: async () => { clearInterval(iv); await radioUnshare(); m.dlg.close(); } }, 'End it now'),
    h('button', { class: 'primary', onclick: () => { clearInterval(iv); m.dlg.close(); } }, 'Done'));
  m.dlg.addEventListener('close', () => clearInterval(iv));
  m.show();
  radioRefresh();
}

async function radioUnshare() {
  try { await echo.call('POST', '/api/radio', 'unshare'); toast('The code has ended'); } catch (e) { toast(errText(e)); }
  radioRefresh();
}

async function radioPermit(sec) {
  try { await echo.call('POST', '/api/radio', `permit ${sec}`); toast(sec ? 'Joining is open for 3 minutes' : 'Joining closed'); } catch (e) { toast(errText(e)); }
  radioRefresh();
}

async function radioZbLeave() {
  const m = modal('Leave the Zigbee network', 'radio');
  m.body.append(h('p', {}, 'The Echo leaves its Zigbee network (the coordinator sees it go) and looks for one that lets it join again. Remove it in ZHA or Zigbee2MQTT too, if it stays listed there.'));
  const go = h('button', { class: 'danger', onclick: async () => {
    go.disabled = true;
    try { await echo.call('POST', '/api/radio', 'zb_leave'); m.dlg.close(); toast('Left: looking for a network again'); } catch (e) { toast(errText(e)); go.disabled = false; }
    radioRefresh();
  } }, 'Leave');
  m.foot.append(h('button', { onclick: () => m.dlg.close() }, 'Cancel'), go);
  m.show();
}
