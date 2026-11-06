#!/usr/bin/env python3
"""The PC's stand-in for src/cam/cam.c's play mode (HASSMIC_FAKE_VIDEO): reads its records on stdin until it ends, as
the player shows them on an Echo Show's screen.  FAKE_VIDEO_LOG: a file it appends "start <pid> self=<0|1> blur=<ms>"
(HMCAM_SELF: this Show's own picture in the corner; HMCAM_BLUR_MS: frosted at first), "rec <type> <bytes>"
per record and "stop" to (hassmic kills it at a call's end: then no "stop", its pid gone: gone()); like cam.c it ends
by itself after 3 s without a record (PLAY_IDLE_MS).  FAKE_VIDEO_DUMP: a file the records themselves are appended to
(to play what a browser sent on an Echo Show's player by hand).  This Show's own picture (HMCAM_SELF=1) logs into
FAKE_VIDEO_LOG + ".self" instead, so each log is one player's."""
import os, select, struct, sys


def gone(log_path):
    """every player the log names has ended (exited, or killed by hassmic's video.c), the self-view's too: no picture
    left on a screen"""
    for p in (log_path, log_path + ".self"):
        if not os.path.exists(p): continue
        for l in open(p).read().splitlines():
            if l.startswith("start "):
                try: os.kill(int(l.split()[1]), 0); return False
                except ProcessLookupError: pass
    return True


def main():
    path = os.environ.get("FAKE_VIDEO_LOG")
    if path and os.environ.get("HMCAM_SELF") == "1": path += ".self"
    log = open(path, "a", buffering=1) if path else None
    say = (lambda s: log.write(s + "\n")) if log else (lambda s: None)
    say(f"start {os.getpid()} self={os.environ.get('HMCAM_SELF', '0')} blur={os.environ.get('HMCAM_BLUR_MS', '0')}")
    dump = open(os.environ["FAKE_VIDEO_DUMP"], "ab") if os.environ.get("FAKE_VIDEO_DUMP") else None
    inp = sys.stdin.buffer

    def read(n):
        b = b""
        while len(b) < n:
            if not select.select([inp], [], [], 3.0)[0]: return None      # idle: hidden, as cam.c
            c = os.read(inp.fileno(), n - len(b))
            if not c: return None
            b += c
        return b

    while True:
        h = read(20)
        if not h or h[:4] != b"HMCV": break
        n = struct.unpack_from("<I", h, 8)[0]
        d = read(n)
        if d is None: break
        say(f"rec {h[4]} {n}")
        if dump: dump.write(h + d); dump.flush()
    say("stop")


if __name__ == "__main__":
    main()
