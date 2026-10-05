# Helpers for the guided setup (scripts/setup.sh and devices/<codename>/setup.sh).  bash; sourced from the repository root.
# The screen: a header with a progress bar and the step list, redrawn per step; below it one line per thing being done
# (a spinner while it runs, ✓ or ✗ after) and a highlighted box whenever the user has to do something.  Command output
# goes to $LOG, and only its tail is shown when something fails.  Every prompt reads from /dev/tty.

if [ -t 1 ]; then
    TTY=1 B=$'\e[1m' DIM=$'\e[2m' RED=$'\e[31m' GRN=$'\e[32m' YEL=$'\e[33m' CYN=$'\e[36m' N=$'\e[0m'
else
    TTY= B= DIM= RED= GRN= YEL= CYN= N=
fi
LOG=${LOG:-/dev/null}
SPIN=(⠋ ⠙ ⠹ ⠸ ⠼ ⠴ ⠦ ⠧ ⠇ ⠏)
_f=0 TASK_PID=

say()  { printf '  %s\n' "$*"; }
info() { printf '  %s%s%s\n' "$DIM" "$*" "$N"; }
ok()   { printf '  %s✓%s %s\n' "$GRN" "$N" "$*"; }
warn() { printf '  %s!%s %s\n' "$YEL" "$N" "$*"; }
fail() { printf '  %s✗ %s%s\n' "$RED" "$*" "$N"; }
_clr() { [ -n "$TTY" ] && printf '\r\e[K\e[?25h\e[?7h'; }
_rep() { local s; printf -v s '%*s' "$2" ''; printf '%s' "${s// /$1}"; }
_dur() { [ "$1" -ge 60 ] && printf '%dm%02ds' $(($1 / 60)) $(($1 % 60)) || printf '%ds' "$1"; }

# header CUR TOTAL TITLES...: clear the screen, title, progress bar and the step list with step CUR (1-based) current;
# CUR > TOTAL: all done.  DONE_IDS (space separated) marks steps done; the ones before CUR that are not were skipped.
header() {
    local cur=$1 n=$2 i=0 w=32 d t id
    shift 2
    [ -n "$TTY" ] && printf '\e[H\e[2J'
    printf '\n  %sEcho → Home Assistant%s   %s%s%s\n\n' "$B" "$N" "$DIM" "$MODEL_NAME${DEVICE:+ · $DEVICE}" "$N"
    d=$(( (cur - 1) * w / n )); [ $d -gt $w ] && d=$w
    printf '  %s%s%s%s%s  %s%d/%d%s\n\n' "$GRN" "$(_rep █ $d)" "$DIM" "$(_rep ░ $((w - d)))" "$N" "$DIM" \
        $((cur > n ? n : cur - 1)) "$n" "$N"
    for t in "$@"; do
        i=$((i + 1)); id=${t%%|*}; t=${t#*|}
        if [ $i = "$cur" ]; then printf '  %s▸ %s%s\n' "$B$CYN" "$t" "$N"
        elif [[ " $DONE_IDS " == *" $id "* ]]; then printf '  %s✓%s %s%s%s\n' "$GRN" "$N" "$DIM" "$t" "$N"
        elif [ $i -lt "$cur" ]; then printf '  %s– %s (skipped)%s\n' "$DIM" "$t" "$N"
        else printf '  %s· %s%s\n' "$DIM" "$t" "$N"; fi
    done
    printf '\n'
}

# one spinner frame: label, time since T0, optional note.  The cursor stays hidden until _clr.
_tick() {
    [ -n "$TTY" ] || return 0
    printf '\e[?25l'
    local e=$((SECONDS - $2))
    printf '\r\e[K  %s%s%s %s%s%s%s' "$CYN" "${SPIN[_f++ % 10]}" "$N" "$1" "$DIM" \
        "$([ $e -ge 3 ] && printf '  %s' "$(_dur $e)")${3:+  · $3}" "$N"
}

# task "label" CMD...: run CMD (stdin closed, output into the log) behind a spinner.  $TASK_NOTE, if set, is a command
# whose output is shown next to the label while it runs (e.g. how much of a download is there).  On failure: the log's tail.
task() {
    local label=$1 r t0=$SECONDS note= i=0 from
    shift
    if [ -n "$DRY" ]; then printf '  %s· %s  $ %s%s\n' "$DIM" "$label" "$*" "$N"; return 0; fi
    printf '\n=== %s: %s\n' "$label" "$*" >> "$LOG"
    from=$(wc -l < "$LOG" 2>/dev/null || echo 0)
    [ -n "$TTY" ] || say "… $label"
    "$@" >> "$LOG" 2>&1 < /dev/null &
    TASK_PID=$!
    while kill -0 $TASK_PID 2>/dev/null; do
        [ -n "$TASK_NOTE" ] && [ $((i++ % 10)) = 0 ] && note=$(eval "$TASK_NOTE" 2>/dev/null)
        _tick "$label" $t0 "$note"; sleep 0.1
    done
    wait $TASK_PID; r=$?; TASK_PID=
    _clr
    if [ $r = 0 ]; then ok "$label"; return 0; fi
    fail "$label"
    [ "$LOG" != /dev/null ] && { tail -n +$((from + 1)) "$LOG" | tail -n 12 | sed "s/^/    $DIM/; s/$/$N/"; info "  full log: $LOG"; }
    return $r
}
# live "label" CMD...: for a command that talks to the user itself (asks, waits for keys): runs in the foreground with
# its output on screen, framed by the label and ✓/✗
live() {
    local label=$1 r
    shift
    if [ -n "$DRY" ]; then printf '  %s· %s  $ %s%s\n' "$DIM" "$label" "$*" "$N"; return 0; fi
    printf '  %s┌ %s%s\n' "$CYN" "$label" "$N"
    ( "$@" ) < /dev/tty; r=$?          # a subshell: CMD may be in_dir, whose cd must not stick
    if [ $r = 0 ]; then ok "$label"; else fail "$label"; fi
    return $r
}
# in_dir DIR CMD...: for task, CMD run from DIR
in_dir() { cd "$1" && shift && "$@"; }

# waitfor "label[|done label]" CHECK [HINT [AFTER [TIMEOUT]]]: spinner until the shell command CHECK succeeds (tried
# every 2 s).  HINT is shown once after AFTER seconds (default 60); with TIMEOUT seconds, gives up and returns 1.
# An Echo that stays "offline" on adb over Wi-Fi (adb_is counts it) gets a hint of its own.
waitfor() {
    local label=${1%%|*} donel=${1#*|} check=$2 hint=$3 after=${4:-60} timeout=$5 t0=$SECONDS i offhint=1
    if [ -n "$DRY" ]; then printf '  %s· %s%s\n' "$DIM" "$label" "$N"; return 0; fi
    ADB_OFFLINE=0
    until eval "$check" > /dev/null 2>&1; do
        if [ -n "$hint" ] && [ $((SECONDS - t0)) -ge "$after" ]; then _clr; warn "$hint"; hint=; fi
        if [ -n "$offhint" ] && [ $ADB_OFFLINE -ge 10 ]; then
            _clr; warn "The Echo takes adb's connection but stays \"offline\": its adb hangs. Unplug the Echo's power and plug it back in."
            offhint=
        fi
        [ -n "$timeout" ] && [ $((SECONDS - t0)) -ge "$timeout" ] && { _clr; fail "$label: gave up after $(_dur $timeout)"; return 1; }
        for i in {1..20}; do _tick "$label" $t0; sleep 0.1; done
    done
    _clr
    ok "$donel"
}

# tell "title" [line]...: what the user has to do next, while the script goes on (e.g. waits for the device)
tell() {
    printf '\n  %s▶ %s%s\n' "$YEL$B" "$1" "$N"
    shift
    local l; for l; do printf '    %s\n' "$l"; done
}
# todo "title" [line]...: as tell, then waits for Enter
todo() {
    tell "$@"
    [ -n "$DRY" ] && return 0
    printf '\n  %sPress Enter when done.%s ' "$DIM" "$N"
    read -r _ < /dev/tty || exit 1
    [ -n "$TTY" ] && printf '\e[1A\r\e[K'
    return 0
}

# ask "question" [y|n]: default answer second; returns 0 for yes.  A dry run takes the default.
ask() {
    local d=${2:-y} a p
    [ "$d" = y ] && p="Y/n" || p="y/N"
    [ -n "$DRY" ] && { printf '  %s? %s [%s] %s%s\n' "$DIM" "$1" "$p" "$d" "$N"; [ "$d" = y ]; return; }
    while :; do
        printf '  %s?%s %s %s[%s]%s ' "$CYN" "$N" "$1" "$DIM" "$p" "$N"
        read -r a < /dev/tty || exit 1
        case ${a:-$d} in [yY]*) return 0;; [nN]*) return 1;; esac
    done
}
# prompt VAR "question" [default] [-s]: -s hides the input.  A dry run takes the default.
prompt() {
    local _answer                     # a name no caller uses: printf -v would set this local instead of the caller's VAR
    [ -n "$DRY" ] && { printf -v "$1" '%s' "$3"; return 0; }
    printf '  %s?%s %s%s ' "$CYN" "$N" "$2" "${3:+ $DIM[$3]$N}"
    if [ "$4" = -s ]; then read -r -s _answer < /dev/tty || exit 1; echo; else read -r _answer < /dev/tty || exit 1; fi
    printf -v "$1" '%s' "${_answer:-$3}"
}

# menu VAR ITEM...: pick one with the arrow keys (numbers work too); VAR gets its index.  $MENU_SEL: the one selected
# at first (default 0).  Without a terminal: that one.  An empty ITEM is a blank line between groups: never selected.
menu() {
    local _var=$1 _n=$(($# - 1)) _sel=${MENU_SEL:-0} _i _k _k2 _d      # underscores: see prompt
    shift
    [ -n "$TTY" ] || { printf -v "$_var" '%s' $_sel; return 0; }
    # line wrap off while it is drawn: the redraw goes up one line per item, so an item wider than the terminal
    # has to be cut, not wrapped
    printf '\e[?25l\e[?7l'
    while :; do
        for ((_i = 0; _i < _n; _i++)); do
            if [ -z "${@:_i+1:1}" ]; then printf '\r\e[K\n'
            elif [ $_i = $_sel ]; then printf '\r\e[K  %s❯ %s%s\n' "$CYN$B" "${@:_i+1:1}" "$N"
            else printf '\r\e[K    %s\n' "${@:_i+1:1}"; fi
        done
        IFS= read -rsn1 _k < /dev/tty || exit 1
        _d=0
        case $_k in
        $'\e') read -rsn2 -t 0.1 _k2 < /dev/tty; case $_k2 in '[A') _d=-1;; '[B') _d=1;; esac;;
        k) _d=-1;;
        j) _d=1;;
        [1-9]) [ "$_k" -le $_n ] && [ -n "${@:_k:1}" ] && _sel=$((_k - 1));;
        '') break;;
        esac
        # step over blank lines
        [ $_d != 0 ] && { _sel=$(( (_sel + _n + _d) % _n )); while [ -z "${@:_sel+1:1}" ]; do _sel=$(( (_sel + _n + _d) % _n )); done; }
        printf '\e[%dA' $_n
    done
    printf '\e[?25h\e[?7h'
    printf -v "$_var" '%s' $_sel
}

# checklist VAR ITEM...: tick any number of items (Space or the item's number toggles one, a: all or none), Enter to
# go on; VAR gets the indices of the ticked ones, space separated.  All ticked at first except the indices in
# $CHECK_OFF.  Without a terminal: that default.
checklist() {
    local _var=$1 _n=$(($# - 1)) _sel=0 _i _k _k2 _box _all _on=() _out=      # underscores: see prompt
    shift
    for ((_i = 0; _i < _n; _i++)); do _on[_i]=1; done
    for _i in $CHECK_OFF; do _on[_i]=0; done
    if [ -n "$TTY" ]; then
        printf '\e[?25l\e[?7l'                  # line wrap off: see menu
        while :; do
            for ((_i = 0; _i < _n; _i++)); do
                [ ${_on[_i]} = 1 ] && _box="[$GRN✓$N]" || _box='[ ]'
                if [ $_i = $_sel ]; then printf '\r\e[K  %s❯%s %s %s%s%s\n' "$CYN$B" "$N" "$_box" "$B" "${@:_i+1:1}" "$N"
                else printf '\r\e[K    %s %s\n' "$_box" "${@:_i+1:1}"; fi
            done
            printf '\r\e[K\n\r\e[K    %s↑↓ move · Space tick · a all/none · Enter go on%s\n' "$DIM" "$N"
            IFS= read -rsn1 _k < /dev/tty || exit 1
            case $_k in
            $'\e') read -rsn2 -t 0.1 _k2 < /dev/tty; case $_k2 in '[A') _sel=$(( (_sel + _n - 1) % _n ));; '[B') _sel=$(( (_sel + 1) % _n ));; esac;;
            k) _sel=$(( (_sel + _n - 1) % _n ));;
            j) _sel=$(( (_sel + 1) % _n ));;
            ' ') _on[_sel]=$((1 - _on[_sel]));;
            [1-9]) [ "$_k" -le $_n ] && { _sel=$((_k - 1)); _on[_sel]=$((1 - _on[_sel])); };;
            a) _all=1; for ((_i = 0; _i < _n; _i++)); do [ ${_on[_i]} = 1 ] || _all=0; done
               for ((_i = 0; _i < _n; _i++)); do _on[_i]=$((1 - _all)); done;;
            '') break;;
            esac
            printf '\e[%dA' $((_n + 2))
        done
        printf '\e[?25h\e[?7h'
    fi
    for ((_i = 0; _i < _n; _i++)); do [ ${_on[_i]} = 1 ] && _out="$_out $_i"; done
    printf -v "$_var" '%s' "${_out# }"
}

# need_sudo "why": ask for the sudo password now, not in the middle of a task
need_sudo() {
    [ -n "$DRY" ] && return 0
    sudo -n true 2>/dev/null && return 0
    info "sudo: $1"
    sudo -v < /dev/tty
}

# package that provides TOOL, for the package manager MGR
_pkg() {
    case $2:$1 in
    pacman:adb|pacman:fastboot|dnf:adb|dnf:fastboot) echo android-tools;;
    pacman:pyusb) echo python-pyusb;;  apt:pyusb) echo python3-usb;;  dnf:pyusb) echo python3-pyusb;;
    pacman:python3) echo python;;
    *:debugfs) echo e2fsprogs;;
    pacman:7z) echo p7zip;;  apt:7z) echo p7zip-full;;  dnf:7z) echo p7zip p7zip-plugins;;
    pacman:sqlite3|dnf:sqlite3) echo sqlite;;
    *:cc) echo gcc;;
    *:sha256sum) echo coreutils;;
    apt:xz) echo xz-utils;;
    *) echo "$1";;
    esac
}
_have() { if [ "$1" = pyusb ]; then python3 -c 'import usb' 2>/dev/null; else command -v "$1" > /dev/null; fi; }

# need_tools TOOL...: all there?  If not, offers to install them with the system's package manager.  TOOL is a command,
# or pyusb (the python3 module).
need_tools() {
    local t miss=() mgr= cmd pk
    for t; do _have "$t" || miss+=("$t"); done
    [ ${#miss[@]} = 0 ] && { ok "all tools there"; return 0; }
    for t in pacman apt-get dnf; do command -v $t > /dev/null && { mgr=$t; break; }; done
    fail "missing: ${miss[*]}"
    case $mgr in
    pacman) cmd="pacman -S --needed --noconfirm";;
    apt-get) cmd="apt-get install -y"; mgr=apt;;
    dnf) cmd="dnf install -y";;
    *) info "install them with your package manager, then try again"; return 1;;
    esac
    pk=$(for t in "${miss[@]}"; do _pkg "$t" $mgr; done | tr ' ' '\n' | sort -u | tr '\n' ' ')
    ask "Install ${pk% } with $mgr?" || return 1
    need_sudo "to install packages" || return 1
    task "Installing ${pk% }" sudo $cmd $pk || return 1
    [ -n "$DRY" ] && return 0
    for t in "${miss[@]}"; do _have "$t" || { fail "still missing: $t"; return 1; }; done
}

# need_files DEST SHA256 "where to get it" [DEST SHA256 "where"]...: waits until every DEST is there with its checksum.
# Files are picked up from ~/Downloads or the repository root the moment they land there (not while a browser is still
# writing them), so the user only has to download them.
declare -A _seen
_good() { [ -z "$2" ] || [ "$(sha256sum < "$1" | cut -c1-64)" = "$2" ]; }
_pickup() {
    local f=$1 sum=$2 name=${1##*/} c sig
    for c in "$HOME/Downloads/$name" "./$name"; do
        [ -s "$c" ] && [ ! -e "$c.part" ] && [ ! -e "$c.crdownload" ] || continue
        sig=$(stat -c %s.%Y "$c"); [ "${_seen[$c]}" = "$sig" ] && continue
        _seen[$c]=$sig
        if _good "$c" "$sum"; then mkdir -p "${f%/*}"; mv "$c" "$f"; return 0; fi
        _clr; fail "$c is not the right file (checksum); download it again"
    done
    return 1
}
need_files() {
    local dest=() sum=() src=() miss=() left lines=() i t0=$SECONDS
    while [ $# -ge 3 ]; do dest+=("$1"); sum+=("$2"); src+=("$3"); shift 3; done
    for i in "${!dest[@]}"; do
        if [ -f "${dest[i]}" ] && ! _good "${dest[i]}" "${sum[i]}"; then
            mv "${dest[i]}" "${dest[i]}.wrong"; warn "${dest[i]##*/} is not the right file; moved aside to ${dest[i]}.wrong"
        fi
        [ -f "${dest[i]}" ] || _pickup "${dest[i]}" "${sum[i]}" || { miss+=($i); continue; }
        ok "${dest[i]##*/}"
    done
    [ ${#miss[@]} = 0 ] && return 0
    for i in "${miss[@]}"; do lines+=("$B${dest[i]##*/}$N" "  $DIM${src[i]}$N"); done
    tell "Download into ~/Downloads (picked up from there)" "${lines[@]}"
    [ -n "$DRY" ] && return 0
    while [ ${#miss[@]} -gt 0 ]; do
        left=()
        for i in "${miss[@]}"; do
            if _pickup "${dest[i]}" "${sum[i]}"; then _clr; ok "${dest[i]##*/}"; else left+=($i); fi
        done
        miss=("${left[@]}")
        [ ${#miss[@]} = 0 ] && break
        for i in {1..20}; do _tick "Waiting for ${#miss[@]} download(s)" $t0; sleep 0.1; done
    done
    _clr
}

# build_mode: where the Echo's binaries come from (scripts/lib/build.sh), asked once and kept in $OUT/setup.build: the
# release build CI made of this commit, where GitHub has one (nothing to compile, so no NDK, kernel tools or firmware
# unpacking), or a build here.  Sets BUILD_MODE and exports PREBUILT for the scripts the steps run.
build_mode() {
    local t m
    if [ -f $OUT/setup.build ]; then m=$(cat $OUT/setup.build)
    elif t=$(prebuilt_tag); then
        ok "GitHub has a release build of this commit ($t)"
        menu m "Use it (recommended): nothing to compile" "Build it here (Android NDK, 1 GB, and a compiler)"
        [ "$m" = 0 ] && m=prebuilt || m=source
    else
        info "no release build of this commit on GitHub (changes here, or not published yet): it is built here"
        m=source
    fi
    [ -n "$DRY" ] || echo $m > $OUT/setup.build
    build_mode_set $m
}
build_mode_set() { BUILD_MODE=$1; if [ "$1" = prebuilt ]; then export PREBUILT=1; else export PREBUILT=0; fi; }

# kernel_tools: what Wi-Fi motion's kernel module is built with (make kernel-tools: the model's kernel sources and the
# compiler Amazon used), so that the build has it as CI's does.  Without them `make` quietly leaves the module out, and
# the install once stopped over the missing file (issue #5).
kernel_tools() {
    [ -z "$DRY" ] && KERNEL_TOOLS_CHECK=1 make -s kernel-tools DEVICE=$DEVICE > /dev/null 2>&1 &&
        { ok "kernel sources and compiler (Wi-Fi motion)"; return 0; }
    mkdir -p toolchain
    TASK_NOTE="du -ch toolchain/.kernel-tools.* 2>/dev/null | tail -1 | cut -f1" \
        task "Downloading kernel sources and compiler for Wi-Fi motion (115 MB)" make -s kernel-tools DEVICE=$DEVICE
}

# USB_ONLY (the guided setup): the Echo being set up is the one on USB; installed ones on adb over Wi-Fi must not be
# mistaken for it.  Its serial is looked up again each time (TWRP may report another one than Fire OS) and exported, so
# the scripts the steps run talk to it as well.
usb_serial() {
    local s
    s=$(adb -d get-serialno 2>/dev/null)
    if [ -n "$s" ] && [ "$s" != unknown ]; then export ANDROID_SERIAL=$s; else unset ANDROID_SERIAL; return 1; fi
}

# With several Echos on adb, one has to be picked, as for every script (ANDROID_SERIAL).
pick_serial() {
    local list s
    [ -n "$ANDROID_SERIAL" ] || [ -n "$DRY" ] || [ -n "$USB_ONLY" ] && return 0
    list=($(adb devices 2>/dev/null | awk 'NR > 1 && $2 ~ /^(device|recovery)$/ { print $1 }'))
    [ ${#list[@]} -gt 1 ] || return 0
    say "More than one Echo is on adb. Which one?"
    menu s "${list[@]}"
    export ANDROID_SERIAL=${list[s]}
}
# adb_is device|recovery: the Echo on adb in that state?  Over Wi-Fi (ANDROID_SERIAL host:port) adb does not come back
# by itself after a reboot, so it is reconnected here.  ADB_OFFLINE counts the checks in a row that found it "offline"
# even after reconnecting: the Echo's adbd takes the connection and never answers.  Seen on an Echo Dot 3 after the
# Alexa app moved it to another Wi-Fi network and back (2026-10-05); only a power cycle brought adb back.
adb_is() {
    local s
    [ -n "$USB_ONLY" ] && { usb_serial || return 1; }
    s=$(adb get-state 2>/dev/null)
    if [ "$s" != "$1" ] && [[ $ANDROID_SERIAL == *:* ]]; then
        [ "$s" = offline ] && adb disconnect "$ANDROID_SERIAL" > /dev/null 2>&1
        timeout 5 adb connect "$ANDROID_SERIAL" > /dev/null 2>&1; s=$(adb get-state 2>/dev/null)
    fi
    [ "$s" = offline ] && ADB_OFFLINE=$((${ADB_OFFLINE:-0} + 1)) || ADB_OFFLINE=0
    [ "$s" = "$1" ]
}

# wait_adb device|recovery: until the Echo is on adb in that state
wait_adb() {
    pick_serial
    case $1 in
    recovery) waitfor "Waiting for TWRP (white ring)|TWRP is up" "adb_is recovery" "Nothing yet? Unplug the Echo's power and plug it back in.";;
    *) waitfor "Waiting for the Echo to boot|Echo is up" "adb_is device" "Nothing yet? Unplug the Echo's power and plug it back in.";;
    esac
}

# ashell CMD: adb shell, output without the carriage returns
ashell() { adb shell "$@" | tr -d '\r'; }

# install_satellite: the last step of every model.  Asks the name, installs (scripts/install-system.sh), waits until the
# installed satellite runs.  Name and address go to $OUT/setup.env for the final screen.  There is no trial run before
# it: run.sh does not read hassmic.conf, so a trial announced itself under the default name, and Home Assistant kept that
# device next to the installed one.
install_satellite() {
    local name ip tok
    prompt name "Name for this Echo in Home Assistant" "$DEFAULT_NAME"
    wait_adb device || return 1
    task "Installing (the Echo reboots)" scripts/install-system.sh "$name" || return 1
    [ -n "$DRY" ] || sleep 10
    wait_adb device || return 1
    # main.sh restarts a hassmic that dies, so it has to be seen twice, 10 s apart, to not be a crash loop
    waitfor "Waiting for the satellite to start|Satellite started" '[ -n "$(ashell pidof hassmic)" ]' "" 60 180 &&
        task "Checking it stays up" sh -c 'sleep 10; adb shell pidof hassmic | grep -q .' ||
        { [ -n "$DRY" ] || { info "end of its log:"; ashell tail -8 /data/local/hassmic/boot.log | sed 's/^/    /'; }; return 1; }
    [ -n "$DRY" ] && return 0
    ip=$(ashell ifconfig $WLAN | sed -n 's/.*inet addr:\([0-9.]*\).*/\1/p')     # toybox ifconfig; no ip on the Echo
    # hassmic logs its Sendspin pairing token at every start
    tok=$(ashell "grep -o 'SP:0[A-Z0-9]*' /data/local/hassmic/boot.log" | tail -1)
    printf 'SAT_NAME=%q\nSAT_IP=%q\nSAT_TOKEN=%q\n' "$name" "$ip" "$tok" > $OUT/setup.env
}

# done_screen [line]...: the end: what to do in Home Assistant, then the model's own notes
done_screen() {
    local SAT_NAME=$DEFAULT_NAME SAT_IP= SAT_TOKEN=
    [ -f $OUT/setup.env ] && . $OUT/setup.env
    printf '  %s%s"%s" is running%s%s\n' "$GRN$B" "✓ " "$SAT_NAME" "${SAT_IP:+ at $SAT_IP}" "$N"
    tell "Add it in Home Assistant" \
        "Settings → Devices & services → discovered ESPHome \"$SAT_NAME\" → Add" \
        "${DIM}(not listed? Add ESPHome by hand: ${SAT_IP:-its IP}, port 26053, no encryption key)$N" \
        "Then pick an Assist pipeline (and the wake word) in its settings."
    # wake word arbitration hands its network key over as an ESPHome action: without the permission a second Echo
    # never joins, and both answer
    tell "Allow it to perform Home Assistant actions" \
        "ESPHome → \"$SAT_NAME\" → Configure → \"Allow the device to perform Home Assistant actions\"" \
        "${DIM}With several Echos, only the one that heard the wake word best answers; this is how they agree.$N"
    [ -n "$SAT_TOKEN" ] && tell "Music Assistant (optional)" \
        "It finds the Echo by itself; to play on it, pair it with this token:" \
        "$B$SAT_TOKEN$N" \
        "${DIM}Or switch on \"Music Assistant without pairing\" on the settings page (any server on the LAN may then play).$N"
    warn "Back up secrets/update.key: it signs your updates."
    local l; for l; do info "$l"; done
    printf '\n'
}
