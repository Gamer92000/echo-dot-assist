#!/system/bin/sh
# Run the stock DAVS downloader (assetmgrd) with libcurlspy.so preloaded, to learn the real request and download URLs of
# cloud artifacts such as wake-word models.  Needs MODE=stock-online and a registered device.  From the PC:
#   make build/libcurlspy.so && adb push build/libcurlspy.so build/runas scripts/device/davs-spy.sh /data/local/tmp/
#   adb shell sh /data/local/tmp/davs-spy.sh        then change the wake word in the Alexa app
#   adb pull /data/davs/curlspy.log device-logs/    (holds the bearer token: device-logs/ is git-ignored)
# Same user, groups, environment and SELinux domain as the init service.  The domain matters: started from the su domain
# the daemon comes up, but no client can reach its AIPC service, init keeps starting a second copy that cannot register
# either, and the Alexa app shows the device as unavailable.  So: the shim carries a label that domain may load, the log is
# in the daemon's own directory, and runcon makes the transition (allowed because su is permissive).
# init does not restart what it did not start: reboot to undo.
T=/data/local/tmp
cp $T/libcurlspy.so /data/davs/libcurlspy.so; chown davs:davs /data/davs/libcurlspy.so; chmod 755 /data/davs/libcurlspy.so
chcon u:object_r:system_file:s0 /data/davs/libcurlspy.so
touch /data/davs/curlspy.log; chown davs:davs /data/davs/curlspy.log; chcon u:object_r:davs_data_file:s0 /data/davs/curlspy.log
chmod 755 $T/runas
# No leftovers: a second copy cannot register the service.  pkill does not get them here, kill -9 does.
stop assetmgrd; sleep 1; for p in $(pidof assetmgrd); do kill -9 $p; done
SHMLIB_DISABLE_ONLOAD=true CURLSPY_LOG=/data/davs/curlspy.log LD_PRELOAD=/data/davs/libcurlspy.so \
    $T/runas davs davs,dbus,aipc,inet,ace_kvstore,ace_group,ace_maplite \
    /system/bin/runcon u:r:assetmgrd:s0 /system/bin/assetmgrd > /dev/null 2>&1 < /dev/null &
sleep 20
for p in $(pidof assetmgrd); do echo "assetmgrd pid $p: shim $(grep -c curlspy /proc/$p/maps) mappings, domain $(cat /proc/$p/attr/current)"; done
echo "clients that cannot reach it (should be 0): $(logcat -d -t 500 2>/dev/null | grep -c 'Error connecting to service com.amazon.assetmgrd')"
