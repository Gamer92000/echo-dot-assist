#!/system/bin/sh
# Boot entry point, run by init (hassmic.rc) as root in the su domain.
#   boot.sh firewall    egress lock + re-assert loop; started at "on boot", before Wi-Fi associates
#   boot.sh satellite   Alexa off, then keep hassmic running
# Config: /data/local/hassmic/hassmic.conf (shell syntax).  No config = do nothing = stock behaviour.
#   NAME="Echo Dot"             optional
#   PROTO=esphome               optional: esphome (default, port 26053) or wyoming (port 16700)
#   ARGS=""                     optional extra hassmic arguments
D=/system/hassmic
CONF=/data/local/hassmic/hassmic.conf
LOG=/data/local/hassmic/boot.log
[ -f $CONF ] || exit 0
. $CONF

# uxeventd plays the "ready for setup" voice prompts and the orange setup spinner on an unregistered device.  hassmic drives
# LEDs (ledctrl) and earcons itself, so it goes.  Stopped before "class_start main" it never starts (SVC_DISABLED).
quiet() { stop uxeventd; for s in oobed_on_boot oobed_on_press oobed_on_no_nw; do stop $s; done; }

# Amazon's wifisvc runs HTTP connectivity tests against AWS hosts.  Behind the egress lock they always fail, and it then
# tears the Wi-Fi link down and rebuilds it (seen: ~100 s after boot, link gone for 193 s, and again later).  It is only needed
# to bring the link up: wpa_supplicant (saved profile, reconnects by itself) and dhcpcd keep it up.  So stop it once there is
# an address, and let it run again only if the address stays away for a minute.
# boot.log is appended to by several long-lived processes (hassmic, the firewall watcher, this loop), all through ">>",
# i.e. O_APPEND.  So it is rotated by copy + truncate: their next write simply lands at the new end.  One old copy is kept.
LOG_MAX=1048576
rotate_log() {
    [ "$(wc -c < $LOG 2>/dev/null || echo 0)" -gt $LOG_MAX ] || return 0
    cp $LOG $LOG.1 && : > $LOG && echo "== log rotated, previous part in $LOG.1"
}

netwatch() {
    miss=0
    while :; do
        rotate_log
        if ifconfig wlan0 2>/dev/null | grep -q "inet addr"; then
            miss=0
            [ "$(getprop init.svc.wifisvc)" = running ] && { sleep 5; stop wifisvc; echo "netwatch: link up, wifisvc stopped"; }
        else
            miss=$((miss + 1))
            [ $miss -ge 6 ] && [ "$(getprop init.svc.wifisvc)" != running ] && { start wifisvc; echo "netwatch: no address for 60 s, wifisvc started"; miss=0; }
        fi
        sleep 10
    done
}

case "$1" in
firewall)
    quiet
    exec sh $D/lockdown.sh watch >> $LOG 2>&1
    ;;
satellite)
    {
        rotate_log
        echo "== satellite start, uptime $(cut -d. -f1 /proc/uptime)s"
        sh $D/lockdown.sh > /dev/null     # stops the cloud daemons that were not up yet at "on boot"
        sh $D/alexa-off.sh; quiet
    } >> $LOG 2>&1
    # A binary in /data wins over the installed one: lets a new build be tried without a trip through TWRP.
    BIN=$D/hassmic; [ -x /data/local/hassmic/hassmic ] && BIN=/data/local/hassmic/hassmic
    mkdir -p /data/local/hassmic/state && chown puffin /data/local/hassmic/state    # settings changed from Home Assistant (runs as puffin)
    netwatch >> $LOG 2>&1 &
    # mDNS through the stock avahi-daemon: hassmic prints the service file for its protocol, name and MAC address.
    mkdir -p /data/misc/avahi/services
    $BIN -P ${PROTO:-esphome} -n "${NAME:-Echo Dot}" $ARGS -S > /data/misc/avahi/services/hassmic.service 2>> $LOG
    chmod 644 /data/misc/avahi/services/hassmic.service
    # The init-started avahi runs in its own SELinux domain, which may not read /data/misc/avahi/services (avc denied), and
    # magiskpolicy cannot parse a rule for a type with a hyphen ("avahi-daemon").  So run it from here, in our domain.
    stop avahi-daemon; pkill avahi-daemon; sleep 1
    avahi-daemon --no-drop-root > /dev/null 2>&1 &
    while :; do
        # AIPC refuses uid 0, so run as the stock Alexa client's user.
        $D/runas puffin aipc,audio,system,inet,shell,dbus,ace_group,ace_kvstore,input \
            $BIN -P ${PROTO:-esphome} -n "${NAME:-Echo Dot}" $ARGS >> $LOG 2>&1
        echo "hassmic exited rc=$?, restart in 3 s" >> $LOG
        # PUFFIN_START may have been set again meanwhile (ledcontroller restart)
        sh $D/alexa-off.sh > /dev/null 2>&1; quiet
        sleep 3
    done
    ;;
esac
