#!/system/bin/sh
# wp210 kp10 (Android, root, read-only): find where the mtk_dvfsrc driver keeps its register
# address, so a boot-time probe can read the DVFSRC from INIT on. Probes mtk_dvfsrc_send_request
# (x0 = the DVFSRC device) and triggers one request by withdrawing a DRAM request that isn't set
# (dvfsrc_req_ddr_opp 99), or waits for screen activity. Writes /data/local/tmp/kp10.txt
. /data/adb/wp210/lib.sh
O=/data/local/tmp/kp10.txt; : > $O
log() { echo "$*" >> $O; }
FNR=mtk_dvfsrc:mtk_dvfsrc_send_request
NODE=$(ls /sys/devices/platform/soc/*dvfsrc*/*dvfsrc-helper/dvfsrc_req_ddr_opp /sys/devices/platform/*dvfsrc*/*dvfsrc-helper/dvfsrc_req_ddr_opp 2>/dev/null | head -1)
log "trigger node: ${NODE:-none (touch the screen while it runs)}"
# rq NAME ARGS: probe the request function once, return the values
rq() {
  inst wp210r
  echo "p:wp210/$1 $FNR $2" >> $T/kprobe_events 2>/dev/null || { echo "REFUSED: $(tail -2 $T/error_log | tr '\n' '|')"; return 1; }
  echo > $T/instances/wp210r/trace
  echo 1 > $T/instances/wp210r/events/wp210/$1/enable
  n=0; L=""
  while [ -z "$L" ] && [ $n -lt 40 ]; do
    [ -n "$NODE" ] && echo 99 > $NODE 2>/dev/null
    sleep 0.5; n=$((n + 1))
    L=$(grep " $1: " $T/instances/wp210r/trace | head -1)
  done
  echo 0 > $T/instances/wp210r/events/wp210/$1/enable
  echo "-:wp210/$1" >> $T/kprobe_events
  [ -n "$L" ] || { echo "NOHIT"; return 1; }
  echo "$L" | sed -E 's/^.* ([0-9]+\.[0-9]+): [^ ]+ \([^)]*\) /ts=\1 /'
}
lock
DV=$(probe h "dv=+32(%x0):x64" | sed 's/.*dv=//')
log "DVFSRC registers (from the helper): $DV"
A=""; o=0; while [ $o -lt 512 ]; do A="$A q$(printf '%03x' $o)=+$o(%x0):x64"; o=$((o + 8)); done
V=$(rq s0 "$A"); log "device words: $V"
C=""
for f in $V; do
  case "$f" in q???=0xffffff[89ab]*) C="$C ${f%%=*}";; esac   # pointers into the kernel's RAM
done
log "pointer fields: $C"
FOUND=""
for q in $C; do
  off=$((0x${q#q}))
  A=""; k=0; while [ $k -lt 256 ]; do A="$A k$(printf '%02x' $k)=+$k(+$off(%x0)):x64"; k=$((k + 8)); done
  W=$(rq s1 "$A")
  case "$W" in *"=$DV"*)
    K=$(for w in $W; do case "$w" in *"=$DV") echo ${w%%=*};; esac; done | head -1)
    FOUND="dev+$off -> +$((0x${K#k})) = $DV"; log "MATCH: $q -> $K"; break;;
  esac
done
log "result: ${FOUND:-not found}"
if [ -n "$FOUND" ]; then
  O1=$off; O2=$((0x${K#k}))
  V=$(rq s2 "v290=+0x290(+$O2(+$O1(%x0))):x32 v29c=+0x29c(+$O2(+$O1(%x0))):x32 v3ac=+0x3ac(+$O2(+$O1(%x0))):x32 v5f0=+0x5f0(+$O2(+$O1(%x0))):x32")
  log "check (0x290 must be 0x60400020): $V"
fi
unlock
chmod 644 $O; cat $O
