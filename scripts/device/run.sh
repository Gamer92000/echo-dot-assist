#!/system/bin/sh
# Start the satellite in the foreground.  Extra arguments go to hassmic (e.g. -P wyoming, -w remote, -E, -V).
D=/data/local/hassmic
. $D/device.conf || exit 1
# A model whose Android has no toybox (rook, Android 5.1) ships ours (tools/toybox.sh): the applets the system lacks go on
# PATH, after the system's own, so nothing that exists changes
if [ -x $D/toybox ]; then
    TB=/data/local/hassmic/tb; mkdir -p $TB
    for a in $($D/toybox); do [ -e /system/bin/$a ] || [ -e /system/xbin/$a ] || ln -sf $D/toybox $TB/$a; done
    PATH=$PATH:$TB
fi
sh $D/alexa-off.sh >/dev/null
mkdir -p $D/state && chown $DAEMON_USER $D/state    # settings changed from Home Assistant (runs as DAEMON_USER)
[ -n "$UX_SERVICE" ] && stop $UX_SERVICE
mkdir -p /data/misc/avahi/services && $D/hassmic "$@" -S > /data/misc/avahi/services/hassmic.service
# init's avahi may not read that directory (SELinux); run our own from this shell's permissive domain
stop avahi-daemon; pkill avahi-daemon; sleep 1; avahi-daemon --no-drop-root --no-proc-title > /dev/null 2>&1 &
# AIPC refuses uid 0 ("Root user is not allowed to use AIPC"), so run as the stock Alexa client's user.
exec $D/runas -r 3990 -Z u:r:hassmic:s0 $DAEMON_USER $DAEMON_GROUPS $D/hassmic "$@"
