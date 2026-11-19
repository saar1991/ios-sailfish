#!/system/bin/sh
# wp210 kp9 (Android, root): replace the boot-trace hook with one that also keeps the trace
# header and the per-CPU ring-buffer stats (lost/overwritten events), and a SIP-only extract.
mkdir -p /data/adb/post-fs-data.d
cat > /data/adb/post-fs-data.d/wp210-boottrace.sh <<'HOOK'
#!/system/bin/sh
# wp210: save the boot-time secure-call trace (only on a boot with the wp210b/smc kprobe)
T=/sys/kernel/tracing
[ -d $T/events/wp210b/smc ] || exit 0
BID=$(cut -c1-8 /proc/sys/kernel/random/boot_id)
D=/data/local/tmp/wp210-probe/$BID; mkdir -p $D
save() {
  F=$D/boottrace-$1.txt
  { echo "uptime $(cut -d' ' -f1 /proc/uptime) tracing_on $(cat $T/tracing_on) buffer_kb $(cat $T/buffer_size_kb) clock $(cat $T/trace_clock)"
    echo "cmdline $(cat /proc/cmdline)"
    for c in $T/per_cpu/cpu*; do echo "== ${c##*/}"; cat $c/stats; done
    cat $T/trace; } > $F
  grep -E '^#|^==|uptime|cmdline|entries|overrun|oldest|a0=0x(82|c2|84|b2|bb)' $F | grep -v 'a0=0xc4000001' > $D/boottrace-sip-$1.txt
  dmesg > $D/dmesg-$1.txt 2>&1 || logcat -b kernel -d > $D/dmesg-$1.txt 2>&1
  cat /proc/bootprof > $D/bootprof-$1.txt 2>/dev/null
  cat /proc/modules > $D/modules-$1.txt
  chmod 644 $D/*-$1.txt
}
save postfs
( while [ "$(getprop sys.boot_completed)" != 1 ]; do sleep 2; done; save booted ) > /dev/null 2>&1 &
HOOK
chmod 755 /data/adb/post-fs-data.d/wp210-boottrace.sh
ls -l /data/adb/post-fs-data.d/wp210-boottrace.sh
