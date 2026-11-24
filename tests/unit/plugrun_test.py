#!/usr/bin/env python3
"""The plugins' launcher (src/tools/plugrun.c, docs/plugins.md 2.1 to 2.4) on the PC: build/plugrun-host started as
main.sh starts it, with this script in hassmic's place on the socketpair.

    tests/unit/plugrun_test.py build/plugrun-host build/plugrun_child [build/plugprobe-host]

What it cannot show is the device's part: the user change, the SELinux domain, the CPU group, the ARM filter
(scripts/plugin-probe.sh on an Echo)."""
import array
import os
import select
import signal
import socket
import struct
import subprocess
import sys
import time

LAUNCHER, CHILD = sys.argv[1], sys.argv[2]
READY, SPAWN, SPAWNED, FAILED, KILL, EXITED, USAGE, USED = range(1, 9)
MSG = struct.Struct("=6I")
fails = 0


def check(what, ok, got=""):
    global fails
    print(("ok   " if ok else "FAIL ") + what + ("" if ok else f": {got!r}"))
    fails += not ok


class Launcher:
    """plugrun -L -- <a stand-in hassmic>: the stand-in hands its descriptor back over a socket and sleeps"""

    def __init__(self, env=None):
        a, b = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
        code = ("import os,socket,array,sys,time\n"
                "s=socket.socket(fileno=%d)\n"
                "fd=int(os.environ['HASSMIC_PLUGIN_FD'])\n"
                "s.sendmsg([b'x'],[(socket.SOL_SOCKET,socket.SCM_RIGHTS,array.array('i',[fd]))])\n"
                "os.close(fd)\n"                       # only the test holds hassmic's end now
                "time.sleep(600)\n") % b.fileno()
        self.proc = subprocess.Popen([LAUNCHER, "-L", "--", sys.executable, "-c", code], pass_fds=[b.fileno()],
                                     env={**os.environ, **(env or {})})
        b.close()
        _, anc, _, _ = a.recvmsg(1, socket.CMSG_SPACE(4))
        fd = array.array("i")
        fd.frombytes(anc[0][2][:4])
        self.s = socket.socket(fileno=fd[0])
        a.close()

    def recv(self, timeout=5):
        r, _, _ = select.select([self.s], [], [], timeout)
        if not r:
            return None
        d = self.s.recv(256)
        return MSG.unpack(d) if len(d) == MSG.size else ("eof" if not d else ("short", d))

    def spawn(self, slot, mem=32, threads=4, cls=0, prog=CHILD, raw=None, nfd=3):
        ctl, mine = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        r, w = os.pipe()
        p = os.open(prog, os.O_RDONLY)
        fds = [p, ctl.fileno(), w][:nfd]
        self.s.sendmsg([raw if raw is not None else MSG.pack(SPAWN, slot, mem, threads, cls, 0)],
                       [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array("i", fds))] if fds else [])
        os.close(p); os.close(w); ctl.close()
        return mine, os.fdopen(r, "rb", buffering=0)

    def gone(self, timeout=6):
        """the launcher ended: its end of the socket is closed"""
        end = time.time() + timeout
        while time.time() < end:
            m = self.recv(end - time.time())
            if m == "eof":
                return True
            if m is None:
                return False
        return False

    def close(self):
        self.s.close()
        self.proc.kill()
        self.proc.wait()


def lines(f, until="ready", timeout=5):
    """the plugin's stderr up to a line, as { word: value }; read unbuffered: select must see what is left"""
    out, buf = {}, getattr(f, "rest", b"")
    end = time.time() + timeout
    while time.time() < end:
        while b"\n" in buf:
            line, _, buf = buf.partition(b"\n")
            line = line.decode(errors="replace")
            for w in line.split():
                k, _, v = w.partition("=")
                out[k] = v
            if line.strip() == until:
                f.rest = buf
                return out
        r, _, _ = select.select([f], [], [], max(0, end - time.time()))
        d = os.read(f.fileno(), 4096) if r else b""
        if not d:
            break
        buf += d
    f.rest = buf
    return out


def alive(pid):
    try:
        os.kill(pid, 0)
        return open(f"/proc/{pid}/stat").read().split(")")[-1].split()[0] != "Z"
    except OSError:
        return False


# ---- the start, a plugin, its sandbox
L = Launcher()
m = L.recv()
check("READY: version 3, sandbox ok", m and m[0] == READY and m[2] == 3 and m[3] == 1, m)
ctl, err = L.spawn(2, mem=40)
m = L.recv()
check("SPAWNED for the slot, with a pid", m and m[0] == SPAWNED and m[1] == 2 and m[2] > 1, m)
pid = m[2] if m and m[0] == SPAWNED else 0
o = lines(err)
check("the plugin runs as 'plugin' with only HASSMIC_PLUGIN=1", o.get("argv0") == "plugin" and o.get("argc") == "1" and o.get("env") == "HASSMIC_PLUGIN=1", o)
for call in ("open", "socket", "fork", "execve", "kill", "setfl", "mmap_exec", "mmap_fd"):
    check(f"refused: {call}", o.get(call) == "EPERM", o.get(call))
for call in ("getfl", "mmap", "clock", "thread", "fd0"):
    check(f"allowed: {call}", o.get(call) == "ok", o.get(call))
check("the program's descriptor and the report pipe are closed in the plugin", o.get("fd4") == "closed" and o.get("fd5") == "closed", o)
check("limits: 64 descriptors, the memory asked for", o.get("nofile") == "64" and o.get("as_mb") == "40", o)
check("the plugin is a session of its own", pid and os.getsid(pid) == pid, pid and os.getsid(pid))

# ---- the probe (src/tools/plugprobe.c): every rule of the filter asked from inside, both ways
if len(sys.argv) > 3:
    ctl.send(b"exit 0"); L.recv()
    pc, pe = L.spawn(2, prog=sys.argv[3])
    m = L.recv()
    out = lines(pe, "never", 10)
    m = L.recv()
    check("the probe: every check as sdk/filter.def says", "wrong" in out and m and m[0] == EXITED and os.WIFEXITED(m[2]) and os.WEXITSTATUS(m[2]) == 0,
          (m, getattr(pe, "rest", b"")[-300:]))
    ctl, err = L.spawn(2, mem=40); L.recv(); lines(err)

# ---- a slot twice; the exit's status
c2, e2 = L.spawn(2)
m = L.recv()
check("a slot that is in use: FAILED EBUSY before any fork", m and m[0] == FAILED and m[1] == 2 and m[2] == 16 and m[3] == 0, m)
ctl.send(b"exit 7")
m = L.recv()
check("EXITED with the wait status", m and m[0] == EXITED and m[1] == 2 and os.WIFEXITED(m[2]) and os.WEXITSTATUS(m[2]) == 7, m)
check("EXITED carries the peak RSS", m and m[5] > 0, m)
ctl, err = L.spawn(2)
m = L.recv()
check("the slot is free again", m and m[0] == SPAWNED, m)
lines(err)
ctl.send(b"abort")
m = L.recv()
check("abort() reaches the plugin itself (tgkill to its own pid)", m and m[0] == EXITED and os.WIFSIGNALED(m[2]) and os.WTERMSIG(m[2]) == signal.SIGABRT, m)

# ---- a musl program (plugins are: docs/plugins.md 5.8), whose raise(), pthread_kill(), pthread_cancel() and abort()
# signal with tkill, not tgkill (a review of 6631974: refused, abort() ended as SIGSEGV, and with the development flag
# the first raise() as SIGSYS).  tests/unit/plugrun_musl.c, built with zig where there is one (PLUGRUN_MUSL)
MUSL = os.environ.get("PLUGRUN_MUSL")
if MUSL and os.path.exists(MUSL):
    for develop in (0, 1):
        how = " (the development flag)" if develop else ""
        ctl, err = L.spawn(2, prog=MUSL, raw=MSG.pack(SPAWN, 2, 32, 4, 0, develop))
        m = L.recv()
        o = lines(err)
        check(f"musl: raise() reaches its own handler{how}", m and m[0] == SPAWNED and o.get("raise") == "ok" and o.get("handled", "0") != "0", (m, o))
        check(f"musl: pthread_kill() to its own thread, and pthread_cancel(){how}", o.get("pthread_kill") == "ok" and o.get("pthread_cancel") == "ok", o)
        ctl.send(b"abort")
        m = L.recv()
        check(f"musl: abort() ends it as SIGABRT, not SIGSEGV or SIGSYS{how}", m and m[0] == EXITED and os.WIFSIGNALED(m[2]) and os.WTERMSIG(m[2]) == signal.SIGABRT,
              (m, m and os.WIFSIGNALED(m[2]) and signal.Signals(os.WTERMSIG(m[2])).name))
else:
    print("skip musl: no PLUGRUN_MUSL program (zig cc builds tests/unit/plugrun_musl.c: make unit-plugrun with zig on PATH)")

# ---- limits clamped, something that is no program
ctl, err = L.spawn(0, mem=4000, threads=99)
L.recv()
o = lines(err)
check("memory clamped to 96 MB", o.get("as_mb") == "96", o)
ctl.send(b"exit 0"); L.recv()
ctl, err = L.spawn(1, prog="/etc/hostname")
m = L.recv()
check("not a program: FAILED at the exec (step 9)", m and m[0] == FAILED and m[1] == 1 and m[3] == 9, m)
m = L.recv()
check("and its EXITED frees the slot", m and m[0] == EXITED and m[1] == 1, m)

# ---- KILL: SIGTERM, then SIGKILL after 2 s
ctl, err = L.spawn(3)
m = L.recv(); pid = m[2]
lines(err)
ctl.send(b"spin"); lines(err, "spinning")
t = time.time()
L.s.send(MSG.pack(KILL, 3, 0, 0, 0, 0))
m = L.recv(6)
check("KILL: a plugin that ignores SIGTERM is killed", m and m[0] == EXITED and m[1] == 3 and os.WIFSIGNALED(m[2]) and os.WTERMSIG(m[2]) == signal.SIGKILL, m)
check("... 2 s later", 1.5 < time.time() - t < 4, time.time() - t)
L.s.send(MSG.pack(KILL, 5, 0, 0, 0, 0))
check("KILL for an empty slot: nothing", L.recv(0.5) is None)

# ---- USAGE: a plugin's CPU time, read by the launcher (its parent, and root on an Echo, where hassmic cannot)
def usage(slot):
    L.s.send(MSG.pack(USAGE, slot, 0, 0, 0, 0))
    return L.recv(3)
m = usage(5)
check("USAGE for an empty slot: USED with no pid and nothing read", m and m[0] == USED and m[1] == 5 and m[2] == 0 and m[5] == 0, m)
ctl, err = L.spawn(3)
pid = L.recv()[2]
lines(err)
a = usage(3); time.sleep(1.0); b = usage(3)
check("USED: the slot's pid, read", a and b and a[0] == b[0] == USED and a[1] == 3 and a[2] == b[2] == pid and a[5] == b[5] == 1, (a, b))
check("a plugin that waits: next to no CPU in a second, on the launcher's clock", a and b and b[3] - a[3] < 100 and 900 < b[4] - a[4] < 1500, (a, b))
ctl.send(b"burn"); lines(err, "burning")
a = usage(3); time.sleep(1.5); b = usage(3)
check("one that spins, in a thread beside its first: its CPU time grows with the clock (1.5 s: 1.2 s and more)", a and b and 1200 <= b[3] - a[3] <= b[4] - a[4] + 100 and 1400 < b[4] - a[4] < 2200, (a, b))
check("the launcher only reports: the plugin runs on", alive(pid) and L.recv(0.3) is None)
L.s.send(MSG.pack(KILL, 3, 0, 0, 0, 0))
m = L.recv(6)
check("(ended)", m and m[0] == EXITED and m[1] == 3, m)
ctl, err = L.spawn(3); L.recv(); lines(err)
t = time.time()
L.s.send(MSG.pack(KILL, 3, 0, 0, 0, 0))
m = L.recv(4)
check("KILL: a plugin without a handler of its own ends on SIGTERM, at once", m and m[0] == EXITED and os.WIFSIGNALED(m[2]) and os.WTERMSIG(m[2]) == signal.SIGTERM and time.time() - t < 1, (m, time.time() - t))

# ---- the development flag (SPAWN's d, version 3): a refused call raises SIGSYS instead of answering EPERM.  The
# child's first word is a refused open: with the flag it never says it
ctl, err = L.spawn(4, raw=MSG.pack(SPAWN, 4, 32, 4, 0, 1))
m = L.recv()
check("SPAWNED with the development flag", m and m[0] == SPAWNED and m[1] == 4, m)
m = L.recv()
o = lines(err, "ready", 2)
check("the development flag: the first refused call ends it with SIGSYS", m and m[0] == EXITED and m[1] == 4 and os.WIFSIGNALED(m[2]) and os.WTERMSIG(m[2]) == signal.SIGSYS and "open" not in o, (m, o))
ctl, err = L.spawn(4, raw=MSG.pack(SPAWN, 4, 32, 4, 0, 2))
m = L.recv()
o = lines(err)
check("a flag it does not know: none (EPERM as ever)", m and m[0] == SPAWNED and o.get("open") == "EPERM", (m, o))
ctl.send(b"exit 0"); L.recv()

# ---- the socket closes: the launcher ends and takes its plugins along
ctl, err = L.spawn(4)
pid = L.recv()[2]
lines(err)
L.s.close()
end = time.time() + 5
while alive(pid) and time.time() < end:
    time.sleep(0.05)
check("the socket closed: the plugin is gone", not alive(pid))
L.proc.kill(); L.proc.wait()

# ---- hassmic is gone though a copy of the descriptor lives on
L = Launcher(); L.recv()
ctl, err = L.spawn(0)
pid = L.recv()[2]
lines(err)
L.proc.kill(); L.proc.wait()
end = time.time() + 6
while alive(pid) and time.time() < end:
    time.sleep(0.05)
check("the process started as hassmic is gone: the plugin too, within 2 s and a bit", not alive(pid))
check("... and the launcher", L.gone(3))
L.close()

# ---- not the protocol: each ends the launcher
for what, send in (
        ("a short message", lambda L: L.s.send(b"\x02\x00\x00")),
        ("a long message", lambda L: L.s.send(MSG.pack(KILL, 0, 0, 0, 0, 0) + b"x")),
        ("an unknown type", lambda L: L.s.send(MSG.pack(99, 0, 0, 0, 0, 0))),
        ("a slot out of range", lambda L: L.s.send(MSG.pack(KILL, 8, 0, 0, 0, 0))),
        ("SPAWN with two descriptors", lambda L: L.spawn(0, nfd=2)),
        ("SPAWN without descriptors", lambda L: L.spawn(0, nfd=0)),
        ("KILL with descriptors", lambda L: L.spawn(0, raw=MSG.pack(KILL, 0, 0, 0, 0, 0))),
        ("USAGE with descriptors", lambda L: L.spawn(0, raw=MSG.pack(USAGE, 0, 0, 0, 0, 0))),
        ("USAGE for a slot out of range", lambda L: L.s.send(MSG.pack(USAGE, 8, 0, 0, 0, 0))),
        ("the launcher's own answer to it", lambda L: L.s.send(MSG.pack(USED, 0, 0, 0, 0, 0))),
        ("a launcher's own message", lambda L: L.s.send(MSG.pack(EXITED, 0, 0, 0, 0, 0)))):
    L = Launcher(); L.recv()
    keep, e = L.spawn(6)
    pid = L.recv()[2]
    lines(e)
    send(L)
    check(f"{what}: the launcher ends", L.gone())
    end = time.time() + 3
    while alive(pid) and time.time() < end:
        time.sleep(0.05)
    check("... and its plugin with it", not alive(pid))
    L.close()

# ---- no filter: no plugin sees it (the PC's stand-in for a kernel without one)
L = Launcher({"HASSMIC_PLUGIN_SANDBOX": "broken"})
m = L.recv()
check("a kernel whose filter does not hold: READY says so", m and m[0] == READY and m[3] == 0, m)
keep, e = L.spawn(0)
m = L.recv()
check("... and nothing is started there, whoever asks (never unsandboxed)", m and m[0] == FAILED and m[2] == 38 and m[3] == 0, m)
L.close()
L = Launcher({"HASSMIC_PLUGIN_SANDBOX": "0"})
m = L.recv()
ctl, err = L.spawn(0)
L.recv()
o = lines(err)
check("HASSMIC_PLUGIN_SANDBOX=0 (PC only): no filter", m and m[3] == 1 and o.get("socket") == "ok", o)
L.close()

# ---- -T: the launcher by itself, for the probe on an Echo (scripts/plugin-probe.sh)
if len(sys.argv) > 3:
    r = subprocess.run([LAUNCHER, "-T", sys.argv[3]], capture_output=True, text=True, timeout=30)
    check("-T: the program's lines and its exit status come through", r.returncode == 0 and "wrong 0" in r.stderr and "the kernel's filter holds" in r.stderr, (r.returncode, r.stderr[-300:]))
    r = subprocess.run([LAUNCHER, "-T", "-D", sys.argv[3]], capture_output=True, text=True, timeout=30)
    check("-T -D: the development flag, the probe stopped at its first refused call", r.returncode == 128 + signal.SIGSYS and "SIGSYS" in r.stderr, (r.returncode, r.stderr[-300:]))
    r = subprocess.run([LAUNCHER, "-T", "/etc/hostname"], capture_output=True, text=True, timeout=30)
    check("-T: something that is no program: 254 and the step", r.returncode == 254 and "step 9" in r.stderr, (r.returncode, r.stderr[-200:]))

# ---- without -L's socket the command still runs
r = subprocess.run([LAUNCHER, "-L", "--", "sh", "-c", "exit 5"])
check("the command's own exit status comes through", r.returncode == 5, r.returncode)

print(f"plugrun: {'all passed' if not fails else str(fails) + ' failed'}")
sys.exit(1 if fails else 0)
