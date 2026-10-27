#!/usr/bin/env python3
"""mkhome: hassmic's home screen for Android models (checkers), an APK without code.

  python3 tools/mkhome.py            write scripts/device/hassmic-home.apk
  python3 tools/mkhome.py --check    re-read the one in the tree: manifest, digests, signature

Android needs a home activity: boot completion (sys.boot_completed, on which the satellite starts) waits for the first
activity to go idle.  Stock's last one was Settings' FallbackHome, and Amazon's Settings cannot start without Amazon's
account apps (MAP's authenticator, "No central apk detected", then CountryHelper throws), which a satellite removes.
So this: one activity, android.app.Activity itself (the framework's class; android:hasCode="false", as pure native apps
declare android.app.NativeActivity), black, full screen, MAIN/HOME/DEFAULT, directBootAware: Android 7 starts the first
home before user 0 is unlocked, when only direct boot aware activities count (Settings' FallbackHome is one for that
reason).  Without it the Echo Show found "No home screen" and never finished booting (2026-10-13).  Nothing runs in it.

The APK is AndroidManifest.xml (Android's binary XML) and a v1 JAR signature (SHA-256 digests, an ECDSA P-256
certificate; Android 7 takes v1 alone, from API 18 with these algorithms).  Rebuilding makes a new key: for a package
of the system image PackageManager takes the new certificate ("system package signature changed", it recovers), so an
Echo updates to it; installed as an ordinary app it would refuse.
Resource ids are the framework's own (android.R.attr / android.R.style of 8149's boot class path, the same for every
Android 7 build).
"""
import base64, datetime, hashlib, io, struct, sys, zipfile

OUT = "scripts/device/hassmic-home.apk"
PACKAGE = "org.hassmic.home"
NS = "http://schemas.android.com/apk/res/android"
ATTR = {  # android.R.attr
    "theme": 0x01010000, "label": 0x01010001, "name": 0x01010003, "hasCode": 0x0101000C,
    "stateNotNeeded": 0x01010016, "excludeFromRecents": 0x01010017, "launchMode": 0x0101001D,
    "minSdkVersion": 0x0101020C, "versionCode": 0x0101021B, "versionName": 0x0101021C, "targetSdkVersion": 0x01010270,
    "directBootAware": 0x01010505,
}
THEME_BLACK_FULLSCREEN = 0x0103000A  # android.R.style.Theme_Black_NoTitleBar_Fullscreen
T_REF, T_STRING, T_INT, T_BOOL = 0x01, 0x03, 0x10, 0x12

# (tag, [(attr name or None for no namespace, type, value)], children)
MANIFEST = ("manifest", [(None, "package", T_STRING, PACKAGE), ("versionCode", None, T_INT, 1), ("versionName", None, T_STRING, "1")], [
    ("uses-sdk", [("minSdkVersion", None, T_INT, 24), ("targetSdkVersion", None, T_INT, 25)], []),
    ("application", [("label", None, T_STRING, "hassmic"), ("hasCode", None, T_BOOL, False), ("directBootAware", None, T_BOOL, True)], [
        ("activity", [("theme", None, T_REF, THEME_BLACK_FULLSCREEN), ("name", None, T_STRING, "android.app.Activity"),
                      ("stateNotNeeded", None, T_BOOL, True), ("excludeFromRecents", None, T_BOOL, True),
                      ("launchMode", None, T_INT, 2), ("directBootAware", None, T_BOOL, True)], [   # singleTask
            ("intent-filter", [], [
                ("action", [("name", None, T_STRING, "android.intent.action.MAIN")], []),
                ("category", [("name", None, T_STRING, "android.intent.category.HOME")], []),
                ("category", [("name", None, T_STRING, "android.intent.category.DEFAULT")], []),
            ]),
        ]),
    ]),
])


def axml(root):
    """Android binary XML: string pool (UTF-16), resource map for the attribute names first, then the elements."""
    attr_names, others = [], []

    def collect(node):
        tag, attrs, kids = node
        for a in attrs:
            if a[0] is not None and a[0] not in attr_names:
                attr_names.append(a[0])
        for a in attrs:
            for s in ([a[1]] if a[0] is None else []) + ([a[3]] if a[2] == T_STRING else []):
                if s not in others:
                    others.append(s)
        if tag not in others:
            others.append(tag)
        for k in kids:
            collect(k)
    collect(root)
    attr_names.sort(key=lambda n: ATTR[n])
    strings = attr_names + [s for s in ["android", NS] + others if s not in attr_names]
    idx = {s: i for i, s in enumerate(strings)}

    data, offsets = b"", []
    for s in strings:
        offsets.append(len(data))
        u = s.encode("utf-16-le")
        data += struct.pack("<H", len(s)) + u + b"\0\0"
    data += b"\0" * (-len(data) % 4)
    hdr = 28
    pool = struct.pack("<HHIIIIII", 0x0001, hdr, hdr + 4 * len(strings) + len(data), len(strings), 0, 0,
                       hdr + 4 * len(strings), 0) + b"".join(struct.pack("<I", o) for o in offsets) + data
    resmap = struct.pack("<HHI", 0x0180, 8, 8 + 4 * len(attr_names)) + b"".join(struct.pack("<I", ATTR[n]) for n in attr_names)

    body = [struct.pack("<HHIIIII", 0x0100, 16, 24, 1, 0xFFFFFFFF, idx["android"], idx[NS])]
    line = [1]

    def element(node):
        tag, attrs, kids = node
        line[0] += 1
        enc = []
        for name, plain, typ, val in sorted(attrs, key=lambda a: ATTR[a[0]] if a[0] else 0):
            ns = idx[NS] if name else 0xFFFFFFFF
            nm = idx[name] if name else idx[plain]
            if typ == T_STRING:
                raw, data_ = idx[val], idx[val]
            else:
                raw, data_ = 0xFFFFFFFF, (0xFFFFFFFF if val is True else 0 if val is False else val)
            enc.append(struct.pack("<IIIHBBI", ns, nm, raw, 8, 0, typ, data_))
        body.append(struct.pack("<HHIIIIIHHHHHH", 0x0102, 16, 36 + 20 * len(enc), line[0], 0xFFFFFFFF, 0xFFFFFFFF,
                                idx[tag], 20, 20, len(enc), 0, 0, 0) + b"".join(enc))
        for k in kids:
            element(k)
        body.append(struct.pack("<HHIIIII", 0x0103, 16, 24, line[0], 0xFFFFFFFF, 0xFFFFFFFF, idx[tag]))
    element(root)
    body.append(struct.pack("<HHIIIII", 0x0101, 16, 24, line[0], 0xFFFFFFFF, idx["android"], idx[NS]))
    payload = pool + resmap + b"".join(body)
    return struct.pack("<HHI", 0x0003, 8, 8 + len(payload)) + payload


def b64sha(b):
    return base64.b64encode(hashlib.sha256(b).digest()).decode()


def sign(entries):
    """v1 signature: MANIFEST.MF, CERT.SF, CERT.EC (PKCS#7, detached, over CERT.SF)."""
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives.serialization import pkcs7
    from cryptography.x509.oid import NameOID
    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "hassmic home")])
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
            .serial_number(1).not_valid_before(datetime.datetime(2026, 1, 1))
            .not_valid_after(datetime.datetime(2126, 1, 1)).sign(key, hashes.SHA256()))
    mf = "Manifest-Version: 1.0\r\nCreated-By: hassmic tools/mkhome.py\r\n\r\n"
    sections = []
    for n, d in entries:
        sec = f"Name: {n}\r\nSHA-256-Digest: {b64sha(d)}\r\n\r\n"
        sections.append((n, sec)); mf += sec
    sf = (f"Signature-Version: 1.0\r\nCreated-By: hassmic tools/mkhome.py\r\nSHA-256-Digest-Manifest: {b64sha(mf.encode())}\r\n\r\n"
          + "".join(f"Name: {n}\r\nSHA-256-Digest: {b64sha(sec.encode())}\r\n\r\n" for n, sec in sections))
    sig = (pkcs7.PKCS7SignatureBuilder().set_data(sf.encode()).add_signer(cert, key, hashes.SHA256())
           .sign(serialization.Encoding.DER, [pkcs7.PKCS7Options.DetachedSignature, pkcs7.PKCS7Options.NoCapabilities,
                                               pkcs7.PKCS7Options.Binary]))
    return mf.encode(), sf.encode(), sig


def build():
    man = axml(MANIFEST)
    mf, sf, sig = sign([("AndroidManifest.xml", man)])
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        for n, d, c in (("AndroidManifest.xml", man, zipfile.ZIP_STORED), ("META-INF/MANIFEST.MF", mf, zipfile.ZIP_DEFLATED),
                        ("META-INF/CERT.SF", sf, zipfile.ZIP_DEFLATED), ("META-INF/CERT.EC", sig, zipfile.ZIP_DEFLATED)):
            i = zipfile.ZipInfo(n, (2026, 1, 1, 0, 0, 0)); i.compress_type = c
            z.writestr(i, d)
    return buf.getvalue()


def strings_of(man):
    """the string pool of a binary manifest, for --check"""
    t, h, size, n, _, flags, start, _ = struct.unpack("<HHIIIIII", man[8:36])
    assert t == 1 and not flags & 0x100, "UTF-16 string pool expected"
    offs = struct.unpack(f"<{n}I", man[36:36 + 4 * n])
    out = []
    for o in offs:
        p = 8 + start + o
        ln = struct.unpack("<H", man[p:p + 2])[0]
        out.append(man[p + 2:p + 2 + 2 * ln].decode("utf-16-le"))
    return out


def check(path):
    z = zipfile.ZipFile(path)
    man, mf, sf = z.read("AndroidManifest.xml"), z.read("META-INF/MANIFEST.MF").decode(), z.read("META-INF/CERT.SF").decode()
    assert struct.unpack("<HHI", man[:8]) == (3, 8, len(man)), "manifest: not binary XML of its own length"
    s = strings_of(man)
    for want in (PACKAGE, "android.app.Activity", "android.intent.category.HOME", "android.intent.category.DEFAULT"):
        assert want in s, f"manifest lacks {want}"
    assert f"SHA-256-Digest: {b64sha(man)}" in mf, "MANIFEST.MF: digest of AndroidManifest.xml"
    assert f"SHA-256-Digest-Manifest: {b64sha(mf.encode())}" in sf, "CERT.SF: digest of MANIFEST.MF"
    from cryptography.hazmat.primitives.serialization import pkcs7
    certs = pkcs7.load_der_pkcs7_certificates(z.read("META-INF/CERT.EC"))
    assert len(certs) == 1, "CERT.EC: one certificate"
    print(f"ok {path}: {PACKAGE}, android.app.Activity as HOME, signed by {certs[0].subject.rfc4514_string()}")


if __name__ == "__main__":
    if sys.argv[1:] == ["--check"]:
        check(OUT)
    elif not sys.argv[1:]:
        open(OUT, "wb").write(build())
        check(OUT)
    else:
        sys.exit(__doc__)
