"""Boot-block ADFs that test CLXDAT: a 1-plane lores line repeated (BPL1MOD -40),
manual sprites, CLXCON as given; every CLXDAT read ORed into $7F000.
usage: clxboot.py out.adf clxcon plane_hex40bytes sprite... (sprite: n:hstart:data)"""
import struct, sys

def w(v): return struct.pack('>H', v & 0xFFFF)
def l(v): return struct.pack('>I', v & 0xFFFFFFFF)
def movew(imm, addr): return w(0x33FC) + w(imm) + l(addr)          # move.w #imm,addr.l

def build(clxcon, plane, sprites, con0=0x1200):
    c = b''
    c += movew(0x7FFF, 0xDFF09A) + movew(0x7FFF, 0xDFF096) + movew(0x7FFF, 0xDFF09C)
    lea_at = len(c)
    c += w(0x43FA) + w(0)                                          # lea plane(pc),a1 (patched)
    c += w(0x23C9) + l(0xDFF0E0)                                   # move.l a1,BPL1PT
    for reg, v in ((0x100, con0), (0x102, 0), (0x104, 0x24), (0x08E, 0x2C81), (0x090, 0x2CC1),
                   (0x092, 0x38), (0x094, 0xD0), (0x108, -len(plane)), (0x10A, -len(plane)), (0x098, clxcon),
                   (0x180, 0x000), (0x182, 0xFFF), (0x1A2, 0xF00), (0x1AA, 0x0F0)):
        c += movew(v, 0xDFF000 + reg)
    for n, hstart, data in sprites:
        base = 0xDFF140 + 8 * n
        c += movew(0x2C00 | (hstart >> 1), base) + movew(0xF000 | (hstart & 1), base + 2)
        c += movew(0, base + 6) + movew(data, base + 4)            # DATB, then DATA arms it
    c += movew(0x8300, 0xDFF096)                                   # DMA: bitplanes
    c += w(0x4279) + l(0x7F000)                                    # clr.w $7F000
    c += w(0x3039) + l(0xDFF00E)                                   # loop: move.w CLXDAT,d0
    c += w(0x8179) + l(0x7F000)                                    # or.w d0,$7F000
    c += w(0x60F2)                                                 # bra.s loop
    plane_at = len(c)
    c += plane
    # patch the LEA: displacement from the extension word (code at 12)
    disp = plane_at - (lea_at + 2)
    c = c[:lea_at + 2] + w(disp) + c[lea_at + 4:]
    bb = bytearray(1024)
    bb[0:4] = b'DOS\0'
    bb[8:12] = l(880)
    bb[12:12 + len(c)] = c
    s = 0
    for i in range(0, 1024, 4):
        s += struct.unpack('>I', bb[i:i + 4])[0]
        if s > 0xFFFFFFFF:
            s = (s & 0xFFFFFFFF) + 1
    bb[4:8] = l(~s)
    return bytes(bb) + bytes(901120 - 1024)

if __name__ == '__main__':
    out, clxcon, plane = sys.argv[1], int(sys.argv[2], 16), bytes.fromhex(sys.argv[3])
    assert len(plane) in (40, 80)
    sprites = [tuple(int(x, 0) for x in a.split(':')) for a in sys.argv[4:]]
    open(out, 'wb').write(build(clxcon, plane, sprites, 0x9200 if len(plane) == 80 else 0x1200))
