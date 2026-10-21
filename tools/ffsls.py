"""List an RDB HDF partition's AmigaDOS tree (OFS/FFS), a few levels deep."""
import struct, sys
d = open(sys.argv[1], 'rb').read()
depth = int(sys.argv[2]) if len(sys.argv) > 2 else 2
L = lambda b, o: struct.unpack('>I', b[o:o+4])[0]
r = d[:512]
part = d[L(r, 28) * 512:][:512]
env = part[128:]
E = lambda i: L(env, 4 * i)
heads, secs, low, high = E(3), E(5), E(9), E(10)
start = low * heads * secs
blocks = (high - low + 1) * heads * secs
root = start + blocks // 2
def blk(n): return d[n * 512:(n + 1) * 512]
def name(b): return b[433:433 + b[432]].decode('latin-1')
print('volume', name(blk(root)), 'root', root)
def walk(n, indent, lvl):
    b = blk(n)
    for i in range(72):
        h = L(b, 24 + 4 * i)
        while h:
            e = blk(start + h)
            t = struct.unpack('>i', e[508:512])[0]
            print('  ' * indent + name(e) + ('/' if t == 2 else ''))
            if t == 2 and lvl < depth:
                walk(start + h, indent + 1, lvl + 1)
            h = L(e, 496)
walk(root, 1, 1)
