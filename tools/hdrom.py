"""Assemble the hard disk board's boot ROM into components/amiga/hdrom.h.

The few 68000 instructions it needs are written out here (two passes for
the labels); ILLEGAL ($4AFC) + a function word calls the native code in
hdboard.c. Run it after changing the ROM:  python3 tools/hdrom.py
"""
import os, sys

FN = dict(DIAG=1, INIT=2, NEXT=3, BOOTNODE=4, OPEN=5, CLOSE=6, BEGINIO=7)
LVO = dict(FindResident=-96, Enqueue=-270, ReplyMsg=-378, CloseLibrary=-414, AddDevice=-432,
           OpenLibrary=-552, MakeDosNode=-144)
EB_MOUNTLIST = 74
D0, D1 = 0, 1
A0, A1, A2, A3, A4, A5, A6 = range(7)
BRA, BNE, BEQ = 0, 6, 7


class Asm:
    def __init__(self):
        self.code = bytearray()
        self.labels = {}
        self.fixes = []
        self.notes = []                 # (offset, text) for the listing

    def at(self):
        return len(self.code)

    def label(self, name):
        self.labels[name] = self.at()

    def w(self, *words, note=''):
        if note:
            self.notes.append((self.at(), note))
        for x in words:
            self.code += bytes([(x >> 8) & 255, x & 255])

    def data(self, b, note=''):
        if note:
            self.notes.append((self.at(), note))
        self.code += b

    def even(self):
        if self.at() & 1:
            self.code.append(0)

    def string(self, s, note=''):
        self.data(s.encode() + b'\0', note or repr(s))
        self.even()

    # instructions
    def movem_save(self):
        self.w(0x48E7, 0x3F3E, note='movem.l d2-d7/a2-a6,-(sp)')

    def movem_restore(self):
        self.w(0x4CDF, 0x7CFC, note='movem.l (sp)+,d2-d7/a2-a6')

    def execbase(self, an):
        self.w(0x2078 | an << 9, 0x0004, note='movea.l 4.w,a%d' % an)

    def moveq(self, v, dn):
        self.w(0x7000 | dn << 9 | (v & 255), note='moveq #%d,d%d' % (v, dn))

    def tst(self, dn):
        self.w(0x4A80 | dn, note='tst.l d%d' % dn)

    def jsr_lvo(self, name):
        self.w(0x4EAE, LVO[name] & 0xFFFF, note='jsr %s(a6)' % name)

    def exg(self, x, y):
        self.w(0xC148 | x << 9 | y, note='exg a%d,a%d' % (x, y))

    def movea_d(self, dn, an):
        self.w(0x2040 | an << 9 | dn, note='movea.l d%d,a%d' % (dn, an))

    def movea_a(self, src, an):
        self.w(0x2048 | an << 9 | src, note='movea.l a%d,a%d' % (src, an))

    def lea(self, d16, src, dst):
        self.w(0x41E8 | dst << 9 | src, d16 & 0xFFFF, note='lea %d(a%d),a%d' % (d16, src, dst))

    def movea_ind(self, d16, src, dst):
        self.w(0x2068 | dst << 9 | src, d16 & 0xFFFF, note='movea.l %d(a%d),a%d' % (d16, src, dst))

    def move_b_imm(self, imm, d16, an):
        self.w(0x117C | an << 9, imm & 0xFF, d16 & 0xFFFF, note='move.b #%d,%d(a%d)' % (imm, d16, an))

    def push_a(self, an):
        self.w(0x2F08 | an, note='move.l a%d,-(sp)' % an)

    def pop_a(self, an):
        self.w(0x205F | an << 9, note='movea.l (sp)+,a%d' % an)

    def jmp_ind(self, an):
        self.w(0x4ED0 | an, note='jmp (a%d)' % an)

    def jsr_ind(self, an):
        self.w(0x4E90 | an, note='jsr (a%d)' % an)

    def rts(self):
        self.w(0x4E75, note='rts')

    def native(self, fn):
        self.w(0x4AFC, FN[fn], note='native %s' % fn)

    def b(self, cond, target):
        self.fixes.append(('b8', self.at(), cond, target))
        self.w(0, note='%s.s %s' % ({BRA: 'bra', BNE: 'bne', BEQ: 'beq'}[cond], target))

    def lea_pc(self, target, an):
        self.fixes.append(('pc16', self.at() + 2, an, target))
        self.w(0x41FA | an << 9, 0, note='lea %s(pc),a%d' % (target, an))

    def resolve(self):
        for kind, at, x, target in self.fixes:
            dest = self.labels[target]
            if kind == 'b8':
                disp = dest - (at + 2)
                assert -128 <= disp <= 127 and disp != 0, (target, disp)
                word = 0x6000 | x << 8 | (disp & 255)
                self.code[at:at + 2] = bytes([word >> 8, word & 255])
            else:
                disp = dest - at
                assert -32768 <= disp <= 32767
                self.code[at:at + 2] = bytes([(disp >> 8) & 255, disp & 255])


def build():
    a = Asm()
    # init: the Resident's (A6 = ExecBase)
    a.label('init')
    a.movem_save()
    a.execbase(A6)
    a.native('INIT')                    # a1 = the device; d0 = 0: nothing to do
    a.tst(D0)
    a.b(BEQ, 'out')
    a.jsr_lvo('AddDevice')
    a.lea_pc('expname', A1)
    a.moveq(0, D0)
    a.jsr_lvo('OpenLibrary')
    a.tst(D0)
    a.b(BEQ, 'out')
    a.movea_d(D0, A5)
    a.label('next')
    a.native('NEXT')                    # a0 = the next parameter packet; d0 = 0: done
    a.tst(D0)
    a.b(BEQ, 'done')
    a.exg(A5, A6)
    a.jsr_lvo('MakeDosNode')
    a.exg(A5, A6)
    a.native('BOOTNODE')                # d0 = the DeviceNode; a1 = its BootNode; d0 = 0: skip
    a.tst(D0)
    a.b(BEQ, 'next')
    a.lea(EB_MOUNTLIST, A5, A0)
    a.jsr_lvo('Enqueue')
    a.b(BRA, 'next')
    a.label('done')
    a.movea_a(A5, A1)
    a.jsr_lvo('CloseLibrary')
    a.label('out')
    a.movem_restore()
    a.moveq(1, D0)
    a.rts()

    # the device's functions (A6 = the device)
    a.label('open')                     # a1 = IORequest, d0 = unit, d1 = flags
    a.native('OPEN')
    a.rts()
    a.label('close')
    a.native('CLOSE')
    a.moveq(0, D0)
    a.rts()
    a.label('expunge')                  # (also the null function)
    a.moveq(0, D0)
    a.rts()
    a.label('beginio')                  # a1 = IORequest
    a.move_b_imm(5, 8, A1)              # LN_TYPE = NT_MESSAGE
    a.native('BEGINIO')                 # d0 = 1: reply
    a.tst(D0)
    a.b(BEQ, 'beginio_done')
    a.push_a(A6)
    a.execbase(A6)
    a.jsr_lvo('ReplyMsg')
    a.pop_a(A6)
    a.label('beginio_done')
    a.rts()
    a.label('abortio')                  # nothing is ever pending
    a.moveq(0, D0)
    a.rts()
    a.label('diagnative')               # a0 = the board, a2 = the DiagArea's copy, a3 = ConfigDev
    a.native('DIAG')
    a.moveq(1, D0)                      # keep the copy
    a.rts()

    a.label('devname')
    a.string('esp32ami.device')
    a.label('idstring')
    a.string('esp32ami.device 1.0 (21.10.2026) hard disk images')
    a.label('expname')
    a.string('expansion.library')

    # the DiagArea: copied to RAM, position independent
    while a.at() % 16:
        a.code.append(0)
    diag = a.at()
    a.label('diag')
    a.w(0x9000, note='da_Config DAC_WORDWIDE|DAC_CONFIGTIME, da_Flags')
    size_at = a.at()
    a.w(0, note='da_Size')
    dp_at = a.at()
    a.w(0, 0, 0, 0, 0, note='da_DiagPoint, da_BootPoint, da_Name, reserved')
    a.label('diagpoint')                # a0 = the board
    a.lea(a.labels['diagnative'], A0, A1)
    a.jmp_ind(A1)
    a.label('bootpoint')
    a.execbase(A6)
    a.lea_pc('dosname', A1)
    a.jsr_lvo('FindResident')
    a.tst(D0)
    a.b(BEQ, 'boot_fail')
    a.movea_d(D0, A0)
    a.movea_ind(22, A0, A0)             # RT_INIT
    a.jsr_ind(A0)
    a.label('boot_fail')
    a.rts()
    a.label('dosname')
    a.string('dos.library')
    a.label('boardname')
    a.string('esp32ami hard disk')
    a.label('romtag')                   # its pointers filled in by FN_DIAG
    a.w(0x4AFC, note='rt_MatchWord')
    a.w(0, 0, 0, 0, note='rt_MatchTag, rt_EndSkip')
    a.w(0x0101, note='rt_Flags RTF_COLDSTART, rt_Version 1')
    a.w(0x030A, note='rt_Type NT_DEVICE, rt_Pri 10')
    a.w(0, 0, 0, 0, 0, 0, note='rt_Name, rt_IdString, rt_Init')
    a.label('diag_end')
    a.resolve()
    d = a.labels
    rel = lambda name: d[name] - diag
    for off, v in ((size_at, d['diag_end'] - diag), (dp_at, rel('diagpoint')), (dp_at + 2, rel('bootpoint')),
                   (dp_at + 4, rel('boardname'))):
        a.code[off:off + 2] = bytes([v >> 8, v & 255])
    return a


def main():
    a = build()
    d = a.labels
    out = os.path.join(os.path.dirname(__file__), '..', 'components', 'amiga', 'hdrom.h')
    notes = dict(a.notes)
    lines = ['/* The hard disk board\'s boot ROM: made by tools/hdrom.py (edit that, then', ' * run it) */',
             '#pragma once', '']
    for name in ('init', 'open', 'close', 'expunge', 'beginio', 'abortio', 'diagnative', 'devname', 'idstring',
                 'diag'):
        lines.append('#define HDROM_%-12s 0x%04X' % (name.upper(), d[name]))
    lines.append('#define HDROM_ROMTAG       0x%04X   /* in the DiagArea */' % (d['romtag'] - d['diag']))
    lines.append('#define HDROM_DIAG_SIZE    0x%04X' % (d['diag_end'] - d['diag']))
    lines += ['', 'static const uint8_t hdrom[] = {']
    i = 0
    while i < len(a.code):
        n = 2
        while i + n < len(a.code) and (i + n) not in notes and n < 8:
            n += 2
        chunk = a.code[i:i + n]
        text = ', '.join('0x%02X' % x for x in chunk) + ','
        lines.append('    %-40s /* %03X %s */' % (text, i, notes.get(i, '')))
        i += n
    lines.append('};')
    open(out, 'w').write('\n'.join(lines) + '\n')
    print('%s: %d bytes' % (out, len(a.code)))


if __name__ == '__main__':
    main()
