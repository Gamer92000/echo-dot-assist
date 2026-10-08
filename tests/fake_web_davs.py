#!/usr/bin/env python3
"""Amazon's artifacts downloaded on the Echo itself (src/hassmic/davs.c), against a fake Amazon: one build/hassmic-host
with the fake attestation HAL of tests/unit (a fixed RSA key) and faked engine attributes.  The login by code pair
(the code on the page, /auth/register polled with the attestation token in it until the code is "entered", the
tokens on disk 0600 and never in an answer), the DAVS request with the Echo's own engine ids, the tar.gz download
(PAX headers and a directory entry skipped, the sound detection model under AED/, sets with two levels of folders as
whisper and alexa-de-DE have them; deeper refused), the staging checks of artifacts.c,
root's installer, and the deregister (a revoked token refreshed first, so the Echo does not stay on the account).
A login on another Amazon that is cancelled leaves the registration where it was; a site that is none is refused.  Also an Echo whose attestation does not answer (donut, the PC build): it
says so and refuses to log in."""
import base64, gzip, io, json, os, subprocess, sys, tarfile, tempfile, threading, time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webclient import Browser

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = 17040


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False


# ---------------------------------------------------------------- the fake Amazon

def c_array(text, name):
    import re
    m = re.search(rf"{name}\[\d*\] = {{([0-9,]+)}}", text)
    return bytes(int(x) for x in m.group(1).split(","))
KEYH = open(f"{ROOT}/tests/unit/dha_hal_key.h").read()
MOD = c_array(KEYH, "static const unsigned char dha_mod")
MAC = c_array(KEYH, "static const unsigned char dha_mac")
b64u = lambda b: base64.urlsafe_b64encode(b).rstrip(b"=").decode()
LONG_NAME = "n" * 58 + ".bin"


def tar_bytes(entries):
    """entries: (name, type, data).  Hand-rolled, so the strange things real archives carry can come along: a
    directory entry, a PAX extended header, a leading slash."""
    out = io.BytesIO()
    for name, typ, data in entries:
        h = bytearray(512)
        nb = name.encode()
        h[:len(nb)] = nb
        sz = b"%011o\0" % len(data)
        h[124:124 + 12] = sz
        h[156] = typ if isinstance(typ, int) else ord(typ)
        h[257:263] = b"ustar\0"
        h[263:265] = b"00"
        h[148:156] = b"        "
        h[148:156] = b"%06o\0 " % sum(h)
        out.write(h)
        out.write(data)
        out.write(b"\0" * (-len(data) % 512))
    out.write(b"\0" * 1024)
    return gzip.compress(out.getvalue())


class Amazon:
    """what api.amazon.* and api.amazonalexa.com answer, and what they saw"""
    def __init__(self):
        self.entered = False
        self.code_expires = 120
        self.revoked = set()                  # access tokens deregister answers 401 to
        self.registers, self.token_calls, self.davs, self.dereg = [], [], [], []
        self.tars = {
            "echo": tar_bytes([("/", b"5", b""), ("pryon.manifest", 0, b"manifest\n"),
                               ("kw.cfg.json", 0, b"{}"), ("int16_streaming.onnx", 0, os.urandom(100000)),
                               (LONG_NAME, 0, b"62 characters, one below what a model folder keeps")]),
            "aed": tar_bytes([("AED/", b"5", b""), ("AED/pryon.manifest", 0, b"aed\n"), ("AED/AED.json", 0, b"{}"),
                              ("AED/model.mlp", 0, os.urandom(60000))]),
            # the whisper archive carries a PAX header, as the real one does (Apple xattrs of the packing Mac)
            "whisper": tar_bytes([("PaxHeaders/0/HCLG.fst", b"x", b"40 LIBARCHIVE.xattr.com.apple.quarantine=whatever\n"),
                                  ("pryon_whisper.manifest", 0, b"w\n"), ("HCLG.fst", 0, os.urandom(50000)),
                                  ("._crumb", 0, b"junk"), ("whisper_components/", b"5", b""),
                                  ("whisper_components/model.v8.0.mlp", 0, os.urandom(30000)),
                                  ("whisper_components/._model.v8.0.mlp", 0, b"junk")]),
            # alexa-de-DE and every en-US set: two levels of folders (issue 14), more than 64 files
            "alexa": tar_bytes([("BDPGeneratedFiles/", b"5", b""), ("BDPGeneratedFiles/kw.cfg.json", 0, b"{}"),
                                ("BDPGeneratedFiles/words.txt", 0, os.urandom(4000)), ("pryon.manifest", 0, b"m\n"),
                                ("nttfusionconfig/ntt_conv/", b"5", b""), ("nttfusionconfig/ntt_conv/._afe_ued.cfg.json", 0, b"junk")]
                               + [(f"nttfusionconfig/ntt_conv/c{i}.cfg.json", 0, b"{}") for i in range(66)]),
            # nothing Amazon sends, but what the Echo must not unpack: three levels of folders
            "computer": tar_bytes([("pryon.manifest", 0, b"m\n"), ("model.bin", 0, os.urandom(2000)), ("a/b/c/deep.bin", 0, b"x")]),
        }


AMZ = Amazon()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a): pass

    def reply(self, code, obj):
        body = json.dumps(obj).encode() if isinstance(obj, dict) else obj
        self.send_response(code)
        self.send_header("Content-Type", "application/json" if isinstance(obj, dict) else "application/octet-stream")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        body = json.loads(self.rfile.read(n) or b"{}")
        if self.path == "/auth/create/codepair":
            check(body["code_data"]["device_type"] == "A3S5BH2HU6VAYF" and body["code_data"]["device_serial"] == "G000TEST00000000",
                  f"the code pair asked for with this Echo's serial and type: {body['code_data']}")
            self.reply(200, {"public_code": "D4Y7LK", "private_code": "36-char-private-code-xxxxxxxxxx",
                             "secureSessionToken": "sst-1", "polling_interval_in_seconds": 1, "expires_in": AMZ.code_expires})
        elif self.path == "/auth/register":
            AMZ.registers.append((dict(self.headers), body))
            sst = self.headers.get("x-amzn-identity-secure-session-token", "")
            check(sst == "sst-1" and self.headers.get("x-amzn-identity-auth-domain", "").startswith("127.0.0.1"),
                  "the register call carries the session token and the host header MAP's policy sets")
            tok = body["registration_data"].get("device_authentication_token", "")
            parts = tok.split(".")
            ok = len(parts) == 3
            if ok:
                head = json.loads(base64.urlsafe_b64decode(parts[0] + "==="))
                pay = json.loads(base64.urlsafe_b64decode(parts[1] + "==="))
                ok = (head["typ"] == "drvV1" and head["alg"] == "PS256" and head["jwk"]["n"] == b64u(MOD)
                      and head["jwk"]["mac"] == b64u(MAC) and pay["dev"]["dt"] == "A3S5BH2HU6VAYF"
                      and pay["dev"]["dsn"] == "G000TEST00000000" and pay["cust"]["typ"] == "v1"
                      and body["registration_data"]["device_type"] == "A3S5BH2HU6VAYF")
            check(ok, f"every register carries a drvV1 attestation token of this Echo's key: {tok[:40]}…")
            if not ok:
                self.reply(400, {"response": {"error": {"code": "InvalidDevice", "message": "The device information is invalid."}}})
            elif not AMZ.entered:     # what the real Amazon answers until the code is entered (2026-10-06)
                self.reply(401, {"response": {"error": {"code": "Unauthorized", "message": "The request is not authorized."}}})
            else:
                self.reply(200, {"response": {"success": {"tokens": {"bearer": {"access_token": "AT1", "refresh_token": "RT1",
                                                            "expires_in": 0}},
                                        "extensions": {"device_info": {"device_name": "Küchen Echo"},
                                                       "customer_info": {"preferred_marketplace": "A1PA6"}}}}})
        elif self.path == "/auth/token":
            AMZ.token_calls.append(body)
            self.reply(200, {"access_token": f"AT{len(AMZ.token_calls) + 1}", "expires_in": 3600})
        elif self.path == "/auth/deregister":
            auth = self.headers.get("Authorization", "")
            AMZ.dereg.append(auth)
            self.reply(401 if auth.split()[-1] in AMZ.revoked else 200, {})
        else:
            self.reply(404, {"error": "no such path"})

    def do_GET(self):
        if self.path.startswith("/v2/deviceArtifacts/"):
            AMZ.davs.append((self.headers.get("Authorization", ""),
                             json.loads(base64.b64decode(urllib.parse.unquote(
                                 urllib.parse.parse_qs(self.path.split("?", 1)[1])["artifactFilter"][0])))))
            auth, want = AMZ.davs[-1]
            key = want["artifactKey"]
            if key not in ("echo", "AED", "whisper-static", "alexa", "computer"):
                self.reply(404, {"message": "No suitable artifact found for request."})
            elif key == "echo" and want["filters"]["engineCompatibilityIdList"] != [1, 2, 3]:
                self.reply(400, {"message": "wrong engine ids"})
            else:
                name = {"echo": "echo", "AED": "aed", "whisper-static": "whisper", "alexa": "alexa", "computer": "computer"}[key]
                ans = {"artifactIdentifier": f"id-{name}", "downloadUrl": f"http://127.0.0.1:{PORT}/dl/{name}"}
                if name == "whisper": ans["pad"] = "x" * 13000      # more than the Echo keeps of an answer: cut, still ended
                self.reply(200, ans)
        elif self.path.startswith("/dl/"):
            body = AMZ.tars[self.path[4:]]
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.reply(404, {})


def start_amazon():
    srv = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


# ---------------------------------------------------------------- the Echo

class Echo:
    def __init__(self, name, port, with_dha=True):
        self.name, self.web, self.api = name, port, port + 100
        self.data = tempfile.mkdtemp()
        self.state = os.path.join(self.data, "state"); os.makedirs(self.state)
        self.idme = os.path.join(self.data, "idme"); os.makedirs(self.idme)
        for f, v in ("serial", "G000TEST00000000\0"), ("device_type_id", "A3S5BH2HU6VAYF \0"), ("mac_addr", "011122334455\0"):
            open(os.path.join(self.idme, f), "w").write(v)
        fake = os.path.join(self.data, "pryon_test")
        with open(fake, "w") as f: f.write('#!/bin/sh\nexit 3\n')          # every set loads
        os.chmod(fake, 0o755)
        self.log = os.path.join(self.state, "log")
        self.env = dict(os.environ, HASSMIC_STATE=self.state, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw",
                        HASSMIC_PLAY=os.path.join(self.state, "play.raw"), HASSMIC_MDNS_FILE=os.path.join(self.state, "none"),
                        HASSMIC_MODELS=os.path.join(self.data, "models"), HASSMIC_AED=os.path.join(self.data, "aed"),
                        HASSMIC_WHISPER=os.path.join(self.data, "whisper"), HASSMIC_PRYON_TEST=fake,
                        HASSMIC_IDME=self.idme, HASSMIC_DAVS_API=f"http://127.0.0.1:{PORT}",
                        HASSMIC_FAKE_WAKE_ATTRS='{"wakeword_ecids":[1,2,3],"aed_ecids":[7]}')
        if with_dha:
            self.env["HASSMIC_DHA_HAL"] = f"{ROOT}/build/dha_hal_fake.so"

    def start(self):
        self.proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(self.api), "-n", self.name, "-L",
                                      "-z", "0", "-o", "0", "-a", "0", "-W", str(self.web)], env=self.env,
                                     stderr=open(self.log, "a"))
        time.sleep(0.6)

    def stop(self): self.proc.terminate(); self.proc.wait()


def davs_of(b, seen):
    st, _, data = b.call("GET", "/api/davs")
    seen.append(data.decode())
    return json.loads(data) if st == 200 else None


def wait_state(want, b, seen, timeout=30):
    d = None
    for _ in range(timeout * 2):
        d = davs_of(b, seen)
        if d and d["state"] == want: return d
        time.sleep(0.5)
    return d


def main():
    start_amazon()
    e = Echo("Echo Davs", 17051)
    e.start()
    seen = []
    try:
        b = Browser(e.web)
        check(b.login_with_button(e.proc), "logged in to the page with the action button")
        check(__import__("webclient").req(e.web, "GET", "/api/davs")[0] == 401, "the Amazon state: only signed")
        d = davs_of(b, seen)
        check(d == {"state": "none", "dha": True, "domain": "de", "code": "", "url": "https://www.amazon.de/code",
                    "left_s": 0, "device": "", "busy": "", "progress": 0, "error": "", "done": ""},
              f"first: no login, but this Echo could attest itself: {d}")

        st, _, body = b.call("POST", "/api/davs/login", b"de")
        d = davs_of(b, seen)
        check(st == 200 and d["state"] == "waiting" and d["domain"] == "de",
              f"login started: waiting from the start, before Amazon gave a code ({d['code']!r})")
        for _ in range(20):
            d = davs_of(b, seen)
            if d["code"]: break
            time.sleep(0.5)
        check(d["state"] == "waiting" and d["code"] == "D4Y7LK" and d["url"] == "https://www.amazon.de/code" and d["left_s"] > 60,
              f"the code on the page, with the link and the time left: {d['code']}, {d['left_s']} s")
        for _ in range(20):
            if "register: HTTP 401 Unauthorized" in open(e.log).read(): break
            time.sleep(0.5)
        check("D4Y7LK" in open(e.log).read() and "register: HTTP 401 Unauthorized" in open(e.log).read(),
              "the Echo polls register meanwhile (before the entry: 401 Unauthorized, as on the real Amazon)")

        AMZ.entered = True                    # the user enters the code
        d = wait_state("registered", b, seen)
        check(d["state"] == "registered" and d["device"] == "Küchen Echo",
              f"registered: {d['state']}, Amazon named it \"{d['device']}\"")
        reg = os.path.join(e.state, "davs")
        check(os.path.exists(reg) and "RT1" in open(reg).read() and (os.stat(reg).st_mode & 0o777) == 0o600,
              "the tokens on disk, mode 600")
        check(not any(t in s for t in ("RT1", "AT1", "AT2") for s in seen), "and never in an answer to the page")
        check(AMZ.registers[-1][1]["registration_data"]["device_name"] == "Echo Davs",
              f"registered under the satellite's name: {AMZ.registers[-1][1]['registration_data']['device_name']!r}")

        # downloads
        for key, loc, want in (("echo", "de-DE", "wake:echo-de-DE"), ("aed", "de-DE", "sound"), ("whisper", "en-US", "whisper")):
            st, _, body = b.call("POST", "/api/davs/fetch", f"{key} {loc}".encode())
            check(st == 200, f"fetch {key} {loc} started")
            d = wait_state("registered", b, seen)
            check(d["done"] == want and not d["error"], f"{key}: downloaded, checked, staged ({d['done']}, error {d['error']!r})")
            listing = json.loads(b.call("GET", "/api/artifacts")[2])
            have = {x["id"]: x for x in listing["artifacts"]}
            check(want.replace(":", ".") in listing["staged"] and have.get(want) is None,
                  f"{key}: ready to install, not live yet")
        check(AMZ.token_calls and AMZ.token_calls[0]["source_token"] == "RT1",
              "the short-lived access token made the Echo refresh it (as the page never saw either)")
        check(AMZ.davs and AMZ.davs[0][0] == "Bearer AT2" and AMZ.davs[0][1]["filters"]["engineCompatibilityIdList"] == [1, 2, 3]
              and AMZ.davs[1][1]["filters"]["location"] == ["EU"] and AMZ.davs[2][1]["artifactKey"] == "whisper-static",
              "DAVS asked with the fresh token, this engine's ids, the region by locale, whisper's own request")

        # a set that travels as folders (alexa-de-DE on the real Amazon)
        b.call("POST", "/api/davs/fetch", b"alexa de-DE")
        d = wait_state("registered", b, seen)
        check(d["done"] == "wake:alexa-de-DE" and not d["error"], f"a set with a folder inside: staged ({d['done']}, {d['error']!r})")

        # what cannot arrive
        b.call("POST", "/api/davs/fetch", b"computer de-DE")
        d = wait_state("registered", b, seen)
        staged = json.loads(b.call("GET", "/api/artifacts")[2])["staged"]
        check(d["error"] and "cannot keep" in d["error"] and "wake.echo-de-DE" in staged and "wake.computer-de-DE" not in staged,
              f"three levels of folders: refused, the others untouched ({d['error'][:70]})")
        st, _, body = b.call("POST", "/api/davs/fetch", b"echo deDE")
        check(st == 400, "a locale that is none: refused")

        # install: root's part
        st, _, _ = b.call("POST", "/api/artifact/install")
        reqf = open(os.path.join(e.state, "artifacts", "request")).read().split()
        check(st == 200 and sorted(reqf) == ["sound", "wake:alexa-de-DE", "wake:echo-de-DE", "whisper"],
              f"handed to root: {reqf}")
        r = subprocess.run(["sh", f"{ROOT}/scripts/device/artifact-install.sh", e.state, e.data], capture_output=True, text=True,
                           env=dict(os.environ, READ_AS=""))
        check(r.returncode == 0 and "OK wake:echo-de-DE" in r.stdout and "OK sound" in r.stdout and "OK whisper" in r.stdout
              and os.path.exists(os.path.join(e.data, "models", "echo-de-DE", "pryon.manifest"))
              and os.path.exists(os.path.join(e.data, "whisper", "pryon_whisper.manifest")),
              f"root installed them: {r.stdout.strip()!r}")
        def tree(d_):
            top = os.path.join(e.data, d_)
            return sorted(os.path.relpath(os.path.join(r, f), top) for r, _, fs in os.walk(top) for f in fs)
        for d_, want_files in (("models/echo-de-DE", ["int16_streaming.onnx", "kw.cfg.json", LONG_NAME, "pryon.manifest"]),
                               ("models/alexa-de-DE", ["BDPGeneratedFiles/kw.cfg.json", "BDPGeneratedFiles/words.txt", "pryon.manifest"]
                                + [f"nttfusionconfig/ntt_conv/c{i}.cfg.json" for i in range(66)]),
                               ("aed", ["AED.json", "model.mlp", "pryon.manifest"]),
                               ("whisper", ["HCLG.fst", "pryon_whisper.manifest", "whisper_components/model.v8.0.mlp"])):
            got = tree(d_)
            check(got == sorted(want_files), f"{d_}: every file of the archive, folders and names whole: {got}")
        check(oct(os.stat(os.path.join(e.data, "whisper", "whisper_components")).st_mode & 0o777) == "0o755"
              and oct(os.stat(os.path.join(e.data, "whisper", "whisper_components", "model.v8.0.mlp")).st_mode & 0o777) == "0o644",
              "root's copy: folders 755, files 644")
        arts = {a["id"]: a for a in json.loads(b.call("GET", "/api/artifacts")[2])["artifacts"]}
        check("whisper_components/model.v8.0.mlp" in [f["name"] for f in arts["whisper"]["files"]],
              "the models list names files in folders by their path, for copies to other Echos")
        check(open(os.path.join(e.data, "models", "echo-de-DE", "int16_streaming.onnx"), "rb").read()
              == tarfile.open(fileobj=io.BytesIO(AMZ.tars["echo"])).extractfile("int16_streaming.onnx").read(),
              "and byte for byte what was packed")

        # a login on another Amazon, cancelled: the registration stays on amazon.de
        st, _, body = b.call("POST", "/api/davs/login", b"nl")
        check(st == 400 and b"not one of the Amazons" in body, "amazon.nl (no Alexa): refused")
        AMZ.entered = False
        st, _, _ = b.call("POST", "/api/davs/login", b"co.uk")
        d = wait_state("waiting", b, seen)
        check(st == 200 and d["domain"] == "co.uk" and d["url"] == "https://www.amazon.co.uk/code",
              f"a second login, on amazon.co.uk, shows its own link: {d['url']}")
        st, _, _ = b.call("POST", "/api/davs/cancel")
        d = wait_state("registered", b, seen)
        check(st == 200 and d["state"] == "registered" and d["domain"] == "de" and "domain de" in open(reg).read()
              and "RT1" in open(reg).read() and not d["error"],
              f"cancelled: still registered on amazon.{d['domain']}")
        st, _, _ = b.call("POST", "/api/davs/cancel")
        check(st == 400, "cancel with no login waiting: refused")

        # logout, with the access token revoked meanwhile: refreshed and tried again, not taken for "already gone"
        AMZ.revoked.add("AT2")
        st, _, _ = b.call("POST", "/api/davs/logout")
        d = wait_state("none", b, seen)
        check(d and d["state"] == "none" and not os.path.exists(os.path.join(e.state, "davs"))
              and AMZ.dereg == ["Bearer AT2", "Bearer AT3"] and AMZ.token_calls[-1]["source_token"] == "RT1",
              f"log out: a 401 made the Echo refresh and deregister again, then the tokens went ({AMZ.dereg})")
    finally:
        try: e.stop()
        except Exception: pass

    # an Echo whose attestation does not answer (donut's key builds a token Amazon takes differently, the PC has none)
    e2 = Echo("Echo NoDha", 17052, with_dha=False)
    e2.start()
    try:
        b2 = Browser(e2.web)
        b2.login_with_button(e2.proc)
        d = davs_of(b2, [])
        check(d and d["dha"] is False and d["state"] == "none", "an Echo that cannot attest: the page says so")
        st, _, body = b2.call("POST", "/api/davs/login", b"de")
        check(st == 400 and b"cannot prove" in body, f"and refuses to log in: {json.loads(body).get('error')}")
    finally:
        try: e2.stop()
        except Exception: pass

    if check.failed:
        print(open(e.log).read()[-3000:]); print("FAILED"); raise SystemExit(1)
    print("PASS")


main()
