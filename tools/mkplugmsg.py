#!/usr/bin/env python3
"""The broker protocol's wire format from sdk/messages.def (docs/plugins.md 3.1, 5.8):

    tools/mkplugmsg.py            writes sdk/plugmsg.h, the message reference inside sdk/PROTOCOL.md and the test
                                  vectors sdk/vectors/<MESSAGE>.vec, and records in sdk/scopes.lock each scope version
                                  messages.def declares that it has not recorded yet
    tools/mkplugmsg.py --check    fails when any of them is not what the definition makes, or the definition not what
                                  sdk/scopes.lock recorded of a version (make unit-plugmsg)
    tools/mkplugmsg.py --c OUT    writes OUT: a C program that holds plugmsg.h's encoder and decoder to the vectors as
                                  they are on file (make unit-plugmsg runs it)
    --def F, --lock F             another definition and lock (tests/unit/plugmsg_scopes: the rules proved on one)
    --history                     only the definition's rules and the lock: fails on a version recorded otherwise,
                                  or declared and not recorded (with --record: recorded instead)
    --released                    only the lock and the definition against every release's lock (CI): fails where a
                                  version a release carried is otherwise here, or gone where a release this tree
                                  descends from carried it, where two releases carried one otherwise, and where they
                                  cannot all be read: no git, no release tag, a shallow clone (which tags this tree
                                  descends from cannot be told past its boundary), a tag whose sdk/scopes.lock is no
                                  file (a directory, a symbolic link, a submodule) or has a line that is no lock's.  Any other run of this tree's lock holds it the same where git and a tag with
                                  a lock are, says "skipped" where it cannot (shallow: skipped as a whole), and names
                                  each tag whose lock it could not read
    --header OUT                  only the header, into OUT
    --vectors DIR                 only the test vectors, into DIR

Versions (sdk/PROTOCOL.md "Versions"): every message, field, enum value, constant and manifest key belongs to a
version of a scope, a scope's next version only adds, and each version above 1 is marked as one that widens what its
capability lets a plugin do or one within.  What a recorded version had is in sdk/scopes.lock, one line per thing it
had, as a snapshot of that version: a version of messages.def that would make any recorded snapshot otherwise edits
the history of a version a release may have carried, and fails.  The lock is never rewritten here, and what every
release (a git tag v*: betas of main and releases of the release branch alike) carried is held to, each version
exactly as it carried it, in the lock and in messages.def: only a version no release carried may be deleted by hand
and recorded anew (finding 1 of a review of 4e0a202; findings 1 and 2 of a review of 60a8589: the newest tag by its
time alone, and its lines as a subset, let a released version change).

plugmsg.h is all of the format in C, header-only: hassmic's broker and the C client include the same file, and one
fuzz target covers it (tests/unit/plugmsg_test.c).  Every type is a struct and a bounds check: no allocation, strings
point into the packet, a list is walked in place."""
import itertools
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEF, HDR, DOC, LOCK = (os.path.join(ROOT, "sdk", f) for f in ("messages.def", "plugmsg.h", "PROTOCOL.md", "scopes.lock"))
# the greeting: read before any version is known, so it never grows
GREETING = ("HELLO", "WELCOME", "REFUSED")
# what the greeting's lists hold at most, for good (check_rules): every scope there will ever be
SCOPES_MAX = 255
# a version's words (version lines' "what it adds"), at most: the install dialog and an update's lines show every
# widening version's words up to the one required (plugin_store.c), each line whole
VERSION_DOC_MAX = 300
# the bits of pm_caps_t (hassmic's set of capabilities, and the C client's: never on the wire, so wider whenever more
# capabilities come; check_rules says when)
CAPS_MAX = 64
VEC = os.path.join(ROOT, "sdk", "vectors")
SIZES = {"u8": 1, "u16": 2, "u32": 4, "u64": 8}
BEGIN, END = "<!-- messages: made by tools/mkplugmsg.py from messages.def -->", "<!-- messages: end -->"


def since_of(tok, what):
    """'name@N' -> (name, N); 'name' -> (name, 1)"""
    if "@" not in tok:
        return tok, 1
    n, v = tok.split("@", 1)
    if not re.fullmatch(r"[1-9][0-9]{0,4}", v):
        raise ValueError(f"{what}: @{v} is no version")
    return n, int(v)


def load(path=None):
    """the definition: (scopes, consts, enums, msgs, versions, keys, falls).  scopes [(name, version, doc)]; consts
    [(name, value, scope, since)]; enums [(name, [(value name, value, since)], scope)]; msgs [{name, nr, dir, scopes:
    [(scope, since)], drop (marked: it may go to an older peer not at all), doc, fields}], a field {type, name, max, doc, since (None in a message of several scopes: its
    first layout), enum (the enum its values are, or None), fields}; versions {(scope, N): (widens, doc)} for every
    version above 1 (widens: True or False); keys [(context, key, scope, since)] of the manifest; falls {(enum, value
    name): the value name a peer of an earlier version gets instead, or "-": none, the packet is not sent to it} for
    every value of a version above 1 (`B=2@2>A`, `C=4@2>-`).
    Each rule of the file's head is held here: SystemExit with where and why"""
    path = path or DEF
    scopes, consts, enums, msgs, stack, versions, keys, falls = [], [], [], [], [], {}, [], {}
    for no, raw in enumerate(open(path), 1):
        line = raw.split("#")[0].rstrip() if '"' not in raw else raw.rstrip()
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        depth = (len(line) - len(line.lstrip())) // 4
        m = re.match(r'\s*(\S+)\s+(.*?)\s*(?:"(.*)")?$', line)
        kind, rest, doc = m.group(1), m.group(2).split(), m.group(3) or ""
        try:
            if depth == 0 and kind == "scope":
                if not re.fullmatch(r"[a-z][a-z0-9._]{0,23}", rest[0]) or not re.fullmatch(r"[1-9][0-9]{0,4}", rest[1]) or len(rest) != 2:
                    raise ValueError("scope NAME VERSION: a name of 1 to 24 of a-z, 0-9, . and _, a version from 1")
                scopes.append((rest[0], int(rest[1]), doc))
            elif depth == 0 and kind == "const":
                sc, v = since_of(rest[1], rest[0])
                consts.append((rest[0], int(rest[2]), sc, v))
            elif depth == 0 and kind == "enum":
                vals = []
                for x in rest[2:]:
                    k, eq, rhs = x.partition("=")
                    rhs, gt, fb = rhs.partition(">")
                    if not eq or (gt and not fb):
                        raise ValueError(f"enum {rest[0]}: {x!r} is no NAME=VALUE[@N[>FALLBACK]]")
                    num, sv = since_of(rhs, x)
                    vals.append((k, int(num), sv))
                    if gt: falls[(rest[0], k)] = fb
                enums.append((rest[0], vals, rest[1]))
            elif depth == 0 and kind == "version":
                if len(rest) != 3 or not re.fullmatch(r"[1-9][0-9]{0,4}", rest[1]) or rest[2] not in ("widens", "within") or not doc:
                    raise ValueError('version SCOPE N widens|within "what it adds": a mark for every version above 1')
                if (rest[0], int(rest[1])) in versions:
                    raise ValueError(f"version {rest[0]} {rest[1]} twice")
                versions[(rest[0], int(rest[1]))] = (rest[2] == "widens", doc)
            elif depth == 0 and kind == "keys":
                if len(rest) < 3 or not re.fullmatch(r"[a-z][a-z0-9._]{0,39}", rest[0]):
                    raise ValueError("keys CONTEXT SCOPE[@N] KEY ...: a context of a-z, 0-9, . and _")
                sc, v = since_of(rest[1], rest[0])
                for k in rest[2:]:
                    if not re.fullmatch(r"[a-z][a-z0-9_]{0,23}", k):
                        raise ValueError(f"keys {rest[0]}: {k!r} is no key (a-z, 0-9 and _, from a letter, 24 at most)")
                    keys.append((rest[0], k, sc, v))
            elif depth == 0 and kind == "msg":
                if len(rest) not in (4, 5) or rest[4:] not in ([], ["drop"]):
                    raise ValueError('msg NAME NUMBER DIR SCOPE[@N][|SCOPE[@N]] [drop] "what it is"')
                sc = [since_of(t, rest[0]) for t in rest[3].split("|")]
                msgs.append({"name": rest[0], "nr": int(rest[1]), "dir": rest[2], "scopes": sc, "drop": len(rest) == 5, "doc": doc, "fields": []})
                stack = [msgs[-1]["fields"]]
            elif depth >= 1 and (kind == "list" or kind in SIZES or kind in ("str", "bytes")):
                at = [t for t in rest[1:] if t.startswith("@")]
                en = [t[5:] for t in rest[1:] if t.startswith("enum:")]
                plain = [t for t in rest[1:] if not t.startswith("@") and not t.startswith("enum:")]
                since = since_of("x" + at[0], rest[0])[1] if at else None
                if len(at) > 1 or (kind in ("list", "str", "bytes")) != (len(plain) == 1) or len(plain) > 1 or len(en) > 1 or (en and kind not in SIZES):
                    raise ValueError(f"{kind} {rest[0]}: TYPE name [MAX] [enum:ENUM] [@N] (an enum's values only in a number)")
                f = {"type": kind, "name": rest[0], "max": int(plain[0]) if plain else 0, "doc": doc, "since": since, "enum": en[0] if en else None, "fields": []}
                stack = stack[:depth]
                stack[-1].append(f)
                if kind == "list":
                    stack.append(f["fields"])
            else:
                raise ValueError(kind)
        except (IndexError, ValueError) as e:
            raise SystemExit(f"{path}:{no}: {e}")
    check_rules(path, scopes, consts, enums, msgs, versions, keys, falls)
    return scopes, consts, enums, msgs, versions, keys, falls


def cap_name(scope):
    """a scope's CAP_* name: net.lan -> NET_LAN"""
    return scope.upper().replace(".", "_")


def check_rules(path, scopes, consts, enums, msgs, versions=None, keys=(), falls=None):
    """the rules a definition keeps whatever the lock says (sdk/PROTOCOL.md "Versions"); fills in each field's since"""
    def no(why):
        raise SystemExit(f"{path}: {why}")
    versions, falls = versions or {}, falls or {}
    have = dict((n, v) for n, v, _ in scopes)
    if len(have) != len(scopes):
        no("a scope declared twice")
    if "core" not in have:
        no("no scope core")
    # The greeting names every scope a client has (HELLO) and every one spoken (WELCOME), each in a list of at most
    # SCOPES_MAX: the greeting never grows, so that bound is the protocol's for good (and a scope's version is a u16)
    if len(scopes) > SCOPES_MAX:
        no(f"{len(scopes)} scopes: the greeting names {SCOPES_MAX} at most, for good")
    for n, v, _ in scopes:
        if v > 65535:
            no(f"scope {n} {v}: a version is a u16 on the wire")
    # every version above 1 says whether it widens what its capability lets a plugin do (docs/plugins.md 3.3): an
    # update that comes to require a widening version asks for the button, whatever else it is.  No default: the
    # word is the author's of the version, said once, and recorded with it
    for n, top, _ in scopes:
        for v in range(2, top + 1):
            if (n, v) not in versions:
                no(f"scope {n} {v}: no line 'version {n} {v} widens|within \"...\"' (whether it lets a plugin do more than its capability's line says)")
    for (n, v), (_, d) in versions.items():
        if len(d.encode()) > VERSION_DOC_MAX:
            no(f"version {n} {v}: its words are {len(d.encode())} bytes, {VERSION_DOC_MAX} at most (shown whole in the install dialog)")
    cap = next((vals for n, vals, _ in enums if n == "CAP"), [])
    caps = {k: v for k, v, _ in cap}
    for n in have:
        if n != "core" and n.upper().replace(".", "_") not in caps:
            no(f"scope {n} has no capability bit CAP_{n.upper().replace('.', '_')} (a scope is core or a capability)")
    for k, v, sv in cap:
        if not any(n.upper().replace(".", "_") == k for n in have):
            no(f"CAP_{k} has no scope")
        # (a capability's value is its scope's, from its version 1: never a later version's of core.  Below CAPS_MAX:
        # the bits of pm_caps_t, hassmic's own and never on the wire)
        if sv != 1:
            no(f"CAP_{k} @{sv}: a capability's value comes with its scope, never with a version")
        if not 0 <= v < CAPS_MAX:
            no(f"CAP_{k} = {v}: the bits of pm_caps_t are 0 to {CAPS_MAX - 1} (internal: make it wider in mkplugmsg.py, CAPS_MAX)")
    for ctx, k, sc, v in keys:
        if sc not in have:
            no(f"keys {ctx}: no scope {sc}")
        if v > have[sc]:
            no(f"keys {ctx} {k}: @{v}, and scope {sc} is at {have[sc]}")
    if len({(c, k) for c, k, _, _ in keys}) != len(keys):
        no("a key of the manifest twice in one context")

    def at(scope, v, what):
        if scope not in have:
            no(f"{what}: no scope {scope}")
        if v > have[scope]:
            no(f"{what}: @{v}, and scope {scope} is at {have[scope]}")
    for n, _, sc, v in consts:
        at(sc, v, f"const {n}")
    for (n, v) in versions:
        at(n, v, f"version {n} {v}")
        if v < 2:
            no(f"version {n} {v}: no such version of a scope above 1 (version 1 is the capability's own line)")
    for n, vals, sc in enums:
        at(sc, 1, f"enum {n}")
        for k, _, v in vals:
            at(sc, v, f"enum {n} {k}")
        if len({v for _, v, _ in vals}) != len(vals) or len({k for k, _, _ in vals}) != len(vals):
            no(f"enum {n}: a value or a name twice")
        # A value a later version brought names the value a peer of an earlier version gets in its place (finding 4
        # of a review of 60a8589): a sender maps through it (pm_<enum>_for, pm_fit) and never drops the packet for it,
        # so a version-1 plugin still hears that its call ended when a later hassmic has a new reason for it.  The
        # fallback is a value of the same enum from an earlier version (a chain ends at version 1), recorded with
        # the value: which value an older peer gets is part of what that version is.  Or "-" (`C=4@2>-`): no value of
        # an earlier version is the right one, and the packet is not sent to such a peer at all (finding 3 of a review
        # of 4c55b54: a key of the buttons that a button-1 plugin heard as the action button would be a press that
        # never happened; the authoring rule is the file's head).  A chain may end in it: C=4@3>B, B=3@2>-
        since = {k: sv for k, _, sv in vals}
        for k, num, sv in vals:
            fb = falls.get((n, k))
            # (PM_NOT_SENT, pm_<enum>_for's word for "-", is the largest u64: never a value)
            if num >= (1 << 64) - 1:
                no(f"enum {n} {k}: {num} is no value (the largest u64 is pm_<enum>_for's PM_NOT_SENT)")
            if sv > 1 and not fb:
                no(f"enum {n} {k} @{sv}: a value of a later version names the value a peer of an earlier version gets instead ({k}=...@{sv}>OLDER, or >- where none is right)")
            if fb and sv == 1:
                no(f"enum {n} {k}: a value of version 1 has no fallback (every peer of the scope speaks it)")
            if fb and fb != "-" and fb not in since:
                no(f"enum {n} {k}: its fallback {fb} is no value of {n}")
            if fb and fb != "-" and since[fb] >= sv:
                no(f"enum {n} {k} @{sv}: its fallback {fb} is of version {since[fb]}, not of an earlier one")
    if len({m["nr"] for m in msgs}) != len(msgs) or len({m["name"] for m in msgs}) != len(msgs):
        no("a message's number or name twice")
    for m in msgs:
        if m["dir"] not in "<>=" or len(m["dir"]) != 1:
            no(f"{m['name']}: its direction is >, < or =")
        if len({sc for sc, _ in m["scopes"]}) != len(m["scopes"]):
            no(f"{m['name']}: a scope twice")
        for sc, v in m["scopes"]:
            at(sc, v, m["name"])
        one = len(m["scopes"]) == 1
        base = m["scopes"][0][1]
        evals = {n: (vals, sc) for n, vals, sc in enums}
        for f in m["fields"]:
            if m["name"] in GREETING and f["type"] == "list" and f["name"] == "scopes" and f["max"] != SCOPES_MAX:
                no(f"{m['name']} scopes: a list of {SCOPES_MAX} (every scope there will ever be: the greeting never grows)")
        for f in flat(m["fields"]):
            if f["enum"] is None:
                continue
            if f["enum"] not in evals:
                no(f"{m['name']} {f['name']}: no enum {f['enum']}")
            vals, esc = evals[f["enum"]]
            if any(v >= 1 << 8 * SIZES[f["type"]] or v < 0 for _, v, _ in vals):
                no(f"{m['name']} {f['name']}: a value of {f['enum']} does not fit a {f['type']}")
            # (the version an enum's value is held to is its scope's: one the message's peer speaks.  CAP's values
            # are each its own capability's, named by any plugin that knows the capability.  A message of several
            # scopes is spoken by a peer of any one of them: an enum of one of those scopes is one the others' peers
            # may not speak at all, and each such packet would be malformed to them (finding 3 of a review of
            # 60a8589): only core's enums and CAP there)
            if f["enum"] != "CAP" and esc != "core" and esc not in dict(m["scopes"]):
                no(f"{m['name']} {f['name']}: enum {f['enum']} is {esc}'s, and the message is not of {esc} or core")
            if f["enum"] != "CAP" and esc != "core" and len(m["scopes"]) > 1:
                no(f"{m['name']} {f['name']}: enum {f['enum']} is {esc}'s, and a message of several scopes holds only core's enums and CAP (a peer of another of its scopes may not speak {esc})")
        # (`drop`: the message may be left unsent whole, to a peer whose version has no word for a value it holds.
        # Never the greeting, which is read before a version is known, nor OK or ERR, which answer a request: an
        # answer that does not come leaves its asker waiting)
        # Nor a plugin's own message (>): what a plugin sends is never left unsent for a value (sdk/PROTOCOL.md
        # "Versions": its later values go as their fallbacks), and the clients' answer for one that is is
        # HM_ERR_NOT_SPOKEN, which says the message is not spoken at all (a review of 6631974: mkplugmsg took `drop`
        # on a `>` message)
        if m["drop"] and (m["name"] in GREETING or m["dir"] != "<"):
            no(f"{m['name']}: marked drop, and it is " + ("the greeting (read before a version is known)" if m["name"] in GREETING
                                                          else "an answer either way (its asker waits for it)" if m["dir"] == "="
                                                          else "a plugin's own message (>: never left unsent for a value; only what hassmic tells a plugin, <, may be)"))

        def walk(fields, low, where):
            """LOW: the version of the struct the fields are in (the message's, or its list's): an unmarked field's"""
            last = low
            for f in fields:
                if f["since"] is not None and (not one or m["name"] in GREETING):
                    no(f"{m['name']}: {where}{f['name']} @{f['since']}: " + ("the greeting never grows (it is read before a version is known)" if one else
                       "a message of several scopes never grows (which of its scopes' versions would say what is there?): what grows is a new message of the scope that needs it"))
                if f["since"] is None:
                    f["since"] = low if one else None
                if one and f["since"] < last:
                    no(f"{m['name']}: {where}{f['name']} @{f['since']} comes after a field of version {last}: a field is appended after every field of an earlier version")
                if one:
                    at(m["scopes"][0][0], f["since"], f"{m['name']} {where}{f['name']}")
                    last = f["since"]
                if f["type"] == "list":
                    walk(f["fields"], f["since"] if one else None, f"{where}{f['name']}.")
        walk(m["fields"], base, "")
    # `>-` only where the enum is what the message is about (finding 1 of a review of 61e3d8e): a packet that is not
    # sent to an older peer is lost to it whole, so the value's message must be one whose whole point is that value,
    # an event the peer was never promised (a key of the buttons that a button-1 plugin does not have: BUTTON), and
    # marked so (`drop`, recorded in the lock with it).  Never an answer (ERR dropped: the C and Rust clients waited
    # for it with only PINGs answering, and the watchdog never fired), an end (CALL_END, CALL_HANGUP: the call's
    # claim held, the far side left connected), a state (STATE, MUSIC_STATE: the last one told stands, stale) or a
    # reason or qualifier of something else (FROM, MWHY, REQSRC: the action, the command, the request lost with it,
    # while action_send and entity_set told Home Assistant it went): there a new value names the earlier one it is a
    # case of, and one that is a case of none is a new message
    carriers = {}
    for m in msgs:
        for f in flat(m["fields"]):
            if f["enum"] is not None and m["name"] not in carriers.setdefault(f["enum"], []):
                carriers[f["enum"]].append(m["name"])
    drops = {m["name"] for m in msgs if m["drop"]}
    for (n, k), fb in falls.items():
        for c in carriers.get(n, []) if fb == "-" else []:
            if c not in drops:
                no(f"enum {n} {k}: >- (not sent to a peer of an earlier version), and {c} holds {n} and is not marked drop: "
                   f"a message that must arrive (an answer, an end, a state, a reason) never goes unsent: {k} names the earlier value it is a case of, or it is a new message")
    # (0xffff of ERR is the clients' own word for a request not sent because it is not of what is spoken with
    # hassmic: hassmic_plugin.h HM_ERR_NOT_SPOKEN, the Rust client's ERR_NOT_SPOKEN; never a value on the wire)
    for n, vals, _ in enums:
        if n == "ERR" and any(v == 0xffff for _, v, _ in vals):
            no("enum ERR: 65535 is the clients' own HM_ERR_NOT_SPOKEN, never a value of the wire's")


def ctype(f):
    return {"u8": "uint8_t", "u16": "uint16_t", "u32": "uint32_t", "u64": "uint64_t", "str": "struct pm_str", "bytes": "struct pm_str", "list": "struct pm_list"}[f["type"]]


def gen_struct(o, name, fields):
    for f in fields:
        if f["type"] == "list":
            gen_struct(o, f"{name}_{f['name']}", f["fields"])
    o.append(f"struct pm_{name} {{ " + " ".join(f"{ctype(f)} {f['name']};" for f in fields) + " };")


def versioned(m):
    """a message whose layout depends on its scope's version: of one scope, with a field of a later version than
    the message's own somewhere in it.  Its reader and writer take that version (V); every other keeps its one
    layout and no such argument, so a message that grows changes its functions' signature and the compiler finds
    every caller that has to say which version it speaks"""
    def later(fields, low):
        return any(f["since"] is not None and f["since"] > low or (f["type"] == "list" and later(f["fields"], f["since"] or low)) for f in fields)
    return len(m["scopes"]) == 1 and later(m["fields"], m["scopes"][0][1])


def gated(f, low, stmt, vm):
    """STMT only from the field's version on (in a versioned message, for a field later than the struct it is in)"""
    return f"if (v >= {f['since']}) {{ {stmt} }}" if vm and f["since"] > low else stmt


def read_fields(fields, r, m, prefix, vm=False, low=1):
    """statements that read a struct from reader r into *m: its fields in order, a list's being its count; then the
    elements of its lists, in order, each walked once (which is what validates them).  VM: the message is versioned
    (v in scope: the version spoken), LOW the struct's own version"""
    s = []
    for f in fields:
        if f["type"] == "str":
            x = f"{m}->{f['name']} = pm_rstr({r}, {f['max']});"
        elif f["type"] == "bytes":
            x = f"{m}->{f['name']} = pm_rbytes({r}, {f['max']});"
        elif f["type"] == "list":
            x = f"{m}->{f['name']}.n = (uint16_t)pm_rn({r}, 2); if ({m}->{f['name']}.n > {f['max']}) ({r})->bad = 1;" 
        else:
            x = f"{m}->{f['name']} = ({ctype(f)})pm_rn({r}, {SIZES[f['type']]});"
        s.append(gated(f, low, x, vm))
    for f in fields:
        if f["type"] == "list":
            s.append(gated(f, low, f"{m}->{f['name']}.r = *{r}; "
                     f"for (unsigned i_ = {m}->{f['name']}.n; i_ && !({r})->bad; i_--) pm_skip_{prefix}_{f['name']}({r}{', v' if vm else ''});", vm))
    return " ".join(s)


def write_fields(fields, m, vm=False, low=1):
    s = []
    for f in fields:
        if f["type"] == "str":
            x = f"pm_wstr(w, {m}->{f['name']}, {f['max']});"
        elif f["type"] == "bytes":
            x = f"pm_wbytes(w, {m}->{f['name']}, {f['max']});"
        elif f["type"] == "list":
            x = f"if ({m}->{f['name']}.n > {f['max']}) w->over = 1; pm_wn(w, {m}->{f['name']}.n, 2);"
        else:
            x = f"pm_wn(w, {m}->{f['name']}, {SIZES[f['type']]});"
        s.append(gated(f, low, x, vm))
    return " ".join(s)


def gen_lists(o, name, fields, vm=False, low=1):
    """per list of a message: skip (the walk that validates), next (an element out of a list), add (one into a packet).
    In a versioned message each takes the version spoken"""
    for f in fields:
        if f["type"] != "list":
            continue
        n, fl = f"{name}_{f['name']}", f["since"] if vm else 1
        gen_lists(o, n, f["fields"], vm, fl)
        va, vd = (", unsigned v", "") if vm else ("", "")
        o.append(f"static inline void pm_skip_{n}(struct pm_r *r{va}) {{ struct pm_{n} e_; " + ("memset(&e_, 0, sizeof e_); " if vm else "") + f"{read_fields(f['fields'], 'r', '(&e_)', n, vm, fl)} (void)e_; }}")
        o.append(f"static inline int pm_next_{n}(struct pm_list *l, struct pm_{n} *e{va}) {{ " + ("memset(e, 0, sizeof *e); " if vm else "") + "if (!l->n) return 0; l->n--; "
                 f"{read_fields(f['fields'], '(&l->r)', 'e', n, vm, fl)} return !l->r.bad; }}")
        o.append(f"static inline void pm_add_{n}(struct pm_w *w, const struct pm_{n} *e{va}) {{ {write_fields(f['fields'], 'e', vm, fl)} }}")


def enum_checks(fields, m, vm, low):
    """statements of pm_check: each field of FIELDS that holds an enum's values holds one the peer speaks (VER), else
    the packet is malformed (1).  In a versioned message only a field the version spoken has (v: its scope's)"""
    s = []
    for f in fields:
        if f["enum"]:
            x = f"if (!pm_{f['enum'].lower()}_ok({m}.{f['name']}, ver)) return 1;"
            s.append(f"if (v >= {f['since']}) {{ {x} }}" if vm and f["since"] > low else x)
    return " ".join(s)


def walk(fields, m, name, va="", vm=False, low=1):
    """statements that take every element of every list out once (pm_check: nothing reads past the packet), and hold
    each element's enum values to what the peer speaks; VA a versioned message's version argument"""
    s = []
    for f in fields:
        if f["type"] == "list":
            n, fl = f"{name}_{f['name']}", f["since"] if vm else 1
            inner = f"{{ struct pm_{n} e_{f['name']}; while (pm_next_{n}(&{m}.{f['name']}, &e_{f['name']}{va})) {{ {enum_checks(f['fields'], 'e_' + f['name'], vm, fl)} {walk(f['fields'], 'e_' + f['name'], n, va, vm, fl)} }} }}"
            s.append(f"if (v >= {f['since']}) {{ {inner} }}" if vm and f["since"] > low else inner)
    return " ".join(s)


def cname(scope):
    return "PM_SCOPE_" + scope.upper().replace(".", "_")


def later_values(name, vals):
    """whether enum NAME has a value of a later version than 1 (CAP never: check_rules)"""
    return name != "CAP" and any(sv > 1 for _, _, sv in vals)


def chain(name, k, vals, falls):
    """the values a peer may get for value K of enum NAME, K first, each by the version that brought it: [(value
    name, since)], each the fallback of the one before, the last of version 1, or ("-", 0): none, not sent"""
    since = {n: sv for n, _, sv in vals}
    out = [(k, since[k])]
    while out[-1][0] != "-" and (name, out[-1][0]) in falls:
        fb = falls[(name, out[-1][0])]
        out.append((fb, since[fb] if fb != "-" else 0))
    return out


def fits(fields, fitting):
    """whether FIELDS hold a field of an enum of FITTING (one with later values), at any depth"""
    return any(f["enum"] in fitting or (f["type"] == "list" and fits(f["fields"], fitting)) for f in fields)


def fit_fields(fields, name, vm, low, fitting):
    """statements of pm_fit for a struct's FIELDS, walked as read_fields reads them: every field of an enum of FITTING
    rewritten in place to what VER speaks, the rest passed over; then its lists' elements, by pm_fit_<list> where an
    element holds such a field, else by pm_skip_<list>"""
    s = [f"unsigned c_{f['name']} = 0;" for f in fields if f["type"] == "list"]
    for f in fields:
        if f["type"] == "str":
            x = f"(void)pm_rstr(r, {f['max']});"
        elif f["type"] == "bytes":
            x = f"(void)pm_rbytes(r, {f['max']});"
        elif f["type"] == "list":
            x = f"c_{f['name']} = (unsigned)pm_rn(r, 2);"
        elif f["enum"] in fitting:
            n = SIZES[f["type"]]
            x = f"{{ uint8_t *at_ = (uint8_t *)(uintptr_t)r->p; uint64_t x_ = pm_rn(r, {n}); if (!r->bad) {{ uint64_t y_ = pm_{f['enum'].lower()}_for(x_, ver); if (y_ == PM_NOT_SENT) *unsent = 1; else pm_wat(at_, y_, {n}); }} }}"
        else:
            x = f"(void)pm_rn(r, {SIZES[f['type']]});"
        s.append(gated(f, low, x, vm))
    for f in fields:
        if f["type"] == "list":
            fn = f"pm_fit_{name}_{f['name']}(r, ver{', v' if vm else ''}, unsent)" if fits(f["fields"], fitting) else f"pm_skip_{name}_{f['name']}(r{', v' if vm else ''})"
            s.append(gated(f, low, f"for (unsigned i_ = c_{f['name']}; i_ && !r->bad; i_--) {fn};", vm))
    return " ".join(s)


def gen_fits(o, name, fields, fitting, vm=False, low=1):
    """pm_fit_<msg>_<list> for every list whose elements hold such a field (the innermost first)"""
    for f in fields:
        if f["type"] != "list" or not fits(f["fields"], fitting):
            continue
        n, fl = f"{name}_{f['name']}", f["since"] if vm else 1
        gen_fits(o, n, f["fields"], fitting, vm, fl)
        o.append(f"static inline void pm_fit_{n}(struct pm_r *r, const uint16_t *ver{', unsigned v' if vm else ''}, int *unsent) {{ (void)ver; (void)unsent;{' (void)v;' if vm else ''} {fit_fields(f['fields'], n, vm, fl, fitting)} }}")


def header(scopes, consts, enums, msgs, versions=None, keys=(), falls=None):
    o = ["/* The broker protocol's wire format (docs/plugins.md 3.1, sdk/PROTOCOL.md): made by tools/mkplugmsg.py from",
         " * sdk/messages.def, not to be edited.  Header-only, the same file for hassmic's broker and the C client.",
         " *",
         " * A packet (SOCK_SEQPACKET, one message each, PM_MAX_PACKET bytes at most): u16 type, u16 flags, u32 id, then the",
         " * type's fields, little-endian; a string is u16 length + bytes (no NUL), a bytes field the same with any byte.",
         " * A list is a u16 count where the field stands, and its elements after the last field of the struct it is in (of",
         " * several lists: in their order).",
         " *   reading:  pm_head() for the header, pm_get_<msg>() for the fields (0: well-formed; strings point into the",
         " *             packet and are not terminated: pm_cstr() copies one), pm_next_<msg>_<list>() per list element",
         " *   writing:  pm_winit(), pm_put_<msg>(), pm_add_<msg>_<list>() once per element counted in the list's n,",
         " *             pm_wlen(): the packet's length, -1 when something did not fit (never send a cut message)",
         " * Versions (sdk/PROTOCOL.md \"Versions\"): each message is part of one or more scopes (PM_SCOPE_*), from a",
         " * version of each; a message that grew (fields of a later version of its one scope) has readers and writers",
         " * that take the version spoken (v), the others one layout.  pm_spoken() says whether a type is part of what a",
         " * peer speaks (VER: the version of each scope spoken, 0 none; NULL: this build's own, PM_SCOPE_VERSION_*).",
         " * So does every value of an enum (pm_<enum>_ok: a field that holds one holds one the peer speaks, else the packet",
         " * is malformed; pm_check asks, and so does every sender before a packet goes) and every constant",
         " * (PM_CONST_SPOKEN).  A packet is exactly its layout: bytes after its last field make it malformed.",
         " * The manifest's keys have versions too (pm_key: the scope and version a key of a context came with).",
         " * pm_caps_t is a set of capabilities (PM_CAPBIT(PM_CAP_*)): this build's own, never on the wire. */",
         "#ifndef PLUGMSG_H", "#define PLUGMSG_H", "#include <stddef.h>", "#include <stdint.h>", "#include <string.h>", ""]
    o += [f"#define PM_{n} {v}" for n, v, _, _ in consts]
    o.append("/* each constant's scope and the version that brought it: PM_CONST_SPOKEN(NAME, VER), whether a peer that speaks")
    o.append(" * VER (NULL: this build) has it.  A constant of a later version applies only to a peer that speaks it */")
    o += [f"#define PM_SCOPE_OF_{n} {cname(sc)}\n#define PM_SINCE_{n} {sv}" for n, _, sc, sv in consts]
    o.append("#define PM_CONST_SPOKEN(name, ver) (pm_ver((ver), PM_SCOPE_OF_##name) >= PM_SINCE_##name)")
    o.append("/* a version's words (pm_version_doc) are this many bytes at most; PM_WIDENING versions above 1 widen, of all scopes")
    o.append(" * (what an install dialog lists at most beside the capabilities' lines) */")
    o.append(f"#define PM_VERSION_DOC_MAX {VERSION_DOC_MAX}")
    o.append(f"#define PM_WIDENING {sum(1 for w, _ in (versions or {}).values() if w)}")
    o.append("#define PM_HEADER 8")
    o.append("/* pm_<enum>_for's answer where a value's fallback is none (messages.def `>-`): the packet is not sent to that peer */")
    o.append("#define PM_NOT_SENT UINT64_MAX")
    o.append(f"#define PM_SCOPES_MAX {SCOPES_MAX}")
    for name, vals, _ in enums:
        o.append("enum { " + ", ".join(f"PM_{name}_{k} = {v}" for k, v, _ in vals) + " };")
    o.append(f"/* a set of capabilities, 1 << PM_CAP_* each: {CAPS_MAX} bits (hassmic's and the C client's own, never on the wire:")
    o.append(" * WELCOME names what was granted as the scopes spoken) */")
    o.append("typedef uint64_t pm_caps_t;")
    o.append("#define PM_CAPBIT(c) ((pm_caps_t)1 << (c))")
    o.append("enum { " + ", ".join(f"PM_{m['name']} = {m['nr']}" for m in msgs) + " };")
    o.append(f"/* the scopes, and the version of each this build has: what it speaks at most */")
    o.append(f"#define PM_SCOPES {len(scopes)}")
    o.append("enum { " + ", ".join(f"{cname(n)} = {i}" for i, (n, _, _) in enumerate(scopes)) + " };")
    o.append("enum { " + ", ".join(f"{cname(n).replace('PM_SCOPE_', 'PM_SCOPE_VERSION_')} = {v}" for n, v, _ in scopes) + " };")
    o += ["",
          "struct pm_hdr { uint16_t type, flags; uint32_t id; };",
          "struct pm_str { const char *s; uint16_t n; };",
          "struct pm_r { const uint8_t *p, *end; int bad; };",
          "struct pm_list { struct pm_r r; uint16_t n; };",
          "/* fitted: pm_fit made it what one peer speaks (hassmic's psend, the C client's put()): never sent to another */",
          "struct pm_w { uint8_t *b, *p, *end; int over, fitted; };",
          "",
          "static inline uint64_t pm_rn(struct pm_r *r, int n)",
          "{",
          "    uint64_t v = 0;",
          "    if (r->bad || r->end - r->p < n) { r->bad = 1; return 0; }",
          "    for (int i = 0; i < n; i++) v |= (uint64_t)r->p[i] << (8 * i);",
          "    r->p += n;",
          "    return v;",
          "}",
          "static inline struct pm_str pm_rstr(struct pm_r *r, unsigned max)",
          "{",
          "    struct pm_str s = { \"\", 0 }; unsigned n = (unsigned)pm_rn(r, 2);",
          "    if (r->bad || n > max || (size_t)(r->end - r->p) < n || memchr(r->p, 0, n)) { r->bad = 1; return s; }",
          "    s.s = (const char *)r->p; s.n = (uint16_t)n; r->p += n;",
          "    return s;",
          "}",
          "/* bytes: the same without the rule about NUL (a value a plugin keeps; s is not text) */",
          "static inline struct pm_str pm_rbytes(struct pm_r *r, unsigned max)",
          "{",
          "    struct pm_str s = { \"\", 0 }; unsigned n = (unsigned)pm_rn(r, 2);",
          "    if (r->bad || n > max || (size_t)(r->end - r->p) < n) { r->bad = 1; return s; }",
          "    s.s = (const char *)r->p; s.n = (uint16_t)n; r->p += n;",
          "    return s;",
          "}",
          "/* N bytes of V at P, little-endian (pm_fit: a number rewritten where it stands) */",
          "static inline void pm_wat(uint8_t *p, uint64_t v, int n) { for (int i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8 * i)); }",
          "static inline void pm_wn(struct pm_w *w, uint64_t v, int n)",
          "{",
          "    if (w->over || w->end - w->p < n) { w->over = 1; return; }",
          "    for (int i = 0; i < n; i++) *w->p++ = (uint8_t)(v >> (8 * i));",
          "}",
          "static inline void pm_wstr(struct pm_w *w, struct pm_str s, unsigned max)",
          "{",
          "    if (s.n > max || (s.n && memchr(s.s, 0, s.n))) { w->over = 1; return; }",
          "    pm_wn(w, s.n, 2);",
          "    if (w->over || (size_t)(w->end - w->p) < s.n) { w->over = 1; return; }",
          "    if (s.n) memcpy(w->p, s.s, s.n);",
          "    w->p += s.n;",
          "}",
          "static inline void pm_wbytes(struct pm_w *w, struct pm_str s, unsigned max)",
          "{",
          "    if (s.n > max) { w->over = 1; return; }",
          "    pm_wn(w, s.n, 2);",
          "    if (w->over || (size_t)(w->end - w->p) < s.n) { w->over = 1; return; }",
          "    if (s.n) memcpy(w->p, s.s, s.n);",
          "    w->p += s.n;",
          "}",
          "/* N bytes at P as a bytes field */",
          "static inline struct pm_str pm_b(const void *p, size_t n) { struct pm_str r = { p ? (const char *)p : \"\", (uint16_t)(n > 0xffff ? 0xffff : n) }; return r; }",
          "/* a C string as a field */",
          "static inline struct pm_str pm_s(const char *s) { struct pm_str r = { s ? s : \"\", (uint16_t)(s ? strlen(s) : 0) }; if (s && strlen(s) > 0xffff) r.n = 0xffff; return r; }",
          "/* a field as a C string: 0, or -1 when it does not fit (out is \"\" then) */",
          "static inline int pm_cstr(char *out, size_t cap, struct pm_str s)",
          "{",
          "    if (!cap) return -1;",
          "    if (s.n >= cap) { out[0] = 0; return -1; }",
          "    if (s.n) memcpy(out, s.s, s.n);",
          "    out[s.n] = 0;",
          "    return 0;",
          "}",
          "static inline int pm_streq(struct pm_str s, const char *c) { size_t n = strlen(c); return s.n == n && !memcmp(s.s, c, n); }",
          "static inline void pm_winit(struct pm_w *w, uint8_t *buf, size_t cap) { w->b = w->p = buf; w->end = buf + (cap > PM_MAX_PACKET ? PM_MAX_PACKET : cap); w->over = w->fitted = 0; }",
          "static inline long pm_wlen(const struct pm_w *w) { return w->over ? -1 : (long)(w->p - w->b); }",
          "static inline void pm_whead(struct pm_w *w, uint16_t type, uint32_t id) { pm_wn(w, type, 2); pm_wn(w, 0, 2); pm_wn(w, id, 4); }",
          "/* the header of a packet: 0 and *body, *blen its fields; -1: too short or too long to be one */",
          "static inline int pm_head(const uint8_t *pkt, size_t len, struct pm_hdr *h, const uint8_t **body, size_t *blen)",
          "{",
          "    struct pm_r r = { pkt, pkt + len, 0 };",
          "    if (len < PM_HEADER || len > PM_MAX_PACKET) return -1;",
          "    h->type = (uint16_t)pm_rn(&r, 2); h->flags = (uint16_t)pm_rn(&r, 2); h->id = (uint32_t)pm_rn(&r, 4);",
          "    *body = r.p; *blen = len - PM_HEADER;",
          "    return 0;",
          "}",
          ""]
    for m in msgs:
        n, vm, low = m["name"].lower(), versioned(m), m["scopes"][0][1]
        va = ", unsigned v" if vm else ""
        o.append(f"/* {m['name']} ({m['dir']}; " + ", ".join(f"{sc} {v}" for sc, v in m["scopes"]) + (": versioned" if vm else "") + f"): {m['doc']} */")
        if m["fields"]:
            gen_struct(o, n, m["fields"])
            gen_lists(o, n, m["fields"], vm, low)
            o.append(f"static inline int pm_get_{n}(const uint8_t *body, size_t len, struct pm_{n} *m{va}) "
                     f"{{ struct pm_r r_ = {{ body, body + len, 0 }}, *r = &r_; " + ("memset(m, 0, sizeof *m); " if vm else "") + f"{read_fields(m['fields'], 'r', 'm', n, vm, low)} return r->bad || r->p != r->end ? -1 : 0; }}")
            o.append(f"static inline void pm_put_{n}(struct pm_w *w, uint32_t id, const struct pm_{n} *m{va}) "
                     f"{{ pm_whead(w, PM_{m['name']}, id); {write_fields(m['fields'], 'm', vm, low)} }}")
        else:
            o.append(f"static inline void pm_put_{n}(struct pm_w *w, uint32_t id) {{ pm_whead(w, PM_{m['name']}, id); }}")
    o += ["",
          "/* the version of scope S that VER speaks (NULL: this build's own) */",
          "static inline unsigned pm_ver(const uint16_t *ver, int s)",
          "{",
          "    static const uint16_t own[PM_SCOPES] = { " + ", ".join(cname(n).replace("PM_SCOPE_", "PM_SCOPE_VERSION_") for n, _, _ in scopes) + " };",
          "    return s < 0 || s >= PM_SCOPES ? 0 : ver ? ver[s] : own[s];",
          "}",
          "/* the version of scope S this build has (0: none such) */",
          "static inline unsigned pm_scope_version(int s) { return pm_ver(NULL, s); }",
          "/* 1 when TYPE is part of what VER speaks: one of its scopes spoken at a version that has it */",
          "static inline int pm_spoken(uint16_t type, const uint16_t *ver)",
          "{",
          "    (void)ver;",
          "    switch (type) {"]
    for m in msgs:
        o.append(f"    case PM_{m['name']}: return " + " || ".join(f"pm_ver(ver, {cname(sc)}) >= {v}" for sc, v in m["scopes"]) + ";")
    o += ["    }", "    return 0;", "}"]
    # each enum's values, by the version that brought each (CAP's: a capability of this build's, by its own scope)
    for name, vals, sc in enums:
        if name == "CAP":
            o += ["/* V is a capability of this build's (a CAP_* value names a capability: any client that knows it may name it, and", " * the broker answers DENIED where it is not the plugin's) */",
                  "static inline int pm_cap_ok(uint64_t v, const uint16_t *ver)", "{", "    (void)ver;", "    switch (v) {"]
            o += [f"    case PM_CAP_{k}: return 1;" for k, _, _ in vals]
        else:
            o += [f"/* V is a value of {name}_* that a peer speaking VER has (NULL: this build) */",
                  f"static inline int pm_{name.lower()}_ok(uint64_t v, const uint16_t *ver)", "{", "    (void)ver;", "    switch (v) {"]
            o += [f"    case PM_{name}_{k}: return pm_ver(ver, {cname(sc)}) >= {sv};" for k, _, sv in vals]
        o += ["    }", "    return 0;", "}"]
        if name != "CAP":
            # the value a sender sends a peer: V where VER speaks it, else its fallback (messages.def's `>`), and
            # that one's where VER lacks it too, down to version 1 (finding 4 of a review of 60a8589: a value a
            # later version brought dropped its whole packet, a REQ_END or a CALL_HANGUP lost to a version-1 plugin)
            o += [f"/* the value of {name}_* a peer speaking VER gets for V: V where it has it, else the fallback of the version before it,", " * PM_NOT_SENT where that is none (messages.def `>-`: the packet is not sent to that peer) (a value no version has: V, which", " * pm_check refuses) */",
                  f"static inline uint64_t pm_{name.lower()}_for(uint64_t v, const uint16_t *ver)", "{", "    (void)ver;"]
            if later_values(name, vals):
                o.append("    switch (v) {")
                for k, _, sv in vals:
                    if sv > 1:
                        c = chain(name, k, vals, falls or {})
                        o.append(f"    case PM_{name}_{k}: " + " ".join(f"if (pm_ver(ver, {cname(sc)}) >= {s_}) return PM_{name}_{n_};" for n_, s_ in c[:-1]) + (" return PM_NOT_SENT;" if c[-1][0] == "-" else f" return PM_{name}_{c[-1][0]};"))
                o.append("    }")
            o += ["    return v;", "}"]
    o += ["/* A packet's fields against its type, every list walked to its end and nothing after it, and every value of an",
          " * enum one VER has: 0 well-formed, 1 not,",
          " * -1 a type this build does not know or VER does not speak (answered ERR_UNKNOWN, never a reason to end) */",
          "static inline int pm_check(uint16_t type, const uint8_t *body, size_t len, const uint16_t *ver)",
          "{",
          "    if (!pm_spoken(type, ver)) return -1;",
          "    switch (type) {"]
    for m in msgs:
        n, vm, low = m["name"].lower(), versioned(m), m["scopes"][0][1]
        if m["fields"]:
            vv = ", v" if vm else ""
            vdecl = f"unsigned v = pm_ver(ver, {cname(m['scopes'][0][0])}); " if vm else ""
            o.append(f"    case PM_{m['name']}: {{ {vdecl}struct pm_{n} m; if (pm_get_{n}(body, len, &m{vv})) return 1; {enum_checks(m['fields'], 'm', vm, low)} {walk(m['fields'], 'm', n, vv, vm, low)} return 0; }}")
        else:
            o.append(f"    case PM_{m['name']}: (void)body; return len ? 1 : 0;")
    o += ["    }", "    return -1;", "}", ""]
    # pm_fit: every field of an enum with later values made what the peer speaks, in place, before a packet goes
    fitting = {n for n, vals, _ in enums if later_values(n, vals)}
    for m in msgs:
        if fits(m["fields"], fitting):
            gen_fits(o, m["name"].lower(), m["fields"], fitting, versioned(m), m["scopes"][0][1])
    o += ["/* The packet's enum values made what VER speaks, before it goes (sdk/PROTOCOL.md \"Values, constants and keys\"): a",
          " * value of a later version than VER has is rewritten in place to its fallback (pm_<enum>_for).  hassmic's psend and",
          " * the C client's put() run it on every packet, so no sender drops a message for a value a later version brought,",
          " * but for one whose fallback is none (messages.def `>-`): that packet is not for this peer at all, and is dropped",
          " * without a word, as what an older peer cannot be told.  Only a message marked `drop` (pm_droppable) is ever such a",
          " * one: mkplugmsg.py refuses `>-` on an enum any other message holds, so an answer, an end or a state always goes.",
          " * In place: BODY is the packet as it goes to this one peer",
          " * (encode a packet per peer, never send one buffer to two).",
          " * 0; 1 not to be sent to this peer (`>-`); -1 a packet that does not read (left as it is: pm_check refuses it) */",
          "static inline int pm_fit(uint16_t type, uint8_t *body, size_t len, const uint16_t *ver)",
          "{",
          "    struct pm_r r_ = { body, body + len, 0 }, *r = &r_; int un_ = 0, *unsent = &un_; (void)r; (void)ver; (void)unsent;",
          "    switch (type) {"]
    for m in msgs:
        if fits(m["fields"], fitting):
            vm, low = versioned(m), m["scopes"][0][1]
            vdecl = f"unsigned v = pm_ver(ver, {cname(m['scopes'][0][0])}); " if vm else ""
            o.append(f"    case PM_{m['name']}: {{ {vdecl}{fit_fields(m['fields'], m['name'].lower(), vm, low, fitting)} return r->bad ? -1 : un_; }}")
    o += ["    }", "    return 0;", "}",
          "/* 1 when TYPE may be left unsent whole to a peer whose version has no word for a value it holds (messages.def `drop`):",
          " * the only messages pm_fit ever answers 1 for.  A sender whose caller learns \"it went\" from its answer sends none of these */",
          "static inline int pm_droppable(uint16_t type)", "{", "    switch (type) {"]
    o += [f"    case PM_{m['name']}: return 1;" for m in msgs if m["drop"]]
    o += ["    }", "    return 0;", "}", "",
          "static inline const char *pm_name(uint16_t type)", "{", "    switch (type) {"]
    o += [f"    case PM_{m['name']}: return \"{m['name']}\";" for m in msgs]
    o += ["    }", "    return \"?\";", "}",
          "/* who sends a type: '>' the plugin, '<' the broker, '=' either, 0 unknown */",
          "static inline char pm_dir(uint16_t type)", "{", "    switch (type) {"]
    o += [f"    case PM_{m['name']}: return '{m['dir']}';" for m in msgs]
    o += ["    }", "    return 0;", "}",
          "/* the capabilities a type needs, any one of them (PM_CAPBIT(PM_CAP_*)), 0 none */",
          "static inline pm_caps_t pm_caps(uint16_t type)", "{", "    switch (type) {"]
    o += [f"    case PM_{m['name']}: return " + " | ".join(f"PM_CAPBIT(PM_CAP_{cap_name(c)})" for c, _ in m["scopes"]) + ";" for m in msgs if m["scopes"][0][0] != "core"]
    o += ["    }", "    return 0;", "}"]
    # the scopes by name, and each capability's
    o += ["/* scope S's name, \"\" past the last */",
          "static inline const char *pm_scope_name(int s)", "{", "    switch (s) {"]
    o += [f"    case {cname(n)}: return \"{n}\";" for n, _, _ in scopes]
    o += ["    }", "    return \"\";", "}",
          "/* the scope named by the N bytes at NAME, -1 none of this build's */",
          "static inline int pm_scope_of(const char *name, size_t n)",
          "{",
          "    for (int s = 0; s < PM_SCOPES; s++) { const char *c = pm_scope_name(s); if (strlen(c) == n && !memcmp(c, name, n)) return s; }",
          "    return -1;",
          "}",
          "/* the capability bit (PM_CAP_*) scope S is of, -1 for core */",
          "static inline int pm_scope_cap(int s)", "{", "    switch (s) {"]
    o += [f"    case {cname(n)}: return PM_CAP_{n.upper().replace('.', '_')};" for n, _, _ in scopes if n != "core"]
    o += ["    }", "    return -1;", "}"]
    # the versions above 1, and whether each widens what its capability lets a plugin do (docs/plugins.md 3.3)
    versions = versions or {}
    o += ["/* 1 when a version of scope S above FROM and up to TO widens what its capability lets a plugin do (docs/plugins.md 3.3:",
          " * an update that comes to require it is wider than what was accepted) */",
          "static inline int pm_widens(int s, unsigned from, unsigned to)", "{", "    (void)s; (void)from; (void)to;"]
    o += [f"    if (s == {cname(sc)} && from < {v} && to >= {v}) return 1;" for (sc, v), (w, _) in sorted(versions.items(), key=lambda x: ([n for n, _, _ in scopes].index(x[0][0]), x[0][1])) if w]
    o += ["    return 0;", "}",
          "/* what version V of scope S brought, in its definition's words (\"\" for version 1 and for none such) */",
          "static inline const char *pm_version_doc(int s, unsigned v)", "{", "    (void)s; (void)v;"]
    o += [f"    if (s == {cname(sc)} && v == {v}) return \"{d.replace(chr(34), chr(92) + chr(34))}\";" for (sc, v), (_, d) in sorted(versions.items(), key=lambda x: ([n for n, _, _ in scopes].index(x[0][0]), x[0][1]))]
    o += ["    return \"\";", "}"]
    # the manifest's keys
    o += ["/* KEY of the manifest's context CTX (sdk/messages.def's keys lines: \"manifest\", \"setting\", \"net.lan\" ...): the version of",
          " * its scope that brought it, and *SCOPE that scope; 0 when this build knows no such key there */",
          "static inline unsigned pm_key(const char *ctx, const char *key, int *scope)", "{",
          "    static const struct { const char *ctx, *key; int scope; unsigned since; } t[] = {"]
    o += [f"        {{ \"{c}\", \"{k}\", {cname(sc)}, {sv} }}," for c, k, sc, sv in keys]
    o += ["        { 0, 0, 0, 0 } };",
          "    for (size_t i = 0; t[i].ctx; i++) if (!strcmp(t[i].ctx, ctx) && !strcmp(t[i].key, key)) { if (scope) *scope = t[i].scope; return t[i].since; }",
          "    return 0;", "}", "#endif", ""]
    return "\n".join(o)


def keys_of(keys, ctx):
    return [k for k in keys if k[0] == ctx]


def reference(scopes, consts, enums, msgs, versions=None, keys=(), falls=None):
    def rows(fields, indent="", low=None):
        r = []
        for f in fields:
            t = f"{f['type']} (≤ {f['max']})" if f["type"] in ("str", "bytes") else f"list (≤ {f['max']})" if f["type"] == "list" else f"{f['type']}, `{f['enum']}_*`" if f["enum"] else f["type"]
            later = f" (from version {f['since']})" if low is not None and f["since"] and f["since"] > low else ""
            r.append(f"| {indent}`{f['name']}` | {t}{later} | {f['doc']} |")
            if f["type"] == "list":
                r += rows(f["fields"], indent + "&nbsp;&nbsp;↳ ", f["since"] if low is not None else None)
        return r
    o = [BEGIN, ""]
    o.append("Scopes, with the version this hassmic has of each:\n")
    o += ["| Scope | Version | |", "|---|---|---|"] + [f"| `{n}` | {v} | {d} |" for n, v, d in scopes] + [""]
    o.append("Constants: " + ", ".join(f"`{n}` = {v} ({sc}" + (f" {sv}" if sv > 1 else "") + ")" for n, v, sc, sv in consts) + ".\n")
    for name, vals, sc in enums:
        if name == "CAP":
            o.append(f"`CAP_*` (each the capability's own scope, from its version 1): " + ", ".join(f"`{k}` = {v}" for k, v, _ in vals) + ".\n")
        else:
            o.append(f"`{name}_*` ({sc}): " + ", ".join(f"`{k}` = {v}" + ((f" (from {sc} {sv}; not sent to a peer of an earlier version)" if (falls or {})[(name, k)] == "-" else f" (from {sc} {sv}; to a peer of an earlier version `{(falls or {})[(name, k)]}`)") if sv > 1 else "") for k, v, sv in vals) + ".\n")
    versions = versions or {}
    if versions:
        o.append("Versions above 1, each with whether it widens what its capability lets a plugin do (an update that comes to require a widening version asks for the button):\n")
        o += ["| Scope | Version | Widens | What it brought |", "|---|---|---|---|"] + [f"| `{sc}` | {v} | {'yes' if w else 'no'} | {d} |" for (sc, v), (w, d) in versions.items()] + [""]
    if keys:
        ctxs = list(dict.fromkeys(c for c, _, _, _ in keys))
        o.append("The manifest's keys, by where they stand (a key of a later version than the manifest requires refuses it):\n")
        o += ["| Where | Keys |", "|---|---|"]
        for c in ctxs:
            o.append(f"| `{c}` | " + ", ".join(f"`{k}` ({sc}" + (f" {sv}" if sv > 1 else " 1") + ")" if i == 0 or keys_of(keys, c)[i - 1][2:] != (sc, sv) else f"`{k}`"
                                               for i, (_, k, sc, sv) in enumerate(keys_of(keys, c))) + " |")
        o.append("")
    word = {">": "plugin → broker", "<": "broker → plugin", "=": "either way"}
    for m in msgs:
        o.append(f"#### {m['nr']} `{m['name']}` ({word[m['dir']]}, " + " or ".join(f"`{sc}` {v}" for sc, v in m["scopes"]) + ")\n")
        o.append(m["doc"] + "\n")
        if m["drop"]:
            o.append("Marked `drop`: a peer whose version has no word for a value it holds (a value \"not sent to a peer of an earlier version\") gets none of it, not this message at all.  The only kind of message that is ever left unsent so.\n")
        if m["fields"]:
            o += ["| Field | Type | |", "|---|---|---|"] + rows(m["fields"], "", m["scopes"][0][1] if len(m["scopes"]) == 1 else None) + [""]
        else:
            o.append("No fields.\n")
    o.append(END)
    return "\n".join(o)


# ---------------------------------------------------------------- test vectors (sdk/vectors, sdk/PROTOCOL.md "Test vectors")
#
# One file a message: a sample of every field, fixed (the same at every run, from the field's place and kind), and the
# packet plugmsg.h makes of it; then the same for the edges (CASES: every string and list empty; every string and bytes
# field at its bound and every number at its largest; every list at its bound), and packets a decoder must refuse
# (refusals: cut short, a string or bytes field one over its bound, a NUL in a string, a list one over its bound).  A
# client in another language (the Rust example client of hassmic-plugin-template) reads them; the C test made by --c holds plugmsg.h to
# the same bytes both ways, and to every refusal (a review of 0dabfa6: none of the edges was on file).

# an enum's values as the vectors use them ({name: ([values of its version 1], [every value])}: a field that holds an
# enum's values holds one the peer speaks, and every peer speaks version 1), set by vectors() and c_test()
ENUMS = {}


def ev(f):
    """the values of the enum F holds: (version 1's, all), or None"""
    return ENUMS.get(f["enum"]) if f.get("enum") else None


def not_of(f):
    """a value F's type holds and its enum has in no version (vectors' refuse enum:<path>)"""
    have = set(ENUMS[f["enum"]][1])
    return next(x for x in range(1 << 8 * SIZES[f["type"]]) if x not in have)


def sample(fields, seed):
    """values for FIELDS: a dict name -> int, bytes (str and bytes fields) or a list of dicts (two elements, or the
    list's bound when it is smaller).  A field of an enum's values: one of its version 1's"""
    v = {}
    for i, f in enumerate(fields):
        k = seed * 7 + i * 13 + 1
        if ev(f):
            one = ev(f)[0]
            v[f["name"]] = one[k % len(one)]
        elif f["type"] in SIZES:
            bits = 8 * SIZES[f["type"]]
            v[f["name"]] = (0x0123456789abcdef * k + k) % (1 << bits) if bits > 8 else (k * 37) % 256
        elif f["type"] == "str":
            t = f"{f['name']}-{seed}-é"                     # (a character of two bytes: lengths are bytes)
            v[f["name"]] = t.encode()[:f["max"]].decode("utf-8", "ignore").encode()
        elif f["type"] == "bytes":
            v[f["name"]] = bytes([0, 1, 0xff, 0x80, seed & 0xff, 0x0a])[:f["max"]]
        else:
            v[f["name"]] = [sample(f["fields"], seed * 3 + j + 1) for j in range(min(2, f["max"]))]
    return v


def edge(fields, how):
    """values for FIELDS at an edge: HOW "empty" (numbers 0, strings, bytes and lists empty), "long" (numbers at their
    largest, strings and bytes at their bound, a list one element long of the same), "full" (every list at its bound,
    its elements "full" again where the packet still fits, else "empty"; numbers 1, strings and bytes empty)"""
    v = {}
    for f in fields:
        # (an enum's field: its smallest value of version 1, its largest, or 1 where version 1 has it)
        if ev(f): one = ev(f)[0]; v[f["name"]] = min(one) if how == "empty" else max(one) if how == "long" else 1 if 1 in one else min(one)
        elif f["type"] in SIZES: v[f["name"]] = 0 if how == "empty" else (1 << 8 * SIZES[f["type"]]) - 1 if how == "long" else 1
        elif f["type"] in ("str", "bytes"):
            if how != "long": v[f["name"]] = b""
            elif f["type"] == "str": v[f["name"]] = "é".encode() * (f["max"] // 2) + b"a" * (f["max"] % 2)     # (bytes, not characters)
            else: v[f["name"]] = bytes((i * 7 + 1) % 256 for i in range(f["max"]))
        else:
            n = 0 if how == "empty" else 1 if how == "long" else f["max"]
            inner = edge(f["fields"], how)
            if how == "full" and 8 + n * len(encode(f["fields"], inner)) > 32768: inner = edge(f["fields"], "empty")
            v[f["name"]] = [inner] * n
    return v


CASES = ("empty", "long", "full")


def layout(fields, v):
    """FIELDS as version V of their message has them (fields of a later version left out, at every level)"""
    return [dict(f, fields=layout(f["fields"], v)) for f in fields if f["since"] is None or f["since"] <= v]


def older(m):
    """the earlier layouts a versioned message has: [(version, fields)], oldest first (none for any other)"""
    if not versioned(m):
        return []
    sc, low = m["scopes"][0]
    vs = sorted({low} | {f["since"] for f in flat(m["fields"])})
    return [(v, layout(m["fields"], v)) for v in vs[:-1]]


def flat(fields):
    return [x for f in fields for x in [f] + flat(f["fields"])]


def paths(fields, v, path=""):
    """(path, field, setter) for every field of V, those of a list's first element too"""
    o = []
    for f in fields:
        o.append((path + f["name"], f, (lambda val, d=v, k=f["name"]: d.__setitem__(k, val))))
        if f["type"] == "list" and v[f["name"]]:
            o += paths(f["fields"], v[f["name"]][0], f"{path}{f['name']}[0].")
    return o


def refusals(m):
    """[(what, packet)]: packets of M a decoder must refuse"""
    import copy
    head = lambda: m["nr"].to_bytes(2, "little") + b"\0\0" + (0x10000 + m["nr"]).to_bytes(4, "little")
    sample_pkt = head() + encode(m["fields"], sample(m["fields"], m["nr"]))
    out = [("short", sample_pkt[:7])]
    if len(sample_pkt) > 8: out.append(("truncated", sample_pkt[:-1]))
    # a byte after the last field: refused (a packet is exactly its layout at the version spoken)
    if len(sample_pkt) < 32768: out.append(("trailing", sample_pkt + b"\0"))
    # a value of an enum's field that no version has, and one of a later version than a peer speaks (refused by
    # that peer: the packet as the version it speaks has it, the value's scope one version lower)
    for path, f, _ in paths(m["fields"], sample(m["fields"], m["nr"])):
        if not ev(f):
            continue
        v = copy.deepcopy(sample(m["fields"], m["nr"]))
        dict((p, s) for p, _, s in paths(m["fields"], v))[path](not_of(f))
        out.append((f"enum:{path}", head() + encode(m["fields"], v)))
        esc = EOWN[f["enum"]]
        for val, since in ENUMS[f["enum"]][2]:
            if since < 2: continue
            low = since - 1
            mine = dict(m["scopes"]).get(esc)
            if mine is not None and (len(m["scopes"]) > 1 or mine > low):
                continue                                    # (the message itself is not spoken at that version)
            fl = layout(m["fields"], low) if mine is not None and versioned(m) else m["fields"]
            v = copy.deepcopy(sample(fl, m["nr"]))
            pv = dict((p, s) for p, _, s in paths(fl, v))
            if path not in pv: continue                     # (the field is not of that version's layout)
            pv[path](val)
            out.append((f"later:{path}:{esc}:{low}", head() + encode(fl, v)))
    for path, f, _ in paths(m["fields"], sample(m["fields"], m["nr"])):
        for what, val in ((f"over:{path}", None), (f"nul:{path}", b"a\0b")):
            if f["type"] not in ("str", "bytes", "list") or (what.startswith("nul") and f["type"] != "str"): continue
            if f["type"] == "list" and what.startswith("nul"): continue
            v = copy.deepcopy(sample(m["fields"], m["nr"]))
            setter = dict((p, s) for p, _, s in paths(m["fields"], v))[path]
            if f["type"] == "list": setter([edge(f["fields"], "empty")] * (f["max"] + 1)); what = f"count:{path}"
            else: setter(val if val is not None else b"a" * (f["max"] + 1))
            pkt = head() + encode(m["fields"], v)
            if len(pkt) <= 32768: out.append((what, pkt))
    return out


def fitted(m):
    """[(what, packet, sent)]: for every field of M that holds an enum's value of a later version, and every earlier
    version of the enum's scope a peer may speak with M: a packet with that value (as a sender that has it writes it),
    and the packet pm_fit makes of it for that peer, the value's fallback in its place (vectors' fit lines); SENT None
    where the chain ends in none (`>-`): not sent to that peer (vectors' unsent lines)"""
    import copy
    head = lambda: m["nr"].to_bytes(2, "little") + b"\0\0" + (0x10000 + m["nr"]).to_bytes(4, "little")
    out = []
    for path, f, _ in paths(m["fields"], sample(m["fields"], m["nr"])):
        if not ev(f) or f["enum"] == "CAP":
            continue
        esc = EOWN[f["enum"]]
        for val, since in ENUMS[f["enum"]][2]:
            for low in range(1, since):
                mine = dict(m["scopes"]).get(esc)
                if mine is not None and (len(m["scopes"]) > 1 or mine > low):
                    continue                                # (the message itself is not spoken at that version)
                fl = layout(m["fields"], low) if mine is not None and versioned(m) else m["fields"]
                v = copy.deepcopy(sample(fl, m["nr"]))
                pv = dict((p, s) for p, _, s in paths(fl, v))
                if path not in pv: continue                 # (the field is not of that version's layout)
                pv[path](val); a = head() + encode(fl, v)
                fb = val
                while fb is not None and dict(ENUMS[f["enum"]][2])[fb] > low: fb = FALLV[f["enum"]][fb]
                if fb is None:
                    out.append((f"{path}:{esc}:{low}", a, None)); continue
                pv[path](fb); out.append((f"{path}:{esc}:{low}", a, head() + encode(fl, v)))
    return out


def encode(fields, v):
    """a struct's fields in order (a list: its count), then its lists' elements in order, each element the same way"""
    out = b""
    for f in fields:
        x = v[f["name"]]
        if f["type"] in SIZES: out += x.to_bytes(SIZES[f["type"]], "little")
        elif f["type"] in ("str", "bytes"): out += len(x).to_bytes(2, "little") + x
        else: out += len(x).to_bytes(2, "little")
    for f in fields:
        if f["type"] == "list":
            for e in v[f["name"]]: out += encode(f["fields"], e)
    return out


def describe(fields, v, path=""):
    """the sample as lines "<path> <type> <value>": a number in decimal, a str or bytes field as hex (its text beside it
    after #, for people), a list as its count; an element's fields under <list>[<i>]."""
    o = []
    for f in fields:
        x, p = v[f["name"]], path + f["name"]
        if f["type"] in SIZES: o.append(f"{p} {f['type']} {x}")
        elif f["type"] in ("str", "bytes"): o.append(f"{p} {f['type']} {x.hex() or '-'}" + (f"  # {x.decode()!r}" if f["type"] == "str" else ""))
        else:
            o.append(f"{p} list {len(x)}")
            for j, e in enumerate(x): o += describe(f["fields"], e, f"{p}[{j}].")
    return o


EOWN = {}
FALLV = {}


def set_enums(enums, falls=None):
    """ENUMS, EOWN and FALLV from a definition's enums: ENUMS name -> ([version 1's values], [all values], [(value,
    since)]), EOWN name -> its scope, FALLV name -> {value: its fallback's value, None for `>-`}"""
    ENUMS.clear(); EOWN.clear(); FALLV.clear()
    for n, vals, sc in enums:
        ENUMS[n] = (sorted(v for _, v, s in vals if s == 1), sorted(v for _, v, _ in vals), [(v, s) for _, v, s in vals])
        EOWN[n] = sc
        num = {k: v for k, v, _ in vals}
        FALLV[n] = {num[k]: (None if fb == "-" else num[fb]) for (e, k), fb in (falls or {}).items() if e == n}


def vectors(msgs, enums=(), falls=None):
    """{file name: text}"""
    set_enums(enums, falls)
    files = {}
    for m in msgs:
        v = sample(m["fields"], m["nr"]); mid = 0x10000 + m["nr"]
        hd = m["nr"].to_bytes(2, "little") + b"\0\0" + mid.to_bytes(4, "little")
        lines = [f"# {m['name']}: a sample and its packet, made by tools/mkplugmsg.py from sdk/messages.def (sdk/PROTOCOL.md, Test vectors)",
                 f"message {m['name']} {m['nr']}", f"id {mid}"] + describe(m["fields"], v) + [f"packet {(hd + encode(m['fields'], v)).hex()}"]
        for c in CASES if m["fields"] else ():
            e = edge(m["fields"], c)
            lines += [f"case {c}", f"id {mid}"] + describe(m["fields"], e) + [f"packet {(hd + encode(m['fields'], e)).hex()}"]
        # a versioned message: the sample at each earlier version of its scope, as a peer that speaks that one has it
        for ver, fl in older(m):
            e = sample(fl, m["nr"])
            lines += [f"case v{ver}", f"id {mid}", f"version {m['scopes'][0][0]} {ver}"] + describe(fl, e) + [f"packet {(hd + encode(fl, e)).hex()}"]
        lines += [f"refuse {what} {pkt.hex()}" for what, pkt in refusals(m)]
        lines += [f"fit {what} {a.hex()} {b.hex()}" if b is not None else f"unsent {what} {a.hex()}" for what, a, b in fitted(m)]
        files[m["name"] + ".vec"] = "\n".join(lines + [""])
    return files


def c_test(msgs, enums=(), falls=None):
    """a C program: for each message the sample put with plugmsg.h's encoder must be the packet on file, and the packet
    on file read with its decoder must give the sample back; every packet a decoder must refuse refused, and every
    packet with a value of a later version made what its peer speaks (pm_fit)"""
    set_enums(enums, falls)
    def cstr(b): return '"' + "".join(f"\\x{c:02x}" for c in b) + '"'
    o = ["/* made by tools/mkplugmsg.py --c: plugmsg.h against sdk/vectors (make unit-plugmsg) */", "#include \"plugmsg.h\"", "#include <stdio.h>", "#include <stdlib.h>",
         "static int fails;", "#define EQ(c, what) do { if (!(c)) { printf(\"FAIL vector %s\\n\", what); fails++; } } while (0)",
         "static int unhex(const char *h, uint8_t *o) { int n = 0; unsigned x; while (h[0] && h[1] && sscanf(h, \"%2x\", &x) == 1) { o[n++] = (uint8_t)x; h += 2; } return n; }",
         "/* the packet of case CASE (sample: the first, before any case line) */",
         "static const char *packet(const char *file, const char *c, char *buf, size_t cap) { FILE *f = fopen(file, \"r\"); const char *r = NULL; char at[32] = \"sample\"; while (f && !r && fgets(buf, (int)cap, f)) { buf[strcspn(buf, \"\\n\")] = 0; if (!strncmp(buf, \"case \", 5)) snprintf(at, sizeof at, \"%s\", buf + 5); else if (!strncmp(buf, \"packet \", 7) && !strcmp(at, c)) r = buf + 7; } if (f) fclose(f); return r; }",
         "/* every refusal on file must be refused: the header does not read, or the fields do not (pm_check > 0); the count.",
         " * later:<path>:<scope>:<N>: by a peer that speaks that scope at N (this build's versions for the others) */",
         "static const uint16_t *spoken(const char *what, uint16_t *ver) { const char *c = strrchr(what, ':'), *b; int s; if (strncmp(what, \"later:\", 6) || !c) return NULL; for (b = c - 1; b > what && *b != ':'; b--) ; for (s = 0; s < PM_SCOPES; s++) ver[s] = (uint16_t)pm_ver(NULL, s); if ((s = pm_scope_of(b + 1, (size_t)(c - b - 1))) < 0) return NULL; ver[s] = (uint16_t)atoi(c + 1); return ver; }",
         "static int refused(const char *file, char *buf, size_t cap, uint8_t *p) { FILE *f = fopen(file, \"r\"); int n = 0; while (f && fgets(buf, (int)cap, f)) { char *h, what[200]; int k; struct pm_hdr hd; const uint8_t *body; size_t bl; uint16_t ver[PM_SCOPES]; buf[strcspn(buf, \"\\n\")] = 0; if (strncmp(buf, \"refuse \", 7) || !(h = strchr(buf + 7, ' '))) continue; snprintf(what, sizeof what, \"%.*s\", (int)(h - buf - 7), buf + 7); k = unhex(h + 1, p); n++; if (!pm_head(p, (size_t)k, &hd, &body, &bl) && pm_check(hd.type, body, bl, spoken(what, ver)) <= 0) { printf(\"FAIL vector %s: %s taken\\n\", file, what); fails++; } } if (f) fclose(f); return n; }",
         "/* every fit line on file: pm_fit makes its first packet the second for the peer it names, which pm_check takes; the count */",
         "static int fitted(const char *file, char *buf, size_t cap, uint8_t *p, uint8_t *q) { FILE *f = fopen(file, \"r\"); int n = 0; while (f && fgets(buf, (int)cap, f)) { char *h, *g, what[200]; int k, j; struct pm_hdr hd; const uint8_t *body; size_t bl; uint16_t ver[PM_SCOPES]; const uint16_t *vp; buf[strcspn(buf, \"\\n\")] = 0; if (strncmp(buf, \"fit \", 4) || !(h = strchr(buf + 4, ' ')) || !(g = strchr(h + 1, ' '))) continue; snprintf(what, sizeof what, \"later:%.*s\", (int)(h - buf - 4), buf + 4); *g = 0; k = unhex(h + 1, p); j = unhex(g + 1, q); n++; vp = spoken(what, ver); if (pm_head(p, (size_t)k, &hd, &body, &bl) || !vp || pm_check(hd.type, body, bl, vp) != 1 || pm_fit(hd.type, p + PM_HEADER, bl, vp) || k != j || memcmp(p, q, (size_t)k) || pm_check(hd.type, p + PM_HEADER, bl, vp)) { printf(\"FAIL vector %s: %s not made what its peer speaks\\n\", file, what + 6); fails++; } } if (f) fclose(f); return n; }",
         "/* every unsent line on file: pm_fit says its packet is not for the peer it names (1), which pm_check refuses; the count */",
         "static int unsent(const char *file, char *buf, size_t cap, uint8_t *p) { FILE *f = fopen(file, \"r\"); int n = 0; while (f && fgets(buf, (int)cap, f)) { char *h, what[200]; int k; struct pm_hdr hd; const uint8_t *body; size_t bl; uint16_t ver[PM_SCOPES]; const uint16_t *vp; buf[strcspn(buf, \"\\n\")] = 0; if (strncmp(buf, \"unsent \", 7) || !(h = strchr(buf + 7, ' '))) continue; snprintf(what, sizeof what, \"later:%.*s\", (int)(h - buf - 7), buf + 7); k = unhex(h + 1, p); n++; vp = spoken(what, ver); if (pm_head(p, (size_t)k, &hd, &body, &bl) || !vp || pm_check(hd.type, body, bl, vp) != 1 || pm_fit(hd.type, p + PM_HEADER, bl, vp) != 1) { printf(\"FAIL vector %s: %s not left unsent\\n\", file, what + 6); fails++; } } if (f) fclose(f); return n; }",
         "static uint8_t want[PM_MAX_PACKET], got[PM_MAX_PACKET]; static char line[2 * PM_MAX_PACKET + 64], path[600];"]
    calls = []
    def puts(fields, v, struct, var, put_stmt, name, va=""):
        """C statements: declare VAR of STRUCT from V, then PUT_STMT, then the lists' elements added in order (VA: a
        versioned message's version argument, ", <n>")"""
        s = []
        inits = []
        for f in fields:
            x = v[f["name"]]
            if f["type"] in SIZES: inits.append(f".{f['name']} = {x}ULL")
            elif f["type"] in ("str", "bytes"): inits.append(f".{f['name']} = {{ {cstr(x)}, {len(x)} }}")
            else: inits.append(f".{f['name']} = {{ {{ 0 }}, {len(x)} }}")
        s.append(f"{{ struct pm_{struct} {var} = {{ {', '.join(inits)} }};" if fields else "{")
        s.append(put_stmt)
        for f in fields:
            if f["type"] == "list":
                for j, e in enumerate(v[f["name"]]):
                    s += puts(f["fields"], e, f"{struct}_{f['name']}", f"{var}_{f['name']}{j}", f"pm_add_{struct}_{f['name']}(&w, &{var}_{f['name']}{j}{va});", name, va)
        s.append("}")
        return s
    def gets(fields, v, struct, var, name, va=""):
        s = []
        for f in fields:
            x, fv = v[f["name"]], f"{var}.{f['name']}"
            if f["type"] in SIZES: s.append(f"EQ({fv} == {x}ULL, \"{name} {f['name']}\");")
            elif f["type"] in ("str", "bytes"): s.append(f"EQ({fv}.n == {len(x)} && !memcmp({fv}.s, {cstr(x)}, {len(x)}), \"{name} {f['name']}\");")
            else: s.append(f"EQ({fv}.n == {len(x)}, \"{name} {f['name']} count\");")
        for f in fields:
            if f["type"] == "list":
                for j, e in enumerate(v[f["name"]]):
                    ev = f"{var.replace('.', '_')}_{f['name']}{j}"
                    s.append(f"{{ struct pm_{struct}_{f['name']} {ev}; EQ(pm_next_{struct}_{f['name']}(&{var}.{f['name']}, &{ev}{va}), \"{name} {f['name']}[{j}]\");")
                    s += gets(f["fields"], e, f"{struct}_{f['name']}", ev, name, va)
                    s.append("}")
        return s
    for m in msgs:
        n, mid, vm = m["name"].lower(), 0x10000 + m["nr"], versioned(m)
        sc = cname(m["scopes"][0][0])
        runs = [(c, m["fields"], None) for c in ("sample",) + (CASES if m["fields"] else ())] + [(f"v{ver}", fl, ver) for ver, fl in older(m)]
        for c, fl, ver in runs:
            v, tag = sample(fl, m["nr"]) if c == "sample" or ver else edge(fl, c), f"{m['name']} ({c})"
            # the version spoken: an earlier one's case, else this build's (the message's whole layout)
            vv = f"{ver}u" if ver else sc.replace("PM_SCOPE_", "PM_SCOPE_VERSION_")
            va = f", {vv}" if vm else ""
            # (a function a case: one main() of all of them took the compiler minutes)
            o.append(f"static void v_{n}_{c}(const char *dir)")
            o.append("{")
            o.append("    struct pm_w w; int wn; long gn; uint16_t ver[PM_SCOPES];")
            o.append(f"    for (int s = 0; s < PM_SCOPES; s++) {{ ver[s] = (uint16_t)pm_ver(NULL, s); }} ver[{sc}] = (uint16_t){vv}; (void)ver;")
            calls.append(f"    v_{n}_{c}(dir);")
            o.append(f"    /* {tag} */")
            o.append(f"    snprintf(path, sizeof path, \"%s/{m['name']}.vec\", dir); {{ const char *h = packet(path, \"{c}\", line, sizeof line); EQ(h != NULL, \"{tag}: on file\"); wn = h ? unhex(h, want) : 0; }}")
            o.append("    pm_winit(&w, got, sizeof got);")
            o += ["    " + x for x in puts(fl, v, n, "m", f"pm_put_{n}(&w, {mid}u, &m{va});" if m["fields"] else f"pm_put_{n}(&w, {mid}u);", tag, va)]
            o.append(f"    gn = pm_wlen(&w); EQ(gn == wn && !memcmp(got, want, (size_t)wn), \"{tag}: the encoder makes the packet on file\");")
            o.append(f"    {{ struct pm_hdr h; const uint8_t *body; size_t bl; EQ(!pm_head(want, (size_t)wn, &h, &body, &bl) && h.type == PM_{m['name']} && h.id == {mid}u && pm_check(h.type, body, bl, ver) == 0, \"{tag}: the header, well-formed\");")
            if m["fields"]:
                o.append(f"      {{ struct pm_{n} d; EQ(!pm_get_{n}(body, bl, &d{va}), \"{tag}: read\");")
                o += ["        " + x for x in gets(fl, v, n, "d", tag, va)]
                o.append("      }")
            o.append("    }")
            if c == "sample":
                o.append(f"    EQ(refused(path, line, sizeof line, want) == {len(refusals(m))}, \"{m['name']}: its refusals on file\");")
                o.append(f"    EQ(fitted(path, line, sizeof line, want, got) == {sum(b is not None for _, _, b in fitted(m))}, \"{m['name']}: its fits on file\");")
                o.append(f"    EQ(unsent(path, line, sizeof line, want) == {sum(b is None for _, _, b in fitted(m))}, \"{m['name']}: its unsent packets on file\");")
            o.append("}")
    o += ["int main(int argc, char **argv)", "{", "    const char *dir = argc > 1 ? argv[1] : \"sdk/vectors\";"] + calls
    o += ["    printf(\"plugmsg vectors: %s\\n\", fails ? \"FAILED\" : \"all agree\");", "    return fails != 0;", "}", ""]
    return "\n".join(o)


# ---------------------------------------------------------------- the history (sdk/scopes.lock)

def token(f):
    if f["type"] == "list":
        return f"list:{f['name']}:{f['max']}(" + " ".join(token(x) for x in f["fields"]) + ")"
    return f"{f['type']}:{f['name']}" + (f":{f['max']}" if f["type"] in ("str", "bytes") else "") + (f"={f['enum']}" if f["enum"] else "")


def snapshot(scopes, consts, enums, msgs, versions=None, keys=(), falls=None):
    """{(scope, version): [line, ...]}: what each version of each scope has, as the lock records it.  A message of
    several scopes is in each (its one layout); a field, value, constant or manifest key from the version that
    brought it on, a value of a later version with its fallback (B=2>A), a message with its mark (drop); each version above 1 with its mark (widens
    or within), the versions before it with theirs.  A capability's CAP_* value is its scope's (every version of it),
    never core's: a capability more is a scope more, and core stays as it was"""
    snap, versions, falls = {}, versions or {}, falls or {}
    for sc, top, _ in scopes:
        for v in range(1, top + 1):
            lines = [f"const {n} {val}" for n, val, s, sv in consts if s == sc and sv <= v]
            lines += [f"version {w} {'widens' if versions[(sc, w)][0] else 'within'}" for w in range(2, v + 1) if (sc, w) in versions]
            lines += [f"key {c} {k}" for c, k, s, sv in keys if s == sc and sv <= v]
            for n, vals, s in enums:
                if n == "CAP":
                    lines += [f"cap {k}={val}" for k, val, _ in vals if k == cap_name(sc)]
                elif s == sc:
                    lines.append(f"enum {n} " + " ".join(f"{k}={val}" + (f">{falls[(n, k)]}" if (n, k) in falls else "") for k, val, sv in vals if sv <= v))
            for m in msgs:
                at = dict(m["scopes"]).get(sc)
                if at is not None and at <= v:
                    lines.append((f"msg {m['name']} {m['nr']} {m['dir']} " + ("drop " if m["drop"] else "") + " ".join(token(f) for f in layout(m["fields"], v))).rstrip())
            snap[(sc, v)] = sorted(lines)
    return snap


LOCK_HEAD = """# What each version of each scope of the plugin protocol had (sdk/PROTOCOL.md "Versions"): written by
# tools/mkplugmsg.py the first time messages.def declares a version, never by it again.  `make unit-plugmsg` fails
# where messages.def makes a recorded version otherwise (that would change a version a plugin may have been built
# for), and where a version that any release carried (any git tag v*, betas and releases, on main or on release) is
# not here exactly as that release's lock had it: nothing taken out, nothing added, whether messages.def or this file
# has it.  A version that no release carried may be deleted, by hand, to record it anew.  A version a release that
# this tree descends from carried may not go at all; one only a release elsewhere carried (a release branch's newer
# tag, a beta of main the release branch lacks) may be missing here, and is held to that release where it is here.
# Two releases that carried one version otherwise fail too (tools/mkplugmsg.py --released; CI runs it).  These lines
# are tools/mkplugmsg.py's LOCK_HEAD, held word for word; a note of the lock's own goes below them, after a blank line.
#   <scope> <version> const NAME VALUE | enum NAME A=1 B=2>A C=3>- ... | cap NAME=BIT | key CONTEXT NAME
#                     | version N widens|within | msg NAME NUMBER DIR [drop] <type:name[:max][=ENUM]> ...
"""


TAG = re.compile(r"v(\d{4}\.\d\d\.\d\d\.\d{6})(-beta)?")


def lock_parse(text):
    """{(scope, version): {line, ...}} of a lock's text (its lines without scope and version); lines that are none
    are left out (history() names them in the tree's own lock)"""
    out = {}
    for line in text.splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        sc, v, rest = (line.rstrip("\n").split(" ", 2) + ["", ""])[:3]
        if v.isdigit() and rest:
            out.setdefault((sc, int(v)), set()).add(rest)
    return out


def released_locks(root, strict):
    """the lock of every release: ([(tag, ancestor, {(scope, version): {line, ...}})], None), oldest first, or (None,
    why there is none), or (that list, what of the releases could not be read: a tag whose sdk/scopes.lock is no
    file, by its entry's mode in the tag's tree, or has a line that is no lock's).  A release is a tag v<commit time>[-beta] (CI publishes one at every push to main and to
    release: a beta is a release too, which plugins are built against), on any branch: betas of main the release
    branch does not have, and releases of the release branch main does not have (finding 1 of a review of 60a8589:
    the newest by its time alone let main change a version one of its betas carried once a release branch's tag was
    newer).  ANCESTOR: the tag is one HEAD descends from (git merge-base --is-ancestor; where git cannot say, as if
    it were).  A tag without sdk/scopes.lock is from before the lock.  STRICT (CI): no git, no tag at all (a clone
    without its tags), a shallow clone, a history git cannot follow, or a tag's lock that is no file or no lock fails
    instead of being said.  A shallow clone (finding 1 of a review of 4c55b54) holds nothing: git merge-base --is-ancestor
    answers "no" (not "cannot say") for a tag past the boundary, so a version a release this tree descends from
    carried could go without a word"""
    import subprocess
    def no(why):
        if strict: raise SystemExit(f"mkplugmsg --released: {why}")
        return None, why
    try:
        r = subprocess.run(["git", "-C", root, "tag", "-l", "v*"], capture_output=True, text=True)
    except OSError:
        r = None
    if r is None or r.returncode:
        return no("no git here to read the releases' locks from")
    sh = subprocess.run(["git", "-C", root, "rev-parse", "--is-shallow-repository"], capture_output=True, text=True)
    if sh.returncode or sh.stdout.strip() not in ("true", "false"):
        return no("git cannot say whether this clone is shallow: " + (sh.stderr.strip() or sh.stdout.strip()))
    if sh.stdout.strip() == "true":
        return no("a shallow clone: which releases this tree descends from cannot be told past its boundary (git fetch --unshallow --tags; fetch-depth 0)")
    found = sorted((m.group(1), m.group(2) is None, t) for t in r.stdout.split() if (m := TAG.fullmatch(t)))
    tags = [t for _, _, t in found]
    if not tags:
        return no("no release tag (v*) here: fetch them (a clone with its tags, fetch-depth 0)")
    # Every tag's sdk/ in one git process, then every lock's blob in another: "<object> <type> <size>\n<content>\n" (of
    # any type: its content follows whatever it is, and is passed over whole, finding 2 of a review of 4c55b54: a
    # tree's was not, and every later tag's lock was lost without a word), or "<name> missing\n" (a tag from before
    # the lock).  The lock by its entry in sdk/'s tree, mode and all (finding 4 of a review of 61e3d8e: "<tag>:<path>"
    # gives a symbolic link as a blob of its target's name, which read as a lock that carried nothing)
    def batch(names):
        x = subprocess.run(["git", "-C", root, "cat-file", "--batch"], input="".join(f"{n}\n" for n in names).encode(), capture_output=True)
        if x.returncode:
            return None, "git cat-file failed: " + x.stderr.decode(errors="replace").strip()
        out, data, at = [], x.stdout, 0
        for n in names:
            nl = data.find(b"\n", at)
            head = data[at:nl].split() if nl >= 0 else []
            at = nl + 1
            if len(head) == 2 and head[1] == b"missing":
                out.append((None, None)); continue
            if len(head) != 3 or not head[2].isdigit() or at + int(head[2]) >= len(data):
                return None, f"git cat-file's answer for {n} does not read ({b' '.join(head).decode(errors='replace')!r}): no release's lock is held"
            size = int(head[2])
            out.append((head[1].decode(errors="replace"), data[at:at + size])); at += size + 1
        return out, None

    def entry(tree, name):
        """(mode, object id) of NAME in a tree object's content, or None"""
        at = 0
        while at < len(tree):
            sp, nul = tree.find(b" ", at), tree.find(b"\0", at)
            if sp < 0 or nul < 0 or nul + 21 > len(tree):
                return None
            if tree[sp + 1:nul] == name:
                return tree[at:sp].decode(), tree[nul + 1:nul + 21].hex()
            at = nul + 21
        return None
    sdks, why = batch([f"{t}:sdk" for t in tags])
    if why:
        return no(why)
    out, odd, found = [], [], []
    for t, (kind, content) in zip(tags, sdks):
        if kind is None:
            continue                                        # (no sdk/ yet: from before the lock)
        e = entry(content, b"scopes.lock") if kind == "tree" else ("sdk", None)
        if e is None:
            continue                                        # (sdk/ without a lock: from before it)
        if e[0] not in ("100644", "100755"):
            what = {"sdk": f"under an sdk that is a {kind}", "120000": "a symbolic link", "160000": "a submodule", "40000": "a tree"}.get(e[0], f"of mode {e[0]}")
            why = f"{t}'s sdk/scopes.lock is {what}, not a file: what that release carried cannot be read"
            if strict: raise SystemExit(f"mkplugmsg --released: {why}")
            odd.append(why)
            continue
        found.append((t, e[1]))
    blobs, why = batch([o for _, o in found])
    if why:
        return no(why)
    for (t, _), (kind, content) in zip(found, blobs):
        # (a file whose lines are not a lock's: a line of it that is none, read by lock_parse as nothing, would let
        # what that release carried go unheld without a word, finding 4 of a review of 61e3d8e)
        text = (content or b"").decode(errors="replace")
        bad = [l for l in text.splitlines() if l.strip() and not l.startswith("#") and not re.fullmatch(r"\S+ [1-9][0-9]* \S.*", l)]
        if kind != "blob" or bad:
            why = f"{t}'s sdk/scopes.lock " + (f"is a {kind}" if kind != "blob" else f"has {len(bad)} line(s) that are no lock's ({bad[0][:60]!r})") + ": what that release carried cannot be read"
            if strict: raise SystemExit(f"mkplugmsg --released: {why}")
            odd.append(why)
            continue
        a = subprocess.run(["git", "-C", root, "merge-base", "--is-ancestor", t, "HEAD"], capture_output=True, text=True)
        if a.returncode not in (0, 1) and strict:
            raise SystemExit(f"mkplugmsg --released: git cannot say whether {t} is an ancestor of HEAD: {a.stderr.strip()}")
        out.append((t, a.returncode != 1, lock_parse(text)))
    if not out:
        return None, "; ".join(odd + [f"none of the {len(tags)} release tags has sdk/scopes.lock yet" if not odd else "no other release tag has sdk/scopes.lock"])
    return out, "; ".join(odd) or None


def released_why(snap, lock, releases):
    """[] when every version a release carried is here as it carried it; else the reasons.  SNAP: what messages.def
    makes (snapshot()); LOCK: the tree's lock; RELEASES: released_locks()'s list.  Per version any release carried:
    the lock's lines and messages.def's, wherever either has it, are exactly that release's (finding 2 of a review of
    60a8589: a line added to a released version, a message to state 1 with its lock lines written by hand, passed,
    and a plugin that requires state 1 got what older state-1 builds lack); missing from both, it fails only where a
    release this tree descends from carried it; two releases that carried it otherwise fail"""
    if not releases:
        return []
    here = lock_parse(open(lock).read()) if os.path.exists(lock) else {}
    why, first, odd = [], {}, set()
    for tag, _, rec in releases:
        for key, lines in rec.items():
            if key not in first:
                first[key] = (tag, lines)
            elif first[key][1] != lines and key not in odd:
                odd.add(key)
                t0, l0 = first[key]
                why.append(f"releases {t0} and {tag} carried {key[0]} {key[1]} otherwise: one of them changed a released version"
                           + "".join(f"\n    {t0}: {x}" for x in sorted(l0 - lines)) + "".join(f"\n    {tag}: {x}" for x in sorted(lines - l0)))

    def diff(what, have, tag, lines, key):
        gone, added = sorted(lines - set(have)), sorted(set(have) - lines)
        if gone or added:
            why.append(f"{what} has {key[0]} {key[1]} otherwise than release {tag} carried it: a released version is for good, nothing taken out "
                       f"and nothing added (git show {tag}:sdk/scopes.lock; what is new goes into a version of its own)"
                       + "".join(f"\n    released: {x}" for x in gone[:20]) + "".join(f"\n    not released: {x}" for x in added[:20])
                       + (f"\n    ... and {len(gone) + len(added) - 40} more" if len(gone) > 20 or len(added) > 20 else ""))
    for key in sorted(first, key=lambda k: (k[0], k[1])):
        if key in odd:
            continue
        tag, lines = first[key]
        if key not in here and key not in snap:
            mine = [t for t, anc, rec in releases if anc and key in rec]
            if mine:
                why.append(f"{key[0]} {key[1]} is gone (neither {lock} nor messages.def has it), and release {mine[0]}, which this tree descends from, carried it: a released version is for good")
            continue
        if key in here: diff(lock, here[key], tag, lines, key)
        if key in snap: diff("messages.def", snap[key], tag, lines, key)
    return why


def history(parts, lock, record, released=None):
    """the definition against what LOCK recorded: [] or the reasons it fails.  RECORD: the versions not recorded yet
    are written into LOCK (only when nothing else fails).  RELEASED: released_locks()'s list: every version a
    release carried is still what it carried, in LOCK and in the definition alike (a version a release had is never
    recorded anew otherwise, whatever was deleted first)"""
    scopes = parts[0]
    snap, rec = snapshot(*parts), {}
    why = released_why(snap, lock, released)
    # The lock's head is LOCK_HEAD, word for word (finding 5 of a review of 4c55b54: LOCK_HEAD was reworded and the
    # lock's own head still told the rule it replaced): a plain run writes it, --check and --history fail on another
    head, text = "", ""
    if os.path.exists(lock):
        text = open(lock).read()
        head = "".join(itertools.takewhile(lambda l: l.startswith("#"), text.splitlines(True)))
        if head != LOCK_HEAD and not record:
            why.append(f"{lock}: its head is not tools/mkplugmsg.py's LOCK_HEAD word for word (run tools/mkplugmsg.py, which writes it; a note of the lock's own goes below it, after a blank line)")
    if os.path.exists(lock):
        for no, line in enumerate(open(lock), 1):
            if not line.strip() or line.startswith("#"):
                continue
            sc, v, rest = (line.rstrip("\n").split(" ", 2) + ["", ""])[:3]
            if not v.isdigit() or not rest:
                why.append(f"{lock}:{no}: not a line of the lock")
                continue
            rec.setdefault((sc, int(v)), []).append(rest)
    order = [n for n, _, _ in scopes]
    for key in sorted(rec, key=lambda k: (order.index(k[0]) if k[0] in order else len(order), k)):
        if key not in snap:
            why.append(f"{lock} records {key[0]} {key[1]}, which messages.def no longer has: a scope version never goes, and no version goes back")
            continue
        gone = sorted(set(rec[key]) - set(snap[key])); new = sorted(set(snap[key]) - set(rec[key]))
        if gone or new or sorted(rec[key]) != snap[key]:
            why.append(f"messages.def edits the history of {key[0]} {key[1]}, which {lock} recorded: what it had never changes"
                       + "".join(f"\n    recorded: {x}" for x in gone) + "".join(f"\n    now:      {x}" for x in new)
                       + "\n  (append what is new with a version of its own, @N after a field, and a new scope where something must change)")
    missing = [k for k in sorted(snap, key=lambda k: (order.index(k[0]), k[1])) if k not in rec]
    if missing and not record:
        why.append(f"{lock} has not recorded " + ", ".join(f"{sc} {v}" for sc, v in missing) + ": run tools/mkplugmsg.py (a recorded version is for good)")
    if not why and record and (missing or head != LOCK_HEAD):
        text = LOCK_HEAD + text[len(head):]
        if not text.endswith("\n"): text += "\n"
        text += "".join(f"{sc} {v} {line}\n" for sc, v in missing for line in snap[(sc, v)])
        open(lock, "w").write(text)
        if missing: print("recorded in " + lock + ": " + ", ".join(f"{sc} {v}" for sc, v in missing))
        if head != LOCK_HEAD: print(f"wrote LOCK_HEAD's words as the head of {lock}")
    return why


def main():
    def opt(name, default):
        return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default
    defp, lock = opt("--def", DEF), opt("--lock", LOCK)
    parts = load(defp)
    # what every release carried (finding 1 of a review of 4e0a202: deleting a recorded version's lines and
    # recording it anew changed what a released plugin speaks; findings 1 and 2 of a review of 60a8589: the newest
    # tag alone, and its lines as a subset): from git, for this tree's own lock only (the tests make a repository of
    # their own with releases on two branches, tests/unit/plugmsg_scopes_test.py)
    released = None
    if any(a in sys.argv for a in ("--header", "--vectors", "--c")):
        pass                                                # (what is made of a definition: no history asked)
    elif os.path.abspath(lock) == os.path.abspath(LOCK):
        released, why = released_locks(ROOT, "--released" in sys.argv)
        if why and not released:
            print(f"mkplugmsg: skipped: sdk/scopes.lock not held to the releases: {why}")
        elif why:
            print(f"mkplugmsg: sdk/scopes.lock not held to every release: {why}")
    if "--released" in sys.argv:
        why = released_why(snapshot(*parts), lock, released)
        if why: raise SystemExit("\n".join(why))
        if released:
            print(f"mkplugmsg: sdk/scopes.lock and messages.def keep every version that {len(released)} release(s) with a lock carried ({released[0][0]} to {released[-1][0]})")
        return
    if "--history" in sys.argv:
        why = history(parts, lock, "--record" in sys.argv, released)
        if why: raise SystemExit("\n".join(why))
        return
    if "--header" in sys.argv:
        open(opt("--header", None), "w").write(header(*parts))
        return
    hdr, ref, vec = header(*parts), reference(*parts), vectors(parts[3], parts[2], parts[6])
    if "--vectors" in sys.argv:
        d = opt("--vectors", None); os.makedirs(d, exist_ok=True)
        for f, t in vec.items(): open(os.path.join(d, f), "w").write(t)
        return
    if "--c" in sys.argv:
        open(sys.argv[sys.argv.index("--c") + 1], "w").write(c_test(parts[3], parts[2], parts[6]))
        return
    why = history(parts, lock, "--check" not in sys.argv, released)
    if why: raise SystemExit("\n".join(why))
    doc = open(DOC).read() if os.path.exists(DOC) else f"# The plugin protocol\n\n{BEGIN}\n{END}\n"
    if BEGIN not in doc or END not in doc:
        raise SystemExit(f"{DOC}: the message reference's markers are gone")
    new = doc[:doc.index(BEGIN)] + ref + doc[doc.index(END) + len(END):]
    want = [(HDR, hdr), (DOC, new)] + [(os.path.join(VEC, f), t) for f, t in sorted(vec.items())]
    if "--check" in sys.argv:
        stale = [p for p, t in want if not os.path.exists(p) or open(p).read() != t]
        extra = [f for f in (os.listdir(VEC) if os.path.isdir(VEC) else []) if f not in vec]
        if stale or extra:
            raise SystemExit("not what sdk/messages.def makes (run tools/mkplugmsg.py): " + ", ".join(stale + [os.path.join(VEC, f) for f in extra]))
        return
    os.makedirs(VEC, exist_ok=True)
    for f in os.listdir(VEC):
        if f.endswith(".vec") and f not in vec: os.unlink(os.path.join(VEC, f))
    for p, t in want: open(p, "w").write(t)


if __name__ == "__main__":
    main()
