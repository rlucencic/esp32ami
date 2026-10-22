# esp32ami: to do

In order: "what's next" is the first item here. Each one says why, where,
how, how it's checked, and roughly how big it is. Done items move to
`docs/architecture.md` (Progress) and leave this list.

## 1. An RP9 sets up the machine its manifest names

- **Why**: the user's "nice to have". The games name their machine: 63
  an A500 (59 with Kickstart 1.2), 3 an A500+ (Ants: ECS, 1 MB chip RAM),
  4 an A2000, 25 an A1200, 1 an A4000. Today the setup screen has to match
  by hand before the package goes in.
- **The manifest**: `<system>` (a-500, a-500plus, a-2000, a-1200,
  a-4000), the Kickstart as `<rom type="system">130</rom>` or
  `<systemrom>310</systemrom>`, sometimes `<ram type="fast|chip|z3">bytes</ram>`.
  `rp9.c` keeps the system and the Kickstart, not the RAM yet.
- **How**: one mapping in `components/amiga` (`rp9_machine()`: model,
  Kickstart, chip/slow/fast RAM), used by `main.c` and the host:
  a-500: A500, 512 + 512 KB, Kickstart 1.2 (110, 120) or 1.3;
  a-500plus: A500 with 1 MB chip RAM (ECS follows), no slow RAM;
  a-2000: A2000; a-1200 and a-4000: A1200, Kickstart 3.0 or 3.1, fast RAM
  from `<ram type="fast">` (z3: the most our Zorro II board has, 8 MB).
  A package going into the boot drive (DF0:, or DH0: for a hard disk
  game) from the setup screen or the web page, naming another machine
  than the settings: save the settings as the console's `R` does
  (`settings_model_defaults()`, Kickstart, RAM, `settings_save()`),
  `emu_restart()`, the disk stays in (the drives are in NVS), a message
  says what changed. The host: `AMI_DF0`/`AMI_HD0` with an RP9 sets the
  machine the same way unless `AMI_MODEL` is given (`tools/collection.py`
  then drops its own mapping).
- **Decide with the user**: automatic, or a button on the drive's line
  ("Set up as A500+, 1 MB chip, Kickstart 1.3").
- **Check**: host: the collection with the manifest's machine gives the
  pictures `tools/collection.py` gives now; Ants reaches its menu without
  settings. Tab5: Ants' package into DH0: on an A1200 restarts as an
  A500+ and reaches its menu; Aerial Racers' into DF0: on an A500 restarts
  as an A1200.
- **Size**: small to medium (~150 lines).

## 2. The blitter's timing slot by slot

- **Why**: phase 4's rest. A blit is done at once and BBUSY and its
  interrupt wait `8 + cck * 2` clocks: its words times the colour clocks a
  word takes with its channels (the HRM's table), whatever else the line
  does. A real Agnus gives the blitter only the slots left by refresh (4),
  disk (3), audio (4), sprites (16), the bitplane fetch and the copper: with
  6 lores or 4 hires planes there are few in the fetch window, and blits
  last longer. Games that wait for the blitter, or count on it being slow,
  see it end early here.
- **How**: a count of each line's slots free for the blitter (227 or 228
  colour clocks less the fixed ones, the fetch's from `display_fetch()`, the
  copper's from the instructions it ran); `blit.left` (the slots it still
  needs, from the table) goes down by that at each line's end from the
  line it started in; BBUSY until it reaches 0, the end time worked out
  within that line. The 68000's share (`steal`, BLTPRI) as now.
- **Where**: `blitter.c` (`start()`, `blitter_busy()`, the end event),
  `amiga.c` (the frame loop's events), `display.c` (the fetch's slots).
- **Check**: a boot disk (`tools/blitboot.py`, as `laceboot.py`) timing one
  blit in lines with 0, 4 and 6 lores planes, against the HRM's figures;
  the collection before and after (pictures that move: look at them); Kick
  Off 2 and Aerial Racers as today.
- **Size**: medium (~150 lines and the test disk). Item 3 builds on its
  slot count.

## 3. The 68000 on the chip bus, slot by slot

- **Why**: the DMA's even slots cost the 68000 an average (`SLOT_WAIT`, 3
  clocks a slot, owed as `ami.stall`, at most 4 lines). SysInfo 4.4 gives
  1.00 times an A600 in chip RAM, xSysInfo 0.12 1.09: a slot-exact bus
  brings both to 1.00 and timing-sensitive code closer.
- **How**: each line's slot map (item 2's count, kept as a 228-bit mask:
  refresh, disk, audio, sprites, the fetch; the copper's as it runs; the
  blitter's), and each 68000 access to chip RAM or the chips at its time
  (`ami_now()` plus 4 clocks a bus cycle into the instruction: a counter
  reset by the instruction hook) waits for the next free even slot (a
  count of trailing zeros over the mask), the wait taken from Musashi's
  remaining cycles as the 68EC020's are. Fast RAM and the ROM: no wait
  (as now).
- **Where**: `mem.c` (the 68000's chip and custom banks), `amiga.c`
  (`ami.stall` goes), `display.c`, `copper.c`, `blitter.c`.
- **Check**: SysInfo and xSysInfo on an A500 with 1.3 and an A600 (the
  recipe in CLAUDE.md, Testing): 1.00 and 1.00; Test Kit's CIA timers;
  the collection; the Tab5's core 1 in Kick Off 2 (56% now) with room.
- **Size**: large (~250 lines). After item 2.

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
- Assembler (RISC-V, PIE) for hot paths: only if a game falls behind
  (Aerial Racers' race: core 1 73-81%).
