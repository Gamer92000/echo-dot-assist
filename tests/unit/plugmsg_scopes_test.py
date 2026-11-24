#!/usr/bin/env python3
"""The rules of the plugin protocol's versions (sdk/PROTOCOL.md "Versions"), each broken once on a copy of
tests/unit/plugmsg_scopes/demo.def or its lock, and the real sdk/messages.def against sdk/scopes.lock: tools/mkplugmsg.py
must refuse every one with its reason (make unit-plugmsg).  A scope's next version only adds; a recorded version never
changes; a message of several scopes and the greeting never grow."""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOL = [sys.executable, os.path.join(ROOT, "tools", "mkplugmsg.py")]
DEMO = os.path.join(ROOT, "tests", "unit", "plugmsg_scopes")
fails = 0


def check(c, what):
    global fails
    print(("ok   " if c else "FAIL ") + what)
    fails += not c


def run(text, lock_text, *extra):
    with tempfile.TemporaryDirectory() as d:
        dp, lp = os.path.join(d, "x.def"), os.path.join(d, "x.lock")
        open(dp, "w").write(text)
        if lock_text is not None: open(lp, "w").write(lock_text)
        r = subprocess.run(TOOL + ["--def", dp, "--lock", lp, "--history"] + list(extra), capture_output=True, text=True)
        return r.returncode, r.stdout + r.stderr, open(lp).read() if os.path.exists(lp) else None


good, lock = open(os.path.join(DEMO, "demo.def")).read(), open(os.path.join(DEMO, "demo.lock")).read()
rc, out, _ = run(good, lock)
check(rc == 0, f"the definition is what its lock recorded: {out.strip()}")


def demo_lock_dir(d):
    """the lock with PICK in the direction D (a break of the rules that is no break of the history)"""
    return lock.replace("msg PICK 14 < drop", f"msg PICK 14 {d} drop")


def breaks(what, old, new, why, lock_text=None, *extra):
    assert old in good, old
    rc, out, _ = run(good.replace(old, new, 1), lock if lock_text is None else lock_text, *extra)
    check(rc != 0 and why in out, f"{what}: refused ({why!r})" + ("" if rc != 0 and why in out else f": {rc} {out.strip()}"))


breaks("a field of a recorded version changes its type", '    u8 a enum:KIND ""', '    u16 a enum:KIND ""', "edits the history of demo 1")
breaks("a string's bound raised", '    str s 16 ""', '    str s 32 ""', "edits the history of demo 1")
breaks("a field added to a recorded version without a version of its own", '    u16 b @2 ""', '    u16 b @2 ""\n    u8 c @2 ""', "edits the history of demo 2")
breaks("a field appended without @N after one of a later version", '    u16 b @2 ""', '    u16 b @2 ""\n    u8 c ""', "comes after a field of version 2")
breaks("a field of a later version before one of an earlier one", '    u8 a enum:KIND ""\n    str s 16 ""', '    u8 a enum:KIND @2 ""\n    str s 16 ""', "comes after a field of version 2")
breaks("a field in a list's element out of order", '        u8 x enum:KIND ""\n        u16 y @2 ""', '        u16 y @2 ""\n        u8 x enum:KIND ""', "comes after a field of version 2")
breaks("a message of several scopes grows", '    u8 k ""', '    u8 k ""\n    u8 l @2 ""', "a message of several scopes never grows")
breaks("the greeting grows", '    u32 sdk ""', '    u32 sdk ""\n    u8 more @1 ""', "the greeting never grows")
breaks("a version past the scope's", '    u16 b @2 ""', '    u16 b @3 ""', "scope demo is at 2")
breaks("a message removed", 'msg BOTH 12 > demo|other "of two scopes: never grows"\n    u8 k ""\n', "", "edits the history of demo 1")
breaks("a message renumbered", "msg NEW 11 <", "msg NEW 13 <", "edits the history of demo 2")
breaks("a direction changed", "msg NEW 11 <", "msg NEW 11 >", "edits the history of demo 2")
breaks("a message moved to a later version", "msg NEW 11 < demo@2", "msg NEW 11 < demo@1", "edits the history of demo 1")
breaks("an enum value renumbered", "B=2@2", "B=5@2", "edits the history of demo 2")
breaks("an enum value dropped", "KIND demo A=1 C=3 B=2@2>A D=4@2>-", "KIND demo A=1 C=3 B=2@2>A", "edits the history of demo 2")
# a value of a later version names the one a peer of an earlier version gets (finding 4 of a review of 60a8589: the
# packet was dropped instead), of the same enum and an earlier version, recorded with it
breaks("a value of a later version without its fallback", "B=2@2>A", "B=2@2", "a value of a later version names the value a peer of an earlier version gets instead")
breaks("a fallback on a value of version 1", "KIND demo A=1 C=3 B=2@2>A", "KIND demo A=1>B C=3 B=2@2>A", "a value of version 1 has no fallback")
breaks("a fallback that is no value", "B=2@2>A", "B=2@2>Z", "its fallback Z is no value of KIND")
breaks("a fallback of the same version", "KIND demo A=1 C=3 B=2@2>A", "KIND demo A=1 C=3 B=2@2>A E=5@2>B", "its fallback B is of version 2, not of an earlier one")
breaks("a fallback changed (to another value of version 1)", "B=2@2>A", "B=2@2>C", "edits the history of demo 2")
breaks("a value that is none", "B=2@2>A", "B=2@2>", "is no NAME=VALUE")
# a fallback that is none (`>-`, finding 3 of a review of 4c55b54): the packet is not sent to a peer of an earlier
# version; recorded with the value as any fallback is, and never on a value of version 1
breaks("none as the fallback of a value of version 1", "KIND demo A=1 C=3", "KIND demo A=1>- C=3", "a value of version 1 has no fallback")
breaks("none turned into a fallback", "D=4@2>-", "D=4@2>A", "edits the history of demo 2")
breaks("a fallback turned into none", "B=2@2>A", "B=2@2>-", "edits the history of demo 2")
breaks("a value that is pm_<enum>_for's PM_NOT_SENT", "D=4@2>-", "D=4@2>- E=18446744073709551615@2>-", "is no value (the largest u64")
# `>-` only on an enum that messages marked drop alone hold (finding 1 of a review of 61e3d8e: it passed on ERR,
# CALL_END, a qualifier and a state, each a message that must arrive): the mark recorded with the message
breaks("`>-` on an enum a message not marked drop holds", 'msg PICK 14 < demo drop "', 'msg PICK 14 < demo "', "enum KIND D: >- (not sent to a peer of an earlier version), and PICK holds KIND and is not marked drop")
breaks("... the one whose list's element holds it", 'msg PUT 10 < demo drop "', 'msg PUT 10 < demo "', "and PUT holds KIND and is not marked drop")
breaks("the mark put on a recorded message", 'msg NEW 11 < demo@2 "', 'msg NEW 11 < demo@2 drop "', "edits the history of demo 2")
breaks("a mark that is another word", 'msg BOTH 12 > demo|other "', 'msg BOTH 12 > demo|other maybe "', "[drop]")
# (only what hassmic tells a plugin: a plugin's own message never goes unsent for a value, a review of 6631974)
breaks("a plugin's own message (>) marked drop", 'msg PICK 14 < demo drop "', 'msg PICK 14 > demo drop "', "PICK: marked drop, and it is a plugin's own message", demo_lock_dir(">"))
breaks("the greeting marked drop", 'msg HELLO 1 > core "', 'msg HELLO 1 > core drop "', "HELLO: marked drop, and it is the greeting")
rc, out, _ = run(good.replace('msg NEW 11 < demo@2 "', 'msg OKAY 15 = demo "either way"\nmsg NEW 11 < demo@2 "', 1).replace("msg OKAY 15 = demo", "msg OKAY 15 = demo drop", 1), lock)
check(rc != 0 and "OKAY: marked drop, and it is an answer either way" in out, f"a message of either way (OK, ERR: an answer) marked drop: refused ({out.strip()[:120]})")


def real(scope, enum, value, *more):
    """sdk/messages.def with SCOPE at version 2 and VALUE appended to ENUM's line (MORE: (old, new) edits after)"""
    t = open(os.path.join(ROOT, "sdk", "messages.def")).read()
    t = t.replace(f'scope {scope} 1 "', f'version {scope} 2 within "the test\'s"\nscope {scope} 2 "', 1)
    line = next(l for l in t.splitlines() if l.startswith(f"enum {enum} "))
    t = t.replace(line + "\n", line + " " + value + "\n", 1)
    for a, b in more:
        assert a in t, a; t = t.replace(a, b, 1)
    return t


# ... on the real definition, each kind of message the rule is for: an answer, an end, a qualifier, a state, and an
# event whose whole point is the value (BUTTON, marked drop: taken; recorded with --record into a lock of the test's)
reallock = open(os.path.join(ROOT, "sdk", "scopes.lock")).read()
for what, scope, enum, value, msg in (("an answer: ERR", "core", "ERR", "GONE=9@2>-", "ERR"), ("an end: CALL_END", "call", "RESULT", "MOVED=6@2>-", "CALL_END"),
                                      ("an end: CALL_HANGUP", "call", "HANGUP", "ELSEWHERE=7@2>-", "CALL_HANGUP"), ("a qualifier: ACTION's FROM", "ha.actions", "FROM", "AUTOMATION=3@2>-", "ACTION"),
                                      ("a reason: MUSIC_CMD's MWHY", "audio.music", "MWHY", "TIMER=5@2>-", "MUSIC_CMD"), ("a state: STATE", "state", "STATE", "RINGING=4@2>-", "STATE"),
                                      ("a state: MUSIC_STATE", "audio.music", "MUSIC", "BUFFERING=4@2>-", "MUSIC_STATE")):
    rc, out, _ = run(real(scope, enum, value), reallock, "--record")
    check(rc != 0 and f">- (not sent to a peer of an earlier version), and {msg} holds {enum} and is not marked drop" in out, f"`>-` on {what}: refused ({out.strip()[:160]})")
    rc, out, _ = run(real(scope, enum, value.replace(">-", ">" + {"ERR": "FAILED", "RESULT": "FAILED", "HANGUP": "STOPPED", "FROM": "HOME_ASSISTANT", "MWHY": "OTHER_SOURCE", "STATE": "IDLE", "MUSIC": "PAUSED"}[enum])), reallock, "--record")
    check(rc == 0, f"... the same value with a fallback: taken ({out.strip()[:160]})")
rc, out, rec = run(real("button", "BUTTON_KEY", "MIC=4@2>-"), reallock, "--record")
check(rc == 0 and "button 2 enum BUTTON_KEY ACTION=1 VOLUME_UP=2 VOLUME_DOWN=3 MIC=4>-" in rec and "button 2 msg BUTTON 120 < drop " in rec, f"`>-` on BUTTON's key (an event, marked drop): taken, the mark recorded ({out.strip()[:160]})")
rc, out, _ = run(real("button", "BUTTON_KEY", "MIC=4@2>-", ('msg BUTTON 120 < button drop "', 'msg BUTTON 120 < button "')), reallock, "--record")
check(rc != 0 and "BUTTON holds BUTTON_KEY and is not marked drop" in out, f"... and without the mark: refused ({out.strip()[:120]})")
rc, out, _ = run(real("core", "ERR", "ODD=65535@2>FAILED"), reallock, "--record")
check(rc != 0 and "65535 is the clients' own HM_ERR_NOT_SPOKEN" in out, f"ERR 65535 (the clients' own word for a request not of what is spoken): refused ({out.strip()[:120]})")
rc, out, _ = run(open(os.path.join(ROOT, "sdk", "messages.def")).read().replace('msg OK 10 = core "', 'msg OK 10 = core drop "', 1), reallock)
check(rc != 0 and "OK: marked drop, and it is an answer either way" in out, f"OK marked drop: refused ({out.strip()[:120]})")
breaks("a constant changed", "const LIM demo 10", "const LIM demo 11", "edits the history of demo 1")
breaks("a scope's version lowered below what is in use", 'scope demo 2 "', 'scope demo 1 "', "scope demo is at 1")
breaks("a scope that is no capability", 'scope other 1 "a second capability"', 'scope other 1 "a second capability"\nscope third 1 "none"', "has no capability bit")
# every version above 1 says whether it widens what its capability lets a plugin do (docs/plugins.md 3.3), and the
# lock records the word: an update that comes to require a widening version asks for the button
breaks("a version above 1 without its mark", 'version demo 2 widens "NEW, PUT\'s b and the key extra"\n', "", "no line 'version demo 2 widens|within")
breaks("a version's mark turned round", "version demo 2 widens", "version demo 2 within", "edits the history of demo 2")
breaks("a mark that is neither word", "version demo 2 widens", "version demo 2 maybe", "a mark for every version above 1")
breaks("a mark for version 1", "version demo 2 widens", 'version demo 1 widens "x"\nversion demo 2 widens', "no such version of a scope above 1")
breaks("a mark twice", "version demo 2 widens", 'version demo 2 within "x"\nversion demo 2 widens', "version demo 2 twice")
# the manifest's keys: each from a version of its scope, recorded with it
breaks("a key of a version past the scope's", "keys demo.entry demo@2 extra", "keys demo.entry demo@3 extra", "scope demo is at 2")
breaks("a key moved to an earlier version", "keys demo.entry demo@2 extra", "keys demo.entry demo extra", "edits the history of demo 1")
breaks("a key dropped", "keys demo.entry demo@2 extra\n", "", "edits the history of demo 2")
breaks("a key twice", "keys demo.entry demo@2 extra", "keys demo.entry demo@2 extra name", "a key of the manifest twice")
breaks("a key of no scope", "keys demo.entry demo@2 extra", "keys demo.entry nothing@1 extra", "no scope nothing")
# a field that holds an enum's values: part of its layout; the enum there, its values fitting the field
breaks("a field no longer of its enum", "    u8 a enum:KIND", "    u8 a", "edits the history of demo 1")
breaks("a field of an enum that is not there", "    u8 a enum:KIND", "    u8 a enum:NONE", "no enum NONE")
breaks("an enum's value that does not fit its field", "B=2@2", "B=300@2", "does not fit a u8")
breaks("an enum of another scope than the message's", "    u32 sdk \"\"", "    u32 sdk enum:KIND \"\"", "is demo's, and the message is not of demo or core")
# a message of several scopes: only core's enums and CAP (finding 3 of a review of 60a8589: a peer speaking only
# "other" found every BOTH with a value of demo's KIND malformed)
breaks("an enum of one of its scopes in a message of several", '    u8 k ""', '    u8 k enum:KIND ""', "a message of several scopes holds only core's enums and CAP")
breaks("a version's words past their bound", '"NEW, PUT\'s b and the key extra"', '"' + "x" * 301 + '"', "its words are 301 bytes, 300 at most")
# a capability's value is its scope's from its version 1, and one of pm_caps_t's bits
breaks("a capability's value of a later version", "CAP core DEMO=0 OTHER=1", "CAP core DEMO=0 OTHER=1@2", "comes with its scope")
breaks("a capability past pm_caps_t's bits", "CAP core DEMO=0 OTHER=1", "CAP core DEMO=0 OTHER=64", "bits of pm_caps_t")
# the greeting's bounds are for good: a list of every scope there will ever be, 255
breaks("the greeting's list of scopes under 255", '    u32 sdk ""', '    u32 sdk ""\n    list scopes 32 ""\n        u16 version ""', "a list of 255")
many = good.replace('scope other 1 "a second capability"', "\n".join(f'scope s{i} 1 ""' for i in range(253)) + '\nscope other 1 "a second capability"', 1)
rc, out, _ = run(many, lock)
check(rc != 0 and "256 scopes: the greeting names 255 at most" in out, f"a 256th scope: refused ({out.strip()[:100]})")
breaks("a version past a u16", 'scope demo 2 "', 'scope demo 70000 "', "a version is a u16 on the wire")

# Tied to the releases (finding 1 of a review of 4e0a202; findings 1 and 2 of a review of 60a8589): every version
# any release carried (a tag v<commit time>[-beta] on any branch: main publishes betas, release publishes releases,
# and neither has all of the other's) is here exactly as it carried it, in the lock and in messages.def; a version
# missing altogether fails only where a release this tree descends from carried it; two releases that carried one
# version otherwise fail.  In a repository of their own, with mkplugmsg.py and demo.def as its sdk/ at each step
sys.path.insert(0, os.path.join(ROOT, "tools"))
import mkplugmsg
three = good.replace('scope demo 2 "', 'scope demo 3 "', 1).replace('msg BOTH 12', 'msg LATER 13 > demo@3 "demo 3\'s"\n    u8 q ""\nmsg BOTH 12', 1).replace("version demo 2 widens", 'version demo 3 within "LATER"\nversion demo 2 widens', 1)
with tempfile.TemporaryDirectory() as d:
    env = dict(os.environ, GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1", GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t", GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t")
    git = lambda *a: subprocess.run(["git", "-C", d] + list(a), capture_output=True, text=True, env=env)
    sdk = lambda f: os.path.join(d, "sdk", f)
    def tool(*a):
        r = subprocess.run([sys.executable, os.path.join(d, "tools", "mkplugmsg.py")] + list(a), capture_output=True, text=True, env=env)
        return r.returncode, r.stdout + r.stderr
    def put(defn=None, lck=None):
        if defn is not None: open(sdk("messages.def"), "w").write(defn)
        if lck is not None: open(sdk("scopes.lock"), "w").write(lck)
    def commit(msg, tag=None):
        git("add", "-A"); git("commit", "-qm", msg)
        if tag: git("tag", tag)
    def record(defn, lck):
        """DEFN with LCK, and what --record writes for the versions LCK lacks"""
        put(defn, lck); rc_, out_ = tool("--history", "--record")
        assert rc_ == 0, out_
        return open(sdk("scopes.lock")).read()
    def refused(what, why, *args):
        rc_, out_ = tool(*(args or ("--released",)))
        check(rc_ != 0 and why in out_, f"{what}: refused ({why!r})" + ("" if rc_ != 0 and why in out_ else f": {rc_} {out_.strip()[:300]}"))
    def passes(what, *args):
        rc_, out_ = tool(*(args or ("--released",)))
        check(rc_ == 0, f"{what}: {out_.strip()[:200]}")
    git("init", "-q", "-b", "main")
    os.makedirs(os.path.join(d, "tools")); os.makedirs(os.path.join(d, "sdk"))
    shutil.copy(os.path.join(ROOT, "tools", "mkplugmsg.py"), os.path.join(d, "tools", "mkplugmsg.py"))
    open(os.path.join(d, "README"), "w").write("x\n"); commit("before the lock")
    check(mkplugmsg.released_locks(d, False) == (None, "no release tag (v*) here: fetch them (a clone with its tags, fetch-depth 0)"), "no tag: nothing held, said")
    try:
        mkplugmsg.released_locks(d, True); check(False, "strict (CI) without a tag: fails")
    except SystemExit as e:
        check("no release tag" in str(e), f"strict (CI) without a tag: fails ({e})")
    git("tag", "v2025.12.01.000000-beta"); git("tag", "release-2099")          # (a release from before the lock; no release's tag)
    check(mkplugmsg.released_locks(d, True) == (None, "none of the 1 release tags has sdk/scopes.lock yet"), f"a release from before the lock: none, said: {mkplugmsg.released_locks(d, True)}")
    put(good, lock); commit("the lock", "v2026.01.01.000000")
    passes("the lock as the release carried it")
    passes("and --history the same", "--history")
    # main: demo 3 recorded, a beta of it; the release branch: a hotfix released later, without demo 3
    git("branch", "release")
    lock3 = record(three, lock); commit("demo 3", "v2026.02.01.000000-beta")
    git("checkout", "-q", "release"); open(os.path.join(d, "README"), "w").write("hotfix\n"); commit("hotfix", "v2026.03.01.000000")
    check(sorted(t for t, _, _ in mkplugmsg.released_locks(d, True)[0]) == ["v2026.01.01.000000", "v2026.02.01.000000-beta", "v2026.03.01.000000"], "every release with a lock, on both branches")
    passes("the release branch without main's demo 3, which only a beta it does not descend from carried (a push to release passes)")
    git("checkout", "-q", "main")
    passes("main, with demo 3 as its beta carried it, though a newer release lacks it")
    # (finding 1 of the review: the newest tag by time was the release branch's, without demo 3, and main changed it)
    changed = three.replace('    u8 q ""', '    u16 q ""')
    cut = "".join(l for l in lock3.splitlines(True) if not l.startswith("demo 3 "))
    put(changed, cut)
    rc, out = tool("--history", "--record")
    check(rc != 0 and "messages.def has demo 3 otherwise than release v2026.02.01.000000-beta carried it" in out and open(sdk("scopes.lock")).read() == cut,
          f"a beta's version deleted from the lock and recorded anew otherwise, a newer release elsewhere: refused, nothing recorded ({out.strip()[:160]})")
    refused("the same, --released", "messages.def has demo 3 otherwise")
    put(three, cut)
    passes("demo 3 declared as released and not recorded yet: the definition is what the beta carried", "--released")
    put(good, lock)
    refused("demo 3 gone from main altogether (a beta of main carried it)", "demo 3 is gone (neither")
    # (finding 2 of the review: a line added to a released version, with its lock lines written by hand)
    more = three.replace('msg BOTH 12', 'msg MORE 15 > demo "added to demo 1 after its release"\n    u8 m ""\nmsg BOTH 12', 1)
    put(more, lock3 + "".join(f"demo {v} msg MORE 15 > u8:m\n" for v in (1, 2, 3)))
    rc, out, _ = run(more, lock3 + "".join(f"demo {v} msg MORE 15 > u8:m\n" for v in (1, 2, 3)))
    check(rc == 0, f"(the definition and its lock agree: without the releases it would pass: {out.strip()[:100]})")
    refused("a message added to released versions, its lines in the lock by hand", "not released: msg MORE 15 > u8:m")
    put(three, lock3.replace("demo 1 const LIM 10\n", ""))
    refused("a released line taken out of the lock", "released: const LIM 10")
    put(three, lock3)
    # the release branch releases a version of its own (other 2); main does not descend from it
    git("checkout", "-q", "release")
    other2 = good.replace('scope other 1 "', 'scope other 2 "', 1).replace('msg BOTH 12', 'msg OTHER2 16 > other@2 "other 2\'s"\n    u8 o ""\nmsg BOTH 12', 1).replace("version demo 2 widens", 'version other 2 within "OTHER2"\nversion demo 2 widens', 1)
    lock_o = record(other2, lock); commit("other 2", "v2026.04.01.000000")
    passes("the release branch with other 2, without demo 3")
    git("checkout", "-q", "main")
    passes("main without other 2, which only the release branch carried")
    both = three.replace('scope other 1 "', 'scope other 2 "', 1).replace('msg BOTH 12', 'msg OTHER2 16 > other@2 "other 2\'s"\n    u8 o ""\nmsg BOTH 12', 1).replace("version demo 2 widens", 'version other 2 within "OTHER2"\nversion demo 2 widens', 1)
    put(both, lock3 + "".join(l for l in lock_o.splitlines(True) if l.startswith("other 2 ")))
    passes("main taking other 2 as the release branch carried it", "--history")
    put(both.replace('    u8 o ""', '    u16 o ""'), lock3 + "".join(l.replace("u8:o", "u16:o") for l in lock_o.splitlines(True) if l.startswith("other 2 ")))
    refused("main declaring other 2 otherwise than the release branch's release carried it", "has other 2 otherwise than release v2026.04.01.000000 carried it")
    put(three, lock3)
    # two releases that carried one version otherwise (one of them changed it): fails whoever asks
    git("checkout", "-q", "-b", "odd", "v2026.01.01.000000")
    odd = good.replace("const LIM demo 10", "const LIM demo 11")
    put(odd, run(odd, mkplugmsg.LOCK_HEAD, "--record")[2]); commit("odd", "v2026.05.01.000000-beta")
    git("checkout", "-q", "main")
    refused("two releases that carried demo 1 otherwise", "releases v2026.01.01.000000 and v2026.05.01.000000-beta carried demo 1 otherwise")
    git("tag", "-d", "v2026.05.01.000000-beta")
    passes("that release gone again")
    # a tag whose sdk/scopes.lock is no file, a directory (finding 2 of a review of 4c55b54: git cat-file's answer for
    # it was misread, and the locks of every tag after it were lost without a word: a version a later beta carried
    # could go): --released fails and names it; any other run names it and holds every other release all the same
    git("checkout", "-q", "-b", "odd2", "v2025.12.01.000000-beta")
    os.makedirs(sdk("scopes.lock"), exist_ok=True); open(os.path.join(sdk("scopes.lock"), "x"), "w").write("not a lock\n")
    commit("a lock that is a directory", "v2025.12.15.000000-beta")
    git("checkout", "-q", "main")
    refused("a release whose sdk/scopes.lock is a directory, before every other release", "v2025.12.15.000000-beta's sdk/scopes.lock is a tree, not a file")
    put(good, lock)
    rc, out = tool("--history")
    check(rc != 0 and "demo 3 is gone (neither" in out and "v2025.12.15.000000-beta's sdk/scopes.lock is a tree" in out,
          f"... and a plain run names it and still holds the releases after it: demo 3 gone from main refused ({out.strip()[:300].replace(chr(10), ' | ')})")
    put(three, lock3)
    rc, out = tool("--history")
    check(rc == 0 and "not held to every release: v2025.12.15.000000-beta's sdk/scopes.lock is a tree" in out, f"... the tree as its releases carried it: passes, the tag named ({out.strip()[:200]})")
    git("tag", "-d", "v2025.12.15.000000-beta")
    passes("that release gone again")
    # a tag whose lock is a symbolic link (git cat-file "<tag>:<path>" gives it as a blob of its target's name, which
    # read as a lock that carried nothing) or a file whose lines are no lock's (finding 4 of a review of 61e3d8e):
    # --released fails and names it, any other run names it and holds every other release the same
    for name, tag, make, says in (("a symbolic link", "v2025.12.16.000000-beta", lambda: os.symlink("messages.def", sdk("scopes.lock")), "is a symbolic link, not a file"),
                                  ("a file of no lock's lines", "v2025.12.17.000000-beta", lambda: open(sdk("scopes.lock"), "w").write("# a head\nnot a lock at all\n"), "has 1 line(s) that are no lock's ('not a lock at all')")):
        git("checkout", "-q", "-b", "odd-" + tag[1:11], "v2025.12.01.000000-beta")
        os.makedirs(os.path.join(d, "sdk"), exist_ok=True)
        if os.path.lexists(sdk("scopes.lock")): os.unlink(sdk("scopes.lock"))
        make(); commit(f"a lock that is {name}", tag)
        git("checkout", "-q", "main")
        refused(f"a release whose sdk/scopes.lock is {name}", f"{tag}'s sdk/scopes.lock {says}")
        put(three, lock3)
        rc, out = tool("--history")
        check(rc == 0 and f"not held to every release: {tag}'s sdk/scopes.lock {says}" in out, f"... a plain run names it ({out.strip()[:200]})")
        put(good, lock)
        rc, out = tool("--history")
        check(rc != 0 and "demo 3 is gone (neither" in out, f"... and still holds the releases after it: demo 3 gone from main refused ({out.strip()[:200].replace(chr(10), ' | ')})")
        put(three, lock3)
        git("tag", "-d", tag)
        passes("that release gone again")
    # a shallow clone with its tags (finding 1 of a review of 4c55b54: git merge-base --is-ancestor answers "not an
    # ancestor" for a tag past the boundary, so a version a release this tree descends from carried went without a
    # word): --released fails, any other run says it skipped, whatever the tree has
    with tempfile.TemporaryDirectory() as d2:
        c = os.path.join(d2, "c")
        subprocess.run(["git", "clone", "-q", "--depth", "1", "--no-single-branch", "file://" + d, c], capture_output=True, env=env)
        subprocess.run(["git", "-C", c, "fetch", "-q", "--depth", "1", "--tags"], capture_output=True, env=env)
        ctool = lambda *a: subprocess.run([sys.executable, os.path.join(c, "tools", "mkplugmsg.py")] + list(a), capture_output=True, text=True, env=env)
        check(subprocess.run(["git", "-C", c, "rev-parse", "--is-shallow-repository"], capture_output=True, text=True).stdout.strip() == "true"
              and "v2026.02.01.000000-beta" in subprocess.run(["git", "-C", c, "tag"], capture_output=True, text=True).stdout, "(a shallow clone with every tag)")
        open(os.path.join(c, "sdk", "messages.def"), "w").write(good); open(os.path.join(c, "sdk", "scopes.lock"), "w").write(lock)
        r = ctool("--released")
        check(r.returncode != 0 and "a shallow clone" in r.stderr, f"a shallow clone, demo 3 gone (a beta it descends from carried it): --released fails ({(r.stdout + r.stderr).strip()[:200]})")
        r = ctool("--history")
        check(r.returncode == 0 and "skipped: sdk/scopes.lock not held to the releases: a shallow clone:" in r.stdout, f"... any other run says it skipped ({(r.stdout + r.stderr).strip()[:200]})")
    # a plain run (make unit-plugmsg's --check, and the generator) holds the releases the same, and records nothing
    put(changed, cut)
    rc, out = tool("--history", "--record")
    check(rc != 0 and open(sdk("scopes.lock")).read() == cut, "a plain run records no released version anew")

# a new version: refused by --history until recorded, recorded once by --record, and then held to as the others are
three = good.replace('scope demo 2 "', 'scope demo 3 "', 1).replace('msg BOTH 12', 'msg LATER 13 > demo@3 "demo 3\'s"\n    u8 q ""\nmsg BOTH 12', 1).replace("version demo 2 widens", 'version demo 3 within "LATER"\nversion demo 2 widens', 1)
rc, out, _ = run(three, lock)
check(rc != 0 and "has not recorded demo 3" in out, f"a version declared and not recorded: refused ({out.strip()[:120]})")
rc, out, rec = run(three, lock, "--record")
check(rc == 0 and rec.startswith(lock) and "demo 3 msg LATER 13 > u8:q" in rec and "demo 3 version 3 within" in rec and rec.count("\ndemo 3 ") >= 3, "--record writes it, after what was there, with its mark")
rc, out, _ = run(three.replace('    u8 q ""', '    u16 q ""'), rec)
check(rc != 0 and "edits the history of demo 3" in out, "and from then on it is history too")
rc, out, _ = run(good, rec)
check(rc != 0 and "records demo 3, which messages.def no longer has" in out, "a recorded version gone from messages.def (the scope's version lowered): refused")
rc, out, _ = run(good, lock.replace("demo 1 const LIM 10\n", ""))
check(rc != 0 and "edits the history of demo 1" in out, "a line taken out of the lock: refused (what the lock says is the history)")
# the lock's head is LOCK_HEAD word for word (finding 5 of a review of 4c55b54: the lock's own head still told the rule
# LOCK_HEAD had replaced): --history refuses another, --record writes LOCK_HEAD's in its place, and a note of the
# lock's own below it, after a blank line, stays
old_head = lock.replace("never by it again.", "never by it again, and only lines recorded since the newest release may go.", 1)
rc, out, _ = run(good, old_head)
check(rc != 0 and "its head is not tools/mkplugmsg.py's LOCK_HEAD" in out, f"a lock whose head is not LOCK_HEAD's words: refused ({out.strip()[:100]})")
rc, out, rec = run(good, old_head.replace("core 1 const", "\n# a note of the lock's own\ncore 1 const", 1), "--record")
check(rc == 0 and rec.startswith(mkplugmsg.LOCK_HEAD + "\n# a note of the lock's own\n") and run(good, rec)[0] == 0, f"--record writes LOCK_HEAD's words, the note below stays ({out.strip()[:100]})")
rc, out, _ = run(good, lock.replace("#   <scope>", "#  <scope>", 1))
check(rc != 0 and "its head is not" in out, "a space less in the head: refused (word for word)")
# the demo's vectors: D (`>-`) has an unsent line wherever a field holds KIND, at demo 1 (make unit-plugmsg's vectors
# test holds pm_fit to each; a generator that left them out would hold it to none)
with tempfile.TemporaryDirectory() as d:
    subprocess.run(TOOL + ["--def", os.path.join(DEMO, "demo.def"), "--vectors", d], check=True)
    put_v, pick_v = open(os.path.join(d, "PUT.vec")).read(), open(os.path.join(d, "PICK.vec")).read()
    check("\nunsent a:demo:1 " in put_v and "\nunsent items[0].x:demo:1 " in put_v and "\nunsent kind:demo:1 " in pick_v and "\nfit a:demo:1 " in put_v,
          "the demo's vectors: an unsent line for D in each field of KIND (PUT, its list, PICK), beside B's fit lines")

# CI asks the same of every release, and asks again right before it publishes (finding 6 of a review of 4c55b54: a
# push to main and one to release build at once, and each test job's --released ran before the other's tag existed):
# the test job's step, and in the release job (whole history) the tags fetched anew, then --released, then the release
wf = open(os.path.join(ROOT, ".github", "workflows", "build.yml")).read()
def step(job, name):
    """the job's text (to the next job) and its step NAME's (to the next step)"""
    import re
    j = wf[wf.index(f"\n  {job}:\n") + 1:]
    m = re.search(r"\n  [a-z-]+:\n", j); j = j[:m.start()] if m else j
    at = j.index(f"      - name: {name}\n"); end = j.find("\n      - ", at + 1)
    return j, "".join(l for l in j[at:end if end > 0 else len(j)].splitlines(True) if not l.lstrip().startswith("#"))
job, pub = step("release", "Publish")
at = [pub.find(x) for x in ("git fetch --prune --prune-tags --tags --force origin", "python3 tools/mkplugmsg.py --released", "gh release create")]
check("fetch-depth: 0" in job and 0 <= at[0] < at[1] < at[2],
      "the release job: tags fetched anew (those deleted on GitHub gone here too: --prune --prune-tags, finding 5 of a review of 61e3d8e), --released, then gh release create (whole history)")
# ... and that line, as the workflow has it, run in a clone whose origin deleted a tag since: the tag is gone here too
fetch = next(l.strip() for l in pub.splitlines() if l.strip().startswith("git fetch "))
with tempfile.TemporaryDirectory() as d:
    env = dict(os.environ, GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1", GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t", GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t")
    o, c = os.path.join(d, "o"), os.path.join(d, "c")
    g = lambda w, *a: subprocess.run(["git", "-C", w] + list(a), capture_output=True, text=True, env=env)
    os.makedirs(o); g(o, "init", "-q", "-b", "main"); open(os.path.join(o, "f"), "w").write("x\n"); g(o, "add", "f"); g(o, "commit", "-qm", "x")
    g(o, "tag", "v2026.01.01.000000"); g(o, "tag", "v2026.02.01.000000-beta")
    subprocess.run(["git", "clone", "-q", o, c], capture_output=True, env=env)
    g(o, "tag", "-d", "v2026.02.01.000000-beta"); g(o, "tag", "v2026.03.01.000000")
    r = subprocess.run(fetch, shell=True, cwd=c, capture_output=True, text=True, env=env)
    tags = g(c, "tag").stdout.split()
    check(r.returncode == 0 and tags == ["v2026.01.01.000000", "v2026.03.01.000000"], f"... run in a clone: a tag deleted at origin is gone, a new one there ({fetch!r}: {tags})")
job, t = step("test", "Plugin protocol against every release")
check("fetch-depth: 0" in job and "python3 tools/mkplugmsg.py --released" in t, "the test job: --released, whole history")

# the real definition against the real lock
r = subprocess.run(TOOL + ["--history"], capture_output=True, text=True)
check(r.returncode == 0, f"sdk/messages.def keeps sdk/scopes.lock: {(r.stdout + r.stderr).strip()}")
print("plugmsg scopes rules: " + ("FAILED" if fails else "all passed"))
sys.exit(1 if fails else 0)
