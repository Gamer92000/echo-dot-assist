#!/bin/sh
# Install hassmic on a model with INSTALL=boot in devices/<codename>/device.conf (checkers, Echo Show 5 1st gen): no A/B
# slots, /system under dm-verity, the SELinux policy inside boot.img's ramdisk.  scripts/install-system.sh hands over to
# this one.  Two parts:
#   1. boot: the firmware's own boot.img (from FIRMWARE_FILE, checked against BOOT_SHA256) with root adb (and ADBD: the
#      stock adbd never stays root), without "verify" in its fstab, and with its policy patched by sepolicy.rules
#      (magiskpolicy under qemu-arm: the Echo has no root yet), built by scripts/mkbootroot.py and flashed in amonet's
#      hacked fastboot.  Skipped when the boot partition holds that image already.
#   2. system: /system/hassmic/ and /system/etc/init/hassmic.rc from the running OS (scripts/system/sysinstall.sh), and
#      /data/local/hassmic/hassmic.conf, as on donut.
# Writes the boot partition and the system partition, nothing else: never lk, preloader, tee or anything of the
# bootloader (amonet's README: a brick there is permanent on most units).
# Needs: amonet-checkers done (TWRP and hacked fastboot), the Echo on USB in TWRP or booted; the firmware .bin, unpacked to
# firmware/<codename>/rootfs (qemu runs magiskpolicy against its linker); SEPOLICY_TOOL; qemu-arm, fastboot, adb.
# Once /system has been written only a boot image without verify may boot it: the stock boot.img would find its hashes
# wrong.  Back to stock means flashing the firmware .bin in TWRP (system and boot together).
#   install-boot.sh [name]          default DEFAULT_NAME of the model
#   install-boot.sh --boot-only     only part 1: root adb, e.g. for scripts/deploy.sh trial runs before installing
#   install-boot.sh --uninstall     Alexa's apps back on, hassmic off the system partition (the root boot image stays)
set -e
cd "$(dirname "$0")/.."
. scripts/lib/device.sh; device_load adb
. scripts/lib/build.sh
[ "$INSTALL" = boot ] || die "$DEVICE installs with INSTALL=$INSTALL, not with this script ($DDIR/README.md)"
STAGE=/data/local/tmp/hm-install
IMG=$OUT/boot-hassmic.img
t() { adb shell "$@" | tr -d '\r'; }
state() { adb get-state 2>/dev/null || true; }
sha() { sha256sum "$1" | cut -c1-64; }

# --- part 1, on the PC: the boot image
boot_image() {
    mkdir -p $OUT
    [ -f $FW/$FIRMWARE_FILE ] || die "no $FW/$FIRMWARE_FILE ($DDIR/README.md)"
    unzip -p $FW/$FIRMWARE_FILE boot.img > $OUT/boot.stock.img
    [ "$(sha $OUT/boot.stock.img)" = "$BOOT_SHA256" ] || die "boot.img in $FIRMWARE_FILE is not the one this was built for"
    [ -f $SEPOLICY_TOOL ] && [ "$(sha $SEPOLICY_TOOL)" = "$SEPOLICY_TOOL_SHA256" ] ||
        die "no $SEPOLICY_TOOL (or not the pinned one): patch/magiskpolicy32 of donut's boot-root.zip ($DDIR/README.md)"
    [ -f $ADBD ] && [ "$(sha $ADBD)" = "$ADBD_SHA256" ] ||
        die "no $ADBD (or not the pinned one): sbin/adbd of boot-root.img in checkers' own boot-root.zip ($DDIR/README.md)"
    [ -e $FW/rootfs/system/bin/linker ] || die "$FW/rootfs not unpacked: qemu runs magiskpolicy against its linker ($DDIR/README.md)"
    { command -v qemu-arm > /dev/null || [ -x toolchain/qemu-arm ]; } || die "no qemu-arm (qemu user mode; devices/checkers/setup.sh puts one in toolchain/)"
    python3 scripts/mkbootroot.py --extract $OUT/boot.stock.img sepolicy $OUT/sepolicy.stock
    rm -f $OUT/sepolicy.hassmic
    chmod 755 $SEPOLICY_TOOL
    DEVICE=$DEVICE tools/qrun.sh -t 120 $SEPOLICY_TOOL --load $OUT/sepolicy.stock --apply $DDIR/sepolicy.rules \
        --save $OUT/sepolicy.hassmic > /dev/null 2>&1 || true
    [ -s $OUT/sepolicy.hassmic ] || die "the policy patch failed (tools/qrun.sh $SEPOLICY_TOOL)"
    DEVICE=$DEVICE tools/qrun.sh -t 120 $SEPOLICY_TOOL --load $OUT/sepolicy.hassmic --print-rules > $OUT/sepolicy.rules.txt 2>/dev/null || true
    grep -qx 'permissive adbd' $OUT/sepolicy.rules.txt && grep -q '^allow init su process' $OUT/sepolicy.rules.txt ||
        die "the patched policy lacks its rules ($OUT/sepolicy.rules.txt)"
    python3 scripts/mkbootroot.py $OUT/boot.stock.img $IMG --sepolicy $OUT/sepolicy.hassmic --adbd $ADBD --no-verity > /dev/null
    echo "boot image: $IMG ($(sha $IMG | cut -c1-16)...)"
}

# ro.build.version.incremental of the Echo's system partition: the boot image must be of the same build
system_build() {
    case "$(state)" in
    device) t getprop ro.build.version.incremental;;
    recovery) t "mkdir -p /mnt/hm_sys; mount -o ro $SYSTEM_DEV /mnt/hm_sys 2>/dev/null
                 sed -n 's/^ro.build.version.incremental=//p' /mnt/hm_sys/build.prop; umount /mnt/hm_sys 2>/dev/null";;
    esac
}

# the boot partition holds IMG already (read as root: our root adb or TWRP; the image's toybox has md5sum, no sha256sum)
boot_is_ours() {
    [ "$(state)" = device ] || [ "$(state)" = recovery ] || return 1
    [ "$(t "head -c $(stat -c %s $IMG) $BOOT_DEV 2>/dev/null | md5sum" | cut -c1-32)" = "$(md5sum $IMG | cut -c1-32)" ]
}

wait_for() {   # wait_for SECONDS CONDITION...
    n=$1; shift
    while [ $n -gt 0 ]; do "$@" && return 0; sleep 2; n=$((n - 2)); done
    return 1
}
in_fastboot() { [ -n "$(fastboot devices 2>/dev/null)" ]; }
booted() { [ "$(state)" = device ] && [ "$(t getprop sys.boot_completed)" = 1 ]; }

flash_boot() {
    st=$(state)
    case "$st" in
    device|recovery) b=$(system_build)
        [ "$b" = "$FIRMWARE_BUILD" ] || die "the Echo's system is build '${b:-unknown}', the boot image is $FIRMWARE_BUILD's: flash $FIRMWARE_FILE in TWRP first:
  adb push $FW/$FIRMWARE_FILE /sdcard/update.zip     # TWRP lists only .zip; check the size on the Echo after
  adb shell twrp install /sdcard/update.zip           # ($DDIR/README.md)";;
    *) die "no Echo on adb: boot it into TWRP (Volume up while connecting power) and run this again";;
    esac
    echo "to hacked fastboot ..."
    adb reboot bootloader
    if ! wait_for 60 in_fastboot; then
        echo "not in fastboot yet: unplug the power, hold Volume down and plug it in again (USB stays connected)"
        wait_for 300 in_fastboot || die "no fastboot device"
    fi
    prod=$(fastboot getvar product 2>&1 | sed -n 's/^product: *//p' | tr 'A-Z' 'a-z')
    [ -z "$prod" ] || [ "$prod" = "$PRODUCT" ] || die "fastboot reports product '$prod', not $PRODUCT: nothing flashed"
    # the boot partition and nothing else
    fastboot flash boot $IMG
    # not "fastboot reboot": adb reboot bootloader leaves the RTC's boot-to-fastboot flag set and kaeru 2.0.0 does not
    # clear it (getvar boot-reason: RTC), so every warm reboot lands in fastboot again; continue boots boot from here
    fastboot continue
    echo "booting (the first boot after a new boot image takes a few minutes) ..."
    if ! wait_for 120 booted; then
        in_fastboot && echo "still in fastboot: unplug the Echo's power, wait 5 s and plug it in again (no buttons; USB stays connected)"
        wait_for 480 booted || die "the Echo did not come up with adb.  Hacked fastboot (Volume down while connecting power), then
  fastboot flash boot $OUT/boot.stock.img     # stock, as long as nothing was written to /system yet
  fastboot continue"
    fi
}

if [ "$1" = --uninstall ]; then
    [ "$(state)" = device ] || die "boot the Echo (with the root boot image) and connect it over USB"
    t "[ -f /system/hassmic/sysinstall.sh ]" || die "hassmic is not installed on this Echo's system partition"
    t "sh /system/hassmic/alexa-on.sh > /dev/null 2>&1; rm -f /data/local/hassmic/hassmic.conf"
    t "rm -rf $STAGE; mkdir -p $STAGE; cp /system/hassmic/otatool /system/hassmic/sysinstall.sh $STAGE/ &&
       sh $STAGE/sysinstall.sh uninstall $STAGE && touch $STAGE/ok"
    t "[ -f $STAGE/ok ]" || die "system partition not written (see above)"
    t "rm -rf $STAGE"
    echo "removed; Alexa's apps are on again.  The root boot image stays: /system was written, the stock boot.img would"
    echo "not accept it.  All the way back to stock: flash $FIRMWARE_FILE in TWRP.  Rebooting"
    adb reboot; exit 0
fi

boot_image
[ "$1" = --boot-only ] && BOOT_ONLY=1 && shift
if boot_is_ours; then echo "boot partition holds this boot image already"
else flash_boot; fi
wait_for 600 booted || die "the Echo is not booted with adb"
[ "$(t id -u)" = 0 ] && [ "$(t cat /proc/self/attr/current | tr -d '\0')" = u:r:su:s0 ] ||
    die "adb is not a root shell in the su domain ($(t id); $(t cat /proc/self/attr/current | tr -d "\\0"))"
echo "root adb: ok"
[ -n "$BOOT_ONLY" ] && exit 0

# --- part 2: the files, from the running OS (as install-system.sh does on donut)
device_check_firmware
NAME=${1:-$DEFAULT_NAME}
build_binaries                          # built here or this commit's release build (PREBUILT, scripts/lib/build.sh)
mkdir -p secrets
[ -f secrets/update.key ] || { python3 scripts/otatool.py keygen secrets/update.key secrets/update.pub; echo "new update signing key: secrets/update.key (keep it, back it up)"; }
printf 'NAME="%s"\nARGS="%s"\n' "$NAME" "$DEFAULT_ARGS" > $OUT/hassmic.conf
t "mkdir -p /data/local/hassmic"; adb push $OUT/hassmic.conf /data/local/hassmic/hassmic.conf > /dev/null
t "rm -rf /data/local/hassmic/ota; rm -f /data/local/hassmic/hassmic"      # boot.sh prefers those; the fresh install wins
FILES="$OUT/hassmic $OUT/runas $(ls $OUT/pryon_test $OUT/*.ko 2>/dev/null || true) $OUT/otatool secrets/update.pub keys/release.pub $DDIR/device.conf
       scripts/system/boot.sh scripts/system/main.sh scripts/system/sysinstall.sh scripts/device/lockdown.sh scripts/device/alexa-off.sh scripts/device/alexa-on.sh scripts/device/artifact-install.sh scripts/device/wifi.sh scripts/device/wifictl.dex"
t "rm -rf $STAGE; mkdir -p $STAGE"
adb push $FILES $DDIR/hassmic.rc $STAGE/ > /dev/null
t "chmod 755 $STAGE/otatool; sh $STAGE/sysinstall.sh install $STAGE && touch $STAGE/ok"
t "[ -f $STAGE/ok ]" || { t "rm -rf $STAGE"; die "system partition not written (see above); nothing to undo"; }
t "rm -rf $STAGE; ls -lZ /system/hassmic /system/etc/init/hassmic.rc"
echo "install done on the running $DEVICE; rebooting"
adb reboot
