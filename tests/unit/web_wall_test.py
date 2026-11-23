#!/usr/bin/env python3
"""web/plugins.js hmwall, through node: whether the install dialog of a package shows docs/plugins.md 2.7's warning in
full (plgWallWarning: the sandbox misses its SELinux wall), and with which words.  The dialog is where every plugin
that is not official is accepted, however community plugins came to be on (the switch's own dialog, an import, a
preset, Compare & copy, "make like this Echo"), so it is shown there for each one, by the installing Echo's own check
(web.c check_json "sandbox", "install_method"); an official plugin never; a complete sandbox never.  An Echo whose
check has neither field: this page's word for itself (/api/plugins) where the dialog is for this Echo, else none.
tests/fake_plugins_community.py has the Echo's side (the check's fields per sandbox)."""
import json, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
JS = """
const m = require(process.argv[1]).hmwall;
const chk = (tier, sandbox, install_method) => Object.assign({ id: 'x', tier }, sandbox === undefined ? {} : { sandbox, install_method });
const mine = (sandbox, install_method) => ({ sandbox, install_method, plugins: [] });
console.log(JSON.stringify([
  m.of(chk('sideload', 'hassmic', 'twrp-ab'), null), m.of(chk('community', 'su', 'boot'), null), m.of(chk('community', 'hassmic', 'cannoli-ubi'), mine('complete', 'twrp-ab')),
  m.of(chk('official', 'su', 'boot'), null), m.of(chk('official', 'hassmic', 'twrp-ab'), mine('su', 'boot')),
  m.of(chk('community', 'complete', 'twrp-ab'), null), m.of(chk('sideload', 'complete', 'boot'), mine('su', 'boot')), m.of(chk('community', 'none', 'boot'), null),
  m.of(chk('community'), mine('su', 'running')), m.of(chk('sideload'), null), m.of(chk('official'), mine('su', 'boot')), m.of(null, null),
]));
"""
r = subprocess.run(["node", "-e", JS, os.path.join(ROOT, "web/plugins.js")], capture_output=True, text=True)
if r.returncode:
    print(r.stderr)
    sys.exit(1)
o = json.loads(r.stdout)
want = [
    {"sandbox": "hassmic", "install": "twrp-ab"},       # a file's, the plugins' own domain missing
    {"sandbox": "su", "install": "boot"},               # a community listing, SELinux missing, worded for a boot image
    {"sandbox": "hassmic", "install": "cannoli-ubi"},   # the installing Echo's check wins over this page's Echo
    None, None,                                         # official: never, whatever the sandbox
    None, None, None,                                   # complete, or no sandbox at all (nothing installs there): none
    {"sandbox": "su", "install": "running"},            # a check without the fields, for this Echo: its own word
    None, None, None,                                   # ... for another Echo: none; official; nothing checked
]
bad = 0
for i, (got, exp) in enumerate(zip(o, want)):
    if got != exp:
        bad += 1
        print(f"FAIL case {i}: {got} != {exp}")
print(f"web wall: {len(want) - bad} of {len(want)} ok")
sys.exit(1 if bad or len(o) != len(want) else 0)
