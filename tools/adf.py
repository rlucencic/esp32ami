"""A small AmigaDOS (OFS/FFS) ADF reader and patcher, for tests.

  python3 tools/adf.py disk.adf                 the root directory
  python3 tools/adf.py disk.adf s/startup-sequence    a file (or a directory)

As a module: ADF(path).lookup(), read(), write_same_size() (a file's
contents replaced by as many bytes, OFS block checksums redone), save().
Used to make Workbench disks that test the emulator by themselves (a
startup-sequence that writes a file, or sets and reads the clock).
"""
import struct, sys

BS = 512

class ADF:
    def __init__(self, path):
        self.path = path
        self.d = bytearray(open(path, 'rb').read())
        self.ffs = self.d[3] & 1
        self.root = len(self.d) // BS // 2     # 880, a high-density disk's 1760

    def blk(self, n):
        return self.d[n * BS:(n + 1) * BS]

    def l(self, n, i):
        return struct.unpack_from('>I', self.d, n * BS + i * 4)[0]

    def setl(self, n, i, v):
        struct.pack_into('>I', self.d, n * BS + i * 4, v & 0xFFFFFFFF)

    def checksum(self, n, at=5):
        """block checksum at long `at` (5 for headers, 5 for OFS data)"""
        self.setl(n, at, 0)
        s = sum(self.l(n, i) for i in range(128)) & 0xFFFFFFFF
        self.setl(n, at, -s)

    @staticmethod
    def hash(name):
        h = len(name)
        for c in name.upper():
            h = (h * 13 + ord(c)) & 0x7FF
        return h % 72

    def name(self, n):
        b = self.blk(n)
        ln = b[BS - 80]
        return bytes(b[BS - 79:BS - 79 + ln]).decode('latin-1')

    def lookup(self, path):
        n = self.root
        for part in path.split('/'):
            h = self.hash(part)
            e = self.l(n, 6 + h)
            while e:
                if self.name(e).lower() == part.lower():
                    break
                e = self.l(e, 124)          # hash chain
            if not e:
                return None
            n = e
        return n

    def entries(self, n=None):
        n = self.root if n is None else n
        out = []
        for h in range(72):
            e = self.l(n, 6 + h)
            while e:
                out.append((self.name(e), e, struct.unpack('>i', self.blk(e)[BS - 4:])[0]))
                e = self.l(e, 124)
        return out

    def file_blocks(self, hdr):
        """data block numbers, in order"""
        blocks = []
        n = hdr
        while n:
            count = self.l(n, 2)
            for i in range(count):
                blocks.append(self.l(n, 77 - i))
            n = self.l(n, 126)              # extension
        return blocks

    def read(self, hdr):
        size = self.l(hdr, 81)
        data = bytearray()
        for b in self.file_blocks(hdr):
            if self.ffs:
                data += self.blk(b)
            else:
                ln = self.l(b, 3)
                data += self.blk(b)[24:24 + ln]
        return bytes(data[:size])

    def write_same_size(self, hdr, data):
        size = self.l(hdr, 81)
        assert len(data) == size, (len(data), size)
        pos = 0
        for b in self.file_blocks(hdr):
            if self.ffs:
                ln = min(BS, size - pos)
                self.d[b * BS:b * BS + ln] = data[pos:pos + ln]
            else:
                ln = self.l(b, 3)
                self.d[b * BS + 24:b * BS + 24 + ln] = data[pos:pos + ln]
                self.checksum(b)
            pos += ln

    def save(self, path=None):
        open(path or self.path, 'wb').write(self.d)

if __name__ == '__main__':
    a = ADF(sys.argv[1])
    if len(sys.argv) > 2:
        n = a.lookup(sys.argv[2])
        if n is None:
            print('not found'); sys.exit(1)
        if a.l(n, 127) == 2:   # dir
            for e in sorted(a.entries(n)): print(e)
        else:
            sys.stdout.write(a.read(n).decode('latin-1'))
    else:
        for e in sorted(a.entries()): print(e)
