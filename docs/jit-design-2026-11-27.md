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
- The translation goes into micro-ops: a small RISC-like form. A C
  interpreter runs them (the host build: so the collection checks the
  translation exactly, and the board as a fallback); on the board a
  RISC-V emitter turns them into code in PSRAM.
- A block: from a PC to an instruction that always changes the flow (BRA,
  JMP, RTS, RTE, BSR, JSR, TRAP, STOP, line A/F, ILLEGAL), at most 64
  instructions and 256 bytes, never across a 64 KB bank. A conditional
  branch inside it exits when taken.

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
