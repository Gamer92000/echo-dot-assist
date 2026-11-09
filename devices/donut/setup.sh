# Guided installation for the Echo Dot 3 (donut), run by scripts/setup.sh (bash, from the repository root, with
# device.conf loaded and the helpers of scripts/lib/setup.sh).  The same steps, written out: README.md next to this file.
# Each step_<id> returns non-zero when it did not finish; the runner then offers retry, skip or quit.  A step checks
# first whether its result is there already, so a rerun (or work done by hand) passes through quietly.

XDA=https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-dot-3rd-gen-2018-donut.4801400/
LOCKED_ISSUE=https://github.com/Gamer92000/echo-dot-assist/issues/112
FTVDB=https://ftvdb.com/echo/firmware/com.amazon.donut.android.os/f7cd5f3b6d6bfa59e5ba6a6779285c34-13222529668-fire-os-6574-1-ns65741-8138-2026-08-31/
# a replug alone may start Fire OS, not TWRP (PR #142): Volume up held while the power goes in starts the recovery
TWRP_HINT="Nothing yet? Unplug the Echo's power, then hold only Volume up (+) while plugging it back in: that starts TWRP."
NDK=toolchain/android-ndk-r21e
NDK_URL=https://dl.google.com/android/repository/android-ndk-r21e-linux-x86_64.zip

STEPS="
tools|Tools on this PC
files|Downloads
udev|USB access for the unlock
usb|USB cable to the Echo
unlock|Unlock, boot TWRP
flash|Stock firmware into both slots
root|Root
build|Build
network|Lock down, join Wi-Fi
install|Install
"
ROOTED_SKIP="udev usb unlock flash root"     # done on an Echo that is rooted already (scripts/setup.sh asks)
NOTES=(
    "The USB wires can come off. adb over Wi-Fi is closed: scripts/adb-wifi.sh <echo-ip> (or debug access on the settings page, http://<echo-ip>:28931/) opens it for 30 min."
    "Updates: git pull, then scripts/ota-push.sh <echo-ip>."
    "More wake words, whisper or sound detection models: the settings page, http://<echo-ip>:28931/ → \"Download from Amazon\"."
    "Music Assistant, Bluetooth speaker: $DDIR/README.md"
)

slot() { ashell bcbtool get_active | tr -d " \n"; }

step_tools() {
    command -v curl > /dev/null || need_tools curl || return 1
    build_mode || return 1
    # python3 for otatool.py; what the unlock needs on this PC is the XDA thread's to say (step_unlock)
    if [ "$BUILD_MODE" = prebuilt ]; then need_tools adb python3 unzip curl sha256sum || return 1; return 0; fi
    need_tools adb python3 make cc unzip debugfs curl sha256sum bc xz || return 1
    space_ahead 500
}

step_files() {
    local need=()
    # a rooted Echo needs the firmware only to build against.  The unlock's files are not asked for here: the XDA
    # thread says which (step_unlock)
    rooted_already && { [ "$BUILD_MODE" = prebuilt ] || [ -d $FW/rootfs/system/lib ]; } ||
        need+=($FW/$FIRMWARE_FILE $FIRMWARE_SHA256 "Fire OS $FIRMWARE_ID: $FTVDB")
    need_files "${need[@]}" || return 1
    [ "$BUILD_MODE" = prebuilt ] && return 0          # nothing to build with
    if [ -x $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang ]; then ok "Android NDK r21e"; else
        mkdir -p toolchain
        need_space $NDK_MB "The Android NDK" || return 1
        TASK_NOTE="du -h toolchain/ndk.zip | cut -f1" task "Downloading Android NDK r21e (1 GB)" curl -fsSL -o toolchain/ndk.zip $NDK_URL &&
            task "Unpacking the NDK" unzip -q -o toolchain/ndk.zip -d toolchain && rm -f toolchain/ndk.zip || return 1
    fi
    kernel_tools
}

# lets the unlock reach the Echo without sudo, and keeps ModemManager from probing the bootrom's serial port mid-handshake
step_udev() { udev_rules; }      # scripts/lib/setup.sh

step_usb() {
    [ -z "$DRY" ] && { adb_is recovery || adb_is device; } && { ok "the Echo is on USB already"; return 0; }
    todo "Solder a USB cable to the Echo's test pads" \
        "Opening the case: the guides under \"Requirements\" in $XDA" \
        "" \
        "black  GND  → TM3 (or any ground)" \
        "white  D-   → TM4" \
        "green  D+   → TM5" \
        "${RED}${B}red    VCC  → nothing: leave it loose and insulate its end$N" \
        "" \
        "A cable with a USB-A plug for the PC. A USB-C plug there needs a 5.1 kΩ resistor from its CC wire" \
        "(often yellow) to ground, or the PC sees nothing." \
        "" \
        "Leave the Echo unplugged for now (no power, no USB). The next step says when to plug in which."
}

step_unlock() {
    twrp_or_unlocked "$TWRP_HINT"; case $? in 0) return 0;; 2) return 1;; esac
    [ -n "$WINDOWS" ] && { unlock_elsewhere; [ -n "$DRY" ]; return; }       # Windows: on adb at the start (setup.sh), not now
    # issue #112 (2026-11-04/05): on two Echos kamakiri ran through without an error, but kaeru never stayed in expdb
    # (its write "did not read back", the partition zeros after) and the Echo came back in locked fastboot (green ring,
    # "restricted on locked hw"), with no way on from there found yet.  One had worked until it was opened, so a dying
    # eMMC does not explain it alone; Echos Amazon updated recently look at risk, how far it reaches is not known.
    # Asked once a run, before the thread; a dry run goes on.
    if [ -z "$UNLOCK_RISK_OK" ]; then
        warn "${B}Warning: the unlock can leave this Echo unusable (issue #112).$N"
        warn "${B}Echos that were updated recently may become unusable. The full extent is currently unknown.$N"
        info "  On two Echos kamakiri ran through without an error, yet the Echo came back locked (a green ring, no"
        info "  rainbow), and no way to finish the unlock or undo it has been found: neither has worked since."
        info "  It is not known yet which Echos or firmware versions are affected, nor whether any unlock is safe now."
        info "  Read $LOCKED_ISSUE first. While it is open, do not unlock an Echo you want to keep."
        ask "Unlock this Echo anyway?" n || [ -n "$DRY" ] ||
            { info "Nothing was written to the Echo. Quit (q) and run scripts/setup.sh $DEVICE later: progress is kept."; return 1; }
        UNLOCK_RISK_OK=1
    fi
    unlock_thread kamakiri-donut "TWRP is running on the Echo (white ring)" \
        "A green ring, and fastboot saying \"restricted on locked hw\"? That is issue #112: read $LOCKED_ISSUE first." || return 1
    [ -n "$DRY" ] || twrp_or_unlocked "$TWRP_HINT"
}

step_flash() {
    local s0 s1 s2
    wait_adb recovery "$TWRP_HINT" || return 1
    twrp_check || return 1
    task "Wiping cache and data" adb shell 'twrp wipe cache && twrp wipe data' || return 1
    task "Copying the firmware to the Echo" adb push $FW/$FIRMWARE_FILE /sdcard/update.zip || return 1
    [ -n "$DRY" ] || s0=$(slot)
    task "Flashing the first slot" adb shell twrp install /sdcard/update.zip || return 1
    # TWRP makes the slot it flashed the active one (checked 2026-09-25).  Switching back by hand would put the second
    # install into the same slot, leaving Amazon's newer firmware in the other.
    [ -n "$DRY" ] || { s1=$(slot); [ "$s1" != "$s0" ] || { fail "active slot is still $s0 after the install; stop here ($DDIR/README.md, step 1)"; return 1; }; }
    task "Restarting TWRP" adb reboot recovery || return 1
    [ -n "$DRY" ] || sleep 5
    wait_adb recovery "$TWRP_HINT" || return 1
    task "Flashing the second slot" adb shell twrp install /sdcard/update.zip || return 1
    [ -n "$DRY" ] || { s2=$(slot); [ "$s2" != "$s1" ] || { fail "the second install went into slot $s1 again"; return 1; }; }
    task "Restarting TWRP" adb reboot recovery
}

step_root() {
    root_thread $XDA "$TWRP_HINT" || return 1
    # online with Amazon it would update itself, and an update can close the hole the unlock uses
    warn "Do not set the Echo up with the Alexa app."
}

step_build() {
    # the firmware's libraries are only needed to link against
    if [ "$BUILD_MODE" != prebuilt ] && [ ! -d $FW/rootfs/system/lib ]; then
        # rdump does not create its target, and debugfs exits 0 whatever happens: the result is checked by hand (issue #4)
        task "Unpacking the firmware" sh -c "unzip -o -q $FW/$FIRMWARE_FILE payload.bin -d $FW &&
            python3 tools/payload_dump.py $FW/payload.bin $FW/images && mkdir -p $FW/rootfs &&
            debugfs -R 'rdump / $FW/rootfs' $FW/images/system.img && [ -d $FW/rootfs/system/lib ]" || return 1
    fi
    magiskpolicy_get || return 1
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
    magiskpolicy_get || return 1        # again: the build step may have been skipped
    install_satellite
}
