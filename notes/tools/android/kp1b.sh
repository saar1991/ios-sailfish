#!/system/bin/sh
# wp210 kprobe step 1b (read-only): same as step 1, but the probe names the module
# (the dump functions exist in more than one place). Writes /sdcard/kp1b.txt
OUT=/sdcard/kp1b.txt
T=/sys/kernel/tracing
exec > $OUT 2>&1
id
OLD=$(cat /proc/sys/kernel/kptr_restrict)
echo 0 > /proc/sys/kernel/kptr_restrict
echo "== named dvfsrc/spm functions (no \$x/\$d)"
grep -E ' [tTwW] ' /proc/kallsyms | grep -iE 'dvfsrc|spm_read|spm_info|spm_cmd' | grep -v '\$' | head -200
H=$(find /sys/devices -name dvfsrc_dump 2>/dev/null | head -1)
echo "== helper file: $H"
echo 1 > $T/tracing_on
for S in dvfsrc_dump_mt6873_spm_info dvfsrc_dump_mt6983_spm_cmd dvfsrc_dump_reg dvfsrc_dump_record dvfsrc_dump_show; do
  echo "== $S: exact matches"
  grep -E " [tTwW] $S(\s|$)" /proc/kallsyms
  A=""; i=0
  while [ $i -lt 40 ]; do A="$A w$i=+$((i*8))(%x0):x64"; i=$((i+1)); done
  echo > $T/error_log
  if echo "p:wp210/k mtk_dvfsrc_helper:$S$A" >> $T/kprobe_events; then
    echo "probe on mtk_dvfsrc_helper:$S"
  else
    ADDR=$(grep -E " [tT] $S\s+\[mtk_dvfsrc_helper\]" /proc/kallsyms | head -1 | cut -d' ' -f1)
    echo "module form refused: $(cat $T/error_log | head -2)"
    if [ -n "$ADDR" ] && echo "p:wp210/k 0x$ADDR$A" >> $T/kprobe_events; then
      echo "probe on 0x$ADDR"
    else
      echo "address form refused: $(cat $T/error_log | tail -2)"
      continue
    fi
  fi
  echo > $T/trace
  echo 1 > $T/events/wp210/k/enable
  cat "$H" > /dev/null
  echo 0 > $T/events/wp210/k/enable
  echo "== $S trace"
  grep ' k: ' $T/trace | head -2
  echo "-:wp210/k" >> $T/kprobe_events
done
echo "== kprobe events left:"; cat $T/kprobe_events
echo $OLD > /proc/sys/kernel/kptr_restrict
echo end
