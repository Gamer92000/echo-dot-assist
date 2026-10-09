#!/system/bin/sh
# Writes hassmic onto the system partition from the running OS, as root: no TWRP.  boot-root turned dm-verity off, so
# the partition only has to be remounted writable (otatool remount: toybox mount cannot, /dev/root does not exist).
#   sysinstall.sh install SRC      what scripts/install-system.sh puts there: SRC holds the files of /system/hassmic, plus
#                                  hassmic.rc and optionally sepolicy (the patched policy).  Files in /system/hassmic that
#                                  SRC lacks are removed (update.pub stays if SRC has none).
#   sysinstall.sh factory SRC      an update that passed its self test (SRC = its directory; main.sh, factory())
#                                  becomes the factory copy: its files, boot.sh and hassmic.rc.  Never the update key or
#                                  the policy: those change only through install-system.sh, with adb.  The release key
#                                  (online updates) does come along: it is part of every build.
#   sysinstall.sh uninstall SRC    what install-system.sh --uninstall does (SRC: where otatool is)
# The factory copy is what boot.sh falls back to, and boot.sh is what init starts: if it breaks, neither service starts,
# there is no egress lock, and Amazon's updaters can reach the internet.  So nothing is written unless every script
# parses and the model is right, each file goes in as a new file renamed over the old one (a power cut leaves the old or
# the new file, never half of one), boot.sh last, and the partition goes back to read-only whatever happens.
umask 022
MODE=$1 SRC=$2
H=/system/hassmic
RC=/system/etc/init/hassmic.rc
SYSLABEL=u:object_r:system_file:s0
say() { echo "sysinstall $MODE: $*"; }
case "$MODE" in install|factory|uninstall) ;; *) say "usage: sysinstall.sh install|factory|uninstall SRC"; exit 2;; esac
[ -x "$SRC/otatool" ] || { say "no $SRC/otatool"; exit 1; }
[ "$(id -u)" = 0 ] || { say "needs root"; exit 1; }

if [ "$MODE" != uninstall ]; then
    for f in boot.sh main.sh lockdown.sh device.conf hassmic runas otatool hassmic.rc; do
        [ -s "$SRC/$f" ] || { say "$f missing in $SRC, nothing written"; exit 1; }
    done
    for f in "$SRC"/*.sh; do sh -n "$f" || { say "${f##*/} does not parse, nothing written"; exit 1; }; done
    prod=$(. "$SRC/device.conf" 2>/dev/null && echo "$PRODUCT")
    [ "$prod" = "$(getprop ro.product.device)" ] ||
        { say "built for ${prod:-an unknown model}, this Echo is $(getprop ro.product.device); nothing written"; exit 1; }
    [ "$MODE" = factory ] || [ -s "$SRC/update.pub" ] || [ -s $H/update.pub ] || { say "no update.pub, nothing written"; exit 1; }
fi

# /system, not $H: otatool resolves the path it is given, and $H does not exist before the first install or after uninstall
"$SRC/otatool" remount rw /system > /dev/null || exit 1
trap '"$SRC/otatool" remount ro /system > /dev/null || say "partition still writable, the next boot mounts it read-only"' EXIT

# new DEST from FILE with MODE OWNER LABEL: written beside DEST as DEST.new; nothing if DEST already has that content.
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
        { say "cannot write $1.new"; rm -f "$1.new" "$tmp"; exit 1; }
    staged="$staged $1"
}
commit() {
    sync
    for f in $staged; do [ "$f" = $H/boot.sh ] || mv "$f.new" "$f"; done
    for f in $staged; do [ "$f" = $H/boot.sh ] && mv "$f.new" "$f"; done
    sync
}

if [ "$MODE" = uninstall ]; then
    if [ -f /sepolicy.pre-hassmic ]; then
        new /sepolicy /sepolicy.pre-hassmic 644 0:0 $SYSLABEL; commit; rm /sepolicy.pre-hassmic
    fi
    rm -rf $H $RC; sync
    say "hassmic removed from the system partition"
    exit 0
fi

mkdir -p $H; chown 0:2000 $H; chmod 755 $H; chcon $SYSLABEL $H
for f in "$SRC"/*; do
    n=${f##*/}
    case "$n" in
    hassmic.rc|sepolicy) continue;;
    update.pub) [ "$MODE" = install ] || continue; m=644;;
    device.conf|VERSION|*.ko|release.pub|wifictl.dex) m=644;;
    *) m=755;;
    esac
    new $H/$n "$f" $m 0:2000 $SYSLABEL
done
new $RC "$SRC/hassmic.rc" 644 0:0 $SYSLABEL
if [ "$MODE" = install ] && [ -f "$SRC/sepolicy" ]; then
    # The policy as boot-root left it stays beside it, for --uninstall; staged first, so commit() renames it into place
    # before the new policy.  Init loads /sepolicy before any policy is in force: its label does not matter.
    [ -f /sepolicy.pre-hassmic ] || new /sepolicy.pre-hassmic /sepolicy 644 0:0 $SYSLABEL
    new /sepolicy "$SRC/sepolicy" 644 0:0 $SYSLABEL
fi
commit
if [ "$MODE" = install ]; then
    for f in $H/*; do
        n=${f##*/}
        [ -e "$SRC/$n" ] || [ "$n" = update.pub ] || rm -f "$f"
    done
    sync
fi
say "$(echo $staged | wc -w) files written$( [ -f $H/VERSION ] && echo ", factory copy is $(cat $H/VERSION)")"
