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
17, an A1200 load survey on the Tab5; 18, wait loops without the CPU).

## Next work

## 19. A faster 68EC020

Why: item 9's assembler waited for a title that falls behind, and item 17
found several. Titles that compute all frame, without waiting, run 24-56k
instructions a frame. Core 1 manages ~15-20k at full speed: about 1 µs an
instruction, with the chipset's ~5 ms. Since item 18 skips the wait
loops, these are the titles still behind (the survey's rerun,
`docs/wait-loops-2026-11-25.md`):

- Deluxe Galaga's instructions screen 47%; in play 81-98%.
- Virtual Karting 2 50-60%.
- AmiBee in play 52-69%.
- Leading Lap 57-65%.
- Gea Lux in play 63-74%.
- Tactical Manager 2 65%.
- Metal Kombat's 3D intro down to 67%.
- Charr's menu 83%.
- Christmas Breakdown and Zero Gravity in play ~91%.

What: shrink the 68020's path, C first as before.

- Where the time goes:
  - Musashi's instruction words through the cache model take 17-36% of
    core 1: `m68k_read_immediate_16`, and `m68ki_read_imm_16/32` another
    3-9%.
  - Its dispatch, `m68k_execute`, takes 10-21%.
- Candidates:
  - Check the cache once per longword or per run of words in one line,
    not per word.
  - Read immediates straight through `s_ic.next_word`.
  - The wait check's cost in titles that compute: about 1.5 points of
    Christmas Breakdown's play (`docs/wait-loops-2026-11-25.md`). Try a
    cheaper first look at the PC as a line begins.
  - Then RISC-V with PIE where C runs out (`../esp32emu/CLAUDE.md`, "To
    do").
- The chips' share, where AGA titles show many planes:
  - `memcpy` of the line records, up to 18% (Megaball).
  - Collisions while a title reads CLXDAT, 9% (Deluxe Galaga).
Done so far:

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

## 20. An RP9's disks in two drives

Why: the runtime keeps a file in one drive (esp32emu's `disks.c`).
Inserting a package into DF1: to get its second disk takes it out of DF0:,
and the boot then comes from disk 2 (Sword's Shell, Leading Lap's insert
screen, item 17). Inserting the package again in the same drive gives its
next disk, so one drive works.

What: let a package be in two drives, each with its own disk. The disk
number is already per drive (`d->rp9_disk`), and the unpacked copies are
separate files (`.<name>.rp9.<n>.adf`). An exception to the one-file rule
in `disks.c` for packages would do it; check how the other projects use
the rule first.

Checked: Sword's package in DF0: and DF1: on the Tab5 boots to its title
with disk 2 in DF1:, as the host does with its disk 2.

Size: small, in esp32emu and `floppy.c`.

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
- Sprites on lines inside the window without bitplanes: Denise (WinUAE)
  wants a BPL1DAT write on a line before showing them; here they show.
  Needs a game or a test on a real Amiga that shows the difference.
