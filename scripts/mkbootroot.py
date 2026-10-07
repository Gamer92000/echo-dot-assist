#!/usr/bin/env python3
"""mkbootroot: a rooted boot.img from the device's own stock one, in the standard library only.

  mkbootroot.py --check BOOT.img            prove the rebuild: stock image in, the same out but for the ID
  mkbootroot.py BOOT.img OUT.img            build the rooted image

The XDA boot-root.imgs are built from some other build's boot.img (checkers': NS6570/6086, to
run against 8149's /system), so this applies their ramdisk changes to the stock image of the
firmware actually installed: kernel, sepolicy and every other ramdisk file stay that build's.
adb is then a root shell without authentication.  The certificate page: Amazon's kernel-signing
cert rides after the last page of the stock image and is copied over, kernel unchanged (the XDA
image drops it and boots too, so an unlocked lk does not look at either).

Boot image: Android header v0, page as the header says; kernel and (if there) ramdisk blobs
carry MediaTek's 512-byte header, whose size field counts the payload without it (the Android
header's sizes with it).  The ramdisk is a gzip'd cpio (newc); the two files are replaced inside
the stream, so every other entry keeps its exact bytes — order, metadata, data.  The new ramdisk
is deflated anew (mtime 0, level 9, the stock stream's header bytes).  The header page is the
stock one but for ramdisk_size and the ID, which AOSP mkbootimg's v0 formula computes (sizes,
then the blobs); the stock ID follows some other formula, but lk does not check it: the XDA
image boots with a foreign one.

In the ramdisk:
- default.prop: ro.secure=0, ro.adb.secure=0, ro.debuggable=1, ro.allow.mock.location=1,
  persist.sys.usb.config=mtp,adb  (the XDA image's changes)
- init.fosflags.sh: adb on, authentication off, nothing else (the XDA image's file, byte for
  byte: it ends without a newline)
"""
import hashlib, struct, sys, zlib

MTK_MAGIC = bytes.fromhex("88168858")
ID_OFF, ID_LEN = 0x240, 0x20

PROPS = [  # default.prop, old line -> new line; each must be there exactly once
    (b"ro.adb.secure=1", b"ro.adb.secure=0"),
    (b"ro.secure=1", b"ro.secure=0"),
    (b"ro.allow.mock.location=0", b"ro.allow.mock.location=1"),
    (b"ro.debuggable=0", b"ro.debuggable=1"),
    (b"persist.sys.usb.config=none", b"persist.sys.usb.config=mtp,adb"),
]
FOSFLAGS = (b"#!/system/bin/sh\n"
            b"PATH=/sbin:/system/sbin:/system/bin:/system/xbin\n"
            b"\n"
            b"setprop persist.sys.usb.config mtp,adb\n"
            b"setprop amazon.fos_flags.noadbauth 1")


class Boot:
    def __init__(self, raw):
        assert raw[:8] == b"ANDROID!", "not a boot image"
        (self.ksz, self.kaddr, self.rsz, self.raddr, self.ssz, _, _, self.page) = \
            struct.unpack("<8I", raw[8:40])
        assert self.ssz == 0, "a second stage is not handled"

        def pages(n):
            return (n + self.page - 1) // self.page

        self.hdr = raw[:self.page]
        self.kernel = raw[self.page:self.page + self.ksz]
        ro = self.page + pages(self.ksz) * self.page
        ramdisk = raw[ro:ro + self.rsz]
        self.ramdisk_mtk = ramdisk[:4] == MTK_MAGIC
        if self.ramdisk_mtk:  # same convention as the kernel: the header's size is the payload's
            assert struct.unpack("<I", ramdisk[4:8])[0] == self.rsz - 512
            ramdisk = ramdisk[512:]
        self.ramdisk = ramdisk
        self.trailing = raw[ro + pages(self.rsz) * self.page:]  # the certificate page, or ""

    def cpio(self):
        return gunzip_mem(self.ramdisk)

    def assemble(self, ramdisk):
        blob = (mtk_header(len(ramdisk), b"ROOTFS") + ramdisk) if self.ramdisk_mtk else ramdisk
        h = hashlib.sha1()
        h.update(struct.pack("<3I", self.ksz, len(blob), self.ssz))
        h.update(self.kernel + blob)  # AOSP mkbootimg's v0 ID

        def pad(b):
            return b + bytes(pages_(len(b), self.page) * self.page - len(b))

        hdr = bytearray(self.hdr)
        struct.pack_into("<I", hdr, 0x10, len(blob))
        hdr[ID_OFF:ID_OFF + ID_LEN] = h.digest() + bytes(ID_LEN - 20)
        return bytes(hdr) + pad(self.kernel) + pad(blob) + self.trailing


def pages_(n, page):
    return (n + page - 1) // page


def mtk_header(size, name):
    return MTK_MAGIC + struct.pack("<I", size) + name.ljust(8, b"\0") + bytes(512 - 16)


def gzip_mem(data):  # stock stream: mtime 0, XFL 0, OS 3; deflate level 9
    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    return (b"\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03" + co.compress(data) + co.flush()
            + struct.pack("<II", zlib.crc32(data) & 0xFFFFFFFF, len(data) & 0xFFFFFFFF))


def gunzip_mem(data):
    d = zlib.decompressobj(16 + zlib.MAX_WBITS)
    out = d.decompress(data) + d.flush()
    assert not d.unused_data and not d.unconsumed_tail, "more than one gzip member"
    return out


def edit_cpio(cpio):
    """Replace the two files inside the stream; every other byte of an entry stays."""
    out, off, done = [], 0, set()

    def edited(name, data):
        if name == b"default.prop":
            for old, new in PROPS:
                assert data.count(old + b"\n") == 1, old + b" not exactly once in default.prop"
                data = data.replace(old + b"\n", new + b"\n")
        elif name == b"init.fosflags.sh":
            data = FOSFLAGS
        else:
            return None
        done.add(name)
        return data

    while True:
        assert cpio[off:off + 6] in (b"070701", b"070702"), "not a newc cpio at %d" % off
        f = [int(cpio[off + 6 + 8 * i:off + 6 + 8 * (i + 1)], 16) for i in range(13)]
        size, namesize = f[6], f[11]
        name = cpio[off + 110:off + 110 + namesize - 1]
        data_off = (off + 110 + namesize + 3) & ~3
        data = cpio[data_off:data_off + size]
        if name == b"TRAILER!!!":
            out.append(cpio[off:data_off])  # the trailer and whatever follows it, verbatim
            break
        new = edited(name, data)
        if new is not None:
            f[6] = len(new)
            out.append(cpio[off:off + 54] + b"%08x" % f[6] + cpio[off + 62:off + 110]
                       + name + b"\0" + b"\0" * (-(110 + namesize) % 4)
                       + new + b"\0" * (-len(new) % 4))
        else:
            out.append(cpio[off:data_off + size + (-size % 4)])
        off = data_off + size + (-size % 4)
    missing = {b"default.prop", b"init.fosflags.sh"} - done
    assert not missing, "not in the ramdisk: %s" % b", ".join(sorted(missing)).decode()
    return b"".join(out)


def entries(cpio):
    """(name, metadata without the size, data) in stream order; to compare ramdisks file by file."""
    out, off = [], 0
    while True:
        assert cpio[off:off + 6] in (b"070701", b"070702"), "not a newc cpio at %d" % off
        f = [int(cpio[off + 6 + 8 * i:off + 6 + 8 * (i + 1)], 16) for i in range(13)]
        size, namesize = f[6], f[11]
        name = cpio[off + 110:off + 110 + namesize - 1]
        data_off = (off + 110 + namesize + 3) & ~3
        if name == b"TRAILER!!!":
            return out
        out.append((name, tuple(f[:6] + f[7:]), cpio[data_off:data_off + size]))
        off = data_off + size + (-size % 4)


def compare(stock, new, label):
    a, b = entries(stock), entries(new)
    assert [e[0] for e in a] == [e[0] for e in b], label + ": the file list changed"
    for (na, ha, da), (nb, hb, db) in zip(a, b):
        assert na == nb and hb == ha, label + ": " + na.decode() + "'s metadata changed"
        if da != db:
            assert na in (b"default.prop", b"init.fosflags.sh"), label + ": " + na.decode() + " changed"
            print("  " + na.decode() + " changed (%d -> %d bytes)" % (len(da), len(db)))


def main(argv):
    check = argv[:1] == ["--check"]
    argv = argv[1:] if check else argv
    if len(argv) != (1 if check else 2):
        sys.exit(__doc__)
    boot = Boot(open(argv[0], "rb").read())
    assert boot.kernel[:4] == MTK_MAGIC, "the kernel blob has no MediaTek header"
    assert struct.unpack("<I", boot.kernel[4:8])[0] == boot.ksz - 512

    if check:
        rebuilt = boot.assemble(boot.ramdisk)
        stock = open(argv[0], "rb").read()
        assert len(rebuilt) == len(stock), "check: length %d, stock %d" % (len(rebuilt), len(stock))
        diff = [i for i in range(len(stock)) if rebuilt[i] != stock[i]]
        assert all(ID_OFF <= i < ID_OFF + ID_LEN for i in diff), \
            "check: the rebuild differs outside the ID: %s" % diff[:8]
        print("check: %d of %d bytes differ, all inside the ID" % (len(diff), len(stock)))
        return

    cpio = edit_cpio(boot.cpio())
    out = boot.assemble(gzip_mem(cpio))
    compare(boot.cpio(), Boot(out).cpio(), "the built image")
    open(argv[1], "wb").write(out)
    print("%s: %d bytes, ramdisk blob %d -> %d\n  sha256 %s" %
          (argv[1], len(out), boot.rsz, Boot(out).rsz, hashlib.sha256(out).hexdigest()))


if __name__ == "__main__":
    main(sys.argv[1:])
