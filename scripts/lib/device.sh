# Sourced by the PC scripts, from the repository root: which Echo model, and its facts (devices/$DEVICE/device.conf).
#   DEVICE=<codename>   pick the model by hand; the default is donut, or with device_load adb the model of the Echo on adb
# After device_load: DEVICE, DDIR (devices/$DEVICE), OUT (build/$DEVICE, as the Makefile), FW (firmware/$DEVICE), plus
# everything device.conf sets.  With two Echos on adb, ANDROID_SERIAL picks one, as for adb itself.

die() { echo "$*" >&2; exit 1; }

# codename whose device.conf names this product (ro.product.device)
device_for_product() {
    for c in devices/*/device.conf; do
        [ "$(. "$c"; echo "$PRODUCT")" = "$1" ] && { c=${c%/device.conf}; echo "${c##*/}"; return 0; }
    done
    return 1
}

# device_load [adb]: with "adb", ask the connected Echo which model it is, and refuse when DEVICE says otherwise.
# In recovery the properties are TWRP's own, so there DEVICE (or the default) is taken as given.
device_load() {
    if [ "$1" = adb ] && [ "$(adb get-state 2>/dev/null)" = device ]; then
        prod=$(adb shell getprop ro.product.device | tr -d '\r')
        [ -n "$prod" ] || die "no Echo on adb (with several: export ANDROID_SERIAL=<serial from adb devices>)"
        found=$(device_for_product "$prod") ||
            die "the Echo on adb reports product '$prod', which no devices/*/device.conf names: not supported (devices/README.md)"
        [ -z "$DEVICE" ] || [ "$DEVICE" = "$found" ] || die "DEVICE=$DEVICE, but the Echo on adb is a $found ($prod)"
        DEVICE=$found
    fi
    DEVICE=${DEVICE:-donut}
    DDIR=devices/$DEVICE
    [ -f $DDIR/device.conf ] || die "unknown DEVICE '$DEVICE': no $DDIR/device.conf"
    . $DDIR/device.conf
    OUT=build/$DEVICE
    FW=firmware/$DEVICE
    export DEVICE
}

# The binaries link against one firmware's libraries; on another one they may crash or misbehave in the audio path.
# device_check_firmware [warn]: stop (or only warn) when the Echo on adb runs a different firmware.
# firmware_ok <id>: the pinned firmware, or a build device.conf lists in FIRMWARE_ALSO (checked: the stock libraries of
# PROBE_FILES are byte-identical to the pinned build's, so the binaries link against the same code).
firmware_ok() {
    local f
    [ "$1" = "$FIRMWARE_ID" ] && return 0
    for f in $FIRMWARE_ALSO; do [ "$1" = "$f" ] && return 0; done
    return 1
}

device_check_firmware() {
    have=$(adb shell getprop ro.build.display.id | tr -d '\r')
    firmware_ok "$have" && return 0
    [ "$1" = warn ] && { echo "warning: this Echo runs firmware '$have', $DEVICE support is built for $FIRMWARE_ID" >&2; return 0; }
    # A model installed from the running OS (INSTALL=running) is rooted before this check, and nothing here flashes a
    # build-bound image: the other builds of a family mostly carry the same libraries, so the person decides.
    # (scripts/probe.sh, which setup.sh runs after the build, compares the libraries themselves.)  HASSMIC_FIRMWARE_OK=1
    # takes the answer for scripts without a terminal.
    if [ "$INSTALL" = running ]; then
        {
            echo
            echo "################################################################################"
            echo "# WARNING: this Echo runs firmware '$have'"
            echo "#          $DEVICE support is built and tested for $FIRMWARE_ID${FIRMWARE_ALSO:+ (also $FIRMWARE_ALSO)}"
            echo "# hassmic links against that build's libraries. If Amazon changed the audio or wake word libraries"
            echo "# in this one, hassmic may crash or hear nothing. scripts/probe.sh tells (no DIFFERENT = identical)."
            echo "################################################################################"
            echo
        } >&2
        [ "$HASSMIC_FIRMWARE_OK" = 1 ] && return 0
        if { : < /dev/tty; } 2>/dev/null; then
            printf 'Install anyway? [y/N] ' >&2; read -r a < /dev/tty
            [ "$a" = y ] || [ "$a" = Y ] || die "cancelled: reflash $FIRMWARE_ID ($DDIR/README.md), or set HASSMIC_FIRMWARE_OK=1"
            return 0
        fi
        die "no terminal to ask on: set HASSMIC_FIRMWARE_OK=1 to install anyway"
    fi
    die "this Echo runs firmware '$have', $DEVICE support is built for $FIRMWARE_ID: reflash that one ($DDIR/README.md)"
}
