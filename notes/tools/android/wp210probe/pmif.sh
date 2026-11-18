#!/system/bin/sh
# wp210 probe kit: PMIF / SPMI master / DRAM PHY registers on stock Android (read-only).
# Finds Android's own mappings of these blocks in /proc/vmallocinfo and reads them with
# kprobe absolute fetches (@ADDR), triggered through the DVFSRC debug driver's dump.
# Output: /data/local/tmp/wp210-probe/<boot id>/pmif-<uptime>.txt
. /data/adb/wp210/lib.sh
O=$D/pmif-$(up).txt
: > $O
log() { echo "$*" >> $O; }
log "uptime $(up) rate $(rate) suspends $(susp) screen $(screen)"
log "== mappings (vmallocinfo) of 0x1c804000 PMIF, 0x1c801000 SPMI master, 0x1c805000, 0x1c00f000 DVFSRC, 0x1c001000 SPM, 0x10238000/0x10239000 DDRPHY_AO ch0, 0x10230000 DRAMC_AO ch0"
KR=$(cat /proc/sys/kernel/kptr_restrict); echo 0 > /proc/sys/kernel/kptr_restrict
grep -E 'phys=0x0*(1c804000|1c801000|1c805000|1c00f000|1c001000|10238000|10239000|10230000|10236000|10234000)' /proc/vmallocinfo >> $O
# ADDR VA OFFSET: VA + OFFSET on the low 32 bits only (the shell's arithmetic is 32-bit)
addr() { H=${1%????????}; L=${1#??????????}; S=00000000$(printf '%x' $((0x$L + $2))); echo "$H${S#"${S%????????}"}"; }
VMI=$(grep ' ioremap' /proc/vmallocinfo)
echo $KR > /proc/sys/kernel/kptr_restrict
# first mapping of a physical block; nothing if the address is hidden (all zero)
va() { V=$(echo "$VMI" | grep -E "phys=0x0*$1 " | head -1 | cut -d- -f1); case "$V" in 0x0000000000000000|"") ;; *) echo $V ;; esac; }
# dump BASE_VA FIRST LAST TAG : words FIRST..LAST (byte offsets), 100 per probe
dumpblk() {
  V=$1; A=$2; Z=$3; TAG=$4; i=0
  [ -n "$V" ] || { log "== $TAG: no usable address (not mapped, or hidden)"; return; }
  log "== $TAG at $V"
  while [ $A -le $Z ]; do
    ARGS=""; n=0
    while [ $n -lt 100 ] && [ $A -le $Z ]; do
      ADDR=$(addr $V $A)
      ARGS="$ARGS $(printf 'r%03x' $A)=@$ADDR:x32"
      A=$((A + 4)); n=$((n + 1))
    done
    lock; R=$(probe p$i "$ARGS"); unlock
    log "$TAG$i $R"; i=$((i + 1))
  done
}
dumpblk "$(va 1c804000)" 0 $((0x8fc)) pmif
dumpblk "$(va 1c801000)" 0 $((0xfc)) spmimst
dumpblk "$(va 1c805000)" 0 $((0xfc)) pmif2
PHY=$(va 10238000)
if [ -n "$PHY" ]; then
  log "== DDRPHY_AO ch0 speed fields at $PHY (rate now $(rate))"
  ARGS=""
  for o in 0x904 0x924 0x908 0x928 0x90c 0x944 0xef0 0xef4 0xf34 0xe74 0x1204 0x1224 0x1208 0x1228 0x120c 0x1244 0x17f0 0x17f4 0x1834 0x1774; do
    ARGS="$ARGS $(printf 'r%x' $o)=@$(addr $PHY $o):x32"
  done
  lock; R=$(probe ph "$ARGS"); unlock; log "phy $R rate_after $(rate)"
else
  log "== DDRPHY_AO ch0 not mapped by Android"
fi
chmod 644 $O
echo "written $O"; head -12 $O; tail -3 $O
