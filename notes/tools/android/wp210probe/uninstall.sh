#!/system/bin/sh
# wp210 probe kit: remove the boot scripts and probes (results in /data/local/tmp/wp210-probe stay)
touch /data/adb/wp210/stop
. /data/adb/wp210/lib.sh
smc_stop
for e in $(grep -o 'wp210/[a-z0-9]*' /sys/kernel/tracing/kprobe_events); do echo "-:$e" >> /sys/kernel/tracing/kprobe_events; done
rm -f /data/adb/post-fs-data.d/wp210-early.sh /data/adb/service.d/wp210-service.sh
rm -rf /data/adb/wp210
echo "removed"
