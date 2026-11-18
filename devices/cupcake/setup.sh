# Guided installation for the Echo Input (cupcake), run by scripts/setup.sh (bash, from the repository root, with
# device.conf loaded and the helpers of scripts/lib/setup.sh).  The same steps, written out: README.md next to this file.
# donut's steps (the unlock by hand as the XDA thread says, kamakiri and TWRP, the firmware into both A/B slots, donut's
# boot-root, the install from the running OS), without the soldering: the Echo Input's micro-USB socket carries data,
# and powers it.
# Each step_<id> returns non-zero when it did not finish; the runner then offers retry, skip or quit.  A step checks
# first whether its result is there already, so a rerun (or work done by hand) passes through quietly.

XDA=https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-input-2018-cupcake.4797603/
XDA_DONUT=https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-dot-3rd-gen-2018-donut.4801400/
FTVDB=https://ftvdb.com/echo/firmware/com.amazon.cupcake.android.os/675ff746519090e1cbd2366a92132d55-13323244164-fire-os-6-5-7-5-ns6575-8338-2026-10-14/
NDK=toolchain/android-ndk-r21e
NDK_URL=https://dl.google.com/android/repository/android-ndk-r21e-linux-x86_64.zip

STEPS="
tools|Tools on this PC
files|Downloads
udev|USB access for the unlock
unlock|Unlock, boot TWRP
flash|Stock firmware into both slots
root|Root
build|Build
network|Lock down, join Wi-Fi
install|Install
"
ROOTED_SKIP="udev unlock flash root"        # done on an Echo that is rooted already (scripts/setup.sh asks)
NOTES=(
    "The USB cable may go back to a power adapter. adb over Wi-Fi is closed: scripts/adb-wifi.sh <echo-ip> (or debug access on the settings page, http://<echo-ip>:28931/) opens it for 30 min."
    "Updates: git pull, then scripts/ota-push.sh <echo-ip>."
    "More wake words, whisper or sound detection models: the settings page, http://<echo-ip>:28931/ → \"Download from Amazon\"."
    "No speaker on this model: sound comes from its 3.5 mm jack, or from a Bluetooth speaker ($DDIR/README.md)."
)

slot() { ashell bcbtool get_active | tr -d " \n"; }

step_tools() {
    command -v curl > /dev/null || need_tools curl || return 1
    build_mode || return 1
    # python3 for otatool.py; what the unlock needs on this PC is the XDA thread's to say (step_unlock)
    if [ "$BUILD_MODE" = prebuilt ]; then need_tools adb python3 unzip curl sha256sum || return 1; return 0; fi
    need_tools adb python3 make cc unzip debugfs curl sha256sum bc xz || return 1
    space_ahead 3000
}

step_files() {
    local need=()
    rooted_already && { [ "$BUILD_MODE" = prebuilt ] || [ -d $FW/rootfs/system/lib ]; } ||
        need+=($FW/$FIRMWARE_FILE $FIRMWARE_SHA256 "Fire OS $FIRMWARE_ID: $FTVDB")
    # the unlock's files are not asked for here: the XDA thread says which (step_unlock)
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

# the Echo Input runs from its micro-USB socket: from the unlock on the PC's port powers it (USB 2: 0.5 A, less than its
# own adapter), which may restart it or drop adb in the later steps
step_unlock() {
    twrp_or_unlocked; case $? in 0) return 0;; 2) return 1;; esac
    [ -n "$WINDOWS" ] && { unlock_elsewhere; [ -n "$DRY" ]; return; }       # Windows: on adb at the start (setup.sh), not now
    unlock_thread kamakiri-cupcake "TWRP is running on the Echo" \
        "Restarts, or adb dropping out, with the Echo powered from this PC's USB port? A powered USB hub between them." || return 1
    [ -n "$DRY" ] || twrp_or_unlocked
}

step_flash() {
    local s0 s1 s2
    wait_adb recovery || return 1
    twrp_check || return 1
    task "Wiping cache and data" adb shell 'twrp wipe cache && twrp wipe data' || return 1
    task "Copying the firmware to the Echo" adb push $FW/$FIRMWARE_FILE /sdcard/update.zip || return 1
    [ -n "$DRY" ] || s0=$(slot)
    task "Flashing the first slot" adb shell twrp install /sdcard/update.zip || return 1
    # TWRP makes the slot it flashed the active one (donut, 2026-09-25: the same TWRP device tree, echo-mt8167)
    [ -n "$DRY" ] || { s1=$(slot); [ "$s1" != "$s0" ] || { fail "active slot is still $s0 after the install; stop here ($DDIR/README.md, step 1)"; return 1; }; }
    task "Restarting TWRP" adb reboot recovery || return 1
    [ -n "$DRY" ] || sleep 5
    wait_adb recovery || return 1
    task "Flashing the second slot" adb shell twrp install /sdcard/update.zip || return 1
    [ -n "$DRY" ] || { s2=$(slot); [ "$s2" != "$s1" ] || { fail "the second install went into slot $s1 again"; return 1; }; }
    task "Restarting TWRP" adb reboot recovery
}

step_root() {
    # its system-as-root path fits this firmware (docs/re-cupcake.md)
    { [ -n "$DRY" ] || ! root_adb; } && info "The Echo Input takes the Echo Dot 3's boot-root.zip: that is the thread below."
    root_thread $XDA_DONUT "" || return 1
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
    network_lock || return 1
    # before the Echo is online: Alexa's apps and Amazon's setup off (noted for alexa-on.sh), Android told setup is done;
    # the install's satellite start runs it again and finds nothing left to do
    task "Switching off Alexa's apps and Amazon's setup" sh -c 'adb shell sh /data/local/hassmic/alexa-off.sh;
        [ "$(adb shell settings get global device_provisioned | tr -d "\r")" = 1 ]' || return 1
    network_wifi
}

step_install() {
    magiskpolicy_get || return 1        # again: the build step may have been skipped
    install_satellite
}
