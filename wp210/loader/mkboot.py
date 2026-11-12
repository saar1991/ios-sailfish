#!/usr/bin/env python3
"""Pack a boot image header v4 (same layout as stock WP210 Pro boot.img).
usage: mkboot.py KERNEL STOCK_BOOT.img OUT.img   (reuses the stock ramdisk)"""
import struct,sys
PG=4096
def pad(b): return b+b'\0'*((-len(b))%PG)
k=open(sys.argv[1],'rb').read(); s=open(sys.argv[2],'rb').read()
assert s[:8]==b'ANDROID!' and struct.unpack_from('<I',s,40)[0]==4, 'stock image must be v4'
ks,rs,osv=struct.unpack_from('<III',s,8)
rd=s[PG+((ks+PG-1)//PG)*PG:][:rs]
h=struct.pack('<8sIII I 4I I',b'ANDROID!',len(k),len(rd),osv,1584,0,0,0,0,4)
h+=b'\0'*1536+struct.pack('<I',0)   # cmdline, signature_size
open(sys.argv[3],'wb').write(pad(h)+pad(k)+pad(rd))
print('wrote',sys.argv[3])
