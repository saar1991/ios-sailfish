#!/system/bin/sh
# wp210 kp6 (read-only): find the PMIF / SPMI-master register base through the
# spmi_mtk_pmif driver's own data, then dump those registers.
# The driver stores its ioremapped bases inside the static template it matched
# (mt6xxx_pmif_arb or one of the mt68xx ones), so we read those templates first.
# Run as root after the kit is installed. Writes /data/local/tmp/kp6.txt
. /data/adb/wp210/lib.sh
O=/data/local/tmp/kp6.txt
: > $O
log() { echo "$*" >> $O; }
# rp NAME "ARGS": like probe(), but reports refusals and missing lines in the output
rp() {
  H=$(helper) || { echo "NOHELPER"; return 1; }
  inst wp210r
  if ! echo "p:wp210/$1 $FN $2" >> $T/kprobe_events 2>/dev/null; then
    echo "REFUSED: $(tail -3 $T/error_log 2>/dev/null | tr '\n' '|')"; return 1
  fi
  echo > $T/instances/wp210r/trace
  echo 1 > $T/instances/wp210r/events/wp210/$1/enable
  cat "$H" > /dev/null
  echo 0 > $T/instances/wp210r/events/wp210/$1/enable
  L=$(grep " $1: " $T/instances/wp210r/trace | head -1)
  [ -n "$L" ] || L="NOLINE: $(grep -v '^#' $T/instances/wp210r/trace | head -2 | tr '\n' '|')"
  echo "-:wp210/$1" >> $T/kprobe_events
  echo "$L" | sed 's/^.*dvfsrc_dump_reg[^)]*) //'
}
# add a hex offset to a kernel address (only the low 32 bits change; fine for these addresses)
addr() { H=${1%????????}; L=${1#????????}; S=00000000$(printf '%x' $((0x$L + $2))); echo "$H${S#"${S%????????}"}"; }

KR=$(cat /proc/sys/kernel/kptr_restrict); echo 0 > /proc/sys/kernel/kptr_restrict
lock
# leftovers from an earlier run
echo 0 > $T/instances/wp210r/events/wp210/enable 2>/dev/null
for e in $(grep '^p:wp210/' $T/kprobe_events | cut -d' ' -f1 | cut -d: -f2); do echo "-:$e" >> $T/kprobe_events; done
log "uptime $(up) rate $(rate)"
log "kprobe_events before: $(grep -c . $T/kprobe_events)"

LB=$(grep -E ' [DdRr] linux_banner$' /proc/kallsyms | head -1 | cut -d' ' -f1)
log "== test 1: 8 bytes of linux_banner (expect 0x657620786e75694c), DVFSRC/SPM pointers"
log "t1 $(rp t1 "a=@0x$LB:x64 b=@linux_banner:x64 dv=+32(%x0):x64 sp=+40(%x0):x64")"
log "== test 2: the old kp5 form, one value"
log "t2 $(rp t2 "v1=@0x$LB:x64")"

log "== driver templates (16 x 8 bytes each)"
PTRS=""
for S in mt6xxx_pmif_arb mt6893_pmif_arb mt6873_pmif_arb mt6853_pmif_arb mt6833_pmif_arb wp; do
  A=$(grep -E " [dDbB] $S	\[spmi_mtk_pmif\]" /proc/kallsyms | head -1 | cut -d' ' -f1)
  [ -n "$A" ] || { log "$S: not in kallsyms"; continue; }
  ARGS=""; k=0
  while [ $k -lt 128 ]; do ARGS="$ARGS o$(printf '%02x' $k)=@0x$(addr $A $k):x64"; k=$((k + 8)); done
  V=$(rp s_$S "$ARGS")
  log "$S @$A: $V"
  # remember every field that holds an ioremap-style pointer (same top 32 bits as the DVFSRC one)
  for f in $V; do
    case "$f" in o??=0xffffffc0*) PTRS="$PTRS $S:${f%%=*}:$A";; esac
  done
done
log "pointer fields: $PTRS"

# dump FIELD_ADDR FIRST LAST TAG : 32-bit words at (pointer stored at FIELD_ADDR)+FIRST..LAST
dump() {
  FA=$1; o=$2; Z=$3; TAG=$4; j=0
  while [ $o -le $Z ]; do
    ARGS=""; c=0
    while [ $c -lt 80 ] && [ $o -le $Z ]; do
      ARGS="$ARGS r$(printf '%03x' $o)=+0x$(printf '%x' $o)(@0x$FA):x32"; o=$((o + 4)); c=$((c + 1))
    done
    log "$TAG.$j $(rp d$j "$ARGS")"; j=$((j + 1))
  done
}
first=1
for p in $PTRS; do
  S=${p%%:*}; R=${p#*:}; F=${R%%:*}; A=${R#*:}
  FA=$(addr $A $((0x${F#o})))
  log "== $S field ${F#o} (stored at $FA): words 0x000-0x0fc"
  dump $FA 0 252 "$S.${F#o}"
  if [ $first = 1 ]; then
    log "== $S field ${F#o}: words 0x100-0x8fc (first pointer = PMIF base in every known layout)"
    dump $FA 256 2300 "$S.${F#o}"
    first=0
  fi
done
unlock
echo $KR > /proc/sys/kernel/kptr_restrict
chmod 644 $O
echo "written $O"; head -8 $O
