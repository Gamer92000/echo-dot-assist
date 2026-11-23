#!/usr/bin/env python3
"""web/plugins.js's rows of a plugin's own (docs/plugins.md 4.3 "As built"), through node: how each entity
marked "page" reads on the plugin's card, by its kind, from the row /api/plugins carries (plugin.c plugin_json
"rows": label, kind, unit, decimals, and the value the plugin said, which ent_value_ok checked against the kind):
on/off as words, a sensor to its decimals with its unit, a number without a float's stray digits, an option and a
text as they are, nothing said yet; and which entities the card lists besides (hmrows.listed): not those shown as a
row, but one Home Assistant does not list ("unlisted": "nokey") always, with its notice.  What is checked here is the
text hmrows.text returns: markup in a value comes back as the same characters, unescaped and unchanged.  That the page
then puts it in as a text node (plgRows -> infoRow -> app.js h(), which makes text nodes of strings) is not run here:
no test renders the page's DOM.  tests/unit/plugin_broker_test.c has the rows' JSON, tests/fake_plugins_button.py
the page's call."""
import json, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
JS = """
const m = require(process.argv[1]).hmrows;
const r = (kind, value, extra) => Object.assign({ name: 'x', label: 'X', kind, unit: '', decimals: 0 }, value === undefined ? {} : { value }, extra || {});
const out = [
  m.text(r('binary_sensor', 'on')), m.text(r('binary_sensor', 'off')), m.text(r('switch', 'on')), m.text(r('switch', 'off')),
  m.text(r('sensor', '12.25', { unit: 'W', decimals: 1 })), m.text(r('sensor', '12', { decimals: 0 })), m.text(r('sensor', '-0.5', { unit: '°C', decimals: 2 })),
  m.text(r('sensor', '3', { decimals: 9 })), m.text(r('sensor', '1e3', { unit: 'lx' })),
  m.text(r('number', '0.30000000000000004', { unit: 'dB' })), m.text(r('number', '7')), m.text(r('number', '-2.5')),
  m.text(r('select', 'Fast')), m.text(r('text_sensor', '<b>fine</b> & "ok"')), m.text(r('text_sensor', '')),
  m.text(r('sensor')), m.text(r('text_sensor', null)),
];
const L = (p) => m.listed(p).map((e) => e.name);
const ents = [{ name: 'a' }, { name: 'b', unlisted: 'nokey' }, { name: 'c' }];
out.push(L({ entities: ents, rows: [{ name: 'a' }] }), L({ entities: ents, rows: [{ name: 'a' }, { name: 'b' }] }), L({ entities: ents }), L({}));
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
check(o[0:4] == ["On", "Off", "On", "Off"], f"a binary sensor and a switch: On and Off: {o[0:4]}")
check(o[4:9] == ["12.3 W", "12", "-0.50 °C", "3.000000", "1000 lx"], f"a sensor to its decimals (six at most) with its unit: {o[4:9]}")
check(o[9:12] == ["0.3 dB", "7", "-2.5"], f"a number as it is, without a float's stray digits, with its unit: {o[9:12]}")
check(o[12:15] == ["Fast", '<b>fine</b> & "ok"', ""], f"an option and a text returned as they are (markup the same characters, not escaped or dropped): {o[12:15]}")
check(o[15:17] == ["Nothing said yet", "Nothing said yet"], f"no value yet: said so: {o[15:17]}")
check(o[17] == ["b", "c"], f"the entities listed beside the rows: not one shown as a row: {o[17]}")
check(o[18] == ["b", "c"], f"one Home Assistant does not list (nokey) is listed with its notice even where a row names it: {o[18]}")
check(o[19] == ["a", "b", "c"] and o[20] == [], f"no rows: every entity; none: nothing: {o[19:21]}")
print(f"web rows: {n - bad} of {n} passed")
sys.exit(1 if bad else 0)
