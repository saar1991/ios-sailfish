#!/system/bin/sh
# wp210 kp11 (Android, root, read-only): find where the mtk_dvfsrc driver keeps its register
# address, so a boot-time probe can read the DVFSRC from INIT on. Probes mtk_dvfsrc_send_request
# (x0 = the DVFSRC device) and triggers one request by withdrawing a DRAM request that isn't set
# (dvfsrc_req_ddr_opp 99), or waits for screen activity. Writes /data/local/tmp/kp11.txt
. /data/adb/wp210/lib.sh
O=/data/local/tmp/kp11.txt; : > $O
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
log "helper's own DVFSRC mapping: $DV"
# kp11 run 1: GKI struct device = kobject 0x60 (with 4 KABI words), parent 0x60, p 0x68, init_name 0x70,
# type 0x78, bus 0x80, driver 0x88, platform_data 0x90, driver_data 0x98. The main driver maps the
# DVFSRC itself (another address than the helper's), so look for any ioremap-style pointer in its struct.
A=""; k=0; while [ $k -lt 512 ]; do A="$A k$(printf '%03x' $k)=+$k(+0x98(%x0)):x64"; k=$((k + 8)); done
V=$(rq s0 "$A"); log "driver_data words: $V"
A=""
for f in $V; do
  case "$f" in k???=0xffffffc0*) kk=$((0x$(echo ${f%%=*} | cut -c2-))); A="$A ${f%%=*}=+0x290(+$kk(+0x98(%x0))):x32";; esac
done
log "ioremap-style fields: $A"
[ -n "$A" ] && log "0x290 through each (the DVFSRC reads 0x60400020): $(rq s1 "$A")"
unlock
chmod 644 $O; cat $O
