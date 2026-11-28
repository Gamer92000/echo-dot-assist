#!/usr/bin/env python3
"""The Thread and Zigbee radio (radio.c; docs/re-thread.md) against build/hassmic-host: root's side played by the real
rcpbridge (build/rcpbridge-host) in front of a stand-in RCP (tests/fake_rcp.py), and by a stand-in for our otbr-agent's
REST API on its unix socket.  The remote radio forwarded to Home Assistant's address only (none before it connects,
another address refused until the page names it), one host at a time, a fresh RCP for each; the border router's REST
forwarded to Home Assistant alone and only in that role; the page's status (never the network's key) and its actions
(a new network, a dataset sealed and joined, leaving), each as the border router's REST is asked; state/config as
root's main.sh reads it."""
import asyncio, json, os, socket, subprocess, sys, tempfile, threading, time
from http.server import BaseHTTPRequestHandler, HTTPServer
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from esphome_common import check, until, home_assistant, run
from fake_rcp import hdlc, frames

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT = 17241, 17242, 17243, 17244, 17245
REST, RCP, ZBP = 17246, 17247, 17248
RDIR = tempfile.mkdtemp()
RCPLOG = os.path.join(RDIR, "rcp.log")
KEY = "00112233445566778899aabbccddeeff"
DATASET = bytes.fromhex("0e080000000000010000000300000f35060004001fffe00208dead00beef00cafe0708fd000db800a00000"
                        "0510" + KEY + "030f4f70656e5468726561642d31323334010212340410d6ed4ba1e0d3e88e4b30b2b6bbc3c4b70c0402a0f7f8")

# ---------------------------------------------------------------- otbr-agent's REST, as radio.c asks it
rest_log = []
node = {"state": "leader", "networkName": "OpenThread-1234", "extPanId": "dead00beef00cafe", "routerCount": 3, "baId": "7f"}

class Rest(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def address_string(self): return "unix"
    def reply(self, code, body=b""):
        self.send_response(code); self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(body)))
        self.end_headers(); self.wfile.write(body)
    def body(self): n = int(self.headers.get("Content-Length") or 0); return self.rfile.read(n).decode() if n else ""
    def do_GET(self):
        rest_log.append(("GET", self.path, None, None))
        if self.path == "/node": self.reply(200, json.dumps(node).encode())
        elif self.path == "/node/state": self.reply(200, json.dumps(node["state"]).encode())
        elif self.path == "/node/dataset/active": self.reply(200, json.dumps({"channel": 15, "panId": 4660, "networkKey": KEY, "networkName": node["networkName"]}).encode())
        else: self.reply(404)
    def do_DELETE(self): rest_log.append(("DELETE", self.path, None, None)); self.reply(200)
    def do_PUT(self):
        rest_log.append(("PUT", self.path, self.headers.get("Content-Type"), self.body()))
        self.reply(200 if self.path == "/node/state" else 201)
    def do_POST(self):
        b = self.body(); rest_log.append(("POST", self.path, self.headers.get("Content-Type"), b))
        if self.path == "/node/ba-epskc/key":      # ot-br-posix's ephemeral key: lifetime (ms), port; the code as "tap"
            self.reply(200, json.dumps({"tap": "123456789", "port": json.loads(b).get("port", 0)}).encode())
        else: self.reply(405)                      # as ot-br-posix: /node/state takes PUT

class UnixHTTP(HTTPServer):
    address_family = socket.AF_UNIX
    def server_bind(self):
        if os.path.exists(self.server_address): os.unlink(self.server_address)
        self.socket.bind(self.server_address); self.server_name = "unix"; self.server_port = 0


def connect(port, src="127.0.0.1"):
    s = socket.socket(); s.settimeout(3); s.bind((src, 0)); s.connect(("127.0.0.1", port)); return s


def closed(s, secs=2.0):
    """the other end closed the connection (a refusal): recv answers b"" """
    s.settimeout(secs)
    try: return s.recv(1) == b""
    except (ConnectionResetError, BrokenPipeError): return True
    except socket.timeout: return False


def spinel_version(s):
    """PROP_VALUE_GET NCP_VERSION over the forwarded radio: the RCP's answer"""
    s.sendall(hdlc(bytes([0x81, 2, 2]))); buf = bytearray(); end = time.monotonic() + 3
    while time.monotonic() < end:
        try: d = s.recv(4096)
        except socket.timeout: break
        if not d: break
        buf += d
        for f in frames(buf):
            if f[:3] == bytes([0x81, 6, 2]): return f[3:].rstrip(b"\0").decode()
    return None


def rest_get(port, path, src="127.0.0.1"):
    s = connect(port, src)
    s.sendall(f"GET {path} HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n".encode())
    out = b""
    try:
        while True:
            d = s.recv(4096)
            if not d: break
            out += d
    except (socket.timeout, ConnectionResetError): pass
    s.close()
    return out


def starts(): return sum(1 for l in open(RCPLOG) if l.startswith("started")) if os.path.exists(RCPLOG) else 0
def ends(): return sum(1 for l in open(RCPLOG) if l.startswith("ended")) if os.path.exists(RCPLOG) else 0


async def main(e):
    page = e.page
    def setting(**kw):
        body = "".join(f"{k}={v}\n" for k, v in kw.items()).encode()
        st, _, data = page.call("POST", "/api/set", body)
        check(st == 200 and json.loads(data).get("applied") == len(kw), f"set {kw}")
    def status(): return json.loads(page.call("GET", "/api/radio")[2])
    def action(body):
        st, _, data = page.call("POST", "/api/radio", body.encode() if isinstance(body, str) else body); return st, json.loads(data)

    names = [s["name"] for s in page.state()["settings"]]
    check(all(n in names for n in ("radio", "thread_role", "zigbee_role")), "the radio's three settings are on the page")
    st = status()
    check(st["present"] and st["mode"] == "off" and not st["rcp"] and not st["rest"], f"off by default: nothing forwarded ({st})")

    # ---- the remote radio (Zigbee2MQTT's zoh adapter)
    bridge = subprocess.Popen([f"{ROOT}/build/rcpbridge-host", os.path.join(RDIR, "rcp.sock"), sys.executable, f"{ROOT}/tests/fake_rcp.py"],
                              env=dict(os.environ, FAKE_RCP_LOG=RCPLOG), stderr=subprocess.PIPE, text=True)
    try:
        await until(lambda: os.path.exists(os.path.join(RDIR, "rcp.sock")), 3)
        setting(radio="zigbee", zigbee_role="remote")
        check(e.conf().get("radio") == "zigbee", "state/config says radio=zigbee for root's main.sh")
        await until(lambda: status()["rcp"], 2)
        await asyncio.sleep(0.3)
        s = connect(RCP)
        check(closed(s), "no Home Assistant connected yet: the radio's port takes nobody")
        s.close()
        ha = await home_assistant(e)
        s = connect(RCP)
        v = spinel_version(s)
        check(v and v.startswith("OPENTHREAD/404cfe2ff"), f"Home Assistant's address: the radio answers through the port ({v})")
        check(starts() == 1, f"a fresh RCP for that host ({starts()} started)")
        check(status()["holder"] == "127.0.0.1", "the page says who has the radio")
        s3 = connect(RCP, "127.0.0.5")
        check(closed(s3), "another address than Home Assistant's: refused")
        s3.close()
        # the holder again (Zigbee2MQTT restarted, its old connection never closed): the new one takes the radio
        s2 = connect(RCP)
        check(closed(s, 4), "the holder connects again: its old connection ends")
        v = spinel_version(s2)
        await until(lambda: ends() >= 1, 3)
        check(v is not None and starts() == 2 and ends() == 1, f"the new one has the radio, a fresh RCP ({starts()} started, {ends()} ended)")
        s.close(); s = s2
        s.close()
        await until(lambda: ends() >= 2, 3)
        check(ends() == 2, "the host gone: its RCP ended (rcpbridge)")
        st_, _ = action("allow 127.0.0.5")
        check(st_ == 200 and status()["allow"] == "127.0.0.5", "the page names another address")
        check(open(os.path.join(e.state, "radio")).read() == "allow 127.0.0.5\n", "kept in state/radio")
        await asyncio.sleep(0.3)
        s3 = connect(RCP, "127.0.0.5")
        v = spinel_version(s3)
        check(v is not None and starts() == 3, f"that address may use the radio now, with an RCP of its own ({starts()} started)")
        s4 = connect(RCP)
        check(closed(s4), "a second host while the first has it: refused")
        s4.close()
        s3.close()
        check(action("allow nonsense")[0] == 400, "an address that is none: refused")
        check(action("allow -")[0] == 200 and status()["allow"] is None, "and taken back")
        # the mode changes under a host: its connection ends
        s = connect(RCP); spinel_version(s)
        setting(zigbee_role="remote", radio="off")
        check(closed(s, 3), "radio off: the host's connection to the radio ended")
        s.close()
        await asyncio.sleep(0.3)
        try: connect(RCP).close(); listening = True
        except OSError: listening = False
        check(not listening, "and its port closed")
    finally:
        bridge.terminate()
        _, err = bridge.communicate(timeout=5)
    check("a host connected" in err and "the host left" in err, "rcpbridge said what it did")

    # ---- the border router (our otbr-agent's REST on its unix socket)
    srv = UnixHTTP(os.path.join(RDIR, "rest.sock"), Rest)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        setting(radio="thread", thread_role="border_router")
        conf = e.conf()
        check(conf.get("radio") == "thread" and conf.get("thread_role") == "border_router", "state/config: radio=thread, thread_role=border_router")
        await until(lambda: status()["rest"], 2)
        await asyncio.sleep(0.3)
        st = status()
        t = st["thread"]
        check(t["up"] and t["state"] == "leader" and t["network"] == "OpenThread-1234" and t["channel"] == 15 and t["routers"] == 3,
              f"the page shows the border router's network ({t})")
        check(KEY not in json.dumps(st), "and never its key")
        r = rest_get(REST, "/node")
        check(b"200 OK" in r and b"OpenThread-1234" in r, "Home Assistant reaches the border router's REST on its port")
        check(rest_get(REST, "/node", "127.0.0.6") == b"", "another address does not")
        for hd in ("Origin: http://evil.example", "sec-fetch-site: cross-site"):
            s = connect(REST)
            s.sendall(f"GET /node/dataset/active HTTP/1.1\r\nHost: x\r\n{hd}\r\nConnection: close\r\n\r\n".encode())
            got = b""
            try:
                while (d := s.recv(4096)): got += d
            except (socket.timeout, ConnectionResetError): pass
            s.close()
            check(got == b"", f"a browser's request ({hd.split(':')[0]}) from Home Assistant's address: dropped, nothing answered")
        del rest_log[:]
        st_, _ = action(lambda ctr: f"join {page.seal(ctr, DATASET).hex()}".encode())
        put = [x for x in rest_log if x[0] == "PUT" and x[1] == "/node/dataset/active"]
        check(st_ == 200 and [x[:2] for x in rest_log if x[0] != "GET"] == [("DELETE", "/node"), ("PUT", "/node/dataset/active"), ("PUT", "/node/state")],
              f"join: forgotten, the dataset put, Thread started ({[x[:2] for x in rest_log]})")
        check(put and put[0][1] == "/node/dataset/active" and put[0][2] == "text/plain" and put[0][3] == DATASET.hex().upper(), "the dataset as the TLVs given, unsealed")
        en = [x for x in rest_log if x[:2] == ("PUT", "/node/state")]
        check(en and en[0][3] == '"enable"', "Thread enabled")
        st_, err = action(lambda ctr: f"join {page.seal(ctr, DATASET[:20]).hex()}".encode())
        check(st_ == 400 and "dataset" in err.get("error", ""), f"a dataset cut off: refused with words ({err})")
        del rest_log[:]
        st_, _ = action(f"form {b'Upstairs'.hex()}")
        put = [x for x in rest_log if x[:2] == ("PUT", "/node/dataset/active")]
        check(st_ == 200 and put and json.loads(put[0][3]) == {"networkName": "Upstairs"} and put[0][2] == "application/json",
              f"a new network: forgotten, then made with that name ({rest_log})")
        # the radio program root runs (main.sh radio_program), as the page shows it
        open(os.path.join(RDIR, "program"), "w").write("fallback\n")
        check(status()["program"] == "fallback", "the radio program root runs: Amazon's after ours kept failing (main.sh's file)")
        open(os.path.join(RDIR, "program"), "w").write("gprcp\n")
        check(status()["program"] == "gprcp", "and ours")
        # CSL as root has it (main.sh radio_csl's file): on, or unavailable with root's words (never a quote through)
        check(status()["csl"] is None, "CSL: nothing said while root's file is not there")
        open(os.path.join(RDIR, "csl"), "w").write("on\n")
        check(status()["csl"] == "on", "CSL on, as root says")
        open(os.path.join(RDIR, "csl"), "w").write('unavailable the module "x" did not load\n')
        st = status()
        check(st["csl"] == "unavailable" and st["csl_why"] == "the module  x  did not load", f"CSL unavailable, root's why ({st.get('csl_why')!r})")
        setting(thread_csl="on")
        csl_set = next((x for x in page.state()["settings"] if x["name"] == "thread_csl"), {})
        check(csl_set.get("value") in (True, 1), f"the setting thread_csl, on ({csl_set})")
        setting(thread_csl="off")
        os.unlink(os.path.join(RDIR, "csl"))
        # the ephemeral key: the code sealed for this browser alone, its time in the status, never the code
        del rest_log[:]
        st_, h_, data = page.call("POST", "/api/radio", lambda ctr: b"share")
        j = json.loads(data).get("share") if st_ == 200 else None
        post = [x for x in rest_log if x[:2] == ("POST", "/node/ba-epskc/key")]
        check(st_ == 200 and j and post and json.loads(post[0][3]) == {"lifetime": 120000, "port": 28938},
              f"share: an ephemeral key asked of the border router for 2 min on port 28938 ({post}, {data[:120]})")
        code = page.seal2(page.ctr, bytes.fromhex(j["sealed"]), b"hassmic seal back 3", bytes.fromhex(j["nonce"])) if j else b""
        check(code == b"123456789" and b"123456789" not in data, f"the code reaches the page sealed, never in clear ({code!r})")
        st = status()
        check(100 < st["share_left"] <= 120 and "123456789" not in json.dumps(st), f"the status shows its time left, not the code ({st['share_left']})")
        check(action("unshare")[0] == 200 and status()["share_left"] == 0 and ("DELETE", "/node/ba-epskc/key", None, None) in rest_log, "unshare: the key ended")
        del rest_log[:]
        check(action("leave")[0] == 200 and [x[0] for x in rest_log if x[0] != "GET"] == ["DELETE"], "leave: the network forgotten")
        # a router: no REST for Home Assistant, the page still shows the network
        setting(thread_role="router")
        await asyncio.sleep(0.5)
        st = status()
        check(not st["rest"] and st["thread"]["up"], "a router: the REST port closed, the page still shows its network")
        try: connect(REST).close(); listening = True
        except OSError: listening = False
        check(not listening, "nothing listens on the REST port")
        setting(thread_role="remote")
        check(status()["rcp"] and status()["thread"] == None, "Thread as a remote radio: the radio forwarded, no network of the Echo's own")
        setting(radio="off")
        check(action("leave")[0] == 400, "radio off: the Thread actions are refused")
    finally:
        srv.shutdown()

    # ---- the Zigbee coordinator: our own stack (a real zigbeed on tests/zbair.py's air), ZHA's library through hassmic
    from zbair import Air
    air = Air(); zsock = os.path.join(RDIR, "air.sock")
    air.node("C", 0x00155F0000000001, sock=zsock)
    await air.start()
    zdir = os.path.join(RDIR, "zigbee-coordinator"); os.makedirs(zdir, exist_ok=True)
    zlog = open(os.path.join(RDIR, "zigbeed.log"), "w")
    zp = subprocess.Popen([f"{ROOT}/build/zigbeed-host", "-r", "coordinator", "-R", zsock, "-d", zdir,
                           "-H", os.path.join(RDIR, "zb.sock"), "-C", os.path.join(RDIR, "zb.ctl")], stderr=zlog, env=dict(os.environ, ZIGBEED_ROTATE_MS="2000"))
    app = None
    try:
        await until(lambda: os.path.exists(os.path.join(RDIR, "zb.ctl")), 5)
        setting(radio="zigbee", zigbee_role="coordinator")
        check(e.conf().get("zigbee_role") == "coordinator", "state/config: zigbee_role=coordinator for root's main.sh")
        await until(lambda: "_zigbee-coordinator._tcp" in open(e.mdns).read(), 3)
        svc = open(e.mdns).read()
        check("_zigbee-coordinator._tcp" in svc and f"<port>{ZBP}</port>" in svc and "radio_type=deconz" in svc,
              "mDNS: the coordinator announced as _zigbee-coordinator._tcp, radio_type=deconz (ZHA discovers it)")
        check("_esphomelib._tcp" in svc, "beside ESPHome's own service")
        st = status()
        check(st["zb"] and st["zigbee"]["up"] and st["zigbee"]["role"] == "coordinator" and st["zigbee"]["state"] == "offline",
              f"the page: the coordinator up, waiting for its host ({st['zigbee']})")
        s_ = connect(ZBP, "127.0.0.7")
        check(closed(s_), "another address than Home Assistant's: the coordinator's port refused")
        s_.close()
        import zigpy.config as zc
        from zigpy_deconz.zigbee.application import ControllerApplication
        app = await asyncio.wait_for(ControllerApplication.new({
            zc.CONF_DEVICE: {zc.CONF_DEVICE_PATH: f"socket://127.0.0.1:{ZBP}"},
            zc.CONF_DATABASE: os.path.join(RDIR, "zigbee.db"), zc.CONF_NWK_BACKUP_ENABLED: False, zc.CONF_TOPO_SCAN_ENABLED: False,
            zc.CONF_NWK: {zc.CONF_NWK_CHANNEL: 20, zc.CONF_NWK_CHANNELS: [20]}}, auto_form=True, start_radio=True), 60)
        check(app.state.network_info.channel == 20, f"ZHA's library formed the network through hassmic's port (channel {app.state.network_info.channel})")
        st = status()
        check(st["zigbee"]["state"] == "connected" and st["zigbee"]["host"] and st["zb_holder"] == "127.0.0.1" and st["zigbee"]["channel"] == 20,
              f"the page: on its network, the host connected ({st['zigbee']})")
        check(action("permit 60")[0] == 200 and status()["zigbee"]["permit"] > 50, "the page opens joining")
        check(action("permit 0")[0] == 200 and status()["zigbee"]["permit"] == 0, "and closes it")
        check(action("zb_leave")[0] == 400, "a coordinator does not leave")
        check(action("zb_prefer any")[0] == 400, "nor chooses a network")
        check(action("zb_rotate")[0] == 200 and status()["zigbee"]["rotating"], "the page gives the network a new key: on its way")
        await until(lambda: status()["zigbee"].get("key_seq") == 1 and not status()["zigbee"].get("rotating"), 30)
        check(status()["zigbee"].get("key_seq") == 1, "and the coordinator switched to it")
        await app.shutdown(); app = None
        setting(zigbee_role="router")
        await asyncio.sleep(0.5)
        check(not status()["zb"] and "_zigbee-coordinator._tcp" not in open(e.mdns).read(), "a router: the coordinator's port closed and no longer announced")
    finally:
        if app: await app.shutdown()
        zp.terminate(); zp.wait(5)
        await air.stop()
        setting(radio="off")
    await ha.cli.disconnect()


run(main, (PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT), {"HASSMIC_RADIO_DIR": RDIR, "HASSMIC_RADIO_PORTS": f"{REST}:{RCP}:{ZBP}",
                                                         "HASSMIC_RADIO_PROGRAM": os.path.join(RDIR, "program"),
                                                         "HASSMIC_RADIO_CSL": os.path.join(RDIR, "csl")}, log=True)
