#!/system/bin/sh
# Start the satellite in the foreground.  Extra arguments go to hassmic (e.g. -w remote, -E, -V).
D=/data/local/hassmic
sh $D/alexa-off.sh >/dev/null
mkdir -p /data/misc/avahi/services && cp $D/hassmic.service /data/misc/avahi/services/ && start avahi-daemon
# AIPC refuses uid 0 ("Root user is not allowed to use AIPC"), so run as the stock Alexa client's user.
exec $D/runas puffin aipc,audio,system,inet,shell,dbus,ace_group,ace_kvstore,input $D/hassmic -n "Echo Dot" "$@"
