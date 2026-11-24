#!/bin/sh
# The least serials of the plugin index this build takes (docs/plugins.md 5.5; keys/plugins.conf PLUGINS_LEAST_INDEX,
# PLUGINS_LEAST_REVOKED): read from the published site, both signatures checked against the index keys this tree
# builds in (PLUGINS_PUB, and PLUGINS_PUB2 where one is named: either's), and written into keys/plugins.conf.  Run by
# hand before a release (DEVELOPMENT.md "CI and releases"), and after a revocation that has to reach Echos that were
# reset since: every Echo of that release then refuses an older index or revocation list, also one that has never
# seen any (new, or after a factory reset).
# Not in CI: a build is reproducible from its commit (BUILD_TIME is the commit's), and a value read at build time
# would make two builds of one commit differ.  So the values are committed, and the commit says when they were read.
#   scripts/plugins-least.sh [SITE]     SITE: another base address (a fork's index; default PLUGIN_INDEX)
#   scripts/plugins-least.sh --check    say what the site has against keys/plugins.conf, change nothing
set -e
cd "$(dirname "$0")/.."
check=; [ "$1" = --check ] && { check=1; shift; }
. keys/plugins.conf
[ -f secrets/fork.conf ] && . secrets/fork.conf
# the second key only with the first it belongs with, by content (the Makefile's rule: PLUGINS_PUB2_WITH)
[ -n "$PLUGINS_PUB2" ] && [ "$PLUGINS_PUB2_WITH" != "$(od -An -tx1 -v "$PLUGINS_PUB" | tr -d ' \n')" ] && PLUGINS_PUB2=
site=${1:-$PLUGIN_INDEX}
exec python3 - "$site" "$check" "$PLUGINS_PUB" $PLUGINS_PUB2 <<'EOF'
import json, os, re, sys, urllib.request
sys.path.insert(0, "scripts")
import otatool
site, check, pubs = sys.argv[1], sys.argv[2], [open(k, "rb").read() for k in sys.argv[3:]]
def get(name, most):
    with urllib.request.urlopen(site + name, timeout=30) as r:
        data = r.read(most + 1)
    if len(data) > most:
        sys.exit(f"plugins-least: {site}{name} is larger than an Echo reads")
    return data
serials = {}
for name, purpose, most in (("index.json", b"hassmic plugin index 1\n", 1 << 20), ("revoked.json", b"hassmic plugin revoked 1\n", 512 << 10)):
    data, sig = get(name, most), get(name + ".sig", 64)
    if not any(otatool.check(sig, pub, purpose + data) for pub in pubs):
        sys.exit(f"plugins-least: {site}{name}: not signed by an index key this tree builds in: nothing written")
    serials[name] = json.loads(data)["serial"]
    if type(serials[name]) is not int or serials[name] < 0:
        sys.exit(f"plugins-least: {site}{name}: no serial")
ix, rv = serials["index.json"], serials["revoked.json"]
conf = open("keys/plugins.conf").read()
was = {k: int(v) for k, v in re.findall(r"^(PLUGINS_LEAST_(?:INDEX|REVOKED))=(\d+)$", conf, re.M)}
print(f"the site: index {ix}, revocation list {rv}; keys/plugins.conf: index {was.get('PLUGINS_LEAST_INDEX')}, revocation list {was.get('PLUGINS_LEAST_REVOKED')}")
if check:
    sys.exit(0)
if ix < was.get("PLUGINS_LEAST_INDEX", 0) or rv < was.get("PLUGINS_LEAST_REVOKED", 0):
    sys.exit("plugins-least: the site's serials are lower than the ones written: an Echo of this build would refuse every list it can get. Nothing written")
conf = re.sub(r"^PLUGINS_LEAST_INDEX=\d+$", f"PLUGINS_LEAST_INDEX={ix}", conf, flags=re.M)
conf = re.sub(r"^PLUGINS_LEAST_REVOKED=\d+$", f"PLUGINS_LEAST_REVOKED={rv}", conf, flags=re.M)
open("keys/plugins.conf", "w").write(conf)
print("written: commit keys/plugins.conf with the release")
EOF
