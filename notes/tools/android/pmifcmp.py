#!/usr/bin/env python3
"""Compare stock Android's PMIF/SPMI-master registers (kp6.txt) with a sweetlog's
'SWEET PMIF:' / 'SWEET SPMIMST:' lines (patch 0247).  usage: pmifcmp.py kp6.txt sweetlog.txt"""
import re, sys
def android(path):
    pm, sm = {}, {}
    sect = None
    for l in open(path, errors='replace'):
        if l.startswith('== mt6xxx_pmif_arb field 00'): sect = pm
        elif l.startswith('== mt6xxx_pmif_arb field 10'): sect = sm
        elif l.startswith('mt6xxx_pmif_arb.') and sect is not None:
            for o, v in re.findall(r'r([0-9a-f]{3})=0x([0-9a-f]+)', l):
                sect[int(o, 16)] = int(v, 16)
    return pm, sm
def ours(path):
    d = {}
    for l in open(path, errors='replace'):
        m = re.search(r'SWEET (PMIF|SPMIMST): (\S+) ([0-9a-f]{3})((?: [0-9a-f]+)+)', l)
        if not m: continue
        blk, tag, base = m.group(1), m.group(2), int(m.group(3), 16)
        for i, v in enumerate(m.group(4).split()):
            d.setdefault(tag, {}).setdefault(blk, {})[base + 4 * i] = int(v, 16)
    return d
apm, asm = android(sys.argv[1])
for tag, blks in ours(sys.argv[2]).items():
    for blk, a in (('PMIF', apm), ('SPMIMST', asm)):
        o = blks.get(blk, {})
        diff = [k for k in sorted(set(a) & set(o)) if a[k] != o[k]]
        print(f'== {blk} {tag}: {len(diff)} of {len(set(a) & set(o))} words differ')
        for k in diff:
            print(f'  {k:03x}  android {a[k]:#010x}  ours {o[k]:#010x}')
