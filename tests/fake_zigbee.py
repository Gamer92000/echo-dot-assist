#!/usr/bin/env python3
"""zigbeed (src/zigbee/, the Echo 4's Zigbee stack) end to end on the PC: build/zigbeed-host nodes on a shared simulated
medium (tests/zbair.py: OpenThread RCPs as Amazon's answers), the coordinator's host Home Assistant's own ZHA libraries
(zigpy with zigpy-deconz, over socket:// as ZHA connects to a network coordinator; the deCONZ protocol, deconz.c).
  C (coordinator) -- R (router, zigbeed -r router) -- E (sleepy end device, zigbeed -r enddevice), E out of C's reach:
zigpy forms the network; R joins with C's permit joining (the network key from the trust center, then a link key of its
own: request, verify, confirm), announces itself, and zigpy interviews it (node descriptor, endpoints, Basic's model);
E joins through R (an update device to C, the key tunnelled back through R, held for E's polls), and zigpy interviews
it through R (a route discovery or a source route, frames held for a sleepy child); a read of an attribute through two
hops; zigpy's view of the network (Mgmt_Lqi); the coordinator restarted: the same network, devices still reached;
a leave (zigpy removes E: E leaves and looks for a network again).  Then what is rarer: APS fragmentation both ways
(a read of every Basic attribute, longer than a frame, and a write of a long string), a Green Power switch G that only
R hears commissioned through R's proxy into zigpy's sink and its protected toggle tunnelled (CCM* checked on R, the
counter kept: a replay goes nowhere), an address conflict (a frame of R's address from another EUI-64: R moves and
announces itself), a new network key from the coordinator (R and the sleepy E switch, zigpy still reads both), the
deCONZ watchdog left to run out (the host's connection ends), and a PAN ID conflict (beacons of another network on our
PAN ID: the coordinator moves, R follows).  The air's pcap goes into build/test-logs."""
import asyncio, json, os, shutil, signal, socket, struct, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from zbair import Air

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "zigbeed-host")
TCP = 17341
fails = 0


def check(cond, what):
    global fails
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: fails += 1


async def until(cond, secs):
    end = time.monotonic() + secs
    while time.monotonic() < end:
        r = cond()
        if asyncio.iscoroutine(r): r = await r
        if r: return True
        await asyncio.sleep(0.1)
    return False


class Zigbeed:
    def __init__(self, tmp, name, role, extra=()):
        self.dir = os.path.join(tmp, name); os.makedirs(self.dir, exist_ok=True)
        self.sock = os.path.join(tmp, f"{name}.rcp"); self.host = os.path.join(tmp, f"{name}.host")
        self.name, self.role, self.extra = name, role, list(extra)
        self.log = open(os.path.join(tmp, f"{name}.log"), "a"); self.p = None

    def start(self):
        self.ctl = os.path.join(os.path.dirname(self.dir), f"{self.name}.ctl")
        args = [BIN, "-r", self.role, "-R", self.sock, "-d", self.dir, "-C", self.ctl] + (["-H", self.host] if self.role == "coordinator" else []) + self.extra
        self.p = subprocess.Popen(args, stderr=self.log, stdout=self.log, env=dict(os.environ, ZIGBEED_ROTATE_MS="4000"))

    def ctl_cmd(self, line):
        c = socket.socket(socket.AF_UNIX); c.settimeout(3); c.connect(self.ctl); c.sendall(line.encode() + b"\n")
        out = b""
        while (d := c.recv(4096)): out += d
        c.close(); return out.decode()

    def stop(self):
        if self.p and self.p.poll() is None:
            self.p.send_signal(signal.SIGTERM)
            try: self.p.wait(5)
            except subprocess.TimeoutExpired: self.p.kill()

    def text(self): return open(self.log.name).read()


async def forward(host_sock, port):
    """TCP to the coordinator's unix socket, as hassmic forwards it on the Echo (radio.c)"""
    async def handle(r, w):
        try: ur, uw = await asyncio.open_unix_connection(host_sock)
        except OSError: w.close(); return
        async def pipe(a, b):
            try:
                while (d := await a.read(4096)): b.write(d); await b.drain()
            except (ConnectionError, OSError): pass
            finally:
                try: b.close()
                except Exception: pass
        await asyncio.gather(pipe(r, uw), pipe(ur, w))
    return await asyncio.start_server(handle, "127.0.0.1", port)


async def zigpy_app(tmp, port):
    import zigpy.config as zc
    from zigpy_deconz.zigbee.application import ControllerApplication
    cfg = ({
        zc.CONF_DEVICE: {zc.CONF_DEVICE_PATH: f"socket://127.0.0.1:{port}"},
        zc.CONF_DATABASE: os.path.join(tmp, "zigbee.db"),
        zc.CONF_NWK_BACKUP_ENABLED: False,
        zc.CONF_TOPO_SCAN_ENABLED: False,
        zc.CONF_NWK: {zc.CONF_NWK_CHANNEL: 15, zc.CONF_NWK_CHANNELS: [15]},
    })
    return await ControllerApplication.new(cfg, auto_form=True, start_radio=True)


# ---------------------------------------------------------------- frames of our own on the air

def mac_bcast(seq, pan, nwk_frame, src=None):
    """a MAC data frame to 0xFFFF: no source address (a GPDF) or a short one (PAN ID compressed); FCS bytes at the end"""
    if src is None: return struct.pack("<HBHH", 0x0801, seq, 0xFFFF, 0xFFFF) + nwk_frame + b"\0\0"
    return struct.pack("<HBHHH", 0x8841, seq, pan, 0xFFFF, src) + nwk_frame + b"\0\0"


def nwk_secured(key, src_nwk, src_ieee, fc, key_seq, payload, dst=0xFFFC, seq=7):
    """a NWK data frame secured with the network key as a Zigbee PRO node sends it (CCM*, level 5, the extended nonce)"""
    from cryptography.hazmat.primitives.ciphers.aead import AESCCM
    hdr = struct.pack("<HHHBBQ", 0x0008 | 1 << 9 | 1 << 12, dst, src_nwk, 1, seq, src_ieee)
    aux = bytes([0x28]) + struct.pack("<IQ", fc, src_ieee) + bytes([key_seq])
    nonce = struct.pack("<QI", src_ieee, fc) + b"\x2d"
    a = hdr + bytes([0x2d]) + aux[1:]
    ct = AESCCM(bytes(key), tag_length=4).encrypt(nonce, payload, a)
    return hdr + aux + ct


def beacon(seq, pan, src, epan):
    sf = 0x0FFF | 1 << 14
    z = bytes([0, 0x22, 0x84]) + struct.pack("<Q", epan) + b"\xff\xff\xff\x00"
    return struct.pack("<HBHH", 0x8000, seq, pan, src) + struct.pack("<H", sf) + b"\0\0" + z + b"\0\0"


def deconz_frame(seq, cmd, payload):
    f = bytes([cmd, seq, 0]) + struct.pack("<H", 5 + len(payload)) + payload
    cs = (-sum(f)) & 0xFFFF
    f += struct.pack("<H", cs)
    o = bytearray(b"\xc0")
    for b in f:
        if b == 0xC0: o += b"\xdb\xdc"
        elif b == 0xDB: o += b"\xdb\xdd"
        else: o.append(b)
    return bytes(o + b"\xc0")


async def main():
    tmp = tempfile.mkdtemp(prefix="zb")
    logs = os.path.join(ROOT, "build", "test-logs"); os.makedirs(logs, exist_ok=True)
    air = Air(os.path.join(logs, "zigbee.pcap"))
    C = Zigbeed(tmp, "C", "coordinator"); R = Zigbeed(tmp, "R", "router"); E = Zigbeed(tmp, "E", "enddevice")
    air.node("C", 0x00155F0000000001, sock=C.sock); air.node("R", 0x00155F0000000002, sock=R.sock); air.node("E", 0x00155F0000000003, sock=E.sock)
    air.node("G", 0x00155F00000000AA); air.node("X", 0x00155F00000000BB); air.node("N", 0x00155F00000000CC)
    air.link("C", "R", 230); air.link("R", "E", 210)          # E hears R only
    air.link("R", "G", 200)                                   # the Green Power switch: R only
    air.link("X", "R", 220)                                   # a stranger with R's address: R only
    air.link("N", "C", 200); air.link("N", "R", 200)          # a neighbour network's router
    await air.start()
    srv = await forward(C.host, TCP)
    app = None
    try:
        C.start()
        await until(lambda: os.path.exists(C.host), 5)
        app = await asyncio.wait_for(zigpy_app(tmp, TCP), 60)
        ni = app.state.network_info
        check(ni.channel == 15 and ni.pan_id not in (0, 0xFFFF), f"zigpy formed the network on the coordinator (channel {ni.channel}, PAN {ni.pan_id:#06x})")
        check(app.state.node_info.nwk == 0x0000 and str(app.state.node_info.ieee) == "00:15:5f:00:00:00:00:01", f"the coordinator is 0x0000 with the radio's EUI-64 ({app.state.node_info.ieee})")
        coord = app.get_device(nwk=0x0000)
        check(coord.node_desc is not None and coord.node_desc.is_coordinator, "zigpy read the coordinator's own node descriptor")
        import zigpy.types as zt
        en = await asyncio.wait_for(app.energy_scan(channels=zt.Channels.ALL_CHANNELS, duration_exp=1, count=1), 30)
        check(len(en) == 16 and all(0 <= v <= 255 for v in en.values()), f"zigpy's energy scan, channel by channel on the RCP ({en})")

        await app.permit(120)
        R.start()
        ok = await until(lambda: any(d.nwk != 0 and d.is_initialized for d in app.devices.values()), 40)
        r = next((d for d in app.devices.values() if d.nwk != 0), None)
        check(ok and r is not None, "the router joined and zigpy interviewed it")
        if r:
            check(str(r.ieee) == "00:15:5f:00:00:00:00:02" and r.node_desc and r.node_desc.is_router, f"as a router ({r.ieee}, {r.node_desc})")
            check(r.model == "Echo 4 Zigbee router" and r.manufacturer == "hassmic", f"its Basic cluster read: {r.manufacturer} {r.model}")
        rt = R.text()
        check("the network key from" in rt, "R took the network key from the trust center")
        await until(lambda: "confirmed our link key" in R.text(), 15)
        check("confirmed our link key" in R.text(), "R asked for a link key of its own and the trust center confirmed it")

        # APS fragmentation: an answer and a request longer than a frame holds (82 bytes of data)
        if r:
            # one request for every attribute (zigpy's read_attributes asks a few at a time)
            tsn = app.get_sequence()
            attrs = [0, 1, 2, 3, 4, 5, 6, 7, 0x4000, 5, 5]
            try:
                res = await asyncio.wait_for(r.request(0x0104, 0x0000, 1, 1, tsn, bytes([0x00, tsn, 0x00]) + struct.pack("<11H", *attrs)), 30)
            except Exception as ex:
                res = ex
            check(not isinstance(res, Exception) and " bytes to 0000 in " in R.text(),
                  f"a read answer of {len(attrs)} attributes, fragmented by R and put together by C: {res!r}"[:300])
            from zigpy.zcl import foundation
            import zigpy.types as zt2
            rec = foundation.Attribute(0x0010, foundation.TypeValue(0x42, zt2.CharacterString("x" * 110)))
            try:
                rsp = await asyncio.wait_for(r.endpoints[1].basic.write_attributes_raw([rec]), 30)
            except Exception as ex:
                rsp = ex
            check("in 2 blocks" in C.text() and not isinstance(rsp, Exception), f"a write of 110 bytes, fragmented by C, put together and answered by R: {rsp!r}")

        await app.permit(120)               # the router takes joiners too now (ZHA's "add device" again)
        E.start()
        ok = await until(lambda: any(str(d.ieee) == "00:15:5f:00:00:00:00:03" and d.is_initialized for d in app.devices.values()), 60)
        e = next((d for d in app.devices.values() if str(d.ieee) == "00:15:5f:00:00:00:00:03"), None)
        check(ok, "the end device joined through the router (the key tunnelled) and zigpy interviewed it through two hops")
        ct = C.text()
        check("tunnelled through its parent" in ct, "C sent E's key through R")
        if e:
            check(e.node_desc is not None and e.node_desc.is_end_device, f"an end device: {e.node_desc}")
            basic = e.endpoints[1].basic
            t0 = time.monotonic()
            res = await asyncio.wait_for(basic.read_attributes(["model", "power_source"], allow_cache=False), 30)
            check(res[0].get("model") == "hassmic end device", f"a read through R to the sleeping device ({time.monotonic() - t0:.1f} s): {res}")

        # Green Power: G commissioned through R's proxy (zigpy's sink: the proxies in commissioning mode while joining
        # is open), then a protected toggle tunnelled
        import zigpy.zgp.crypto as gpc
        from zigpy.zgp.commands import GPCommissioningPayload, GPCommissioningOptions, GPCommissioningExtendedOptions
        from zigpy.zgp.types import DeviceID, SecurityLevel, SecurityKeyType
        from zigpy.zgp.util import synthetic_ieee
        ch = app.state.network_info.channel
        src_id, gkey, gfc = 0x0155F00A, bytes(range(0x30, 0x40)), 100
        await app.permit(60)
        await until(lambda: "gp: commissioning mode" in R.text(), 10)
        check("gp: commissioning mode" in R.text(), "the sink put R's Green Power proxy into commissioning mode")
        ek, kmic = gpc.encrypt_security_key(src_id, gkey)
        comm = GPCommissioningPayload(
            device_id=DeviceID.OnOffSwitch,
            options=GPCommissioningOptions(mac_seq_num_capability=0, rx_on_capability=0, app_info_present=0, _reserved=0,
                                           pan_id_request=0, security_key_request=0, fixed_location=1, extended_options_present=1),
            extended_options=GPCommissioningExtendedOptions(security_level=SecurityLevel.Encrypted, key_type=SecurityKeyType.IndividualKey,
                                                            key_present=1, key_encrypted=1, outgoing_counter_present=1),
            security_key=zt.KeyData(ek), key_mic=zt.uint32_t(int.from_bytes(kmic, "little")), outgoing_counter=zt.uint32_t(gfc))
        gpdf = bytes([0x8C, 0x00]) + struct.pack("<I", src_id) + bytes([0xE0]) + comm.serialize()
        for i in range(3):                                    # a GPD repeats each frame
            air.inject("G", mac_bcast(40, 0, gpdf), ch); await asyncio.sleep(0.05)
        gieee = synthetic_ieee(src_id)
        await until(lambda: gieee in app.devices and "gp: %08x paired" % src_id in R.text(), 15)
        check(gieee in app.devices, f"zigpy commissioned the switch from R's commissioning notification ({gieee})")
        check("gp: %08x paired" % src_id in R.text(), "and R took it into its proxy table from the sink's pairing")
        def toggle(fc):
            hdr = bytes([0x8C, 0x38]) + struct.pack("<II", src_id, fc)
            ct, mic = gpc.encrypt_payload(src_id, fc, gkey, bytes([0x22]), header=hdr, security_level=SecurityLevel.Encrypted)
            return mac_bcast(41 + fc % 100, 0, hdr + ct + mic)
        await app.permit(0)
        await asyncio.sleep(1)
        air.inject("G", toggle(gfc + 1), ch)
        g = app.devices.get(gieee)
        await until(lambda: g is not None and g.frame_counter == gfc + 1, 10)
        check(g is not None and g.frame_counter == gfc + 1, f"a protected toggle, checked by R and tunnelled as a GP notification (counter {g.frame_counter if g else None})")
        await asyncio.sleep(2.5)                              # past the duplicate filter: the same frame again
        air.inject("G", toggle(gfc + 1), ch)
        await asyncio.sleep(1)
        air.inject("G", toggle(gfc + 2) [:-6] + b"\0\0\0\0\0\0", ch)  # a forged one: its MIC does not hold
        await asyncio.sleep(1)
        check(g is not None and g.frame_counter == gfc + 1 and "did not hold" in R.text(), "a replay and a forged one went nowhere")

        # an address conflict: a frame with R's address from another EUI-64 (secured: it is a member's)
        rn = r.nwk if r else 0
        key = app.state.network_info.network_key
        stranger = nwk_secured(key.key, rn, 0x00155F00000000BB, 5000, key.seq, bytes([0x08, 0xFF]) + struct.pack("<HHB", 0, 0x0104, 1) + b"\x01\x00")
        air.inject("X", mac_bcast(9, app.state.network_info.pan_id, stranger, src=rn), ch)
        await until(lambda: "address conflict" in R.text() and r.nwk != rn, 15)
        check("address conflict" in R.text() and r.nwk != rn, f"an address conflict: R moved from {rn:04x} to {r.nwk:04x} and announced it")
        if r:
            res = await asyncio.wait_for(r.endpoints[1].basic.read_attributes(["model"], allow_cache=False), 20)
            check(res[0].get("model") == "Echo 4 Zigbee router", "and zigpy reaches it at its new address")

        # a new network key from the coordinator (its control socket: the page's button)
        check(C.ctl_cmd("rotate").strip() == "ok", "the coordinator rotates the network key")
        await until(lambda: "switched to network key 1" in R.text() and "switched to network key 1" in E.text() and "switched to network key 1" in C.text(), 30)
        check("switched to network key 1" in C.text() and "switched to network key 1" in R.text(), "C and R switched to the new key")
        check("switched to network key 1" in E.text(), "and the sleepy end device too (the broadcasts held for its polls)")
        if r:
            res = await asyncio.wait_for(r.endpoints[1].basic.read_attributes(["manufacturer"], allow_cache=False), 20)
            check(res[0].get("manufacturer") == "hassmic", "the router answers under the new key")
        if e:
            res = await asyncio.wait_for(e.endpoints[1].basic.read_attributes(["power_source"], allow_cache=False), 40)
            check(res[0].get("power_source") is not None, "and the end device too")

        # the network as zigpy sees it from the coordinator
        rsp = await asyncio.wait_for(coord.zdo.Mgmt_Lqi_req(0), 10)
        check(rsp[0] == 0 and any(str(n.ieee) == "00:15:5f:00:00:00:00:02" for n in rsp[1].NeighborTableList), f"Mgmt_Lqi: the coordinator's neighbors list the router ({rsp[1] if rsp[0] == 0 else rsp})")

        # the coordinator restarted: its network and devices kept
        await app.shutdown(); app = None
        C.stop(); C.start()
        await until(lambda: os.path.exists(C.host), 5)
        await asyncio.sleep(1)
        app = await asyncio.wait_for(zigpy_app(tmp, TCP), 60)
        check(app.state.network_info.pan_id == ni.pan_id and app.state.network_info.channel == 15, "after the coordinator's restart: the same network")
        r2 = app.get_device(ieee=r.ieee) if r else None
        if r2:
            res = await asyncio.wait_for(r2.endpoints[1].basic.read_attributes(["manufacturer"], allow_cache=False), 20)
            check(res[0].get("manufacturer") == "hassmic", "and the router still answers")

        # ZHA's backup restored onto a coordinator that lost everything (a migration, a new Echo): the same network
        backup = await asyncio.wait_for(app.backups.create_backup(load_devices=True), 30)
        await app.shutdown(); app = None
        C.stop()
        os.unlink(os.path.join(C.dir, "zb.state"))
        C.start()
        await until(lambda: os.path.exists(C.host), 5)
        await asyncio.sleep(1)
        import zigpy.config as zc
        from zigpy_deconz.zigbee.application import ControllerApplication
        bare = ControllerApplication({zc.CONF_DEVICE: {zc.CONF_DEVICE_PATH: f"socket://127.0.0.1:{TCP}"},
                                      zc.CONF_DATABASE: os.path.join(tmp, "restore.db")})
        await asyncio.wait_for(bare.connect(), 30)
        await asyncio.wait_for(bare.backups.restore_backup(backup, create_new=False), 60)
        await bare.disconnect()
        app = await asyncio.wait_for(zigpy_app(tmp, TCP), 60)
        ni2 = app.state.network_info
        check(ni2.pan_id == backup.network_info.pan_id and ni2.extended_pan_id == backup.network_info.extended_pan_id and
              ni2.network_key.key == backup.network_info.network_key.key and ni2.network_key.tx_counter >= backup.network_info.network_key.tx_counter,
              f"ZHA's backup restored onto a wiped coordinator: the same network, key and a counter past the old one ({ni2.network_key.tx_counter})")
        if r:
            r3 = app.get_device(ieee=r.ieee)
            res = await asyncio.wait_for(r3.endpoints[1].basic.read_attributes(["manufacturer"], allow_cache=False), 30)
            check(res[0].get("manufacturer") == "hassmic", "and the router answers the restored coordinator")

        # a leave: the end device asked to go
        if e:
            e2 = app.get_device(ieee=e.ieee)
            await asyncio.wait_for(app.remove(e2.ieee), 30)
            await until(lambda: "asked to leave" in E.text(), 20)
            check("asked to leave" in E.text(), "zigpy removed the end device: it left the network")

        # the deCONZ watchdog: a host that sets 1 s and never feeds it loses its connection (a ConBee would reset)
        pan = app.state.network_info.pan_id; epan = app.state.network_info.extended_pan_id
        await app.shutdown(); app = None
        await asyncio.sleep(0.5)
        rd, wr = await asyncio.open_unix_connection(C.host)
        wr.write(deconz_frame(1, 0x0B, struct.pack("<HBI", 5, 0x26, 1))); await wr.drain()
        t0 = time.monotonic()
        try:
            while await asyncio.wait_for(rd.read(1024), 5): pass
            gone = True
        except asyncio.TimeoutError:
            gone = False
        check(gone and time.monotonic() - t0 < 4 and "watchdog ran out" in C.text(), f"the watchdog ran out: the host's connection ended ({time.monotonic() - t0:.1f} s)")
        wr.close()

        # a PAN ID conflict: another network's router answers beacon requests with our PAN ID (three over 30 s)
        other = 0x1122334455667788
        for i in range(4):
            air.inject("N", beacon(60 + i, pan, 0x4321, other), ch)
            if i < 3: await asyncio.sleep(11)
        await until(lambda: "PAN ID %04x ->" % pan in C.text() and "PAN ID %04x ->" % pan in R.text(), 15)
        check("PAN ID conflict" in C.text() and "PAN ID %04x ->" % pan in C.text(), "the coordinator saw the conflict and moved to another PAN ID")
        check("PAN ID %04x ->" % pan in R.text(), "the router followed the network update")
    finally:
        if app: await app.shutdown()
        for z in (C, R, E): z.stop()
        srv.close(); await air.stop()
        for z in (C, R, E):
            shutil.copy(z.log.name, os.path.join(logs, f"zigbee-{z.name}.log"))
        if fails:
            for z in (C, R, E): print(f"--- {z.name}:\n" + "".join(z.text().splitlines(True)[-40:]))
    print("FAILED" if fails else "all good")
    sys.exit(1 if fails else 0)


asyncio.run(main())
