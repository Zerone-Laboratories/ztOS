import mmap, os, sys, struct
base=int(sys.argv[1],16); size=int(sys.argv[2],16); out=sys.argv[3]
fd=os.open('/dev/mem', os.O_RDONLY|os.O_SYNC)
m=mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ, offset=base)
mv=memoryview(m).cast('I')
data=struct.pack('<%dI'%(size//4), *[mv[i] for i in range(size//4)])
open(out,'wb').write(data); print(len(data))
