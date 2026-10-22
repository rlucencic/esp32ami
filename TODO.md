# esp32ami: to do

In order: "what's next" is the first item here. Each one says why, where,
how, how it's checked, and roughly how big it is. Done items move to
`docs/architecture.md` (Progress, "The to-do list") and leave this list;
the others keep their numbers (done: 1, an RP9 setting up its machine;
2, the blitter's timing slot by slot; 3, the 68000 on the chip bus slot by
slot).

## 4. Paula's attach modes

- **Why**: not done at all. ADKCON's bits 0-3 (USExV) send channel x's
  words to channel x+1's volume, bits 4-7 (USExP) to its period; with both,
  the words alternate volume and period. The attached channel itself is
  silent.
- **How**: `audio.c`: a word channel c fetches (DMA) while attached goes
  to `ch[c + 1].vol` and/or `.per` instead of playing; channel 3's bits
  have no partner.
- **Check**: Datastorm is the collection's only user (ADKCON $15DF from
  frame 687: bits 0-4, 6, 7; maybe its loader's leftovers): `AMI_WAV`
  before and after, the user listens. A tone disk
  (`tools/attachboot.py`: channel 0 sweeping channel 1's period) for the
  exact behaviour.
- **Size**: small (~40 lines).

## 5. ECS super-hires: the rest

- **Why**: Workbench in super-hires works (the user went through every
  ScreenMode mode on the Tab5); what isn't there: dual playfield in
  super-hires (2 planes drawn as one 4-colour playfield in `draw_shres()`'s
  ECS path; AGA's has it), BPLCON1's scroll there (taken as lores pixels, 4
  super-hires ones: check the units), sprites' 70 ns positions, and a lores
  or hires screen dragged down over a super-hires one (lines of both in a
  frame: the frame is drawn 1440 wide, its other lines doubled; not tried).
- **How**: compare the behaviour with the ECS documentation and UAE (not
  copied); dual playfield as the AGA path does it with ECS's colours.
- **Check**: Workbench 3.1 in PAL super-hires on the A600 (the recipe in
  CLAUDE.md, Testing), a lores screen opened and dragged down (by eye,
  host and Tab5); a boot disk for dual playfield and scroll
  (`tools/shresboot.py`).
- **Size**: medium.

## 6. Writing extended ADFs

- **Why**: UAE-1ADF images (raw MFM tracks) are read-only. Son Shu Shi
  (both disks) is the collection's only one; whether it writes (high
  scores) isn't known.
- **How**: `floppy.c`: a track written to an extended ADF: AmigaDOS tracks
  (type 0) decoded back into its data as an ADF's; raw tracks (type 1) take
  the written words (one turn's worth; a longer track rewrites the image
  with a new track table at the flush); `flush_drive()` writes the image
  whole.
- **Check**: an extended ADF made from Workbench's ADF (type 0 tracks)
  with the write test (CLAUDE.md, Testing: a startup-sequence that writes
  a file); Son Shu Shi played until it writes, if it does.
- **Size**: medium (~100 lines).

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
