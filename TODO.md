# esp32ami: to do

In order: "what's next" is the first item here. Each one says why, where,
how, how it's checked, and roughly how big it is. Done items move to
`docs/architecture.md` (Progress, "The to-do list") and leave this list;
the others keep their numbers (done: 1, an RP9 setting up its machine;
2, the blitter's timing slot by slot; 3, the 68000 on the chip bus slot by
slot; 4, Paula's attach modes; 5, ECS super-hires; 6, writing extended
ADFs; 7, AGA's untested and missing parts; 8, the 68EC020's instruction
cache; 11, the Voyage prefetch, CIA and floppy follow-up).

## 10. Titles that still fail (the smoke runs)

Why: found running the user's demos and game ADFs with `tools/smoke.py`
(CLAUDE.md, Testing and Open items); each could be a fault of ours.
Check each with `smoke.py` on its folder (`ONLY=<name>`) and the
collection for the rest. Roughly a session each, in this order:

- Under Pressure (A1200 demo), paused 2026-10-24 at the user's request:
  reproduced on `3db07b0`, with its RP9's A1200, Kickstart 3.1, 2 MB
  chip + 8 MB fast RAM, no input and `AMI_TIME=1729641600`. Previous
  observation: fails with chip waits of 7-8 clocks, works at 5; the cause
  is still unproven. Its manifest requests a maximum-speed 68020.
  The first trapped negative starting Y is at frame 4553: entry to the
  software line drawer `$23F25C`, endpoints (60,-3) and (57,28), buffer
  A2 `$31A7D8`. At `$23F268`, `mulu.w D5,D1` treats a negative Y as
  unsigned, producing an address far beyond the buffer. At frame 4628,
  line 165, `st (A0)` at `$23F28A` writes through A0 `$D1AF00`, a custom
  mirror, setting BPLCON0 to `$FFFF`; exception 3 follows at frame 5462.
  Next: trace the projected coordinates produced by `$23F29C` into
  `$5FA89C` (source `$30AC0C`, called at `$23CB8A`), then drawn by the
  call at `$23CB9C`. Establish why they leave the screen: geometry or
  instruction behavior, missed timing/scene transition, or the demo's
  CPU-speed assumption. No blitter/copper/VBL race has been established;
  do not lower the global wait just to hide it.
  Local evidence: `build/underpressure-20261024/` contains `old.log`,
  `bad.log`/`bad.bin` (16 MB dump), `probe.log` (first negative Y),
  `stop.log`, pictures, copied RP9/system disks, and temporary instrumented
  sources/binaries. No emulator source was changed. Reproduce with
  `AMI_HD0="$PWD/build/underpressure-20261024/title.rp9" AMI_TIME=1729641600
  AMI_EXCSTOP=3:400 components/amiga/host/build/ami_host roms 5500
  build/underpressure-20261024/repro.ppm` (join onto one shell line;
  always run from a copy of the disks).
- All New World of Lemmings (AGA crack, no fast RAM): an OS hook at
  $C6720 gets overwritten, then a jump through it every ~14 frames once
  Space is pressed at the title. Find the writer: a host write watch
  (`AMI_WATCH=<addr>`, to add) would show it; then whether a real A1200
  without fast RAM would do the same.
- Killer (A1200, marked for a JIT): quits at once. Check whether it
  wants a 68030 or an FPU (out of scope then).
- Pango Fun (a beta): a Guru on every machine; see whether the beta is
  just broken (another dump, WinUAE's behaviour described anywhere).

## 9. Waiting on something

Nothing here can be done as things are; each says what it waits for.

- DMS modes 2-5 (quick, medium, deep, heavy 1): written after xDMS,
  untested. Needs an archive packed in one of them (none in the
  collection; no DMS packer at hand).
- The clock's saved offset keeps a clock set while the board's time was
  wrong (unlikely with the Tab5's RTC): a remark, nothing to do.
- The 68000's bus cycles where a real one has them: the slot model takes
  an instruction's back to back from its start (SysInfo 1.03, xSysInfo
  1.06 times an A600); 1.00 needs each bus cycle's place in the
  instruction (a bus-cycle 68000 core, not Musashi): a project of its
  own. Then the 2 clocks a 68000 on an odd colour clock waits could come
  back (`mem.c`: they made two loaders fail with the back-to-back cycles).
- Assembler (RISC-V, PIE) for hot paths: only if a game falls behind
  (Aerial Racers' race: core 1 71-78% with the 68EC020's cache and the
  poll skip).
- Sprites on lines inside the window without bitplanes: Denise (WinUAE)
  wants a BPL1DAT write on a line before showing them; here they show.
  Needs a game or a test on a real Amiga that shows the difference.
