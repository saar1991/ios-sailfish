#!/system/bin/sh
# wp210 kprobe step 3 (read-only). Run right after a fresh Android boot, USB connected.
# 1) key SPM/DVFSRC registers before Android has ever suspended (through the DVFSRC
#    debug driver's pointers, as kp2.sh), with the suspend count and DRAM rate;
# 2) 60 s of MediaTek SIP secure calls (__arm_smccc_smc with x0 in 0x82xxxxxx/0xc2xxxxxx);
# 3) the registers again. Writes /sdcard/kp3.txt
OUT=/sdcard/kp3.txt
T=/sys/kernel/tracing
F=mtk_dvfsrc_helper:dvfsrc_dump_reg
H=$(find /sys/devices -name dvfsrc_dump 2>/dev/null | head -1)
R=$(find /sys/bus/platform/drivers -name dram_data_rate 2>/dev/null | head -1)
: > $OUT
log() { echo "$*" >> $OUT; sync; }
log "$(id)"
log "uptime: $(cat /proc/uptime)"
echo 1 > $T/tracing_on
regs() {
  log "== registers ($1): uptime $(cut -d' ' -f1 /proc/uptime) s"
  log "suspend success: $(cat /sys/power/suspend_stats/success 2>&1) fail: $(cat /sys/power/suspend_stats/fail 2>&1)"
  log "dram rate: $(cat $R 2>&1)"
  echo "p:wp210/r $F scu0=+0x100(+40(%x0)):x32 scu1=+0x104(+40(%x0)):x32 scu2=+0x108(+40(%x0)):x32 scu3=+0x10c(+40(%x0)):x32 sta0=+0x110(+40(%x0)):x32 pc=+0x194(+40(%x0)):x32 spmi=+0x308(+40(%x0)):x32 ev=+0x380(+40(%x0)):x32 dsta=+0x388(+40(%x0)):x32 lv1=+0x38c(+40(%x0)):x32 lv2=+0x390(+40(%x0)):x32 ultra=+0x398(+40(%x0)):x32 flag0=+0x600(+40(%x0)):x32 rsv7=+0x62c(+40(%x0)):x32 ctl=+0x0(+32(%x0)):x32 dbg0=+0x29c(+32(%x0)):x32 last=+0x3ac(+32(%x0)):x32 r5c8=+0x5c8(+32(%x0)):x32 level=+0x5f0(+32(%x0)):x32" >> $T/kprobe_events || log "register probe refused: $(tail -3 $T/error_log)"
  echo > $T/trace
  echo 1 > $T/events/wp210/r/enable
  cat "$H" > /dev/null
  echo 0 > $T/events/wp210/r/enable
  log "$(grep ' r: ' $T/trace | head -1 | sed 's/.*) //')"
  echo "-:wp210/r" >> $T/kprobe_events
}
regs "first"
log "== SIP secure calls for 60 s"
echo 8192 > $T/buffer_size_kb
if echo "p:wp210/smc __arm_smccc_smc a0=%x0:x64 a1=%x1:x64 a2=%x2:x64 a3=%x3:x64 a4=%x4:x64 a5=%x5:x64" >> $T/kprobe_events; then
  echo '(a0 >= 0x82000000 && a0 < 0x83000000) || (a0 >= 0xc2000000 && a0 < 0xc3000000)' > $T/events/wp210/smc/filter || log "filter refused: $(tail -2 $T/error_log)"
  echo > $T/trace
  echo 1 > $T/events/wp210/smc/enable
  sleep 60
  echo 0 > $T/events/wp210/smc/enable
  log "calls recorded: $(grep -c ' smc: ' $T/trace)"
  grep ' smc: ' $T/trace | sed 's/^ *//' | awk '{c=$1; $1=$2=$3=""; print c, $0}' | sed 's/(__arm_smccc_smc+0x0\/0x[0-9a-f]*) //' > /sdcard/kp3-calls.txt
  log "distinct calls (count, a0 a1 a2 a3 a4 a5):"
  sed 's/^[^ ]* *//' /sdcard/kp3-calls.txt | sed 's/^[0-9.:]* *//' | sort | uniq -c | sort -rn | head -150 >> $OUT
  log "callers of the first 40:"
  head -40 /sdcard/kp3-calls.txt >> $OUT
  echo "-:wp210/smc" >> $T/kprobe_events
else
  log "smc probe refused: $(tail -3 $T/error_log)"
fi
regs "after 60 s"
log "kprobe events left: $(cat $T/kprobe_events)"
log end
