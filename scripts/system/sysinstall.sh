#!/system/bin/sh
# Writes hassmic onto the system partition from the running OS, as root: no TWRP.  boot-root turned dm-verity off, so
# the partition only has to be remounted writable (otatool remount: toybox mount cannot, /dev/root does not exist).
#   sysinstall.sh install SRC      what scripts/install-system.sh puts there: SRC holds the files of /system/hassmic, plus
#                                  hassmic.rc and optionally sepolicy (the patched policy: /sepolicy, or device.conf's
#                                  SEPOLICY_FILE where the policy lives elsewhere, athena's /vendor/etc/selinux).  Files in /system/hassmic that
#                                  SRC lacks are removed (update.pub stays if SRC has none).
#   sysinstall.sh factory SRC      an update that passed its self test (SRC = its directory; main.sh, factory())
#                                  becomes the factory copy: its files, boot.sh and hassmic.rc.  Never the update key or
#                                  the policy: those change only through install-system.sh, with adb.  The release key
#                                  (online updates) does come along: it is part of every build.
#   sysinstall.sh leds SRC DIR     the light ring's animations that hassmic -G made in DIR (main.sh leds()): ours into
#                                  /system/etc/led-resources beside stock's, and stock's layer config with our names in
#                                  place of stock's.  ledcontroller reads every layer_config*.json there, each into a
#                                  compositor of its own, so ours cannot be a file of its own; stock's is kept one
#                                  directory up for -G and uninstall.  DIR/digest goes in last: the set is complete.
#   sysinstall.sh uninstall SRC    what install-system.sh --uninstall does (SRC: where otatool is)
# install and factory also take the stock apps of the model's STOCK_APPS_OUT (device.conf) off the partition, into
# /system/hassmic-removed (a rename on the same file system), and put back any that the list no longer names;
# uninstall puts every one back.  Where it names HOME_APK, that APK (in SRC) goes in as a system app,
# /system/app/HassmicHome/HassmicHome.apk: the home screen once stock's are off (tools/mkhome.py); uninstall removes it.
# The factory copy is what boot.sh falls back to, and boot.sh is what init starts: if it breaks, neither service starts,
# there is no egress lock, and Amazon's updaters can reach the internet.  So nothing is written unless every script
# parses and the model is right, each file goes in as a new file renamed over the old one (a power cut leaves the old or
# the new file, never half of one), boot.sh last, and the partition goes back to read-only whatever happens.
umask 022
MODE=$1 SRC=$2
H=/system/hassmic
RC=/system/etc/init/hassmic.rc
LR=/system/etc/led-resources
LSTOCK=/system/etc/layer_config_common.json.pre-hassmic
# Binary animations (the Echo Dot 5, ring.c "ACS"): ours are "<index>.hassmic_<name>.animation_bin", and the index files
# are rewritten with them; stock's are kept as /system/etc/<file>.pre-hassmic, as the layer config is
ACSIDX="acs_animation_list.txt acs_animation_count.txt acs_animation_name_list.txt acs_auto_stop_conf.txt layer_config_common.txt"
ours_in() { if [ -f "$1/acs_animation_list.txt" ] && ls "$1"/*.hassmic_*.animation_bin > /dev/null 2>&1; then ls "$1"/*.hassmic_*.animation_bin; else ls "$1"/hassmic_*.animation 2>/dev/null; fi; }
SYSLABEL=u:object_r:system_file:s0
say() { echo "sysinstall $MODE: $*"; }
case "$MODE" in install|factory|uninstall|leds) ;; *) say "usage: sysinstall.sh install|factory|uninstall|leds SRC [DIR]"; exit 2;; esac
[ -x "$SRC/otatool" ] || { say "no $SRC/otatool"; exit 1; }
[ "$(id -u)" = 0 ] || { say "needs root"; exit 1; }

if [ "$MODE" = leds ]; then
    L=$3
    [ -s "$L/digest" ] && [ -s "$L/layer_config_common.json" ] || { say "no animation set in $L, nothing written"; exit 1; }
    [ -f $LSTOCK ] || ! grep -q hassmic_ $LR/layer_config_common.json || { say "stock's layer config is gone, nothing written"; exit 1; }
    ACS=; [ -f "$L/acs_animation_list.txt" ] && ACS=1
    for f in $ACSIDX; do
        [ -z "$ACS" ] || [ -s "$L/$f" ] || { say "no $f in $L, nothing written"; exit 1; }
        [ -z "$ACS" ] || [ -f /system/etc/$f.pre-hassmic ] || ! grep -q hassmic_ $LR/$f || { say "stock's $f is gone, nothing written"; exit 1; }
    done
    # Designs from the settings page make the set as large as people draw them (ring.c caps them at 3 MB): room for it
    # on the partition, counting ours it replaces, and half a megabyte to spare
    need=$(du -sk "$L" | { read -r k _; echo "$k"; })
    have=$(df -k /system | { read -r _; read -r _ _ _ a _; echo "$a"; })
    mine=$(du -ck $(ours_in $LR) /dev/null 2>/dev/null | { t=0; while read -r k w; do [ "$w" = total ] && t=$k; done; echo $t; })
    case "$need$have$mine" in
    *[!0-9]*|'') say "cannot tell the set's size or the system partition's room, nothing written"; exit 1;;
    *) [ $need -lt $((have + mine - 512)) ] || { say "the system partition has room for $((have + mine - 512)) kB, the set is $need kB: nothing written"; exit 1; };;
    esac
elif [ "$MODE" != uninstall ]; then
    for f in boot.sh main.sh lockdown.sh device.conf hassmic runas otatool hassmic.rc; do
        [ -s "$SRC/$f" ] || { say "$f missing in $SRC, nothing written"; exit 1; }
    done
    for f in "$SRC"/*.sh; do sh -n "$f" || { say "${f##*/} does not parse, nothing written"; exit 1; }; done
    prod=$(. "$SRC/device.conf" 2>/dev/null && echo "$PRODUCT")
    [ "$prod" = "$(getprop ro.product.device)" ] ||
        { say "built for ${prod:-an unknown model}, this Echo is $(getprop ro.product.device); nothing written"; exit 1; }
    [ "$MODE" = factory ] || [ -s "$SRC/update.pub" ] || [ -s $H/update.pub ] || { say "no update.pub, nothing written"; exit 1; }
fi

# The SELinux policy: /sepolicy in the system partition's root (the Dots), or the file device.conf's SEPOLICY_FILE names
# (athena: /vendor/etc/selinux/precompiled_sepolicy, a partition of its own).  The installed device.conf serves where SRC
# has none (uninstall); tested before it is sourced: a missing file ends the subshell with ".", the fallback never ran.
conf_get() { c=$SRC/device.conf; [ -f "$c" ] || c=$H/device.conf; [ -f "$c" ] && (. "$c"; eval "echo \"\${$1}\""); }
# Cannoli's root is immutable SquashFS; only /system has a writable overlay.
# Its init imports /init.hassmic.rc before that overlay exists. A factory update
# must retain this boot contract, not install a second rc in the auto-scan path.
KEEP_INIT=
if [ "$(conf_get INSTALL)" = cannoli-ubi ]; then
    case "$MODE" in factory|leds) ;; *) say "Cannoli firmware changes require the TWRP ZIP"; exit 1;; esac
    [ "$MODE" = leds ] || cmp -s "$SRC/hassmic.rc" /init.hassmic.rc ||
        { say "Cannoli boot integration changed; install a matching TWRP ZIP first"; exit 1; }
    KEEP_INIT=1
fi
POLICY=$(conf_get SEPOLICY_FILE); POLICY=${POLICY:-/sepolicy}
PVOL=; case "$POLICY" in /vendor/*) PVOL=/vendor;; esac       # the partition the policy is on, when it is not /system's
# that partition writable only when the policy is written: a factory copy (every promoted update) or leds never touch it
case "$MODE" in install) [ -f "$SRC/sepolicy" ] || PVOL=;; uninstall) [ -f $POLICY.pre-hassmic ] || PVOL=;; *) PVOL=;; esac
PLABEL=$SYSLABEL; [ -z "$PVOL" ] || PLABEL=$(ls -dZ $POLICY 2>/dev/null | cut -d" " -f1)   # a policy in /vendor keeps its own label
[ -n "$PLABEL" ] || PLABEL=$SYSLABEL

# /system, not $H: otatool resolves the path it is given, and $H does not exist before the first install or after uninstall
"$SRC/otatool" remount rw /system > /dev/null || exit 1
[ -z "$PVOL" ] || "$SRC/otatool" remount rw $PVOL > /dev/null || { "$SRC/otatool" remount ro /system > /dev/null; exit 1; }
trap '"$SRC/otatool" remount ro /system > /dev/null || say "partition still writable, the next boot mounts it read-only"
      [ -z "$PVOL" ] || "$SRC/otatool" remount ro $PVOL > /dev/null || say "$PVOL still writable, the next boot mounts it read-only"' EXIT

# new DEST from FILE with MODE OWNER LABEL: written beside DEST as DEST.new; nothing if DEST already has that content.
# One that fails takes the .new files staged before it along (a whole aligner set is 2.5 MB) and ends the run.
# A file for / is made in /system and renamed to /: created in / it would get the label of /, rootfs, and SELinux
# refuses that on this ext4 partition (no "rootfs labeledfs:filesystem associate"; checked for the file's label, so the
# permissive su domain does not help; issue #2).  The rename keeps /system's label; relabelling to rootfs is refused the
# same way, so files in / end up system_file, as TWRP's install has left /sepolicy.pre-hassmic.
staged=
new() {
    cmp -s "$2" "$1" && [ "$(stat -c %a:%u:%g "$1")" = "$3:$4" ] && return 0
    tmp=$1.new; [ -n "${1%/*}" ] || tmp=/system$1.new
    rm -f "$1.new" "$tmp"
    cp "$2" "$tmp" && chown "$4" "$tmp" && chmod "$3" "$tmp" && chcon "$5" "$tmp" &&
        { [ "$tmp" = "$1.new" ] || mv "$tmp" "$1.new"; } ||
        { say "cannot write $1.new"; rm -f "$1.new" "$tmp"; for s in $staged; do rm -f "$s.new"; done; exit 1; }
    staged="$staged $1"
}
commit() {
    sync
    for f in $staged; do [ "$f" = $H/boot.sh ] || mv "$f.new" "$f"; done
    for f in $staged; do [ "$f" = $H/boot.sh ] && mv "$f.new" "$f"; done
    sync
}

if [ "$MODE" = leds ]; then
    [ -f $LSTOCK ] || new $LSTOCK $LR/layer_config_common.json 644 0:0 $SYSLABEL       # stock's, before ours replaces it
    for f in $ACSIDX; do [ -z "$ACS" ] || [ -f /system/etc/$f.pre-hassmic ] || new /system/etc/$f.pre-hassmic $LR/$f 644 0:0 $SYSLABEL; done
    # Ours are files of their own, ~2400 of them (most are the LEDs' own, issue #57): in bulk, as one at a time takes
    # minutes, and over the old ones.  The digest goes first: a copy cut half way leaves old and new files mixed, and without a digest the next start
    # writes the whole set again (with the old one it would take the set for installed)
    rm -f /system/etc/hassmic-leds.digest; sync
    new_files=$(ours_in "$L")
    [ -n "$new_files" ] && cp $new_files $LR/ && (cd $LR && for f in $new_files; do chmod 644 "${f##*/}" && chcon $SYSLABEL "${f##*/}" || exit 1; done) ||
        { say "cannot write the animations"; exit 1; }
    new $LR/layer_config_common.json "$L/layer_config_common.json" 644 0:0 $SYSLABEL
    for f in $ACSIDX; do [ -z "$ACS" ] || new $LR/$f "$L/$f" 644 0:0 $SYSLABEL; done
    commit
    # files of an earlier set only once the config naming them is gone: a cut before this leaves a config whose files exist
    for f in $(ours_in $LR); do [ -e "$L/${f##*/}" ] || rm -f "$f"; done
    staged=; new /system/etc/hassmic-leds.digest "$L/digest" 644 0:0 $SYSLABEL; commit
    say "light ring: $(ours_in $LR | wc -l) animations of ours"
    exit 0
fi

# Stock apps off the partition and back (STOCK_APPS_OUT): PackageManager scans /system/app, priv-app and the like at
# boot, not REMOVED, so a package moved there is gone from the next boot on, whatever its enabled state said.
REMOVED=/system/hassmic-removed
HOMEDIR=/system/app/HassmicHome
stock_apps() {      # stock_apps "LIST": those in REMOVED, the rest back where they were
    set -- " $(echo $1) "                   # one line: device.conf spreads a long list over several
    for d in $REMOVED/*/*; do
        [ -e "$d" ] || continue
        p=${d#$REMOVED/}
        case "$1" in *" $p "*) continue;; esac
        if [ -e "/system/$p" ]; then say "$p is back on the partition already, kept in $REMOVED"
        else mv "$d" "/system/$p" && say "$p put back"; fi
    done
    for p in $1; do
        case "$p" in /*|*..*|*/*/*|"") say "not a folder of /system/<dir>/: $p"; continue;; esac
        [ -e "/system/$p" ] || continue
        if [ -e "$REMOVED/$p" ]; then say "$p is in $REMOVED already, the partition's left alone"; continue; fi
        mkdir -p "$REMOVED/${p%/*}" && chcon $SYSLABEL $REMOVED "$REMOVED/${p%/*}" && mv "/system/$p" "$REMOVED/$p" &&
            say "$p taken off the partition (in $REMOVED; gone from the next boot on)" || say "cannot move $p"
    done
    rmdir $REMOVED/* $REMOVED 2>/dev/null
    sync
}

if [ "$MODE" = uninstall ]; then
    # a call hassmic left in Android's call mode (audio_android.c's mark): MODE_NORMAL before stock takes over again
    if [ -f /data/local/hassmic/state/call-mode ] && command -v service > /dev/null; then
        [ "$(getprop ro.build.version.sdk)" -ge 28 ] 2>/dev/null && tx=4 || tx=3
        service call media.audio_policy $tx i32 0 > /dev/null 2>&1; rm -f /data/local/hassmic/state/call-mode
    fi
    stock_apps ""
    if [ -f $POLICY.pre-hassmic ]; then
        new $POLICY $POLICY.pre-hassmic 644 0:0 $PLABEL; commit; rm $POLICY.pre-hassmic
    fi
    if [ -f $LSTOCK ]; then
        staged=; new $LR/layer_config_common.json $LSTOCK 644 0:0 $SYSLABEL; commit; rm $LSTOCK
    fi
    for f in $ACSIDX; do
        [ -f /system/etc/$f.pre-hassmic ] && { staged=; new $LR/$f /system/etc/$f.pre-hassmic 644 0:0 $SYSLABEL; commit; rm /system/etc/$f.pre-hassmic; }
    done
    rm -f $LR/hassmic_*.animation $LR/*.hassmic_*.animation_bin /system/etc/hassmic-leds.digest
    rm -rf $H $RC $HOMEDIR; sync
    say "hassmic removed from the system partition"
    exit 0
fi

mkdir -p $H; chown 0:2000 $H; chmod 755 $H; chcon $SYSLABEL $H
# The wake word aligners (*.wa a word, *.was a language: 2.5 MB; hassmic reads them next to itself) are written only
# where the partition has room for them beside everything else this run writes, and a megabyte to spare: without them
# hassmic finds the word's end by the level.  Counted as new() writes: a file the same as its destination takes nothing,
# any other its whole size, beside the old one until commit().  Held against the aligners alone, the room could take
# them and then fail on a first install's other files; and an update counted the aligners it kept against the room they
# took already, so a nearly full partition lost them and got them back the time after.  (new() also rewrites a file of
# the same content whose mode or owner differ, counted here as nothing: only after someone changed it by hand.)
home=$(. "$SRC/device.conf" 2>/dev/null && echo "$HOME_APK")
kb=0
cost() {        # cost FILE DEST: kb + what FILE takes on /system written as DEST
    [ -f "$1" ] || return 0
    [ -f "$2" ] && cmp -s "$1" "$2" && return 0
    k=$(du -k "$1"); k=${k%%[!0-9]*}; kb=$((kb + ${k:-0}))
}
wa=
for f in "$SRC"/*; do
    n=${f##*/}
    case "$n" in
    sepolicy) [ "$MODE" = install ] && [ -z "$PVOL" ] || continue      # athena's goes onto /vendor
              [ -f $POLICY.pre-hassmic ] || cost $POLICY $POLICY.pre-hassmic
              cost "$f" $POLICY;;
    hassmic.rc) [ -n "$KEEP_INIT" ] || cost "$f" $RC;;
    update.pub) [ "$MODE" = install ] && cost "$f" $H/$n;;
    *.wa|*.was) k0=$kb; cost "$f" $H/$n; [ $kb = $k0 ] || wa=1;;    # wa: one of them to write
    *) cost "$f" $H/$n;;
    esac
done
[ -z "$home" ] || cost "$SRC/$home" $HOMEDIR/HassmicHome.apk
room=$(df -k /system | { read -r _; read -r _ _ _ a _; echo "$a"; })
case "$room" in *[!0-9]*|'') room= wa_fit=0;; *) [ $kb -lt $((room - 1024)) ] && wa_fit=1 || wa_fit=0;; esac
[ -z "$wa" ] || [ $wa_fit = 1 ] ||
    say "the system partition has no room for the wake word aligners: $kb kB to write with them, ${room:-unknown} kB free; left out"
for f in "$SRC"/*; do
    n=${f##*/}
    [ -d "$f" ] && continue                 # oat/: what ART leaves beside a dex it loaded (ringbar.dex, wifictl.dex)
    case "$n" in
    hassmic.rc|sepolicy) continue;;
    update.pub) [ "$MODE" = install ] || continue; m=644;;
    *.wa|*.was) [ $wa_fit = 1 ] || continue; m=644;;
    device.conf|VERSION|*.ko|release.pub|*.dex|*.apk|*.so) m=644;;
    *) m=755;;
    esac
    new $H/$n "$f" $m 0:2000 $SYSLABEL
done
[ -n "$KEEP_INIT" ] || new $RC "$SRC/hassmic.rc" 644 0:0 $SYSLABEL
if [ -n "$home" ] && [ -s "$SRC/$home" ]; then
    mkdir -p $HOMEDIR; chown 0:0 $HOMEDIR; chmod 755 $HOMEDIR; chcon $SYSLABEL $HOMEDIR
    new $HOMEDIR/HassmicHome.apk "$SRC/$home" 644 0:0 $SYSLABEL
fi
if [ "$MODE" = install ] && [ -f "$SRC/sepolicy" ]; then
    # The policy as boot-root left it stays beside it, for --uninstall; staged first, so commit() renames it into place
    # before the new policy.  Init loads it before any policy is in force: its label does not matter.
    [ -f $POLICY.pre-hassmic ] || new $POLICY.pre-hassmic $POLICY 644 0:0 $PLABEL
    new $POLICY "$SRC/sepolicy" 644 0:0 $PLABEL
fi
# Where the factory copy has nothing of its own to fall back to (device.conf ROLLBACK: cannoli's is in the overlay on
# /data, and every promotion overwrites it), the copy being replaced is kept for the boot's starter (prepare.sh) to put
# back: only one that was reached from the network since it was written (main.sh reach_check wrote its version into
# ROLLBACK/reached), so a copy that never was cannot push out one that was.  Taken before commit(): the .new files
# staged beside the old ones are left out; written and synced as previous.new, then the old one to previous.old and the
# new one into place (a power cut between those two renames: prepare.sh takes previous.new).
RB=$(conf_get ROLLBACK)
if [ "$MODE" = factory ] && [ -n "$RB" ] && [ -d "$RB" ] && [ ! -L "$RB" ]; then
    v=$(cat $H/VERSION 2>/dev/null); kept=$(cat "$RB/previous/VERSION" 2>/dev/null)
    if [ -n "$v" ] && grep -qxF "$v" "$RB/reached" 2>/dev/null; then
        rm -rf "$RB/previous.new" "$RB/previous.old"
        if cp -a $H "$RB/previous.new" && rm -f "$RB/previous.new"/*.new && sync &&
           { [ ! -d "$RB/previous" ] || mv "$RB/previous" "$RB/previous.old"; } && mv "$RB/previous.new" "$RB/previous"; then
            rm -rf "$RB/previous.old"; say "$v kept to roll back to"
        else
            rm -rf "$RB/previous.new"; [ -d "$RB/previous" ] || mv "$RB/previous.old" "$RB/previous" 2>/dev/null
            say "!! cannot keep $v to roll back to: ${kept:-nothing} stays"
        fi
    else
        say "${v:-the factory copy} was never reached from the network: not kept to roll back to, ${kept:-nothing} stays"
    fi
    sync
fi
commit
# Aligners on the partition that are not the run's own go once it is in place (a write that failed left them as they
# were): an older install's other names (a file per word and locale, 6.6 MB, before the .was files), and older
# versions there was no room to replace.  The run's own stay, written now or the same already, also where the rest
# left no room for new ones: a word file whose .was is gone or another is refused by its checks, never read wrong
if [ "$SRC" != $H ]; then
    for f in $H/*.wa $H/*.was; do
        [ -e "$f" ] && ! cmp -s "$f" "$SRC/${f##*/}" && rm -f "$f"
    done
fi
stock_apps "$(. "$SRC/device.conf" 2>/dev/null && echo "$STOCK_APPS_OUT")"
if [ "$MODE" = install ]; then
    for f in $H/*; do
        n=${f##*/}
        [ -e "$SRC/$n" ] || [ "$n" = update.pub ] || rm -f "$f"
    done
    sync
fi
say "$(echo $staged | wc -w) files written$( [ -f $H/VERSION ] && echo ", factory copy is $(cat $H/VERSION)")"
