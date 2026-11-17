"""tz.c against Python's tzdata: a bionic tzdata file built from it as Android's ZoneCompactor does (header, 52-byte
index, the TZif files as they are), then for every zone's rule (the POSIX TZ string Home Assistant sends,
aioesphomeapi's timezone.py) the zone tz_lookup finds must have that very rule, and be one zone.tab lists; the zone
set is kept when it has it;
a rule no zone has finds none; a file that is not bionic's finds none."""
import os, struct, subprocess, sys, tempfile
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from tzdata_file import build

tz_test = sys.argv[1]
path = tempfile.mktemp(suffix=".tzdata")
zones, zonetab, blob = build(path)
names = sorted(zones)
listed = {l.split("\t")[2] for l in zonetab.decode().splitlines() if l and not l.startswith("#")}
rule = lambda n: zones[n].split(b"\n")[-2].decode()

def found(posix, cur=None, cc=None):
    """(zone, how) or ("-", None)"""
    out = subprocess.run([tz_test, path, posix, cur or ""] + ([cc] if cc else []), capture_output=True, text=True, check=True).stdout.split()
    return (out[0], out[1]) if len(out) == 2 else ("-", None)
look = lambda posix, cur=None, cc=None: found(posix, cur, cc)[0]

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
# the Wi-Fi's country picks among the zones of a rule: zone.tab's first zone of that country with it
check(found(rule("Europe/Berlin")) == ("Europe/Amsterdam", "guess"), "no country, no zone set: a guess")
check(found(rule("Europe/Berlin"), None, "DE") == ("Europe/Berlin", "country"), "DE: Europe/Berlin, by the country")
check(found(rule("Europe/Berlin"), "Europe/Amsterdam", "DE") == ("Europe/Berlin", "country"), "a zone set in another country gives way")
check(found(rule("Europe/Berlin"), "Europe/Busingen", "DE") == ("Europe/Busingen", "country"), "one of the country's zones set: kept")
check(found(rule("Europe/Berlin"), "Europe/Berlin") == ("Europe/Berlin", "kept"), "no country: the zone set kept")
check(found(rule("Europe/Berlin"), "Europe/Amsterdam") == ("Europe/Amsterdam", "guess"), "the zone set is the guess's own: a guess still")
check(found(rule("Europe/Berlin"), None, "US") == ("Europe/Amsterdam", "guess"), "a country with no zone of that rule: as none")
check(found(rule("Europe/Berlin"), "Europe/Paris", "US") == ("Europe/Paris", "kept"), "and the zone set kept then")
check(look(rule("America/Regina"), None, "MX") == "America/Mexico_City", "CST6 in Mexico: America/Mexico_City")
check(look(rule("America/Regina"), None, "CA") == "America/Regina", "CST6 in Canada: America/Regina")
check(look(rule("Australia/Sydney"), None, "AU").startswith("Australia/"), f"AU with Sydney's rule: an Australia/ zone, not Antarctica/Macquarie ({look(rule('Australia/Sydney'), None, 'AU')})")
check(look(rule("America/Argentina/Buenos_Aires"), None, "AR") == "America/Argentina/Buenos_Aires", "<-03>3 in Argentina")
check(look(rule("America/Sao_Paulo"), None, "BR") in ("America/Belem", "America/Fortaleza", "America/Sao_Paulo"), f"<-03>3 in Brazil: {look(rule('America/Sao_Paulo'), None, 'BR')}")
check(look(rule("Europe/Lisbon"), None, "ES") == "Atlantic/Canary", "WET in Spain: the Canaries")
check(found(rule("Europe/Berlin"), None, "de")[1] == "guess", "a country is two capitals as the element has it")
tab = [l.split("\t") for l in zonetab.decode().splitlines() if l and not l.startswith("#")]
wrong = [(cc, z, got) for cc, _, z, *_ in tab if z in zones and rule(z)
         and not ((got := found(rule(z), None, cc))[1] == "country" and [cc, got[0]] in [[c, n] for c, _, n, *_ in tab])]
check(not wrong, f"every zone.tab country, every rule of its zones: a zone of that country {wrong[:3]}")
open(path, "wb").write(blob[:-len(zonetab)])
check(look(rule("Europe/Berlin")) == "Europe/Amsterdam", "without zone.tab: area order alone")
check(found(rule("Europe/Berlin"), None, "DE") == ("Europe/Amsterdam", "guess"), "without zone.tab: no country either")
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
