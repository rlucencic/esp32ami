"""A boot-block ADF that tests interlace: a 1-plane hires interlaced screen,
one line repeated (BPL1MOD -40), white in the long frames and black in the
short ones (BPL1PT set at each vertical blank from VPOSR's LOF). Woven, the
picture's lines alternate white and black.
usage: laceboot.py out.adf"""
import struct, sys

def w(v): return struct.pack('>H', v & 0xFFFF)
def l(v): return struct.pack('>I', v & 0xFFFFFFFF)
def movew(imm, addr): return w(0x33FC) + w(imm) + l(addr)          # move.w #imm,addr.l

def build():
    c = b''
    c += movew(0x7FFF, 0xDFF09A) + movew(0x7FFF, 0xDFF096) + movew(0x7FFF, 0xDFF09C)
    lea_white = len(c)
    c += w(0x43FA) + w(0)                                          # lea white(pc),a1 (patched)
    lea_black = len(c)
    c += w(0x45FA) + w(0)                                          # lea black(pc),a2 (patched)
    for reg, v in ((0x100, 0x1204), (0x102, 0), (0x104, 0), (0x08E, 0x2C81), (0x090, 0x2CC1),
                   (0x092, 0x38), (0x094, 0xD0), (0x108, -40), (0x10A, -40), (0x180, 0x000), (0x182, 0xFFF)):
        c += movew(v, 0xDFF000 + reg)                              # BPLCON0: hires, 1 plane, LACE
    c += w(0x23C9) + l(0xDFF0E0)                                   # move.l a1,BPL1PT
    c += movew(0x8300, 0xDFF096)                                   # DMA: bitplanes

    def branch(op, target):                                        # bcc.s target
        return w(op | ((target - (len(c) + 2)) & 0xFF))

    loop = len(c)
    c += w(0x0839) + w(5) + l(0xDFF01F)                            # btst #5,INTREQR+1 (VERTB)
    c += branch(0x6700, loop)                                      # beq.s loop
    c += movew(0x0020, 0xDFF09C)                                   # clear VERTB
    c += w(0x3039) + l(0xDFF004)                                   # move.w VPOSR,d0
    long_frame = len(c) + 2 + 6 + 2
    c += branch(0x6B00, long_frame)                                # bmi.s long (LOF)
    c += w(0x23CA) + l(0xDFF0E0)                                   # move.l a2,BPL1PT
    c += branch(0x6000, loop)                                      # bra.s loop
    assert len(c) == long_frame
    c += w(0x23C9) + l(0xDFF0E0)                                   # long: move.l a1,BPL1PT
    c += branch(0x6000, loop)                                      # bra.s loop
    white = len(c)
    c += b'\xff' * 40
    black = len(c)
    c += b'\x00' * 40
    c = bytearray(c)
    c[lea_white + 2:lea_white + 4] = w(white - (lea_white + 2))
    c[lea_black + 2:lea_black + 4] = w(black - (lea_black + 2))
    return bytes(c)

def boot_block(code):
    bb = bytearray(1024)
    bb[0:4] = b'DOS\0'
    bb[8:12] = l(880)
    bb[12:12 + len(code)] = code
    s = 0
    for i in range(0, 1024, 4):
        s += struct.unpack('>I', bb[i:i + 4])[0]
        if s > 0xFFFFFFFF:
            s = (s & 0xFFFFFFFF) + 1
    bb[4:8] = l(~s)
    return bytes(bb)

if __name__ == '__main__':
    open(sys.argv[1], 'wb').write(boot_block(build()) + bytes(901120 - 1024))
