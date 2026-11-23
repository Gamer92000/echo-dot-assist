// The Plugins page (web.c /api/plugins, plugin.c, docs/plugins.md): programs beside hassmic, each in a sandbox.
// Everything a plugin says about itself (its name, its labels, its help texts) is its author's words: shown as text,
// never as markup, and inside a frame that says whose they are.  A row "Home Assistant password" on a plugin's card
// must not look like this page asking.  The page shows and asks; the Echo decides: where a package comes from,
// whether its signatures hold, whether the action button is needed.
'use strict';

let plg = null, plgBox = null, plgTop = null, plgSig = '', plgBusy = '', plgSwitch = null;
let plgIx = null, plgIxAsked = false;           // what the plugin index lists (/api/plugins/index); a look asked once per page shown
const plgFind = { q: '', runs: true, tier: '', box: null, list: null, head: null, sig: '' };     // the browse page (#/plugins/browse)
const plgShown = () => curPage === 'plugins' || curPage === 'plugins/browse';
const plgOpen = new Set();                      // items whose "Tune" is open
const PLG_PIECE = 200000;                       // web.c takes 256 kB a request

async function getPlugins() {
  plg = (await echo.call('GET', '/api/plugins')).json();
  // the index's list: what the Echo knows, read with the rest; it asks the index only when told to (plgLook)
  try { plgIx = (await echo.call('GET', '/api/plugins/index')).json(); } catch (e) { plgIx = { state: 'none', plugins: [] }; }     // (an Echo on a build before this)
  // The page has the Echo look once when what it knows is over an hour old: on "Browse plugins", which is asking to
  // see the list, and on the Plugins page with plugins switched on, whose cards say where an update is.  A look that
  // failed counts like one that found the list: its reason and its age are shown, and "Look again" is there.  With
  // plugins off the Plugins page asks nothing of the index.  The Echo holds the hour itself, whatever a page asks:
  // "look" is this page by itself, "again" the owner's press, which may ask every 30 s.
  const wants = curPage === 'plugins/browse' || (curPage === 'plugins' && settingValue('plugins'));
  if (!plgIxAsked && wants && plgIx.state !== 'none' && (plgIx.state === 'never' || plgIx.age > 3600)) plgLook(false);
  renderPlugins();
}

// WORD: "relist", a row's "Take its listing" (the index's listing of a version installed here, docs/plugins.md 5.2)
async function plgLook(pressed, word) {
  plgIxAsked = true;
  const relisting = () => (plgIx.plugins || []).some((x) => x.relist && x.relist.state === 'pending');
  let relisted = false;
  try {
    await echo.call('POST', '/api/plugins/index', word || (pressed ? 'again' : 'look'));
    plgIx = (await echo.call('GET', '/api/plugins/index')).json();
    // (the look runs in the Echo's own thread, and after it the index's listings of what is installed here are
    // taken in another: asked again until both are over)
    for (let i = 0; (plgIx.state === 'looking' || relisting()) && i < 120; i++) {
      relisted = relisted || relisting();
      renderPlugins(true);
      await new Promise((res) => setTimeout(res, 500));
      plgIx = (await echo.call('GET', '/api/plugins/index')).json();
    }
    if (relisted || word) plg = (await echo.call('GET', '/api/plugins')).json();       // (the cards' tier, and what runs now)
  } catch (e) { toast(errText(e)); }
  renderPlugins(true);
}

async function plgDo(what, fn) {
  plgBusy = what; renderPlugins(true);
  try { await fn(); } catch (e) { toast(errText(e)); }
  plgBusy = '';
  await getPlugins().catch(() => {}); await load().catch(() => {});
}

// The Plugins page: the switch, then the plugins installed here (each card says when the index has a newer version
// for this hassmic), "Install from file…" and "Browse plugins", a page of its own (#/plugins/browse: buildBrowse)
function buildPlugins(el, list) {
  const sw = (list || []).find((s) => s.name === 'plugins');
  plgSwitch = sw ? settingRow(sw) : null;
  plgTop = h('div', { class: 'card', id: 'plugins-top' });
  plgBox = h('div', { id: 'plugins-list' });
  plgCopyBox = h('div', { class: 'card', id: 'plugins-copy' });
  plgSig = ''; plgIxAsked = false; pcopy.sig = '';
  el.append(plgTop, plgBox, plgCopyBox);
  getPlugins().catch(() => {});
  if (!buildPlugins.timer) buildPlugins.timer = setInterval(() => {
    if (!plgShown() || !echo || document.hidden) return;
    if (!plgBusy) getPlugins().catch(() => {});
    // the other Echos' plugins, for "Copy plugins": two requests each, one of them digests of every file a plugin was
    // given, so only while that page shows and every 20 s
    if (curPage === 'plugins' && !pcopy.busy && Date.now() - pcopy.at > 20000) plgPeers().catch(() => {});
  }, 3000);
}

// ---- installing: upload, the Echo's check, what it may do, the action button

const SANDBOX = {
  complete: ['Sandbox: complete', 'Each plugin runs as a user of its own, under a system call filter, without files or sockets of its own making, and in an SELinux domain of its own.'],
  hassmic: ['Sandbox: without its SELinux wall', 'Each plugin runs as a user of its own, under a system call filter, without files or sockets of its own making. This Echo\'s system install is older than the plugins\' own SELinux domain: if a plugin gets past the filter, SELinux would stop little of what it tries. Running the system install again (scripts/install-system.sh) adds that wall.'],
  su: ['Sandbox: without SELinux at all', 'Each plugin runs as a user of its own, under a system call filter, without files or sockets of its own making. This Echo\'s system install is older than hassmic\'s own SELinux domain: if a plugin gets past the filter, SELinux stops nothing, and hassmic itself runs without that protection too. Running the system install again (scripts/install-system.sh) closes the gap.'],
  none: ['No sandbox on this Echo', 'This Echo\'s kernel cannot hold a plugin in its sandbox, so no plugin runs here.'],
};
// what an update asks for beyond what was accepted (web.c /api/plugins/check "changes"): the Echo's lines, this page's words in front
// ("limit": its memory or threads, which are no powers: shown apart, and the button is never for them; "speaks": a later
// version of a scope it requires that adds only more of what its capability's line says, shown apart too.  A later
// version that lets it do more is "more", and asks)
const PLG_CHANGE = { new: 'New', more: 'More', changed: 'Changed' };
// What it may do: each capability's line (its scope's version 1, in the Echo's words; the microphone's two in bold as
// docs/plugins.md 3.3 words them), and under it every later version it requires that lets it do more than that line
// says, in that version's words (web.c and plugin.c "widens"; finding 5 of a review of 60a8589).  null: nothing
function plgCapList(caps, widens) {
  const l = hmbrowse.capLines(caps, widens);
  return l.length ? h('ul', { class: 'plg-caps' }, l.map((c) => h('li', {}, PLG_BOLD.has(c.cap) ? h('b', {}, c.line) : c.line,
    c.more.length ? h('ul', {}, c.more.map((x) => h('li', {}, x))) : null))) : null;
}
// How the system install writes this model's SELinux policy (web.c /api/plugins "install_method": device.conf INSTALL), and so how the gap
// in the sandbox closes (docs/plugins.md 2.7's table, word for word where it has the words)
function plgFix(install, model) {
  if (install === 'boot') return ['run ', h('code', {}, 'scripts/install-system.sh'), ' again; it flashes the boot image again, which needs the Echo in fastboot as at the first install (', h('code', {}, `devices/${model}/README.md`), ')'];
  if (install === 'cannoli-ubi') return ['build and flash the firmware ZIP again (', h('code', {}, `devices/${model}/README.md`), ')'];
  return ['run the system install again: ', h('code', {}, 'scripts/install-system.sh'), ' (or ', h('code', {}, `scripts/setup.sh ${model} --from install`), ')'];
}
const SANDBOX_LEVEL = { complete: 'complete (the plugins\' own SELinux domain)', hassmic: 'without its SELinux wall (hassmic\'s domain)', su: 'without SELinux at all' };
// The warning where the plugins' own SELinux domain is missing (docs/plugins.md 2.7), before community plugins go on
// from the switch and in the install dialog of each plugin that is not official (plgWallFor): in full, never behind
// an ⓘ, worded by what is missing there and by how that model's policy is written
function plgWallWarning(sandbox, install, model) {
  return callout('warn', h('p', {}, h('b', {}, 'This Echo\'s sandbox is missing one of its walls.')),
    h('p', {}, 'Community plugins are programs by people this project does not know, and nobody has reviewed them.'),
    h('p', {}, h('b', {}, 'Missing here: '), 'the SELinux wall. ', sandbox === 'hassmic' ? 'If a plugin gets past the system call filter, SELinux would stop little of what it tries.'
      : 'This Echo\'s system install is older than hassmic\'s own SELinux domain: if a plugin gets past the system call filter, SELinux stops nothing, and hassmic itself runs without that protection too.'),
    h('p', {}, 'The three levels: complete (plugin domain) / without its SELinux wall (hassmic\'s domain) / without SELinux at all. This Echo: ', h('b', {}, SANDBOX_LEVEL[sandbox] || sandbox), '.'),
    h('p', {}, h('b', {}, 'Still in place: '), 'a user of its own for each plugin, the system call filter (no files, no programs, no sockets of its own making), no file system, memory and thread limits, and only the network destinations you approve.'),
    h('p', {}, h('b', {}, 'To close the gap: '), 'connect the Echo to your PC (USB, or adb over Wi-Fi from this page) and ', ...plgFix(install, model),
      install === 'boot' || install === 'cannoli-ubi' ? '. Then this warning goes away.' : '. Nothing of your settings changes. Then this warning goes away.'));
}
// the sandbox whose missing wall the install dialog of a plugin that is not official names in full (plgDialog), and
// that Echo's install method for the words that close the gap: hmwall.of
function plgWallFor(info, e) { return hmwall.of(info, e === echo ? plg : null); }
// Community plugins (docs/plugins.md 2.7, 3.3): anything the plugin index does not list as official, its community
// listings and packages from a file.  Off by default; a setting as any other (POST /api/set, no press: each one is
// installed with the action button all the same), behind a dialog that warns first.  Off, the Echo installs none
// and stops those installed (kept)
function plgCommunityRow() {
  const on = !!plg.community, sw = h('input', { type: 'checkbox', id: 's-plugins_community', role: 'switch', disabled: !!plgBusy,
    onchange: () => { sw.checked = on; plgCommunity(!on); } });
  sw.checked = on;
  const stopped = plg.plugins.filter((p) => p.community_off).length;
  const more = moreOf('plugins_community', h('p', {}, 'Official plugins are made by this project, reviewed, and listed as official by the plugin index. Everything else is a community plugin: listed by the index without a review, or installed from a file nobody vouches for. Each one still asks for the action button when it is installed, and gets only what you accept then.'),
    h('p', {}, 'Off, the Echo installs no community plugin, whichever way it comes, and stops those it has: they stay installed with their settings, and run again once you switch this on. Copied to another Echo with its other settings, it lets nothing in there by itself: each plugin is still installed with that Echo\'s action button.'));
  return h('div', { class: 'row', id: 'plugins_community' },
    h('div', { class: 'row-t' }, h('div', { class: 'row-label' }, h('label', { for: 's-plugins_community' }, 'Community plugins'), more.btn),
      h('p', { class: 'help' }, on ? 'On: plugins nobody at this project reviewed may be installed and run' : stopped ? `Off: ${plural(stopped, 'community plugin')} installed here ${stopped === 1 ? 'is' : 'are'} stopped` : 'Off: only official plugins are installed and run')),
    h('div', { class: 'ctl' }, h('label', { class: 'switch' }, sw, h('span'))), more.panel);
}

async function plgCommunity(on) {
  const mine = plg.plugins.filter((p) => p.tier !== 'official' && p.here !== false);
  if (!on) {
    const engine = mine.find((p) => settingValue('wake_engine') === `plugin:${p.id}`);
    if (mine.length && !(await ask({ title: 'Switch community plugins off?', ok: 'Switch off', danger: !!engine,
      body: [h('p', {}, `${mine.map((p) => p.name).join(', ')} ${mine.length === 1 ? 'is' : 'are'} stopped and kept, with ${mine.length === 1 ? 'its' : 'their'} settings, until you switch community plugins on again.`),
        engine ? callout('warn', h('p', {}, `${engine.name} is this Echo's wake word engine: while it is stopped the Echo hears no wake word. Choose another engine on the Wake word page first, or keep this on.`)) : null].filter(Boolean) }))) return;
    plgDo('Switching community plugins off…', () => plgCommunitySet('off'));
    return;
  }
  const wall = plg.sandbox === 'hassmic' || plg.sandbox === 'su';
  if (!(await ask({ title: 'Switch community plugins on?', ok: 'Switch on', danger: true,
    body: [wall ? plgWallWarning(plg.sandbox, plg.install_method, echo.hello.codename) : null,
      h('p', {}, 'Community plugins are programs by people this project does not know: the plugin index lists them without a review, and a file from anywhere is one too. The sandbox limits mistakes and makes abuse expensive; it does not make an unknown author\'s code safe. Each still asks for the action button when it is installed, and gets only what you accept then.')].filter(Boolean) }))) return;
  plgDo('Switching community plugins on…', async () => { await plgCommunitySet('on'); toast('Community plugins are on'); });
}
async function plgCommunitySet(v) {
  const r = (await echo.call('POST', '/api/set', `plugins_community=${v}\n`)).json();
  if (r.errors) throw new Error(r.errors.trim());
}
const PLG_BOLD = new Set(['wake', 'request.audio']);     // the install lines docs/plugins.md 3.3 has in bold
const TIER = {
  official: ['Official', 'Made by this project, reviewed, and listed as official by the plugin index, whose key every hassmic carries.'],
  community: ['Community, not reviewed', 'Listed by the plugin index. Built by its author\'s own GitHub CI from the commit the index names, which the index checked (an attestation): that says which commit\'s workflow built it, not what the code does. Nobody at this project has reviewed its code.'],
  sideload: ['From a file, not reviewed', 'Installed from a file the plugin index does not vouch for. Nobody but its author has looked at it.'],
};

async function plgInstall(file) {
  if (!file) return;
  if (file.size > 4 << 20) { toast('A plugin package is at most 4 MB'); return; }
  let info;
  plgBusy = 'Uploading…'; renderPlugins(true);
  try {
    const data = new Uint8Array(await file.arrayBuffer());
    await echo.call('POST', '/api/plugins/upload/begin', String(data.length));
    for (let off = 0; off < data.length; off += PLG_PIECE) await echo.call('POST', `/api/plugins/upload/${off}`, data.subarray(off, off + PLG_PIECE));
    info = (await echo.call('POST', '/api/plugins/check')).json();
  } catch (e) { plgBusy = ''; renderPlugins(true); toast(errText(e)); return; }
  plgBusy = ''; renderPlugins(true);
  plgConfirm(info);
}

// A plugin from the plugin index: the Echo downloads its version for this hassmic and checks it as it checks an upload
// (its size and SHA-256 against the index's signed list first); then the same dialog and the same install.  The Echo
// takes nothing this way but a package with the index key's listing.
async function plgFetch(id) {
  let info = null;
  plgBusy = 'The Echo is fetching it from the plugin index…'; renderPlugins(true);
  try {
    await echo.call('POST', '/api/plugins/fetch', id);
    for (let i = 0; !info && i < 900; i++) {                // as long as the Echo's own download may take
      await new Promise((res) => setTimeout(res, i < 10 ? 300 : 1000));
      const st = (await echo.call('GET', '/api/plugins/fetch')).json();
      if (st.state === 'checked' && st.check) info = st.check;
      else if (st.state !== 'fetching') throw new Error(st.error || 'The Echo did not fetch it');
      if (!plgShown()) { echo.call('POST', '/api/plugins/cancel').catch(() => {}); throw new Error('Not fetched: the Plugins page was left'); }
    }
    if (!info) { echo.call('POST', '/api/plugins/cancel').catch(() => {}); throw new Error('The Echo did not fetch it in time'); }
  } catch (e) { plgBusy = ''; renderPlugins(true); toast(errText(e)); getPlugins().catch(() => {}); return; }
  plgBusy = ''; renderPlugins(true);
  plgConfirm(info);
}

// what an Echo's check said of a package (uploaded, fetched, or carried from another Echo), for the owner to accept or
// not.  e: the Echo it is to be installed on (this one, or another the page is logged in to); list: that Echo's
// plugins; why: the Echo was offered another Echo's voucher for it and wants its own button all the same.  true: go on
function plgDialog(info, e = echo, list = (plg || {}).plugins || [], why = '') {
  // have: a version that hassmic runs is there (a card is also shown for what cannot run: p.here false)
  const [tier, tierHelp] = TIER[info.tier] || TIER.sideload, have = list.find((p) => p.id === info.id && p.here !== false);
  // (a line the Echo adds for another reason than the manifest's, the version it cannot compare, comes with update false)
  const { changes, limits, speaks, more, shownMore } = hmbrowse.changes(info.changes, info.changes_more, info.shown_more);
  const hears = (info.caps || []).some((c) => c.cap === 'request.audio');
  const there = e === echo ? '' : ` on ${e.hello.name}`;
  return ask({ title: `${have ? 'Update' : 'Install'} ${info.name}${there}?`, ok: info.button ? 'Go on' : have ? 'Update' : 'Install', danger: info.tier !== 'official',
    body: [why ? callout('info', h('p', {}, h('b', {}, e.hello.name), act(' wants its own action button for this: ', e.hello), why, '.')) : null,
      h('p', {}, h('b', {}, `Plugin by ${info.author}. `), h('span', { class: 'badge' + (info.tier === 'official' ? '' : ' warn') }, tier), ' ', tierHelp),
      // nobody vouches for a file: the one thing that says whose it is, is the key it is signed with
      info.tier !== 'official' && info.key ? h('p', {}, 'Signed with its author\'s key ', h('b', { class: 'plg-key' }, info.key),
        have ? ', the same as the version installed here.' : '. Compare it with the one the author publishes: a package from anyone else shows another.') : null,
      info.description ? h('p', {}, info.description) : null,
      // where its card's "Source and issues" will lead: the manifest's own word, so shown whole and as text here,
      // where it is accepted with the rest; never a link in this dialog
      info.source ? h('p', {}, 'Its card will link to this address as "Source and issues": ', h('b', { class: 'plg-key' }, info.source),
        /^https:\/\//.test(info.source) ? null : ' (not an https address: the link will lead nowhere)') : null,
      changes.length ? [h('p', {}, h('b', {}, info.update ? 'This version is not what you accepted:' : 'Before you go on:')),
        h('ul', { class: 'plg-caps plg-changes' }, changes.map((c) => h('li', {}, h('b', {}, `${PLG_CHANGE[c.kind] || 'Changed'}: `), c.line)),
          more ? h('li', {}, more) : null)] : null,
      limits.length ? [h('p', {}, h('b', {}, 'Its limits change:')), h('ul', { class: 'plg-caps' }, limits.map((c) => h('li', {}, c.line)))] : null,
      // (the later versions' lines that found no place counted too: the lines that ask come first, so all of these
      // may be left out of a long list)
      speaks.length || shownMore ? [h('p', {}, h('b', {}, 'It speaks a later version of the plugin protocol (nothing it may do beyond what you accepted):')), h('ul', { class: 'plg-caps' }, speaks.map((c) => h('li', {}, c.line)),
        shownMore ? h('li', {}, shownMore) : null)] : null,
      h('p', {}, h('b', {}, changes.length ? 'All it may do then:' : 'What it may do:')),
      // (the microphone's two, in bold as docs/plugins.md 3.3 words them: the one that hears all the time, the one that hears every request)
      plgCapList(info.caps, info.widens) || h('p', {}, 'Nothing beyond running and reading its own settings.'),
      // the strongest warning, for a plugin nobody vouches for that hears every request (3.3), on top of the one for any such plugin
      info.tier !== 'official' && hears ? callout('warn', h('p', {}, h('b', {}, 'It will hear every request: '), 'everything said to this Echo after the wake word or the button, the voices and the room behind them, for as long as it is installed. Nobody at this project has reviewed its code.')) : null,
      info.tier !== 'official' ? callout('warn', h('p', {}, 'A plugin is a program by someone else. The sandbox limits mistakes and makes abuse expensive; it does not make an unknown author\'s code safe. Install it only if you trust who made it.')) : null,
      // and where that Echo's sandbox misses its SELinux wall, 2.7's warning in full: this dialog is where every plugin
      // that is not official is accepted, however community plugins came to be on there (the switch's own dialog, an
      // import, a preset, Compare & copy, "make like this Echo"; a review of 00d492a, 2).  The Echo's own check says
      // its sandbox and policy (an Echo before them: this page's word for itself only)
      ((w) => w ? plgWallWarning(w.sandbox, w.install, e.hello.codename) : null)(plgWallFor(info, e)),
      info.button && info.second ? h('p', {}, act('It hears every request and can send what it hears off the Echo: the Echo asks for its action button twice, the second time on a screen of its own.', e.hello)) : null,
      info.button ? h('p', {}, act(changes.length ? 'Then press its own action button on the Echo: that accepts what changed. Until then the version installed keeps running with what you accepted.'
          : 'Then press its own action button on the Echo: that accepts this list.', e.hello))
        : h('p', {}, 'It is what you accepted before, or less of it: no button needed.')].filter(Boolean) });
}

// the checked package installed on e: at once, or once its action button was pressed.  Throws with why not
// The second press (docs/plugins.md 3.3): a plugin that hears every request and can send what it hears off the Echo is
// accepted with the action button twice, the second time from a screen of its own that says where it can send it.  The
// Echo's rule: it waits for the second press whatever this page shows.  true: ask for it
function plgSecond(name, line, e = echo) {
  return ask({ title: `A second press for ${name}`, ok: 'Ask for the second press', danger: true,
    body: [h('p', {}, h('b', {}, line || 'This plugin hears every request and can send what it hears off this Echo.')),
      h('p', {}, 'That is a recording of whatever is said to this Echo, sent where the plugin chooses within what you accept. Accept it only if you meant to give it exactly that.'),
      // (docs/SECURITY.md, a review of 7be9343: "your network" is no wall; the Echo refuses the name service's ports, not every relay)
      /your network/.test(line || '') ? h('p', {}, 'Your network is not a wall against the internet: a device on it that passes traffic on (a proxy, your router\'s own services) can carry what the plugin sends further.') : null,
      h('p', {}, act('Then press the action button on the Echo once more. Nothing is installed without it.', e.hello))] });
}

async function plgFinish(e = echo, name = 'the plugin') {
  let a = (await e.call('POST', '/api/plugins/install')).json(); const kind = { login: 'A login', adb: 'adb over Wi-Fi', plugin: 'Another plugin', note: 'The note of a copied plugin', anyway: 'A withdrawn plugin to run anyway' };
  let r = a.install;
  // (a voucher stood for the first press: what is left is the second, on its own screen)
  if (r === 'second') {
    if (!(await plgSecond(name, a.second, e))) { e.call('POST', '/api/plugins/cancel').catch(() => {}); throw new Error('Not installed: the second press was not asked for'); }
    a = (await e.call('POST', '/api/plugins/install')).json(); r = a.install;
  }
  // one thing waits for the button at a time, and a press serves that: never sent to press for something else.
  // A login that waits is over within its minute: asked again by itself then (app.js waitNoLogin), the Echo keeps
  // browsers that are not let in back meanwhile
  if (r === 'busy' && a.holds === 'login' && await waitNoLogin(e)) { a = (await e.call('POST', '/api/plugins/install')).json(); r = a.install; }
  if (r === 'busy') throw new Error(`${kind[a.holds] || 'Something else'} waits for the action button of this Echo first. Do not press for this plugin: nothing was asked. Try again in a minute`);
  for (let i = 0; r === 'waiting' && i < 70; i++) {
    await new Promise((res) => setTimeout(res, 1000));
    const st = (await e.call('GET', '/api/plugins')).json().install;
    if (st.state === 'installed') r = 'installed';
    // the first press came: the second, on its own screen, then asked for and waited for as the first
    else if (st.state === 'second') {
      if (!(await plgSecond(name, st.second, e))) { e.call('POST', '/api/plugins/cancel').catch(() => {}); throw new Error('Not installed: the second press was not asked for'); }
      a = (await e.call('POST', '/api/plugins/install')).json(); r = a.install; i = 0;
      if (r === 'busy') throw new Error(`${kind[a.holds] || 'Something else'} waits for the action button of this Echo first. Do not press for this plugin: nothing was asked. Try again in a minute`);
    }
    // (the Echo says at every look what a press would serve: this install, or the wait is over)
    else if (st.state === 'waiting' && st.holds && st.holds !== 'plugin' && st.holds !== 'none') throw new Error('The Echo no longer waits for the button for this plugin: nothing was installed');
    else if (st.state !== 'waiting') throw new Error(st.error || 'The action button was not pressed in time');
  }
  if (r !== 'installed') { e.call('POST', '/api/plugins/cancel').catch(() => {}); throw new Error('The action button was not pressed in time'); }
}

// "Run it anyway" (docs/plugins.md 5.6): a plugin the plugin index withdrew runs again only with this Echo's action
// button, for the list this Echo has; a newer list that names it again stops it again.  One wait for the button as
// every other (web.c W_ANYWAY): never sent to press while something else waits
async function plgAnyway(p) {
  const engine = settingValue('wake_engine') === `plugin:${p.id}`;
  if (!(await ask({ title: `Run ${p.name} anyway?`, ok: 'Ask the Echo for its button', danger: true,
    body: [h('p', {}, 'The plugin index withdrew this version: ', h('b', {}, p.withdrawn), '.'),
      h('p', {}, 'The index withdraws a plugin that does harm or has a flaw, by its own word. If you run it anyway, it runs with what you accepted for it, until the index publishes another list that names it again.'),
      engine ? h('p', {}, 'It is this Echo\'s wake word engine: until it runs, or you choose another engine on the Wake word page, the Echo hears no wake word.') : null,
      h('p', {}, act('Then press the action button on the Echo.', echo.hello))].filter(Boolean) }))) return;
  plgDo('Waiting for the action button on the Echo…', async () => {
    let a = (await echo.call('POST', `/api/plugins/${p.id}/anyway`, 'run')).json();
    if (a.anyway === 'busy' && a.holds === 'login' && await waitNoLogin(echo)) a = (await echo.call('POST', `/api/plugins/${p.id}/anyway`, 'run')).json();
    if (a.anyway === 'busy') throw new Error(`${(WAIT_KIND[a.holds] || 'Something else').replace(/^./, (c) => c.toUpperCase())} waits for the action button of this Echo first. Do not press for this plugin: nothing was asked. Try again in a minute`);
    if (a.anyway === 'running') { toast(`${p.name} is not withdrawn now`); return; }
    if (a.anyway !== 'waiting') throw new Error('The Echo does not have this plugin');
    for (let i = 0; i < 65; i++) {
      await new Promise((res) => setTimeout(res, 1000));
      const all = (await echo.call('GET', '/api/state')).json(), q = ((await echo.call('GET', '/api/plugins')).json().plugins || []).find((x) => x.id === p.id);
      if (q && (q.anyway || !q.withdrawn)) { toast(`${p.name} runs anyway`); return; }
      if (!all.button || all.button.holds !== 'anyway') break;
    }
    echo.call('POST', `/api/plugins/${p.id}/anyway`, 'cancel').catch(() => {});
    throw new Error('The action button was not pressed in time: the plugin stays stopped');
  });
}

async function plgConfirm(info) {
  const have = ((plg || {}).plugins || []).find((p) => p.id === info.id && p.here !== false);
  if (!(await plgDialog(info))) { echo.call('POST', '/api/plugins/cancel').catch(() => {}); return; }
  plgDo(info.button ? 'Waiting for the action button on the Echo…' : 'Installing…', async () => {
    await plgFinish(echo, info.name);
    toast(`${info.name} ${have ? 'updated' : 'installed'}`);
  });
}

// ---- a plugin's settings: the manifest's rows, in the page's own controls

async function plgSet(p, s, value) {
  try { await echo.call('POST', `/api/plugins/${p.id}/set`, `${s.name}=${value}`); toast('Saved'); }
  catch (e) { toast(errText(e)); }
  getPlugins().catch(() => {});
}

function plgSecret(p, s) {
  return ask({ title: s.label, ok: 'Save', input: { label: s.set ? 'A new value (empty: remove it)' : 'Value', value: '', max: 500 } }).then(async (v) => {
    if (v == null || v === false) return;
    try {
      await echo.call('POST', `/api/plugins/${p.id}/secret/${s.name}`, (ctr) => {
        const t = enc.encode(String(v)), b = new Uint8Array(Math.max(64, Math.ceil((t.length + 1) / 64) * 64)); b.set(t);   // padded: its length stays inside
        return hex(seal(echo.k, ctr, b));
      });
      toast(v ? 'Saved. It is never shown again.' : 'Removed');
    } catch (e) { toast(errText(e)); }
    getPlugins().catch(() => {});
  });
}

function plgControl(p, s) {
  const id = `plg-${p.id}-${s.name}`;
  if (s.type === 'bool') {
    const i = h('input', { type: 'checkbox', id, role: 'switch', checked: s.value === '1', onchange: () => plgSet(p, s, i.checked ? '1' : '0') });
    return h('label', { class: 'switch' }, i, h('span'));
  }
  if (s.type === 'select') return h('select', { id, onchange: (e) => plgSet(p, s, e.target.value) }, s.options.map((o) => h('option', { value: o, selected: o === s.value }, o)));
  if (s.type === 'secret') return h('button', { class: 'small', id, onclick: () => plgSecret(p, s) }, s.set ? 'Change' : 'Set');
  const i = h('input', { id, type: s.type === 'int' ? 'number' : 'text', value: s.value, min: s.type === 'int' ? String(s.min) : null, max: s.type === 'int' ? String(s.max) : null,
    maxlength: s.type === 'int' ? null : '500', onchange: () => plgSet(p, s, i.value) });
  return i;
}

// ---- files: what the owner gives a plugin (a wake word plugin's models).  One section per list, all rows alike

const plgItemId = (name) => name.replace(/\.[^.]+$/, '').toLowerCase().replace(/[^a-z0-9_-]+/g, '_').replace(/^[^a-z0-9]+/, '').slice(0, 40);
const plgSuffix = (name) => { const m = /\.[A-Za-z0-9]+$/.exec(name); return m ? m[0].toLowerCase() : ''; };

async function plgUpload(p, s, id, files, e = echo, from = '') {           // files: suffix -> Uint8Array; e: the Echo it goes to; from: 'from-wake' for a copy of a wake word plugin's
  await e.call('POST', `/api/plugins/${p.id}/item/${s.name}/${id}/begin`, from);
  for (const [suffix, data] of Object.entries(files))
    for (let off = 0; off < Math.max(data.length, 1); off += PLG_PIECE) await e.call('POST', `/api/plugins/${p.id}/item/${s.name}/${id}/chunk/${suffix}/${off}`, data.subarray(off, off + PLG_PIECE));
  return (await e.call('POST', `/api/plugins/${p.id}/item/${s.name}/${id}/done`)).json();
}

// files picked: grouped by stem, as the manifest's item is made of (a model and its manifest of the same name)
async function plgFiles(p, s, list) {
  const groups = new Map(), want = s.item_files.map((f) => f.suffix);
  for (const f of list) {
    const suffix = plgSuffix(f.name), id = plgItemId(f.name);
    if (!want.includes(suffix) || !id) { toast(`${f.name}: ${p.name} takes ${nameList(want)} files`); return; }
    const g = groups.get(id) || {}; g[suffix] = f; groups.set(id, g);
  }
  const lacks = [...groups].filter(([, g]) => s.item_files.some((f) => !f.optional && !g[f.suffix])).map(([id]) => id);
  if (lacks.length) { toast(`${nameList(lacks)}: pick ${nameList(s.item_files.filter((f) => !f.optional).map((f) => 'the ' + f.suffix))} file too`); return; }
  const again = [...groups.keys()].filter((id) => s.items.some((it) => it.id === id));
  if (again.length && !(await ask({ title: `Replace ${nameList(again)}?`, ok: 'Replace', body: 'This Echo has it already. What you picked takes its place; what you tuned stays.' }))) return;
  plgDo('Adding…', async () => {
    let waits = 0;
    for (const [id, g] of groups) {
      const data = {};
      for (const [suffix, f] of Object.entries(g)) data[suffix] = new Uint8Array(await f.arrayBuffer());
      if (!(await plgUpload(p, s, id, data)).checked) waits++;
    }
    toast(waits ? `Added. ${p.name} is not running: it looks at ${waits > 1 ? 'them' : 'it'} when it next does` : 'Added');
  });
}

// one of the manifest's catalog: this browser fetches it, the Echo gets it from here (the Echo fetches nothing)
function plgCatalog(p, s, c) {
  plgDo(`Fetching ${c.title}…`, async () => {
    const data = {};
    for (const f of s.item_files) {
      let r;
      try { r = await fetch(s.catalog.base + (c.path || c.id) + f.suffix); } catch (e) { throw new Error(`Cannot fetch ${c.title} (this browser needs the internet)`); }
      if (r.ok) data[f.suffix] = new Uint8Array(await r.arrayBuffer());
      else if (!f.optional) throw new Error(`Cannot fetch ${c.title}: ${r.status}`);
    }
    await plgUpload(p, s, plgItemId(c.id), data);
    toast(`${c.title} added`);
  });
}

function plgItemRow(p, s, it) {
  const key = `${p.id}/${s.name}/${it.id}`, open = plgOpen.has(key);
  const save = async () => {
    for (const f of it.files) {
      const b = (await echo.call('GET', `/api/plugins/${p.id}/file/${s.name}/${it.id}/${f.suffix}`)).bytes;
      const a = h('a', { href: URL.createObjectURL(new Blob([b])), download: it.id + f.suffix }); a.click(); URL.revokeObjectURL(a.href);
    }
  };
  const field = (f) => {
    const i = h('input', { type: f.type === 'text' ? 'text' : 'number', value: it.fields[f.name] == null ? '' : it.fields[f.name], min: f.type === 'text' ? null : String(f.min),
      max: f.type === 'text' ? null : String(f.max), step: f.type === 'number' ? String(f.step || 'any') : null, maxlength: f.type === 'text' ? '250' : null,
      onchange: async () => {
        try { await echo.call('POST', `/api/plugins/${p.id}/item/${s.name}/${it.id}/field`, `${f.name}=${i.value}`); toast('Saved'); } catch (e) { toast(errText(e)); }
        getPlugins().catch(() => {});
      } });
    return h('label', { class: 'plg-field' }, h('span', {}, f.label), i);
  };
  return h('div', { class: 'row info' },
    h('div', { class: 'row-t' }, h('div', { class: 'row-label' }, h('span', {}, it.title || it.id),
        it.bad ? h('span', { class: 'badge warn' }, 'Cannot be used') : !it.checked ? h('span', { class: 'badge' }, 'Not checked yet') : null),
      h('p', { class: 'help' }, `${it.id} · ${it.files.map((f) => `${f.suffix} ${Math.ceil(f.size / 1024)} kB`).join(', ')}`)),
    h('div', { class: 'ctl btns' },
      s.fields.length ? h('button', { class: 'small', 'aria-expanded': String(open), onclick: () => { if (open) plgOpen.delete(key); else plgOpen.add(key); renderPlugins(true); } }, 'Tune') : null,
      h('button', { class: 'small', onclick: () => save().catch((e) => toast(errText(e))) }, 'Save file'),
      h('button', { class: 'small danger', onclick: async () => {
        if (!(await ask({ title: `Delete ${it.title || it.id}?`, ok: 'Delete', danger: true }))) return;
        plgDo('Deleting…', () => echo.call('POST', `/api/plugins/${p.id}/item/${s.name}/${it.id}/delete`));
      } }, 'Delete')),
    open ? h('div', { class: 'row-more plg-fields' }, s.fields.map(field)) : null);
}

function plgFilesSection(p, s) {
  const pick = h('input', { type: 'file', multiple: true, class: 'hidden', accept: s.item_files.map((f) => f.suffix).join(','), onchange: () => { plgFiles(p, s, [...pick.files]); pick.value = ''; } });
  const cat = s.catalog && Array.isArray(s.catalog.items) ? s.catalog : null;
  return [h('div', { class: 'sub-head' }, s.label),
    s.help ? h('p', { class: 'help plg-help' }, s.help) : null,
    s.items.length ? s.items.map((it) => plgItemRow(p, s, it)) : h('p', { class: 'help plg-help' }, 'None yet.'),
    h('div', { class: 'card-foot' }, pick,
      h('button', { class: 'small', onclick: () => pick.click() }, `Add files (${s.item_files.map((f) => f.suffix + (f.optional ? ', optional' : '')).join(' + ')})`),
      cat ? h('span', { class: 'help' }, ` ${cat.label || 'Or fetch'}: `) : null,
      cat ? cat.items.filter((c) => c && c.id && !s.items.some((it) => it.id === plgItemId(String(c.id)))).map((c) =>
        h('button', { class: 'small', onclick: () => plgCatalog(p, s, { id: String(c.id), title: String(c.title || c.id), path: c.path ? String(c.path) : '' }) }, String(c.title || c.id))) : null)];
}

// ---- what a plugin does with storage, the log, its server, music, its ports, Home Assistant: rows of the card, hassmic's words

const plgSecsAgo = (s) => s < 90 ? `${s} s ago` : s < 5400 ? `${Math.round(s / 60)} min ago` : `${Math.round(s / 3600)} h ago`;
const plgKB = (b) => b < 1024 ? `${b} bytes` : `${(b / 1024).toFixed(b < 10240 ? 1 : 0)} kB`;
function plgUses(p) {
  const has = (c) => (p.caps || []).some((x) => x.cap === c), rows = [];
  if (has('storage')) rows.push(infoRow('Its data', `${plgKB(p.storage_used || 0)} of ${p.storage_kb} kB${p.storage_keys ? `, ${p.storage_keys} ${p.storage_keys === 1 ? 'entry' : 'entries'}` : ''}`,
    'What the plugin keeps on this Echo for itself (a place in the log it forwarded, say). It goes when the plugin is removed, and stays when it is updated. The Echo takes ten writes of it, then one a minute: its flash would wear out.', null));
  if (has('log.read')) rows.push(infoRow('Reads the log', p.log ? `Following it now: ${p.log_lines} ${p.log_lines === 1 ? 'line' : 'lines'} given to it since it started${p.log_unanswered ? '. Does not answer LOG (no OK in 10 s): built on an SDK before flow control? No more of the log goes to it until it does' : ''}` : 'Not following it now',
    'This Echo\'s technical log as the plugin gets it: never a line with what was said to the Echo or what it answered, nor one with the name of a network, a Bluetooth device or a timer (the log marks those where they are written); MAC and Bluetooth addresses blanked; of what the system itself writes only its updates, resets and the firewall\'s alarms; nothing of a wake word plugin\'s. The settings page\'s own log shows you all of it.', null));
  if (has('net.server')) rows.push(infoRow('Its server', p.net_ok >= 0 && (p.net_fail < 0 || p.net_ok <= p.net_fail) ? `Connected ${plgSecsAgo(p.net_ok)}` : p.net_fail >= 0 ? `Could not connect, ${plgSecsAgo(p.net_fail)}: the log says why` : 'Not connected yet',
    'It connects only to the server you enter in its settings, at the ports its install named: hassmic makes the connection and hands it over, and refuses any other address, this Echo itself and the ports of its own services. A server outside your network is reached only if you enter one.', null));
  // ---- music, the network of its own, proposals, Home Assistant: hassmic's words, the plugin's values as text
  if (has('audio.music')) rows.push(infoRow('Plays music', p.music === 'playing' ? `Playing${p.title ? `: ${p.title}${p.artist ? ` (${p.artist})` : ''}` : ''}` : p.music === 'paused' ? 'Paused' : 'Not playing now',
    'It plays on this Echo\'s speaker as a music source beside Sendspin and a phone over Bluetooth: the newest one plays and pauses the others. The action button pauses and resumes it, "<wake word>, stop" pauses it, the voice assistant turns it down while it listens and answers, and Home Assistant\'s media player pauses, resumes and stops it.', null));
  // ---- its calls (hassmic shows and ends them as a Drop In; this row says what the plugin's line does now)
  if (has('call')) rows.push(infoRow('Calls', p.call === 'ringing' ? 'Ringing here now' : p.call === 'answered' ? 'Answered here, connecting' : p.call === 'calling' ? 'Calling out now' : p.call === 'live' ? 'In a call now' : 'No call now',
    'It makes and takes calls through this Echo. A call coming in rings here as a Drop In does, and only a person at the Echo answers it: the action button or the screen. Home Assistant and this page cannot answer it, and nothing answers by itself. A call goes out only when Home Assistant (an automation too) or a button here asks for one through an action of the plugin\'s marked as placing calls, and it shows on the light (and the screen) the whole time. It hears the room only while a call is connected. The action button, "<wake word>, stop" or "hang up" end it. Do not disturb refuses its calls, and three rings nobody answered in ten minutes stop it ringing for a while.', null));
  // ---- a copy of every request (request.audio), the texts (request.text)
  if (has('request.audio')) rows.push(infoRow('Hears every request', p.requests && p.requests.now ? 'Hearing one now'
    : `${p.requests && p.requests.runs ? `${plural(p.requests.runs, 'request')} since hassmic started, the last ${plgSecsAgo(p.requests.last_s)}${p.requests.lost ? `; it did not take ${(p.requests.lost / 16000).toFixed(1)} s of them in time` : ''}` : 'None since hassmic started'}`,
    `It gets a copy of what Home Assistant is sent of every request said to this Echo, from the wake word or the button to the end, the voices and the room behind them: as long as the Echo listens to a request, and never at other times (with the wake word in Home Assistant, from Home Assistant's wake word on). It cannot change, delay or end a request, or answer one: Home Assistant answers. The light that shows the Echo listening shows it, unless its brightness is turned down to nothing.${p.second ? ` ${p.second}` : ''}`, null));
  if (has('request.text')) rows.push(infoRow('Reads what was said', 'What speech to text understood and what the assistant answered',
    'Each request\'s words as Home Assistant understood them and its answer, as text (with Wyoming the words only), whether or not Home Assistant shows them as "Last heard" and "Last reply". A "stop" that the Echo acts on itself is not passed on.', null));
  if (has('net.udp') || has('net.discover')) rows.push(infoRow('Its ports', (p.ports || []).length ? `UDP ${(p.ports || []).join(', ')}` : 'None open now',
    'Ports of its own on this Echo, the same at every start. Only devices on your network reach them: the Echo\'s firewall and the socket itself drop what comes from anywhere else, and it sends only to devices on your network.', null));
  for (const pr of p.proposals || []) rows.push(infoRow(`Found ${pr.value}`, `For "${pr.label}". Use it?`,
    'The plugin found this on your network. Nothing is written until you say so here: what a plugin connects to is yours to choose.',
    h('span', {}, h('button', { class: 'small', disabled: !!plgBusy, onclick: () => plgDo('Writing…', () => echo.call('POST', `/api/plugins/${p.id}/propose`, `take ${pr.setting} ${pr.value}`)) }, 'Use it'), ' ',
      h('button', { class: 'small', disabled: !!plgBusy, onclick: () => plgDo('…', () => echo.call('POST', `/api/plugins/${p.id}/propose`, `drop ${pr.setting} ${pr.value}`)) }, 'No'))));
  // Home Assistant's commands and actions come over its keyed link only (proto_esphome.c plugin_command): before it set
  // the Echo's key (plg.ha_key 0; -1 under Wyoming, where there are none) they are refused, which only the log said
  if (plg && plg.ha_key === 0 && ((has('ha.entities') && (p.entities || []).some((e) => ['switch', 'button', 'number', 'select'].includes(e.kind))) || has('ha.actions')))
    rows.push(infoRow('Home Assistant\'s commands', 'Not taken yet: Home Assistant has not given this Echo its encryption key',
      'Only Home Assistant\'s own encrypted connection may set this plugin\'s switches, numbers and choices, press its buttons or call its actions: a plugin\'s entity may open a door. Home Assistant gives the Echo its key as it adopts it; until it has, what it sets is refused, and its states still show there.', null));
  // (an entity Home Assistant does not list: its key could not be kept on the flash, plugin.c plugin_json "unlisted").
  // One the card shows as a row of the plugin's own (plgRows) is not said twice (hmrows.listed)
  for (const e of hmrows.listed(p)) rows.push(infoRow(`In Home Assistant: ${e.label}`, e.unlisted === 'nokey' ? 'Not listed in Home Assistant yet: this Echo could not write its key down (the log says why)'
    : e.value !== undefined ? String(e.value) : (p.state === 'running' ? 'Nothing said yet' : 'Unavailable while it does not run'), null, null));
  for (const a of (p.actions || []).filter((x) => x.page)) rows.push(infoRow(a.label, 'One of its actions, which Home Assistant can call too', null,
    h('button', { class: 'small', disabled: !!plgBusy || p.state !== 'running', onclick: () => plgDo('…', () => echo.call('POST', `/api/plugins/${p.id}/action`, a.name)) }, 'Run')));
  return rows;
}

// the development flag: for whoever writes plugins, on this Echo only, never copied
function plgDevelop(p) {
  // an older launcher (plugrun before version 3) starts every plugin without the flag: shown as not available there
  if (plg && plg.develop_flag === false) return infoRow('Development flag', 'Not available: this Echo\'s plugin launcher is an older one, which starts plugins without it', null);
  const i = h('input', { type: 'checkbox', role: 'switch', checked: !!p.develop, onchange: () => plgDo(i.checked ? 'Starting again with the flag…' : 'Starting again without it…', () => echo.call('POST', `/api/plugins/${p.id}/develop`, i.checked ? 'on' : 'off')) });
  return infoRow('Development flag', p.develop ? 'On: a system call the sandbox refuses stops the plugin' : 'Off',
    'For whoever writes plugins. With it on, a system call the sandbox does not allow stops the plugin at once (SIGSYS), and the log names the call (the SDK\'s client writes it), instead of answering "not permitted" and leaving the plugin to cope. It allows nothing more. The plugin starts again for it.',
    h('label', { class: 'switch' }, i, h('span')));
}

// ---- rows of the plugin's own: its entities marked "page", their states as Home Assistant has them, under
// the labels its manifest gives (accepted with it).  Read-only: nothing on them sets anything.  plugin_json carries
// them only while the plugin runs and Home Assistant lists the entity (its key kept).  How a state reads, as a pure function
// (tests/unit/web_rows_test.py, node): every value is a text the Echo checked against the entity's kind (plugin.c
// ent_value_ok), shown as text, never as markup (h() makes text nodes)
const hmrows = (() => {
  const unit = (r, t) => (r.unit ? `${t} ${r.unit}` : t);
  return {
    // the entities plgUses lists: all but those shown as a row; one Home Assistant does not list ("unlisted") always,
    // with its notice, whatever rows say (plugin.c gives such an entity no row: a row is a state Home Assistant has)
    listed(p) {
      return (p.entities || []).filter((x) => x.unlisted || !(p.rows || []).some((r) => r.name === x.name));
    },
    text(r) {
      if (r.value === undefined || r.value === null) return 'Nothing said yet';
      const v = String(r.value), num = Number(v);
      switch (r.kind) {
        case 'binary_sensor': case 'switch': return v === 'on' ? 'On' : v === 'off' ? 'Off' : v;
        // a sensor as Home Assistant shows it: its decimals (0 to 6), its unit
        case 'sensor': return v !== '' && Number.isFinite(num) ? unit(r, num.toFixed(Math.min(6, Math.max(0, r.decimals | 0)))) : v;
        // a number as it is, without the digits a float carries (0.30000000000000004), and its unit
        case 'number': return v !== '' && Number.isFinite(num) ? unit(r, String(Number(num.toPrecision(12)))) : v;
        default: return v;            // a select's option, a text sensor's text
      }
    },
  };
})();
function plgRows(p) {
  return (p.rows || []).length ? [h('div', { class: 'sub-head' }, 'What it shows'), p.rows.map((r) => infoRow(String(r.label), hmrows.text(r), null, null))] : null;
}

// ---- a plugin's card: framed and named as not this page's own

const PLG_STATE = { running: 'Running', starting: 'Starting…', stopping: 'Stopping…', restarting: 'Stopped: started again shortly', waiting: 'Waits for a place', crashed: 'Switched off after crashing five times', stopped: 'Not running' };

// the index's entry for a plugin, and the version of it this hassmic would install when that is newer than what is
// here (a card whose manifest this build does not read says the version its listing names, else 0; one for a newer
// hassmic says its own)
const plgIndexed = (id) => ((plgIx || {}).plugins || []).find((x) => x.id === id);
function plgNewer(p) {
  const x = plgIndexed(p.id);
  return x && x.here && x.here.version > p.version ? x.here : null;
}

function plgCard(p) {
  const [tier, tierHelp] = TIER[p.tier] || TIER.sideload, engine = settingValue('wake_engine') === `plugin:${p.id}`, newer = plgNewer(p);
  const rows = p.settings.filter((s) => s.type !== 'files').map((s) => infoRow(s.label, s.type === 'secret' ? (s.set ? 'Set. Never shown again.' : /^synced\b/.test(p.how || '') ? 'Not set here. Secrets are not copied between Echos: set it on this one.' : 'Not set here.') : (s.unit || ''), s.help || null, plgControl(p, s)));
  return h('div', { class: 'card plugin', id: `plugin-${p.id}` },
    h('div', { class: 'plg-head' },
      // the version is the Echo's number; what its author calls it is a label beside that, never in its place
      h('div', {}, h('div', { class: 'plg-name' }, p.name, ' ', p.version ? h('span', { class: 'help' }, `version ${p.version}${p.version_name ? ` (${p.version_name})` : ''}`) : null),
        h('div', { class: 'help' }, `Plugin by ${p.author}. Its words below are its author's, not this page's. `, h('span', { class: 'badge' + (p.tier === 'official' ? '' : ' warn'), title: tierHelp }, tier),
          newer ? [' ', h('span', { class: 'badge', title: 'The plugin index has a newer version for this hassmic' }, 'Update')] : null)),
      p.here === false ? null : h('label', { class: 'switch', title: p.enabled ? 'Switch it off' : 'Switch it on' },
        h('input', { type: 'checkbox', role: 'switch', checked: p.enabled, onchange: (e) => plgDo(e.target.checked ? 'Starting…' : 'Stopping…', () => echo.call('POST', `/api/plugins/${p.id}/enable`, e.target.checked ? 'on' : 'off')) }), h('span'))),
    p.description ? h('p', { class: 'help plg-help' }, p.description) : null,
    plgRows(p),
    infoRow(PLG_STATE[p.state] || p.state, p.detail || (p.state === 'running' ? `Up to ${p.memory_mb} MB of memory` : ''), null, null),
    // withdrawn by the plugin index (5.6): stopped and kept; removing it or running it anyway is the owner's
    p.withdrawn && !p.anyway ? callout('warn', h('p', {}, `The plugin index withdrew this version: ${p.withdrawn}. It is stopped and kept. Remove it, update it when the index has a version it does not withdraw, or run it anyway with the action button.`),
      h('button', { class: 'small danger', disabled: !!plgBusy, onclick: () => plgAnyway(p) }, 'Run it anyway…')) : null,
    p.withdrawn && p.anyway ? callout('warn', h('p', {}, `The plugin index withdrew this version (${p.withdrawn}), and you said to run it anyway. A newer list from the index that names it again stops it again.`)) : null,
    p.here !== false && !p.accepted ? callout('warn', h('p', {}, 'What this version may do was not accepted yet: install its package again to see the list and accept it.')) : null,
    // installed by a newer hassmic than this one (a fall back): kept, and it runs again under that one
    p.newer ? callout('info', h('p', {}, 'It needs a newer hassmic than this one: it was installed under a later version, and this Echo runs an older one now (an update that did not stay, or the factory copy). It is kept with its settings and files, and runs again once the Echo runs a hassmic that has what it needs.')) : null,
    // (p.wake also stands while a wipe of what it was given as one is due: the capability says what it is)
    p.caps.some((c) => c.cap === 'wake') ? infoRow(engine ? 'This Echo\'s wake word engine' : 'A wake word engine', engine ? (p.listening ? 'Listening for the wake word Home Assistant picked.' : 'Chosen as the engine, and not listening now.')
      : 'Not in use. Choose it on the Wake word page to have it listen.', null, engine ? null : h('a', { class: 'small', href: '#/wake/wake_engine' }, 'Wake word page')) : null,
    h('div', { class: 'sub-head' }, 'What it may do'),
    plgCapList(p.caps, p.widens) || h('p', { class: 'help plg-help' }, 'Nothing beyond running and reading its own settings.'),
    plgUses(p),
    rows.length ? [h('div', { class: 'sub-head' }, 'Settings'), rows] : null,
    p.settings.filter((s) => s.type === 'files').map((s) => plgFilesSection(p, s)),
    p.here === false ? null : plgDevelop(p),
    h('div', { class: 'card-foot' },
      // the manifest's address, accepted with the install (its dialog shows it whole): a new tab that gets neither
      // this page's window nor its address
      p.source ? h('a', { class: 'small', href: /^https:\/\//.test(p.source) ? p.source : '#', target: '_blank', rel: 'noopener noreferrer', title: p.source }, 'Source and issues') : null,
      h('span', { class: 'help' }, p.license ? ` Licence: ${p.license} ` : ' '),
      // the version the index has for this hassmic: fetched by the Echo, checked, then the same dialog as any install
      // (the button only for what was not accepted before)
      newer ? h('button', { class: 'small primary', disabled: !!plgBusy, onclick: () => plgFetch(p.id) }, p.here === false ? `Install version ${newer.version_name || newer.version} for this hassmic` : `Update to version ${newer.version_name || newer.version}`) : null,
      h('button', { class: 'small danger', onclick: async () => {
        if (!(await ask({ title: `Remove ${p.name}?`, ok: 'Remove', danger: true, body: `The plugin goes, with its settings and the files you gave it.${engine ? ' It is this Echo\'s wake word engine: Amazon\'s wake word listens until it is installed again or you choose another engine.' : ''}` }))) return;
        plgDo('Removing…', () => echo.call('POST', `/api/plugins/${p.id}/remove`, 'remove'));
      } }, 'Remove')));
}

// ---- the plugin index (plugin_fetch.c): browsed on a page of its own, one list of rows all alike

// What the browse page shows of the index's list, as pure functions (tests/unit/web_browse_test.py, node): which rows
// a search, the "runs on this Echo" switch and the tier keep, what a row offers, and why one does not run here.
// x: an entry of /api/plugins/index (plugin_fetch.c pf_json); mine: /api/plugins' plugins
// Pure (tests/unit/web_wall_test.py): whether the install dialog of a package shows 2.7's warning in full, and its
// words.  info: the installing Echo's check (web.c check_json: tier, "sandbox", "install_method"); mine: this Echo's
// /api/plugins where the dialog is for this Echo (an Echo whose check is older than those two fields: its word for
// itself), else null.  { sandbox, install } where the plugin is not official and the sandbox misses its SELinux wall
// ("hassmic": the plugins' own domain missing; "su": SELinux missing), else null
const hmwall = {
  of(info, mine) {
    const sb = (info && info.sandbox) || (mine && mine.sandbox) || '';
    if (!info || info.tier === 'official' || (sb !== 'hassmic' && sb !== 'su')) return null;
    return { sandbox: sb, install: (info.sandbox ? info.install_method : mine && mine.install_method) || '' };
  },
};

const hmbrowse = (() => {
  // what a capability is about (the manifest's names are the Echo's; these are the words people type)
  const CAP_WORDS = { storage: 'keeps data storage', files: 'files models you give it', state: 'state of the echo', 'log.read': 'log', 'log.read.full': 'whole log',
    'net.server': 'network server port', 'net.lan': 'network local lan', 'net.internet': 'internet network cloud', 'net.udp': 'network udp', 'net.discover': 'network discovery mdns',
    'audio.music': 'music player audio', 'audio.volume': 'volume', call: 'calls telephone sip', wake: 'wake word engine microphone listens',
    'request.audio': 'microphone requests audio', 'request.text': 'requests text transcript', 'ha.entities': 'home assistant entities', 'ha.actions': 'home assistant actions' };
  const words = (list) => list.length < 2 ? list.join('') : `${list.slice(0, -1).join(', ')} and ${list[list.length - 1]}`;
  // the broad capabilities the index flags on a community version (index.json's "broad", docs/plugins.md 5.3), in
  // the words the row says them in
  const BROAD_WORDS = { 'net.lan': 'connects to any device on your network', 'net.internet': 'connects to the internet', call: 'makes and takes calls',
    'request.audio': 'hears every request said to this Echo', 'request.text': 'reads every request and reply', 'log.read.full': 'reads the whole log' };
  // what the search looks in: what the plugin says of itself, and what it may do in names and in people's words
  // offer's word for X with HAVE, the plugin as installed here (or none)
  const offered = (x, have) => !x.here ? '' : !have ? 'install' : x.here.version > have.version ? 'update' : have.newer ? 'newer' : have.here === false ? 'unread' : 'installed';
  const hay = (x) => [x.name, x.author, x.description, x.id, x.tier, ...((x.here || x.top || {}).caps || []).map((c) => `${c} ${CAP_WORDS[c] || ''}`)].join(' ').toLowerCase();
  return {
    // the rows to show, in the index's order, how many more match but do not run here (RUNS on), and how many match
    // and are community plugins, left out while this Echo has those switched off (COMMUNITY false: it installs none)
    find(all, q, runs, tier, community = true) {
      const w = String(q || '').toLowerCase().split(/\s+/).filter(Boolean);
      const matched = (all || []).filter((x) => (!tier || x.tier === tier) && w.every((t) => hay(x).includes(t)));
      const found = community ? matched : matched.filter((x) => x.tier === 'official');
      const shown = found.filter((x) => !runs || x.here);
      return { shown, hidden: found.length - shown.length, withheld: matched.length - found.length };
    },
    // what the row's button is: "install", "update", "installed" (the same or newer here), "newer" (installed under a
    // newer hassmic than this one, a fall back, and the index has nothing newer for this one: Update would fetch an
    // older version, which the Echo refuses as not newer, finding 2 of a review of 4e0a202), "unread" (installed with a
    // manifest this build does not read otherwise, and the index has nothing newer than the version its listing
    // names: Update would fetch what the Echo refuses as not newer, finding 7 of a review of 60a8589), or "" (nothing
    // runs here).  Such a card says the version its listing names, 0 where none does: then any version is an update
    offer(x, mine) { return offered(x, (mine || []).find((p) => p.id === x.id)); },
    // why a plugin the index lists does not run here, in words.  A version's "needs": what of its requires this
    // hassmic lacks, a scope and the version it needs ("button 2"), as the Echo says them (plugin_fetch.c)
    whyNot(x) {
      const t = x.top;
      if (!t) return 'No released version yet.';
      if ((t.needs || []).length) return `Needs a newer hassmic: it speaks ${words(t.needs)}, which this one does not yet.`;
      if (t.unknown) return 'Needs a newer hassmic: it asks for something this one does not know.';
      return 'Not one this hassmic installs (a version under the least it runs, or a package over 4 MB).';
    },
    // an update's lines as the Echo's check gives them (web.c "changes"), in the dialog's three lists: what is not as
    // accepted (the button), its limits, and later versions of the protocol it speaks within what was accepted
    // ("limit" and "speaks" ask for nothing); and what of them found no place in the check's list, counted (web.c
    // "changes_more" that ask, "shown_more" that only show: finding 3 of a review of 61e3d8e), as the line that ends
    // each list ("And 9 more."), or null
    changes(all, more, shownMore) {
      const a = all || [], n = (x) => (Number.isInteger(x) && x > 0 ? `And ${x} more.` : null);
      return { changes: a.filter((c) => c.kind !== 'limit' && c.kind !== 'speaks'), limits: a.filter((c) => c.kind === 'limit'), speaks: a.filter((c) => c.kind === 'speaks'),
        more: n(more), shownMore: n(shownMore) };
    },
    // the capabilities' lines (web.c "caps") with the words of every later version required that widens what each lets
    // a plugin do (web.c "widens"), each under its capability's line ("From net.lan 2: ..."); core's, no capability's,
    // as lines of their own after them: [{ cap, line, more: [words] }]
    capLines(caps, widens) {
      const c = caps || [], w = widens || [], of = (x) => `From ${x.scope} ${x.version}: ${x.words}`;
      return [...c.map((x) => ({ cap: x.cap, line: x.line, more: w.filter((v) => v.scope === x.cap).map(of) })),
        ...w.filter((v) => !c.some((x) => x.cap === v.scope)).map((v) => ({ cap: v.scope, line: `Speaks ${v.scope} ${v.version}, which lets a plugin do more: ${v.words}`, more: [] }))];
    },
    // what a version requires, in words: "core 1, button 1" (a version's "requires" object)
    requires(v) { return Object.entries((v && v.requires) || {}).map(([s, n]) => `${s} ${n}`).join(', '); },
    // the version shown's broad capabilities as the index flags them, in words ('' for none), and whether the Echo
    // asks for its button twice for it (every request heard, and a way out: docs/plugins.md 3.3's second press)
    // HAVE: the plugin as installed here (/api/plugins), or none.  The Echo's rule (plugin_store.c
    // ps_stage_install): the second press wherever the button is asked for and the version hears every request
    // and has a way out.  A first install always asks for the button: TWICE.  An update asks for it where it is
    // wider than what was accepted, which only the check says: one that adds the pair asks for sure; one of a
    // plugin whose accepted version has the pair already asks twice where it asks at all, MAY (it was not said to
    // ask twice, and then asked: a review of 3c5d7ff, R9).  Installed as a card this build does not read (have.here
    // false: a manifest for a newer hassmic, or none it reads): the check holds the update to an accepted manifest
    // /api/plugins does not show, and it may ask for nothing: MAY (it said TWICE for sure: a review of 3fcd319, S7).
    // Nothing offered (installed, none for this hassmic): nothing asked
    broad(x, have) {
      const v = x.here || x.top || {}, b = (v.broad || []).filter((c) => BROAD_WORDS[c]), caps = v.caps || [];
      const both = (cs) => cs.includes('request.audio') && cs.some((c) => c.startsWith('net.'));
      const had = have && have.here !== false ? (have.caps || []).map((c) => (typeof c === 'string' ? c : c.cap)) : [];
      const what = offered(x, have);
      const twice = both(caps) && (what === 'install' || what === 'update');
      return { words: words(b.map((c) => BROAD_WORDS[c])), twice, may: twice && what === 'update' && (have.here === false || both(had)) };
    },
  };
})();
if (typeof module !== 'undefined') module.exports = Object.assign({ hmrows, hmwall }, hmbrowse);
// the plugin index's list of withdrawn plugins this Echo has (plugin_index.c): its date (its serial is the time of
// its last change) and how long ago the Echo last had word that it is the newest.  The network can keep it away,
// never change it (docs/SECURITY.md): so its age is shown, and a week without word is a warning
const plgDate = (t) => t > 1000000000 ? new Date(t * 1000).toLocaleDateString() : `serial ${t}`;
function plgAgo(s) { const m = Math.round(s / 60); return m < 1 ? 'just now' : m < 120 ? `${m} min ago` : m < 48 * 60 ? `${Math.round(m / 60)} hours ago` : `${Math.round(m / 1440)} days ago`; }
function plgRevokedRow(r) {
  const more = moreOf('plg-revoked', h('p', {}, 'The plugin index publishes a signed list of the plugins it withdrew (for a flaw, or for doing harm). This Echo stops what it names and refuses to install it. While plugins are on and one is installed, the Echo asks the index for that list by itself once a day, at a random time, and nothing else: the request names no plugin and no version, GitHub Pages sees the Echo\'s address. It also takes the list from the other Echos of its network and hands its own on, so an Echo without internet learns of it too: a newer list from them counts as word, the same list does not (only the index can say there is no newer one). With plugins off it asks nothing.'));
  return h('div', { class: 'row info' }, h('div', { class: 'row-t' }, h('div', { class: 'row-label' }, h('span', {}, 'Withdrawn plugins'), more.btn),
    h('p', { class: 'help' }, r.serial < 0 ? 'This Echo has not had the plugin index\'s list yet.' : `The list of ${plgDate(r.serial)}${r.least > r.serial ? ', older than this hassmic wants' : ''}${r.age_s >= 0 ? `, known to be the newest ${plgAgo(r.age_s)}` : ', not known when it was last the newest'}.`)), h('div', { class: 'ctl' }), more.panel);
}
const plgMins = (o) => { const m = o.age >= 0 ? Math.round(o.age / 60) : -1; return m < 0 ? '' : m < 1 ? 'just now' : m === 1 ? 'a minute ago' : m < 120 ? `${m} minutes ago` : `${Math.round(m / 60)} hours ago`; };

function buildBrowse(el) {
  const q = h('input', { type: 'search', id: 'plugins-q', placeholder: 'Name, author, what it does', 'aria-label': 'Search plugins', oninput: () => { plgFind.q = q.value; renderBrowse(true); } });
  const runs = h('input', { type: 'checkbox', role: 'switch', checked: plgFind.runs, onchange: () => { plgFind.runs = runs.checked; renderBrowse(true); } });
  const tier = h('select', { 'aria-label': 'Tier', onchange: () => { plgFind.tier = tier.value; renderBrowse(true); } },
    h('option', { value: '' }, 'All'), h('option', { value: 'official' }, 'Official'), h('option', { value: 'community' }, 'Community'));
  plgFind.foot = h('div', { class: 'card-foot' });
  plgFind.say = h('div');
  plgFind.list = h('div', { class: 'card', id: 'plugins-browse' });
  plgFind.sig = '';
  el.append(h('p', { class: 'help plg-back' }, h('a', { href: '#/plugins' }, '← Plugins installed on this Echo')),
    h('div', { class: 'card', id: 'plugins-find' },
      infoRow('Search', 'In the name, the author, what it says of itself and what it may do', null, q),
      infoRow('Only what runs on this Echo', 'Needing nothing this hassmic lacks', 'A plugin names the least version of each part of the plugin protocol it speaks (its scopes: one per capability, and core). A hassmic update only adds to them, so a plugin that runs keeps running; one that needs a later version of a scope waits for the hassmic that has it. Off, this list shows those too, and says what each needs.', h('label', { class: 'switch' }, runs, h('span'))),
      infoRow('Tier', 'Official: this project\'s own, reviewed. Community: listed, built by its author\'s own GitHub CI from the commit the index names (attested), not reviewed', null, tier),
      plgFind.say, plgFind.foot),
    plgFind.list);
  if (plg) renderBrowse(true);
}

// the browse page's list and its foot, from what the Echo last read of the index
function renderBrowse(force) {
  if (!plgFind.list || !plgIx) return;
  const o = plgIx, looking = o.state === 'looking', mine = (plg || {}).plugins || [];
  const community = !plg || plg.community !== false;        // (an Echo before the switch: all of them, as it installed them)
  const sig = JSON.stringify([Object.assign({}, o, { age: Math.round(o.age / 60) }), mine.map((p) => [p.id, p.version, p.here]), plgFind.q, plgFind.runs, plgFind.tier, plgBusy, community]);
  if (!force && sig === plgFind.sig) return;
  plgFind.sig = sig;
  const site = o.site ? h('b', { class: 'plg-key' }, o.site.replace(/^https:\/\//, '')) : 'the plugin index';
  plgFind.say.replaceChildren(...[
    o.state === 'none' ? callout('warn', h('p', {}, 'This Echo runs a hassmic from before the plugin index: update it first.')) : null,
    o.state === 'unreachable' ? callout('warn', h('p', {}, 'This Echo cannot reach the plugin index', o.error ? ` (${o.error})` : '', '. An Echo kept off the internet installs a plugin from a file: download its .hmp on a computer from ', site, ', then use "Install from file" on the Plugins page.')) : null,
    o.state === 'failed' ? callout('warn', h('p', {}, `The plugin index's list was not taken: ${o.error || 'it answered something else'}. The plugins installed here are not touched by this.`)) : null,
    o.note ? callout('info', h('p', {}, `Beside it: ${o.note}.`)) : null,
    o.dropped ? callout('info', h('p', {}, `The index lists more than this Echo keeps: ${plural(o.dropped, 'entry', 'entries')} left out.`)) : null].filter(Boolean));
  plgFind.foot.replaceChildren(...[
    h('button', { class: 'small', disabled: looking || !!plgBusy, onclick: () => plgLook(true) }, looking ? 'Looking…' : o.state === 'never' ? 'Look' : 'Look again'),
    h('span', { class: 'help' }, looking ? ' The Echo is asking the plugin index.' : o.state === 'never' ? ' Not looked yet. The Echo asks the index itself: GitHub Pages sees its address, nothing else.'
      : ` Looked ${plgMins(o)}${o.serial ? ` (list ${new Date(o.serial * 1000).toISOString().slice(0, 10)})` : ''}.`),
    plgBusy ? h('span', { class: 'help' }, ` ${plgBusy}`) : null].filter(Boolean));
  const all = o.plugins || [], { shown, hidden, withheld } = hmbrowse.find(all, plgFind.q, plgFind.runs, plgFind.tier, community);
  const row = (x) => {
    const have = mine.find((p) => p.id === x.id), v = x.here || x.top, offer = hmbrowse.offer(x, mine), broad = hmbrowse.broad(x, have);
    const [t, tHelp] = TIER[x.tier] || TIER.community;
    // the index's listing of the version installed here (docs/plugins.md 5.2: a new index key, or a higher tier),
    // which the Echo takes after a look by itself: no button, no install.  Being taken, or not taken (and why): with a
    // button to ask for it again
    const rl = offer === 'installed' && x.relist ? x.relist : null;
    const ctl = !offer ? null : rl ? (rl.state === 'pending' ? h('span', { class: 'help' }, 'Installed. Taking the index\'s listing of it…')
        : h('button', { class: 'small', disabled: !!plgBusy, title: `Not taken: ${rl.error || 'it failed'}`, onclick: () => plgLook(true, 'relist') }, 'Take its listing'))
      : offer === 'installed' ? h('span', { class: 'help' }, have.version > x.here.version ? `Installed, version ${have.version}` : 'Installed')
      : offer === 'newer' ? h('span', { class: 'help', title: 'It was installed under a newer hassmic than this one runs; it runs again under that one' }, `Installed, version ${have.version}: needs a newer hassmic`)
      : offer === 'unread' ? h('span', { class: 'help', title: 'This hassmic cannot read its manifest; the index has no newer version for it. Remove it on its card' }, `Installed, version ${have.version}: not one this hassmic reads`)
      : h('button', { class: 'small' + (offer === 'update' ? ' primary' : ''), disabled: !!plgBusy, onclick: () => plgFetch(x.id) }, offer === 'update' ? 'Update' : 'Install');
    return h('div', { class: 'row info plg-find' },
      h('div', { class: 'row-t' }, h('div', { class: 'row-label' }, h('span', {}, x.name), h('span', { class: 'badge' + (x.tier === 'official' ? '' : ' warn'), title: tHelp }, t)),
        h('p', { class: 'help' }, `by ${x.author}${v ? ` · version ${v.version_name || v.version}` : ''}`),
        x.description ? h('p', { class: 'help plg-desc' }, x.description) : null,
        broad.words || broad.twice ? h('p', { class: 'help plg-why' }, broad.words ? `Broad: it ${broad.words}. ` : '', broad.twice ? (broad.may ? 'It hears every request and has a way out: where this update asks for the action button (it asks for more than you accepted), the Echo asks for it twice. ' : 'It hears every request and has a way out: the Echo asks for its button twice. ') : '', 'Read what each lets it do when the Echo shows it, before you press.') : null,
        x.here ? null : h('p', { class: 'help plg-why' }, hmbrowse.whyNot(x)),
        v && v.requires ? h('p', { class: 'help' }, `Speaks ${hmbrowse.requires(v)}`) : null,
        rl && rl.state === 'failed' ? h('p', { class: 'help plg-why' }, `The plugin index lists this version anew (a new key of the index, or a higher tier), and this Echo did not take that listing: ${rl.error || 'it failed'}. "Take its listing" asks again.`) : null),
      h('div', { class: 'ctl btns' }, ctl));
  };
  plgFind.list.replaceChildren(...[h('div', { class: 'sub-head' }, o.state === 'never' && !all.length ? 'Plugins' : `Plugins (${shown.length}${shown.length !== all.length ? ` of ${all.length}` : ''})`),
    ...(shown.length ? shown.map(row) : [empty('box', o.state === 'never' ? 'Not looked yet: "Look" asks the plugin index.' : !all.length ? (o.state === 'ok' ? 'The plugin index lists no plugin yet.' : 'No list from the plugin index.')
      : 'No plugin matches.')]),
    // community listings while this Echo has community plugins off: not offered (it would refuse them), and said
    withheld ? h('p', { class: 'help plg-help', id: 'plugins-withheld' }, `${plural(withheld, 'community plugin')} ${withheld === 1 ? 'matches' : 'match'} and ${withheld === 1 ? 'is' : 'are'} not shown: community plugins are switched off on this Echo, which installs none of them. Switch them on on the `, h('a', { href: '#/plugins/plugins_community' }, 'Plugins page'), ' to see them.') : null,
    hidden ? h('p', { class: 'help plg-help' }, `${plural(hidden, 'more', 'more')} ${hidden === 1 ? 'matches' : 'match'} and ${hidden === 1 ? 'does' : 'do'} not run on this Echo: switch off "Only what runs on this Echo" to see ${hidden === 1 ? 'it' : 'them'}.`) : null,
    h('p', { class: 'help plg-help' }, 'Installing: the Echo downloads the package from the index, checks its signatures, and shows what the plugin may do; you accept that with the Echo\'s action button. An update that asks for nothing new needs no button.')].filter(Boolean));
}

function renderPlugins(force) {
  renderBrowse(force);
  if (!plgBox || !plg) return;
  const sig = JSON.stringify(plg) + JSON.stringify(((plgIx || {}).plugins || []).map((x) => [x.id, x.here && x.here.version])) + plgBusy + [...plgOpen].join();
  if (!force && sig === plgSig) return;
  if (!force && plgBox.contains(document.activeElement) && /^(INPUT|SELECT|TEXTAREA)$/.test(document.activeElement.tagName)) return;      // not under someone's typing
  plgSig = sig;
  const [sb, sbHelp] = SANDBOX[plg.sandbox] || SANDBOX.none, pick = h('input', { type: 'file', class: 'hidden', accept: '.hmp', onchange: () => { plgInstall(pick.files[0]); pick.value = ''; } });
  const more = moreOf('plg-sandbox', h('p', {}, sbHelp)), updates = plg.plugins.filter(plgNewer).length;
  plgTop.replaceChildren(...[plgSwitch, plgCommunityRow(),
    plg.falls_off ? callout('warn', h('p', {}, 'hassmic itself crashed three times while plugins were started. Since then no plugin starts but the one that is this Echo\'s wake word engine. If you have removed or updated the plugin you suspect, switch them on again. '),
      h('button', { class: 'small', disabled: !!plgBusy, onclick: () => plgDo('Switching plugins on again…', () => echo.call('POST', '/api/plugins/resume')) }, 'Switch plugins on again')) : null,
    plg.revoked && plg.revoked.stale ? callout('warn', h('p', {}, plg.revoked.stale)) : null,
    // (3.6: what says that a plugin hears every request is this page and the install, not a light)
    (plg.hears || []).length ? callout('warn', h('p', {}, h('b', {}, 'Hears every request: '), plg.hears.map((id) => (plg.plugins.find((x) => x.id === id) || { name: id }).name).join(', '),
      `. ${plg.hears.length === 1 ? 'It gets' : 'They get'} a copy of everything said to this Echo after the wake word or the button.`)) : null,
    !plg.launcher || plg.sandbox === 'none' ? callout('warn', h('p', {}, `No plugin can run on this Echo now: ${plg.why}.`))
      : h('div', { class: 'row info' }, h('div', { class: 'row-t' }, h('div', { class: 'row-label' }, h('span', {}, sb), more.btn),
          h('p', { class: 'help' }, plg.available ? `Up to ${plg.run_max} plugins run at once, with up to ${plg.run_mem} MB of memory between them (each counts the most it may use). One that finds no place waits for one and says so on its card.` : `Not running any: ${plg.why}.`)), h('div', { class: 'ctl' }), more.panel),
    plg.revoked ? plgRevokedRow(plg.revoked) : null,
    // an Echo whose system install is older than hassmic's own domain (2.7's third kind): hassmic itself gains from the
    // system install being run again, plugins or not
    plg.launcher && plg.sandbox === 'su' ? callout('info', h('p', {}, 'This Echo\'s system install is older than hassmic\'s own SELinux domain: hassmic itself runs without that protection too, plugins or not. To close the gap, connect the Echo to your PC (USB, or adb over Wi-Fi from this page) and ', ...plgFix(plg.install_method, echo.hello.codename), '.')) : null,
    h('div', { class: 'card-foot' }, pick,
      h('button', { class: 'small primary', onclick: () => { location.hash = '#/plugins/browse'; } }, 'Browse plugins'),
      h('button', { class: 'small', disabled: !!plgBusy, onclick: () => pick.click() }, 'Install from file…'),
      h('span', { class: 'help' }, plgBusy ? ` ${plgBusy}` : updates ? ` ${plural(updates, 'update')} in the plugin index: on ${updates === 1 ? 'its card' : 'their cards'} below.` : ' A file: a plugin package (.hmp), from the index\'s page or its author.'))].filter(Boolean));
  plgBox.replaceChildren(...(plg.plugins.length ? [h('div', { class: 'sub-head plg-installed' }, `Installed on this Echo (${plg.plugins.length})`), ...plg.plugins.map(plgCard)]
    : [h('p', { class: 'help plg-help' }, 'No plugin is installed. "Browse plugins" shows what the plugin index has.')]));
  renderCopy();
  if (typeof reflowPage === 'function') reflowPage();
}

// ---------------------------------------------------------------- copy plugins between Echos (web.c "sync"; docs/plugins.md 5.4)
// One table as "Copy models" (matrix.js): per plugin its package, its settings and each file it was given, a column per
// Echo, any cell the source.  The page only carries: the package member by member from the source, packed again
// (tar.js) and uploaded to the target as any package is; then the target is offered the source's voucher that the
// owner accepted the plugin there.  Whether that is enough is the target's to say (an official plugin, both Echos in
// one network, this browser approved on both): if not, its dialog and its own action button, as for any install.
// Secrets never travel: no Echo hands one to a page.  Files go by digest: what the other Echo has is not sent again.
const pcopy = { plan: new Map(), locked: new Set(), sig: '', busy: null, notes: [], peers: new Map(), mine: null, at: 0 };
let plgCopyBox = null;

async function plgPeers() {
  pcopy.at = Date.now();
  try { pcopy.mine = (await echo.call('GET', '/api/plugins/sync')).json(); } catch (e) { pcopy.mine = null; }
  const there = new Set();
  await Promise.all(ready().map(async (d) => {
    there.add(d.echo.base);
    // (an Echo on a build before this has no /api/plugins/sync: it is left out of the table)
    try { pcopy.peers.set(d.echo.base, { p: (await d.echo.call('GET', '/api/plugins')).json(), s: (await d.echo.call('GET', '/api/plugins/sync')).json() }); }
    catch (e) { pcopy.peers.delete(d.echo.base); }
  }));
  for (const k of [...pcopy.peers.keys()]) if (!there.has(k)) pcopy.peers.delete(k);
  if (!there.size && others.size) pcopy.at = Date.now() - 15000;        // the other Echos are still being looked at: again in 5 s
  renderCopy();
}

// a column: an Echo, its plugins (/api/plugins) and their digests (/api/plugins/sync)
function plgCols() {
  const col = (name, sub, key, e, p, s) => ({ name, sub, key, e, p, s,
    has: (id) => ((p || {}).plugins || []).find((x) => x.id === id && x.here !== false),
    dig: (id) => ((s || {}).plugins || []).find((x) => x.id === id) });
  return [col(echo.hello.name, 'This Echo', '', echo, plg, pcopy.mine),
    ...ready().filter((d) => pcopy.peers.get(d.echo.base)).map((d) => { const x = pcopy.peers.get(d.echo.base); return col(d.echo.hello.name, shortAddr(d.echo.base), d.echo.base, d.echo, x.p, x.s); })];
}

// Rows: "pkg <id>" the package (its version and its manifest's digest), "set <id>" every setting but files and secrets,
// "item <id> <list>/<item>" a file the plugin was given (its files' digest and its fields), "store <id>" what the
// plugin keeps (its digest): copied only where the owner picks it, as every row (5.4: storage only when ticked)
const plgRowOf = (row) => { const [kind, id, rest] = row.split(' '); return { kind, id, rest }; };
const plgPlain = (p) => p.settings.filter((x) => x.type !== 'files' && x.type !== 'secret');
function plgCopyVal(row, c) {
  const { kind, id, rest } = plgRowOf(row), p = c.has(id), d = c.dig(id);
  if (!p || !d) return undefined;
  if (kind === 'pkg') return `${p.version} ${d.manifest}`;
  if (kind === 'set') { const v = plgPlain(p).map((x) => [x.name, x.value]); return v.length ? JSON.stringify(v) : undefined; }
  if (kind === 'store') return d.storage === undefined ? undefined : d.storage;
  const [list, item] = rest.split('/'), l = p.settings.find((x) => x.type === 'files' && x.name === list), it = l && l.items.find((x) => x.id === item);
  return it && d.items[rest] ? `${d.items[rest]} ${JSON.stringify(Object.entries(it.fields).sort())}` : undefined;
}
// an Echo takes a plugin's settings and files only where the plugin is, or comes with this very plan
const plgCopyTakes = (row, c) => { const { kind, id } = plgRowOf(row); return !!c.s && (kind === 'pkg' || !!c.has(id) || pcopy.plan.has('pkg ' + id)); };
const plgStage = (row) => ['pkg', 'set', 'item', 'store'].indexOf(plgRowOf(row).kind);

function renderCopy() { if (plgCopyBox && plg) keepFocus(plgCopyBox, () => keepScroll(plgCopyBox, drawCopy)); }
function drawCopy() {
  const cols = plgCols(), ids = [...new Set(cols.flatMap((c) => ((c.p || {}).plugins || []).filter((x) => x.here !== false && c.dig(x.id)).map((x) => x.id)))].sort();
  const sig = JSON.stringify([cols.map((c) => [c.key, c.name, c.p && c.p.plugins, c.s]), [...pcopy.locked], [...pcopy.plan].map(([n, p]) => [n, p.from, [...p.skip]]), pcopy.busy, pcopy.notes]);
  if (sig === pcopy.sig) return;
  pcopy.sig = sig;
  plgCopyBox.classList.toggle('hidden', cols.length < 2 && !ids.length);
  plgCopyBox.innerHTML = '';
  plgCopyBox.append(cardHead('Copy plugins', 'Click the Echo a plugin, its settings or its files should come from', 'box', null,
    act('Between your Echos, instead of installing on each. An official plugin you accepted on one Echo, with its action button, is installed on the others of the same network without theirs. What stands in for the press is a voucher any Echo of the network can make, so each Echo checks the package itself (only one the plugin index listed as official, never older than one it has seen in the index) and then says so where it cannot be missed: a note on every page of the Echo it went to (and on the pages of your other Echos while they show that Echo and this browser is logged in to it), which the browser that copied cannot take off by itself, the log, and Home Assistant. Everything else asks for that Echo\'s own action button, as any install does: a plugin from a file, an Echo outside the network, an Echo in Kiosk Satellite mode, an Echo with plugins switched off. Settings follow, secrets never: no Echo hands one out. Files are sent only where the other Echo has not got them.')));
  const notes = pcopy.notes.length ? callout('info', pcopy.notes.map((t) => h('p', {}, t)), h('button', { class: 'link', onclick: () => { pcopy.notes = []; renderCopy(); } }, 'Dismiss')) : null;
  if (cols.length < 2) { plgCopyBox.append(notes || '', empty('net', 'Log in to another Echo on the Echos page to copy plugins to it.')); return; }
  const val = plgCopyVal, takes = plgCopyTakes, rows = [], all = [];
  let same = 0;
  for (const id of ids) {
    const any = cols.map((c) => c.has(id)).find(Boolean), mine = [];
    const add = (row, label, sub) => {
      all.push(row);
      // what every Echo that could have it has, the same, has nothing to copy: left out (as models are), unless picked
      const first = cols.find((c) => val(row, c) !== undefined);
      if (!pcopy.plan.has(row) && !cols.some((c) => (takes(row, c) || val(row, c) !== undefined) && val(row, c) !== val(row, first))) { same++; return; }
      mine.push({ id: row, label, sub, rank: hmmatrix.rank(row, cols, val) });
    };
    const [tier] = TIER[any.tier] || TIER.sideload;
    add('pkg ' + id, 'The plugin', any.tier === 'official' ? tier : act(`${tier}: each Echo asks for its own action button`));
    if (cols.some((c) => c.has(id) && plgPlain(c.has(id)).length)) add('set ' + id, 'Its settings', any.settings.some((x) => x.type === 'secret') ? 'Without its secrets: those are set on each Echo' : '');
    const items = new Map();
    for (const c of cols) for (const l of ((c.has(id) || {}).settings || []).filter((x) => x.type === 'files')) for (const it of l.items) if (!items.has(`${l.name}/${it.id}`)) items.set(`${l.name}/${it.id}`, [l, it]);
    for (const [k, [l, it]] of [...items].sort()) add(`item ${id} ${k}`, it.title || it.id, `${l.label} · ${it.id}`);
    if (cols.some((c) => c.dig(id) && c.dig(id).storage !== undefined)) add('store ' + id, 'What it keeps', 'Its own data about the Echo it runs on: copy it only if you mean to');
    if (mine.length) rows.push({ head: any.name }, ...mine);
  }
  if (!ids.length) { plgCopyBox.append(notes || '', empty('box', 'None of these Echos has a plugin yet: install one first, then copy it from here.')); return; }
  const text = (row, c, v) => {
    const { kind, id } = plgRowOf(row), p = c.has(id);
    if (kind === 'pkg') return `Version ${p.version}`;
    if (kind === 'store') { const d = c.dig(id); return d && d.storage_size ? plgKB(d.storage_size) : 'Nothing'; }
    if (kind === 'set') { const t = plgPlain(p).map((x) => `${x.label}: ${x.value === '' ? '–' : x.value}`).join(', '); return t.length > 60 ? t.slice(0, 59) + '…' : t; }
    return hmmatrix.rank(row, cols, val).get(v) ? 'Another version' : 'Has it';
  };
  if (!rows.length) plgCopyBox.append(notes || '', empty('check', `Every Echo here has the same ${plural(ids.length, 'plugin')}, settings and files: nothing to copy.`, true));
  else {
    const groups = hmmatrix.groups(cols, all, (r, c) => val(r, c) || '-', false);
    plgCopyBox.append(notes || '', h('div', { class: 'mx-scroll mx-short', tabindex: '0' }, matrixTable({ groups, rows, cols, val, takes, text, m: pcopy, busy: !!pcopy.busy,
      redraw: renderCopy, none: 'Pick the plugin itself for this Echo first', missing: 'Not there' })));
    if (same) plgCopyBox.append(h('p', { class: 'help copy-same' }, `${plural(same, 'more row')} ${same === 1 ? 'is' : 'are'} the same on every Echo here.`));
  }
  const { run, left } = hmmatrix.steps(hmmatrix.changes(pcopy.plan, cols, val, takes, pcopy.locked), plgStage, (r) => plgRowOf(r).id, (id, c) => !!c.has(id));
  plgCopyBox.append(h('div', { class: 'card-foot sync-foot' },
    pcopy.busy ? h('div', { class: 'busy' }, pcopy.busy.text) : null,
    pcopy.busy ? h('div', { class: 'progress', role: 'progressbar', 'aria-valuemin': '0', 'aria-valuemax': String(pcopy.busy.total), 'aria-valuenow': String(pcopy.busy.done) },
      h('div', { style: `width:${pcopy.busy.total ? (100 * pcopy.busy.done) / pcopy.busy.total : 0}%` })) : null,
    h('div', { class: 'count grow' }, run.length ? h('b', {}, plural(run.length, 'copy', 'copies')) : 'Click the Echo a row comes from',
      left.length ? `. ${plural(left.length, 'more')} left out: the plugin itself is not picked for that Echo` : ''),
    pcopy.plan.size ? h('button', { 'data-k': 'clear', disabled: !!pcopy.busy, onclick: () => { pcopy.plan.clear(); renderCopy(); } }, 'Clear') : null,
    h('button', { class: 'primary', 'data-k': 'go', disabled: !!pcopy.busy || !run.length, onclick: () => plgCopyRun() },
      pcopy.busy ? [h('span', { class: 'spin' }), 'Copying…'] : run.length ? `Copy (${plural(run.length, 'step')})` : 'Copy')));
}

// The package from one Echo onto another.  What came of it, in words; throws where nothing was installed
async function plgCarry(from, to, id) {
  const d = from.dig(id), files = [];
  // member by member, as the source keeps it; the tar is made here.  That it is the very package is the target's to
  // find: the manifest by its signature and digest, the program by the digest the manifest names, the listing by the
  // index key
  for (const m of d.pkg) {
    const parts = [];
    for (let off = 0; off < m.size; off += PLG_PIECE) {
      const len = Math.min(PLG_PIECE, m.size - off), r = await from.e.call('GET', `/api/plugins/${id}/pkg/${m.name}/${off}/${len}`);
      if (r.bytes.length !== len) throw new Error(`${from.name} sent less of the package than asked`);
      parts.push(r.bytes);
    }
    files.push({ path: m.name, parts });
  }
  const data = new Uint8Array(await new Blob(hmtar.write(files, 0)).arrayBuffer());
  await to.e.call('POST', '/api/plugins/upload/begin', String(data.length));
  for (let off = 0; off < data.length; off += PLG_PIECE) await to.e.call('POST', `/api/plugins/upload/${off}`, data.subarray(off, off + PLG_PIECE));
  const info = (await to.e.call('POST', '/api/plugins/check')).json();
  const cancel = () => to.e.call('POST', '/api/plugins/cancel').catch(() => {});
  if (!info.button) { await plgFinish(to.e, info.name); return `${info.name} updated: it is what was accepted there already`; }
  // the target wants its button for it, unless the source vouches that the owner accepted this very package there
  let why = '', voucher = null, nonce = '';
  try {
    nonce = (await to.e.call('POST', '/api/plugins/accept/nonce')).json().nonce;
    voucher = (await from.e.call('POST', '/api/plugins/accept/issue', `${to.e.hello.pub} ${nonce} ${id}`)).json();
  } catch (e) { why = e.message === 'old' ? 'one of the two runs a hassmic from before plugins were copied' : errText(e).replace(/^Error: /, ''); }
  if (voucher) {
    let r;
    try { r = (await to.e.call('POST', '/api/plugins/install', `synced ${voucher.node} ${nonce} ${voucher.voucher}`)).json(); } catch (e) { cancel(); throw e; }
    if (r.install === 'installed') return act(`${info.name} installed without its action button, on ${from.name}'s word that you accepted it there. Its note shows on every page of ${to.name} until it is dismissed, and on the pages of your other Echos while they show ${to.name} and this browser is logged in there`, to.e.hello);
    // the voucher stood for the first press; the second is the target's own (3.3, 5.4)
    if (r.install === 'second') {
      pcopy.busy.text = act(`${info.name}: ${to.name} wants its action button once, for the second press`, to.e.hello); renderCopy();
      if (!(await plgSecond(info.name, r.second, to.e))) { cancel(); throw new Error(act('left out: the second press was not asked for', to.e.hello)); }
      pcopy.busy.text = act(`Waiting for the action button on ${to.name}…`, to.e.hello); renderCopy();
      await plgFinish(to.e, info.name);
      return act(`${info.name} installed: ${from.name}'s word stood for the first press, its own action button gave the second`, to.e.hello);
    }
    why = r.why || 'it did not say why';
  }
  pcopy.busy.text = act(`${info.name}: ${to.name} wants its own action button`, to.e.hello); renderCopy();
  if (!(await plgDialog(info, to.e, (to.p || {}).plugins || [], why))) { cancel(); throw new Error(act('left out: it wants its own action button', to.e.hello)); }
  pcopy.busy.text = act(`Waiting for the action button on ${to.name}…`, to.e.hello); renderCopy();
  await plgFinish(to.e, info.name);
  return act(`${info.name} installed, accepted with its own action button`, to.e.hello);
}

// every setting but files and secrets, onto the rows of the same name and kind the other Echo's version has
async function plgCarrySettings(from, to, id) {
  const sp = from.has(id), tp = (await to.e.call('GET', '/api/plugins')).json().plugins.find((x) => x.id === id && x.here !== false);
  if (!sp || !tp) throw new Error('the plugin is not there');
  for (const s of plgPlain(sp)) {
    const t = tp.settings.find((x) => x.name === s.name && x.type === s.type);
    if (t && t.value !== s.value) await to.e.call('POST', `/api/plugins/${id}/set`, `${s.name}=${s.value}`);
  }
}

// a file the plugin was given: its files where the other Echo's digest is another (through that Echo's own upload and
// the plugin's own check there), then its fields
async function plgCarryItem(from, to, id, rest) {
  const [list, item] = rest.split('/'), sp = from.has(id), sl = sp && sp.settings.find((x) => x.type === 'files' && x.name === list), it = sl && sl.items.find((x) => x.id === item);
  if (!it) throw new Error('it is no longer there');
  const tp = (await to.e.call('GET', '/api/plugins')).json().plugins.find((x) => x.id === id && x.here !== false);
  const tl = tp && tp.settings.find((x) => x.type === 'files' && x.name === list);
  if (!tl) throw new Error('the plugin there takes no such files');
  // a wake word plugin's fields are what it said of the file while it heard the microphone: never into a plugin that
  // is not one (the target refuses it too, told by "from-wake")
  const fw = sp.wake ? 'from-wake' : '';
  if (sp.wake && !tp.wake) throw new Error('a wake word plugin\'s file, and there the plugin is not one: not copied');
  const td = ((await to.e.call('GET', '/api/plugins/sync')).json().plugins.find((x) => x.id === id) || { items: {} }).items[rest];
  if (td !== from.dig(id).items[rest]) {
    const files = {};
    for (const f of it.files) files[f.suffix] = (await from.e.call('GET', `/api/plugins/${id}/file/${list}/${item}/${f.suffix}`)).bytes;
    await plgUpload(tp, tl, item, files, to.e, fw);
  }
  const have = ((tl.items.find((x) => x.id === item) || {}).fields) || {};
  for (const [k, v] of Object.entries(it.fields)) if (td !== from.dig(id).items[rest] || have[k] !== v) await to.e.call('POST', `/api/plugins/${id}/item/${list}/${item}/field`, `${k}=${v}${fw ? `\n${fw}` : ''}`);
}

// what the plugin keeps, from one Echo onto another: the source's as one tar, replaced whole on the target (the plugin
// stopped there meanwhile; the target holds it to the kB its owner accepted).  Sealed both ways (it may hold a token
// the plugin was issued; web.c seal2): opened here with the source's key, each piece sealed again for the target.  The
// first line is the source's word whether the plugin is a wake word plugin there: the target refuses that into one
// that is not
async function plgCarryStorage(from, to, id) {
  const sp = from.has(id), tp = (await to.e.call('GET', '/api/plugins')).json().plugins.find((x) => x.id === id && x.here !== false);
  if (sp && sp.wake && tp && !tp.wake) throw new Error('a wake word plugin\'s data, and there the plugin is not one: not copied');
  // the answer: "hassmic sealed 3 <nonce hex>\n" in clear (the Echo's own nonce, new with every answer), then sealed
  const r = await from.e.call('GET', `/api/plugins/${id}/storage`), head = 'hassmic sealed 3 ', b = r.bytes;
  const nl = b.indexOf(10), first = nl > 0 ? new TextDecoder().decode(b.subarray(0, nl)) : '';
  if (!first.startsWith(head) || !/^[0-9a-f]{32}$/.test(first.slice(head.length))) throw new Error('the hassmic on the Echo it comes from is an older one: update it first');
  const data = seal2(from.e.k, r.ctr, b.subarray(nl + 1), 'hassmic seal back 3', unhex(first.slice(head.length)));
  await to.e.call('POST', `/api/plugins/${id}/storage/begin`, String(data.length));
  for (let off = 0; off < data.length; off += PLG_PIECE) {
    const piece = data.subarray(off, off + PLG_PIECE);
    await to.e.call('POST', `/api/plugins/${id}/storage/chunk/${off}`, (ctr) => seal2(to.e.k, ctr, piece, 'hassmic seal 2'));
  }
  await to.e.call('POST', `/api/plugins/${id}/storage/done`);
}

// Apply: per Echo the package first, then the settings, then the files (matrix.js steps); what did not get there stays
// picked, and each Echo's outcome is said in the card
async function plgCopyRun() {
  const cols = plgCols(), ch = hmmatrix.changes(pcopy.plan, cols, plgCopyVal, plgCopyTakes, pcopy.locked);
  const { run } = hmmatrix.steps(ch, plgStage, (r) => plgRowOf(r).id, (id, c) => !!c.has(id)), failed = new Set(), dead = new Set();
  pcopy.notes = []; pcopy.busy = { text: 'Starting…', done: 0, total: run.length }; renderCopy();
  for (const [i, c] of run.entries()) {
    const { kind, id, rest } = plgRowOf(c.row), key = `${id}\n${c.to.key}`, name = (c.from.has(id) || {}).name || id;
    const what = kind === 'pkg' ? name : kind === 'set' ? `${name}, its settings` : kind === 'store' ? `${name}, what it keeps` : `${name}, ${rest.split('/')[1]}`;
    pcopy.busy = { text: `${what}: ${c.from.name} → ${c.to.name}`, done: i, total: run.length }; renderCopy();
    try {
      if (dead.has(key)) throw new Error('the plugin itself did not get there');
      if (kind === 'pkg') pcopy.notes.push(`${c.to.name}: ${await plgCarry(c.from, c.to, id)}`);
      else if (kind === 'set') await plgCarrySettings(c.from, c.to, id);
      else if (kind === 'store') await plgCarryStorage(c.from, c.to, id);
      else await plgCarryItem(c.from, c.to, id, rest);
    } catch (e) { failed.add(c.row); if (kind === 'pkg') dead.add(key); pcopy.notes.push(`${c.to.name}: ${what}: ${errText(e).replace(/^Error: /, '')}`); }
  }
  const done = run.filter((c) => !failed.has(c.row)).length;
  if (done) pcopy.notes.push(`${plural(done, 'step')} of ${run.length} done.`);
  for (const r of [...pcopy.plan.keys()]) if (!failed.has(r)) pcopy.plan.delete(r);
  pcopy.busy = null;
  await getPlugins().catch(() => {}); await plgPeers().catch(() => {}); load().catch(() => {});
  renderCopy();
}
