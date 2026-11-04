#!/bin/bash
# A fork's own releases (scripts/fork-release.sh; keys/release.conf, secrets/fork.conf): the repository the Makefile
# compiles in, what scripts/lib/device.sh release_load gives the PC scripts, the key bundle.sh packs, CI's ci mode, the
# values both make and sh must read alike, the setup's refusals, and what it asks of GitHub, against a stand-in gh.
# Never this checkout's secrets/ (the maintainer's release key may be there): a tree of links, its own secrets/.
cd "$(dirname "$0")/../.."; ROOT=$PWD; Q=$(mktemp -d); trap 'rm -rf "$Q"' EXIT; fail=0
ok_() { if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; fail=1; fi; }
UP=$(sed -n 's/^UPDATE_REPO=//p' keys/release.conf)
mkdir -p $Q/r $Q/bin
for e in "$ROOT"/*; do case ${e##*/} in secrets|build) ;; *) ln -s "$e" $Q/r/;; esac; done
cd $Q/r
python3 scripts/otatool.py keygen $Q/f.key $Q/f.pub > /dev/null

# the repository the Makefile passes to update.c, without building anything
repo() { make -s -n build/hassmic-host 2>&1 | grep -o "DREPO='\"[^\"]*\"'" | head -1 | sed "s/^DREPO='\"//; s/\"'\$//"; }
loaded() { bash -c '. scripts/lib/device.sh; release_load; echo "$UPDATE_REPO $RELEASE_PUB"' 2>&1; }

[ "$(repo)" = "$UP" ] && [ "$(loaded)" = "$UP keys/release.pub" ]; ok_ $? "no fork.conf: this project's repository and key ($(repo))"
env -u RELEASE_PUBLIC_KEY GITHUB_REPOSITORY=me/fork sh scripts/fork-release.sh ci > /dev/null && [ ! -e secrets ]
ok_ $? "ci without the variable: nothing written"
! RELEASE_PUBLIC_KEY=Zm9v GITHUB_REPOSITORY=me/fork sh scripts/fork-release.sh ci > /dev/null 2>&1; ok_ $? "ci: a variable that is no key fails the job"
RELEASE_PUBLIC_KEY=$(base64 -w0 $Q/f.pub) GITHUB_REPOSITORY=me/fork sh scripts/fork-release.sh ci > /dev/null &&
    cmp -s secrets/release.pub $Q/f.pub && [ "$(repo)" = me/fork ] && [ "$(loaded)" = "me/fork secrets/release.pub" ]
ok_ $? "ci: the variable's key and the workflow's repository, for make and the scripts"

# bundle.sh packs the fork's key as release.pub, the name the Echo looks for
mkdir -p build/donut; for f in hassmic runas otatool; do echo $f > build/donut/$f; done
DEVICE=donut sh scripts/bundle.sh $Q/f.key 1 > /dev/null &&
    python3 scripts/otatool.py install secrets/release.pub build/donut/hassmic.bundle build/donut/hassmic.bundle.sig $Q/u > /dev/null &&
    cmp -s $Q/u/release.pub $Q/f.pub
ok_ $? "bundle.sh: the fork's key in it as release.pub"

# make keeps what sh drops or reads otherwise: such a fork.conf stops both, never one of them going elsewhere
conf() { printf '%s\nRELEASE_PUB=secrets/release.pub\n' "$1" > secrets/fork.conf; }
for line in 'UPDATE_REPO="me/fork"' "UPDATE_REPO='me/fork'" 'UPDATE_REPO = me/fork'; do
    conf "$line"
    make -s -n build/hassmic-host > /dev/null 2>&1; m=$?
    loaded | grep -q 'no quotes or spaces'; s=$?
    { [ $m != 0 ] || [ "$(repo)" = me/fork ]; } && [ $s = 0 ]; ok_ $? "fork.conf $line: refused (make $m, scripts)"
done
conf 'UPDATE_REPO=me/fork   # the fork'
[ "$(repo)" = me/fork ] && [ "$(loaded)" = "me/fork secrets/release.pub" ]; ok_ $? "fork.conf with a comment: the same for both"
printf 'UPDATE_REPOS=a\nUPDATE_REPO=me/fork\nRELEASE_PUB=secrets/release.pub\n' > secrets/fork.conf
[ "$(loaded)" = "me/fork secrets/release.pub" ]; ok_ $? "another name that starts the same: not taken for UPDATE_REPO"
rm -rf secrets build

# the setup's refusals, all before GitHub is asked anything
cat > $Q/bin/gh << EOF
#!/bin/sh
echo "\$*" >> $Q/gh.log
case "\$*" in
"repo view "*) echo "\${GH_PERM:-ADMIN} \${GH_NAME:-me/fork}";;
"api repos/"*"/environments/release/deployment-branch-policies --jq "*) printf "\$GH_POLICIES";;
"api repos/"*"/environments/release --jq "*)
    case \$GH_ENV in none) echo 'gh: Not Found (HTTP 404)' >&2; exit 1;; *) echo \$GH_ENV;; esac;;
"secret set "*) cat > $Q/secret;;
"variable set "*) echo "\$*" | sed 's/.*--body //' > $Q/variable;;
esac
EOF
chmod +x $Q/bin/gh; export PATH=$Q/bin:$PATH
setup() { rm -f $Q/gh.log; sh scripts/fork-release.sh "$@" 2>&1; }
writes() { grep -qs -e '-X PUT' -e '-X POST' -e 'secret set' -e 'variable set' $Q/gh.log; }
for a in "$UP" "$(echo "$UP" | tr a-z A-Z)" a/b/c x 'me/re po' me/; do
    setup "$a" | grep -q . && [ ! -e $Q/gh.log ] && [ ! -e secrets ]; ok_ $? "refused before GitHub: '$a'"
done
mkdir secrets; touch secrets/release.key
setup me/fork | grep -q 'not a fork' && [ ! -e $Q/gh.log ]; ok_ $? "a release key without fork.conf (the maintainer's PC): left alone"
rm secrets/release.key; echo UPDATE_REPO=other/fork > secrets/fork.conf
setup me/fork | grep -q "other/fork's" && [ ! -e $Q/gh.log ]; ok_ $? "another fork's fork.conf: refused"
echo '# nothing' > secrets/fork.conf
setup me/fork | grep -q "is 's;" && [ ! -e $Q/gh.log ]; ok_ $? "a fork.conf without UPDATE_REPO: named as such, not as $UP's"
rm -rf secrets
GH_PERM=WRITE setup me/fork | grep -q 'admin rights' && [ ! -e secrets ]; ok_ $? "not an admin of the fork: refused, no key made"
GH_NAME=$UP setup old/name | grep -q 'this project itself' && [ ! -e secrets ] && ! writes
ok_ $? "a name GitHub redirects to this project: refused, no key made, nothing sent"
# GitHub's side looked at first: refused there, nothing written here or there
GH_ENV=all setup me/fork | grep -q 'admits every branch' && [ ! -e secrets ] && ! writes
ok_ $? "an environment for every branch: refused, no key made, nothing sent"
GH_ENV=custom GH_POLICIES='main\n**\n' setup me/fork | grep -q "pattern '\*\*'" && [ ! -e secrets ] && ! writes
ok_ $? "a pattern that admits every branch: refused, no key made, nothing sent"

# a fork without the environment: made for main and release, the key's halves as secret and variable, under the name as
# GitHub spells it (CI's GITHUB_REPOSITORY), whatever was typed
GH_ENV=none GH_POLICIES= GH_NAME=Me/Fork setup me/fork > /dev/null
[ $? = 0 ] && [ "$(stat -c %s secrets/release.key)" = 64 ] && [ "$(stat -c %a secrets/release.key)" = 600 ] &&
    [ "$(grep -v '^#' secrets/fork.conf)" = "$(printf 'UPDATE_REPO=Me/Fork\nRELEASE_PUB=secrets/release.pub')" ] &&
    grep -q '^api -X PUT repos/Me/Fork/environments/release .*custom_branch_policies\]=true' $Q/gh.log &&
    grep -q 'repos/Me/Fork/environments/release/deployment-branch-policies -f name=main -f type=branch' $Q/gh.log &&
    grep -q 'repos/Me/Fork/environments/release/deployment-branch-policies -f name=release -f type=branch' $Q/gh.log &&
    grep -q '^secret set RELEASE_SIGNING_KEY --env release --repo Me/Fork' $Q/gh.log &&
    [ "$(base64 -d $Q/secret | cmp - secrets/release.key && echo same)" = same ] &&
    [ "$(base64 -d $Q/variable | cmp - secrets/release.pub && echo same)" = same ]
ok_ $? "new: key pair, fork.conf in GitHub's spelling, environment for main and release, secret and variable"
cp secrets/release.key $Q/k1 && cp secrets/release.pub $Q/p1
python3 scripts/otatool.py keygen $Q/k2 $Q/p2 > /dev/null; cp $Q/p2 secrets/release.pub
setup me/fork | grep -q 'not one key pair' && ! writes; ok_ $? "again, the halves of two key pairs: refused, nothing sent"
cp $Q/p1 secrets/release.pub
out=$(GH_ENV=custom GH_POLICIES='main\nrelease\nfeature\n' setup me/fork)
[ $? = 0 ] && cmp -s secrets/release.key $Q/k1 && ! grep -q -e '-X PUT' -e '-X POST' $Q/gh.log &&
    echo "$out" | grep -q 'also admits: feature'
ok_ $? "again: key kept, the environment left as it is, another branch it admits named"
out=$(GH_ENV=protected setup me/fork)
[ $? = 0 ] && ! grep -q -e '-X PUT' -e '-X POST' -e 'deployment-branch-policies' $Q/gh.log && grep -q '^secret set' $Q/gh.log &&
    echo "$out" | grep -q 'admits protected branches'
ok_ $? "protected branches only: taken as it is, no branch policies, the key given"
exit $fail
