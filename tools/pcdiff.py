"""Where two runs part: compares two host PC logs (AMI_PCLOG=<file>:<frame>
[:<lo>-<hi>], each instruction's PC and frame) and prints the first
instruction where they differ, with the instructions before it in both.
That is often only timing (a wait loop's passes, an interrupt a little
sooner), so it also prints the code each run executes and the other never
does, in the order first reached: where their paths really part.

    python3 tools/pcdiff.py a.bin b.bin [instructions before, 12]

Both runs must start logging at the same point (the same frame, before
anything differs in the range); frames are shown, not compared.
"""
import sys
from array import array

CHUNK = 1 << 20     # records compared at a time


def load(path):
    a = array('I')
    with open(path, 'rb') as f:
        a.frombytes(f.read())
    if sys.byteorder != 'little':
        a.byteswap()
    return a


def first_difference(a, b):
    n = min(len(a), len(b)) // 2
    for start in range(0, n, CHUNK):
        end = min(n, start + CHUNK)
        if a[2 * start:2 * end:2] != b[2 * start:2 * end:2]:
            for i in range(start, end):
                if a[2 * i] != b[2 * i]:
                    return i
    return None if len(a) == len(b) else n


def show(name, a, i, before):
    print(name)
    for k in range(max(0, i - before), min(len(a) // 2, i + 3)):
        mark = '>' if k == i else ' '
        print(f'  {mark} {k:10d}  ${a[2 * k]:06x}  frame {a[2 * k + 1]}')


def first_visits(a):
    """{pc: (instruction index, frame)} of each PC's first execution"""
    seen = {}
    for k in range(0, len(a), 2):
        pc = a[k]
        if pc not in seen:
            seen[pc] = (k // 2, a[k + 1])
    return seen


def only_in(name, mine, other, count=12):
    pcs = sorted((v, pc) for pc, v in mine.items() if pc not in other)
    print(f'{name}: {len(pcs)} instructions the other run never executes; the first:')
    for (index, frame), pc in pcs[:count]:
        print(f'    ${pc:06x}  instruction {index}, frame {frame}')


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    before = int(sys.argv[3]) if len(sys.argv) > 3 else 12
    a, b = load(sys.argv[1]), load(sys.argv[2])
    i = first_difference(a, b)
    print(f'{len(a) // 2} and {len(b) // 2} instructions logged')
    if i is None:
        print('the same all through')
        return
    print(f'first difference at instruction {i}')
    show(sys.argv[1], a, i, before)
    show(sys.argv[2], b, i, before)
    va, vb = first_visits(a), first_visits(b)
    only_in(sys.argv[1], va, vb)
    only_in(sys.argv[2], vb, va)


main()
