#!/system/bin/sh
# Egress lock: the Echo may talk to local addresses only, never to the internet (Amazon; hassmic's own sockets and, where
# it is installed, Kiosk Satellite's uid are the exceptions below).  Local = private ranges
# (RFC 1918), link-local, multicast, DHCP broadcast; IPv6: link-local, unique local, link multicast.  Home Assistant, media
# URLs, other VLANs of the home network all stay reachable without naming a subnet.
# Run BEFORE the first Wi-Fi join, and at every boot.
#   lockdown.sh          apply once
#   lockdown.sh watch    apply, then check every rule every 5 s and apply again if one is not as it has to be (stock
#                        firewall.sh flushes all rules at sys.boot_completed and inserts its own chains at position 1)
#   lockdown.sh check    change nothing: say what is wrong with the rules and exit 1, or say nothing and exit 0
#   lockdown.sh services only stop the cloud daemons, leave the firewall to the watcher: two runs rebuilding the chain at
#                        the same time interleave (duplicate rules, or the DROP ahead of some of them)
#   lockdown.sh ota-only [watch|check]
#                        stock Alexa with internet, firmware updates still impossible (MODE=stock-online in hassmic.conf):
#                        no egress lock, nothing stopped except the updaters.  Those that run as UPDATE_UID (donut: otad,
#                        ace_otad) lose all network access; UPDATE_ONDEMAND (update_engine, root) is kept stopped.
# Besides its own chain the script keeps what the satellite needs of stock's firewall (see keep below): stock's rules can
# be missing after a boot, and then the Echo is cut off with everything on it running.
# It also closes adb over Wi-Fi (see adb_gate below): in every mode, and on every run except "services" and "check".
# hassmic itself may connect anywhere: it creates its outgoing sockets with filesystem group NET_GID (runas -r in main.sh
# and run.sh, setfsgid in net.c; the owner match checks that group) and only fetches what Home
# Assistant (encrypted, paired connection) or Music Assistant send it.  That can be a public host name, a Tailscale address
# or a global IPv6 address when that is how Home Assistant is reached.  The lock is about Amazon's daemons; none of them
# has that group.
# DNS (port 53) is also allowed to the resolvers the network handed out, whatever their address: DHCP often names a public
# one next to the router (8.8.8.8), and a name that resolves publicly to a LAN address then still works.  Queries already
# leave through a local resolver anyway, so this opens nothing new.
# Firmware without ip6tables (biscuit's 6574.1 has none) cannot filter IPv6 at all: there IPv6 is switched off on every
# interface instead, so a router that hands out global IPv6 addresses cannot open a way around the lock.  hassmic only
# speaks IPv4, so nothing of ours is lost.
# Does not cover the seconds between Wi-Fi association at boot and this script: block the device's MAC at the router as
# well if "never" has to be strict.
LOCAL4="10.0.0.0/8 172.16.0.0/12 192.168.0.0/16 169.254.0.0/16 224.0.0.0/4 255.255.255.255/32"
LOCAL6="fe80::/10 fc00::/7 ff02::/16"
KSRC=; for d in $LOCAL4; do case $d in 224.*|255.*) ;; *) KSRC="$KSRC $d";; esac; done   # unicast: Kiosk Satellite's sources
NET_GID=3990
# Service names and the updaters' user: device.conf of this model, next to this script.  Without it the egress lock
# still goes up (it needs none of that); only stopping services and the stock-online guard have nothing to work with.
case $0 in */*) HERE=${0%/*};; *) HERE=.;; esac          # run by a bare name: this folder
CONF="$HERE/device.conf"
if [ -f "$CONF" ]; then . "$CONF"; else echo "!! $CONF missing: services not stopped, stock-online guard unavailable"; fi
WLAN=${WLAN:-wlan0}
NL='
'
# Kiosk Satellite's wake word arbitration (hassmic's "arbitration_mode" setting, arb.c) broadcasts its claims to UDP
# 2330, a port it fixes and that lies outside the 16384-32767 every port of ours is in: admitted only while hassmic's
# state/config says arbitration_mode=kiosk, and taken out again when it does not.  hassmic reads nothing there but
# those claims.  Not in stock-online mode: no hassmic runs then.
KIOSK_IN="INPUT -i $WLAN -p udp -m udp --dport 2330 -j ACCEPT"
# Kiosk Satellite itself (device.conf KIOSK_PACKAGE, an app of the user's choice) may reach the internet for its own
# updates and the pages it shows, unless the setting "kiosk_internet" is off: one rule for its uid, nothing else gets out
# by it (Amazon's daemons have other users and stay local).  Default on: hassmic writes the setting to state/config only
# once somebody saved it, so "not there" has to mean on (looking for "=on" kept a fresh install's Kiosk Satellite offline).
# KSUID is empty when the setting is off or it is not installed; KSAPP says it is installed, which is what admits its ports.
# KSTCP, KSSDP: what of its DLNA renderer is admitted (kiosk_dlna below).
kiosk_state() {
    local l net=on dlna= cf=/data/local/hassmic/state/config
    KIOSK=; KSUID=; KSAPP=; KSTCP=; KSSDP=; KSSDP_BY=
    [ -z "$OTA_ONLY" ] || return 0
    # whole lines, as grep -x would (a last one without its newline too); read by the shell: no fork every 5 s.  The
    # error's redirection first: one for a missing file every 5 s would fill the log
    [ -f $cf ] && [ ! -L $cf ] || cf=/dev/null         # not through a link, nor a FIFO (it would hold this loop)
    while IFS= read -r l || [ -n "$l" ]; do
        case $l in arbitration_mode=kiosk) KIOSK=1;; kiosk_internet=off) net=off;; kiosk_dlna=on) dlna=1;; esac
    done 2>/dev/null < $cf
    [ -n "$KIOSK_PACKAGE" ] || return 0
    u=$(while read -r p i _; do [ "$p" = "$KIOSK_PACKAGE" ] && { echo "$i"; break; }; done < /data/system/packages.list) 2>/dev/null
    case "$u" in *[!0-9]*) u=;; esac
    [ -n "$u" ] || return 0
    KSAPP=1
    [ $net = off ] || KSUID=$u
    [ -z "$dlna" ] || kiosk_dlna $u
    return 0
}
# The app's DLNA renderer (issue #109), off in the app by default; no login (DLNA has none: anyone on a private address
# can play on it and control it); admitted only while the setting "kiosk_dlna" is on
# (off by default, so "=on" it has to be).  athena, 2026-10 (rmaffeo): device description, SOAP control and event
# subscriptions on TCP 2326; discovery is SSDP, a controller's M-SEARCH to 239.255.255.250:1900.  From the app's source
# (jxlarrea/kiosk-satellite, dlna_manager.dart, 2026-11): the renderer binds 0.0.0.0 at its port setting (2325 when
# empty) or up to 3 above it, whichever is free first (its secure-context proxy holds 2325 on 127.0.0.1, hence 2326),
# and writes the port it got back into that setting, flutter.ks.dlna.port in FlutterSharedPreferences.xml.  A range of
# what the app listens on will not do: it also serves its voice chime on 0.0.0.0:2329 (sound_manager.dart) and an
# intercom on a port of the kernel's.  So the port is the setting's, read as the app's own user (runas: a link or a
# FIFO put there reaches only what the app could anyway, as main.sh's kiosk_prefs), and admitted only while the app's
# uid listens on it beyond loopback (/proc/net/tcp*, state 0A): the rule goes when the renderer stops, and nothing else
# that may listen there later is reached through it.  The preferences unreadable (no runas next to this script, another
# format): the app's listeners in 2325-2328, the renderer's own range at the default.  INPUT cannot ask whose socket a
# packet is for (-m owner is OUTPUT's), so 1900 is admitted only while the app holds a socket there and no other uid
# does: nothing else on the Echo gets the network's searches through this rule.  The renderer's answers, announcements
# and events leave as any local traffic does.  All of it is looked at every 5 s (wrong): another uid that binds 1900 in
# between gets the searches until then.  Each look: four /proc reads, one runas while the app listens on anything but
# 2324 and 6053, and the forks of kiosk_ports and its pipe.
RUNAS=$HERE/runas
kiosk_dlna() {
    local f la st uid p own tcp want
    own=; tcp=
    for f in /proc/net/tcp /proc/net/tcp6 /proc/net/udp /proc/net/udp6; do
        while read -r _ la _ st _ _ _ uid _; do
            case ${la%:*} in ??????7F|0000000000000000FFFF0000??????7F|00000000000000000000000001000000) continue;; esac  # 127/8, ::1
            case $f in
            */tcp*) [ "$st" = 0A ] && [ "$uid" = "$1" ] || continue
                    p=$((0x${la##*:}))
                    case " 2324 6053 $tcp " in *" $p "*) ;; *) tcp="$tcp $p";; esac;;
            *)      [ "${la##*:}" = 076C ] || continue                                  # 1900
                    if [ "$uid" = "$1" ]; then own=1; else case "$KSSDP_BY " in *" $uid "*) ;; *) KSSDP_BY="$KSSDP_BY $uid";; esac; fi;;
            esac
        done < $f
    done 2>/dev/null
    if [ -n "$tcp" ]; then
        want=$(kiosk_dlna_port "$1")
        case " $tcp " in *" $want "*) [ -n "$want" ] && KSTCP=$want;; esac
        # no setting, or one the app does not listen on (moved, its write not there yet): the default's range
        if [ -z "$KSTCP" ]; then
            for p in $tcp; do [ $p -ge 2325 ] && [ $p -le 2328 ] && KSTCP="$KSTCP $p"; done
            KSTCP=${KSTCP# }
        fi
    fi
    [ -n "$own" ] || KSSDP_BY=
    [ -n "$own" ] && [ -z "$KSSDP_BY" ] && KSSDP=1
    return 0
}
# The renderer's port from the app's preferences, as its user (u0_a<n> for uid 10000 + n), or nothing
kiosk_dlna_port() {
    [ -x "$RUNAS" ] && [ "$1" -ge 10000 ] && [ "$1" -le 19999 ] || return 0
    "$RUNAS" u0_a$(($1 - 10000)) $1 sh -c 'p=/data/data/$0/shared_prefs/FlutterSharedPreferences.xml
        [ -f "$p" ] && [ ! -L "$p" ] || exit 0
        sed -n "s|^ *<string name=\"flutter.ks.dlna.port\">\([0-9]\{1,5\}\)</string> *$|\1|p" "$p"' "$KIOSK_PACKAGE" 2>/dev/null |
        while read -r p; do [ "$p" -ge 1024 ] && [ "$p" -le 65535 ] && echo $p; break; done 2>/dev/null
}
# INPUT rules for the app: TCP to the ports $2..., and with $1 = 1 SSDP's multicast to 1900; from private addresses only.
kiosk_rules() {
    local s d p
    s=$1; shift
    for d in $KSRC; do
        for p in "$@"; do echo "INPUT -s $d -i $WLAN -p tcp -m tcp --dport $p -j ACCEPT"; done
        [ "$s" != 1 ] || echo "INPUT -s $d -d 239.255.255.250/32 -i $WLAN -p udp -m udp --dport 1900 -j ACCEPT"
    done
}
# Those wanted now.  Its remote administration (its web page and API, TCP 2324) and its ESPHome API (TCP 6053: Home
# Assistant's ESPHome integration connects to it; hassmic's own is 26053), both outside 16384-32767, from private
# addresses only, and only with the app installed: what might listen there otherwise is not for the network.  Its DLNA
# renderer's as kiosk_dlna found them.
kiosk_ports() { [ -z "$KSAPP" ] || kiosk_rules "$KSSDP" 2324 6053 $KSTCP; }
# Those rules of the listing $1 ("iptables -S") that kiosk_rules could have made (from a source of KSRC, TCP to a port of
# 1024-65535: the renderer's setting may name any; or SSDP's) and that are not wanted now: to take out.  Every 5 s
# (wrong), so no fork per rule: the shape is read with the shell's own patterns.  On the screen models no rule of
# anyone else's has that shape (no stock firewall.sh, FW_KEEP is DHCP's).
kiosk_stale() {
    local w r s p g=$-
    [ -n "$KIOSK_PACKAGE" ] || return 0                    # a model without the app never loads any
    case "$1" in *"-A INPUT -s "*) ;; *) return 0;; esac    # ip6tables: none of them has a source
    w=$NL$(kiosk_ports)$NL
    # lines by IFS and no glob, so no pipe and its subshell (40 ms a table on checkers, 2026-11-11)
    local IFS=$NL; set -f
    for r in $1; do
        case "$r" in "-A INPUT -s "*" -i $WLAN -p "*" -j ACCEPT") ;; *) continue;; esac
        s=${r#"-A INPUT -s "}; s=${s%% *}
        case " $KSRC " in *" $s "*) ;; *) continue;; esac
        case "$r" in
        "-A INPUT -s $s -i $WLAN -p tcp -m tcp --dport "*" -j ACCEPT")
            p=${r#"-A INPUT -s $s -i $WLAN -p tcp -m tcp --dport "}; p=${p%" -j ACCEPT"}
            case "$p" in ''|*[!0-9]*|??????*) continue;; esac
            [ "$p" -ge 1024 ] && [ "$p" -le 65535 ] || continue;;
        "-A INPUT -s $s -d 239.255.255.250/32 -i $WLAN -p udp -m udp --dport 1900 -j ACCEPT") ;;
        *) continue;;
        esac
        r=${r#-A }
        case "$w" in *"$NL$r$NL"*) ;; *) echo "$r";; esac
    done
    case $g in *f*) ;; *) set +f;; esac
}

HAVE6=; command -v ip6tables > /dev/null && HAVE6=1
v6off() { for f in /proc/sys/net/ipv6/conf/*/disable_ipv6; do echo 1 > $f || echo "!! IPv6 NOT OFF ($f): take the Echo offline"; done; }

# DNS servers from DHCP (dhcp.<iface>.dnsN) and the system's own (net.dnsN), one per line, sorted, without a scope.
# Android 7.1 writes net.dnsN with Java's getHostAddress, which names the interface of a link-local IPv6 resolver the
# router announces ("fe80::1%wlan0"); ip6tables lists no scope (it refuses the rule, or loads it as fe80::1), so the
# chain never matched its rules and was loaded again every 5 s (Echo Show 5, issue #100).
resolvers() { getprop | sed -nE 's/^\[(dhcp\.[^.]+|net)\.dns[0-9]+\]: \[([^]%]+)(%[^]]*)?\]$/\2/p' | sort -u; }

# The updaters' user as a number: UPDATE_UID (a user of the image, Dots), else the uid Android gave the updater app
# (UPDATE_PACKAGES, checkers: packages.list, "<name> <uid> ...").  Nothing if neither is there: no rule then, and on
# checkers the updater stays disabled in stock-online (alexa-on.sh keep-updates).
update_uid() {
    if [ -n "$UPDATE_UID" ]; then id -u "$UPDATE_UID"; return; fi
    for p in $UPDATE_PACKAGES; do sed -n "s/^$p \([0-9][0-9]*\) .*/\1/p" /data/system/packages.list 2>/dev/null; done | head -1
}

# The chain as it has to be, for $1 = iptables or ip6tables: one rule per line, worded as "iptables -S" prints it (a
# length on every address, "-m udp" after "-p udp", the updaters' user as a number), because the check compares that
# text.  The chain is loaded from this list and checked against it.
rules() {
    if [ $1 = iptables ]; then loc=$LOCAL4; else loc=$LOCAL6; fi
    # adbd asks for no key and its shell is root, and loopback is open: while adb over Wi-Fi is open, hassmic could
    # connect to it itself (127.0.0.1:5555, or its own address, which goes over lo too) and be root.  hassmic is the
    # one that asks for adb to be opened, so one taken over would only have had to ask (issue #130).  No process on
    # the Echo but root's reaches the port, however it opens the socket (another user's service included): the owner
    # match on the user, first, before lo is let through.  The network's own connections are not this chain's.
    echo "-A hassmic_out -p tcp -m tcp --dport 5555 -m owner ! --uid-owner 0 -j DROP"
    echo "-A hassmic_out -o lo -j RETURN"
    if [ -n "$OTA_ONLY" ]; then u=$(update_uid); [ -n "$u" ] && echo "-A hassmic_out -m owner --uid-owner $u -j DROP"; return; fi
    for d in $loc; do echo "-A hassmic_out -d $d -j RETURN"; done
    for d in $DNS; do
        case $d in *:*) [ $1 = iptables ] && continue; d=$d/128;; *) [ $1 = iptables ] || continue; d=$d/32;; esac
        for p in udp tcp; do echo "-A hassmic_out -d $d -p $p -m $p --dport 53 -j RETURN"; done
    done
    [ -n "$KSUID" ] && echo "-A hassmic_out -m owner --uid-owner $KSUID -j RETURN"
    echo "-A hassmic_out -m owner --gid-owner $NET_GID -j RETURN"
    echo "-A hassmic_out -j DROP"
}

# Stock's rules that the satellite cannot do without, in the same wording.  firewall.sh loads them at sys.boot_completed
# (INPUT and OUTPUT policy DROP, then these among a hundred others), but it calls iptables without -w, and a call that
# meets another iptables fails with "Another app is currently holding the xtables lock": that rule is then simply not
# there (Dot 3, 2026-09-30: 2 of 100 appends lost beside a second writer; our own watcher is one).  Seen in the field
# (issue #1): no "-o wlan0 -j ACCEPT" after a boot, so nothing left the Echo - lock up, hassmic running, Home Assistant
# "unavailable".  Any of the others missing looks the same from outside.
keep() {
    echo "-A OUTPUT -o $WLAN -j ACCEPT"                 # the chain only hands on what may pass: this lets it out
    echo "-A OUTPUT -o lo -j ACCEPT"
    echo "-A INPUT -i lo -j ACCEPT"
    for p in tcp udp; do echo "-A INPUT -i $WLAN -p $p -m state --state RELATED,ESTABLISHED -j ACCEPT"; done   # answers to us
    if [ $1 = iptables ]; then
        # every port hassmic listens on is in this range (ESPHome, Wyoming, Sendspin, push updates, arbitration)
        for p in udp tcp; do echo "-A INPUT -i $WLAN -p $p -m $p --dport 16384:32767 -j ACCEPT"; done
        echo "-A INPUT -i $WLAN -p udp -m udp --dport 5353 -j ACCEPT"      # mDNS: how Home Assistant finds the Echo
        echo "-A INPUT -p icmp -m state --state RELATED,ESTABLISHED -j ACCEPT"
        # ping (is the Echo up? Home Assistant's Ping integration, a router's device list): echo requests from private
        # and link-local addresses only, never from a multicast or broadcast one; the reply leaves under the chain's
        # local-address rules like everything else (issue #45)
        for d in $LOCAL4; do
            case $d in 224.*|255.*) continue;; esac
            echo "-A INPUT -s $d -i $WLAN -p icmp -m icmp --icmp-type 8 -j ACCEPT"
        done
        kiosk_ports | while read -r r; do echo "-A $r"; done                # with the app installed (kiosk_state)
        if [ -n "$ADB" ]; then echo "-A $ADB_IN"; fi                       # only while adb over Wi-Fi is open (adb_gate)
        # what this model's stock lacks (device.conf FW_KEEP, rules separated by ";"): checkers has no firewall.sh, and
        # nothing else admits the DHCP server's answers once INPUT drops (they come to a broadcast, not as a reply)
        [ -z "$FW_KEEP" ] || echo "$FW_KEEP" | tr ';' '\n' | while read -r r; do [ -n "$r" ] && echo "-A $r"; done
        if [ -n "$KIOSK" ]; then echo "-A $KIOSK_IN"; fi                   # only in Kiosk Satellite mode (kiosk_state)
    else
        echo "-A INPUT -p icmpv6 -j ACCEPT"                                # neighbour discovery: no IPv6 without it
        echo "-A INPUT -i $WLAN -p udp -m udp --dport 546 -j ACCEPT"       # DHCPv6
    fi
}
# Those of them that the listing $2 ("iptables -S") does not have
missing() { keep $1 | while read -r r; do case "$NL$2$NL" in *"$NL$r$NL"*) ;; *) echo "$r";; esac; done; }

# adb over Wi-Fi.  Stock leaves it open to everyone: init sets service.adb.tcp.port 5555 at "on boot", firewall.sh admits
# the port from any address, and the root that the unlock gives (ro.adb.secure=0, fos_flags noadbauth) asks for no key:
# a root shell for whoever reaches the Echo on the network (issue #1).  So it is closed twice over: adbd runs without
# its TCP listener (USB is not touched), and stock's INPUT rule is taken out and kept out.  It is open only
#   - for ADB_SECS after hassmic asked for it (state/adb-request = 1), which it does for the switch in Home Assistant
#     (only over the connection with the key) and for whoever signs its challenge with the update key
#     (scripts/adb-wifi.sh: the way in without Home Assistant); 0 closes it again, or
#   - while hassmic.conf has ADB_WIFI=1: for development, and the only way with Wyoming or MODE=stock-online.
# The end of the window is a property (uptime in seconds): every run of this script sees it, a restarted watcher goes
# on with it, and a reboot forgets it, so every boot starts closed.  While it is open, the file adb-open exists next to
# hassmic.conf: that is what hassmic reports back.  Not in state/: that directory is the daemon's, and root writing
# through a name in there writes wherever a link points.
# adbd takes the port from the property at its start, hence the restart.  ctl.restart and not stop + start: run from
# an adb shell this script dies with adbd, and it would be between the two.  Dot 2, 2026-09-30: port 0 + restart, USB
# back within 8 s, nothing listening on 5555, connect refused from the PC; 5555 + restart, listening again.
HCONF=/data/local/hassmic/hassmic.conf
ADB_REQ=/data/local/hassmic/state/adb-request
ADB_OPEN=/data/local/hassmic/adb-open
ADB_REQ_TAKEN=/data/local/hassmic/adb-request.taken
ADB_SECS=1800
ADB_IN="INPUT -p tcp -m tcp --dport 5555 -j ACCEPT"          # stock's rule, as "iptables -S" prints it
up() { read -r upt _ < /proc/uptime; echo ${upt%.*}; }
# ADB=1 if it is to be open now, ADB_WHY says on whose account
adb_state() {
    ADB=; ADB_WHY=
    if grep -q -e '^ADB_WIFI=1' -e '^ADB_WIFI="1' $HCONF 2>/dev/null; then ADB=1; ADB_WHY="ADB_WIFI=1 in hassmic.conf"; return; fi
    u=$(getprop hassmic.adb.until)
    if [ "${u:-0}" -gt "$(up)" ] 2>/dev/null; then ADB=1; ADB_WHY="asked for through hassmic, $(( (u - $(up) + 59) / 60 )) min left"; fi
}
# a socket listening on port 5555 (hex 15B3, state 0A), IPv4 or IPv6; no awk on the Echo
adb_listening() { grep -qE '^ *[0-9]+: [0-9A-F]+:15B3 [0-9A-F]+:0000 0A ' /proc/net/tcp /proc/net/tcp6 2>/dev/null; }
# Take a request, then make adbd what it has to be.  True if that changed (the rules then have to follow).
adb_gate() {
    req=; got=
    # taken into root's folder before it is read (a rename moves a link as a link: no FIFO that holds this loop), and
    # only a plain file with one name, as main.sh's take()
    rm -rf $ADB_REQ_TAKEN 2>/dev/null
    if { [ -e $ADB_REQ ] || [ -L $ADB_REQ ]; } && mv -f $ADB_REQ $ADB_REQ_TAKEN 2>/dev/null &&
            [ -f $ADB_REQ_TAKEN ] && [ ! -L $ADB_REQ_TAKEN ] && set -- $(ls -ld $ADB_REQ_TAKEN 2>/dev/null) && [ "$2" = 1 ]; then
        got=1; read -r req < $ADB_REQ_TAKEN
        if [ "$req" = 1 ]; then setprop hassmic.adb.until $(($(up) + ADB_SECS)); else setprop hassmic.adb.until 0; fi
    fi
    adb_state
    port=$(getprop service.adb.tcp.port); changed=1
    # an adbd that stock has not started is left that way; it reads the port if it ever starts.  The restart itself is
    # apply's, after the rules (see there).
    run=; [ "$(getprop init.svc.adbd)" = running ] && run=1
    if [ -n "$ADB" ] && [ "$port" != 5555 ]; then
        setprop service.adb.tcp.port 5555; ADB_RESTART=$run; changed=0
        echo "adb over Wi-Fi OPEN: a root shell for everyone on the network, no password ($ADB_WHY)"
    elif [ -z "$ADB" ] && [ "$port" = 5555 ]; then
        setprop service.adb.tcp.port 0; ADB_RESTART=$run; changed=0
        echo "adb over Wi-Fi closed, USB only (uptime $(up)s)"
    elif [ -z "$ADB" ] && [ -n "$run" ] && adb_listening; then
        # the port says closed and adbd still listens: a restart that never came (apply's runs after its caller ended,
        # and something may kill it first).  The rules drop 5555 meanwhile.
        ADB_RESTART=1; changed=0
        echo "adbd still listened on 5555 with adb over Wi-Fi closed: restarted"
    fi
    # the answer hassmic reads; the request is gone already (taken above, a few ms before: hassmic looks every 2 s)
    if [ -n "$ADB" ]; then [ -f $ADB_OPEN ] || : > $ADB_OPEN; else rm -f $ADB_OPEN; fi
    [ -z "$got" ] || rm -f $ADB_REQ_TAKEN
    return $changed
}

# Everything in one iptables-restore call: the chain (declared again = emptied), the jump to it once and first, those
# of stock's rules that are missing, stock's adb rule out while adb over Wi-Fi is closed, INPUT policy DROP (stock's
# too: without it every open port of Amazon's daemons is reachable).  One commit, so the chain is never half built, and two holds of the xtables lock (a listing, the call)
# instead of some thirty: each is a moment for a call of stock's firewall.sh to fail (see keep).  Dot 2, 2026-09-30,
# beside a loop of appends without -w: 0.95 of those failed per load this way and a load took 0.3 s, against 6.3 and
# 4.7 s rule by rule (-w waits in steps of 1 s).  A line that is refused fails the whole call and leaves everything
# as it was; the jumps to take out come from the listing, so a flush by stock between listing and call does that.
# The lines are put together first: iptables-restore holds the lock from its start, and a listing taken while it
# waits for its input waits for that lock for ever.
load_once() {
    all=$($1 -w -S 2>/dev/null)
    in=$(
        echo "*filter"; echo ":INPUT DROP [0:0]"; echo ":hassmic_out - [0:0]"
        rules $1
        echo "$all" | while read -r r; do [ "$r" = "-A OUTPUT -j hassmic_out" ] && echo "-D OUTPUT -j hassmic_out"; done
        echo "-I OUTPUT 1 -j hassmic_out"
        missing $1 "$all"
        [ -n "$ADB" ] || echo "$all" | while read -r r; do [ "$r" = "-A $ADB_IN" ] && echo "-D $ADB_IN"; done
        [ -n "$KIOSK" ] || echo "$all" | while read -r r; do [ "$r" = "-A $KIOSK_IN" ] && echo "-D $KIOSK_IN"; done
        kiosk_stale "$all" | while read -r r; do echo "-D $r"; done
        echo COMMIT
    )
    echo "$in" | $1-restore -w --noflush
}
# The same rule by rule.  Stock firewall.sh uses the owner match itself, so the kernel has it; if a rule fails to load
# all the same, the others still go up this way, and it must not pass silently.
load_each() {
    $1 -w -N hassmic_out 2>/dev/null
    $1 -w -F hassmic_out
    rules $1 | while read -r r; do
        $1 -w $r || case "$r" in
            "-A hassmic_out -m owner --uid-owner $KSUID -j RETURN") echo "!! Kiosk Satellite limited to local addresses ($1)";;
            *uid-owner*) echo "!! OTA GUARD NOT ACTIVE ($1): take the Echo offline";;
            *gid-owner*) echo "!! hassmic limited to local addresses ($1)";;
            *) echo "!! rule not loaded ($1): $r";;
        esac
    done
    while $1 -w -D OUTPUT -j hassmic_out 2>/dev/null; do :; done
    $1 -w -I OUTPUT 1 -j hassmic_out
    missing $1 "$($1 -w -S)" | while read -r r; do $1 -w $r || echo "!! rule not loaded ($1): $r"; done
    [ -n "$ADB" ] || while $1 -w -D $ADB_IN 2>/dev/null; do :; done
    [ -n "$KIOSK" ] || while $1 -w -D $KIOSK_IN 2>/dev/null; do :; done
    kiosk_stale "$($1 -w -S)" | while read -r r; do while $1 -w -D $r 2>/dev/null; do :; done; done
    $1 -w -P INPUT DROP
}
load() { load_once $1 2>/dev/null || load_once $1 || { echo "!! $1-restore failed twice: loading rule by rule"; load_each $1; }; }
apply() {
    DNS=$(resolvers); kiosk_state
    [ -z "$KSSDP_BY" ] || echo "Kiosk Satellite's DLNA renderer: UDP 1900 not admitted, uid$KSSDP_BY listens there too"
    load iptables
    if [ -n "$HAVE6" ]; then load ip6tables; else v6off; fi
    # adbd last, and a second later in the background: its restart ends every adb shell, the one running this script
    # too.  Stock opens port 5555 at every boot, so the first run over adb after a boot (setup.sh's lock-down step)
    # closes it, and restarting adbd at once ended that run with exit 255 before a single rule was loaded (issue #49).
    # HUP ignored: the restart must happen once the port is set.  If it does not all the same, adbd listens on 5555
    # while the property says 0: adb_gate's next run (the watcher's, 5 s) sees the listener and restarts it.
    [ -z "$ADB_RESTART" ] || { ADB_RESTART=; ( trap '' HUP; sleep 1; setprop ctl.restart adbd ) < /dev/null > /dev/null 2>&1 & }
}

# What is wrong with the rules, in words, and false; nothing and true when all is as it has to be.  Three listings per
# table and hardly another process (this runs every 5 s): every rule of the chain and their order, the jump to it
# first in OUTPUT, INPUT policy DROP, each of stock's rules we need, the adb port not admitted unless it is open, nor
# 2330 unless in Kiosk Satellite mode, nor a port of the app's that is not wanted now; without ip6tables, IPv6 off on every interface.
wrong() {
    kiosk_state
    for t in iptables ${HAVE6:+ip6tables}; do
        [ "$($t -w -S hassmic_out 2>/dev/null)" = "-N hassmic_out$NL$(rules $t)" ] || { echo "$t: hassmic_out is not as loaded"; return 1; }
        o=$($t -w -S OUTPUT 2>/dev/null)                   # policy line, then its rules (cutting them out of the whole
        case "${o#*"$NL"}$NL" in "-A OUTPUT -j hassmic_out$NL"*) ;; *) echo "$t: hassmic_out is not first in OUTPUT"; return 1;; esac   # listing took mksh 0.9 s)
        all=$($t -w -S 2>/dev/null)
        case "$NL$all" in *"$NL-P INPUT DROP$NL"*) ;; *) echo "$t: INPUT policy is not DROP"; return 1;; esac
        m=$(missing $t "$all"); [ -z "$m" ] || { echo "$t: missing:" $m; return 1; }
        [ -n "$ADB" ] || case "$NL$all$NL" in *"$NL-A $ADB_IN$NL"*) echo "$t: port 5555 (adb) admitted"; return 1;; esac
        [ -n "$KIOSK" ] || case "$NL$all$NL" in *"$NL-A $KIOSK_IN$NL"*) echo "$t: port 2330 (Kiosk Satellite) admitted"; return 1;; esac
        s=$(kiosk_stale "$all"); [ -z "$s" ] || { echo "$t: Kiosk Satellite's ports admitted, not wanted now:" $(echo "$s" | sed 's/.*--dport \([0-9]*\).*/\1/' | sort -u); return 1; }
    done
    [ -n "$HAVE6" ] || for f in /proc/sys/net/ipv6/conf/*/disable_ipv6; do
        v=; { read v < $f; } 2>/dev/null; [ "$v" = 1 ] || { echo "IPv6 is on (${f%/*})"; return 1; }
    done
    return 0
}
ota_off() { for s in $UPDATE_SERVICES $UPDATE_ONDEMAND; do stop $s 2>/dev/null; done; }

OTA_ONLY=; [ "$1" = ota-only ] && { OTA_ONLY=1; shift; }
[ "$1" = check ] && { DNS=$(resolvers); adb_state; wrong; exit; }
# A request left from before a reboot is not one: the window would open at boot with nobody asking.
[ "$1" = watch ] && rm -f $ADB_REQ

if [ -n "$OTA_ONLY" ]; then
    adb_gate; apply; ota_off
    echo "stock-online: only the updaters are cut off"; iptables -w -S hassmic_out
    # update_engine is started on demand, and "start" undoes a "stop": keep at it.
    [ "$1" = watch ] && while sleep 5; do
        [ -n "$HAVE6" ] || v6off
        if adb_gate; then apply; elif ! w=$(wrong); then apply; echo "OTA guard re-applied ($w)"; fi
        ota_off
    done
    exit 0
fi

[ "$1" = services ] || { adb_gate; apply; }
# Everything that phones home.  mixer, shmd, ledcontroller, acebuttond, netmgrd, wifisvc stay.
# perfmonitord stays too: every new micAsr stream makes the mixer connect to it over AIPC and wait up to 20 s for it
# before opening the mic, so without it each hassmic (re)start was 20 s deaf.  Anything it sends out is dropped by the egress lock.
for s in $ALEXA_SERVICES $UPDATE_SERVICES $UPDATE_ONDEMAND $CLOUD_SERVICES; do stop $s 2>/dev/null; done
[ -n "$ALEXA_PROP" ] && setprop $ALEXA_PROP 0
[ "$1" = services ] && exit 0
echo "egress limited to local addresses$([ -n "$HAVE6" ] || echo ", IPv6 off (no ip6tables)")"; iptables -w -S hassmic_out

# Re-apply as well when the resolvers change (other network, new DHCP lease).
[ "$1" = watch ] && while sleep 5; do
    [ -n "$HAVE6" ] || v6off                        # an interface that comes up later starts with IPv6 on
    if adb_gate; then apply                         # says so itself
    elif [ "$(resolvers)" != "$DNS" ]; then apply; echo "lockdown re-applied, resolvers:" $DNS
    elif ! w=$(wrong); then apply; echo "lockdown re-applied ($w)"; fi
done
exit 0
