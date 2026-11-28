# esp32ami: to do

In order: "what's next" is the first item here. Each one says why, where,
how, how it's checked, and roughly how big it is. Done items move to
`docs/architecture.md` (Progress, "The to-do list") and leave this list;
the others keep their numbers (done: 1, an RP9 setting up its machine;
2, the blitter's timing slot by slot; 3, the 68000 on the chip bus slot by
slot; 4, Paula's attach modes; 5, ECS super-hires; 6, writing extended
ADFs; 7, AGA's untested and missing parts; 8, the 68EC020's instruction
cache; 10, titles from the smoke runs; 11, the Voyage prefetch, CIA and
floppy follow-up; 12, Workbench's display changes on the Tab5; 13, DMS
through the live web page; 14, title replays that tell floppy from memory
failures; 15, uploads while the A1200 runs; 16, faster Tab5 downloads;
17, an A1200 load survey on the Tab5; 18, wait loops without the CPU; 20,
an RP9's disks in two drives; 21, Hyperion's reads and the drive's speed).

## Next work

## 19. A faster 68EC020

Why: item 9's assembler waited for a title that falls behind, and item 17
found several. Titles that compute all frame, without waiting, run 24-56k
instructions a frame; the interpreter managed ~15-20k at full speed. With
the translating core (`docs/jit-design-2026-11-27.md`) these still fall
behind in their slowest stretches (median, slowest; `board_pass.py`):

- Deluxe Galaga 99.7% (92.3%: its later windows, the chips'; its
  credits, an idle loop that polls the joystick and keyboard through the
  OS, 92-95%, collisions 12% of core 1 there).
- Leading Lap 99.8% (96.8%).
- Gea Lux 100% (97.9%), AmiBee 100% (99.0%), Virtual Karting 2 99.8%
  (99.6%).
- (Metal Kombat's 82.8% was the board pass's own: a screenshot and a
  profile in that window, a disk insert in the next slowest; otherwise
  99.4% or more.)

What: fewer P4 instructions per 68020 instruction.

- Where the time goes now (the titles that compute): the loop 18-36% of
  core 1 (28 P4 instructions around an instruction), the handlers and
  Musashi's helpers 20-32% (spread over ~400 handlers: no handful worth
  special-casing), the extension words 4-11%, the data callbacks 5-12%,
  Lisa's fetch 5-7%.
- Core 1 retires 0.62-0.78 P4 instructions a clock, the same in the slow
  stretches as elsewhere, and where the code lives barely matters
  (internal RAM: 1-2 points; the TCM: 25-40% slower). So the cost is the
  instruction count.
- The C side is near its floor (the handlers' document, "What is left").
  Its two small items are done: the extension-word reads always inline
  (no change beyond the spread); `mem.c`'s `s_m` can't be reached from gp
  (the linker wants the whole 3 KB object in the window).
- The translating core (`docs/jit-design-2026-11-27.md`): stage 1 (the
  machinery) and stage 3's first batch are done: the commonest families
  native in RISC-V, the rest by their handlers, flags pending, the board
  running it (see "Done so far"). Measured in the slow stretches
  (2026-11-27, the document's "Where the slow stretches spend it"):
  handlers run 0.2-0.9% of the instructions there (0.3-2.7% of core 1),
  while a block's entry and exit cost ~93 RISC-V instructions a run
  (24-61% of the translated code's, 54% in Deluxe Galaga's credits at
  3.9 instructions a block run) plus the C dispatcher's 8-10% of core 1,
  and 60-84% of the exits go to a fixed PC whose block exists (linked
  since: see "Done so far"; the computed exits keep their targets too).
  Next, in that order:
  - Denser code (Leading Lap still flushes its 2 MB arena 25 times in
    15000 frames on the host: each flush is followed by its working set
    run 64 times in the interpreter and made again, which is most of its
    link exits that come back unlinked: 75679 of 85431 in its slow
    stretch; the exits' records smaller, the P4's compressed
    instructions).
  - The handlers left, last: Virtual Karting 2's BFINS, MULS.L and DIVS.L
    (~350 a frame), Leading Lap's DIVS.W and DIVU.W (~160). Handlers are
    esp32emu's Musashi: the ST and the Macs must be built and checked if
    it changes.
  - Deluxe Galaga: collisions are 12% of core 1 in its credits (worked
    out on every line while it reads CLXDAT: `pf_line()`, `bits32()`,
    `collide()`), and its later slow windows (~89-91%) run ~6700
    instructions a frame: the chips', not the CPU's. A line with no
    sprite armed and the playfields' bit already set (or not enabled)
    needs no work; profile the later windows on the board first.
- PSRAM: with the A1200's 8 MB of fast RAM and the 2 MB arena ~2.2 MB are
  free with two floppies in. A high-density ADF's 1.72 MB buffer fits; an
  extended ADF's 2.5 MB may not in one piece, even with the arena given
  back (its hole isn't next to the rest: 5 MB free, no 2.5 MB block, seen
  once). Extended ADFs are OCS games' (A500: no arena); if one is wanted
  on the A1200, size its buffer from the file (and its growth) or take
  the arena's 2 MB for it.
- Not levers: this P4 (revision 1.x) tops out at 360 MHz; a 512 KB L2
  cache needs 256 KB of internal RAM; code in internal RAM or the TCM;
  extension words read inline (step 10's window, a wash; reading the
  loop's own state instead saves ~3 instructions a word on average, at
  695 call sites).
- Board runs take 40 minutes a build: measure steps too small to show
  above the runs' spread (a point or two) together, and tell them apart
  by the profiles.

Done so far:

- Writes beside code (2026-11-28, `jit.c`, `docs/jit-design-2026-11-27.md`,
  "Writes beside code"): each page has a mask of the words its blocks
  cover; a write looks at the mask before the page's blocks, the CPU's
  in `jit_cpu_wrote()` alone. Measured first: Leading Lap's writes into
  pages with code walked 31 blocks each (14.1 M in 1637 frames) and
  never touched code. Its slowest window 94.3% → 96.8%, its slow stretch
  94-96% → 97-100%; Deluxe Galaga's credits 1-2 points; `jit_written()`
  3.0-4.8% of Leading Lap's core 1 → 0.5%. `jit_test` writes at more
  places and sizes, twice a case, with blocks on two pages.

- The computed exits' targets (2026-11-27, `jit.c`, `jit_rv.c`,
  `docs/jit-design-2026-11-27.md`, "The computed exits' targets" and
  "kept targets"): RTS and JSR/JMP (An) compare the PC with their last
  two targets (kept after their record by the dispatcher) and jump into
  the matching block. Measured first: Deluxe Galaga's credits 99% the
  last target, elsewhere RTS 53-76% the last, 84-94% one of two. The
  dispatcher's trips a frame in the credits 2292 → 625 (615 of them
  slice ends), 43.5 M → 24.8 M over the board's pass; the credits' windows
  2-3 points faster (90.1-92.2% → 92.8-94.4%), the dispatcher there 4.1%
  → 2.2% of core 1; the other titles the same within the runs' spread.

- Blocks reaching each other (2026-11-27, `jit.c`, `jit_rv.c`,
  `docs/jit-design-2026-11-27.md`, "Blocks reaching each other").
  - An exit whose flow goes on to a fixed PC (a branch out, BSR, JSR or
    JMP to an address, a block's end) is linked to that PC's block the
    first time it comes back to the dispatcher: its two words become a
    jump to the block's entry (one for an opcode reached by a jump, one
    in sequence), past the prologue and the dispatcher. Each exit's
    record follows its code (the block returns its address); a dropped
    block's entries lead back to the dispatcher.
  - The dispatcher's trips 63-84% fewer in the slow stretches (Deluxe
    Galaga's credits 9542 → 2292 a frame), the RISC-V instructions run
    9-24% fewer; on the board the dispatcher 10.3% → 4.8% of core 1 in
    the credits.
  - Deluxe Galaga's credits ~81% → 90-92%, Leading Lap's slowest 90.2% →
    94.5%, AmiBee's 94.5% → 99.5%, Virtual Karting 2's 96.9% → 99.4%,
    Gea Lux's 95.6% → 96.8%; core 1's medians 1-10 points lower.

- A larger arena (2026-11-27, `jit.c`, `docs/jit-design-2026-11-27.md`,
  "The larger arena").
  - 2044 KB, the shared routines (1408 bytes) in its middle: a jump
    reaches them from either end. Smaller (1 MB, 512 KB, 256 KB) while
    PSRAM is short, 512 KB left to the rest; given back when a disk's
    buffer can't be had otherwise (`ami_malloc_big()`), taken again at the
    next translation.
  - The PSRAM for it: the page table per 64 KB bank, made in the arena as
    code lands there (256 KB before); the Kickstart and AROS's extension
    loaded straight into their working copies (1 MB of copies gone,
    `rom_store.c` checking both before writing either); a high-density
    ADF's buffer its own size (1.72 MB, not 2.5). 2229 KB free with the
    A1200's 8 MB of fast RAM, the 2 MB arena and two floppies (1841 KB
    with the 1 MB one).
  - Leading Lap 95.6% → 99.4% (its slowest 86.3% → 90.2%); on the host
    238000 → 72000 translations and 211 → 33 flushes in 15000 frames.
    Virtual Karting 2 the same (99.6%, 96.9%), Christmas Breakdown 99.9%
    (99.4%) with its HD disk in. The board's counters over the two
    titles' runs (from power-on): 271000 → 93000 translations, 233 → 46
    flushes, 22.4 s → 8.0 s translating.

- The handlers' side (2026-11-26, `mem.c`, esp32emu's Musashi,
  `docs/faster-020-handlers-2026-11-26.md`).
  - The data callbacks without a stack frame (the 68EC020's odd-address
    splits out of line): a fast RAM word write 29 → 21 P4 instructions.
  - Musashi's state within the global pointer's reach (`.sdata.m68k`,
    the 68030 MMU's page cache apart): one instruction, not two, to reach
    it in every handler.
  - `m68ki_get_ea_ix()`'s brief format saves three registers, not five;
    the address-error check tests the address before the CPU type.
  - Virtual Karting 2 78% → 81%, Leading Lap 97% → 99.5%, Deluxe Galaga
    95% → 98%, Gea Lux's slowest 93% → 95%. The A500's core 1 up to 4
    points lower (X-Balls 84% → 80%).

- The 68EC020's own loop (2026-11-26, `mem.c`, esp32emu's
  `CONFIG_M68K_HOST_LOOP`, `docs/faster-020-loop-2026-11-26.md`).
  - Musashi's quick loop hands runs of instructions to `m68k_host_run()`,
    the opcode's fetch inline: 28 P4 instructions around an instruction
    (a second word), 36 (a cached first word), against 42 and 50.
  - A branch's target in the same 64 KB bank is read inline too.
  - Lisa's fetch copies two words a step into two hashes.
  - The line keys' hash folds its top half back (`mix()`): Deluxe
    Pacman's 64-pixel sprites left stale lines before.
  - Virtual Karting 2 70% → 78%, Leading Lap 91% → 97%, Deluxe Galaga 91%
    → 95%, Gea Lux's slowest 83% → 93%. The A500 titles no slower.

- The chips' share (2026-11-26, `display.c`,
  `docs/faster-020-2026-11-26.md`).
  - Collisions: each line's playfield matches worked out once, not for
    every 16 pixels of every sprite (Deluxe Galaga 11-15% of core 1 →
    5-6%).
  - The line records point to an earlier record's copy of the palette
    while it is the same (`memcpy` 7-13% → 1-4%).
  - Charr's menu → 100% (core 1 88%), Leading Lap 79% → 90-91%, Deluxe
    Galaga's median 82% → 91%, Virtual Karting 2 66% → 70%. The A500
    titles no slower.
  - Tried and not kept: the hottest 13 KB in internal RAM (1-2 points),
    7.9 KB in the TCM (25-40% slower).

- Musashi's work around each instruction (2026-11-25, esp32emu).
  - The 68000's prefetch moved into `mem.c`, so the 68EC020 no longer
    pays for it.
  - A quick dispatch loop runs while no trace bit and no debugger is
    active.
  - Extension words are read inline.
  - The dispatch loop went from 34 to 24 P4 instructions an instruction.
  - Speed gains of 4-16%: Charr 85% → 95%, Christmas Breakdown → 100%,
    Gea Lux in play 73% → 85%. The A500 is unchanged.
- Longer waits skipped (2026-11-25): loops of up to 16 instructions worked
  out a pass at a time (the beam wait through VPOSR as a long, compiled
  flag waits). Tactical Manager 2 65% → 100%, AmiBee in play 53% → 95-99%,
  Leading Lap and Virtual Karting 2 a few points.
- A cheaper fetch (2026-11-25, `mem.c`): 17 P4 instructions a second word
  and 25 a cached first word (19 and 28); 2-4 points of speed in the
  titles that compute.
- The wait loops skipped (item 18, 2026-11-25): the titles that only
  waited run at full speed, so what is left is the CPU's own cost.

- The 68EC020's instruction fetch through a pointer (2026-11-10, ~7%).
- The 68000's bus cycles inline: Kick Off 2's credits went from 16.7 to
  9.9 ms of core 1 a frame (`docs/bus-cycle-experiment-2026-11-10.md`).
- Kid Pool's VHPOSR wait skipped
  (`docs/poll-vhpos-experiment-2026-11-11.md`).
- The A500 survey (2026-11-11, `docs/architecture.md`, "Speed in C"):
  none below ~98%.

Checked:

- The survey's table again.
- The A500 titles no slower. Profile an A500 title too: a test in front of
  the 68000's path costs every A500 game.
- The collection's pictures the same.

Size: a project, each step measured on its own.

## 9. Waiting on something

Nothing here can be done as things are; each says what it waits for.

- The clock's saved offset keeps a clock set while the board's time was
  wrong (unlikely with the Tab5's RTC): a remark, nothing to do.
- The 68000's bus cycles where a real one has them: the slot model takes
  an instruction's back to back from its start (SysInfo 1.00, xSysInfo
  1.06 times an A600); xSysInfo's 1.00 needs each bus cycle's place in the
  instruction (a bus-cycle 68000 core, not Musashi): a project of its
  own. Then the 2 clocks a 68000 on an odd colour clock waits could come
  back (`mem.c`: they made two loaders fail with the back-to-back cycles).
  It would also place Hyperion's reads where a real A500 does: they land
  within a word or two of a sector's second sync word, and its load still
  fails ~9% of the time with the drive's speed varying (item 21,
  `docs/hyperion-2026-11-26.md`).
- Sprites on lines inside the window without bitplanes: Denise (WinUAE)
  wants a BPL1DAT write on a line before showing them; here they show.
  Needs a game or a test on a real Amiga that shows the difference.
