"""Boot-block ADFs that test Paula's attach modes (ADKCON bits 0-7), and a
check of the sound the host build records (AMI_WAV). Channel 0's words
modulate channel 1 (vol, per, both: a word each 10 ms period of channel 0),
channel 1 plays a square (+127 +127 -127 -127: 4 periods a cycle); off3
plays the square on channel 3 with its attach bits (silent), none plays
channels 0 and 1 unattached.
usage: attachboot.py out.adf vol|per|both|off3|none
       attachboot.py check vol|per|both|off3|none <wav>
       (each of channel 1's stretches over three rounds of the table: the
       volume and period expected, the frequency and level measured)"""
import struct, sys

P0 = 35469                  # channel 0's period: 10 ms (PAL colour clocks)
CCK, RATE = 3546895, 44100
TABLES = 0x60000            # the words, then the square
MODES = {                   # ADKCON, channel 0's words, channel 1's period and volume before them
    'vol':  (0x01, [64, 48, 32, 16, 0, 24, 40, 56], 400, 10),
    'per':  (0x10, [300, 400, 500, 600, 700, 800, 900, 1000], 250, 64),
    'both': (0x11, [64, 300, 32, 500, 48, 700, 16, 900, 0, 400, 56, 1000], 250, 10),
    'off3': (0x88, [0], 400, 64),
    'none': (0x00, [0x4040, 0xC0C0], 400, 64),
}

def w(v): return struct.pack('>H', v & 0xFFFF)
def l(v): return struct.pack('>I', v & 0xFFFFFFFF)
def movew(imm, addr): return w(0x33FC) + w(imm) + l(addr)          # move.w #imm,addr.l
def movel(imm, addr): return w(0x23FC) + l(imm) + l(addr)          # move.l #imm,addr.l

def build(mode):
    adk, words, per1, vol1 = MODES[mode]
    square = 3 if mode == 'off3' else 1
    c = movew(0x7FFF, 0xDFF09A) + movew(0x7FFF, 0xDFF096) + movew(0x7FFF, 0xDFF09C)
    c += movew(0x00FF, 0xDFF09E)
    c += w(0x08F9) + w(1) + l(0xBFE001)                            # bset #1,$BFE001: the LED's filter off
    lea_at = len(c)
    c += w(0x41FA) + w(0)                                          # lea data(pc),a0 (patched)
    c += w(0x43F9) + l(TABLES)                                     # lea TABLES,a1
    data = b''.join(w(x) for x in words) + w(0x7F7F) + w(0x8181)
    c += w(0x303C) + w(len(data) // 2 - 1)                         # move.w #words-1,d0
    c += w(0x32D8) + w(0x51C8) + w(0xFFFC)                         # move.w (a0)+,(a1)+; dbra d0,*-2
    c += movel(TABLES, 0xDFF0A0) + movew(len(words), 0xDFF0A4)
    c += movew(P0, 0xDFF0A6) + movew(64, 0xDFF0A8)
    base = 0xDFF0A0 + 0x10 * square
    c += movel(TABLES + 2 * len(words), base) + movew(2, base + 4)
    c += movew(per1, base + 6) + movew(vol1, base + 8)
    c += movew(0x8000 | adk, 0xDFF09E)
    c += movew(0x8200 | (1 << square) | (0 if mode == 'off3' else 1), 0xDFF096)
    c += w(0x60FE)                                                 # bra.s *
    data_at = len(c)
    c += data
    disp = data_at - (lea_at + 2)
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

def stretches(mode):
    """(start in colour clocks, volume, period) of channel 1's stretches"""
    adk, words, per, vol = MODES[mode]
    out = []
    if mode == 'per':
        out.append((0, vol, per))
    for k in range(3 * len(words)):
        word = words[k % len(words)]
        if mode == 'vol':
            out.append((2 * k * P0, word, per))
        elif mode == 'per':
            out.append(((2 * k + 1) * P0, vol, word))
        elif k % 2 == 0:
            vol = word
            out.append((k * P0, vol, per))
        else:
            per = word
            out.append((k * P0, vol, per))
    return out

def measure(r, a, b):
    """The level (peak) and frequency (rising zero crossings) of r[a:b]"""
    peak = max(abs(x) for x in r[a:b])
    ups = [i - 1 + -r[i - 1] / (r[i] - r[i - 1]) for i in range(a + 1, b) if r[i - 1] < 0 <= r[i]]
    freq = (len(ups) - 1) * RATE / (ups[-1] - ups[0]) if len(ups) >= 3 else 0
    return peak, freq

def check(mode, wav):
    raw = open(wav, 'rb').read()[44:]
    n = len(raw) // 4
    v = struct.unpack('<%dh' % (2 * n), raw[:4 * n])
    left, right = v[0::2], v[1::2]
    if mode in ('off3', 'none'):
        print('peaks: left %d, right %d' % (max(map(abs, left)), max(map(abs, right))))
        return
    t0 = next(i for i, x in enumerate(right) if abs(x) > 200)
    rows = stretches(mode)
    full = 127 * 64 * 2                 # a channel's largest output (audio.c's emit() doubles it)
    bad = 0
    for k, (start, vol, per) in enumerate(rows):
        end = rows[k + 1][0] if k + 1 < len(rows) else start + rows[1][0] - rows[0][0]
        a = t0 + int((start + 0.2 * (end - start)) * RATE / CCK)
        b = t0 + int((start + 0.8 * (end - start)) * RATE / CCK)
        if b > n:
            print('the recording ends at stretch %d' % k)
            bad += 1
            break
        peak, freq = measure(right, a, b)
        level = peak / full
        expect = CCK / (4 * per)
        ok = level < 0.02 if vol == 0 else abs(freq / expect - 1) < 0.02 and abs(level * 64 / vol - 1) < 0.05
        bad += not ok
        print('%2d %6.1f ms  vol %2d per %4d %5.0f Hz | %5.0f Hz, level %.2f (vol/64 %.2f)%s' %
              (k, start * 1000 / CCK, vol, per, expect, freq, level, vol / 64, '' if ok else '  <-- wrong'))
    quiet = max(abs(x) for x in left[t0:])
    if quiet > 50:
        bad += 1
    print('left (channel 0, attached): peak %d%s' % (quiet, '' if quiet <= 50 else '  <-- not silent'))
    print('%d wrong' % bad)
    sys.exit(1 if bad else 0)

if __name__ == '__main__':
    if sys.argv[1] == 'check':
        check(sys.argv[2], sys.argv[3])
    else:
        open(sys.argv[1], 'wb').write(build(sys.argv[2]))
