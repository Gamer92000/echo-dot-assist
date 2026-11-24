#!/usr/bin/env python3
"""otatool, the PC side: update bundles for hassmic, in Python's standard library only (no compiler on the PC).

  otatool.py keygen  SECRET PUBLIC                       new signing key pair (keep SECRET off the device)
  otatool.py pack    SECRET VERSION OUT FILE[:MODE]...   OUT = bundle, OUT.sig = detached signature
  otatool.py verify  PUBLIC BUNDLE SIG                   exit 0 if the signature is good; prints the version
  otatool.py install PUBLIC BUNDLE SIG DESTDIR           verify, then unpack into DESTDIR (must not exist yet)
  otatool.py push    HOST PORT BUNDLE SIG                send to a running hassmic, print its answer (exit 0 on "OK ...")
  otatool.py adb     HOST PORT SECRET                    open adb over Wi-Fi on it for 30 min (sign its challenge)
  otatool.py plugin  HOST PORT SECRET PACKAGE [WAIT]     an official plugin's .hmp to it (scripts/ota-push.sh --plugin)

The same commands, files and answers as src/tools/otatool.c, which stays the Echo's own (it has no Python) and is
what checks these bundles there.  tests/otatool_test.sh holds the two to each other: same keys, signatures byte for
byte (EdDSA signatures are deterministic), each one's bundles installed by the other.

Signature: EdDSA as Monocypher defines it (crypto_eddsa_*: edwards25519 as Ed25519, BLAKE2b-512 where Ed25519 has
SHA-512), so no Python package does it; the arithmetic below is RFC 8032's.  Not constant time: fine for signing on
the owner's PC or in CI, where nobody times it.
Bundle: "HMOTA1\\n" "version <v>\\n" then per file "file <name> <octal mode> <size>\\n" + <size> bytes, then "end\\n".
"""
import hashlib, os, re, socket, sys, time

P = 2**255 - 19
L = 2**252 + 27742317777372353535851937790883648493
D = -121665 * pow(121666, P - 2, P) % P
SQRT_M1 = pow(2, (P - 1) // 4, P)


def h(*parts):
    return int.from_bytes(hashlib.blake2b(b"".join(parts), digest_size=64).digest(), "little")


# points in extended coordinates (X, Y, Z, T), x = X/Z, y = Y/Z, xy = T/Z
def add(p, q):
    x1, y1, z1, t1 = p
    x2, y2, z2, t2 = q
    a, b = (y1 - x1) * (y2 - x2) % P, (y1 + x1) * (y2 + x2) % P
    c, d = 2 * t1 * t2 * D % P, 2 * z1 * z2 % P
    e, f, g, hh = b - a, d - c, d + c, b + a
    return (e * f % P, g * hh % P, f * g % P, e * hh % P)


def mul(s, p):
    q = (0, 1, 1, 0)
    while s:
        if s & 1:
            q = add(q, p)
        p = add(p, p)
        s >>= 1
    return q


def encode(p):
    x, y, z, _ = p
    zi = pow(z, P - 2, P)
    x, y = x * zi % P, y * zi % P
    return (y | (x & 1) << 255).to_bytes(32, "little")


def decode(b):
    y = int.from_bytes(b, "little")
    sign, y = y >> 255, y & ((1 << 255) - 1)
    if y >= P:
        return None
    x2 = (y * y - 1) * pow(D * y * y + 1, P - 2, P) % P
    x = pow(x2, (P + 3) // 8, P)
    if (x * x - x2) % P:
        x = x * SQRT_M1 % P
    if (x * x - x2) % P or (x == 0 and sign):
        return None
    if x & 1 != sign:
        x = P - x
    return (x, y, 1, x * y % P)


G = decode((4 * pow(5, P - 2, P) % P).to_bytes(32, "little"))


def expand(seed):
    """crypto_eddsa_key_pair / _sign: the secret scalar (clamped) and the nonce prefix, from BLAKE2b of the seed."""
    hb = hashlib.blake2b(seed, digest_size=64).digest()
    a = int.from_bytes(hb[:32], "little")
    a = (a & ~7 & ((1 << 254) - 1)) | (1 << 254)
    return a, hb[32:]


def key_pair(seed):
    a, _ = expand(seed)
    pk = encode(mul(a, G))
    return seed + pk, pk                       # Monocypher's 64-byte secret key: seed, then the public key


def sign(sk, msg):
    a, prefix = expand(sk[:32])
    r = h(prefix, msg) % L
    rb = encode(mul(r, G))
    return rb + ((r + h(rb, sk[32:], msg) % L * a) % L).to_bytes(32, "little")


IDENTITY = (0, 1, 1, 0)


def weak(pk):
    """no key to take: a public key under which any signature holds (src/include/edkey.h), one of small order, [8]A
    the identity.  crypto_eddsa_check is cofactored, so under such a key the null signature (R the identity, s = 0)
    verifies for every text.  keygen never makes one ([a]B for a secret scalar); a placeholder or a file of zeros is
    one.  And any key decode() refuses: six such encodings (y = p and p + 1, the order-2 point and the identity with
    the sign bit set, each sign) Monocypher reads as points of small order, and the Echo refuses them (ed_key_weak)
    while the Makefile, the index's tool and an author's check took them (a review of 3c5d7ff, R4); the rest are no
    point at all, under which nothing verifies.  A key to take is a point this reads, of large order"""
    a = decode(pk) if len(pk) == 32 else None
    return a is None or encode(mul(8, a)) == encode(IDENTITY)


def check(sig, pk, msg):
    a = decode(pk)
    if a is None or len(sig) != 64 or weak(pk):
        return False
    s = int.from_bytes(sig[32:], "little")
    if s >= L:
        return False
    # R = [s]B - [k]A, as crypto_eddsa_check compares it
    neg = ((-a[0]) % P, a[1], a[2], (-a[3]) % P)
    return encode(add(mul(s, G), mul(h(sig[:32], pk, msg) % L, neg))) == sig[:32]


def die(msg):
    print(f"otatool: {msg}", file=sys.stderr)
    sys.exit(1)


def slurp(path):
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError as e:
        die(f"{path}: {e.strerror}")


def spit(path, data, mode):
    try:
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, mode)
    except OSError as e:
        die(f"{path}: {e.strerror}")
    with os.fdopen(fd, "wb") as f:
        f.write(data)
        os.fchmod(f.fileno(), mode)             # not subject to umask


NAME = re.compile(rb"[A-Za-z0-9._-]{1,64}")


def name_ok(n):
    return NAME.fullmatch(n) is not None and not n.startswith(b".")


def secret(path):
    sk = slurp(path)
    if len(sk) != 64:
        die("bad secret key")
    return sk


def keygen(sec, pub):
    sk, pk = key_pair(os.urandom(32))
    if weak(pk):                                # (never: [a]B has the order of B; said, not assumed)
        die("keygen made a key of small order")
    spit(sec, sk, 0o600)
    spit(pub, pk, 0o644)


def pack(sec, version, out, files):
    sk = secret(sec)
    b = bytearray(b"HMOTA1\nversion %s\n" % version.encode())
    for arg in files:
        path, mode = arg, 0o755
        if ":" in arg:
            path, m = arg.rsplit(":", 1)
            mode = int(m, 8)
        base = os.path.basename(path).encode()
        if not name_ok(base):
            die(f"cannot pack {path}")
        data = slurp(path)
        b += b"file %s %o %d\n" % (base, mode, len(data)) + data
    b += b"end\n"
    for p in (out, out + ".sig"):
        if os.path.lexists(p):
            os.unlink(p)
    spit(out, bytes(b), 0o644)
    spit(out + ".sig", sign(sk, bytes(b)), 0o644)
    print(f"{out}: version {version}, {len(files)} files, {len(b)} bytes, signed")


def verify_install(pub, bundle, sigpath, dest=None):
    """Verify, then (dest) unpack.  Parsing only ever happens on authenticated data."""
    pk, b, sig = slurp(pub), slurp(bundle), slurp(sigpath)
    if len(pk) != 32 or len(sig) != 64:
        die("bad key, bundle or signature file")
    if weak(pk):
        die(f"{pub}: a key under which any signature holds (small order) or no point at all: no key, nothing installed")
    if not check(sig, pk, b):
        die("SIGNATURE DOES NOT VERIFY, nothing installed")
    if not b.startswith(b"HMOTA1\n"):
        die("not a bundle")
    m = re.match(rb"version ([^\n]{1,63})\n", b[7:])
    if not m:
        die("no version")
    version, p, files = m.group(1), 7 + m.end(), []
    while not b.startswith(b"end\n", p):
        nl = b.find(b"\n", p)
        f = re.fullmatch(rb"file (\S{1,79}) ([0-7]+) ([0-9]+)", b[p:nl]) if nl > 0 else None
        if not f or not name_ok(f.group(1)) or int(f.group(3)) > len(b) - nl - 1:
            die("malformed bundle")
        size = int(f.group(3))
        files.append((f.group(1).decode(), int(f.group(2), 8), b[nl + 1:nl + 1 + size]))
        p = nl + 1 + size
    if dest:
        try:
            os.mkdir(dest, 0o755)
        except OSError as e:
            die(f"{dest}: {e.strerror}")
        os.chmod(dest, 0o755)
        for name, mode, data in files:
            spit(os.path.join(dest, name), data, mode & 0o755)
        spit(os.path.join(dest, "VERSION"), version, 0o644)
    print(version.decode())


def connect(host, port):
    try:
        return socket.create_connection((host, int(port)), timeout=60)
    except OSError as e:
        die(f"cannot connect to {host}:{port}: {e.strerror or e}")


def read_line(s):
    line = bytearray()
    while len(line) < 299:
        c = s.recv(1)
        if not c or c == b"\n":
            break
        line += c
    return line.decode(errors="replace")


def answer(s):
    try:
        line = read_line(s)
    except OSError:
        line = ""
    s.close()
    print(line or "FAILED no answer")
    return 0 if line.startswith("OK") else 1


def push(host, port, bundle, sigpath):
    b, sig = slurp(bundle), slurp(sigpath)
    if len(sig) != 64:
        return 1
    s = connect(host, port)
    try:
        s.sendall(b"HMOTA-PUSH1 %d\n" % len(b) + sig + b)
    except OSError:
        pass  # the Echo closed while we sent: its reason ("FAILED busy ...") is the line it left, read below
    return answer(s)


def challenge(host, port, sec, req):
    """Send REQ, sign REQ + the nonce the Echo answers with, print its verdict.  Protocol: src/hassmic/ota.c."""
    sk = secret(sec)
    s = connect(host, port)
    s.sendall(req)
    line = read_line(s)
    if not re.fullmatch(r"NONCE [0-9a-fA-F]{64}", line):
        print(line or "FAILED no challenge (an older hassmic?)")
        return 1
    s.sendall(sign(sk, req + bytes.fromhex(line[6:])))
    return answer(s)


def plugin(host, port, sec, package, wait="0"):
    """An official plugin's package to an installed Echo (src/hassmic/ota.c "HMOTA-PLUG1"): the update key answers
    the port's challenge over the package's digest, the package's own listing by the release key is what the Echo
    installs it for.  WAIT: seconds to keep trying to connect (a hassmic just pushed is starting)."""
    sk, b = secret(sec), slurp(package)
    req = b"HMOTA-PLUG1 %d %s\n" % (len(b), hashlib.blake2b(b, digest_size=32).hexdigest().encode())
    end = time.monotonic() + float(wait)
    while True:
        try:
            s = socket.create_connection((host, int(port)), timeout=60)
            break
        except OSError as e:
            if time.monotonic() >= end:
                die(f"cannot connect to {host}:{port}: {e.strerror or e}")
            time.sleep(1)
    s.sendall(req)
    line = read_line(s)
    if not re.fullmatch(r"NONCE [0-9a-fA-F]{64}", line):
        print(line or "FAILED no challenge (an older hassmic?)")
        return 1
    s.sendall(sign(sk, req + bytes.fromhex(line[6:])))
    line = read_line(s)
    if line != "SEND":
        print(line or "FAILED no answer")
        return 1
    try:
        s.sendall(b)
    except OSError:
        pass  # as in push()
    return answer(s)


def main(a):
    if len(a) == 3 and a[0] == "keygen":
        return keygen(a[1], a[2])
    if len(a) >= 5 and a[0] == "pack":
        return pack(a[1], a[2], a[3], a[4:])
    if len(a) == 4 and a[0] == "verify":
        return verify_install(a[1], a[2], a[3])
    if len(a) == 5 and a[0] == "install":
        return verify_install(a[1], a[2], a[3], a[4])
    if len(a) == 5 and a[0] == "push":
        return push(*a[1:])
    if len(a) == 4 and a[0] == "adb":
        return challenge(a[1], a[2], a[3], b"HMOTA-ADB1\n")
    if len(a) in (5, 6) and a[0] == "plugin":
        return plugin(*a[1:])
    print("usage: otatool.py keygen SECRET PUBLIC | push HOST PORT BUNDLE SIG | adb HOST PORT SECRET | plugin HOST PORT SECRET PACKAGE [WAIT] | "
          "pack SECRET VERSION OUT FILE[:MODE]... | verify PUBLIC BUNDLE SIG | install PUBLIC BUNDLE SIG DESTDIR",
          file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]) or 0)
