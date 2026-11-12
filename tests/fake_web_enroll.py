#!/usr/bin/env python3
"""A wake word of your own, trained on the Echo (enroll.c), through the settings page's API on build/hassmic-host.
The PC build has no Amazon engine and no tools of ours that run on it: pryon_extract and pryon_train are stand-ins
(HASSMIC_PRYON_EXTRACT / _TRAIN) that behave as the real ones print, and the stock set is a small stand-in too
(HASSMIC_ENROLL_BASE).  What is real here: the prompts and the countdown, the recordings taken from the capture thread's
audio (and the wake word kept out meanwhile), the manifest the trainer is given (which recordings are held back), the
progress, the verdict gate, the set made and staged as any model set is, the refusals, a cancel that ends the tool, and
the wake word that becomes the pick.  The real tools run under qemu in tests/fake_ha.py's world, and on the Echo."""
import json, os, stat, subprocess, sys, tempfile, threading, time, shutil, fcntl
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webclient import wait_port, Browser

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT, WEB = 16985, 16986


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False

EXTRACT = """#!/usr/bin/env python3
import struct, sys, time
manifest, audio, out = sys.argv[1:4]
n = len(open(audio, 'rb').read()) // 2
time.sleep(0.05)
cycles = n // 960
open(out, 'wb').write(struct.pack('<4I', 0x46595250, 1, 25, 100) + bytes(cycles * 10000))
sys.stderr.write('samples=%d cycles=%d rows=25 columns=100 capture_error=0\\n' % (n, cycles))
"""

TRAIN = """#!/usr/bin/env python3
import json, os, shutil, sys, time
source, manifest, out, steps = sys.argv[1:5]
rows = json.load(open(manifest)); here = os.path.dirname(manifest)
open(os.path.join(os.environ['FAKE_LOG_DIR'], 'trainer-args'), 'w').write(json.dumps(
    dict(rows=rows, files=[os.path.exists(os.path.join(here, r['path'])) for r in rows], steps=steps, source=source)))
print(json.dumps(dict(phase='loaded', training_recordings=sum(r['split'] == 'train' for r in rows))), flush=True)
slow = os.environ.get('FAKE_TRAIN_SLOW')
for s in (0, 500, 1000):
    print(json.dumps(dict(phase='initialization', step=s, loss=1.0)), flush=True); time.sleep(30 if slow else 0.15)
for s in (0, 800, 1600):
    print(json.dumps(dict(phase='qat', step=s, loss=0.1, best_loss=0.1)), flush=True); time.sleep(0.15)
shutil.copyfile(source, out)
bad = os.environ.get('FAKE_TRAIN_FAIL')
t = '{"tp":%d,"fn":0,"fp":0,"tn":%d}'
print('{"phase":"complete","seconds":1.0,"train":%s,"test":{"tp":%d,"fn":%d,"fp":0,"tn":2}}' % (t % (6, 6), 1 if bad else 2, 1 if bad else 0), flush=True)
"""


def tool(path, text):
    open(path, "w").write(text); os.chmod(path, 0o755)


def status(b):
    st, _, body = b.call("GET", "/api/enroll")
    return json.loads(body) if st == 200 else {}


def start(b, phrase, secs=2, use=1):
    st, _, body = b.call("POST", "/api/enroll/start", f"phrase={phrase}\nseconds={secs}\nuse={use}\n".encode())
    return st, json.loads(body)


def wait_state(b, states, secs=120, seen=None, prompts=None):
    end = time.monotonic() + secs
    while time.monotonic() < end:
        s = status(b).get("session", {})
        if seen is not None and s.get("state") and (not seen or seen[-1] != s["state"]): seen.append(s["state"])
        if prompts is not None and s.get("prompt") and s["prompt"]["phase"] == "speak":
            t = (s["prompt"]["index"], s["prompt"]["kind"], s["prompt"]["text"], s["prompt"]["held_out"])
            if t not in prompts: prompts.append(t)
        if s.get("state") in states: return s
        time.sleep(0.1)
    return status(b).get("session", {})


def make_base(d):
    os.makedirs(d)
    w = lambda n, t: open(os.path.join(d, n), "w").write(t)
    w("pryon.manifest", '{"checksum":"checksum.txt"}'); w("checksum.txt", "74a26372ecb79fc6e5954d9f9fff29a3")
    w("kw.cfg.json", '{"keyword-orchestra":{"keyword-spotters":[{"name":"ALEXA","t":0.923},{"name":"STOP","t":0.55}]}}')
    w("op.cfg.json", '{"awake":{"rules":[{"name":"ALEXA","next":"awake"},{"name":"STOP","next":"sleep"}]},"sleep":{"rules":[{"name":"ALEXA","next":"awake"}]}}')
    w("watermark.cfg.json", '{"enabled_keys":[{"keyword_name":"ALEXA"}]}')
    w("words.shrunk.txt", "<eps> 0\nALEXA 1\nSTOP 2\n")
    w("runtime_stateless_standard.streaming.fixed.int16.onnx", "ORIGINAL-MODEL")


class Root(threading.Thread):
    """root's watcher (main.sh, artifact-install.sh): takes state/artifacts/request and answers in state/artifacts/result"""
    def __init__(self, state):
        super().__init__(daemon=True); self.state, self.mode, self.seen, self.stop = state, "ok", [], False
    def run(self):
        a = os.path.join(self.state, "artifacts"); req = os.path.join(a, "request")
        while not self.stop:
            if os.path.exists(req):
                ids = open(req).read().split(); self.seen.append(ids); os.remove(req)
                time.sleep(1.5)
                if self.mode == "ok":
                    for i in ids:
                        shutil.copytree(os.path.join(a, i.replace(":", ".")), os.path.join(self.state, "models", i.split(":")[1]), dirs_exist_ok=True)
                open(os.path.join(a, "result"), "w").write("".join(("OK %s\n" % i) if self.mode == "ok" else ("FAILED %s: cannot put it in place\n" % i) for i in ids))
            time.sleep(0.05)


def main():
    state = tempfile.mkdtemp(); log = os.path.join(state, "log"); base = os.path.join(state, "base"); make_base(base)
    models = os.path.join(state, "models"); os.makedirs(models)
    lockpath = os.path.join(state, "installer.lock"); open(lockpath, "w").close()
    tool(os.path.join(state, "extract"), EXTRACT); tool(os.path.join(state, "train"), TRAIN)
    env = dict(os.environ, HASSMIC_STATE=state, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw", HASSMIC_PLAY=os.path.join(state, "play.raw"),
               HASSMIC_MDNS_FILE=os.path.join(state, "none"), HASSMIC_ARB_ADDR="127.255.255.255", HASSMIC_MODELS=models, HASSMIC_FAKE_WHISPER="1",
               HASSMIC_LOG=log, HASSMIC_CLOCK_SYNCED="0", HASSMIC_ENROLL_BASE=base, HASSMIC_ENROLL_READY_MS="150",
               HASSMIC_PRYON_EXTRACT=os.path.join(state, "extract"), HASSMIC_PRYON_TRAIN=os.path.join(state, "train"), FAKE_LOG_DIR=state, HASSMIC_ENROLL_INSTALL_WAIT_S="20", HASSMIC_ARTIFACT_LOCK=lockpath)
    proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(PORT), "-n", "Echo Enroll", "-L", "-z", "0", "-o", "0", "-a", "0", "-W", str(WEB)],
                            env=env, stderr=open(log, "a"))
    root = Root(state); root.start()
    try:
        wait_port(PORT, WEB)
        b = Browser(WEB)
        check(b.login_with_button(proc), "logged in with the button")
        s = status(b)
        check(s.get("available") and s.get("engine_ok") and s["session"]["state"] == "idle", f"available, Amazon's engine, idle: {s}")

        # refusals
        for phrase, why in (("Alexa", "Amazon's own word"), ("stop", "Amazon's own word"), ("ab", "too short"), ("café", "accents"), ("9lives", "starts with a digit")):
            st, j = start(b, phrase)
            check(st == 400 and j.get("error"), f"{phrase!r} refused ({why}): {j.get('error')}")
        st, j = b.call("POST", "/api/enroll/start", b"phrase=Nova\nseconds=9\n")[::2]
        check(st == 400, "9 s per recording refused")
        os.makedirs(os.path.join(models, "echo-en-US")); open(os.path.join(models, "echo-en-US", "pryon.manifest"), "w").write("{}")
        st, j = start(b, "Echo")
        check(st == 400 and "already" in j.get("error", ""), f"a name that is one of Amazon's sets is refused: {j}")

        # a whole session: recording, extracting, training, packaging, installed
        seen, prompts = [], []
        n0 = len(open(log).read())
        st, j = start(b, "Hey Nova")
        check(st == 200, f"started: {j}")
        st2, j2 = start(b, "Other")
        check(st2 == 400 and "already" in j2.get("error", ""), f"a second one is refused while it runs: {j2}")
        s = wait_state(b, ("installing", "failed", "cancelled"), 180, seen, prompts)
        check(s.get("state") == "installing" and not os.path.exists(os.path.join(state, "wake_word")), "no selection before root confirms")
        b.call("POST", "/api/enroll/cancel")  # delayed request must not overwrite the installation result
        s = wait_state(b, ("done", "failed", "cancelled"), 30, seen, prompts)
        check(s.get("state") == "done", f"done: {s.get('state')} {s.get('error')}")
        check(seen[:2] == ["recording", "extracting"] and "training" in seen, f"phases in order: {seen}")
        kinds = [k for _, k, _, _ in prompts]
        check(len(prompts) == 16 and kinds == ["word"] * 8 + ["other"] * 8, f"8 of the word, then 8 others: {kinds}")
        check({t for _, k, t, _ in prompts if k == "word"} == {"Hey Nova"}, "the word asked for is the one typed")
        others = [t for _, k, t, _ in prompts if k == "other"]
        check("Nova" in others and "Hey" in others, f"parts of the word among the others: {others}")
        check([h for *_, h in prompts] == [0] * 6 + [1] * 2 + [0] * 6 + [1] * 2, "the last two of each kind are marked held back")
        args = json.load(open(os.path.join(state, "trainer-args")))
        check(len(args["rows"]) == 16 and all(args["files"]), "the trainer got 16 activation files")
        check([r["split"] for r in args["rows"]] == ["train"] * 6 + ["test"] * 2 + ["train"] * 6 + ["test"] * 2 and [r["label"] for r in args["rows"]] == [1] * 8 + [0] * 8,
              "manifest: labels and the held-back split")
        check(args["steps"] == "1600" and args["source"] == os.path.join(base, "runtime_stateless_standard.streaming.fixed.int16.onnx"), "the trainer started from the stock model, 1600 steps")
        check(s.get("result", {}).get("test") == [2, 0, 0, 2], f"the verdict is shown: {s.get('result')}")
        stage = os.path.join(state, "artifacts", "wake.hey_nova-en-US")
        rd = lambda n: open(os.path.join(stage, n)).read()
        check(os.path.exists(stage + ".ready") and root.seen == [["wake:hey_nova-en-US"]], f"staged, committed and handed to root's installer: {root.seen}")
        check(rd("words.shrunk.txt") == "<eps> 0\nHEY_NOVA 1\nSTOP 2\n" and "STOP" not in rd("kw.cfg.json") and "HEY_NOVA" in rd("op.cfg.json") and "STOP" not in rd("op.cfg.json"), "the word in Alexa's place, STOP out")
        check(rd("runtime_stateless_standard.streaming.fixed.int16.onnx") == "ORIGINAL-MODEL" and "held-out=2/2,2/2" in rd("enrolled.txt"), "the trained model and the note")
        check(open(os.path.join(state, "wake_word")).read() == "hey_nova-en-US\n", "made the wake word")
        check(not os.path.exists(os.path.join(state, "enroll")), "the recordings and activations are gone")
        s2 = status(b)
        check(s2["session"]["state"] == "done" and s2["session"]["last"] == {"id": "hey_nova-en-US", "phrase": "Hey Nova", "in_use": 1}, f"the result stays until dismissed, and what was trained is remembered: {s2['session'].get('last')}")
        b.call("POST", "/api/enroll/dismiss")
        check(status(b)["session"]["state"] == "idle", "dismissed")
        log_text = open(log).read()[n0:]
        check("enroll: hey_nova-en-US (Hey Nova) installed, the wake word" in log_text, "the log says what happened")

        # a head that fails its check on the held-back recordings: nothing is installed
        os.remove(stage + ".ready"); os.remove(os.path.join(state, "wake_word"))
        subprocess.run(["rm", "-rf", stage])
        os.environ["FAKE_TRAIN_FAIL"] = "1"
        proc.terminate(); proc.wait(5)
        env["FAKE_TRAIN_FAIL"] = "1"
        proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(PORT), "-n", "Echo Enroll", "-L", "-z", "0", "-o", "0", "-a", "0", "-W", str(WEB)], env=env, stderr=open(log, "a"))
        wait_port(PORT, WEB); b = Browser(WEB); check(b.login_with_button(proc), "logged in again")
        st, j = start(b, "Orion", use=1)
        s = wait_state(b, ("done", "failed", "cancelled"), 180)
        check(s.get("state") == "failed" and "did not pass" in s.get("error", "") and s["result"]["test"] == [1, 1, 0, 2], f"a head that misses a held-back word is not installed: {s.get('error')}")
        check(not os.path.exists(os.path.join(state, "artifacts", "wake.orion-en-US")) and not os.path.exists(os.path.join(state, "wake_word")), "nothing staged, the wake word unchanged")
        b.call("POST", "/api/enroll/dismiss")

        # root refuses the install: not reported as done, the wake word it was to become is not the pick, nothing remembered
        proc.terminate(); proc.wait(5); env.pop("FAKE_TRAIN_FAIL", None)
        proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(PORT), "-n", "Echo Enroll", "-L", "-z", "0", "-o", "0", "-a", "0", "-W", str(WEB)], env=env, stderr=open(log, "a"))
        wait_port(PORT, WEB); b = Browser(WEB); b.login_with_button(proc)
        root.mode = "fail"; open(os.path.join(state, "wake_word"), "w").write("alexa\n")
        st, j = start(b, "Lyra", use=1)
        s = wait_state(b, ("done", "failed", "cancelled"), 180)
        check(s.get("state") == "failed" and "refused" in s.get("error", ""), f"an install root refuses is a failure: {s.get('state')} {s.get('error')}")
        check(open(os.path.join(state, "wake_word")).read() == "alexa\n", "the wake word is back to what it was")
        check(open(os.path.join(state, "enroll-last")).read() == "hey_nova-en-US\nHey Nova\n1\n", "failed install preserves the previous successful result")
        root.mode = "ok"
        # a cancel that comes after the install began is not acted on
        b.call("POST", "/api/enroll/dismiss")

        # Root can restart hassmic before the waiting enrollment thread reads its answer.
        proc.terminate(); proc.wait(5)
        digest = open(os.path.join(models, "hey_nova-en-US", "checksum.txt")).read()
        open(os.path.join(state, "enroll-pending"), "w").write(f"hey_nova-en-US\nHey Nova\n1\n{digest}\n")
        open(os.path.join(state, "artifacts", "result"), "w").write("OK wake:hey_nova-en-US\n")
        proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(PORT), "-n", "Echo Enroll", "-L", "-z", "0", "-o", "0", "-a", "0", "-W", str(WEB)], env=env, stderr=open(log, "a"))
        wait_port(PORT, WEB); b = Browser(WEB); b.login_with_button(proc)
        check(status(b)["session"]["state"] == "done" and open(os.path.join(state, "wake_word")).read() == "hey_nova-en-US\n", "restart reconciles confirmed install before loading the wake selection")
        check(not os.path.exists(os.path.join(state, "enroll-pending")), "confirmed intent consumed")
        b.call("POST", "/api/enroll/dismiss")

        # Terminal errors must be recoverable without deleting models or leaving training permanently blocked.
        for outcome in ("silent", "mismatch", "selection-failed"):
            proc.terminate(); proc.wait(5)
            expected = "f" * 32 if outcome == "mismatch" else digest
            open(os.path.join(state, "enroll-pending"), "w").write(f"hey_nova-en-US\nHey Nova\n1\n{expected}\n")
            open(os.path.join(state, "artifacts", "result"), "w").write("" if outcome == "silent" else "OK wake:hey_nova-en-US\n")
            if outcome == "selection-failed":
                os.unlink(os.path.join(state, "wake_word")); os.mkdir(os.path.join(state, "wake_word"))
            proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(PORT), "-n", "Echo Enroll", "-L", "-z", "0", "-o", "0", "-a", "0", "-W", str(WEB)], env=env, stderr=open(log, "a"))
            wait_port(PORT, WEB); b = Browser(WEB); b.login_with_button(proc)
            check(status(b)["session"]["state"] == "unresolved", f"{outcome}: explicit unresolved state")
            with open(lockpath) as fence:
                fcntl.flock(fence, fcntl.LOCK_EX)
                check(b.call("POST", "/api/enroll/dismiss")[0] == 400, f"{outcome}: clearing refused while root holds the lock")
            check(b.call("POST", "/api/enroll/dismiss")[0] == 200 and status(b)["session"]["state"] == "idle", f"{outcome}: clearing succeeds once root is idle")
            check(not os.path.exists(os.path.join(state, "enroll-pending")) and os.path.exists(os.path.join(models, "hey_nova-en-US", "pryon.manifest")), f"{outcome}: pending removed, installed model preserved")
            if outcome == "selection-failed":
                os.rmdir(os.path.join(state, "wake_word")); open(os.path.join(state, "wake_word"), "w").write("alexa\n")

        # a cancel during training ends the tool
        proc.terminate(); proc.wait(5)
        env.pop("FAKE_TRAIN_FAIL", None); env["FAKE_TRAIN_SLOW"] = "1"
        proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(PORT), "-n", "Echo Enroll", "-L", "-z", "0", "-o", "0", "-a", "0", "-W", str(WEB)], env=env, stderr=open(log, "a"))
        wait_port(PORT, WEB); b = Browser(WEB); b.login_with_button(proc)
        check(start(b, "Vega")[0] == 200, "new training accepted after unresolved-install recovery")
        s = wait_state(b, ("training", "failed", "done"), 120)
        check(s.get("state") == "training", f"training: {s.get('state')}")
        time.sleep(0.5)
        b.call("POST", "/api/enroll/cancel")
        s = wait_state(b, ("cancelled", "failed", "done"), 20)
        check(s.get("state") == "cancelled", f"cancelled: {s.get('state')}")
        time.sleep(0.5)
        alive = subprocess.run(["pgrep", "-f", os.path.join(state, "train")], capture_output=True).stdout.strip()
        check(not alive, "the trainer was ended with it")
        check(not os.path.exists(os.path.join(state, "enroll")) and not os.path.exists(os.path.join(state, "artifacts", "wake.vega-en-US.ready")), "nothing left, nothing installed")
    finally:
        root.stop = True
        proc.terminate()
        try: proc.wait(5)
        except Exception: proc.kill()
    print("FAILED" if check.failed else "all ok")
    sys.exit(1 if check.failed else 0)


main()
