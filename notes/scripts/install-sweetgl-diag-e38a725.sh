#!/system/bin/sh
# WP210 only. Fast-path build, commit e38a725 (branch sweetgl-diag-0x502), based on 1465988ae4f2a32059c9b129db344550b80c8876.
# Default: check only. --install performs the bounded raw-device write.
# Never use this script on another storage layout or after remapping the file.
set -eu
umask 077
die() { echo "STOP: $*" >&2; exit 1; }
MODE=${1:---check}
case "$MODE" in --check|--install) ;; *) die 'Use --check or --install' ;; esac
[ "$(id -u)" = 0 ] || die 'Root is required'
for cmd in dd sha256sum wc cp sync mktemp grep cat od tr; do
    command -v "$cmd" >/dev/null 2>&1 || die "Missing tool: $cmd"
done
DEVICE=/dev/block/sdc
START=122729397
COUNT=4202
BYTES=17211392
OLD=a313176ca26616bed7f12634d6511bb2ecf7bce2ba8d32a2384447d0ed349818
NEW=6eda66af04b770f61c2a3ad8efce8074e40b840a8bebd4908a9f99d31198a55a
INPUT=/data/local/tmp/sweetgl-diag-e38a725-allocation.bin
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
# Catalog check, by field instead of whole-block hashes: every iOS boot
# rewrites the volume header and the first catalog nodes (dates, counts),
# which made the block hashes go stale after each boot. What the write
# depends on is checked directly:
#   - volume header: HFSX version 5, 4 KiB blocks, same size, the catalog
#     file in the same extents (so catalog node N is where we read it)
#   - catalog header node: 8 KiB nodes, 20480 of them, node 19204 in use
#   - node 19204: a leaf whose live records (by its offset table, not stray
#     bytes) include /usr/lib/sweetgl.dylib exactly once, as file 318814,
#     17208704 bytes, one extent: volume block 3015861 x 4202 = device
#     block 122729397 x 4202, the range this script writes
#   - the four catalog index/path nodes that never changed: hashes as before
hx() {
    # bytes of file $1 (a copy already read from the device) at offset $2,
    # length $3, as lowercase hex without spaces
    od -A n -t x1 -v -j "$2" -N "$3" "$1" | tr -d ' \n'
}
expect_hex() {
    got=$(hx "$1" "$2" "$3") || die "Cannot read $5"
    [ "$got" = "$4" ] || die "Catalog check failed: $5 (found $got)"
}
SHIM_KEY=00200004ca40000d007300770065006500740067006c002e00640079006c00690062
SHIM_FORK=0000000001069580000000000000106a002e04b50000106a0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
CAT_FORK=000000000c000000020000000000c0000002cd820000400000033b050000200000114cc000002000001a626c00002000002e279600002000000000000000000000000000000000000000000000000000
check_catalog_files() {
    vh=$1 node0=$2 leaf=$3
    expect_hex "$vh" 1024 4 48580005 'volume header signature and version (HFSX 5)'
    expect_hex "$vh" 1064 8 0000100000400000 'volume block size and block count'
    expect_hex "$vh" 1296 80 "$CAT_FORK" 'catalog file size and extents'
    expect_hex "$node0" 8 1 01 'catalog header node kind'
    expect_hex "$node0" 32 2 2000 'catalog node size'
    expect_hex "$node0" 36 4 00006000 'catalog node count'
    expect_hex "$node0" 8186 2 00f8 'catalog map record offset'
    map=$(hx "$node0" 2648 1) || die 'Cannot read the catalog node map'
    [ $((0x$map & 8)) -ne 0 ] || die 'Catalog check failed: node 19204 is not in use'
    expect_hex "$leaf" 8 2 ff01 'node 19204 kind and height (leaf)'
    nrec=$(hx "$leaf" 10 2) || die 'Cannot read node 19204'
    nrec=$((0x$nrec))
    [ "$nrec" -ge 1 ] && [ "$nrec" -le 300 ] || die "Catalog check failed: node 19204 has $nrec records"
    found=''
    i=0
    while [ "$i" -lt "$nrec" ]; do
        off=$(hx "$leaf" $((8190 - 2 * i)) 2) || die 'Cannot read node 19204 offsets'
        off=$((0x$off))
        if [ "$off" -ge 14 ] && [ "$off" -le 7900 ] && [ "$(hx "$leaf" "$off" 34)" = "$SHIM_KEY" ]; then
            [ -z "$found" ] || die 'Catalog check failed: two sweetgl.dylib records in node 19204'
            found=$off
        fi
        i=$((i + 1))
    done
    [ -n "$found" ] || die 'Catalog check failed: the sweetgl.dylib record is no longer in node 19204'
    expect_hex "$leaf" $((found + 34)) 2 0002 'sweetgl.dylib record type (file)'
    expect_hex "$leaf" $((found + 42)) 4 0004dd5e 'sweetgl.dylib file ID'
    expect_hex "$leaf" $((found + 122)) 80 "$SHIM_FORK" 'sweetgl.dylib size and allocation'
}
check_metadata() {
    read_region 119713536 1 "$WORK/vh.bin"
    read_region 119897218 2 "$WORK/catalog-node0.bin"
    read_region 121448308 2 "$WORK/catalog-node19204.bin"
    check_catalog_files "$WORK/vh.bin" "$WORK/catalog-node0.bin" "$WORK/catalog-node19204.bin"
    # catalog index nodes on the path to the record: unchanged by every boot so far
    while read -r block count expected; do
        read_region "$block" "$count" "$WORK/metadata.bin"
        hash_check "$WORK/metadata.bin" "$expected"
    done <<'METADATA'
121450324 2 467d21737003d9850f6397a6ae3a7c89bb6970b2fcd6d394c6e26b7e5c15ddfc
121450322 2 2380f94acf92bae547722bad5f85a53b66a5dab6f3d7f5467b804d195f005770
121448304 2 f1374e3dd8df716a89ae4db945775ce3db98c7c18cb6a95e711f5fce7eab2963
121448452 2 4a857befe9e3a12b0db676f885c76d0c8f6c5003ccf518f9e00116feb3e41d53
METADATA
    echo "Catalog check passed: sweetgl.dylib is file 318814, 17208704 bytes at device block 122729397 x 4202 (node 19204 offset $found, $nrec records)."
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
