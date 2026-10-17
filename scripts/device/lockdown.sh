#!/system/bin/sh
# Egress lock: the Echo may talk to local addresses only, never to the internet (Amazon).  Local = private ranges
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
NET_GID=3990
# Service names and the updaters' user: device.conf of this model, next to this script.  Without it the egress lock
# still goes up (it needs none of that); only stopping services and the stock-online guard have nothing to work with.
CONF="${0%/*}/device.conf"
if [ -f "$CONF" ]; then . "$CONF"; else echo "!! $CONF missing: services not stopped, stock-online guard unavailable"; fi
WLAN=${WLAN:-wlan0}
NL='
'
# Kiosk Satellite's wake word arbitration (hassmic's "arbitration_mode" setting, arb.c) broadcasts its claims to UDP
# 2330, a port it fixes and that lies outside the 16384-32767 every port of ours is in: admitted only while hassmic's
# state/config says arbitration_mode=kiosk, and taken out again when it does not.  hassmic reads nothing there but
# those claims.  Not in stock-online mode: no hassmic runs then.
KIOSK_IN="INPUT -i $WLAN -p udp -m udp --dport 2330 -j ACCEPT"
kiosk_state() { KIOSK=; [ -z "$OTA_ONLY" ] && grep -qx 'arbitration_mode=kiosk' /data/local/hassmic/state/config 2>/dev/null && KIOSK=1; }

HAVE6=; command -v ip6tables > /dev/null && HAVE6=1
v6off() { for f in /proc/sys/net/ipv6/conf/*/disable_ipv6; do echo 1 > $f || echo "!! IPv6 NOT OFF ($f): take the Echo offline"; done; }

# DNS servers from DHCP (dhcp.<iface>.dnsN) and the system's own (net.dnsN), one per line, sorted
resolvers() { getprop | sed -nE 's/^\[(dhcp\.[^.]+|net)\.dns[0-9]+\]: \[([^]]+)\]$/\2/p' | sort -u; }

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
    echo "-A hassmic_out -o lo -j RETURN"
    if [ -n "$OTA_ONLY" ]; then u=$(update_uid); [ -n "$u" ] && echo "-A hassmic_out -m owner --uid-owner $u -j DROP"; return; fi
    for d in $loc; do echo "-A hassmic_out -d $d -j RETURN"; done
    for d in $DNS; do
        case $d in *:*) [ $1 = iptables ] && continue; d=$d/128;; *) [ $1 = iptables ] || continue; d=$d/32;; esac
        for p in udp tcp; do echo "-A hassmic_out -d $d -p $p -m $p --dport 53 -j RETURN"; done
    done
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
# Take a request, then make adbd what it has to be.  True if that changed (the rules then have to follow).
adb_gate() {
    req=; got=
    if [ -f $ADB_REQ ] && [ ! -L $ADB_REQ ]; then
        got=1; read -r req < $ADB_REQ
        if [ "$req" = 1 ]; then setprop hassmic.adb.until $(($(up) + ADB_SECS)); else setprop hassmic.adb.until 0; fi
    fi
    adb_state
    port=$(getprop service.adb.tcp.port); changed=1
    # an adbd that stock has not started is left that way; it reads the port if it ever starts
    run=; [ "$(getprop init.svc.adbd)" = running ] && run="setprop ctl.restart adbd"
    if [ -n "$ADB" ] && [ "$port" != 5555 ]; then
        setprop service.adb.tcp.port 5555; $run; changed=0
        echo "adb over Wi-Fi OPEN: a root shell for everyone on the network, no password ($ADB_WHY)"
    elif [ -z "$ADB" ] && [ "$port" = 5555 ]; then
        setprop service.adb.tcp.port 0; $run; changed=0
        echo "adb over Wi-Fi closed, USB only (uptime $(up)s)"
    fi
    # the answer hassmic reads, then the request: gone means answered
    if [ -n "$ADB" ]; then [ -f $ADB_OPEN ] || : > $ADB_OPEN; else rm -f $ADB_OPEN; fi
    [ -z "$got" ] || rm -f $ADB_REQ
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
    $1 -w -P INPUT DROP
}
load() { load_once $1 2>/dev/null || load_once $1 || { echo "!! $1-restore failed twice: loading rule by rule"; load_each $1; }; }
apply() {
    DNS=$(resolvers); kiosk_state
    load iptables
    if [ -n "$HAVE6" ]; then load ip6tables; else v6off; fi
}

# What is wrong with the rules, in words, and false; nothing and true when all is as it has to be.  Three listings per
# table and hardly another process (this runs every 5 s): every rule of the chain and their order, the jump to it
# first in OUTPUT, INPUT policy DROP, each of stock's rules we need, the adb port not admitted unless it is open, nor
# Kiosk Satellite's unless in that mode; without ip6tables, IPv6 off on every interface.
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
