import mmap, os, struct
fd=os.open('/dev/mem', os.O_RDONLY|os.O_SYNC)
m=mmap.mmap(fd,0x1000,mmap.MAP_SHARED,mmap.PROT_READ,offset=0x0204f000)
w=lambda o: struct.unpack_from('<I',m,o)[0]
base=0x800
def line(d): return [w(base+16*d+8), w(base+16*d+12)]
names={0:'cpu_status',1:'seq_status',2:'cur_seq0',3:'cur_seq1',10:'sw_flag',16:'soc_count',17:'mif_count',21:'mif_always_on',22:'sleep_ap',23:'sleep_mif',24:'sicd_ap',25:'sicd_mif',26:'cur_pmd'}
for d,n in names.items(): print(n, [hex(x) for x in line(d)])
