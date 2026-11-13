#!/usr/bin/env python3
"""The settings page's wake word list on build/hassmic-host (Amazon's engine): this Echo's sets with the one in use and
which were trained here (the mark file enrolled.txt, enroll.c), "Use", and removing a word trained here: refused for
Amazon's sets and for the one in use; the request root's artifact-install.sh delete acts on, which refuses the same
things again by itself (it is run here as root's watcher would run it)."""
import json, os, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webclient import wait_port, Browser

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT, WEB = 16991, 16992


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False


def words(b):
    st, _, body = b.call("GET", "/api/wakewords")
    return {w["id"]: w for w in json.loads(body)["words"]} if st == 200 else {}


def post(b, path):
    st, _, body = b.call("POST", path)
    return st, json.loads(body)


def make_set(models, name, ours):
    d = os.path.join(models, name); os.makedirs(d)
    open(os.path.join(d, "pryon.manifest"), "w").write("{}")
    if ours: open(os.path.join(d, "enrolled.txt"), "w").write("phrase=x\n")


def main():
    data = tempfile.mkdtemp(); state = os.path.join(data, "state"); models = os.path.join(data, "models"); os.makedirs(state); os.makedirs(models)
    log = os.path.join(data, "log")
    for name, ours in (("echo-en-US", False), ("nimbus-en-US", True), ("vega-en-US", True)): make_set(models, name, ours)
    env = dict(os.environ, HASSMIC_STATE=state, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw", HASSMIC_PLAY=os.path.join(state, "play.raw"),
               HASSMIC_MDNS_FILE=os.path.join(state, "none"), HASSMIC_ARB_ADDR="127.255.255.255", HASSMIC_MODELS=models, HASSMIC_FAKE_WHISPER="1",
               HASSMIC_LOG=log, HASSMIC_CLOCK_SYNCED="0")
    proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(PORT), "-n", "Echo Words", "-L", "-z", "0", "-o", "0", "-a", "0", "-W", str(WEB)],
                            env=env, stderr=open(log, "a"))
    try:
        wait_port(PORT, WEB)
        b = Browser(WEB); check(b.login_with_button(proc), "logged in with the button")
        w = words(b)
        check({"echo-en-US", "nimbus-en-US", "vega-en-US"} <= set(w) and len(w) == 4, f"the firmware's word and the three sets: {sorted(w)}")
        check(w["nimbus-en-US"]["ours"] and w["vega-en-US"]["ours"] and not w["echo-en-US"]["ours"] and not any(x["ours"] for k, x in w.items() if k not in ("nimbus-en-US", "vega-en-US")),
              "only the sets with the mark file are 'ours'")
        active = [k for k, x in w.items() if x["active"]]
        check(len(active) == 1, f"one in use: {active}")

        st, j = post(b, "/api/wakeword/use/nimbus-en-US")
        check(st == 200 and open(os.path.join(state, "wake_word")).read().split()[0] == "nimbus-en-US" and words(b)["nimbus-en-US"]["active"], f"Use: nimbus is the pick and saved: {j}")
        check(post(b, "/api/wakeword/use/nothing-en-US")[0] == 400, "Use of a word that is not there is refused")

        st, j = post(b, "/api/wakeword/delete/nimbus-en-US")
        check(st == 400 and "in use" in j.get("error", ""), f"the word in use is not removed: {j.get('error')}")
        st, j = post(b, "/api/wakeword/delete/echo-en-US")
        check(st == 400 and "not a wake word trained" in j.get("error", ""), f"Amazon's set is not removed: {j.get('error')}")
        check(post(b, "/api/wakeword/delete/..%2Fx")[0] == 400, "a bad name is refused")
        check(not os.path.exists(os.path.join(state, "artifacts", "delete")), "nothing asked of root so far")

        st, j = post(b, "/api/wakeword/delete/vega-en-US")
        check(st == 200, f"a word trained here: asked for: {j}")
        req = os.path.join(state, "artifacts", "delete")
        check(open(req).read() == "wake:vega-en-US\n", "the request root reads")
        run = lambda: subprocess.run(["sh", f"{ROOT}/scripts/device/artifact-install.sh", "delete", state, data], capture_output=True, text=True, env=dict(os.environ, READ_AS=""))
        r = run()
        check(r.returncode == 0 and "OK deleted wake:vega-en-US" in r.stdout and not os.path.exists(os.path.join(models, "vega-en-US")) and not os.path.exists(req), f"root removed it: {r.stdout.strip()}")
        check(os.path.isdir(os.path.join(models, "echo-en-US")) and os.path.isdir(os.path.join(models, "nimbus-en-US")), "the others are untouched")

        # root does not trust hassmic's request either
        for who, expect in (("wake:echo-en-US", "not a wake word trained on this Echo"), ("wake:nimbus-en-US", "in use"), ("wake:../x", "bad name"), ("sound", "not a wake word set")):
            open(req, "w").write(who + "\n"); r = run()
            check(r.returncode != 0 and expect in r.stdout, f"root refuses {who!r}: {r.stdout.strip()}")
        check(os.path.isdir(os.path.join(models, "echo-en-US")) and os.path.isdir(os.path.join(models, "nimbus-en-US")), "still there")
        open(req, "w").write("wake:nimbus-en-US\n"); os.symlink("/", os.path.join(models, "link-en-US"))
        open(req, "w").write("wake:link-en-US\n"); r = run()
        check(r.returncode != 0 and os.path.exists(os.path.join(models, "link-en-US")), "a link is not followed or removed")
    finally:
        proc.terminate()
        try: proc.wait(5)
        except Exception: proc.kill()
    print("FAILED" if check.failed else "all ok")
    sys.exit(1 if check.failed else 0)


main()
