"""acpmlogptr.py get | set ADDR | dump [ADDR]  -- ACPM flexpmu sleep-log pointer (runtime 0x15bb8)."""
import mmap, os, struct, sys
SRAM=0x02039000
fd=os.open('/dev/mem', os.O_RDWR|os.O_SYNC)
def m32(phys, write=None):
    page=phys&~0xfff
    m=mmap.mmap(fd,0x1000,mmap.MAP_SHARED,mmap.PROT_READ|mmap.PROT_WRITE,offset=page)
    mv=memoryview(m).cast('I'); i=(phys-page)//4
    if write is not None: mv[i]=write
    v=mv[i]; mv.release(); m.close(); return v
cmd=sys.argv[1]
if cmd=='get':
    print(hex(m32(SRAM+0x15bb8)), 'off', hex(m32(SRAM+0x15bb4)), 'cpu_stat+0x1fc', hex(m32(SRAM+0x16800+0x1fc)), 'mifup', hex(m32(SRAM+0x16818)))
elif cmd=='set':
    a=int(sys.argv[2],16); print('old', hex(m32(SRAM+0x15bb8))); m32(SRAM+0x15bb8, a); print('new', hex(m32(SRAM+0x15bb8)))
elif cmd=='clear':
    a=int(sys.argv[2],16)
    for k in range(0,0x820,4): m32(a+k,0)
    print('cleared')
elif cmd=='dump':
    a=int(sys.argv[2],16)
    idx=m32(a); print('index', idx)
    for n in range(128):
        e=a+16*(n+1); h=m32(e); s=struct.pack('<II',m32(e+4),m32(e+8)); v=m32(e+12)
        if h or v or any(s): print(f'{n:3d} hdr={h:08x} {s.decode("latin1").rstrip(chr(0))!r:12} val={v:#x}')
