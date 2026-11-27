# A translating core for the 68EC020: the design, 2026-11-27

TODO item 19's next step. The interpreter is near its floor in C (the
loop's 28 P4 instructions around each instruction, ~400 handlers sharing
the time: `faster-020-handlers-2026-11-26.md`). Blocks of 68EC020 code
translated into RISC-V take the loop, the opcode fetch, the jump table,
the extension words and the flags' round trips through memory away from
the hot code.

## What the P4 allows (measured 2026-11-27)

A probe (`main/jit_probe.c`, the console's `X`) wrote RISC-V into the
PSRAM heap and called it:

- With `CONFIG_SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION` off, PSRAM has no
  PMP entries and the P4's PMA has the region RWX: the code ran.
- Rewritten and synced (`esp_cache_msync()`: the data cache written back,
  C2M; the instruction cache invalidated, M2C with `TYPE_INST`, aligned to
  the cache line), the new code ran.
- 4096 straight `addi` from PSRAM: 14 cycles each cold, 1.76 warm.
- Internal RAM isn't executable with the IDRAM split; the TCM is 8 KB
  (and was slower in the code-placement trial). So: PSRAM.

## The rule: exact

The collection's pictures must stay the same with the translated code as
with the interpreter (96 of 96): every cycle and every access at the same
moment. So:

- **Cycles**: each instruction subtracts what Musashi's does (its table's
  count, and what the handler adds), after its body; memory waits as the
  callbacks take them, at the access. `remaining` is up to date at every
  callback.
- **Slices**: a block stops at the instruction boundary where the
  interpreter's slice would end (`remaining <= 0`), the trace mark, or a
  request to stop (below). Interrupts are then where they were.
- **Instruction fetch**: the same `ic_longword()` timing, in the same
  order: an opcode reached by a jump charges its longword, one reached in
  sequence only a longword's first word; extension words the same.
  `s_ic.next` and its pointer are left as the interpreter leaves them.
- **State seen by the chips**: `REG_PPC` and the 68k registers are in
  Musashi's state whenever a callback can reach a chip register or a CIA
  (the poll skip reads the PC and a D register there).
- **Code that changes**: a write into translated code (by the CPU, the
  blitter, disk DMA, `mem_copy_to()`, the debugger) drops the blocks it
  touches; the block running stops after the writing instruction. A new
  memory map (`mem_update_map()`) or a reset drops everything.

## The parts

- `jit.c`: the block cache (68k PC to block, a hash table), the pages
  (256 bytes) that hold translated code and the blocks on each, the
  dispatcher (`jit_run()`, called from `m68k_host_run()` for the
  68EC020), the invalidation.
- The translation goes straight into RISC-V (`jit_rv.c`, the encoder in
  `rv.h`), no micro-ops in between: the host runs that very code through
  its own RV32IM interpreter (`host/rvsim.c`), so the collection checks
  the code the board runs, not a stand-in. (Planned as micro-ops with a C
  interpreter for the host; the interpreter of RISC-V was the simpler
  way to the same check.)
- A block: from a PC to an instruction that always changes the flow (BRA,
  JMP, RTS, RTE, BSR, JSR, TRAP, STOP, line A/F, ILLEGAL), at most 16
  instructions and 256 bytes, never across a 64 KB bank. A conditional
  branch inside it goes on within the block when its target is there,
  else exits when taken.

## Stages

1. **The machinery, every instruction a handler call** (micro-op `CALL`:
   the opcode's fetch timing, PPC/PC/IR and the fetch state set, the
   handler called directly, its cycles, the exits). No faster than the
   interpreter; proves the cache, the exits and the timing exact on the
   host (the collection, the replays, the A1200 survey's titles).
2. **RISC-V for the board** from those micro-ops; the board's pictures
   against the host's, and its speed (expected: about the interpreter's).
3. **Native instructions**, the commonest first (MOVE, MOVEQ, ADD/SUB/CMP
   and their Q and A forms, AND/OR/EOR, TST, CLR, LEA, Bcc, DBcc, shifts):
   extension words as constants (their fetch timing still charged), the
   68k registers in RISC-V registers within a block, flags in Musashi's
   format only where read or at an exit, RAM accesses inline (the bank's
   pointer and wait), chips and CIAs through the callbacks with the state
   written back first. Each batch checked on the host (the collection the
   same) and measured on the board.

## Open questions

- How much the fetch timing costs in translated code (a tag check per
  longword); a block whose lines stay in the cache could skip it.
- Self-modifying loops writing next to their code: 256-byte pages, the
  blocks on a page checked by address, may need finer pages.
- How big the code cache needs to be (PSRAM: ~3.4 MB free with the
  A1200's 8 MB of fast RAM).

## Stage 1, done (2026-11-27)

`jit.c` and `jit.h`, with `mem.c`'s `jit_exec_block()` running a block's
handler calls and `mem_interp_one()` the interpreter's step (for code it
can't translate). The hooks: the 68EC020's writes (`jit_check_write()`,
a byte of the page bitmap per 2 KB first), each blit's destination range
and each disk read (`blitter.c`, `floppy.c`), `mem_copy_to()`,
`amiga_poke8()`; `mem_update_map()` and `mem_cpu_reset()` drop all. The
JIT stands aside unless chip RAM is 2 MB (the A1200's: no mirrors, so a
page is an address).

- **Exact on the host**: the collection 96 of 96, the A1200 survey's 26
  packages 26 of 26, the replays 4 of 4 (`-DAMI_JIT` in the host build;
  `AMI_NOJIT` runs the interpreter alone).
- **The one difference found**: a Bcc.W not taken skips its displacement
  without fetching it (`REG_PC += 2`), so `s_ic.next` stays on that word
  and the next opcode counts as reached by a jump: its longword charged.
  The first version charged only a longword's first word inside a block:
  8 cycles short in Aerial Racers' decruncher (`move.b -(A0),-(A3)`
  after `bmi.w`), found by logging each instruction's cycles left
  (`AMI_PCLOGCYC` with `AMI_PCLOG`). Now each opcode decides as the
  interpreter does.
- **On the Tab5** (the C runner): slower, as expected: Virtual Karting 2
  81% -> 67%, Charr's core 1 ~80% -> 95%. The games ran right. Off in
  the board build (`components/amiga/CMakeLists.txt`) until stage 3.

A RISC-V version of stage 1 alone would cost about the interpreter's 28
P4 instructions an instruction (PC, PPC, IR, the fetch state, the cache
check, the call, the cycles, the exits): stage 2 waits for stage 3's
native instructions, and the emitter comes with them.

## Stage 3, the first batch (2026-11-27)

Stage 2 (RISC-V for every instruction a handler call) and stage 3 came
together: the translation writes RISC-V directly (`jit_rv.c`, `rv.h`), and
an instruction it doesn't know calls its handler from that code.

**Native**: MOVE, MOVEA, MOVEQ, LEA, PEA, CLR, TST, NEG, NOT, EXT, EXTB,
SWAP, EXG, ADDQ, SUBQ, ADD, SUB, AND, OR, EOR, CMP, ADDA, SUBA, CMPA, the
immediate forms, BTST BCHG BCLR BSET, the shifts and rotates of Dn by an
immediate count, ADDX and SUBX between registers, MULU.W, MULS.W, Scc,
Bcc, DBcc, BRA, BSR, JSR, JMP, RTS, MOVEM, NOP; every addressing mode but
the full index format. About 98% of the instructions the slow titles run
(their counts: the host's `AMI_OPSTAT`). The other shifts and rotates of
Dn call their handler directly (no PC, no fetch state).

**How**:

- The host runs the same code through an RV32IM interpreter
  (`host/rvsim.c`): a jump out of the code arena calls the host function
  at that address. The host build is not position independent and keeps
  its heap low, so every address fits the code's 32 bits; Musashi's
  state, the fetch state and the translated code's own state sit in one
  section (`jitstate`, as `.sdata.m68k`/`.sdata.ami` on the board), all
  reached from one register.
- Shared routines at the arena's start: the prologue and exit, a
  handler's call, and the data accesses: each the callback's plain memory
  path inline (the bank's pointer and wait; for a write `jit_code_pages`
  too), the callback itself (REG_PPC and the cycles left stored first)
  for the chips, the CIAs, odd addresses and anything at a bank's end.
  An access is two instructions where it's made (the PC register's
  update, the call).
- Flags pending: a flag setter leaves its source, destination and result
  in three registers; a Bcc, Scc or DBcc after a CMP, a SUB or a MOVE
  tests them directly (BLT after CMP is one signed compare of the two
  operands, shifted up for a byte or a word); they reach memory only
  before a handler, at an exit (the exit's own routine), at a jump within
  the block, and before BTST (which sets Z alone) or ADDX/SUBX (which
  read Z). X is never pending. They match Musashi's where its tests look:
  N and V bit 7, C and X bit 8, Z nonzero.
- The opcode's fetch timing as stage 1's, decided at translation where
  the path is known: an instruction reached in sequence charges a
  longword's first word, one reached by a jump within the block its
  longword (the jump knows its own last word), the block's first and the
  one after a handler compare `s_ic.next` at run time. The cache check of
  a longword: three instructions in fast RAM or the ROM (no wait: only the
  tag stored while the cache fills), ten in chip RAM.
- Blocks are translated once their address has run 64 times, and hold
  at most 16 instructions: Workbench's boot made 95000 blocks in 30 s
  with every block translated at once (the Tab5 at 18%), and blocks
  running on past their branches copied a routine's tail for every entry
  into it (Virtual Karting 2's race: its 1 MB of code rebuilt 165 times
  in 4000 frames). Now 23 flushes of the arena in 12000 frames.
- Instruction lengths from the opcode for the families above (Musashi's
  disassembler for the rest; on the host every length is checked against
  it): the disassembler's text formatting took most of a translation's
  ~210 µs on the board.

**Checked on the host**: the collection 96 of 96, the A1200 survey's 26
packages 26 of 26 and the replays 4 of 4, the same as the interpreter, with
the board's code exactly (`AMI_JITNOHOOK`) and with the hooks;
`host/jit_test.c`, a million random instructions and flag-reading pairs
against the interpreter (registers, flags, PC, PPC, IR, the cycles left,
the fetch state, the cache's tags, chip RAM), none different; `rv.h`'s
encodings against the GNU assembler's (2360, the same).

**The Tab5** (`board_pass.py`, median and slowest stretch):

| Title | Interpreter | Translated |
|---|---|---|
| Virtual Karting 2 | 81.0% (75.6%) | 99.6% (96.7%) |
| AmiBee | its slowest ~68% | 99.9% (94.5%), core 1 51% |
| Charr | 100%, core 1 ~88% | 99.9%, core 1 68% |
| Deluxe Galaga | 98.1% (66.6%) | 97.8% (77.2%) |
| Gea Lux | its slowest 95.1% | 100% (95.6%) |
| Metal Kombat | its slowest 66.7% | 100% (82.8%) |
| Leading Lap | 99.5% (88.0%) | 95.6% (86.3%) |

(The interpreter's from `faster-020-handlers-2026-11-26.md` and the
survey's runs; the translated from `board_pass.py`, 2026-11-27: Virtual
Karting 2 and Leading Lap on the final firmware, the shifts by a
register calling their handlers directly; the others on the one before,
98.1% (91.4%) and 95.5% (83.5%) there. The games run right on the
board; its pictures can't be compared with the host's frame by frame:
Virtual Karting 2's intro differs between any two board runs, the
interpreter's too.)

**Where the time goes now** (core 1's samples in the slowest stretches;
the flash's code runs from PSRAM at 0x48000000, so only the arena's
addresses are translated code): translated code and its routines 15-35%,
the dispatcher (`jit_run()`, `mem_fetch_state()`) 5-11%, the chips as
before. Leading Lap: its code doesn't fit the 1 MB arena (17 RISC-V
instructions and 1.17 exits a 68k instruction, ~95 bytes with the exits'
table; on the host 65 flushes in 9300 frames, 11 with 2 MB), translation
and `jit_written()` 2.7% each. The board's counters over the six titles:
416000 translations, 39 s translating, 6 s syncing the caches, of ~30
minutes.

**Next**:

- Denser code, so the hot code of a title like Leading Lap fits: the
  exits' table in 8 bytes, not 16; the P4's compressed instructions (half
  the size for most of what the blocks use; the host's interpreter and
  the assembler check would need them too); and/or a 2 MB arena (the
  routines are in its middle already: a jump reaches 1 MB either way),
  if the PSRAM can spare it (2 MB free with the A1200's 8 MB of fast RAM
  and the 1 MB arena; an extended ADF's 2.5 MB buffer no longer fits
  then).
- Blocks reaching each other without returning to the dispatcher.
- The handlers the titles still call (the host's `AMI_JITSTAT` lists
  them): the long multiplies and divides, the bit fields.
