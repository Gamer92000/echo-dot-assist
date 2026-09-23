#!/system/bin/sh
# Revert alexa-off.sh without rebooting.
pkill hassmic; pkill mixcap; pkill mixplay
rm -f /data/misc/avahi/services/hassmic.service
ledctrl -c
for s in otad ace_otad btmanagerd; do start $s; done
setprop com.amazon.puffin.PUFFIN_START 1      # init trigger starts puffin, puffinmrmd, dacd, smarthomed
