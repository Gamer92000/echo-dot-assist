import importlib.util
import json
import os
import tempfile

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location('misto_join', os.path.join(root, 'misto-wifi-join.py'))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

class FakeControl:
    fail_association = False
    selected = 0
    commands = []
    def command(self, cmd):
        self.commands.append(cmd.split(' psk ')[0])
        if cmd == 'STATUS':
            active = 0 if self.fail_association else self.selected
            return 'wpa_state=COMPLETED\nid=%d\nip_address=192.0.2.10' % active
        if cmd == 'LIST_NETWORKS': return 'network id / ssid / bssid / flags\n0\told\tany\t[CURRENT]'
        if cmd == 'ADD_NETWORK': return '1'
        if cmd.startswith('SELECT_NETWORK '):
            type(self).selected = int(cmd.split()[1])
        return 'OK'
    def close(self): pass

with tempfile.TemporaryDirectory() as d:
    m.out, m.lock, m.config = d, d + '/lock', d + '/wpa.conf'
    m.Control = FakeControl
    m.dhcp_stop = lambda: None
    m.dhcp_start = lambda marker: object()
    m.wait_lease = lambda p, marker: '192.0.2.20'
    clock = [0]
    m.uptime = lambda: clock[0]
    m.time.sleep = lambda n: clock.__setitem__(0, clock[0] + n)
    original = 'ctrl_interface=/tmp/wpa\nnetwork={\n    ssid=6f6c64\n}\n'
    psk = 'a' * 64
    def prepare():
        with open(m.config, 'w') as f: f.write(original)
        os.mkdir(m.lock)
        with open(m.lock + '/request', 'w') as f: f.write('join 123 6e6577 %s\n' % psk)
        FakeControl.selected = 0
        FakeControl.commands = []
        clock[0] = 0
    prepare()
    FakeControl.fail_association = True
    m.work()
    assert open(m.config).read() == original
    assert 'SELECT_NETWORK 0' in FakeControl.commands
    assert 'failed' in open(m.out + '/result').read()
    assert not os.path.exists(m.lock)
    prepare()
    FakeControl.fail_association = False
    m.wait_lease = lambda p, marker: (_ for _ in ()).throw(RuntimeError('dhcp'))
    m.work()
    assert open(m.config).read() == original
    assert 'SELECT_NETWORK 0' in FakeControl.commands
    assert 'failed' in open(m.out + '/result').read()
    assert not os.path.exists(m.lock)
    prepare()
    FakeControl.fail_association = False
    m.wait_lease = lambda p, marker: '192.0.2.20'
    m.work()
    saved = open(m.config).read()
    assert original.rstrip() in saved and 'ssid=6e6577' in saved and psk in saved
    assert '123 ok 6e6577 192.0.2.20 1' in open(m.out + '/result').read()
    assert not os.path.exists(m.lock)
    prepare()
    os.unlink(m.lock + '/request')
    with open(m.lock + '/pid', 'w') as f: f.write('99999999')
    j = {'id': 123, 'ssid': '6e6577', 'old': 0, 'new': 1,
         'enabled': [0], 'dhcp_changed': True,
         'boot': open('/proc/sys/kernel/random/boot_id').read().strip()}
    with open(m.lock + '/journal', 'w') as f: json.dump(j, f)
    m.recover()
    assert open(m.config).read() == original
    assert 'SELECT_NETWORK 0' in FakeControl.commands
    assert 'failed' in open(m.out + '/result').read()
    assert not os.path.exists(m.lock)
    print('PASS authentication and DHCP rollback, commit, interrupted-worker recovery')
