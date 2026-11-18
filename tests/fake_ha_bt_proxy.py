#!/usr/bin/env python3
"""The Bluetooth proxy switched on the settings page (issue #152), against build/hassmic-host with ble.c talking H4 to
the controller of tests/fake_improv.py on a pseudo terminal (HASSMIC_BT_DEV), which here also scans: on by default,
Home Assistant (aioesphomeapi) finds the proxy in the device info, subscribes, and the controller's advertisements
reach it, and a tag connects; switched off, the links close, the scan and the tag's link end, the device info names no
proxy, and subscriptions, slots and connection requests are refused, while Bluetooth audio's entity and the setup from
a phone (Improv advertising) stay; on again, all of it is back.  Issue #167: a link comes up at 30-50 ms and is moved to
ESPHome's 8.75-11.25 ms once up, unless the device asked for parameters of its own; HA's GATT requests pause the scan,
which comes back a second after the last."""
import asyncio, struct, tempfile, threading
from aioesphomeapi import APIClient, BluetoothScannerMode, BluetoothScannerState
from esphome_common import check, until, sync, run
from fake_improv import Controller, CONN

PORTS = (17311, 17312, 17313, 17314, 17315)
FLAGS = 1 | 2 | 4 | 8 | 32 | 64                 # proto_esphome.c BLE_FEATURES
TAG = 0x112233445566                            # a tag near the Echo
OP_SCAN_ENABLE, OP_CONNECT, OP_DISCONNECT, OP_CONN_UPDATE = 0x200c, 0x200d, 0x0406, 0x2013
VALUE = b"\x2a\x07"                              # what the tag's characteristic reads
ECHO = "C4:95:00:56:95:C4"                       # the Echo's Bluetooth address


class Scanner(Controller):
    """The controller, keeping every command: LE Set Scan Enable says whether it scans.  A connection the Echo asks for
    comes up at once with the tag as the peer (handle CONN), its MTU request answered, then writes and reads (the value
    VALUE); a disconnect is done at once.  ask: the tag asks for 30-50 ms itself (L2CAP Connection Parameter Update)"""
    def __init__(self):
        self.ops, self.ask = [], False
        super().__init__()

    def command(self, op, par):
        self.ops.append((op, bytes(par)))
        super().command(op, par)
        if op == OP_CONNECT: threading.Thread(target=self.central, daemon=True).start()
        elif op == OP_DISCONNECT: self.disconnect()

    def central(self):                          # LE Connection Complete, role central, then ATT Exchange MTU
        self.event(0x3e, bytes([1, 0]) + struct.pack("<HBB", CONN, 0, 0) + TAG.to_bytes(6, "little") + struct.pack("<HHHB", 24, 0, 400, 0))
        if self.ask:
            l2 = struct.pack("<HH", 12, 5) + bytes([0x12, 1]) + struct.pack("<HHHHH", 8, 24, 40, 0, 500)
            self.send(bytes([2]) + struct.pack("<HH", CONN | 0x2000, len(l2)) + l2)
        try: pdu = self.att.get(timeout=5)
        except Exception: return
        while True:
            if not pdu: return
            rsp = {0x02: b"\x03" + struct.pack("<H", 23), 0x12: b"\x13", 0x0a: b"\x0b" + VALUE}.get(pdu[0])
            if rsp:
                l2 = struct.pack("<HH", len(rsp), 4) + rsp
                self.send(bytes([2]) + struct.pack("<HH", CONN | 0x2000, len(l2)) + l2)
            try: pdu = self.att.get(timeout=30)
            except Exception: return

    def scanning(self):
        return ([p[0] for op, p in self.ops if op == OP_SCAN_ENABLE] or [0])[-1] == 1

    def advert(self, addr, data, rssi=-60):     # LE Advertising Report: one ADV_IND
        rep = bytes([0x02, 1, 0, 0]) + addr.to_bytes(6, "little") + bytes([len(data)]) + data + struct.pack("b", rssi)
        self.event(0x3e, rep)


ctl = Scanner()
ADV = bytes([2, 0x01, 0x06, 5, 0x09]) + b"Tag1"     # flags, name


async def ha():
    cli = APIClient("127.0.0.1", PORTS[0], None)
    await asyncio.wait_for(cli.connect(login=True), 5)
    return cli


async def proxy_works(cli, what):
    advs, free = [], []
    cli.subscribe_bluetooth_le_raw_advertisements(lambda r: advs.extend(a.address for a in r.advertisements))
    cli.subscribe_bluetooth_connections_free(lambda f, l, a: free.append((f, l)))
    check(await until(ctl.scanning), f"{what}: Home Assistant subscribed, the controller scans")
    ctl.advert(TAG, ADV)
    check(await until(lambda: TAG in advs), f"{what}: the controller's advertisement reaches Home Assistant")
    check(await until(lambda: free) and free[0] == (3, 3), f"{what}: 3 connection slots free of 3: {free}")


async def main(e):
    check(await until(lambda: "bluetooth: controller ready" in e.log(), 10), "the controller came up")
    check(e.pv("bluetooth_proxy") == 1, "Bluetooth proxy on by default")
    cli = await ha()
    info = await cli.device_info()
    check(info.bluetooth_proxy_feature_flags == FLAGS and info.legacy_bluetooth_proxy_version == 5 and info.bluetooth_mac_address.upper() == ECHO,
          f"device info: the proxy's features ({info.bluetooth_proxy_feature_flags}) and the Bluetooth address ({info.bluetooth_mac_address})")
    caps = await cli.device_capabilities()
    check(caps.bluetooth_proxy.feature_flags == FLAGS and caps.bluetooth_proxy.mac_address.upper() == ECHO,
          f"capabilities (API 1.15): the same features and address ({caps.bluetooth_proxy.feature_flags}, {caps.bluetooth_proxy.mac_address})")
    await proxy_works(cli, "on")
    up = []
    try: await cli.bluetooth_device_connect(TAG, lambda c, m, err: up.append((c, m, err)), timeout=5, feature_flags=FLAGS, address_type=0)
    except Exception as x: up.append(x)
    check(up[:1] == [(True, 23, 0)], f"a tag connects through the proxy: {up}")
    par = [p for op, p in ctl.ops if op == OP_CONNECT][-1]
    check(struct.unpack("<HH", par[13:17]) == (24, 40), f"connected at 30-50 ms: {struct.unpack('<HH', par[13:17])}")
    updates = lambda since=0: [struct.unpack("<HH", p[2:6]) for op, p in ctl.ops[since:] if op == OP_CONN_UPDATE]
    check(await until(lambda: updates() == [(7, 9)], 3), f"once up, moved to 8.75-11.25 ms, as ESPHome's proxy: {updates()}")

    # ---- issue #167: HA's requests pause the scan (gatt_busy), a second after the last it scans again
    check(await until(ctl.scanning), "connected: scanning again")
    states = []
    cli.subscribe_bluetooth_scanner_state(lambda s: states.append(s.state))
    ops = len(ctl.ops)
    await cli.bluetooth_gatt_write(TAG, 0x10, b"\x01", True, timeout=5)      # raises unless answered
    check(await cli.bluetooth_gatt_read(TAG, 0x12, timeout=5) == VALUE, "a read is answered")
    check(await until(lambda: [p[0] for op, p in ctl.ops[ops:] if op == OP_SCAN_ENABLE][:1] == [0], 3), "the scan pauses for HA's requests")
    ops, n = len(ctl.ops), len(states)
    await cli.bluetooth_gatt_write(TAG, 0x10, b"\x01", True, timeout=5)      # paused again for a second from here
    cli.bluetooth_scanner_set_mode(BluetoothScannerMode.ACTIVE)     # answered with the state, during the pause
    check(await until(lambda: len(states) > n, 3) and all(s == BluetoothScannerState.RUNNING for s in states[n:]),
          f"asked during the pause, HA hears the scanner running, not idle: {states[n:]}")
    check(not ctl.scanning() and not [p for op, p in ctl.ops[ops:] if op == OP_SCAN_ENABLE and p[0] == 1],
          "and the scan was paused all along")
    for _ in range(4):                          # a desk's loop: a request every ~150 ms keeps it paused (each restarts the 1 s hold)
        await asyncio.sleep(0.15)
        await cli.bluetooth_gatt_write(TAG, 0x10, b"\x01", True, timeout=5)
        check(not ctl.scanning(), "requests keep coming: still paused")
    check(await until(ctl.scanning, 3), "the requests ended: scanning again")

    # ---- off on the page: the links close, the scan and the tag's link end with the last client
    ops = len(ctl.ops)
    r = e.page.set(bluetooth_proxy="off")
    check(r.get("applied") == 1 and e.conf().get("bluetooth_proxy") == "off", f"switched off on the page, kept in the settings file: {r}")
    check(await until(lambda: not ctl.scanning()), "the links closed: the controller stopped scanning")
    check(await until(lambda: any(op == OP_DISCONNECT and p[:2] == struct.pack("<H", CONN) for op, p in ctl.ops[ops:])), "the tag's link is closed")
    try: await cli.disconnect()
    except Exception: pass
    cli = await ha()
    info = await cli.device_info()
    check(info.bluetooth_proxy_feature_flags == 0 and info.legacy_bluetooth_proxy_version == 0 and not info.bluetooth_mac_address,
          f"device info: no proxy, no Bluetooth address ({info.bluetooth_proxy_feature_flags}, {info.bluetooth_mac_address!r})")
    caps = await cli.device_capabilities()
    check(caps.bluetooth_proxy.feature_flags == 0 and not caps.bluetooth_proxy.mac_address, "capabilities: no proxy either")
    es, _ = await cli.list_entities_services()
    check(any(x.name == "Bluetooth pairing" for x in es), "Bluetooth audio from phones stays: its \"Bluetooth pairing\" switch is listed")
    check(await until(lambda: ctl.adv["on"], 8), "the setup from a phone stays: Improv advertises (not set up, offline)")
    advs, free = [], []
    cli.subscribe_bluetooth_le_raw_advertisements(lambda r: advs.extend(a.address for a in r.advertisements))
    cli.subscribe_bluetooth_connections_free(lambda f, l, a: free.append((f, l)))
    await sync(cli)
    ops = len(ctl.ops)
    try: await cli.bluetooth_device_connect(TAG, lambda *a: None, timeout=1)
    except Exception: pass
    ctl.advert(TAG, ADV)
    await asyncio.sleep(1.5)                    # what must not happen
    check(not ctl.scanning() and not advs, f"a subscription is refused: no scan, no advertisements ({advs})")
    check(not free, f"connection slots: no answer ({free})")
    check(not [op for op, _ in ctl.ops[ops:] if op in (OP_CONNECT, OP_SCAN_ENABLE)], "a connection request is refused: nothing asked of the controller")
    st, _, text = e.page.call("GET", "/api/export")
    check(st == 200 and "\nbluetooth_proxy=off\n" in text.decode(), "the export has it")

    # ---- on again
    e.page.set(bluetooth_proxy="on")
    try: await cli.disconnect()
    except Exception: pass
    await asyncio.sleep(0.3)
    cli = await ha()
    info = await cli.device_info()
    check(info.bluetooth_proxy_feature_flags == FLAGS and info.bluetooth_mac_address.upper() == ECHO, "on again: the device info names the proxy")
    await proxy_works(cli, "on again")             # 3 of 3 free: the tag's slot was given back

    # ---- a device that asks for its own connection parameters gets them, and never ours
    ctl.ask, ops, up = True, len(ctl.ops), []
    try: await cli.bluetooth_device_connect(TAG, lambda c, m, err: up.append(c), timeout=5, feature_flags=FLAGS, address_type=0)
    except Exception as x: up.append(x)
    check(up[:1] == [True], f"the tag connects again, asking for 30-50 ms itself: {up}")
    check(await until(lambda: updates(ops), 3) and updates(ops) == [(24, 40)], f"it gets what it asked for, nothing faster: {updates(ops)}")
    await asyncio.sleep(0.5)
    check(updates(ops) == [(24, 40)], f"and keeps it: {updates(ops)}")
    await cli.disconnect()


mac = tempfile.mktemp()                         # idme's bt_mac: what read_bdaddr takes the address from
open(mac, "w").write(ECHO.replace(":", "").lower() + "\n")
# offline (no such file) and not set up: Improv advertises after 1 s (improv.c's times: not set up, lost, auth, provisioned)
run(main, PORTS, {"HASSMIC_BT_DEV": ctl.path, "HASSMIC_BT_MAC": mac, "HASSMIC_FAKE_ONLINE": mac + ".online",
                  "HASSMIC_IMPROV_TIMES": "1 1 4 2"}, log=True)
