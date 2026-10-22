"""A boot-block ADF that tests ECS super-hires, and a check of the host
build's picture of it (AMI_MODEL=2 AMI_ECS=1, ~400 frames). The copper
draws bands of 10 lines in one frame, two planes of a 32-pixel pattern
(all 16 pairs of 2-bit pixels) under a palette graphics.library would
never set:
  lores, hires (pixels 4 and 2 picture pixels wide, 12-bit colours);
  super-hires (pairs a, b: register 4b + a, a its upper gun bits, b the
  lower ones), BPLCON1 3 (12 pixels later), dual playfield (as one);
  sprite 0 over super-hires (colours by pairs from registers 16-31), with
  SPRxCTL bit 4 (2 pixels later), sprite 4 (not shown in super-hires),
  both over lores (12-bit colours).
usage: shresboot.py out.adf
       shresboot.py check picture.ppm   (the host's, or the board's screenshot as PPM)"""
import struct, sys

COP, PLANES = 0x20000, 0x30000
H = 0xA0                    # the sprites' HSTART (lores); sprite 4 48 later
PAIRS = [(a, b) for b in range(4) for a in range(4)]
PIXELS = [v for pair in PAIRS for v in pair]                       # 32 pixel values
COLORS = [(i * 0x5A3 ^ 0x3C6) & 0xFFF for i in range(16)] + [(i * 0x2D7 ^ 0x9A5) & 0xFFF for i in range(16)]
COLORS[21] = 0xC3C                                                 # sprite pairs (1, 1): magenta then green
BANDS = [   # first line, BPLCON0, BPLCON1, modulo, {sprite: (hstart, SH1)}
    (50, 0x2200, 0x00, -40, {}),                                   # lores
    (60, 0xA200, 0x00, -80, {}),                                   # hires
    (70, 0x2240, 0x00, -160, {}),                                  # super-hires
    (80, 0x2240, 0x33, -160, {}),                                  # scrolled by 3
    (90, 0x2640, 0x00, -160, {}),                                  # dual playfield
    (100, 0x2240, 0x00, -160, {0: (H, 0), 4: (H + 48, 0)}),
    (110, 0x2240, 0x00, -160, {0: (H, 1), 4: (H + 48, 0)}),
    (120, 0x2200, 0x00, -40, {0: (H, 0), 4: (H + 48, 0)}),
    (130, 0x0200, 0x00, -40, {}),
]

def w(v): return struct.pack('>H', v & 0xFFFF)
def l(v): return struct.pack('>I', v & 0xFFFFFFFF)
def movew(imm, addr): return w(0x33FC) + w(imm) + l(addr)          # move.w #imm,addr.l

def plane_long(p):
    v = 0
    for x in PIXELS:
        v = v << 1 | (x >> p) & 1
    return v

def copper():
    c = b''.join(w(0x180 + 2 * i) + w(v) for i, v in enumerate(COLORS))
    regs, armed = {}, {}
    for line, con0, con1, mod, sprites in BANDS:
        c += w((line - 1) << 8 | 0xE1) + w(0xFFFE)                 # wait for the line before's end
        for reg, v in ((0x100, con0), (0x102, con1), (0x108, mod), (0x10A, mod)):
            if regs.get(reg) != v:
                c += w(reg) + w(v)
                regs[reg] = v
        for n in sorted(set(armed) | set(sprites)):
            base = 0x140 + 8 * n
            if n not in sprites:
                c += w(base + 2) + w(0)                            # CTL: disarmed
                del armed[n]
            elif armed.get(n) != sprites[n]:
                h, sh1 = sprites[n]
                c += w(base) + w(line << 8 | h >> 1) + w(base + 2) + w(h & 1 | sh1 << 4)
                c += w(base + 6) + w(0) + w(base + 4) + w(0xFFFF)  # DATB, then DATA arms it
                armed[n] = sprites[n]
    return c + w(0xFFFF) + w(0xFFFE)

def build():
    c = movew(0x7FFF, 0xDFF09A) + movew(0x7FFF, 0xDFF096) + movew(0x7FFF, 0xDFF09C)
    for p in range(2):
        c += w(0x41F9) + l(PLANES + 0x100 * p)                     # lea plane,a0
        c += w(0x303C) + w(39)                                     # move.w #39,d0: 160 bytes
        c += w(0x20FC) + l(plane_long(p)) + w(0x51C8) + w(0xFFF8)  # move.l #pattern,(a0)+; dbra d0,*-6
    lea_at = len(c)
    cop = copper()
    c += w(0x41FA) + w(0)                                          # lea copper(pc),a0 (patched)
    c += w(0x43F9) + l(COP) + w(0x303C) + w(len(cop) // 2 - 1)     # lea COP,a1; move.w #words-1,d0
    c += w(0x32D8) + w(0x51C8) + w(0xFFFC)                         # move.w (a0)+,(a1)+; dbra d0,*-2
    for reg, v in ((0x100, 0x0200), (0x102, 0), (0x104, 0x24), (0x08E, 0x2C81), (0x090, 0x2CC1),
                   (0x092, 0x38), (0x094, 0xD0), (0x0E0, PLANES >> 16), (0x0E2, PLANES & 0xFFFF),
                   (0x0E4, (PLANES + 0x100) >> 16), (0x0E6, (PLANES + 0x100) & 0xFFFF)):
        c += movew(v, 0xDFF000 + reg)
    c += w(0x23FC) + l(COP) + l(0xDFF080)                          # move.l #COP,COP1LC
    c += movew(0, 0xDFF088) + movew(0x8380, 0xDFF096)              # COPJMP1; DMA: copper, planes
    c += w(0x60FE)                                                 # bra.s *
    cop_at = len(c)
    c += cop
    c = c[:lea_at + 2] + w(cop_at - (lea_at + 2)) + c[lea_at + 4:]
    assert 12 + len(c) <= 1024, len(c)
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

# ---- the check ----

def ppm(c12):
    """A 12-bit colour in RGB565 (the pictures are compared in it)"""
    r, g, b = (c12 >> 8) * 17, (c12 >> 4 & 15) * 17, (c12 & 15) * 17
    return (r * 31 // 255, g * 63 // 255, b * 31 // 255)

def half(c12, upper):
    out = 0
    for sh in (8, 4, 0):
        n = c12 >> sh & 15
        n = (n & 12) | (n & 12) >> 2 if upper else (n & 3) << 2 | (n & 3)
        out |= n << sh
    return ppm(out)

def shres_pixel(j):
    a, b = PAIRS[j // 2 % 16]
    return half(COLORS[4 * b + a], j % 2 == 0)

def read(path):
    data = open(path, 'rb').read()
    parts = data.split(b'\n', 3)
    wd, ht = map(int, parts[1].split())
    px = parts[3]
    rgb565 = lambda i: (px[i] >> 3, px[i + 1] >> 2, px[i + 2] >> 3)  # (either way of widening it)
    return wd, [[rgb565(3 * (y * wd + x)) for x in range(wd)] for y in range(ht)]

def find(row, expect, period):
    """The first x where the row repeats expect's period, and how far it goes on"""
    for x in range(len(row) - period):
        if all(row[x + i] == expect(i) for i in range(period)):
            n = 0
            while x + n < len(row) and row[x + n] == expect(n):
                n += 1
            return x, n
    return None, 0

def check(path):
    wd, pic = read(path)
    assert wd == 1440, 'the picture is %d wide: no super-hires lines?' % wd
    row = lambda line: pic[(line + 5 - 29) * 2]
    bad = []
    def ok(cond, what):
        print(('  ok  ' if cond else '  WRONG ') + what)
        if not cond:
            bad.append(what)
    lores = lambda i: ppm(COLORS[PIXELS[i // 4 % 32]])
    hires = lambda i: ppm(COLORS[PIXELS[i // 2 % 32]])
    xl, nl = find(row(50), lores, 128)
    ok(xl is not None and nl >= 1200, 'lores: its pattern 4 pixels a pixel, from %s for %d' % (xl, nl))
    xh, nh = find(row(60), hires, 64)
    ok(xh is not None and nh >= 1200, 'hires: 2 pixels a pixel, from %s for %d' % (xh, nh))
    x0, n0 = find(row(70), shres_pixel, 32)
    ok(x0 is not None and n0 >= 1200, 'super-hires: the pairs\' colours, from %s for %d' % (x0, n0))
    x3, n3 = find(row(80), shres_pixel, 32)
    ok(x3 is not None and n3 >= 1100 and (x3 - x0) % 32 == 12,
       'BPLCON1 3: 12 pixels later (from %s for %d)' % (x3, n3))
    ok(row(90) == row(70), 'dual playfield: drawn as one playfield')
    xs = 4 * (H - 108)
    s0 = [half(COLORS[21], i % 4 < 2) for i in range(64)]
    ok(row(100)[xs:xs + 64] == s0, 'sprite 0 at %d: pairs (1, 1) from register 21, upper then lower bits' % xs)
    ok(row(100)[xs - 4:xs] == row(70)[xs - 4:xs] and row(100)[xs + 64:xs + 68] == row(70)[xs + 64:xs + 68],
       'sprite 0: the playfield on either side')
    s1 = [half(COLORS[20], False)] * 2 + [half(COLORS[21], i % 4 < 2) for i in range(60)] + [half(COLORS[17], True)] * 2
    ok(row(110)[xs + 2:xs + 66] == s1 and row(110)[xs:xs + 2] == row(70)[xs:xs + 2],
       'sprite 0 with SPRxCTL bit 4: 2 pixels later (registers 20, 21, 17 at its ends)')
    x4 = 4 * (H + 48 - 108)
    ok(row(100)[x4 - 4:x4 + 68] == row(70)[x4 - 4:x4 + 68], 'sprite 4 at %d: not shown in super-hires' % x4)
    ok(row(120)[xs:xs + 64] == [ppm(COLORS[17])] * 64, 'lores: sprite 0 in register 17')
    ok(row(120)[x4:x4 + 64] == [ppm(COLORS[25])] * 64, 'lores: sprite 4 in register 25')
    print('%d wrong' % len(bad))
    sys.exit(1 if bad else 0)

if __name__ == '__main__':
    if sys.argv[1] == 'check':
        check(sys.argv[2])
    else:
        open(sys.argv[1], 'wb').write(build())
