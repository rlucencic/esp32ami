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
instructions a frame. Core 1 manages ~15-20k at full speed: about 1 µs an
instruction, with the chipset's ~5 ms. Since the waits are skipped (item
18, and this item's first step), these are the titles still behind
(`docs/faster-020-handlers-2026-11-26.md`):

- Virtual Karting 2's race 76-81%.
- Deluxe Galaga's credits and instructions ~67% (its median 98%).
- AmiBee's 5000-7000 ~68% (not measured again since 2026-11-25's step 2).
- Metal Kombat's 3D intro down to 67% (not measured again).
- Leading Lap 88% at its slowest (its median 99.5%).
- Gea Lux in play 95% at its slowest.

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
- Next: the translating core (`docs/jit-design-2026-11-27.md`). Stage 1
  is done: the machinery (`jit.c`: blocks, invalidation, exits, the fetch
  timing), every instruction still a handler call; exact on the host (the
  collection, the A1200 survey, the replays), slower on the Tab5 (off in
  the board build). Next, stage 3: native micro-ops for the commonest
  instructions (MOVE, MOVEQ, ADD/SUB/CMP, TST, CLR, LEA, Bcc, DBcc, ...),
  each batch checked exact on the host, then the RISC-V emitter for them
  (PSRAM is executable with `CONFIG_SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION`
  off: `main/jit_probe.c`, the console's `X`). Handlers are esp32emu's
  Musashi: the ST and the Macs must be built and checked if it changes.
- Not levers: this P4 (revision 1.x) tops out at 360 MHz; a 512 KB L2
  cache needs 256 KB of internal RAM; code in internal RAM or the TCM;
  extension words read inline (step 10's window, a wash; reading the
  loop's own state instead saves ~3 instructions a word on average, at
  695 call sites).
- Board runs take 40 minutes a build: measure steps too small to show
  above the runs' spread (a point or two) together, and tell them apart
  by the profiles.

Done so far:

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
