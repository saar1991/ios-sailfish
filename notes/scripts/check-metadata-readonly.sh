#!/system/bin/sh
# READ-ONLY. Compares the nine iOS catalog blocks with the expected hashes
# used by install-sweetgl-diag-0x502.sh and prints MATCH or DIFFERS for each.
# It never writes to /dev/block. Scratch files go in a new directory under
# /data/local/tmp, which is kept for review.
umask 077
DEVICE=/dev/block/sdc
WORK=$(mktemp -d /data/local/tmp/meta-check.XXXXXX) || { echo "STOP: cannot create directory"; exit 1; }
echo "Scratch directory (kept): $WORK"
if dd if="$DEVICE" of="$WORK/probe.bin" bs=4096 skip=119713536 count=1 iflag=direct 2>/dev/null; then
    DD="dd"
else
    DD="/data/adb/magisk/busybox dd"
fi
echo "Direct reads with: $DD"
MATCH=0
DIFF=0
while read -r block count expected; do
    if ! $DD if="$DEVICE" of="$WORK/block.bin" bs=4096 skip="$block" count="$count" iflag=direct 2>/dev/null; then
        echo "$block READ FAILED"
        DIFF=$((DIFF + 1))
        continue
    fi
    got=$(sha256sum "$WORK/block.bin" | cut -d' ' -f1)
    if [ "$got" = "$expected" ]; then
        echo "$block MATCH"
        MATCH=$((MATCH + 1))
    else
        echo "$block DIFFERS now=$got expected=$expected"
        DIFF=$((DIFF + 1))
    fi
done <<'METADATA'
119713536 1 1c7407f21c9a55e2678ca2e882c304c5996eae872578be74465134924116ff4a
119897218 1 2d186c4596cfacf0989dc08b50b1771b80031ee9f26d48a5e10bb4fb5d7be55a
121450324 2 467d21737003d9850f6397a6ae3a7c89bb6970b2fcd6d394c6e26b7e5c15ddfc
121450322 2 2380f94acf92bae547722bad5f85a53b66a5dab6f3d7f5467b804d195f005770
119897220 6 44dc184e97fc36d74b1ff400d12b7a458fb6f37a8da76f800257573a59b18465
121448304 2 f1374e3dd8df716a89ae4db945775ce3db98c7c18cb6a95e711f5fce7eab2963
119897768 2 f13f5455632a038e7b3012d797de2175a6c025568e29c0841dec03237813ce5a
121448452 2 4a857befe9e3a12b0db676f885c76d0c8f6c5003ccf518f9e00116feb3e41d53
121448308 2 9383f50bf70110fd49cfe412c8d42039890cc89d9fbda54feab2c10b0b082bfb
METADATA
echo "Summary: $MATCH match, $DIFF differ or failed (of 9)"
echo "No writes were made to the device."
