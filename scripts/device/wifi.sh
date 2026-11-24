#!/system/bin/sh
# Wi-Fi for the settings page (src/hassmic/wifi.c), as root: hassmic may not talk to wpa_supplicant and faces the
# network, so it only leaves a request in its state/, which main.sh's watcher hands to this script.
#   wifi.sh take [STATE OUT]     the request state/wifi-request: "scan <id>", "join <id> <ssid hex> <psk hex|->", or
#                                "address <id> auto" / "address <id> fixed <ip> <prefix> <gateway> <dns> <dns2|->"
#   wifi.sh status [STATE OUT]   what the Wi-Fi says now (main.sh netwatch, when the link comes up)
#   wifi.sh clean [STATE OUT]    at the firewall service's start: a request from before is nobody's, a lock of a run
#                                that is gone is nobody's either
#   wifi.sh forget [STATE OUT]   a factory reset (main.sh): every saved network goes, the Echo leaves the one it is on
#   wifi.sh saved                prints how many networks are saved (netwatch: none = nothing for wifisvc to bring up)
#   wifi.sh keep [STATE OUT]     the Dots: the fixed address on the network it is for, DHCP anywhere else (netwatch)
# Answers go to OUT (/data/local/hassmic/wifi: root's, the daemon only reads): status, scan, result, address,
# address-result; lock/ while a run works.  wifi.c says what is in them.
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
# Two worlds: the Dots' wpa_supplicant (W below) and, on an Android model (device.conf ANDROID: checkers), Android's
# framework, which owns the networks: no dhcpcd exists (DHCP runs when WifiService provisions a network it
# knows) and it re-syncs wpa_supplicant from its own WifiConfigStore on every start, so wpa_supplicant cannot
# be driven directly.  There everything goes through wifictl.dex next to this script (tools/mkwifictl.py):
# the same WifiService binder calls stock's own setup apps make, from root, with wpa_cli's output shape.  Q is
# whichever client this model's world has.
android() { [ -n "$ANDROID" ]; }
WIFICTL="${0%/*}/wifictl.dex"
# WIFICTL_USER (athena, Android 9): the framework shows the SSID and the scan results to system apps only (as uid 0
# with any package name they came back empty; as uid 1000 with "android", the name the dex passes, complete).
A() { [ -f $WIFICTL ] || { echo "wifi: no $WIFICTL" >&2; return 1; }
      if [ -n "$WIFICTL_USER" ]; then
          "${0%/*}/runas" $WIFICTL_USER $WIFICTL_USER env CLASSPATH=$WIFICTL app_process / Wifictl "$@"
      else CLASSPATH=$WIFICTL app_process / Wifictl "$@"; fi; }
Q() { if android; then A "$@"; else W "$@"; fi; }
aok() { [ "$(A "$@" | tail -n 1)" = OK ]; }
TAB=$(printf '\t')
aval() { A "$@" | tail -n 1; }
# how long to wait: for the scan, to get on the new network, for an address and the router behind it, to get back.
# The last three count looks, one every STEP s: tests (tests/fake_web_wifi.py, whose stand-ins move on per look, not by
# the clock) look every 0.1 s and give the counts in tenths
SCAN_SECS=${WIFI_SCAN_SECS:-5} JOIN_SECS=${WIFI_JOIN_SECS:-30} ADDR_SECS=${WIFI_ADDR_SECS:-30} BACK_SECS=${WIFI_BACK_SECS:-45}
REACH_SECS=${WIFI_REACH_SECS:-120}             # a fixed address: how long it waits for a connection to it (looks, as above)
STEP=${WIFI_STEP:-1}
TCP=${WIFI_TCP:-/proc/net/tcp}

say() { printf 'wifi: %s\n' "$*"; }        # printf, not echo: mksh's echo takes the backslashes of escaped SSIDs
# wpa_supplicant's socket for the interface, else its global one with the interface named, as Android's framework talks
# to it: a "reconfigure" takes the interface's socket away until wpa_supplicant restarts (Dot 2, 2026-10-07)
W() { if [ -S $WPA_SOCKETS/$WLAN ]; then wpa_cli -i $WLAN -p $WPA_SOCKETS "$@"; else wpa_cli -g@android:wpa_$WLAN IFNAME=$WLAN "$@"; fi 2>/dev/null; }
ok() { [ "$(W "$@" | tail -n 1)" = OK ]; }
val() { W "$@" | tail -n 1; }
put() { cat > $O/$1.$$ && chmod 644 $O/$1.$$ && mv $O/$1.$$ $O/$1; }       # what hassmic reads: whole or not at all
hex() { case $1 in ''|*[!0-9a-f]*) return 1;; esac; [ ${#1} -le $2 ] && [ $(( ${#1} % 2 )) = 0 ]; }
field() { printf '%s\n' "$1" | sed -n "s/^$2=//p"; }

# What the page offers as a fixed address: the mask, the router and the DNS servers the Echo has now.  The router: the
# lease's (the Dots' dhcpcd properties), else the main table's default route, else Android's own table for the
# interface (its routes are per network, the main table has none: checkers, 2026-11-24); the DNS servers: the lease's,
# else what Android's framework set (net.dns1, net.dns2)
status() {
    local m g d
    m=$(ifconfig $WLAN 2>/dev/null | sed -n 's/.*Mask:\([0-9.]*\).*/\1/p')
    g=$(getprop dhcp.$WLAN.gateway 2>/dev/null); case $g in ''|0.0.0.0) g=;; esac
    [ -n "$g" ] || g=$(route_gw)
    [ -n "$g" ] || g=$(ip route show table all 2>/dev/null | sed -n "s/^default via \([0-9.]*\) dev $WLAN .*/\1/p" | head -n 1)
    d=$(for k in dhcp.$WLAN.dns1 dhcp.$WLAN.dns2; do getprop $k; done 2>/dev/null | grep -v '^0\.0\.0\.0$')
    [ -n "$d" ] || d=$(for k in net.dns1 net.dns2; do getprop $k; done 2>/dev/null)
    { Q status | sed 's/^ssid="\(.*\)"$/ssid=\1/' | grep -E '^(wpa_state|id|ssid|ip_address|freq)='       # WifiInfo quotes the name
      echo "netmask=$m"; echo "gateway=$g"; echo "dns="$(echo $d | tr -cd '0-9. ' ); } | put status
}

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
# dhcpcd is made to ask afresh, and a lease counts only once its hook (95-configured) has written dhcp.<if>.result
# again.  By init's service started anew, not "dhcpcd -n" (which the Dot 2 took: RENEW within a second on the same
# network): a control command's own arguments replace the daemon's, and without init's -K it checks the carrier it
# read when it started, never updated since (-K).  Cannoli, 2026-11-15, the phone's setup after a reset: dhcpcd
# started with no network, "dhcpcd -n" -> "waiting for carrier" for good on the new network, no address; with -K
# added, one DISCOVER and the next 64 s later (its backoff since it started); without -L, IPv4LL.
# Started anew it reads the carrier, takes init's arguments and backs off from 4 s.  An Echo that has had no network
# since boot (new, or after a factory reset: improv.c) has no dhcpcd yet, nor init.svc.<if>, which init sets at a
# service's first start: it is started all the same, and the property tells whether init has such a service at all.
# init stops a service with SIGKILL (Android 7), so dhcpcd leaves its address on the interface, and the new one, not
# knowing it, adds its lease beside it: cannoli, 2026-11-15, a stale 10.99.0.5 stayed next to the new lease, first in
# ifconfig, and online() would never have matched it.  So the addresses go first (ifconfig 0.0.0.0 removes the first
# of them, the next moves up); on the same network the new dhcpcd has its lease back within milliseconds (REQUEST/ACK).
# A fixed address (below) is init's dhcpcd-static, which runs instead of dhcpcd-<if>: it goes too.
lease() {
    if [ -z "$PROPS" ]; then command -v dhcpcd > /dev/null || return 0; PROPS=1; fi
    renewed=1
    dhcp_off
    setprop dhcp.$WLAN.result hassmic; start dhcpcd-$WLAN
    k=0; while [ $k -lt 3 ] && [ -z "$(getprop init.svc.dhcpcd-$WLAN)" ]; do sleep $STEP; k=$((k + 1)); done
    [ -n "$(getprop init.svc.dhcpcd-$WLAN)" ] || { PROPS= renewed=; setprop dhcp.$WLAN.result ''; }
}
# On the network for real: a fresh lease (above) on the interface, and its router answers ARP on this link.  ARP rather
# than the ping's answer: a router may drop pings, never ARP.  Without dhcpcd's properties: the interface's address.
online() {
    ip=$(ifconfig $WLAN 2>/dev/null | sed -n 's/.*inet addr:\([0-9.]*\).*/\1/p') gw=
    [ -n "$ip" ] || return 1
    [ -n "$PROPS" ] || return 0
    [ "$(getprop dhcp.$WLAN.result)" = ok ] && [ "$(getprop dhcp.$WLAN.ipaddress)" = "$ip" ] &&
        # Cannoli's DHCPv6 hook can overwrite reason immediately after the
        # successful IPv4 lease. Still require the fresh result, matching IPv4
        # address and the gateway's ARP response below.
        case "$(getprop dhcp.$WLAN.reason)" in BOUND|REBOOT|RENEW|REBIND|INFORM6) true;; *) false;; esac || { ip=; return 1; }
    gw=$(getprop dhcp.$WLAN.gateway)
    [ -n "$gw" ] || return 0
    ping -c 1 -W 1 $gw > /dev/null 2>&1
    grep "^$gw " $ARP 2>/dev/null | grep -q " 0x[26] .* $WLAN\$"
}

# Both of init's dhcpcd services stopped (the one that runs: a service never started has no init.svc), and the
# addresses a killed one left on the interface gone (lease() says why)
dhcp_off() {
    local sv k was=
    for sv in dhcpcd-$WLAN dhcpcd-static; do
        [ -n "$(getprop init.svc.$sv)" ] || continue
        was=1; [ "$(getprop init.svc.$sv)" = stopped ] && continue
        stop $sv
        k=0; while [ $k -lt 5 ] && [ "$(getprop init.svc.$sv)" != stopped ]; do sleep $STEP; k=$((k + 1)); done
    done
    [ -n "$was" ] || return 0
    k=0; while [ $k -lt 4 ] && ifconfig $WLAN 2>/dev/null | grep -q 'inet addr:'; do ifconfig $WLAN 0.0.0.0; k=$((k + 1)); done
}

# ---- a fixed address (issue #191).  The setting is root's, in OUT/address: "fixed <ip> <prefix> <gateway> <dns>
# <dns2|-> <ssid hex>", the network it was set on last; no file = DHCP.  hassmic only asks for one (state/wifi-request
# "address ..."), and this script writes it only once the Echo was reached on it.
# The Dots: Amazon's own static mode, init's dhcpcd-static (dhcpcd_static.sh on donut, biscuit, radar, cannoli:
# "dhcpcd -S ip_address=... -S subnet_mask=... -S routers=... -S domain_name_servers=... wlan0 -K", the values from the
# dhcp.wlan0.* properties, labelled dhcpcd-static_exec), in place of dhcpcd-wlan0.  Its hooks are the lease's: the
# routes, the DNS servers into /data/misc/dhcp/resolv.conf (8.8.8.8 added third, Amazon's rule), reason STATIC with
# result ok.  Dot 2, 2026-11-24: 192.168.103.250/22 in a second, the router pinged, github.com resolved, hassmic
# announced the address over mDNS; back to dhcpcd-wlan0 the same way, its lease (BOUND) at once.  wifisvc starts
# dhcpcd-wlan0 (a boot, a lost network), so main.sh's netwatch hands the link to "keep" every look.
# Android's models: the address is the network's own (WifiConfiguration's IpConfiguration, wifictl "ip"), which
# WifiService keeps and takes at every connect; but not while connected: checkers (Android 7, 2026-11-24) stored it and
# stayed on its lease until "reassoc" (reassociate()), ~14 s later on the fixed address.  Back to DHCP the same way;
# the router may hand the fixed address out again then (Android's DHCP asks for the address it had).
quad() {
    local o
    case $1 in ''|*[!0-9.]*|.*|*.|*..*) return 1;; esac
    set -- $(echo $1 | tr . ' ')
    [ $# = 4 ] || return 1
    for o; do case $o in 0?*) return 1;; esac; [ ${#o} -le 3 ] && [ $o -le 255 ] || return 1; done
}
num() { set -- $(echo $1 | tr . ' '); echo $(( ($1 << 24) | ($2 << 16) | ($3 << 8) | $4 )); }
dotted() { echo "$(( ($1 >> 24) & 255 )).$(( ($1 >> 16) & 255 )).$(( ($1 >> 8) & 255 )).$(( $1 & 255 ))"; }
mask_of() { dotted $(( (0xffffffff << (32 - $1)) & 0xffffffff )); }
# /proc/net/tcp's form of an address: the four bytes in hex, the last first (little-endian: ARM and the PC alike)
tcphex() { set -- $(echo $1 | tr . ' '); printf '%02X%02X%02X%02X' $4 $3 $2 $1; }
# what hassmic asked for, checked again (root never takes its word): unicast addresses, the router inside the network
# and not the Echo's own, the Echo's address neither the network's nor its broadcast
fixed_ok() {   # ip prefix gateway dns dns2|-
    local m i g h all
    quad "$1" && quad "$3" && quad "$4" && { [ "$5" = - ] || quad "$5"; } || return 1
    case $2 in ''|*[!0-9]*|0?*) return 1;; esac
    [ $2 -ge 8 ] && [ $2 -le 30 ] || return 1
    for i in $1 $3 $4 ${5#-}; do case $i in 0.*|127.*|22[4-9].*|2[3-5][0-9].*) return 1;; esac; done
    m=$(( (0xffffffff << (32 - $2)) & 0xffffffff )) i=$(num $1) g=$(num $3)
    [ $(( i & m )) = $(( g & m )) ] && [ "$1" != "$3" ] || return 1
    all=$(( ~m & 0xffffffff ))
    for h in $(( i & all )) $(( g & all )); do [ $h != 0 ] && [ $h != $all ] || return 1; done
}
# the SSID of the network it is on, as hex (wpa_supplicant and WifiInfo print "name", or hex where it has bytes that
# do not print)
ssid_hex() {
    local v
    if android; then v=$(field "$(A status)" ssid); else v=$(val get_network $(field "$(W status)" id) ssid); fi
    case $v in
    \"*\") v=${v#\"}; printf '%s' "${v%\"}" | od -An -tx1 | tr -d ' \n';;
    ''|*[!0-9a-fA-F]*) ;;                               # FAIL, nothing: no name known (empty)
    *) printf '%s' "$v" | tr 'A-F' 'a-f';;
    esac
}
fixed_read() { fx= fip= fpre= fgw= fd1= fd2= fssid=; [ -f $O/address ] && read -r fx fip fpre fgw fd1 fd2 fssid < $O/address; [ "$fx" = fixed ]; }
# the Dots: dhcpcd-static with these values (dhcpcd_static.sh reads them from the properties at its start)
static_on() {   # ip prefix gateway dns dns2|-
    dhcp_off
    setprop dhcp.$WLAN.ipaddress $1; setprop dhcp.$WLAN.mask $(mask_of $2); setprop dhcp.$WLAN.gateway $3
    setprop dhcp.$WLAN.dns1 $4; setprop dhcp.$WLAN.dns2 "${5#-}"
    setprop dhcp.$WLAN.result hassmic; start dhcpcd-static
}
# on it: the address on the interface, dhcpcd-static's hook through (the Dots), and the router answers
static_up() {   # ip gateway
    ip=$(ifconfig $WLAN 2>/dev/null | sed -n 's/.*inet addr:\([0-9.]*\).*/\1/p') gw=$2
    [ "$ip" = "$1" ] || return 1
    android || { [ "$(getprop dhcp.$WLAN.result)" = ok ] && [ "$(getprop dhcp.$WLAN.reason)" = STATIC ]; } || return 1
    answers $2
}
# Android: the network's IP configuration, then the link made again so that the framework takes it: off the network
# for a look or more (5 at most), then on it again
aipconf() {   # id, then auto or: ip prefix gateway dns dns2|-
    local n=$1 r k; shift
    if [ "$1" = auto ]; then set -- auto; else set -- $1/$2 $3 $4 ${5#-}; fi
    r=$(aval ip $n "$@")
    # a network Amazon's setup saved: root may not change it (ajoin says the same), the system user may
    if [ "$r" != "$n" ] && [ -z "$WIFICTL_USER" ] && [ -x "${0%/*}/runas" ]; then WIFICTL_USER=system; r=$(aval ip $n "$@"); fi
    [ "$r" = "$n" ] || return 1
    A reassoc > /dev/null
    k=0; while [ $k -lt 5 ] && [ "$(field "$(A status)" wpa_state)" = COMPLETED ]; do sleep $STEP; k=$((k + 1)); done
    k=0; while [ $k -lt $JOIN_SECS ] && [ "$(field "$(A status)" wpa_state)" != COMPLETED ]; do sleep $STEP; k=$((k + 1)); done
}
# Reached on address $1: a connection from elsewhere to one of hassmic's ports there (ESPHome 26053, Wyoming 16700,
# Sendspin 28928, the push port 28929, the settings page 28931: main.sh's reach_check counts the same), not there at
# $2's snapshot (taken as the address was set: with the address unchanged, the links from before prove nothing), the
# other end neither loopback nor the Echo itself.  Not the Echo's own connections out: they prove nothing of who can
# reach it.  Established, or in TIME_WAIT: the settings page answers one request a connection and closes it first,
# so its connections stay a minute in TIME_WAIT on the Echo's side (/proc/net/tcp: "a.b.c.d:port" in hex).  Music
# Assistant's Sendspin connects again within seconds (Dot 2, 2026-11-24: 7 s), Home Assistant within two minutes.
links() { while read -r _ l r st _; do case $st in 01|06) echo "$l-$r";; esac; done < $TCP 2>/dev/null; }
reached() {
    local h=$(tcphex $1) x l r
    for x in $(links); do
        l=${x%-*} r=${x#*-}
        [ "${l%:*}" = "$h" ] || continue
        case ${l#*:} in 65C5|413C|7100|7101|7103) ;; *) continue;; esac
        case ${r%:*} in 0100007F|00000000|$h) continue;; esac
        case " $2 " in *" $x "*) continue;; esac
        return 0
    done
    return 1
}

# The request: the address on the network the Echo is on now, tried before it is kept.  Fixed: not taken by another
# device (it answers ARP), the address on, the router answers, then a connection reaches it (the settings page asks
# the new address, Home Assistant connects again) within REACH_SECS; else the Echo goes back to what it had.  DHCP:
# a lease and the router behind it.  A setting that worked is written down as the network's, and the page is told.
address() {
    local cur nid here now snap nm why=
    echo "$id applying ${aip:--}" | put address-result
    if android; then cur=$(A status); else cur=$(W status); fi
    nid=$(field "$cur" id) now=$(ifconfig $WLAN 2>/dev/null | sed -n 's/.*inet addr:\([0-9.]*\).*/\1/p')
    here=$(ssid_hex) nm=$(field "$cur" ssid | sed 's/^"//;s/"$//')
    # the record names the network it is for: no network (or none to name) is nothing to set it on
    if [ "$(field "$cur" wpa_state)" != COMPLETED ] || [ -z "$here" ]; then say "address: not on a network"; echo "$id failed offline ${now:--}" | put address-result; return; fi
    if [ -n "$aip" ] && [ "$aip" != "$now" ]; then
        ping -c 1 -W 1 $aip > /dev/null 2>&1
        if grep "^$aip " $ARP 2>/dev/null | grep -q " 0x[26] "; then
            say "address: $aip is taken by another device"; echo "$id failed inuse $now" | put address-result; return
        fi
    fi
    fixed_read; pfx=$fx pip=$fip ppre=$fpre pgw=$fgw pd1=$fd1 pd2=$fd2 pssid=$fssid
    [ "$pssid" = "$here" ] || pfx=                      # a fixed address of another network: this one is on DHCP
    snap=$(links | tr '\n' ' ')
    if [ -n "$aip" ]; then
        say "address: $aip/$apre, router $agw, DNS $ad1 ${ad2#-}, on $nm; kept once the Echo is reached there"
        set_fixed $nid $aip $apre $agw $ad1 $ad2 || why=bad
        [ -n "$why" ] || within $ADDR_SECS static_up $aip $agw || { [ "$ip" = "$aip" ] && why=nogateway || why=noaddress; }
        if [ -z "$why" ]; then
            echo "$id waiting $aip" | put address-result; status
            within $REACH_SECS reached $aip "$snap" || why=unreached
        fi
    else
        say "address: DHCP again on $nm"
        set_auto $nid || why=bad
        [ -n "$why" ] || within $ADDR_SECS dhcp_up || { [ -n "$ip" ] && why=nogateway || why=noaddress; }
    fi
    if [ -n "$why" ]; then
        say "address: $why, back to $([ -n "$pfx" ] && echo "$pip" || echo DHCP)"
        # the fixed address it had, where it had one; one that does not come back up (taken since, the router moved):
        # DHCP, and the record goes, else keep would set it again at every look
        if [ -n "$pfx" ] && set_fixed $nid $pip $ppre $pgw $pd1 $pd2 && within $BACK_SECS static_up $pip $pgw; then :
        else
            [ -n "$pfx" ] && { say "address: $pip does not come back either: DHCP"; rm -f $O/address; }
            set_auto $nid; within $BACK_SECS dhcp_up
        fi
        status
        echo "$id failed $why ${ip:--}" | put address-result
        return
    fi
    if [ -n "$aip" ]; then echo "fixed $aip $apre $agw $ad1 $ad2 $here" | put address; else rm -f $O/address; fi
    say "address: $([ -n "$aip" ] && echo "fixed at $aip" || echo "DHCP, $ip"), kept"
    status
    echo "$id ok ${ip:--}" | put address-result
}
set_fixed() { if android; then aipconf "$@"; else shift; static_on "$@"; fi; }
set_auto() { if android; then aipconf $1 auto; else PROPS=1; lease; fi; }
dhcp_up() { if android; then aonline; else online; fi; }
# true once "$@" is, looking every STEP for $1 looks; the look after the last sleep counts too
within() { local n=$1 t=0; shift; while ! "$@"; do [ $t -lt $n ] || return 1; sleep $STEP; t=$((t + 1)); done; }

# main.sh's netwatch, every look (the Dots): the fixed address wherever the network it was set on is the one the Echo is
# on, DHCP wherever it is not.  wifisvc starts dhcpcd-wlan0 at a boot and after a lost network; this takes the link
# back from it.  Not while a run of this script has the link, and holding the lock while it acts (a take that starts
# meanwhile waits for the next look of main.sh's watcher).  A network it cannot name (wpa_cli not answering) is left
# as it is: taken for another network, the Echo would move to DHCP and back with every such look.
keep() {
    android && return 0
    alive && return 0
    local st=$(W status) run=$(getprop init.svc.dhcpcd-static) here
    [ "$(field "$st" wpa_state)" = COMPLETED ] || return 0
    here=$(ssid_hex); [ -n "$here" ] || return 0
    if fixed_read && [ "$here" = "$fssid" ]; then
        [ "$run" = running ] && [ "$(getprop dhcp.$WLAN.ipaddress)" = "$fip" ] &&
            ifconfig $WLAN 2>/dev/null | grep -q "inet addr:$fip " && return 0
        lock || return 0
        say "the fixed address $fip set again"
        static_on $fip $fpre $fgw $fd1 $fd2
    elif [ "$run" = running ]; then
        lock || return 0
        say "on a network the fixed address is not for: DHCP"
        PROPS=1; lease
    fi
}

# Wait up to $1 s until wpa_supplicant is through with network $2.  why says how far it got: notfound (never tried to
# associate: out of reach, or a name typed wrong), noassoc (the access point would not have it), wrongkey (reached the
# key handshake and failed there: almost always the password)
connected() {
    t=0
    while [ $t -lt $1 ]; do
        sleep $STEP; t=$((t + 1))
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
        # its lease again, if the new network's was asked for; or its fixed address, where it has one
        if [ -n "$renewed" ] && fixed_read && [ "$(ssid_hex)" = "$fssid" ]; then static_on $fip $fpre $fgw $fd1 $fd2; within $BACK_SECS static_up $fip $fgw
        elif [ -n "$renewed" ]; then lease; t=0; while [ $t -lt $BACK_SECS ] && ! online; do sleep $STEP; t=$((t + 1)); done; fi
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
        sleep $STEP; t=$((t + 1))
    done
    if [ $t -ge $ADDR_SECS ]; then say "on $name, but $([ $why = noaddress ] && echo "no address" || echo "the router at ${gw:-?} did not answer") after $ADDR_SECS s, going back"; back; return; fi
    # It works: every network saved before goes (the same name with an older password too), then it is saved.  Not
    # wpa_supplicant's P2P groups (disabled=2): Wi-Fi Direct's, not a network to join.
    for i in $ids; do [ "$(val get_network $i disabled)" = 2 ] || W remove_network $i > /dev/null; done
    saved=1; ok save_config || { saved=0; say "!! on $name, but wpa_supplicant.conf could not be written: a reboot goes back"; }
    fixed_read && { rm -f $O/address; say "the fixed address $fip was the old network's: DHCP on this one"; }
    say "switched to $name, address $ip"
    status
    echo "$id ok $ssid $ip $saved" | put result
}

# --- checkers: the switch through WifiService.  The framework saves every change at once (its WifiConfigStore),
# so the Dots' "nothing on disk until it worked" has no equivalent; what stays is the try-out.  The new network
# goes in and is switched to; only when it comes up with an address of its own do the networks saved before go,
# and on any failure the new one is removed and the old ones enabled again, so a reboot lands on the old ones
# either way.  WifiService keeps one network per name and security: an add of a name saved already changes that
# entry in place, password included (the same network typed again, a mistyped password), so the keys are read
# first (wifictl keys: the framework hands root the stored ones) and that entry gets its own back, not removed.
# Android runs its DHCP itself once its connect lands (there is no dhcpcd): the address on the interface is the
# lease.  The gateway, to tell a silent router from none at all, comes from /proc/net/route
# (hex, little-endian; no awk on the Echo), pinged once to fill the ARP cache as the Dots' online() does.
route_gw() {
    g=$(sed -n "s/^$WLAN[[:space:]]\{1,\}00000000[[:space:]]\{1,\}\([0-9A-Fa-f]\{8\}\).*/\1/p" $ROUTE 2>/dev/null | head -n 1)
    [ -n "$g" ] || return 0
    # bytes by sed, numbers by printf's 0x: ${g:6:2} and 16# are mksh's, and the PC's tests run this under dash
    printf '%d.%d.%d.%d' $(echo $g | sed 's/\(..\)\(..\)\(..\)\(..\)/0x\4 0x\3 0x\2 0x\1/')
}
# the router at $1 answers ARP on this link (pinged once to fill the cache)
answers() {
    ping -c 1 -W 1 $1 > /dev/null 2>&1
    grep "^$1 " $ARP 2>/dev/null | grep -q " 0x[26] .* $WLAN\$"
}
agateway() {
    gw=$(route_gw)
    [ -n "$gw" ] || return 0
    answers $gw
}
aonline() {
    ip=$(ifconfig $WLAN 2>/dev/null | sed -n 's/.*inet addr:\([0-9.]*\).*/\1/p')
    [ -n "$ip" ] || return 1
    agateway
}
aback() {
    failed=$why
    if [ -z "$n" ]; then :
    elif [ -z "$replaced" ]; then A remove $n > /dev/null
    elif [ -z "$oldkey" ] || [ "$oldkey" = "*" ]; then say "!! $name was saved before, and its old password could not be read: it keeps the new one"
    elif [ "$oldkey" != - ] && [ "$(aval add $ssid "$oldkey")" != "$n" ]; then say "!! $name was saved before, and its old password could not be put back"
    fi
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
    keys=$(A keys)                                      # never printed: what goes back if the add replaces one
    n=$(aval add $ssid $psk)
    # Android 7 lets only a network's creator or the system user change it: one Amazon's setup or a stock update saved
    # is refused to root ("Not authorized to update network", cronos 2026-10-26; wifi-join.sh does the same).  Then the
    # whole switch as the system user: selecting it is refused the same way
    if [ "$n" = -1 ] && [ -z "$WIFICTL_USER" ] && [ -x "${0%/*}/runas" ]; then WIFICTL_USER=system; n=$(aval add $ssid $psk); fi
    case $n in ''|*[!0-9]*) n=;; esac
    why=notfound
    if [ -z "$n" ]; then why=bad; say "!! WifiService did not add the network"; printf '%s\n' "$id failed $ssid $why $(field "$cur" ssid)" | put result; return; fi
    replaced= oldkey=
    case " $(echo $on) " in *" $n "*) replaced=1; oldkey=$(printf '%s\n' "$keys" | sed -n "s/^$n$TAB//p");; esac
    name=$(aval name $ssid)
    say "switching to $name, from $(field "$cur" ssid | sed 's/^"//;s/"$//'); nothing forgotten until it works"
    aok select $n || { why=bad; aback; return; }
    if ! connected $JOIN_SECS $n; then say "not on $name after $JOIN_SECS s ($why), going back"; aback; return; fi
    why=noaddress t=0
    while [ $t -lt $ADDR_SECS ]; do
        aonline && break
        [ -n "$ip" ] && why=nogateway
        sleep $STEP; t=$((t + 1))
    done
    if [ $t -ge $ADDR_SECS ]; then say "on $name, but $([ $why = noaddress ] && echo "no address" || echo "the router at ${gw:-?} did not answer") after $ADDR_SECS s, going back"; aback; return; fi
    for i in $on; do [ "$i" = "$n" ] || A remove $i > /dev/null; done      # it worked: the ones before go
    fixed_read && { rm -f $O/address; say "the fixed address $fip was the old network's: DHCP on this one"; }
    say "switched to $name, address $ip"
    status
    echo "$id ok $ssid $ip 1" | put result
}

mkdir -p $O
case "$CMD" in
status) status;;
clean) rm -f $S/wifi-request; alive || rm -rf $O/lock;;
saved) Q list_networks | grep -c '^[0-9]' ;;
keep) keep;;
forget)
    if android; then
        # WifiService: every configured network goes, saved at once; the link it was on ends with its network
        # Android 7 lets only the network's creator or the system user remove it: a network of Amazon's own setup (the
        # unlock leaves one, crown 2026-10-15: "Not authorized to remove network", the Echo stayed on it) is refused to root
        for i in $(A list_networks | sed -n 's/^\([0-9][0-9]*\)[[:space:]].*/\1/p'); do
            aok remove $i && continue
            [ -x "${0%/*}/runas" ] && "${0%/*}/runas" system system env CLASSPATH=$WIFICTL app_process / Wifictl remove $i > /dev/null
        done
        [ "$(A list_networks | grep -c '^[0-9]')" = 0 ] && say "every saved network forgotten" || say "!! a saved network could not be removed: the Echo stays on it"
    else
        # Not wpa_supplicant's P2P groups (disabled=2), as in join.  Saved at once: the reset is meant to survive a reboot.
        # Removing the network it is on ends that link; no "disconnect", which would keep it off until told to reconnect.
        for i in $(W list_networks | sed -n 's/^\([0-9][0-9]*\)[[:space:]].*/\1/p'); do
            [ "$(val get_network $i disabled)" = 2 ] || W remove_network $i > /dev/null
        done
        ok save_config && say "every saved network forgotten" || say "!! networks removed, but wpa_supplicant.conf could not be written: a reboot brings them back"
    fi
    # the fixed address goes with the networks (on Android it was theirs); the Dots back on DHCP for whatever comes next
    fixed_read && say "the fixed address $fip forgotten"
    rm -f $O/scan $O/result $O/address $O/address-result
    android || [ "$(getprop init.svc.dhcpcd-static)" != running ] || dhcp_off
    status;;
take)
    R=$S/wifi-request T=$O/request
    [ -f $R ] && [ ! -L $R ] || exit 0
    lock || exit 0                                      # another run has it: the request waits for the next look
    # taken into root's folder before it is read (a rename moves a link as a link: nothing swapped in between, no FIFO
    # that holds the run), and only a plain file with one name (/data has no protected_hardlinks: a hard link to a
    # file of root's would be a plain file too); what it says goes into the log and to wpa_supplicant
    rm -rf $T; mv -f $R $T 2>/dev/null || exit 0
    if ! { [ -f $T ] && [ ! -L $T ] && set -- $(ls -ld $T 2>/dev/null) && [ "$2" = 1 ]; }; then
        rm -rf $T; say "request not a plain file, dropped"; exit 0
    fi
    read -r what id a1 a2 a3 a4 a5 a6 a7 < $T; rm -f $T
    ssid=$a1 psk=$a2
    case $id in ''|*[!0-9]*) say "request not understood, dropped"; exit 0;; esac
    case $what in
    scan) scan;;
    join)
        if hex "$ssid" 64 && { [ "$psk" = - ] || { hex "$psk" 64 && [ ${#psk} = 64 ]; }; } && [ -z "$a3" ]; then
            if android; then ajoin; else join; fi
        else say "join request not understood, dropped"; fi;;
    address)
        aip= apre= agw= ad1= ad2=
        if [ "$a1" = auto ] && [ -z "$a2" ]; then address
        elif [ "$a1" = fixed ] && [ -z "$a7" ] && fixed_ok "$a2" "$a3" "$a4" "$a5" "$a6"; then
            aip=$a2 apre=$a3 agw=$a4 ad1=$a5 ad2=$a6; address
        else say "address request not understood, dropped"; echo "$id failed bad -" | put address-result; fi;;
    *) say "request \"$what\" not understood, dropped";;
    esac;;
*) echo "usage: wifi.sh take|status|clean|forget|keep [STATE OUT] | saved"; exit 2;;
esac
exit 0
