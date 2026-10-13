#!/system/bin/sh
# The updatable part of the boot logic; started by /system/hassmic/boot.sh (root, su domain), which picks the factory copy
# or a signed update and exports HASSMIC_DIR (where this script and its neighbours are) and HASSMIC_SYS.
#   main.sh firewall    egress lock + re-assert loop, and the root side of push updates; started at "on boot"
#   main.sh satellite   Alexa off, then keep hassmic running, and check that the firewall service keeps its rules right
# Model: device.conf next to this script (devices/<codename>/device.conf: service names, the daemon's user).
# Config: /data/local/hassmic/hassmic.conf (shell syntax).  No config = do nothing = stock behaviour.
#   (NAME= of older installs is ignored: hassmic names itself after the model and its MAC address)
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
# Every line of the log starts with when it was written (src/hassmic/clock.c says why): UTC once the clock was set from
# Home Assistant since boot (property hassmic.clock.synced, set below), else seconds since boot.  hassmic stamps its own
# lines; say for single lines from here, stamped for what a long-running piece writes.  Not for what goes on a pipe
# hassmic reads (factory(), results).
stamp() { if [ "$(getprop hassmic.clock.synced)" = 1 ]; then date -u '+%Y-%m-%d %H:%M:%SZ'; else read -r u _ < /proc/uptime; echo "boot+$u"; fi; }
say() { echo "$(stamp) $*"; }
stamped() { while IFS= read -r l; do echo "$(stamp) $l"; done; }
# Both of our services are oneshot, and init drops a "start" that comes while the service is still running: "stop" only
# asks init to kill it and returns, and when init gets to the start before it has reaped the old process, that start
# does nothing and the reaped oneshot stays stopped.  Lost so after a push update on an Echo Show 8 (2026-10-15: the
# satellite down for 3 min).  ctl.restart is init's own stop-then-start: it starts the service again once the old one
# is reaped, and returns at once (lockdown.sh restarts adbd so too).  Where the service must really be gone before
# anything else happens (the factory reset empties its state, the firewall rules are loaded from here), svc_stop waits
# until init says so ("stopping" until it reaped it), 20 s at most; false if it is still there then.
svc_restart() { setprop ctl.restart $1; }
svc_stop() {
    stop $1
    i=0
    while :; do
        case "$(getprop init.svc.$1)" in running|stopping) ;; *) return 0;; esac
        [ $i -lt 40 ] || return 1
        sleep 0.5; i=$((i + 1))
    done
}
[ -f $CONF ] || exit 0
# Without the model's facts the satellite cannot start, but the firewall needs none of them: it must go up regardless.
if [ ! -f $D/device.conf ]; then
    say "== $D/device.conf missing: no satellite" >> $LOG
    [ "$1" = firewall ] && sh $D/lockdown.sh watch 2>&1 | stamped >> $LOG
    exit 1
fi
. $D/device.conf
# Root runs what this file says, and ADB_WIFI in it opens a root shell: nobody else may write it.  "adb push" leaves it
# writable for everyone (seen on two of three Echos), and hassmic faces the network.
chown root:root $CONF; chmod 644 $CONF
. $CONF

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
    fwmiss=0
    if svc_stop hassmic_fw; then
        sh $D/lockdown.sh > /dev/null
        start hassmic_fw
    else
        # still there after 20 s: hung, so not building the chain beside us; the lock matters more than the risk of
        # two loads interleaving.  init starts a new watcher once the old one is gone.
        echo "== the firewall service did not stop within 20 s: rules loaded anyway, restarted when it has"
        sh $D/lockdown.sh > /dev/null
        svc_restart hassmic_fw
    fi
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

# The time from Home Assistant (src/hassmic/clock.c): hassmic, which may not set the clock, leaves "<epoch> <its
# CLOCK_BOOTTIME when that was the time>" in state/clock, the latter as /proc/uptime words it (that clock).  What has
# passed since is added, the clock set, and the RTC, so that the next boot starts close.  hassmic takes the time only from
# Home Assistant's keyed link; here only a time from 2026 on counts, and only a fresh one (a file from before a reboot
# speaks of another boot's clock).  mksh's numbers end at 2^31 on these Echos (2038 for the time, 248 days of uptime
# in centiseconds): so seconds and centiseconds apart, and nothing larger is formed.
CLOCK=/data/local/hassmic/state/clock
WIFI=/data/local/hassmic/wifi                   # scripts/device/wifi.sh's answers and lock
clock_set() {
    [ -f $CLOCK ] && [ ! -L $CLOCK ] || return 0
    local t= at= up now was d a first ds
    read -r t at < $CLOCK; rm -f $CLOCK
    read -r up _ < /proc/uptime
    case "$t" in ''|*[!0-9]*) t=x;; esac
    case "$at" in [0-9]*.[0-9][0-9]) case "${at%.*}${at#*.}" in *[!0-9]*) t=x;; esac;; *) t=x;; esac
    [ "$t" = x ] && { echo "clock: state/clock not understood, ignored"; return; }
    if [ ${#t} -ne 10 ] || [ $t -lt 1767225600 ]; then echo "clock: $t is not a time of ours, ignored"; return; fi
    ds=$(( ${up%.*} - ${at%.*} ))                   # whole seconds since hassmic had the time
    if [ ${#at} -gt 12 ] || [ $ds -lt 0 ] || [ $ds -gt 600 ]; then echo "clock: Home Assistant's time is stale, ignored"; return; fi
    now=$(( t + (ds * 100 + 1${up#*.} - 1${at#*.} + 50) / 100 )) was=$(date +%s)     # 1xx: centiseconds without octal
    if ! date -u @$now > /dev/null 2>&1 || [ $(( $(date +%s) - now )) -gt 2 ]; then echo "clock: could not be set"; return; fi
    hwclock -w -u 2>/dev/null
    first=; [ "$(getprop hassmic.clock.synced)" = 1 ] || first=1
    setprop hassmic.clock.synced 1
    d=$((now - was)); a=behind; [ $d -lt 0 ] && { d=$((-d)); a=ahead; }
    echo "clock: set from Home Assistant, $d s $a${first:+ (times in this log are UTC from here on, seconds since boot before)}"
}

# Home Assistant's time zone (src/hassmic/clock.c, tz.c): state/timezone holds a zone name of this Echo's time zone
# data, or a POSIX TZ string where none has Home Assistant's rule (bionic takes either).  The property is what native
# code reads; where Android's framework runs (Echo Show), its alarm service is told too (IAlarmManager.setTimeZone,
# transaction 3 on Android 7 to 9): it sets the same property and tells the Java side, which takes names only.  A rule
# (no zone of the Echo's data has Home Assistant's) is right for native code only: Java reads the property at boot and
# makes it GMT, so an Echo Show's screen shows GMT then.  The log stays in UTC.
# state/ is the daemon's: the file is moved out into root's directory first, so what is read is what was checked (no
# link, no FIFO that would hold this loop) and a newer one hassmic writes meanwhile is not deleted with it.  The binder
# calls are bounded: a stuck system_server must not stop the firewall check that runs after this.
TZFILE=/data/local/hassmic/state/timezone
TZTAKE=/data/local/hassmic/timezone.take
tz_set() {
    [ -e $TZFILE ] || [ -L $TZFILE ] || return 0
    rm -rf $TZTAKE; mv -f $TZFILE $TZTAKE 2>/dev/null || return 0      # rm -rf: a directory too, links not followed
    local z= t
    [ -f $TZTAKE ] && [ ! -L $TZTAKE ] && z=$(dd if=$TZTAKE bs=92 count=1 2>/dev/null)  # bounded: the daemon wrote it (Dot 3 toybox head has no -c)
    rm -rf $TZTAKE
    # a zone name (Europe/Berlin, Etc/GMT+3) or a POSIX rule (CET-1CEST,M3.5.0,M10.5.0/3, <+03>-3, IST-5:30), never
    # a path: bionic reads the property in every process (tzcode takes ":<file>")
    case "$z" in
    [A-Za-z]*|\<*) ;;
    *) z=;;
    esac
    case "$z" in ''|*..*|*//*|*/|*[!A-Za-z0-9\<\>+,./:_-]*) z=;; esac
    case "$z" in
    *,*) case "${z%%,*}" in */*) z=;; esac;;          # a rule: "/" only in its rules, after the first ","
    */*) case "$z" in *[:\<\>.,]*) z=;; esac;;        # a name: no ":" (UTC:/etc/x), "<", ">", "."
    esac
    [ -n "$z" ] || { echo "time zone: state/timezone not understood, ignored"; return; }
    [ ${#z} -le 91 ] || { echo "time zone: state/timezone too long, ignored"; return; }
    [ "$z" = "$(getprop persist.sys.timezone)" ] && return
    case "$z" in
    *,*|*\<*|*:*) ;;                                  # a rule ("M10.5.0/3" has a slash too): Java would make it GMT
    [A-Za-z]*/*)
        t=; command -v timeout > /dev/null && t="timeout 5"
        command -v service > /dev/null && $t service check alarm 2>/dev/null | grep -q ': found' &&
            $t service call alarm 3 s16 "$z" > /dev/null 2>&1;;
    esac
    # the alarm service set it already where it runs; everywhere else (and if that call failed) it is set here
    [ "$z" = "$(getprop persist.sys.timezone)" ] || setprop persist.sys.timezone "$z"
    echo "time zone: $z, from Home Assistant"
}

# mDNS through the stock avahi-daemon, from the service file hassmic prints (satellite start, below).  The init-started
# avahi runs in its own SELinux domain, which may not read /data/misc/avahi/services (avc denied), and magiskpolicy
# cannot parse a rule for a type with a hyphen ("avahi-daemon").  So it runs from here, in our domain.
# Host name = the ESPHome node name, as on a real ESPHome device.  Stock avahi calls every Echo "linux" (a second one
# "linux-2"), and Home Assistant showed that next to the name.  [server] is the stock file's first section, so host-name
# lands in it.
# Also a restart when hassmic asks (state/mdns, src/hassmic/netaddr.c: a new address, or Home Assistant missing while
# the Echo is set up).  The old daemon says goodbye to its records when it ends, and has to be gone before the new one
# starts: two at once see each other's host name and the new one renames itself.  The new one announces a few seconds
# after the goodbye, so Home Assistant sees the service go and come back and looks at where it is now.
AVAHI_SVC=/data/misc/avahi/services/hassmic.service
avahi_start() {
    local conf=/system/etc/avahi-daemon.conf node i=0
    node=$(sed -n 's|^ *<name>\([a-z0-9-]*\)</name>$|\1|p' $AVAHI_SVC 2>/dev/null)
    if [ -n "$node" ]; then
        { echo "[server]"; echo "host-name=$node"; grep -v '^\[server\]' $conf; } > /data/misc/avahi/avahi-daemon.conf
        conf=/data/misc/avahi/avahi-daemon.conf
    fi
    stop avahi-daemon; pkill avahi-daemon
    while [ $i -lt 5 ] && pidof avahi-daemon > /dev/null; do sleep 1; i=$((i + 1)); done
    sleep 2
    avahi-daemon -f $conf --no-drop-root > /dev/null 2>&1 &
}

# While the settings page switches networks (wifi.sh holds its lock), the link is that script's business: no wifisvc.
# Whenever the address changes, wifi.sh writes down which network the Echo is on, for the page: wpa_supplicant can also
# move to another saved network by itself when the one it is on goes away.
netwatch() {
    miss=0 addr=
    while :; do
        rotate_log
        clock_set
        tz_set
        fwcheck
        if [ -f /data/local/hassmic/state/mdns ] && [ ! -L /data/local/hassmic/state/mdns ]; then
            why=$(dd if=/data/local/hassmic/state/mdns bs=60 count=1 2>/dev/null | tr -cd 'a-zA-Z0-9 ')
            rm -f /data/local/hassmic/state/mdns
            [ -f $AVAHI_SVC ] && { echo "mDNS: announcing again (${why:-asked for}), avahi restarted"; avahi_start; }
        fi
        if a=$(ifconfig $WLAN 2>/dev/null | sed -n 's/.*inet addr:\([0-9.]*\).*/\1/p'); [ -n "$a" ]; then
            miss=0
            [ "$a" = "$addr" ] || { addr=$a; [ -f $D/wifi.sh ] && sh $D/wifi.sh status; }
            kmod
            [ -n "$WIFI_SERVICE" ] && [ "$(getprop init.svc.$WIFI_SERVICE)" = running ] && [ ! -d $WIFI/lock ] && { sleep 5; stop $WIFI_SERVICE; echo "netwatch: link up, $WIFI_SERVICE stopped"; }
        elif [ -d $WIFI/lock ]; then
            miss=0 addr=
        else
            miss=$((miss + 1)) addr=
            # No saved network (a reset, a new install): nothing for wifisvc to bring up; the phone's setup (Improv) does
            # (checkers has no WIFI_SERVICE of its own: Android's WifiService brings its networks up)
            [ $miss -ge 6 ] && [ -n "$WIFI_SERVICE" ] && [ "$(getprop init.svc.$WIFI_SERVICE)" != running ] && [ "$(sh $D/wifi.sh saved 2>/dev/null)" != 0 ] &&
                { start $WIFI_SERVICE; echo "netwatch: no address for 60 s, $WIFI_SERVICE started"; miss=0; }
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
rejected() { say "== update rejected: $1"; rm -rf $new; echo "FAILED $1" > $IN/result.tmp; }
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
    elif out=$(sh $cur/sysinstall.sh factory $cur 2>&1); then echo "$out" | stamped >&2; echo "OK $want is now the factory copy"
    else echo "$out" | stamped >&2; echo "FAILED $(echo "$out" | tail -1)"
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
                say "== factory copy: $(factory "$(cat $cur/VERSION)")"       # its stderr: the log
            fi
        fi
        # artifacts another Echo's page copied here (src/hassmic/artifacts.c): into the models folders, which are root's
        if [ -f /data/local/hassmic/state/artifacts/request ]; then
            if out=$(DAEMON_USER=$DAEMON_USER READ_AS="$D/runas $DAEMON_USER $DAEMON_GROUPS" \
                     sh $D/artifact-install.sh /data/local/hassmic/state /data/local/hassmic 2>&1); then
                say "== artifacts installed, restarting hassmic: $(echo $out)"
                svc_restart hassmic
            else say "== artifacts not installed: $(echo $out)"
            fi
        fi
        # Designs for the light ring applied on the settings page (src/hassmic/ringdesign.c): the set made and installed
        # again.  ledcontroller then starts with nothing on: hassmic sets what showed again once it reads the answer,
        # given after ledcontroller loaded the set (~1.3 s on a Dot 2)
        # The request is taken into root's own folder first (a rename: a link moves as a link) and only looked at and
        # read there, where the daemon's user cannot swap it for a link or a FIFO between the look and the read
        rm -f $LEDNEW.req
        if mv /data/local/hassmic/state/leds-request $LEDNEW.req 2>/dev/null && [ -f $LEDNEW.req ] && [ ! -L $LEDNEW.req ]; then
            id=$(dd if=$LEDNEW.req bs=16 count=1 2>/dev/null | tr -cd 0-9)
            rm -f $LEDNEW.req
            leds > $LEDNEW.out 2>&1; stamped < $LEDNEW.out; rm -f $LEDNEW.out
            if [ $LEDS = ok ]; then sleep 2; sh $D/alexa-off.sh > /dev/null 2>&1; fi
            echo "$id $LEDS $LEDWHY" > /data/local/hassmic/leds.result.new && mv /data/local/hassmic/leds.result.new /data/local/hassmic/leds.result
            chmod 644 /data/local/hassmic/leds.result
            say "== light ring designs: $LEDS${LEDWHY:+ ($LEDWHY)}"
        fi
        rm -f $LEDNEW.req
        # Wi-Fi from the settings page (src/hassmic/wifi.c): a scan, or a switch to another network, which takes up to two
        # minutes; so in the background, one at a time (wifi.sh's lock), and a request waits while one runs
        if [ -f /data/local/hassmic/state/wifi-request ] && [ ! -d $WIFI/lock ] && [ -f $D/wifi.sh ]; then
            sh $D/wifi.sh take 2>&1 | stamped &           # takes the lock first thing, long before the next look
        fi
        # Factory reset (main.c core_reset: the action button held 10 s, Home Assistant's button, the settings page):
        # everything of hassmic's goes (state/: settings, names, Home Assistant's key, approved browsers, Bluetooth
        # bonds, Sendspin, microWakeWord models) and every saved Wi-Fi network; the installed system, Amazon's models in
        # models/ aed/ whisper/ and hassmic.conf stay.  The satellite starts again from scratch: the setup (OOBE).
        if [ -f /data/local/hassmic/state/reset ] && [ ! -L /data/local/hassmic/state/reset ]; then
            why=$(dd if=/data/local/hassmic/state/reset bs=40 count=1 2>/dev/null | tr -cd 'a-zA-Z0-9 ')
            # Emptying state/ under a running hassmic would leave it writing into the empty one: if it does not go, the
            # request stays for the next look, and the rest of this loop (a reboot, an update) still gets its turn.
            if ! svc_stop hassmic; then
                [ -n "$resetwait" ] || say "== factory reset (${why:-asked for}): hassmic did not stop within 20 s, trying again"
                resetwait=1
            else
                resetwait=
                say "== factory reset (${why:-asked for}): forgetting settings and Wi-Fi networks"
                while [ -d $WIFI/lock ] && [ -n "$(cat $WIFI/lock/pid 2>/dev/null)" ] && kill -0 "$(cat $WIFI/lock/pid)" 2>/dev/null; do sleep 1; done   # a switch running ends first
                [ -f $D/wifi.sh ] && sh $D/wifi.sh forget 2>&1 | stamped
                st=/data/local/hassmic/state
                for f in $st/* $st/.[!.]*; do [ -e "$f" ] || [ -L "$f" ] && rm -rf "$f"; done
                rm -f /data/misc/avahi/services/hassmic.service
                start hassmic
                say "== factory reset done, the satellite starts again"
                continue
            fi
        else resetwait=
        fi
        # A reboot (main.c core_reboot: the settings page, Home Assistant's restart button): what a power cycle does, for
        # an Echo that is stuck where nothing less helps (issue #17).  The request goes first, and if it cannot go there
        # is no reboot: one left behind would reboot every boot (the firewall start drops one from before it, too).
        if [ -f /data/local/hassmic/state/reboot ] && [ ! -L /data/local/hassmic/state/reboot ]; then
            why=$(dd if=/data/local/hassmic/state/reboot bs=40 count=1 2>/dev/null | tr -cd 'a-zA-Z0-9 ')
            rm -f /data/local/hassmic/state/reboot
            if [ -e /data/local/hassmic/state/reboot ]; then say "== reboot asked for, but its request cannot be removed: not rebooting"
            else say "== reboot (${why:-asked for})"; sync; setprop sys.powerctl reboot; sleep 30; fi
        fi
        # asked for on the settings page: a rename (main.c core_rename; the name is in what the satellite builds at start,
        # mDNS and avahi's host name included) or the wake word moving to or from Home Assistant (core_wake_engine)
        if [ -f /data/local/hassmic/state/restart ]; then
            why=$(dd if=/data/local/hassmic/state/restart bs=40 count=1 2>/dev/null | tr -cd 'a-z ')
            rm -f /data/local/hassmic/state/restart
            say "== ${why:-restart} asked for on the settings page, restarting hassmic"
            svc_restart hassmic
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
            say "== update $ver installed (signed with ${key##*/}), restarting"
            echo "OK $ver" > $IN/result.tmp
        fi
        rm -f $IN/bundle $IN/bundle.sig
        chown $DAEMON_USER $IN/result.tmp; mv $IN/result.tmp $IN/result
        if grep -q "^OK" $IN/result; then
            sleep 2                                     # let hassmic relay the result to the pusher
            svc_restart hassmic
            exec sh $SYS/boot.sh firewall               # and run the new firewall script as well
        fi
    done
}

# The light ring's own animations (src/hassmic/ring.c): Home Assistant's light, and the listening ring turned towards
# the talker beside a ringing timer.  ledcontroller reads animations only at its start and only from the system
# partition, so the hassmic about to run makes its set from stock's files (-G) and, when that is not the installed
# set, sysinstall.sh writes it and ledcontroller starts again (about a second without the ring, before hassmic runs).
# The set includes the designs the settings page applied (state/ring-applied, src/hassmic/ringdesign.c), and ota_watch
# runs this again when it applies new ones.  One at a time (a satellite start and a request could meet).  LEDS says
# what came of it: ok (installed, ledcontroller restarted), same (nothing to do) or failed (LEDWHY: why).
# Each LED's own layer (the fill, issue #57) takes 9.7 MB of the partition: without room for it, the set without them
# (HASSMIC_RING_LEDS=0), which counts as installed from then on; Home Assistant then has the light but no fill.
# A binary in /data wins over the installed one: lets a new build be tried without a trip through TWRP.
BIN=$D/hassmic; [ -x /data/local/hassmic/hassmic ] && BIN=/data/local/hassmic/hassmic
LEDNEW=/data/local/hassmic/leds.new
LEDLOCK=/data/local/hassmic/leds.lock
leds() {
    LEDS=failed LEDWHY=
    [ -d /system/etc/led-resources ] || { LEDWHY="no light ring on this model"; return 0; }    # no ledcontroller (checkers: a screen)
    i=0
    boot=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null)
    while ! mkdir $LEDLOCK 2>/dev/null; do
        # one left by an install that was killed half way (the satellite restarted): its shell is gone, or it was of
        # an earlier boot, whose pid may be taken again by now.  No pid yet: one being taken this moment, so that only
        # counts after two seconds
        p= b=; read -r p b 2>/dev/null < $LEDLOCK/pid
        if { [ -z "$p" ] && [ $i -ge 2 ]; } || { [ -n "$p" ] && { [ "$b" != "$boot" ] || ! kill -0 "$p" 2>/dev/null; }; }; then
            rm -rf $LEDLOCK; continue
        fi
        i=$((i + 1)); [ $i -lt 60 ] || { LEDWHY="another install of the ring's set still runs"; echo "light ring: $LEDWHY"; return 0; }
        sleep 1
    done
    echo "$$ $boot" > $LEDLOCK/pid
    have=$(cat /system/etc/hassmic-leds.digest 2>/dev/null)
    want=$($BIN -G - 2>/dev/null) || want=
    [ -n "$want" ] && [ "$want" != "$have" ] && [ -n "$have" ] && [ "$(HASSMIC_RING_LEDS=0 $BIN -G - 2>/dev/null)" = "$have" ] && want=$have
    if [ -n "$want" ] && [ "$want" = "$have" ]; then LEDS=same; rm -rf $LEDLOCK; return 0; fi  # 0.1 s, not 0.7
    for own in 1 0; do
        rm -rf $LEDNEW; mkdir -p $LEDNEW
        if ! out=$(HASSMIC_RING_LEDS=$own $BIN -G $LEDNEW 2>&1); then LEDWHY="no animations of ours"; echo "light ring: no animations of ours ($out)"; break; fi
        small=; [ $own = 1 ] || small=", without the LEDs' own"
        echo "light ring: installing ours (${out#ring: })$small"
        if out=$(sh $D/sysinstall.sh leds $D $LEDNEW 2>&1); then echo "$out"; stop ledcontroller; start ledcontroller; LEDS=ok LEDWHY=; break; fi
        echo "$out"; LEDWHY=$(echo "$out" | head -n 1); LEDWHY=${LEDWHY#sysinstall leds: }
        case "$out" in *"has room for"*) ;; *) break;; esac     # no room: again without the LEDs' own
    done
    rm -rf $LEDNEW $LEDLOCK
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
    # hassmic is off on purpose: that must not count as an update that failed to come up.  Alexa's apps that
    # alexa-off.sh disabled (checkers) come back, all but the updater.
    [ "$1" = firewall ] || { echo 0 > $OTA/tries; [ -s /data/local/hassmic/alexa-disabled ] && sh $D/alexa-on.sh keep-updates 2>&1 | stamped >> $LOG; exit 0; }
    { rotate_log; echo "== stock-online, uptime $(cut -d. -f1 /proc/uptime)s: Alexa runs, updaters cut off"; } 2>&1 | stamped >> $LOG
    sh $D/lockdown.sh ota-only watch 2>&1 | stamped >> $LOG
    exit 0
fi

case "$1" in
firewall)
    quiet
    [ -f $D/wifi.sh ] && sh $D/wifi.sh clean           # a Wi-Fi request from before this start is nobody's
    rm -f /data/local/hassmic/state/reboot             # nor a reboot: it would reboot every boot
    sh $D/lockdown.sh watch 2>&1 | stamped >> $LOG &
    ota_watch >> $LOG 2>&1                              # execs on an update: no pipe around it, its lines say when
    ;;
satellite)
    # The installer that unpacked us may have been an older one running with umask 077: make sure the daemon's user gets in.
    [ "$D" != "$SYS" ] && chmod 755 $D
    {
        rotate_log
        echo "== satellite start, uptime $(cut -d. -f1 /proc/uptime)s, $(cat $D/VERSION 2>/dev/null || echo factory) from $D"
        sh $D/lockdown.sh services        # stops the cloud daemons that were not up yet at "on boot"; the firewall is the watcher's
        # wake word sets under a short name (echo-de) from installs by hand: the name scripts/artifacts.sh gives them
        [ -f $D/artifact-install.sh ] && sh $D/artifact-install.sh migrate /data/local/hassmic/state /data/local/hassmic
        leds                              # before alexa-off.sh: a ledcontroller that started again may want Alexa back
        sh $D/alexa-off.sh; quiet
        # Android's own stream volumes at full (wifictl.dex through IAudioService): hassmic's gain per player is the
        # volume (audio_android.c), the framework's may not take more off.  It is up: these models start the
        # satellite at sys.boot_completed.
        [ "$INSTALL" = boot ] && [ -f $D/wifictl.dex ] && CLASSPATH=$D/wifictl.dex app_process / Wifictl volume-max
    } 2>&1 | stamped >> $LOG
    # hassmic offers Wi-Fi motion where the module can be loaded (wifimotion.c), the loop below loads it when needed
    [ -n "$KMOD" ] && [ -f $D/$KMOD ] && export HASSMIC_WIFI_KMOD=/proc/${KMOD%.ko}
    # online updates: hassmic checks downloads against the key root will check them against (ota.c)
    export HASSMIC_RELEASE_PUB=$(release_pub)
    netwatch 2>&1 | stamped >> $LOG &
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
        $BIN -P ${PROTO:-esphome} $ARGS -S > $AVAHI_SVC
        chown $DAEMON_USER $AVAHI_SVC; chmod 644 $AVAHI_SVC
        avahi_start
    ) 2>&1 | stamped >> $LOG &
    # The bootstrap counted this start as an attempt; a minute of hassmic running counts as success.
    (sleep 60; pidof hassmic > /dev/null && echo 0 > $OTA/tries) &
    fast=0
    while :; do
        t0=$(cut -d. -f1 /proc/uptime)
        # AIPC refuses uid 0, so run as the stock Alexa client's user (device.conf).  Real group 3990, which no stock process
        # has: hassmic creates its outgoing sockets under it and lockdown.sh lets that reach any address, so replies and music
        # play from wherever Home Assistant points.  Not the effective group: the mixer only records for group aipc.
        $D/runas -r 3990 $DAEMON_USER $DAEMON_GROUPS \
            $BIN -P ${PROTO:-esphome} $ARGS >> $LOG 2>&1
        say "hassmic exited rc=$?, restart in 3 s" >> $LOG
        # An update whose daemon does not stay up is worse than no update: with hassmic down there is no push port either.
        # Five exits within 20 s each -> back to the factory copy right now, without waiting for three reboots.
        if [ $(( $(cut -d. -f1 /proc/uptime) - t0 )) -lt 20 ]; then fast=$((fast + 1)); else fast=0; fi
        if [ $fast -ge 5 ] && [ "$D" != "$SYS" ]; then
            say "== update $(cat $D/VERSION 2>/dev/null) keeps exiting: running the factory copy" >> $LOG
            echo 3 > $OTA/tries
            exec sh $SYS/boot.sh satellite
        fi
        # ALEXA_PROP may have been set again meanwhile (ledcontroller restart)
        sh $D/alexa-off.sh > /dev/null 2>&1; quiet
        sleep 3
    done
    ;;
esac
