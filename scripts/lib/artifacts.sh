# Amazon's artifacts for an installed Echo, shared by scripts/artifacts.sh and the last step of scripts/setup.sh.  bash;
# sourced from the repository root after scripts/lib/device.sh and scripts/lib/setup.sh.
# Two kinds, both from Amazon's DAVS (Device Artifact Vending Service) and the same for every Echo: wake word model sets, installed on the Echo (Home
# Assistant then offers each in the Echo's wake word select), and other models, installed on the Echo as well: whisper
# detection (Home Assistant's binary sensor "Last request whispered", docs/re-whisper.md), and Amazon's newest sound
# detection model in place of the firmware's (docs/re-aed.md).  Ones fetched before (device-logs/models/, git-ignored) are only copied over.  Each
# wake word set is first loaded by the Echo's own engine (pryon_test): an older engine (radar) cannot load every set.
# The user ticks everything wanted first (a menu with a checklist per kind); then one run does it all.  Downloading needs the Echo registered to an
# Amazon account once: it runs stock Alexa with the updaters cut off (MODE=stock-online) until then, and everything is
# undone afterwards (registration, the Wi-Fi the Alexa app added, the mode).  Stopped halfway, a new run finds the Echo
# in stock-online mode and goes on there.
# Over Wi-Fi the Echo's adb has to be open (scripts/adb-wifi.sh, or debug access on the settings page).  The two reboots of
# the Amazon way would close it, so for that way it is kept open with ADB_WIFI=1 in hassmic.conf, marked as ours, until
# the end.

MODELS=device-logs/models
D=/data/local/hassmic
MAPDB=/data/ace/kvstorage/map.db
ADB_OURS="ADB_WIFI=1 # artifacts.sh"
ADB_OURS_OLD="ADB_WIFI=1 # wakeword.sh"   # the marker before the rename; an Echo stopped halfway may still carry it
WW_KEYS=(echo computer amazon ziggy alexa)
LOCALES=(de-DE en-US en-GB fr-FR it-IT es-ES ja-JP pt-BR en-CA fr-CA en-AU en-IN es-MX)

# m_stage ID: on its own, a screen per step of M_PLAN; inside the setup (M_SETUP set) only a line
m_stage() {
    local i list=() cur=0
    if [ -n "$M_SETUP" ]; then
        for ((i = 0; i < ${#M_PLAN[@]}; i += 2)); do
            [ "${M_PLAN[i]}" = "$1" ] && [ "$1" != connect ] && [ "$1" != choose ] && printf '\n  %s%s%s\n' "$B" "${M_PLAN[i+1]}" "$N"
        done
        return 0
    fi
    DONE_IDS=
    for ((i = 0; i < ${#M_PLAN[@]}; i += 2)); do
        list+=("${M_PLAN[i]}|${M_PLAN[i+1]}"); [ "${M_PLAN[i]}" = "$1" ] && cur=${#list[@]}
        [ $cur = 0 ] && DONE_IDS="$DONE_IDS ${M_PLAN[i]}"
    done
    [ "$1" = done ] && cur=$((${#list[@]} + 1))
    header $cur ${#list[@]} "${list[@]}"
}

# model id "echo-de-DE" -> "Echo (de-DE)", "aed-EU" -> "Sound detection (EU)", "whisper-en-US" -> "Whisper detection"
label() {
    local k=${1%%-*}
    case $k in aed) k=sound\ detection;; whisper) printf 'Whisper detection'; return;; esac
    printf '%s (%s)' "${k^}" "${1#*-}"
}
is_aed() { [ "${1%%-*}" = aed ]; }
is_whisper() { [ "${1%%-*}" = whisper ]; }
# DAVS keeps the sound detection model by region, not by language (tools/davs-fetch.py)
region() { case $1 in en-US|en-CA|fr-CA|es-MX|pt-BR) echo NA;; ja-JP|en-AU|en-IN) echo FE;; *) echo EU;; esac; }
wpa() { ashell "wpa_cli -i $WLAN -p $WPA_SOCKETS $*"; }
net_ids() { wpa list_networks | awk 'NR > 1 { print $1 }' | tr '\n' ' '; }
# token: the registered Echo's access token, pulled into $TMP/map.db, and one DAVS takes.  A registration left behind by
# an older run keeps its expired token in map.db (DAVS: HTTP 403), so being there is not enough.
token() {
    rm -f $TMP/map.db*
    for f in $(ashell "ls $MAPDB*" 2>/dev/null); do adb pull "$f" $TMP/ > /dev/null 2>&1; done
    [ -n "$(sqlite3 $TMP/map.db "select value from deviceData where key='access_token'" 2>/dev/null)" ] &&
        python3 tools/davs-fetch.py $TMP/map.db check > /dev/null 2>&1
}
# update_block: a firmware update would cost the unlock, so nothing goes on without main.sh's guard in place
update_block() {
    waitfor "Waiting for the update block|Firmware updates blocked" \
        "ashell iptables -S hassmic_out | grep -q 'uid-owner.*-j DROP'" "" 60 120 ||
        { fail "the update block is not in place: unplug the Echo's power and run this again"; return 1; }
}
satellite_up() { waitfor "Waiting for the satellite|Satellite running" '[ -n "$(ashell pidof hassmic)" ]' "" 60 180; }

# install_whisper ID: into $D/whisper, apart from the wake words: the model carries a stray pryon.manifest (speaker ID)
# that the scan of $D/models would take for one.  The hassmic started afterwards loads it.
install_whisper() {
    local id=$1 t=$D/.whisper-try
    task "Copying $(label $id)" sh -c "adb shell 'rm -rf $t' && adb push $MODELS/$id/unpacked $t && adb shell 'chmod -R a+rX,go-w $t && rm -rf $D/whisper && mv $t $D/whisper'" || return 1
    INSTALLED+=("$id")
}

# install_aed ID: Amazon's newest sound detection model into $D/aed; hassmic takes it in place of the firmware's
# whenever sound detection starts (sound_pryon.c), and the firmware's again if it does not load.
install_aed() {
    local id=$1 t=$D/.aed-try
    task "Copying $(label $id)" sh -c "adb shell 'rm -rf $t' && adb push $MODELS/$id/unpacked/AED $t && adb shell 'chmod -R a+rX,go-w $t && rm -rf $D/aed && mv $t $D/aed'" || return 1
    INSTALLED+=("$id")
}

# install_model ID: load it with the Echo's engine first; only a set that loads goes into $D/models (hidden until then:
# hassmic skips names starting with a dot)
install_model() {
    local id=$1 t=$D/models/.try-$1
    task "Copying $(label $id)" sh -c "adb shell 'rm -rf $t; mkdir -p $D/models' && adb push $MODELS/$id/unpacked $t && adb shell chmod -R a+rX,go-w $t" || return 1
    # pryon_test: 1 = the set did not load, 3 = loaded but heard nothing (there is no audio)
    if ! task "Trying it on this Echo's engine" sh -c "adb shell '$PT -m $t/pryon.manifest /dev/null > /dev/null 2>&1; echo rc=\$?' | tee /dev/stderr | grep -qv rc=1"; then
        ashell "rm -rf $t"
        fail "$(label $id) does not work with this Echo's wake word engine"
        return 1
    fi
    ashell "rm -rf $D/models/$id; mv $t $D/models/$id"
    INSTALLED+=("$id")
}

# installed under its own name, or by hand under the short one (echo-de)
have() { [[ $HAVE == *" $1 "* || $HAVE == *" ${1%-*} "* ]]; }

# build_lists: the wake word and the other artifacts lists for $LOC, all ticked but the sound detection model (the
# newer one scored as the firmware's on every test clip).  That one is always offered: Amazon may have a newer one
# since (davs-fetch.py says which).
build_lists() {
    local m id k
    WW_IDS=() WW_ITEMS=() OT_IDS=() OT_ITEMS=()
    for m in $MODELS/*/unpacked/pryon.manifest; do        # downloaded before, not on this Echo yet
        [ -f "$m" ] || continue
        id=${m#$MODELS/}; id=${id%%/*}
        { is_aed $id || is_whisper $id || have $id; } && continue      # those may carry a pryon.manifest too
        WW_IDS+=("$id"); WW_ITEMS+=("$(label $id) ${DIM}· downloaded before: install it$N")
    done
    for k in "${WW_KEYS[@]}"; do
        id=$k-$LOC
        [ -d $MODELS/$id/unpacked ] || have $id && continue
        WW_IDS+=("$id"); WW_ITEMS+=("$(label $id) ${DIM}· download from Amazon and install it$N")
    done
    # one model for every language, from DAVS for en-US only (tools/davs-fetch.py); one fetched before under a
    # locale's name (whisper-de-DE) is the same file
    id=whisper-en-US
    for m in $MODELS/whisper-*/unpacked/pryon_whisper.manifest; do [ -f "$m" ] && { id=${m#$MODELS/}; id=${id%%/*}; break; }; done
    OT_IDS+=("$id")
    if [ -n "$HAVE_WHISPER" ]; then OT_ITEMS+=("$(label $id) ${DIM}· on this Echo already: install again$N")
    elif [ -d $MODELS/$id/unpacked ]; then OT_ITEMS+=("$(label $id) ${DIM}· downloaded before: install it$N")
    else OT_ITEMS+=("$(label $id) ${DIM}· download and install: sensor \"Last request whispered\"$N"); fi
    id=aed-$(region $LOC)               # by region, not language (davs-fetch.py)
    OT_IDS+=("$id")
    if [ -n "$HAVE_AED" ]; then OT_ITEMS+=("$(label $id) ${DIM}· Amazon's newest on this Echo already: fetch again, may be newer$N")
    else OT_ITEMS+=("$(label $id) ${DIM}· Amazon's newest in place of the firmware's (scores the same so far)$N"); fi
    WW_ON=$(seq -s ' ' 0 $((${#WW_IDS[@]} - 1)) 2>/dev/null)
    OT_ON=; [ -n "$HAVE_WHISPER" ] || OT_ON=0     # whisper unless it is there; never the sound detection model
}
# ticked ONVAR IDSVAR: "3 of 5 ticked"
ticked() {
    local -n _on=$1 _ids=$2; local n=0 i
    [ ${#_ids[@]} = 0 ] && { printf 'nothing new'; return; }
    for i in $_on; do n=$((n + 1)); done
    printf '%d of %d ticked' $n ${#_ids[@]}
}
# sub_list "title" WW|OT: the checklist of one kind, starting from its ticks so far
sub_list() {
    local -n _ids=$2_IDS _items=$2_ITEMS _on=$2_ON; local i off= r
    m_stage choose
    say "$1"; printf '\n'
    if [ ${#_ids[@]} = 0 ]; then
        info "nothing new for $LOC: every one is on this Echo or on the PC already"
        [ -n "$TTY" ] && { printf '\n  %sPress Enter.%s ' "$DIM" "$N"; read -r _ < /dev/tty; }
        return 0
    fi
    for ((i = 0; i < ${#_ids[@]}; i++)); do [[ " $_on " == *" $i "* ]] || off="$off $i"; done
    CHECK_OFF=$off checklist r "${_items[@]}"
    _on=$r
}

# fetch_model ID: one download from DAVS with the token in $TMP/map.db
fetch_model() {
    local id=$1 key=${1%%-*} loc=${1#*-} ecids=$ECIDS
    is_aed $id && { ecids=$AED_ECIDS; loc=$LOC; }
    [ $key = whisper ] && ecids=          # davs-fetch.py has its own request
    task "Downloading $(label $id)" python3 tools/davs-fetch.py ${ecids:+--ecids $ecids} $TMP/map.db $key $loc $MODELS
}

# artifacts_run [setup]: the whole thing; with "setup" as a step of scripts/setup.sh (the Echo on adb is the one just
# installed, and the wake word stays "Alexa" unless something is ticked)
artifacts_run() {
    local r
    M_SETUP=$1 M_PLAN=(connect "Connect" choose "Choose") INSTALLED=() FETCHED=() FAILED=()
    if [ -n "$DRY" ]; then info "offers the wake words and whisper detection in $MODELS/ and Amazon's; the dry run keeps \"Alexa\" and adds nothing"; return 0; fi
    TMP=$(mktemp -d)                  # map.db is the account's device credential: never kept on the PC
    _artifacts_run; r=$?
    rm -rf "$TMP"
    return $r
}

_artifacts_run() {
    # --- connect
    m_stage connect
    if [ -z "$M_SETUP" ]; then
        # the same Echo on USB too: USB it is, as over Wi-Fi it is out of reach while stock waits in setup mode.  The
        # same = the Wi-Fi one's serial, or (Wi-Fi unreachable) the USB one is halfway through a run (stock-online)
        local usb=$(adb -d get-serialno 2>/dev/null) wifi
        if [[ $ANDROID_SERIAL == *:* ]] && [ -n "$usb" ] && [ "$usb" != unknown ]; then
            wifi=$(timeout 5 adb get-serialno 2>/dev/null)
            if [ "$wifi" = "$usb" ] || { [ -z "$wifi" ] &&
                adb -s "$usb" shell "grep -q '^MODE=stock-online' $D/hassmic.conf" 2>/dev/null; }; then
                export ANDROID_SERIAL=$usb; info "this Echo is on USB too: using USB ($usb)"
            fi
        fi
        pick_serial
        waitfor "Waiting for the Echo on adb|Echo on adb" "adb_is device" \
            "Nothing? Connect it by USB, or open adb over Wi-Fi (scripts/adb-wifi.sh <echo-ip>) and give its address: scripts/artifacts.sh <echo-ip>. Orange ring? A run stopped halfway left it a stock Echo waiting for the Alexa app: set it up there (Devices → + → Add device → Amazon Echo), then scripts/artifacts.sh <its address> goes on." 15 || return 1
        device_load adb
        MODEL_NAME="Artifacts · $MODEL_NAME"
    else
        wait_adb device || return 1
    fi
    [ "$(ashell id -u)" = 0 ] || { fail "adb shell is not root: is this Echo set up with scripts/setup.sh?"; return 1; }
    [ -n "$(ashell "ls $D/hassmic.conf 2>/dev/null")" ] ||
        { fail "no hassmic installed on this Echo: scripts/setup.sh first"; return 1; }
    PT=
    for p in $D/pryon_test /system/hassmic/pryon_test; do [ -n "$(ashell "ls $p 2>/dev/null")" ] && { PT=$p; break; }; done
    if [ -z "$PT" ]; then
        [ -f build/$DEVICE/pryon_test ] || { fail "no pryon_test on the Echo or in build/$DEVICE: make DEVICE=$DEVICE"; return 1; }
        adb push build/$DEVICE/pryon_test /data/local/tmp/ > /dev/null && PT=/data/local/tmp/pryon_test
    fi
    ATTRS=$(ashell "$PT -m /nonexistent /dev/null 2>&1")
    ECIDS=$(grep -o '"wakeword_ecids":\[[0-9,]*\]' <<< "$ATTRS" | grep -o '[0-9][0-9,]*')
    AED_ECIDS=$(grep -o '"aed_ecids":\[[0-9,]*\]' <<< "$ATTRS" | grep -o '[0-9][0-9,]*')
    NETS=build/artifacts-$(adb get-serialno | tr -c 'A-Za-z0-9\n' _).nets      # Wi-Fi networks before the Alexa app
    [ -f $NETS ] || [ ! -f ${NETS/artifacts-/wakeword-} ] || mv ${NETS/artifacts-/wakeword-} $NETS   # a run before the rename
    ONLINE=; [ -n "$(ashell "grep '^MODE=stock-online' $D/hassmic.conf")" ] && ONLINE=1
    HAVE=" $(ashell "ls $D/models 2>/dev/null" | tr '\n' ' ') "
    HAVE_WHISPER=$(ashell "ls $D/whisper/pryon_whisper.manifest 2>/dev/null")
    HAVE_AED=$(ashell "ls $D/aed/pryon.manifest 2>/dev/null")
    [ -n "$M_SETUP" ] || ok "$MODEL_NAME${ANDROID_SERIAL:+, $ANDROID_SERIAL}"
    [ -n "$ONLINE" ] && warn "This Echo is in stock-online mode from an earlier run: going on with that."

    # --- choose: a main menu with a checklist per kind; the language decides which Amazon artifacts are new
    local def=0 i c id lists_for= WW_IDS WW_ITEMS WW_ON OT_IDS OT_ITEMS OT_ON
    for i in "${!LOCALES[@]}"; do [ "${LOCALES[i]//-/_}" = "${LANG%%.*}" ] && def=$i; done    # the PC's language first
    LOC=${LOCALES[def]}
    while :; do
        [ "$lists_for" = "$LOC" ] || { build_lists; lists_for=$LOC; }
        m_stage choose
        [ -n "${HAVE// }" ] && info "On this Echo already:$HAVE"
        say "Choose what to do ${DIM}(Enter opens an entry)$N"; printf '\n'
        # the language first and apart: it decides what the lists below it offer
        local entries=("Language for downloads from Amazon: $B$LOC$N  ${DIM}· decides what the lists below offer$N" ""
                       "Wake words …        ${DIM}$(ticked WW_ON WW_IDS)$N") acts=(lang - ww)
        entries+=("Other artifacts …   ${DIM}$(ticked OT_ON OT_IDS)$N"); acts+=(ot)
        entries+=("" "Go on ${DIM}· shows what will happen first$N"); acts+=(- go)
        MENU_SEL=${MAIN_SEL:-0} menu c "${entries[@]}"; MAIN_SEL=$c
        case ${acts[c]} in
        ww)  sub_list "Wake words ${DIM}(installed on the Echo; Home Assistant offers each in its wake word select)$N" WW ;;
        ot)  sub_list "Other artifacts ${DIM}(installed on the Echo)$N" OT ;;
        lang) m_stage choose; say "Language for downloads from Amazon:"; printf '\n'
              MENU_SEL=$(for i in "${!LOCALES[@]}"; do [ "${LOCALES[i]}" = "$LOC" ] && echo $i; done) menu c "${LOCALES[@]}"
              LOC=${LOCALES[c]} ;;
        go)  break ;;
        esac
    done
    WANT=() AMAZON=()
    for i in $WW_ON; do WANT+=("${WW_IDS[i]}"); [ -d $MODELS/${WW_IDS[i]}/unpacked ] || AMAZON+=("${WW_IDS[i]}"); done
    for i in $OT_ON; do                 # the sound detection model is always fetched again (the newest); whisper only when not on the PC
        WANT+=("${OT_IDS[i]}"); is_aed ${OT_IDS[i]} || [ ! -d $MODELS/${OT_IDS[i]}/unpacked ] && AMAZON+=("${OT_IDS[i]}")
    done
    if [ ${#WANT[@]} = 0 ] && [ -z "$ONLINE" ]; then
        [ -n "$M_SETUP" ] && ok "wake word: Alexa" || info "nothing ticked"
        return 0
    fi

    # the plan, shown before anything changes
    local need_amazon=; [ ${#AMAZON[@]} -gt 0 ] || [ -n "$ONLINE" ] && need_amazon=1
    local n_install=${#WANT[@]}
    [ -n "$need_amazon" ] && M_PLAN+=(online "Online with Amazon" register "Register in the Alexa app" fetch "Download from Amazon")
    [ $n_install -gt 0 ] && M_PLAN+=(install "Install")
    [ -n "$need_amazon" ] && M_PLAN+=(back "Back to satellite")
    printf '\n'; say "This will:"
    for id in "${WANT[@]}"; do
        if is_aed $id; then say "  · download Amazon's newest $(label $id) model and install it"
        elif is_whisper $id && [[ " ${AMAZON[*]} " == *" $id "* ]]; then say "  · download $(label $id) and install it"
        elif is_whisper $id; then say "  · install $(label $id)"
        elif [[ " ${AMAZON[*]} " == *" $id "* ]]; then say "  · download wake word $(label $id) and install it"
        else say "  · install wake word $(label $id)"; fi
    done
    [ -n "$ONLINE" ] && say "  · finish the earlier run: back to satellite, then you deregister it"
    [ -n "$need_amazon" ] && info "Downloading needs this Echo registered to your Amazon account for a few minutes (Alexa app); it is undone at the end."
    ask "Go on?" y || return 0

    if [ -n "$need_amazon" ]; then
        need_tools python3 sqlite3 || return 1
        # --- online: stock Alexa with internet, updaters cut off
        m_stage online
        if [ -z "$ONLINE" ]; then
            # the Alexa app adds its own Wi-Fi network; the ones there now are kept, the rest is removed at the end
            net_ids > $NETS
            ashell "sed -i '/^MODE=/d' $D/hassmic.conf; echo MODE=stock-online >> $D/hassmic.conf"
            [[ $ANDROID_SERIAL == *:* ]] && ashell "grep -q '^ADB_WIFI=' $D/hassmic.conf || echo '$ADB_OURS' >> $D/hassmic.conf"
            task "Restarting the Echo as a stock Echo" adb reboot || return 1
            sleep 10
        fi
        # --- register.  Unregistered, stock comes up in setup mode.  The Echo Dot 2 drops the Wi-Fi network (dhcpcd
        # killed, network disabled) and opens its own access point for the Alexa app (2026-10-04); over Wi-Fi it is back
        # on adb only once the app has set it up, so the app comes first and the connection is checked after it.  The
        # Echo Dot 3 stays on the Wi-Fi network, and the app then moved it to a network of its own and back (2026-10-05).
        m_stage register
        [[ $ANDROID_SERIAL == *:* ]] || { wait_adb device || return 1; }
        local guarded=
        if adb_is device; then update_block || return 1; guarded=1; fi
        if [ -n "$guarded" ] && token; then ok "registered already"
        else
            tell "Set the Echo up in the Alexa app" \
                "Devices → + → Add device → Amazon Echo, on the Wi-Fi Home Assistant is on." \
                "The app may show \"updating\" for a while: that is the blocked update check, it is fine." \
                "The Echo needs internet access: if your router blocks it, allow it until this is done." \
                "Leave it registered until this script asks you to deregister it: deregistered while it runs as a stock Echo, it resets itself to factory settings."
            if [ -z "$guarded" ]; then
                info "Until the app has set it up, the Echo is off your Wi-Fi (it runs its own setup network)."
                waitfor "Waiting for the Echo back on Wi-Fi|Echo is back on Wi-Fi" "adb_is device" \
                    "Set up in the app and still nothing? It may have another address now: Ctrl-C, then scripts/artifacts.sh <new-ip>." 600
                update_block || return 1
            fi
            waitfor "Waiting for the registration|Registered" "adb_is device && token" || return 1      # adb_is: reconnects over Wi-Fi
        fi

        # --- fetch: every download in one go, a failed one does not stop the rest
        m_stage fetch
        for id in "${AMAZON[@]}"; do
            if fetch_model $id; then FETCHED+=("$id"); else FAILED+=("$id"); fi
        done
        rm -f $TMP/map.db*
        [ ${#FAILED[@]} -gt 0 ] &&
            info "Not downloaded: ${FAILED[*]} (why: above). Run this again to try again."
    fi

    # --- install everything that is on the PC now
    if [ $n_install -gt 0 ]; then
        m_stage install
        for id in "${WANT[@]}"; do
            [ -d $MODELS/$id/unpacked ] || continue          # its download failed
            if is_whisper $id; then install_whisper $id || FAILED+=("$id")
            elif is_aed $id; then install_aed $id || FAILED+=("$id")
            else install_model $id || FAILED+=("$id"); fi
        done
    fi

    if [ -n "$need_amazon" ]; then
        # --- back: forget the app's Wi-Fi and the registration, satellite mode, and only then deregister.  A stock Echo
        # that is online and gets deregistered resets itself to factory settings, /data included: hassmic.conf, state/,
        # the models (user report, 2026-10-05).  As a satellite Amazon's daemons reach only local addresses.
        m_stage back
        keep=" $(cat $NETS 2>/dev/null) " drop=
        if [ "$keep" != "  " ]; then
            for n in $(net_ids); do [[ $keep == *" $n "* ]] || drop="$drop $n"; done
            # in one go: the Echo may be on the app's network right now, and adb over Wi-Fi goes with it
            [ -n "$drop" ] && task "Removing the Wi-Fi the Alexa app added" adb shell "for n in$drop; do
                wpa_cli -i $WLAN -p $WPA_SOCKETS remove_network \$n; done; wpa_cli -i $WLAN -p $WPA_SOCKETS save_config" &&
                { wait_adb device || return 1; }
        fi
        task "Clearing the registration" sh -c "adb pull $MAPDB $TMP/map.db && sqlite3 $TMP/map.db 'delete from deviceData; vacuum;' &&
            adb push $TMP/map.db $MAPDB && adb shell 'rm -f $MAPDB-wal $MAPDB-shm; chown ace_maplite:ace_maplite $MAPDB; chmod 660 $MAPDB'" || return 1
        rm -f $TMP/map.db* $NETS
        ashell "sed -i '/^MODE=/d' $D/hassmic.conf"
        task "Restarting the Echo as a satellite" adb reboot || return 1
        sleep 10
        wait_adb device && satellite_up || return 1
        # the firewall service closes adb over Wi-Fi within 5 s
        ashell "sed -i -e '/^$ADB_OURS\$/d' -e '/^$ADB_OURS_OLD\$/d' $D/hassmic.conf"
        info "The Echo can lose its internet access at the router again."
        todo "Now remove the Echo from your Amazon account" "Alexa app → Devices → this Echo → ⚙ → Deregister" \
            "Only now: it runs as a satellite again, out of Amazon's reach. Deregistered while it ran as a stock Echo, it would reset itself to factory settings."
    elif [ ${#INSTALLED[@]} -gt 0 ]; then
        # main.sh starts a hassmic that exits again; the new one scans $D/models
        task "Restarting hassmic" adb shell 'kill $(pidof hassmic)' && sleep 3 && satellite_up || return 1
    fi

    m_stage done
    local names=()
    for id in "${INSTALLED[@]}"; do names+=("$(label $id)"); done
    [ ${#names[@]} -gt 0 ] && ok "installed: ${names[*]}"
    [ ${#FAILED[@]} -gt 0 ] && fail "not done: ${FAILED[*]}"
    local ww=0 wh= ae=; for id in "${INSTALLED[@]}"; do if is_whisper $id; then wh=1; elif is_aed $id; then ae=1; else ww=1; fi; done
    [ -n "$ae" ] && tell "Sound detection takes the new model" "whenever it is on: \"Sound detection\" on the settings page (http://<echo-ip>:28931/)."
    [ $ww = 1 ] && tell "Pick it in Home Assistant" "Settings → Devices & services → this Echo → Wake word"
    for id in "${WANT[@]}"; do            # ticked, not installed: said plainly, as nothing below mentions it then
        is_whisper $id && [ -z "$wh" ] && fail "whisper detection is not installed: see $LOG"
    done
    if [ -n "$wh" ]; then                 # what the hassmic started last said about it
        local said=$(ashell "grep -E 'whisper: (model|no model)' $D/boot.log | tail -1")
        case $said in
        *loaded*) tell "Whisper detection is on" "Home Assistant: binary sensor \"Last request whispered\" of this Echo." \
                      "Use it in the conversation agent's instructions, e.g. {{ is_state('binary_sensor.<echo>_last_request_whispered', 'on') }}";;
        "")       tell "Update this Echo to use it" "Its hassmic has no whisper detection yet. The model is in place:" \
                      "Home Assistant → this Echo → Firmware (Online updates), and the sensor comes with the new version.";;
        *)        fail "hassmic did not load the whisper model: see $D/boot.log";;
        esac
    fi
    printf '\n'
    [ ${#FAILED[@]} = 0 ]
}
