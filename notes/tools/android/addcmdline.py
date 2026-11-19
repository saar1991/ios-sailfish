#!/usr/bin/env python3
"""Append text to the kernel command line of a boot image (header v4), for `fastboot boot` only.
usage: addcmdline.py IN.img OUT.img "extra args" """
import struct, sys
b = bytearray(open(sys.argv[1], 'rb').read())
assert b[:8] == b'ANDROID!', 'not an Android boot image'
v = struct.unpack_from('<I', b, 40)[0]
assert v in (3, 4), f'header v{v}: only v3/v4 handled'
old = bytes(b[44:44 + 1536]).split(b'\0')[0]
new = (old + b' ' if old else b'') + sys.argv[3].encode()
assert len(new) < 1536, f'cmdline too long ({len(new)})'
b[44:44 + 1536] = new + b'\0' * (1536 - len(new))
open(sys.argv[2], 'wb').write(b)
print(f'header v{v}; cmdline was [{old.decode()}]\nnow [{new.decode()}]\nwrote {sys.argv[2]}')
