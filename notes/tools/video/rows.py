import numpy as np, sys, subprocess
V=sys.argv[1]; W,H=478,850
p=subprocess.Popen(['ffmpeg','-v','error','-i',V,'-vf','format=gray','-f','rawvideo','-'],stdout=subprocess.PIPE)
win=[];idx=0;res=[]
fs=W*H
while True:
    b=p.stdout.read(fs)
    if len(b)<fs: break
    win.append(np.frombuffer(b,np.uint8).reshape(H,W).astype(np.int16))
    if len(win)>5: win.pop(0)
    if len(win)==5:
        c=win[2]; med=np.median(np.stack([win[0],win[1],win[3],win[4]]),axis=0)
        d=c-med
        rowm=d.mean(axis=1)            # signed per row
        rowa=np.abs(d).mean(axis=1)
        base=np.median(rowa)
        # best narrow band (1..12 rows) by abs
        k=np.argmax(rowa)
        res.append((idx-2,float(rowa.max()-base),int(k),float(rowm[k]),float(np.abs(d).mean())))
    idx+=1
np.save('rows.npy',np.array(res))
res.sort(key=lambda r:-r[1])
for r in res[:50]: print(f"f{r[0]:5d} t={r[0]/60:7.2f}s rowpeak {r[1]:6.2f} row {r[2]:3d} signed {r[3]:7.2f} whole {r[4]:5.2f}")
