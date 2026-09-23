#!/system/bin/sh
# The updatable part of the boot logic; started by /system/hassmic/boot.sh (root, su domain), which picks the factory copy
# or a signed update and exports HASSMIC_DIR (where this script and its neighbours are) and HASSMIC_SYS.
#   main.sh firewall    egress lock + re-assert loop, and the root side of push updates; started at "on boot"
#   main.sh satellite   Alexa off, then keep hassmic running
# Config: /data/local/hassmic/hassmic.conf (shell syntax).  No config = do nothing = stock behaviour.
#   NAME="Echo Dot"             optional
#   PROTO=esphome               optional: esphome (default, port 26053) or wyoming (port 16700)
#   ARGS=""                     optional extra hassmic arguments
#   MODE=stock-online           optional: stock Alexa with internet, e.g. to let it fetch a wake-word model.  hassmic stays
#                               off, nothing is stopped or blocked except firmware updates (lockdown.sh ota-only)
umask 022                                   # init gives us 077; what we create must be readable by the daemon's user
D=${HASSMIC_DIR:-/system/hassmic}
SYS=${HASSMIC_SYS:-/system/hassmic}
OTA=/data/local/hassmic/ota
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

# Root side of a push update.  hassmic (user puffin) received a bundle, checked its signature and left it in state/ota/
# with a "request" file.  Here the bundle is verified again by the tool and key from the read-only system partition - that
# check is the one that counts - unpacked into a fresh root-owned directory and made current.  This runs in the firewall
# service so that it can restart the satellite service.
ota_watch() {
    IN=/data/local/hassmic/state/ota
    while sleep 2; do
        [ -f $IN/request ] || continue
        rm -f $IN/request $IN/result
        new=$OTA/v$(cut -d. -f1 /proc/uptime)-$$
        if [ ! -f $SYS/update.pub ]; then echo "FAILED no update key on this device (install-system.sh puts it there)" > $IN/result.tmp
        elif ver=$($SYS/otatool install $SYS/update.pub $IN/bundle $IN/bundle.sig $new 2>&1) && chmod 755 $new && [ -f $new/main.sh ] &&
             $new/runas puffin shell $new/hassmic -T > /dev/null 2>&1; then     # the daemon's user can really run it
            old=$(readlink $OTA/current)
            ln -sfn $new $OTA/current                   # toybox: replaces the link itself (checked on the device); no mv -T there
            echo 0 > $OTA/tries
            for d in $OTA/v*; do [ "$d" = "$new" ] || [ "$d" = "$old" ] || rm -rf "$d"; done        # keep the previous one
            echo "== update $ver installed, restarting"
            echo "OK $ver" > $IN/result.tmp
        else
            # otatool prints the version on success, so a version here means: unpacked fine, but it is not a runnable payload
            case "$ver" in *" "*|"") why=$(echo "$ver" | tail -1);; *) why="version $ver does not run as the daemon's user (self-check failed), not installed";; esac
            echo "== update rejected: $why"; rm -rf $new
            echo "FAILED $why" > $IN/result.tmp
        fi
        rm -f $IN/bundle $IN/bundle.sig
        chown puffin $IN/result.tmp; mv $IN/result.tmp $IN/result
        if grep -q "^OK" $IN/result; then
            sleep 2                                     # let hassmic relay the result to the pusher
            stop hassmic; start hassmic
            exec sh $SYS/boot.sh firewall               # and run the new firewall script as well
        fi
    done
}

# A push update runs "firewall" again (exec, same PID) while the previous firewall watcher is still looping: stop it first,
# or every update adds one and old and new rules take turns.  Matched by command line, which also catches the ones that
# earlier versions left behind.
if [ "$1" = firewall ]; then
    for p in /proc/[0-9]*; do
        case "$(tr '\0' ' ' 2>/dev/null < $p/cmdline)" in *lockdown.sh*watch*) kill ${p#/proc/} 2>/dev/null;; esac
    done
fi

if [ "$MODE" = stock-online ]; then
    # hassmic is off on purpose: that must not count as an update that failed to come up.
    [ "$1" = firewall ] || { echo 0 > $OTA/tries; exit 0; }
    { rotate_log; echo "== stock-online, uptime $(cut -d. -f1 /proc/uptime)s: Alexa runs, updaters cut off"; } >> $LOG 2>&1
    exec sh $D/lockdown.sh ota-only watch >> $LOG 2>&1
fi

case "$1" in
firewall)
    quiet
    sh $D/lockdown.sh watch >> $LOG 2>&1 &
    ota_watch >> $LOG 2>&1
    ;;
satellite)
    # The installer that unpacked us may have been an older one running with umask 077: make sure the daemon's user gets in.
    [ "$D" != "$SYS" ] && chmod 755 $D
    {
        rotate_log
        echo "== satellite start, uptime $(cut -d. -f1 /proc/uptime)s, $(cat $D/VERSION 2>/dev/null || echo factory) from $D"
        sh $D/lockdown.sh services        # stops the cloud daemons that were not up yet at "on boot"; the firewall is the watcher's
        sh $D/alexa-off.sh; quiet
    } >> $LOG 2>&1
    # A binary in /data wins over the installed one: lets a new build be tried without a trip through TWRP.
    BIN=$D/hassmic; [ -x /data/local/hassmic/hassmic ] && BIN=/data/local/hassmic/hassmic
    netwatch >> $LOG 2>&1 &
    # mDNS through the stock avahi-daemon: hassmic prints the service file for its protocol, name and MAC address.
    mkdir -p /data/misc/avahi/services
    $BIN -P ${PROTO:-esphome} -n "${NAME:-Echo Dot}" $ARGS -S > /data/misc/avahi/services/hassmic.service 2>> $LOG
    chmod 644 /data/misc/avahi/services/hassmic.service
    # The init-started avahi runs in its own SELinux domain, which may not read /data/misc/avahi/services (avc denied), and
    # magiskpolicy cannot parse a rule for a type with a hyphen ("avahi-daemon").  So run it from here, in our domain.
    stop avahi-daemon; pkill avahi-daemon; sleep 1
    avahi-daemon --no-drop-root > /dev/null 2>&1 &
    # The bootstrap counted this start as an attempt; a minute of hassmic running counts as success.
    (sleep 60; pidof hassmic > /dev/null && echo 0 > $OTA/tries) &
    fast=0
    while :; do
        t0=$(cut -d. -f1 /proc/uptime)
        # AIPC refuses uid 0, so run as the stock Alexa client's user.  Real group 3990, which no stock process has: hassmic
        # creates its outgoing sockets under it and lockdown.sh lets that reach any address, so replies and music play from
        # wherever Home Assistant points.  Not the effective group: the mixer only records for group aipc.  net_bt_stack:
        # /dev/stpbt, the Bluetooth controller.
        $D/runas -r 3990 puffin aipc,audio,system,inet,shell,dbus,ace_group,ace_kvstore,input,net_bt_stack \
            $BIN -P ${PROTO:-esphome} -n "${NAME:-Echo Dot}" $ARGS >> $LOG 2>&1
        echo "hassmic exited rc=$?, restart in 3 s" >> $LOG
        # An update whose daemon does not stay up is worse than no update: with hassmic down there is no push port either.
        # Five exits within 20 s each -> back to the factory copy right now, without waiting for three reboots.
        if [ $(( $(cut -d. -f1 /proc/uptime) - t0 )) -lt 20 ]; then fast=$((fast + 1)); else fast=0; fi
        if [ $fast -ge 5 ] && [ "$D" != "$SYS" ]; then
            echo "== update $(cat $D/VERSION 2>/dev/null) keeps exiting: running the factory copy" >> $LOG
            echo 3 > $OTA/tries
            exec sh $SYS/boot.sh satellite
        fi
        # PUFFIN_START may have been set again meanwhile (ledcontroller restart)
        sh $D/alexa-off.sh > /dev/null 2>&1; quiet
        sleep 3
    done
    ;;
esac
