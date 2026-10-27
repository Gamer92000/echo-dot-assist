#!/usr/bin/env python3
"""The volume curve of an Android Echo (the Echo Show's music stream index), measured with another Echo's microphones.

The Show has no raw microphone stream (startCapture is refused on ship builds) and its processed one cancels its own
playback, so a Dot beside it listens: its micRaw (mixcap), fixed gain, no echo canceller.  The Show plays a steady
multi-tone through slplay (OpenSL ES on the music stream, as hassmic's players) while the index is stepped through
IAudioService as audio_android.c sets it; each step is recorded on the Dot and measured per tone in a narrow band (2 s,
0.5 Hz bins), the room's noise taken from the bins beside each tone, so a loud room only costs the quietest steps.
Four tones at -30 dBFS each, none a harmonic of another: speaker and microphone stay linear, and a loudness dependent
equalizer would show as tones that disagree.  A reference index is measured again every few steps (drift, a volume
that follows the room's noise), and the whole range a second time upwards.  The curve averages the tones whose
reference readings held within REF_MAX_DB; one that wandered (the Show's 3770 Hz did, by 11.6 dB at the same index) is
shown but left out.

  .venv/bin/python tools/vol-curve.py --show <serial> --mic <serial> [--out dir]
  .venv/bin/python tools/vol-curve.py --analyse device-logs/volcurve-...     the table again from the recordings

Needs build/<show model>/slplay, a Dot with mixcap and runas installed (scripts/deploy.sh or a push update), and the
two side by side.  Puts the Show's index back to what hassmic's state/volume says.  Results: device-logs/volcurve-*/
(each step's recording as <step>-idx<index>.raw, curve.json, the table printed).
"""
import argparse, glob, json, os, re, subprocess, sys, time
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RATE = 16000
TONES = [470, 1070, 2230, 3770]          # Hz, integer: a 10 s file loops without a seam
LEVEL = 10 ** (-30 / 20)
SETTLE, REC, SKIP, WIN = 0.8, 3.2, 0.8, 2.0     # s: after setting the index; recorded; dropped (mixcap's backlog); measured
MIN_SNR, REF_MAX_DB = 10, 2.0


def adb(serial, *cmd, check=True):
    r = subprocess.run(["adb", "-s", serial, *cmd], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if check and r.returncode: sys.exit(f"adb -s {serial} {' '.join(cmd)}: {r.stderr.decode().strip()}")
    return r.stdout


def sh(serial, cmd): return adb(serial, "shell", cmd).decode(errors="replace").strip()


def conf(model):
    v = {}
    for line in open(f"{ROOT}/devices/{model}/device.conf"):
        m = re.match(r"(\w+)=\"?([^\"#]*?)\"?\s*(#.*)?$", line.strip())
        if m: v[m.group(1)] = m.group(2)
    return v


def parcel_int(text):
    m = re.search(r"Parcel\(([0-9a-f]{8}) ([0-9a-f]{8})", text)
    return int(m.group(2), 16) if m and int(m.group(1), 16) == 0 else None


def measure(raw):
    """each tone's level (dB, the noise beside it taken off) and how far above that noise it is"""
    x = np.frombuffer(raw[: len(raw) // 2 * 2], dtype="<i2").astype(float) / 32768
    seg = x[int(SKIP * RATE): int((SKIP + WIN) * RATE)]
    if len(seg) < WIN * RATE * 0.9: return None
    spec = np.abs(np.fft.rfft(seg * np.hanning(len(seg)))) ** 2
    hz = len(seg) / RATE                                          # bins per Hz
    res = {"peak": float(np.abs(x).max())}
    for f in TONES:
        k = int(round(f * hz))
        tone = spec[k - 3: k + 4].sum()
        side = np.r_[spec[k - int(40 * hz): k - int(8 * hz)], spec[k + int(8 * hz): k + int(40 * hz)]]
        noise = np.mean(side) * 7                                 # the 7 bins' worth: a mean (a median of noise bins is ln 2 of it)
        sig = max(tone - noise, 1e-30)
        res[f] = {"db": float(10 * np.log10(sig)), "snr": float(10 * np.log10(sig / max(noise, 1e-30)))}
    return res


def row_line(k, i, m):
    return (f"  {k:3d} index {i:2d}: " + "  ".join(f"{f}:{m[f]['db']:6.1f}({m[f]['snr']:5.1f})" for f in TONES)
            + f"  peak {m['peak']:.2f}")


def analyse(out, rows, mx, ref_index, model="", mic=""):
    """rows: [{step, index, <tone>: {db, snr}, peak}]; prints the table, writes curve.json"""
    refs = [r for r in rows if r["index"] == ref_index]
    if len(refs) < 2: print(f"warning: index {ref_index} measured {len(refs)} time(s): no telling which tones drift, all of them taken")
    drift = {f: float(np.ptp([r[f]["db"] for r in refs])) if len(refs) > 1 else 0.0 for f in TONES}
    used = [f for f in TONES if drift[f] <= REF_MAX_DB]
    top = [r for r in rows if r["index"] == mx]
    if not top: sys.exit(f"no reading at index {mx}, the top everything is measured against: measure again")
    ref = {f: np.mean([r[f]["db"] for r in top]) for f in TONES}
    curve = {}
    for i in range(mx + 1):
        rs = [r for r in rows if r["index"] == i]
        per = {f: [r[f]["db"] - ref[f] for r in rs if r[f]["snr"] >= MIN_SNR] for f in TONES}
        good = [np.mean(per[f]) for f in used if per[f]]
        curve[i] = {"db": float(np.mean(good)) if good else None, "spread": float(np.ptp(good)) if len(good) > 1 else None,
                    "tones": len(good), "per": {f: (float(np.mean(v)) if v else None) for f, v in per.items()}}
    print(f"\nreference index {ref_index}, its {len(refs)} readings span per tone: "
          + "  ".join(f"{f}: {drift[f]:.1f} dB" for f in TONES) + f"; the curve from {used}")
    print(f"\nindex  dB under {mx}  spread  tones   " + "  ".join(f"{f:>6}" for f in TONES))
    for i, c in curve.items():
        d = "    --" if c["db"] is None else f"{c['db']:6.1f}"
        s = "      " if c["spread"] is None else f"{c['spread']:6.1f}"
        print(f"{i:5d}  {d:>11}  {s}  {c['tones']:5d}   " + "  ".join("    --" if v is None else f"{v:6.1f}" for v in c["per"].values()))
    # for board.c: a point every 5 volume points, each at the index hassmic plays that volume at (audio_android.c)
    pts = []
    for v in range(0, 101, 5):
        db = curve[(v * mx + 50) // 100]["db"]
        if db is not None or v == 0: pts.append(f"{{ {v}, {-66.0 if db is None else round(db, 1)}f }}")
        else: print(f"warning: volume {v} (index {(v * mx + 50) // 100}) not heard over the noise: left out, the table interpolates there")
    print("\n.volume_curve = (const struct board_vol[]){ " + ", ".join(pts) + ", { -1, 0 } },")
    json.dump({"model": model, "mic": mic, "max": mx, "tones": TONES, "used": used, "ref_index": ref_index, "ref_drift": drift,
               "rows": rows, "curve": curve}, open(f"{out}/curve.json", "w"), indent=1)   # int keys come out as strings
    print(f"\n{out}/curve.json")


def from_files(out, ref_index):
    rows = []
    for p in sorted(glob.glob(f"{out}/[0-9][0-9][0-9]-idx[0-9][0-9].raw")):
        k, i = map(int, re.findall(r"(\d+)-idx(\d+)", p)[0])
        m = measure(open(p, "rb").read())
        if m: rows.append({"step": k, "index": i, **{f: m[f] for f in TONES}, "peak": m["peak"]}); print(row_line(k, i, m))
    analyse(out, rows, max(r["index"] for r in rows), ref_index)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--show"); ap.add_argument("--mic"); ap.add_argument("--out"); ap.add_argument("--analyse")
    ap.add_argument("--ref", type=int, default=21, help="index measured again every few steps")
    a = ap.parse_args()
    if a.analyse: return from_files(a.analyse.rstrip("/"), a.ref)
    if not (a.show and a.mic): ap.error("--show and --mic (adb serials), or --analyse")
    show_model = sh(a.show, "getprop ro.product.device").replace("_puffin", "")
    mic_model = sh(a.mic, "getprop ro.product.device").replace("_puffin", "")
    sdk = int(sh(a.show, "getprop ro.build.version.sdk") or 25)
    tx_set, tx_max = ("7", "14") if sdk >= 28 else ("3", "10")      # audio_android.c
    out = a.out or f"{ROOT}/device-logs/volcurve-{show_model}-{time.strftime('%Y%m%d-%H%M%S')}"
    os.makedirs(out, exist_ok=True)

    # the Dot's microphones: mixcap next to the running build, as its uid (AIPC refuses root)
    mc = conf(mic_model)
    t = sh(a.mic, "for d in /data/local/hassmic/ota/current /system/hassmic /data/local/hassmic; do "
                  "[ -x $d/mixcap ] && [ -x $d/runas ] && { echo $d; break; }; done")
    if not t: sys.exit("no mixcap on the microphone's Echo")
    cap = f"{t}/runas {mc['DAEMON_USER']} {mc['DAEMON_GROUPS']} {t}/mixcap -t micRaw -s {REC} 2>/dev/null"

    def record(name):
        raw = adb(a.mic, "exec-out", cap)
        open(f"{out}/{name}.raw", "wb").write(raw)
        return measure(raw)

    # the Show: slplay and the tone, its maximum index, what to put back
    sp = f"{ROOT}/build/{show_model}/slplay"
    if not os.path.exists(sp): sys.exit(f"{sp} missing: make DEVICE={show_model} {sp[len(ROOT) + 1:]}")
    n = np.arange(10 * RATE) / RATE
    tone = sum(LEVEL * np.sin(2 * np.pi * f * n) for f in TONES)
    open(f"{out}/tone.raw", "wb").write((tone * 32767).astype("<i2").tobytes())
    adb(a.show, "push", sp, "/data/local/tmp/slplay"); adb(a.show, "push", f"{out}/tone.raw", "/data/local/tmp/tone.raw")
    mx = parcel_int(sh(a.show, f"service call audio {tx_max} i32 3"))
    if not mx: sys.exit("no maximum for the music stream")
    # what goes back afterwards: the music stream's index as it is now on the speaker ("dumpsys audio"), else what
    # hassmic's state/volume makes of it; neither: no sweep (the Show would be left at the top index, full volume)
    dump = sh(a.show, "dumpsys audio")
    m = re.search(r"- STREAM_MUSIC:.*?Current:[^\n]*?\b2 \(speaker\): (\d+)", dump, re.S)
    vol = sh(a.show, "cat /data/local/hassmic/state/volume 2>/dev/null").split()
    back = int(m.group(1)) if m else (int(vol[0]) * mx + 50) // 100 if vol else None
    if back is None: sys.exit("the Show's volume now is not known (dumpsys audio, state/volume): nothing to put back, no sweep")

    def index(i):
        r = sh(a.show, f"service call audio {tx_set} i32 3 i32 {i} i32 0 s16 android")
        if "Parcel(00000000" not in r: print("  setting", i, "failed:", r)     # a void method: no exception

    a.ref = min(a.ref, mx)                       # one the Echo has: Android clamps a higher one, the rows would say otherwise
    print(f"{show_model}: index 0..{mx}, measured on {mic_model}'s micRaw; tones {TONES} Hz at -30 dBFS each")
    rows = []
    noise0 = record("noise-before")
    if not noise0: sys.exit("the microphone's Echo recorded nothing (mixcap): is hassmic's build there with mixcap and runas?")
    play = subprocess.Popen(["adb", "-s", a.show, "shell",
                             # the loop ends with the player: a cat writing to it then dies of SIGPIPE
                             "cd /data/local/tmp && chmod 755 slplay && while cat tone.raw; do :; done | ./slplay -r 16000"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.5)
        steps = []
        for i in range(mx, -1, -1):
            steps.append(i)
            if i % 5 == 0 and i != a.ref: steps.append(a.ref)
        steps += list(range(0, mx + 1, 3))
        for k, i in enumerate(steps):
            index(i); time.sleep(SETTLE)
            m = record(f"{k:03d}-idx{i:02d}")
            if not m: print("  short recording at", i); continue
            rows.append({"step": k, "index": i, **{f: m[f] for f in TONES}, "peak": m["peak"]})
            print(row_line(k, i, m), flush=True)
    finally:
        adb(a.show, "shell", "pkill slplay", check=False); play.terminate()       # by name: the loop ends with it
        index(back); print(f"index back at {back}")
    noise1 = record("noise-after") or noise0
    print("noise before/after, dB in the tone bands: " + " ".join(f"{f}: {noise0[f]['db']:.0f}/{noise1[f]['db']:.0f}" for f in TONES))
    analyse(out, rows, mx, a.ref, show_model, mic_model)


if __name__ == "__main__":
    main()
