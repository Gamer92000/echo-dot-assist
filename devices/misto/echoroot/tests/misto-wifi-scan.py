import os
import subprocess
import sys
import tempfile

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
with tempfile.TemporaryDirectory() as d:
    state, out = os.path.join(d, 'state'), os.path.join(d, 'out')
    os.mkdir(state)
    cli = os.path.join(d, 'wpa_cli')
    with open(cli, 'w') as f:
        f.write('''#!/bin/sh
case "$3" in
 status) printf 'wpa_state=COMPLETED\\nssid=test\\nip_address=192.0.2.1\\n';;
 scan) printf 'OK\\n';;
 scan_results) printf 'bssid / frequency / signal level / flags / ssid\\n00:00:00:00:00:00\\t2412\\t-42\\t[WPA2-PSK-CCMP][ESS]\\ttest\\n';;
 *) exit 1;;
esac
''')
    os.chmod(cli, 0o755)
    env = dict(os.environ, HASSMIC_STATE=state, HASSMIC_WIFI=out, MISTO_WPA_CLI=cli)
    req = os.path.join(state, 'wifi-request')
    with open(req, 'w') as f: f.write('scan 123\n')
    subprocess.check_call([sys.executable, os.path.join(root, 'misto-wifi-scan.py')], env=env)
    with open(os.path.join(out, 'scan')) as f: scan = f.read()
    assert scan.startswith('id 123\n') and '\t2412\t-42\t' in scan
    assert not os.path.exists(req) and not os.path.exists(os.path.join(out, 'lock'))
    print('PASS scan result and request cleanup')
