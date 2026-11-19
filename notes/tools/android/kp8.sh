#!/system/bin/sh
# wp210 kp8 (Android, root, read-only): what Android's kernel does in its first seconds.
# Run soon after a fresh boot. Saves the kernel log, MediaTek's boot profile, the module
# list and the power-management kernel modules themselves (to read their start-up code).
# Output: /data/local/tmp/kp8.tgz
K=/data/local/tmp/kp8; rm -rf $K; mkdir -p $K/ko
rm -f /data/adb/post-fs-data.d/wp210-boottrace.sh   # kp7's hook: fastboot boot is not supported here
echo "uptime $(cut -d' ' -f1 /proc/uptime) kernel $(uname -r)" > $K/info.txt
dmesg > $K/dmesg.txt 2>&1 || logcat -b kernel -d > $K/dmesg.txt 2>&1
cat /proc/cmdline > $K/cmdline.txt
cat /proc/bootprof > $K/bootprof.txt 2>&1
cat /proc/modules > $K/modules.txt
for d in /vendor_dlkm/lib/modules /vendor/lib/modules /system/lib/modules /lib/modules /odm/lib/modules; do
  [ -d $d ] || continue
  echo "== $d" >> $K/module-dirs.txt
  ls -l $d >> $K/module-dirs.txt
  for f in modules.load modules.dep modules.alias modules.softdep; do [ -f $d/$f ] && cp $d/$f $K/$(echo $d | tr / _)-$f; done
  for f in $d/*.ko; do
    case "${f##*/}" in
      *lpm*|*spm*|*dvfsrc*|*sspm*|*scmi*|*tinysys*|*dramc*|*emi*|*qos*|*scpsys*|*vcore*|*dvfs*|*mcupm*|*cm_mgr*|*swpm*|*clkbuf*|*srclken*|*cpuidle*|*cpufreq*|*pmif*|*spmi*|*pmic_wrap*|*mtk_pm*|*mtk_low_power*|*slbc*|*mmdvfs*|*apmcu*)
        [ -e $K/ko/${f##*/} ] || cp $f $K/ko/ ;;
    esac
  done
done
ls -l $K/ko > $K/ko-list.txt
cd /data/local/tmp && tar czf kp8.tgz kp8 && chmod 644 kp8.tgz
ls -l kp8.tgz; echo "modules copied: $(ls $K/ko | wc -l)"; head -3 $K/dmesg.txt; grep -c . $K/dmesg.txt
