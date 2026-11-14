#!/system/bin/sh
# WP210 only. Fast-path diagnostics build, commit 14f179d (branch sweetgl-diag-0x502), based on 1465988ae4f2a32059c9b129db344550b80c8876.
# Default: check only. --install performs the bounded raw-device write.
# Never use this script on another storage layout or after remapping the file.
set -eu
umask 077
die() { echo "STOP: $*" >&2; exit 1; }
MODE=${1:---check}
case "$MODE" in --check|--install) ;; *) die 'Use --check or --install' ;; esac
[ "$(id -u)" = 0 ] || die 'Root is required'
for cmd in dd sha256sum wc cp sync mktemp grep cat; do
    command -v "$cmd" >/dev/null 2>&1 || die "Missing tool: $cmd"
done
DEVICE=/dev/block/sdc
START=122729397
COUNT=4202
BYTES=17211392
OLD=f7c8d02b2fcfd1d3c3e06997d8f0d4f8319c8f4c88e8208ea997a0462c180793
NEW=b1059f3388ebfe0cc445d36648255626abc47828a1366d3682d64274454c15d4
INPUT=/data/local/tmp/sweetgl-diag-14f179d-allocation.bin
[ -b "$DEVICE" ] || die 'Expected block device is absent'
[ "$(cat /sys/class/block/sdc/size)" = 999686144 ] || die 'Disk size changed'
[ "$(cat /sys/class/block/sdc/queue/logical_block_size)" = 4096 ] || die 'Block size changed'
[ "$(cat /sys/class/block/sdc70/start)" = 28426240 ] || die 'userdata start changed'
[ "$(cat /sys/class/block/sdc70/size)" = 929282048 ] || die 'userdata size changed'
[ "$(cat /sys/class/block/sdc71/start)" = 999653312 ] || die 'flashinfo start changed'
[ "$(cat /sys/class/block/sdc71/size)" = 32768 ] || die 'flashinfo size changed'
# Check every current sdc partition against the entire reserved iOS range.
# Sysfs start/size are 512-byte sectors, not the 4096-byte dd units.
for part in /sys/class/block/sdc[0-9]*; do
    [ -f "$part/start" ] || continue
    ps=$(cat "$part/start")
    pn=$(cat "$part/size")
    pe=$((ps + pn))
    if [ "$ps" -lt 999653312 ] && [ "$pe" -gt 957708288 ]; then
        die "Partition overlaps reserved iOS region: $part"
    fi
done
# Refuse any mounted HFS volume. This procedure is for Android, with iOS inactive.
mounts=$(cat /proc/mounts) || die 'Cannot inspect mounts'
if printf '%s\n' "$mounts" | grep -Eq '[[:space:]]hfs(plus|x)?[[:space:]]'; then
    die 'An HFS filesystem is mounted; no raw writes allowed'
fi
WORK=$(mktemp -d /data/local/tmp/sweetgl-check.XXXXXX) || die 'Cannot create staging directory'
echo "Verification directory: $WORK (retained for backup/review)"
# Android toybox dd may reject iflag=direct. Probe read-only and try BusyBox,
# including Magisk's bundled binary. Never fall back to cached verification.
DIRECT_BUSYBOX=''
if dd if="$DEVICE" of="$WORK/direct-probe.bin" bs=4096 skip=119713536 count=1 iflag=direct 2>"$WORK/dd-probe.log"; then
    echo 'Direct reads: system dd'
else
    for candidate in "$(command -v busybox || true)" /data/adb/magisk/busybox; do
        [ -n "$candidate" ] && [ -x "$candidate" ] || continue
        if "$candidate" dd if="$DEVICE" of="$WORK/direct-probe.bin" bs=4096 skip=119713536 count=1 iflag=direct 2>"$WORK/busybox-probe.log"; then
            DIRECT_BUSYBOX=$candidate
            break
        fi
    done
    [ -n "$DIRECT_BUSYBOX" ] || die 'No working direct-read dd found. No raw-device writes performed.'
    echo "Direct reads: $DIRECT_BUSYBOX dd"
fi
direct_dd() {
    if [ -n "$DIRECT_BUSYBOX" ]; then
        "$DIRECT_BUSYBOX" dd "$@"
    else
        dd "$@"
    fi
}
hash_check() {
    digest=$(sha256sum "$1") || die "Cannot hash $1"
    digest=${digest%% *}
    [ "$digest" = "$2" ] || die "SHA-256 mismatch: $1"
}
read_region() {
    # Direct I/O avoids satisfying verification from the block-device page cache.
    # Unsupported iflag=direct causes a stop BEFORE any write.
    direct_dd if="$DEVICE" of="$3" bs=4096 skip="$1" count="$2" iflag=direct || die 'Read failed'
    length=$(wc -c < "$3")
    [ "$length" -eq "$(( $2 * 4096 ))" ] || die 'Short read'
}
check_metadata() {
    while read -r block count expected; do
        read_region "$block" "$count" "$WORK/metadata.bin"
        hash_check "$WORK/metadata.bin" "$expected"
    done <<'METADATA'
119713536 1 06190332e511ed9807174cf330407329e89579a70a279b14e841310773b5a4e3
119897218 1 21eefde0092292e418b311f19290b59e8190816590714748573f24d8fb498045
121450324 2 467d21737003d9850f6397a6ae3a7c89bb6970b2fcd6d394c6e26b7e5c15ddfc
121450322 2 2380f94acf92bae547722bad5f85a53b66a5dab6f3d7f5467b804d195f005770
119897220 6 78b5022b2104403275f9857da5077fd61249ff268493339fc72a406885d491fa
121448304 2 f1374e3dd8df716a89ae4db945775ce3db98c7c18cb6a95e711f5fce7eab2963
119897768 2 d1cd37e13795b3e06b7b13ad29068d69be9fd493d80577daa59351d6dad07db9
121448452 2 4a857befe9e3a12b0db676f885c76d0c8f6c5003ccf518f9e00116feb3e41d53
121448308 2 8398afb0faeb1c740d74310e3af374ad8fb663cb3b9f77d9a0918405e587c2a4
METADATA
}
[ -f "$INPUT" ] || die 'Upload the prepared allocation payload first'
cp "$INPUT" "$WORK/replacement.bin" || die 'Cannot stage replacement'
[ "$(wc -c < "$WORK/replacement.bin")" -eq "$BYTES" ] || die 'Wrong payload size'
hash_check "$WORK/replacement.bin" "$NEW"
check_metadata
read_region "$START" "$COUNT" "$WORK/before.bin"
hash_check "$WORK/before.bin" "$OLD"
if [ "$MODE" = --check ]; then
    echo 'CHECK PASSED: payload, geometry, catalog path and current allocation match.'
    echo 'No raw-device writes performed. Re-run with --install only when ready.'
    exit 0
fi
echo 'Writing exactly 4202 blocks to sdc at block 122729397.'
echo "Current allocation backup: $WORK/before.bin"
dd if="$WORK/replacement.bin" of="$DEVICE" bs=4096 seek="$START" count="$COUNT" conv=notrunc || die "WRITE FAILED: do not boot iOS; backup is $WORK/before.bin"
sync || die 'Sync failed; do not boot iOS'
read_region "$START" "$COUNT" "$WORK/after.bin"
hash_check "$WORK/after.bin" "$NEW"
check_metadata
echo 'INSTALL VERIFIED: replacement allocation matches; catalog metadata unchanged.'
echo 'No reboot performed.'
