import re,sys,json
L=open(sys.argv[1],errors='replace').read().splitlines()
src={};cur=None
for l in L:
    m=re.search(r'unknown (fs|vs) (\d+) source',l)
    if m: cur=(m.group(1),int(m.group(2))); src[cur]=''; continue
    if cur is None: continue
    if l.startswith('|backboardd| sweetgl:   | '): src[cur]+=l[len('|backboardd| sweetgl:   | '):]+'\n'; continue
    if l.startswith('|backboardd| sweetgl:'): cur=None; continue
    if l.startswith('|backboardd| '): src[cur]+=l[len('|backboardd| '):]+'\n'; continue
    if l.startswith('|') or l.startswith('SWEET') or l.startswith('PROF') or l.startswith('WP210') or l.startswith('IOK') or l.startswith('UBT'): cur=None; continue
    src[cur]+=l+'\n'
json.dump({f'{k[0]}{k[1]}':v for k,v in src.items()},open(sys.argv[2],'w'))
