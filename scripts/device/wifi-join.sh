#!/system/bin/sh
# Join Wi-Fi without the Alexa app, through the stock wpa_supplicant.  Run lockdown.sh first.
#   wifi-join.sh "<ssid>" "<passphrase>"
# Profile is saved to /data/misc/wifi/wpa_supplicant.conf (update_config=1), so it survives reboots.
#   wifi-join.sh <file>                   line 1 = SSID, line 2 = passphrase (keeps the secret out of shell history;
#                                         scripts/wifi-join.sh on the PC pushes secrets/wifi.conf and deletes it after)
if [ -f "$1" ]; then { IFS= read -r SSID; IFS= read -r PSK; } < "$1"
else SSID=${1:?usage: wifi-join.sh <ssid> <passphrase> | <file>}; PSK=${2:?passphrase missing}; fi
W="wpa_cli -i wlan0 -p /data/misc/wifi/sockets"
iptables -S OUTPUT | grep -q hassmic_out || { echo "refusing: run lockdown.sh first"; exit 1; }
$W status >/dev/null 2>&1 || { start wpa_supplicant; sleep 3; }
N=$($W add_network | tail -1)
$W set_network $N ssid $(printf %s "$SSID" | od -An -tx1 | tr -d " \n")      # hex form: apostrophes and spaces are safe
# Probe for the name too: hidden networks do not advertise their SSID in scan results.
$W set_network $N scan_ssid 1
# FAIL for a passphrase wpa_supplicant does not take (WPA2: 8 to 63 characters)
$W set_network $N psk "\"$PSK\"" | grep -q OK || { echo "the password is not one WPA2 takes (8 to 63 characters)"; $W remove_network $N >/dev/null; exit 1; }
$W set_network $N key_mgmt WPA-PSK
$W enable_network $N
$W select_network $N
$W save_config
# connected, or why not: the script used to end the same either way, and an Echo with a mistyped name sat in SCANNING
# while setup went on (forum, 2026-10-10)
i=0; while [ $i -lt 20 ]; do $W status | grep -q "^wpa_state=COMPLETED" && break; sleep 1; i=$((i + 1)); done
$W status | grep -E "^(wpa_state|ssid|ip_address)"
S=$($W status | grep "^wpa_state=" | cut -d= -f2)
if [ "$S" != COMPLETED ]; then
    case $S in
    *HANDSHAKE*) echo "not connected ($S): the password is probably wrong";;
    *) echo "not connected ($S): no network named \"$SSID\" in range (names are case-sensitive), or the password is wrong";;
    esac
    # select_network disabled every other network: enable them again, or a profile that worked stays off after a reboot
    $W remove_network $N >/dev/null; $W enable_network all >/dev/null; $W save_config >/dev/null
    exit 1
fi
# DHCP is normally started by Amazon's netmgrd once the link is up; fall back to the init service.
ifconfig wlan0 | grep -q "inet addr" || { start dhcpcd-wlan0; sleep 5; ifconfig wlan0 | grep "inet addr"; }
