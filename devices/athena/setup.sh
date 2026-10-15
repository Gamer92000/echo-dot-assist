# Guided installation for the Echo Show 8 2nd gen (athena), run by scripts/setup.sh (bash, from the repository root, with
# device.conf loaded and the helpers of scripts/lib/setup.sh).  The same steps, written out: README.md next to this file.
# Each step_<id> returns non-zero when it did not finish; the runner then offers retry, skip or quit.  A step checks first
# whether its result is there already, so a rerun (or work done by hand) passes through quietly.
# What is different from crown: no flashing from here.  The unlock (saiyaku, a serial exploit) and the root zip are the
# user's own work in front of this (README.md), and the install is written from the running OS (INSTALL=running).

XDA="the XDA thread of saiyaku-athena (devices/athena/unlock.md)"
SAIYAKU=saiyaku-athena-v1.0.0.zip
BOOTROOT=$FW/boot-root.zip
MAGISKPOLICY=$FW/boot-root/patch/magiskpolicy32
QEMU=toolchain/qemu-arm
NDK=toolchain/android-ndk-r21e
NDK_URL=https://dl.google.com/android/repository/android-ndk-r21e-linux-x86_64.zip

STEPS="
tools|Tools on this PC
files|Downloads
usb|Power and USB to the Echo
root|Unlocked and rooted
build|Build
network|Lock down, switch Alexa off
install|Install
"
ROOTED_SKIP="usb root"           # an Echo rooted already: scripts/setup.sh asks
NOTES=(
    "The screen is black with the light band along its bottom edge; Alexa's apps stay off while hassmic runs ($DDIR/README.md)."
    "adb over Wi-Fi: the settings page's debug access (http://<echo-ip>:28931/) opens it for 30 min, as on the other Echos."
    "Updates: git pull, then scripts/ota-push.sh <echo-ip>."
    "\"Download from Amazon\" does not work on this model yet (its attestation service is another one than crown's, docs/re-athena.md). Models copied from another Echo through the settings page do."
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
    if [ "$BUILD_MODE" = prebuilt ]; then need_tools adb unzip brotli python3 debugfs curl sha256sum || return 1; return 0; fi
    need_tools adb unzip brotli python3 make cc debugfs curl sha256sum bc xz || return 1
    [ "$(df -Pk . | awk 'NR == 2 { print $4 }')" -gt 8000000 ] || warn "less than 8 GB free here; the NDK, firmware and images need about that"
}

step_files() {
    local need=()
    need+=($FW/$FIRMWARE_FILE $FIRMWARE_SHA256 "Fire OS $FIRMWARE_ID, from Amazon: the link is in $DDIR/README.md")
    need_files "${need[@]}" || return 1
    [ "$BUILD_MODE" = prebuilt ] && return 0          # nothing to build with
    if [ -x $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang ]; then ok "Android NDK r21e"; else
        mkdir -p toolchain
        TASK_NOTE="du -h toolchain/ndk.zip | cut -f1" task "Downloading Android NDK r21e (1 GB)" curl -fsSL -o toolchain/ndk.zip $NDK_URL &&
            task "Unpacking the NDK" unzip -q -o toolchain/ndk.zip -d toolchain && rm -f toolchain/ndk.zip || return 1
    fi
}

step_usb() {
    [ -z "$DRY" ] && adb_is device && { ok "the Echo is on USB already"; return 0; }
    todo "Connect the Echo to this PC with a micro-USB cable, powered by its own supply" \
        "" \
        "It must be unlocked and rooted first: $DDIR/README.md (saiyaku, then the boot-root zip in TWRP)." \
        "Do not set it up with the Alexa app or its touchscreen."
}

step_root() {
    [ -z "$DRY" ] && adb_is device && [ "$(ashell id -u)" = 0 ] && { ok "root adb already"; return 0; }
    todo "Unlock and root the Echo yourself, then run this setup again" \
        "" \
        "1. saiyaku-athena (serial exploit): $DDIR/unlock.md" \
        "2. flash boot-root.zip in TWRP (it patches system, vendor and adbd in place)" \
        "" \
        "Afterwards 'adb shell id' must say uid=0."
    return 1
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
    wait_adb device || return 1
    [ -f $MAGISKPOLICY ] || { [ -f $BOOTROOT ] && task "Unpacking boot-root.zip (the policy tool)" unzip -q -o $BOOTROOT -d $FW/boot-root; } ||
        { fail "no $MAGISKPOLICY: unzip boot-root.zip into $FW/boot-root ($DDIR/README.md)"; return 1; }
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
    install_satellite || return 1
}
