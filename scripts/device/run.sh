#!/system/bin/sh
# Start the satellite in the foreground.  Extra arguments go to hassmic (e.g. -P wyoming, -w remote, -E, -V).
D=/data/local/hassmic
sh $D/alexa-off.sh >/dev/null
mkdir -p /data/local/hassmic/state && chown puffin /data/local/hassmic/state    # settings changed from Home Assistant (runs as puffin)
stop uxeventd
mkdir -p /data/misc/avahi/services && $D/hassmic -n "Echo Dot" "$@" -S > /data/misc/avahi/services/hassmic.service
# init's avahi may not read that directory (SELinux); run our own from this shell's permissive domain
stop avahi-daemon; pkill avahi-daemon; sleep 1; avahi-daemon --no-drop-root > /dev/null 2>&1 &
# AIPC refuses uid 0 ("Root user is not allowed to use AIPC"), so run as the stock Alexa client's user.
exec $D/runas -r 3990 puffin aipc,audio,system,inet,shell,dbus,ace_group,ace_kvstore,input,net_bt_stack $D/hassmic -n "Echo Dot" "$@"
