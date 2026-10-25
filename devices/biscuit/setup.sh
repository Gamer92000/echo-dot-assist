# Guided installation for the Echo Dot 2 (biscuit), run by scripts/setup.sh (bash, from the repository root, with
# device.conf loaded and the helpers of scripts/lib/setup.sh).  The same steps, written out: README.md next to this file.
# Each step_<id> returns non-zero when it did not finish; the runner then offers retry, skip or quit.  A step checks
# first whether its result is there already, so a rerun (or work done by hand) passes through quietly.
# Tried on a real Echo Dot 2 (2026-09-28).  The unlock follows R0rt1z2's XDA thread, whose instructions are
# authoritative where this script simplifies.

XDA=https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-dot-2nd-gen-2016-biscuit.4761416/
FTVDB=https://ftvdb.com/echo/firmware/com.amazon.biscuit.android.os/8ca06ee4ef2806c974d2944b7fadd543-13222531716-fire-os-6574-1-ns65741-8146-2026-09-17/
# v2.0.1 (issue #50): adds fastbrick-20260617.img for Fire OS 8338's LK 2c3569d-20260617_045829, which v2.0.0's
# default image could not unlock ("restricted on locked hw"); its kaeru boots the active slot (v2.0.0: boot_a)
AMONET=amonet-biscuit-v2.0.1.zip
AMONET_SHA256=ea9cada067783a0d83f025bdd669fd5689148f555054475c7c70c634c542f3b5
BOOTROOT=boot-root.zip                      # from this model's XDA thread; byte-identical to donut's
BOOTROOT_SHA256=de49cc88b27a8e77cf97cf0156bee50e4ddc0e116c41aaede06b494e38397be0
NDK=toolchain/android-ndk-r21e
NDK_URL=https://dl.google.com/android/repository/android-ndk-r21e-linux-x86_64.zip

STEPS="
tools|Tools on this PC
files|Downloads
udev|USB access for the unlock
usb|Micro-USB cable
unlock|Unlock, boot TWRP
flash|Stock firmware into both slots
root|Root
build|Build
network|Lock down, join Wi-Fi
install|Install
"
ROOTED_SKIP="udev usb unlock flash root"     # done on an Echo that is rooted already (scripts/setup.sh asks)
# Windows (issue #60): the unlock step runs there too, with firebreak (unlock_firebreak), so scripts/setup.sh does not
# stop at the start for want of an Echo on adb.  Pinned: its command line and where it leaves the Echo are relied on.
UNLOCK_WINDOWS=1
FIREBREAK=firebreak@0.8.0
FIREBREAK_DRIVERS=https://github.com/bboe/firebreak#windows
NOTES=(
    "adb over Wi-Fi is closed: scripts/adb-wifi.sh <echo-ip> (or debug access on the settings page, http://<echo-ip>:28931/) opens it for 30 min."
    "Updates: git pull, then scripts/ota-push.sh <echo-ip>."
    "More wake words, whisper or sound detection models: the settings page, http://<echo-ip>:28931/ → \"Download from Amazon\"."
    "Music Assistant, Bluetooth speaker: $DDIR/README.md"
)

slot() { ashell bcbtool get_active | tr -d " \n"; }

step_tools() {
    command -v curl > /dev/null || need_tools curl || return 1
    build_mode || return 1
    local unlock="fastboot python3 pyusb"
    { rooted_already || [ -n "$WINDOWS" ]; } && unlock=python3      # no unlock here: python3 only for otatool.py
    [ -n "$WINDOWS" ] && ! rooted_already && unlock="python3 fastboot uvx"   # firebreak, run by uv, takes fastboot
    if [ "$BUILD_MODE" = prebuilt ]; then need_tools adb $unlock unzip curl sha256sum || return 1; return 0; fi
    need_tools adb $unlock make cc unzip debugfs curl sha256sum bc xz || return 1
    [ "$(df -Pk . | awk 'NR == 2 { print $4 }')" -gt 5000000 ] || warn "less than 5 GB free here; the NDK and firmware need about that"
}

step_files() {
    local need=()
    # a rooted Echo needs the firmware only to build against, and the unlock tool not at all; nor does Windows, where
    # firebreak downloads the same build and amonet itself (checked against its own pins) and nothing is built
    { rooted_already || [ -n "$WINDOWS" ]; } && { [ "$BUILD_MODE" = prebuilt ] || [ -d $FW/rootfs/system/lib ]; } ||
        need+=($FW/$FIRMWARE_FILE $FIRMWARE_SHA256 "Fire OS $FIRMWARE_ID: $FTVDB")
    rooted_already || [ -n "$WINDOWS" ] || need+=($FW/$AMONET $AMONET_SHA256 "attachment in $XDA")
    need_files "${need[@]}" $FW/$BOOTROOT $BOOTROOT_SHA256 "attachment in $XDA" || return 1
    [ "$BUILD_MODE" = prebuilt ] && return 0          # nothing to build with
    if [ -x $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang ]; then ok "Android NDK r21e"; else
        mkdir -p toolchain
        TASK_NOTE="du -h toolchain/ndk.zip | cut -f1" task "Downloading Android NDK r21e (1 GB)" curl -fsSL -o toolchain/ndk.zip $NDK_URL &&
            task "Unpacking the NDK" unzip -q -o toolchain/ndk.zip -d toolchain && rm -f toolchain/ndk.zip || return 1
    fi
    kernel_tools
}

# lets the unlock reach the Echo without sudo, and keeps ModemManager from probing the bootrom's serial port mid-handshake
step_udev() {
    local rules=/etc/udev/rules.d/51-echo-unlock.rules
    cmp -s scripts/51-echo-unlock.rules $rules && { ok "udev rules installed"; return 0; }
    need_sudo "to install udev rules" || return 1
    task "Installing udev rules" sh -c "sudo cp scripts/51-echo-unlock.rules $rules && sudo udevadm control --reload"
}

step_usb() {
    [ -z "$DRY" ] && { adb_is recovery || adb_is device; } && { ok "the Echo is on USB already"; return 0; }
    # a working micro-USB socket, unlike the Dot 3: no case opening, no soldering; power still from its own adapter
    todo "Connect the Echo's micro-USB socket to this PC" "A data cable, not a charge-only one. Leave the power adapter unplugged."
}

step_unlock() {
    [ -n "$WINDOWS" ] && { unlock_firebreak; return; }
    [ -z "$DRY" ] && adb_is recovery && { ok "TWRP is running already"; return 0; }
    # Fire OS on adb: unlocked before (stock has no adb), and the unlock left TWRP on the recovery partition
    if [ -z "$DRY" ] && adb_is device; then
        ok "unlocked already"
        task "Restarting into TWRP" adb reboot recovery || return 1
        wait_adb recovery; return
    fi
    local adir udir=$FW/${AMONET%.zip}        # a folder per version: an older one unpacked before is not reused
    # a run whose files step passed with an older amonet (issue #50: v2.0.0) has not got this one yet
    [ -f $FW/$AMONET ] || need_files $FW/$AMONET $AMONET_SHA256 "attachment in $XDA" || return 1
    [ -d $udir ] || task "Unpacking amonet" unzip -q $FW/$AMONET -d $udir || return 1
    # run fastbrick.sh from wherever the zip put it (the release zip keeps it in a folder amonet/ of its own)
    adir=$(find $udir -name fastbrick.sh -print -quit 2>/dev/null); adir=${adir%/*}
    [ -n "$adir" ] || [ -n "$DRY" ] || { fail "no fastbrick.sh in $udir; delete that folder and try again"; return 1; }
    tell "Put the Echo into fastboot mode" "Hold the action button (•) and plug the power in." \
        "Keep holding until the ring shows a green light (an Echo unlocked before shows a rainbow instead)."
    waitfor "Waiting for fastboot|Fastboot" '[ -n "$(fastboot devices)" ]' "No green light? Unplug the power and try again." || return 1
    tell "amonet asks you to type YES, and for a key press at the end" "${RED}${B}Do not interrupt it once it runs: that can brick the Echo.$N"
    live "amonet fastbrick (about a minute)" in_dir "$adir" ./fastbrick.sh || return 1
    unlock_wait_twrp       # gives up when the unlock did not take (issue #50)
}

# Windows (issue #60): firebreak's "unlock fireos6" ends where flash and root end here, on rooted Fire OS 6 build 8146
# ($FIRMWARE_FILE) in both slots with boot-root, amonet v2.0.1 and TWRP 3.7.0 on recovery (its README and
# route.py).  It takes the Echo there from stock or from any amonet before (v1's TWRP 3.2.3 too, which cannot install
# this firmware: twrp_check), checks the bootloader first, and goes on where a stopped run left off; it asks for the
# fastboot gesture itself.  So on Windows the flash step only checks its result, and root passes on root adb.  An Echo
# rooted on $FIRMWARE_ID already is left as it is: firebreak has nothing to do there, and nothing here wipes it.
fw_rooted() { adb_is device && [ "$(ashell id -u)" = 0 ] && [ "$(ashell getprop ro.build.display.id)" = "$FIRMWARE_ID" ]; }
unlock_firebreak() {
    [ -z "$DRY" ] && fw_rooted && { ok "rooted, on $FIRMWARE_ID"; return 0; }
    tell "firebreak unlocks the Echo on Windows (https://github.com/bboe/firebreak)" \
        "It needs two USB drivers, installed once per PC from an administrator PowerShell: $FIREBREAK_DRIVERS" \
        "Without them the ring stays solid blue at the fastboot gesture."
    ask "Are the drivers installed?" || { info "Install them, then try again."; return 1; }
    tell "firebreak asks for the fastboot gesture and says what to do" "${RED}${B}Do not unplug the Echo while it writes: that can brick it.$N" \
        "Stopped or failed? Run this step again: firebreak goes on where it left off."
    # --managed-python: a Store Python keeps firebreak's files where adb and fastboot cannot see them (firebreak's README)
    live "firebreak unlock fireos6 (about 10 minutes)" uvx --managed-python $FIREBREAK unlock fireos6 || return 1
    [ -n "$DRY" ] && return 0
    wait_adb device || return 1
    fw_rooted || { fail "firebreak ended, but the Echo is not on rooted Fire OS $FIRMWARE_ID"; return 1; }
    ok "rooted, on $FIRMWARE_ID"
}

step_flash() {
    local s0 s1 s2
    if [ -n "$WINDOWS" ]; then
        [ -n "$DRY" ] && { info "Windows: firebreak flashed both slots in the unlock step"; return 0; }
        fw_rooted && { ok "Fire OS $FIRMWARE_ID in both slots (firebreak)"; return 0; }
        unlock_firebreak; return            # not there (the unlock step skipped?): firebreak goes on from where it is
    fi
    wait_adb recovery || return 1
    twrp_check || return 1
    task "Wiping cache and data" adb shell 'twrp wipe cache && twrp wipe data' || return 1
    task "Copying the firmware to the Echo" adb push $FW/$FIRMWARE_FILE /sdcard/update.zip || return 1
    [ -n "$DRY" ] || s0=$(slot)
    task "Flashing the first slot" adb shell twrp install /sdcard/update.zip || return 1
    # TWRP makes the slot it flashed the active one (radar setup log 2026-10-10: get_active said b right after flashing
    # B, and switching back to a by hand put the second install into B again).  Switch only if it did not.
    if [ -z "$DRY" ]; then
        s1=$(slot)
        if [ "$s1" = "$s0" ]; then
            task "Switching slots" adb shell 'case $(bcbtool get_active) in a) bcbtool set_active b;; b) bcbtool set_active a;; *) exit 1;; esac' || return 1
            s1=$(slot); [ "$s1" != "$s0" ] || { fail "slot switch did not take (active: $s1)"; return 1; }
        fi
    fi
    task "Restarting TWRP" adb reboot recovery || return 1
    [ -n "$DRY" ] || sleep 5
    wait_adb recovery || return 1
    task "Flashing the second slot" adb shell twrp install /sdcard/update.zip || return 1
    [ -n "$DRY" ] || { s2=$(slot); [ "$s2" != "$s1" ] || { fail "the second install went into slot $s1 again"; return 1; }; }
}

step_root() {
    [ -z "$DRY" ] && adb_is device && [ "$(ashell id -u)" = 0 ] && { ok "root adb already"; return 0; }
    # patches BOTH slots: boot cmdline, dm-verity off, properties, forced adb, fstab, OTA neutered, permissive su policy
    wait_adb recovery || return 1
    task "Installing boot-root" sh -c "adb push $FW/$BOOTROOT /sdcard/ && adb shell twrp install /sdcard/$BOOTROOT" || return 1
    task "Rebooting into Fire OS" adb reboot || return 1
    [ -n "$DRY" ] || sleep 8
    wait_adb device || return 1
    [ -n "$DRY" ] || [ "$(ashell id -u)" = 0 ] || { fail "adb shell is not root"; return 1; }
    ok "root adb"
    # online with Amazon it would update itself, and an update can close the hole the unlock uses
    warn "Do not set the Echo up with the Alexa app."
}

step_build() {
    # the firmware's libraries are only needed to link against
    if [ "$BUILD_MODE" != prebuilt ] && [ ! -d $FW/rootfs/system/lib ]; then
        task "Unpacking the firmware" sh -c "unzip -o -q $FW/$FIRMWARE_FILE payload.bin -d $FW &&
            python3 tools/payload_dump.py $FW/payload.bin $FW/images && mkdir -p $FW/rootfs &&
            debugfs -R 'rdump / $FW/rootfs' $FW/images/system.img && [ -d $FW/rootfs/system/lib ]" || return 1
    fi
    bootroot_unpack || return 1
    if [ "$BUILD_MODE" = prebuilt ]; then task "Downloading the release build of this commit" build_binaries || return 1
    else task "Building" build_binaries || return 1; fi
    wait_adb device || return 1
    task "Checking the Echo's firmware" sh -c 'out=$(scripts/probe.sh) && echo "$out" && ! echo "$out" | grep -q DIFFERENT' ||
        { fail "the Echo runs another firmware than $FIRMWARE_ID: redo the firmware step"; return 1; }
}

step_network() {
    wait_adb device || return 1
    task "Copying hassmic to the Echo" scripts/deploy.sh || return 1
    # adb can drop out for a moment here, shortly after the boot (seen on an Echo Dot 2, issue #2)
    network_lock && network_wifi
}

step_install() {
    bootroot_unpack || return 1        # again: the build step may have been skipped
    install_satellite
}
