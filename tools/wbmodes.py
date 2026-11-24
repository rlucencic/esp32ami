"""An Extras 3.1 copy that switches Workbench through display modes from
Execute Command, and shows a lores picture on a screen of its own.

    python3 tools/wbmodes.py Extras3.1.adf out.adf

Writes, over files of Extras 3.1 that the test doesn't need (as many
bytes, the rest zeros: an IFF reader stops at its FORM's length):
  prefs/input.info   ScreenMode prefs: PAL lores 320 x 256, 256 colours
  prefs/locale.info  PAL hires interlaced 640 x 512, 256 colours
  prefs/sound.info   PAL super-hires 1280 x 256, 16 colours
  prefs/palette      a 320 x 256 lores ILBM (32 colours: blocks and a grid)

Then on an A1200 with Kickstart 3.1 and a Workbench 3.1 hard disk (this
disk in DF0:), for each mode: Right-Amiga+E, Right-Amiga+X (the field
keeps the last command), and
  extras3.1:prefs/screenmode from extras3.1:prefs/input.info use
(locale.info, sound.info); then
  sys:utilities/multiview extras3.1:prefs/palette screen
and Left-Amiga held while the mouse drags the picture's screen down.
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from adf import ADF

MODES = [  # file to overwrite, display ID, width, height, depth
    ('prefs/input.info', 0x00021000, 320, 256, 8),
    ('prefs/locale.info', 0x00029004, 640, 512, 8),
    ('prefs/sound.info', 0x00029020, 1280, 256, 4),
]
PICTURE = 'prefs/palette'
W, H, PLANES = 320, 256, 5


def chunk(tag, data):
    return tag + struct.pack('>I', len(data)) + data + (b'\0' if len(data) & 1 else b'')


def screenmode_prefs(display_id, width, height, depth):
    prhd = struct.pack('>BBI', 0, 0, 0)
    scrm = struct.pack('>4IIHHHH', 0, 0, 0, 0, display_id, width, height, depth, 1)    # SMF_AUTOSCROLL
    return chunk(b'FORM', b'PREF' + chunk(b'PRHD', prhd) + chunk(b'SCRM', scrm))


def byterun1(row):
    out, i = bytearray(), 0
    while i < len(row):
        j = i
        while j < len(row) and j - i < 128 and row[j] == row[i]:
            j += 1
        if j - i >= 3:
            out += bytes([(257 - (j - i)) & 0xFF, row[i]])
            i = j
            continue
        j = i
        while j < len(row) and j - i < 128 and not (j + 2 < len(row) and row[j] == row[j + 1] == row[j + 2]):
            j += 1
        out += bytes([j - i - 1]) + row[i:j]
        i = j
    return bytes(out)


def pixel(x, y):
    if y % 32 == 0 or x % 40 == 0:
        return 1                                    # the grid, white
    return 2 + (x // 40 + 8 * (y // 32)) % 30       # 64 blocks of 30 colours


def ilbm():
    cmap = bytearray(b'\x00\x00\x00\xff\xff\xff')
    for i in range(30):
        cmap += bytes([(i * 37) & 0xFF, (i * 89 + 40) & 0xFF, (255 - i * 8) & 0xFF])
    body = bytearray()
    for y in range(H):
        for p in range(PLANES):
            row = bytearray(W // 8)
            for x in range(W):
                if pixel(x, y) >> p & 1:
                    row[x // 8] |= 0x80 >> (x % 8)
            body += byterun1(bytes(row))
    bmhd = struct.pack('>HHhhBBBBHBBhh', W, H, 0, 0, PLANES, 0, 1, 0, 0, 10, 11, W, H)
    form = b'ILBM' + chunk(b'BMHD', bmhd) + chunk(b'CMAP', bytes(cmap)) + \
        chunk(b'CAMG', struct.pack('>I', 0)) + chunk(b'BODY', bytes(body))
    return chunk(b'FORM', form)


def overwrite(adf, name, data):
    hdr = adf.lookup(name)
    size = len(adf.read(hdr))
    if len(data) > size:
        sys.exit(f'{len(data)} bytes don\'t fit {name} ({size})')
    adf.write_same_size(hdr, data + bytes(size - len(data)))
    return size


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    adf = ADF(sys.argv[1])
    for name, mode, w, h, d in MODES:
        size = overwrite(adf, name, screenmode_prefs(mode, w, h, d))
        print(f'{name}: ${mode:08X} {w} x {h}, {1 << d} colours')
    pic = ilbm()
    size = overwrite(adf, PICTURE, pic)
    print(f'{PICTURE}: a {W} x {H} lores ILBM, {len(pic)} of {size} bytes')
    adf.save(sys.argv[2])
