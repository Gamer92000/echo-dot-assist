"""http.c and tls.c (issue #236) against Python's servers: build/http_test (tests/unit/http_test.c) per case.

TLS 1.3 alone (Home Assistant's "Modern" profile) and TLS 1.2 alone, certificates for an address and for a name, a
self-signed one, one for another name, one run out; bodies by length, by chunks and by the connection's end, one cut
off; redirects (relative, absolute, to another origin without the caller's headers, to plain http:// refused where
asked, too many, 303 and a POST); a POST's body; the low speed limit, the whole's time, a stop from another thread, the
reader ending it; addresses and headers refused.

    python3 tests/unit/http_test.py build/http_test
"""
import datetime, hashlib, ipaddress, os, socket, ssl, subprocess, sys, tempfile, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, rsa
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

BIN = sys.argv[1]
failed = False


def check(cond, what):
    global failed
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: failed = True


d = tempfile.mkdtemp()
now = datetime.datetime.now(datetime.timezone.utc)


def name(cn): return x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, cn)])


def cert(subject, key, issuer, issuer_key, ca=False, san=None, days=(1, 30), eku=None):
    b = (x509.CertificateBuilder().subject_name(name(subject)).issuer_name(issuer).public_key(key.public_key())
         .serial_number(x509.random_serial_number()).not_valid_before(now - datetime.timedelta(days=days[0]))
         .not_valid_after(now + datetime.timedelta(days=days[1])).add_extension(x509.BasicConstraints(ca=ca, path_length=None), critical=True))
    if san: b = b.add_extension(x509.SubjectAlternativeName(san), critical=False)
    if eku: b = b.add_extension(x509.ExtendedKeyUsage(eku), critical=False)
    return b.sign(issuer_key, hashes.SHA256())


def pem(path, *objs):
    with open(path, "wb") as f:
        for o in objs:
            f.write(o.public_bytes(serialization.Encoding.PEM) if isinstance(o, x509.Certificate) else
                    o.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    return path


ca_key = ec.generate_private_key(ec.SECP256R1())
ca = cert("hassmic http test CA", ca_key, name("hassmic http test CA"), ca_key, ca=True)
CA = pem(os.path.join(d, "ca.pem"), ca)
k_ip, k_name, k_self, k_rsa = ec.generate_private_key(ec.SECP384R1()), ec.generate_private_key(ec.SECP256R1()), ec.generate_private_key(ec.SECP256R1()), rsa.generate_private_key(65537, 2048)
IP = [x509.IPAddress(ipaddress.ip_address("127.0.0.1"))]
chains = {
    "ip": pem(os.path.join(d, "ip.pem"), cert("127.0.0.1", k_ip, ca.subject, ca_key, san=IP), k_ip),
    "rsa": pem(os.path.join(d, "rsa.pem"), cert("127.0.0.1", k_rsa, ca.subject, ca_key, san=IP), k_rsa),
    "name": pem(os.path.join(d, "name.pem"), cert("localhost", k_name, ca.subject, ca_key, san=[x509.DNSName("localhost")]), k_name),
    "other": pem(os.path.join(d, "other.pem"), cert("elsewhere", k_name, ca.subject, ca_key, san=[x509.DNSName("elsewhere.example")]), k_name),
    "old": pem(os.path.join(d, "old.pem"), cert("127.0.0.1", k_ip, ca.subject, ca_key, san=IP, days=(30, -1)), k_ip),
    "self": pem(os.path.join(d, "self.pem"), cert("127.0.0.1", k_self, name("127.0.0.1"), k_self, san=IP), k_self),
    # signed by the CA for the right address and name, but for clients only (no serverAuth)
    "client_ip": pem(os.path.join(d, "client_ip.pem"), cert("127.0.0.1", k_ip, ca.subject, ca_key, san=IP, eku=[ExtendedKeyUsageOID.CLIENT_AUTH]), k_ip),
    "client_name": pem(os.path.join(d, "client_name.pem"), cert("localhost", k_name, ca.subject, ca_key, san=[x509.DNSName("localhost")], eku=[ExtendedKeyUsageOID.CLIENT_AUTH]), k_name),
}

BIG = bytes(range(256)) * 4096                                 # 1 MB
seen = {}                                                      # path -> the request's headers, as the server got them


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a): pass

    def send(self, code, body=b"", headers=()):
        self.send_response(code)
        for k, v in headers: self.send_header(k, v)
        if not any(k.lower() in ("content-length", "transfer-encoding") for k, _ in headers):
            self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body: self.wfile.write(body)

    def answer(self, body_in=b""):
        p = self.path.split("?")[0]
        seen[p] = dict(self.headers.items()); seen[p]["_method"] = self.command; seen[p]["_body"] = body_in
        if p == "/big": return self.send(200, BIG)
        if p == "/chunked":
            self.send_response(200); self.send_header("Transfer-Encoding", "chunked"); self.end_headers()
            for i in range(0, len(BIG), 7000):
                c = BIG[i:i + 7000]; self.wfile.write(b"%x;ext=1\r\n" % len(c) + c + b"\r\n")
            self.wfile.write(b"0\r\nX-Trailer: 1\r\n\r\n"); return
        if p == "/eof":
            self.send_response(200); self.send_header("Connection", "close"); self.end_headers()
            self.wfile.write(BIG[:300000]); self.close_connection = True; return
        if p == "/cut":
            self.send_response(200); self.send_header("Content-Length", "500000"); self.end_headers()
            self.wfile.write(BIG[:100000]); self.wfile.flush(); self.close_connection = True; return
        if p == "/slow":
            self.send_response(200); self.send_header("Content-Length", "100"); self.end_headers(); self.wfile.flush()
            time.sleep(4); return
        if p == "/trickle":                                    # 10 bytes a second for 6 s
            self.send_response(200); self.send_header("Content-Length", "60"); self.end_headers()
            for _ in range(6): self.wfile.write(b"x" * 10); self.wfile.flush(); time.sleep(1)
            return
        if p == "/404": return self.send(404, b"no such thing")
        if p == "/rel/a": return self.send(302, headers=[("Location", "b")])
        if self.path == "/ftp": return self.send(302, headers=[("Location", "ftp://127.0.0.1/x")])
        if self.path == "/absdots": return self.send(302, headers=[("Location", f"https://127.0.0.1:{self.server.server_port}/api/x/../v2/file")])
        if self.path == "/api/v1/x": return self.send(302, headers=[("Location", "../v2/./file")])
        if self.path == "/api/v2/file": return self.send(200, b"dots")
        if self.path == "/f/file?x=1":                  # a fragment alone: the same document again (then served)
            H.frag = getattr(H, "frag", 0) + 1
            return self.send(302, headers=[("Location", "#t=10")]) if H.frag == 1 else self.send(200, b"same")
        if self.path == "/q/file?x=1": return self.send(302, headers=[("Location", "?page=2")])
        if self.path == "/q/file?page=2": return self.send(200, b"query")
        if p == "/rel/b": return self.send(200, b"relative")
        if p == "/abs": return self.send(301, headers=[("Location", "/big")])
        if p == "/loop": return self.send(302, headers=[("Location", "/loop")])
        if p == "/see": return self.send(303, headers=[("Location", "/echo")])
        if p == "/keep": return self.send(307, headers=[("Location", "/echo")])
        if p == "/echo": return self.send(200, (self.command + " ").encode() + body_in)
        if p == "/sum": return self.send(200, hashlib.sha256(body_in).hexdigest().encode())
        if p == "/long": return self.send(302, headers=[("Location", "/echo?" + "a" * 3000)])
        if p.startswith("/to/") or p.startswith("/to7/"):    # /to/<port>/<scheme>: a redirect to another server (to7: 307)
            _, kind, port, scheme = p.split("/")
            return self.send(302 if kind == "to" else 307, headers=[("Location", f"{scheme}://127.0.0.1:{port}/echo")])
        if p == "/same": return self.send(302, headers=[("Location", "/echo")])
        if p == "/continue":
            self.wfile.write(b"HTTP/1.1 100 Continue\r\n\r\n"); return self.send(200, b"after 100")
        return self.send(200, b"hello")

    def do_GET(self): self.answer()

    def do_POST(self):
        if self.path == "/sum": time.sleep(1.5)                # the request's body waits in the sockets: the client's send blocks
        self.answer(self.rfile.read(int(self.headers.get("Content-Length", "0"))))


class Quiet(ThreadingHTTPServer):
    daemon_threads = True
    def handle_error(self, request, client_address): pass     # the client ending a body early is a case here


def serve(chain=None, tls=None):
    h = Quiet(("127.0.0.1", 0), H)
    if chain:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(chain)
        if tls == "1.3": ctx.minimum_version = ssl.TLSVersion.TLSv1_3
        if tls == "1.2": ctx.maximum_version = ssl.TLSVersion.TLSv1_2
        h.socket = ctx.wrap_socket(h.socket, server_side=True)
    threading.Thread(target=h.serve_forever, daemon=True).start()
    return h.server_address[1]


s13, s12, srsa13, srsa12 = serve(chains["ip"], "1.3"), serve(chains["ip"], "1.2"), serve(chains["rsa"], "1.3"), serve(chains["rsa"], "1.2")
sname, sother, sold, sself = serve(chains["name"]), serve(chains["other"]), serve(chains["old"]), serve(chains["self"])
sclient_ip, sclient_name = serve(chains["client_ip"]), serve(chains["client_name"])
plain, plain2 = serve(), serve()
out = os.path.join(d, "out")


def run(*args, env_ca=True):
    env = dict(os.environ)
    if env_ca: env["HASSMIC_CA_FILE"] = CA
    r = subprocess.run([BIN, "-o", out, *args], capture_output=True, text=True, env=env, timeout=60)
    line = [l for l in r.stdout.splitlines() if l.startswith("rc=")]
    if not line: return {"rc": None, "status": None, "bytes": None, "err": r.stdout + r.stderr}
    f = dict(x.split("=", 1) for x in line[0].split(" ", 3))
    return {"rc": int(f["rc"]), "status": int(f["status"]), "bytes": int(f["bytes"]), "err": f["err"]}


def body(): return open(out, "rb").read()


def ok(r, n=None): return r["rc"] == 0 and r["status"] == 200 and (n is None or r["bytes"] == n)


# the protocol versions and certificates
for port, what in ((s13, "TLS 1.3 alone (Home Assistant's Modern profile), ECDSA P-384"), (s12, "TLS 1.2 alone, ECDSA"),
                   (srsa13, "TLS 1.3 alone, RSA (rsa_pss_rsae)"), (srsa12, "TLS 1.2 alone, RSA")):
    r = run(f"https://127.0.0.1:{port}/big")
    check(ok(r, len(BIG)) and body() == BIG, f"{what}: 1 MB read whole ({r})")
r = run(f"https://localhost:{sname}/big")
check(ok(r, len(BIG)), f"a certificate for a name, reached by it ({r['err']})")
r = run(f"https://127.0.0.1:{sname}/big")
check(r["rc"] == -1 and "does not check out" in r["err"], f"a name's certificate, reached by an address: refused ({r['err']})")
r = run(f"https://localhost:{sother}/")
check(r["rc"] == -1 and "does not check out" in r["err"] and "CN" in r["err"], f"a certificate for another name: refused ({r['err']})")
r = run(f"https://127.0.0.1:{sold}/")
check(r["rc"] == -1 and "does not check out" in r["err"] and "expired" in r["err"], f"a certificate run out: refused ({r['err']})")
r = run(f"https://127.0.0.1:{sself}/")
check(r["rc"] == -1 and "does not check out" in r["err"], f"self-signed: refused ({r['err']})")
r = run(f"https://127.0.0.1:{sclient_ip}/")
check(r["rc"] == -1 and "does not check out" in r["err"], f"an address's certificate for clients only (no serverAuth): refused ({r['err']})")
r = run(f"https://localhost:{sclient_name}/")
check(r["rc"] == -1 and "does not check out" in r["err"], f"a name's certificate for clients only: refused ({r['err']})")
r = run("-k", f"https://127.0.0.1:{sself}/big")
check(ok(r, len(BIG)), f"self-signed with checks off: taken ({r['err']})")
r = run(f"https://127.0.0.1:{s13}/", env_ca=False)
check(r["rc"] == -1 and "does not check out" in r["err"], f"the system's CAs do not know the test CA ({r['err']})")
r = run(f"http://127.0.0.1:{s13}/", "-t", "5")
check(r["rc"] == -1, f"plain HTTP to a TLS port: a failure, not a hang ({r['err']})")
r = run(f"https://127.0.0.1:{plain}/", "-t", "5")
check(r["rc"] == -1 and "handshake" in r["err"].lower(), f"TLS to a plain port: the handshake fails ({r['err']})")

# bodies
r = run(f"https://127.0.0.1:{s13}/chunked")
check(ok(r, len(BIG)) and body() == BIG, f"chunks (with extensions and a trailer) over TLS ({r['err']})")
r = run(f"http://127.0.0.1:{plain}/chunked")
check(ok(r, len(BIG)) and body() == BIG, f"chunks over plain HTTP ({r['err']})")
r = run(f"https://127.0.0.1:{s13}/eof")
check(ok(r, 300000) and body() == BIG[:300000], f"no length: the body ends with the connection ({r['err']})")
r = run(f"https://127.0.0.1:{s13}/cut")
check(r["rc"] == -1 and "cut off after 100000 of 500000" in r["err"], f"a body shorter than its length: a failure ({r['err']})")
r = run(f"https://127.0.0.1:{s13}/404")
check(r["rc"] == 0 and r["status"] == 404 and body() == b"no such thing", f"an error's status and body reach the caller ({r})")
r = run(f"http://127.0.0.1:{plain}/continue")
check(ok(r) and body() == b"after 100", f"a 100 Continue before the answer ({r['err']})")

# redirects
r = run("-n", "3", f"https://127.0.0.1:{s13}/rel/a")
check(ok(r) and body() == b"relative", f"a relative redirect ({r['err']})")
r = run("-n", "3", f"https://127.0.0.1:{s13}/ftp")
check(r["rc"] == -1 and "another scheme" in r["err"], f"a redirect to ftp:// refused, not read as a path here ({r['err']})")
r = run("-n", "3", f"https://127.0.0.1:{s13}/absdots")
check(ok(r) and body() == b"dots", f"an absolute redirect with .. segments: taken out too ({r['err']})")
r = run("-n", "3", f"https://127.0.0.1:{s13}/api/v1/x")
check(ok(r) and body() == b"dots", f"a relative redirect with . and .. segments: taken out (RFC 3986 5.2.4) ({r['err']})")
r = run("-n", "3", f"https://127.0.0.1:{s13}/f/file?x=1")
check(ok(r) and body() == b"same", f"a redirect to a fragment alone: the same document (RFC 3986 5.2.2) ({r['err']})")
r = run("-n", "3", f"https://127.0.0.1:{s13}/q/file?x=1")
check(ok(r) and body() == b"query", f"a redirect to a query alone keeps the path (RFC 3986 5.2.2) ({r['err']})")
r = run("-n", "3", f"https://127.0.0.1:{s13}/abs")
check(ok(r, len(BIG)), f"an absolute path's redirect ({r['err']})")
r = run(f"https://127.0.0.1:{s13}/abs")
check(r["rc"] == 0 and r["status"] == 301, f"no redirects asked: the 3xx is the answer ({r})")
r = run("-n", "4", f"https://127.0.0.1:{s13}/loop")
check(r["rc"] == -1 and "more than 4 redirects" in r["err"], f"too many redirects ({r['err']})")
r = run("-n", "3", "-S", f"https://127.0.0.1:{s13}/to/{plain}/http")
check(r["rc"] == -1 and "plain http://" in r["err"], f"a redirect to http:// refused where only HTTPS may follow ({r['err']})")
r = run("-n", "3", f"https://127.0.0.1:{s13}/to/{plain}/http")
check(ok(r) and body() == b"GET ", f"... and followed where it may ({r['err']})")
seen.clear()
r = run("-n", "3", "-H", "Authorization: Bearer secret", "-H", "X-Other: 1", f"https://127.0.0.1:{s13}/to/{s12}/https")
check(ok(r) and "Authorization" not in seen.get("/echo", {}) and "X-Other" not in seen.get("/echo", {}) and seen.get("/to/{}/https".format(s12), {}).get("Authorization") == "Bearer secret",
      f"to another origin: none of the caller's headers along (they went to the first) ({r['err']}, {seen.get('/echo')})")
seen.clear()
r = run("-n", "3", "-H", "Authorization: Bearer secret", f"https://127.0.0.1:{s13}/same")
check(ok(r) and seen.get("/echo", {}).get("Authorization") == "Bearer secret", f"the same origin: the headers stay ({r['err']})")
seen.clear()
r = run("-n", "3", "-d", "abc", f"https://127.0.0.1:{s13}/see")
check(ok(r) and body() == b"GET ", f"303 after a POST: a GET, no body ({r['err']}, {body()})")
r = run("-n", "3", "-d", "abc", f"https://127.0.0.1:{s13}/keep")
check(ok(r) and body() == b"POST abc", f"307 after a POST: the POST again with its body ({r['err']}, {body()})")
r = run("-n", "3", f"https://127.0.0.1:{s13}/long")
check(r["rc"] == -1 and "longer than" in r["err"], f"a redirect's address too long to keep: refused, not followed cut ({r['err']})")
r = run("-n", "3", "-d", "secret", f"https://127.0.0.1:{s13}/to/{s12}/https".replace("/to/", "/to7/"))
check(r["rc"] == -1 and "body to another server" in r["err"], f"a 307 that would take the body to another server: refused ({r['err']})")
r = run("-P", f"http://127.0.0.1:{plain}/big")
check(ok(r, len(BIG)), f"plain only: http:// works ({r['err']})")
r = run("-P", f"https://127.0.0.1:{s13}/")
check(r["rc"] == -1 and "only http://" in r["err"], f"plain only: https:// refused ({r['err']})")
r = run("-L", "-P", f"http://127.0.0.1:{plain}/")
check(ok(r) and body() == b"hello", f"a plain socket (the local network) ({r['err']})")

# a POST and its headers
seen.clear()
r = run("-d", '{"a":1}', "-H", "Content-Type: application/json", f"https://127.0.0.1:{s13}/echo")
e = seen.get("/echo", {})
check(ok(r) and body() == b'POST {"a":1}' and e.get("Content-Type") == "application/json" and e.get("Content-Length") == "7"
      and e.get("Host") == f"127.0.0.1:{s13}" and e.get("User-Agent") == "hassmic-test" and e.get("Connection") == "close",
      f"a POST: body, length, Host with the port, the caller's headers ({r['err']}, {e})")
seen.clear()
r = run(f"https://localhost:{sname}/echo")
check(ok(r), f"a name: {r['err']}")

# the clocks and the stops
t = time.time(); r = run("-l", "2:1", f"https://127.0.0.1:{s13}/slow"); dt = time.time() - t
check(r["rc"] == -1 and "nothing came for 2 s" in r["err"] and dt < 3.8, f"nothing for 2 s ends it ({r['err']}, {dt:.1f} s)")
r = run("-l", "2:100", f"https://127.0.0.1:{s13}/trickle")
check(r["rc"] == -1 and "slower than 100 bytes a second" in r["err"], f"below the low speed limit ({r['err']})")
r = run("-l", "3:5", f"https://127.0.0.1:{s13}/trickle")
check(ok(r, 60), f"above it: read whole ({r['err']})")
t = time.time(); r = run("-t", "2", f"https://127.0.0.1:{s13}/slow"); dt = time.time() - t
check(r["rc"] == -1 and "within 2 s" in r["err"] and dt < 3.5, f"the whole's time ({r['err']}, {dt:.1f} s)")
t = time.time(); r = run("-x", "500", f"https://127.0.0.1:{s13}/slow"); dt = time.time() - t
check(r["rc"] == -3 and r["err"] == "cancelled" and dt < 1.8, f"a stop from another thread: within 250 ms ({r['err']}, {dt:.1f} s)")
t = time.time(); r = run("-l", "2:1", "-w", "3000", f"https://127.0.0.1:{s13}/big"); dt = time.time() - t
check(ok(r, len(BIG)) and body() == BIG and dt >= 3, f"a reader that holds a piece back 3 s is no silence of the server's ({r['err']}, {dt:.1f} s)")
big = os.urandom(3 << 20); bf = os.path.join(d, "post.bin"); open(bf, "wb").write(big)
for port, what in ((s13, "TLS"), (plain, "plain")):
    r = run("-D", bf, "-t", "30", f"{'https' if port == s13 else 'http'}://127.0.0.1:{port}/sum")
    check(ok(r) and body() == hashlib.sha256(big).hexdigest().encode(), f"3 MB POST to a server that reads late ({what}): the body whole, each byte once ({r['err']})")
r = run("-e", "20000", f"https://127.0.0.1:{s13}/big")
check(r["rc"] == -2 and 20000 <= r["bytes"] < len(BIG), f"the reader ends it ({r})")
# a server that never answers the handshake: the connection's time
silent = socket.socket(); silent.bind(("127.0.0.1", 0)); silent.listen(5)
t = time.time(); r = run("-c", "2", f"https://127.0.0.1:{silent.getsockname()[1]}/"); dt = time.time() - t
check(r["rc"] == -1 and "handshake" in r["err"] and dt < 3.5, f"no handshake: the connection's time ({r['err']}, {dt:.1f} s)")
t = time.time(); r = run("-x", "400", f"https://127.0.0.1:{silent.getsockname()[1]}/"); dt = time.time() - t
check(r["rc"] == -3 and dt < 1.5, f"a stop during the handshake ({r['err']}, {dt:.1f} s)")
r = run(f"https://127.0.0.1:1/")
check(r["rc"] == -1 and "refused" in r["err"].lower(), f"nobody there ({r['err']})")

# addresses and headers that are refused
for u, why in ((f"https://user:pw@127.0.0.1:{s13}/", "user or password"), (f"https://127.0.0.1:{s13}/a b", "space or control"),
               (f"https://127.0.0.1:{s13}/a\r\nX: 1", "space or control"), ("ftp://x/", "not an http"), ("https://:443/", "no host"),
               (f"https://127.0.0.1:99999/", "bad port"), ("https://[::1/", "IPv6")):
    r = run(u)
    check(r["rc"] == -1 and why in r["err"], f"refused: {u!r} ({r['err']})")
r = run("-H", "X-Bad: a\r\nX-Injected: 1", f"https://127.0.0.1:{s13}/")
check(r["rc"] == -1 and "control character" in r["err"], f"a header with a line break: refused ({r['err']})")

sys.exit(1 if failed else 0)
