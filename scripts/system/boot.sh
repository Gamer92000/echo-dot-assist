#!/system/bin/sh
# Boot entry point, run by init (hassmic.rc) as root in the su domain.
#   boot.sh firewall    egress lock + re-assert loop; started at "on boot", before Wi-Fi associates
#   boot.sh satellite   Alexa off, then keep hassmic running
# Config: /data/local/hassmic/hassmic.conf (shell syntax).  No config = do nothing = stock behaviour.
#   LAN=192.168.100.0/22        required
#   NAME="Echo Dot"             optional
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
        mkdir -p /data/misc/avahi/services && cp $D/hassmic.service /data/misc/avahi/services/
    } >> $LOG 2>&1
    # A binary in /data wins over the installed one: lets a new build be tried without a trip through TWRP.
    BIN=$D/hassmic; [ -x /data/local/hassmic/hassmic ] && BIN=/data/local/hassmic/hassmic
    while :; do
        # AIPC refuses uid 0, so run as the stock Alexa client's user.
        $D/runas puffin aipc,audio,system,inet,shell,dbus,ace_group,ace_kvstore,input \
            $BIN -n "${NAME:-Echo Dot}" $ARGS >> $LOG 2>&1
        echo "hassmic exited rc=$?, restart in 3 s" >> $LOG
        # PUFFIN_START may have been set again meanwhile (ledcontroller restart)
        sh $D/alexa-off.sh > /dev/null 2>&1; quiet
        sleep 3
    done
    ;;
esac
