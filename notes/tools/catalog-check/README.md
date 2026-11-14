# Field-level catalog check (install scripts from eed85c5 on)

`newcheck.sh` is the check built into `install-sweetgl-diag-eed85c5.sh`
(`check_catalog_files`). It replaces the whole-block hashes of the volume
header and the first catalog nodes, which every iOS boot rewrites, with the
fields the write depends on:

- volume header: HFSX 5, 4096-byte blocks, 4194304 blocks, catalog file
  in extents 183682x16384, 211717x8192, 1133760x8192, 1729132x8192 (so node
  19204 is device blocks 121448308-121448309), and since some boot before
  18:21 on 2026-11-14 a fifth, 3024790x8192: 201326592 bytes. A growing
  catalog stops the check; update CAT_FORK and the node count after
  confirming the first four extents are unchanged.
- catalog header node: node size 8192, 24576 nodes (20480 before the growth), map record at 248, the
  map bit of node 19204 set (in use)
- node 19204: leaf (kind -1, height 1); among its live records (offset
  table) the key parent 313920 + "sweetgl.dylib" exactly once; record type
  file, file ID 318814, data fork 17208704 bytes, 4202 blocks, one extent
  at volume block 3015861 (device 122729397)
- index nodes 20212, 20211, 19202, 19276 (device blocks 121450324,
  121450322, 121448304, 121448452): whole-node hashes, unchanged all day

Test (on a PC, with dumps of the volume header, catalog node 0 and node
19204): `sh testcheck.sh vh.bin node0.bin node19204.bin`. It was tested on
the dumps from 00:09, 09:44 and 10:18 (pass) and on broken copies (size,
extent, a stale record that is not at a live offset, node kind, map bit,
catalog extents: all stop). The full install script was run on a sparse
simulated sdc (real volume header and catalog, 14f179d as the installed
shim): --check, a simulated boot (dates and counts changed), --install
verified; it refused to write when the allocation was already new, the
record's extent moved, an index node changed, or the block size changed.
