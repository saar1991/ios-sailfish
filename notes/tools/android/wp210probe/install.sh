#!/system/bin/sh
# wp210 probe kit installer: run as root from the pushed folder
S=${0%/*}
mkdir -p /data/adb/wp210 /data/adb/post-fs-data.d /data/adb/service.d
for f in lib.sh now.sh pmif.sh collect.sh stop.sh uninstall.sh; do cp "$S/$f" /data/adb/wp210/; done
cp "$S/wp210-early.sh" /data/adb/post-fs-data.d/
cp "$S/wp210-service.sh" /data/adb/service.d/
chmod 755 /data/adb/wp210/*.sh /data/adb/post-fs-data.d/wp210-early.sh /data/adb/service.d/wp210-service.sh
rm -f /data/adb/wp210/stop
[ -n "$1" ] && echo "$1" > /data/adb/wp210/duration
echo "installed: runs at every boot for $(cat /data/adb/wp210/duration 2>/dev/null || echo 3600) s"
ls -la /data/adb/post-fs-data.d/wp210-early.sh /data/adb/service.d/wp210-service.sh
