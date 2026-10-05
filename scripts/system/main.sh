#!/system/bin/sh
# The updatable part of the boot logic; started by /system/hassmic/boot.sh (root, su domain), which picks the factory copy
# or a signed update and exports HASSMIC_DIR (where this script and its neighbours are) and HASSMIC_SYS.
#   main.sh firewall    egress lock + re-assert loop, and the root side of push updates; started at "on boot"
#   main.sh satellite   Alexa off, then keep hassmic running, and check that the firewall service keeps its rules right
# Model: device.conf next to this script (devices/<codename>/device.conf: service names, the daemon's user).
# Config: /data/local/hassmic/hassmic.conf (shell syntax).  No config = do nothing = stock behaviour.
#   NAME="Echo Dot"             optional, default DEFAULT_NAME from device.conf
#   PROTO=esphome               optional: esphome (default, port 26053) or wyoming (port 16700)
#   ARGS=""                     optional extra hassmic arguments
#   MODE=stock-online           optional: stock Alexa with internet, e.g. to let it fetch a wake-word model.  hassmic stays
#                               off, nothing is stopped or blocked except firmware updates (lockdown.sh ota-only)
#   ADB_WIFI=1                  optional: leave adb over Wi-Fi open (root shell for the whole network, no password; read
#                               by lockdown.sh).  Without it: closed, opened for 30 min from the settings page (action button)
umask 022                                   # init gives us 077; what we create must be readable by the daemon's user
D=${HASSMIC_DIR:-/system/hassmic}
SYS=${HASSMIC_SYS:-/system/hassmic}
OTA=/data/local/hassmic/ota
CONF=/data/local/hassmic/hassmic.conf
LOG=/data/local/hassmic/boot.log
[ -f $CONF ] || exit 0
# Without the model's facts the satellite cannot start, but the firewall needs none of them: it must go up regardless.
if [ ! -f $D/device.conf ]; then
    echo "== $D/device.conf missing: no satellite" >> $LOG
    [ "$1" = firewall ] && exec sh $D/lockdown.sh watch >> $LOG 2>&1
    exit 1
fi
. $D/device.conf
# Root runs what this file says, and ADB_WIFI in it opens a root shell: nobody else may write it.  "adb push" leaves it
# writable for everyone (seen on two of three Echos), and hassmic faces the network.
chown root:root $CONF; chmod 644 $CONF
. $CONF
NAME=${NAME:-$DEFAULT_NAME}

# uxeventd plays the "ready for setup" voice prompts and the orange setup spinner on an unregistered device.  hassmic drives
# LEDs (ledctrl) and earcons itself, so it goes.  Stopped before "class_start main" it never starts (SVC_DISABLED).
quiet() { for s in $UX_SERVICE $SETUP_SERVICES; do stop $s; done; }

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

# The firewall rules belong to the firewall service (hassmic_fw: "lockdown.sh watch" loads them and checks them every
# 5 s).  That service can fail without anyone noticing: on the Echo 2 it hung at its start and there was no lock for the
# whole boot (2026-09-30, see the scan below).  So this service looks too, every 10 s, with the same check ("lockdown.sh
# check": every rule of the chain, the chain first in OUTPUT, INPUT policy DROP, each of stock's rules the satellite
# needs, IPv6 off where it cannot be filtered): wrong twice in a row means the watcher is not doing its job.  Then say so, stop that service (two runs building the chain at once interleave their
# rules), load the rules from here, and start the service again.
fwmiss=0
fwcheck() {
    if fwwrong=$(sh $D/lockdown.sh check); then fwmiss=0; return; fi
    fwmiss=$((fwmiss + 1))
    [ $fwmiss -ge 2 ] || return
    echo "== firewall rules wrong on two looks 10 s apart ($fwwrong; uptime $(cut -d. -f1 /proc/uptime)s): the firewall service is not doing its job; rules loaded from here, service restarted"
    stop hassmic_fw
    sh $D/lockdown.sh > /dev/null
    start hassmic_fw
    fwmiss=0
}

# Wi-Fi motion (device.conf KMOD): our kernel module hooks the Wi-Fi driver's receive path for the level of every frame
# from the access point (src/kmod/).  Loaded only once Wi-Fi motion is switched on (wifi_motion=on in hassmic's
# state/config; field 13 of the state/settings of older versions; hassmic waits for /proc/<module> meanwhile), so while it is off - the default - no kernel code is touched; and
# only with the link up, so that a driver that is a module itself (donut's) is there to be hooked.  It cannot be
# unloaded: it stays until the next reboot.  A failed load is not tried again until then.
kmod() {
    [ -n "$KMOD" ] && [ -f $D/$KMOD ] || return 0
    grep -q "^${KMOD%.ko} " /proc/modules && return 0
    if [ -f /data/local/hassmic/state/config ]; then
        grep -qx 'wifi_motion=on' /data/local/hassmic/state/config || return 0
    else
        set -- $(cat /data/local/hassmic/state/settings 2>/dev/null)
        [ "${13}" = 1 ] || return 0
    fi
    if out=$(insmod $D/$KMOD $KMOD_ARGS 2>&1); then echo "kmod: $KMOD loaded (Wi-Fi motion switched on)"
    else echo "kmod: $KMOD not loaded, not tried again until reboot: $out"; KMOD=; fi
}

netwatch() {
    miss=0
    while :; do
        rotate_log
        fwcheck
        if ifconfig $WLAN 2>/dev/null | grep -q "inet addr"; then
            miss=0
            kmod
            [ "$(getprop init.svc.$WIFI_SERVICE)" = running ] && { sleep 5; stop $WIFI_SERVICE; echo "netwatch: link up, $WIFI_SERVICE stopped"; }
        else
            miss=$((miss + 1))
            [ $miss -ge 6 ] && [ "$(getprop init.svc.$WIFI_SERVICE)" != running ] && { start $WIFI_SERVICE; echo "netwatch: no address for 60 s, $WIFI_SERVICE started"; miss=0; }
        fi
        sleep 10
    done
}

# Root side of a push update.  hassmic (DAEMON_USER) received a bundle, checked its signature and left it in state/ota/
# with a "request" file.  Here the bundle is verified again by the tool and key from the read-only system partition - that
# check is the one that counts - unpacked into a fresh root-owned directory and made current, if it was built for this
# model (its device.conf names the product this Echo reports).  This runs in the firewall service so that it can restart
# the satellite service.
# Two keys count: the owner's (update.pub, written by install-system.sh: push updates) and the project's release key
# (keys/release.pub, in every build: the online updates hassmic downloads once the owner switches them on in Home
# Assistant, update.c).  The release key of the copy that runs wins over the factory copy's: both are root's, written
# from bundles that verified, and so a new release key can come with an update signed by the old one.
release_pub() { for k in $D/release.pub $SYS/release.pub; do [ -s $k ] && { echo $k; return; }; done; }
rejected() { echo "== update rejected: $1"; rm -rf $new; echo "FAILED $1" > $IN/result.tmp; }
# The installed update passed its self test (hassmic left state/ota/healthy: started, wake word engine loaded, a second
# of microphone audio; main.c): it becomes the factory copy on the system partition, bootstrap included, so the Echo
# falls back to the last version that worked.  Only if it is the update installed now and the hassmic running is its
# binary: not after a fall back to the factory copy, not with a test binary from deploy.sh in /data.
factory() {
    want=$1 cur=$(readlink $OTA/current)
    have=$(cat $cur/VERSION 2>/dev/null)
    running=
    for p in $(pidof hassmic); do [ "$(readlink /proc/$p/exe)" = "$cur/hassmic" ] && running=1; done
    if [ -z "$cur" ] || [ "$have" != "$want" ]; then echo "FAILED $want is not the installed update (that is ${have:-none}); nothing written"
    elif [ -z "$running" ]; then echo "FAILED $want is installed but not what runs now (fell back to the factory copy?); nothing written"
    elif cmp -s $cur/VERSION $SYS/VERSION; then echo "OK $want is the factory copy already"
    elif out=$(sh $cur/sysinstall.sh factory $cur 2>&1); then echo "$out" >&2; echo "OK $want is now the factory copy"
    else echo "$out" >&2; echo "FAILED $(echo "$out" | tail -1)"
    fi
}
ota_watch() {
    IN=/data/local/hassmic/state/ota
    while sleep 2; do
        if [ -f $IN/healthy ]; then
            rm -f $IN/healthy
            cur=$(readlink $OTA/current)
            # every start says so; only an update that is not the factory copy yet has anything to do
            if [ -n "$cur" ] && [ -f $cur/VERSION ] && ! cmp -s $cur/VERSION $SYS/VERSION; then
                echo "== factory copy: $(factory "$(cat $cur/VERSION)")"      # its stderr: the log
            fi
        fi
        [ -f $IN/request ] || continue
        rm -f $IN/request $IN/result
        new=$OTA/v$(cut -d. -f1 /proc/uptime)-$$
        key=
        for k in $SYS/update.pub $(release_pub); do
            [ -s $k ] && $SYS/otatool verify $k $IN/bundle $IN/bundle.sig > /dev/null 2>&1 && { key=$k; break; }
        done
        if [ ! -f $SYS/update.pub ]; then echo "FAILED no update key on this device (install-system.sh puts it there)" > $IN/result.tmp
        elif [ -z "$key" ]; then rejected "the signature verifies against neither the update key nor the release key"
        elif ! ver=$($SYS/otatool install $key $IN/bundle $IN/bundle.sig $new 2>&1); then
            rejected "$(echo "$ver" | tail -1)"
        elif prod=$(. $new/device.conf 2>/dev/null && echo "$PRODUCT"); [ "$prod" != "$(getprop ro.product.device)" ]; then
            rejected "version $ver is built for ${prod:-an unknown model}, this Echo is $(getprop ro.product.device); not installed"
        elif ! { chmod 755 $new && [ -f $new/main.sh ] && $new/runas $DAEMON_USER shell $new/hassmic -T > /dev/null 2>&1; }; then  # the daemon's user can really run it
            rejected "version $ver does not run as the daemon's user (self-check failed), not installed"
        elif [ -f $new/otatool ] && ! $new/otatool verify $key $IN/bundle $IN/bundle.sig > /dev/null 2>&1; then
            # Once it passes its self test, its otatool becomes the one on the system partition that checks every later
            # update: so it must pass that check itself.
            rejected "version $ver brings an otatool that does not verify it; not installed"
        else
            old=$(readlink $OTA/current)
            ln -sfn $new $OTA/current                   # toybox: replaces the link itself (checked on the device); no mv -T there
            echo 0 > $OTA/tries
            for d in $OTA/v*; do [ "$d" = "$new" ] || [ "$d" = "$old" ] || rm -rf "$d"; done        # keep the previous one
            echo "== update $ver installed (signed with ${key##*/}), restarting"
            echo "OK $ver" > $IN/result.tmp
        fi
        rm -f $IN/bundle $IN/bundle.sig
        chown $DAEMON_USER $IN/result.tmp; mv $IN/result.tmp $IN/result
        if grep -q "^OK" $IN/result; then
            sleep 2                                     # let hassmic relay the result to the pusher
            stop hassmic; start hassmic
            exec sh $SYS/boot.sh firewall               # and run the new firewall script as well
        fi
    done
}

# A push update runs "firewall" again (exec, same PID) while the previous firewall watcher is still looping: stop it first,
# or every update adds one and old and new rules take turns.  Matched by command line, which also catches the ones that
# earlier versions left behind.  Read with the shell itself: a process that exits between the open and the read leaves
# the Echo 2's tr (Fire OS 6572) spinning on the read error for ever, and this script never got to the firewall watcher
# and the installer below (seen 2026-09-30 right after a boot: no egress lock, push updates unanswered).
if [ "$1" = firewall ]; then
    for p in /proc/[0-9]*; do
        c=; while IFS= read -r -d '' a; do c="$c $a"; done 2>/dev/null < $p/cmdline
        case "$c" in *lockdown.sh*watch*) kill ${p#/proc/} 2>/dev/null;; esac
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
    # hassmic offers Wi-Fi motion where the module can be loaded (wifimotion.c), the loop below loads it when needed
    [ -n "$KMOD" ] && [ -f $D/$KMOD ] && export HASSMIC_WIFI_KMOD=/proc/${KMOD%.ko}
    # online updates: hassmic checks downloads against the key root will check them against (ota.c)
    export HASSMIC_RELEASE_PUB=$(release_pub)
    netwatch >> $LOG 2>&1 &
    # mDNS through the stock avahi-daemon: hassmic prints the service file for its protocol, name and MAC address.
    # The MAC in it is how Home Assistant tells devices apart.  On radar wlan0 appears only later in the boot, hassmic
    # printed its placeholder MAC, and Home Assistant offered the adopted Echo as a new device.  So wait for Wi-Fi
    # (there is no mDNS without it anyway); hassmic keeps going meanwhile.  The directory belongs to the daemon's user so
    # hassmic can rewrite the file itself when Home Assistant sets or clears the encryption key.
    mkdir -p /data/misc/avahi/services
    chown $DAEMON_USER /data/misc/avahi/services
    (
        i=0
        while [ $i -lt 120 ] && ! grep -q '[1-9a-f]' /sys/class/net/$WLAN/address 2>/dev/null; do sleep 1; i=$((i + 1)); done
        [ $i -gt 0 ] && echo "mDNS: waited ${i}s for the $WLAN address"
        $BIN -P ${PROTO:-esphome} -n "$NAME" $ARGS -S > /data/misc/avahi/services/hassmic.service
        chown $DAEMON_USER /data/misc/avahi/services/hassmic.service; chmod 644 /data/misc/avahi/services/hassmic.service
        # The init-started avahi runs in its own SELinux domain, which may not read /data/misc/avahi/services (avc denied),
        # and magiskpolicy cannot parse a rule for a type with a hyphen ("avahi-daemon").  So run it from here, in our domain.
        # Host name = the ESPHome node name, as on a real ESPHome device.  Stock avahi calls every Echo "linux" (a second
        # one "linux-2"), and Home Assistant showed that next to the name.  [server] is the stock file's first section, so
        # host-name lands in it.
        conf=/system/etc/avahi-daemon.conf
        node=$(sed -n 's|^ *<name>\([a-z0-9-]*\)</name>$|\1|p' /data/misc/avahi/services/hassmic.service)
        if [ -n "$node" ]; then
            { echo "[server]"; echo "host-name=$node"; grep -v '^\[server\]' $conf; } > /data/misc/avahi/avahi-daemon.conf
            conf=/data/misc/avahi/avahi-daemon.conf
        fi
        stop avahi-daemon; pkill avahi-daemon; sleep 1
        avahi-daemon -f $conf --no-drop-root > /dev/null 2>&1 &
    ) >> $LOG 2>&1 &
    # The bootstrap counted this start as an attempt; a minute of hassmic running counts as success.
    (sleep 60; pidof hassmic > /dev/null && echo 0 > $OTA/tries) &
    fast=0
    while :; do
        t0=$(cut -d. -f1 /proc/uptime)
        # AIPC refuses uid 0, so run as the stock Alexa client's user (device.conf).  Real group 3990, which no stock process
        # has: hassmic creates its outgoing sockets under it and lockdown.sh lets that reach any address, so replies and music
        # play from wherever Home Assistant points.  Not the effective group: the mixer only records for group aipc.
        $D/runas -r 3990 $DAEMON_USER $DAEMON_GROUPS \
            $BIN -P ${PROTO:-esphome} -n "$NAME" $ARGS >> $LOG 2>&1
        echo "hassmic exited rc=$?, restart in 3 s" >> $LOG
        # An update whose daemon does not stay up is worse than no update: with hassmic down there is no push port either.
        # Five exits within 20 s each -> back to the factory copy right now, without waiting for three reboots.
        if [ $(( $(cut -d. -f1 /proc/uptime) - t0 )) -lt 20 ]; then fast=$((fast + 1)); else fast=0; fi
        if [ $fast -ge 5 ] && [ "$D" != "$SYS" ]; then
            echo "== update $(cat $D/VERSION 2>/dev/null) keeps exiting: running the factory copy" >> $LOG
            echo 3 > $OTA/tries
            exec sh $SYS/boot.sh satellite
        fi
        # ALEXA_PROP may have been set again meanwhile (ledcontroller restart)
        sh $D/alexa-off.sh > /dev/null 2>&1; quiet
        sleep 3
    done
    ;;
esac
