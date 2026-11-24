#!/usr/bin/env python3
"""web/plugins.js's browse page ("Browse plugins", #/plugins/browse), through node: which of the plugin index's
entries a search, the switch "Only what runs on this Echo" and the tier keep (in the index's order, with how many
more match and do not run here), what each row offers (install, update, installed, nothing), and why one that does
not run here does not, in the page's words, and the broad capabilities the index flags on a community version.  The entries are as /api/plugins/index gives them (plugin_fetch.c
pf_json); tests/fake_plugins_fetch.py has the Echo's side."""
import json, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
JS = """
const m = require(process.argv[1]);
const v = (version, caps, extra) => Object.assign({ version, version_name: `${version}.0`, caps }, extra || {});
const ix = [
  { id: 'microwakeword', name: 'microWakeWord', author: 'hassmic', description: 'The open wake word engine of ESPHome.', tier: 'official',
    here: v(3, ['files', 'wake'], { requires: { core: 1, files: 1, wake: 1 }, needs: [] }), top: v(3, ['files', 'wake'], { requires: { core: 1, files: 1, wake: 1 }, needs: [] }) },
  { id: 'syslog', name: 'Syslog forwarder', author: 'someone', description: 'Sends the log to a server.', tier: 'community',
    here: null, top: v(2, ['log.read', 'net.lan'], { requires: { core: 1, 'log.read': 2, 'net.lan': 3 }, needs: ['log.read 2', 'net.lan 3'] }) },
  { id: 'nextapi', name: 'Next', author: 'hassmic', description: 'For the next hassmic.', tier: 'official',
    here: null, top: v(1, ['files'], { requires: { core: 2, files: 1 }, needs: ['core 2'] }) },
  { id: 'oldapi', name: 'Old', author: 'hassmic', description: 'Under the least version.', tier: 'community',
    here: null, top: v(1, ['files'], { requires: { core: 1, files: 1 }, needs: [] }) },
  { id: 'nothing', name: 'Nothing yet', author: 'hassmic', description: '', tier: 'official', here: null, top: null },
  { id: 'unknown', name: 'Unknown', author: 'x', description: 'Asks for something new.', tier: 'community', here: null, top: v(1, [], { requires: { core: 1 }, needs: [], unknown: true }) },
];
const ids = (r) => [r.shown.map((x) => x.id), r.hidden];
const out = {
  all: ids(m.find(ix, '', false, '')), runs: ids(m.find(ix, '', true, '')),
  official: ids(m.find(ix, '', false, 'official')), community: ids(m.find(ix, '', false, 'community')),
  byName: ids(m.find(ix, 'WAKE', true, '')), byAuthor: ids(m.find(ix, 'someone', false, '')), byWords: ids(m.find(ix, 'network server', false, '')),
  byCap: ids(m.find(ix, 'microphone', true, '')), both: ids(m.find(ix, 'wake  engine', true, '')), none: ids(m.find(ix, 'zzz', false, '')),
  hiddenOnly: ids(m.find(ix, 'log', true, '')),
  // community plugins switched off on this Echo (docs/plugins.md 2.7): only official ones, and how many matched besides
  off: ((r) => [r.shown.map((x) => x.id), r.hidden, r.withheld])(m.find(ix, '', false, '', false)),
  offRuns: ((r) => [r.shown.map((x) => x.id), r.hidden, r.withheld])(m.find(ix, '', true, '', false)),
  offCommunity: ((r) => [r.shown.map((x) => x.id), r.hidden, r.withheld])(m.find(ix, '', false, 'community', false)),
  onWithheld: m.find(ix, '', false, '').withheld,
  offer: [m.offer(ix[0], []), m.offer(ix[0], [{ id: 'microwakeword', version: 2 }]), m.offer(ix[0], [{ id: 'microwakeword', version: 3 }]),
          m.offer(ix[0], [{ id: 'microwakeword', version: 4 }]), m.offer(ix[0], [{ id: 'microwakeword', version: 0, here: false }]), m.offer(ix[1], []),
          // installed under a newer hassmic (a fall back): its version kept, here false.  The index's version for this
          // hassmic is older: nothing to update to (Update fetched it and the Echo refused it as not newer, finding 2
          // of a review of 4e0a202); a newer one there all the same: an update
          m.offer(ix[0], [{ id: 'microwakeword', version: 5, here: false, newer: true }]), m.offer(ix[0], [{ id: 'microwakeword', version: 2, here: false, newer: true }]),
          // installed with a manifest this build does not read otherwise, its version known from its listing (finding 7
          // of a review of 60a8589): an update only of a newer one, as the Echo takes no other; 0 (none known): any
          m.offer(ix[0], [{ id: 'microwakeword', version: 3, here: false }]), m.offer(ix[0], [{ id: 'microwakeword', version: 4, here: false }]),
          m.offer(ix[0], [{ id: 'microwakeword', version: 2, here: false }])],
  // what it may do: each capability's line, under it the words of every later version required that widens it (web.c
  // "widens"; finding 5 of a review of 60a8589), core's as lines of their own; none: nothing
  caps: [m.capLines([{ cap: 'net.lan', line: 'Connects to any device on your network (TCP, port 80)' }, { cap: 'storage', line: 'Keeps up to 4 kB' }],
                    [{ scope: 'net.lan', version: 2, words: 'names, not only addresses' }, { scope: 'net.lan', version: 4, words: 'any port' }, { scope: 'core', version: 2, words: 'a later core' }]),
         m.capLines([], []), m.capLines([{ cap: 'state', line: 'Sees when this Echo listens' }], undefined)],
  why: ix.slice(1).map((x) => m.whyNot(x)),
  req: [m.requires(ix[0].here), m.requires(ix[1].top), m.requires(null), m.requires({})],
  // an update's lines in the dialog's lists: a raise within ("speaks") apart from what asks
  split: ((c) => [c.changes.map((x) => x.kind), c.limits.length, c.speaks.length])(m.changes([{ kind: 'more' }, { kind: 'speaks' }, { kind: 'limit' }, { kind: 'changed' }, { kind: 'new' }])),
  // and what found no place in the check's list, counted (web.c changes_more, shown_more; finding 3 of a review of
  // 61e3d8e: "And more." without a count): a line that ends each list, none for 0 or no number
  more: [9, 0, undefined, '3'].map((x) => ((c) => [c.more, c.shownMore])(m.changes([], x, x === 9 ? 2 : x))),
  // the index's broad flags on a community version (index.json "broad"), of the version shown, in words; a name the
  // page does not know left out; the second press where it hears every request and has a way out
  broad: [m.broad(ix[0]), m.broad({ here: null, top: v(1, ['storage', 'net.internet', 'call'], { broad: ['net.internet', 'call', 'teleport'] }) }),
          m.broad({ here: v(2, ['request.audio', 'net.server'], { broad: ['request.audio'] }), top: v(3, ['files'], { broad: ['log.read.full'] }) }),
          m.broad({ here: null, top: null }), m.broad({ here: v(1, ['request.audio']), top: null }),
          // an update of a plugin whose accepted version hears every request and has a way out already: it asks twice
          // wherever it asks for the button at all (plugin_store.c: button && ps_second), which only the check says: "may"
          // (it was said to ask for nothing, a review of 3c5d7ff, R9); one that adds the pair, or a first install, asks
          // for sure; the version installed already: nothing offered, nothing asked.  Installed and needing a newer
          // hassmic (here: false): the check holds it against an accepted manifest the page does not have, so "may"
          // whatever its card's caps say (it was "twice" for sure: a review of 3fcd319, S7)
          m.broad({ here: v(2, ['request.audio', 'net.server']), top: null }, { id: 'x', version: 1, caps: [{ cap: 'request.audio' }, { cap: 'net.lan' }] }),
          m.broad({ here: v(2, ['request.audio', 'net.server']), top: null }, { id: 'x', version: 1, caps: [{ cap: 'request.audio' }] }),
          m.broad({ here: v(2, ['request.audio', 'net.server']), top: null }, { id: 'x', version: 1, here: false, caps: [{ cap: 'request.audio' }, { cap: 'net.lan' }] }),
          m.broad({ here: v(2, ['request.audio', 'net.server']), top: null }, { id: 'x', version: 1, here: false, caps: [] }),
          m.broad({ here: v(2, ['request.audio', 'net.server']), top: null }, { id: 'x', version: 2, caps: [{ cap: 'request.audio' }, { cap: 'net.server' }] }),
          // installed under a newer hassmic, the index's for this one older: nothing offered, nothing asked
          m.broad({ here: v(2, ['request.audio', 'net.server']), top: null }, { id: 'x', version: 5, here: false, newer: true, caps: [] })],
};
console.log(JSON.stringify(out));
"""
bad = n = 0


def check(cond, what):
    global bad, n
    n += 1
    if not cond:
        bad += 1
        print("FAIL", what)


r = subprocess.run(["node", "-e", JS, os.path.join(ROOT, "web/plugins.js")], capture_output=True, text=True)
if r.returncode:
    print(r.stderr)
    sys.exit(1)
o = json.loads(r.stdout)
check(o["all"] == [["microwakeword", "syslog", "nextapi", "oldapi", "nothing", "unknown"], 0], f"everything, in the index's order: {o['all']}")
check(o["runs"] == [["microwakeword"], 5], f"only what runs here, and how many more there are: {o['runs']}")
check(o["off"] == [["microwakeword", "nextapi", "nothing"], 0, 3] and o["offRuns"] == [["microwakeword"], 2, 3] and o["offCommunity"] == [[], 0, 3] and o["onWithheld"] == 0,
      f"community plugins off here: the official ones only, the community ones that match counted apart (not as \"does not run\"): {o['off']} {o['offRuns']} {o['offCommunity']} {o['onWithheld']}")
check(o["official"][0] == ["microwakeword", "nextapi", "nothing"] and o["community"][0] == ["syslog", "oldapi", "unknown"], f"by tier: {o['official']} {o['community']}")
check(o["byName"] == [["microwakeword"], 0], f"a word of the name, any case: {o['byName']}")
check(o["byAuthor"] == [["syslog"], 0], f"the author: {o['byAuthor']}")
check(o["byWords"] == [["syslog"], 0], f"what it may do, in people's words (net.lan: network ...; every word must be found): {o['byWords']}")
check(o["byCap"] == [["microwakeword"], 0] and o["both"] == [["microwakeword"], 0], f"a capability's words, and two words with more space between: {o['byCap']} {o['both']}")
check(o["none"] == [[], 0], "nothing matches: nothing")
check(o["hiddenOnly"] == [[], 1], f"one that matches and does not run here: not shown with the switch on, but counted: {o['hiddenOnly']}")
check(o["offer"] == ["install", "update", "installed", "installed", "update", "", "newer", "update", "unread", "unread", "update"],
      f"install when not here, update when the index has a newer one for this hassmic (or the one here is a manifest this build does not read), "
      f"installed otherwise, \"newer\" for one installed under a newer hassmic with nothing newer for this one, \"unread\" for one whose manifest this "
      f"build does not read with nothing newer than its listing's version (any where none is known), nothing when none runs here: {o['offer']}")
check(o["caps"] == [[{"cap": "net.lan", "line": "Connects to any device on your network (TCP, port 80)", "more": ["From net.lan 2: names, not only addresses", "From net.lan 4: any port"]},
                     {"cap": "storage", "line": "Keeps up to 4 kB", "more": []},
                     {"cap": "core", "line": "Speaks core 2, which lets a plugin do more: a later core", "more": []}],
                    [], [{"cap": "state", "line": "Sees when this Echo listens", "more": []}]],
      f"what it may do: each capability's line with every widening version's words under it, core's apart: {o['caps']}")
check(o["why"] == ["Needs a newer hassmic: it speaks log.read 2 and net.lan 3, which this one does not yet.", "Needs a newer hassmic: it speaks core 2, which this one does not yet.",
                   "Not one this hassmic installs (a version under the least it runs, or a package over 4 MB).", "No released version yet.", "Needs a newer hassmic: it asks for something this one does not know."],
      f"why it does not run here, in words (what of its requires this hassmic lacks, as the Echo names it): {o['why']}")
check(o["req"] == ["core 1, files 1, wake 1", "core 1, log.read 2, net.lan 3", "", ""], f"what a version requires, in words: {o['req']}")
check(o["more"] == [["And 9 more.", "And 2 more."], [None, None], [None, None], [None, None]], f"the lines left out of the check's list, counted, under each list: {o['more']}")
check(o["split"] == [["more", "changed", "new"], 1, 1], f"an update's lines: what asks, its limits, and the protocol it speaks within what was accepted, each apart: {o['split']}")
check(o["broad"] == [{"words": "", "twice": False, "may": False}, {"words": "connects to the internet and makes and takes calls", "twice": False, "may": False},
                     {"words": "hears every request said to this Echo", "twice": True, "may": False}, {"words": "", "twice": False, "may": False},
                     {"words": "", "twice": False, "may": False}, {"words": "", "twice": True, "may": True}, {"words": "", "twice": True, "may": False},
                     {"words": "", "twice": True, "may": True}, {"words": "", "twice": True, "may": True}, {"words": "", "twice": False, "may": False},
                     {"words": "", "twice": False, "may": False}],
      f"the broad capabilities the index flags, of the version this Echo would install (else the newest), in words; a name it does not know left out; "
      f"the second press for every request heard with a way out where the button may be asked: for sure on a first install or an update that adds "
      f"the pair, \"may\" on an update of a plugin accepted with both or installed for a newer hassmic, none for the version installed: {o['broad']}")
print(f"web browse: {n - bad} of {n} checks passed")
sys.exit(1 if bad else 0)
