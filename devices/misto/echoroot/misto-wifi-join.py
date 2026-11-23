#!/usr/bin/python
"""Transactional Wi-Fi switch for EchoRoot; started independently of SSH."""
from __future__ import print_function
import json
import fcntl
import os
import re
import signal
import socket
import subprocess
import struct
import sys
import tempfile
import time

base = os.environ.get('ECHOASSIST_HOME', '/home/root/echo1-assist')
out = os.environ.get('HASSMIC_WIFI', base + '/wifi-local')
lock = out + '/lock'
config = os.environ.get('MISTO_WPA_CONFIG', '/etc/wpa_supplicant.conf')
ctrl = os.environ.get('MISTO_WPA_SOCKET', '/run/wpa_supplicant/wlan0')
dhcp = '/sbin/udhcpc'
pidfile = '/var/run/udhcpc.wlan0.pid'
hook = base + '/misto-wifi-dhcp.sh'
os.umask(0o077)

def uptime():
    return float(open('/proc/uptime').read().split()[0])

def atomic(path, value, mode=0o600):
    fd, tmp = tempfile.mkstemp(prefix='.wifi-', dir=os.path.dirname(path))
    try:
        os.fchmod(fd, mode)
        with os.fdopen(fd, 'wb') as f:
            f.write(value.encode('latin-1'))
            f.flush()
            os.fsync(f.fileno())
        os.rename(tmp, path)
        try:
            parent = os.open(os.path.dirname(path), os.O_RDONLY)
            try: os.fsync(parent)
            finally: os.close(parent)
        except OSError:
            pass
    finally:
        if os.path.exists(tmp): os.unlink(tmp)

def journal(j):
    atomic(lock + '/journal', json.dumps(j))

def result(j, state, why='', ip=''):
    if state == 'ok':
        text = '%s ok %s %s 1\n' % (j['id'], j['ssid'], ip)
    elif state == 'switching':
        text = '%s switching %s\n' % (j['id'], j['ssid'])
    else:
        text = '%s failed %s %s\n' % (j['id'], j['ssid'], why or 'failed')
    atomic(out + '/result', text)

class Control(object):
    def __init__(self):
        self.tmp = tempfile.mkdtemp(prefix='echo-wpa-')
        self.path = self.tmp + '/sock'
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.s.bind(self.path)
        self.s.settimeout(3)
        self.s.connect(ctrl)
    def command(self, cmd):
        self.s.send(cmd.encode('ascii'))
        r = self.s.recv(8192).decode('latin-1').strip()
        if r.startswith('FAIL') or r == 'UNKNOWN COMMAND':
            raise RuntimeError('wpa command failed')
        return r
    def close(self):
        self.s.close()
        os.unlink(self.path)
        os.rmdir(self.tmp)

def values(text):
    return dict(line.split('=', 1) for line in text.splitlines() if '=' in line)

def dhcp_stop():
    try:
        pid = int(open(pidfile).read().strip())
        cmd = open('/proc/%d/cmdline' % pid, 'rb').read()
        if b'udhcpc' not in cmd or b'wlan0' not in cmd: raise RuntimeError('unexpected DHCP process')
        os.kill(pid, signal.SIGTERM)
        until = uptime() + 5
        while os.path.exists('/proc/%d' % pid) and uptime() < until:
            try:
                if open('/proc/%d/stat' % pid).read().split()[2] == 'Z': break
            except IOError: break
            time.sleep(0.2)
        else:
            if os.path.exists('/proc/%d' % pid): raise RuntimeError('dhcp_stop')
    except IOError:
        pass

def dhcp_start(marker):
    env = dict(os.environ, MISTO_DHCP_MARKER=marker)
    devnull = open('/dev/null', 'r+b')
    p = subprocess.Popen([dhcp, '-f', '-n', '-t', '4', '-T', '3', '-p', pidfile,
                          '-i', 'wlan0', '-s', hook], stdin=devnull, stdout=devnull,
                         stderr=devnull, env=env, preexec_fn=os.setsid)
    devnull.close()
    return p

def wait_lease(p, marker):
    until = uptime() + 20
    while uptime() < until:
        if os.path.exists(marker):
            ip = open(marker).read().strip()
            if re.match(r'^[0-9.]{7,15}$', ip) and ip != '0.0.0.0':
                s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                try: actual = socket.inet_ntoa(fcntl.ioctl(s.fileno(), 0x8915, struct.pack('256s', b'wlan0'))[20:24])
                finally: s.close()
                if actual == ip: return ip
        if p.poll() is not None: break
        time.sleep(0.5)
    raise RuntimeError('dhcp')

def cleanup():
    for name in ('request', 'pid', 'journal', 'lease'):
        try: os.unlink(lock + '/' + name)
        except OSError: pass
    os.rmdir(lock)

def rollback(j):
    c = Control()
    try:
        if j.get('new') is not None:
            try: c.command('REMOVE_NETWORK %d' % j['new'])
            except Exception: pass
        c.command('SELECT_NETWORK %d' % j['old'])
        for n in j.get('enabled', []):
            if n != j['old']:
                try: c.command('ENABLE_NETWORK %d' % n)
                except Exception: pass
    finally:
        c.close()
    if j.get('dhcp_changed'):
        dhcp_stop()
        dhcp_start(out + '/lease-restored')

def commit(j, psk):
    original = open(config, 'rb').read().decode('latin-1')
    if '\x00' in original: raise RuntimeError('config')
    priorities = [int(x) for x in re.findall(r'(?m)^\s*priority\s*=\s*(-?\d+)\s*$', original)]
    priority = max([0] + priorities) + 1
    if priority > 2147483647: raise RuntimeError('priority')
    marker = '# echoassist-wifi-transaction-' + str(j['id'])
    block = '\n%s\nnetwork={\n    ssid=%s\n    priority=%d\n' % (marker, j['ssid'], priority)
    block += '    key_mgmt=NONE\n' if psk == '-' else '    key_mgmt=WPA-PSK\n    psk=%s\n' % psk
    block += '}\n'
    atomic(config, original.rstrip() + '\n' + block)

def work():
    request = lock + '/request'
    with open(request) as f: words = f.read(256).split()
    os.unlink(request)
    if len(words) != 4 or words[0] != 'join' or not re.match(r'^[0-9]{1,10}$', words[1]) or not re.match(r'^(?:[0-9a-fA-F]{2}){1,32}$', words[2]) or not re.match(r'^(?:-|[0-9a-fA-F]{64})$', words[3]):
        raise RuntimeError('invalid request')
    j = {'id': int(words[1]), 'ssid': words[2], 'new': None, 'dhcp_changed': False,
         'boot': open('/proc/sys/kernel/random/boot_id').read().strip()}
    psk = words[3]
    c = Control()
    committed = False
    cleaned = True
    try:
        st = values(c.command('STATUS'))
        if st.get('wpa_state') != 'COMPLETED' or not st.get('id', '').isdigit(): raise RuntimeError('not_connected')
        j['old'] = int(st['id'])
        j['enabled'] = [int(x.split('\t')[0]) for x in c.command('LIST_NETWORKS').splitlines()[1:]
                        if x.split('\t')[0].isdigit() and '[DISABLED]' not in x]
        journal(j)
        result(j, 'switching')
        j['new'] = int(c.command('ADD_NETWORK'))
        journal(j)
        c.command('SET_NETWORK %d ssid %s' % (j['new'], j['ssid']))
        c.command('SET_NETWORK %d key_mgmt %s' % (j['new'], 'NONE' if psk == '-' else 'WPA-PSK'))
        if psk != '-': c.command('SET_NETWORK %d psk %s' % (j['new'], psk))
        c.command('SET_NETWORK %d priority 2147483647' % j['new'])
        c.command('SELECT_NETWORK %d' % j['new'])
        until = uptime() + 35
        while uptime() < until:
            st = values(c.command('STATUS'))
            if st.get('wpa_state') == 'COMPLETED' and st.get('id') == str(j['new']): break
            time.sleep(1)
        else: raise RuntimeError('authentication')
        j['dhcp_changed'] = True
        journal(j)
        dhcp_stop()
        marker = lock + '/lease'
        try: os.unlink(marker)
        except OSError: pass
        p = dhcp_start(marker)
        ip = wait_lease(p, marker)
        for n in j['enabled']:
            c.command('ENABLE_NETWORK %d' % n)
        st = values(c.command('STATUS'))
        if st.get('wpa_state') != 'COMPLETED' or st.get('id') != str(j['new']): raise RuntimeError('association_lost')
        commit(j, psk)
        committed = True
        result(j, 'ok', ip=ip)
    except Exception as e:
        if not committed:
            try: rollback(j)
            except Exception:
                cleaned = False
                sys.stderr.write('misto wifi: rollback failed; retrying; reboot restores saved configuration\n')
            reason = {'authentication': 'noassoc', 'dhcp': 'noaddress'}.get(str(e), 'bad')
            result(j, 'failed', reason)
        else:
            sys.stderr.write('misto wifi: connected and saved, result reporting failed\n')
    finally:
        psk = ''
        c.close()
        if cleaned: cleanup()

def recover():
    if not os.path.isdir(lock): return
    try:
        pid = int(open(lock + '/pid').read().strip())
        cmd = open('/proc/%d/cmdline' % pid, 'rb').read()
        state = open('/proc/%d/stat' % pid).read().split()[2]
        if b'misto-wifi-join.py' in cmd and state != 'Z': return
    except (IOError, ValueError):
        pass
    try:
        j = json.load(open(lock + '/journal'))
    except (IOError, ValueError):
        cleanup()
        return
    marker = '# echoassist-wifi-transaction-' + str(j['id'])
    saved = marker in open(config, 'rb').read().decode('latin-1')
    rebooted = j['boot'] != open('/proc/sys/kernel/random/boot_id').read().strip()
    if saved:
        c = Control()
        try: ip = values(c.command('STATUS')).get('ip_address', '0.0.0.0')
        finally: c.close()
        result(j, 'ok', ip=ip)
    else:
        if not rebooted: rollback(j)
        result(j, 'failed', 'interrupted')
    cleanup()

if __name__ == '__main__':
    try:
        if len(sys.argv) > 1 and sys.argv[1] == '--recover': recover()
        else:
            until = uptime() + 3
            while not os.path.exists(lock + '/pid') and uptime() < until: time.sleep(0.05)
            if os.path.exists(lock + '/pid'): work()
    except Exception:
        sys.stderr.write('misto wifi: transaction did not start\n')
