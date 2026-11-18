#!/system/bin/sh
# wp210 kprobe step 1 (read-only): find the DVFSRC debug driver's dump functions
# and record the first 40 words of its struct (where it keeps the register pointers).
# Run as root:  su -c "sh /data/local/tmp/kp1.sh"   -> writes /sdcard/kp1.txt
OUT=/sdcard/kp1.txt
T=/sys/kernel/tracing
exec > $OUT 2>&1
id
uname -r
OLD=$(cat /proc/sys/kernel/kptr_restrict)
echo 0 > /proc/sys/kernel/kptr_restrict
echo "== dvfsrc symbols"
grep -iE 'dvfsrc|spm_read' /proc/kallsyms | head -300
H=$(find /sys/devices -name dvfsrc_dump 2>/dev/null | head -1)
echo "== helper file: $H"
echo "== existing kprobe events:"; cat $T/kprobe_events
echo 1 > $T/tracing_on
for S in dvfsrc_dump_mt6873_spm_info dvfsrc_dump_mt6983_spm_cmd dvfsrc_dump_reg dvfsrc_dump_record dvfsrc_dump_show; do
  if ! grep -qw "$S" /proc/kallsyms; then echo "== $S: not in kallsyms"; continue; fi
  A=""; i=0
  while [ $i -lt 40 ]; do A="$A w$i=+$((i*8))(%x0):x64"; i=$((i+1)); done
  if ! echo "p:wp210/k $S$A" >> $T/kprobe_events; then echo "== $S: probe refused"; cat $T/error_log | tail -5; continue; fi
  echo > $T/trace
  echo 1 > $T/events/wp210/k/enable
  cat "$H" > /dev/null
  echo 0 > $T/events/wp210/k/enable
  echo "== $S"
  grep ' k: ' $T/trace | head -2
  echo "-:wp210/k" >> $T/kprobe_events
done
echo "== kprobe events left:"; cat $T/kprobe_events
echo $OLD > /proc/sys/kernel/kptr_restrict
echo end
