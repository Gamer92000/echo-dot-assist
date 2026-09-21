#!/system/bin/sh
# Boot entry point, run by init (hassmic.rc) as root in the su domain.
#   boot.sh firewall    egress lock + re-assert loop; started at "on boot", before Wi-Fi associates
#   boot.sh satellite   Alexa off, then keep hassmic running
# Config: /data/local/hassmic/hassmic.conf (shell syntax).  No config = do nothing = stock behaviour.
#   LAN=192.168.100.0/22        required
#   NAME="Echo Dot"             optional
#   PROTO=esphome               optional: esphome (default, port 26053) or wyoming (port 16700)
#   ARGS=""                     optional extra hassmic arguments
D=/system/hassmic
CONF=/data/local/hassmic/hassmic.conf
LOG=/data/local/hassmic/boot.log
[ -f $CONF ] || exit 0
. $CONF
[ -n "$LAN" ] || { echo "boot.sh: LAN missing in $CONF" >> $LOG; exit 1; }

# uxeventd plays the "ready for setup" voice prompts and the orange setup spinner on an unregistered device.  hassmic drives
# LEDs (ledctrl) and earcons itself, so it goes.  Stopped before "class_start main" it never starts (SVC_DISABLED).
quiet() { stop uxeventd; for s in oobed_on_boot oobed_on_press oobed_on_no_nw; do stop $s; done; }

case "$1" in
firewall)
    quiet
    exec sh $D/lockdown.sh "$LAN" watch >> $LOG 2>&1
    ;;
satellite)
    {
        echo "== satellite start, uptime $(cut -d. -f1 /proc/uptime)s"
        sh $D/lockdown.sh "$LAN"  > /dev/null     # stops the cloud daemons that were not up yet at "on boot"
        sh $D/alexa-off.sh; quiet
    } >> $LOG 2>&1
    # A binary in /data wins over the installed one: lets a new build be tried without a trip through TWRP.
    BIN=$D/hassmic; [ -x /data/local/hassmic/hassmic ] && BIN=/data/local/hassmic/hassmic
    mkdir -p /data/local/hassmic/state && chown puffin /data/local/hassmic/state    # settings changed from Home Assistant (runs as puffin)
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
