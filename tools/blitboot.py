"""A boot-block ADF that times the blitter against the display's DMA: for
no planes, 4 and 6 lores planes and 4 hires planes (the window 44-300,
DDFSTRT/STOP $38/$D0 lores, $3C/$D4 hires, no sprites, audio or disk DMA)
it waits for line 60, starts a D-only blit of 64 x 32 words (two colour
clocks a word: 4096 slots) and counts the lines until BBUSY drops, a word
each at $7F000. The bus slots a PAL line leaves the blitter: 227 colour
clocks less refresh's 4 and the fetch's words (lores 20 a plane, hires 40).
usage: blitboot.py out.adf
       blitboot.py --check dump.bin   (AMI_DUMP=7F000:8:dump.bin, ~600 frames)"""
import struct, sys

CASES = [  # BPLCON0, DDFSTRT, DDFSTOP, the fetch's slots a line
    (0x0200, 0x38, 0xD0, 0),
    (0x4200, 0x38, 0xD0, 4 * 20),
    (0x6200, 0x38, 0xD0, 6 * 20),
    (0xC200, 0x3C, 0xD4, 4 * 40),
]
SLOTS = 64 * 32 * 2

def w(v): return struct.pack('>H', v & 0xFFFF)
def l(v): return struct.pack('>I', v & 0xFFFFFFFF)
def movew(imm, addr): return w(0x33FC) + w(imm) + l(addr)          # move.w #imm,addr.l
def movel(imm, addr): return w(0x23FC) + l(imm) + l(addr)          # move.l #imm,addr.l

def build():
    c = b''
    for reg in (0x9A, 0x96, 0x9C):
        c += movew(0x7FFF, 0xDFF000 + reg)                         # interrupts, DMA, requests off
    c += w(0x47F9) + l(0x7F000)                                    # lea $7f000,a3
    lea_cases = len(c)
    c += w(0x45FA) + w(0)                                          # lea cases(pc),a2 (patched)
    c += w(0x7E00 | (len(CASES) - 1))                              # moveq #cases-1,d7
    case = len(c)
    c += w(0x33DA) + l(0xDFF100)                                   # move.w (a2)+,BPLCON0
    c += w(0x33DA) + l(0xDFF092)                                   # move.w (a2)+,DDFSTRT
    c += w(0x33DA) + l(0xDFF094)                                   # move.w (a2)+,DDFSTOP
    for reg, v in ((0x08E, 0x2C81), (0x090, 0x2CC1), (0x108, 0), (0x10A, 0)):
        c += movew(v, 0xDFF000 + reg)                              # the window, modulos
    c += movew(0x8340, 0xDFF096)                                   # DMA: DMAEN, BPLEN, BLTEN

    def branch(op, target):                                        # bcc.s target
        return w(op | ((target - (len(c) + 2)) & 0xFF))

    wait = len(c)
    c += w(0x3239) + l(0xDFF006)                                   # move.w VHPOSR,d1
    c += w(0xE049)                                                 # lsr.w #8,d1
    c += w(0x0C41) + w(60)                                         # cmpi.w #60,d1
    c += branch(0x6600, wait)                                      # bne.s wait
    c += movew(0x0100, 0xDFF040) + movew(0, 0xDFF042)              # BLTCON0: D, minterm 0; BLTCON1
    c += movel(0x30000, 0xDFF054) + movew(0, 0xDFF066)             # BLTDPT, BLTDMOD
    c += w(0x3439) + l(0xDFF006)                                   # move.w VHPOSR,d2
    c += movew(64 << 6 | 32, 0xDFF058)                             # BLTSIZE: 64 lines of 32 words
    c += w(0x4A79) + l(0xDFF002)                                   # tst.w DMACONR (first access)
    busy = len(c)
    c += w(0x0839) + w(6) + l(0xDFF002)                            # btst #6,DMACONR (BBUSY)
    c += branch(0x6600, busy)                                      # bne.s busy
    c += w(0x3639) + l(0xDFF006)                                   # move.w VHPOSR,d3
    c += w(0xE04A) + w(0xE04B)                                     # lsr.w #8,d2; lsr.w #8,d3
    c += w(0x9642) + w(0x0243) + w(0xFF)                           # sub.w d2,d3; andi.w #$ff,d3
    c += w(0x36C3)                                                 # move.w d3,(a3)+
    c += movew(0x0100, 0xDFF096)                                   # BPLEN off
    c += w(0x51CF) + w(case - (len(c) + 2))                        # dbra d7,case
    c += w(0x60FE)                                                 # bra.s * (done)
    cases = len(c)
    for con0, start, stop, _ in CASES:
        c += w(con0) + w(start) + w(stop)
    c = bytearray(c)
    c[lea_cases + 2:lea_cases + 4] = w(cases - (lea_cases + 2))
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

def check(path):
    got = struct.unpack('>%dH' % len(CASES), open(path, 'rb').read()[:2 * len(CASES)])
    names = ['no planes', '4 lores planes', '6 lores planes', '4 hires planes']
    for (_, _, _, fetch), name, lines in zip(CASES, names, got):
        free = 227 - 4 - fetch
        print('%-15s %3d free slots a line: %5.1f lines expected, %3d measured' % (name, free, SLOTS / free, lines))

if __name__ == '__main__':
    if sys.argv[1] == '--check':
        check(sys.argv[2])
    else:
        open(sys.argv[1], 'wb').write(boot_block(build()) + bytes(901120 - 1024))
