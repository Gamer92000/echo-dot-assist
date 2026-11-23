#!/bin/sh
# Bridge hassmic's signed Wi-Fi requests to this device's wpa_supplicant.
# A trial is kept in memory until association and a fresh DHCP lease succeed.
set -u
umask 077
base=${L09G_WIFI_BASE:-/data/l09g-assist}
state=${HASSMIC_STATE:-$base/state}
out=${HASSMIC_WIFI:-/run/l09g-assist-wifi}
iface=${L09G_WIFI_IFACE:-wlan0}
step=${L09G_WIFI_STEP:-1}
scan_wait=${L09G_WIFI_SCAN_WAIT:-5}
join_tries=${L09G_WIFI_JOIN_TRIES:-30}
dhcp_tries=${L09G_WIFI_DHCP_TRIES:-30}
back_tries=${L09G_WIFI_BACK_TRIES:-45}
request=$state/wifi-request
tag=l09g-current-lan
transaction=0
mkdir -p "$state" "$out"
chmod 700 "$out"

wpa() { wpa_cli -i "$iface" "$@" 2>/dev/null; }
value() { wpa "$@" | tail -n 1; }
ok() { [ "$(value "$@")" = OK ]; }
put() { cat > "$out/.$1.$$" && mv -f "$out/.$1.$$" "$out/$1"; }
field() { printf '%s\n' "$1" | sed -n "s/^$2=//p" | head -n 1; }
status() { wpa status | grep -E '^(wpa_state|id|ssid|ip_address|freq)=' | put status; }
numeric() { case "$1" in ''|*[!0-9]*) return 1;; esac; }
hex() { case "$1" in ''|*[!0-9a-f]*) return 1;; esac; [ ${#1} -le "$2" ] && [ $(( ${#1} % 2 )) -eq 0 ]; }

# SpeakerLab's firewall stays in place. Its stock rule names the old subnet;
# add a marked rule only for the current on-link subnet, and remove our old one.
firewall() {
    iptables -S SPEAKER_LOCAL >/dev/null 2>&1 || return 0
    lan=$(ip -4 route show dev "$iface" scope link 2>/dev/null | awk '$1 ~ /^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+\/[0-9]+$/ {print $1; exit}')
    case "$lan" in ''|0.0.0.0/*) return 1;; *[!0-9./]*) return 1;; esac
    if ! iptables -C SPEAKER_LOCAL -d "$lan" -m comment --comment "$tag" -j RETURN >/dev/null 2>&1; then
        iptables -I SPEAKER_LOCAL 2 -d "$lan" -m comment --comment "$tag" -j RETURN >/dev/null 2>&1 || return 1
    fi
    old_lan=$(cat "$out/allowed-subnet" 2>/dev/null || true)
    case "$old_lan" in ''|*[!0-9./]*) old_lan=;; esac
    if [ -n "$old_lan" ] && [ "$old_lan" != "$lan" ]; then
        iptables -D SPEAKER_LOCAL -d "$old_lan" -m comment --comment "$tag" -j RETURN >/dev/null 2>&1 || true
    fi
    printf '%s\n' "$lan" | put allowed-subnet
}

scan() {
    wpa scan >/dev/null || true
    sleep "$scan_wait"
    { printf 'id %s\n' "$id"; wpa scan_results | grep -E '^[0-9a-fA-F][0-9a-fA-F]:' || true; } | put scan
    status
}

connected() {
    target=$1 tries=$2; i=0
    while [ "$i" -lt "$tries" ]; do
        cur=$(wpa status); st=$(field "$cur" wpa_state); current=$(field "$cur" id)
        if [ "$st" = COMPLETED ] && [ "$current" = "$target" ]; then return 0; fi
        case "$st" in 4WAY_HANDSHAKE|GROUP_HANDSHAKE) why=wrongkey;; AUTHENTICATING|ASSOCIATING|ASSOCIATED) [ "$why" != notfound ] || why=noassoc;; esac
        sleep "$step"; i=$((i + 1))
    done
    return 1
}

# Release the old lease before a fresh request: an address left from another
# Wi-Fi must never make an unsuccessful switch look successful.
renew_dhcp() {
    if ! dhcpcd -k "$iface" >/dev/null 2>&1; then
        # A failed previous attempt may already have stopped the daemon.  It
        # is safe to start it again only when no old global address remains.
        ip -o -4 addr show dev "$iface" scope global 2>/dev/null | grep -q ' inet ' && return 1
    fi
    # -n starts dhcpcd if the release stopped its last interface; -b keeps
    # the timeout under our control even when the candidate has no DHCP.
    dhcpcd -n -b -f /etc/dhcpcd.conf "$iface" >/dev/null 2>&1 || return 1
}
online() {
    check=$(wpa status)
    [ "$(field "$check" wpa_state)" = COMPLETED ] && [ "$(field "$check" id)" = "$1" ] || return 1
    ipaddr=$(ip -o -4 addr show dev "$iface" scope global 2>/dev/null | awk '{split($4,a,"/"); print a[1]; exit}')
    case "$ipaddr" in ''|0.*|169.254.*|*[!0-9.]*) return 1;; esac
    gateway=$(ip -4 route show default dev "$iface" 2>/dev/null | awk '$1 == "default" && $2 == "via" {print $3; exit}')
    case "$gateway" in ''|*[!0-9.]*) return 1;; esac
    firewall || return 1
    ping -c 1 -W 1 "$gateway" >/dev/null 2>&1 && return 0
    # A router can ignore ICMP yet still answer ARP on the local link.
    awk -v gw="$gateway" -v dev="$iface" '$1 == gw && ($3 == "0x2" || $3 == "0x6") && $6 == dev { found=1 } END { exit !found }' "${L09G_WIFI_ARP:-/proc/net/arp}"
}

restore_enabled() {
    for old_enabled in $enabled; do ok enable_network "$old_enabled" || true; done
}
back() {
    transaction=0
    failed=$why
    [ -z "${new_id:-}" ] || wpa remove_network "$new_id" >/dev/null
    if [ -n "$old" ]; then ok select_network "$old" || true; else wpa reassociate >/dev/null; fi
    restore_enabled
    was=; why=notfound
    if [ -n "$old" ] && connected "$old" "$back_tries"; then
        was=$(field "$cur" ssid)
        if [ "$released" = 1 ]; then
            renew_dhcp || true
            i=0; while [ "$i" -lt "$back_tries" ]; do online "$old" && break; sleep "$step"; i=$((i + 1)); done
        fi
        firewall || true
    fi
    status
    printf '%s failed %s %s %s\n' "$id" "$ssid" "$failed" "$was" | put result
    echo "wifi: switch failed ($failed); previous network restored where possible"
}

join() {
    printf '%s switching %s\n' "$id" "$ssid" | put result
    cur=$(wpa status); old=$(field "$cur" id); released=0; new_id=; enabled=; max_priority=0
    if [ "$(field "$cur" wpa_state)" != COMPLETED ] || ! numeric "$old"; then
        printf '%s failed %s noassoc\n' "$id" "$ssid" | put result
        echo 'wifi: no current connection to restore; switch refused'
        return
    fi
    ids=$(wpa list_networks | awk -F '\t' 'NR>1 && $1 ~ /^[0-9]+$/ {print $1}')
    for net_id in $ids; do
        disabled=$(value get_network "$net_id" disabled)
        case "$disabled" in 1|2) :;; *) enabled="$enabled $net_id";; esac
        priority=$(value get_network "$net_id" priority)
        numeric "$priority" || priority=0
        if [ "$priority" -gt "$max_priority" ] && [ "$priority" -lt 1000000 ]; then max_priority=$priority; fi
    done
    new_id=$(value add_network)
    if ! numeric "$new_id"; then new_id=; why=bad; back; return; fi
    transaction=1
    if ok set_network "$new_id" ssid "$ssid" && ok set_network "$new_id" scan_ssid 1 &&
       ok set_network "$new_id" priority "$((max_priority + 1))" &&
       { if [ "$psk" = - ]; then ok set_network "$new_id" key_mgmt NONE;
         else ok set_network "$new_id" psk "$psk" && ok set_network "$new_id" key_mgmt WPA-PSK; fi; } &&
       ok select_network "$new_id"; then :; else why=bad; back; return; fi
    why=notfound
    if ! connected "$new_id" "$join_tries"; then back; return; fi
    # Assume release may have happened even if a control command fails.
    released=1
    if ! renew_dhcp; then why=noaddress; back; return; fi
    i=0; why=noaddress
    while [ "$i" -lt "$dhcp_tries" ]; do
        if online "$new_id"; then break; fi
        [ -z "${ipaddr:-}" ] || why=nogateway
        sleep "$step"; i=$((i + 1))
    done
    if [ "$i" -ge "$dhcp_tries" ]; then back; return; fi
    restore_enabled
    saved=1; ok save_config || saved=0
    transaction=0
    status
    printf '%s ok %s %s %s\n' "$id" "$ssid" "$ipaddr" "$saved" | put result
    echo "wifi: connected; settings saved=$saved"
}

interrupted() {
    trap - HUP INT TERM
    if [ "$transaction" = 1 ]; then why=interrupted; back; fi
    rmdir "$out/lock" 2>/dev/null || true
    exit 0
}
trap interrupted HUP INT TERM

take() {
    [ -f "$request" ] || return 0
    mkdir "$out/lock" 2>/dev/null || return 0
    pending=$out/.request.$$
    if ! mv "$request" "$pending" 2>/dev/null; then rmdir "$out/lock"; return; fi
    IFS=' ' read -r action id ssid psk extra < "$pending" || true
    rm -f "$pending"
    action=${action:-}; id=${id:-}; ssid=${ssid:-}; psk=${psk:-}; extra=${extra:-}
    if ! numeric "$id" || [ -n "$extra" ]; then echo 'wifi: invalid request'
    elif [ "$action" = scan ] && [ -z "$ssid$psk" ]; then scan
    elif [ "$action" = join ] && hex "$ssid" 64 && [ -n "$ssid" ] &&
         { [ "$psk" = - ] || { [ ${#psk} -eq 64 ] && hex "$psk" 64; }; }; then join
    else echo 'wifi: invalid request'; fi
    psk=; ssid=; action=
    rmdir "$out/lock" 2>/dev/null || true
}

case "${1:-run}" in
    once) firewall || true; status; take;;
    run)
        # The old process cannot survive a reboot. A stale in-flight result is
        # shown as interrupted; a queued scan is safe to perform.
        rmdir "$out/lock" 2>/dev/null || true
        if [ -f "$request" ]; then
            IFS=' ' read -r stale_action stale_id stale_ssid stale_psk stale_extra < "$request" || true
            if [ "${stale_action:-}" = join ]; then
                rm -f "$request"
                if numeric "${stale_id:-}" && hex "${stale_ssid:-}" 64; then
                    printf '%s failed %s interrupted\n' "$stale_id" "$stale_ssid" | put result
                fi
            fi
            stale_psk=; stale_ssid=
        fi
        while :; do firewall || true; status; take; sleep 2; done;;
    *) exit 2;;
esac
