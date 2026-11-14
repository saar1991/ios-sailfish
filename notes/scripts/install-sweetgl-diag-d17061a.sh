#!/system/bin/sh
# WP210 only. Diagnostic build, commit d17061a (branch sweetgl-diag-0x502), based on 1465988ae4f2a32059c9b129db344550b80c8876.
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
OLD=552d50d6752af295747711f36f8562e5ea5313cdb719cd4da65bb6825e00a5a3
NEW=08cc1bf0ae32de3feee0d54499421be70367d953d273133739320f07dabd2814
INPUT=/data/local/tmp/sweetgl-diag-d17061a-allocation.bin
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
119713536 1 4b67140afe6bb4cae0bdfd184a10d9d11e09a121b53e11f4086e198d7227ae12
119897218 1 4b68b2ddd1df1ae6edff8673e2987665ad20d4a9af4f5de2237068def3ed617c
121450324 2 467d21737003d9850f6397a6ae3a7c89bb6970b2fcd6d394c6e26b7e5c15ddfc
121450322 2 2380f94acf92bae547722bad5f85a53b66a5dab6f3d7f5467b804d195f005770
119897220 6 ca76f6e6cc96d067edbeb1282b73de8abb3098e4aff2200168b2fc48b595eee1
121448304 2 f1374e3dd8df716a89ae4db945775ce3db98c7c18cb6a95e711f5fce7eab2963
119897768 2 ee214b1cd2662e20589570314f4ee2a9ee9b91545766ea223319ca8dc9ba53f4
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
