#!/bin/sh
# LX05 web Wi-Fi bridge. Keep the original credentials until the candidate is
# connected to the same protected LAN and its original gateway responds.
umask 077
base=/data/lx05-assist
state=$base/state
out=$base/wifi
conf=/data/lx05/wpa.conf
sock=/var/run/wpa_supplicant
printf '%s\n' "${LX05_LAN_PREFIX:-}" | grep -Eq '^([0-9]{1,3}\.){3}$' || exit 1
mkdir -p "$out"
chmod 700 "$out"
watcher=$(cat "$out/watcher.pid" 2>/dev/null)
case "$watcher" in ''|*[!0-9]*) :;; *)
    if [ "$watcher" != "$$" ] && kill -0 "$watcher" 2>/dev/null &&
       grep -q 'wifi-service.sh' "/proc/$watcher/cmdline" 2>/dev/null; then exit 0; fi;;
esac
echo "$$" > "$out/watcher.pid"
rmdir "$out/lock" 2>/dev/null || :
W() { wpa_cli -i wlan0 -p "$sock" "$@" 2>/dev/null; }
last() { W "$@" | tail -n 1; }
ok() { [ "$(last "$@")" = OK ]; }
put() { cat > "$out/$1.tmp" && mv -f "$out/$1.tmp" "$out/$1"; }
status() { W status | grep -E '^(wpa_state|id|ssid|ip_address|freq)=' | put status; }
field() { printf '%s\n' "$1" | sed -n "s/^$2=//p" | head -n 1; }
hex() { case "$1" in ''|*[!0-9a-f]*) return 1;; esac; [ "${#1}" -le "$2" ] && [ $(( ${#1} % 2 )) = 0 ]; }
guard() {
    ip rule show | grep -q '100:.*lookup 123' &&
    ip route show table 123 | grep -q '^unreachable default' &&
    ! ip route get 1.1.1.1 >/dev/null 2>&1
}
scan() {
    W scan >/dev/null
    sleep 5
    { echo "id $id"; W scan_results | grep '^[0-9a-f][0-9a-f]:'; } | put scan
    status
}
rollback() {
    [ -n "$old" ] && W select_network "$old" >/dev/null
    [ -n "$new" ] && W remove_network "$new" >/dev/null
    /etc/init.d/dhcpc restart >/dev/null 2>&1
    n=0
    while [ "$n" -lt 35 ]; do
        cur=$(W status)
        [ "$(field "$cur" wpa_state)" = COMPLETED ] && [ "$(field "$cur" id)" = "$old" ] && break
        n=$((n+1)); sleep 1
    done
    status
    printf '%s failed %s %s\n' "$id" "$ssid" "$why" | put result
    [ -n "${rescue:-}" ] && rm -f "$rescue"
}
join() {
    printf '%s switching %s\n' "$id" "$ssid" | put result
    cur=$(W status)
    old=$(field "$cur" id)
    oldgw=$(ip route show default | awk '/^default via / {print $3; exit}')
    new= why=bad
    case "$old" in ''|*[!0-9]*) old=;; esac
    case "$oldgw" in "$LX05_LAN_PREFIX"*) :;; *) oldgw=;; esac
    if [ -z "$old" ] || [ -z "$oldgw" ] || ! guard; then
        printf '%s failed %s bad\n' "$id" "$ssid" | put result; return
    fi
    # If this helper dies halfway through, restart the Wi-Fi service using
    # the last saved temporary configuration. A reboot also uses the last
    # committed configuration from /data/lx05/wpa.conf.
    rescue="/tmp/lx05-wifi-rescue-$id"
    touch "$rescue"
    ( sleep 120; if [ -f "$rescue" ]; then /etc/init.d/wireless restart >/dev/null 2>&1; rm -f "$rescue"; fi ) </dev/null >/dev/null 2>&1 &
    new=$(last add_network)
    case "$new" in ''|*[!0-9]*) new=; rollback; return;; esac
    if ! ok set_network "$new" ssid "$ssid" || ! ok set_network "$new" scan_ssid 1; then rollback; return; fi
    if [ "$psk" = - ]; then
        ok set_network "$new" key_mgmt NONE || { rollback; return; }
    else
        ok set_network "$new" psk "$psk" && ok set_network "$new" key_mgmt WPA-PSK || { rollback; return; }
    fi
    ifconfig wlan0 0.0.0.0
    if ! ok select_network "$new"; then rollback; return; fi
    why=notfound; n=0
    while [ "$n" -lt 30 ]; do
        cur=$(W status)
        [ "$(field "$cur" wpa_state)" = COMPLETED ] && [ "$(field "$cur" id)" = "$new" ] && break
        n=$((n+1)); sleep 1
    done
    if [ "$n" -ge 30 ]; then rollback; return; fi
    /etc/init.d/dhcpc restart >/dev/null 2>&1
    why=noaddress; n=0; ip=
    while [ "$n" -lt 30 ]; do
        cur=$(W status); ip=$(field "$cur" ip_address)
        case "$ip" in "$LX05_LAN_PREFIX"*) break;; esac
        n=$((n+1)); sleep 1
    done
    case "$ip" in "$LX05_LAN_PREFIX"*) :;; *) rollback; return;; esac
    # The app, multicast and isolation are configured for this LAN. Even a
    # same-numbered subnet must reach the previous gateway before committing.
    why=nogateway
    if ! guard || ! ping -c 1 -W 2 "$oldgw" >/dev/null 2>&1; then rollback; return; fi
    tmp="$conf.new"
    {
        printf 'ctrl_interface=/var/run/wpa_supplicant\nap_scan=1\nnetwork={\nssid=%s\n' "$ssid"
        if [ "$psk" = - ]; then printf 'key_mgmt=NONE\n'; else printf 'psk=%s\nkey_mgmt=WPA-PSK\n' "$psk"; fi
        printf 'scan_ssid=1\n}\n'
    } > "$tmp" || { rm -f "$tmp"; why=bad; rollback; return; }
    chmod 600 "$tmp"
    if ! cp "$conf" /tmp/lx05-wpa-backup.conf; then rm -f "$tmp"; why=bad; rollback; return; fi
    if ! cp "$tmp" /tmp/lx05-wpa.conf || ! mv -f "$tmp" "$conf"; then
        cp /tmp/lx05-wpa-backup.conf /tmp/lx05-wpa.conf
        rm -f "$tmp" /tmp/lx05-wpa-backup.conf; why=bad; rollback; return
    fi
    rm -f /tmp/lx05-wpa-backup.conf
    W remove_network "$old" >/dev/null
    sync; status
    printf '%s ok %s %s 1\n' "$id" "$ssid" "$ip" | put result
    rm -f "$rescue"
}
rm -f "$state/wifi-request"
status
tick=0
while :; do
    if [ -f "$state/wifi-request" ] && [ ! -L "$state/wifi-request" ]; then
        if mkdir "$out/lock" 2>/dev/null; then
            request="$out/request"
            if mv -f "$state/wifi-request" "$request" && [ -f "$request" ] && [ ! -L "$request" ]; then
                read -r what id ssid psk < "$request"
                rm -f "$request"
                case "$id" in ''|*[!0-9]*) :;; *)
                    case "$what" in
                        scan) scan;;
                        join) if hex "$ssid" 64 && { [ "$psk" = - ] || { hex "$psk" 64 && [ "${#psk}" = 64 ]; }; }; then join; fi;;
                    esac;;
                esac
            fi
            rm -f "$request"
            rmdir "$out/lock" 2>/dev/null
            tick=0
        fi
    fi
    tick=$((tick+1)); if [ "$tick" -ge 10 ]; then status; tick=0; fi
    sleep 1
done
