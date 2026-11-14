# Catalog check, by field instead of whole-block hashes: every iOS boot
# rewrites the volume header and the first catalog nodes (dates, counts),
# which made the block hashes go stale after each boot. What the write
# depends on is checked directly:
#   - volume header: HFSX version 5, 4 KiB blocks, same size, the catalog
#     file in the same four extents (so catalog node N is where we read it)
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
CAT_FORK=000000000a000000020000000000a0000002cd820000400000033b050000200000114cc000002000001a626c000020000000000000000000000000000000000000000000000000000000000000000000
check_catalog_files() {
    vh=$1 node0=$2 leaf=$3
    expect_hex "$vh" 1024 4 48580005 'volume header signature and version (HFSX 5)'
    expect_hex "$vh" 1064 8 0000100000400000 'volume block size and block count'
    expect_hex "$vh" 1296 80 "$CAT_FORK" 'catalog file size and extents'
    expect_hex "$node0" 8 1 01 'catalog header node kind'
    expect_hex "$node0" 32 2 2000 'catalog node size'
    expect_hex "$node0" 36 4 00005000 'catalog node count'
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
