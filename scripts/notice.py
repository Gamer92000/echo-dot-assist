#!/usr/bin/env python3
"""The NOTICE file that goes with one model's binaries, made from the repository's NOTICE (its one source).

    scripts/notice.py <codename> [<owner/name>] > NOTICE

NOTICE is the title and its opening, then parts, each a heading underlined with "=": "Every model", or one that
names the models it is about by codename in parentheses ("The Echo 4 (laser)").  The opening and every part a
model's binaries carry are kept, in their order; the rest is left out.  A line "Full text: <path>" becomes that
file of the repository, whole: the licences' texts are shipped as their projects wrote them, and kept nowhere else
here.  <owner/name> is where the bundle's releases and source are published (UPDATE_REPO, keys/release.conf or a
fork's secrets/fork.conf: scripts/lib/device.sh release_load); it replaces the project's own wherever NOTICE names
it.  Written by scripts/bundle.sh, deploy.sh, install-system.sh and install-boot.sh beside the binaries
(device.sh notice_make).  Stdlib only: the PC needs no packages for it.
"""
import os
import re
import sys

TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FULL = re.compile(r"^Full text: (\S+)$")


def die(msg):
    sys.stderr.write("notice.py: %s\n" % msg)
    sys.exit(1)


def project_repo():
    with open(os.path.join(TOP, "keys", "release.conf")) as f:
        for line in f:
            if line.startswith("UPDATE_REPO="):
                return line.split("=", 1)[1].strip()
    die("keys/release.conf names no UPDATE_REPO")


def parts(lines):
    """(heading or None for the opening, its lines) in order; a part's heading is a line underlined with "="."""
    out, cur, title = [], [None, []], True
    for i, line in enumerate(lines):
        nxt = lines[i + 1] if i + 1 < len(lines) else ""
        if line.strip() and re.fullmatch(r"=+", nxt) and len(nxt) == len(line):
            if title:                       # the file's own title: part of the opening
                title = False
            else:
                out.append(cur)
                cur = [line, []]
        cur[1].append(line)
    out.append(cur)
    return out


def models(heading):
    """The codenames a part is for: None for "Every model"."""
    if heading == "Every model":
        return None
    m = re.search(r"\(([a-z0-9, ]+)\)$", heading)
    if not m:
        die('part "%s": neither "Every model" nor codenames in parentheses at its end' % heading)
    names = [n.strip() for n in m.group(1).split(",")]
    for n in names:
        if not os.path.isfile(os.path.join(TOP, "devices", n, "device.conf")):
            die('part "%s": no model "%s" (devices/%s/device.conf)' % (heading, n, n))
    return names


def full_text(path):
    # by its path in the tree, not where links lead: tests run the scripts in a tree of links (fork_release_test.sh)
    full = os.path.join(TOP, path)
    if os.path.isabs(path) or ".." in path.split("/") or not os.path.isfile(full):
        die("Full text: %s is no file of the repository" % path)
    with open(full, encoding="utf-8") as f:
        text = f.read()
    return text.rstrip("\n").split("\n")


def main():
    if len(sys.argv) not in (2, 3):
        die("usage: scripts/notice.py <codename> [<owner/name>]")
    model = sys.argv[1]
    if not re.fullmatch(r"[a-z0-9]+", model) or not os.path.isfile(os.path.join(TOP, "devices", model, "device.conf")):
        die("unknown model %s" % model)
    repo = sys.argv[2] if len(sys.argv) == 3 else project_repo()
    if not re.fullmatch(r"[A-Za-z0-9._-]+/[A-Za-z0-9._-]+", repo):
        die("not owner/name: %s" % repo)
    with open(os.path.join(TOP, "NOTICE"), encoding="utf-8") as f:
        lines = f.read().split("\n")
    out = []
    for heading, body in parts(lines):
        if heading is not None:
            names = models(heading)
            if names is not None and model not in names:
                continue
        for line in body:
            m = FULL.match(line)
            out.extend(full_text(m.group(1)) if m else [line])
    text = "\n".join(out).rstrip("\n") + "\n"
    own = project_repo()
    if repo != own:
        text = text.replace("github.com/" + own, "github.com/" + repo)
    sys.stdout.write(text)


main()
