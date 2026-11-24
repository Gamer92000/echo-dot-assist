#!/usr/bin/env python3
"""Packages for hassmic plugins (docs/plugins.md 5.1, 5.8), Python's standard library only.

  plugcheck.py keygen SECRET PUBLIC                 an author's signing key pair (keep SECRET to yourself)
  plugcheck.py elf PROGRAM [--arch arm|x86_64]      the binary rules a file shows: static, no interpreter, a stack
                                                    that is marked and not executable, no writable+executable segment
  plugcheck.py pack MANIFEST PROGRAM SECRET OUT.hmp [--license FILE] [--listing INDEX_SECRET] [--arch ...]
        checks the program, writes its digest and the author's key into the manifest, signs it, and makes the
        package: a sideload as it is.  (--listing adds an official listing in the same step: tests only)
  plugcheck.py requires MANIFEST                    the "requires" a manifest of its capabilities starts from: core
        and the scope of each capability, at version 1 (sdk/PROTOCOL.md "Versions": raise one where the plugin
        uses what a later version of that scope brought)
  plugcheck.py list PACKAGE.hmp INDEX_SECRET OUT.hmp [--tier official|community] [--serial N] [--arch ...]
        what the plugin index's build adds to a package it lists (hassmic-plugins' tools/index.py, docs/plugins.md
        5.3): the package checked, and given a listing signed by the index key.  Here for hassmic's tests and a
        fork's own index; nothing is built and nothing of the package is run
  plugcheck.py show PACKAGE.hmp [--index-pub KEY]   what is in a package, and whether its signatures hold

A package is a plain tar of manifest.json, manifest.sig, plugin, LICENSE and, for a plugin the index lists,
listing.sig.  Signatures are hassmic's (scripts/otatool.py: Monocypher's EdDSA with BLAKE2b, not RFC 8032's
Ed25519), each over a text that starts with its purpose:
  manifest.sig   over "hassmic plugin manifest 1\\n" + manifest.json's bytes, by the author
  listing.sig    its hex on the first line, then the text it signs, by the index key:
                 "hassmic plugin listing 2\\nid <id>\\nversion <n>\\ntier official|community\\nmanifest <digest>\\nserial <n>\\n"
                 (2: the first form was the release key's, in hassmic's own releases; no Echo takes it any more)
hassmic checks all of it again on the Echo (src/hassmic/plugin_store.c); this is for finding out before."""
import hashlib, io, json, os, re, struct, sys, tarfile

_here = os.path.dirname(os.path.abspath(__file__))           # otatool.py: beside this file in a copy of the SDK taken
sys.path[:0] = [_here, os.path.join(os.path.dirname(_here), "scripts")]    # out of hassmic's tree, else hassmic's scripts/
import otatool                                               # the signatures

SIGN_MANIFEST, SIGN_LISTING = b"hassmic plugin manifest 1\n", b"hassmic plugin listing 2\n"
TIERS = ("official", "community")
PACKAGE_MAX = 4 << 20
# a package's members and what each may hold, as the Echo takes them (plugin_store.c)
MEMBERS = {"manifest.json": 32768, "manifest.sig": 64, "plugin": PACKAGE_MAX, "LICENSE": 65536, "listing.sig": 1024}
# as the Echo's ps_id_ok (plugin_store.c): no hyphen at either end and none twice in a row.  A package's file is
# <id>-<version>.hmp, and "abc--1.hmp" was version 1 of "abc-" as well as of nothing; the old pattern here packed
# and signed such an id, and every Echo then refused the package
ID = re.compile(r"(?=.{3,24}\Z)[a-z0-9]+(?:-[a-z0-9]+)*")
ARCH = {"arm": (1, 40, "32-bit ARM"), "x86_64": (2, 62, "x86_64 (the PC build's test plugins)")}   # EI_CLASS, e_machine
# the capabilities, each a scope of the plugin protocol (sdk/messages.def, CAP_*), and core beside them
CAPS = ("storage", "files", "state", "log.read", "log.read.full", "net.server", "net.lan", "net.internet", "net.udp", "net.discover", "audio.music",
        "audio.volume", "call", "wake", "request.audio", "request.text", "ha.entities", "ha.actions", "button")
SCOPE = re.compile(r"[a-z0-9._]{1,24}")


def requires_for(capabilities):
    """the "requires" of a manifest of CAPABILITIES (its object): core and the scope of each, at version 1, in their
    order: what a plugin that uses nothing a later version brought names"""
    return dict([("core", 1)] + [(c, 1) for c in capabilities if c in CAPS])


def requires_check(m):
    """None where M's "requires" is what the Echo takes of its form (plugin_store.c requires_parse): an object of
    scope names and whole versions from 1, core among them, exactly core and the scope of each capability; else why.
    Which versions there are is each hassmic's to say (a version above its own: "needs a newer hassmic")"""
    r, caps = m.get("requires"), m.get("capabilities") if isinstance(m.get("capabilities"), dict) else {}
    if not isinstance(r, dict):
        return f'the manifest has no "requires": the least version of each scope it speaks, e.g. {json.dumps(requires_for(caps))}'
    for k, v in r.items():
        if not SCOPE.fullmatch(str(k)):
            return f'"requires": a scope\'s name is 1 to 24 of a-z, 0-9, . and _ ({str(k)[:30]!r})'
        if type(v) is not int or not 1 <= v <= 65535:
            return f'"requires": "{k}": a version, a whole number from 1'
    if "core" not in r:
        return '"requires" names no version of core, which every plugin speaks'
    for c in caps:
        if c in CAPS and c not in r:
            return f'capability "{c}": its scope is not in "requires"'
    for k in r:
        if k != "core" and k in CAPS and k not in caps:
            return f'"requires" names {k}, which the manifest does not ask for'
    return None


def die(msg):
    print(f"plugcheck: {msg}", file=sys.stderr)
    sys.exit(1)


def elf_check(b, arch="arm"):
    """None when the program keeps the rules, else what is wrong.  The same checks as ps_elf_check."""
    cls, machine, what = ARCH[arch]
    if len(b) < 52 or b[:4] != b"\x7fELF" or b[4] != cls or b[5] != 1:
        return f"not an ELF for {what}"
    if cls == 1:
        e_type, e_machine = struct.unpack_from("<HH", b, 16)
        e_phoff, = struct.unpack_from("<I", b, 28)
        e_flags, = struct.unpack_from("<I", b, 36)
        e_phentsize, e_phnum = struct.unpack_from("<HH", b, 42)
        ph = lambda o: (struct.unpack_from("<I", b, o)[0], struct.unpack_from("<I", b, o + 24)[0])      # p_type, p_flags
        size = 32
    else:
        e_type, e_machine = struct.unpack_from("<HH", b, 16)
        e_phoff, = struct.unpack_from("<Q", b, 32)
        e_flags, = struct.unpack_from("<I", b, 48)
        e_phentsize, e_phnum = struct.unpack_from("<HH", b, 54)
        ph = lambda o: struct.unpack_from("<II", b, o)
        size = 56
    if e_machine != machine or e_type not in (2, 3):
        return f"not an executable for {what}"
    if arch == "arm" and e_flags & 0xFF000000 != 0x05000000:
        return "not EABI5"
    if e_phentsize != size or not e_phnum or e_phoff + e_phnum * size > len(b):
        return "its program headers are broken"
    stack = False
    for i in range(e_phnum):
        p_type, p_flags = ph(e_phoff + i * size)
        if p_type == 3:
            return "not static: it names an interpreter, and a plugin can open no library"
        if p_type == 0x6474E551:
            stack = True
            if p_flags & 1:
                return "it asks for an executable stack"
        if p_type == 1 and p_flags & 2 and p_flags & 1:
            return "a segment is writable and executable"
    if not stack:
        return "its stack is not marked as not executable (no PT_GNU_STACK): link with -z noexecstack"
    for needle, why in ((b"/dev/urandom", "it names /dev/urandom, which is not there: use getrandom (hm_random); libc++'s std::random_device aborts"),
                        (b"/system/bin/linker", "it looks like a bionic program: static bionic does not start in the sandbox")):
        if needle in b:
            print(f"plugcheck: note: {why}", file=sys.stderr)
    return None


def no_twice(pairs):
    """json.loads' object_pairs_hook: a key twice is no manifest (which of the two counts is each reader's own)"""
    d = {}
    for k, v in pairs:
        if k in d:
            raise ValueError(f'"{k}" is there twice')
        d[k] = v
    return d


def named(m):
    """id and version are what the Echo takes (ps_manifest_parse), and go into texts that get signed: a version that
    was a string with lines of its own would put them into the index key's listing"""
    if not isinstance(m, dict) or not isinstance(m.get("id"), str) or not ID.fullmatch(m["id"]):
        die("the manifest's id: 3 to 24 lower case letters, digits and - (none first, last or twice in a row)")
    if type(m.get("version")) is not int or not 1 <= m["version"] <= 1000000000:
        die("the manifest's version: a whole number from 1")


def listing_text(m, mj, tier="official", serial=0):
    """what a listing signs: the plugin's id and version, its tier, the manifest's digest and the index's serial"""
    named(m)
    if tier not in TIERS or type(serial) is not int or not 0 <= serial < 10 ** 12:
        die("a listing's tier is official or community, its serial a whole number (the index's)")
    return SIGN_LISTING + (f"id {m['id']}\nversion {m['version']}\ntier {tier}\n"
                           f"manifest {hashlib.blake2b(mj, digest_size=32).hexdigest()}\nserial {serial}\n").encode()


def listing(m, mj, secret, tier="official", serial=0):
    """listing.sig: the index key's signature as hex, then the text it signs"""
    text = listing_text(m, mj, tier, serial)
    return otatool.sign(otatool.secret(secret), text).hex().encode() + b"\n" + text


def listing_read(f, m, pub):
    """(tier, serial) of the package's listing when the key PUB signed it for exactly this manifest, else None"""
    sig, _, text = f.get("listing.sig", b"").partition(b"\n")
    x = re.fullmatch(rb"hassmic plugin listing 2\nid ([a-z0-9-]+)\nversion ([0-9]+)\ntier (official|community)\nmanifest ([0-9a-f]{64})\nserial ([0-9]{1,12})\n", text)
    if (not x or not re.fullmatch(rb"[0-9a-f]{128}", sig) or x.group(1).decode() != m["id"] or x.group(2).decode() != str(m["version"])
            or x.group(4).decode() != hashlib.blake2b(f["manifest.json"], digest_size=32).hexdigest()
            or not otatool.check(bytes.fromhex(sig.decode()), pub, text)):
        return None
    return x.group(3).decode(), int(x.group(5))


def tar_bytes(files):
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as t:
        for name, data in files:                              # nothing of the machine it was made on: the same bytes anywhere
            ti = tarfile.TarInfo(name)
            ti.size, ti.mode, ti.mtime, ti.uid, ti.gid, ti.uname, ti.gname = len(data), 0o755 if name == "plugin" else 0o644, 0, 0, 0, "", ""
            t.addfile(ti, io.BytesIO(data))
    return buf.getvalue()


def tar_write(files, out):
    data = tar_bytes(files)
    if len(data) > PACKAGE_MAX:
        die(f"the package is {len(data) >> 10} kB, over {PACKAGE_MAX >> 20} MB: strip the program")
    with open(out, "wb") as f:
        f.write(data)


def members(package):
    """a package's files by name: a plain tar of the five names, each once and within its size, read no further"""
    f = {}
    try:
        if os.path.getsize(package) > PACKAGE_MAX:
            die(f"{package}: over {PACKAGE_MAX >> 20} MB")
        with tarfile.open(package, mode="r:") as t:          # r: is a tar as it is, never an unpacked one
            for ti in t:
                if not ti.isfile() or ti.name not in MEMBERS or ti.name in f or ti.size > MEMBERS[ti.name]:
                    die(f"{package}: \"{ti.name}\" does not belong in a package, is there twice or is too large")
                f[ti.name] = t.extractfile(ti).read()
    except (tarfile.TarError, EOFError, ValueError, OSError) as e:
        die(f"{package}: no package: {e}")
    return f


def checked(package, f):
    """the manifest of a package whose author's signature and program digest hold"""
    try:
        m = json.loads(f["manifest.json"], object_pairs_hook=no_twice)
        named(m)
        ok = otatool.check(f.get("manifest.sig", b""), bytes.fromhex(m["author_key"]), SIGN_MANIFEST + f["manifest.json"])
        ok = ok and hashlib.blake2b(f["plugin"], digest_size=32).hexdigest() == m["digest"]
        ok = ok and f["manifest.json"][:1] == b"{"            # UTF-8 as the Echo reads it: json.loads would take UTF-16 too
    except (KeyError, ValueError, TypeError, RecursionError) as e:
        die(f"{package}: no package: {e!r}")
    if not ok:
        die(f"{package}: the author's signature or the program's digest does not hold")
    return m


def list_package(package, secret, out, tier="official", serial=0, arch="arm"):
    """the package, checked, with the index key's listing (what tools/index.py of the plugin index adds)"""
    f = members(package)
    m = checked(package, f)
    bad = elf_check(f["plugin"], arch)
    if bad:
        die(f"{package}: {bad}")
    if "LICENSE" not in f:
        die(f"{package}: no LICENSE in it")
    if "listing.sig" in f:
        die(f"{package}: it carries a listing already: the index makes it, nobody else")
    f["listing.sig"] = listing(m, f["manifest.json"], secret, tier, serial)
    tar_write([(n, f[n]) for n in ("manifest.json", "manifest.sig", "plugin", "LICENSE", "listing.sig") if n in f], out)
    print(f"{out}: {m['id']} version {m['version']}, listed ({tier})")


def pack(manifest, program, secret, out, license_=None, listed=None, arch="arm"):
    prog = otatool.slurp(program)
    bad = elf_check(prog, arch)
    if bad:
        die(f"{program}: {bad}")
    sk = otatool.secret(secret)
    try:
        m = json.loads(otatool.slurp(manifest))
    except ValueError as e:
        die(f"{manifest}: {e}")
    named(m)
    bad = requires_check(m)
    if bad:
        die(f"{manifest}: {bad}")
    m["author_key"] = sk[32:].hex()
    m["digest"] = hashlib.blake2b(prog, digest_size=32).hexdigest()
    mj = json.dumps(m, indent=1, ensure_ascii=False).encode() + b"\n"
    files = [("manifest.json", mj), ("manifest.sig", otatool.sign(sk, SIGN_MANIFEST + mj)), ("plugin", prog)]
    if license_:
        files.append(("LICENSE", otatool.slurp(license_)))
    if listed:
        files.append(("listing.sig", listing(m, mj, listed)))
    tar_write(files, out)
    print(f"{out}: {m['id']} version {m['version']}, {len(prog) >> 10} kB of program, {'listed as official' if listed else 'no listing (a sideload)'}, "
          f"author's key {fingerprint(sk[32:])}")


def fingerprint(key):
    """An author's key as the settings page names it for a package without a listing (plugin_store.c ps_fingerprint):
    the first 16 hex digits of its BLAKE2b-256, in fours.  What an author publishes for owners to compare."""
    h = hashlib.blake2b(key, digest_size=32).hexdigest()
    return " ".join(h[i:i + 4] for i in range(0, 16, 4))


def show(package, index_pub=None):
    f = members(package)
    try:
        m = json.loads(f["manifest.json"], object_pairs_hook=no_twice)
        named(m)
        signed = otatool.check(f.get("manifest.sig", b""), bytes.fromhex(m["author_key"]), SIGN_MANIFEST + f["manifest.json"])
        program = hashlib.blake2b(f["plugin"], digest_size=32).hexdigest() == m["digest"]
        caps = sorted(str(k) for k in m["capabilities"]) if isinstance(m.get("capabilities"), dict) else "none that are a manifest's"
    except (KeyError, ValueError, TypeError, RecursionError) as e:
        die(f"{package}: no package: {e!r}")
    ok = signed and program
    # the manifest's own words as Python writes them (!r): a name cannot draw over these lines with escape codes
    print(f"{m['id']} {m.get('name')!r} version {m['version']} by {m.get('author')!r}, requires {m.get('requires')!r}")
    print(f"  author's signature: {'good' if signed else 'DOES NOT VERIFY'}")
    print(f"  author's key: {fingerprint(bytes.fromhex(m['author_key']))} (its fingerprint, as the settings page shows it)")
    print(f"  program: {len(f['plugin'])} bytes, digest {'right' if program else 'WRONG'}")
    print(f"  capabilities: {caps!r}")
    print(f"  manifest: {hashlib.blake2b(f['manifest.json'], digest_size=32).hexdigest()}")
    if "listing.sig" in f:
        got = listing_read(f, m, otatool.slurp(index_pub)) if index_pub else None
        ok = ok and (got or not index_pub)
        print(f"  listing: {'good, ' + got[0] + ', index serial ' + str(got[1]) if got else 'not checked (no key given)' if not index_pub else 'DOES NOT VERIFY'}")
    else:
        print("  listing: none (a sideload)")
    return 0 if ok else 1


def main():
    a = sys.argv[1:]
    opt = {}
    for name in ("--arch", "--license", "--listing", "--index-pub", "--tier", "--serial"):
        if name in a:
            i = a.index(name)
            if i + 1 >= len(a):
                die(__doc__)
            opt[name] = a[i + 1]
            del a[i:i + 2]
    if len(a) == 3 and a[0] == "keygen":
        otatool.keygen(a[1], a[2])
    elif len(a) == 2 and a[0] == "elf":
        bad = elf_check(otatool.slurp(a[1]), opt.get("--arch", "arm"))
        if bad:
            die(f"{a[1]}: {bad}")
        print(f"{a[1]}: keeps the rules a file shows")
    elif len(a) == 5 and a[0] == "pack":
        pack(a[1], a[2], a[3], a[4], opt.get("--license"), opt.get("--listing"), opt.get("--arch", "arm"))
    elif len(a) == 4 and a[0] == "list" and re.fullmatch(r"[0-9]{1,12}", opt.get("--serial", "0")):
        list_package(a[1], a[2], a[3], opt.get("--tier", "official"), int(opt.get("--serial", "0")), opt.get("--arch", "arm"))
    elif len(a) == 2 and a[0] == "show":
        sys.exit(show(a[1], opt.get("--index-pub")))
    elif len(a) == 2 and a[0] == "requires":
        try:
            m = json.loads(otatool.slurp(a[1]))
        except ValueError as e:
            die(f"{a[1]}: {e}")
        print(json.dumps(requires_for(m.get("capabilities") or {})))
    else:
        die(__doc__)


if __name__ == "__main__":
    main()
