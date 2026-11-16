#!/usr/bin/env python3
"""Stable local build identity, including uncommitted changes and archive builds.

This is a source identity for the pinned Cannoli toolchain/firmware, not a binary
checksum or an upstream release version. Private signing keys are never read.
"""
import hashlib
from pathlib import Path
import re


def identity(root):
    paths = {root / 'Makefile', root / 'tools/embed.py'}
    for directory in ['src', 'web', 'wakealign', 'scripts/system', 'scripts/device', 'devices/cannoli']:
        # .md is prose; stubs/*.syms are generated (tools/mkstubs.sh) and must not
        # change the identity between the firmware and stub link of one check.
        paths.update(p for p in (root / directory).rglob('*')
                     if p.is_file() and p.suffix not in ('.md', '.syms') and '__pycache__' not in p.parts)
    paths.update(root / p for p in ['scripts/bundle.sh', 'scripts/otatool.py', 'scripts/lib/device.sh'])
    release = {}
    for name in ['keys/release.conf', 'secrets/fork.conf']:
        path = root / name
        if not path.exists():
            continue
        paths.add(path)
        for line in path.read_text().splitlines():
            match = re.fullmatch(r'(UPDATE_REPO|RELEASE_PUB)=([A-Za-z0-9._/-]+)\s*(?:#.*)?', line)
            if match:
                release[match[1]] = match[2]
    pub = release.get('RELEASE_PUB', 'keys/release.pub')
    # Same basename requirement as release_load. Never accept a secret key path.
    if Path(pub).name != 'release.pub':
        raise ValueError('RELEASE_PUB must name release.pub')
    paths.add(root / pub)
    digest = hashlib.sha256()
    for path in sorted(paths):
        data = path.read_bytes()
        digest.update(str(path.relative_to(root)).encode() + b'\0')
        digest.update(len(data).to_bytes(8, 'big') + data)
    return 'cannoli8349-' + digest.hexdigest()[:24]


if __name__ == '__main__':
    print(identity(Path(__file__).resolve().parents[2]))
