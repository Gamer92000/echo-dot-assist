# Sourced by the PC scripts after scripts/lib/device.sh (DEVICE, OUT): where the Echo's binaries in build/<codename>/
# come from.  Built here (make: the Android NDK and the unpacked firmware), or the release CI published of this very
# commit: hassmic-<codename>.bundle from UPDATE_REPO's releases, checked against RELEASE_PUB (device.sh release_load:
# this project's, or a fork's own) and unpacked by scripts/otatool.py, so nothing has to be installed or compiled for
# it.  It is what online updates install, the same files byte for byte.
#   PREBUILT=1    the release's; stop if this commit has none
#   PREBUILT=0    always build
#   (unset)       build where the NDK is there, else the release's
# $OUT/PREBUILT marks a release's files there: "<tag> <commit> <version>", then their names.  A build here removes them
# first, or make would take the downloaded binaries (newer than the sources) as up to date.

_head() { git rev-parse HEAD 2>/dev/null; }
_vhead() { TZ=UTC0 git log -1 --format=%cd --date=format-local:%Y.%m.%d.%H%M%S; }
# no changes to tracked files: only then is the release's build this tree's
_clean() { git diff --quiet HEAD -- 2>/dev/null; }
# _gh_tag TAG: the commit GitHub's tag points at (CI's tags are lightweight)
_gh_tag() {
    curl -fsS --max-time 20 "https://api.github.com/repos/$UPDATE_REPO/git/ref/tags/$1" 2>/dev/null |
        sed -n 's/.*"sha": *"\([0-9a-f]*\)".*/\1/p' | head -1
}
# _bundle_free PATH...: none of them goes into a model's bundle (scripts/bundle.sh) or into making what does: the docs,
# the tests, the PC's own scripts.  Anything not named counts as the Echo's: wrong that way, a commit only goes without
# a release build, as all did before.
_bundle_free() {
    for _p; do
        case $_p in
        scripts/device/*|scripts/system/*|scripts/bundle.sh|scripts/content-id.sh|scripts/otatool.py|\
        scripts/lib/device.sh|scripts/kernel-tools.sh) return 1;;
        *.md|docs/*|tests/*|testdata/*|blueprints/*|.github/*|scripts/*|devices/*/setup.sh|devices/*/probe.md5|\
        LICENSE|.gitignore|.gitattributes|pyproject.toml|uv.lock) ;;
        *) return 1;;
        esac
    done
}

# _ci_same COMMIT: the release CI found COMMIT's bundles the same as.  A build workflow that ran on its push and passed
# without publishing (no tag of it) compared every model's content ID with the newest release's in its release job's
# step "Anything new" (gh release list's first: the newest by created_at, which GitHub sets to the tag commit's time)
# and found them all alike; that release is the newest one published before that step ended.  Not before the run
# ended: a release from the other branch can come out in between.  Exact where _bundle_free guesses: #129 changed
# scripts/lib/device.sh, which bundle.sh uses and so counts as the Echo's, and was left without a build (#132).
# Releases go through stdin: their notes outgrow argv.  60 API calls an hour without a login; past them, the 403 is a
# "no" and _bundle_free has its say.
_ci_same() {
    _u=$(curl -fsS --max-time 20 "https://api.github.com/repos/$UPDATE_REPO/actions/runs?head_sha=$1&event=push&status=success" 2>/dev/null |
        python3 -c '
import json, sys
runs = [r for r in json.load(sys.stdin)["workflow_runs"]
        if r["path"] == ".github/workflows/build.yml" and r["conclusion"] == "success"]
print(min(runs, key=lambda r: r["created_at"])["jobs_url"])' 2>/dev/null) || return 1
    # only where its step "Publish" was skipped: a commit that did publish was found different from that release.  A
    # release job skipped (its tag there already) has no steps: no answer
    _e=$(curl -fsS --max-time 20 "$_u" 2>/dev/null | python3 -c '
import json, sys
job = [j for j in json.load(sys.stdin)["jobs"] if j["name"] == "release"][0]
step = {s["name"]: s for s in job["steps"]}
step["Publish"]["conclusion"] == "skipped" and step["Anything new"]["conclusion"] == "success" or sys.exit(1)
print(step["Anything new"]["completed_at"])' 2>/dev/null) || return 1
    curl -fsS --max-time 20 "https://api.github.com/repos/$UPDATE_REPO/releases?per_page=30" 2>/dev/null | python3 -c '
import json, sys
before = [r for r in json.load(sys.stdin) if not r["draft"] and r["published_at"] and r["published_at"] <= sys.argv[1]]
before or sys.exit(1)
print(max(before, key=lambda r: r["created_at"])["tag_name"])' "$_e" 2>/dev/null
}

# prebuilt_tag: the release of HEAD.  CI tags v<commit time, UTC> (-beta from main) on the commit it built.  It
# publishes no beta of a commit that changes nothing on the Echos (scripts/content-id.sh: docs, tests, the PC's
# scripts), so failing that, the release CI compared it with (_ci_same), and before CI is done, the newest release tag
# before HEAD when nothing after it touches a bundle (issue #106: a README commit on top of main left Windows, which
# builds nothing, no release to take).  Fails for a tree with changes, a commit not pushed, CI not done yet with a
# change to the Echo's files, or no git checkout.
prebuilt_tag() {
    _clean || return 1
    _h=$(_head) _v=$(_vhead)
    for _t in v$_v-beta v$_v; do
        [ "$(_gh_tag $_t)" = "$_h" ] && { echo "$_t"; return 0; }
    done
    _t=$(_ci_same $_h) && [ -n "$_t" ] && { echo "$_t"; return 0; }
    # --no-renames: a file moved out of src/ into docs/ is listed under both names
    _t=$(git describe --tags --abbrev=0 --match 'v[0-9]*' HEAD 2>/dev/null) && _c=$(git rev-parse "$_t^{commit}") &&
        _bundle_free $(git diff --name-only --no-renames $_c HEAD) && [ "$(_gh_tag $_t)" = "$_c" ] &&
        { echo "$_t"; return 0; }
    return 1
}
# prebuilt_of TAG: which commit's the release is, for messages
prebuilt_of() {
    if [ "${1%-beta}" = "v$(_vhead)" ]; then echo "of this commit ($1)"
    else echo "for this commit ($1: what came after it changes nothing on the Echo)"; fi
}

# prebuilt_current: $OUT holds the release of HEAD already
prebuilt_current() {
    [ -f $OUT/PREBUILT ] && _clean && [ "$(sed -n '1s/^[^ ]* \([^ ]*\) .*/\1/p' $OUT/PREBUILT)" = "$(_head)" ]
}

# prebuilt_clear: a release's files out of $OUT (before a build here)
prebuilt_clear() {
    [ -f $OUT/PREBUILT ] || return 0
    for _f in $(sed 1d $OUT/PREBUILT); do rm -f "$OUT/$_f"; done
    rm -f $OUT/PREBUILT
}

# prebuilt_fetch TAG: download the release's bundle for this model, verify it, put its binaries into $OUT.  The scripts
# and device.conf in it are this checkout's own (same commit): those stay where they are.
prebuilt_fetch() {
    _d=$OUT/.prebuilt.$$ _b=hassmic-$DEVICE.bundle
    rm -rf $_d; mkdir -p $_d
    for _f in $_b $_b.sig; do
        curl -fsSL --retry 3 -o $_d/$_f "https://github.com/$UPDATE_REPO/releases/download/$1/$_f" ||
            { rm -rf $_d; echo "cannot download $_f of release $1" >&2; return 1; }
    done
    python3 scripts/otatool.py install $RELEASE_PUB $_d/$_b $_d/$_b.sig $_d/files > /dev/null ||
        { rm -rf $_d; echo "release $1: $_b does not verify against $RELEASE_PUB" >&2; return 1; }
    prebuilt_clear
    _names=
    for _f in $_d/files/*; do
        case ${_f##*/} in VERSION|device.conf|release.pub|hassmic.rc|*.sh) continue;; esac
        cp -p $_f $OUT/ && _names="$_names ${_f##*/}"
    done
    { echo "$1 $(_head) $(cat $_d/files/VERSION)"; printf '%s\n' $_names; } > $OUT/PREBUILT
    rm -rf $_d
    echo "release build $(prebuilt_of $1), verified:$_names"
}

# build_binaries: the device binaries into $OUT, from wherever PREBUILT says
build_binaries() {
    mkdir -p $OUT
    if [ "$PREBUILT" != 0 ]; then
        prebuilt_current && { echo "release build $(prebuilt_of "$(head -1 $OUT/PREBUILT | cut -d' ' -f1)")"; return 0; }
        if [ "$PREBUILT" = 1 ] || [ ! -d toolchain/android-ndk-r21e ]; then
            if _t=$(prebuilt_tag); then prebuilt_fetch "$_t"; return; fi
            [ "$PREBUILT" = 1 ] && die "GitHub has no release build of this commit (changes here, not pushed, or CI not done yet); build it here: PREBUILT=0"
            echo "no release build of this commit on GitHub, and no Android NDK to build with: $DDIR/README.md" >&2
        fi
    fi
    prebuilt_clear
    make -s all DEVICE=$DEVICE
}

# build_version: what the hassmic in $OUT reports, and so what a bundle of it must be called
build_version() {
    if [ -f $OUT/PREBUILT ]; then head -1 $OUT/PREBUILT | cut -d' ' -f3; else make -s version; fi
}
