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
  A1200's 8 MB of fast RAM; 2 MB since, "The larger arena").

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
