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
  blocks on a page checked by address, may need finer pages. (Answered by
  a mask of each page's words with code: "Writes beside code".)
- How big the code cache needs to be (PSRAM: ~3.4 MB free with the
  A1200's 8 MB of fast RAM; 2 MB since, "The larger arena"; Leading Lap's
  slow stretch needs 62 KB once slices resume inside blocks, "Slices
  resumed inside blocks").

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
  the assembler check would need them too). (The 2 MB arena: done, below.)
- Blocks reaching each other without returning to the dispatcher.
- The handlers the titles still call (the host's `AMI_JITSTAT` lists
  them): the long multiplies and divides, the bit fields.

## The larger arena (2026-11-27)

Leading Lap's hot code didn't fit the 1 MB arena: on the board it ran
slower translated (95.6%, its slowest 86.3%) than interpreted (99.5%,
88.0%).

**The arena**: 2044 KB, the shared routines (1408 bytes) in a 4 KB room
at its middle, so a `jal` (1 MB either way) reaches every routine from
either end: 2 MB less that room is the most a single copy of them allows.
`ready()` takes the largest of 2044 KB, 1 MB, 512 KB and 256 KB that
leaves 512 KB of PSRAM to the rest (uploads, RP9s, the setup screen) and
fits in one free block; with none it tries again 4096 translations later
or at the next flush. `jit_release()` gives it back (the machine stopped:
a disk going in); the next translation takes one again, as large as
there's room for. `ami_malloc_big()` (`amiga.c`) does that when a disk's
buffer can't be had (`floppy.c`'s image and track buffers). The host:
`AMI_JITARENA=<KB>` for a smaller one, `AMI_JITRELEASE=<frames>` to give it
back before those frames.

**The PSRAM for it** (the board: the A1200 with 8 MB of fast RAM, two
floppies in):

- The block lists per page: 65536 of them (256 KB) for the 16 MB; now a
  directory of 256 banks in the arena's first bytes, each bank's table (1
  KB, its 256 pages) made in the arena when a block first lands there
  (after the block's code; no room: as a full arena), all gone at a flush.
- The Kickstart and AROS's extension ROM loaded straight into their
  working copies (`main.c`, `rom_store.c`; `amiga_init()` swaps each word
  in place): 1 MB of copies gone. They are the running machine's, and a
  restart that fails must leave it running (`emu_restart()` resumes it):
  `rom_store_load()` finds and checks both ROMs (the mapping of a
  partition, a card file read a piece at a time) before either buffer is
  written.
- 2229 KB free with the 2044 KB arena (1841 KB with the 1 MB arena
  before).

**A high-density ADF's buffer** (found on the board): Christmas Breakdown's
HD disk was refused. Its image buffer was the extended ADF's 2.5 MB; with
the arena given back the board had 5 MB free, but no 2.5 MB in one piece
(the arena's hole and the rest weren't neighbours), and the arena came
back at 1 MB. An HD ADF is written in place and never grows: its buffer
is its own 1.72 MB now, and it fits beside the 2 MB arena without giving
anything back. An extended ADF's 2.5 MB may still not be found in one
piece on the A1200 (OCS games, run on an A500: no arena there).

**Checked on the host**: the collection 96 of 96, the A1200 survey 26 of
26 and the replays 4 of 4 the same as the interpreter; both again with a
256 KB arena given back five times a run (`AMI_JITARENA=256
AMI_JITRELEASE=1500:2600:3500:4700:5500`: the page tables made and dropped
over and over, the arena taken again); `jit_test` 300000 random cases.
Leading Lap (15000 frames, the survey's input): 238000 translations and
211 flushes (163 full) with 1 MB, 72000 and 33 (25 full) with 2 MB.

**The Tab5** (`board_pass.py`, median and slowest stretch):

| Title | 1 MB arena | 2 MB arena |
|---|---|---|
| Leading Lap | 95.6% (86.3%) | 99.4% (90.2%) |
| Virtual Karting 2 | 99.6% (96.7%) | 99.6% (96.9%) |
| Christmas Breakdown (its HD disk in) | refused | 99.9% (99.4%), core 1 80% |

The counters over Leading Lap's and Virtual Karting 2's runs (from
power-on): 271000 → 93000 translations, 233 → 46 flushes, 22.4 s → 8.0 s
translating, 4.0 s → 1.3 s syncing the caches. AROS (its extension ROM
in place) booted on the A1200, Kickstart 1.3 on an A500 and 3.1 on the
A1200 started from their working copies; the arena given back once (the
refused HD disk above) and taken again at 1 MB, the A1200 running on.

## Where the slow stretches spend it (2026-11-27)

Before more native instructions, a measurement: which instructions the
titles' slowest stretches still leave to handlers, and what else they
cost. The board's 5 s windows (`board_pass.py`'s `board.json`) and its
profiles of core 1, and the host over the same frames with the survey's
input (`AMI_JITSTAT=<frame>` with `AMI_OPSTAT`, with and without the hooks;
the scripts in `build/measure-2026-11-27/`, not in git).

**Two of the "slowest stretches" were the board pass's own**: Metal
Kombat's 82.8% (frames 7809-8223) holds a profile and a screenshot, its
next slowest (83.7%, 5600-5813) a disk insert through the web page (the
machine paused); every other window is 99.4% or more, core 1 at 81% at
most. Deluxe Galaga's 77.2% holds a profile too: without one it is 80.7%
(3133-3337), 20 of its 45 windows under 97.5%. Leading Lap's slowest
(90.2%) has no mark: 10 of 40 windows under 97.5%.

**The host over those frames** (a frame's instructions; the share run
by the translated code, by handlers called from blocks and outside any
block; the blocks run; the RISC-V instructions run in the arena, without
the hooks, the board's code):

| Stretch | Instructions | Translated | Handlers | Outside | Blocks run | Instructions a block run | RISC-V a frame |
|---|---|---|---|---|---|---|---|
| Deluxe Galaga 3133-3938 (credits) | 37218 | 100% | 0.0% | 0.0% | 9542 | 3.9 | 1646k |
| Deluxe Galaga 10000-11400 | 6749 | 97.9% | 0.4% | 1.7% | 1621 | 4.1 | 247k |
| Leading Lap 6290-7927 | 25787 | 96.3% | 0.6% | 3.1% | 3155 | 7.9 | 849k |
| AmiBee 14015-14266 | 10969 | 88.6% | 0.2% | 11.2% | 1485 | 6.6 | 316k |
| Gea Lux 3850-4089 | 6122 | 87.3% | 0.4% | 12.3% | 640 | 8.4 | 175k |
| Virtual Karting 2 13084-13341 | 44888 | 99.1% | 0.9% | 0.0% | 3091 | 14.5 | 1185k |

- **Handlers: 0.2-0.9% of the instructions.** Leading Lap's are DIVS.W
  (126 a frame) and DIVU.W (38), Virtual Karting 2's BFINS (140), MULS.L
  (110) and DIVS.L (94); the board's profiles agree (Musashi's handlers 0.3% of core 1
  in Deluxe Galaga's credits, 0.5% in Virtual Karting 2's race, 2.7% in
  Leading Lap). Translating more families has almost nothing left here.
- **The blocks' entries and exits**: a fit over the six stretches gives
  ~20 RISC-V instructions a 68020 instruction and ~93 a block run (the
  prologue keeps ra and s0-s11 and sets up the cycles and the tags, the
  exit restores them; the exit's pending flags to memory; the first
  instruction's fetch decided at run time). That is 24-61% of the
  arena's instructions (54% in Deluxe Galaga's credits), and the C
  dispatcher (`jit_run()`, `mem_fetch_state()`) is another 8-10% of
  core 1 on the board.
- **Where the exits go** (`AMI_JITSTAT`'s exits):

| Stretch | To a fixed PC with a block there | none there | the slice over | Computed (RTS, JMP (An)...) | After a handler |
|---|---|---|---|---|---|
| Deluxe Galaga credits | 76.0% | 0.0% | 6.2% | 17.9% | 0.0% |
| Deluxe Galaga 10000- | 84.3% | 0.3% | 10.5% | 4.6% | 0.3% |
| Leading Lap | 63.8% | 1.0% | 15.6% | 19.0% | 0.6% |
| AmiBee | 67.2% | 2.3% | 15.4% | 14.8% | 0.4% |
| Gea Lux | 59.4% | 3.9% | 11.3% | 24.4% | 1.0% |
| Virtual Karting 2 | 81.7% | 0.0% | 13.9% | 2.6% | 1.8% |

  60-84% of the exits could go straight to the next block's body; the
  dispatcher's lookups take ~1 step of the hash chain each (not the cost).
- **Deluxe Galaga's credits are its idle loop**, ~120 instructions a
  pass, ~300 passes a frame: lowlevel.library's ReadJoyPort (through
  potgo.resource's WritePotgo: INTENA and POTGO written), CIA-A's serial
  register against 20 key codes, two RAM flags. A real A1200 spins there
  too; with the calls and the chips' writes `wait.c` can't pass it, so
  only cheaper translated code helps.
- **Deluxe Galaga's later slow windows** (88-89% at 10130-11309 on the
  board) run only ~6700 instructions a frame on the host: the chips', not
  the CPU's (its collisions are worked out on every line while it reads
  CLXDAT: `pf_line()`, `collide()`, `bits32()` 10% of core 1 in the
  credits' profile). No board profile of those frames yet.

**So the next step is the blocks reaching each other**: an exit to a fixed
PC whose block exists jumps into its body (the slice's cycles and
`jit_stop` checked, the fetch charged as known, the flags to memory as
now), the dispatcher's prologue and exit paid once per slice instead of
once per block; computed exits (RTS) through a small cache of targets
next. A rough count: ~160 P4 instructions saved an exit chained, ~1.2 M a
frame in Deluxe Galaga's credits (a fifth of core 1 there), ~0.3 M in
Leading Lap's slow stretch.

## Blocks reaching each other (2026-11-27)

The step the measurement above pointed to: an exit whose flow goes on to a
fixed PC jumps straight into that PC's block once both exist, instead of
returning to the dispatcher (which kept s0-s11 and set up the cycles and
the tags for every block, then looked the next one up in C).

**The exits** (`jit_rv.c`):

- Each exit ends in `jal a0` to the shared exit with its 16-byte record
  (PC, PPC, the last word fetched, IR, kind) right after the jal: the
  block returns the record's address, so the dispatcher knows the exit
  whichever block of a chain took it (the exits' table after the code is
  gone).
- A new kind, `JIT_EXIT_LINK`: a branch out of the block, BSR, JSR or JMP
  to a fixed address, the end of a block that goes on. Its code: the
  pending flags to memory, the cycles left checked (unless the last
  instruction's own check just did, at a block's end), `jit_stop` checked
  after BSR's and JSR's push (only a write sets it, and every other
  write's instruction checks it itself), then two words, NOPs until
  linked, then the jal to the shared exit.
- The other exits stay as they were: the per-instruction ones (the slice
  over, `jit_stop`), the computed ones (RTS, JMP (An)) and those after a
  handler.

**The entries**: a block has an entry for an opcode reached by a jump and
one for an opcode reached in sequence (they differ only when the first
opcode is a longword's second word: its fetch is charged or not). Each
sets the tags' base (s2) and charges the first opcode as known; the
dispatcher's entry runs the prologue and picks one by the fetch state, as
its run-time check did before.

**Linking** (`jit.c`): when a link exit comes back to the dispatcher with
the slice going on, the dispatcher looks the PC up (translating it when
hot, as before) and makes the exit's two words a jump to that block's
entry (`jal a0` within 1 MB, else `auipc` + `jalr a0`), the entry chosen
by the exit's last word fetched; then the caches synced for those 8
bytes. A chain runs until the slice is over, a computed exit, a handler
that stops, or a write that set `jit_stop`.

**Dropped blocks**: a block dropped (its code written) gets each entry's
first word made a jump to a shared routine that leaves as the link exit
that jumped there (a0 points at that exit's jal, its record after it):
the dispatcher then links that exit to the new block. A drop while a
block runs is queued and done when it returns (a drop is always a
write's, and the write's instruction stops the block before any other
link is taken); more than 64 at once flush everything.

**Checked on the host**: the collection 96 of 96, the A1200 survey 26 of
26 and the replays 4 of 4 the same as the interpreter (the board's code,
`AMI_JITNOHOOK`). `jit_test` runs each case again without a flush (its
links taken) and, in chip RAM, after the word behind the instruction is
written over (its block's entries killed); the code after the instruction
is a `bra.s *`, a loop counting the cycles down, or twenty NOPs looping
(blocks cut at 16 instructions: links into the next in sequence). 20000
cases and two runs of 100000, none different (~22000 links and ~66000
killed entries a 100000). It catches a link to the wrong entry, a dropped
block not killed and a link without its cycles check (that one hangs); the
`jit_stop` check after a push into a linked block isn't reached by it.

**The host's count** over the slow stretches (`AMI_JITSTAT`, without the
hooks):

| Stretch | RISC-V a frame | Dispatcher trips a frame |
|---|---|---|
| Deluxe Galaga credits | 1.65 M → 1.34 M (−19%) | 9542 → 2292 |
| Deluxe Galaga 10000-11400 | 247k → 188k (−24%) | 1622 → 255 |
| Leading Lap | 849k → 762k (−10%) | 3207 → 1164 |
| AmiBee | 316k → 273k (−14%) | 1567 → 509 |
| Gea Lux | 175k → 155k (−11%) | 697 → 288 |
| Virtual Karting 2 | 1.18 M → 1.07 M (−9%) | 3092 → 565 |

**The Tab5** (`board_pass.py`, median and the slowest window without the
pass's own screenshots, profiles or disk inserts in it; core 1's median):

| Title | Before | Linked |
|---|---|---|
| Deluxe Galaga | 97.8% (80.7%), core 1 96% | 99.7% (90.4%), core 1 89% |
| Leading Lap | 99.4% (90.2%), core 1 98% | 99.6% (94.5%), core 1 90% |
| AmiBee | 99.9% (94.5%), core 1 51% | 99.9% (99.5%), core 1 41% |
| Gea Lux | 100% (95.6%), core 1 32% | 100% (96.8%), core 1 30% |
| Virtual Karting 2 | 99.6% (96.9%), core 1 90% | 99.9% (99.4%), core 1 85% |

Deluxe Galaga's credits went from ~81% to 90-92% (every window there
held a screenshot or profile this time); its slowest windows now are the
later ones the chips limit (90.4%, 88.5-89.5% before), windows under
97.5% 20 of 45 → 10 of 42. Leading Lap's 10 of 40 → 5 of 41. The board's
profile in Deluxe Galaga's credits: the dispatcher (`jit_run()`,
`mem_fetch_state()`) 10.3% of core 1 → 4.8%. Over the five titles'
pass: 103760 links, 1485 kills, 3.2 s syncing the caches in all (links
and kills included) of ~26 minutes.

**Next**: computed exits (RTS, JMP (An): 3-24% of the exits) through a
small cache of targets in the code; Deluxe Galaga's later windows are the
chips' (collisions on every line while it reads CLXDAT); Leading Lap's
`jit_written()` (4.1% of core 1 in its profile: writes into pages with
code).

## The computed exits' targets (2026-11-27)

Before giving RTS, JSR (An) and JMP (An) a memory of their targets: how
often does such an exit go where it went before? The host over the same
stretches (`AMI_JITSTAT`: per exit the last 1, 2 or 4 targets, the latest
first; only exits with the slice going on, the ones a jump could replace):

| Stretch | Dispatcher trips a frame (slice going on / over) | RTS: the last target, among 2, among 4 | JSR (An) | JMP (An) |
|---|---|---|---|---|
| Deluxe Galaga credits | 1677 (1675 computed) / 615 | 837 a frame: 99.2%, 99.3%, 99.8% | 629: 99.4%, 99.8%, 100% | 209: 100% |
| Deluxe Galaga 10000- | 80 / 174 | 49: 85.6%, 87.1%, 96.4% | 22: 83.9%, 93.8%, 99.3% | 2: 98.7% |
| Leading Lap | 634 (600) / 530 | 434: 75.8%, 94.3%, 97.0% | 163: 82.4%, 88.1%, 93.1% | 3: 98.9% |
| AmiBee | 260 (226) / 249 | 183: 58.7%, 87.3%, 93.9% | 32: 85.5%, 89.0%, 97.7% | 10: 82.6%, 98.6% |
| Gea Lux | 205 (164) / 83 | 109: 52.7%, 83.9%, 90.0% | 42: 90.5%, 95.3%, 97.8% | 13: 80.6%, 97.0% |
| Virtual Karting 2 | 102 (76) / 463 | 76: 68.8%, 76.0%, 82.9% | 0 | 0 |

- With the links made, computed exits are nearly all the dispatcher's
  trips while the slice goes on; the rest are slice ends (the chips'
  events), which stay.
- Deluxe Galaga's credits: 99% go where they went last time (its idle
  loop calls the same library routines through `jsr (-$12,A6)` and
  returns to the same places): ~1660 trips a frame a memory of one target
  would replace.
- Elsewhere RTS repeats its last target 53-76% of the time, 84-94% with
  two kept (a routine called from two places, in turn); JSR (An) 82-91%
  with one.
- So two targets per exit, the latest first. The target's block is there
  for 97-100% of the repeats (it ran before). At ~180 P4 instructions a
  trip less ~10 for the compares, about 0.3 M instructions a frame in
  Deluxe Galaga's credits (~5-7% of core 1), ~0.1 M in Leading Lap's
  slow stretch.

## The computed exits' kept targets (2026-11-27)

**The code** (`jit_rv.c`): a computed exit (RTS, JSR/JMP (An)) checks the
cycles left (and `jit_stop` after JSR's push), loads the PC from memory
and compares it with two targets kept after its record (data words: PC,
entry; the latest first; an odd PC for none), then jumps to the matching
block's entry with a0 at its jal to the shared exit, as a link does (a
dropped block's entry leaves through the same record). Else on to the
dispatcher as before, which keeps that target's block as the latest
(`remember()`, the entry chosen as for a link; data the code reads, so no
caches to sync). 8 instructions to a hit on the first target, 11 on the
second.

**Checked on the host**: the collection 96 of 96, the survey 26 of 26,
the replays 4 of 4 the same as the interpreter; `jit_test`'s RTS and
JSR/JMP (An) now mostly go to the code after them, with the cycles to go
on (459 targets kept in 20000 cases); it catches a kept target's wrong
entry and a target taken without its compare.

**The host's count** (dispatcher trips a frame, all kinds: unlinked →
links → kept targets; the RISC-V instructions run):

| Stretch | Trips | RISC-V a frame |
|---|---|---|
| Deluxe Galaga credits | 9542 → 2292 → 625 (615 slice ends) | 1.65 M → 1.34 M → 1.27 M |
| Leading Lap | 3207 → 1164 → 615 (530) | 849k → 762k → 743k |
| AmiBee | 1567 → 509 → 362 (244) | 316k → 273k → 263k |
| Gea Lux | 697 → 288 → 145 (78) | 175k → 155k → 141k |
| Virtual Karting 2 | 3092 → 565 → 507 (463) | 1.18 M → 1.07 M → 1.07 M |

**The Tab5** (median, slowest window without a mark; core 1's median):
AmiBee 99.9% (99.5%), 42%; Deluxe Galaga 99.5% (88.9%), 89%; Gea Lux
100% (97.1%), 31%; Leading Lap 99.6% (94.3%), 90%; Virtual Karting 2
99.9% (99.6%), 85%: the same as with links alone within the runs'
spread. Deluxe Galaga's credits, in the four windows that hold the same
screenshots and profiles in both runs: 90.1, 92.2, 87.3, 91.1% → 92.8,
94.4, 90.1, 93.1%; the dispatcher in its profile 4.1% → 2.2% of core 1.
A trip costs the board less than the ~180 instructions reckoned (~0.3
µs). The dispatcher entered 43.5 M → 24.8 M times over the pass.

**What's left in Deluxe Galaga's credits** (core 1 at 100%): translated
code 34%, the chips 34% (Lisa's fetch 7.5%, Paula 6.9%, the line's end
5%), collisions 12% (`pf_line()` 6.3%, `bits32()` and `collide()` ~3%
each: worked out on every line while it reads CLXDAT), `memcpy` 4%. Its
later slow windows (~89-91%) do little on the CPU. So its next lever is
the collisions, not the CPU.

## Writes beside code (2026-11-28)

Leading Lap's profile had `jit_written()` at 3.0-4.8% of core 1, Deluxe
Galaga's 1.2-1.3%, Virtual Karting 2's 1.7%. **Counted on the host**
(`AMI_JITSTAT`'s write lines, Leading Lap's slow stretch, frames
6290-7927):

- 1.39 M CPU writes came to `jit_written()` (their 2 KB has code: ~850 a
  frame), 450000 of them into a 256-byte page with code. Each walked the
  page's list: 14.1 M blocks, 31 a write (a block starts at every branch
  target and runs on up to 16 instructions, so many overlap). Not one
  write touched code: the game's variables sit between its routines
  (`move.w d0,$2208fe` beside code ending a few bytes before).
- Masks of the code's bytes in pieces of 2, 4, 8 and 16 bytes would
  leave 0, 5843, 10867 and 263450 of those writes walking. So a bit a
  word.
- The link exits that came back unlinked (~33 a frame) were not drops
  (there were none): 75679 of 85431 went to code not yet translated, the
  working set made again after each of the stretch's 8 arena flushes (64
  runs in the interpreter first). That is the arena's size, not writes.

**The code** (`jit.c`):

- Each page has a mask of the words its blocks cover (128 bits), beside
  its list in the bank's table (5 KB a bank on the board instead of 1
  KB: Leading Lap makes 7 a fill, 28 KB more of the 2 MB arena). A block
  is 256 bytes at most (`MAX_BYTES`), so it lies on one or two pages and
  is on both lists: a page's list holds every block over it, and the
  mask is exact.
- A block's words are set as it goes on a list; a dropped block's page
  gets its mask made again from the blocks left.
- `jit_written()` looks at the mask's bits for the written words and
  walks the list only if one is set.
- `jit_cpu_wrote()` (the CPU's write of 1-4 bytes in one page) does it
  all itself, a leaf: 18 instructions back for a page without code, ~40
  with the mask, the walk a tail call.

**Checked on the host**: in all six stretches no write walks a list any
more (Deluxe Galaga's credits: 339000 writes into pages with code, all
beside it). The collection 96 of 96, the survey 26 of 26 and the replays
4 of 4 the same as the interpreter, with and without the fast path.
`jit_test` now writes over the code at more places: the opcode (of a
one-word instruction: an extension word made an opcode could address
anything), the filler's words and words beside any block, as a word, a
byte or a long (the word before the code and the opcode: only the second
word over a block), twice a case (the second after the first's drops
made the mask again), with the code at a page's end in half the cases
(blocks on two pages). It catches a write's last word left out, the mask
not made again after a drop, a block's second page unmasked, the fast
path's last word left out and its mask from the wrong page. The new
cases first showed two failures that were the test's own: a branch back
into an earlier case's leftover code, and an extension word made an
opcode, read CIA-B's TOD and timer, which the interpreter's run and the
translated run don't share. The code's area is cleared before each case
now: 3 seeds x 100000 cases, none failing.

**The Tab5** (median, slowest window without a mark; links and kept
targets → the masks → the fast path too):

| Title | Before | Masks | Fast path |
|---|---|---|---|
| Leading Lap | 99.6% (94.3%) | 99.7% (96.4%) | 99.8% (96.8%) |
| Deluxe Galaga | 99.5% (88.9%) | 99.6% (92.6%) | 99.7% (92.3%) |
| AmiBee | 99.9% (99.5%) | 100% (99.3%) | 100% (99.0%) |
| Gea Lux | 100% (97.1%) | 100% (96.5%) | 100% (97.9%) |
| Virtual Karting 2 | 99.9% (99.6%) | 99.8% (99.4%) | 99.8% (99.6%) |

- Leading Lap's slow stretch (frames ~6800-8000): 94.3-96.0% → 96.4-99.4%
  → 96.8-100.4%; 4 of 39 windows under 97.5% → 2 of 42 → 2 of 41.
- Deluxe Galaga's credits, the four windows holding the same screenshots
  and profiles in each run: 92.8, 94.4, 90.1, 93.1% → 92.8, 94.1, 90.3,
  93.4% → 93.9, 94.9, 92.2, 94.6%. Its windows under 97.5% 10 → 10 → 7.
- The profiles: `jit_written()` 3.0-4.8% of Leading Lap's core 1 → 1.4%
  → `jit_cpu_wrote()` 0.5%; Deluxe Galaga's 1.2-1.3% → 1.3% → 0.5-0.6%;
  Virtual Karting 2's 1.7% → 0.7% → under 0.4%. With the masks alone the
  calls themselves were the cost (~850 a frame).
- Left of the writes: `m68k_write_memory_16` ~1% (the translated code
  sends a write in a 2 KB with code to it, the interpreter's too).

**Next**: Leading Lap's arena (25 flushes in 15000 frames on the host,
each followed by its working set run 64 times in the interpreter and
made again): denser code (done otherwise: "Slices resumed inside
blocks"). Deluxe Galaga: its collisions.

## Slices resumed inside blocks (2026-11-28)

The plan was denser code: Leading Lap still flushed its 2 MB arena 25
times in 15000 frames on the host. **Measured first** (`AMI_JITSTAT`'s
new lines, the slow stretch at frames 6290-7927, the code without hooks:
the board's):

- 108 bytes of arena a 68k instruction: the instructions' own work 28,
  the exits' records 19, the block's header 9, the handlers' table 8 (kept
  for every instruction), the fetch's charges 8, the checks after each
  instruction 6, the jumps to the shared exit 5, the cycles 5, the exits'
  flags 4, the rest 16. Records in 8 bytes, the table only for handlers
  and a smaller header would have saved ~20%, the P4's compressed
  instructions about as much again.
- But the stretch's blocks held 99658 bytes of 68k code over 20214
  distinct bytes: each instruction in ~5 blocks. Its working set (each
  block made in the stretch once): 3449 blocks, 3.2 MB. 2838 of them (2.6
  MB) began where a slice had started (1136) or at an instruction the
  interpreter ran on to from there (1702); 608 at a branch's, link's or
  computed exit's target.
- A slice ends at any instruction (the CPU's slice runs to the next event:
  ~480 a frame). The dispatcher entered blocks only at their start, so the
  next slice ran the interpreter from there, and every PC it met heated:
  after each flush every instruction of a hot loop reached 64 runs at
  once, each a block's start with the 15 instructions after it. So no
  smaller encoding would have made it fit.

**The code** (`jit.c`, `jit_rv.c`):

- Each instruction after a block's first has a resume word (`JIT_RES`):
  its body's place in the code and the translation's state there: s5's
  instruction, the pending flags' kind and size.
- The exits a slice may resume after (a slice's, a write's, a handler's)
  enter the shared exit four stores early (`ST_EXIT_RF`): the pending
  flags' registers (s6-s8) and s2 into `jit_rf`. s2 is the block's first
  longword: links may have run on from the block the dispatcher started,
  and the dispatcher finds the block that left from it
  (`exit_block()`). The PC, that block and the registers go into a slot
  by the PC's hash (256, in the arena after the directory).
- A slice starting at a PC with no block but a slot: `jit_rv_resume()`
  finds the instruction's word and checks that the registers make the
  flags in memory what they are, where Musashi's tests look
  (`flags_match()`; wait.c's passes may have changed the flags: no resume
  then). It fills `jit_rs` and the shared routine `ST_RESUME` runs: the
  prologue, s2, s5, the registers, the opcode's fetch as the fetch state
  says (as a block's entry does), then the body. The host's hook is called
  first (the PC log as before). A link's or computed exit's target gets a
  block of its own when hot (one to link to): it resumes only meanwhile.
- Only code reached by a jump heats: a PC that isn't the next word in
  sequence (`s_ic.next`: a branch's target, an exception's, an exit's to
  a PC its code doesn't run on to), a link's or computed exit's flow, or
  code after code with no block of its own (an instruction `translate()`
  failed at: Virtual Karting 2's across a 64 KB bank at $24FFFC; an exit
  past its block, after a handler at the block's end). A slice resumed or
  run on to in sequence never starts a block.

**Checked on the host**: the collection 96 of 96, the survey 26 of 26 and
the replays 4 of 4 the same as the interpreter. `jit_test` runs each case
again in four slices, each the next resuming the block where the last
ended (each slice starts with cycles, as `m68k_execute()` does: the test
first added them to a slice already over, and the translated side ran an
instruction the interpreter didn't), the flags sometimes changed between
them; ~51000 resumes in 20000 cases, 3 seeds x 100000 cases none failing.
It catches a resume without the flags' registers, without the flags'
check, without the opcode's fetch, and with the pending kind left out.
(`jit_heat_all` heats every PC there: every case is translated at once.)

The six stretches (the build with the masks → resumes):

| Stretch | Translations | Flushes | Interpreted | RISC-V run | Resumed |
|---|---|---|---|---|---|
| Deluxe Galaga 3133-3938 | 43 → 4 | 0 → 0 | 2196 → 1316 | 1025 → 1031 M | 377585 |
| Leading Lap 6290-7927 | 8994 → 74 | 4 → 0 | 772390 → 35717 | 1216 → 1249 M | 723709 |
| AmiBee 14015-14266 | 1352 → 16 | 1 → 0 | 157080 → 8236 | 67 → 72 M | 50989 |
| Gea Lux 3850-4089 | 1633 → 135 | 1 → 0 | 203516 → 54672 | 33 → 38 M | 15623 |
| Virtual Karting 2 13084-13341 | 17 → 1 | 0 → 0 | 617 → 283 | 275 → 277 M | 89018 |
| Deluxe Galaga 10000-11400 | 1713 → 177 | 1 → 0 | 155770 → 102015 | 262 → 261 M | 182711 |

Leading Lap's working set in its stretch 3.2 MB → 62 KB; its whole run
(15000 frames) 74925 → 3542 translations, 33 flushes (22 full) → 3 (1
full), 6.43 M → 1.22 M instructions interpreted. The RISC-V instructions
run grow by what the interpreter no longer runs (and ~25 a resume).

**The Tab5** (median, slowest window without the pass's pictures and
profiles; core 1's median):

| Title | Before | Resumes |
|---|---|---|
| Leading Lap | 99.8% (96.8%), core 1 87% | 99.8% (99.5%), core 1 79% |
| Deluxe Galaga | 99.7% (92.3%), 88% | 99.6% (94.1%), 85% |
| AmiBee | 100% (99.0%), 41% | 99.9% (99.5%), 40% |
| Gea Lux | 100% (97.9%), 29% | 100% (98.9%), 30% |
| Virtual Karting 2 | 99.8% (99.5%), 83% | 99.9% (99.5%), 80% |

- Leading Lap's slow stretch (frames ~6800-8000): 96.8-100.4% →
  99.6-100.0%; its windows under 97.5% 3 → 0.
- Deluxe Galaga's credits (the windows with the pass's pictures and
  profiles): 95.4, 93.9, 94.9, 92.2, 94.6% → 94.6, 93.9, 94.5, 91.5,
  94.5%, the same within the runs' spread: collisions.
- The board's counters over the pass: 150600 → 17900 translations, 76
  flushes (52 full) → 24 (4 full), 13.9 s → 1.9 s translating, 3.1 s →
  0.4 s syncing the caches; 15.3 M slices resumed.
- The profiles: in Leading Lap the interpreter (2.7% of core 1) and the
  translation (2.1%) gone. Where slices resume often (Deluxe Galaga's
  credits, Virtual Karting 2) the dispatcher costs ~2.5 points more
  (`jit_run` +1.5, `jit_rv_resume` ~1%: two lookups an exit for its block,
  the slot, the resume word's search), the translated code ~3-4 points
  less.

The records in 8 bytes, the handlers' table, the header and compressed
instructions: not needed now (no flush in the six stretches).

**Next**: Deluxe Galaga's collisions; the handlers left. The dispatcher's
resume could carry its block in the exit's record (the pc, ppc and fe
words' top bytes are free) instead of two lookups.

## The resumes' records (2026-11-28)

The resumes cost the dispatcher two lookups an exit (the block that left,
from s2) and a search of the block's resume words a resume. An exit's
record now says both: ppc holds a 24-bit address (pc and fe can't give
their top bytes: a JMP to an address with its top byte set keeps it in
the PC, and fe reaches $1000000 at the top of the address space), so its
top byte and the record's spare byte's top half give the words back from
the record
to its block's header (12 bits: 16 KB; further, the lookups as before),
the spare byte's low half the instruction a slice may resume at after
it (a slice's exit: the PC's instruction; a handler's: the next one).
`exit_state()` masks ppc; `jit_rv_resume()` takes the record's
instruction when its address is the PC's, else looks for it.

**The host**: the six stretches the same in every count (translations,
instructions interpreted, RISC-V run, resumes), the record's instruction
right at every resume, the dispatcher's lookups 32-65% fewer (Leading
Lap's stretch 2.64 M → 0.97 M, Deluxe Galaga's credits 1.30 M → 0.50
M). The collection 96 of 96, the survey 26 of 26 and the replays 4 of 4
the same as the interpreter. `jit_test` caught ppc unmasked; a wrong way
back to the block only stops the resumes (correct, slower), so a run of
1000 cases or more with none fails now.

**The Tab5** (Deluxe Galaga and Virtual Karting 2): the profiles at the
same frame (the pass's fixed one, 3250): `jit_rv_resume()` 1.2% → 0.8% of
core 1 in Deluxe Galaga's credits, 1.1% → 0.9% in Virtual Karting 2;
`jit_run()` 3.5% → 3.5% and 3.3% → 2.9%. Half a point, inside the runs'
spread: Deluxe Galaga 99.8% (94.5%, frame ~9970 in play; 2 windows under
97.5% as before), Virtual Karting 2 99.9% (99.3%). Deluxe Galaga's game
differs from run to run on the board (its pictures at the pass's fixed
frames differ), and core 1's median in play with it (83-91%).

## The divides and the long multiply (2026-11-28)

The handlers the translated code still called in the six stretches
(`AMI_JITSTAT`'s handler lines): Leading Lap's DIVS.W and DIVU.W between
data registers (162 a frame; DIVU.W (d16,An) ~5% of them), Virtual
Karting 2's MULS.L/MULU.L, DIVS.L/DIVU.L and its bit fields (323 a
frame), Deluxe Galaga's DIVS.W (23 a frame), and the privileged ones
(RTE, MOVE to and from SR, ORI to SR). Each call stored the pending
flags, went through `jit_fallback()`, and the next opcode's fetch came
from the fetch state in memory.

**The code** (`jit_rv.c`), each as Musashi's 64-bit handler does it:

- DIVU.W, DIVS.W by a data register or an immediate: div and rem. A
  quotient past 16 bits sets V alone (the pending flags to memory first,
  N, Z and C as they were); else Dn gets the quotient and the remainder,
  N from bit 15, Z the quotient, V and C clear, to memory too (both ways
  meet with no flags pending). First version: the overflow by the
  handler too, and out of the block after it; Leading Lap's DIVU.W D7,D6
  overflows a quarter of its runs, the slice couldn't resume after it
  (the flags not what the next instruction's code expects) and its
  stretch interpreted 25% more.
- MULU.L, MULS.L from any data operand (its extension word fetched
  first): mul and mulh(u); the 32-bit form's V when the product doesn't
  fit, the 64-bit form's N its top bit and Z both longs; C clear; the
  flags to memory; Dh before Dl.
- DIVU.L, DIVS.L of a 32-bit dividend by a register or an immediate: div
  and rem ($80000000 / -1 as Musashi's 64-bit arithmetic: $80000000, no
  overflow), the remainder before the quotient (Dr = Dq keeps the
  quotient), N and Z pending.
- A zero divisor (the trap) and DIVS.W's $80000000 / -1 (Musashi's own
  result) call the handler from the translated code (`handler_case()`)
  before anything changed or was fetched past the opcode (an immediate's
  fetch after those checks: the division touches no bus), then leave the
  block. A divisor in memory (its read would come twice), a 64-bit
  dividend (no 64-bit division on RV32) and the bit fields stay handlers.

**The host**: the handler calls in Leading Lap's stretch 279940 → 22129,
Virtual Karting 2's 98950 → 46574 (its bit fields left), Deluxe
Galaga's later stretch 38173 → 5373; translations, interpreted
instructions and resumes as before. `jit_test` makes these instructions
(a register or immediate divisor mostly, zero or $80000000 / -1 at
times, after a CMP.L at times: flags pending) and caught six planted
errors: a signed remainder taken unsigned, the multiply's V left clear,
the quotient written before the remainder, the handler's path without
the pending flags, an overflow without them, an immediate's fetch
charged before the handler's cases (the handler's path and the fetch
only once the generator put a CMP.L before the division at times and
chose register and immediate divisors mostly: no flags were ever
pending as a division began, and an immediate divisor came up once in
~1300 cases). The collection, the survey and the replays the same as
the interpreter. (The commit's message says seven: six.)

**The Tab5** (Leading Lap, Virtual Karting 2, Deluxe Galaga; the
profiles at the pass's fixed frame): `jit_fallback()` 0.4% → 0.1% of core
1 in Leading Lap and Virtual Karting 2, Musashi's handlers in Virtual
Karting 2 0.5% → 0.0%. Leading Lap 99.9% (99.3%), Virtual Karting 2
99.8% (99.5%), Deluxe Galaga 99.8% (97.2%, 2 windows under 97.5%): the
same within the runs' spread, as expected of half a point.
