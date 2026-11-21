#!/system/bin/sh
# WP210 only. llvmpipe shim 9ee4913: fc34580's rasterizer fast path plus
# copies with Core Animation's noise dither: workflow sweetgl-llvmpipe run
# 44894873357 (macos-15), branch llvmpipe-test commit 9ee4913.
# Replaces the fc34580 shim in the same space.
# Goes into the space of /usr/standalone/update/ramdisk/arm64SURamDisk.dmg
# (the software-update ramdisk, unused by this port): file 315534,
# 113246235 bytes, one extent at device block 121877958 x 27649.
# /usr/lib/sweetgl.dylib (softpipe) is not touched.
#
#   --check     read-only: the space must hold the fc34580 shim (OLD) or this one
#   --install   writes, after the same checks; keeps a backup of OLD
# Never use this script on another storage layout or after remapping the file.
set -eu
umask 077
die() { echo "STOP: $*" >&2; exit 1; }
MODE=${1:---check}
case "$MODE" in
    --check|--install) ;;
    *) die 'Use --check or --install' ;;
esac
[ "$(id -u)" = 0 ] || die 'Root is required'
for cmd in dd sha256sum wc cp sync mktemp grep cat od tr rm; do
    command -v "$cmd" >/dev/null 2>&1 || die "Missing tool: $cmd"
done
DEVICE=/dev/block/sdc
START=121877958
COUNT=27649
BYTES=113250304
OLD=4240099e4cdda0b0e9ccfb9f67ff8c9c424a284c8255bc05a9eca252ace14eb9
NEW=fe66c0159f4579133dee4266f090c9c1ed39ea8a79b268c74cd8dd1f94179fd9
# the sweetgl.dylib from the sweetgl-llvmpipe artifact; the script pads it
# with zeros to the 27649 blocks (= NEW)
INPUT=/data/local/tmp/sweetgl-llvmpipe-9ee4913.dylib
DYLIB=12eb5f9f769e9708fefa9786be2d6898215167c672115a007c4703020b852e14
DYLIB_BYTES=51343136
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
WORK=$(mktemp -d /data/local/tmp/sweetgl-lp-check.XXXXXX) || die 'Cannot create staging directory'
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
sha_of() {
    digest=$(sha256sum "$1") || die "Cannot hash $1"
    echo "${digest%% *}"
}
hash_check() {
    [ "$(sha_of "$1")" = "$2" ] || die "SHA-256 mismatch: $1"
}
read_region() {
    # Direct I/O avoids satisfying verification from the block-device page cache.
    direct_dd if="$DEVICE" of="$3" bs=4096 skip="$1" count="$2" iflag=direct 2>/dev/null || die 'Read failed'
    length=$(wc -c < "$3")
    [ "$length" -eq "$(( $2 * 4096 ))" ] || die 'Short read'
}
hx() {
    # bytes of file $1 at offset $2, length $3, as lowercase hex without spaces
    od -A n -t x1 -v -j "$2" -N "$3" "$1" | tr -d ' \n'
}
expect_hex() {
    got=$(hx "$1" "$2" "$3") || die "Cannot read $5"
    [ "$got" = "$4" ] || die "Catalog check failed: $5 (found $got)"
}
# Catalog check, by field (every iOS boot rewrites dates and counts):
#   - volume header: HFSX 5, 4 KiB blocks, same size, the catalog file in
#     the same extents (so catalog node N is where it is read)
#   - catalog header node: 8 KiB nodes, 24576 of them, depth 4, root 20212,
#     node 19291 in use
#   - the index path root 20212 -> 275 -> 19391 -> 19291: each an index
#     node of the right height with a live record pointing to the next
#   - leaf 19291: its live records include .../ramdisk/arm64SURamDisk.dmg
#     exactly once, as file 315534, mode 0644, 113246235 bytes, one extent:
#     volume block 2164422 x 27649 = device block 121877958 x 27649
RD_KEY=002a0004cc72001200610072006d003600340053005500520061006d004400690073006b002e0064006d0067
RD_FORK=0000000006c0001b0000000000006c01002106c600006c010000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
CAT_FORK=000000000c000000020000000000c0000002cd820000400000033b050000200000114cc000002000001a626c00002000002e279600002000000000000000000000000000000000000000000000000000
live_offsets() {
    # live record offsets of node file $1 (from its offset table)
    n=$(hx "$1" 10 2) || die "Cannot read $1"
    n=$((0x$n))
    [ "$n" -ge 1 ] && [ "$n" -le 600 ] || die "Catalog check failed: $1 has $n records"
    i=0
    while [ "$i" -lt "$n" ]; do
        o=$(hx "$1" $((8190 - 2 * i)) 2) || die "Cannot read $1 offsets"
        echo $((0x$o))
        i=$((i + 1))
    done
}
check_index() {
    # $1 node file, $2 node number, $3 height (hex byte), $4 child node number
    expect_hex "$1" 8 2 "00$3" "node $2 kind and height (index, $3)"
    want=$(printf '%08x' "$4")
    hits=0
    for o in $(live_offsets "$1"); do
        [ "$o" -ge 14 ] && [ "$o" -le 8000 ] || continue
        kl=$(hx "$1" "$o" 2)
        kl=$((0x$kl))
        [ "$kl" -ge 6 ] && [ "$kl" -le 516 ] || continue
        [ "$(hx "$1" $((o + 2 + kl)) 4)" = "$want" ] && hits=$((hits + 1))
    done
    [ "$hits" -eq 1 ] || die "Catalog check failed: node $2 points to node $4 $hits times"
}
check_metadata() {
    read_region 119713536 1 "$WORK/vh.bin"
    read_region 119897218 2 "$WORK/catalog-node0.bin"
    read_region 121450324 2 "$WORK/catalog-node20212.bin"
    read_region 119897768 2 "$WORK/catalog-node275.bin"
    read_region 121448682 2 "$WORK/catalog-node19391.bin"
    read_region 121448482 2 "$WORK/catalog-node19291.bin"
    vh=$WORK/vh.bin node0=$WORK/catalog-node0.bin leaf=$WORK/catalog-node19291.bin
    expect_hex "$vh" 1024 4 48580005 'volume header signature and version (HFSX 5)'
    expect_hex "$vh" 1064 8 0000100000400000 'volume block size and block count'
    expect_hex "$vh" 1296 80 "$CAT_FORK" 'catalog file size and extents'
    expect_hex "$node0" 8 1 01 'catalog header node kind'
    expect_hex "$node0" 14 6 000400004ef4 'catalog depth 4, root node 20212'
    expect_hex "$node0" 32 2 2000 'catalog node size'
    expect_hex "$node0" 36 4 00006000 'catalog node count'
    expect_hex "$node0" 8186 2 00f8 'catalog map record offset'
    map=$(hx "$node0" 2659 1) || die 'Cannot read the catalog node map'
    [ $((0x$map & 16)) -ne 0 ] || die 'Catalog check failed: node 19291 is not in use'
    check_index "$WORK/catalog-node20212.bin" 20212 04 275
    check_index "$WORK/catalog-node275.bin" 275 03 19391
    check_index "$WORK/catalog-node19391.bin" 19391 02 19291
    expect_hex "$leaf" 8 2 ff01 'node 19291 kind and height (leaf)'
    found=''
    for o in $(live_offsets "$leaf"); do
        if [ "$o" -ge 14 ] && [ "$o" -le 7900 ] && [ "$(hx "$leaf" "$o" 44)" = "$RD_KEY" ]; then
            [ -z "$found" ] || die 'Catalog check failed: two arm64SURamDisk.dmg records in node 19291'
            found=$o
        fi
    done
    [ -n "$found" ] || die 'Catalog check failed: the arm64SURamDisk.dmg record is no longer in node 19291'
    expect_hex "$leaf" $((found + 44)) 2 0002 'arm64SURamDisk.dmg record type (file)'
    expect_hex "$leaf" $((found + 52)) 4 0004d08e 'arm64SURamDisk.dmg file ID'
    expect_hex "$leaf" $((found + 84)) 4 000081a4 'arm64SURamDisk.dmg mode (regular file, 0644)'
    expect_hex "$leaf" $((found + 132)) 80 "$RD_FORK" 'arm64SURamDisk.dmg size and allocation'
    echo "Catalog check passed: arm64SURamDisk.dmg is file 315534, 113246235 bytes at device block 121877958 x 27649 (node 19291 offset $found)."
}
[ -f "$INPUT" ] || die "Push the artifact's sweetgl.dylib to $INPUT first"
[ "$(wc -c < "$INPUT")" -eq "$DYLIB_BYTES" ] || die 'Wrong dylib size'
hash_check "$INPUT" "$DYLIB"
make_payload() {
    dd if=/dev/zero of="$1" bs=4096 count="$COUNT" 2>/dev/null || die 'Cannot make the payload'
    dd if="$INPUT" of="$1" bs=4096 conv=notrunc 2>/dev/null || die 'Cannot make the payload'
    [ "$(wc -c < "$1")" -eq "$BYTES" ] || die 'Wrong payload size'
    hash_check "$1" "$NEW"
}
check_metadata
read_region "$START" "$COUNT" "$WORK/before.bin"
CUR=$(sha_of "$WORK/before.bin")
[ "$CUR" != "$NEW" ] || die 'This shim is already installed there (current content = payload). Nothing to do.'
[ "$CUR" = "$OLD" ] || die "The space does not hold the fc34580 llvmpipe shim (now $CUR). No raw-device writes performed."
if [ "$MODE" = --check ]; then
    make_payload "$WORK/replacement.bin"
    rm -f "$WORK/replacement.bin"
    echo "Backup of the current content (fc34580 shim): $WORK/before.bin"
    echo 'CHECK PASSED: dylib, payload, current content, geometry and catalog path match.'
    echo 'No raw-device writes performed. To install, run:'
    echo "  sh $0 --install"
    exit 0
fi
make_payload "$WORK/replacement.bin"
echo "Writing exactly $COUNT blocks to sdc at block $START."
echo "Current content backup: $WORK/before.bin"
dd if="$WORK/replacement.bin" of="$DEVICE" bs=4096 seek="$START" count="$COUNT" conv=notrunc 2>/dev/null || die "WRITE FAILED: do not boot iOS with the llvmpipe args; backup (fc34580) is $WORK/before.bin"
sync || die 'Sync failed; do not boot iOS'
read_region "$START" "$COUNT" "$WORK/after.bin"
hash_check "$WORK/after.bin" "$NEW"
rm -f "$WORK/after.bin" "$WORK/replacement.bin"
check_metadata
echo 'INSTALL VERIFIED: the 9ee4913 llvmpipe shim (fast path, noise copies) is in place; catalog metadata unchanged.'
echo "Backup of the old content: $WORK/before.bin (keep it)"
echo 'No reboot performed.'
