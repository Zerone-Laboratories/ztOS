#!/bin/bash
# fetch.sh NAME: bring /tmp/ztrec from the phone into build/NAME and make build/NAME.png
set -e
cd "$(dirname "$0")/../.."
N=$1; mkdir -p build/$N
(timeout 60 socat -u "TCP-LISTEN:5596,bind=172.16.42.2,reuseaddr" "OPEN:build/$N.tar,creat,trunc" &); sleep 1
timeout 60 scripts/ssh-pw.py alarm "${ZT_PW:-alarm}" 172.16.42.1 'cd /tmp/ztrec && tar cf - . > /dev/tcp/172.16.42.2/5596' >/dev/null 2>&1 || true
sleep 1; tar xf build/$N.tar -C build/$N 2>/dev/null || true
python3 - "$N" <<'PY'
import sys,glob,os
from PIL import ImageFile; ImageFile.LOAD_TRUNCATED_IMAGES=True
from PIL import Image,ImageDraw,ImageEnhance
N=sys.argv[1]
fs=sorted(glob.glob('build/%s/*.ppm'%N));ts=[int(os.path.basename(f)[:-4]) for f in fs]
# bursts of frames (animations), split where nothing was drawn for a while
b=[[0]]
for i in range(len(ts)-1):
  if ts[i+1]-ts[i]>600: b.append([])
  b[-1].append(i+1)
print('bursts:',[(len(x),ts[x[-1]]-ts[x[0]]) for x in b])
big=[x for x in b if len(x)>5]
idx=big[-1]; t0=ts[idx[0]]
ims=[]
for i in idx:
  im=ImageEnhance.Brightness(Image.open(fs[i]).convert('RGB')).enhance(2.0).resize((180,370))
  d=ImageDraw.Draw(im);d.text((3,3),'+%d'%(ts[i]-t0),fill=(0,255,255));ims.append(im)
w,h=ims[0].size;n=10;c=Image.new('RGB',(w*n,h*((len(ims)+n-1)//n)),(60,60,60))
for j,im in enumerate(ims): c.paste(im,((j%n)*w,(j//n)*h))
c.save('build/%s.png'%N);print(len(fs),'frames; return:',len(ims),'frames over',ts[idx[-1]]-t0,'ms')
PY
