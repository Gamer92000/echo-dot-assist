#!/usr/bin/env python3
"""Copying Amazon's artifacts (wake word sets, the newer sound detection model, the whisper model) from one Echo to another
through the settings page's API (src/hassmic/artifacts.c), with two build/hassmic-host: listed with digests, read and
written in pieces over signed requests with signed answers, checked on arrival (sizes, digest, the receiving engine
loads it: pryon_test, faked here), then installed by root's scripts/device/artifact-install.sh, which this test runs
as root would, and found after a restart."""
import hashlib, json, os, shutil, signal, stat, subprocess, sys, tempfile, time
from urllib.parse import quote
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webclient import Browser

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHUNK = 192 * 1024


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False


class Echo:
    def __init__(self, name, port):
        self.name, self.port, self.web = name, port, port + 10
        self.data = tempfile.mkdtemp()                          # /data/local/hassmic
        self.state = os.path.join(self.data, "state"); os.makedirs(self.state)
        self.log = os.path.join(self.state, "log")
        fake = os.path.join(self.data, "pryon_test")            # 1 = the set does not load, 3 = loaded (no audio), as the real one
        with open(fake, "w") as f: f.write('#!/bin/sh\ngrep -q BROKEN "$2" && exit 1\nexit 3\n')
        os.chmod(fake, 0o755)
        self.env = dict(os.environ, HASSMIC_STATE=self.state, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw",
                        HASSMIC_PLAY=os.path.join(self.state, "play.raw"), HASSMIC_MDNS_FILE=os.path.join(self.state, "none"),
                        HASSMIC_MODELS=os.path.join(self.data, "models"), HASSMIC_AED=os.path.join(self.data, "aed"),
                        HASSMIC_WHISPER=os.path.join(self.data, "whisper"), HASSMIC_PRYON_TEST=fake)

    def start(self):
        self.proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(self.port), "-n", self.name, "-L",
                                      "-z", "0", "-o", "0", "-a", "0", "-W", str(self.web)], env=self.env, stderr=open(self.log, "a"))
        time.sleep(0.6)

    def stop(self): self.proc.terminate(); self.proc.wait()

    def model(self, rel, files):
        d = os.path.join(self.data, rel); os.makedirs(d, exist_ok=True)
        for n, b in files.items():
            os.makedirs(os.path.dirname(os.path.join(d, n)), exist_ok=True)       # "sub/file": one level of folders
            with open(os.path.join(d, n), "wb") as f: f.write(b)


def digest(files):
    """artifacts.c's: BLAKE2b-256 over name\\0size\\0content, file by file in name order"""
    h = hashlib.blake2b(digest_size=32)
    for n in sorted(files): h.update(n.encode() + b"\0" + str(len(files[n])).encode() + b"\0" + files[n])
    return h.hexdigest()


def arts(b):
    st, _, data = b.call("GET", "/api/artifacts")
    return json.loads(data) if st == 200 else None


def copy(src, dst, art, tamper=False):
    """what the page does: begin, the pieces from src to dst, commit; returns commit's (status, answer)"""
    spec = f"{art['id']} {art['digest']}\n" + "".join(f"{f['name']} {f['size']}\n" for f in art["files"])
    st, _, data = dst.call("POST", "/api/artifact/begin", spec.encode())
    if st != 200: return st, json.loads(data)
    for f in art["files"]:
        for off in range(0, f["size"], CHUNK):
            n = min(CHUNK, f["size"] - off)
            st, _, piece = src.call("GET", f"/api/artifact/read/{art['id']}/{quote(f['name'], safe='')}/{off}/{n}")
            assert st == 200 and len(piece) == n, (st, len(piece))
            if tamper and off == 0: piece = bytes([piece[0] ^ 1]) + piece[1:]
            st, _, data = dst.call("POST", f"/api/artifact/chunk/{art['id']}/{quote(f['name'], safe='')}/{off}", piece)
            if st != 200: return st, json.loads(data)
    st, _, data = dst.call("POST", f"/api/artifact/commit/{art['id']}")
    return st, json.loads(data)


def root_install(e, read_as=""):
    """root's watcher in main.sh: the installer, here without runas (READ_AS empty: the test's own user reads)"""
    return subprocess.run(["sh", f"{ROOT}/scripts/device/artifact-install.sh", e.state, e.data], capture_output=True, text=True,
                          env=dict(os.environ, READ_AS=read_as))


def main():
    a, b = Echo("Echo Source", 17001), Echo("Echo Target", 17002)
    wake = {"pryon.manifest": b"manifest\n", "int16_streaming.onnx": os.urandom(CHUNK * 3 + 1234), "kw.cfg.json": b"{}"}
    broken = {"pryon.manifest": b"BROKEN\n", "x.bin": os.urandom(1000)}
    aed = {"pryon.manifest": b"aed\n", "AED.json": b"{}", "model.mlp": os.urandom(50000)}
    whisper = {"pryon_whisper.manifest": b"w\n", "HCLG.fst": os.urandom(300000),
               "whisper_components/model.v8.0.mlp": os.urandom(CHUNK + 10), "whisper_components/dnn_vad.mlp": os.urandom(2000)}
    a.model("models/echo-de-DE", wake); a.model("models/ziggy-de-DE", broken); a.model("aed", aed); a.model("whisper", whisper)
    a.model("models/.try-x", {"pryon.manifest": b"half\n"})            # scripts/artifacts.sh testing one: not listed
    a.start(); b.start()
    try:
        pa, pb = Browser(a.web), Browser(b.web)
        check(pa.login_with_button(a.proc) and pb.login_with_button(b.proc), "logged in to both with their action buttons")
        la = arts(pa)
        by = {x["id"]: x for x in la["artifacts"]}
        check(set(by) == {"wake:echo-de-DE", "wake:ziggy-de-DE", "sound", "whisper"}, f"listed: the sets, sound and whisper models, no .try-: {sorted(by)}")
        check(by["wake:echo-de-DE"]["digest"] == digest(wake) and by["sound"]["digest"] == digest(aed) and by["whisper"]["size"] == sum(map(len, whisper.values()))
              and by["whisper"]["digest"] == digest(whisper),
              "digests as artifacts.c defines them (files in a folder by their path), sizes")
        check(arts(pb)["artifacts"] == [] and la["free"] > 0, "the target has none; free space reported")
        st = __import__("webclient").req(a.web, "GET", "/api/artifacts")[0]
        check(st == 401, "unsigned: refused")

        # a wake word set, in pieces (the big file takes four), from one Echo to the other
        st, r = copy(pa, pb, by["wake:echo-de-DE"])
        check(st == 200 and r.get("ok") and "arrived whole" in open(b.log).read(), f"wake word set copied and checked: {r}")
        st, r = copy(pa, pb, by["whisper"], tamper=True)
        check(st == 400 and "digest" in r["error"] and not os.path.exists(os.path.join(b.state, "artifacts", "whisper")),
              f"a piece changed on the way: digest does not match, dropped: {r}")
        st, r = copy(pa, pb, by["wake:ziggy-de-DE"])
        check(st == 400 and "cannot load" in r["error"], f"a set this Echo's engine cannot load: refused: {r}")
        st, r = copy(pa, pb, by["sound"])
        check(st == 200, "the sound detection model copied")
        st, _, data = pb.call("POST", "/api/artifact/begin", b"wake:../etc " + b"0" * 64 + b"\npasswd 10\n")
        check(st == 400, f"an id with a path in it: refused: {data}")
        st, _, data = pb.call("POST", "/api/artifact/begin", b"wake:echo-en-US " + b"0" * 64 + b"\n../x 10\n")
        check(st == 400, f"a file name with a path in it: refused: {data}")
        st, _, data = pa.call("GET", "/api/artifact/read/wake:echo-de-DE/..%2Fx/0/10")
        check(st == 400, "reading outside the set: refused")
        pb.call("POST", "/api/artifact/begin", f"whisper {by['whisper']['digest']}\nHCLG.fst 10\n".encode())
        st, _, data = pb.call("POST", "/api/artifact/chunk/whisper/HCLG.fst/5", b"x" * 6)
        check(st == 400 and b"past the end" in data, "a piece past the end of its file: refused")
        st, _, data = pb.call("POST", "/api/artifact/commit/whisper")
        check(st == 400 and b"incomplete" in data, "commit with a file not whole: refused")
        st, r = copy(pa, pb, by["whisper"])
        check(st == 200, f"the whisper model with its folder copied: {r}")
        st, _, data = pb.call("POST", "/api/artifact/begin", b"wake:echo-en-US " + b"0" * 64 + b"\na/b/x 10\n")
        check(st == 400, f"a file in a folder in a folder: refused: {data}")
        check(sorted(arts(pb)["staged"]) == ["sound", "wake.echo-de-DE", "whisper"], f"ready to install: {arts(pb)['staged']}")

        # install: hassmic names them for root; root's installer puts them in place
        st, _, _ = pb.call("POST", "/api/artifact/install")
        req = open(os.path.join(b.state, "artifacts", "request")).read().split()
        check(st == 200 and sorted(req) == ["sound", "wake:echo-de-DE", "whisper"], f"handed to root: {req}")
        r = root_install(b)
        dst = os.path.join(b.data, "models", "echo-de-DE")
        check(r.returncode == 0 and "OK wake:echo-de-DE" in r.stdout and "OK sound" in r.stdout
              and all(open(os.path.join(dst, n), "rb").read() == c for n, c in wake.items())
              and stat.S_IMODE(os.stat(dst).st_mode) == 0o755 and not os.path.exists(os.path.join(b.state, "artifacts", "wake.echo-de-DE")),
              f"root installed them (folder 755, staged copy gone): {r.stdout.strip()!r}")
        wd = os.path.join(b.data, "whisper")
        check("OK whisper" in r.stdout and all(open(os.path.join(wd, n), "rb").read() == c for n, c in whisper.items())
              and stat.S_IMODE(os.stat(os.path.join(wd, "whisper_components")).st_mode) == 0o755
              and stat.S_IMODE(os.stat(os.path.join(wd, "whisper_components", "dnn_vad.mlp")).st_mode) == 0o644,
              "and the whisper model with its folder (755, files 644)")
        check(arts(pb)["artifacts"] and {x["id"]: x for x in arts(pb)["artifacts"]}["whisper"]["digest"] == by["whisper"]["digest"],
              "the target now lists the same whisper model")
        check("OK wake:echo-de-DE" in arts(pb)["result"], "the result shows on the page")

        # root reads what is staged as the daemon: a link there is refused, never followed
        stage = os.path.join(b.state, "artifacts", "wake.evil")
        os.makedirs(stage); os.symlink("/etc/hostname", os.path.join(stage, "pryon.manifest"))
        open(stage + ".ready", "w").close()
        with open(os.path.join(b.state, "artifacts", "request"), "w") as f: f.write("wake:evil\nwake:../../x\n")
        r = root_install(b)
        check(r.returncode != 0 and "FAILED wake:evil: pryon.manifest is not a plain file" in r.stdout and "FAILED wake:../../x: bad name" in r.stdout
              and not os.path.exists(os.path.join(b.data, "models", "evil")), f"a staged link and a bad id: refused by root: {r.stdout.strip()!r}")
        # the same one level down, and a folder deeper than that
        for name, make in (("evil2", lambda s: (os.makedirs(os.path.join(s, "sub")), os.symlink("/etc/hostname", os.path.join(s, "sub", "x")))),
                           ("deep", lambda s: (os.makedirs(os.path.join(s, "a", "b")), open(os.path.join(s, "a", "b", "x"), "w").close()))):
            stage = os.path.join(b.state, "artifacts", "wake." + name)
            make(stage); open(os.path.join(stage, "pryon.manifest"), "w").close(); open(stage + ".ready", "w").close()
            with open(os.path.join(b.state, "artifacts", "request"), "w") as f: f.write(f"wake:{name}\n")
            r = root_install(b)
            check(r.returncode != 0 and f"FAILED wake:{name}:" in r.stdout and not os.path.exists(os.path.join(b.data, "models", name)),
                  f"{'a link in a folder' if name == 'evil2' else 'a folder in a folder'}: refused by root: {r.stdout.strip()!r}")

        # wake word sets installed by hand under the short name get the long one at the next start (main.sh, as root)
        b.model("models/echo-de", wake)                     # the same set as echo-de-DE: goes
        b.model("models/alexa-de", {"pryon.manifest": b"a\n"})   # alone: renamed, and the active one: the choice follows
        b.model("models/echo-en", {"pryon.manifest": b"e\n"})    # en comes in several regions: left alone
        b.model("models/computer-it", {"pryon.manifest": b"c\n"}); b.model("models/computer-it-IT", {"pryon.manifest": b"other\n"})
        with open(os.path.join(b.state, "wake_word"), "w") as f: f.write("alexa-de\n")
        r = subprocess.run(["sh", f"{ROOT}/scripts/device/artifact-install.sh", "migrate", b.state, b.data], capture_output=True, text=True)
        names = sorted(os.listdir(os.path.join(b.data, "models")))
        check(r.returncode == 0 and names == ["alexa-de-DE", "computer-it", "computer-it-IT", "echo-de-DE", "echo-en"]
              and open(os.path.join(b.state, "wake_word")).read().strip() == "alexa-de-DE",
              f"short names migrated (same set: dropped; different: both kept; en: left), active wake word followed: {names} {r.stdout.strip()!r}")
        r = subprocess.run(["sh", f"{ROOT}/scripts/device/artifact-install.sh", "migrate", b.state, b.data], capture_output=True, text=True)
        check(r.stdout.strip() == "models: computer-it and computer-it-IT differ, both kept" and sorted(os.listdir(os.path.join(b.data, "models"))) == names,
              "a second start changes nothing")

        # restarted (root restarts hassmic): the set is in the wake word list's folder, with the same digest
        b.stop(); b.start()
        lb = {x["id"]: x for x in arts(pb)["artifacts"]}
        check(lb.get("wake:echo-de-DE", {}).get("digest") == by["wake:echo-de-DE"]["digest"] and lb.get("sound", {}).get("digest") == by["sound"]["digest"]
              and "wake:alexa-de-DE" in lb and "wake:echo-de" not in lb, "after the restart: the same sets on the target, under the long names")
        check("wake word: alexa-de-DE \"Alexa\" (de), active" in open(b.log).read(), "the migrated wake word is the active one")
    finally:
        for e in (a, b):
            try: e.stop()
            except Exception: pass
    if check.failed:
        for e in (a, b): print(f"--- {e.name}\n{open(e.log).read()[-3000:]}")
        print("FAILED"); raise SystemExit(1)
    print("PASS")

main()
