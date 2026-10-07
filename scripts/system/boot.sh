#!/system/bin/sh
# Bootstrap, run by init (hassmic.rc) as root in the su domain.  Lives on the read-only system partition and never changes
# after the one-time install; everything that can change is in main.sh and its neighbours, which exist twice:
#   /system/hassmic/                 factory copy (what scripts/install-system.sh put there)
#   /data/local/hassmic/ota/current  installed by a signed push update (scripts/ota-push.sh), root-owned
# The update is used when it is there, unless it failed to come up three times in a row: then the factory copy runs again.
# Config: /data/local/hassmic/hassmic.conf.  No config = do nothing = stock behaviour.
umask 022
SYS=/system/hassmic
BASE=/data/local/hassmic
OTA=$BASE/ota
if [ ! -f $BASE/hassmic.conf ]; then
    # Alexa's apps, where alexa-off.sh had to disable them (checkers), stay disabled through reboots: stock behaviour
    # means turning them back on.  The Dots never leave that list.
    [ "$1" = satellite ] && [ -s $BASE/alexa-disabled ] && [ -f $SYS/alexa-on.sh ] && sh $SYS/alexa-on.sh > /dev/null 2>&1
    exit 0
fi
[ -f $SYS/device.conf ] && . $SYS/device.conf   # this model's facts, installed together with this file; main.sh copes without

# Only root may put things where root executes them from; the daemon (DAEMON_USER) owns state/ alone.
chown root:root $BASE; chmod 755 $BASE
mkdir -p $OTA $BASE/state; chown root:root $OTA; chmod 755 $OTA; chown ${DAEMON_USER:-root} $BASE/state; chmod 700 $BASE/state

D=$SYS
if [ -f $OTA/current/main.sh ]; then
    tries=$(cat $OTA/tries 2>/dev/null); tries=${tries:-0}
    if [ "$tries" -ge 3 ]; then
        # when, as main.sh stamps its lines: UTC once the clock was set from Home Assistant, else seconds since boot
        if [ "$(getprop hassmic.clock.synced)" = 1 ]; then at=$(date -u '+%Y-%m-%d %H:%M:%SZ'); else read -r at _ < /proc/uptime; at=boot+$at; fi
        [ "$1" = satellite ] && echo "$at == update $(cat $OTA/current/VERSION 2>/dev/null) failed to start $tries times: running the factory copy" >> $BASE/boot.log
    else
        D=$OTA/current
        [ "$1" = satellite ] && echo $((tries + 1)) > $OTA/tries       # main.sh resets it once hassmic has run for a minute
    fi
fi
export HASSMIC_DIR=$D HASSMIC_SYS=$SYS
exec sh $D/main.sh "$@"
