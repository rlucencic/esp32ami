"""Extended ADFs (UAE-1ADF) for tests: made from an ADF and back, and a
boot disk that writes a raw track of its own.
usage: extadf.py make in.adf out.adf [raw [words]]
           AmigaDOS tracks (type 0), or raw MFM ones of words (6400)
       extadf.py adf in.adf out.adf
           its sectors (raw tracks decoded: all 11 sectors, checksums right)
       extadf.py tracks in.adf          the track table (type, bytes, bits)
       extadf.py rawboot out.adf track
           an extended ADF (AmigaDOS tracks) whose boot block writes two sync
           words and 2000 of a pattern to the track through trackdisk's
           TD_RAWWRITE, its io_Error to $7F000
       extadf.py find in.adf track
           where rawboot's words are in that track (cyclically)"""
import struct, sys

SYNC = 0x4489
SECTOR_BYTES = 512
TRACK_BYTES = 11 * SECTOR_BYTES
PATTERN = [SYNC, SYNC] + [(0x5A5A + 0x0123 * i) & 0xFFFF for i in range(2000)]

def mfm_long(data, prev):
    data &= 0x55555555
    clock = ~((data << 1) | (data >> 1) | ((prev & 1) << 31)) & 0xAAAAAAAA
    return data | clock

def mfm_track(track, sectors, words):
    out = []
    last = [0]
    def put_word(v):
        out.append(v)
        last[0] = v
    def put_long(raw):
        m = mfm_long(raw, last[0])
        put_word(m >> 16)
        put_word(m & 0xFFFF)
    def odd_even(vals):
        s = 0
        for v in vals:
            s ^= (v >> 1) & 0x55555555
            put_long((v >> 1) & 0x55555555)
        for v in vals:
            s ^= v & 0x55555555
            put_long(v & 0x55555555)
        return s
    for s in range(11):
        put_word(0x2AAA if last[0] & 1 else 0xAAAA)
        put_word(0xAAAA)
        put_word(SYNC)
        put_word(SYNC)
        info = 0xFF000000 | track << 16 | s << 8 | (11 - s)
        hsum = odd_even([info]) ^ odd_even([0, 0, 0, 0])
        data = list(struct.unpack('>128I', sectors[s * 512:(s + 1) * 512]))
        dsum = 0
        for v in data:
            dsum ^= ((v >> 1) & 0x55555555) ^ (v & 0x55555555)
        odd_even([hsum])
        odd_even([dsum])
        odd_even(data)
    while len(out) < words:
        put_word(0x2AAA if last[0] & 1 else 0xAAAA)
    return out

def dos_sectors(w, track):
    """The 11 sectors of a raw track (checksums right), or None"""
    n = len(w)
    get_long = lambda i: w[i % n] << 16 | w[(i + 1) % n]
    decode = lambda o, e: ((get_long(o) & 0x55555555) << 1) | (get_long(e) & 0x55555555)
    out = {}
    for i in range(n):
        if w[i] != SYNC or w[(i + 1) % n] == SYNC:
            continue
        h = i + 1
        info = decode(h, h + 2)
        s = info >> 8 & 0xFF
        if info >> 24 != 0xFF or info >> 16 & 0xFF != track or s >= 11:
            continue
        hsum = dsum = 0
        for k in range(10):
            hsum ^= get_long(h + 2 * k)
        for k in range(256):
            dsum ^= get_long(h + 28 + 2 * k)
        if hsum & 0x55555555 != decode(h + 20, h + 22) or dsum & 0x55555555 != decode(h + 24, h + 26):
            continue
        out[s] = b''.join(struct.pack('>I', decode(h + 28 + 2 * k, h + 284 + 2 * k)) for k in range(128))
    return b''.join(out[s] for s in range(11)) if len(out) == 11 else None

def make(adf, raw=False, words=6400):
    n = len(adf) // TRACK_BYTES
    table, data = b'', b''
    for t in range(n):
        sectors = adf[t * TRACK_BYTES:(t + 1) * TRACK_BYTES]
        if raw:
            body = b''.join(struct.pack('>H', v) for v in mfm_track(t, sectors, words))
            table += struct.pack('>HHII', 0, 1, len(body), len(body) * 8)
        else:
            body = sectors
            table += struct.pack('>HHII', 0, 0, len(body), len(body) * 8)
        data += body
    return b'UAE-1ADF' + struct.pack('>HH', 0, n) + table + data

def tracks(ext):
    assert ext[:8] == b'UAE-1ADF', 'not an extended ADF'
    n = struct.unpack('>H', ext[10:12])[0]
    off = 12 + 12 * n
    out = []
    for t in range(n):
        _, typ, size, bits = struct.unpack('>HHII', ext[12 + 12 * t:24 + 12 * t])
        out.append((typ, size, bits, ext[off:off + size]))
        off += size
    return out

def words_of(body, bits):
    return list(struct.unpack('>%dH' % (bits // 16), body[:bits // 16 * 2]))

def to_adf(ext):
    out = b''
    for t, (typ, size, bits, body) in enumerate(tracks(ext)):
        sectors = body[:TRACK_BYTES] if typ == 0 else dos_sectors(words_of(body, bits), t)
        if sectors is None:
            print('track %d: raw, not 11 AmigaDOS sectors: zeros' % t)
            sectors = bytes(TRACK_BYTES)
        out += sectors
    return out

def w(v): return struct.pack('>H', v & 0xFFFF)
def l(v): return struct.pack('>I', v & 0xFFFFFFFF)

def rawboot(track):
    c = w(0x41F9) + l(0x60000)                                     # lea $60000,a0
    c += w(0x30FC) + w(SYNC) + w(0x30FC) + w(SYNC)                 # move.w #SYNC,(a0)+ (twice)
    c += w(0x303C) + w(len(PATTERN) - 3) + w(0x323C) + w(PATTERN[2])   # move.w #n-1,d0; move.w #first,d1
    c += w(0x30C1) + w(0x0641) + w(0x0123) + w(0x51C8) + w(0xFFF8) # move.w d1,(a0)+; addi.w #$123,d1; dbra
    c += w(0x2449)                                                 # move.l a1,a2 (the IOStdReq)
    c += w(0x357C) + w(17) + w(28)                                 # move.w #TD_RAWWRITE,IO_COMMAND(a2)
    c += w(0x257C) + l(0x60000) + w(40)                            # move.l #buffer,IO_DATA(a2)
    c += w(0x257C) + l(2 * len(PATTERN)) + w(36)                   # move.l #bytes,IO_LENGTH(a2)
    c += w(0x257C) + l(track) + w(44)                              # move.l #track,IO_OFFSET(a2)
    c += w(0x157C) + w(0) + w(30)                                  # move.b #0,IO_FLAGS(a2)
    c += w(0x224A) + w(0x4EAE) + w(-456)                           # move.l a2,a1; jsr DoIO(a6)
    c += w(0x13C0) + l(0x7F000)                                    # move.b d0,$7F000
    c += w(0x357C) + w(9) + w(28) + w(0x42AA) + w(36)              # TD_MOTOR, IO_LENGTH 0: off
    c += w(0x224A) + w(0x4EAE) + w(-456)                           # DoIO
    c += w(0x60FE)                                                 # bra.s *
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
    return make(bytes(bb) + bytes(901120 - 1024))

def find(ext, track):
    typ, size, bits, body = tracks(ext)[track]
    if typ != 1:
        print('track %d: type %d (not raw)' % (track, typ))
        return 1
    ws = words_of(body, bits)
    n = len(ws)
    for i in range(n):
        if all(ws[(i + k) % n] == PATTERN[k] for k in range(len(PATTERN))):
            print('track %d: raw, %d words; the %d written words at word %d' % (track, n, len(PATTERN), i))
            return 0
    print('track %d: raw, %d words; the written words not there' % (track, n))
    return 1

if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'make':
        raw = len(sys.argv) > 4 and sys.argv[4] == 'raw'
        words = int(sys.argv[5]) if len(sys.argv) > 5 else 6400
        open(sys.argv[3], 'wb').write(make(open(sys.argv[2], 'rb').read(), raw, words))
    elif cmd == 'adf':
        open(sys.argv[3], 'wb').write(to_adf(open(sys.argv[2], 'rb').read()))
    elif cmd == 'tracks':
        for t, (typ, size, bits, _) in enumerate(tracks(open(sys.argv[2], 'rb').read())):
            print('%3d type %d %6d bytes %7d bits' % (t, typ, size, bits))
    elif cmd == 'rawboot':
        open(sys.argv[2], 'wb').write(rawboot(int(sys.argv[3])))
    elif cmd == 'find':
        sys.exit(find(open(sys.argv[2], 'rb').read(), int(sys.argv[3])))
