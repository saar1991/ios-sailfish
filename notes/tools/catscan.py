import struct, sys
cat = open(sys.argv[1],'rb').read()
NS = 8192
hdr = cat[:NS]
# header record at 14
(depth, root, leafRecords, firstLeaf, lastLeaf, nodeSize, maxKeyLen, totalNodes, freeNodes) = struct.unpack('>HIIIIHHII', cat[14:14+30])
folders = {}  # cnid -> (parent, name)
files = []
n = firstLeaf
seen = 0
while n and seen < totalNodes:
    seen += 1
    node = cat[n*NS:(n+1)*NS]
    if len(node) < NS: break
    flink, blink, kind, height, nrec = struct.unpack('>IIbBH', node[:12])
    offs = [struct.unpack('>H', node[NS-2*(i+1):NS-2*i])[0] for i in range(nrec)]
    for o in offs:
        klen = struct.unpack('>H', node[o:o+2])[0]
        parent = struct.unpack('>I', node[o+2:o+6])[0]
        nlen = struct.unpack('>H', node[o+6:o+8])[0]
        name = node[o+8:o+8+2*nlen].decode('utf-16-be', 'replace')
        r = o + 2 + klen
        rtype = struct.unpack('>h', node[r:r+2])[0]
        if rtype == 1:  # folder
            cnid = struct.unpack('>I', node[r+8:r+12])[0]
            folders[cnid] = (parent, name)
        elif rtype == 2:  # file
            cnid = struct.unpack('>I', node[r+8:r+12])[0]
            # bsdInfo at r+32: ownerID4 groupID4 adminFlags1 ownerFlags1 fileMode2 special4
            adminf, ownerf, mode = struct.unpack('>BBH', node[r+40:r+44])
            df = r + 88
            lsize, clump, tblocks = struct.unpack('>QII', node[df:df+16])
            ext = [struct.unpack('>II', node[df+16+8*i:df+24+8*i]) for i in range(8)]
            ext = [e for e in ext if e[1]]
            rf = df + 80
            rsize = struct.unpack('>Q', node[rf:rf+8])[0]
            files.append((lsize, tblocks, cnid, parent, name, ownerf, mode, ext, rsize))
    n = flink
def path(cnid):
    parts = []
    while cnid in folders and cnid != 2:
        p, nm = folders[cnid]
        parts.append(nm); cnid = p
    return '/' + '/'.join(reversed(parts))
print('files', len(files), 'folders', len(folders))
files.sort(key=lambda f: -f[1])
k = 0
for f in files:
    lsize, tblocks, cnid, parent, name, ownerf, mode, ext, rsize = f
    comp = bool(ownerf & 0x20)
    if comp or lsize < 40*2**20: continue
    k += 1
    print(f'{lsize/2**20:7.1f} MB blocks {tblocks} extents {len(ext)} first {ext[0] if ext else None} {path(parent)}/{name} id {cnid}')
    if k >= 40: break
