# Guided installation for the Echo Dot 2 (biscuit), run by scripts/setup.sh (bash, from the repository root, with
# device.conf loaded and the helpers of scripts/lib/setup.sh).  The same steps, written out: README.md next to this file.
# Each step_<id> returns non-zero when it did not finish; the runner then offers retry, skip or quit.  A step checks
# first whether its result is there already, so a rerun (or work done by hand) passes through quietly.
# Tried on a real Echo Dot 2 (2026-09-28).  The unlock follows R0rt1z2's XDA thread, whose instructions are
# authoritative where this script simplifies.

XDA=https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-dot-2nd-gen-2016-biscuit.4761416/
FTVDB=https://ftvdb.com/echo/firmware/com.amazon.biscuit.android.os/8ca06ee4ef2806c974d2944b7fadd543-13222531716-fire-os-6574-1-ns65741-8146-2026-09-17/
AMONET=amonet-biscuit-v2.0.0.zip
AMONET_SHA256=98297293701082bc7272efe077f941c56fc7b6e1f27ef6f2e93b6e4c6fc7b62d
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
NOTES=(
    "adb over Wi-Fi is closed: scripts/adb-wifi.sh <echo-ip> (or debug access on the settings page, http://<echo-ip>:28931/) opens it for 30 min."
    "Updates: git pull, then scripts/ota-push.sh <echo-ip>."
    "More wake words, whisper or sound detection models: the settings page, http://<echo-ip>:28931/ → \"Download from Amazon\"."
    "Music Assistant, Bluetooth speaker: $DDIR/README.md"
)

slot() { ashell bcbtool get_active | tr -d " \n"; }

step_tools() {
    command -v curl > /dev/null || need_tools curl || return 1
    build_mode
    local unlock="fastboot python3 pyusb"
    rooted_already && unlock=python3
    if [ "$BUILD_MODE" = prebuilt ]; then need_tools adb $unlock unzip sqlite3 curl sha256sum || return 1; return 0; fi
    need_tools adb $unlock make cc unzip 7z sqlite3 curl sha256sum bc xz || return 1
    [ "$(df -Pk . | awk 'NR == 2 { print $4 }')" -gt 5000000 ] || warn "less than 5 GB free here; the NDK and firmware need about that"
}

step_files() {
    local need=()
    # a rooted Echo needs the firmware only to build against, and the unlock tool not at all
    rooted_already && { [ "$BUILD_MODE" = prebuilt ] || [ -d $FW/rootfs/system/lib ]; } ||
        need+=($FW/$FIRMWARE_FILE $FIRMWARE_SHA256 "Fire OS $FIRMWARE_ID: $FTVDB")
    rooted_already || need+=($FW/$AMONET $AMONET_SHA256 "attachment in $XDA")
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
    [ -z "$DRY" ] && adb_is recovery && { ok "TWRP is running already"; return 0; }
    # Fire OS on adb: unlocked before (stock has no adb), and the unlock left TWRP on the recovery partition
    if [ -z "$DRY" ] && adb_is device; then
        ok "unlocked already"
        task "Restarting into TWRP" adb reboot recovery || return 1
        wait_adb recovery; return
    fi
    local adir
    [ -d $FW/amonet ] || task "Unpacking amonet" unzip -q $FW/$AMONET -d $FW/amonet || return 1
    # run fastbrick.sh from wherever the zip put it (the release zip keeps it in a folder amonet/ of its own)
    adir=$(find $FW/amonet -name fastbrick.sh -print -quit 2>/dev/null); adir=${adir%/*}
    [ -n "$adir" ] || [ -n "$DRY" ] || { fail "no fastbrick.sh in $FW/amonet; delete that folder and try again"; return 1; }
    tell "Put the Echo into fastboot mode" "Hold the action button (•) and plug the power in." \
        "Keep holding until the ring shows a green light."
    waitfor "Waiting for fastboot|Fastboot" '[ -n "$(fastboot devices)" ]' "No green light? Unplug the power and try again." || return 1
    tell "amonet asks you to type YES, and for a key press at the end" "${RED}${B}Do not interrupt it once it runs: that can brick the Echo.$N"
    live "amonet fastbrick (about a minute)" in_dir "$adir" ./fastbrick.sh || return 1
    wait_adb recovery
}

step_flash() {
    local s0 s1
    wait_adb recovery || return 1
    task "Wiping cache and data" adb shell 'twrp wipe cache && twrp wipe data' || return 1
    task "Copying the firmware to the Echo" adb push $FW/$FIRMWARE_FILE /sdcard/update.zip || return 1
    [ -n "$DRY" ] || s0=$(slot)
    task "Flashing the first slot" adb shell twrp install /sdcard/update.zip || return 1
    # the thread's own step: switch back to the unused slot, so the second install lands there and the FIRST slot ends
    # up active again (checked 2026-09-28 on biscuit)
    task "Switching slots" adb shell 's=$(bcbtool get_active); case $s in a) bcbtool set_active b;; b) bcbtool set_active a;; *) echo "unexpected: $s"; exit 1;; esac' || return 1
    [ -n "$DRY" ] || { s1=$(slot); [ "$s1" = "$s0" ] || { fail "slot switch did not take (active: $s1, before: $s0)"; return 1; }; }
    task "Restarting TWRP" adb reboot recovery || return 1
    [ -n "$DRY" ] || sleep 5
    wait_adb recovery || return 1
    task "Flashing the second slot" adb shell twrp install /sdcard/update.zip
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
            python3 tools/payload_dump.py $FW/payload.bin $FW/images && 7z x -o$FW/rootfs -y $FW/images/system.img" || return 1
    fi
    bootroot_unpack || return 1
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
    bootroot_unpack || return 1        # again: the build step may have been skipped
    install_satellite
}
