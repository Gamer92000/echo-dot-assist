"""geo/borders.bin: the countries' borders hassmic finds Home Assistant's home location in (src/hassmic/geo.c), and
this module's reference of the same lookup (tools/geo/build.py's self-check, tests/unit/geo_ref.py).

The file is zlib'd; inside, little-endian:
    "HMGEO1\\n\\0"  u32 scale (units a degree)  u32 rings
    per ring: char cc[2]  u8 flags (1: a hole)  u8 0  i32 min_lon min_lat max_lon max_lat (units)  u32 points  u32 bytes
              then the points as zigzag varints of the difference to the one before (lon, lat; the first from 0, 0)
Rings are closed implicitly.  A point in several outer rings (an enclave: San Marino in Italy, Lesotho in South Africa)
belongs to the smallest by its box; holes (the enclave's outline as the surrounding country's) count only for what is
near.  Nothing found: the nearest
ring within NEAR_MAX_KM (a coast drawn coarser than the house next to it)."""
import math, struct, zlib

MAGIC = b"HMGEO1\n\0"
NEAR_MAX_KM = 50.0              # geo.c GEO_NEAR_MAX_KM
BORDER_KM = 3.0                 # geo.c GEO_BORDER_KM: other countries this close are named too


def zz(v): return v * 2 if v >= 0 else -v * 2 - 1
def unzz(u): return u >> 1 if not u & 1 else -(u >> 1) - 1


GEO_NEAR_MAX = 8                # geo.h: at most this many named near, the first by name


def encode(rings, scale):
    """rings: [(cc, [(lon_units, lat_units), ...], hole)] -> the file's bytes (zlib'd)"""
    out = bytearray(MAGIC + struct.pack("<II", scale, len(rings)))
    for cc, pts, hole in rings:
        body = bytearray(); px = py = 0
        for x, y in pts:
            for v in (zz(x - px), zz(y - py)):
                while v >= 128: body.append(v & 127 | 128); v >>= 7
                body.append(v)
            px, py = x, y
        xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
        out += cc.encode() + bytes([1 if hole else 0, 0]) + struct.pack("<iiiiII", min(xs), min(ys), max(xs), max(ys), len(pts), len(body)) + body
    return zlib.compress(bytes(out), 9)


def decode(blob):
    """-> (scale, [(cc, (minx, miny, maxx, maxy), [(x, y), ...], hole)])"""
    raw = zlib.decompress(blob)
    assert raw[:8] == MAGIC
    scale, n = struct.unpack_from("<II", raw, 8); at = 16; rings = []
    for _ in range(n):
        cc = raw[at:at + 2].decode(); hole = bool(raw[at + 2] & 1); box = struct.unpack_from("<iiii", raw, at + 4); npts, nb = struct.unpack_from("<II", raw, at + 20)
        at += 28; end = at + nb; pts = []; x = y = 0; vals = []
        while at < end:
            v = s = 0
            while True:
                b = raw[at]; at += 1; v |= (b & 127) << s; s += 7
                if not b & 128: break
            vals.append(unzz(v))
        for i in range(0, len(vals), 2): x += vals[i]; y += vals[i + 1]; pts.append((x, y))
        assert len(pts) == npts
        rings.append((cc, box, pts, hole))
    return scale, rings


def inside(pts, x, y):
    c = False; n = len(pts)
    for i in range(n):
        x1, y1 = pts[i]; x2, y2 = pts[i - 1]
        if (y1 > y) != (y2 > y) and x < (x2 - x1) * (y - y1) / (y2 - y1) + x1: c = not c
    return c


def seg_km(lat0, ax, ay, bx, by, px, py, scale):
    """distance of p to segment ab in km, all in units, flat around lat0 (Echo-sized distances)"""
    k = 111.195 / scale; cx = math.cos(math.radians(lat0))
    ax, bx, px = ax * cx, bx * cx, px * cx
    dx, dy = bx - ax, by - ay; L = dx * dx + dy * dy
    t = 0.0 if L == 0 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / L))
    return math.hypot(px - ax - t * dx, py - ay - t * dy) * k


def ring_km(pts, lat, x, y, scale):
    return min(seg_km(lat, *pts[i - 1], *pts[i], x, y, scale) for i in range(len(pts)))


def lookup(data, lat, lon):
    """(country or None, sorted other countries within BORDER_KM) as geo.c's geo_country"""
    scale, rings = data
    x, y = round(lon * scale), round(lat * scale)          # half to even: geo.c's lrint
    best = None
    for cc, (x0, y0, x1, y1), pts, hole in rings:
        if not hole and x0 <= x <= x1 and y0 <= y <= y1 and inside(pts, x, y):
            a = (x1 - x0) * (y1 - y0)
            if best is None or a < best[1]: best = (cc, a)
    near = {}
    for cc, (x0, y0, x1, y1), pts, _ in rings:
        reach = NEAR_MAX_KM if best is None else BORDER_KM
        rx = reach / 111.195 * scale / max(math.cos(math.radians(lat)), 0.01); ry = reach / 111.195 * scale
        if x0 - rx <= x <= x1 + rx and y0 - ry <= y <= y1 + ry:
            d = ring_km(pts, lat, x, y, scale)
            if d <= reach and d < near.get(cc, 1e9): near[cc] = d
    if best is None:
        if not near: return None, []
        cc = min(near, key=lambda c: (near[c], c))
        if near[cc] > NEAR_MAX_KM: return None, []
        return cc, sorted(c for c, d in near.items() if c != cc and d <= BORDER_KM)[:GEO_NEAR_MAX]
    return best[0], sorted(c for c, d in near.items() if c != best[0] and d <= BORDER_KM)[:GEO_NEAR_MAX]
