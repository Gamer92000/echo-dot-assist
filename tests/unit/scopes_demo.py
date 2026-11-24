#!/usr/bin/env python3
"""sdk/messages.def as a later hassmic's would be, for the tests of what a version above 1 brings: net.lan 2 widens
what the capability lets a plugin do (its entries may name a host, the key "names"), net.lan 3 only says more of the
same and net.lan 4 widens again (a raise over several versions, each said), ha.entities 2 only says more of the same
(a sensor's "precision"), call 2 too (a new reason a call ended each way, HANGUP_ELSEWHERE and
RESULT_ELSEWHERE, which a peer of call 1 gets as STOPPED and FAILED); button 2 brings a value no earlier one stands for
(`>-`: BUTTON_KEY_MIC, a key a button-1 plugin must not hear as the action button), not sent to a peer of version 1
at all, which BUTTON alone may be (marked drop: an event whose whole point is the key), and audio.music 2 a state
that is a case of one before it (MUSIC_BUFFERING, which a hassmic of audio.music 1 gets as PLAYING: a state is never
left unsent, the last one told would stand; finding 1 of a review of 61e3d8e); and state goes to 35, versions within and widening ones by turns after three within (state 2 to
4: one line in an update's dialog), for an update that raises a scope over many versions.  Written to OUT (the
definition), whose header
tools/mkplugmsg.py --def makes: make unit-plugin_store, unit-plugin_broker and unit-hmplugin build their tests
against it a second time (-DSCOPES_DEMO), and the plugin index's tests/test_index.py holds tools/index.py
manifest_check to the same build on it (finding 3 and 4 of a review of 4e0a202: no version above 1 existed to prove
the rules on; finding 4 of a review of 60a8589: a value of a later version, which psend dropped with its packet;
findings 3 and 4 of a review of 4c55b54: a value no earlier one stands for, and the dialog's lines for many raises;
finding 1 of a review of 61e3d8e: `>-` only where the message may go unsent, and a state never).
    scopes_demo.py OUT"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# what the demo adds, also for the index's test (which reads them from here)
SCOPES = {"net.lan": 4, "ha.entities": 2, "call": 2, "button": 2, "audio.music": 2, "state": 35}
KEYS = [("net.lan", "names", "net.lan", 2), ("ha.entities.sensor", "precision", "ha.entities", 2)]
# whether each version above 1 widens what its capability lets a plugin do
WIDENS = {("net.lan", 2): True, ("net.lan", 3): False, ("net.lan", 4): True, ("ha.entities", 2): False, ("call", 2): False,
          ("button", 2): False, ("audio.music", 2): False}
WIDENS.update({("state", v): v >= 5 and v % 2 == 1 for v in range(2, SCOPES["state"] + 1)})
# values of a later version, each with the value a peer of the version before it gets: (enum, its line's last value, what follows it)
VALUES = [("HANGUP", "STOPPED=6", " ELSEWHERE=7@2>STOPPED"), ("RESULT", "FAILED=5", " ELSEWHERE=6@2>FAILED"),
          ("BUTTON_KEY", "VOLUME_DOWN=3", " MIC=4@2>-"), ("MUSIC", "STOPPED=3", " BUFFERING=4@2>PLAYING")]


def demo(text):
    for sc, v in SCOPES.items():
        old = f"scope {sc} 1 "
        assert old in text, old
        text = text.replace(old, f"scope {sc} {v} ", 1)
        text = text.replace(f"scope {sc} {v} ", "".join(f'version {sc} {w} {"widens" if WIDENS[(sc, w)] else "within"} "{sc} {w}, the demo\'s"\n' for w in range(2, v + 1)) + f"scope {sc} {v} ", 1)
    for e, last, more in VALUES:
        old = next(l for l in text.splitlines() if l.startswith(f"enum {e} "))
        assert old.endswith(last), old
        text = text.replace(old + "\n", old + more + "\n", 1)
    anchor = "\n# ---------------------------------------------------------------- core\n"
    assert anchor in text
    return text.replace(anchor, "\n" + "".join(f"keys {c} {sc}@{v} {k}\n" for c, k, sc, v in KEYS) + anchor, 1)


if __name__ == "__main__":
    open(sys.argv[1], "w").write(demo(open(os.path.join(ROOT, "sdk", "messages.def")).read()))
