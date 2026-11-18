#!/system/bin/sh
# wp210 probe kit: stop the running sampler (and keep it from starting at the next boot)
touch /data/adb/wp210/stop
. /data/adb/wp210/lib.sh
smc_stop
echo "stopped; to run again at the next boot: rm /data/adb/wp210/stop"
