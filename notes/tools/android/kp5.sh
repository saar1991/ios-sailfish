#!/system/bin/sh
# wp210 kp5 (read-only): find where the PMIF/SPMI/clkbuf drivers keep their register base.
# 1) checks absolute-address kprobe fetches with a known string (linux_banner);
# 2) lists global variables of the pmif/spmi/clkbuf modules and reads 8 bytes of each.
# Run from /data/local/tmp as root after the kit is installed. Writes /data/local/tmp/kp5.txt
. /data/adb/wp210/lib.sh
O=/data/local/tmp/kp5.txt
: > $O
log() { echo "$*" >> $O; }
KR=$(cat /proc/sys/kernel/kptr_restrict); echo 0 > /proc/sys/kernel/kptr_restrict
log "== modules"
grep -iE 'pmif|spmi|clkbuf|pmic|mt6363|mt6368|regulator' /proc/modules >> $O
LB=$(grep -E ' [DdRr] linux_banner$' /proc/kallsyms | head -1 | cut -d' ' -f1)
log "== fetch check: linux_banner at 0x$LB"
lock; R=$(probe chk1 "s=@linux_banner:string a=@0x$LB:string"); unlock
log "chk $R"
log "== candidate globals (data/bss) in pmif/spmi/clkbuf modules"
grep -E ' [dDbB] ' /proc/kallsyms | grep -iE '\[(.*pmif.*|.*spmi.*|clkbuf.*|.*pmic.*)\]|pmif|spmi' | grep -v '\$' | head -600 > /data/local/tmp/kp5-syms.txt
log "$(wc -l < /data/local/tmp/kp5-syms.txt) symbols"
cat /data/local/tmp/kp5-syms.txt | head -200 >> $O
log "== 8-byte contents (x64), 60 per probe"
i=0; ARGS=""; n=0; NAMES=""
while read A T N M; do
  n=$((n + 1)); ARGS="$ARGS v$n=@0x$A:x64"; NAMES="$NAMES v$n=$N$M"
  if [ $n -ge 60 ]; then
    lock; R=$(probe q$i "$ARGS"); unlock
    log "names$i:$NAMES"; log "vals$i: $R"
    i=$((i + 1)); n=0; ARGS=""; NAMES=""
  fi
done < /data/local/tmp/kp5-syms.txt
if [ $n -gt 0 ]; then lock; R=$(probe q$i "$ARGS"); unlock; log "names$i:$NAMES"; log "vals$i: $R"; fi
echo $KR > /proc/sys/kernel/kptr_restrict
chmod 644 $O
echo "written $O"; head -6 $O
