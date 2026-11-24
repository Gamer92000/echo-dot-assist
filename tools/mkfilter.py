#!/usr/bin/env python3
"""The plugins' seccomp filter (docs/plugins.md 2.4) from sdk/filter.def:

    tools/mkfilter.py sdk/filter.def build/plugfilter.h     the BPF programs for src/tools/plugrun.c (ARM, x86_64)
    tools/mkfilter.py sdk/filter.def --list [arm|x86_64]    what each program allows, one call a line

Each rule returns its own verdict: a conditional jump of classic BPF reaches 255 instructions ahead at most, so
nothing jumps to a shared "allow" at the end.  Values only the launcher knows (the plugin's pid, the three pointers of
its one exec) are placeholders it patches per start: plugfilter_patches says where.

load() and judge() are for whoever else reads the table (the SDK's harness judges qemu's trace by it)."""
import re
import sys

CONST = dict(
    EPERM=1, ENOSYS=38,
    F_GETFD=1, F_SETFD=2, F_GETFL=3, F_DUPFD_CLOEXEC=1030, FIONREAD=0x541B,
    MSG_DONTWAIT=0x40, MSG_NOSIGNAL=0x4000,
    SOL_SOCKET=1, SO_TYPE=3, SO_ERROR=4, SO_SNDBUF=7, SO_RCVBUF=8, SO_KEEPALIVE=9,
    IPPROTO_IP=0, IP_TOS=1, IPPROTO_TCP=6, TCP_NODELAY=1, TCP_KEEPIDLE=4, TCP_KEEPINTVL=5, TCP_KEEPCNT=6,
    PROT_EXEC=4, MAP_PRIVATE=2, MAP_FIXED=0x10, MAP_ANONYMOUS=0x20, MAP_NORESERVE=0x4000, MAP_STACK=0x20000,
    MREMAP_MAYMOVE=1, MADV_DONTNEED=4, MADV_FREE=8,
    FUTEX_WAIT=0, FUTEX_WAKE=1, FUTEX_REQUEUE=3, FUTEX_CMP_REQUEUE=4, FUTEX_WAIT_BITSET=9, FUTEX_WAKE_BITSET=10,
    FUTEX_PRIVATE_FLAG=128, FUTEX_CLOCK_REALTIME=256,
    PR_SET_NAME=15, PR_GET_NAME=16, AT_EMPTY_PATH=0x1000,
)
PLACEHOLDERS = ["PID", "EXEC_PATH", "EXEC_ARGV", "EXEC_ENVP"]
ARCHS = {"arm": (0, 0x40000028, 32), "x86_64": (1, 0xC000003E, 64)}     # column in "nr", AUDIT_ARCH_*, pointer bits

RET_KILL, RET_ALLOW, RET_ERRNO = 0x00000000, 0x7FFF0000, 0x00050000
LD, JEQ, JGT, JA, AND, RET = 0x20, 0x15, 0x25, 0x05, 0x54, 0x06     # BPF_LD|W|ABS, JMP|JEQ|K, JMP|JGT|K, JMP|JA, ALU|AND|K, RET|K


def value(expr):
    if not re.fullmatch(r"[\w\s|&~()x]+", expr):
        raise SystemExit(f"filter.def: bad expression {expr!r}")
    return eval(expr, {"__builtins__": {}}, CONST) & 0xFFFFFFFF


def cond(text):
    """'a3 & ~(X|Y) in 0,1' -> (3, mask, [values]);  'a0 == $PID' -> (0, None, 'PID')"""
    m = re.fullmatch(r"a([0-5])\s*==\s*\$(\w+)", text)
    if m:
        if m.group(2) not in PLACEHOLDERS:
            raise SystemExit(f"filter.def: unknown ${m.group(2)}")
        return int(m.group(1)), None, m.group(2)
    m = re.fullmatch(r"a([0-5])\s*(?:&\s*(.+?))?\s+in\s+(.+)", text)
    if not m:
        raise SystemExit(f"filter.def: bad condition {text!r}")
    return int(m.group(1)), value(m.group(2)) if m.group(2) else 0xFFFFFFFF, [value(v) for v in m.group(3).split(",")]


def load(path):
    """-> { 'nr': {name: (arm, x86_64)}, 'above': (n, errno) or None, 'rules': [(pc, 'allow'|errno, name, [cond])] }"""
    t = {"nr": {}, "above": None, "rules": []}
    for no, line in enumerate(open(path), 1):
        line = line.split("#")[0].strip()
        if not line:
            continue
        w = line.split()
        pc = w[0] == "pc"
        if pc:
            w = w[1:]
            line = line.split(None, 1)[1]
        try:
            if w[0] == "nr":
                t["nr"][w[1]] = tuple(None if x == "-" else int(x, 0) for x in w[2:4])
            elif w[0] == "above":
                t["above"] = (int(w[1]), CONST[w[2]])
            elif w[0] == "errno":
                for name in w[2].split("|"):
                    t["rules"].append((pc, CONST[w[1]], name, []))
            elif w[0] == "allow":
                names, _, conds = line[len("allow"):].strip().partition(" if ")
                cs = [cond(c.strip()) for c in conds.split(" and ")] if conds else []
                for name in names.strip().split("|"):
                    t["rules"].append((pc, "allow", name, cs))
            else:
                raise SystemExit(f"{path}:{no}: {w[0]}?")
        except (IndexError, KeyError, ValueError) as e:
            raise SystemExit(f"{path}:{no}: {e!r}")
    for _, _, name, _ in t["rules"]:
        if name not in t["nr"]:
            raise SystemExit(f"{path}: no number for {name}")
    return t


def calls(t, arch):
    """the table for one architecture, in the file's order: [(number, name, errno or None, [[cond]...])]"""
    col = ARCHS[arch][0]
    out, at = [], {}
    for pc, what, name, cs in t["rules"]:
        nr = t["nr"][name][col]
        if nr is None or (pc and arch != "x86_64"):
            continue
        if nr not in at:
            at[nr] = len(out)
            out.append([nr, name, None, []])
        e = out[at[nr]]
        if what == "allow":
            e[3].append(cs)
        else:
            e[2] = what
    for nr, name, err, alts in out:
        if err is not None and alts:
            raise SystemExit(f"filter.def: {name} is both allowed and refused on {arch}")
    return out


def judge(t, arch, nr, args, known=None):
    """What the filter says to a call: 'allow', or the errno.  known: the placeholders' values, when the caller has
    them (without: a condition on one never holds)."""
    for n, _, err, alts in calls(t, arch):
        if n != nr:
            continue
        if err is not None:
            return err
        for cs in alts:
            ok = True
            for a, mask, vals in cs:
                if mask is None:
                    ok = ok and known is not None and args[a] == known.get(vals)
                else:
                    ok = ok and (args[a] & 0xFFFFFFFF & mask) in vals
            if ok:
                return "allow"
        return CONST["EPERM"]
    if arch == "arm" and t["above"] and nr > t["above"][0]:
        return t["above"][1]
    return CONST["EPERM"]


def program(t, arch):
    """-> ([(code, jt, jf, k)], [(index, placeholder number, half)])"""
    _, audit, bits = ARCHS[arch]
    ins, patches, labels = [], [], {}       # ins: [code, jt, jf, k] with label names where a jump goes
    fresh = iter(range(1 << 30))

    def emit(code, jt=0, jf=0, k=0):
        ins.append([code, jt, jf, k])

    def label(name):
        labels[name] = len(ins)

    emit(LD, k=4)
    emit(JEQ, "arch_ok", 0, audit)
    emit(RET, k=RET_KILL)
    label("arch_ok")
    emit(LD, k=0)
    todo = calls(t, arch)
    i = 0
    while i < len(todo):
        nr, name, err, alts = todo[i]
        if err is None and alts and all(not cs for cs in alts):
            # a run of calls allowed whatever their arguments: one comparison each, one verdict for the run
            run = []
            while i < len(todo) and len(run) < 120 and todo[i][2] is None and todo[i][3] and all(not cs for cs in todo[i][3]):
                run.append(todo[i][0])
                i += 1
            yes, on = f"yes{next(fresh)}", f"on{next(fresh)}"
            for n in run:
                emit(JEQ, yes, 0, n)
            emit(JA, k=on)
            label(yes)
            emit(RET, k=RET_ALLOW)
            label(on)
            continue
        i += 1
        nxt = f"next{next(fresh)}"
        emit(JEQ, 0, nxt, nr)
        if err is not None:
            emit(RET, k=RET_ERRNO | err)
        else:
            for cs in alts:
                fail = f"fail{next(fresh)}"
                for a, mask, vals in cs:
                    lo = 16 + 8 * a
                    if mask is None:                      # a placeholder: all of the argument
                        ph = PLACEHOLDERS.index(vals)
                        emit(LD, k=lo)
                        patches.append((len(ins), ph, 0))
                        emit(JEQ, 0, fail, 0)
                        if bits == 64:
                            emit(LD, k=lo + 4)
                            patches.append((len(ins), ph, 1))
                            emit(JEQ, 0, fail, 0)
                        continue
                    emit(LD, k=lo)
                    if mask != 0xFFFFFFFF:
                        emit(AND, k=mask)
                    if len(vals) == 1:
                        emit(JEQ, 0, fail, vals[0])
                    else:
                        ok = f"ok{next(fresh)}"
                        for v in vals:
                            emit(JEQ, ok, 0, v)
                        emit(JA, k=fail)
                        label(ok)
                emit(RET, k=RET_ALLOW)
                label(fail)
            emit(RET, k=RET_ERRNO | CONST["EPERM"])
        label(nxt)
        emit(LD, k=0)                                   # the conditions loaded arguments: the number again
    if arch == "arm" and t["above"]:
        emit(JGT, 0, "low", t["above"][0])
        emit(RET, k=RET_ERRNO | t["above"][1])
        label("low")
    emit(RET, k=RET_ERRNO | CONST["EPERM"])

    out = []
    for at, (code, jt, jf, k) in enumerate(ins):
        if code == JA:
            k = labels[k] - at - 1
        else:
            jt = labels[jt] - at - 1 if isinstance(jt, str) else jt
            jf = labels[jf] - at - 1 if isinstance(jf, str) else jf
        if not (0 <= jt < 256 and 0 <= jf < 256 and k >= 0):
            raise SystemExit(f"filter: a jump at {at} reaches too far ({jt}, {jf}, {k})")
        out.append((code, jt, jf, k))
    if len(out) > 4096:                                  # BPF_MAXINSNS
        raise SystemExit("filter: too long")
    return out, patches


def header(t):
    o = ["/* The plugins' seccomp filter: made by tools/mkfilter.py from sdk/filter.def, not to be edited.",
         " * plugfilter_patches: the instructions whose k the launcher sets per start (what: PLUGFILTER_*, half: 0 the",
         " * value's low 32 bits, 1 its high ones). */",
         "#ifndef PLUGFILTER_H", "#define PLUGFILTER_H", "#include <linux/filter.h>", ""]
    o += [f"#define PLUGFILTER_{p} {i}" for i, p in enumerate(PLACEHOLDERS)]
    o += ["struct plugfilter_patch { unsigned short at; unsigned char what, half; };", ""]
    for n, (arch, guard) in enumerate((("arm", "__arm__"), ("x86_64", "__x86_64__"))):
        prog, patches = program(t, arch)
        o.append(f"#{'elif' if n else 'if'} defined({guard})    /* {len(prog)} instructions */")
        o.append("static const struct sock_filter plugfilter_prog[] = {")
        o += ["    " + " ".join(f"{{0x{c:02x},{jt},{jf},0x{k:08x}}}," for c, jt, jf, k in prog[i:i + 6]) for i in range(0, len(prog), 6)]
        o.append("};")
        o.append("static const struct plugfilter_patch plugfilter_patches[] = { "
                 + " ".join(f"{{{at},{what},{half}}}," for at, what, half in patches) + " };")
    o += ["#else", "#define PLUGFILTER_NONE 1     /* no filter for this architecture: the launcher starts nothing */", "#endif", "#endif", ""]
    return "\n".join(o)


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    t = load(sys.argv[1])
    if sys.argv[2] == "--list":
        arch = sys.argv[3] if len(sys.argv) > 3 else "arm"
        for nr, name, err, alts in calls(t, arch):
            print(nr, name, "allow" if err is None else f"errno {err}", "" if all(not c for c in alts) else f"({len(alts)} rule(s) on arguments)")
        return
    open(sys.argv[2], "w").write(header(t))

if __name__ == "__main__":
    main()
