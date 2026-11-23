#!/usr/bin/env python3
"""The plugins' seccomp programs (tools/mkfilter.py from sdk/filter.def) against the table they were made from: each
program run in a small interpreter of classic BPF, on every call of the table and its neighbours with arguments on
and beside every value a rule names, and compared with judge(), which reads the table directly.  The ARM program
cannot run on the PC's kernel; this is what says that it is the table (jumps, the reload of the call's number after a
rule looked at arguments, the placeholders), on both architectures."""
import os, random, struct, sys
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import mkfilter as m

LD, JEQ, JGT, JA, AND, RET = m.LD, m.JEQ, m.JGT, m.JA, m.AND, m.RET


def run(prog, data):
    """the verdict (a RET's k) of prog on seccomp_data's bytes"""
    a, pc = 0, 0
    for _ in range(len(prog) + 1):
        code, jt, jf, k = prog[pc]
        pc += 1
        if code == LD: a = struct.unpack_from("<I", data, k)[0]
        elif code == AND: a &= k
        elif code == JEQ: pc += jt if a == k else jf
        elif code == JGT: pc += jt if a > k else jf
        elif code == JA: pc += k
        elif code == RET: return k
        else: raise AssertionError(f"an instruction the generator should not make: {code:#x}")
    raise AssertionError("the program does not end")


def verdict(k):
    return "allow" if k == m.RET_ALLOW else "kill" if k == m.RET_KILL else k & 0xFFFF if k & 0xFFFF0000 == m.RET_ERRNO else f"?{k:#x}"


def main():
    t = m.load(os.path.join(ROOT, "sdk", "filter.def"))
    fails = checks = 0
    rnd = random.Random(1)
    for arch, (_, audit, bits) in m.ARCHS.items():
        prog, patches = m.program(t, arch)
        known = {"PID": 4242, "EXEC_PATH": 0x7F0012345678 if bits == 64 else 0xB6F01234, "EXEC_ARGV": 0x7F00AAAA0000 if bits == 64 else 0xB6F0AAAA, "EXEC_ENVP": 0x7F00BBBB0010 if bits == 64 else 0xB6F0BBBB}
        prog = list(prog)
        for at, what, half in patches:                    # as the launcher patches them
            c, jt, jf, _ = prog[at]
            prog[at] = (c, jt, jf, (known[m.PLACEHOLDERS[what]] >> (32 if half else 0)) & 0xFFFFFFFF)
        # values worth trying for an argument: every one a rule names, its neighbours, single bits, the placeholders
        vals = {0, 1, 2, 0xFFFFFFFF, 0x80000000}
        for _, _, _, cs in t["rules"]:
            for _, mask, v in cs:
                if mask is None: continue
                for x in list(v) + [mask, ~mask & 0xFFFFFFFF]:
                    vals |= {x, x ^ 1, x + 1 & 0xFFFFFFFF, x | 0x10, x | 4, x | 0x40000}
        vals |= {1 << b for b in range(32)} | set(known.values()) | {v + 1 for v in known.values()} | {v ^ (1 << 40) for v in known.values() if bits == 64}
        vals = sorted(vals)
        numbers = sorted({n for n, *_ in m.calls(t, arch)} | {n + d for n, *_ in m.calls(t, arch) for d in (-1, 1) if n + d >= 0} | {0, 386, 387, 403, 435, 0xF0001, 0xF0005, 0xF0006, 5000})
        for nr in numbers:
            for trial in range(400):
                args = [rnd.choice(vals) for _ in range(6)] if trial else [0] * 6
                if trial % 3 == 1:                           # near a rule's own values: one argument off at most
                    args = [known["PID"], 129, 3, 0x22, 0, 0]; args[rnd.randrange(6)] = rnd.choice(vals)
                if trial % 7 == 2: args[:3] = [known["EXEC_PATH"], known["EXEC_ARGV"], known["EXEC_ENVP"]]
                if trial % 7 == 3: args[:3] = [known["EXEC_PATH"], known["EXEC_ARGV"] ^ (1 << (40 if bits == 64 else 3)), known["EXEC_ENVP"]]
                data = struct.pack("<iIQ6Q", nr, audit, 0, *args)
                got = verdict(run(prog, data))
                # the table compares 32 bits, a placeholder all of them: judge() the same way
                want = m.judge(t, arch, nr, [a if a in known.values() or bits == 64 and a >> 32 else a & 0xFFFFFFFF for a in args], known)
                checks += 1
                if got != want:
                    fails += 1
                    if fails < 12: print(f"FAIL {arch} call {nr} {[hex(a) for a in args]}: the program says {got}, the table {want}")
        if verdict(run(prog, struct.pack("<iIQ6Q", 3, audit ^ 1, 0, *[0] * 6))) != "kill":
            fails += 1; print(f"FAIL {arch}: another architecture is not killed")
        # what the design promises, said once in so many words (docs/plugins.md 2.4)
        say = lambda name, args: m.judge(t, arch, t["nr"][name][m.ARCHS[arch][0]], args, known)
        if arch == "arm":
            for name, args, want in (("mmap2", [0, 4096, 3, 0x22, 0xFFFFFFFF, 0], "allow"), ("mmap2", [0, 4096, 7, 0x22, 0xFFFFFFFF, 0], 1), ("mmap2", [0, 4096, 1, 0x02, 4, 0], 1),
                                     ("mmap2", [0, 4096, 3, 0x21, 0xFFFFFFFF, 0], 1), ("mmap2", [0x10000, 4096, 3, 0x32, 0xFFFFFFFF, 0], "allow"),
                                     ("clone", [0x7D0F00, 0, 0, 0, 0, 0], "allow"), ("clone", [0x50F00, 0, 0, 0, 0, 0], "allow"), ("clone", [0x3D0F00, 0, 0, 0, 0, 0], 1), ("clone", [17, 0, 0, 0, 0, 0], 1),
                                     ("sendto", [3, 0, 10, 0x4040, 0, 0], "allow"), ("sendto", [3, 0, 10, 0, 0x1000, 16], 1), ("send", [3, 0, 10, 1, 0, 0], 1),
                                     ("fcntl64", [3, 4, 0x800, 0, 0, 0], 1), ("fcntl64", [3, 3, 0, 0, 0, 0], "allow"), ("ioctl", [3, 0x5421, 0, 0, 0, 0], 1), ("ioctl", [3, 0x541B, 0, 0, 0, 0], "allow"),
                                     ("futex", [0, 6, 0, 0, 0, 0], 1), ("futex", [0, 128, 0, 0, 0, 0], "allow"), ("tgkill", [1, 1, 9, 0, 0, 0], 1), ("tgkill", [4242, 7, 6, 0, 0, 0], "allow"),
                                     ("mprotect", [0, 4096, 5, 0, 0, 0], 1), ("prctl", [4, 0, 0, 0, 0, 0], 1), ("setsockopt", [3, 1, 13, 0, 0, 0], 1), ("getsockopt", [3, 1, 17, 0, 0, 0], 1),
                                     ("execve", [known["EXEC_PATH"], known["EXEC_ARGV"], known["EXEC_ENVP"], 0, 0, 0], "allow"), ("execve", [known["EXEC_PATH"], 0, known["EXEC_ENVP"], 0, 0, 0], 1)):
                checks += 1
                if say(name, args) != want: fails += 1; print(f"FAIL the table itself: {name} {args} is {say(name, args)}, not {want}")
            for nr, want in ((5, 1), (322, 1), (281, 1), (283, 1), (296, 1), (2, 1), (190, 1), (37, 1), (26, 1), (136, 1), (403, 38), (435, 38), (0xF0002, 38), (0xF0005, "allow")):
                checks += 1                                      # open openat socket connect sendmsg fork vfork kill ptrace personality; time64, clone3: ENOSYS; cacheflush; set_tls
                if m.judge(t, "arm", nr, [0] * 6, known) != want: fails += 1; print(f"FAIL the table itself: call {nr} is {m.judge(t, 'arm', nr, [0] * 6, known)}, not {want}")
            names = {n for _, w, n, _ in t["rules"] if w == "allow"}
            for never in ("arch_prctl", "newfstatat", "getrlimit"):
                if t["nr"][never][0] is not None and m.judge(t, "arm", t["nr"][never][0], [0, 0, 0, 0x1000, 0, 0], known) == "allow":
                    fails += 1; print(f"FAIL a PC-only row ({never}) is in the ARM program")
        print(f"{arch}: {len(prog)} instructions, {len(patches)} patched")
    print(f"plugfilter: {checks} calls judged, {'all as the table says' if not fails else str(fails) + ' wrong'}")
    sys.exit(1 if fails else 0)


main()
