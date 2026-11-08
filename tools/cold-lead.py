#!/usr/bin/env python3
"""How much of a sound an Android Echo loses when its output was closed: board.cold_lead_ms (audio_android.c).

AudioFlinger closes the output after ~3 s without audio, and on some models the speaker amplifier then switches on only
as the next sound starts and swallows its start (the Echo Show 8's 120 ms wake blip was never heard, 2026-10-14).  The
Echo plays, through slplay (OpenSL ES on the music stream, as hassmic's players), a lead-in of silence, a 500 ms 1 kHz
tone A, 300 ms of silence and the same tone B, after WAIT seconds without audio, while a microphone on this PC records
it (pw-record, PipeWire).  B plays on a warm output: A shorter than B within 10 dB of B's level is what the cold
start cost.  The Echo's own microphones cannot measure it (no raw stream on the Shows, and the processed one cancels its
playback), and the PC's clock is not the Echo's, hence the tone against its own copy.  Each lead-in is played once
per wait; the result names the shortest one that lost nothing.

    tools/cold-lead.py [-s pipewire-source] [-l 0,100,200,350] [-w 4,8,15,30]

The Echo on adb (ANDROID_SERIAL with two), build/<codename>/slplay built (make DEVICE=<codename>), hassmic quiet
meanwhile (no music, no timer), the microphone within a metre or two of the Echo, the room quiet.
"""
import argparse, math, os, shutil, struct, subprocess, sys, tempfile, time, wave

RATE = 48000
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEV_DIR = "/data/local/tmp/cold-lead"
LOST_MS = 6                   # within the analysis' 2 ms blocks and the room's reverb


def tone(ms):
    n = RATE * ms // 1000
    return [int(12000 * min(1, i / 480, (n - i) / 480) * math.sin(2 * math.pi * 1000 * i / RATE)) for i in range(n)]


def signal(lead_ms):
    s = [0] * (RATE * lead_ms // 1000) + tone(500) + [0] * (RATE * 3 // 10) + tone(500) + [0] * (RATE // 10)
    return struct.pack("<%dh" % len(s), *s)


def adb(*args, **kw):
    return subprocess.run(["adb", *args], check=True, capture_output=True, text=True, **kw).stdout


def bandpass(x, f=1000.0, q=5.0):
    """RBJ biquad around the tone: a voice or a door in the room is not the tone (2 ms of group delay, both tones alike)"""
    w = 2 * math.pi * f / RATE; al = math.sin(w) / (2 * q); a0 = 1 + al
    b0, b2, a1, a2 = al / a0, -al / a0, -2 * math.cos(w) / a0, (1 - al) / a0
    y, x1, x2, y1, y2 = [], 0.0, 0.0, 0.0, 0.0
    for v in x:
        o = b0 * v + b2 * x2 - a1 * y1 - a2 * y2
        x2, x1, y2, y1 = x1, v, y1, o
        y.append(o)
    return y


def levels(path):
    """dBFS per 2 ms of the loudest channel, in the tone's band"""
    with wave.open(path) as w:
        ch, n = w.getnchannels(), w.getnframes()
        x = struct.unpack("<%dh" % (n * ch), w.readframes(n))
    best = max(range(ch), key=lambda c: sum(abs(v) for v in x[c::ch]))
    s, b = bandpass(x[best::ch]), RATE // 500
    return [20 * math.log10(math.sqrt(sum(v * v for v in s[i:i + b]) / b) / 32768 + 1e-9) for i in range(0, len(s) - b, b)]


def tones(e):
    """the two tones: (A length within 10 dB of B's level, A's time to -3 dB, the same for B, B over the room), ms;
    A swallowed whole: one tone, 0 and 0 for A; None if not even B is there"""
    floor = sorted(e)[len(e) // 10]
    segs, i = [], 0
    while i < len(e):
        if e[i] > floor + 15:
            j = i
            while j < len(e) and max(e[j:j + 10]) > floor + 15: j += 1
            segs.append((i, j)); i = j
        else: i += 1
    segs = [s for s in segs if s[1] - s[0] > 100]
    if not segs: return None
    (a0, a1), (b0, b1) = segs[-2] if len(segs) > 1 else (0, 0), segs[-1]
    mid = sorted(e[b0 + 50:b1 - 20])
    level = mid[len(mid) // 2]
    if level < floor + 25: return None
    def length(s0, s1): return 2 * sum(1 for k in range(s0, s1) if e[k] > level - 10)
    def rise(s0, s1): return 2 * next((k - s0 for k in range(s0, s1) if e[k] > level - 3), s1 - s0)
    return length(a0, a1), rise(a0, a1), length(b0, b1), rise(b0, b1), level - floor


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("-s", "--source", help="PipeWire source (pactl list short sources); default: the default one")
    ap.add_argument("-l", "--leads", default="0,100,200,350", help="lead-ins to try, ms")
    ap.add_argument("-w", "--waits", default="4,8,15,30", help="seconds without audio before each play")
    a = ap.parse_args()
    leads, waits = [int(v) for v in a.leads.split(",")], [float(v) for v in a.waits.split(",")]
    if not shutil.which("pw-record"): sys.exit("no pw-record (PipeWire) on this PC")
    codename = adb("shell", "getprop", "ro.product.device").strip()
    slplay = os.path.join(REPO, "build", codename, "slplay")
    if not os.path.exists(slplay): sys.exit(f"no {slplay}: make DEVICE={codename}")
    adb("shell", f"mkdir -p {DEV_DIR}")
    try:
        adb("push", slplay, f"{DEV_DIR}/slplay"); adb("shell", f"chmod 755 {DEV_DIR}/slplay")
        tmp = tempfile.mkdtemp(prefix="cold-lead-")
        for L in leads:
            p = os.path.join(tmp, f"t{L}.raw")
            with open(p, "wb") as f: f.write(signal(L))
            adb("push", p, f"{DEV_DIR}/t{L}.raw")
        print(f"{codename}: lead-in ms, wait s: tone A against B (length within 10 dB, time to -3 dB), lost")
        worst = {}
        for L in leads:
            for W in waits:
                time.sleep(W)
                wav = os.path.join(tmp, f"l{L}_w{W:g}.wav")
                rec = subprocess.Popen(["pw-record", *(["--target", a.source] if a.source else []), "--rate", str(RATE),
                                        "--format", "s16", wav], stderr=subprocess.DEVNULL)
                time.sleep(0.5)
                if rec.poll() is not None:
                    sys.exit("pw-record did not start: pactl list short sources names the sources (-s)")
                adb("shell", f"{DEV_DIR}/slplay < {DEV_DIR}/t{L}.raw")
                time.sleep(1); rec.terminate(); rec.wait()
                t = tones(levels(wav))
                if not t: print(f"  {L:4} {W:4g}: tones not found (too quiet, a noisy room?): {wav}"); continue
                if abs(t[2] - 490) > 30:            # B plays warm: not its 500 ms, a noise in the room
                    print(f"  {L:4} {W:4g}: B {t[2]} ms long, not 500: disturbed, not counted ({wav})"); continue
                if not t[0]: print(f"  {L:4} {W:4g}: tone A lost whole, B {t[2]} ms"); worst[L] = 500; continue
                # the length within 10 dB: a cut, and a fade-in that keeps the start under -10 dB; the rise to -3 dB is
                # shown only, a room's noise moves it by 10-20 ms (checkers, 2026-11-07: B 14 ms quiet, 26 ms not)
                lost = max(t[2] - t[0], 0)
                worst[L] = max(worst.get(L, 0), lost)
                print(f"  {L:4} {W:4g}: A {t[0]} ms, {t[1]} ms; B {t[2]} ms, {t[3]} ms; lost {lost} ms ({t[4]:.0f} dB over the room)")
    finally:
        subprocess.run(["adb", "shell", f"rm -r {DEV_DIR}"], capture_output=True)
    ok = [L for L in leads if L in worst and worst[L] <= LOST_MS]
    if not ok: print("every lead-in lost part of the tone: try longer ones (-l)"); return 1
    print(f"shortest lead-in that lost nothing: {min(ok)} ms -> board.cold_lead_ms = {min(ok) or -1}")
    if not min(ok):
        print("without a lead-in the output's open is in earsup.c's echo delay: check in logcat that createTrack to the "
              "HAL's first write plus 85 ms stays well under its D_MAX, 400 ms (checkers: 56 ms)")


if __name__ == "__main__":
    sys.exit(main())
