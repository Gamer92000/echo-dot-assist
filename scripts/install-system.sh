#!/bin/sh
# Phase 5: install hassmic into the active system slot through TWRP, so it starts at boot instead of Alexa.
# Needs: boot-root already flashed (permissive su domain), device reachable over adb.
# Touches: /system/hassmic/ (new), /system/etc/init/hassmic.rc (new), /sepolicy (3 allow rules added; old copy kept as
# /sepolicy.pre-hassmic), /data/local/hassmic/hassmic.conf (new).
# Undo: scripts/install-system.sh --uninstall, or delete /data/local/hassmic/hassmic.conf (boot.sh then does nothing).
#   install-system.sh <lan-cidr> [name]      e.g. install-system.sh 192.168.100.0/22 "Echo Dot"
#   install-system.sh --uninstall
set -e
cd "$(dirname "$0")/.."
MNT=/mnt/hm_system
t() { adb shell "$@"; }

if [ "$1" != --uninstall ]; then
    LAN=${1:?usage: install-system.sh <lan-cidr> [name] | --uninstall}; NAME=${2:-Echo Dot}
    make -s all
    # Backup of the policy as boot-root left it, and the config boot.sh reads.  Both need the running OS.
    if [ "$(adb get-state 2>/dev/null)" = device ]; then
        mkdir -p device-logs/backup
        # Base = the policy as boot-root left it.  On a re-install /sepolicy is already patched; the untouched copy is kept beside it.
        BASEF=/sepolicy; t "[ -f /sepolicy.pre-hassmic ]" && BASEF=/sepolicy.pre-hassmic
        adb pull $BASEF device-logs/backup/sepolicy.boot-root >/dev/null
        printf 'LAN=%s\nNAME="%s"\nARGS=""\n' "$LAN" "$NAME" > build/hassmic.conf
        t "mkdir -p /data/local/hassmic"; adb push build/hassmic.conf /data/local/hassmic/hassmic.conf >/dev/null
        t "rm -f /data/local/hassmic/hassmic"      # boot.sh prefers a binary here (scripts/deploy.sh test builds); the fresh install wins
        # Patch the policy here, not in TWRP: magiskpolicy is dynamically linked and aborts in the recovery environment.
        adb push boot-root/patch/magiskpolicy32 /data/local/tmp/mp >/dev/null
        adb push scripts/system/sepolicy.rules /data/local/tmp/ >/dev/null
        t "cd /data/local/tmp; chmod 755 mp; rm -f sepolicy.new
           ./mp --load $BASEF --apply sepolicy.rules --save sepolicy.new >/dev/null 2>&1
           [ -s sepolicy.new ] && ./mp --load sepolicy.new --print-rules 2>/dev/null | grep -q '^allow init su process'" ||
            { echo "sepolicy patch failed, nothing changed"; exit 1; }
        adb pull /data/local/tmp/sepolicy.new build/sepolicy.hassmic >/dev/null
        md5sum device-logs/backup/sepolicy.boot-root > build/sepolicy.hassmic.base
    fi
    [ -s build/sepolicy.hassmic ] || { echo "no patched policy yet: boot the normal OS (adb reboot) and run this again"; exit 1; }
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
    echo "removed; rebooting"; adb reboot; exit 0
fi

# Policy: the copy patched and verified under the running OS.  Refuse if the partition's policy is not the one it was made from.
BASE=$(cut -d' ' -f1 build/sepolicy.hassmic.base)
WANT=$(md5sum build/sepolicy.hassmic | cut -d' ' -f1)
if [ "$(t "md5sum $MNT/sepolicy" | cut -d' ' -f1)" = "$WANT" ]; then
    echo "sepolicy already patched"
else
    t "[ -f $MNT/sepolicy.pre-hassmic ] || cp -p $MNT/sepolicy $MNT/sepolicy.pre-hassmic"
    [ "$(t "md5sum $MNT/sepolicy.pre-hassmic" | cut -d' ' -f1)" = "$BASE" ] ||
        { echo "policy on system_$SLOT differs from the one that was patched; nothing written"; t "umount $MNT"; exit 1; }
    adb push build/sepolicy.hassmic /tmp/sepolicy.new >/dev/null
    [ "$(t 'md5sum /tmp/sepolicy.new' | cut -d' ' -f1)" = "$WANT" ] ||
        { echo "policy transfer corrupted; nothing written"; t "umount $MNT"; exit 1; }
    t "cat /tmp/sepolicy.new > $MNT/sepolicy"          # in place: keeps inode and label
fi
echo "sepolicy patched"

t "rm -rf $MNT/system/hassmic; mkdir -p $MNT/system/hassmic"
adb push build/hassmic build/runas build/mixcap build/mixplay build/pryon_test \
         scripts/system/boot.sh scripts/device/lockdown.sh scripts/device/alexa-off.sh scripts/device/alexa-on.sh \
         scripts/device/hassmic.service $MNT/system/hassmic/ >/dev/null
adb push scripts/system/hassmic.rc $MNT/system/etc/init/hassmic.rc >/dev/null
t "chown -R 0:2000 $MNT/system/hassmic; chmod 755 $MNT/system/hassmic $MNT/system/hassmic/*; chmod 644 $MNT/system/hassmic/hassmic.service
   chown 0:0 $MNT/system/etc/init/hassmic.rc; chmod 644 $MNT/system/etc/init/hassmic.rc
   chcon -R u:object_r:system_file:s0 $MNT/system/hassmic $MNT/system/etc/init/hassmic.rc $MNT/sepolicy.pre-hassmic
   ls -lZ $MNT/system/hassmic $MNT/system/etc/init/hassmic.rc $MNT/sepolicy $MNT/system/etc/init/fosflags.rc
   sync; umount $MNT"
echo "installed into system_$SLOT; rebooting"
adb reboot
