#!/system/bin/sh
# Join Wi-Fi without the Alexa app, through the stock wpa_supplicant.  Run lockdown.sh first.
#   wifi-join.sh "<ssid>" "<passphrase>"
# Profile is saved to /data/misc/wifi/wpa_supplicant.conf (update_config=1), so it survives reboots.
# UNTESTED on device.  If wpa_cli cannot connect, wpa_supplicant is not running yet: `start wpa_supplicant`.
#   wifi-join.sh <file>                   line 1 = SSID, line 2 = passphrase (keeps the secret out of shell history;
#                                         scripts/wifi-join.sh on the PC pushes secrets/wifi.conf and deletes it after)
if [ -f "$1" ]; then { IFS= read -r SSID; IFS= read -r PSK; } < "$1"
else SSID=${1:?usage: wifi-join.sh <ssid> <passphrase> | <file>}; PSK=${2:?passphrase missing}; fi
W="wpa_cli -i wlan0 -p /data/misc/wifi/sockets"
iptables -S OUTPUT | grep -q hassmic_out || { echo "refusing: run lockdown.sh first"; exit 1; }
$W status >/dev/null 2>&1 || { start wpa_supplicant; sleep 3; }
N=$($W add_network | tail -1)
$W set_network $N ssid $(printf %s "$SSID" | od -An -tx1 | tr -d " \n")      # hex form: apostrophes and spaces are safe
$W set_network $N psk "\"$PSK\""
$W set_network $N key_mgmt WPA-PSK
$W enable_network $N
$W select_network $N
$W save_config
sleep 8
$W status | grep -E "^(wpa_state|ssid|ip_address)"
# DHCP is normally started by Amazon's netmgrd once the link is up; fall back to the init service.
ifconfig wlan0 | grep -q "inet addr" || { start dhcpcd-wlan0; sleep 5; ifconfig wlan0 | grep "inet addr"; }
