#!/usr/bin/env python3
"""The owner's apps on an Echo Show from the settings page or the Echo Local integration, end to end: build/hassmic-host
(web.c, apps.c) with a browser written from the protocol (webclient.py), root's scripts/device/apps.sh with a stand-in
pm and packages.xml: the list, internet per app, an APK sent in pieces, checked, installed, removed; what is refused."""
import hashlib, json, os, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
from webclient import Browser, req, wait_port

WEB = 16991
fails = 0
def check(cond, what):
    global fails
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: fails += 1

PKG = '<package name="{n}" codePath="{c}" publicFlags="{f}" privateFlags="0" ft="1" it="1" ut="{ut}" version="{v}" userId="{u}">\n'
def xml(apps):
    return "<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n<packages>\n" + "".join(PKG.format(**a) for a in apps) + "</packages>\n"

# the stand-in pm: install reads the package name from the APK's first line ("pkg <name>"), a file starting "bad" is
# refused as Android refuses a 64-bit only one; it keeps packages.xml as PackageManager would
PM = r'''#!/bin/sh
X=$PACKAGES_XML
case "$1" in
install) f=$3; head -c 3 "$f" | grep -q bad && { echo "Failure [INSTALL_FAILED_NO_MATCHING_ABIS: no native code]"; exit 1; }
    n=$(head -1 "$f" | sed -n 's/^PK pkg //p')
    grep -v "name=\"$n\"" $X > $X.new
    sed -i '$d' $X.new
    echo "<package name=\"$n\" codePath=\"/data/app/$n-1\" publicFlags=\"0\" privateFlags=\"0\" ft=\"1\" it=\"1\" ut=\"$(date +%s%N)\" version=\"7\" userId=\"10050\">" >> $X.new
    echo "</packages>" >> $X.new; mv $X.new $X; echo Success;;
uninstall) grep -v "name=\"$2\"" $X > $X.new; mv $X.new $X; echo Success;;
esac
'''

def main():
    tmp = tempfile.mkdtemp(prefix="hm-apps-")
    state, adir, fake = (os.path.join(tmp, d) for d in ("state", "apps", "bin"))
    for d in (state, fake): os.makedirs(d)
    px = os.path.join(tmp, "packages.xml")
    with open(px, "w") as f:
        f.write(xml([
            dict(n="org.schabi.newpipe", c="/data/app/org.schabi.newpipe-1", f=671628868, ut="a1", v=1015, u=10005),
            dict(n="io.github.nt_f.echowall", c="/data/app/io.github.nt_f.echowall-1", f=0, ut="a2", v=1, u=10008),
            dict(n="com.amazon.webview.chromium", c="/system/app/AmazonWebView", f=-1207386555, ut="a3", v=60807432, u=10001)]))
    # the home app, as screen.c keeps it (state/screen_home): root's apps.sh never removes it
    with open(os.path.join(state, "screen_home"), "w") as f: f.write("io.github.nt_f.echowall\n")
    with open(os.path.join(fake, "pm"), "w") as f: f.write(PM)
    os.chmod(os.path.join(fake, "pm"), 0o755)
    renv = dict(os.environ, PATH=f"{fake}:{os.environ['PATH']}", APPS_STATE=state, APPS_DIR=adir, PACKAGES_XML=px, READ_AS="env")
    def root():
        o = subprocess.run(["sh", f"{ROOT}/scripts/device/apps.sh", "take"], env=renv, capture_output=True, text=True, timeout=60)
        return o.stdout + o.stderr

    log = os.path.join(tmp, "log")
    env = dict(os.environ, HASSMIC_STATE=state, HASSMIC_PACKAGES_XML=px, HASSMIC_APPS_RESULT=os.path.join(adir, "result"),
               HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw", HASSMIC_PLAY=os.path.join(tmp, "play.raw"),
               HASSMIC_MDNS_FILE=os.path.join(tmp, "none"), HASSMIC_ARB_ADDR="127.255.255.255",
               HASSMIC_MODELS=os.path.join(tmp, "models"), HASSMIC_ADB_OPEN=os.path.join(tmp, "adb-open"), HASSMIC_LOG=log)
    proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", "16990", "-n", "Echo Apps", "-L", "-z", "0", "-o", "0",
                             "-a", "16992", "-W", str(WEB)], env=env, stderr=open(log, "w"))
    try:
        wait_port(16990, WEB)
        b = Browser(WEB)
        check(b.login_with_button(proc), "logged in with the action button")
        check(req(WEB, "GET", "/api/apps")[0] == 401 and req(WEB, "POST", "/api/apps/apk/0", b"PK")[0] == 401, "unsigned: refused")
        st, _, body = b.call("GET", "/api/apps"); d = json.loads(body)
        check(st == 200 and [a["package"] for a in d["apps"]] == ["org.schabi.newpipe", "io.github.nt_f.echowall"]
              and d["apps"][0] == {"package": "org.schabi.newpipe", "version": 1015, "net": False, "lan": False}, "the owner's apps, Amazon's left out")
        check(d["install"]["state"] == "idle", "nothing under way")

        # ---- internet per app
        st, _, _ = b.call("POST", "/api/apps", b"net org.schabi.newpipe 1")
        check(st == 200 and open(os.path.join(state, "apps")).read() == "net org.schabi.newpipe\n", "internet on: state/apps for root")
        check(b.call("POST", "/api/apps", b"net com.amazon.webview.chromium 1")[0] == 400, "never for Amazon's")
        # reachable from the home network (issue #109): its own line for root's firewall (lockdown.sh hassmic_in)
        st, _, _ = b.call("POST", "/api/apps", b"lan org.schabi.newpipe 1")
        check(st == 200 and open(os.path.join(state, "apps")).read() == "net org.schabi.newpipe\nlan org.schabi.newpipe\n", "network on: a lan line for root")
        check(b.call("POST", "/api/apps", b"lan com.amazon.webview.chromium 1")[0] == 400 and b.call("POST", "/api/apps", b"bad org.schabi.newpipe 1")[0] == 400,
              "never for Amazon's; only net or lan")
        st, _, body = b.call("GET", "/api/apps")
        check(json.loads(body)["apps"][0].get("lan") is True, "the list says so")
        b.call("POST", "/api/apps", b"lan org.schabi.newpipe 0")

        # ---- an APK in pieces
        apk = b"PK pkg org.example.tube\n" + bytes((i * 13) & 255 for i in range(700000))
        digest = hashlib.blake2b(apk, digest_size=32).hexdigest()
        piece = 262144
        # refused before the signature is read (web.c read_request): an answer without one
        check(req(WEB, "POST", "/api/apps/apk/0", apk[:piece + 1], b.headers("POST", "/api/apps/apk/0", apk[:piece + 1]))[0] == 413, "a piece over 256 kB: refused")
        for off in range(0, len(apk), piece):
            st, _, _ = b.call("POST", f"/api/apps/apk/{off}", apk[off:off + piece])
            if st != 200: break
        check(st == 200, "sent in pieces")
        check(b.call("POST", "/api/apps/apk/5", b"xx")[0] == 409, "a piece not at the end: 409")
        check(b.call("POST", "/api/apps/apk/install", f"{len(apk)} {'0' * 64}".encode())[0] == 400, "another digest: refused")
        check(b.call("POST", "/api/apps/apk/install", f"{len(apk) - 1} {digest}".encode())[0] == 400, "another size: refused")
        st, _, _ = b.call("POST", "/api/apps/apk/install", f"{len(apk)} {digest}".encode())
        check(st == 200 and os.path.exists(os.path.join(state, "apps-request")), "install: a request for root")
        check(b.call("POST", "/api/apps/uninstall", b"org.schabi.newpipe")[0] == 409, "one at a time: 409")
        check(json.loads(b.call("GET", "/api/apps")[2])["install"]["state"] == "installing", "installing until root answers")
        out = root()
        d = json.loads(b.call("GET", "/api/apps")[2])
        check(d["install"]["state"] == "ok" and d["install"]["package"] == "org.example.tube", f"installed, its package named ({out.strip()})")
        check("org.example.tube" in [a["package"] for a in d["apps"]], "in the list")
        check(not os.path.exists(os.path.join(state, "apps-staged.apk")), "the staged APK gone")

        # ---- pm refuses one
        bad = b"bad" + bytes(1000)
        b.call("POST", "/api/apps/apk/0", bad)
        b.call("POST", "/api/apps/apk/install", f"{len(bad)} {hashlib.blake2b(bad, digest_size=32).hexdigest()}".encode())
        root()
        i = json.loads(b.call("GET", "/api/apps")[2])["install"]
        check(i["state"] == "failed" and i["error"] == "INSTALL_FAILED_NO_MATCHING_ABIS", f"pm's reason told ({i})")

        # ---- removing
        check(b.call("POST", "/api/apps/uninstall", b"com.amazon.webview.chromium")[0] == 400, "Amazon's: not removable")
        b.call("POST", "/api/apps/uninstall", b"io.github.nt_f.echowall"); root()
        i = json.loads(b.call("GET", "/api/apps")[2])["install"]
        check(i["state"] == "failed" and "screen app" in i["error"], "the screen app: refused by root")
        b.call("POST", "/api/apps/uninstall", b"org.example.tube"); root()
        d = json.loads(b.call("GET", "/api/apps")[2])
        check(d["install"]["state"] == "ok" and "org.example.tube" not in [a["package"] for a in d["apps"]], "removed")
    finally:
        proc.terminate(); proc.wait()
        if fails: print(open(log).read()[-3000:])
        shutil.rmtree(tmp, ignore_errors=True)
    print("apps: all ok" if not fails else f"apps: {fails} FAILED")
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())
