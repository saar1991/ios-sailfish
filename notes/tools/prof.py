import re,collections,bisect,struct,sys
# usage: python3 prof.py sweetlog.txt sweetgl.unstripped.dylib syms.txt
# syms.txt: nm -n sweetgl.unstripped.dylib (local symbols are needed, so not the stripped file)
# Resolves the sweetprof PC samples that fall inside the shim (base found by matching BL call sites).
logf=sys.argv[1]
d=open(sys.argv[2],'rb').read()
syms=[]
for l in open(sys.argv[3]):
    a,t,n=l.split(None,2); syms.append((int(a,16),n.strip()))
addrs=[a for a,_ in syms]
def sym(off):
    i=bisect.bisect_right(addrs,off)-1
    return syms[i][1] if i>=0 else '?'
lines=open(logf,errors='replace').read().splitlines()
cur=None; leaf=collections.defaultdict(collections.Counter); call=collections.defaultdict(collections.Counter)
for l in lines:
    m=re.search(r'PROF (\w+)\[\d+\] round',l)
    if m: cur=m.group(1); continue
    if l.startswith('PROF ') and cur:
        for pc,n in re.findall(r'([0-9a-f]{6,}):(\d+)',l):
            pc=int(pc,16)
            (call if pc&3 else leaf)[cur][pc&~3]+=int(n)
bb_call=[pc for pc in call['backboardd'] if 0x102000000<=pc<0x107000000]
def isbl(w): return (w>>26)==0b100101 or (w & 0xFFFFFC1F)==0xD63F0000
best=[]
for b in range(0x102000000, 0x106000000, 0x4000):
    ok=0
    for pc in bb_call:
        o=pc-b-4
        if 0x4000<=o<0x620abc and isbl(struct.unpack('<I',d[o:o+4])[0]): ok+=1
    if ok: best.append((ok,b))
best.sort(reverse=True)
print('callers',len(bb_call),'best bases', [(hex(b),ok) for ok,b in best[:4]])
B=best[0][1]
for proc in ['backboardd','SpringBoard']:
    L=leaf[proc]; tot=sum(L.values())
    ins=sum(n for pc,n in L.items() if B<=pc<B+0xe84000)
    print(f'{proc}: leaf samples listed {tot}; in sweetgl.dylib {ins} ({100*ins/max(tot,1):.0f}%)')
tot=sum(leaf['backboardd'].values())
fn=collections.Counter()
for pc,n in leaf['backboardd'].items():
    if B<=pc<B+0xe84000: fn[sym(pc-B)]+=n
print('\nbackboardd self time in sweetgl (% of listed leaf samples):')
for f,n in fn.most_common(20): print(f'{100*n/tot:6.1f}%  {f}')
inc=collections.Counter()
for pc,n in call['backboardd'].items():
    if B<=pc<B+0xe84000: inc[sym(pc-B)]+=n
print('\nbackboardd callers in sweetgl (inclusive, % of listed leaf samples):')
for f,n in inc.most_common(25): print(f'{100*n/tot:6.1f}%  {f}')
