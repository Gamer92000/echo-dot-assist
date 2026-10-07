#!/usr/bin/env bash
# Guided installation.  Picks the Echo model, then runs that model's steps (devices/<codename>/setup.sh) one after the
# other and only stops where the user has to do something.  Progress is kept in build/<codename>/setup.done, so it can
# be stopped at any point (Ctrl-C) and started again where it left off; command output goes to build/<codename>/setup.log.
#   scripts/setup.sh [codename] [--dry-run] [--restart] [--from STEP] [--preset FILE]
#     --dry-run   go through all steps and show the commands, run none of them (file checks still happen)
#     --restart   forget the progress, e.g. for the next Echo of the same model
#     --from S    count every step before S as done, e.g. --from build (tools files udev usb unlock flash root build
#                 network install wakeword)
#     --preset F  settings to start with: an export from another Echo's settings page (hassmic-settings.conf);
#                 without it, the install step asks (Enter for none)
# Only the Echo on USB is worked on, whatever else is on adb over Wi-Fi.  Keep the cable in to the end: the installed
# Echo closes adb over Wi-Fi at its first boot.
# The written instructions are the same steps: devices/<codename>/README.md.
cd "$(dirname "$0")/.."
. scripts/lib/device.sh
. scripts/lib/setup.sh
. scripts/lib/artifacts.sh
. scripts/lib/build.sh

DRY= RESTART= PRESET= FROM= ROOTED_NOTE=
while [ $# -gt 0 ]; do
    a=$1; shift
    case $a in
    --dry-run) DRY=1;;
    --restart) RESTART=1;;
    --from) FROM=$1; shift;;
    --from=*) FROM=${a#--from=};;
    --preset) PRESET=$1; shift; [ -f "$PRESET" ] || die "--preset: no file $PRESET";;
    --preset=*) PRESET=${a#--preset=}; [ -f "$PRESET" ] || die "--preset: no file $PRESET";;
    -h|--help) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 0;;
    -*) die "unknown option $a (--help)";;
    *) DEVICE=$a;;
    esac
done

bye() {
    [ -n "$TASK_PID" ] && kill $TASK_PID 2>/dev/null
    [ -n "$TMP" ] && rm -rf "$TMP"
    [ -n "$TTY" ] && printf '\e[?25h\e[?7h\r\e[K'
    printf '\n  %sStopped. Run scripts/setup.sh%s again to go on from here.%s\n' "$DIM" "${DEVICE:+ $DEVICE}" "$N"
    exit 130
}
trap bye INT TERM
trap '[ -n "$TTY" ] && printf "\e[?25h\e[?7h"' EXIT

# The Echo being set up is on USB; an explicit ANDROID_SERIAL (e.g. to finish the last steps over Wi-Fi) wins
[ -n "$ANDROID_SERIAL" ] || USB_ONLY=1

# --- which Echo: the one on USB if it runs its stock OS, else asked
if [ -z "$DEVICE" ] && [ -z "$DRY" ] && adb_is device 2>/dev/null; then
    DEVICE=$(device_for_product "$(ashell getprop ro.product.device)")
fi
if [ -z "$DEVICE" ]; then
    [ -n "$TTY" ] && printf '\e[H\e[2J'
    printf '\n  %sEcho → Home Assistant%s\n\n  Which Echo?\n\n' "$B" "$N"
    models=() labels=()
    for t in 0 1; do                        # tried on a real Echo first
        for c in devices/*/device.conf; do
            d=${c%/device.conf}; d=${d##*/}
            [ -f devices/$d/setup.sh ] || continue
            grep -q '^UNTESTED=1' devices/$d/setup.sh && u=1 || u=0
            [ $u = $t ] || continue
            models+=("$d")
            labels+=("$(. "$c"; printf '%s  %s%s%s%s' "$MODEL_NAME" "$DIM" "${MODEL_NUMBER:+$MODEL_NUMBER, }$d" "$([ $u = 1 ] && echo ", untested")" "$N")")
        done
    done
    menu m "${labels[@]}"
    DEVICE=${models[m]}
fi
device_load
[ -f $DDIR/setup.sh ] || die "$MODEL_NAME ($DEVICE) is not supported by the guided setup yet: $DDIR/README.md"
. $DDIR/setup.sh

mkdir -p $OUT
STATE=$PWD/$OUT/setup.done LOG=$PWD/$OUT/setup.log
[ -n "$RESTART" ] && [ -z "$DRY" ] && rm -f $STATE $OUT/setup.env $OUT/setup.build
touch $STATE
# asked in the tools step; a rerun past it goes on as decided then
[ -f $OUT/setup.build ] && build_mode_set "$(cat $OUT/setup.build)"
[ -n "$DRY" ] && STATE=/dev/null LOG=/dev/null        # a dry run walks every step, and leaves the progress alone
mapfile -t STEP_LIST < <(printf '%s\n' "$STEPS" | grep .)
n=${#STEP_LIST[@]}
DONE_IDS=$(tr '\n' ' ' < $STATE)
done_ids() { DONE_IDS="$DONE_IDS $1"; [ -n "$DRY" ] || echo "$1" >> $STATE; }
mark_done() { local id; for id; do [[ " $DONE_IDS " == *" $id "* ]] || done_ids $id; done; }

if [ -n "$FROM" ]; then
    ids=()
    for st in "${STEP_LIST[@]}"; do [ "${st%%|*}" = "$FROM" ] && break; ids+=("${st%%|*}"); done
    [ ${#ids[@]} = $n ] && die "--from: no step $FROM (steps: $(printf '%s\n' "${STEP_LIST[@]}" | cut -d'|' -f1 | tr '\n' ' '))"
    mark_done "${ids[@]}"
fi

# --- an Echo rooted already, by these steps before or some other way (issue #10): the unlock would reboot it into TWRP
# and the firmware step wipe it, so offer to go straight on.  Only with the firmware this model's support is built for;
# anything else needs the firmware step anyway.
if [ ! -s $STATE ] && [ -z "$DRY" ] && [ -n "$ROOTED_SKIP" ] && adb_is device 2>/dev/null && [ "$(ashell id -u)" = 0 ]; then
    fwid=$(ashell getprop ro.build.display.id)
    if [ "$fwid" = "$FIRMWARE_ID" ]; then
        header 1 $n "${STEP_LIST[@]}"
        ok "The $MODEL_NAME on USB is rooted already and runs $FIRMWARE_ID."
        menu a "Keep it as it is: skip the unlock, firmware and root steps" "Start from scratch: unlock, wipe and reflash it"
        [ "$a" = 0 ] && mark_done $ROOTED_SKIP rooted
    else
        ROOTED_NOTE="The $MODEL_NAME on USB is rooted, but runs '$fwid', not $FIRMWARE_ID: the firmware step reflashes it."
    fi
fi

# --- once, before anything touches the Echo
if [ ! -s $STATE ] && [ -z "$DRY" ]; then
    header 1 $n "${STEP_LIST[@]}"
    say "Turns this $MODEL_NAME into a Home Assistant voice satellite."
    say "About an hour; it stops whenever you have to do something."
    printf '\n'
    warn "${B}This unlocks and wipes the Echo and can brick it.$N"
    warn "Nothing here was reviewed by anyone but its author."
    [ -n "$ROOTED_NOTE" ] && warn "$ROOTED_NOTE"
    printf '\n'
    prompt a "Type yes to start:"
    [ "$a" = yes ] || { info "Nothing done."; exit 0; }
fi

for ((i = 1; i <= n; i++)); do
    id=${STEP_LIST[i-1]%%|*}
    [[ " $DONE_IDS " == *" $id "* ]] && continue
    header $i $n "${STEP_LIST[@]}"
    [ -n "$DRY" ] && info "dry run: nothing is run"
    while :; do
        if step_$id; then done_ids $id; break; fi
        printf '\n  %sEnter%s try again  %ss%s skip this step  %sq%s quit ' "$B" "$N" "$B" "$N" "$B" "$N"
        read -r a < /dev/tty || exit 1
        case $a in
        s*) break;;
        q*) printf '\n  %sProgress is kept: run scripts/setup.sh %s again to go on.%s\n' "$DIM" "$DEVICE" "$N"; exit 1;;
        esac
        header $i $n "${STEP_LIST[@]}"
    done
done

header $((n + 1)) $n "${STEP_LIST[@]}"
done_screen "${NOTES[@]}"
