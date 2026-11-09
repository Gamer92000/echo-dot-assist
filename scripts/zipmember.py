#!/usr/bin/env python3
"""One member of a zip on a web server, read with HTTP range requests: only the central directory and that member
come over the wire, not the whole archive.  Standard library only (the setup runs it on Windows' Python too).

    zipmember.py URL MEMBER OUT                 the member as it is
    zipmember.py URL MEMBER OUT --cpio PATH     the member is a gzip'd cpio (newc, an Android ramdisk): PATH out of it
    zipmember.py URL -                          lists the members

Made for the Echo Shows' adbd (scripts/lib/setup.sh adbd_get): Google's Android 7.1.1 emulator image is 953 MB, its
ramdisk 1.3 MB.  The caller checks what comes out against its own sha256; the zip's CRC is checked here."""
import gzip
import io
import sys
import urllib.request
import zipfile


class HttpFile(io.RawIOBase):
    """A file on a web server, seekable through Range requests."""

    def __init__(self, url):
        self.url, self.pos = url, 0
        with urllib.request.urlopen(urllib.request.Request(url, method="HEAD"), timeout=60) as r:
            if r.headers.get("Accept-Ranges") != "bytes" or not (r.headers.get("Content-Length") or "").isdigit():
                raise SystemExit(f"zipmember: {url} takes no range requests (Accept-Ranges, Content-Length)")
            self.size = int(r.headers["Content-Length"])

    def readable(self):
        return True

    def seekable(self):
        return True

    def tell(self):
        return self.pos

    def seek(self, off, whence=0):
        self.pos = off if whence == 0 else self.pos + off if whence == 1 else self.size + off
        return self.pos

    def readinto(self, b):
        if self.pos >= self.size:
            return 0
        end = min(self.pos + len(b), self.size) - 1
        req = urllib.request.Request(self.url, headers={"Range": f"bytes={self.pos}-{end}"})
        with urllib.request.urlopen(req, timeout=60) as r:
            if r.status != 206:
                raise SystemExit(f"zipmember: {self.url} answered {r.status} to a range request")
            data = r.read()
        b[:len(data)] = data
        self.pos += len(data)
        return len(data)


def cpio_file(data, path):
    """PATH out of a newc cpio archive (070701/070702 headers, 4-byte alignment)."""
    off = 0
    while off + 110 <= len(data):
        hdr = data[off:off + 110]
        if hdr[:6] not in (b"070701", b"070702"):
            raise SystemExit("zipmember: not a newc cpio")
        try:
            size, namesize = int(hdr[54:62], 16), int(hdr[94:102], 16)
        except ValueError:
            raise SystemExit("zipmember: a broken cpio header") from None
        name = data[off + 110:off + 110 + namesize - 1].decode()
        start = (off + 110 + namesize + 3) & ~3
        if name == "TRAILER!!!":
            break
        if name.lstrip("./") == path.lstrip("/"):
            return data[start:start + size]
        off = (start + size + 3) & ~3
    raise SystemExit(f"zipmember: no {path} in the cpio")


def main(argv):
    if len(argv) not in (2, 3, 5) or (len(argv) == 5 and argv[3] != "--cpio"):
        raise SystemExit(__doc__)
    url, member = argv[0], argv[1]
    z = zipfile.ZipFile(io.BufferedReader(HttpFile(url), buffer_size=1 << 20))
    if member == "-":
        for i in z.infolist():
            print(i.file_size, i.filename)
        return
    data = z.read(member)                       # checks the member's CRC-32
    if len(argv) == 5:
        data = cpio_file(gzip.decompress(data), argv[4])
    with open(argv[2], "wb") as f:
        f.write(data)


if __name__ == "__main__":
    main(sys.argv[1:])
