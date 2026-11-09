#!/usr/bin/env python3
"""The Bluetooth proxy switched on the settings page (issue #152), against build/hassmic-host with ble.c talking H4 to
the controller of tests/fake_improv.py on a pseudo terminal (HASSMIC_BT_DEV), which here also scans: on by default,
Home Assistant (aioesphomeapi) finds the proxy in the device info, subscribes, and the controller's advertisements
reach it, and a tag connects; switched off, the links close, the scan and the tag's link end, the device info names no
proxy, and subscriptions, slots and connection requests are refused, while Bluetooth audio's entity and the setup from
a phone (Improv advertising) stay; on again, all of it is back."""
import asyncio, struct, tempfile, threading
from aioesphomeapi import APIClient
from esphome_common import check, until, sync, run
from fake_improv import Controller, CONN

PORTS = (17311, 17312, 17313, 17314, 17315)
FLAGS = 1 | 2 | 4 | 8 | 32 | 64                 # proto_esphome.c BLE_FEATURES
TAG = 0x112233445566                            # a tag near the Echo
OP_SCAN_ENABLE, OP_CONNECT, OP_DISCONNECT = 0x200c, 0x200d, 0x0406
ECHO = "C4:95:00:56:95:C4"                       # the Echo's Bluetooth address


class Scanner(Controller):
    """The controller, keeping every command: LE Set Scan Enable says whether it scans.  A connection the Echo asks for
    comes up at once with the tag as the peer (handle CONN), its MTU request answered; a disconnect is done at once"""
    def __init__(self):
        self.ops = []
        super().__init__()

    def command(self, op, par):
        self.ops.append((op, bytes(par)))
        super().command(op, par)
        if op == OP_CONNECT: threading.Thread(target=self.central, daemon=True).start()
        elif op == OP_DISCONNECT: self.disconnect()

    def central(self):                          # LE Connection Complete, role central, then ATT Exchange MTU
        self.event(0x3e, bytes([1, 0]) + struct.pack("<HBB", CONN, 0, 0) + TAG.to_bytes(6, "little") + struct.pack("<HHHB", 24, 0, 400, 0))
        try: pdu = self.att.get(timeout=5)
        except Exception: return
        if pdu[:1] == b"\x02":
            l2 = struct.pack("<HH", 3, 4) + b"\x03" + struct.pack("<H", 23)
            self.send(bytes([2]) + struct.pack("<HH", CONN | 0x2000, len(l2)) + l2)

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
    await proxy_works(cli, "on")
    up = []
    try: await cli.bluetooth_device_connect(TAG, lambda c, m, err: up.append((c, m, err)), timeout=5, feature_flags=FLAGS, address_type=0)
    except Exception as x: up.append(x)
    check(up[:1] == [(True, 23, 0)], f"a tag connects through the proxy: {up}")

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
    await cli.disconnect()


mac = tempfile.mktemp()                         # idme's bt_mac: what read_bdaddr takes the address from
open(mac, "w").write(ECHO.replace(":", "").lower() + "\n")
# offline (no such file) and not set up: Improv advertises after 1 s (improv.c's times: not set up, lost, auth, provisioned)
run(main, PORTS, {"HASSMIC_BT_DEV": ctl.path, "HASSMIC_BT_MAC": mac, "HASSMIC_FAKE_ONLINE": mac + ".online",
                  "HASSMIC_IMPROV_TIMES": "1 1 4 2"}, log=True)
