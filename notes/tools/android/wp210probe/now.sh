#!/system/bin/sh
# wp210 probe kit: one key line and one full dump right now (checks the kit works)
. /data/adb/wp210/lib.sh
REASON=now; key; full now
echo "folder: $D"; tail -2 $D/summary.txt; tail -3 $D/events.txt; ls -la $D
