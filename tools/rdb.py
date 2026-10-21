"""Print an HDF's Rigid Disk Block: partitions and filesystems."""
import struct, sys
d = open(sys.argv[1], 'rb').read()
def L(b, o): return struct.unpack('>I', b[o:o+4])[0]
def blk(n): return d[n*512:(n+1)*512]
r = blk(0)
assert r[:4] == b'RDSK'
print('blocksize', L(r, 16), 'cyls', L(r, 64), 'secs', L(r, 68), 'heads', L(r, 72), 'part list', L(r, 28), 'fshd list', L(r, 32))
n = L(r, 28)
while n != 0xFFFFFFFF:
    p = blk(n)
    name = p[37:37 + p[36]].decode()
    env = p[128:]
    E = lambda i: L(env, 4 * i)
    print('PART', name, 'flags', hex(L(p, 20)), 'sizeblock', E(1), 'surfaces', E(3), 'sectors/blk', E(4), 'blkspertrack', E(5),
          'reserved', E(6), 'lowcyl', E(9), 'highcyl', E(10), 'buffers', E(11), 'bufmem', E(12), 'maxtransfer', hex(E(13)), 'mask', hex(E(14)),
          'bootpri', E(15), 'dostype', hex(E(16)))
    n = L(p, 16)
n = L(r, 32)
while n != 0xFFFFFFFF:
    f = blk(n)
    print('FSHD dostype', hex(L(f, 32)), 'version', hex(L(f, 36)), 'seglist block', L(f, 72))
    n = L(f, 16)
