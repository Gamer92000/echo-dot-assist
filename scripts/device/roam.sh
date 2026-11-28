# Roaming between the access points of one Wi-Fi network (issue #234).  Sourced by main.sh; netwatch calls roam_look
# every 10 s.  Needs wpa (main.sh's wpa_cli wrapper), cfg (state/config's path) and WIFI (root's answers for hassmic).
#
# Why here: 802.11 leaves the move to the client, and on the Dots the client that moved was wifisvc, which netwatch
# stops once the link is up (its connectivity checks fail behind the egress lock).  What is left does not move:
#  - wpa_supplicant 2.6 as Amazon built it (every Dot) has no bgscan module ("simple" and "learn" are not in it; the
#    setting is parsed and does nothing: no scan while associated, a donut 10 h on one access point, 2026-11-25), and
#    the drivers refuse SIGNAL_MONITOR (no CQM).  Amazon's own BSS picker in it (adptClntPickBss) prefers 5 GHz when it
#    associates: a Dot 2 joined 5500 MHz at -76 dBm with the same network at -59 on 2462, 20 % of its pings lost.
#  - The MediaTek drivers' own roaming is off in the firmware's wifi.cfg (DisRoaming 1; donut, cannoli) and taking it
#    back at run time (SET_CFG DisRoaming 0) starts nothing.
#  - 802.11k: wpa_supplicant has RRM but the drivers do not offer it ("Insufficient RRM support"), so no neighbor
#    report.  Its stand-in here: the channels this network was seen on, learnt from every scan, scanned alone (4
#    channels 0.5 s, all 37 3 to 4 s off the channel on a Dot 2 and a Dot 5).
#  - 802.11r: an FT AKM in the network makes the Dot 2's and the Dot 3's drivers refuse the association outright
#    ("Association request to the driver failed: err=22", as #90's PSK-SHA256; the Dot 5's takes it, its FT roam is
#    untried), so none is offered; a PSK roam without it took 160 to 360 ms.
#  - 802.11v: wpa_supplicant has WNM: it tells the access point it takes BSS transition requests and follows them by
#    itself.  Where the access point says it handles them (Extended Capabilities bit 19), a weak link first asks it
#    (BTM query, reason 16 "low RSSI") and gives it one look to move the Echo; then the scan.
# The move: wpa_cli roam, with the target pinned on the network (bssid) for its time.  Unpinned, Amazon's picker went
# elsewhere (a Dot 5 told to roam to 2437 MHz joined 2462; one told to go back to 5500 hung 5 s in authentication and
# fell back): pinned, every move landed where it was told, 1 or 2 pings of 160 lost.  The pin is taken off at once, and
# a pin a killed netwatch left (ROAM_PIN) at its next look.  On the gen4m drivers (donut, cannoli) a roam is a
# deauthentication and a fresh authentication; on the Dot 2's gen2 a reassociation: both kept the address and dhcpcd
# (-K) noticed nothing.
# What counts as weak, better and how often is a client's rule of thumb (Apple's and Android's are alike): under
# -70 dBm for 30 s, a candidate 8 dB stronger and at least -67; a scan a minute while weak, 2, 4, then every 5 min
# while none is found, every channel every 10 min, 20, 40, up to every hour (one router far away: nothing to move
# to, and every scan takes the Echo off its channel).
# Measured here: -73 to -76 dBm lost 20 to 45 % of pings to the router, -59 to -64 under 4 %.
# Switched off by the setting wifi_roaming=off (state/config; the System page): the access point is still recorded.
# For the settings page, $WIFI/roam (hassmic's wifi.c reads it), written only when the access point changes:
#   ap <bssid> <freq> <since: epoch>
#   moved <epoch> <how: roam|ap|self> <from> <from freq> <from dBm> <to> <to freq> <to dBm> <ms|->     (last 5, newest last)
# "self": wpa_supplicant went elsewhere by itself (the link was lost, or a BSS transition request it was not asked for).

ROAM_BELOW=-70          # dBm: weak under this...
ROAM_LOOKS=3            # ...for this many looks in a row (30 s)
ROAM_GAIN=8             # dB a candidate must be stronger
ROAM_FLOOR=-67          # dBm a candidate must have at least
ROAM_SCAN_GAP=6         # looks between two scans while weak (60 s), doubled after each that found nothing...
ROAM_GAP_MAX=30         # ...up to 5 min: one router far away has nothing to move to, and each scan costs the link
ROAM_FULL_GAP=60        # looks between two scans of every channel (10 min), twice as long after each that found
ROAM_FULL_MAX=360       # nothing to move to (1 h at most)
ROAM_BAD_LOOKS=60       # a target that refused: left alone that long (10 min), twice as long each time it refuses again
ROAM_BAD_MAX=360        # (1 h at most)
ROAM_SETTLE=6           # looks after any move before the next look for one (60 s)
ROAM_WAIT=20            # quarter seconds a move may take before it counts as failed
ROAM_PIN=$WIFI/roam-pin
rm_bss= rm_freq= rm_rssi= rm_ssid= rm_weak=0 rm_wait=0 rm_full=0 rm_gap=$ROAM_SCAN_GAP rm_fgap=$ROAM_FULL_GAP rm_scanned=
rm_bad= rm_bad_n=0 rm_bad_len=$ROAM_BAD_LOOKS rm_freqs= rm_next= rm_said= rm_asked= rm_btm_bss= rm_btm= rm_cand=

rfield() { printf '%s\n' "$1" | sed -n "s/^$2=//p" | head -n 1; }
rm_num() { case $1 in -[0-9]*|[0-9]*) [ "${1#-}" -eq "${1#-}" ] 2>/dev/null;; *) return 1;; esac; }

# The access point says it takes BSS transition queries: Extended Capabilities (element 127), bit 19 (octet 2, 0x08).
# The element walk is over wpa_cli bss's ie= hex; at most once per access point.
rm_btm_of() {
    local ie i t l
    [ "$1" = "$rm_btm_bss" ] && { [ "$rm_btm" = 1 ]; return; }
    rm_btm_bss=$1 rm_btm=0
    ie=$(rfield "$(wpa bss $1)" ie)
    i=0
    case $ie in *[!0-9a-fA-F]*) ie=;; esac
    while [ $((i + 4)) -le ${#ie} ]; do
        t=$((16#${ie:$i:2})) l=$((16#${ie:$((i + 2)):2}))
        # an element cut short (a truncated dump) ends the walk: an empty ${ie:...} in $(( )) would end netwatch
        [ $((i + 4 + 2 * l)) -le ${#ie} ] || break
        if [ $t = 127 ] && [ $l -ge 3 ]; then
            [ $(( 16#${ie:$((i + 8)):2} & 8 )) = 8 ] && rm_btm=1
            break
        fi
        i=$((i + 4 + 2 * l))
    done 2>/dev/null
    [ "$rm_btm" = 1 ]
}

# the record for the page: the access point now, and the last moves
rm_record() {   # [moved line]
    local f=$WIFI/roam
    [ -d $WIFI ] || mkdir -p $WIFI
    {
        echo "ap $rm_bss $rm_freq $(date +%s)"
        grep '^moved ' $f 2>/dev/null | tail -n 4
        [ -z "$1" ] || echo "moved $1"
    } > $f.$$ && chmod 644 $f.$$ && mv -f $f.$$ $f
}

# a pin a netwatch that ended mid-move left on the network ("<id> <bssid>"): taken off before anything else, also
# while wifi.sh works, and only where that network still has it (after a reboot an id may be another network's)
rm_unpin() {
    local id b
    [ -f $ROAM_PIN ] || return 0
    read -r id b < $ROAM_PIN 2>/dev/null
    rm_num "$id" && [ -n "$b" ] && [ "$(wpa get_network $id bssid)" = "$b" ] && wpa set_network $id bssid any > /dev/null
    rm -f $ROAM_PIN
}

# the channels the network was seen on (wpa_cli scan_results: bssid, freq, signal, flags, [country,] ssid: the SSID is
# the last field; a Dot 5 prints the country column, a Dot 2 does not), and the strongest other access point of it
# that asks for the same as this one (flags but [WPS]: a guest or neighbour's network of the same name with another
# key or WPA3 only would not take the Echo).  Levels as signal_poll's: -99 to -1 dBm, a made-up one is none
rm_learn() {   # ssid; rm_cand: "<bssid> <freq> <signal>" (not in a $(): rm_freqs)
    local tab b f s fl rest sr cf= bs=-999
    rm_cand=
    tab=$(printf '\t')
    sr=$(wpa scan_results)
    while IFS=$tab read -r b f s fl rest; do
        [ "$b" = "$rm_bss" ] && cf=$(echo "$fl" | sed 's/\[WPS\]//g')
    done <<EOF
$sr
EOF
    while IFS=$tab read -r b f s fl rest; do
        [ -n "$rest" ] && [ "${rest##*$tab}" = "$1" ] || continue
        rm_num "$f" && rm_num "$s" && [ $s -lt 0 ] && [ $s -gt -100 ] || continue
        [ -z "$cf" ] || [ "$(echo "$fl" | sed 's/\[WPS\]//g')" = "$cf" ] || continue
        case " $rm_freqs " in *" $f "*) ;; *) rm_freqs="$rm_freqs $f";; esac
        [ "$b" != "$rm_bss" ] && { [ "$b" != "$rm_bad" ] || [ $rm_bad_n = 0 ]; } && [ $s -gt $bs ] || continue
        rm_cand="$b $f $s"; bs=$s
    done <<EOF
$sr
EOF
}

rm_move() {     # id bssid freq signal
    local t0 t1 i=0 st ok= ms=- fb=$rm_bss ff=$rm_freq fr=$rm_rssi
    [ -d $WIFI ] || mkdir -p $WIFI
    echo "$1 $2" > $ROAM_PIN
    wpa set_network $1 bssid $2 > /dev/null
    t0=$(cut -d' ' -f1 /proc/uptime | tr -d .)
    # FAIL: no longer in wpa_supplicant's BSS table (gone for 3 minutes; it was in the scan of the look before)
    [ "$(wpa roam $2)" = OK ] || i=$ROAM_WAIT
    while [ $i -lt $ROAM_WAIT ]; do
        st=$(wpa status)
        case "$st" in *"bssid=$2"*wpa_state=COMPLETED*|*wpa_state=COMPLETED*"bssid=$2"*) ok=1; break;; esac
        sleep 0.25; i=$((i + 1))
    done
    t1=$(cut -d' ' -f1 /proc/uptime | tr -d .)
    wpa set_network $1 bssid any > /dev/null
    rm -f $ROAM_PIN
    [ -n "$t0" ] && [ -n "$t1" ] && ms=$(( (t1 - t0) * 10 ))
    if [ -n "$ok" ]; then
        rm_bss=$2 rm_freq=$3 rm_rssi=$4 rm_wait=$ROAM_SETTLE rm_weak=0 rm_said= rm_gap=$ROAM_SCAN_GAP
        echo "netwatch: Wi-Fi moved to $2 ($3 MHz, $4 dBm) from $fb ($fr dBm) in $ms ms"
        rm_record "$(date +%s) roam $fb $ff $fr $2 $3 $4 $ms"
    else
        if [ "$rm_bad" = "$2" ]; then rm_bad_len=$((rm_bad_len * 2)); [ $rm_bad_len -le $ROAM_BAD_MAX ] || rm_bad_len=$ROAM_BAD_MAX
        else rm_bad_len=$ROAM_BAD_LOOKS; fi
        rm_bad=$2 rm_bad_n=$rm_bad_len rm_wait=$ROAM_SCAN_GAP
        echo "netwatch: Wi-Fi move to $2 ($3 MHz, $4 dBm) did not take within $((ROAM_WAIT / 4)) s: left alone for $((rm_bad_len / 6)) min"
    fi
}

roam_look() {
    local st bss freq id ssid rssi c how
    [ -n "$WIFI_SERVICE" ] || return 0
    rm_unpin
    [ ! -d $WIFI/lock ] || return 0
    st=$(wpa status)
    case "$st" in *wpa_state=COMPLETED*) ;; *) rm_weak=0 rm_next=; return 0;; esac
    bss=$(rfield "$st" bssid) freq=$(rfield "$st" freq) id=$(rfield "$st" id) ssid=$(rfield "$st" ssid)
    rssi=$(rfield "$(wpa signal_poll)" RSSI)
    # a level a driver makes up (-9999, 0) would be weak for good, and every access point stronger
    [ -n "$bss" ] && rm_num "$id" && rm_num "$rssi" && [ "$rssi" -lt 0 ] && [ "$rssi" -gt -100 ] || return 0
    if [ "$bss" != "$rm_bss" ]; then
        # moved without us: a lost link joined again, or the access point's BSS transition request
        how=self; [ "$rm_next" = btm ] && how=ap
        if [ -n "$rm_bss" ] && [ "$ssid" = "$rm_ssid" ]; then
            echo "netwatch: Wi-Fi on $bss ($freq MHz, $rssi dBm), was $rm_bss ($rm_freq MHz, $rm_rssi dBm)$([ $how = ap ] && echo ": moved by the access point (802.11v)")"
            c="$(date +%s) $how $rm_bss $rm_freq $rm_rssi $bss $freq $rssi -"
        else
            echo "netwatch: Wi-Fi on $bss ($freq MHz, $rssi dBm)"; c=
        fi
        # another network (wifi.sh's switch): its channels and access points are none of the old one's, and no move
        [ "$ssid" = "$rm_ssid" ] || { [ -z "$rm_ssid" ] || c=; rm_freqs= rm_bad= rm_bad_n=0 rm_bad_len=$ROAM_BAD_LOOKS rm_full=0; }
        rm_bss=$bss rm_freq=$freq rm_rssi=$rssi rm_ssid=$ssid rm_weak=0 rm_next= rm_said= rm_asked= rm_wait=$ROAM_SETTLE
        rm_gap=$ROAM_SCAN_GAP rm_fgap=$ROAM_FULL_GAP
        rm_record "$c"
        return 0
    fi
    rm_rssi=$rssi
    case " $rm_freqs " in *" $freq "*) ;; *) rm_freqs="$rm_freqs $freq";; esac
    [ $rm_wait -gt 0 ] && rm_wait=$((rm_wait - 1))
    [ $rm_full -gt 0 ] && rm_full=$((rm_full - 1))
    [ $rm_bad_n -gt 0 ] && rm_bad_n=$((rm_bad_n - 1))      # rm_bad kept after: a second refusal waits twice as long
    grep -qx 'wifi_roaming=off' $(cfg) && { rm_weak=0 rm_next=; return 0; }
    # strong again: the next weak spell starts afresh (its 30 s, a scan a minute)
    if [ $rssi -lt $ROAM_BELOW ]; then rm_weak=$((rm_weak + 1)); else rm_weak=0 rm_said= rm_gap=$ROAM_SCAN_GAP rm_fgap=$ROAM_FULL_GAP rm_wait=0; fi
    # last look's scan answered: the best other access point of the network
    if [ "$rm_next" = scan ]; then
        rm_next=
        rm_learn "$ssid"; c=$rm_cand
        if [ -n "$c" ] && [ ${c##* } -ge $((rssi + ROAM_GAIN)) ] && [ ${c##* } -ge $ROAM_FLOOR ]; then
            set -- $c
            rm_move $id $1 $2 $3
            return 0
        fi
        [ -n "$rm_said" ] || echo "netwatch: Wi-Fi weak ($rssi dBm on $bss, $freq MHz), no access point of the network $ROAM_GAIN dB stronger${c:+ (best: ${c% * *} ${c##* } dBm)}"
        [ "$rm_scanned" = full ] && { rm_fgap=$((rm_fgap * 2)); [ $rm_fgap -le $ROAM_FULL_MAX ] || rm_fgap=$ROAM_FULL_MAX; }
        rm_said=1 rm_wait=$rm_gap rm_gap=$((rm_gap * 2))
        [ $rm_gap -le $ROAM_GAP_MAX ] || rm_gap=$ROAM_GAP_MAX
        return 0
    fi
    [ $rm_weak -ge $ROAM_LOOKS ] && [ $rm_wait = 0 ] || { [ "$rm_next" = btm ] && [ $rm_weak = 0 ] && rm_next=; return 0; }
    # a network the owner tied to one access point (its bssid set, not by us) stays there
    case "$(wpa get_network $id bssid)" in ''|FAIL|any|00:00:00:00:00:00) ;; *) rm_weak=0; return 0;; esac
    # weak for 30 s: the access point asked first where the Echo should go (once per stay on it), then a scan
    if [ "$rm_asked" != "$bss" ] && rm_btm_of $bss && [ "$(wpa wnm_bss_query 16)" = OK ]; then
        echo "netwatch: Wi-Fi weak ($rssi dBm on $bss, $freq MHz), access point asked for a better one (802.11v)"
        rm_asked=$bss rm_next=btm rm_said=1 rm_wait=1
        return 0
    fi
    # answered within the look (0.5 s, every channel 3 to 6 s): read at the next; a scan refused (FAIL-BUSY) waits its gap
    rm_next= rm_wait=$rm_gap
    if [ $rm_full = 0 ] || [ -z "$rm_freqs" ]; then
        [ "$(wpa scan)" = OK ] && rm_next=scan rm_wait=1 rm_full=$rm_fgap rm_scanned=full
    else
        [ "$(wpa scan freq=$(echo $rm_freqs | tr ' ' ,))" = OK ] && rm_next=scan rm_wait=1 rm_scanned=some
    fi
}
