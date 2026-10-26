#!/usr/bin/env python3
"""web/tar.js (the settings page's models file) against Python's tarfile, through node: what the page writes, tarfile
reads back whole (paths past ustar's 100 bytes, sizes around a block, a file in several pieces, an empty one); what
tarfile writes as ustar, GNU and pax, the page lists with every path, size and content, folders and links left out; a
damaged header or a file cut short is refused."""
import io, json, os, subprocess, sys, tarfile, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
JS = """
const t = require(process.argv[1]), fs = require('fs'), [mode, file] = process.argv.slice(2);
if (mode === 'write') {
  const spec = JSON.parse(fs.readFileSync(0, 'utf8'));            // [[path, hex], ...]: each file in two pieces
  const files = spec.map(([p, x]) => { const b = Buffer.from(x, 'hex'), k = b.length >> 1; return { path: p, parts: [b.subarray(0, k), b.subarray(k)] }; });
  fs.writeFileSync(file, Buffer.concat(t.write(files, 1700000000)));
} else {
  const buf = fs.readFileSync(file), read = async (off, len) => new Uint8Array(buf.subarray(off, off + len));
  t.list(read, buf.length).then((l) => console.log(JSON.stringify(l.map((x) => [x.path, buf.subarray(x.off, x.off + x.size).toString('hex')]))),
    (e) => console.log(JSON.stringify({ error: e.message })));
}
"""
bad = n = 0


def check(cond, what):
    global bad, n
    n += 1
    if not cond:
        bad += 1
        print("FAIL", what)


def node(mode, path, stdin=""):
    r = subprocess.run(["node", "-e", JS, os.path.join(ROOT, "web/tar.js"), mode, path], input=stdin, capture_output=True, text=True)
    if r.returncode:
        print(r.stderr)
        sys.exit(1)
    return r.stdout


deep = "wake/" + "a" * 63 + "/nttfusionconfig/ntt_conv/" + "b" * 63       # 157 bytes: prefix and name
files = {"hassmic-models.json": b'{"hassmic_models":1}', deep: os.urandom(700), "sound/empty": b"",
         "whisper/whisper_components/f511": os.urandom(511), "whisper/f512": os.urandom(512), "whisper/f513": os.urandom(513),
         "mww/x/model.tflite": os.urandom(200000), "wake/ä-test/datei": b"umlaut"}

with tempfile.TemporaryDirectory() as d:
    # the page writes, tarfile reads
    p = os.path.join(d, "page.tar")
    node("write", p, json.dumps([[k, v.hex()] for k, v in files.items()]))
    with tarfile.open(p) as tf:
        got = {m.name: tf.extractfile(m).read() for m in tf.getmembers() if m.isfile()}
    check(got == files, f"the page's tar, read by tarfile: {sorted(got)} vs {sorted(files)}")
    check(os.path.getsize(p) % 512 == 0, "whole blocks")

    # tarfile writes (three formats, a folder and a link among the files), the page lists
    for name, fmt in (("ustar", tarfile.USTAR_FORMAT), ("gnu", tarfile.GNU_FORMAT), ("pax", tarfile.PAX_FORMAT)):
        b = io.BytesIO()
        with tarfile.open(fileobj=b, mode="w", format=fmt) as tf:
            dirinfo = tarfile.TarInfo("wake/"); dirinfo.type = tarfile.DIRTYPE; tf.addfile(dirinfo)
            link = tarfile.TarInfo("wake/link"); link.type = tarfile.SYMTYPE; link.linkname = "x"; tf.addfile(link)
            for k, v in files.items():
                ti = tarfile.TarInfo(k); ti.size = len(v); tf.addfile(ti, io.BytesIO(v))
        p = os.path.join(d, name + ".tar")
        open(p, "wb").write(b.getvalue())
        got = json.loads(node("list", p))
        check(isinstance(got, list) and {k: bytes.fromhex(v) for k, v in got} == files and len(got) == len(files),
              f"tarfile's {name} tar, listed by the page: {got if isinstance(got, dict) else sorted(k for k, _ in got)}")

    # refused: a header that does not add up, a file cut in the middle of a file
    raw = bytearray(open(os.path.join(d, "ustar.tar"), "rb").read())
    raw[1024 + 10] ^= 1                                       # a name byte of the third header (the folder and link have no data)
    open(p, "wb").write(raw)
    got = json.loads(node("list", p))
    check(isinstance(got, dict) and "damaged" in got.get("error", ""), f"a damaged header: {got if isinstance(got, dict) else 'listed'}")
    open(p, "wb").write(open(os.path.join(d, "page.tar"), "rb").read()[:2000])
    got = json.loads(node("list", p))
    check(isinstance(got, dict) and "cut short" in got.get("error", ""), f"a cut file: {got if isinstance(got, dict) else 'listed'}")

    # headers that add up but lie: a negative size (walked backwards, the reader never ended), digits that are not
    # octal, a long-name record of megabytes.  Refused, and at once (node would hang on the first)
    def header(name, size_field, typ=b"0"):
        h = bytearray(512)
        h[0:len(name)] = name; h[100:108] = b"0000644\0"; h[124:136] = size_field; h[156:157] = typ; h[257:263] = b"ustar\0"; h[263:265] = b"00"
        h[148:156] = b" " * 8
        h[148:156] = b"%06o\0 " % sum(h)
        return bytes(h)
    for what, raw in (("a negative size", header(b"x", b"-0000001750\0") + bytes(4096)),
                      ("a size that is no octal number", header(b"x", b"0000000089A\0") + bytes(4096)),
                      ("a long name of 2 MB", header(b"././@LongLink", b"%011o\0" % (2 << 20), b"L") + bytes((2 << 20) + 1024))):
        open(p, "wb").write(raw)
        try: got = json.loads(subprocess.run(["node", "-e", JS, os.path.join(ROOT, "web/tar.js"), "list", p], capture_output=True, text=True, timeout=10).stdout)
        except subprocess.TimeoutExpired: got = {"error": "hung"}
        check(isinstance(got, dict) and "damaged" in got.get("error", ""), f"{what}: {got if isinstance(got, dict) else 'listed'}")

    # what `tar -cf f.tar .` makes: every path "./..."; the page finds them without it
    b = io.BytesIO()
    with tarfile.open(fileobj=b, mode="w", format=tarfile.GNU_FORMAT) as tf:
        for k, v in files.items():
            ti = tarfile.TarInfo("./" + k); ti.size = len(v); tf.addfile(ti, io.BytesIO(v))
    open(p, "wb").write(b.getvalue())
    got = json.loads(node("list", p))
    check(isinstance(got, list) and {k: bytes.fromhex(v) for k, v in got} == files, "paths of a tar made in its folder (\"./\") without it")

print(f"FAIL {bad} of {n}" if bad else f"ok   web tar: {n} checks")
sys.exit(1 if bad else 0)
