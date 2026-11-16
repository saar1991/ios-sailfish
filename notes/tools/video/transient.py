import numpy as np, sys, subprocess
# one-frame transients: frame c differs from both sides while the sides agree
V=sys.argv[1]; t0=float(sys.argv[2]) if len(sys.argv)>2 else 0.0; W,H=240,426
p=subprocess.Popen(['ffmpeg','-nostdin','-v','error','-ss',str(t0),'-i',V,'-vf',f'scale={W}:{H},format=gray','-f','rawvideo','-'],stdout=subprocess.PIPE)
fs=W*H; win=[]; idx=0; out=[]
while True:
    b=p.stdout.read(fs)
    if len(b)<fs: break
    win.append(np.frombuffer(b,np.uint8).reshape(H,W).astype(np.int16))
    if len(win)>5: win.pop(0)
    if len(win)==5:
        c=win[2]; a=win[1]; z=win[3]; aa=win[0]; zz=win[4]
        m=(np.abs(c-a)>40)&(np.abs(c-z)>40)&(np.abs(a-z)<20)&(np.abs(aa-zz)<30)
        k=m[:H//6*6,:W//6*6].reshape(H//6,6,W//6,6).mean(axis=(1,3))
        out.append((t0+(idx-2)/60, float(m.mean()), float((k>0.5).sum()), int(np.argmax(k.max(axis=1))*6), int(np.argmax(k.max(axis=0))*6)))
    idx+=1
o=sorted(out,key=lambda r:-r[2])
for r in o[:25]: print(f't={r[0]:7.2f} frac {r[1]:.4f} blocks {int(r[2])} at y {r[3]} x {r[4]}')
