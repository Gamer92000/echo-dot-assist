#!/system/bin/sh
# hassmic-prepare-api 1
# Called synchronously at post-fs-data (hassmic.rc), before hassmic services are enabled.
# Kernel 5.4.86's stock config enables OVERLAY_FS and UBIFS xattrs. Keep the
# SquashFS base intact; signed updates and generated LED resources use /data.
#
# One file, two parts.  init runs the copy flashed into the SquashFS (no argument), before the overlay on /system is
# mounted, and that copy stays as it was flashed.  So it does little itself: it runs the copy the last promoted update
# left in the overlay's upper folder ($U: this file, written there by sysinstall.sh from a verified bundle) with
# "steps", read from /data directly, the overlay not being up yet.  Every step comes with updates that way, the mount
# too: it is made anew at every boot by the update's copy.  Its own steps are the fallback: no update yet (a fresh
# flash), opted out (no hassmic.conf: the update's copy is not run; the overlay is still mounted, as it always was),
# or the update's copy is not what this part can trust or did not do its job (below).  Whatever happened,
# hassmic.storage.ready is set at the end: it starts the firewall service (whose loop re-asserts the egress lock) and
# the satellite; left unset, neither started and adb over Wi-Fi went with them, USB only (the first flashed copies ran
# under set -e and left it unset on any failure).  What only a reflash changes: this part, and hassmic.rc.
#
# What every later version keeps (the interface; the flashed part of any older version relies on it):
# - the line "# hassmic-prepare-api 1" (a copy without it is not run);
# - "prepare.sh steps" does the steps and nothing else; above the dispatch line there are only definitions and
#   variables;
# - its exit status is 0 only when every step did its job (try() keeps count; a hassmic.conf that does not parse is
#   no step's failure, the fallback could not mend it: logged, the full lockdown), and it mounts the overlay on /system.
# Steps must be ones that can run twice: the fallback runs after an update's copy that failed half way.  The
# fallback's lockdown.sh is the overlay's too (the update's: the SquashFS copy is under the overlay once it is mounted),
# as the firewall service's is; it only gets the same time limit.
# Rollback: a promotion overwrites the copy in the overlay (sysinstall.sh factory), so one that passed its self test
# (hassmic's) and still cuts the Echo off (a lockdown.sh, boot.sh or main.sh that closes the network) would have
# nothing to fall back to but a USB cable.  sysinstall.sh keeps the copy it replaces in $O/previous (only one that was
# reached from the network), main.sh puts $O/boots back to 0 once the Echo is reached (another address connected to a
# port of ours), and this part counts the boots and steps back after two in a row without that (rollback() below):
# the update not promoted yet, else the factory copy to $O/previous, else to the copy flashed into the SquashFS.
# Home Assistant, the page and Music Assistant all away for two boots step back too.  A time limit kills the shell it started only: a
# child of it (lockdown's iptables-restore) may go on beside the fallback's; the firewall service, started by the
# property, loads the chain anew at once.  Without timeout (the firmware has it) the update's copy is not run at all:
# nothing else would keep it from holding the boot.  An update's steps first run at the boot after its
# promotion: its self test is hassmic's, not theirs.
B=/data/local/hassmic
O=/data/hassmic-system
U=$O/upper/hassmic/prepare.sh
# A time limit for what init waits for; the firmware's toybox has timeout (-k: a shell that ignores TERM is killed too).
T=; command -v timeout > /dev/null && T="timeout -k 5 60"
F=0
say() { echo "boot+$(cut -d' ' -f1 /proc/uptime) prepare: $*" >> "$B/boot.log"; }
try() { "$@"; r=$?; [ $r = 0 ] || { F=$((F + 1)); say "!! failed ($r): $*"; }; return $r; }
steps() {
    try mkdir -p "$B" "$O"
    try chown 0:0 "$B" "$O"
    try chmod 755 "$B"
    try chmod 700 "$O"
    try mkdir -p "$O/upper" "$O/work"
    # OverlayFS exposes the upper root's mode and label as /system itself. Init's
    # restrictive umask otherwise creates a 0700 system_data_file root and prevents
    # the non-root stock services from executing their binaries and loading libs.
    try chmod 755 "$O/upper"
    try chcon u:object_r:system_file:s0 "$O/upper"
    if ! grep -q ' /system overlay ' /proc/mounts; then
        try mount -t overlay overlay -o lowerdir=/system,upperdir=$O/upper,workdir=$O/work /system
    fi
    # first boot: hassmic.conf made once, so that deleting it later stays the opt-out; the mark only once it is there
    if [ ! -f "$B/cannoli-initialized" ]; then
        [ -f "$B/hassmic.conf" ] || try sh -c 'printf "ARGS=\"\"\n" > "$1"' - "$B/hassmic.conf"
        [ -f "$B/hassmic.conf" ] && try chmod 600 "$B/hassmic.conf" && try touch "$B/cannoli-initialized"
    fi
    # Deleting hassmic.conf remains the upstream opt-out. On an enabled boot,
    # establish the network rules now, before class main starts its services.
    if [ -f "$B/hassmic.conf" ]; then
        # Same root-owned shell config as main.sh. Keep its documented temporary
        # Alexa mode: stopping Alexa here cannot be undone by the later OTA guard.
        mode=$( . "$B/hassmic.conf"; printf '%s' "${MODE:-}" ) || say "!! hassmic.conf does not parse: MODE taken as unset"
        case "$mode" in
            stock-online) try $T sh /system/hassmic/lockdown.sh ota-only;;
            *) try $T sh /system/hassmic/lockdown.sh;;
        esac
    fi
}
[ "$1" = steps ] && { steps > /dev/null 2>&1; exit $((F > 0)); }

# The update's copy runs as root before anything else, so it is taken only from a folder no one but root can have
# written: $O a real folder, root's, 0700 (steps make it so; /data is system's 0771, so a system process could have put
# another $O there), and $U no link.  Its lines are then root's (sysinstall.sh, from a verified bundle at promotion).
own_o() {
    [ -d "$O" ] && [ ! -L "$O" ] || return 1
    set -- $(ls -ldn "$O")
    [ "$1" = drwx------ ] && [ "$3" = 0 ] && [ "$4" = 0 ] || { say "!! $O is not root's alone ($1 $3:$4)"; return 1; }
}
trusted() {
    own_o || return 1
    [ ! -L "$O/upper" ] && [ ! -L "$O/upper/hassmic" ] && [ ! -L "$U" ] ||
        { say "!! a link on the way to the update's prepare.sh"; return 1; }
    set -- $(ls -ln "$U")                                   # root's and written by root only, should $O ever open up
    case "$1:$3" in -????-??-?:0) ;; *) say "!! the update's prepare.sh is not root's alone ($1 $3)"; return 1;; esac
    grep -qx '# hassmic-prepare-api 1' "$U" || { say "!! the update's prepare.sh is not of interface 1"; return 1; }
    sh -n "$U" 2> /dev/null || { say "!! the update's prepare.sh does not parse"; return 1; }
}
# The boot count, and the rollback (above), a step at a time: after two boots in a row nobody on the network reached the
# Echo on (main.sh excuses a boot without a network address or before set-up, for a version reached before), the third
# takes the first of
#   1. an update in ota/current that is not the factory copy (never promoted, or the one before the factory copy's):
#      dropped, boot.sh runs the factory copy;
#   2. the factory copy, with $O/previous there: previous in its place;
#   3. the factory copy, with none: moved aside, the copy flashed into the SquashFS shows through the overlay;
# and each version taken out goes into ota/refused (main.sh installs it never again).  Two boots more without being
# reached take the next step.  Done in an order a power cut cannot spoil: the refusal and ota/current first, previous
# relabelled before it moves, a swap half done is finished at the next start, the count last.  Only with no overlay yet
# (init's run, not one by hand): the overlay's upper folder is not to be changed under it; /system is the SquashFS then.
refuse() { [ -z "$1" ] || grep -qxF "$1" "$B/ota/refused" 2>/dev/null || echo "$1" >> "$B/ota/refused"; }
rollback() {
    n=$(cat "$O/boots" 2>/dev/null); case "$n" in ''|*[!0-9]*) n=0;; esac
    P=$O/previous H=$O/upper/hassmic
    # sysinstall.sh keeping a new previous (previous.new written and synced, the old one to previous.old, the new one in
    # place): cut between its two renames, previous.new is the complete one
    [ -d "$P" ] || [ ! -d "$O/previous.old" ] || [ ! -d "$O/previous.new" ] || { mv "$O/previous.new" "$P" && rm -rf "$O/previous.old"; }
    if [ ! -d "$H" ] && [ -d "$P" ] && [ ! -L "$P" ] && [ -f "$P/prepare.sh" ]; then
        mv "$P" "$H" && say "!! a roll back cut short is finished: $(cat "$H/VERSION" 2>/dev/null) is the factory copy"
    fi
    if [ "$n" -ge 2 ]; then
        mkdir -p "$B/ota"
        fv=$(cat "$H/VERSION" 2>/dev/null || cat /system/hassmic/VERSION 2>/dev/null)
        cv=$(cat "$B/ota/current/VERSION" 2>/dev/null)
        why="$n boots without being reached from the network"
        if [ -L "$B/ota/current" ] && [ "$cv" != "$fv" ]; then
            refuse "$cv"; rm -f "$B/ota/current"; sync
            say "!! $why: the update ${cv:-?} dropped, the factory copy ${fv:-?} runs"
        elif [ -d "$P" ] && [ ! -L "$P" ] && [ -f "$P/prepare.sh" ] && [ -d "$H" ]; then
            pv=$(cat "$P/VERSION" 2>/dev/null)
            chcon -R u:object_r:system_file:s0 "$P"
            refuse "$fv"; rm -f "$B/ota/current"; rm -rf "$O/rolled-back"; sync
            mv "$H" "$O/rolled-back" && mv "$P" "$H" && sync &&
                say "!! $why: rolled back from ${fv:-?} to ${pv:-?}" ||
                say "!! $why: the roll back from ${fv:-?} to ${pv:-?} failed"
        elif [ -d "$H" ]; then
            refuse "$fv"; rm -f "$B/ota/current"; rm -rf "$O/rolled-back"; sync
            mv "$H" "$O/rolled-back" && sync &&
                say "!! $why: ${fv:-?} moved aside, the flashed copy $(cat /system/hassmic/VERSION 2>/dev/null) runs" ||
                say "!! $why: moving ${fv:-?} aside failed"
        else
            say "!! $why, on the flashed copy already: nothing left to roll back"
        fi
        n=0
    fi
    [ "$n" -lt 99 ] || n=98
    echo $((n + 1)) > "$O/boots.new" && mv -f "$O/boots.new" "$O/boots"
    sync
}
[ -f "$B/hassmic.conf" ] && ! grep -q ' /system overlay ' /proc/mounts && own_o && rollback

ok=
if [ -n "$T" ] && [ -f "$B/hassmic.conf" ] && [ -f "$U" ] && trusted; then
    # 90 s: its lockdown.sh has 60 of its own, after which its steps still end and say so
    timeout -k 5 90 sh "$U" steps; r=$?
    if [ $r != 0 ]; then say "!! the update's steps failed ($r): the flashed ones too"
    elif ! grep -q ' /system overlay ' /proc/mounts; then say "!! the update's steps left /system without the overlay"
    else ok=1; fi
fi
[ -n "$ok" ] || steps > /dev/null 2>&1
setprop hassmic.storage.ready 1
