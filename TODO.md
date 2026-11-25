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
17, an A1200 load survey on the Tab5).

## Next work

## 18. Skip the wait loops the poll skip misses

Why: the A1200 survey (item 17, `docs/a1200-load-survey-2026-11-25.md`)
found titles falling behind while most of their instructions are a
two-instruction wait:

- Soccer Kid in play, 82%: `tst.w $d05ba.l` / `beq`.
- Starbase 13 70%, The Lost Prince 42%, Christmas Breakdown 31%:
  `cmp.l (-$86,A5),D0` / `beq`.
- Deluxe Galaga in play, 59%: `tst.w ($1742,A6)`.
- Fatman, 74-99%: `cmpi.b #$EC,(6,A6)` on VHPOSR, and `btst #4,$10043c.l`.
- Black Viper, 64-99%: `cmpi.b #$64` and `#$F4,$dff006.l` above $20000,
  and `btst #0,$bfdd00.l` (CIA-B's ICR).
- Deluxe Pacman 20%, Virtual Karting 2's race 13%.
- Zero Gravity, 8%: `cmp.b $bfe801.l,D0` (CIA-A's TOD).

What: work as `poll_wait()` does. Memory changes from outside the CPU only
where a slice ends:

- an interrupt starts a slice;
- the blitter runs at once at a write, the CPU's or the copper's, and that
  write ends the slice;
- disk DMA moves at the line's end and at DSKLEN.

So a loop of a read-only memory test and a Bcc back to it can skip to the
slice's end while it will loop. Find it at the branch: a taken Bcc to the
instruction just before it, the test decoded once per PC. Not in the read
callbacks, where a test is paid on every access (the A500's lesson).

Two cases need care:

- VHPOSR's other compares and addressing modes. First find why the
  broader gate broke Warm Up, Ziriax and Tactical Manager 2
  (`docs/poll-vhpos-experiment-2026-11-11.md`).
- CIA reads. ICR clears as it is read, and a timer whose interrupt is off
  isn't an event: a loop on one may skip only up to the timer's
  underflow.

Checked:

- The survey's slow titles again on the board: speed and core 1, with
  `build/a1200-load-2026-11-24/board_pass.py`.
- The collection and the replays as before, or the same scenes a frame
  apart, as when the skip came in item 8.
- SysInfo the same, Kid Pool's title as before.

Size: a session or two.

## 19. A faster 68EC020

Why: item 9's assembler waited for a title that falls behind, and item 17
found several. Titles that compute all frame, without waiting, run 24-56k
instructions a frame. Core 1 manages ~15-20k at full speed: about 1 µs an
instruction, with the chipset's ~5 ms. Their speeds:

- Deluxe Galaga's instructions screen 48%.
- Virtual Karting 2 50-54%.
- Leading Lap 57-65%.
- Tactical Manager 2 64%.
- Gea Lux 65-70%.
- AmiBee in play 54%.
- Charr's menu 84%, Metal Kombat's 3D intro 83%.

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
  - Then RISC-V with PIE where C runs out (`../esp32emu/CLAUDE.md`, "To
    do").
- The chips' share, where AGA titles show many planes:
  - `memcpy` of the line records, up to 18% (Megaball).
  - Collisions while a title reads CLXDAT, 9% (Deluxe Galaga).
- Do it after 18, so the loops don't hide the gain.

Done so far:

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
