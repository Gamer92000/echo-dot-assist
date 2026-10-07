#!/system/bin/sh
# Revert alexa-off.sh without rebooting.
. "${0%/*}/device.conf" || exit 1
pkill hassmic; pkill mixcap; pkill mixplay
rm -f /data/misc/avahi/services/hassmic.service
command -v ledctrl > /dev/null && ledctrl -c
for s in $UPDATE_SERVICES $BT_SERVICE; do start $s; done
[ -n "$ALEXA_PROP" ] && setprop $ALEXA_PROP 1     # init trigger starts the Alexa services ($ALEXA_SERVICES)
# Alexa's apps that alexa-off.sh disabled (checkers).  "keep-updates": all but the updater (MODE=stock-online)
DISABLED=/data/local/hassmic/alexa-disabled
if [ -f $DISABLED ]; then
    left=
    while read -r p; do
        case " $UPDATE_PACKAGES " in *" $p "*) [ "$1" = keep-updates ] && { left="$left $p"; continue; };; esac
        pm enable "$p" < /dev/null > /dev/null 2>&1 || { echo "alexa-on: $p could not be enabled"; left="$left $p"; }
    done < $DISABLED
    if [ -n "$left" ]; then printf '%s\n' $left > $DISABLED; else rm -f $DISABLED; fi
fi
