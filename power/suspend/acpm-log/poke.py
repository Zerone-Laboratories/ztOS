import mmap, os, sys
a=int(sys.argv[1],16); fd=os.open('/dev/mem', os.O_RDWR|os.O_SYNC)
m=mmap.mmap(fd,0x1000,mmap.MAP_SHARED,mmap.PROT_READ|mmap.PROT_WRITE,offset=a&~0xfff)
mv=memoryview(m).cast('I'); i=(a&0xfff)//4
if len(sys.argv)>2:
    for k,v in enumerate(sys.argv[2:]): mv[i+k]=int(v,16)
print([hex(mv[i+k]) for k in range(4)])
