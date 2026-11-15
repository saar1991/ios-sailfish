import numpy as np, sys, subprocess
V=sys.argv[1]; W,H=478,850; t0=float(sys.argv[2])
p=subprocess.Popen(['ffmpeg','-v','error','-ss',str(t0),'-i',V,'-vf','format=gray','-f','rawvideo','-'],stdout=subprocess.PIPE)
fs=W*H; win=[]; idx=0; out=[]
while True:
    b=p.stdout.read(fs)
    if len(b)<fs: break
    win.append(np.frombuffer(b,np.uint8).reshape(H,W)[:,60:410].astype(np.int16))
    if len(win)>3: win.pop(0)
    if len(win)==3:
        a,c,n=win
        # pixel differs from both neighbours in the same direction while neighbours agree
        dp=c-a; dn=c-n; agree=np.abs(a-n)<12
        m=(np.abs(dp)>18)&(np.abs(dn)>18)&agree
        f=m.mean(axis=1)              # fraction per row
        # band score: max over rows of f[r] minus mean of f at r-12..r-6 and r+6..r+12
        k=np.convolve(f,np.ones(3)/3,'same')
        ctx=np.maximum(np.roll(k,9),np.roll(k,-9))
        s=k-ctx
        r=int(np.argmax(s))
        out.append((t0+(idx-1)/60, float(s[r]), r, float(f[max(r-3,0):r+4].max())))
    idx+=1
np.save('bands.npy',np.array(out))
