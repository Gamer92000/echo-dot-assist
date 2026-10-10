# Guided installation for the Echo Show 5 1st gen (checkers), run by scripts/setup.sh (bash, from the repository
# root, with device.conf loaded and the helpers of scripts/lib/setup.sh).  The same steps, written out: README.md
# next to this file.  Each step_<id> returns non-zero when it did not finish; the runner then offers retry, skip or
# quit.  A step checks first whether its result is there already, so a rerun (or work done by hand) passes through
# quietly.

UNTESTED=1                                 # nothing here has run on an Echo Show yet (scripts/setup.sh says so)
XDA=https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-show-5-1st-gen-2019-checkers.4762900/
AMONET=amonet-checkers-v2.0.1.zip
AMONET_SHA256=770324a8ed5ab922c0383f8ba072d70fc0190cc2c879f12f67b8d6cfa3ad30ee
# donut's boot-root.zip: its patch/magiskpolicy32 is the policy tool (device.conf).  Where donut's setup keeps it (a PC
# that set up a Dot has it), not in firmware/checkers: boot-root.zip there is checkers' own XDA image (README.md)
BOOTROOT=firmware/donut/boot-root.zip
BOOTROOT_SHA256=de49cc88b27a8e77cf97cf0156bee50e4ddc0e116c41aaede06b494e38397be0
# checkers' own boot-root.zip (the XDA image): its sbin/adbd goes into our boot image (device.conf ADBD)
XDAROOT=$FW/boot-root.zip
XDAROOT_SHA256=b2474113a1f3a4a8de6728ff72774761051945641e1a5ffa78ee25c2b19a809e
DONUT_XDA=https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-dot-3rd-gen-2018-donut.4801400/
QEMU=toolchain/qemu-arm                     # magiskpolicy runs under it, against the unpacked firmware
QEMU_URL=https://github.com/multiarch/qemu-user-static/releases/download/v7.2.0-1/qemu-arm-static
QEMU_SHA256=9f07762a3cd0f8a199cb5471a92402a4765f8e2fcb7fe91a87ee75da9616a806
NDK=toolchain/android-ndk-r21e
NDK_URL=https://dl.google.com/android/repository/android-ndk-r21e-linux-x86_64.zip

STEPS="
tools|Tools on this PC
files|Downloads
usb|Power and USB to the Echo
unlock|Unlock, boot TWRP
root|Root boot image
build|Build
network|Lock down, join Wi-Fi
install|Install
"
ROOTED_SKIP="usb unlock root"     # done on an Echo that is rooted already (scripts/setup.sh asks)
NOTES=(
    "The screen keeps Amazon's clock face and settings; Alexa's apps stay off while hassmic runs ($DDIR/README.md)."
    "adb over Wi-Fi: the settings page's debug access (http://<echo-ip>:28931/), as on the other Echos, opens it for 30 min."
    "Updates: git pull, then scripts/ota-push.sh <echo-ip>."
    "More wake words, whisper or sound detection models: \"Download from Amazon\" on the settings page (scripts/artifacts.sh <echo-ip> is the fallback)."
    "No Bluetooth and no Music Assistant pairing on this model yet: Android's stack owns the controller ($DDIR/README.md)."
)

sha() { sha256sum < "$1" | cut -c1-64; }
policy_tool() { [ -f $SEPOLICY_TOOL ] && [ "$(sha $SEPOLICY_TOOL)" = "$SEPOLICY_TOOL_SHA256" ]; }
# the policy tool, out of donut's boot-root (the files step brought it)
magiskpolicy() {
    policy_tool && return 0
    task "Taking magiskpolicy out of donut's boot-root" sh -c "mkdir -p ${SEPOLICY_TOOL%/*} &&
        unzip -p $BOOTROOT patch/magiskpolicy32 > $SEPOLICY_TOOL &&
        [ \"\$(sha256sum < $SEPOLICY_TOOL | cut -c1-64)\" = $SEPOLICY_TOOL_SHA256 ]" || { rm -f $SEPOLICY_TOOL; return 1; }
}
adbd_ok() { [ -f $ADBD ] && [ "$(sha $ADBD)" = "$ADBD_SHA256" ]; }
# the root-capable adbd, out of the XDA image in checkers' boot-root (the files step brings it; a setup that passed
# that step before it asked for this one asks here)
adbd() {
    adbd_ok && return 0
    need_files $XDAROOT $XDAROOT_SHA256 "boot-root.zip, attachment in $XDA (checkers' own, for its adbd)" || return 1
    task "Taking adbd out of the XDA boot-root" sh -c "unzip -p $XDAROOT boot-root.img > $FW/boot-root.img &&
        python3 scripts/mkbootroot.py --extract $FW/boot-root.img sbin/adbd $ADBD && rm -f $FW/boot-root.img &&
        [ \"\$(sha256sum < $ADBD | cut -c1-64)\" = $ADBD_SHA256 ]" || { rm -f $ADBD $FW/boot-root.img; return 1; }
}
# The firmware unpacked, whatever the build mode: the build links against its libraries, and every boot image is
# made from it (install-boot.sh: qemu runs the policy tool against its linker), at the root step as at the install.
# debugfs exits 0 whatever happens and rdump does not create its target (issue #4): the result is checked by hand.
rootfs() {
    [ -e $FW/rootfs/system/bin/linker ] && return 0
    task "Unpacking the firmware" sh -c "unzip -o -q $FW/$FIRMWARE_FILE system.new.dat system.transfer.list -d $FW &&
        mkdir -p $FW/images $FW/rootfs/system &&
        python3 tools/sdat2img.py $FW/system.transfer.list $FW/system.new.dat $FW/images/system.img &&
        rm -f $FW/system.new.dat $FW/system.transfer.list &&
        debugfs -R 'rdump / $FW/rootfs/system' $FW/images/system.img && [ -e $FW/rootfs/system/bin/linker ]"
}

step_tools() {
    command -v curl > /dev/null || need_tools curl || return 1
    build_mode
    local unlock="fastboot python3"
    rooted_already && unlock=python3
    if [ "$BUILD_MODE" = prebuilt ]; then need_tools adb $unlock unzip debugfs sqlite3 curl sha256sum || return 1; return 0; fi
    need_tools adb $unlock unzip make cc debugfs sqlite3 curl sha256sum bc xz || return 1
    [ "$(df -Pk . | awk 'NR == 2 { print $4 }')" -gt 5000000 ] || warn "less than 5 GB free here; the NDK and firmware need about that"
}

step_files() {
    local need=()
    # the firmware, rooted Echo or not, built here or not: the boot image is made from its boot.img and policy
    need+=($FW/$FIRMWARE_FILE $FIRMWARE_SHA256 "Fire OS $FIRMWARE_ID, from Amazon: the link is in $DDIR/README.md")
    rooted_already || need+=($FW/$AMONET $AMONET_SHA256 "attachment in $XDA")
    adbd_ok || need+=($XDAROOT $XDAROOT_SHA256 "boot-root.zip, attachment in $XDA (checkers' own, for its adbd)")
    policy_tool || need+=($BOOTROOT $BOOTROOT_SHA256 "boot-root.zip, attachment in $DONUT_XDA (the Echo Dot 3's, for its magiskpolicy; not checkers' own)")
    need_files "${need[@]}" || return 1
    # the policy is patched on the PC, under qemu against the unpacked firmware; a system qemu-arm is fine too
    if ! command -v qemu-arm > /dev/null && [ ! -x $QEMU ]; then
        mkdir -p toolchain
        task "Downloading qemu-arm (runs the policy tool on the PC)" sh -c "curl -fsSL -o $QEMU $QEMU_URL &&
            [ \"\$(sha256sum < $QEMU | cut -c1-64)\" = $QEMU_SHA256 ] && chmod 755 $QEMU" || { rm -f $QEMU; return 1; }
    fi
    [ "$BUILD_MODE" = prebuilt ] && return 0          # nothing to build with
    if [ -x $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang ]; then ok "Android NDK r21e"; else
        mkdir -p toolchain
        TASK_NOTE="du -h toolchain/ndk.zip | cut -f1" task "Downloading Android NDK r21e (1 GB)" curl -fsSL -o toolchain/ndk.zip $NDK_URL &&
            task "Unpacking the NDK" unzip -q -o toolchain/ndk.zip -d toolchain && rm -f toolchain/ndk.zip || return 1
    fi
}

step_usb() {
    [ -z "$DRY" ] && { adb_is recovery || adb_is device; } && { ok "the Echo is on USB already"; return 0; }
    todo "Sit the Echo near this PC, with its power supply and a micro-USB cable" \
        "" \
        "Nothing has to be opened: this model unlocks through its USB port." \
        "Leave the Echo running normally for now; the cable goes in when the unlock step says so."
}

step_unlock() {
    [ -z "$DRY" ] && adb_is recovery && { ok "TWRP is running already"; return 0; }
    # Fire OS on adb: unlocked before (stock has no adb), and the unlock left TWRP on the recovery partition
    if [ -z "$DRY" ] && adb_is device; then
        ok "unlocked already"
        task "Restarting into TWRP" adb reboot recovery || return 1
        wait_adb recovery; return
    fi
    [ -x $FW/amonet/fastbrick.sh ] || task "Unpacking amonet" unzip -q -o $FW/$AMONET -d $FW || return 1
    tell "On the Echo, hold all three buttons (both volume keys and the camera shutter)" \
        "Keep them held until its screen says \"=> FASTBOOT mode...\", then let go" \
        "Plug the micro-USB cable into the Echo." \
        "" \
        "${RED}${B}The unlock must not be interrupted once it starts (up to 5 min): that bricks it for good.$N"
    # in the foreground: it asks to type YES, and for a key press at the end
    live "Unlocking with amonet's fastbrick" in_dir $FW/amonet env PYTHONUNBUFFERED=1 \
        bash -o pipefail -c "./fastbrick.sh 2>&1 | tee -a '$LOG'" || return 1
    wait_adb recovery || { fail "no TWRP on adb; $DDIR/README.md, step by step, then try again"; return 1; }
    warn "Leave the Echo in TWRP for now: stock Fire OS has no adb."
}

step_root() {
    [ -z "$DRY" ] && adb_is device && [ "$(ashell id -u)" = 0 ] && { ok "root adb already"; return 0; }
    rootfs && magiskpolicy && adbd || return 1
    # booted, adb as shell: an older boot image of ours (8149's own adbd never stays root) or stock with adb on;
    # install-boot.sh takes it from the running OS as well as from TWRP
    if [ -z "$DRY" ] && adb_is device; then info "adb is not root on the running Echo: its boot image is replaced"
    else wait_adb recovery || return 1; fi
    # the rooted boot image from the firmware's own (root adb, no verify, the policy patched for our services);
    # flashed in amonet's hacked fastboot, then the Echo boots Fire OS with root adb.  In the foreground: it may ask
    # for the power to be replugged
    live "Flashing the rooted boot image (the Echo reboots)" bash -o pipefail -c "scripts/install-boot.sh --boot-only 2>&1 | tee -a '$LOG'" || return 1
    ok "root adb"
    # online with Amazon it would update itself, and an update can close the way in
    warn "Do not set the Echo up with the Alexa app or its touchscreen."
}

step_build() {
    rootfs || return 1                  # the libraries to link against (the root step unpacked it already, as a rule)
    if [ "$BUILD_MODE" = prebuilt ]; then task "Downloading the release build of this commit" build_binaries || return 1
    else task "Building" build_binaries || return 1; fi
    wait_adb device || return 1
    task "Checking the Echo's firmware" sh -c 'out=$(scripts/probe.sh) && echo "$out" && ! echo "$out" | grep -q DIFFERENT' ||
        { fail "the Echo runs another firmware than $FIRMWARE_ID: flash $FIRMWARE_FILE in TWRP first ($DDIR/README.md)"; return 1; }
}

step_network() {
    local ssid pass new=
    wait_adb device || return 1
    task "Copying hassmic to the Echo" scripts/deploy.sh || return 1
    # adb can drop out for a moment here, shortly after the boot (seen on an Echo Dot 2, issue #2)
    wait_adb device || return 1
    task "Locking down the Echo's internet access" sh -c 'adb shell sh /data/local/hassmic/lockdown.sh &&
        adb shell iptables -S hassmic_out | grep -q -- "-j DROP"' || return 1
    if [ ! -f secrets/wifi.conf ]; then
        prompt ssid "Wi-Fi name (WPA2, same network as Home Assistant)"
        prompt pass "Wi-Fi password" "" -s
        [ -n "$DRY" ] || { mkdir -p secrets; ( umask 077; printf '%s\n%s\n' "$ssid" "$pass" > secrets/wifi.conf ); new=1; }
    fi
    # a typo in what was just typed would otherwise be retried forever
    task "Joining Wi-Fi" scripts/wifi-join.sh || { [ -n "$new" ] && rm -f secrets/wifi.conf; return 1; }
    # the Wi-Fi profile survives a reboot (Android's WifiConfigStore), the egress lock does not until the install
    warn "Do not restart the Echo until it is installed."
    info "Tip: give it a fixed address in your router (DHCP reservation)."
}

step_install() {
    rootfs && magiskpolicy && adbd || return 1        # again: the steps before may have been skipped
    install_satellite
}

