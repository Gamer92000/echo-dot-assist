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
#   install-boot.sh                 the Echo names itself (model and MAC address); name it in Home Assistant
#   install-boot.sh --boot-only     only part 1: root adb, e.g. for scripts/deploy.sh trial runs before installing
#   install-boot.sh --uninstall     Alexa's apps back on, hassmic off the system partition, cache and data wiped in TWRP
#                                   (the root boot image stays); --uninstall --yes without asking
set -e
cd "$(dirname "$0")/.."
. scripts/lib/device.sh; device_load adb
. scripts/lib/build.sh
[ "$INSTALL" = boot ] || die "$DEVICE installs with INSTALL=$INSTALL, not with this script ($DDIR/README.md)"
STAGE=/data/local/tmp/hm-install
IMG=$OUT/boot-hassmic.img
t() { adb shell "$@" | tr -d '\r'; }
# whether FILE exists on the Echo: read from its answer, since t's pipe into tr leaves adb's exit status behind
on_echo() { [ "$(t "[ -e $1 ] && echo yes")" = yes ]; }
state() { adb get-state 2>/dev/null || true; }
sha() { sha256sum "$1" | cut -c1-64; }

# --- part 1, on the PC: the boot image
boot_image() {
    mkdir -p $OUT
    [ -f $FW/$FIRMWARE_FILE ] || die "no $FW/$FIRMWARE_FILE ($DDIR/README.md)"
    unzip -p $FW/$FIRMWARE_FILE boot.img > $OUT/boot.stock.img
    [ "$(sha $OUT/boot.stock.img)" = "$BOOT_SHA256" ] || die "boot.img in $FIRMWARE_FILE is not the one this was built for"
    [ -f $SEPOLICY_TOOL ] && [ "$(sha $SEPOLICY_TOOL)" = "$SEPOLICY_TOOL_SHA256" ] ||
        die "no $SEPOLICY_TOOL (or not the pinned one): lib/armeabi-v7a/libmagiskpolicy.so of Magisk v30.7's APK ($DDIR/README.md; scripts/setup.sh fetches it)"
    [ -f $ADBD ] && [ "$(sha $ADBD)" = "$ADBD_SHA256" ] ||
        die "no $ADBD (or not the pinned one): sbin/adbd of Google's Android 7.1.1 emulator image ($DDIR/README.md; scripts/setup.sh fetches it)"
    [ -e $FW/rootfs/system/bin/linker ] || die "$FW/rootfs not unpacked: qemu runs magiskpolicy against its linker ($DDIR/README.md)"
    { command -v qemu-arm > /dev/null || [ -x toolchain/qemu-arm ]; } || die "no qemu-arm (qemu user mode; $DDIR/setup.sh puts one in toolchain/)"
    python3 scripts/mkbootroot.py --extract $OUT/boot.stock.img sepolicy $OUT/sepolicy.stock
    rm -f $OUT/sepolicy.hassmic
    chmod 755 $SEPOLICY_TOOL
    DEVICE=$DEVICE tools/qrun.sh -t 120 $SEPOLICY_TOOL --load $OUT/sepolicy.stock --apply $DDIR/sepolicy.rules \
        --save $OUT/sepolicy.hassmic > $OUT/qrun.log 2>&1 || true
    # qrun.sh says there why it did not run qemu (no user namespace: Ubuntu 24.04 and later)
    [ -s $OUT/sepolicy.hassmic ] ||
        die "the policy patch failed (tools/qrun.sh $SEPOLICY_TOOL): $(grep '^qrun.sh' $OUT/qrun.log || tail -n 3 $OUT/qrun.log)"
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
on_adb() { [ "$(state)" = device ] || [ "$(state)" = recovery ]; }
booted() { [ "$(state)" = device ] && [ "$(t getprop sys.boot_completed)" = 1 ]; }

# flash_boot [--boot-only]: IMG into the boot partition through hacked fastboot and booted; --boot-only skips the flash
# (the partition holds IMG, the Echo sits in TWRP).  Always through fastboot continue, never a plain reboot: see there
flash_boot() {
    st=$(state)
    case "$st" in
    device|recovery) b=$(system_build)
        # in TWRP, where a write of the system partition may have been cut short: its build.prop is in the first part
        if [ "$b" = "$FIRMWARE_BUILD" ] && [ "$st" = recovery ] && [ -n "$SYSTEM_IMG_SHA256" ]; then
            echo "checking the system partition (2 min) ..."
            [ "$(t sha256sum $SYSTEM_DEV | cut -c1-64)" = "$SYSTEM_IMG_SHA256" ] || b="$FIRMWARE_BUILD, but not all of it (a write cut short)"
        fi
        [ "$b" = "$FIRMWARE_BUILD" ] || die "the Echo's system is build '${b:-unknown}', the boot image is $FIRMWARE_BUILD's: write $FIRMWARE_ID's
  system partition first (scripts/setup.sh $DEVICE does it in its step \"System partition\"; by hand, in TWRP:
  $DDIR/README.md \"System partition\")";;
    *) die "no Echo on adb (state: $(adb get-state 2>&1 | head -1)): boot it into TWRP (Volume up while connecting power),
  tap \"Keep Read Only\" if TWRP asks \"Keep System Read only?\", and run this again";;
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
    [ "$1" = --boot-only ] || fastboot flash boot $IMG
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

# The uninstall ends with a wipe of cache and data in TWRP.  The install took Amazon's apps off the system partition
# (STOCK_APPS_OUT), and Android then forgot them: their data went, and with it the permissions it grants system apps
# by default only at the first boot after a wipe.  Put back on /system they came up as new apps without either: on a
# cronos after an uninstall (2026-11-03), Amazon's setup wizard (com.amazon.kindle.otter.oobe) crashed every 2-3 s
# (ConfigBundleProvider: no unzipped device config bundle, a NullPointerException) and amazon.speech.sim on its
# missing RECORD_AUDIO: "OOBE has stopped" and "SpeechUI has stopped" on the screen for good.  After the wipe the
# first boot granted it (SYSTEM_FIXED GRANTED_BY_DEFAULT), the wizard came up, no crash.  Nothing is kept that a
# working Echo would need: the uninstalled one starts at Amazon's setup either way.  The wipe: scripts/lib/twrp.sh.
. scripts/lib/twrp.sh
in_recovery() { [ "$(state)" = recovery ]; }

# yes or no: hassmic on the system partition.  In TWRP from the partition itself, read-only (as system_build), or
# where TWRP has it mounted already (a second mount read-only next to a writable one fails).  Nothing when the Echo
# does not answer or TWRP cannot mount it
hassmic_on_system() {
    case "$(state)" in
    device) t '[ -e /system/hassmic/sysinstall.sh ] && echo yes || echo no';;
    recovery) t 'd=/mnt/hm_sys
                 if grep -q " /system " /proc/mounts; then d=/system
                 else mkdir -p $d; mount -o ro '"$SYSTEM_DEV"' $d 2>/dev/null || d=; fi
                 [ -z "$d" ] || { [ -e $d/hassmic/sysinstall.sh ] && echo yes || echo no; }
                 [ "$d" != /mnt/hm_sys ] || umount $d';;
    esac
}

if [ "$1" = --uninstall ]; then
    on_adb || die "boot the Echo (with the root boot image) and connect it over USB"
    # Installed: hassmic off /system first, from Fire OS, where sysinstall.sh runs.  Not (a run cut off before its
    # wipe, or an Echo uninstalled by an older version of this script): only the wipe is left to do.  In TWRP the
    # partition is read as well: a wipe alone would leave hassmic there and Amazon's apps off it, under an Echo told
    # to start at Amazon's setup
    case "$(hassmic_on_system)" in
    yes) in_recovery && die "hassmic is still on this Echo's system partition, and it comes off from Fire OS: boot it
  (adb reboot) and run this again.  Nothing changed"
         INSTALLED=1;;
    no) INSTALLED=;;
    *) die "cannot tell whether hassmic is on this Echo's system partition (no answer$(in_recovery &&
            echo ", or TWRP cannot mount $SYSTEM_DEV")): run this again.  Nothing changed";;
    esac
    if [ -n "$INSTALLED" ]; then
        echo "This takes hassmic off the Echo and wipes its data (Wi-Fi, Amazon account, hassmic's settings and its link"
        echo "to Home Assistant): the Echo starts at Amazon's setup.  Without the wipe Amazon's apps, put back on the"
        echo "system partition, crash in a loop (\"OOBE has stopped\")."
    else
        echo "hassmic is not on this Echo's system partition (uninstalled before): this only wipes its data (Wi-Fi,"
        echo "Amazon account).  Without the wipe Amazon's apps, put back on the system partition, crash in a loop"
        echo "(\"OOBE has stopped\").  The Echo starts at Amazon's setup."
    fi
    if [ "$2" != --yes ]; then
        [ -t 0 ] || die "not on a terminal: run it as install-system.sh --uninstall --yes"
        printf 'Type YES to go on: '; read -r a; [ "$a" = YES ] || die "nothing changed"
    fi
    if [ -n "$INSTALLED" ]; then
        t "sh /system/hassmic/alexa-on.sh > /dev/null 2>&1; rm -f /data/local/hassmic/hassmic.conf"
        t "rm -rf $STAGE; mkdir -p $STAGE; cp /system/hassmic/otatool /system/hassmic/sysinstall.sh $STAGE/ &&
           sh $STAGE/sysinstall.sh uninstall $STAGE && touch $STAGE/ok"
        on_echo $STAGE/ok || die "system partition not written (see above)"
        t "rm -rf $STAGE"
        echo "hassmic is off the system partition"
    fi
    if ! in_recovery; then
        echo "to TWRP ..."
        adb reboot recovery
        wait_for 120 in_recovery || die "the Echo is not in TWRP: unplug its power, hold Volume up and plug it in again
  (USB stays connected), then run this again"
    fi
    echo "On the Echo's screen: if TWRP asks \"Keep System Read only?\", tap \"Keep Read Only\" (not the swipe)"
    # with nobody to press Enter, the wipe starts at once; wipe_twrp gives it 5 min, should TWRP wait for that tap
    if [ -t 0 ]; then printf 'and press Enter here once TWRP shows its main menu: '; read -r a; fi
    wait_for 60 in_recovery || die "TWRP is not back on adb: run this again"
    echo "wiping cache and data ..."
    wipe_twrp || die "the wipe did not take: run this again, or wipe in TWRP's menu (Wipe, Factory Reset)"
    echo "wiped.  The root boot image stays: /system was written, the stock boot.img would not accept it.  All the way"
    echo "back to stock: $DDIR/README.md.  Rebooting (the first boot after a wipe takes a few minutes) ..."
    adb reboot
    wait_for 600 booted && echo "Fire OS is up: set the Echo up on its screen as a new one" ||
        echo "not booted after 10 min: unplug its power and plug it in again (no buttons)"
    exit 0
fi

boot_image
[ "$1" = --boot-only ] && BOOT_ONLY=1 && shift
# TWRP's "Keep System Read only?" screen: answering it restarts TWRP's USB, and adb is gone for a few seconds
if ! on_adb; then
    echo "waiting for the Echo on adb (TWRP asking \"Keep System Read only?\" on its screen: tap \"Keep Read Only\") ..."
    wait_for 120 on_adb || true
fi
if boot_is_ours; then
    echo "boot partition holds this boot image already"
    [ "$(state)" = recovery ] && flash_boot --boot-only      # in TWRP: boot it
else flash_boot; fi
wait_for 600 booted || die "the Echo is not booted with adb"
[ "$(t id -u)" = 0 ] && [ "$(t cat /proc/self/attr/current | tr -d '\0')" = u:r:su:s0 ] ||
    die "adb is not a root shell in the su domain ($(t id); $(t cat /proc/self/attr/current | tr -d "\\0"))"
echo "root adb: ok"
[ -n "$BOOT_ONLY" ] && exit 0

# --- part 2: the files, from the running OS (as install-system.sh does on donut)
device_check_firmware
build_binaries                          # built here or this commit's release build (PREBUILT, scripts/lib/build.sh)
mkdir -p secrets
[ -f secrets/update.key ] || { python3 scripts/otatool.py keygen secrets/update.key secrets/update.pub; echo "new update signing key: secrets/update.key (keep it, back it up)"; }
printf 'ARGS="%s"\n' "$DEFAULT_ARGS" > $OUT/hassmic.conf
t "mkdir -p /data/local/hassmic"; adb push $OUT/hassmic.conf /data/local/hassmic/hassmic.conf > /dev/null
t "rm -rf /data/local/hassmic/ota; rm -f /data/local/hassmic/hassmic"      # boot.sh prefers those; the fresh install wins
# the wake word aligners too, as bundle.sh ships them (sysinstall.sh leaves them out where /system has no room)
FILES="$OUT/hassmic $OUT/runas $(ls $OUT/pryon_test $OUT/*.ko $OUT/libhmui.so 2>/dev/null || true) $OUT/otatool secrets/update.pub $RELEASE_PUB $DDIR/device.conf
       scripts/system/boot.sh scripts/system/main.sh scripts/system/sysinstall.sh scripts/device/lockdown.sh scripts/device/alexa-off.sh scripts/device/alexa-on.sh scripts/device/artifact-install.sh scripts/device/wifi.sh scripts/device/wifictl.dex scripts/device/ringbar.dex scripts/device/screen.dex
       ${HOME_APK:+scripts/device/$HOME_APK} $(echo wakealign/*.wa wakealign/*.was)"
t "rm -rf $STAGE; mkdir -p $STAGE"
adb push $FILES $DDIR/hassmic.rc $STAGE/ > /dev/null
t "chmod 755 $STAGE/otatool; sh $STAGE/sysinstall.sh install $STAGE && touch $STAGE/ok"
on_echo $STAGE/ok || { t "rm -rf $STAGE"; die "system partition not written (see above); nothing to undo"; }
t "rm -rf $STAGE; ls -lZ /system/hassmic /system/etc/init/hassmic.rc"
echo "install done on the running $DEVICE; rebooting"
adb reboot
# the RTC's fastboot flag may still be set from the flash (kaeru leaves it): then this reboot stops in fastboot too
if wait_for 90 in_fastboot; then
    echo "in fastboot: booting on with fastboot continue"
    fastboot continue
fi
