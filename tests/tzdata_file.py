"""A bionic tzdata file built from Python's tzdata as Android's ZoneCompactor does it (header, 52-byte index, the TZif
files as they are, zone.tab appended): for tz.c (tests/unit/tz_ref.py) and hassmic's HASSMIC_TZDATA in tests."""
import struct
from importlib import resources


def build(path):
    """writes it; (zones {name: TZif bytes}, zone.tab bytes, the whole file)"""
    root = resources.files("tzdata") / "zoneinfo"
    zones = {}
    def walk(d, pre=""):
        for e in d.iterdir():
            if e.is_dir(): walk(e, pre + e.name + "/")
            elif not e.name.endswith((".py", ".tab", ".zi")) and not e.name.startswith(("__", "tzdata")):
                b = e.read_bytes()
                if b[:4] == b"TZif": zones[pre + e.name] = b
    walk(root)
    index, data = b"", b""
    for n in sorted(zones):
        index += struct.pack(">40siii", n.encode(), len(data), len(zones[n]), 0)
        data += zones[n]
    hdr = 24
    zonetab = (root / "zone.tab").read_bytes()
    blob = struct.pack(">12siii", b"tzdata2026e\0", hdr, hdr + len(index), hdr + len(index) + len(data)) + index + data + zonetab
    open(path, "wb").write(blob)
    return zones, zonetab, blob
