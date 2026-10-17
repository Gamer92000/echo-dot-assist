#!/bin/sh
# Phase 5: install hassmic into the active system slot, so it starts at boot instead of Alexa.
# This is the installer for models with INSTALL=twrp-ab in devices/<codename>/device.conf (donut); the model is the one
# on adb, and it must run the firmware its support was built for.
# With the Echo's OS up, the running system partition is written directly (scripts/system/sysinstall.sh: remounted
# writable, which boot-root's disabled dm-verity allows).  TWRP only with --twrp or when the Echo is in recovery already,
# e.g. when an installed Echo no longer boots.
# Needs: boot-root already flashed (permissive su domain), device reachable over adb.
# Touches: /system/hassmic/ (new), /system/etc/init/hassmic.rc (new), /sepolicy (allow rules added; old copy kept as
# /sepolicy.pre-hassmic), /data/local/hassmic/hassmic.conf (new).
# Later versions go over Wi-Fi with scripts/ota-push.sh (each also renews the factory copy and the bootstrap once it passed its self test); this
# script is only needed once per device, and again if secrets/update.key is lost or the SELinux policy has to change.
# Undo: scripts/install-system.sh --uninstall, or delete /data/local/hassmic/hassmic.conf (boot.sh then does nothing).
#   install-system.sh [--twrp]               the Echo names itself (model and MAC address); name it in Home Assistant
#   install-system.sh [--twrp] --uninstall
set -e
cd "$(dirname "$0")/.."
. scripts/lib/device.sh; device_load adb
. scripts/lib/build.sh
[ "$INSTALL" = twrp-ab ] || die "$DEVICE installs with INSTALL=$INSTALL, which this script does not do ($DDIR/README.md)"
MNT=/mnt/hm_system
STAGE=/data/local/tmp/hm-install
t() { adb shell "$@"; }
TWRP=; [ "$1" = --twrp ] && { TWRP=1; shift; }
[ "$(adb get-state 2>/dev/null)" = device ] || TWRP=1

if [ "$1" != --uninstall ]; then
    # before anything is touched: without it the policy patch below fails with no word of why (issue #10)
    [ -f $SEPOLICY_TOOL ] || die "no $SEPOLICY_TOOL: unzip $FW/boot-root.zip -d $FW/boot-root ($DDIR/README.md)"
    [ "$(adb get-state 2>/dev/null)" = device ] && device_check_firmware
    build_binaries                      # built here or this commit's release build (PREBUILT, scripts/lib/build.sh)
    # Signing key for push updates.  The public half goes onto the read-only system partition and is what the device trusts.
    mkdir -p secrets
    [ -f secrets/update.key ] || { python3 scripts/otatool.py keygen secrets/update.key secrets/update.pub; echo "new update signing key: secrets/update.key (keep it, back it up)"; }
    # Backup of the policy as boot-root left it, and the config boot.sh reads.  Both need the running OS.
    if [ "$(adb get-state 2>/dev/null)" = device ]; then
        mkdir -p device-logs/backup
        # Base = the policy as boot-root left it.  On a re-install /sepolicy is already patched; the untouched copy is kept beside it.
        BASEF=/sepolicy; t "[ -f /sepolicy.pre-hassmic ]" && BASEF=/sepolicy.pre-hassmic
        adb pull $BASEF device-logs/backup/sepolicy.boot-root >/dev/null
        printf 'ARGS="%s"\n' "$DEFAULT_ARGS" > $OUT/hassmic.conf
        t "mkdir -p /data/local/hassmic"; adb push $OUT/hassmic.conf /data/local/hassmic/hassmic.conf >/dev/null
        t "rm -rf /data/local/hassmic/ota; rm -f /data/local/hassmic/hassmic"      # boot.sh prefers a binary here (scripts/deploy.sh test builds); the fresh install wins
        # Patch the policy here, not in TWRP: magiskpolicy is dynamically linked and aborts in the recovery environment.
        adb push $SEPOLICY_TOOL /data/local/tmp/mp >/dev/null && adb push $DDIR/sepolicy.rules /data/local/tmp/ >/dev/null ||
            die "could not copy $SEPOLICY_TOOL and $DDIR/sepolicy.rules to the Echo, nothing changed"
        t "cd /data/local/tmp; chmod 755 mp; rm -f sepolicy.new
           ./mp --load $BASEF --apply sepolicy.rules --save sepolicy.new >/dev/null 2>&1
           [ -s sepolicy.new ] && ./mp --load sepolicy.new --print-rules 2>/dev/null | grep -q '^allow init su process'" ||
            { echo "sepolicy patch failed, nothing changed"; exit 1; }
        adb pull /data/local/tmp/sepolicy.new $OUT/sepolicy.hassmic >/dev/null
        md5sum device-logs/backup/sepolicy.boot-root > $OUT/sepolicy.hassmic.base
    fi
    [ -s $OUT/sepolicy.hassmic ] || { echo "no patched policy yet: boot the normal OS (adb reboot) and run this again"; exit 1; }
    BASE=$(cut -d' ' -f1 $OUT/sepolicy.hassmic.base)
    WANT=$(md5sum $OUT/sepolicy.hassmic | cut -d' ' -f1)
fi
# What goes into /system/hassmic, whichever way it gets there.  The tools and the Wi-Fi motion module are optional (no
# kernel toolchain, no .ko): `|| true`, since an assignment takes the substitution's status and set -e would end here.
FILES="$OUT/hassmic $OUT/runas $(ls $OUT/mixcap $OUT/mixplay $OUT/pryon_test $OUT/*.ko 2>/dev/null || true) $OUT/otatool secrets/update.pub keys/release.pub $DDIR/device.conf
       scripts/system/boot.sh scripts/system/main.sh scripts/system/sysinstall.sh scripts/device/lockdown.sh scripts/device/alexa-off.sh scripts/device/alexa-on.sh scripts/device/artifact-install.sh scripts/device/wifi.sh"

if [ -z "$TWRP" ]; then
    t "rm -rf $STAGE; mkdir -p $STAGE"
    if [ "$1" = --uninstall ]; then
        adb push $OUT/otatool scripts/system/sysinstall.sh $STAGE/ >/dev/null
        MODE=uninstall
    else
        adb push $FILES $DDIR/hassmic.rc $STAGE/ >/dev/null
        # Policy: the copy patched and verified above, unless the running one is that already.  Refuse if the policy
        # it was made from is not the one on the partition.
        if [ "$(t "md5sum /sepolicy" | cut -d' ' -f1)" = "$WANT" ]; then
            echo "sepolicy already patched"
        else
            BASEF=/sepolicy; t "[ -f /sepolicy.pre-hassmic ]" && BASEF=/sepolicy.pre-hassmic
            [ "$(t "md5sum $BASEF" | cut -d' ' -f1)" = "$BASE" ] || { echo "policy on the Echo differs from the one that was patched; nothing written"; exit 1; }
            adb push $OUT/sepolicy.hassmic $STAGE/sepolicy >/dev/null
            [ "$(t "md5sum $STAGE/sepolicy" | cut -d' ' -f1)" = "$WANT" ] || { echo "policy transfer corrupted; nothing written"; exit 1; }
        fi
        MODE=install
    fi
    t "chmod 755 $STAGE/otatool; sh $STAGE/sysinstall.sh $MODE $STAGE && touch $STAGE/ok"
    t "[ -f $STAGE/ok ]" || { t "rm -rf $STAGE"; echo "system partition not written (see above); nothing to undo"; exit 1; }
    t "rm -rf $STAGE; ls -lZ /system/hassmic /system/etc/init/hassmic.rc /sepolicy 2>/dev/null"
    [ "$MODE" = uninstall ] && echo "note: pushed updates in /data/local/hassmic/ota are no longer started; remove them with adb once the OS is up"
    echo "$MODE done on the running $DEVICE; rebooting"
    adb reboot; exit 0
fi

echo "rebooting to TWRP ..."
[ "$(adb get-state 2>/dev/null)" = recovery ] || { adb reboot recovery; sleep 15; }
adb wait-for-recovery; sleep 5

SLOT=$(t 'bcbtool get_active' | tr -d '\r\n ')
case "$SLOT" in a|b) ;; *) echo "unexpected slot '$SLOT'"; exit 1;; esac
t "mkdir -p $MNT; umount $MNT 2>/dev/null; mount -o rw \$(ls /dev/block/platform/*/by-name/system_$SLOT | head -1) $MNT"
t "[ -f $MNT/sepolicy ] && [ -d $MNT/system/etc/init ]" || { echo "system_$SLOT does not look right"; exit 1; }
echo "system_$SLOT mounted"

if [ "$1" = --uninstall ]; then
    t "[ -f $MNT/sepolicy.pre-hassmic ] && cat $MNT/sepolicy.pre-hassmic > $MNT/sepolicy && rm $MNT/sepolicy.pre-hassmic
       rm -rf $MNT/system/hassmic $MNT/system/etc/init/hassmic.rc; sync; umount $MNT"
    echo "note: pushed updates in /data/local/hassmic/ota are no longer started; remove them with adb once the OS is up"
    echo "removed; rebooting"; adb reboot; exit 0
fi

# Policy: the copy patched and verified under the running OS.  Refuse if the partition's policy is not the one it was made from.
if [ "$(t "md5sum $MNT/sepolicy" | cut -d' ' -f1)" = "$WANT" ]; then
    echo "sepolicy already patched"
else
    t "[ -f $MNT/sepolicy.pre-hassmic ] || cp -p $MNT/sepolicy $MNT/sepolicy.pre-hassmic"
    [ "$(t "md5sum $MNT/sepolicy.pre-hassmic" | cut -d' ' -f1)" = "$BASE" ] ||
        { echo "policy on system_$SLOT differs from the one that was patched; nothing written"; t "umount $MNT"; exit 1; }
    adb push $OUT/sepolicy.hassmic /tmp/sepolicy.new >/dev/null
    [ "$(t 'md5sum /tmp/sepolicy.new' | cut -d' ' -f1)" = "$WANT" ] ||
        { echo "policy transfer corrupted; nothing written"; t "umount $MNT"; exit 1; }
    t "cat /tmp/sepolicy.new > $MNT/sepolicy"          # in place: keeps inode and label
fi
echo "sepolicy patched"

t "rm -rf $MNT/system/hassmic; mkdir -p $MNT/system/hassmic"
adb push $FILES $MNT/system/hassmic/ >/dev/null
adb push $DDIR/hassmic.rc $MNT/system/etc/init/hassmic.rc >/dev/null
t "chown -R 0:2000 $MNT/system/hassmic; chmod 755 $MNT/system/hassmic $MNT/system/hassmic/*; chmod 644 $MNT/system/hassmic/update.pub $MNT/system/hassmic/release.pub $MNT/system/hassmic/device.conf
   chown 0:0 $MNT/system/etc/init/hassmic.rc; chmod 644 $MNT/system/etc/init/hassmic.rc
   chcon -R u:object_r:system_file:s0 $MNT/system/hassmic $MNT/system/etc/init/hassmic.rc $MNT/sepolicy.pre-hassmic
   ls -lZ $MNT/system/hassmic $MNT/system/etc/init/hassmic.rc $MNT/sepolicy $MNT/system/etc/init/fosflags.rc
   sync; umount $MNT"
echo "installed $DEVICE support into system_$SLOT; rebooting"
adb reboot
