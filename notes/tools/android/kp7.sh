#!/system/bin/sh
# wp210 kp7 (Android, root): prepare a one-time traced Android boot.
# 1) copies the Android boot partition (boot_b, read only) to /data/local/tmp/boot_b.img
# 2) installs a Magisk post-fs-data hook that, on a boot made with the wp210b/smc boot-time
#    kprobe, saves the secure calls the kernel made from the very start (plus dmesg, bootprof).
# Nothing is flashed. Writes /data/local/tmp/kp7.txt
O=/data/local/tmp/kp7.txt; : > $O
log() { echo "$*" >> $O; }
SLOT=$(getprop ro.boot.slot_suffix)
log "slot $SLOT  kernel $(uname -r)"
log "cmdline: $(cat /proc/cmdline)"
B=/dev/block/by-name/boot$SLOT
[ -e $B ] || B=$(ls /dev/block/platform/*/by-name/boot$SLOT /dev/block/platform/*/*/by-name/boot$SLOT 2>/dev/null | head -1)
log "boot partition: $B"
dd if=$B of=/data/local/tmp/boot$SLOT.img bs=1048576 2>> $O
chmod 644 /data/local/tmp/boot$SLOT.img
log "image: $(ls -l /data/local/tmp/boot$SLOT.img)"
log "sha256: $(sha256sum /data/local/tmp/boot$SLOT.img | cut -d' ' -f1)"
log "magic/version: $(dd if=/data/local/tmp/boot$SLOT.img bs=1 count=8 2>/dev/null) v$(od -An -tu4 -j40 -N4 /data/local/tmp/boot$SLOT.img | tr -d ' ')"
log "header cmdline: [$(dd if=/data/local/tmp/boot$SLOT.img bs=1 skip=44 count=1536 2>/dev/null | tr -d '\0')]"
log "magisk: $(magisk -v 2>/dev/null) $(magisk -V 2>/dev/null)"
mkdir -p /data/adb/post-fs-data.d
cat > /data/adb/post-fs-data.d/wp210-boottrace.sh <<'HOOK'
#!/system/bin/sh
# wp210: save the boot-time secure-call trace (only on a boot with the wp210b/smc kprobe)
T=/sys/kernel/tracing
[ -d $T/events/wp210b/smc ] || exit 0
BID=$(cut -c1-8 /proc/sys/kernel/random/boot_id)
D=/data/local/tmp/wp210-probe/$BID; mkdir -p $D
save() {
  echo "uptime $(cut -d' ' -f1 /proc/uptime) tracing_on $(cat $T/tracing_on) buffer_kb $(cat $T/buffer_size_kb)" > $D/boottrace-$1.txt
  grep -v '^#' $T/trace >> $D/boottrace-$1.txt
  dmesg > $D/dmesg-$1.txt 2>&1 || logcat -b kernel -d > $D/dmesg-$1.txt 2>&1
  cat /proc/bootprof > $D/bootprof-$1.txt 2>/dev/null
  cat /proc/modules > $D/modules-$1.txt
  chmod 644 $D/*-$1.txt
}
save postfs
( while [ "$(getprop sys.boot_completed)" != 1 ]; do sleep 2; done; save booted ) > /dev/null 2>&1 &
HOOK
chmod 755 /data/adb/post-fs-data.d/wp210-boottrace.sh
log "hook: $(ls -l /data/adb/post-fs-data.d/wp210-boottrace.sh)"
chmod 644 $O
cat $O
