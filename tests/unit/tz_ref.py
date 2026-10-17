"""tz.c against Python's tzdata: a bionic tzdata file built from it as Android's ZoneCompactor does (header, 52-byte
index, the TZif files as they are), then for every zone's rule (the POSIX TZ string Home Assistant sends,
aioesphomeapi's timezone.py) the zone tz_lookup finds must have that very rule, and be one zone.tab lists; the zone
set is kept when it has it;
a rule no zone has finds none; a file that is not bionic's finds none."""
import os, struct, subprocess, sys, tempfile
from importlib import resources

tz_test = sys.argv[1]
root = resources.files("tzdata") / "zoneinfo"
zones = {}
def walk(d, pre=""):
    for e in d.iterdir():
        if e.is_dir(): walk(e, pre + e.name + "/")
        elif not e.name.endswith((".py", ".tab", ".zi")) and not e.name.startswith(("__", "tzdata")):
            b = e.read_bytes()
            if b[:4] == b"TZif": zones[pre + e.name] = b
walk(root)
names = sorted(zones)
index, data = b"", b""
for n in names:
    index += struct.pack(">40siii", n.encode(), len(data), len(zones[n]), 0)
    data += zones[n]
hdr = 24
zonetab = (root / "zone.tab").read_bytes()                                  # ZoneCompactor appends it as it is
listed = {l.split("\t")[2] for l in zonetab.decode().splitlines() if l and not l.startswith("#")}
blob = struct.pack(">12siii", b"tzdata2026e\0", hdr, hdr + len(index), hdr + len(index) + len(data)) + index + data + zonetab
path = tempfile.mktemp(suffix=".tzdata"); open(path, "wb").write(blob)
rule = lambda n: zones[n].split(b"\n")[-2].decode()

def look(posix, cur=None):
    return subprocess.run([tz_test, path, posix] + ([cur] if cur else []), capture_output=True, text=True, check=True).stdout.strip()

fails = 0
def check(ok, what):
    global fails
    print(("ok   " if ok else "FAIL ") + what)
    fails += not ok

rules = sorted({rule(n) for n in names if rule(n)})
bad = [(r, z) for r in rules if (z := look(r)) == "-" or rule(z) != r]
check(not bad, f"{len(rules)} rules of {len(names)} zones: each finds a zone with that rule {bad[:3]}")
check(look(rule("Europe/Berlin")) == "Europe/Amsterdam", f"Central Europe: the first European zone ({look(rule('Europe/Berlin'))})")
check(look(rule("Europe/Berlin"), "Europe/Berlin") == "Europe/Berlin", "the zone set is kept when it has the rule")
check(look(rule("Europe/Berlin"), "America/Chicago") == "Europe/Amsterdam", "and not when it has another")
check(look(rule("America/Chicago")) == "America/Chicago", f"US Central: an area name ({look(rule('America/Chicago'))})")
aliases = [(n, z) for n in ("Australia/Sydney", "Asia/Kolkata", "Europe/London", "America/New_York") if (z := look(rule(n))) not in listed]
check(not aliases, f"a name zone.tab lists, not an old one (Australia/ACT, Asia/Calcutta) {aliases}")
open(path, "wb").write(blob[:-len(zonetab)])
check(look(rule("Europe/Berlin")) == "Europe/Amsterdam", "without zone.tab: area order alone")
check(look("XYZ5ABC,M3.2.0,M11.1.0") == "-", "a rule no zone has: none")
open(path, "wb").write(b"not tzdata" + blob[10:])
check(look(rule("Europe/Berlin")) == "-", "not bionic's file: none")

# hand-made files: one or two zones, the index and data as tz.c must not trust them
def make(entries, misalign=0):
    idx, dat = b"", b""
    for name, start, blobdata in entries:
        idx += struct.pack(">40siii", name, len(dat) if start is None else start, len(blobdata), 0)
        dat += blobdata
    idx += b"\0" * misalign
    open(path, "wb").write(struct.pack(">12siii", b"tzdata2026e\0", 24, 24 + len(idx), 24 + len(idx) + len(dat)) + idx + dat)
tzif = lambda r: b"TZif2" + b"\0" * 40 + b"\n" + r + b"\n"
make([(b"Europe/Berlin", None, tzif(b"CET-1CEST,M3.5.0,M10.5.0/3"))])
check(look("CET-1CEST,M3.5.0,M10.5.0/3") == "Europe/Berlin", "hand-made file: found")
make([(b"Europe/Berlin", None, tzif(b"CET-1CEST,M3.5.0,M10.5.0/3"))], misalign=4)
check(look("CET-1CEST,M3.5.0,M10.5.0/3") == "-", "an index that is not whole entries: none")
make([(b"Europe/Berlin", 1 << 30, tzif(b"CET-1CEST,M3.5.0,M10.5.0/3"))])
check(look("CET-1CEST,M3.5.0,M10.5.0/3") == "-", "an entry pointing past the data: skipped")
make([(b"A" * 40, None, tzif(b"CET-1CEST,M3.5.0,M10.5.0/3"))])
check(look("CET-1CEST,M3.5.0,M10.5.0/3") == "A" * 40, "a name of all 40 bytes, no NUL: read as 40")
long_rule = b"X" * 200
make([(b"Europe/Long", None, tzif(long_rule))])
check(look(long_rule.decode()) == "-", "a footer longer than tz.c reads: no match")
os.unlink(path)
sys.exit(1 if fails else 0)
