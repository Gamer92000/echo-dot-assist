#!/system/bin/sh
# Egress lock: the Echo may talk to the local subnet only.  Run BEFORE the first Wi-Fi join, and at every boot.
#   lockdown.sh 192.168.1.0/24          apply once
#   lockdown.sh 192.168.1.0/24 watch    apply, then re-assert every 5 s (stock firewall.sh flushes all rules at
#                                       sys.boot_completed and inserts its own chains at position 1)
# UNTESTED on device.  Does not cover the seconds between Wi-Fi association at boot and this script: block the
# device's MAC at the router as well if "never" has to be strict.
LAN=${1:?usage: lockdown.sh <lan-cidr> [watch]}

apply() {
    iptables -w -N hassmic_out 2>/dev/null
    iptables -w -F hassmic_out
    iptables -w -A hassmic_out -o lo -j RETURN
    iptables -w -A hassmic_out -d $LAN -j RETURN
    iptables -w -A hassmic_out -d 224.0.0.0/4 -j RETURN          # mDNS
    iptables -w -A hassmic_out -d 255.255.255.255 -j RETURN      # DHCP
    iptables -w -A hassmic_out -j DROP
    while iptables -w -D OUTPUT -j hassmic_out 2>/dev/null; do :; done
    iptables -w -I OUTPUT 1 -j hassmic_out
    # IPv6: link-local only
    ip6tables -w -N hassmic_out 2>/dev/null
    ip6tables -w -F hassmic_out
    ip6tables -w -A hassmic_out -o lo -j RETURN
    ip6tables -w -A hassmic_out -d fe80::/10 -j RETURN
    ip6tables -w -A hassmic_out -d ff02::/16 -j RETURN
    ip6tables -w -A hassmic_out -j DROP
    while ip6tables -w -D OUTPUT -j hassmic_out 2>/dev/null; do :; done
    ip6tables -w -I OUTPUT 1 -j hassmic_out
}

first() { iptables -w -S OUTPUT 2>/dev/null | grep -m1 '^-A' | grep -q hassmic_out; }

apply
# Everything that phones home.  mixer, shmd, ledcontroller, acebuttond, netmgrd, wifisvc stay.
for s in puffin puffinmrmd dacd smarthomed otad ace_otad update_engine assetmgrd gadgetsd logmgr ace_metricd \
         minerva_service usagestat_protod aceusagestatd trackerd provisionerd adepd sntpd UdssCampSvc fmonitor \
         perfmonitord ace_messaging ahe shs; do stop $s 2>/dev/null; done
setprop com.amazon.puffin.PUFFIN_START 0
echo "egress limited to $LAN"; iptables -w -S hassmic_out

[ "$2" = watch ] && while sleep 5; do first || { apply; echo "lockdown re-applied"; }; done
