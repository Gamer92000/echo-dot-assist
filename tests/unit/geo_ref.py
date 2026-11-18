"""geo.c against tools/geo/geo.py (the same lookup in Python, on the same geo/borders.bin), and places whose country
is known: towns on both sides of borders, enclaves, islands, coasts drawn coarser than the town, the sea."""
import os, random, subprocess, sys
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
sys.path.insert(0, os.path.join(ROOT, "tools", "geo"))
import geo

data = geo.decode(open(os.path.join(ROOT, "geo", "borders.bin"), "rb").read())
fails = 0
def check(ok, what):
    global fails
    print(("ok   " if ok else "FAIL ") + what); fails += not ok

def c_lookup(points):
    out = subprocess.run([sys.argv[1]], input="".join(f"{a!r} {b!r}\n" for a, b in points), capture_output=True, text=True, check=True).stdout.split("\n")
    res = []
    for line in out[:len(points)]:
        if line == "-": res.append((None, []))
        else: cc, near = line.split(" "); res.append((cc, [] if near == "-" else near.split(",")))
    return res

PLACES = [  # (name, lat, lon, country): checked against maps by hand; exactly that country, unless in NEAR_OK
    ("Berlin", 52.52, 13.405, "DE"), ("Cologne", 50.9375, 6.9603, "DE"), ("Aachen", 50.7753, 6.0839, "DE"),
    ("Maastricht", 50.8514, 5.6910, "NL"), ("Amsterdam", 52.3676, 4.9041, "NL"), ("Brussels", 50.8503, 4.3517, "BE"),
    ("Strasbourg", 48.5734, 7.7521, "FR"), ("Kehl", 48.5729, 7.8155, "DE"), ("Basel", 47.5596, 7.5886, "CH"),
    ("Geneva", 46.2044, 6.1432, "CH"), ("Vienna", 48.2082, 16.3738, "AT"), ("Salzburg", 47.8095, 13.0550, "AT"),
    ("Munich", 48.1351, 11.5820, "DE"), ("Konstanz", 47.6779, 9.1732, "DE"), ("Kreuzlingen", 47.6505, 9.1750, "CH"),
    ("Vaduz", 47.1410, 9.5209, "LI"), ("Monaco", 43.7384, 7.4246, "MC"), ("Vatican City", 41.9029, 12.4534, "VA"),
    ("Rome", 41.9028, 12.4964, "IT"), ("San Marino", 43.9356, 12.4473, "SM"), ("Andorra la Vella", 42.5063, 1.5218, "AD"),
    ("Luxembourg", 49.6116, 6.1319, "LU"), ("Paris", 48.8566, 2.3522, "FR"), ("Lisbon", 38.7223, -9.1393, "PT"),
    ("Madrid", 40.4168, -3.7038, "ES"), ("Gibraltar", 36.1408, -5.3536, "GI"), ("Copenhagen", 55.6761, 12.5683, "DK"),
    ("Malmö", 55.6050, 13.0038, "SE"), ("Oslo", 59.9139, 10.7522, "NO"), ("Helsinki", 60.1699, 24.9384, "FI"),
    ("Warsaw", 52.2297, 21.0122, "PL"), ("Frankfurt (Oder)", 52.3471, 14.5506, "DE"), ("Słubice", 52.3503, 14.5600, "PL"),
    ("Bucharest", 44.4268, 26.1025, "RO"), ("Chișinău", 47.0105, 28.8638, "MD"), ("London", 51.5072, -0.1276, "GB"),
    ("Dublin", 53.3498, -6.2603, "IE"), ("Belfast", 54.5973, -5.9301, "GB"), ("Reykjavík", 64.1466, -21.9426, "IS"),
    ("Detroit", 42.3314, -83.0458, "US"), ("Windsor", 42.3149, -83.0364, "CA"), ("Toronto", 43.6532, -79.3832, "CA"),
    ("Tijuana", 32.5149, -117.0382, "MX"), ("San Diego", 32.7157, -117.1611, "US"), ("Mexico City", 19.4326, -99.1332, "MX"),
    ("São Paulo", -23.5505, -46.6333, "BR"), ("Buenos Aires", -34.6037, -58.3816, "AR"), ("Montevideo", -34.9011, -56.1645, "UY"),
    ("Sydney", -33.8688, 151.2093, "AU"), ("Hobart", -42.8821, 147.3272, "AU"), ("Honolulu", 21.3069, -157.8583, "US"),
    ("Tokyo", 35.6762, 139.6503, "JP"), ("Mumbai", 19.0760, 72.8777, "IN"), ("Santa Cruz de Tenerife", 28.4636, -16.2518, "ES"),
    ("Funchal", 32.6669, -16.9241, "PT"), ("Den Helder", 52.9563, 4.7601, "NL"), ("Sylt, Westerland", 54.9079, 8.3101, "DE"),
    ("Baarle-Nassau", 51.4447, 4.9306, "NL"),
]
PLACES += [  # enclaves and exclaves: their borders are land borders (tools/geo/build.py)
    ("Büsingen (DE in CH)", 47.6967, 8.6908, "DE"), ("Llívia (ES in FR)", 42.4646, 1.9822, "ES"),
    ("Campione d'Italia (IT in CH)", 45.9686, 8.9708, "IT"), ("Teyateyaneng (LS in ZA)", -29.15, 27.75, "LS"),
    ("Serravalle (SM in IT)", 43.9686, 12.4806, "SM"), ("St Peter's Basilica (VA in IT)", 41.9022, 12.4539, "VA"),
]
# border towns: the borders are drawn to ~500 m, so the neighbour may come out, with the right one named near (clock.c
# lets the Wi-Fi's country decide); everything else must be exactly right
NEAR_OK = {"Gibraltar", "Słubice", "Kehl", "Basel", "Konstanz", "Kreuzlingen", "Frankfurt (Oder)", "Detroit", "Windsor",
           "Tijuana", "Monaco", "Vaduz", "Baarle-Nassau", "St Peter's Basilica (VA in IT)", "Campione d'Italia (IT in CH)",
           "Büsingen (DE in CH)", "Serravalle (SM in IT)"}
SEA = [("Gulf of Guinea (0, 0: Home Assistant's location when none was set)", 0.0, 0.0), ("North Atlantic", 45.0, -30.0)]

c = c_lookup([(a, b) for _, a, b, _ in PLACES])
for (name, lat, lon, want), (got, near) in zip(PLACES, c):
    ok = got == want or (name in NEAR_OK and want in near)
    check(ok, f"{name}: {want}{' or named near' if name in NEAR_OK else ''} ({got}{', near ' + ','.join(near) if near else ''})")
c = c_lookup([(a, b) for _, a, b in SEA])
for (name, *_), (got, _) in zip(SEA, c):
    check(got is None, f"{name}: none ({got})")

# geo.c is geo.py: random points over the world's land and sea, and many near Europe's borders
rnd = random.Random(7)
pts = [(rnd.uniform(-60, 75), rnd.uniform(-180, 180)) for _ in range(300)] + [(rnd.uniform(43, 56), rnd.uniform(-5, 25)) for _ in range(700)]
# and on the half of a unit (0.00025°: five decimals ending in 25 or 75), where rounding half up and half to even differ
pts += [(round(rnd.uniform(43, 56) * 2000) / 2000 + 0.00025, round(rnd.uniform(-5, 25) * 2000) / 2000 + 0.00025) for _ in range(500)]
c = c_lookup(pts)
diff = [(p, g, geo.lookup(data, *p)) for p, g in zip(pts, c) if g != geo.lookup(data, *p)]
check(not diff, f"{len(pts)} random points: geo.c answers as geo.py {diff[:3]}")
check(c_lookup([(91.0, 0.0), (0.0, 181.0), (float("nan"), 0.0)]) == [(None, [])] * 3, "out of range, NaN: none")
# degrees to units as geo.py rounds them, half to even: a house given to 5 decimals ending in 25 or 75 is on the half
scale = data[0]
degs = [round(rnd.uniform(-180, 180) * 2000) / 2000 + 0.00025 for _ in range(2000)] + [-0.00025, 0.00025, 0.00075, 179.99975]
out = subprocess.run([sys.argv[1], "-u", str(scale)], input="".join(f"{d!r}\n" for d in degs), capture_output=True, text=True, check=True).stdout.split()
bad = [(d, int(o), round(d * scale)) for d, o in zip(degs, out) if int(o) != round(d * scale)]
check(not bad and len(out) == len(degs), f"{len(degs)} coordinates on the half of a unit: geo.c's units are geo.py's {bad[:3]}")
sys.exit(1 if fails else 0)
