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

- Under Pressure (A1200 demo): its line drawer writes past its buffer
  into the custom registers at frame ~4600 with the chip waits at 7-8
  clocks (fine at 5). Find what the loop races against (the blitter, a
  copper or VBL flag) and which of our timings decides it.
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
