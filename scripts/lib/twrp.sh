# TWRP on the Echo Shows, for the PC scripts that use it on the Echo on adb: scripts/lib/setup.sh (the wipe before the
# install, wipe_data) and scripts/install-boot.sh (the end of --uninstall).  POSIX sh, sourced from the repository
# root after device.conf (SYSTEM_DEV: userdata sits beside it).

# wipe_twrp: TWRP's wipe of cache and data, read back from /data/app, since the bugs the wipe prevents show only long
# after (a newer build's app updates left there, PR #92; Amazon's apps put back without their data, PR #108).  Not from
# adb's exit status: what twrp exits with was never seen for a failed wipe, and a USB restart (TWRP's "Keep System Read
# only?" answered late) ends the adb shell while TWRP wipes on, hence the 30 s of tries.  Each ends in a word: nothing
# back means adb is away, and an unmounted /data would pass for an empty one.
# The wipe gets 5 min, run in the background with its status in a file (not $!'s: dash reaps a finished job only on
# wait, kill -0 finds its zombie): whether twrp answers while that question is still on the screen was never seen, and
# with nobody at the PC (install-boot.sh --uninstall --yes) the wait would not end.  Not coreutils' timeout: it runs adb
# from PATH, past a test's adb function, to a real Echo.
wipe_twrp() {
    wrc=$(mktemp) || return 1
    ( if adb shell 'twrp wipe cache && twrp wipe data'; then echo 0; else echo $?; fi > "$wrc" ) &
    wpid=$! wi=0
    while [ ! -s "$wrc" ] && [ $wi -lt 300 ]; do sleep 1; wi=$((wi + 1)); done
    if [ ! -s "$wrc" ]; then
        pkill -P $wpid 2>/dev/null; kill $wpid 2>/dev/null
        echo "twrp wipe: no answer in 5 min (TWRP still asking \"Keep System Read only?\"?)"
    elif [ "$(cat "$wrc")" != 0 ]; then
        echo "adb shell: exit $(cat "$wrc")"
    fi
    rm -f "$wrc"
    wi=0
    while [ $wi -lt 30 ]; do
        wleft=$(adb shell "grep -q ' /data ' /proc/mounts || mount -t ext4 ${SYSTEM_DEV%/*}/userdata /data 2>/dev/null
                if grep -q ' /data ' /proc/mounts; then ls /data/app 2>/dev/null; echo end; else echo unmounted; fi" \
                2>/dev/null | tr -d '\r')
        [ "$wleft" = end ] && return 0
        sleep 1; wi=$((wi + 1))
    done
    echo "after the wipe, /data/app holds: ${wleft:-(no answer from the Echo)}"
    return 1
}
