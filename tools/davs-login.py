#!/usr/bin/env python3
"""Probe: a DAVS token without stock Alexa.  Registers a device with Amazon from the PC, asks DAVS with its token,
deregisters it again.  If this works, the settings page can download models by itself (no MODE=stock-online, no reboot,
no Alexa app).

Two ways in:
  cbl  The Echo's own (libace_map.so, map_registration_calculate_code_pair / _register_device): POST /auth/create/codepair
       with the Echo's serial and device type gives a public code; the user enters it at amazon.<domain>/code; the
       device polls /auth/register with the code pair.  On stock the Alexa app enters the code for the user.
       Serial, device type, device secret and MAC are the Echo's (/proc/idme/, read over adb; see register()).
  app  The Alexa app's (as alexapy/audible do): the user logs in on Amazon's page in a browser (PKCE: the verifier
       stays here), Amazon lands on /ap/maplanding with an authorization code in the URL; the user pastes that URL;
       /auth/register with device type A2IVLV5VM2W81 (Alexa app for iOS) and a random serial.
Open questions this answers: does Amazon register an Echo device type this way, and does DAVS hand wake word, sound
detection and whisper artifacts to a token of each?

  tools/davs-login.py login cbl|app [--domain de] [--serial S --device-type T]
  tools/davs-login.py check [locale]          # refresh the access token, ask DAVS for each kind (no download)
  tools/davs-login.py fetch <key> [locale]    # download through tools/davs-fetch.py into device-logs/models
  tools/davs-login.py logout                  # /auth/deregister, then forget the tokens
  tools/davs-login.py run cbl|app [...]       # login, check, logout
The registration lives in device-logs/davs-login.json (git-ignored, mode 600) between the steps, so a failed run can
still be deregistered.  Tokens are never printed.
"""
import base64, hashlib, importlib.util, json, os, pathlib, secrets, sqlite3, subprocess, sys, tempfile, time
import urllib.error, urllib.parse, urllib.request

STATE = pathlib.Path("device-logs/davs-login.json")
APP_TYPE = "A2IVLV5VM2W81"                  # Alexa app, iOS
# what map.db of a registered Echo says it registered with (reg_app_name ...)
ECHO_REG = {"app_name": "MYAPP", "app_version": "0.1", "os_version": "1.1", "device_model": "EchoDevice",
            "software_version": "1"}
APP_REG = {"app_name": "Amazon Alexa", "app_version": "2.2.556530.0", "os_version": "16.6", "device_model": "iPhone",
           "software_version": "1"}

spec = importlib.util.spec_from_file_location("davs_fetch", pathlib.Path(__file__).with_name("davs-fetch.py"))
davs = importlib.util.module_from_spec(spec); spec.loader.exec_module(davs)

def api(st): return f"https://api.amazon.{st['domain']}"

def post(url, body, token=None, sst=None):
    hdr = {"Content-Type": "application/json", "Accept-Language": "en-US",
           "x-amzn-identity-auth-domain": urllib.parse.urlsplit(url).netloc}
    if token: hdr["Authorization"] = "Bearer " + token
    # the Echo's MAP sends the secureSessionToken of the codepair answer back (libace_connectivity_manager.so policy,
    # stream map_1p_post: metadata "sst"); without it /auth/register refuses the code pair with InvalidDevice
    if sst: hdr["x-amzn-identity-secure-session-token"] = sst
    req = urllib.request.Request(url, json.dumps(body).encode(), hdr)
    try:
        with urllib.request.urlopen(req, timeout=30) as r: return r.status, json.load(r)
    except urllib.error.HTTPError as e:
        raw = e.read()
        try: return e.code, json.loads(raw)
        except ValueError: return e.code, {"raw": raw[:300].decode(errors="replace")}

def err(ans):                                # the error part of an answer, without anything token-like
    a = ans.get("response", ans)
    return json.dumps(a.get("error", a))[:300]

def save(st):
    STATE.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(STATE, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f: json.dump(st, f, indent=1)

def load():
    if not STATE.exists(): sys.exit(f"no registration in {STATE}: login first")
    return json.loads(STATE.read_text())

def adb_idme(name):                           # idme values end in a NUL byte
    return subprocess.run(["adb", "shell", f"cat /proc/idme/{name}"], capture_output=True, text=True,
                          check=True).stdout.strip("\0 \r\n\t")

def store_tokens(st, ans):
    succ = ans["response"]["success"]
    bearer = succ["tokens"]["bearer"]
    st.update(refresh_token=bearer["refresh_token"], access_token=bearer["access_token"],
              expires=time.time() + int(bearer.get("expires_in", 3600)) - 60)
    ext = succ.get("extensions", {})
    st["device_name"] = ext.get("device_info", {}).get("device_name")
    st["marketplace"] = ext.get("customer_info", {}).get("preferred_marketplace")
    save(st)
    print(f"registered: {st['device_type']} '{st['device_name']}', marketplace {st['marketplace']}")

# idme: what the Echo's MAP sends with its registration (fill_idme_info: DsHal entries 0xa serial, 0xb device_type_id,
# 0xe mac_sec, 0xc mac_addr).  mac_sec is the device secret Amazon checks for an Echo device type: without it
# /auth/register answers 400 InvalidDevice even once the code was entered (Echo Dot 2, 2026-10-06).  Kept in memory only.
def register(st, auth_data, idme=None, sst=None):
    reg = dict(domain="Device", device_type=st["device_type"], device_serial=st["serial"], **st["reg"])
    body = {"requested_token_type": ["bearer", "mac_dms"],
            "requested_extensions": ["device_info", "customer_info"],
            "auth_data": auth_data, "registration_data": reg}
    if idme:
        reg["device_secret"] = idme["mac_sec"]
        body["device_metadata"] = {"mac_address": idme["mac_addr"]}
    return post(api(st) + "/auth/register", body, sst=sst)

def login_cbl(st):
    code, ans = post(api(st) + "/auth/create/codepair",
                     {"code_data": {"domain": "Device", "device_serial": st["serial"], "device_type": st["device_type"]}})
    if code != 200: sys.exit(f"codepair: HTTP {code} {err(ans)}")
    idme = {k: adb_idme(k) for k in ("mac_sec", "mac_addr")} if st.get("idme") else None
    pub, priv, sst = ans["public_code"], ans["private_code"], ans.get("secureSessionToken")
    every, until = int(ans.get("polling_interval_in_seconds", 5)), time.time() + int(ans.get("expires_in", 600))
    print(f"\n  Open https://www.amazon.{st['domain']}/code and enter:  {pub}\n  (waiting until it is entered)\n")
    last, start = None, time.time()
    while time.time() < until:
        time.sleep(every)
        code, ans = register(st, {"code_pair": {"public_code": pub, "private_code": priv},
                                  "use_global_authentication": "true"}, idme, sst)
        sst = ans.get("secureSessionToken", sst)            # each answer may carry a newer one
        if code == 200: return store_tokens(st, ans)
        a = ans.get("response", ans).get("error", ans)      # "index" differs in every answer: compare the code
        if (code, a.get("code")) != last:
            last = code, a.get("code"); print(f"  {time.time() - start:4.0f} s register: HTTP {code} {a.get('code')} {a.get('message', '')}")
    sys.exit("code expired")

def login_app(st):
    verifier = base64.urlsafe_b64encode(secrets.token_bytes(32)).rstrip(b"=").decode()
    challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).rstrip(b"=").decode()
    client_id = (st["serial"] + "#" + st["device_type"]).encode().hex()
    tld = st["domain"].split(".")[-1]
    handle = "amzn_dp_project_dee_ios" + ("" if st["domain"] == "com" else "_" + tld)
    q = {"openid.return_to": f"https://www.amazon.{st['domain']}/ap/maplanding",
         "openid.assoc_handle": handle, "pageId": "amzn_dp_project_dee_ios",
         "openid.identity": "http://specs.openid.net/auth/2.0/identifier_select",
         "openid.claimed_id": "http://specs.openid.net/auth/2.0/identifier_select",
         "openid.mode": "checkid_setup", "openid.ns": "http://specs.openid.net/auth/2.0",
         "openid.ns.oa2": "http://www.amazon.com/ap/ext/oauth/2", "openid.oa2.client_id": "device:" + client_id,
         "openid.ns.pape": "http://specs.openid.net/extensions/pape/1.0", "openid.pape.max_auth_age": "0",
         "openid.oa2.response_type": "code", "openid.oa2.scope": "device_auth_access",
         "openid.oa2.code_challenge_method": "S256", "openid.oa2.code_challenge": challenge,
         "accountStatusPolicy": "P1", "language": "en_US"}
    print(f"\n  Log in here, then paste the address the browser ends on (…/ap/maplanding?…):\n\n"
          f"  https://www.amazon.{st['domain']}/ap/signin?{urllib.parse.urlencode(q)}\n")
    landed = urllib.parse.parse_qs(urllib.parse.urlsplit(input("  > ").strip()).query)
    code = landed.get("openid.oa2.authorization_code", [None])[0]
    if not code: sys.exit("no openid.oa2.authorization_code in that address")
    status, ans = register(st, {"client_id": client_id, "authorization_code": code, "code_verifier": verifier,
                                "code_algorithm": "SHA-256", "client_domain": "DeviceLegacy",
                                "use_global_authentication": "true"})
    if status != 200: sys.exit(f"register: HTTP {status} {err(ans)}")
    store_tokens(st, ans)

def access(st):
    if time.time() < st["expires"]: return st["access_token"]
    code, ans = post(api(st) + "/auth/token",
                     {"requested_token_type": "access_token", "app_name": st["reg"]["app_name"],
                      "app_version": st["reg"]["app_version"], "source_token_type": "refresh_token",
                      "source_token": st["refresh_token"]})
    if code != 200: sys.exit(f"token refresh: HTTP {code} {err(ans)}")
    st.update(access_token=ans["access_token"], expires=time.time() + int(ans.get("expires_in", 3600)) - 60)
    save(st)
    return st["access_token"]

def check(st, locale="de-DE"):
    tok, ok = access(st), True
    reqs = [("whisper", davs.WHISPER_REQ),
            (f"wake word echo {locale}", {"artifactType": "wakeword", "artifactKey": "echo",
                                          "filters": {"engineCompatibilityIdList": davs.ENGINE_IDS, "locale": [locale],
                                                      "modelClass": ["B"]}}),
            (f"sound detection {davs.REGION.get(locale, 'EU')}",
             {"artifactType": "AED", "artifactKey": "AED",
              "filters": {"filterVersion": ["2"], "engineCompatibilityIdList": davs.AED_IDS, "modelClass": ["class-10"],
                          "location": [davs.REGION.get(locale, "EU")]}})]
    for name, req in reqs:
        try:
            info = davs.ask(tok, req)
            print(f"  {name}: 200, id {info.get('artifactIdentifier', '?')}")
        except urllib.error.HTTPError as e:
            ok = False
            print(f"  {name}: HTTP {e.code} {e.read()[:200]!r}")
    return ok

def fetch(st, key, locale):
    tok = access(st)
    with tempfile.TemporaryDirectory() as d:     # davs-fetch.py takes the token from a map.db
        db = os.path.join(d, "map.db")
        c = sqlite3.connect(db); c.execute("create table deviceData (key text, value blob)")
        c.execute("insert into deviceData values ('access_token', ?)", (tok,)); c.commit(); c.close()
        return subprocess.run([sys.executable, str(pathlib.Path(__file__).with_name("davs-fetch.py")), db, key, locale]).returncode

def logout(st):
    code, ans = post(api(st) + "/auth/deregister",
                     {"request_metadata": {"app_version": st["reg"]["app_version"], "app_name": st["reg"]["app_name"]}},
                     access(st))
    if code != 200:
        sys.exit(f"deregister: HTTP {code} {err(ans)}\n  still registered: Alexa app → Devices → '{st.get('device_name')}' → Deregister, "
                 f"then delete {STATE}")
    STATE.unlink()
    print(f"deregistered '{st.get('device_name')}', {STATE} deleted")

def new_state(way, args):
    opt = dict(zip(args[::2], args[1::2]))
    st = {"way": way, "domain": opt.get("--domain", "com")}
    if way == "cbl":
        st["idme"] = not opt.get("--serial")       # the Echo on adb is the device: its secret goes along
        st.update(serial=opt.get("--serial") or adb_idme("serial"),
                  device_type=opt.get("--device-type") or adb_idme("device_type_id"), reg=ECHO_REG)
    else:
        st.update(serial=opt.get("--serial") or secrets.token_hex(16), device_type=opt.get("--device-type", APP_TYPE),
                  reg=APP_REG)
    if STATE.exists(): sys.exit(f"{STATE} holds a registration: logout first")
    print(f"{way}: device type {st['device_type']}, serial {st['serial'][:4]}…, api.amazon.{st['domain']}")
    return st

def main():
    a = sys.argv[1:]
    if not a: sys.exit(__doc__)
    if a[0] in ("login", "run") and a[1:2] in (["cbl"], ["app"]):
        st = new_state(a[1], a[2:])
        (login_cbl if a[1] == "cbl" else login_app)(st)
        if a[0] == "run":
            check(st)
            logout(st)
    elif a[0] == "check": sys.exit(0 if check(load(), *a[1:2]) else 1)
    elif a[0] == "fetch" and len(a) > 1: sys.exit(fetch(load(), a[1], a[2] if len(a) > 2 else "de-DE"))
    elif a[0] == "logout": logout(load())
    else: sys.exit(__doc__)

if __name__ == "__main__":
    main()
