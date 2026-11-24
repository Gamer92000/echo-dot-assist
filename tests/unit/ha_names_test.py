#!/usr/bin/env python3
"""What Home Assistant makes of an entity's name: it keeps the entity by it (aioesphomeapi build_unique_id, version 3:
"<mac>/0/<type>/<name>"), and makes its entity id from it as the entity is first made, <domain>.slugify("<area>
<device's name> <entity's name>") (Home Assistant 2026.10.1 entity_registry, the area where the device has
one; slugify works part by part, so a common prefix changes no collision; slugify: unidecode, lower case, every run of
anything but a-z0-9 one "_").  An action's service is esphome.<node>_<object id>.

Home Assistant removes the registry entry of an entity that is no longer listed (entry_data.async_remove_entities: a
feature switched off, a plugin switched off or removed), and the next entity whose name slugifies alike takes its
entity id; the first gets "_2" when it comes back, and automations and history follow the other.  "<plugin's name> ·
<label>" was such a name: "Last · heard" is sensor.<node>_last_heard, hassmic's own "Last heard" (K1 of a review of
92ac2e1).  So a plugin's entity is named "Plugin <length of its id> <id>: <label>" (plugin.c ha_name), and this holds:

- no name of hassmic's own slugifies as "plugin_<digits>" or "plugin_<digits>_...": every string literal of
  proto_esphome.c that is a name (field 3 of a listing, and the arrays and run-time formats it names: "Light ring LED
  %d" for every LED), and, wider, no string literal of proto_esphome.c and settings.c at all;
- a plugin's slug says its id: "plugin_<n>_" and then the id's n characters (ids are [a-z0-9-] without "--" or a "-"
  first or last, so the id's slug is as long as the id), so two plugins never make one entity id, whatever their
  labels ("weather" with "x temp", "weather-x" with "temp");
- object ids are "p<length of its id>_<id>_<name>" (plugin.c ha_object_id), and none of hassmic's own begins with "p"
  and a digit.

Slugified with Home Assistant's own slugify where build/ha-venv has it, else with the copy below (which, with Home
Assistant there, is held to it on every name)."""
import html.entities, json, os, random, re, subprocess, sys, unicodedata

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HA_PY = os.path.join(ROOT, "build/ha-venv/bin/python")
fails = 0


def check(ok, what, extra=""):
    global fails
    print(("ok   " if ok else "FAIL ") + what + ("" if ok else f"\n     {extra}"))
    fails += 0 if ok else 1


try:
    from unidecode import unidecode
except ImportError:
    try:
        from text_unidecode import unidecode
    except ImportError:
        unidecode = None        # (then only what is ASCII is slugified as Home Assistant does it: a label's prefix is)


def slug_copy(text):
    """homeassistant.util.slugify: python-slugify's legacy pipeline with separator "_", "unknown" for nothing"""
    if not text: return ""
    t = re.sub(r"[']+", "-", text)
    t = unicodedata.normalize("NFKD", t)
    if not t.isascii(): t = unidecode(t) if unidecode else t.encode("ascii", "ignore").decode()
    t = re.sub(r"&(%s);" % "|".join(html.entities.name2codepoint), lambda m: chr(html.entities.name2codepoint[m.group(1)]), t)
    t = re.sub(r"&#(\d+);", lambda m: chr(int(m.group(1))), t)
    t = re.sub(r"&#x([\da-fA-F]+);", lambda m: chr(int(m.group(1), 16)), t)
    t = unicodedata.normalize("NFKD", t).lower()
    t = re.sub(r"(?<=\d),(?=\d)", "", t)
    t = re.sub(r"[^-a-zA-Z0-9]+", "-", t)
    t = re.sub(r"-{2,}", "-", t).strip("-").replace("-", "_")
    return t or "unknown"


def slug_ha(texts):
    """Home Assistant's own, None where build/ha-venv is not there"""
    if not os.access(HA_PY, os.X_OK): return None
    r = subprocess.run([HA_PY, "-c", "import json, sys\nfrom homeassistant.util import slugify\n"
                        "print(json.dumps([slugify(t) for t in json.load(sys.stdin)]))"],
                       input=json.dumps(texts), capture_output=True, text=True)
    if r.returncode: print("     (build/ha-venv's slugify did not run: " + r.stderr.strip()[-200:] + ")"); return None
    return json.loads(r.stdout)


def literals(src):
    src = re.sub(r'/\*.*?\*/', ' ', src, flags=re.S)
    src = re.sub(r'//[^\n]*', ' ', src)
    return re.findall(r'"((?:[^"\\\n]|\\.)*)"', src)


def cstr(lit):
    """a C literal's text (its escapes, UTF-8 as written or as \\x bytes)"""
    try: return bytes(lit, "utf-8").decode("unicode_escape").encode("latin-1").decode("utf-8")
    except (UnicodeError, ValueError): return lit


# ---- every name of hassmic's own
esp = open(os.path.join(ROOT, "src/hassmic/proto_esphome.c"), encoding="utf-8").read()
code = re.sub(r'/\*.*?\*/', ' ', esp, flags=re.S)
own, unread = [], []
for m in re.finditer(r'pb_str\(&\w+,\s*3,\s*([^;]*?)\);', code):
    e = m.group(1).strip()
    lits = re.findall(r'"((?:[^"\\\n]|\\.)*)"', e)
    if lits: own.append(cstr("".join(lits))); continue          # ("hassmic " VERSION: the literal's part)
    a = re.fullmatch(r'(\w+)\[\w+\]', e)
    if a and (d := re.search(r'\b' + a.group(1) + r'\[[^\]]*\]\s*=\s*\{([^}]*)\}', code)):
        own += [cstr(x) for x in re.findall(r'"((?:[^"\\\n]|\\.)*)"', d.group(1))]; continue
    f = re.fullmatch(r'\w+', e) and re.search(r'snprintf\(' + e + r',\s*sizeof ' + e + r',\s*"((?:[^"\\\n]|\\.)*)"', code)
    if f:
        fmt = cstr(f.group(1))
        check(re.sub(r'%d', '', fmt).count('%') == 0, f"a run-time name's form the test can fill: {fmt!r}")
        own += [fmt.replace("%d", str(i)) for i in range(0, 100)]; continue
    # not a name: the plugin's own (held below), a wake word's language (field 3 of a nested message), the MAC
    if e in ("e->name", "w[i].lang", "mac()"): continue
    unread.append(e)
check(not unread, "every name of hassmic's own read (field 3 of what it sends: a literal, an array of them, a run-time form)", unread)
check(len(own) > 150 and "Last heard" in own and "Light ring LED 12" in own and "Assistant volume" in own and "Equalizer bass" in own
      and "Media Player" in own and "Plugin" in own, f"its names ({len(own)}) include the ones a plugin's once took, the run-time ones, the arrays'")
wide = []
for f in ("src/hassmic/proto_esphome.c", "src/hassmic/settings.c"):
    lits = literals(open(os.path.join(ROOT, f), encoding="utf-8", errors="surrogateescape").read())
    check(len(lits) > 100, f"{f}: its strings read ({len(lits)})")
    wide += [cstr(l) for l in lits]
    pid = [l for l in lits if re.fullmatch(r'p\d+_[a-z0-9_]*', l)]
    check(not pid, f"{f}: no object id of hassmic's own is a plugin's (p<digits>_...)", pid)

# ---- the plugins' form, as plugin.c writes it
plug = open(os.path.join(ROOT, "src/hassmic/plugin.c"), encoding="utf-8").read()
check('snprintf(out, cap, "Plugin %zu %s: %s", strlen(sp->m.id), sp->m.id, sp->m.entities[k].label)' in plug,
      "plugin.c names a plugin's entity \"Plugin <length of the id> <id>: <label>\" (ha_name)")
check('snprintf(out, cap, "p%zu_", strlen(id))' in plug, "plugin.c's object ids begin with \"p\" and the id's length")
check("ha_hidden" not in plug, "no dedupe of names between plugins left (the id in every name makes them two)")


def pname(pid, label): return f"Plugin {len(pid)} {pid}: {label}"


ID_RE = re.compile(r'[a-z0-9](?:[a-z0-9]|-(?=[a-z0-9])){2,23}')
rnd = random.Random(92)
def rid():
    while True:
        s = "".join(rnd.choice("ab1-") for _ in range(rnd.randint(3, 12)))
        if ID_RE.fullmatch(s): return s
LABEL_BITS = ["x", "temp", "9", "12", "plugin", "Plugin 3", "Last", "heard", "·", " ", "-", ":", "Ü", "ß", "&amp;", "'", "1,2", "_", "Ω", "😀", "a b", "weather-x"]
def rlabel(): return "".join(rnd.choice(LABEL_BITS) for _ in range(rnd.randint(1, 5)))

pairs = [("weather", "x temp"), ("weather-x", "temp"), ("abc", "3 xy 8 temp"), ("abc-3-xy", "temp"), ("abc", "xy 8 temp"),
         ("last", "heard"), ("wake", "sound"), ("drop-in", "result"), ("bluetooth", "speaker"), ("testmusic", "Position"),
         ("abc", "😀"), ("abc", "·"), ("abd", "")]
pairs += [(rid(), rlabel()) for _ in range(3000)]
pnames = [pname(i, l) for i, l in pairs]
devs = ["Echo Dot 3 5695c4", "Echo Show 5 fdd111"]
texts = own + wide + pnames + [d + " " + n for d in devs for n in own + pnames]
ha = slug_ha(texts)
mine = [slug_copy(t) for t in texts]
if ha is None:
    print("     (no build/ha-venv: Home Assistant's slugify stood in for by the copy)")
    sl = mine
else:
    held = [(t, a, b) for t, a, b in zip(texts, ha, mine) if unidecode or t.isascii()]
    diff = [x for x in held if x[1] != x[2]]
    check(not diff and len(held) > 5000, f"the copy of slugify is Home Assistant's on {len(held)} names"
          + ("" if unidecode else " (those in ASCII: no unidecode here)"), diff[:5])
    sl = ha
S = dict(zip(texts, sl))

PLUG = re.compile(r'plugin_\d+(_|$)')
bad = [n for n in own if PLUG.match(S[n])]
check(not bad, "no name of hassmic's own slugifies as a plugin's (\"plugin_<digits>...\")", bad)
bad = [n for n in wide if PLUG.match(S[n])]
check(not bad, "... nor any string of proto_esphome.c or settings.c", bad)
check(all(PLUG.match(S[n]) for n in pnames), "every plugin's name slugifies as \"plugin_<digits>...\"")


def decode(s):
    m = re.match(r'plugin_(\d+)_', s)
    if not m: return None
    n, rest = int(m.group(1)), s[m.end():]
    if len(rest) < n or (len(rest) > n and rest[n] != "_"): return None
    return rest[:n]


check(all(decode(S[pname(i, l)]) == i.replace("-", "_") for i, l in pairs), "the id read back from every plugin's slug: two plugins' never alike")
for d in devs:
    own_ids = {S[d + " " + n] for n in own}
    hit = [n for n in pnames if S[d + " " + n] in own_ids]
    check(not hit, f"with the device \"{d}\": no plugin's entity id is one of hassmic's own", hit[:5])
    by = {}
    for (i, l), n in zip(pairs, pnames): by.setdefault(S[d + " " + n], set()).add(i)
    two = {k: v for k, v in by.items() if len(v) > 1}
    check(not two, f"with the device \"{d}\": no entity id two plugins'", list(two.items())[:3])
check(S["Echo Dot 3 5695c4 " + pname("testmusic", "Position")] == "echo_dot_3_5695c4_plugin_9_testmusic_position"
      and S["Echo Dot 3 5695c4 Last heard"] == "echo_dot_3_5695c4_last_heard"
      and S["Echo Dot 3 5695c4 " + pname("last", "heard")] == "echo_dot_3_5695c4_plugin_4_last_heard",
      "K1: \"Last heard\" and a plugin \"last\" with \"heard\": two entity ids (the example in the docs as well)")
print("ha_names: " + ("FAILED" if fails else "all passed") + f" (slugify: {'Home Assistant' if ha is not None else 'the copy'})")
sys.exit(1 if fails else 0)
