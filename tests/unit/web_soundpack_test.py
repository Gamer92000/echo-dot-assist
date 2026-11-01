#!/usr/bin/env python3
"""A .soundpack file (web/tar.js packWrite, packRead) through node against Python's tarfile: what the page writes is a
tar any computer opens, pack.json first with the name, a WAV per sound unchanged; what a computer packs again is read
back; a tar of something else, or a name that is no string, is refused; names that are no sound's are left out."""
import io, json, os, subprocess, sys, tarfile, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
JS = """
const t = require(process.argv[1]), fs = require('fs'), [mode, file] = process.argv.slice(2);
if (mode === 'write') {
  const { name, files } = JSON.parse(fs.readFileSync(0, 'utf8'));
  fs.writeFileSync(file, Buffer.concat(t.packWrite(name, files.map(([id, x]) => ({ id, bytes: Buffer.from(x, 'hex') })), 1700000000)));
} else {
  const buf = fs.readFileSync(file), read = async (off, len) => new Uint8Array(buf.subarray(off, off + len));
  t.packRead(read, buf.length).then((p) => console.log(JSON.stringify({ name: p.name, files: p.files.map((f) => [f.id, buf.subarray(f.off, f.off + f.size).toString('hex')]) })),
    (e) => console.log(JSON.stringify({ error: e.message })));
}
"""
bad = 0


def check(cond, what):
    global bad
    if not cond: bad += 1; print("FAIL", what)


def node(mode, path, stdin=""):
    r = subprocess.run(["node", "-e", JS, os.path.join(ROOT, "web/tar.js"), mode, path], input=stdin, capture_output=True, text=True)
    if r.returncode: print(r.stderr); sys.exit(1)
    return json.loads(r.stdout) if mode == "read" else None


def tar_of(path, members):
    with tarfile.open(path, "w", format=tarfile.USTAR_FORMAT) as t:
        for name, data in members:
            i = tarfile.TarInfo(name); i.size = len(data); t.addfile(i, io.BytesIO(data))


with tempfile.TemporaryDirectory() as d:
    f = os.path.join(d, "p.soundpack")
    name, wake, ring = 'Bips "ü', bytes(range(256)) * 3, b"\x01" * 1000
    node("write", f, json.dumps({"name": name, "files": [["wake", wake.hex()], ["ring_in", ring.hex()]]}))
    with tarfile.open(f) as t:
        check(t.getnames() == ["pack.json", "wake.wav", "ring_in.wav"], f"a plain tar, pack.json first: {t.getnames()}")
        check(json.load(t.extractfile("pack.json")) == {"hassmic_sounds": 1, "name": name}, "the name in it")
        check(t.extractfile("wake.wav").read() == wake and t.extractfile("ring_in.wav").read() == ring, "the sounds unchanged")
    r = node("read", f)
    check(r == {"name": name, "files": [["wake", wake.hex()], ["ring_in", ring.hex()]]}, f"read back: {str(r)[:100]}")

    head = json.dumps({"hassmic_sounds": 1, "name": "again"}).encode()
    tar_of(f, [("ring_out.wav", b"x"), ("pack.json", head), ("notes.txt", b"y"), ("../evil.wav", b"z"), ("Wake.wav", b"z")])
    r = node("read", f)
    check(r == {"name": "again", "files": [["ring_out", b"x".hex()]]}, f"packed again on a computer: only what can be a sound: {r}")

    for what, members in (("no pack.json", [("wake.wav", b"x")]), ("another JSON", [("pack.json", b'{"hassmic_models":1}')]),
                          ("not JSON", [("pack.json", b"hello")]), ("a name that is no string", [("pack.json", b'{"hassmic_sounds":1,"name":3}')])):
        tar_of(f, members)
        check(node("read", f).get("error") == "not a sound pack", f"refused: {what}")
    open(f, "wb").write(b"\x7fnot a tar" * 200)
    check("error" in node("read", f), "refused: no tar")

print(f"web sound pack: {'FAILED' if bad else 'ok'}")
sys.exit(1 if bad else 0)
