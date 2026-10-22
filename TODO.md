# esp32ami: to do

In order: "what's next" is the first item here. Each one says why, where,
how, how it's checked, and roughly how big it is. Done items move to
`docs/architecture.md` (Progress, "The to-do list") and leave this list;
the others keep their numbers (done: 1, an RP9 setting up its machine;
2, the blitter's timing slot by slot; 3, the 68000 on the chip bus slot by
slot; 4, Paula's attach modes; 5, ECS super-hires; 6, writing extended
ADFs).

## 7. AGA: the untested and the missing

- **Untested** (no game in the collection uses them): FMODE 2 fetches,
  32-pixel sprites, super-hires in AGA's colours. A boot disk per case
  (`tools/agaboot.py`): a known pattern, the expected pixels worked out by
  the tool, compared with the host's picture.
- **Missing**: sprites' 35 ns positions (super-hires, SPRES), CLXCON2
  (planes 7 and 8 in collisions), the wide sprites' collisions.
- **Size**: medium; nothing needs them yet.

## 8. The 68EC020's instruction cache

- **Why**: every access to chip RAM waits `WAIT_CHIP` (5 clocks),
  instruction fetches too. A real 68EC020 runs loops from its 256-byte
  cache without the bus, so code in chip RAM (most A1200 games: no fast
  RAM) has different timing; SysInfo's 1.03 is an average that fits.
- **How**: `M68K_SEPARATE_READS` (instruction fetches through their own
  callbacks), a 64-longword direct-mapped cache with CACR's enable and
  clear (Musashi has CACR); a hit doesn't wait.
- **Check**: SysInfo 4.4 on the A1200 near 1.0; Aerial Racers' and
  Deluxe Galaga's speed by eye.
- **Size**: medium; changes every A1200 game's timing.

## 9. Small things

- Sprites outside the vertical window: shown (and colliding) here; does
  OCS Denise hide them? Settle from the documentation and UAE.
- The Overscan editor's "Edit Text Size..." from a super-hires Workbench
  (A600, 3.1, host) never opens its screen; from a hires one it does.
  Find what its task waits for (it may be how AmigaOS is; a hires screen
  over a super-hires one is then still to be tried by eye).
- DMS modes 2-5: written after xDMS, untested (no archive uses them).
- The clock's saved offset keeps a clock set while the board's time was
  wrong (unlikely with the Tab5's RTC).
- The 68000's bus cycles where a real one has them: the slot model takes
  an instruction's back to back from its start (SysInfo 1.03, xSysInfo
  1.06 times an A600); 1.00 needs each bus cycle's place in the
  instruction (a bus-cycle 68000 core, not Musashi). Then the 2 clocks a
  68000 on an odd colour clock waits could come back (`mem.c`: they made
  two loaders fail with the back-to-back cycles).
- Assembler (RISC-V, PIE) for hot paths: only if a game falls behind
  (Aerial Racers' race: core 1 73-81%).
