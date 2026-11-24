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

# the second index key (docs/plugins.md 5.2, a new key's first step): built in as keys/plugins.conf names it, with
# the first key it belongs with (PLUGINS_PUB2_WITH, by content: K5 of a review of a30d6ed), never beside another first
# key, a fork's own where its fork.conf names one; fork-release.sh keeps a fork's index lines when it writes fork.conf
# again, and writes them in CI from the fork's repository variables (K4)
pub2() { make -s -n build/hassmic-host "$@" 2>&1 | grep -o "DPLUGINS_PUB2='\"[^\"]*\"'" | head -1 | sed "s/^DPLUGINS_PUB2='\"//; s/\"'\$//"; }
pub1() { make -s -n build/hassmic-host "$@" 2>&1 | grep -o "DPLUGINS_PUB='\"[^\"]*\"'" | head -1 | sed "s/^DPLUGINS_PUB='\"//; s/\"'\$//"; }
index() { make -s -n build/hassmic-host 2>&1 | grep -o "DPLUGIN_INDEX='\"[^\"]*\"'" | head -1 | sed "s/^DPLUGIN_INDEX='\"//; s/\"'\$//"; }
hexof() { od -An -tx1 -v "$1" | tr -d ' \n'; }
rm keys && mkdir keys && ln -s "$ROOT"/keys/* keys/ && rm keys/plugins.conf && cp "$ROOT"/keys/plugins.conf keys/
python3 scripts/otatool.py keygen $Q/i2.key $Q/i2.pub > /dev/null; python3 scripts/otatool.py keygen $Q/i3.key $Q/i3.pub > /dev/null
UPHEX=$(hexof keys/plugins.pub); UPINDEX=$(index)
[ -z "$(pub2)" ] && make -s -n build/hassmic-host 2>&1 | grep -q "DPLUGINS_PUB2='\"\"'"; ok_ $? "keys/plugins.conf as committed: no second key built in"
sed -i "s|^PLUGINS_PUB2=.*|PLUGINS_PUB2=$Q/i2.pub|" keys/plugins.conf
make -s -n build/hassmic-host 2>&1 | grep -q 'without PLUGINS_PUB2_WITH'; ok_ $? "PLUGINS_PUB2 without PLUGINS_PUB2_WITH: refused"
sed -i "s|^PLUGINS_PUB2_WITH=.*|PLUGINS_PUB2_WITH=$UPHEX|" keys/plugins.conf
[ "$(pub2)" = "$(hexof $Q/i2.pub)" ]; ok_ $? "PLUGINS_PUB2 named, with the first key's hex: built in beside the first"
# K5's three cases: a first key named on make's command line, keys/plugins.pub replaced in place, and the same key
# under another path
[ -z "$(pub2 PLUGINS_PUB=$Q/f.pub)" ] && [ "$(pub1 PLUGINS_PUB=$Q/f.pub)" = "$(hexof $Q/f.pub)" ]; ok_ $? "make PLUGINS_PUB=<a fork's key>: not this project's second key beside it"
# R5 of a review of 3c5d7ff: where PLUGINS_PUB is keys/plugins.conf's own, a PLUGINS_PUB2_WITH that is not its key
# is a typo there (a step-(1) release that silently left the new key out): the build stops.  keys/plugins.pub
# replaced in place by a fork with the second key still named is the same to make: it stops too, never builds it in
rm keys/plugins.pub && cp $Q/f.pub keys/plugins.pub
make -s -n build/hassmic-host 2>&1 | grep -q 'PLUGINS_PUB2 .*belongs with .*a typo in PLUGINS_PUB2_WITH'; ok_ $? "keys/plugins.pub replaced in place by a fork's key, the second key still named: the build stops (never the second key beside it)"
rm keys/plugins.pub && ln -s "$ROOT"/keys/plugins.pub keys/plugins.pub
sed -i "s|^PLUGINS_PUB2_WITH=.*|PLUGINS_PUB2_WITH=$(hexof $Q/f.pub)|" keys/plugins.conf
make -s -n build/hassmic-host 2>&1 | grep -q 'a typo in PLUGINS_PUB2_WITH'; ok_ $? "PLUGINS_PUB2_WITH another key than keys/plugins.conf's own PLUGINS_PUB (a typo upstream): the build stops"
sed -i "s|^PLUGINS_PUB2_WITH=.*|PLUGINS_PUB2_WITH=$(printf %s "$UPHEX" | tr a-f A-F)|" keys/plugins.conf
[ "$(pub2)" = "$(hexof $Q/i2.pub)" ]; ok_ $? "PLUGINS_PUB2_WITH in capitals: the same key, the second built in"
# (S6 of a review of 3fcd319: a value on make's command line goes past a plain assignment, and was compared as typed)
[ "$(pub2 PLUGINS_PUB2_WITH=$(printf %s "$UPHEX" | tr a-f A-F))" = "$(hexof $Q/i2.pub)" ]; ok_ $? "make PLUGINS_PUB2_WITH=<capitals>: the same key, the second built in"
sed -i "s|^PLUGINS_PUB2_WITH=.*|PLUGINS_PUB2_WITH=${UPHEX%?}|" keys/plugins.conf
make -s -n build/hassmic-host 2>&1 | grep -q 'PLUGINS_PUB2_WITH .*not the 64 hex digits'; ok_ $? "PLUGINS_PUB2_WITH of 63 digits: the build stops"
sed -i "s|^PLUGINS_PUB2_WITH=.*|PLUGINS_PUB2_WITH=$UPHEX|" keys/plugins.conf
printf 'PLUGINS_PUB=./keys/plugins.pub\n' > secrets/fork.conf
[ "$(pub2)" = "$(hexof $Q/i2.pub)" ]; ok_ $? "fork.conf naming this project's key under another path (./keys/plugins.pub): the second key kept (the same first key)"
rm secrets/fork.conf
printf 'PLUGINS_PUB=%s\n' $Q/f.pub >> secrets/fork.conf
[ -z "$(pub2)" ]; ok_ $? "a fork with an index key of its own: never this project's second key beside it"
printf 'PLUGINS_PUB2=%s\nPLUGINS_PUB2_WITH=%s\n' $Q/i3.pub "$(hexof $Q/f.pub)" >> secrets/fork.conf
[ "$(pub2)" = "$(hexof $Q/i3.pub)" ]; ok_ $? "... its own second key where its fork.conf names one with its first"
# scripts/plugins-least.sh reads the site with the keys the Makefile builds in: the second only with its first
mkdir -p $Q/site; python3 - $Q/site $Q/i3.key << 'PY'
import sys; sys.path.insert(0, "scripts"); import otatool
site, key = sys.argv[1:]; sk = otatool.secret(key)
for name, purpose, text in (("index.json", b"hassmic plugin index 1\n", b'{"index": 1, "serial": 7}'), ("revoked.json", b"hassmic plugin revoked 1\n", b'{"revoked": 1, "serial": 5}')):
    open(f"{site}/{name}", "wb").write(text); open(f"{site}/{name}.sig", "wb").write(otatool.sign(sk, purpose + text))
PY
least() { sh scripts/plugins-least.sh --check file://$Q/site/ 2>&1; }
least | grep -q 'the site: index 7, revocation list 5'; ok_ $? "plugins-least.sh: a site signed by the fork's second key, read with it beside its first"
sed -i "s|^PLUGINS_PUB2_WITH=.*|PLUGINS_PUB2_WITH=$UPHEX|" secrets/fork.conf
least | grep -q 'not signed by an index key'; ok_ $? "plugins-least.sh: the second key named with another first key: not read (the Makefile's rule)"
sed -i "s|^PLUGINS_PUB2_WITH=.*|PLUGINS_PUB2_WITH=$(hexof $Q/f.pub)|" secrets/fork.conf
printf 'PLUGIN_INDEX=https://me.example/idx/\n' >> secrets/fork.conf
RELEASE_PUBLIC_KEY=$(base64 -w0 $Q/f.pub) GITHUB_REPOSITORY=me/fork sh scripts/fork-release.sh ci > /dev/null &&
    [ "$(grep -v '^#' secrets/fork.conf)" = "$(printf 'UPDATE_REPO=me/fork\nRELEASE_PUB=secrets/release.pub\nPLUGINS_PUB=%s\nPLUGINS_PUB2=%s\nPLUGINS_PUB2_WITH=%s\nPLUGIN_INDEX=https://me.example/idx/' $Q/f.pub $Q/i3.pub "$(hexof $Q/f.pub)")" ] &&
    [ "$(pub2)" = "$(hexof $Q/i3.pub)" ] && [ "$(repo)" = me/fork ]
ok_ $? "fork-release.sh writes fork.conf again: the fork's own index lines kept"
sed -i "s|^PLUGINS_PUB2=.*|PLUGINS_PUB2=$Q/f.pub|" secrets/fork.conf
make -s -n build/hassmic-host 2>&1 | grep -q 'the same key as PLUGINS_PUB'; ok_ $? "a second key that is the first: refused"
sed -i "s|^PLUGINS_PUB2=.*|PLUGINS_PUB2=$Q/f.key|" secrets/fork.conf
make -s -n build/hassmic-host 2>&1 | grep -q 'PLUGINS_PUB2 .*not a public key of 32 bytes'; ok_ $? "a second key that is no key: refused"
# K3: a key under which any signature holds (small order: all zero, the identity) is refused for each key the build
# carries
head -c 32 /dev/zero > $Q/zero.pub; printf '\001' > $Q/id.pub; head -c 31 /dev/zero >> $Q/id.pub
sed -i "s|^PLUGINS_PUB2=.*|PLUGINS_PUB2=$Q/zero.pub|" secrets/fork.conf
make -s -n build/hassmic-host 2>&1 | grep -q 'PLUGINS_PUB2 .*any signature holds'; ok_ $? "a second key of small order (all zero): refused"
# R4 of a review of 3c5d7ff: the encodings Monocypher reads as points of small order and otatool.py's decode() does not
# (it took them for no point, and so for no weak key): refused too; and without python3 nothing passes unasked
for k in 0100000000000000000000000000000000000000000000000000000000000080 ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff \
         edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff \
         eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff; do
    printf "$(printf %s "$k" | sed 's/../\\x&/g')" > $Q/nc.pub
    sed -i "s|^PLUGINS_PUB2=.*|PLUGINS_PUB2=$Q/nc.pub|" secrets/fork.conf
    make -s -n build/hassmic-host 2>&1 | grep -q 'PLUGINS_PUB2 .*any signature holds'; ok_ $? "a second key Monocypher reads as small order, otatool.py's decode() not ($k): refused"
done
sed -i "s|^PLUGINS_PUB2=.*|PLUGINS_PUB2=$Q/i3.pub|" secrets/fork.conf
mkdir -p $Q/nopy; printf '#!/bin/sh\nexit 127\n' > $Q/nopy/python3; chmod +x $Q/nopy/python3
PATH=$Q/nopy:$PATH make -s -n build/hassmic-host 2>&1 | grep -q 'PLUGINS_PUB .*cannot be checked.*needs python3'; ok_ $? "no python3: the keys cannot be checked, the build stops (it took every key)"
sed -i "s|^PLUGINS_PUB2=.*|PLUGINS_PUB2=$Q/i3.pub|; s|^PLUGINS_PUB=.*|PLUGINS_PUB=$Q/id.pub|; s|^PLUGINS_PUB2_WITH=.*|PLUGINS_PUB2_WITH=$(hexof $Q/id.pub)|" secrets/fork.conf
make -s -n build/hassmic-host 2>&1 | grep -q 'PLUGINS_PUB .*any signature holds'; ok_ $? "a first key of small order (the identity): refused"
sed -i "s|^PLUGINS_PUB=.*|PLUGINS_PUB=$Q/f.pub|; s|^PLUGINS_PUB2_WITH=.*|PLUGINS_PUB2_WITH=$(hexof $Q/f.pub)|; s|^RELEASE_PUB=.*|RELEASE_PUB=$Q/zero.pub|" secrets/fork.conf
make -s -n build/hassmic-host 2>&1 | grep -q 'RELEASE_PUB .*any signature holds'; ok_ $? "a release key of small order: refused"
RELEASE_PUBLIC_KEY=$(base64 -w0 $Q/f.pub) GITHUB_REPOSITORY=me/fork sh scripts/fork-release.sh ci > /dev/null
grep -v -e '^PLUGIN' secrets/fork.conf > $Q/fc && cp $Q/fc secrets/fork.conf && cp "$ROOT"/keys/plugins.conf keys/plugins.conf
# K4: in CI the fork's index from its repository variables (secrets/ is not in git: CI never had the PC's fork.conf)
rm -rf secrets
civars() { env RELEASE_PUBLIC_KEY=$(base64 -w0 $Q/f.pub) GITHUB_REPOSITORY=me/fork "$@" sh scripts/fork-release.sh ci; }
civars PLUGIN_INDEX_URL=https://me.example/idx/ PLUGINS_PUBLIC_KEY=$(base64 -w0 $Q/i2.pub) PLUGINS_PUBLIC_KEY2=$(base64 -w0 $Q/i3.pub) > /dev/null &&
    [ "$(index)" = https://me.example/idx/ ] && [ "$(pub1)" = "$(hexof $Q/i2.pub)" ] && [ "$(pub2)" = "$(hexof $Q/i3.pub)" ] && [ "$(repo)" = me/fork ]
ok_ $? "ci: the fork's index, its key and its second key from the variables, built in, beside its releases"
rm -rf secrets
civars PLUGIN_INDEX_URL=https://me.example/idx/ PLUGINS_PUBLIC_KEY=$(base64 -w0 $Q/i2.pub) > /dev/null &&
    [ "$(index)" = https://me.example/idx/ ] && [ "$(pub1)" = "$(hexof $Q/i2.pub)" ] && [ -z "$(pub2)" ]
ok_ $? "ci: one key: no second"
rm -rf secrets
env -u RELEASE_PUBLIC_KEY PLUGIN_INDEX_URL=https://me.example/idx/ PLUGINS_PUBLIC_KEY=$(base64 -w0 $Q/i2.pub) sh scripts/fork-release.sh ci > /dev/null &&
    [ "$(index)" = https://me.example/idx/ ] && [ "$(repo)" = "$UP" ] && [ ! -e secrets/release.pub ]
ok_ $? "ci: an index of its own without releases of its own: the index's lines alone"
rm -rf secrets
! civars PLUGIN_INDEX_URL=https://me.example/idx/ > /dev/null 2>&1 && ! civars PLUGINS_PUBLIC_KEY=$(base64 -w0 $Q/i2.pub) > /dev/null 2>&1 &&
    ! civars PLUGIN_INDEX_URL=http://me.example/idx/ PLUGINS_PUBLIC_KEY=$(base64 -w0 $Q/i2.pub) > /dev/null 2>&1 &&
    ! civars PLUGIN_INDEX_URL=https://me.example/idx/ PLUGINS_PUBLIC_KEY=Zm9v > /dev/null 2>&1 &&
    ! civars PLUGIN_INDEX_URL=https://me.example/idx/ PLUGINS_PUBLIC_KEY=$(base64 -w0 $Q/i2.pub) PLUGINS_PUBLIC_KEY2=Zm9v > /dev/null 2>&1
ok_ $? "ci: an address without a key, a key without an address, an http address, a variable that is no key: the job fails"
rm -rf secrets
civars > /dev/null && [ "$(index)" = "$UPINDEX" ] && [ "$(pub1)" = "$UPHEX" ]; ok_ $? "ci without the index's variables: this project's index"

# bundle.sh packs the fork's key as release.pub, the name the Echo looks for
mkdir -p build/donut; for f in hassmic runas plugrun otatool; do echo $f > build/donut/$f; done
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
"variable list "*) printf "\$GH_VARS";;
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
! grep -q 'variable set PLUGIN' $Q/gh.log; ok_ $? "no index of the fork's own: no index variables"
printf 'PLUGIN_INDEX=https://me.example/idx/\nPLUGINS_PUB=%s\n' $Q/i2.pub >> secrets/fork.conf
GH_ENV=protected setup me/fork > /dev/null; grep -q '^variable set PLUGIN_INDEX_URL --repo me/fork --body https://me.example/idx/' $Q/gh.log &&
    grep -q "^variable set PLUGINS_PUBLIC_KEY --repo me/fork --body $(base64 -w0 $Q/i2.pub)\$" $Q/gh.log && ! grep -q 'PLUGINS_PUBLIC_KEY2' $Q/gh.log &&
    grep -q '^PLUGINS_PUB=' secrets/fork.conf
ok_ $? "a fork.conf with an index of its own: its address and key as the variables CI reads (K4)"
# R3 of a review of 3c5d7ff: what fork.conf no longer names is deleted on GitHub (CI goes by the variables alone, and
# built a dropped key in); fork.conf's values read a line each (an empty PLUGIN_INDEX shifted the key into its place)
printf 'PLUGINS_PUB2=%s\n' $Q/i3.pub >> secrets/fork.conf
GH_ENV=protected GH_VARS='RELEASE_PUBLIC_KEY\nPLUGIN_INDEX_URL\nPLUGINS_PUBLIC_KEY\n' setup me/fork > /dev/null
grep -q "^variable set PLUGINS_PUBLIC_KEY2 --repo me/fork --body $(base64 -w0 $Q/i3.pub)\$" $Q/gh.log && ! grep -q 'variable delete' $Q/gh.log
ok_ $? "a second key named: set, nothing deleted"
grep -v '^PLUGINS_PUB2=' secrets/fork.conf > $Q/fc && cp $Q/fc secrets/fork.conf
GH_ENV=protected GH_VARS='RELEASE_PUBLIC_KEY\nPLUGIN_INDEX_URL\nPLUGINS_PUBLIC_KEY\nPLUGINS_PUBLIC_KEY2\n' setup me/fork > /dev/null
grep -qx 'variable delete PLUGINS_PUBLIC_KEY2 --repo me/fork' $Q/gh.log && [ "$(grep -c 'variable delete' $Q/gh.log)" = 1 ] &&
    grep -q '^variable set PLUGINS_PUBLIC_KEY --repo' $Q/gh.log
ok_ $? "the second key dropped from fork.conf: its variable deleted on GitHub, the rest set"
GH_ENV=protected GH_VARS='RELEASE_PUBLIC_KEY\nPLUGIN_INDEX_URL\nPLUGINS_PUBLIC_KEY\n' setup me/fork > /dev/null
! grep -q 'variable delete' $Q/gh.log; ok_ $? "... and none there to delete: nothing asked"
sed -i 's|^PLUGIN_INDEX=.*|PLUGIN_INDEX=|' secrets/fork.conf; printf 'PLUGINS_PUB2=%s\n' $Q/i3.pub >> secrets/fork.conf
GH_ENV=protected GH_VARS='RELEASE_PUBLIC_KEY\nPLUGIN_INDEX_URL\nPLUGINS_PUBLIC_KEY\nPLUGINS_PUBLIC_KEY2\n' setup me/fork > /dev/null
! grep -q 'variable set PLUGIN' $Q/gh.log && grep -qx 'variable delete PLUGIN_INDEX_URL --repo me/fork' $Q/gh.log &&
    grep -qx 'variable delete PLUGINS_PUBLIC_KEY --repo me/fork' $Q/gh.log && grep -qx 'variable delete PLUGINS_PUBLIC_KEY2 --repo me/fork' $Q/gh.log
ok_ $? "PLUGIN_INDEX empty: no index of its own (the key's path never taken for the address), all three deleted"
grep -v '^PLUGIN' secrets/fork.conf > $Q/fc && cp $Q/fc secrets/fork.conf
GH_ENV=protected GH_VARS='RELEASE_PUBLIC_KEY\nPLUGIN_INDEX_URL\nPLUGINS_PUBLIC_KEY\n' setup me/fork > /dev/null
grep -qx 'variable delete PLUGIN_INDEX_URL --repo me/fork' $Q/gh.log && grep -qx 'variable delete PLUGINS_PUBLIC_KEY --repo me/fork' $Q/gh.log &&
    ! grep -q 'variable delete PLUGINS_PUBLIC_KEY2' $Q/gh.log
ok_ $? "no index lines left in fork.conf: the index's variables there deleted"
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
