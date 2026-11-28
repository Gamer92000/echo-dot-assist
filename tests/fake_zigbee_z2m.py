#!/usr/bin/env python3
"""zigbeed (src/zigbee/) against Zigbee2MQTT's library, zigbee-herdsman (tests/zigbee-js: npm ci there; skipped
without node or the package), on the shared simulated medium (tests/zbair.py):
  1. its deconz adapter as the coordinator's host: Zigbee2MQTT on the Echo as on a ConBee: it starts the network,
     opens joining, a zigbeed router joins and is interviewed (its Basic cluster read), then read again;
  2. its own Zigbee stack, zigbee-on-host (the zoh adapter, an RCP of the air's) as the coordinator and trust center,
     written by others: zigbeed joins it as a router (association, the network key, a link key of its own requested,
     verified and confirmed), announces itself and is interviewed.  A stack that is not ours accepting every frame of
     zigbeed's (security, NWK, APS, ZDO, ZCL) is what holds the formats to the spec."""
import asyncio, json, os, shutil, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from zbair import Air

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
JS = os.path.join(ROOT, "tests", "zigbee-js")
BIN = os.path.join(ROOT, "build", "zigbeed-host")
fails = 0


def check(cond, what):
    global fails
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: fails += 1


async def forward(path, port):
    async def handle(r, w):
        try: ur, uw = await asyncio.open_unix_connection(path)
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


async def herdsman(adapter, port, folder, secs, log):
    p = await asyncio.create_subprocess_exec("node", os.path.join(JS, "z2m.mjs"), adapter, str(port), folder, str(secs),
                                             cwd=JS, stdout=asyncio.subprocess.PIPE, stderr=log)
    return p


async def events(p):
    out = []
    async for line in p.stdout:
        try: out.append(json.loads(line))
        except ValueError: pass
    await p.wait()
    return out


async def deconz(tmp, logs):
    print("--- Zigbee2MQTT's deconz adapter on zigbeed's coordinator")
    air = Air(os.path.join(logs, "zigbee-z2m-deconz.pcap"))
    air.node("C", 0x00155F0000000001, sock=os.path.join(tmp, "C.rcp")); air.node("R", 0x00155F0000000002, sock=os.path.join(tmp, "R.rcp"))
    air.link("C", "R", 230)
    await air.start()
    cl = open(os.path.join(logs, "zigbee-z2m-C.log"), "w"); rl = open(os.path.join(logs, "zigbee-z2m-R.log"), "w")
    c = subprocess.Popen([BIN, "-r", "coordinator", "-R", os.path.join(tmp, "C.rcp"), "-d", os.path.join(tmp, "Cd"), "-H", os.path.join(tmp, "C.host")], stderr=cl)
    os.makedirs(os.path.join(tmp, "Cd"), exist_ok=True); os.makedirs(os.path.join(tmp, "Rd"), exist_ok=True)
    await asyncio.sleep(1)
    srv = await forward(os.path.join(tmp, "C.host"), 17371)
    p = await herdsman("deconz", 17371, os.path.join(tmp, "z2m1"), 30, open(os.path.join(logs, "zigbee-z2m-deconz.log"), "w"))
    await asyncio.sleep(8)
    r = subprocess.Popen([BIN, "-r", "router", "-R", os.path.join(tmp, "R.rcp"), "-d", os.path.join(tmp, "Rd")], stderr=rl)
    ev = await events(p)
    for x in (r, c): x.terminate(); x.wait(5)
    srv.close(); await air.stop()
    st = next((e for e in ev if e["event"] == "started"), None)
    check(st is not None and st["network"]["channel"] == 21 and st["network"]["panID"] == 0x1a62, f"Zigbee2MQTT started the network on zigbeed ({st and st['network']}, {st and st['coordinator'].get('type')})")
    iv = [e for e in ev if e["event"] == "interview" and e["status"] == "successful"]
    check(any(e["ieee"] == "0x00155f0000000002" and e["model"] == "Echo 4 Zigbee router" and e["type"] == "Router" for e in iv),
          f"a zigbeed router joined through it and was interviewed ({iv})")
    rd = [e for e in ev if e["event"] == "read"]
    check(any(e.get("manufacturerName") == "hassmic" for e in rd), f"and read again afterwards ({rd or [e for e in ev if e['event'] == 'read_failed']})")
    if fails: print("".join(open(os.path.join(logs, "zigbee-z2m-C.log")).readlines()[-20:]))


async def zoh(tmp, logs):
    print("--- zigbeed's router on zigbee-on-host, Zigbee2MQTT's own stack, as the trust center")
    air = Air(os.path.join(logs, "zigbee-z2m-zoh.pcap"))
    air.node("Z", 0x00124B0000000AAA, tcp=17372); air.node("R", 0x00155F0000000002, sock=os.path.join(tmp, "R2.rcp"))
    air.node("E", 0x00155F0000000003, sock=os.path.join(tmp, "E2.rcp"))
    air.link("Z", "R", 230); air.link("R", "E", 210)          # the end device hears our router only
    await air.start()
    os.makedirs(os.path.join(tmp, "R2d"), exist_ok=True)
    p = await herdsman("zoh", 17372, os.path.join(tmp, "z2m2"), 50, open(os.path.join(logs, "zigbee-z2m-zoh.log"), "w"))
    await asyncio.sleep(6)
    rl = open(os.path.join(logs, "zigbee-z2m-zoh-R.log"), "w")
    r = subprocess.Popen([BIN, "-r", "router", "-R", os.path.join(tmp, "R2.rcp"), "-d", os.path.join(tmp, "R2d")], stderr=rl)
    await asyncio.sleep(14)
    os.makedirs(os.path.join(tmp, "E2d"), exist_ok=True)
    el = open(os.path.join(logs, "zigbee-z2m-zoh-E.log"), "w")
    e = subprocess.Popen([BIN, "-r", "enddevice", "-R", os.path.join(tmp, "E2.rcp"), "-d", os.path.join(tmp, "E2d")], stderr=el)
    ev = await events(p)
    for x in (e, r): x.terminate(); x.wait(5)
    await air.stop()
    rt = open(os.path.join(logs, "zigbee-z2m-zoh-R.log")).read()
    check("the network key from" in rt, "zigbeed took the network key from zigbee-on-host's trust center")
    check("confirmed our link key" in rt, "asked it for a link key of its own, verified it, and the trust center confirmed it")
    iv = [e for e in ev if e["event"] == "interview" and e["status"] == "successful"]
    check(any(e["model"] == "Echo 4 Zigbee router" and e["manufacturer"] == "hassmic" for e in iv), f"zigbee-on-host interviewed zigbeed ({iv})")
    check(any(e["event"] == "announce" for e in ev), "zigbeed's device announce reached it")
    et = open(os.path.join(logs, "zigbee-z2m-zoh-E.log")).read()
    check("the network key from" in et, "an end device that hears only zigbeed's router joined through it: the key tunnelled by zigbee-on-host, passed on by our router")
    check(any(e["model"] == "hassmic end device" for e in iv), "zigbee-on-host interviewed it through our router (relayed both ways, held for its polls)")
    rd = [e for e in ev if e["event"] == "read"]
    check(sum(1 for e in rd if e.get("manufacturerName") == "hassmic") >= 2, f"and read both again ({rd})")
    if fails: print(rt[-3000:])


async def main():
    if not shutil.which("node") or not os.path.isdir(os.path.join(JS, "node_modules", "zigbee-herdsman")):
        if os.environ.get("CI"): print("FAIL node or tests/zigbee-js/node_modules missing (npm ci in tests/zigbee-js)"); sys.exit(1)
        print("skipped: node or tests/zigbee-js/node_modules missing (npm ci in tests/zigbee-js)"); sys.exit(0)
    tmp = tempfile.mkdtemp(prefix="zbz")
    logs = os.path.join(ROOT, "build", "test-logs"); os.makedirs(logs, exist_ok=True)
    await deconz(tmp, logs)
    await zoh(tmp, logs)
    print("FAILED" if fails else "all good")
    sys.exit(1 if fails else 0)


asyncio.run(main())
