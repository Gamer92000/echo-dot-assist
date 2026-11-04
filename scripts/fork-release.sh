#!/bin/sh
# Releases of a fork's own, for online updates (DEVELOPMENT.md "Releases from a fork").  Online updates install only
# what is signed with the key a build carries; a fork's CI cannot sign with this project's, so a fork that publishes
# builds signs them with a key of its own, and its Echos then look for updates in the fork.
#   scripts/fork-release.sh OWNER/REPO    on the PC, once: key pair, secrets/fork.conf, and on GitHub (gh, as an admin
#                                         of the fork) the environment `release` with the key's secret half and the
#                                         repository variable RELEASE_PUBLIC_KEY with its public half
#   scripts/fork-release.sh ci            in CI (build.yml): secrets/fork.conf from RELEASE_PUBLIC_KEY and the
#                                         repository the workflow runs in; nothing without the variable (this project's)
# Nothing of it goes into git (secrets/ is ignored): the fork's tree stays this one, so its branches make pull requests
# here as any other.  Run again with the same OWNER/REPO, it keeps the key and sets GitHub's side again (a new PC: copy
# secrets/ from the old one first).
set -e
cd "$(dirname "$0")/.."
die() { echo "$*" >&2; exit 1; }
. keys/release.conf
UPSTREAM=$UPDATE_REPO
lower() { printf '%s' "$1" | tr 'A-Z' 'a-z'; }
b64() { base64 < "$1" | tr -d '\n'; }               # no -w0: macOS's base64 has none
# secrets/fork.conf, read by make and sh alike (keys/release.conf); this script's own file, written whole every run
conf() { printf '# written by scripts/fork-release.sh, whole on every run: lines added here are lost\nUPDATE_REPO=%s\nRELEASE_PUB=secrets/release.pub\n' "$1" > secrets/fork.conf; }

if [ "$1" = ci ]; then
    [ -n "$RELEASE_PUBLIC_KEY" ] || { echo "no RELEASE_PUBLIC_KEY: this project's repository and key ($UPSTREAM)"; exit 0; }
    [ -n "$GITHUB_REPOSITORY" ] || die "ci: no GITHUB_REPOSITORY"
    mkdir -p secrets
    printf '%s' "$RELEASE_PUBLIC_KEY" | base64 -d > secrets/release.pub 2>/dev/null &&
        [ "$(wc -c < secrets/release.pub)" -eq 32 ] || die "::error::the variable RELEASE_PUBLIC_KEY is not the base64 of a public key (scripts/fork-release.sh)"
    conf "$GITHUB_REPOSITORY"
    echo "a fork's releases: $GITHUB_REPOSITORY, key $(sha256sum secrets/release.pub | cut -c1-16)"
    exit 0
fi

[ $# = 1 ] || die "usage: scripts/fork-release.sh OWNER/REPO    (the fork on GitHub whose CI is to publish releases)"
REPO=$1
case $REPO in [A-Za-z0-9]*/[A-Za-z0-9._-]*) ;; *) die "'$REPO' is not OWNER/REPO";; esac
case $REPO in */*/*|*[!A-Za-z0-9._/-]*) die "'$REPO' is not OWNER/REPO";; esac
[ "$(lower "$REPO")" != "$(lower "$UPSTREAM")" ] || die "$REPO is this project itself: its key is the maintainer's (DEVELOPMENT.md)"

# secrets/fork.conf and the key: this fork's or none
newkey=
if [ -f secrets/fork.conf ]; then
    had=$(UPDATE_REPO=; . secrets/fork.conf; echo "$UPDATE_REPO")     # not keys/release.conf's, should it lack one
    [ "$(lower "$had")" = "$(lower "$REPO")" ] || die "secrets/fork.conf is $had's; move it away for another fork"
    [ -s secrets/release.key ] && [ -s secrets/release.pub ] || die "secrets/fork.conf without secrets/release.key and .pub: copy them from where the key was made"
    # Monocypher's secret key ends in its public half: halves of two pairs would make every release fail CI's check
    tail -c 32 secrets/release.key | cmp -s - secrets/release.pub ||
        die "secrets/release.key and secrets/release.pub are not one key pair: copy both from where the key was made"
else
    # the maintainer's PC has the project's secrets/release.key: never take that for a fork's, nor overwrite it
    [ ! -e secrets/release.key ] && [ ! -e secrets/release.pub ] ||
        die "secrets/release.key or .pub is there already, without secrets/fork.conf: not a fork's (this project's own?); move them away first"
    newkey=1
fi

command -v gh > /dev/null || die "needs GitHub's CLI, gh (https://cli.github.com), logged in: gh auth login"
# GitHub's own spelling of the name, as CI's GITHUB_REPOSITORY has it: the same bytes in a build here and in CI
got=$(gh repo view "$REPO" --json viewerPermission,nameWithOwner --jq '.viewerPermission + " " + .nameWithOwner') ||
    die "gh cannot see $REPO (gh auth status)"
perm=${got%% *} REPO=${got#* }
# again as GitHub names it: an old name GitHub redirects can be this project's (whose admin is its maintainer)
[ "$(lower "$REPO")" != "$(lower "$UPSTREAM")" ] || die "$1 is this project itself, as $REPO: its key is the maintainer's (DEVELOPMENT.md)"
[ "$perm" = ADMIN ] || die "$REPO: environments and secrets need admin rights there, gh has $perm"

# The environment the release job names, for chosen branches only: the build jobs run whatever a branch or pull request
# brings, and must never see the key (DEVELOPMENT.md "The release key").  Made here only when there is none: a PUT
# carries the whole environment, and one the fork has may have reviewers or a wait timer of its own.  All of it looked
# at before anything is written, here or there: a refusal leaves both as they were.
err=$(mktemp); trap 'rm -f "$err"' EXIT
if env=$(gh api "repos/$REPO/environments/release" --jq 'if .deployment_branch_policy == null then "all"
        elif .deployment_branch_policy.protected_branches then "protected"
        elif .deployment_branch_policy.custom_branch_policies then "custom" else "all" end' 2> "$err"); then :
elif grep -q 'HTTP 404' "$err"; then env=none
else die "$REPO's environment release: $(cat "$err")"
fi
have=
case $env in
all) die "the environment release of $REPO admits every branch (Settings > Environments > release > Deployment branches: \"Selected branches and tags\", or \"Protected branches only\"); set that, then run this again";;
custom)
    have=$(gh api "repos/$REPO/environments/release/deployment-branch-policies" --jq '.branch_policies[] | select(.type != "tag") | .name')
    # a pattern of nothing but * and / matches every branch: all of them would sign
    wide=$(printf '%s\n' "$have" | grep -x '[*/]*' | grep . || true)
    [ -z "$wide" ] || die "the environment release of $REPO admits every branch through the pattern '$wide' (Settings > Environments > release); remove it, then run this again";;
esac

if [ -z "$newkey" ]; then
    echo "key: secrets/release.key, kept"
else
    mkdir -p secrets
    python3 scripts/otatool.py keygen secrets/release.key secrets/release.pub
    echo "key: new, secrets/release.key (back it up: without it, this fork's Echos take updates only from this PC)"
fi
conf "$REPO"

case $env in
none) gh api -X PUT "repos/$REPO/environments/release" -F 'deployment_branch_policy[protected_branches]=false' \
          -F 'deployment_branch_policy[custom_branch_policies]=true' > /dev/null;;
protected) echo "note: the environment release admits protected branches: main and release must be (step 2 below)";;
esac
if [ "$env" != protected ]; then
    for b in main release; do
        printf '%s\n' "$have" | grep -qx "$b" ||
            gh api -X POST "repos/$REPO/environments/release/deployment-branch-policies" -f name=$b -f type=branch > /dev/null
    done
    extra=$(printf '%s\n' "$have" | grep -vx -e main -e release -e '' || true)
    [ -z "$extra" ] || echo "note: the environment release also admits: $extra (each of them can sign; Settings > Environments > release)"
fi
b64 secrets/release.key | gh secret set RELEASE_SIGNING_KEY --env release --repo "$REPO"
gh variable set RELEASE_PUBLIC_KEY --repo "$REPO" --body "$(b64 secrets/release.pub)"
echo "GitHub: environment release with RELEASE_SIGNING_KEY, variable RELEASE_PUBLIC_KEY"

cat << EOF

Builds on this PC now look for updates in $REPO (secrets/fork.conf), and so do the fork's CI builds. Left to do:
  1. https://github.com/$REPO/actions: allow the workflows (a fork's are off until then)
  2. protect main and release in the fork: whoever can push there publishes what its Echos install
  3. push a commit of your own to main: CI publishes a beta signed with the new key (this changed nothing in git, a
     commit whose tag the fork took from $UPSTREAM is not published again, and a manual run never publishes)
  4. each Echo once from this PC, which brings it the key: scripts/ota-push.sh (installed) or scripts/setup.sh
Back up secrets/ (the key and fork.conf). Pull requests to $UPSTREAM work from any branch: none of this is in git.
EOF
