#!/usr/bin/env python3
"""apkcert.py <apk> <sha256 of the signing certificate>: exit 0 only if that certificate alone signs this APK.

Stdlib only (scripts/lib/kiosk.sh runs it wherever setup runs).  It verifies no signature and need not: an APK with a
signature block is verified by Android against exactly these bytes (v2 on Android 7.1, v3 then v2 on 9), and refused if they
do not hold.  So: a v2 block has to be there (without one Android falls back to the JAR signature, which this does not read),
every signer in every block has to be this certificate, and the blocks and each signer are read to their last byte, since
a block this reads and Android calls malformed would leave Android at that fallback (what follows the certificates in the
signed data is covered by the signature).  Whoever lacks the key can name the
certificate here but not pass Android."""
import hashlib, struct, sys

MAGIC = b"APK Sig Block 42"
BLOCKS = {0x7109871a: "v2", 0xf05368c0: "v3", 0x1b93ad61: "v3.1"}


def refuse(why):
    sys.exit("APK refused: " + why)


def lp(b, at):
    """the length-prefixed piece at `at`, and the offset after it"""
    if at + 4 > len(b): refuse("cut short")
    n = struct.unpack_from("<I", b, at)[0]
    if at + 4 + n > len(b): refuse("cut short")
    return b[at + 4:at + 4 + n], at + 4 + n


def pieces(b):
    out, at = [], 0
    while at < len(b):
        p, at = lp(b, at)
        out.append(p)
    return out


def blocks(f):
    """{id: value} of the APK Signing Block"""
    # The end of central directory record, in the last 22 bytes and nowhere else: a zip comment is what lets two readers
    # take different records, and so read different signing blocks.  Android looks for it from the end, comment
    # length 0 first, so it takes this one too.  No release APK has a comment, or needs zip64.
    f.seek(0, 2); size = f.tell()
    if size < 22: refuse("not a zip")
    f.seek(size - 22); eocd = f.read(22)
    if eocd[:4] != b"PK\x05\x06" or eocd[20:] != b"\0\0": refuse("the zip does not end with its directory")
    if size >= 42:
        f.seek(size - 42)
        if f.read(4) == b"PK\x06\x07": refuse("zip64")
    cdsize, cd = struct.unpack_from("<II", eocd, 12)
    if cd + cdsize != size - 22: refuse("the directory is not where the zip says")
    if cd < 32: refuse("no APK Signing Block")
    f.seek(cd - 24); foot = f.read(24)
    if foot[8:] != MAGIC: refuse("no APK Signing Block")
    n = struct.unpack("<Q", foot[:8])[0]
    if n + 8 > cd or n < 24: refuse("bad APK Signing Block")
    f.seek(cd - n - 8); b = f.read(n - 16)      # size, then the pairs: not the size again and the magic
    if struct.unpack_from("<Q", b)[0] != n: refuse("bad APK Signing Block")
    out, at = {}, 8
    while at + 8 <= len(b):
        k = struct.unpack_from("<Q", b, at)[0]
        if k < 4 or at + 8 + k > len(b): refuse("bad APK Signing Block")
        i = struct.unpack_from("<I", b, at + 8)[0]
        if i in out: refuse("a block twice")
        out[i] = b[at + 12:at + 8 + k]
        at += 8 + k
    if at != len(b): refuse("bad APK Signing Block")
    return out


def certs(value, v2):
    """every certificate of every signer of a block, which is read to its last byte"""
    outer, end = lp(value, 0)
    if end != len(value): refuse("bytes after the signers")
    signers = pieces(outer)
    if not signers: refuse("no signer")
    out = []
    for s in signers:
        data, at = lp(s, 0)
        if not v2: at += 8                      # v3: minSdk, maxSdk between the signed data and the signatures
        _, at = lp(s, at)                       # signatures
        _, at = lp(s, at)                       # public key
        if at != len(s): refuse("bytes after a signer")
        _, at = lp(data, 0)                     # digests
        c, _ = lp(data, at)
        out.append(pieces(c))
    return out


def main():
    apk, want = sys.argv[1], sys.argv[2]
    with open(apk, "rb") as f:
        found = blocks(f)
    used = [i for i in found if i in BLOCKS]
    if 0x7109871a not in found: refuse("no v2 signature")
    for i in used:
        for chain in certs(found[i], i == 0x7109871a):
            if not chain: refuse("a signer without a certificate")
            for c in chain:
                have = hashlib.sha256(c).hexdigest()
                if have != want: refuse(f"{BLOCKS[i]} signed by {have}, not the certificate this repository knows")


main()
