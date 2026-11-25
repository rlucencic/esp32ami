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
instruction, with the chipset's ~5 ms. Since the waits are skipped (item
18, and this item's first step), these are the titles still behind
(`docs/faster-020-2026-11-25.md`):

- Deluxe Galaga's instructions and Game Control screens ~51%.
- Virtual Karting 2's race 59-68%.
- AmiBee's 5000-7000 ~68%.
- Leading Lap 65-75%.
- Gea Lux in play 76-78%.
- Metal Kombat's 3D intro down to 67%.
- Charr's menu 85%.

What: shrink the 68020's path, C first as before.

- Where the time goes (the titles that compute): the fetch 22-35% of core
  1, Musashi's dispatch loop 13-23%, the handlers 9-21%, data memory
  5-23%. The samples spread evenly over the code: the cost is the
  instruction count, ~150 P4 instructions a 68020 instruction.
- Candidates, all in esp32emu's Musashi (the ST and the Macs built and
  checked too):
  - Musashi's prefetch bookkeeping around each fetch (~10 P4 instructions
    a word): the 68000 needs it (Voyage), the 68EC020 doesn't.
  - The dispatch loop's per-instruction work: the trace flag's store and
    test, the debugger's map, the cycle table's lookup.
  - The handlers run from flash through the cache ($480xxxxx): the hottest
    in IRAM, if internal RAM allows (~110 KB free, 40 KB at its low point).
  - Then RISC-V with PIE where C runs out (`../esp32emu/CLAUDE.md`, "To
    do").
- The chips' share, where AGA titles show many planes:
  - `memcpy` of the line records, up to 18% (Megaball).
  - Collisions while a title reads CLXDAT, 9% (Deluxe Galaga).
- Not levers: this P4 (revision 1.x) tops out at 360 MHz; a 512 KB L2
  cache needs 256 KB of internal RAM.

Done so far:

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

## 21. Hyperion's reads and the second sync word

Why: Hyperion's loader seeks without end when one of its ~130 track reads
starts at a sector's header. That happens when DSKLEN is written within the
sector's second sync word (32 µs): the DMA then syncs on the second
`$4489`, sector 0 has no sync in the 6656-word buffer, and its copy at the
end is cut off. The loader starts each read at a phase that drifts
deterministically, so whether it hits the window is a coin toss on
clock-level timing: item 18's build fails with a key tapped at frame 208
and item 19's passes, and the other way round without it
(`docs/faster-020-2026-11-25.md`). The collection flips on it after any
timing change.

What: find out what a real Amiga does differently. A real Paula has the
same window, so look at the drive instead:

- its speed varies (300 rpm within about 1%), which makes each read's
  phase random rather than locked;
- WinUAE's DSKLEN and sync handling, compared against ours.

Model the speed variation deterministically (per turn, from the turn's
number) if that is what makes the window rare.

Checked: Hyperion through its load with key taps at frames 200-208 on the
host, every one to its title; the collection; the replays; 9 Fingers and
Lethal Xcess (the other loaders sensitive to the disk's phase).

Size: a session.

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
