#!/usr/bin/env python3
"""The phone (sip.c) against a real Asterisk: build/hassmic-host and a throwaway Asterisk container (andrius/asterisk,
Asterisk 22/23 with PJSIP and codec_opus) on Docker's bridge network, with a config of its own written here.  The Echo
registers (Asterisk's digest), Asterisk's qualify finds it, Asterisk calls it with Opus and with A-law (Milliwatt's
1004 Hz, answered with the action button), the Echo calls an extension that records it (its speech in the recording,
through Asterisk's 401 on the INVITE) and one that is busy.  Skips without docker or the image (docker pull
andrius/asterisk), so it is not in tests/run.py: run it by hand.  Nothing of it touches another Asterisk: its own
container, its own IP, no host ports."""
import base64, os, shutil, signal, subprocess, sys, tempfile, time, wave
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
from webclient import wait_port, Browser
IMAGE, NAME = os.environ.get("ASTERISK_IMAGE", "andrius/asterisk:latest"), f"hassmic-asterisk-test-{os.getpid()}"
API, WEB, SIP = 26070, 26071, 26072
PASS = "t3st-pass"

PJSIP = f"""[transport-udp]
type = transport
protocol = udp
bind = 0.0.0.0:5060

[echo1]
type = aor
max_contacts = 1
remove_existing = yes
qualify_frequency = 5

[echo1]
type = auth
auth_type = userpass
username = echo1
password = {PASS}

[echo1]
type = endpoint
context = test
disallow = all
allow = opus,alaw,ulaw
direct_media = no
rtp_symmetric = yes
rewrite_contact = yes
auth = echo1
aors = echo1

; the same Echo (its AOR) dialled with G.711 only
[echo1alaw]
type = endpoint
context = test
disallow = all
allow = alaw
direct_media = no
rtp_symmetric = yes
aors = echo1
"""
DIALPLAN = """[general]
[test]
exten => 600,1,Answer()
 same => n,Wait(0.3)
 same => n,Record(/rec/out.wav,0,4,qk)
 same => n,Hangup()
exten => 602,1,Busy(5)
"""


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False


def tone_db(x, f, rate=16000):
    x = np.asarray(x, float); t = np.arange(len(x)) / rate
    return 20 * np.log10(abs((x * np.exp(-2j * np.pi * f * t)).sum()) * 2 / max(len(x), 1) / np.sqrt(2) / 32768 + 1e-12)
def db(x): x = np.asarray(x, float); return 10 * np.log10((x ** 2).mean() / 32768 ** 2 + 1e-20) if len(x) else -200
def until(f, secs):
    end = time.monotonic() + secs
    while time.monotonic() < end:
        if f(): return True
        time.sleep(0.1)
    return False


def main():
    if not shutil.which("docker") or subprocess.run(["docker", "image", "inspect", IMAGE], capture_output=True).returncode:
        print(f"skipped: needs docker and the image {IMAGE}"); return 0
    tmp = tempfile.mkdtemp(); rec = os.path.join(tmp, "rec"); os.mkdir(rec); os.chmod(rec, 0o777)
    for n, t in (("pjsip.conf", PJSIP), ("extensions.conf", DIALPLAN)): open(os.path.join(tmp, n), "w").write(t)
    subprocess.run(["docker", "run", "-d", "--rm", "--name", NAME, "-v", f"{tmp}/pjsip.conf:/etc/asterisk/pjsip.conf:ro",
                    "-v", f"{tmp}/extensions.conf:/etc/asterisk/extensions.conf:ro", "-v", f"{rec}:/rec", IMAGE], check=True, capture_output=True)
    ast = lambda cmd: subprocess.run(["docker", "exec", NAME, "asterisk", "-rx", cmd], capture_output=True, text=True).stdout
    p = None
    try:
        check(until(lambda: "echo1" in ast("pjsip show endpoints"), 30), "Asterisk is up with the test endpoint")
        ip = subprocess.run(["docker", "inspect", "-f", "{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}", NAME], capture_output=True, text=True).stdout.strip()
        st = tempfile.mkdtemp()
        open(st + "/api_key", "w").write(base64.b64encode(os.urandom(32)).decode() + "\n")
        open(st + "/config", "w").write("even_volume=off\nphone=on\n")
        open(st + "/phone", "w").write(f"server={ip}\nuser=echo1\npassword={PASS.encode().hex()}\n")
        keys = st + "/keypad"; os.mkfifo(keys); kfd = os.open(keys, os.O_RDWR)
        env = dict(os.environ, HASSMIC_STATE=st, HASSMIC_PLAY=st + "/play.raw", HASSMIC_VOIP=st + "/voip.raw", HASSMIC_MDNS_FILE=st + "/none",
                   HASSMIC_MODELS=st + "/models", HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw")
        log = st + "/log"
        p = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(API), "-n", "Asterisk Echo", "-L", "-z", "0", "-o", "0",
                              "-a", "0", "-c", str(SIP), "-W", str(WEB), "-b", keys], env=env, stderr=open(log, "w"))
        wait_port(API, WEB)
        page = Browser(WEB); check(page.login_with_button(p), "settings page logged in")
        phone = lambda: (page.state() or {}).get("phone") or {}
        voip = lambda s: np.fromfile(st + "/voip.raw", np.int16)[-16000 * s:] if os.path.exists(st + "/voip.raw") else np.zeros(0)
        check(until(lambda: phone().get("line") == "registered", 10), f"the Echo registers through Asterisk's digest ({phone().get('text')})")
        check(until(lambda: "Avail" in ast("pjsip show contacts"), 15), "Asterisk's qualify (OPTIONS) finds it")
        print("    " + ast("core show version").strip() + f", at {ip}")
        for ep, codec in (("echo1", "opus"), ("echo1alaw", "alaw")):
            ast(f"channel originate PJSIP/{ep} application Milliwatt")
            check(until(lambda: phone().get("state") == "ringing", 5), f"[{codec}] Asterisk calls: the Echo rings")
            time.sleep(1); p.send_signal(signal.SIGUSR2)          # the action button
            check(until(lambda: phone().get("state") == "connected", 5), f"[{codec}] the action button answers")
            time.sleep(4)
            x = voip(2); stats = ast("pjsip show channelstats")
            check(tone_db(x, 1004) > -12, f"[{codec}] the Echo plays Milliwatt's 1004 Hz ({tone_db(x, 1004):.1f} dB of {db(x):.1f} dBFS)")
            check(codec in stats.lower(), f"[{codec}] Asterisk runs the call as {codec}")
            ast("channel request hangup all")
            check(until(lambda: phone().get("state") == "idle", 5), f"[{codec}] Asterisk hangs up: idle")
            time.sleep(1)
        page.call("POST", "/api/phone", f"call {b'600'.hex()}".encode())
        check(until(lambda: phone().get("state") == "connected", 8), "the Echo calls 600 (through Asterisk's 401 on the INVITE)")
        check(until(lambda: phone().get("state") == "idle", 10), "Asterisk hangs up after its recording")
        out = os.path.join(rec, "out.wav")
        if os.path.exists(out):
            with wave.open(out) as w: r = np.frombuffer(w.readframes(w.getnframes()), np.int16)
            check(db(r) > -40, f"Asterisk's recording has the Echo's speech ({db(r):.1f} dBFS, {len(r) / 8000:.1f} s)")
        else: check(False, "Asterisk recorded the call")
        page.call("POST", "/api/phone", f"call {b'602'.hex()}".encode())
        check(until(lambda: phone().get("result") == "busy" and phone().get("state") == "idle", 8), f"602 Busy(): result {phone().get('result')}")
    finally:
        if p:
            p.terminate()
            try: p.wait(5)
            except subprocess.TimeoutExpired: p.kill()
            if check.failed: print("\n".join(l for l in open(log).read().splitlines() if "phone" in l or "call:" in l)[-4000:])
        subprocess.run(["docker", "stop", NAME], capture_output=True)
    print("FAILED" if check.failed else "all passed")
    return 1 if check.failed else 0


if __name__ == "__main__":
    sys.exit(main())
