#!/system/bin/sh
# wp210 probe kit, Magisk post-fs-data.d: start as early as Magisk allows (read-only)
(
  . /data/adb/wp210/lib.sh
  [ -e $W/stop ] && exit 0
  REASON=early
  note "early start, $(grep -c . /proc/modules) modules loaded, boot_completed=$(getprop sys.boot_completed)"
  grep -iE 'dvfsrc|lpm|spm|pmif|spmi|mt63|pmic|regulator|vcore' /proc/modules > $D/modules-early.txt
  smc_start
  n=0; while ! helper > /dev/null; do sleep 0.5; n=$((n+1)); [ $n -gt 600 ] && break; done
  note "dvfsrc helper present after $n half-seconds; modules now $(grep -c . /proc/modules)"
  grep -iE 'dvfsrc|lpm|spm|pmif|spmi|mt63|pmic|regulator|vcore' /proc/modules > $D/modules-helper.txt
  REASON=first; key; full first
  while [ "$(getprop sys.boot_completed)" != 1 ]; do REASON=boot; key; sleep 2; done
  note "boot completed (early script done)"
) > /dev/null 2>&1 &
