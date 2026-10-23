"""Boot disks that test AGA, and checks of what the host build gives
(AMI_MODEL=3 AMI_CHIP_KB=2048 AMI_DF0=<copy> ami_host
roms/amiga-os-310-a1200.rom 1500 out.ppm, AMI_DUMP=7F000:2:clx.bin for
the collision cases). The boot block reads a payload from the disk
through trackdisk to $60000 and runs it: custom register writes from a
table (the copper list, planes and sprites are in the payload), then
CLXDAT ORed into $7F000 for good.
cases:
  fmode      4 lores planes fetched with FMODE 0, 1, 2, 3 (bands of 10
             lines): each band the pattern, FMODE 1 and 2 the same line
  shres      super-hires, 4 planes, FMODE 3, 24-bit colours: each pixel
             its own colour
  spr32      sprite 0 32 pixels wide by DMA (FMODE $0004, then $0008)
  sprpos     sprite 0 over super-hires, SPRxCTL bits 4 and 3 (70 and 35
             ns) 00 01 10 11: 0-3 pixels later; SPRES super-hires: a
             pixel a sprite pixel
  clx7 clx7off clx7reset
             8 planes, plane 7 under sprite 0, CLXCON2 ENBP7 with MVBP7 1,
             MVBP7 0, and MVBP7 0 then a CLXCON write (clears CLXCON2)
  clxwide clxwide0
             sprite 0 64 pixels wide, its pixels 48-63 (or 0-15), plane 1
             under 48-63
  sprborder sprborder2
             sprite 0 written by hand, armed all frame: shown inside the
             window only; with BRDSPRT above and below it too
usage: agaboot.py <case> out.adf
       agaboot.py check <case> <picture.ppm | the CLXDAT dump>"""
import struct, sys

PAYLOAD, TABLE, COP, PLANES, SPRITES, NULLSPR = 0x60000, 0x60100, 0x61000, 0x62000, 0x66000, 0x67F00
H = 160                     # the sprites' HSTART (lores)
COLORS = [(i * 0x5A3 ^ 0x3C6) & 0xFFF for i in range(32)]
PAL24 = [(i * 0x1F3A57 ^ 0x6C2D91) & 0xFFFFFF for i in range(16)]

def w(v): return struct.pack('>H', v & 0xFFFF)
def l(v): return struct.pack('>I', v & 0xFFFFFFFF)

def plane_word(p):
    """Plane p's bits for pixels 0-15 with the values 0-15"""
    return sum(((j >> p) & 1) << (15 - j) for j in range(16))

def sprite(n, instances):
    """A sprite's DMA list, n words a line: (vstart, vstop, hstart35, data,
    datb) each, the data the same on each line; then the end"""
    out = b''
    for vstart, vstop, h35, data, datb in instances:
        pos = (vstart & 0xFF) << 8 | (h35 >> 3) & 0xFF
        ctl = ((vstop & 0xFF) << 8 | (vstart >> 8 & 1) << 2 | (vstop >> 8 & 1) << 1 | (h35 >> 2) & 1 |
               (h35 >> 1 & 1) << 4 | (h35 & 1) << 3)
        out += w(pos) + bytes(2 * (n - 1)) + w(ctl) + bytes(2 * (n - 1))
        out += (b''.join(w(v) for v in data) + b''.join(w(v) for v in datb)) * (vstop - vstart)
    return out + bytes(4 * n)

def case(name):
    """The register table, copper list and memory of a case"""
    regs = [(0x1FC, 0), (0x100, 0x0200), (0x102, 0), (0x104, 0x0024), (0x106, 0x0C00), (0x10C, 0x0011),
            (0x08E, 0x2C81), (0x090, 0x2CC1), (0x092, 0x38), (0x094, 0xD0)]
    regs += [(0x180 + 2 * i, c) for i, c in enumerate(COLORS)]
    mem = {NULLSPR: bytes(16)}
    cop = [(0x120 + 4 * i + 2 * k, a) for i in range(8) for k, a in enumerate((NULLSPR >> 16, NULLSPR & 0xFFFF))]
    bands, dma, planes = [], 0x8380, []
    if name == 'fmode':
        planes = [w(plane_word(p)) * 32 for p in range(4)]
        bands = [(50, [(0x1FC, 0), (0x100, 0x4200), (0x108, -40), (0x10A, -40)]),
                 (60, [(0x1FC, 1), (0x108, -44), (0x10A, -44)]),
                 (70, [(0x1FC, 2)]),
                 (80, [(0x1FC, 3), (0x108, -48), (0x10A, -48)]),
                 (90, [(0x100, 0x0200), (0x1FC, 0)])]
    elif name == 'shres':
        planes = [w(plane_word(p)) * 96 for p in range(4)]
        regs += [(0x106, 0x0C00)] + [(0x180 + 2 * i, (c >> 12 & 0xF00) | (c >> 8 & 0xF0) | (c >> 4 & 0xF))
                                    for i, c in enumerate(PAL24)]
        regs += [(0x106, 0x0E00)] + [(0x180 + 2 * i, (c >> 8 & 0xF00) | (c >> 4 & 0xF0) | (c & 0xF))
                                    for i, c in enumerate(PAL24)] + [(0x106, 0x0C00)]
        bands = [(50, [(0x1FC, 3), (0x100, 0x4240), (0x108, -160), (0x10A, -160)]),
                 (60, [(0x100, 0x0200), (0x1FC, 0)])]
    elif name == 'spr32':
        planes = [bytes(64)]
        regs[0] = (0x1FC, 4)
        data, datb = [0xF0F0, 0x3C3C], [0xFF00, 0x0FF0]
        mem[SPRITES] = sprite(2, [(50, 60, 4 * H, data, datb), (70, 80, 4 * H, data, datb)])
        cop += [(0x120, SPRITES >> 16), (0x122, SPRITES & 0xFFFF)]
        bands = [(45, [(0x100, 0x1200), (0x108, -40), (0x10A, -40)]),
                 (65, [(0x1FC, 8)]), (85, [(0x1FC, 4)])]
        dma = 0x83A0
    elif name == 'sprpos':
        planes = [bytes(64)]
        ins = [(50 + 10 * k, 55 + 10 * k, 4 * H + k, [0xC3A5], [0]) for k in range(4)] + [(90, 95, 4 * H, [0xC3A5], [0])]
        mem[SPRITES] = sprite(1, ins)
        cop += [(0x120, SPRITES >> 16), (0x122, SPRITES & 0xFFFF)]
        bands = [(45, [(0x100, 0x1240), (0x108, -40), (0x10A, -40)]),
                 (89, [(0x106, 0x0CC0)]), (96, [(0x106, 0x0C00)])]
        dma = 0x83A0
    elif name.startswith('clx7'):
        planes = [bytes(40)] * 6 + [bytes(4) + w(0xFFFF) + bytes(34), bytes(40)]
        regs += [(0x098, 0), (0x10E, 0x0041 if name == 'clx7' else 0x0040)]
        if name == 'clx7reset':
            regs += [(0x098, 0)]
        bands = [(40, [(0x100, 0x0210), (0x108, -40), (0x10A, -40)]),      # (planes before the window)
                 (50, [(0x140, 0x3200 | (H + 1) >> 1), (0x142, (H + 1) & 1), (0x146, 0), (0x144, 0xFFFF)]),
                 (60, [(0x142, 0)])]                                       # (sprite 0 written: lines 50-59)
    elif name.startswith('sprborder'):
        planes = [bytes(40)]
        regs += [(0x140, 0x2C00 | H >> 1), (0x142, H & 1), (0x146, 0), (0x144, 0xFFFF)]
        if name == 'sprborder2':
            regs += [(0x106, 0x0C02)]                                  # BRDSPRT
        bands = [(40, [(0x100, 0x1200), (0x108, -40), (0x10A, -40)])]
    elif name.startswith('clxwide'):
        planes = [bytes(10) + w(0xFFFF) + bytes(28)]
        regs[0] = (0x1FC, 0x000C)
        data = [0, 0, 0, 0xFFFF] if name == 'clxwide' else [0xFFFF, 0, 0, 0]
        mem[SPRITES] = sprite(4, [(60, 70, 4 * (H + 1), data, [0, 0, 0, 0])])
        cop += [(0x120, SPRITES >> 16), (0x122, SPRITES & 0xFFFF)]
        regs += [(0x098, 0x0041)]
        bands = [(40, [(0x100, 0x1200), (0x108, -40), (0x10A, -40)])]
        dma = 0x83A0
    else:
        raise SystemExit('no case ' + name)
    for p, data in enumerate(planes):
        a = PLANES + 0x400 * p
        mem[a] = data
        cop += [(0x0E0 + 4 * p, a >> 16), (0x0E2 + 4 * p, a & 0xFFFF)]
    c = b''.join(w(r) + w(v) for r, v in cop)
    for line, moves in bands:
        c += w((line - 1) << 8 | 0xE1) + w(0xFFFE)
        c += b''.join(w(r) + w(v) for r, v in moves)
    mem[COP] = c + w(0xFFFF) + w(0xFFFE)
    regs += [(0x080, COP >> 16), (0x082, COP & 0xFFFF), (0x088, 0), (0x096, dma)]
    return regs, mem

def payload(name):
    regs, mem = case(name)
    code = w(0x33FC) + w(0x7FFF) + l(0xDFF09A) + w(0x33FC) + w(0x7FFF) + l(0xDFF096)
    code += w(0x33FC) + w(0x7FFF) + l(0xDFF09C)
    code += w(0x41FA) + w(TABLE - (PAYLOAD + len(code) + 2))      # lea table(pc),a0
    code += w(0x43F9) + l(0xDFF000)                                # lea $DFF000,a1
    code += w(0x3018) + w(0x6B08) + w(0x3218) + w(0x3381) + w(0) + w(0x60F4)   # move.w d1,(a1,d0.w) each
    code += w(0x4279) + l(0x7F000)                                 # clr.w $7F000
    code += w(0x3039) + l(0xDFF00E) + w(0x8179) + l(0x7F000) + w(0x60F2)   # or.w CLXDAT,$7F000 for good
    mem[PAYLOAD] = code
    mem[TABLE] = b''.join(w(r) + w(v) for r, v in regs) + w(0xFFFF)
    end = max(a + len(b) for a, b in mem.items())
    img = bytearray((end - PAYLOAD + 511) // 512 * 512)
    for a, b in sorted(mem.items()):
        assert img[a - PAYLOAD:a - PAYLOAD + len(b)] == bytes(len(b)), 'overlap at %x' % a
        img[a - PAYLOAD:a - PAYLOAD + len(b)] = b
    return bytes(img)

def build(name):
    body = payload(name)
    c = w(0x2449)                                                  # move.l a1,a2 (the IOStdReq)
    c += w(0x357C) + w(2) + w(28)                                  # CMD_READ
    c += w(0x257C) + l(PAYLOAD) + w(40) + w(0x257C) + l(len(body)) + w(36) + w(0x257C) + l(1024) + w(44)
    c += w(0x224A) + w(0x4EAE) + w(-456)                           # DoIO
    c += w(0x357C) + w(9) + w(28) + w(0x42AA) + w(36) + w(0x224A) + w(0x4EAE) + w(-456)   # the motor off
    c += w(0x4EF9) + l(PAYLOAD)                                    # jmp
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
    disk = bytes(bb) + body
    return disk + bytes(901120 - len(disk))

# ---- the checks ----

def c565(c24):
    r, g, b = c24 >> 16, c24 >> 8 & 0xFF, c24 & 0xFF
    return (r * 31 // 255, g * 63 // 255, b * 31 // 255)

def c12(c):
    return c565((c >> 8) * 0x110000 | (c >> 4 & 15) * 0x1100 | (c & 15) * 0x11)

def read(path):
    data = open(path, 'rb').read()
    parts = data.split(b'\n', 3)
    wd, ht = map(int, parts[1].split())
    px = parts[3]
    rgb565 = lambda i: (px[i] >> 3, px[i + 1] >> 2, px[i + 2] >> 3)
    return wd, [[rgb565(3 * (y * wd + x)) for x in range(wd)] for y in range(ht)]

def find(row, expect, period):
    for x in range(len(row) - period):
        if all(row[x + i] == expect(i) for i in range(period)):
            n = 0
            while x + n < len(row) and row[x + n] == expect(n):
                n += 1
            return x, n
    return None, 0

CLX = {'clx7': 0x8023, 'clx7off': 0x8021, 'clx7reset': 0x8023, 'clxwide': 0x8023, 'clxwide0': 0x8021}

def check(name, path):
    bad = []
    def ok(cond, what):
        print(('  ok  ' if cond else '  WRONG ') + what)
        if not cond:
            bad.append(what)
    if name in CLX:
        v = struct.unpack('>H', open(path, 'rb').read()[:2])[0]
        ok(v == CLX[name], 'CLXDAT %04x (expected %04x)' % (v, CLX[name]))
    else:
        wd, pic = read(path)
        row = lambda line: pic[(line + 2 - 29) * 2]
        if name == 'fmode':
            lores = lambda i: c12(COLORS[i // 2 % 16])
            for line, f in ((50, 0), (60, 1), (70, 2), (80, 3)):
                x, n = find(row(line), lores, 32)
                ok(x is not None and n >= 560, 'FMODE %d: the pattern from %s for %d' % (f, x, n))
            ok(row(60) == row(70), 'FMODE 1 and 2: the same line')
        elif name == 'shres':
            ok(wd == 1440, 'a super-hires frame (%d wide)' % wd)
            x, n = find(row(50), lambda i: c565(PAL24[i % 16]), 16)
            ok(x is not None and n >= 1200, 'each pixel its own 24-bit colour, from %s for %d' % (x, n))
        elif name == 'spr32':
            xs = 2 * (H - 108)
            px = lambda a, b, i: (a[i // 16] >> (15 - i % 16) & 1) | (b[i // 16] >> (15 - i % 16) & 1) << 1
            exp = [c12(COLORS[16 + v] if v else COLORS[0]) for v in
                   (px([0xF0F0, 0x3C3C], [0xFF00, 0x0FF0], i // 2) for i in range(64))]
            ok(row(50)[xs:xs + 64] == exp, 'FMODE $0004: 32 pixels at %d' % xs)
            ok(row(70)[xs:xs + 64] == exp, 'FMODE $0008: the same')
            ok(row(50)[xs + 64:xs + 72] == [c12(COLORS[0])] * 8, 'nothing after them')
        elif name.startswith('sprborder'):
            xs, brd = 2 * (H - 108), name == 'sprborder2'
            for line, inside in ((40, False), (100, True), (305, False)):
                shown = row(line - 2)[xs:xs + 32] == [c12(COLORS[17])] * 32
                ok(shown == (inside or brd), 'line %d (%s the window): sprite %s' %
                   (line, 'inside' if inside else 'outside', 'shown' if shown else 'hidden'))
        elif name == 'sprpos':
            ok(wd == 1440, 'a super-hires frame (%d wide)' % wd)
            xs = 4 * (H - 108)
            bits = [(0xC3A5 >> (15 - i)) & 1 for i in range(16)]
            for k, label in enumerate(('00', '01 (35 ns)', '10 (70 ns)', '11')):
                exp = [c12(COLORS[0])] * (8 + k) + [c12(COLORS[17] if bits[i // 4] else COLORS[0]) for i in range(64)]
                exp += [c12(COLORS[0])] * (8 - k)
                ok(row(50 + 10 * k)[xs - 8:xs + 72] == exp, 'SH1 SH0 %s: %d pixels later' % (label, k))
            exp = [c12(COLORS[0])] * 8 + [c12(COLORS[17] if b else COLORS[0]) for b in bits] + [c12(COLORS[0])] * 8
            ok(row(90)[xs - 8:xs + 24] == exp, 'SPRES super-hires: a pixel a sprite pixel')
    print('%d wrong' % len(bad))
    sys.exit(1 if bad else 0)

if __name__ == '__main__':
    if sys.argv[1] == 'check':
        check(sys.argv[2], sys.argv[3])
    else:
        open(sys.argv[2], 'wb').write(build(sys.argv[1]))
