#!/usr/bin/python
"""EchoRoot Wi-Fi request worker."""
from __future__ import print_function
import os
import re
import subprocess
import sys
import tempfile
import time

state = os.environ.get('HASSMIC_STATE', '/home/root/echo1-assist/state-local')
out = os.environ.get('HASSMIC_WIFI', '/home/root/echo1-assist/wifi-local')
cli = os.environ.get('MISTO_WPA_CLI', '/usr/bin/wpa_cli')
os.umask(0o077)

def run(cmd):
    with tempfile.TemporaryFile() as result:
        p = subprocess.Popen([cli, '-i', 'wlan0', cmd], stdout=result, stderr=subprocess.PIPE)
        deadline = time.time() + 8
        while p.poll() is None and time.time() < deadline:
            time.sleep(0.1)
        if p.poll() is None:
            p.kill()
            p.wait()
            raise RuntimeError('wpa_cli timed out')
        if p.returncode:
            raise RuntimeError('wpa_cli failed')
        result.seek(0)
        return result.read(131072).decode('latin-1')

def publish(name, content):
    fd, temp = tempfile.mkstemp(prefix='.' + name + '-', dir=out)
    try:
        with os.fdopen(fd, 'wb') as f:
            f.write(content.encode('latin-1'))
        os.rename(temp, os.path.join(out, name))
    finally:
        if os.path.exists(temp):
            os.unlink(temp)

def main():
    if not os.path.isdir(out):
        os.mkdir(out, 0o700)
    status = run('status')
    if 'wpa_state=' in status:
        publish('status', status)
    request = os.path.join(state, 'wifi-request')
    lock = os.path.join(out, 'lock')
    if os.path.isdir(lock):
        subprocess.call([sys.executable, os.path.join(os.path.dirname(__file__), 'misto-wifi-join.py'), '--recover'])
        if os.path.isdir(lock): return
    if not os.path.isfile(request):
        return
    try:
        os.mkdir(lock, 0o700)
    except OSError:
        return
    job = os.path.join(lock, 'request')
    handed_off = False
    try:
        try:
            os.rename(request, job)
        except OSError:
            return
        with open(job, 'r') as f:
            words = f.read(256).split()
        if len(words) < 2 or not re.match(r'^[0-9]{1,10}$', words[1]):
            raise RuntimeError('invalid request')
        kind, job_id = words[:2]
        if kind == 'join':
            worker = os.path.join(os.path.dirname(__file__), 'misto-wifi-join.py')
            p = subprocess.Popen([sys.executable, worker], close_fds=True, preexec_fn=os.setsid)
            with open(os.path.join(lock, 'pid'), 'w') as f: f.write(str(p.pid))
            handed_off = True
            return
        if kind != 'scan' or len(words) != 2:
            raise RuntimeError('unsupported request')
        os.unlink(job)
        if run('scan').strip() != 'OK':
            raise RuntimeError('scan command refused')
        time.sleep(4)
        rows = run('scan_results')
        if not rows.startswith('bssid / frequency / signal level / flags / ssid'):
            raise RuntimeError('invalid scan result')
        publish('scan', 'id %s\n%s' % (job_id, rows))
    finally:
        if not handed_off:
            if os.path.exists(job): os.unlink(job)
            os.rmdir(lock)

if __name__ == '__main__':
    try:
        main()
    except Exception as e:
        sys.stderr.write('misto wifi: %s\n' % e)
        sys.exit(1)
