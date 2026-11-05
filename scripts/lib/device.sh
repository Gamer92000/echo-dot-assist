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
    release_load
}

# release_load: UPDATE_REPO (whose GitHub releases online updates and prebuilt binaries come from) and RELEASE_PUB (the
# key they are signed with, which every bundle and install carries), as the Makefile reads them: keys/release.conf, or a
# fork's own secrets/fork.conf (scripts/fork-release.sh)
release_load() {
    local _f _bad
    for _f in keys/release.conf secrets/fork.conf; do
        [ -f $_f ] || continue
        # the lines as written: make keeps quotes that sh drops, and reads NAME = value, which sh runs as a command
        _bad=$(grep -E '^[[:space:]]*(export[[:space:]]+)?(UPDATE_REPO|RELEASE_PUB)([^A-Za-z0-9_]|$)' $_f |
            grep -Ev '^(UPDATE_REPO|RELEASE_PUB)=[A-Za-z0-9._/-]+[[:space:]]*(#.*)?$') &&
            die "$_f: '$_bad': NAME=value, no quotes or spaces (keys/release.conf)"
        . ./$_f
    done
    case $UPDATE_REPO in */*/*|*[!A-Za-z0-9._/-]*) die "UPDATE_REPO=$UPDATE_REPO: not owner/name";; ?*/?*) ;;
        *) die "UPDATE_REPO=$UPDATE_REPO: not owner/name";; esac
    # otatool's bundles and adb push name a file by its own name: the Echo looks for release.pub
    case $RELEASE_PUB in release.pub|*/release.pub) ;; *) die "RELEASE_PUB=$RELEASE_PUB: the file must be called release.pub";; esac
    [ -s "$RELEASE_PUB" ] || die "no $RELEASE_PUB (the release key of $UPDATE_REPO)"
}

# The binaries link against one firmware's libraries; on another one they may crash or misbehave in the audio path.
# firmware_same: the stock files of PROBE_FILES on the Echo on adb are byte-identical to the pinned firmware's
# (scripts/probe.sh against devices/<codename>/probe.md5): the libraries whose interfaces are reverse-engineered or
# whose behaviour hassmic is tuned to (wake word engine, mixer or audio HAL, front end).  Not probed: those with a
# public, stable ABI it links (libopus, libz) or dlopens (libcurl, ...); a missing symbol there fails at load, and
# the install's self test with it.  PROBE_DIFF: the probe's DIFFERENT lines.  2 when the probe did not run (no Echo on
# adb, adb gone): nothing is known.  stdin /dev/null: adb shell forwards a terminal's input, and would take what was
# typed ahead for the question below.
firmware_same() {
    local out
    PROBE_DIFF=
    out=$(sh scripts/probe.sh 2>/dev/null < /dev/null) || return 2
    PROBE_DIFF=$(printf '%s\n' "$out" | grep DIFFERENT)
    [ -z "$PROBE_DIFF" ]
}

# firmware_ok [id]: hassmic may go onto the Echo on adb, which runs firmware id (default: asked).  The pinned build, or
# one whose PROBE_FILES are the pinned build's (Amazon's builds of one line mostly are: athena's 7077 and 7082 are 7080's)
# where the install flashes nothing made for one build: INSTALL=twrp-ab and running write files and patch the policy of
# the running OS.  INSTALL=boot flashes the pinned firmware's own boot.img, which must match the system build
# (install-boot.sh checks it): the pinned build only.
firmware_ok() {
    local id=${1-$(adb shell getprop ro.build.display.id | tr -d '\r')}
    [ "$id" = "$FIRMWARE_ID" ] && return 0
    [ "$INSTALL" != boot ] && firmware_same
}

# firmware_warning ID: what a person is told before hassmic goes onto a build whose libraries differ (PROBE_DIFF)
firmware_warning() {
    echo
    echo "################################################################################"
    echo "# WARNING: this Echo runs firmware '$1'"
    echo "#          $DEVICE support is built and tested for $FIRMWARE_ID"
    if [ -n "$PROBE_DIFF" ]; then
        echo "# Of the stock files scripts/probe.sh compares (the audio and wake word code), these differ from $FIRMWARE_ID's:"
        printf '%s\n' "$PROBE_DIFF" | sed 's/^ */#   /'
    else
        echo "# scripts/probe.sh did not run, so it is not known whether its libraries are $FIRMWARE_ID's."
    fi
    echo "# If Amazon changed the audio or wake word code in them, hassmic may crash or hear nothing."
    echo "################################################################################"
    echo
}

# device_check_firmware [warn]: stop (or only warn) when the Echo on adb runs a firmware hassmic may not go onto
# (firmware_ok).  INSTALL=running, libraries different: the person decides, since no step puts the pinned build back
# (the Echo is rooted in the running OS, before this check) and a new line's libraries may differ by a rebuild only
# (athena's 7806N: two of six, build ids, the same exported symbols; it ran).  HASSMIC_FIRMWARE_OK=1 is the answer; without it, asked on stdin when that is a terminal, never on
# /dev/tty: scripts/setup.sh runs install-system.sh under task (output into its log, stdin /dev/null), where a
# question on /dev/tty waited unseen behind the spinner.  setup.sh asks itself (firmware_accept) and passes the answer.
device_check_firmware() {
    local have a
    have=$(adb shell getprop ro.build.display.id | tr -d '\r')
    [ "$have" = "$FIRMWARE_ID" ] && return 0
    if firmware_ok "$have"; then
        echo "this Echo runs firmware '$have', not $FIRMWARE_ID, with the same probed stock files (scripts/probe.sh): going on" >&2
        return 0
    fi
    [ "$1" = warn ] && { echo "warning: this Echo runs firmware '$have', $DEVICE support is built for $FIRMWARE_ID" >&2; return 0; }
    if [ "$INSTALL" = running ]; then
        firmware_warning "$have" >&2
        [ "$HASSMIC_FIRMWARE_OK" = 1 ] && return 0
        [ -t 0 ] || die "no terminal to ask on: set HASSMIC_FIRMWARE_OK=1 to install anyway"
        # || a=: install-system.sh is set -e, and a read at the end of input (Ctrl-D) ended it without a word
        printf 'Install anyway? [y/N] ' >&2; read -r a || a=
        case $a in y|Y|yes) return 0;; esac
        die "cancelled: hassmic is built for $FIRMWARE_ID ($DDIR/README.md); HASSMIC_FIRMWARE_OK=1 installs anyway"
    fi
    die "this Echo runs firmware '$have', $DEVICE support is built for $FIRMWARE_ID: reflash that one ($DDIR/README.md)"
}
