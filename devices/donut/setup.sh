# Guided installation for the Echo Dot 3 (donut), run by scripts/setup.sh (bash, from the repository root, with
# device.conf loaded and the helpers of scripts/lib/setup.sh).  The same steps, written out: README.md next to this file.
# Each step_<id> returns non-zero when it did not finish; the runner then offers retry, skip or quit.  A step checks
# first whether its result is there already, so a rerun (or work done by hand) passes through quietly.

XDA=https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-dot-3rd-gen-2018-donut.4801400/
FTVDB=https://ftvdb.com/echo/firmware/com.amazon.donut.android.os/f7cd5f3b6d6bfa59e5ba6a6779285c34-13222529668-fire-os-6574-1-ns65741-8138-2026-08-31/
KAMAKIRI=kamakiri-donut-v1.0.0.zip
KAMAKIRI_SHA256=4d2bb52eaf661f6616aa4268584d44cf9155c1328c34bfcba01a6558f942b738
BOOTROOT=boot-root.zip
BOOTROOT_SHA256=de49cc88b27a8e77cf97cf0156bee50e4ddc0e116c41aaede06b494e38397be0
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
wakeword|Wake word, whisper and sound detection models
"
NOTES=(
    "The USB wires can come off. adb over Wi-Fi is closed: scripts/adb-wifi.sh <echo-ip> (or debug access on the settings page, http://<echo-ip>:28931/) opens it for 30 min."
    "Updates: git pull, then scripts/ota-push.sh <echo-ip>."
    "More wake words, whisper or sound detection models later: scripts/artifacts.sh <echo-ip>"
    "Music Assistant, Bluetooth speaker: $DDIR/README.md"
)

slot() { ashell bcbtool get_active | tr -d " \n"; }

step_tools() {
    command -v curl > /dev/null || need_tools curl || return 1
    build_mode
    if [ "$BUILD_MODE" = prebuilt ]; then need_tools adb fastboot python3 pyusb unzip sqlite3 curl sha256sum || return 1; return 0; fi
    need_tools adb fastboot python3 pyusb make cc unzip debugfs sqlite3 curl sha256sum bc xz || return 1
    [ "$(df -Pk . | awk 'NR == 2 { print $4 }')" -gt 5000000 ] || warn "less than 5 GB free here; the NDK and firmware need about that"
}

step_files() {
    need_files $FW/$FIRMWARE_FILE $FIRMWARE_SHA256 "Fire OS $FIRMWARE_ID: $FTVDB" \
               $FW/$KAMAKIRI $KAMAKIRI_SHA256 "attachment in $XDA" \
               $FW/$BOOTROOT $BOOTROOT_SHA256 "attachment in $XDA" || return 1
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
    todo "Solder a USB cable to the Echo's test pads" \
        "Opening the case: the guides under \"Requirements\" in $XDA" \
        "" \
        "black  GND  → TM3 (or any ground)" \
        "white  D-   → TM4" \
        "green  D+   → TM5" \
        "${RED}${B}red    VCC  → nothing: leave it loose and insulate its end$N" \
        "" \
        "Leave the Echo unplugged for now (no power, no USB)."
}

step_unlock() {
    [ -z "$DRY" ] && adb_is recovery && { ok "TWRP is running already"; return 0; }
    # Fire OS on adb: unlocked before (stock has no adb), and the unlock left TWRP on the recovery partition
    if [ -z "$DRY" ] && adb_is device; then
        ok "unlocked already"
        task "Restarting into TWRP" adb reboot recovery || return 1
        wait_adb recovery; return
    fi
    local kdir
    [ -d $FW/kamakiri ] || task "Unpacking kamakiri" unzip -q $FW/$KAMAKIRI -d $FW/kamakiri || return 1
    # the zip may keep its scripts in a folder of their own: run them from wherever bootrom-step.sh landed
    kdir=$(find $FW/kamakiri -name bootrom-step.sh -print -quit 2>/dev/null); kdir=${kdir%/*}
    [ -n "$kdir" ] || [ -n "$DRY" ] || { fail "no bootrom-step.sh in $FW/kamakiri; delete that folder and try again"; return 1; }
    tell "Hold the action button (•) and plug the USB cable in" \
        "Keep holding until it asks you to press Enter, then release the button and press Enter." \
        "The ring stays dark until then: the Echo is in its bootrom."
    # in the foreground: after the handshake kamakiri waits for Enter while kicking the watchdog, and again if the rpmb
    # looks broken (a safety stop that a person answers, so no `yes` here).  Without a terminal input() hit EOF and the
    # wait never ended, the Echo dark in bootrom (issue #4).  tee keeps the output in the log; unbuffered, or the prompt
    # would sit in Python's pipe buffer.
    live "Unlocking with kamakiri" in_dir "$kdir" env PYTHONUNBUFFERED=1 \
        bash -o pipefail -c "./bootrom-step.sh 2>&1 | tee -a '$LOG'" || return 1
    waitfor "Waiting for hacked fastboot (rainbow ring)|Hacked fastboot" '[ -n "$(fastboot devices)" ]' \
        "No rainbow? Unplug everything and try this step again." || return 1
    task "Flashing TWRP" in_dir "$kdir" ./fastboot-step.sh || return 1
    wait_adb recovery
}

step_flash() {
    local s0 s1 s2
    wait_adb recovery || return 1
    task "Wiping cache and data" adb shell 'twrp wipe cache && twrp wipe data' || return 1
    task "Copying the firmware to the Echo" adb push $FW/$FIRMWARE_FILE /sdcard/update.zip || return 1
    [ -n "$DRY" ] || s0=$(slot)
    task "Flashing the first slot" adb shell twrp install /sdcard/update.zip || return 1
    # TWRP makes the slot it flashed the active one (checked 2026-09-25).  Switching back by hand would put the second
    # install into the same slot, leaving Amazon's newer firmware in the other.
    [ -n "$DRY" ] || { s1=$(slot); [ "$s1" != "$s0" ] || { fail "active slot is still $s0 after the install; stop here ($DDIR/README.md, step 1)"; return 1; }; }
    task "Restarting TWRP" adb reboot recovery || return 1
    [ -n "$DRY" ] || sleep 5
    wait_adb recovery || return 1
    task "Flashing the second slot" adb shell twrp install /sdcard/update.zip || return 1
    [ -n "$DRY" ] || { s2=$(slot); [ "$s2" != "$s1" ] || { fail "the second install went into slot $s1 again"; return 1; }; }
    task "Restarting TWRP" adb reboot recovery
}

step_root() {
    [ -z "$DRY" ] && adb_is device && [ "$(ashell id -u)" = 0 ] && { ok "root adb already"; return 0; }
    wait_adb recovery || return 1
    task "Installing boot-root" sh -c "adb push $FW/$BOOTROOT /sdcard/ && adb shell twrp install /sdcard/$BOOTROOT" || return 1
    task "Rebooting into Fire OS" adb reboot || return 1
    [ -n "$DRY" ] || sleep 5
    wait_adb device || return 1
    [ -n "$DRY" ] || [ "$(ashell id -u)" = 0 ] || { fail "adb shell is not root"; return 1; }
    ok "root adb"
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
    [ -d $FW/boot-root ] || task "Unpacking boot-root" unzip -q $FW/$BOOTROOT -d $FW/boot-root || return 1
    if [ "$BUILD_MODE" = prebuilt ]; then task "Downloading the release build of this commit" build_binaries || return 1
    else task "Building" build_binaries || return 1; fi
    wait_adb device || return 1
    task "Checking the Echo's firmware" sh -c 'out=$(scripts/probe.sh) && echo "$out" && ! echo "$out" | grep -q DIFFERENT' ||
        { fail "the Echo runs another firmware than $FIRMWARE_ID: redo the firmware step"; return 1; }
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
    # the Wi-Fi profile survives a reboot, the egress lock does not until the install
    warn "Do not restart the Echo until it is installed."
    info "Tip: give it a fixed address in your router (DHCP reservation)."
}

step_install() {
    install_satellite
}

step_wakeword() {
    artifacts_run setup
}
