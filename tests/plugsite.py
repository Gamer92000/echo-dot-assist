"""A stand-in for the plugin index's site (GitHub Pages: index.json, revoked.json, their signatures, packages/), as
hassmic's tests serve it to build/hassmic-host (HASSMIC_PLUGIN_INDEX, HASSMIC_PLUGINS_PUB; plugin_fetch.c).

Built by the index's own tool (hassmic-plugins' tools/index.py build and sign, --arch x86_64 for the PC's test
plugins) where that tree is beside this one (HASSMIC_PLUGINS_TREE for another place), so that the format the Echo
reads is the one the index writes and cannot drift away from it.  Where it is not (CI checks out this repository
alone), by the builder below, which writes the same bytes: Site.build checks that it does whenever the tool is
there, so a change of the tool's format fails a test here before it reaches an Echo.

The index lists only a package its plugin's own GitHub Actions built and attested (hassmic-plugins' tools/attest.py,
docs/plugins.md 5.3): the tool asks gh, which is that tree's stand-in here (tests/fake_gh.py, INDEX_GH: every package
attested as its repository's honest build at the entry's commit, in the entry's workflow's own run for the push of
the release's tag), and the builder below writes the bundle that stand-in makes.  The Echo reads none of it
(index.json's workflow, release, release_sha256, attestation, attestation_sha256)."""
import base64, hashlib, json, os, shutil, struct, subprocess, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "sdk"))
import plugcheck
otatool = plugcheck.otatool
TREE = os.environ.get("HASSMIC_PLUGINS_TREE", os.path.join(os.path.dirname(ROOT), "hassmic-plugins"))
TOOL = os.path.join(TREE, "tools", "index.py")
FAKE_GH = os.path.join(TREE, "tests", "fake_gh.py")
COMMIT = "c" * 40                                   # each version's commit, as the entries name it
WORKFLOW = ".github/workflows/release.yml"          # and its release workflow (the template's, microWakeWord's)
SOURCE = "https://github.com/Gamer92000/"          # the index's OFFICIAL_SOURCE
SIGN_INDEX, SIGN_REVOKED = b"hassmic plugin index 1\n", b"hassmic plugin revoked 1\n"
CAPS = ("storage", "files", "state", "log.read", "log.read.full", "net.server", "net.lan", "net.internet", "net.udp", "net.discover",
        "audio.music", "audio.volume", "call", "wake", "request.audio", "request.text", "ha.entities", "ha.actions", "button")


def tiny_elf():
    """the least an x86_64 program has to show to pass the index's file rules: static, a stack marked not executable
    (a plugin listed and never installed in a test needs no program that runs)"""
    h = bytearray(64)
    h[:7] = b"\x7fELF\x02\x01\x01"
    struct.pack_into("<HH", h, 16, 2, 62)
    struct.pack_into("<Q", h, 32, 64)
    struct.pack_into("<HH", h, 54, 56, 2)
    ph = lambda t, fl: struct.pack("<II6Q", t, fl, 0, 0, 0, 0, 0, 0)
    return bytes(h) + ph(1, 5) + ph(0x6474E551, 6) + b"program"


def dump(obj):
    return json.dumps(obj, indent=1, ensure_ascii=False).encode() + b"\n"


class Site:
    """the files of one site, in DIR, and the server that serves whichever is current"""

    def __init__(self, top, port, key="index"):
        self.top, self.port = top, port
        self.key, self.pub = os.path.join(top, key + ".key"), os.path.join(top, key + ".pub")
        if not os.path.exists(self.key): otatool.keygen(self.key, self.pub)
        self.dir = None
        self.rserial = 0            # the highest revocation serial served: what a build without one keeps (an Echo refuses a lower one)
        self.iserial = 0            # the highest index serial built: a build without one goes past it (the time - 60 s may not)
        self.hits, self.override = [], {}
        self.agents = []            # each request's User-Agent, in order
        self.redirect = {}          # a file's name -> where it is redirected to (302)
        self.down = False           # out of reach: the connection closes without an answer
        self.slow = 0               # seconds before a package's body
        self.slow_api = 0           # seconds before index.json's
        self.used_tool = os.path.isfile(TOOL)
        site = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *a): pass

            def do_GET(self):
                site.hits.append(self.path); site.agents.append(self.headers.get("User-Agent"))
                if site.down: self.close_connection = True; return
                name = self.path.lstrip("/")
                if name in site.redirect:
                    self.send_response(302); self.send_header("Location", site.redirect[name]); self.send_header("Content-Length", "0"); self.end_headers(); return
                data = site.override.get(name)
                if data is None and site.dir and ".." not in name and os.path.isfile(os.path.join(site.dir, name)):
                    data = open(os.path.join(site.dir, name), "rb").read()
                if data is None:
                    body = b"not found"
                    self.send_response(404); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body); return
                try:
                    self.send_response(200); self.send_header("Content-Length", str(len(data))); self.end_headers()
                    if name == "index.json" and site.slow_api: time.sleep(site.slow_api)
                    if name.startswith("packages/") and site.slow: self.wfile.write(data[:100]); self.wfile.flush(); time.sleep(site.slow); data = data[100:]
                    self.wfile.write(data)
                except OSError: pass        # (the Echo let go of the request: a cancelled look or download)

        self.server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    base = property(lambda self: f"http://127.0.0.1:{self.port}/")

    def downloads(self):
        return [h for h in self.hits if h.startswith("/packages/")]

    def build(self, plugins, serial=None, revoked=None, revoked_serial=None, key=None):
        """PLUGINS: {id: [package bytes as its author released it (no listing), ...]} in version order; the site with
        each package listed by KEY (the index key by default; another: what a throwaway key's build makes), the
        lists signed by the index key.  REVOKED: entries of revoked.json, under REVOKED_SERIAL (by default the
        highest so far, one more with entries).  The current site from then on.  Returns its folder"""
        serial = serial or max(int(time.time()) - 60, self.iserial + 1)
        self.iserial = max(self.iserial, serial)
        key = key or self.key
        n = len([d for d in os.listdir(self.top) if d.startswith("site")])
        root, pkgs, out = (os.path.join(self.top, f"{w}{n}") for w in ("siteroot", "sitepkgs", "site"))
        for d in (os.path.join(root, "plugins"), os.path.join(root, "keys"), pkgs): os.makedirs(d)
        shutil.copy(self.pub, os.path.join(root, "keys", "index.pub"))
        rv = {"serial": revoked_serial if revoked_serial is not None else self.rserial + bool(revoked), "entries": revoked or []}
        self.rserial = max(self.rserial, rv["serial"])
        open(os.path.join(root, "revoked.json"), "w").write(json.dumps(rv))
        entries = []
        for pid, packages in plugins.items():
            vs, first = [], None
            for data in packages:
                f = plugcheck_members(data)
                m = json.loads(f["manifest.json"]); first = first or m
                open(os.path.join(pkgs, f"{pid}-{m['version']}.hmp"), "wb").write(data)
                vs.append({"version": m["version"], "commit": COMMIT, "workflow": WORKFLOW, "manifest": hashlib.blake2b(f["manifest.json"], digest_size=32).hexdigest(),
                           "package": f"{SOURCE}{pid}/releases/download/v{m['version']}/{pid}-{m['version']}.hmp"})
            e = {"id": pid, "name": (first or {}).get("name", pid), "description": (first or {}).get("description", "A plugin."),
                 "author": (first or {}).get("author", "tests"), "author_key": (first or {}).get("author_key", "ab" * 32), "tier": "official",
                 "source": f"{SOURCE}{pid}", "versions": vs}
            open(os.path.join(root, "plugins", pid + ".json"), "w").write(json.dumps(e))
            entries.append(e)
        if self.used_tool:
            for cmd in (["build", pkgs, out, "--serial", str(serial), "--key", key, "--arch", "x86_64", "--root", root], ["sign", out, self.key, "--root", root]):
                env = {k: v for k, v in os.environ.items() if k != "INDEX_FAKE_GH"}
                r = subprocess.run([sys.executable, TOOL] + cmd, capture_output=True, text=True, env=dict(env, INDEX_GH=FAKE_GH))
                if r.returncode: raise RuntimeError(f"the index's tool: {' '.join(cmd[:1])}: {r.stdout}{r.stderr}")
            mine = self.make(sorted(entries, key=lambda e: e["id"] + ".json"), pkgs, os.path.join(self.top, f"mine{n}"), serial, rv, key, self.key)
            for name in ("index.json", "revoked.json", "index.json.sig", "revoked.json.sig") + tuple(
                    "packages/" + n for n in sorted(os.listdir(os.path.join(out, "packages")) if os.path.isdir(os.path.join(out, "packages")) else [])
                    if n.endswith(".sigstore.jsonl")):
                if open(os.path.join(out, name), "rb").read() != open(os.path.join(mine, name), "rb").read():
                    raise RuntimeError(f"tests/plugsite.py writes {name} otherwise than the index's tool: follow it")
        else:
            self.make(sorted(entries, key=lambda e: e["id"] + ".json"), pkgs, out, serial, rv, key, self.key)
        self.dir = out
        return out

    @staticmethod
    def make(entries, pkgs, out, serial, rv, key, signer):
        """what the index's build (listing with KEY) and sign (with SIGNER) write, for the Echo's part of it:
        index.json, revoked.json, their signatures, the packages with their listings (index.html is for people)"""
        sk = otatool.secret(signer)
        plugins = []
        os.makedirs(os.path.join(out, "packages"))
        # (what the revocation list withdraws is left out of the index, a plugin all of whose versions it withdraws
        # with them: the index's withdrawn())
        gone = lambda pid, ver, key: any(("author_key" in x and x["author_key"] == key) or (x.get("id") == pid and ("upto" not in x or ver <= x["upto"])) for x in rv["entries"])
        for e in entries:
            vs = []
            for v in e["versions"]:
                name = f"{e['id']}-{v['version']}.hmp"
                if gone(e["id"], v["version"], e["author_key"]): continue
                f = plugcheck_members(open(os.path.join(pkgs, name), "rb").read())
                m = json.loads(f["manifest.json"])
                f["listing.sig"] = plugcheck.listing(m, f["manifest.json"], key, e["tier"], serial)
                data = plugcheck.tar_bytes([(k, f[k]) for k in ("manifest.json", "manifest.sig", "plugin", "LICENSE", "listing.sig") if k in f])
                open(os.path.join(out, "packages", name), "wb").write(data)
                released = unlisted(data)
                tag = v["package"].rsplit("/", 2)[-2]
                bundles = attested_bundle(e["source"][len("https://github.com/"):], v["commit"], v["workflow"], tag, name, released)
                open(os.path.join(out, "packages", name + ".sigstore.jsonl"), "wb").write(bundles)
                vs.append({"version": v["version"], "version_name": m.get("version_name", str(v["version"])), "requires": m["requires"], "models": m.get("models", ["*"]),
                           "capabilities": m["capabilities"], "commit": v["commit"], "workflow": v["workflow"],
                           "manifest": v["manifest"], "package": "packages/" + name, "size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                           "release": v["package"], "release_sha256": hashlib.sha256(released).hexdigest(), "attestation": "packages/" + name + ".sigstore.jsonl",
                           "attestation_sha256": hashlib.sha256(bundles).hexdigest()})
            if vs or not e["versions"] or not all(gone(e["id"], v["version"], e["author_key"]) for v in e["versions"]):
                plugins.append({**{k: e[k] for k in ("id", "name", "description", "author", "author_key", "tier", "source")}, "versions": vs})
        files = {"index.json": dump({"index": 1, "serial": serial, "revoked_serial": rv["serial"], "plugins": plugins}),
                 "revoked.json": dump({"revoked": 1, "serial": rv["serial"], "entries": rv["entries"]})}
        for name, data in files.items():
            open(os.path.join(out, name), "wb").write(data)
            open(os.path.join(out, name + ".sig"), "wb").write(otatool.sign(sk, (SIGN_INDEX if name == "index.json" else SIGN_REVOKED) + data))
        return out

    def resign(self, name, data, key=None):
        """a list of the site replaced by DATA, signed (by KEY, the index key by default): what only the key's holder can make"""
        sk = otatool.secret(key or self.key)
        open(os.path.join(self.dir, name), "wb").write(data)
        open(os.path.join(self.dir, name + ".sig"), "wb").write(otatool.sign(sk, (SIGN_INDEX if name == "index.json" else SIGN_REVOKED) + data))

    def shutdown(self):
        self.server.shutdown()


def plugcheck_members(data):
    """a package's members from its bytes (plugcheck.members reads a file)"""
    import io, tarfile
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:") as t:
        return {ti.name: t.extractfile(ti).read() for ti in t}


def unlisted(data):
    """a package without its listing: what the plugin's repository released, before the index listed it"""
    f = plugcheck_members(data)
    return plugcheck.tar_bytes([(k, f[k]) for k in ("manifest.json", "manifest.sig", "plugin", "LICENSE") if k in f])


def attested_bundle(repo, commit, workflow, tag, name, data):
    """the bundle (one JSON line) the index keeps for the package DATA, NAME, as its stand-in gh attests it: its
    repository REPO's honest build at COMMIT, in WORKFLOW's own run for the push of TAG (hassmic-plugins'
    tests/fake_gh.py, its resolved and bundle_of; the index writes it again as one line, tools/attest.py verify)"""
    ref = f"refs/tags/{tag}"
    f = {"repo": repo, "commit": commit, "runner": "github-hosted", "signer_repo": repo, "signer_commit": commit,
         "issuer": "https://token.actions.githubusercontent.com", "predicate": "https://slsa.dev/provenance/v1", "subject": hashlib.sha256(data).hexdigest(),
         "workflow": workflow, "caller": workflow, "ref": ref, "trigger": "push", "config_commit": commit}
    st = {"_type": "https://in-toto.io/Statement/v1", "subject": [{"name": name, "digest": {"sha256": f["subject"]}}], "predicateType": f["predicate"],
          "predicate": {"buildDefinition": {"buildType": "https://actions.github.io/buildtypes/workflow/v1",
                                            "externalParameters": {"workflow": {"path": workflow, "ref": ref, "repository": f"https://github.com/{repo}"}},
                                            "internalParameters": {"github": {"event_name": "push", "runner_environment": f["runner"]}},
                                            "resolvedDependencies": [{"digest": {"gitCommit": commit}, "uri": f"git+https://github.com/{repo}@{ref}"}]}}}
    b = {"mediaType": "application/vnd.dev.sigstore.bundle.v0.3+json", "verificationMaterial": {"x-fake": f},
         "dsseEnvelope": {"payload": base64.b64encode(json.dumps(st, separators=(",", ":")).encode()).decode(),
                          "payloadType": "application/vnd.in-toto+json", "signatures": [{"sig": "ZmFrZQ=="}]}}
    return json.dumps(b, separators=(",", ":"), ensure_ascii=False).encode() + b"\n"
