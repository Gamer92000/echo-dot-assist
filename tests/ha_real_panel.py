#!/usr/bin/env python3
"""Echo Local's sidebar panel (custom_components/echo_local panel.py, apps.py, frontend/panel.js) in a real Home
Assistant against build/hassmic-host: the panel registered for administrators; the list of Echos; the screen's options
set from the panel's command, checked, and sent on to the Echo (its kiosk config then has them); the kiosk user kept out
of the list and the options; the Echo Show's apps (apps.py; the Echo's apps.c with root's scripts/device/apps.sh and
fake_web_apps.py's stand-in pm): the list, internet per app, an APK uploaded by an administrator and installed, one
refused by Android, one from a path, a path outside the allowed folders, removal; a replaced entry adopting the screen
the Echo shows (kiosk.adopt), the last push and "Send to the Echo again"; the home screens of before gone (Echo-Wall
has an integration of its own); then the panel in headless Chrome: the Echo, the Screen tab, the Apps tab.
build/test-logs/panel*.png show it for the eye.

Same isolation and Home Assistant as tests/ha_real_kiosk.py.  Chrome: google-chrome or chromium on PATH (else that
part is skipped)."""
import asyncio, base64, hashlib, json, os, shutil, signal, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ha_real_dropin as hr
import ha_real_alarms as ra
import ha_real_kiosk as rk
from ha_real_dropin import check

NODE = "echo-show-5-a1b2c3"
EW = "io.github.nt_f.echowall"


# Echo-Wall's integration (Nt-f/Echo-Wall, a repo of its own), for the Home tab: where it is checked out beside this one
EW_INT = "" if os.environ.get("HASSMIC_ECHO_WALL") == "none" else os.environ.get("HASSMIC_ECHO_WALL") or next((p for p in (os.path.join(hr.ROOT, "..", d, "custom_components", "echo_wall")
                                                              for d in ("Echo-Wall", "hassmic-homeview")) if os.path.isdir(p)), "")


# the Echo Show's apps (apps.c, root's apps.sh, Echo Local's Apps tab) where this tree has them
HAVE_APPS = os.path.exists(os.path.join(hr.ROOT, "scripts", "device", "apps.sh"))


def write_config(conf):
    rk.write_config(conf)
    if EW_INT:
        shutil.copytree(EW_INT, os.path.join(conf, "custom_components", "echo_wall"), ignore=shutil.ignore_patterns("__pycache__"))


async def as_user(session, token):
    c = hr.Client(session, token)
    c.sock = await session.ws_connect(f"{hr.HA}/api/websocket")
    await c.sock.receive_json(); await c.sock.send_json({"type": "auth", "access_token": token})
    check((await c.sock.receive_json()).get("type") == "auth_ok", "the kiosk's token on the websocket")
    return c


async def fails(coro):
    try:
        await coro
        return ""
    except RuntimeError as e:
        return str(e)


async def browser(session, token, png):
    chrome = shutil.which("google-chrome") or shutil.which("chromium") or shutil.which("chromium-browser")
    if not chrome:
        print("note: no Chrome here, the panel is not drawn"); return None
    prof = tempfile.mkdtemp(prefix="panel-chrome-")
    proc = subprocess.Popen([chrome, "--headless=new", "--no-sandbox", "--disable-gpu", "--remote-debugging-port=9334",
                             f"--user-data-dir={prof}", "--window-size=1500,1000", "--force-device-scale-factor=1", "about:blank"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            try:
                async with session.get("http://127.0.0.1:9334/json") as r:
                    tabs = await r.json(); break
            except Exception: await asyncio.sleep(0.2)
        page = next(t for t in tabs if t.get("type") == "page")
        ws = await session.ws_connect(page["webSocketDebuggerUrl"], max_msg_size=0)
        n = [0]
        console = []
        async def cdp(method, **params):
            n[0] += 1; i = n[0]
            await ws.send_json({"id": i, "method": method, "params": params})
            while True:
                m = await ws.receive_json()
                if m.get("id") == i: return m.get("result", m.get("error"))
                if m.get("method") == "Runtime.consoleAPICalled":
                    console.append(" ".join(str(a.get("value", a.get("description", ""))) for a in m["params"].get("args", []))[:300])
                elif m.get("method") == "Runtime.exceptionThrown":
                    console.append("exception: " + json.dumps(m["params"]["exceptionDetails"])[:400])
        async def js(expr):
            r = await cdp("Runtime.evaluate", expression=expr, returnByValue=True, awaitPromise=True)
            if r and r.get("exceptionDetails"): return {"error": json.dumps(r["exceptionDetails"])[:400]}
            return (r.get("result") or {}).get("value")
        await cdp("Page.enable"); await cdp("Runtime.enable")
        await cdp("Emulation.setDeviceMetricsOverride", width=1500, height=1000, deviceScaleFactor=1, mobile=False)
        await cdp("Page.navigate", url=f"{hr.HA}/echo_local/panel-test-404")
        await asyncio.sleep(1)
        tokens = {"access_token": token, "token_type": "Bearer", "expires_in": 1800, "hassUrl": hr.HA,
                  "clientId": hr.HA + "/", "expires": 4102444800000, "refresh_token": ""}
        await js(f"localStorage.setItem('hassTokens', {json.dumps(json.dumps(tokens))})")
        await cdp("Page.navigate", url=f"{hr.HA}/echo-local")
        # Home Assistant's frontend, down to our element in its shadow roots
        find = """(() => { const walk = (r) => { if (!r) return null; const p = r.querySelector('echo-local-panel'); if (p) return p;
            for (const e of r.querySelectorAll('*')) { if (e.shadowRoot) { const f = walk(e.shadowRoot); if (f) return f; } } return null; };
            return walk(document); })()"""
        out = {}
        for _ in range(60):
            await asyncio.sleep(0.5)
            out = await js(f"""(() => {{ const p = {find}; if (!p) return {{}}; const r = p.shadowRoot;
                return {{ echos: [...r.querySelectorAll('.echo .t')].map(e => e.textContent), sub: [...r.querySelectorAll('.echo .s')].map(e => e.textContent),
                         tabs: [...r.querySelectorAll('.tab')].map(e => e.textContent) }}; }})()""") or {}
            if out.get("echos") and len(out.get("tabs") or []) >= 1: break
        async def shot(name, clip=None):
            r = await cdp("Page.captureScreenshot", format="png", **({"clip": clip} if clip else {}))
            if r and r.get("data"):
                os.makedirs(os.path.dirname(png), exist_ok=True); open(png.replace(".png", name + ".png"), "wb").write(base64.b64decode(r["data"]))
        await shot("")
        out["screen"] = await js(f"""(async () => {{ const p = {find}; p.tab = 'screen'; await p.loadKiosk();
            if (p.loadApps) await p.loadApps(); await new Promise(r => setTimeout(r, 500)); return {{ rows: [...p.shadowRoot.querySelectorAll('.card .row .name')].map(e => e.textContent),
              apps: [...p.shadowRoot.querySelectorAll('.card select option')].map(e => e.textContent),
              push: (p.shadowRoot.querySelector('.pushst') || {{}}).textContent || '' }}; }})()""")
        await asyncio.sleep(0.5); await shot("-screen")
        if HAVE_APPS: out["appstab"] = await js(f"""(async () => {{ const p = {find}; p.tab = 'apps'; p.render(); await p.loadApps();
            await new Promise(r => setTimeout(r, 500)); const r = p.shadowRoot;
            return {{ tabs: [...r.querySelectorAll('.tab')].map(e => e.textContent), apps: [...r.querySelectorAll('.item.app .nm div:first-child')].map(e => e.textContent),
                     screenApp: [...r.querySelectorAll('.item.app')].filter(e => e.textContent.includes('the home app')).map(e => e.querySelector('.danger').disabled),
                     status: (r.querySelector('.inst') || {{}}).textContent || '' }}; }})()""")
        if HAVE_APPS: await asyncio.sleep(0.3); await shot("-apps")
        out["home"] = await js(f"""(async () => {{ const p = {find}; p.tab = 'home'; p.render();
            for (let i = 0; i < 40; i++) {{ const w = p.shadowRoot.querySelector('echo-wall-panel'); if (w && w.dev && w.shadowRoot.querySelector('main *:not(.empty)')) break;
              await new Promise(r => setTimeout(r, 250)); }}
            const w = p.shadowRoot.querySelector('echo-wall-panel'); if (!w) return {{ text: (p.shadowRoot.querySelector('main') || {{}}).textContent || '' }};
            const before = w.dev; p.tab = 'screen'; p.render(); p.tab = 'home'; p.render();
            return {{ dev: w.dev, kept: p.shadowRoot.querySelector('echo-wall-panel') === w && w.dev === before, embedded: w.hasAttribute('embedded'),
                     header: getComputedStyle(w.shadowRoot.querySelector('header')).display }}; }})()""")
        await asyncio.sleep(1.5); await shot("-home")
        # every tab at a laptop's width and a phone's: nothing wider than the window, the Echos' list kept its width
        out["fit"] = {}
        for wname, w, hgt, mobile in (("laptop", 1100, 800, False), ("phone", 390, 844, True)):
            await cdp("Emulation.setDeviceMetricsOverride", width=w, height=hgt, deviceScaleFactor=1, mobile=mobile)
            await asyncio.sleep(1.0)
            for tab in ("screen", "home", "apps"):
                r = await js(f"""(async () => {{ const p = {find}; p.tab = '{tab}'; p.render();
                    if ('{tab}' === 'screen' && !p.kiosk) await p.loadKiosk();
                    await new Promise(r => setTimeout(r, 900)); const R = p.shadowRoot;
                    const nav = R.querySelector('nav'), main = R.querySelector('main');
                    const wide = [...R.querySelectorAll('main *')].concat([...((R.querySelector('echo-wall-panel') || {{}}).shadowRoot || {{ querySelectorAll: () => [] }}).querySelectorAll('*')])
                      .filter(e => e.getBoundingClientRect().right > innerWidth + 1).slice(0, 4).map(e => e.tagName + '.' + e.className);
                    return {{ tab: p.tab, nav: nav ? Math.round(nav.getBoundingClientRect().width) : 0, main: main ? Math.round(main.getBoundingClientRect().width) : 0,
                             scroll: Math.max(document.documentElement.scrollWidth, main ? main.scrollWidth + main.getBoundingClientRect().left : 0) - innerWidth, wide }}; }})()""")
                out["fit"][f"{wname}-{tab}"] = r
                await shot(f"-{wname}-{tab}")
        await cdp("Emulation.setDeviceMetricsOverride", width=1500, height=1000, deviceScaleFactor=1, mobile=False)
        out["console"] = console
        open(png.replace(".png", "-console.txt"), "w").write("\n".join(console))
        return out
    finally:
        proc.terminate()
        try: proc.wait(10)
        except subprocess.TimeoutExpired: proc.kill()
        shutil.rmtree(prof, ignore_errors=True)


# the Echo Show's apps: a packages.xml as PackageManager keeps it, and a stand-in pm that installs an "APK" whose first
# line names its package and refuses one starting "bad" as Android refuses a 64-bit only one (fake_web_apps.py's)
PKG = '<package name="{n}" codePath="{c}" publicFlags="{f}" privateFlags="0" ft="1" it="1" ut="{ut}" version="{v}" userId="{u}">\n'
def apps_xml(apps):
    return "<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n<packages>\n" + "".join(PKG.format(**a) for a in apps) + "</packages>\n"
APPS_PM = r'''#!/bin/sh
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


async def apps_part(c, session, cfg, conf, root):
    rooter = asyncio.ensure_future(root())
    r = await c.ws(type="echo_local/apps/list", echo=NODE)
    pk = {a["package"]: a for a in r.get("apps", [])}
    check("org.schabi.newpipe" in pk and EW in pk and "com.amazon.webview.chromium" not in pk and not r.get("none"),
          f"the owner's apps, not the system's ({sorted(pk)})")
    await c.ws(type="echo_local/apps/net", echo=NODE, package="org.schabi.newpipe", on=True)
    r = await c.ws(type="echo_local/apps/list", echo=NODE)
    check(any(a["package"] == "org.schabi.newpipe" and a["net"] for a in r["apps"]), "internet for NewPipe switched on")
    e = await fails(k2.ws(type="echo_local/apps/list", echo=NODE)) if (k2 := await as_user(session, cfg.get("token", ""))) else "x"
    check("unauthorized" in e.lower(), "the apps: administrators only")
    async def until_done(want):
        for _ in range(120):
            r = await c.ws(type="echo_local/apps/list", echo=NODE)
            st = (r.get("install") or {}).get("state")
            if (r.get("send") or {}).get("state") != "sending" and st in ("ok", "failed") and want(r): return r
            await asyncio.sleep(0.5)
        return r
    apk = b"PK pkg org.example.app\n" + os.urandom(17 << 20)         # past aiohttp's 16 MB body limit; 69 pieces
    async with session.post(f"{hr.HA}/api/echo_local/apk/{NODE}", data=apk, headers=c.h) as r:
        up = (r.status, await r.json())
    r = await until_done(lambda r: any(a["package"] == "org.example.app" for a in r["apps"]))
    check(up[0] == 200 and up[1].get("size") == len(apk) and r["install"]["state"] == "ok" and r["install"]["package"] == "org.example.app"
          and any(a["package"] == "org.example.app" and not a["net"] for a in r["apps"]),
          f"an APK uploaded, sent in pieces, installed, off the internet ({up}, {r.get('install')}, {r.get('send')})")
    async with session.post(f"{hr.HA}/api/echo_local/apk/{NODE}", data=apk, headers={"Authorization": "Bearer " + cfg.get("token", "")}) as r:
        check(r.status in (401, 403), f"an upload by the kiosk's user: refused ({r.status})")
    bad = os.path.join(conf, "www"); os.makedirs(bad, exist_ok=True); bad = os.path.join(bad, "bad.apk")
    open(bad, "wb").write(b"bad" + os.urandom(1000))
    await c.ws(type="echo_local/apps/install", echo=NODE, path=bad)
    r = await until_done(lambda r: True)
    check(r["install"]["state"] == "failed" and "NO_MATCHING_ABIS" in r["install"].get("error", ""),
          f"one from a path under /config, refused by Android: its reason ({r.get('install')})")
    e = await fails(c.ws(type="echo_local/apps/install", echo=NODE, path="/etc/passwd"))
    check("may read" in e, "a path outside the folders Home Assistant may read: refused")
    await c.ws(type="echo_local/apps/uninstall", echo=NODE, package="org.example.app")
    r = await until_done(lambda r: not any(a["package"] == "org.example.app" for a in r["apps"]))
    check(not any(a["package"] == "org.example.app" for a in r["apps"]), f"removed ({r.get('install')})")
    rooter.cancel()


async def run(tmp):
    import aiohttp
    conf = os.path.join(tmp, "ha")
    write_config(conf)
    net = f"{int.from_bytes(os.urandom(8), 'little') | 1:016x} " + base64.b64encode(os.urandom(32)).decode()
    echo = hr.Echo(tmp, "Echo Show 5 a1b2c3", "02:00:00:a1:b2:c3", 26190, net, os.path.join(hr.ROOT, "build", "hassmic-host"))
    halog = open(os.path.join(tmp, "ha.log"), "w")
    hass = subprocess.Popen([hr.HA_PY, "-m", "homeassistant", "-c", conf, "--skip-pip"], stdout=halog, stderr=subprocess.STDOUT, cwd=conf)
    apps = os.path.join(tmp, "apps"); os.makedirs(os.path.join(apps, "io.github.nt_f.echowall"))
    os.environ["HASSMIC_APP_DATA"] = apps
    # the Echo Show's apps: packages.xml, root's apps.sh with the stand-in pm (as fake_web_apps.py has them)
    fwa = type("fwa", (), {"xml": staticmethod(apps_xml), "PM": APPS_PM})
    px, adir, fake = os.path.join(tmp, "packages.xml"), os.path.join(tmp, "appsroot"), os.path.join(tmp, "bin")
    os.makedirs(fake)
    open(px, "w").write(fwa.xml([
        dict(n="org.schabi.newpipe", c="/data/app/org.schabi.newpipe-1", f=671628868, ut="a1", v=1015, u=10005),
        dict(n="io.github.nt_f.echowall", c="/data/app/io.github.nt_f.echowall-1", f=0, ut="a2", v=1, u=10008),
        dict(n="com.amazon.webview.chromium", c="/system/app/AmazonWebView", f=-1207386555, ut="a3", v=60807432, u=10001)]))
    open(os.path.join(fake, "pm"), "w").write(fwa.PM); os.chmod(os.path.join(fake, "pm"), 0o755)
    os.environ.update(HASSMIC_PACKAGES_XML=px, HASSMIC_APPS_RESULT=os.path.join(adir, "result"))
    renv = dict(os.environ, PATH=f"{fake}:{os.environ['PATH']}", APPS_STATE=echo.state, APPS_DIR=adir, PACKAGES_XML=px, READ_AS="env")
    async def root():
        while True:                     # root's watcher (main.sh ota_watch runs apps.sh take on a request)
            await asyncio.get_running_loop().run_in_executor(None, lambda: subprocess.run(
                ["sh", f"{hr.ROOT}/scripts/device/apps.sh", "take"], env=renv, capture_output=True, timeout=60))
            await asyncio.sleep(0.3)
    echo.start()
    web = f"http://127.0.0.1:{echo.port + 10}"
    try:
        async with aiohttp.ClientSession() as session:
            for _ in range(240):
                try:
                    async with session.get(f"{hr.HA}/api/onboarding") as r:
                        if r.status == 200: break
                except aiohttp.ClientError: pass
                if hass.poll() is not None: raise RuntimeError("Home Assistant ended")
                await asyncio.sleep(0.5)
            admin_tok = await hr.onboard(session)
            c = hr.Client(session, admin_tok); await c.ws_open()
            f = await ra.add_echo_local(c, echo)
            check(f.get("type") == "create_entry", f"Echo Local added ({f.get('type')})")
            await asyncio.sleep(2)
            # the home screens of before are Echo-Wall's own integration's now: nothing of them left here
            e = await fails(c.ws(type="echo_local/home/list"))
            check("unknown" in e.lower(), "the home screens' commands are gone (Echo-Wall's integration has them)")
            async with session.get(f"{hr.HA}/echo_local/echowall/index.html") as r:
                check(r.status == 404, f"Echo-Wall's preview is not served here any more ({r.status})")
            panels = await c.ws(type="get_panels")
            p = panels.get("echo-local") or {}
            check(p.get("component_name") == "custom" and p.get("require_admin") and p.get("title") == "Echo Local"
                  and (p.get("config") or {}).get("_panel_custom", {}).get("module_url") == "/echo_local/panel.js",
                  f"the sidebar panel, administrators only ({ {k: p.get(k) for k in ('component_name', 'require_admin', 'title')} })")
            async with session.get(f"{hr.HA}/echo_local/panel.js") as r:
                check(r.status == 200 and "echo-local-panel" in await r.text(), f"panel.js served ({r.status})")
            lst = await c.ws(type="echo_local/list")
            check([e["echo"] for e in lst["echos"]] == [NODE] and lst["echos"][0]["title"] == "Echo Show 5 a1b2c3",
                  f"the list: the Echo ({lst['echos']})")

            # the kiosk on: its user and token, as the Echo has them
            r = await c.ws(type="echo_local/kiosk/set", echo=NODE, options={"kiosk": True, "kiosk_screens": ["wall", "dashboard"], "kiosk_default": "wall",
                                                                            "kiosk_return": 60, "kiosk_app": EW})
            check(r["options"].get("kiosk") and r["options"].get("kiosk_return") == 60, f"the screen's options from the panel's command ({r['options']})")
            e = await fails(c.ws(type="echo_local/kiosk/set", echo=NODE, options={"kiosk_screens": ["nonsense"]}))
            check("nonsense" in e or "value" in e, "a screen kind there is none of: refused")
            cfg = {}
            for _ in range(60):
                try:
                    async with session.get(f"{web}/kiosk/config/front") as r:
                        cfg = await r.json()
                    if cfg.get("token") and cfg.get("return") == 60: break
                except aiohttp.ClientError: pass
                await asyncio.sleep(0.5)
            check(cfg.get("return") == 60 and [(s["name"], s["url"][:13]) for s in cfg.get("screens", [])] == [("Home", "hassmic:home"), ("Home Assistant", hr.HA[:13])],
                  f"... and the entry reloaded: the Echo has them ({cfg.get('return')}, {[s['name'] for s in cfg.get('screens', [])]})")
            k = await as_user(session, cfg.get("token", ""))
            e = await fails(k.ws(type="echo_local/list"))
            check("unauthorized" in e.lower(), "the list: administrators only")
            e = await fails(k.ws(type="echo_local/kiosk/set", echo=NODE, options={"kiosk_admin": True}))
            check("unauthorized" in e.lower(), "the screen's options: administrators only (the kiosk does not make itself one)")
            # ---- Echo-Wall beside it: its app logged in as this Echo's user, its panel in Echo Local's Home tab
            if not EW_INT:
                print("note: no Echo-Wall integration checked out beside this repo (HASSMIC_ECHO_WALL): the Home tab is not tried")
            else:
                f = await c.post("/api/config/config_entries/flow", {"handler": "echo_wall"})
                f = await c.post(f"/api/config/config_entries/flow/{f['flow_id']}", {})
                check(f.get("type") == "create_entry", f"Echo-Wall's integration added ({f.get('type')})")
                r = await k.ws(type="echo_wall/register", device="dev_echo_show", name="Echo-Wall on the Show", model="Echo Show 5", screen=[960, 470])
                check(r.get("device") == "dev_echo_show", "Echo-Wall's app registered with the Echo's login")
                p = (await c.ws(type="get_panels")).get("echo-wall") or {}
                check(p and not p.get("title"), f"beside Echo Local, Echo-Wall's panel has no sidebar entry of its own ({p.get('title')!r})")
                el = await c.ws(type="echo_local/list")
                check(bool(el["echos"][0].get("user")), "the list: the Echo's user, for the Home tab")
            await k.sock.close()

            # ---- the Echo Show's apps (where the Echo side, apps.c and root's apps.sh, is in this tree)
            if not os.path.exists(f"{hr.ROOT}/scripts/device/apps.sh"):
                print("note: no scripts/device/apps.sh here: the apps part is skipped")
            else:
                await apps_part(c, session, cfg, conf, root)

            # a replaced entry: the screen the Echo shows taken over (kiosk.adopt, Home Assistant's own Python)
            code = ("import json, sys; sys.path.insert(0, sys.argv[1]); from echo_local import kiosk; print(json.dumps(kiosk.adopt(None, json.loads(sys.argv[2]))))")
            old = {"enabled": True, "ha": "http://192.168.1.211:8123", "app": EW, "default": 0, "return": 90,
                   "screens": [{"name": "Home", "url": "http://192.168.1.211:8123/hassmic/wall.html?echo=x"},
                               {"name": "Music", "url": "http://192.168.1.211:8123/d5369777_music_assistant"},
                               {"name": "Home Assistant", "url": "http://192.168.1.211:8123/dashboard-wall"}]}
            o = subprocess.run([hr.HA_PY, "-c", code, os.path.join(conf, "custom_components"), json.dumps(old)], capture_output=True, text=True)
            ad = json.loads(o.stdout or "null") if o.returncode == 0 else o.stderr[-300:]
            check(isinstance(ad, dict) and ad.get("kiosk") and ad.get("kiosk_screens") == ["wall", "dashboard"] and ad.get("kiosk_app") == EW
                  and "kiosk_music" not in ad and ad.get("kiosk_dashboard") == "dashboard-wall" and ad.get("kiosk_return") == 90,
                  f"the Echo's screen adopted by a new entry (Music Assistant's page left out: the drawer's own) ({ad})")
            o = subprocess.run([hr.HA_PY, "-c", code, os.path.join(conf, "custom_components"), json.dumps({**old, "enabled": False})], capture_output=True, text=True)
            check(o.stdout.strip() == "null", "... none while the Echo's screen is off")
            kg = await c.ws(type="echo_local/kiosk/get", echo=NODE)
            check((kg.get("push") or {}).get("ok") and kg["push"].get("kiosk"), f"the last push's outcome for the panel ({kg.get('push')})")
            r = await c.ws(type="echo_local/kiosk/push", echo=NODE)
            check((r.get("push") or {}).get("ok"), f"Send to the Echo again ({r})")

            # the frontend's onboarding's last step (hr.onboard leaves it: the other tests use no browser as an administrator)
            async with session.post(f"{hr.HA}/api/onboarding/integration", headers=c.h,
                                    json={"client_id": f"{hr.HA}/", "redirect_uri": f"{hr.HA}/?auth_callback=1"}) as r: pass
            out = await browser(session, admin_tok, os.path.join(hr.ROOT, "build", "test-logs", "panel.png"))
            if out is not None:
                check(out.get("echos") == ["Echo Show 5 a1b2c3"] and any("Screen on" in x and "Echo-Wall" in x for x in out.get("sub", []))
                      # Home: the home app is Echo-Wall (its panel embedded, or the way to it in HACS where it is missing)
                      and out.get("tabs") == ["Screen", "Home"] + (["Apps"] if HAVE_APPS else []),
                      f"the panel: the Echo, its screen on with Echo-Wall, the Screen and Apps tabs ({out.get('echos')}, {out.get('sub')}, {out.get('tabs')})")
                scr = out.get("screen") or {}
                check("Connect it to Home Assistant" in scr.get("rows", []) and "Sent to the Echo" in scr.get("push", ""),
                      f"the Screen tab: the kiosk's options, the last push ({scr.get('rows', [])[:2]}, {scr.get('push')!r})")
                if EW_INT:
                    hm = out.get("home") or {}
                    check(hm.get("dev") == "dev_echo_show" and hm.get("embedded") and hm.get("header") == "none" and hm.get("kept"),
                          f"the Home tab: Echo-Wall's panel for this Echo's device, no header or list of its own, kept across tabs ({hm})")
                for k_, f_ in (out.get("fit") or {}).items():
                    check(isinstance(f_, dict) and not f_.get("wide") and f_.get("scroll", 1) <= 1 and (k_.startswith("phone") or f_.get("nav", 0) >= 250),
                          f"{k_}: fits the window, the Echos' list whole ({f_})")
                if HAVE_APPS: check("org.schabi.newpipe (installed)" in scr.get("apps", []) and "Echo-Wall" in scr.get("apps", []),
                      f"the Screen tab: any app installed on the Echo offered as the screen app ({[x for x in scr.get('apps', []) if '.' in x or 'Echo' in x][:6]})")
                ap = out.get("appstab") or {}
                if HAVE_APPS: check("Apps" in ap.get("tabs", []) and "org.schabi.newpipe" in ap.get("apps", []) and ap.get("screenApp") == [True]
                      and ap.get("status", "").startswith("Removed org.example.app"),
                      f"the Apps tab: the apps, Remove off for the screen app ({ap})")
    finally:
        echo.stop()
        hass.send_signal(signal.SIGTERM)
        try: hass.wait(30)
        except subprocess.TimeoutExpired: hass.kill()
        log = open(os.path.join(tmp, "ha.log")).read()
        bad = [l for l in log.splitlines() if "echo_local" in l and ("Traceback" in l or "ERROR" in l)]
        check(not bad, f"no errors of echo_local in Home Assistant's log ({bad[:3]})")
        if check.failed:
            print(log[-6000:])


def main():
    if os.environ.get("HASSMIC_IN_NETNS") != "1":
        if not os.path.exists(hr.HA_PY):
            hr.skip(f"no Home Assistant at {hr.HA_PY} (.venv/bin/python tests/ha_real_dropin.py --setup makes one)")
        if not os.path.exists(os.path.join(hr.ROOT, "build", "hassmic-host")): sys.exit("make host first")
        if subprocess.run(["unshare", "-rn", "true"], capture_output=True).returncode:
            hr.skip("unshare -rn is not allowed here (user namespaces)")
        env = dict(os.environ, HASSMIC_IN_NETNS="1")
        sys.exit(subprocess.run(["unshare", "-rn", sys.executable, os.path.abspath(__file__), *sys.argv[1:]], env=env).returncode)
    hr.loopback_up()
    tmp = tempfile.mkdtemp(prefix="ha-home-")
    try: asyncio.run(run(tmp))
    finally: shutil.rmtree(tmp, ignore_errors=True)
    print("FAILED" if check.failed else "all good")
    sys.exit(1 if check.failed else 0)


if __name__ == "__main__":
    main()
