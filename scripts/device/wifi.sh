#!/system/bin/sh
# Wi-Fi for the settings page (src/hassmic/wifi.c), as root: hassmic may not talk to wpa_supplicant and faces the
# network, so it only leaves a request in its state/, which main.sh's watcher hands to this script.
#   wifi.sh take [STATE OUT]     the request state/wifi-request: "scan <id>" or "join <id> <ssid hex> <psk hex|->"
#   wifi.sh status [STATE OUT]   what the Wi-Fi says now (main.sh netwatch, when the link comes up)
#   wifi.sh clean [STATE OUT]    at the firewall service's start: a request from before is nobody's, a lock of a run
#                                that is gone is nobody's either
#   wifi.sh forget [STATE OUT]   a factory reset (main.sh): every saved network goes, the Echo leaves the one it is on
#   wifi.sh saved                prints how many networks are saved (netwatch: none = nothing for wifisvc to bring up)
# Answers go to OUT (/data/local/hassmic/wifi: root's, the daemon only reads): status, scan, result; lock/ while a run
# works.  wifi.c says what is in them.
# Two worlds, one script: the Dots' wpa_supplicant and checkers' WifiService (the block at ROUTE below says which
# is which).  A switch changes nothing that stays until it worked: on the Dots the new network is added and selected
# in wpa_supplicant only, and wpa_supplicant.conf is written once the Echo is on it with an address and its router
# answers; on checkers the framework saves as it goes, so the try-out ends with the new network removed and the old
# ones enabled again instead.  Either way the Echo goes back to the one it was on, and a reboot in between finds the
# old networks too.  Once it works, the networks saved before are forgotten: the Echo is on the new one only, and
# does not wander back to an old one (its key stays nowhere either).  Until then they stay, for the way back.
umask 022
NL='
'
CMD=$1 S=${2:-/data/local/hassmic/state} O=${3:-/data/local/hassmic/wifi}
CONF="${0%/*}/device.conf"
[ -f "$CONF" ] && . "$CONF"
WLAN=${WLAN:-wlan0}
WPA_SOCKETS=${WPA_SOCKETS:-/data/misc/wifi/sockets}
ARP=${WIFI_ARP:-/proc/net/arp}
ROUTE=${WIFI_ROUTE:-/proc/net/route}
# Two worlds: the Dots' wpa_supplicant (W below) and, on a model with INSTALL=boot (checkers), Android's
# framework, which owns the networks: no dhcpcd exists (DHCP runs when WifiService provisions a network it
# knows) and it re-syncs wpa_supplicant from its own WifiConfigStore on every start, so wpa_supplicant cannot
# be driven directly.  There everything goes through wifictl.dex next to this script (tools/mkwifictl.py):
# the same WifiService binder calls stock's own setup apps make, from root, with wpa_cli's output shape.  Q is
# whichever client this model's world has.
android() { [ "$INSTALL" = boot ]; }
WIFICTL="${0%/*}/wifictl.dex"
A() { [ -f $WIFICTL ] || { echo "wifi: no $WIFICTL" >&2; return 1; }
      CLASSPATH=$WIFICTL app_process / Wifictl "$@"; }
Q() { if android; then A "$@"; else W "$@"; fi; }
aok() { [ "$(A "$@" | tail -n 1)" = OK ]; }
aval() { A "$@" | tail -n 1; }
# how long to wait: for the scan, to get on the new network, for an address and the router behind it, to get back
SCAN_SECS=${WIFI_SCAN_SECS:-5} JOIN_SECS=${WIFI_JOIN_SECS:-30} ADDR_SECS=${WIFI_ADDR_SECS:-30} BACK_SECS=${WIFI_BACK_SECS:-45}

say() { printf 'wifi: %s\n' "$*"; }        # printf, not echo: mksh's echo takes the backslashes of escaped SSIDs
# wpa_supplicant's socket for the interface, else its global one with the interface named, as Android's framework talks
# to it: a "reconfigure" takes the interface's socket away until wpa_supplicant restarts (Dot 2, 2026-10-07)
W() { if [ -S $WPA_SOCKETS/$WLAN ]; then wpa_cli -i $WLAN -p $WPA_SOCKETS "$@"; else wpa_cli -g@android:wpa_$WLAN IFNAME=$WLAN "$@"; fi 2>/dev/null; }
ok() { [ "$(W "$@" | tail -n 1)" = OK ]; }
val() { W "$@" | tail -n 1; }
put() { cat > $O/$1.$$ && chmod 644 $O/$1.$$ && mv $O/$1.$$ $O/$1; }       # what hassmic reads: whole or not at all
hex() { case $1 in ''|*[!0-9a-f]*) return 1;; esac; [ ${#1} -le $2 ] && [ $(( ${#1} % 2 )) = 0 ]; }
field() { printf '%s\n' "$1" | sed -n "s/^$2=//p"; }

status() { Q status | sed 's/^ssid="\(.*\)"$/ssid=\1/' | grep -E '^(wpa_state|id|ssid|ip_address|freq)=' | put status; }    # WifiInfo quotes the name

# One run at a time.  The lock names its process and the boot, so that one left by a run that died is known as such.
boot_id() { cat /proc/sys/kernel/random/boot_id 2>/dev/null; }
alive() { [ -d $O/lock ] && [ "$(cat $O/lock/boot 2>/dev/null)" = "$(boot_id)" ] && kill -0 "$(cat $O/lock/pid 2>/dev/null)" 2>/dev/null; }
lock() {
    mkdir $O/lock 2>/dev/null || { alive && return 1; rm -rf $O/lock; mkdir $O/lock 2>/dev/null; } || return 1
    echo $$ > $O/lock/pid; boot_id > $O/lock/boot
    trap 'rm -rf $O/lock' EXIT
}

scan() {
    Q scan > /dev/null                                  # FAIL-BUSY if one runs already: its results are as good
    sleep $SCAN_SECS
    { echo "id $id"; Q scan_results | grep '^[0-9a-f][0-9a-f]:'; } | put scan
    status
}

# The address.  dhcpcd runs with -K on these Echos (init's dhcpcd-wlan0: "dhcpcd wlan0 -AdLK": no link events), so it
# does not notice another network and keeps the old lease.  Dot 2, 2026-10-07: 15 s on no network and back, and the
# address stayed on wlan0, the old router's ARP entry stayed complete, dhcp.wlan0.result was not written again.  So
# dhcpcd is told to rebind (dhcpcd -n: it asks for its address again; another network's server refuses that, or it
# gives up after 5 s, and dhcpcd asks afresh), and a lease counts only once its hook (95-configured) has written
# dhcp.<if>.result again.  On the same network: RENEW within a second (Dot 2).
# An Echo that has had no network since boot (new, or after a factory reset: the phone's setup, improv.c) has no dhcpcd
# running and no properties of it: init's service (dhcpcd-<if>, what wifi-join.sh falls back to) starts it then, and its
# hook writes the properties from then on.  Not "dhcpcd -n" for that: with no daemon there it starts one outside init.
lease() {
    if [ -z "$PROPS" ]; then
        command -v dhcpcd > /dev/null && [ -n "$(getprop init.svc.dhcpcd-$WLAN)" ] || return 0
        PROPS=1
    fi
    renewed=1; setprop dhcp.$WLAN.result hassmic
    if [ "$(getprop init.svc.dhcpcd-$WLAN)" = running ]; then dhcpcd -n $WLAN > /dev/null 2>&1; else start dhcpcd-$WLAN; fi
}
# On the network for real: a fresh lease (above) on the interface, and its router answers ARP on this link.  ARP rather
# than the ping's answer: a router may drop pings, never ARP.  Without dhcpcd's properties: the interface's address.
online() {
    ip=$(ifconfig $WLAN 2>/dev/null | sed -n 's/.*inet addr:\([0-9.]*\).*/\1/p') gw=
    [ -n "$ip" ] || return 1
    [ -n "$PROPS" ] || return 0
    [ "$(getprop dhcp.$WLAN.result)" = ok ] && [ "$(getprop dhcp.$WLAN.ipaddress)" = "$ip" ] &&
        case "$(getprop dhcp.$WLAN.reason)" in BOUND|REBOOT|RENEW|REBIND) true;; *) false;; esac || { ip=; return 1; }
    gw=$(getprop dhcp.$WLAN.gateway)
    [ -n "$gw" ] || return 0
    ping -c 1 -W 1 $gw > /dev/null 2>&1
    grep "^$gw " $ARP 2>/dev/null | grep -q " 0x[26] .* $WLAN\$"
}

# Wait up to $1 s until wpa_supplicant is through with network $2.  why says how far it got: notfound (never tried to
# associate: out of reach, or a name typed wrong), noassoc (the access point would not have it), wrongkey (reached the
# key handshake and failed there: almost always the password)
connected() {
    t=0
    while [ $t -lt $1 ]; do
        sleep 1; t=$((t + 1))
        st=$(Q status)
        case "$NL$st$NL" in *"${NL}wpa_state=COMPLETED$NL"*) [ "$(field "$st" id)" = "$2" ] && return 0;; esac
        case "$st" in
            *wpa_state=4WAY_HANDSHAKE*|*wpa_state=GROUP_HANDSHAKE*) why=wrongkey;;
            *wpa_state=AUTHENTICATING*|*wpa_state=ASSOCIATING*|*wpa_state=ASSOCIATED*) [ $why = notfound ] && why=noassoc;;
        esac
    done
    return 1
}

# The network it was on again, and every network enabled before enabled again (select_network disabled them all);
# nothing was saved, so this is the configuration on disk as well
back() {
    failed=$why                                         # connected() below words its own way back
    [ -n "$n" ] && W remove_network $n > /dev/null
    if [ -n "$old" ]; then W select_network $old > /dev/null; else W reassociate > /dev/null; fi
    for i in $on; do [ "$i" = "$old" ] || W enable_network $i > /dev/null; done
    was=
    if [ -n "$old" ] && connected $BACK_SECS $old; then
        was=$(field "$st" ssid)
        # its lease again, if the new network's was asked for
        if [ -n "$renewed" ]; then lease; t=0; while [ $t -lt $BACK_SECS ] && ! online; do sleep 1; t=$((t + 1)); done; fi
        say "back on $was${ip:+, address $ip}"
    else say "!! not back on the previous network either: the saved configuration is unchanged, a reboot rejoins it"; fi
    status
    printf '%s\n' "$id failed $ssid $failed $was" | put result
}

join() {
    echo "$id switching $ssid" | put result
    cur=$(W status)
    old= ; [ "$(field "$cur" wpa_state)" = COMPLETED ] && old=$(field "$cur" id)
    renewed= PROPS=; [ -n "$(getprop dhcp.$WLAN.result)" ] && command -v dhcpcd > /dev/null && PROPS=1
    # what is enabled now: enabled again on the way back (select_network disables the rest)
    on= ids=$(W list_networks | sed -n 's/^\([0-9][0-9]*\)[[:space:]].*/\1/p')
    for i in $ids; do case "$(val get_network $i disabled)" in 1|2) ;; *) on="$on $i";; esac; done
    # Amazon's Wi-Fi manager would fight it (main.sh netwatch keeps it stopped while the link is up anyway)
    [ -n "$WIFI_SERVICE" ] && [ "$(getprop init.svc.$WIFI_SERVICE)" = running ] && stop $WIFI_SERVICE
    n=$(val add_network); case $n in ''|*[!0-9]*) n=;; esac
    why=notfound
    if [ -z "$n" ]; then why=bad; say "!! wpa_supplicant did not add a network"; printf '%s\n' "$id failed $ssid $why $(field "$cur" ssid)" | put result; return; fi
    # the SSID as hex, the PSK as its 64 hex digits: nothing of either passes the shell or wpa_cli's quoting
    if ok set_network $n ssid $ssid && ok set_network $n scan_ssid 1 &&
       if [ "$psk" = - ]; then ok set_network $n key_mgmt NONE; else ok set_network $n psk $psk && ok set_network $n key_mgmt WPA-PSK; fi
    then :; else why=bad; say "!! wpa_supplicant did not take the new network's settings"; back; return; fi
    name=$(val get_network $n ssid)
    say "switching to $name, from $(field "$cur" ssid) ($(field "$cur" ip_address)); nothing saved until it works"
    ok select_network $n || { why=bad; back; return; }
    if ! connected $JOIN_SECS $n; then say "not on $name after $JOIN_SECS s ($why), going back"; back; return; fi
    why=noaddress t=0
    lease
    while [ $t -lt $ADDR_SECS ]; do
        online && break
        [ -n "$ip" ] && why=nogateway
        sleep 1; t=$((t + 1))
    done
    if [ $t -ge $ADDR_SECS ]; then say "on $name, but $([ $why = noaddress ] && echo "no address" || echo "the router at ${gw:-?} did not answer") after $ADDR_SECS s, going back"; back; return; fi
    # It works: every network saved before goes (the same name with an older password too), then it is saved.  Not
    # wpa_supplicant's P2P groups (disabled=2): Wi-Fi Direct's, not a network to join.
    for i in $ids; do [ "$(val get_network $i disabled)" = 2 ] || W remove_network $i > /dev/null; done
    saved=1; ok save_config || { saved=0; say "!! on $name, but wpa_supplicant.conf could not be written: a reboot goes back"; }
    say "switched to $name, address $ip"
    status
    echo "$id ok $ssid $ip $saved" | put result
}

# --- checkers: the switch through WifiService.  The framework saves every change at once (its WifiConfigStore),
# so the Dots' "nothing on disk until it worked" has no equivalent; what stays is the try-out.  The new network
# goes in and is switched to; only when it comes up with an address of its own do the networks saved before go,
# and on any failure the new one is removed and the old ones enabled again, so a reboot lands on the old ones
# either way.  Android runs its DHCP itself once its connect lands (there is no dhcpcd): the address on the
# interface is the lease.  The gateway, to tell a silent router from none at all, comes from /proc/net/route
# (hex, little-endian; no awk on the Echo), pinged once to fill the ARP cache as the Dots' online() does.
agateway() {
    g=$(sed -n "s/^$WLAN[[:space:]]\{1,\}00000000[[:space:]]\{1,\}\([0-9A-Fa-f]\{8\}\).*/\1/p" $ROUTE 2>/dev/null)
    [ -n "$g" ] || return 0
    gw=$(printf '%d.%d.%d.%d' $((0x${g:6:2})) $((0x${g:4:2})) $((0x${g:2:2})) $((0x${g:0:2})))
    ping -c 1 -W 1 $gw > /dev/null 2>&1
    grep "^$gw " $ARP 2>/dev/null | grep -q " 0x[26] .* $WLAN\$"
}
aonline() {
    ip=$(ifconfig $WLAN 2>/dev/null | sed -n 's/.*inet addr:\([0-9.]*\).*/\1/p')
    [ -n "$ip" ] || return 1
    agateway
}
aback() {
    failed=$why
    [ -n "$n" ] && A remove $n > /dev/null
    for i in $on; do [ "$i" = "$old" ] || aok enable $i keep; done
    was=
    if [ -n "$old" ] && aok enable $old keep && connected $BACK_SECS $old; then
        was=$(field "$st" ssid | sed 's/^"//;s/"$//')       # WifiInfo quotes the name; wpa_cli does not
        say "back on $was"
    else say "!! not back on the previous network either: it is still saved, a reboot rejoins it"; fi
    status
    printf '%s\n' "$id failed $ssid $failed $was" | put result
}
ajoin() {
    echo "$id switching $ssid" | put result
    cur=$(A status)
    old= ; [ "$(field "$cur" wpa_state)" = COMPLETED ] && old=$(field "$cur" id)
    on=$(A list_networks | sed -n 's/^\([0-9][0-9]*\)[[:space:]].*/\1/p')
    n=$(aval add $ssid $psk); case $n in ''|*[!0-9]*) n=;; esac
    why=notfound
    if [ -z "$n" ]; then why=bad; say "!! WifiService did not add the network"; printf '%s\n' "$id failed $ssid $why $(field "$cur" ssid)" | put result; return; fi
    name=$(aval name $ssid)
    say "switching to $name, from $(field "$cur" ssid | sed 's/^"//;s/"$//'); nothing forgotten until it works"
    aok select $n || { why=bad; aback; return; }
    if ! connected $JOIN_SECS $n; then say "not on $name after $JOIN_SECS s ($why), going back"; aback; return; fi
    why=noaddress t=0
    while [ $t -lt $ADDR_SECS ]; do
        aonline && break
        [ -n "$ip" ] && why=nogateway
        sleep 1; t=$((t + 1))
    done
    if [ $t -ge $ADDR_SECS ]; then say "on $name, but $([ $why = noaddress ] && echo "no address" || echo "the router at ${gw:-?} did not answer") after $ADDR_SECS s, going back"; aback; return; fi
    for i in $on; do [ "$i" = "$n" ] || A remove $i > /dev/null; done      # it worked: the ones before go
    say "switched to $name, address $ip"
    status
    echo "$id ok $ssid $ip 1" | put result
}

mkdir -p $O
case "$CMD" in
status) status;;
clean) rm -f $S/wifi-request; alive || rm -rf $O/lock;;
saved) Q list_networks | grep -c '^[0-9]' ;;
forget)
    if android; then
        # WifiService: every configured network goes, saved at once; the link it was on ends with its network
        for i in $(A list_networks | sed -n 's/^\([0-9][0-9]*\)[[:space:]].*/\1/p'); do A remove $i > /dev/null; done
        say "every saved network forgotten"
    else
        # Not wpa_supplicant's P2P groups (disabled=2), as in join.  Saved at once: the reset is meant to survive a reboot.
        # Removing the network it is on ends that link; no "disconnect", which would keep it off until told to reconnect.
        for i in $(W list_networks | sed -n 's/^\([0-9][0-9]*\)[[:space:]].*/\1/p'); do
            [ "$(val get_network $i disabled)" = 2 ] || W remove_network $i > /dev/null
        done
        ok save_config && say "every saved network forgotten" || say "!! networks removed, but wpa_supplicant.conf could not be written: a reboot brings them back"
    fi
    rm -f $O/scan $O/result
    status;;
take)
    R=$S/wifi-request
    [ -f $R ] && [ ! -L $R ] || exit 0
    lock || exit 0                                      # another run has it: the request waits for the next look
    read -r what id ssid psk < $R; rm -f $R
    case $id in ''|*[!0-9]*) say "request not understood, dropped"; exit 0;; esac
    case $what in
    scan) scan;;
    join)
        if hex "$ssid" 64 && { [ "$psk" = - ] || { hex "$psk" 64 && [ ${#psk} = 64 ]; }; }; then
            if android; then ajoin; else join; fi
        else say "join request not understood, dropped"; fi;;
    *) say "request \"$what\" not understood, dropped";;
    esac;;
*) echo "usage: wifi.sh take|status|clean|forget [STATE OUT] | saved"; exit 2;;
esac
exit 0
