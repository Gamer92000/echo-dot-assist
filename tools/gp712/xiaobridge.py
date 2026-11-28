#!/usr/bin/env python3
"""The XIAO's shell held open for a whole session (tools/gp712/csltest.py): its firmware's USB serial (Zephyr's CDC
ACM) hung for good, USB too, when its port was closed while it had lines to send (2026-11-28, twice: one test
process after another), so one process opens it once, reads it all the time and serves commands on a unix socket:
a line in, the shell's answer up to its next prompt back, then the connection closes.  Everything it read goes to
the log too.

    tools/gp712/xiaobridge.py <XIAO's tty> <socket> [log]
csltest.py takes the socket in place of the tty (a path that is a socket)."""
import os
import re
import select
import socket
import sys
import termios
import threading
import time

PROMPT = b"uart:~$"


def main():
    tty, path = sys.argv[1], sys.argv[2]
    log = open(sys.argv[3], "ab", buffering=0) if len(sys.argv) > 3 else None
    st = {"buf": b"", "fd": -1}
    cv = threading.Condition()

    def opened():
        """the port, opened again when the XIAO went (a reset: USB away and back)"""
        while not os.path.exists(tty):
            time.sleep(0.2)
        fd = os.open(tty, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)    # never a write that waits on a hung XIAO
        a = termios.tcgetattr(fd)
        a[0] = a[1] = a[3] = 0
        a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        a[4] = a[5] = termios.B115200
        a[6][termios.VMIN], a[6][termios.VTIME] = 0, 1
        termios.tcsetattr(fd, termios.TCSANOW, a)
        print("port open", flush=True)
        return fd

    st["fd"] = opened()

    def gone():
        """a reset: the port's node is a new one (the old one reads nothing and takes nothing)"""
        try:
            return os.fstat(st["fd"]).st_ino != os.stat(tty).st_ino
        except OSError:
            return True

    def drain():
        while True:
            r, _, _ = select.select([st["fd"]], [], [], 0.5)
            d = b""
            if r:
                try:
                    d = os.read(st["fd"], 4096)
                except BlockingIOError:
                    continue
                except OSError:
                    d = b""
            if not d:
                if gone():
                    try:
                        os.close(st["fd"])
                    except OSError:
                        pass
                    time.sleep(0.5)
                    st["fd"] = opened()
                continue
            if log:
                log.write(d)
            with cv:
                st["buf"] = (st["buf"] + d)[-65536:]
                cv.notify_all()

    threading.Thread(target=drain, daemon=True).start()
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass
    ls = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    ls.bind(path)
    ls.listen(4)
    print("ready", flush=True)
    while True:
        c, _ = ls.accept()
        with c:
            req = b""
            while not req.endswith(b"\n"):
                d = c.recv(4096)
                if not d:
                    break
                req += d
            try:
                wait, line = req.decode().rstrip("\n").split(" ", 1)
                wait = float(wait)
            except ValueError:
                continue
            with cv:
                st["buf"] = b""
            try:
                os.write(st["fd"], line.encode() + b"\r\n")
            except OSError:
                pass
            end = time.monotonic() + wait
            with cv:
                while time.monotonic() < end:
                    b = re.sub(rb"\x1b\[[0-9;]*[A-Za-z]", b"", st["buf"]).rstrip()
                    if b.endswith(PROMPT) and len(b) > len(line) + len(PROMPT):
                        break
                    cv.wait(0.1)
                out = st["buf"]
            try:
                c.sendall(out)
            except OSError:
                pass


if __name__ == "__main__":
    main()
