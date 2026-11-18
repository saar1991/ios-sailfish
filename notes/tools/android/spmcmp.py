import re,sys
names={int(l.split()[0],16):l.split()[1] for l in open('/home/claude/notes-wt/notes/tools/android/mt6895-spm-regs.txt')}
a={}
for l in open('/home/claude/notes-wt/notes/firmware/android-kp2-dvfsrc-spm-regs.txt'):
    for o,v in re.findall(r'\bs([0-9a-f]{3})=0x([0-9a-f]+)',l): a[int(o,16)]=int(v,16)
log=open(sys.argv[1],errors='ignore').read()
for tag in ('before-INIT','after-INIT','after-START'):
    ours={}
    for l in log.splitlines():
        if f'SWEET SPMALL: {tag}' in l:
            for o,v in re.findall(r' ([0-9a-f]{3})=([0-9a-f]+)',l.split(tag,1)[1]): ours[int(o,16)]=int(v,16)
    if not ours: continue
    d=[o for o in sorted(names) if o in ours and o in a and ours[o]!=a[o]]
    print(f'== {tag}: {len(ours)} read, {len(d)} differ from Android')
    if tag!='before-INIT':
        for o in d: print(f'  {o:03x} {names[o]:32s} android {a[o]:#010x}  ours {ours[o]:#010x}')
