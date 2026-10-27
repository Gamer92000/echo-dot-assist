#!/usr/bin/env python3
"""Exercise the unmodified page's card/controls in a real headless Chrome DOM."""
import re, shutil, subprocess, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
chrome = next((shutil.which(x) for x in ("google-chrome", "chromium", "chromium-browser") if shutil.which(x)), None)
if not chrome:
    print("SKIP request-padding UI: Chrome/Chromium not installed")
    raise SystemExit(0)

TEST = r"""
try {
  const assert = (x, why) => { if (!x) throw Error(why); };
  const toggle = { name: 'pad_request_audio', label: 'Home Assistant VAD compatibility', group: 'Features', type: 'bool', feature: 1, value: 0 };
  const number = { name: 'request_audio_padding', label: 'Request audio padding', group: 'Features', type: 'int', unit: 'ms', min: 0, max: 1000, value: 0 };
  const engine = { name: 'wake_engine', type: 'choice', choices: ['amazon', 'microwakeword', 'homeassistant'], value: 0 };
  state = { settings: [toggle, number, engine] };
  const children = state.settings.filter(k => (HELP[k.name] || {}).parent === toggle.name);
  const section = sectionShell({id: 'Features', icon: 'sparkle'});
  const card = featureCard(toggle, children); section.append(card); document.body.append(section);
  const paint = () => { controls.get(toggle.name).update(toggle); if (controls.has(number.name)) controls.get(number.name).update(number); };
  paint();
  assert(card.querySelector('.f-title').textContent.startsWith('Home Assistant VAD compatibility'), 'exact toggle label');
  assert(card.querySelector('.f-help'), 'padding help missing');
  assert(card.querySelector('.f-ha').textContent.includes('Request audio padding'), 'HA numeric control explanation missing');
  const row = card.querySelector('.f-child');
  assert(row && getComputedStyle(row).display === 'none', 'off must hide numeric child');
  const writes = [];
  set = (name, value) => writes.push([name, value]);
  const checkbox = card.querySelector('input[type=checkbox]'); checkbox.click();
  assert(writes[0][0] === toggle.name && writes[0][1] === 'on', 'toggle must send on');
  toggle.value = 1; number.value = 800; paint();
  assert(checkbox.checked && getComputedStyle(row).display !== 'none', 'on must show numeric child');
  const slider = row.querySelector('input[type=range]');
  assert(slider.min === '0' && slider.max === '1000' && slider.step === '1' && slider.value === '800', 'numeric slider bounds/default');
  assert(row.querySelector('output').textContent === '800 ms', 'ms output');
  engine.value = 2; paint();
  assert(getComputedStyle(card).display === 'none', 'Home Assistant wake word must hide the whole compatibility card');
  assert(toggle.value === 1 && number.value === 800, 'hiding must preserve saved compatibility settings');
  engine.value = 1; paint();
  assert(getComputedStyle(card).display !== 'none', 'microWakeWord must show compatibility card');
  engine.value = 0; paint();
  assert(getComputedStyle(card).display !== 'none', 'Amazon wake word must show compatibility card');
  slider.value = '137'; slider.dispatchEvent(new Event('input')); slider.dispatchEvent(new Event('change'));
  assert(writes[1][0] === number.name && writes[1][1] === '137', 'numeric change must send chosen duration');
  toggle.value = 0; number.value = 0; paint();
  assert(!checkbox.checked && slider.value === '0' && getComputedStyle(row).display === 'none', 'off resets/hides child');
  document.body.innerHTML = '<pre id="verdict">PASS request-padding UI: actual Chrome DOM, labels, visibility, controls and ms</pre>';
} catch(e) { document.body.innerHTML = '<pre id="verdict">FAIL ' + e.message + '</pre>'; }
"""

with tempfile.TemporaryDirectory() as tmp:
    page = (ROOT / "web/index.html").read_text()
    for asset in ("crypto.js", "tar.js", "app.js", "ring.js"):
        src = (ROOT / "web" / asset).read_text()
        if asset == "app.js":
            assert src.rstrip().endswith("start();")
            src = src.rstrip()[:-len("start();")]
        page = page.replace(f'<script src="/{asset}"></script>', '<script>' + src + '</script>')
    page = page.replace('</body>', '<script>' + TEST + '</script></body>')
    path = Path(tmp) / "test.html"; path.write_text(page)
    p = subprocess.run([chrome, '--headless', '--no-sandbox', '--disable-gpu', '--disable-dev-shm-usage',
                        '--no-proxy-server', '--dump-dom', '--user-data-dir=' + tmp + '/profile', path.as_uri()],
                       capture_output=True, text=True, timeout=30)
    verdict = re.search(r'<pre id="verdict">([^<]+)</pre>', p.stdout)
    assert p.returncode == 0 and verdict, 'Chrome failed to exercise page: ' + p.stderr[-500:]
    print(verdict[1])
    assert verdict[1].startswith('PASS'), verdict[1]
