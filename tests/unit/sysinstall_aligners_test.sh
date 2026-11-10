#!/bin/bash
# The wake word aligners onto /system (scripts/system/sysinstall.sh), run on the PC with its paths moved into a folder
# and stand-ins for what needs the Echo (id, getprop, df, chown, chcon, stat's owner, otatool).  New ones go in where the
# partition takes them beside everything else the run writes, with a megabyte to spare; those already there and the same
# stay.  Held against the aligners alone, a first install with room for them but not for the rest took them and then
# failed; an update counted the ones it kept against the room they already took, and on a nearly full partition dropped
# them, to write them again the time after.  And the installers stage them at all: until 2026-11-09 a first install
# left them out (PLAN.md).
cd "$(dirname "$0")/../.."; Q=$(mktemp -d); trap 'rm -rf "$Q"' EXIT; fail=0
ok_() { if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; sed 's/^/     /' $Q/out; fail=1; fi; }

R=$Q/root; H=$R/system/hassmic; mkdir -p $Q/bin $R/system/etc/init $R/system/app
printf '#!/bin/sh\necho 0\n' > $Q/bin/id
printf '#!/bin/sh\necho donut\n' > $Q/bin/getprop
printf '#!/bin/sh\nexit 0\n' > $Q/bin/chown
# chcon fails on a file whose name ends in FAKE_FAIL: a write that fails half way
printf '#!/bin/sh\ncase "$2" in *"$FAKE_FAIL") [ -n "$FAKE_FAIL" ] && exit 1;; esac\nexit 0\n' > $Q/bin/chcon
# stat -c %%a:%%u:%%g: the real mode, and the owner chown would have set (not root here), so new() skips a file as there
cat > $Q/bin/stat << EOF
#!/bin/sh
case "\$3" in $H/*) o=0:2000;; *) o=0:0;; esac
echo "\$($(command -v stat) -c %a "\$3"):\$o"
EOF
# toybox's df: a header, then the file system's line; FAKE_ROOM is the "Available" column
printf '#!/bin/sh\necho "Filesystem 1K-blocks Used Available Use%% Mounted on"\necho "/dev/root 999820 245676 $FAKE_ROOM 25%% /"\n' > $Q/bin/df
chmod 755 $Q/bin/*
sed "s#/system#$R/system#g; s#:-/sepolicy}#:-$R/sepolicy}#" scripts/system/sysinstall.sh > $Q/sysinstall.sh

S=$Q/src; mkdir $S
for f in boot.sh main.sh lockdown.sh; do echo ': ok' > $S/$f; done
printf '#!/bin/sh\nexit 0\n' > $S/otatool; chmod 755 $S/otatool
echo 'PRODUCT=donut' > $S/device.conf; echo key > $S/update.pub; echo 'service hassmic' > $S/hassmic.rc; echo r > $S/runas
rnd() { head -c $2 /dev/urandom > $1; }
rnd $S/hassmic 2000000; rnd $S/word-de-DE.wa 800000; rnd $S/german.was 1600000
kb() { du -kc "$@" | tail -1 | cut -f1; }               # the sizes as the run counts them, whatever this PC's blocks are
wa=$(kb $S/*.wa $S/*.was); all=$(kb $S/*); files=$(ls $S | wc -l)

run() {        # MODE ROOM: the exit status in r, the output in $Q/out
    FAKE_ROOM=$2 PATH=$Q/bin:$PATH sh $Q/sysinstall.sh $1 $S > $Q/out 2>&1; r=$?
}
in_() { cmp -s $S/word-de-DE.wa $H/word-de-DE.wa && cmp -s $S/german.was $H/german.was; }
out_() { [ ! -e $H/word-de-DE.wa ] && [ ! -e $H/german.was ]; }
said() { grep -q 'no room for the wake word aligners' $Q/out; }
wrote() { grep -q ": $1 files written" $Q/out; }
fresh() { rm -rf $H $R/system/etc/init/hassmic.rc $R/system/app/HassmicHome; }     # nothing installed: every file counts

# --- a first install
fresh; run install $((all + 1025)); [ $r = 0 ] && in_ && ! said && cmp -s $S/hassmic $H/hassmic && wrote $files
ok_ $? "room for all of it and a megabyte: the aligners go in"
fresh; run install $((all + 1024)); [ $r = 0 ] && out_ && said && cmp -s $S/hassmic $H/hassmic &&
    grep -q "$all kB to write with them, $((all + 1024)) kB free" $Q/out
ok_ $? "exactly a megabyte left over: left out, said with both sizes, the rest written"
fresh; run install $((wa + 1100)); [ $r = 0 ] && out_ && said
ok_ $? "room for the aligners alone but not beside hassmic: left out (counted alone, they went in)"
fresh; run install ''; [ $r = 0 ] && out_ && grep -q 'unknown kB free' $Q/out
ok_ $? "df names no free space: left out"

# the policy of a first install is written twice: the old one kept beside the new (sepolicy.pre-hassmic)
echo old > $R/sepolicy; rnd $S/sepolicy 300000; pol=$(kb $S/sepolicy); old=$(kb $R/sepolicy)
fresh; run install $((all + pol + old + 1025)); [ $r = 0 ] && in_ && cmp -s $S/sepolicy $R/sepolicy && [ -f $R/sepolicy.pre-hassmic ]
ok_ $? "with a policy: room for it and the one kept beside it, the aligners go in"
fresh; echo old > $R/sepolicy; run install $((all + pol + 1025)); [ $r = 0 ] && in_
ok_ $? "installed again, the old policy kept already: it does not count twice"
fresh; echo old > $R/sepolicy; rm $R/sepolicy.pre-hassmic; run install $((all + pol + old + 1024)); [ $r = 0 ] && out_ && said
ok_ $? "a first install's policy, a kilobyte less: left out"
rm $S/sepolicy

# the home screen app goes in twice: beside hassmic and as an app
echo 'HOME_APK=home.apk' >> $S/device.conf; rnd $S/home.apk 500000; ak=$(kb $S/home.apk); all=$(kb $S/*)
fresh; run install $((all + ak + 1025)); [ $r = 0 ] && in_ && cmp -s $S/home.apk $R/system/app/HassmicHome/HassmicHome.apk
ok_ $? "with a home screen app: room for both of its copies, the aligners go in"
fresh; run install $((all + ak + 1024)); [ $r = 0 ] && out_ && said
ok_ $? "a kilobyte less: left out"
rm $S/home.apk; sed -i /HOME_APK/d $S/device.conf; all=$(kb $S/*)

# --- an update promoted to the factory copy, the aligners on the partition already
fresh; run install $((all + 1025)); in_; ok_ $? "(installed)"
rnd $S/hassmic 2000000; hk=$(kb $S/hassmic)
run factory $((hk + 1025)); [ $r = 0 ] && in_ && ! said && cmp -s $S/hassmic $H/hassmic && wrote 1
ok_ $? "aligners unchanged, room for the new hassmic and a megabyte: one file written, they stay"
rnd $S/hassmic 2000000
run factory 1; [ $r = 0 ] && in_ && ! said && cmp -s $S/hassmic $H/hassmic
ok_ $? "aligners unchanged, no room at all: they stay, nothing said (nothing of theirs to write)"
run factory ''; [ $r = 0 ] && in_ && ! said
ok_ $? "aligners unchanged, df names no free space: they stay"
rnd $S/german.was 1600000; was=$(kb $S/german.was)
run factory $((was + 1025)); [ $r = 0 ] && in_ && ! said && wrote 1
ok_ $? "a new .was with room for it: written"
rnd $S/german.was 1600000
run factory $((was + 1024)); [ $r = 0 ] && [ ! -e $H/german.was ] && cmp -s $S/word-de-DE.wa $H/word-de-DE.wa && said
ok_ $? "a new .was without room: the old one taken off (not read with words it was not made for), the same word file kept"
run factory $((was + 1025)); [ $r = 0 ] && in_; ok_ $? "(room again: written)"

# a write that fails: the partition as it was, the aligners there included
cp $H/german.was $Q/was.before; rnd $S/german.was 1600000; rnd $S/hassmic 2000000
FAKE_FAIL=hassmic.new run factory 999999; [ $r != 0 ] && cmp -s $Q/was.before $H/german.was && [ -e $H/word-de-DE.wa ] &&
    ! cmp -s $S/hassmic $H/hassmic && [ -z "$(find $R -name '*.new')" ]
ok_ $? "a write fails half way: nothing committed, the aligners there left alone, no .new files left"
run factory 999999; [ $r = 0 ] && in_; ok_ $? "(and then it goes in)"

echo stale > $H/word-de-DE-old.wa; run factory 999999; [ $r = 0 ] && [ ! -e $H/word-de-DE-old.wa ] && in_
ok_ $? "an aligner the update does not have (an older install's name): taken off"
rnd $S/sepolicy 300000; cp $R/sepolicy $Q/pol.before; rnd $S/hassmic 2000000
run factory $((hk + 1025)); [ $r = 0 ] && in_ && cmp -s $Q/pol.before $R/sepolicy
ok_ $? "a policy in an update: neither written nor counted"
rm $S/sepolicy

fresh; mv $S/word-de-DE.wa $S/german.was $Q/; run install 1; [ $r = 0 ] && ! said && cmp -s $S/hassmic $H/hassmic
ok_ $? "no aligners in it (an older build): nothing said about them"

# the installers stage them (nothing here runs those: a tripwire against the lists losing them again)
: > $Q/out
grep -q '^FILES="$FILES $(echo wakealign/\*.wa wakealign/\*.was)"$' scripts/install-system.sh
ok_ $? "install-system.sh: the aligners among its files"
grep -q '$(echo wakealign/\*.wa wakealign/\*.was)"$' scripts/install-boot.sh
ok_ $? "install-boot.sh: the same"

exit $fail
