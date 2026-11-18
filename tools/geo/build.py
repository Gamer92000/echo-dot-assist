#!/usr/bin/env python3
"""geo/borders.bin from Natural Earth's countries (1:10m, public domain; geo/README.md).

    tools/geo/build.py <ne_10m_admin_0_countries.geojson> geo/borders.bin

Natural Earth v5.1.2: https://raw.githubusercontent.com/nvkelso/natural-earth-vector/v5.1.2/geojson/ne_10m_admin_0_countries.geojson

What hassmic needs is the country a house is in, from Home Assistant's home location (src/hassmic/geo.c): land borders
matter, coasts hardly (a house off a coarse coast takes the nearest country).  So:
  - a land border (a vertex two countries share) and every coast within ~20 km of another country (Detroit and
    Windsor, Malmö and Copenhagen: water between them, coast to both) simplified to BORDER_TOL (Douglas-Peucker), other
    coasts to COAST_TOL;
  - holes that are an enclave's outline kept as holes (for what is near, not for what contains), other holes (lakes)
    left out; an enclave's border is a land border;
  - islands under MIN_AREA with no land border left out, except each country's largest: a house there takes the
    nearest ring; a ring whole where simplifying would leave less than a square of it (Vatican City);
  - points in 1/SCALE degree.
Then the self-check: the simplified file against the full borders on random points of Europe, and on points near
every land border, enclaves' included (tests/unit/geo_ref.py has places whose country is known)."""
import collections, json, math, os, random, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import geo

BORDER_TOL = 0.005      # degrees: ~550 m north-south, ~350 m east-west at 50° N
COAST_TOL = 0.1
NEAR_FOREIGN = 0.2      # degrees: grid cell; a coast vertex with another country in its cell or a neighbour's is "near"
MIN_AREA = 0.003        # square degrees: ~20-35 km² in Europe
SMALL_AREA = 0.01       # rings with a land border under this (Llívia 12.8 km²) drawn to SMALL_TOL
SMALL_TOL = 0.0005
SCALE = 2000            # 0.0005° (~55 m)

# Natural Earth's ISO_A2_EH, else these (its own "-99": no ISO code of their own); the rest of "-99" (disputed reefs,
# Bir Tawil, Siachen, the Southern Patagonian Ice Field) left out
FIXES = {"ESB": "CY", "WSB": "CY", "CYN": "CY", "CNM": "CY", "SOL": "SO", "USG": "CU"}


def dp(pts, tol):
    n = len(pts)
    keep = np.zeros(n, bool)
    if n < 3: keep[:] = True; return keep
    keep[0] = keep[-1] = True
    st = [(0, n - 1)]
    while st:
        a, b = st.pop()
        if b <= a + 1: continue
        p, q = pts[a], pts[b]; seg = pts[a + 1:b]; v = q - p; L = math.hypot(*v)
        d = np.abs(v[0] * (seg[:, 1] - p[1]) - v[1] * (seg[:, 0] - p[0])) / L if L > 0 else np.hypot(*(seg - p).T)
        i = int(np.argmax(d))
        if d[i] > tol: k = a + 1 + i; keep[k] = True; st += [(a, k), (k, b)]
    return keep


def quant(pts):
    """in 1/SCALE degree, without a point that repeats the one before"""
    q = [(int(round(x * SCALE)), int(round(y * SCALE))) for x, y in pts]
    return [p for i, p in enumerate(q) if p != q[i - 1]]


def area(r): x, y = r[:, 0], r[:, 1]; return 0.5 * abs(np.dot(x, np.roll(y, 1)) - np.dot(y, np.roll(x, 1)))


def load(path):
    out = []
    for f in json.load(open(path))["features"]:
        p = f["properties"]; cc = p.get("ISO_A2_EH")
        cc = FIXES.get(p.get("ADM0_A3"), cc)
        if not cc or len(cc) != 2 or not cc.isalpha(): continue
        g = f["geometry"]; polys = g["coordinates"] if g["type"] == "MultiPolygon" else [g["coordinates"]]
        out.append((cc.upper(), [(np.array(ring[:-1], float), k > 0) for poly in polys for k, ring in enumerate(poly)]))
    return out


def simplify(feats):
    owners = collections.defaultdict(set)
    for fi, (_, rings) in enumerate(feats):
        for r, _ in rings:
            for x, y in r: owners[(x, y)].add(fi)
    cells = collections.defaultdict(set)
    for (x, y), o in owners.items(): cells[(math.floor(x / NEAR_FOREIGN), math.floor(y / NEAR_FOREIGN))] |= o
    def near(fi, x, y):
        cx, cy = math.floor(x / NEAR_FOREIGN), math.floor(y / NEAR_FOREIGN)
        return any(cells.get((cx + i, cy + j), set()) - {fi} for i in (-1, 0, 1) for j in (-1, 0, 1))
    out = []
    for fi, (cc, rings) in enumerate(feats):
        big = max(area(r) for r, hole in rings if not hole)
        for r, hole in rings:
            shared = np.array([len(owners[(x, y)]) > 1 for x, y in r])
            # an island under MIN_AREA goes (each country's largest stays); never a ring with a land border (an enclave,
            # an exclave: Llívia, Baarle), nor a hole that is one's outline (other holes: lakes)
            if not shared.any() and (hole or (area(r) < MIN_AREA and area(r) < big)): continue
            fine = shared | np.array([near(fi, x, y) for x, y in r])
            small = area(r) < SMALL_AREA and shared.any()       # an enclave, an exclave: its shape, not a triangle
            keep = np.zeros(len(r), bool)
            cut = np.flatnonzero(np.diff(fine.astype(int))) + 1
            for s, e in zip(np.concatenate([[0], cut]), np.concatenate([cut, [len(r)]])):
                e1 = min(e + 1, len(r))                 # each run with the first point of the next: no gap between them
                keep[s:e1] |= dp(r[s:e1], (SMALL_TOL if small else BORDER_TOL) if fine[s] else COAST_TOL)
            q = quant(r[keep])
            if len(q) < 4: q = quant(r[dp(r, BORDER_TOL)])     # a small island at the border's tolerance, not a sliver
            if len(q) < 4: q = quant(r)                         # a microstate, an enclave whole (Vatican City: 0.44 km²)
            if len(q) >= 3: out.append((cc, q, hole))
    return out


def check(full, data):
    """the full borders against the file: random points of Europe, and points 0.5-6 km from land border vertices"""
    rings = [(cc, r) for cc, rs in full for r, hole in rs if not hole]
    holes = [(cc, r) for cc, rs in full for r, hole in rs if hole]
    boxes = [(r[:, 0].min(), r[:, 1].min(), r[:, 0].max(), r[:, 1].max()) for _, r in rings]
    def truth(lat, lon):
        hit = []
        for (cc, r), (x0, y0, x1, y1) in zip(rings, boxes):
            if x0 <= lon <= x1 and y0 <= lat <= y1:
                x1s, y1s = r[:, 0], r[:, 1]; x2s, y2s = np.roll(x1s, 1), np.roll(y1s, 1)
                m = (y1s > lat) != (y2s > lat)
                xs = (x2s[m] - x1s[m]) * (lat - y1s[m]) / (y2s[m] - y1s[m]) + x1s[m]
                if np.count_nonzero(lon < xs) % 2: hit.append(((x1 - x0) * (y1 - y0), cc))
        return min(hit)[1] if hit else None
    rnd = random.Random(1); n = wrong = 0; far = []
    while n < 1500:
        lat, lon = rnd.uniform(36, 60), rnd.uniform(-10, 30)
        t = truth(lat, lon)
        if t is None: continue
        got, others = geo.lookup(data, lat, lon); n += 1
        if got != t and t not in others: wrong += 1; far.append((lat, lon, t, got))
    print(f"self-check: {n} land points in Europe, {wrong} in another country than the full borders and not named near it")
    # and where it counts: 0.5-6 km from a land border vertex (two countries share it), anywhere in the world
    shared = collections.Counter((x, y) for _, r in rings + holes for x, y in r)
    verts = [p for p, k in shared.items() if k > 1]
    n2 = amb = 0; bad = []
    while n2 < 1500:
        x, y = rnd.choice(verts); d = rnd.uniform(0.5, 6) / 111.195; a = rnd.uniform(0, 2 * math.pi)
        lat, lon = y + d * math.sin(a), x + d * math.cos(a) / math.cos(math.radians(y))
        t = truth(lat, lon)
        if t is None: continue
        got, others = geo.lookup(data, lat, lon); n2 += 1
        if got != t and t in others: amb += 1
        elif got != t: bad.append((round(lat, 4), round(lon, 4), t, got, round(d * 111.195, 1)))
    print(f"self-check: {n2} land points 0.5-6 km from a land border: {n2 - amb - len(bad)} right, {amb} wrong but the right"
          f" one named within {geo.BORDER_KM:g} km (the Wi-Fi's country decides), {len(bad)} wrong")
    for f in (far + bad)[:10]: print("   ", f)
    return wrong + len(bad)


def main():
    feats = load(sys.argv[1])
    rings = simplify(feats)
    blob = geo.encode(rings, SCALE)
    data = geo.decode(blob)
    print(f"{len(rings)} rings ({sum(1 for *_, h in rings if h)} holes), {sum(len(p) for _, p, _ in rings)} points, {len(blob)} bytes")
    if check(feats, data): sys.exit("the file disagrees with the full borders")
    open(sys.argv[2], "wb").write(blob)


if __name__ == "__main__":
    main()
