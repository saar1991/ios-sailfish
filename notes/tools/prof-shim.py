import re,sys,collections,bisect
log,base=sys.argv[1],int(sys.argv[2],16)
S=[(int(l.split()[0],16),l.split()[2]) for l in open('/home/claude/art-lp/syms.txt') if len(l.split())>=3]
A=[a for a,_ in S]; END=max(A)+0x10000
rounds=[];cur=None
for l in open(log,errors='ignore'):
    m=re.match(r'PROF backboardd\[\d+\] round (\d+) samples (\d+) kernel (\d+) of all (\d+)',l)
    if m: cur=collections.Counter(); rounds.append((int(m[1]),cur)); continue
    if l.startswith('PROF ') and cur is not None and 'round' not in l:
        for a,c in re.findall(r'([0-9a-f]+):(\d+)',l): cur[int(a,16)]+=int(c)
def name(a):
    if a>=0xfffffe0000000000 or a>=0xfffffff000000000: return 'KERNEL'
    if base<=a<base+END:
        i=bisect.bisect_right(A,a-base)-1; return S[i][1]
    if 0x180000000<=a<0x300000000: return 'SHAREDCACHE'
    return 'JIT/anon'
selfc=collections.Counter(); incl=collections.Counter(); cls=collections.Counter()
for n,h in rounds:
    for a,c in h.items():
        if a&3==0:
            nm=name(a); selfc[nm]+=c
            k=nm if nm in('KERNEL','SHAREDCACHE','JIT/anon') else 'shim'; cls[k]+=c
        elif a&3==1:
            incl[name(a&~3)]+=c
T=sum(selfc.values())
print('self samples (top-640 lists only)',T, {k:f'{v*100/T:.0f}%' for k,v in cls.most_common()})
for nm,c in selfc.most_common(25): print(f'  self {c*100/T:5.1f}% {nm[:100]}')
print('--- callers (inclusive-ish)')
for nm,c in incl.most_common(25): print(f'  in {c*100/T:6.1f}% {nm[:100]}')
