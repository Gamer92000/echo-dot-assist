"""What the tests/fake_ha_esphome*.py files share: an Echo (build/hassmic-host) with the state, files and fake devices
those tests give it, Home Assistant's media server (HTTP, HTTPS by a CA the Echo is given, HTTPS self-signed), its
settings page logged in, and Home Assistant's side of the ESPHome native API through the reference `aioesphomeapi`
client (the library Home Assistant itself uses), so framing and protobuf layout are checked by the real parser.
One file was 90 s of mostly waiting; split, tests/run.py runs the parts at once.

    async def main(e): ...                            # e: Echo
    run(main, (PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT))"""
import asyncio, io, math, os, signal, socket, struct, subprocess, sys, tempfile, threading, time, types, wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from aioesphomeapi import APIClient, MediaPlayerInfo

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
from webclient import Browser

KEY_HELP, KEY_VOLUMEUP, KEY_VOLUMEDOWN = 138, 115, 114


def tone(rate, seconds, freq=440):
    return b"".join(struct.pack("<h", int(8000 * math.sin(2 * math.pi * freq * i / rate))) for i in range(int(rate * seconds)))


def wav_bytes(rate, seconds):
    b = io.BytesIO()
    with wave.open(b, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate); w.writeframes(tone(rate, seconds))
    return b.getvalue()


late_asked, late_done = threading.Event(), threading.Event()


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith("/moved"):            # Home Assistant behind a proxy that redirects
            self.send_response(302); self.send_header("Location", "/a.wav"); self.send_header("Content-Length", "0"); self.end_headers(); return
        body = wav_bytes(48000, 3.0 if "s=3" in self.path else 0.5)
        if self.path.endswith(".mp3"):                  # what Home Assistant sends for a TTS announcement before any pipeline ran
            # 1 s of 440 Hz, 24 kHz mono: ffmpeg -bitexact -f lavfi -i sine=frequency=440:duration=1 -ar 24000 -ac 1
            # -map_metadata -1 -f mp3 (kept as a file: ffmpeg was most of CI's package install)
            with open(os.path.join(ROOT, "testdata", "sine440_24k.mp3"), "rb") as f: body = f.read()
        self.send_response(200); self.send_header("Content-Type", "audio/wav"); self.end_headers()   # no length: like a transcoding proxy
        if "late" in self.path:                         # streamed TTS while the LLM still works: the audio comes later
            late_asked.set(); self.wfile.flush(); time.sleep(3)
        try: self.wfile.write(body)
        except OSError: pass                            # the Echo hung up
        finally:
            if "late" in self.path: late_done.set()
    def log_message(self, *a): pass


def tls_files(d):
    """A CA and a certificate it signed for 127.0.0.1, and a self-signed one for the same address (Home Assistant with
    its own certificate): PEM files in d."""
    import datetime, ipaddress
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.x509.oid import NameOID
    now = datetime.datetime.now(datetime.timezone.utc)
    def cert(name, key, issuer, issuer_key, ca):
        b = (x509.CertificateBuilder().subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, name)]))
             .issuer_name(issuer).public_key(key.public_key()).serial_number(x509.random_serial_number())
             .not_valid_before(now - datetime.timedelta(days=1)).not_valid_after(now + datetime.timedelta(days=30))
             .add_extension(x509.BasicConstraints(ca=ca, path_length=None), critical=True))
        if not ca: b = b.add_extension(x509.SubjectAlternativeName([x509.IPAddress(ipaddress.ip_address("127.0.0.1"))]), critical=False)
        return b.sign(issuer_key, hashes.SHA256())
    def write(name, *objs):
        with open(os.path.join(d, name), "wb") as f:
            for o in objs:
                f.write(o.public_bytes(serialization.Encoding.PEM) if isinstance(o, x509.Certificate) else
                        o.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
        return os.path.join(d, name)
    ca_key, key, self_key = (ec.generate_private_key(ec.SECP256R1()) for _ in range(3))
    ca_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "hassmic test CA")])
    ca = cert("hassmic test CA", ca_key, ca_name, ca_key, True)
    srv = cert("127.0.0.1", key, ca_name, ca_key, False)
    own = cert("127.0.0.1", self_key, x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "127.0.0.1")]), self_key, False)
    return write("ca.pem", ca), write("srv.pem", srv, key), write("self.pem", own, self_key)


def https_server(port, pem):
    import ssl
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(pem)
    class TLSServer(ThreadingHTTPServer):
        def shutdown_request(self, request):        # close_notify at the end, as aiohttp sends it: the body ends there
            try: request.unwrap()
            except (OSError, ValueError): pass
            super().shutdown_request(request)
    httpd = TLSServer(("127.0.0.1", port), Handler)
    httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: check.failed = True
check.failed = False


# Waiting: for what should happen, until() polls (and gives up after its timeout: the check after it then fails);
# sync() is a request/answer round trip on that connection, so everything sent before it has been handled.  Fixed
# sleeps are left only where a check is that something does NOT happen.
async def until(cond, timeout=5.0):
    end = asyncio.get_running_loop().time() + timeout
    while not cond() and asyncio.get_running_loop().time() < end:
        await asyncio.sleep(0.02)
    return cond()

async def sync(c): await asyncio.wait_for(c.device_info(), 5)

def peak(b): return max(abs(v) for v in struct.unpack(f"<{len(b) // 2}h", b[:len(b) // 2 * 2])) if b else 0

async def speech(mic):
    # testdata/alexa_espeak.raw loops at real time, 8.4 s with 3.8 s of silence: wait for 1 s of audio with speech in
    # it, wherever in the loop the pipeline started
    await until(lambda: len(mic) > 16000 and peak(mic) > 8000, 12)

async def settled(path, quiet=0.4, timeout=5.0):
    # playback has stopped: the file did not grow for `quiet` s (audio_file.c writes at real-time speed, stdio-buffered:
    # it grows in steps of a few KB, under 0.1 s of audio each)
    end = asyncio.get_running_loop().time() + timeout; n, t = -1, 0.0
    while asyncio.get_running_loop().time() < end:
        m = os.path.getsize(path)
        if m != n: n, t = m, asyncio.get_running_loop().time()
        elif asyncio.get_running_loop().time() - t >= quiet: return
        await asyncio.sleep(0.02)

def port_open(port):
    try: socket.create_connection(("127.0.0.1", port), 0.2).close(); return True
    except OSError: return False


def assistant(cli):
    """Home Assistant's voice assistant on that connection: every run's mic audio, starts, stops, announcements"""
    va = types.SimpleNamespace(started=asyncio.Event(), mic=bytearray(), chunks=[], stopped=[], finished=[])
    async def handle_start(conv_id, flags, settings, phrase):
        handle_start.args = (flags, phrase); handle_start.audio = (settings.noise_suppression_level, settings.auto_gain, round(settings.volume_multiplier, 2)); va.started.set(); return 0           # 0 = audio over the API connection
    async def handle_stop(abort): va.stopped.append(abort)
    async def handle_audio(data, *_): va.mic.extend(data); va.chunks.append(len(data))
    async def handle_finished(msg): va.finished.append(msg.success)
    va.handle_start, va.handle_stop, va.handle_audio, va.handle_finished = handle_start, handle_stop, handle_audio, handle_finished
    va.subscribe = lambda c: c.subscribe_voice_assistant(handle_start=handle_start, handle_stop=handle_stop, handle_audio=handle_audio,
                                                         handle_announcement_finished=handle_finished)
    va.subscribe(cli)
    return va


async def home_assistant(e):
    """Home Assistant connected in plaintext: entities listed, states and the voice assistant subscribed"""
    cli = APIClient("127.0.0.1", e.PORT, None)
    await cli.connect(login=True)
    entities, _ = await cli.list_entities_services()
    states = []; cli.subscribe_states(states.append)
    ha = types.SimpleNamespace(cli=cli, entities=entities, states=states, by={x.object_id: x for x in entities},
                               mp=[x for x in entities if isinstance(x, MediaPlayerInfo)], va=assistant(cli))
    ha.last = lambda k, t: ([x.state for x in states if isinstance(x, t) and x.key == k] or [None])[-1]
    await sync(cli)
    return ha


class Echo:
    """build/hassmic-host as the tests know it: its state in a temp directory, a settings file from before the mic
    level, installed wake word models, a keypad (a FIFO), the light sensor and Wi-Fi driver's files, the settings page
    logged in"""
    def __init__(self, ports, extra_env=None):
        self.PORT, self.HTTP_PORT, self.WEB, self.HTTPS_PORT, self.SELF_PORT = ports
        self.play = tempfile.mktemp(suffix=".raw"); open(self.play, "wb").close()   # its size is polled before the first reply
        self.settings = tempfile.mktemp(suffix=".settings")
        self.state = state = tempfile.mkdtemp(); self.mdns = os.path.join(state, "hassmic.service")
        with open(self.settings, "w") as f: f.write("3 9 4.00 0 1 1 0 en\n")    # from before the mic level: gain values for HA
        self.env = env = dict(os.environ, HASSMIC_STATE=state, HASSMIC_SETTINGS=self.settings, HASSMIC_CAP=f"{ROOT}/testdata/alexa_espeak.raw", HASSMIC_PLAY=self.play,
                   HASSMIC_MDNS_FILE=self.mdns, HASSMIC_ARB_ADDR="127.255.255.255",  # arbitration beacons stay on this PC
                   HASSMIC_MODELS=os.path.join(state, "models"),
                   HASSMIC_ADB_OPEN=os.path.join(state, "adb-open.root"),      # on the Echo: in a directory only root writes
                   HASSMIC_LUX=os.path.join(state, "calibrated_lux"),          # the light sensor's sysfs file
                   HASSMIC_FAKE_SOUND="dogBark",                                # every window "hears" a dog (sound_none.c)
                   HASSMIC_FAKE_WHISPER="1",                                    # a model, and every request whispered (whisper_none.c)
                   HASSMIC_FAKE_WIFI=os.path.join(state, "rx_stat"),            # what the Wi-Fi driver answers RX_STAT (wifimotion.c)
                   HASSMIC_CLOCK_SYNCED="0",                                    # an Echo whose clock nobody set (clock.c)
                   HASSMIC_FAKE_ADDRS=os.path.join(state, "addrs"),             # the Echo's addresses (netaddr.c)
                   HASSMIC_ANNOUNCE_TIMES="3 6")                                # mDNS again after 3 s without a client (100 s)
        env.update(extra_env or {})
        with open(env["HASSMIC_FAKE_ADDRS"], "w") as f: f.write("127.0.0.1\n")
        ca_pem, self.srv_pem, self.self_pem = tls_files(state)
        env["HASSMIC_CA_FILE"] = ca_pem                                     # the CAs the Echo trusts (https.c)
        self.rx_stat(112)
        with open(env["HASSMIC_LUX"], "w") as f: f.write("67\n")
        for m in ("echo-de", "computer-en-US", "computer-de-DE", "alexa-en-US"):   # installed wake word models (the PC build loads none of them)
            os.makedirs(os.path.join(state, "models", m)); open(os.path.join(state, "models", m, "pryon.manifest"), "w").close()
        with open(self.mdns, "w") as f:                     # what main.sh does at boot
            subprocess.run([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(self.PORT), "-n", "Echo Dot", "-S"], env=env, stdout=f, check=True)
        keys = os.path.join(state, "keypad"); os.mkfifo(keys)    # the keypad (-b): input events written by the test
        self.keypad = os.open(keys, os.O_RDWR)                   # opened first: hassmic's open() would wait for a writer
        self.proc = subprocess.Popen([f"{ROOT}/build/hassmic-host", "-P", "esphome", "-p", str(self.PORT), "-n", "Echo Dot", "-L", "-W", str(self.WEB), "-b", keys], env=env)
        self.httpd = ThreadingHTTPServer(("127.0.0.1", self.HTTP_PORT), Handler)
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()
        self.httpsd, self.selfd = https_server(self.HTTPS_PORT, self.srv_pem), https_server(self.SELF_PORT, self.self_pem)

    def keyev(self, code, value): os.write(self.keypad, struct.pack("llHHi", 0, 0, 1, code, value))     # struct input_event, EV_KEY
    def rx_stat(self, rcpi): open(self.env["HASSMIC_FAKE_WIFI"], "w").write(f"RX Stat:\nRX SNR (dB)          = 32\nRCPI RX0             = {rcpi}\n")
    def wake(self): self.proc.send_signal(signal.SIGUSR1)          # the PC's stand-in for the wake word

    def conf(self):                                       # state/config: name=value lines (settings.c)
        try: return dict(l.strip().split("=", 1) for l in open(os.path.join(self.state, "config")) if "=" in l and not l.startswith("#"))
        except OSError: return {}

    def pv(self, name):                                   # a setting's value as the settings page has it
        return next((x["value"] for x in self.page.state()["settings"] if x["name"] == name), None)

    def close(self):
        self.proc.terminate(); self.httpd.shutdown(); self.httpsd.shutdown(); self.selfd.shutdown(); os.close(self.keypad)
        for f in (self.play, self.settings):
            if os.path.exists(f): os.unlink(f)


def run(main, ports, extra_env=None):
    """main(e) against a fresh Echo; prints the verdict and exits with it"""
    async def go():
        e = Echo(ports, extra_env)
        await until(lambda: port_open(e.PORT) and port_open(e.WEB))
        try:
            e.page = Browser(e.WEB)                         # the settings page: what is no longer in Home Assistant
            check(e.page.login_with_button(e.proc), "settings page: logged in with the action button")
            await main(e)
        finally:
            e.close()
    asyncio.run(go())
    print("FAILED" if check.failed else "all good")
    sys.exit(1 if check.failed else 0)
