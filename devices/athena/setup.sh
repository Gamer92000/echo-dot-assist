# Guided installation for the Echo Show 8 2nd gen (athena), run by scripts/setup.sh (bash, from the repository root, with
# device.conf loaded and the helpers of scripts/lib/setup.sh).  The same steps, written out: README.md next to this file.
# Each step_<id> returns non-zero when it did not finish; the runner then offers retry, skip or quit.  A step checks first
# whether its result is there already, so a rerun (or work done by hand) passes through quietly.
# What is different from crown: the unlock is saiyaku-athena's serial exploit (its meta-step.sh, run from here with its
# output on the console), TWRP then flashes boot-root.zip, and the install is written from the running OS (INSTALL=running).

XDA=https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-show-8-2nd-gen-2021-athena.4804958/
SAIYAKU=saiyaku-athena-v1.0.0.zip
SAIYAKU_SHA256=6d48b4b253fa6cbecb9166c87c0177be09c8a02e5f2efd2bc0d0aec939626218
BOOTROOT=boot-root.zip                  # in $FW, as bootroot_unpack takes it
BOOTROOT_SHA256=741d904f7e2616c70be2c10f3466efb515769632779546a869f5591a6a9c827a
NDK=toolchain/android-ndk-r21e
NDK_URL=https://dl.google.com/android/repository/android-ndk-r21e-linux-x86_64.zip

STEPS="
tools|Tools on this PC
files|Downloads
usb|Power and USB to the Echo
unlock|Unlock the bootloader
root|Root (TWRP)
build|Build
network|Lock down, switch Alexa off
install|Install
"
ROOTED_SKIP="usb unlock root"           # an Echo rooted already: scripts/setup.sh asks
NOTES=(
    "The screen is black with the light band along its bottom edge; Alexa's apps stay off while hassmic runs ($DDIR/README.md)."
    "adb over Wi-Fi: the settings page's debug access (http://<echo-ip>:28931/) opens it for 30 min, as on the other Echos."
    "Updates: git pull, then scripts/ota-push.sh <echo-ip>."
    "\"Download from Amazon\" on the settings page works here too (code pair login, as on crown): wake words and the newer sound detection model."
)

# The firmware unpacked, whatever the build mode: the build links against its libraries.  The .bin is a block OTA with
# brotli-compressed system and vendor (hence brotli); debugfs exits 0 whatever happens and rdump does not create its
# target (issue #4): the result is checked by hand.  /system as rdump gives it; vendor goes to /system/vendor, where the
# probe files and the build look for it.
rootfs() {
    [ -e $FW/rootfs/system/bin/linker ] && [ -e $FW/rootfs/system/vendor/lib/libasp.so ] && return 0
    task "Unpacking the firmware (1 GB, a few minutes)" sh -c "set -e; mkdir -p $FW/ota $FW/images $FW/rootfs
        unzip -o -q $FW/$FIRMWARE_FILE system.new.dat.br system.transfer.list vendor.new.dat.br vendor.transfer.list boot.img -d $FW/ota
        for p in system vendor; do
            brotli -d -f $FW/ota/\$p.new.dat.br -o $FW/ota/\$p.new.dat
            python3 tools/sdat2img.py $FW/ota/\$p.transfer.list $FW/ota/\$p.new.dat $FW/images/\$p.img
        done
        mv $FW/ota/boot.img $FW/images/boot.img; rm -rf $FW/ota
        debugfs -R 'rdump /system $FW/rootfs' $FW/images/system.img > /dev/null 2>&1 || true
        rm -f $FW/rootfs/system/vendor; mkdir -p $FW/rootfs/system/vendor
        debugfs -R 'rdump / $FW/rootfs/system/vendor' $FW/images/vendor.img > /dev/null 2>&1 || true
        [ -e $FW/rootfs/system/bin/linker ] && [ -e $FW/rootfs/system/vendor/lib/libasp.so ]"
}

step_tools() {
    command -v curl > /dev/null || need_tools curl || return 1
    build_mode
    local unlock=; rooted_already || unlock="pyserial"          # saiyaku talks to the Echo's serial port
    if [ "$BUILD_MODE" = prebuilt ]; then need_tools adb unzip brotli python3 debugfs curl sha256sum $unlock || return 1; return 0; fi
    need_tools adb unzip brotli python3 make cc debugfs curl sha256sum bc xz $unlock || return 1
    [ "$(df -Pk . | awk 'NR == 2 { print $4 }')" -gt 8000000 ] || warn "less than 8 GB free here; the NDK, firmware and images need about that"
}

step_files() {
    local need=()
    need+=($FW/$FIRMWARE_FILE $FIRMWARE_SHA256 "Fire OS $FIRMWARE_ID, from Amazon: the link is in $DDIR/README.md")
    # the unlock and the root zip are the XDA thread's attachments; the root step flashes the zip, and the install takes the
    # policy tool out of it (device.conf SEPOLICY_TOOL): not needed only on an Echo rooted already with the tool unpacked
    rooted_already || need+=($FW/$SAIYAKU $SAIYAKU_SHA256 "the unlocker, attachment in $XDA")
    { rooted_already && [ -f $SEPOLICY_TOOL ]; } || need+=($FW/$BOOTROOT $BOOTROOT_SHA256 "the root zip, attachment in $XDA (it also holds the policy tool)")
    need_files "${need[@]}" || return 1
    [ "$BUILD_MODE" = prebuilt ] && return 0          # nothing to build with
    if [ -x $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang ]; then ok "Android NDK r21e"; else
        mkdir -p toolchain
        TASK_NOTE="du -h toolchain/ndk.zip | cut -f1" task "Downloading Android NDK r21e (1 GB)" curl -fsSL -o toolchain/ndk.zip $NDK_URL &&
            task "Unpacking the NDK" unzip -q -o toolchain/ndk.zip -d toolchain && rm -f toolchain/ndk.zip || return 1
    fi
    kernel_tools
}

step_usb() {
    [ -z "$DRY" ] && { adb_is recovery || adb_is device || usb_present; } && { ok "the Echo is on USB"; return 0; }
    todo "Sit the Echo near this PC, with its power supply and a micro-USB cable" \
        "" \
        "Nothing has to be opened: this model unlocks through its USB port (a serial exploit)." \
        "Do not set it up with the Alexa app or its touchscreen."
}

# a stock Echo Show 8 2nd gen on USB: Amazon's vendor id 1949, "Lab126, Inc. Alexa" (no adb on stock)
usb_present() { lsusb 2> /dev/null | grep -q -i -e "1949:" -e "lab126"; }

# TWRP's first screen asks "Keep System Read only?" at every start until told otherwise: nothing is written from TWRP
# but the boot-root zip, which patches the system itself (it does not need the system writable)
twrp_screen() {
    tell "On the Echo's screen TWRP may ask \"Keep System Read only?\"" \
        "Swipe to allow modifications only if a flash below says the system is read-only; else leave it."
}

step_unlock() {
    [ -z "$DRY" ] && adb_is recovery && { ok "TWRP is running already"; return 0; }
    [ -z "$DRY" ] && adb_is device && { ok "unlocked already (stock has no adb)"; return 0; }
    [ -x $FW/saiyaku-athena/meta-step.sh ] || task "Unpacking saiyaku-athena" unzip -q -o $FW/$SAIYAKU -d $FW || return 1
    [ -n "$DRY" ] || [ -d $FW/saiyaku-athena ] || { fail "$FW/$SAIYAKU holds no saiyaku-athena folder"; return 1; }
    tell "Start the unlocker first, then power the Echo up" \
        "1. Keep the Echo's micro-USB cable plugged into this PC, but the power supply out." \
        "2. The unlocker starts now and asks you to read its disclaimer and type YES." \
        "3. When it says it is waiting for the device, plug in the power supply and let the Echo boot normally." \
        "   It then runs by itself: this takes minutes and several attempts. ${RED}${B}Do not disconnect anything.$N" \
        "4. It ends with the Echo rebooting into TWRP."
    # in the foreground: it asks for YES; its output is the console's (and the log's).  bash: it may need the serial port
    # (group uucp/dialout), and says so
    live "Unlocking with saiyaku-athena" in_dir $FW/saiyaku-athena env PYTHONUNBUFFERED=1 \
        bash -o pipefail -c "./meta-step.sh 2>&1 | tee -a '$LOG'" || {
        fail "the unlock did not finish. 'Permission denied' on the serial port: add yourself to the group that owns it (uucp or dialout) or run it with sudo ($DDIR/unlock.md)"
        return 1; }
    twrp_screen
    wait_adb recovery || { fail "no TWRP on adb; $DDIR/unlock.md, then run this again"; return 1; }
}

step_root() {
    [ -z "$DRY" ] && adb_is device && [ "$(ashell id -u)" = 0 ] && { ok "root adb already"; return 0; }
    if [ -z "$DRY" ] && ! adb_is recovery; then
        if adb_is device; then task "Restarting into TWRP" adb reboot recovery || return 1; fi
        twrp_screen
    fi
    wait_adb recovery || return 1
    tell "Flashing boot-root.zip in TWRP" "It patches system, vendor and adbd in place (root adb, OTA blocked, the policy opened)."
    if [ -z "$DRY" ]; then
        task "Copying boot-root.zip to the Echo" adb push $FW/$BOOTROOT /tmp/boot-root.zip || return 1
        live "Installing boot-root.zip" bash -o pipefail -c "adb shell twrp install /tmp/boot-root.zip 2>&1 | tee -a '$LOG'" || return 1
        adb shell rm -f /tmp/boot-root.zip > /dev/null 2>&1
    fi
    task "Restarting into the system" adb reboot || return 1
    wait_adb device || return 1
    [ -z "$DRY" ] && [ "$(ashell id -u)" != 0 ] && { fail "adb is not root after the flash: run this step again"; return 1; }
    ok "root adb"
    warn "Do not set the Echo up with the Alexa app or its touchscreen."
}

step_build() {
    rootfs || return 1                  # the libraries to link against
    if [ "$BUILD_MODE" = prebuilt ]; then task "Downloading the release build of this commit" build_binaries || return 1
    else task "Building" build_binaries || return 1; fi
    wait_adb device || return 1
    task "Checking the Echo's firmware" sh -c 'out=$(scripts/probe.sh) && echo "$out" && ! echo "$out" | grep -q DIFFERENT' ||
        { fail "the Echo runs another firmware than $FIRMWARE_ID: $DDIR/README.md"; return 1; }
}

step_network() {
    local ssid pass new=
    wait_adb device || return 1
    bootroot_unpack || return 1         # the policy tool (device.conf SEPOLICY_TOOL), for the install
    task "Copying hassmic to the Echo" scripts/deploy.sh || return 1
    wait_adb device || return 1
    # Stock opens adb over Wi-Fi at boot (init.athena.rc); lockdown.sh closes it by restarting adbd, which ends the adb
    # shell that runs it: closed here first, and adb waited for (as crown)
    if [ -z "$DRY" ] && [ "$(ashell getprop service.adb.tcp.port)" = 5555 ]; then
        info "closing adb over Wi-Fi (stock opens it at boot); adb restarts"
        adb shell 'setprop service.adb.tcp.port 0; setprop ctl.restart adbd' > /dev/null 2>&1; sleep 3
        wait_adb device || return 1
    fi
    task "Locking down the Echo's internet access" sh -c 'adb shell sh /data/local/hassmic/lockdown.sh &&
        adb shell iptables -S hassmic_out | grep -q -- "-j DROP"' || return 1
    task "Switching off Alexa's apps" sh -c 'adb shell sh /data/local/hassmic/alexa-off.sh' || return 1
    # An Echo that is online already (the unlock leaves its network saved) keeps it; else as crown: through WifiService
    # (wifictl.dex, as the system user: device.conf WIFICTL_USER)
    if [ -z "$DRY" ] && adb shell ifconfig wlan0 2>/dev/null | grep -q "inet addr:"; then
        info "The Echo is on Wi-Fi already: $(adb shell ifconfig wlan0 | tr -d '\r' | sed -n 's/.*inet addr:\([0-9.]*\).*/\1/p')"
    else
        if [ ! -f secrets/wifi.conf ]; then
            prompt ssid "Wi-Fi name (WPA2, same network as Home Assistant)"
            prompt pass "Wi-Fi password" "" -s
            [ -n "$DRY" ] || { mkdir -p secrets; ( umask 077; printf '%s\n%s\n' "$ssid" "$pass" > secrets/wifi.conf ); new=1; }
        fi
        task "Joining Wi-Fi" scripts/wifi-join.sh || { [ -n "$new" ] && rm -f secrets/wifi.conf; return 1; }
    fi
    warn "Do not restart the Echo until it is installed."
    info "Tip: give it a fixed address in your router (DHCP reservation)."
}

step_install() {
    bootroot_unpack || return 1         # again: the steps before may have been skipped
    install_satellite || return 1
}
