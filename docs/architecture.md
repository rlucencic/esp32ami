# esp32ami: architecture review

An Amiga 500, 2000 and 600 on the M5Stack Tab5 (ESP32-P4), on the shared
runtime in `../esp32emu`; the A1200 as a later phase. This is the plan and
the decisions made before any code, as `../esp32pc/docs/architecture.md`
was for the PC, and the progress since (at the end).

## Summary

- **The Tab5 only** (the user's call, 2026-10-20): the MaTouch's S3 runs
  the ST's 68000 alone at 77-85% of real time with its second core at
  ~90%; the Amiga's custom chips add a scanline's worth of work on top. The
  P4 runs the same 68000 at 8 MHz in 36% of a core and the Mac II's 68020
  at full speed. `CMakeLists.txt` stops other targets, as esp32arc's does.
- **First the A500, A2000 and A600** (OCS/ECS, 68000 at 7.09 MHz PAL);
  the **A1200** (AGA, 68EC020 at 14 MHz) afterwards, if the P4 has the
  speed for it.
- **Two floppy drives** (DF0:, DF1:) with **ADF** images on every model,
  and **hard disks** (HDF) on every model.
- **Kickstart** from the user's own files (Amiga Forever, FS-UAE's set):
  in `roms/` (not in git), flashed to their own partitions, as the ST's TOS
  and the Archimedes' RISC OS are. AROS's free ROM comes the same way (the
  user has it in the same set); none is distributed.
- **Our own chipset** (MIT), written from the Amiga Hardware Reference
  Manual and the chips' documented behaviour; UAE, WinUAE, FS-UAE, vAmiga
  and PUAE (GPL) only to compare behaviour against, never copied.
- **Accuracy target: games and Workbench, not cycle-exact demos.** Chip
  memory access accounted per scanline, the copper's register writes placed
  at their horizontal positions, the blitter exact in its results and
  realistic in its busy time.

## 1. What esp32emu provides

- **Musashi** (`components/musashi`, MIT): the 68000 the ST and the Macs
  run, the 68020 the Mac II and LC run, the 68030 (with MMU) and 68881 of
  the Mac LC III. Its CPU model is fixed at build time
  (`M68K_FIXED_CPU_TYPE`, for speed): one firmware is a 68000 or a 68020.
  The A1200 phase decides between a runtime CPU type (slower dispatch) and
  a second build (see 11).
  - Memory through callbacks with the ST's direct-pointer fast paths;
    `m68k_use_table_copies()` keeps the opcode table in PSRAM;
    `M68K_VECTOR_CALLBACK`, `m68k_bus_error()`, the halt callback.
  - The debugger's 68000 adapter (`components/emudbg`): breakpoints,
    stepping, memory, disassembly from VS Code over WiFi.
- **The runtime** (`components/emu`): the emulation task on core 1 paced
  by the speaker's I2S clock, the presenter on core 0 scaling the
  machine's picture (`render_line()`, `screen_signature()`), the setup
  screen (up to six choice rows; five fit best), the drives
  (`disks_config_t`: up to five, `accepts()` per drive, blank images,
  NVS memory), the web page (uploads, `/machine.js` converters and
  packers), Bluetooth and USB keyboards and mice, touch, sound (the Tab5's
  ES8388, 44.1 kHz stereo), the console.
- **Lessons that carry over**: the PC's picture copies and per-line
  register-change log (copper effects are the same problem), the ST's
  cartridge trap for a native device driver, the PC's option-ROM trap,
  the Archimedes' ADF handling (a different format, the same plumbing),
  the PC's sound clock lesson (no 32.32 of an ever-growing count).

## 2. The machines

| | A500 | A2000 | A600 | A1200 (later) |
|---|---|---|---|---|
| CPU | 68000, 7.09 MHz (PAL) | 68000, 7.09 MHz | 68000, 7.09 MHz | 68EC020, 14.19 MHz |
| Chipset | OCS (ECS Agnus in later boards) | OCS / ECS Agnus | ECS (Agnus 8375, Denise 8373) | AGA (Alice, Lisa) |
| Chip RAM | 512 KB (1 MB with ECS Agnus) | 512 KB or 1 MB | 1 MB (2 MB) | 2 MB |
| Other RAM | 512 KB "slow" at C00000 (trapdoor) | Zorro II fast RAM | fast RAM as a PCMCIA card or accelerator gave it | fast RAM (32-bit) |
| Kickstart | 1.2, 1.3 (256 KB) | 1.3 | 2.05 (37.350), 3.1 | 3.0, 3.1 |
| Disks | DF0:, DF1: (external) | DF0:, DF1: | DF0:, IDE (Gayle) | DF0:, IDE (Gayle) |
| Extras | | RTC, Zorro II slots | Gayle | Gayle, AGA |

- The **A500+** (ECS, Kickstart 2.04, 1 MB chip, RTC) is the A500 with
  ECS chips and that ROM: a Kickstart choice rather than a model.
- **PAL** first (50 Hz, 313 lines of 227 colour clocks, 3.546895 MHz;
  the CPU at two clocks a colour clock); **NTSC** (60 Hz, 262/263 lines,
  3.579545 MHz) as a choice: the beam counters, the CIA TOD rates, ECS's
  BEAMCON0.

## 3. The custom chips

### Time and the scanline scheduler

One scanline at a time, as the ST's frame loop and the PC's do:

1. **The fixed DMA of the line** from the registers as they are: refresh
   (4 slots), disk (3), audio (4), sprites (16), bitplanes (DDFSTRT,
   DDFSTOP, BPLCON0's depth and resolution: in hires with 4 planes they
   take every slot of the display window).
2. **The copper** in the free even slots: MOVE, WAIT (horizontal and
   vertical comparison with the masks, the blitter-finished bit), SKIP;
   its writes are logged with their horizontal position (step 5 applies
   them at those pixels).
3. **The blitter** in what's left (or before the CPU with BLTPRI, "blitter
   nasty").
4. **The CPU** gets the rest: a 68000 bus cycle every other colour clock,
   so bitplane DMA in hires slows code running from chip RAM, and code in
   fast RAM runs on. The cycles it can't have are the line's wait states
   (the ST's `cpu_line_cycles` arithmetic, generalised).
5. **Denise**: at the line's end, what she needs to draw it goes into the
   line's record (the fetched words, BPLCON0-2, the window, the colours as
   the line began and each copper or CPU colour write with its position);
   the presenter on core 0 draws the records: the bitplanes through lookup
   tables (planar to pixel indices), playfield priorities and dual
   playfield (BPLCON2), extra half-brite, HAM6, the eight sprites (phase
   3), the colours changed mid-line at their pixels. Recording where the
   beam is keeps the racing-the-beam effects right. (Phase 1 first drew
   the pixels on core 1; measured, that cost too much: see Progress.)
6. **Paula's audio** for the line (each channel one word at most), the
   **CIAs** (timers in E-clock ticks, 709 kHz; TOD counters from vertical
   and horizontal sync), the **disk** DMA.

Events finer than a line (a CIA timer underflow, the blitter's end, the
copper's wait) cut the CPU's line into pieces, as the PC's event list
does.

### Agnus

- Beam counters (VPOSR, VHPOSR, VPOSW), the long/short frame bit, the
  vertical blank interrupt.
- DMACON (master and channel enables, BBUSY, BZERO), the address
  registers of every channel, modulos.
- **The copper**: COP1LC/COP2LC, COPJMP strobes, the danger bit (COPCON).
- **The blitter**: all 256 minterms, the A and B shifts, first/last word
  masks, ascending and descending, inclusive and exclusive fill, line
  mode (with single-dot), BZERO. Results immediately (exact), BBUSY held
  for the blit's real duration (words x the channels' cycles), and its
  interrupt at the end; fast paths for the common minterms (copy, clear,
  the cookie-cut 0xCA, fill).
- ECS: BEAMCON0 (PAL/NTSC, programmable sync), 1 MB/2 MB chip RAM
  addressing, the BPLCON3 border bits.

### Denise

- Bitplanes 1-6 in lores, 1-4 in hires, interlace (alternate fields),
  scrolling (BPLCON1), the display window (DIWSTRT, DIWSTOP).
- Sprites from sprite DMA or written directly, their control words, the
  collision register CLXDAT worked out when it's read (from the line's
  sprite and playfield masks), CLXCON.
- ECS Denise: super hires (1280 pixels: such frames drawn 1440 wide, one
  to one on the Tab5, see CLAUDE.md), the border blank bits, DENISEID.

### Paula

- **Interrupts**: INTENA/INTREQ to the 68000's levels 1-6 (the CIAs'
  INT2/INT6 through it).
- **Audio**: four channels from DMA or AUDxDAT, period (from 124: ~28.6
  kHz), volume (0-64), the attach modes; mixed at 44.1 kHz in stereo (0
  and 3 left, 1 and 2 right), the A500's fixed 4.5 kHz filter and the
  power LED's switchable one (CIA-A).
- **Disk**: DSKLEN (the double write that starts it), DSKSYNC (0x4489),
  word sync, DSKPT, ADKCON (MFM, precompensation ignored), DSKBYTR, the
  DSKBLK and DSKSYN interrupts.
- **Serial** (SERDAT, SERPER) with nothing on the other end at first;
  **POTGO/POTxDAT** for the mouse's right button and the joystick's
  second button.

### The CIAs (two 8520s)

- Timers A and B (one-shot, continuous, counting E-clocks or underflows),
  TOD (CIA-A from the vertical sync, CIA-B from the horizontal: the
  graphics.library's timing), the serial shift register (the keyboard on
  CIA-A, with its handshake), the alarm, the interrupt control register.
- Ports: CIA-A PRA (the fire buttons, the drives' /RDY, /TK0, /WPRO,
  /CHNG, the power LED, OVL), CIA-B PRB (the drives' step, direction,
  side, select lines, motor); the parallel port (nothing attached).

## 4. Memory and the map

| Address | What |
|---|---|
| 000000 | Chip RAM (512 KB - 2 MB), temporarily overlaid by ROM after reset. CIA-A port A controls OVL on the A500/A2000. The first CIA-A write clears Gayle's reset overlay on the A600/A1200. |
| 200000 | Zorro II fast RAM (autoconfig: up to 8 MB) |
| A00000-BEFFFF | Nothing (the ST's lesson: unmapped space must behave as the real board's) |
| BFD000, BFE001 | CIA-B (even addresses: the data bus's high byte), CIA-A (odd addresses: the low byte), each register every 256 bytes |
| C00000 | Slow RAM (the A500's trapdoor 512 KB) |
| D80000-DFFFFF | Gayle (A600/A1200: DA0000 IDE, DE1000 ID), the RTC (DC0000), custom chips (DFF000, or DF0000-DFFFFF on the A1200) |
| E80000 | Autoconfig space (our hard disk board's ROM, fast RAM boards) |
| F80000 / FC0000 | Kickstart (512 KB, or 256 KB mirrored) |

- One PSRAM block for chip, slow and fast RAM, allocated first at power
  on (the ST's lesson); 32 MB leaves plenty.
- Musashi's fast paths for chip, slow and fast RAM and the ROM (direct
  pointers); custom chips, CIAs and Gayle through the slow path. The CPU's
  chip RAM accesses are counted for the line's contention (step 4).

## 5. Disks

### Floppies

- **ADF** (901,120 bytes: 80 cylinders, 2 sides, 11 sectors of 512),
  read and written in place on the SD card, as the ST's `.ST` and the
  Archimedes' ADF are.
- The track Paula reads is made on demand from the ADF's sectors in the
  Amiga's own format (sync words, header, checksums, MFM): trackdisk.device
  and the games' own loaders both read it, since the DMA and MFM are
  emulated, not the DOS. Writes are decoded from MFM back into sectors.
- The drive: 300 rpm (an index pulse every 200 ms on CIA-B's FLAG), step
  timing, the disk-change line, write protect (a read-only file), the
  motor; two drives (DF0:, DF1:). Real speed first; a faster option later
  (as the ST's 10x) if loading proves painful.
- `.adz` (gzip) and `.dms` unpacked to ADF in the browser
  (`main/web/amifiles.js`, the runtime's converter hook), as the Mac's
  StuffIt files are. Copy-protected originals (IPF/SPS, CAPS library) are
  out of scope.

### Hard disks

- **HDF** images: a whole disk with an RDB, or a single partition.
- **A600** (and the A1200 later): **Gayle's IDE**, which Kickstart 2.05
  (37.350) and 3.1 drive with their own scsi.device: the images as real
  IDE drives (the PC's `hdc.c` shows the task-file behaviour).
- **A500 and A2000**: Kickstart 1.3 has no IDE driver, but it boots from
  autoconfig boards (since 1.3): our own small **hard disk board** in
  autoconfig space, whose ROM holds a tiny exec device (`ami_hd.device`)
  that traps to native code for its reads and writes (the ST's cartridge,
  the PC's option ROM). The same board works on every model and Kickstart.
- Two hard disk drives (DH0:, DH1:). Folders on the SD card as drives (a
  filesystem handler, as UAE's) are a later idea, not in the first scope.

## 6. Kickstart

- `roms/` (gitignored), from the user's FS-UAE set
  (`~/Documents/FS-UAE/Kickstarts`): `amiga-os-120.rom` (1.2, 33.180),
  `amiga-os-130.rom` (1.3, 34.5), `amiga-os-204.rom` (2.04, 37.175),
  `amiga-os-205-a600.rom` (2.05, 37.350), `amiga-os-310-a600.rom` (3.1,
  40.63), `amiga-os-300-a1200.rom` (3.0, 39.106),
  `amiga-os-310-a1200.rom` (3.1, 40.68), AROS (`aros-20250422.rom` and
  its `-ext`, 46.x). All checked (the ROM's checksum, its version).
- Flashed to partitions by `idf.py flash` (esp32arc's
  `esptool_py_flash_to_partition`), so the SD card needs none.
- Amiga Forever's encrypted ROMs (`rom.key`) aren't needed: the set is
  decrypted.

## 7. The picture

- PAL overscan: up to ~362 x 283 lores pixels (724 hires), interlace
  doubles the lines. The machine's picture: hires-width lines (lores
  pixels doubled), 283 lines doubled (interlace: both fields), so 724 x
  566 at most, the visible window chosen from DIWSTRT/DIWSTOP's usual
  range rather than the whole beam.
- On the Tab5's 1280 x 720: 4:3 at 960 x 720 (the PC's and the
  Archimedes' window), the presenter's smooth scaling. `screen_signature()`
  changes when a frame does.

## 8. Input

- **Keyboard**: HID usages to Amiga raw key codes, sent through CIA-A's
  serial port with the keyboard's handshake (and its power-up codes,
  reset warning on Ctrl-Amiga-Amiga = Ctrl+both GUI keys).
- **Mouse** (port 1): Bluetooth/USB mice and touch (drag moves, tap the
  left button, long press the right) into JOY0DAT's counters; buttons
  through CIA-A and POTGO.
- **Joystick** (port 2): the numeric keypad and cursor keys with a fire
  key, switched by a key (the CPC's F12 way); a Bluetooth gamepad if the
  runtime gains one.

## 9. Configuration (the setup screen)

Five rows, as the PC's:

```
Model      [ A500 ] [ A2000 ] [ A600 ]
Kickstart  the model's: A500/A2000 [ 1.3 ] [ 2.04 ] [ 3.1 ] [ AROS ];  A600 [ 2.05 ] [ 3.1 ] [ AROS ]
Chip RAM   [ 512 KB ] [ 1 MB ] [ 2 MB ]          (2 MB: ECS only)
More RAM   A500: [ none ] [ 512 KB slow ] [ + 4 MB fast ] [ + 8 MB fast ];  A2000: fast 0/2/4/8 MB;  A600: [ none ] [ 4 MB fast ]
Video      [ PAL ] [ NTSC ]
```

- Choosing a model sets its defaults (A500: 1.3, 512 KB + 512 KB slow,
  PAL; A2000: 1.3, 1 MB, 2 MB fast; A600: 2.05, 1 MB). NVS namespace
  `ami`.
- Drives: DF0:, DF1: (ADF), DH0:, DH1: (HDF): four drives (`accepts()`
  by extension).

## 10. Speed

- Budget on the P4's core 1 (400 MHz): the 68000 at 7.09 MHz ~35% (the
  ST's measurement at 8 MHz), the chipset per line (scheduler, copper,
  Denise's 724 pixels through tables, Paula) estimated at 20-40%: an A500
  at full speed with room. Measured early (phase 1), as the PC was.
- The hot loops in internal RAM (`IRAM_ATTR`, the x86 core's `X86_HOT`),
  the blitter's common minterms specialised, Denise's planar conversion
  by tables (one byte of each plane at a time).
- The A1200: the 68020 at 14 MHz twice the CPU work, AGA's 8 planes and
  wider fetches; plausible, to be measured before it's promised.

## 11. Risks and open questions

- **Chipset timing against games**: the blitter's duration, the copper's
  horizontal precision, CPU stalls in hires. Mitigation: per-line slot
  accounting from the start, tests (Amiga Test Kit), then games.
- **Copy protection and custom loaders**: loaders that read MFM through
  the DMA work (it's emulated); protections that need flux-level track
  layouts (IPF) don't.
- **Musashi's fixed CPU type**: the A1200's 68020 beside the A500's 68000
  in one firmware needs a runtime CPU type (the dispatch costs) or two
  builds; decided in the A1200 phase.
- **ECS details** the A600 needs (BEAMCON0, the 2 MB chip RAM, Denise's
  ID) are small; **AGA** is not (Lisa's 8 planes, 256 colours, 24-bit
  palette, wider fetch modes, the 32-bit sprites).
- **Speed**: estimated, not measured; phase 1 measures it.
- **Kickstart 1.3 and the hard disk board**: autoboot works from 1.3 on,
  but an exec device in our ROM is new work (the ST's cartridge is the
  pattern).

## 12. Phases

| Phase | Work | Checked by |
|---|---|---|
| 1 | Project (Tab5 only, ROM partitions), host build; memory map, OVL; CIAs; Paula's interrupts; Agnus's beam, DMA slots, copper; the blitter; Denise's bitplanes | Kickstart 1.3's insert-disk screen on the host, then the Tab5; the speed measured |
| 2 | Floppies: ADF to MFM tracks, the drives' lines, disk DMA, writes | Workbench 1.3 boots; Amiga Test Kit's floppy and memory tests |
| 3 | Keyboard, mouse, joystick; sprites (the pointer); Paula's audio | Workbench used by hand; Test Kit's audio and input tests |
| 4 | Compatibility: blitter timing and line mode, copper precision, dual playfield, HAM, collisions | games (Lemmings, Turrican II, Speedball 2, Sensible Soccer, Shadow of the Beast), Test Kit's video tests |
| 5 | A2000 (RTC, Zorro II fast RAM, autoconfig), A600 (ECS, Gayle IDE), the hard disk board, HDF; web page converters (.adz, .dms) | Workbench 2.05/3.1 installed on an HDF; Kickstart 1.3 booting from DH0: |
| 6 | Speed and polish; NTSC; the debugger's machine view | measured on the Tab5 |
| 7 | The A1200: AGA, the 68020 (the CPU type decision) | Workbench 3.1, AGA games |

Each phase ends built, flashed on the Tab5 and checked, with the
measurements written down.

## Progress

### Phase 1 (2026-10-20): done

- The project (Tab5 only, six Kickstart partitions, `rom_store.c` with an
  SD card fallback by version), the host build, and the core: memory map
  and OVL, the CIAs, Paula's interrupts, the beam and the bitplane DMA,
  the copper, the blitter (area and line modes, fill), Denise's
  bitplanes (dual playfield, EHB and HAM too), the drives' status lines
  (no disks yet). The setup screen's five rows (Model, Kickstart, Chip
  RAM, Slow RAM, Video); fast RAM waits for autoconfig (phase 5).
- **Checked**: Kickstart 1.3 (34.5) reaches its insert-disk hand (after
  ~4.2 s of emulated time), 2.04 (37.175), 2.05 (37.350) and 3.1 (40.63)
  their screens (the copper's rainbow tick, 3.1's disk animation), on the
  host and on the Tab5. AROS stays black: it needs its extension ROM at
  $E00000 (later).
- **Speed on the Tab5** (360 MHz): 100% (50.0 fps). Core 1 busy: 28% at
  Kickstart 1.3's insert-disk screen, 29-38% during 3.1's boot. With every
  line of the picture changing every frame (an experiment): core 1 31%,
  core 0 (the presenter) 21%.
- **Drawing moved to core 0**: first Denise drew each line in RGB565 on
  core 1 into PSRAM frames, hashed for the signature: 86% busy at the
  idle insert-disk screen. Skipping lines whose inputs hadn't changed
  brought that to 33%, but a fully changing picture still took 82%:
  ~15% the drawing, ~29% copying the lines into PSRAM (at the P4's PSRAM
  latency per cache line, not its bandwidth). Line records (a few hundred
  bytes a line, rewritten only when they change) cost core 1 ~3% instead.
- **Found on the way**: ECS's DIWHIGH is needed for Kickstart 3.1 (its
  copper list closes a window at line 2 through it); without it a stray
  line appeared under the text and the picture was a row off.
- Not yet: chip RAM contention (the 68000 never waits for DMA), sprites,
  interlace's two fields, NTSC's long and short lines.

## Needs a decision

1. The scope above: the A500, A2000 and A600 first, the A1200 later
   (decided: yes).
2. Kickstart from the user's set in ROM partitions (decided: yes), AROS
   among them.
3. The accuracy target: games and Workbench, per-line chipset timing, not
   cycle-exact demos.
4. Hard disks: Gayle IDE on the A600, our own autoconfig hard disk board
   on the A500/A2000 (and usable on every model).
5. The setup rows above (Model, Kickstart, Chip RAM, More RAM, Video).
6. MIT for esp32ami, our own chipset code.

## Sources

- Amiga Hardware Reference Manual (3rd edition), the ROM Kernel
  Reference Manuals (the device and autoconfig interfaces), the 8520
  data sheet, the MC68000 User's Manual.
- Keir Fraser's Amiga Test Kit (public domain): hardware tests on a
  bootable ADF.
- For behaviour only (GPL, not copied): WinUAE, FS-UAE, vAmiga, PUAE,
  Minimig.

### Phase 2 (2026-10-21): floppies done; the clock too

- **Floppies**: ADF images in DF0: and DF1: (kept in PSRAM, written tracks
  back to the SD card a couple of seconds after the disk stops), the
  track under the head made in AmigaDOS's MFM format from the image,
  Paula's disk DMA with word sync, the DSKBLK and DSKSYN interrupts, the
  index pulse on CIA-B's FLAG, written tracks decoded back into sectors.
  Real speed (300 rpm). The setup screen's and web page's drives (blank
  disks are unformatted ADFs).
- **Checked**: Workbench 1.3 boots with Kickstart 1.3 to its desktop
  (~65 s, as a real A500; core 1 7-20% busy while it loads), 2.05 and 3.1
  with their Kickstarts to the Workbench window, on the host; 1.3 on the
  Tab5 too. Writing: a copy of the Workbench 1.3 disk whose
  startup-sequence copies a file onto itself, the image read back
  afterwards (`tools/adf.py`): the copy identical, the filesystem's
  checksums right, on the host and on the Tab5 (the image downloaded
  from the web page after the board saved it).
- **Found on the way**: the bitplane fetch counts whole 8-colour-clock
  units, the one DDFSTOP falls in too (Kickstart 1.3's hires screens use
  $3C-$D0: 40 words, not 38; Workbench's text had come out as dots).
- **The battery-backed clock** (the user asked for it on every model;
  planned for the A2000 in phase 5): an MSM6242B at $DC0000, the board's
  time plus an offset kept in NVS when AmigaOS sets it. 1.3's SetClock
  can't count past 1999 (2026 came out as 21-Oct-78): with Kickstart 1.x
  the clock gives the year as 100 + (126), which it counts right; 2.0 and
  later read plain digits. Checked: 1.3 and 3.1 show 21-Oct-26 with the
  right weekday and time (the Tab5 too), and a date set with `SetClock
  SAVE` comes back with `SetClock LOAD` under both.
- Not yet: Amiga Test Kit (needs a keyboard or mouse: phase 3), the
  pointer (sprites: phase 3), a faster floppy option, `.adz`/`.dms` on the
  web page (phase 5).

### Phase 3 (2026-10-21): input, the pointer, sound

- **Keyboard**: key codes through CIA-A's serial port with the handshake,
  the power-up codes, Ctrl-Amiga-Amiga's warm reset; on the board by
  position (Windows/Command as the Amiga keys, F11 Help, Caps Lock
  toggling), F12 switching the cursor keys and keypad to the joystick.
- **Mouse** (port 1) and **joystick** (port 2): counters, buttons, POTGO;
  Bluetooth/USB mice; touch as a touchpad (tap, tap-and-drag, hold for
  the menus).
- **Sprites**: DMA, direct writes, attached pairs, priorities against the
  playfields: Intuition's pointer.
- **Sound**: Paula's four channels with DMA and the interrupts, mixed to
  44.1 kHz stereo with the A500's filters, played through the runtime's
  speaker (which then paces the emulation).
- **Checked**: on the host, Workbench 1.3 used by scripted input (the disk
  opened by a double-click, the Shell opened, `dir` typed and run, `say
  hello` spoken); Amiga Test Kit 1.21's CIA, chipset, memory, keyboard,
  ports, audio, clock and floppy tests all pass (see CLAUDE.md, Testing).
  On the Tab5: the same Workbench steps through the console, `say hello`,
  Test Kit's music; 100% speed, core 1 20% (Workbench) to 37% (Test Kit
  playing four channels).
- **Found on the way**: the right mouse button is read with the pot pins
  driven high (POTGO $FF00): a button must pull them low anyway. Drives
  now take 400 ms to be ready after the motor starts (Test Kit flagged the
  instant /RDY as a Gotek-like drive).
- Not yet: Test Kit's video tests, sprite collisions, touch taps placing
  the pointer where the finger is; heard and felt on the board only by
  the user (the speaker, touch, a Bluetooth keyboard and mouse).


### Phase 4 (2026-10-21): under way, games

- **RP9** (Amiga Forever's packages, the user's game collection): the
  manifest read (title, model, Kickstart, disks by priority, turbo
  floppy), the floppies unpacked beside the package and inserted as ADFs;
  the same package again gives its next disk. Of the collection's 96, the
  65 floppy games for an A500/A500+/A2000 all boot on the host (title,
  intro, menu or their insert-disk screen after 80 s); the 19 with hard
  disk images wait for phase 5, the 25 for the A1200 for phase 7.
- **Kickstart 1.2** (33.180) as a choice on the A500 and A2000 (59 of the
  games name it), with the custom chips mirrored above the slow RAM (1.2
  sizes the slow RAM by that mirror).
- **Floppies**: extended ADFs (raw MFM tracks, read-only); Paula's sync
  bit by bit (custom loaders at any bit offset); the speed decided per
  disk instead of a setup row (the user's call): 8x for AmigaDOS disks and
  RP9s marked turbo, real for the rest (Workbench 1.3's desktop in ~40 s).
- **Fixes from the games**: write-only custom registers read the last word
  on the bus (Dr. Mario), VERTB 8 clocks into line 0 (F1 GP Circuits).
- **The 68000 on the chip bus**: instructions rounded up to 4 clocks
  (bus slots) and the DMA's even slots taken from it (lores planes 5-6,
  hires 3-4, the copper, the blitter; 3 clocks a slot on average).
  Measured with SysInfo 4.4 and xSysInfo 0.12 (A500, Kickstart 1.3,
  512 KB chip + 512 KB slow RAM), the host and the Tab5 alike:

  | | SysInfo Dhrystones | x A600 | xSysInfo Dhrystones | x A600 | its MHz |
  |---|---|---|---|---|---|
  | bare Musashi | | | | 1.46 | 10.29 |
  | + 4-clock rounding | 693 | 1.31 | | 1.40 | 9.26 |
  | + DMA slots, 2 clocks each | 607 | 1.14 | 1241 | 1.23 | 8.13 |
  | + 3 clocks a slot (now) | 564 | 1.06 | 1144 | 1.14 | 7.46 |

  Both programs open 3-plane hires screens (their own DMA takes every
  other even slot in 212 lines). A real A600 is 1.0 by definition; the
  rest of the gap is the average model (a slot-exact one would close it).
  The 65 games boot as before (21 pictures differ: the same screens at
  another moment); Test Kit's CIA timers pass.
- **The setup screen's rows scroll** (the shared runtime, the user's
  request): five rows at a three-row card's height instead of squeezed.
- **The copper's wake-up cycle** after a WAIT (found with Test Kit's video
  test F1): the WAIT after `WAIT $FFDF` had passed at once in line 255, so
  what PAL programs put below line 256 came 43+ lines early or not at
  all. Fixed, seven of the games show their lower parts (Kick Off's
  panel, Mega Motion's menu's last row, T-Racer's last high score, Over
  the Net's picture, Pac-Mania's whole screen ...).
- **Collisions** (CLXCON/CLXDAT): sprites against sprites and the
  playfields, odd planes against even, worked out per line only while a
  program reads CLXDAT; checked with boot disks made by
  `tools/clxboot.py` (each case's value from the HRM's rules).
- **Test Kit's video tests**: all six draw (gradients and the PAL/NTSC
  frame, the checkerboards, uniform colours and bars, dots, crosshatch).
- **Past the titles** (host, the same 65 with a click, fire, Space and
  Return at 80-116 s, pictures at 140 s): most reach their game or menus
  (Arkanoid, Blasteroids, Face Off, Hammer Boy, Hyperion's 3D, Krypton
  Egg, Mad Professor Mariarti, Pengo, Rage, Statix, T-Racer, Tip Off,
  Toobin, Warm Up, X-Balls, Zarcan ...); the rest ask for their next disk
  or wait for a particular key. Looked into: Lethal Xcess's rainbow is
  its own "insert disk 2 and click" screen (with disk 2: its menu);
  Exile waits for a click with the display off, then plays; Powerball and
  Tie Break were between screens (Tie Break loads at real speed).
- Not yet: blitter timing per slot; games played by hand on the board.

### Phase 5 (2026-10-21): done

- **Autoconfig and fast RAM**: the expansion boards at $E80000
  (`autoconf.c`), the first one fast RAM (1, 2, 4 or 8 MB, the setup
  screen's own Fast RAM row beside Chip RAM and Slow RAM; the A2000's
  default 2 MB). Kickstart 1.3 configures it at $200000 and adds it to the
  free memory. The 68000 doesn't wait for the chips' DMA while it runs
  from fast RAM or the ROM: SysInfo 4.4 gives 1.00 times a B2000 in fast
  RAM and 1.00 times an A600 in chip RAM; xSysInfo 1.00 (B2000) and 1.09
  (A600), its fast RAM 2.92 MB/s.
- **Gayle's IDE and hard disks** (`gayle.c`): the A600's port (on the
  other models too while a hard disk is in), ATA drives on HDF images,
  DH0: on the setup screen and the web page (RP9 packages with a hard disk
  go there, unpacked once). Kickstart 3.1 on an A600 boots Amiga Forever's
  workbench-135.hdf to Workbench 1.3.5 on the Tab5 (2.05 mounts it too).
  Found: Gayle swaps the bus's bytes (IDENTIFY's words arrive swapped), the
  ROMs' scsi.device only ever uses the master, SD writes need aligned
  buffers (the unpack: 20 s to 3.3 s).
- **The RP9 games' hard disks** are data volumes (AF-Application) for
  Amiga Forever's system disk: with workbench-135/311.hdf beside the
  package, DH0: is one disk of both (`hdf.c`: an RDB of ours, the system's
  partitions bootable, the game's after). Checked on the host and the
  Tab5: Workbench 1.3.5 boots from it and runs Hannibal's user-startup,
  which 3.1's shell stops ("not executable": the E bit 1.3 ignored);
  Football Glory (3.1, an A1200 game) loads and runs with a black picture.
  Bare partitions (no RDB) get our RDB too and mount alone.
- **Our own hard disk board** (`hdboard.c`, Kickstart 1.3 and 2.04: no
  IDE driver in them): an autoconfig board with a boot ROM (a little 68000
  code, `tools/hdrom.py`) calling a native device driver; it loads the
  RDB's filesystem for 1.3 (FFS isn't in its ROM). Checked: an A500 with
  1.3 boots Workbench 1.3.5 from workbench-135.hdf (host and Tab5), 2.04
  too, and Hannibal's RP9 runs to its title and menu on the Tab5 (100%,
  core 1 30%). All six 1.3 RP9 games with a hard disk start on the host
  (Ants needs its manifest's A500+: ECS, 1 MB chip RAM). An A600 with 3.1
  still boots through Gayle, unchanged.
- **The web page's converters** (`main/web/amifiles.js`, the runtime's
  machine script): DMS archives (every track's checksum checked) and
  gzipped images unpacked in the browser. The collection's 27 DMS
  archives unpack to their ADFs (two of its ADFs are other disks); a .dms
  and an .adz went through the board's page in Chrome and came back
  identical.
- **Blank hard disks** (20, 50, 100 MB: an RDB with one partition of the
  old filesystem's type, formatted on the Amiga) and the phase's check:
  Workbench put on a blank hard disk by the Amiga itself (formatted, the
  floppy copied, booted from DH0:), an A600 with 3.1 through Gayle (host
  and Tab5) and an A500 with 1.3 through our board (host).
- **A second hard disk, DH1:**: our board's on every Kickstart (with 2.05
  and 3.1 beside Gayle's DH0:, whose names it avoids). Both volumes on
  Workbench 3.1 on the Tab5 and under 1.3 on the host.
- Not done: the A2000's own differences beyond its RAM (none noticed),
  installing with Commodore's Installer (format and copy instead).
- Later: an RP9 setting the machine from its manifest (the user: a nice
  thing for the future).

### Phase 6 (2026-10-21): done

- **Interlace, both fields**: LOF toggles in frames with LACE on; each
  field keeps its own line records (three frames each, 6 x 284 rows), the
  presenter weaves the long field's lines (even) with the short one's
  (odd). Checked with `tools/laceboot.py` (a white line in long frames, a
  black one in short ones: the picture alternates, host and Tab5) and
  Workbench 1.3 with LaceWB, 640 x 512 (host and Tab5: 100%, core 1 22%,
  core 0 3%). Pictures without LACE unchanged.
- **NTSC**: the picture 720 x 484 (lines 21-262, the 4:3 window filled
  as on an NTSC screen), and on ECS the beam follows BEAMCON0's PAL bit
  (`ami_ntsc_beam()`): Kickstart 3.1's Early Startup Control toggles it
  with a key, the picture switching 568/484 lines (host and Tab5: 60.0
  fps, 100%); graphics.library returns to its DisplayFlags at boot, as the
  ROM does. Workbench 1.3 and 3.1 in NTSC on the host, interlace too.
  Found on the way: an A600 with 3.1 and no hard disk waits 15-20 s for
  its IDE search (real machines do too).
- **The debugger's machine view**: typing (`amiga_type()`, US keymap,
  also the console's `T` and the host's `AMI_INPUT` `t`) and a picture
  safe beside the presenter (the drawing's state is shared: lines drawn
  again when the presenter drew meanwhile). Checked on the host and over
  WiFi on the Tab5.
- **Speed on the Tab5** (the console's `p` now samples either core; the
  profile tool names a static function's file): 100% everywhere tried.
  Workbench 3.1, a Shell listing scrolling: core 1 61% busy, core 0
  52-60%. Kick Off 2 (A500, Kickstart 1.3) with the pitch scrolling:
  99-100%, core 1 56%, core 0 ~65%. Core 1 there: Denise's fetch 7%, the
  68000's memory reads 13%, memcpy 6% (the line records), the 68000's
  dispatch 6%, Paula 5%, the line's end 5%, the blitter 2.4%. Core 0: the
  presenter's scaling 35%, Denise's drawing 14%, memcpy 10%. Paula's
  64-bit divisions (2% of core 1, the ROM's `__divdi3`) are gone.
- **Free memory and disk space** on the web page (bar rows): chip, slow
  and fast RAM from exec's MemList; each disk's free space from its
  AmigaDOS bitmap (`space.c`, the RDB's partitions added up), the same
  as Workbench's own figures.
- **AROS**: its extension ROM mapped at $E00000 (the 68000 doesn't wait
  for the chips there, as in the Kickstart); the setup screen's Kickstart
  "AROS". Its boot screen, then the Workbench 3.1 floppy into its
  Workbook 1.0 with both hard disks, on the host and the Tab5.
- **ECS super-hires** (1280 x 256 and 1280 x 512), one to one on the
  Tab5's 1280 columns, the lines centred; with it Workbench 3.1 in every
  ScreenMode mode, interlaced too (the user, on the Tab5).
- **The touch screen as esp32emu's touchpad** (the Amiga's own moved
  there for every emulator with a mouse).
- **NTSC's lines**: 227 and 228 colour clocks by turns (227.5 on average,
  59.83 frames a second, VPOSR's LOL on ECS), across frames too; PAL's
  timing unchanged bit for bit (Kick Off 2's pictures); Workbench 1.3 and
  3.1 in NTSC on the host.
- **The clock's HOLD**: released after a plain read it no longer sets the
  time (a read across a second set it back one and wrote NVS); setting
  the clock (`setclock save`) changes the offset once.
- Not yet: the line records' copying and the presenter's scaling are the
  places to make faster (PPA, PIE: `../esp32emu/CLAUDE.md`); the Tab5 is
  at 100% everywhere tried, so this waits.

### Phase 7 (2026-10-21): under way, the A1200

- **The CPU type decision**: one firmware, Musashi's type chosen at run
  time (`CONFIG_M68K_RUNTIME` in esp32emu): the 68000's speed measured the
  same on the host (Kick Off 2: the same pictures and instruction count,
  1.23-1.25 s) and the Tab5 (a Shell listing: core 1 42-55% against 61%).
- **The machine**: the setup row's A1200 (Kickstart 3.0 or 3.1 from their
  new partitions, or AROS; 2 MB chip RAM, no slow RAM, Zorro II fast RAM),
  the 68EC020 at 14.19 MHz waiting for the chip bus (SysInfo 4.4: 1.03
  times an A1200; without the waits 2.98, and the Tab5 at 55% in a game),
  Gayle with the A1200's ID.
- **AGA**: Alice's and Lisa's IDs, 256 colours of 24 bits, 8 planes in
  every resolution, FMODE's fetches, HAM8, BPLCON3/4, wide sprites, AGA
  super-hires (see CLAUDE.md, Display). The OCS/ECS pictures stay bit for
  bit; the collection's 25 A1200 games show their AGA pictures on the host
  (Christmas Breakdown's high-density floppy aside).
- **The Tab5**: Workbench 3.1 at 100% (cores ~20%), Aerial Racers' AGA
  title at 100-102% with core 1 93-98%.
- **Bandwidth**: Lisa's ID $00F8 (its bits 9-8 are graphics.library's
  MemType, inverted: 32 bits, double CAS): Workbench 3.1 then offers
  hires in 256 colours and fetches in FMODE 3. The fetch in blocks of
  max(fw, r) words a plane (8 x that / r colour clocks), the first pixel
  16 x fw / that + 1 lores pixels after DDFSTRT x 2: Workbench's hires
  screen and the games' lores ones on their windows, OCS's counts as
  before. Workbench in 256 colours (8 planes, hires, FMODE 3) on the host.
- **Aerial Racers** (2026-10-22): "waiting for its next disk" on its
  title was a hang: its replay stops a channel and waits for the
  interrupt Paula raises once the word playing is done; ours stopped the
  channel silently (fixed in `audio.c`). Past it: the menu, disks 2 and 3,
  the race, which showed two more things: AGA's top two scroll bits set in
  a 16-bit fetch (they reach no further than a fetch: the playfield had
  started 34 pixels late) and the cars, sprites whose VSTART is written
  after the DMA was off at line 25 (they never showed). The collection on
  the host before and after (`tools/collection.py`, 7 pictures each): 92
  of 96 the same; X-Balls (its audio interrupt handler now runs, a level's
  colours differ), Maniac Ball (one frame's ball), Epsilon 9 (differs
  from run to run anyway).
- **The 68EC020's waits inline**: its 5 clocks an access to chip RAM went
  through two calls (`ami_cpu_wait()`, `m68k_modify_timeslice()`: 17.6% of
  core 1 at Aerial Racers' menu); now one subtraction from Musashi's
  remaining cycles, the same arithmetic (the collection's pictures the
  same). The host's race 10% faster; the Tab5's menu core 1 98% -> 80%,
  the race 98-100% speed with core 1 90-99%.
- **No bus errors in Musashi** (`CONFIG_M68K_NO_BUS_ERRORS`, new in
  esp32emu, the Amiga's alone): the 68EC020 copied its 16 registers before
  every instruction to restart it after a bus error the Amiga never
  raises. With the DMA reads, `beam()` and `ami_now()` inline: the Tab5's
  menu core 1 80% -> 60-66%, the race 100-102% with core 1 73-81%
  (Musashi's dispatch 24% -> 12%); the collection's pictures the same.
- **Repeatable host runs**: `AMI_TIME` starts the clock at a given time
  (the collection tool sets it); Epsilon 9 and Maniac Ball were seeded
  from the clock and differed from run to run under load.
- **High-density floppies** (1.76 MB ADFs, 22 sectors a track): the drive
  turns at 150 rpm with one (twice the words a turn) and says $AAAAAAAA
  in its ID; Kickstart 3.1's trackdisk reads and writes them. Christmas
  Breakdown (the collection's last A1200 game without a picture) boots to
  its menu (host and Tab5); a file written onto the disk by its own
  startup-sequence reads back from the image (host and Tab5).
- **The AGA features in use**: a log of each mode's first use over the 26
  A1200 games found HAM8 and 64-pixel sprites (Deluxe Pacman, Soccer Kid),
  FMODE 1 (Football Glory, Leading Lap, Soccer Kid) and the scroll's top
  bits (Deluxe Galaga, Soccer Kid); their pictures look right on the host.
- Not yet: FMODE 2, 32-pixel sprites, AGA super-hires (no game here uses
  them).

### The to-do list (`TODO.md`), item by item

- **1. An RP9 sets up the machine its manifest names** (2026-10-22): the
  manifest's system, Kickstart and RAM as one of our machines
  (`amiga_rp9_machine()`); a package in DF0: or DH0: naming another
  machine than the settings is offered on the setup screen's Disks page
  and the web page ("Kick Off 2 wants an A500: 512 KB chip RAM, 512 KB
  slow RAM, Kickstart 1.2", a button: the user's choice over an automatic
  restart), through a new, general esp32emu mechanism (`disks_config_t`'s
  `offer`/`take_offer`, `/api/status`'s `offers`, `POST
  /api/drive/<n>/offer`). The host sets up the package's machine by itself
  (a ROM folder: the Kickstart too); `tools/collection.py` dropped its own
  mapping: 90 of 96 games the same, the six others where the manifest asks
  for other fast RAM (all six still reach their screens). Tab5: an A1200
  to an A500 with 1.2 for Kick Off 2 from the setup screen, slow RAM added
  from the web page, an A2000 with fast RAM for Hannibal in DH0:.
  On the way: Kickstart 1.2 crashed in a loop whenever a hard disk was in
  (our board's word-wide DiagArea and a bug in 1.2's expansion.library):
  the board isn't offered to 1.2 any more.
- **2. The blitter's timing slot by slot** (2026-10-22): a blit's slots
  (its words x the colour clocks a word takes with its channels) come only
  from what each line's other DMA leaves (refresh, disk, audio, sprites,
  the bitplane fetch: `display_dma_slots()`), spread over the line; its
  end moves on line by line (`plan_end()`). `tools/blitboot.py` times a
  4096-slot blit: 18, 29, 40 and 66 lines with no planes, 4 and 6 lores and
  4 hires planes, as the slots say (18.4, 28.6, 39.8, 65.0); before, 18 to
  22 whatever the display. The collection: 52 of 96 games show other
  moments, all still at their screens (looked at side by side); Flight of
  the Amazon Queen reaches its scene ~1000 frames later, Lethal Xcess its
  next screen sooner. Tab5: Aerial Racers' race 98-100%, core 1 74-82%.
- **3. The 68000 on the chip bus, slot by slot** (2026-10-22): each line
  has a map of the even colour clocks the DMA takes (lores planes 5-6,
  hires 3-4, super-hires 2 at their places in each fetch unit, the
  copper's two an instruction); each 68000 access to chip RAM, slow RAM or
  the chips waits past the taken ones from its time (the instruction's
  start plus 4 clocks a bus cycle before it), a blit 6 clocks (or its
  rest with BLTPRI). SysInfo 4.4 1.03 times an A600 (was 1.00), xSysInfo
  0.12 1.06 (was 1.09): no closer to 1.00 on average, but the two agree
  and the waits fall where the DMA is. Charging a 68000 on an odd colour
  clock its 2 clocks brought both to 1.03 but broke Hyperion's and Lethal
  Xcess's loaders: left out. The collection: 43 games (all 68000 ones)
  show other moments, none stuck or dark (a screen for that, which caught
  the two loaders). Host ~23% slower; Tab5: SysInfo the same, core 1 25%;
  Kick Off 2 at 100%. A `ctz(0)` in the first version's slot scan hung the
  board (undefined: the Mac's gave 32): a table now.
- **4. Paula's attach modes** (2026-10-22): an attached channel is silent
  and its words go to the next channel's volume (where a played word's
  low byte ends, and at the DMA's start) and/or period (where the high
  byte ends); with both, one a period, volume first (the HRM's table,
  WinUAE's state machine). Channel 3's bits silence it. `tools/attachboot.py`:
  boot disks for volume, period, both, channel 3 and none, and a check of
  the recorded sound stretch by stretch: all right (the code before got
  25 and 37 wrong). Datastorm, the collection's one user, sets ADKCON
  $80DF in its loader with the sound off and clears it before its music:
  the same recording as before.
- **5. ECS super-hires: the rest** (2026-10-22): colours as WinUAE has
  them (a hires pixel's two super-hires pixels a, b look up register 4b+a,
  a the upper 2 bits of each gun, b the lower; before, each pixel took
  register 5v's mean), the sprites the same from registers 16-31, on 70
  ns steps with SPRxCTL bit 4, sprites 4-7 hidden. Dual playfield stays
  one playfield and BPLCON1 4 super-hires pixels a step: both as WinUAE
  has them. `tools/shresboot.py`: 11 checks, all right on the host and
  the Tab5 (5 failed before); Workbench 3.1 in super-hires the same
  picture as before; the collection 96 of 96 the same. (A lores screen
  dragged over the super-hires Workbench: done in item 9.)
- **6. Writing extended ADFs** (2026-10-22): a written AmigaDOS track back
  into its sectors when all 11 decode with their checksums, anything else
  as the turn's raw MFM; an AmigaDOS track turning raw moves the tracks
  after it (the image laid out again in its 2.5 MB buffer, written whole
  at the flush). `tools/extadf.py`: Workbench 1.2 writing a file onto an
  extended ADF of AmigaDOS tracks and one of raw tracks: the file right,
  the same blocks as on the plain ADF; a boot block's TD_RAWWRITE (2.04)
  turns a track raw, the image grows by 7036 bytes, the words there. Host
  and Tab5. Son Shu Shi (the collection's one extended ADF) doesn't write
  in its 140 s.
- **7. AGA: the untested and the missing** (2026-10-22): sprites placed
  to 35 ns (SPRxCTL bits 4 and 3, which AGA honours "in all
  resolutions"), CLXCON2 (planes 7-8; a CLXCON write clears it), wide
  sprites' collisions with all their pixels (SPRES too). `tools/agaboot.py`:
  a boot block that loads a payload through trackdisk (register table,
  copper list, planes, sprites); nine cases, all right on the host. FMODE
  2, 32-pixel sprites and super-hires in AGA's colours were right
  already; the build before failed the 35/70 ns steps, CLXCON2's MVBP7
  and the wide sprite's collision. The collection: 95 of 96 the same,
  Virtual Karting 2's sprite text 1-3 super-hires pixels further right
  (it sets those bits).
- **8. The 68EC020's instruction cache** (2026-10-23): instruction reads
  apart from data through esp32emu's new `CONFIG_M68K_PROGRAM_READS`
  (Musashi's `M68K_SEPARATE_READS`; all 24 builds checked); 64 longwords,
  direct-mapped, CACR's E/F/C; a hit costs no wait, a miss one for both
  words of the longword. With it SysInfo's MHz read 14.60 (11.30 before),
  Dhrystone 1.38 times an A1200: the chip waits of 5 clocks had been too
  cheap, hidden by the per-word instruction waits; 8 (two chip bus
  cycles) gives 1.04 (7: 1.13, 9: 0.96, 11: 0.83), the same on the Tab5.
  The faster 68EC020 ran 2.6 times the instructions in Aerial Racers' VBL
  poll loop and the Tab5 fell to 86-94% in its race (core 1 at 100%):
  two-instruction poll loops on DMACONR, VPOSR and INTREQR now skip to
  their slice's end (they can change only there), SysInfo the same on both
  CPUs, the race 98-102% with core 1 at 71-78% (73-81% before). The
  collection: the 20 A1200 games show other moments with the cache
  (Soccer Kid's and Aerial Racers' scripted presses land elsewhere, none
  stuck), all A500 ones the same; the poll skip then 19 others (11 on the
  68000), the same scenes a frame apart.
- **9. Small things** (2026-10-23): sprites outside the vertical window
  are hidden, and don't collide, unless BPLCON3's BRDSPRT (ECS with
  ECSENA, AGA), which also shows them in the side borders: Denise shows
  them only after the line's BPL1DAT write (WinUAE), and the Deniser
  project lists showing them above and below as its bug. `agaboot.py`'s
  sprborder cases (the old build failed the first); the collection 92 of
  96 the same: Hyperion's loading screen and Kick Off 2's title lose
  full-height stripes at the left, Satan's crack intro the junk below
  its window, Deluxe Pacman 8 pixels in one picture. The Overscan editor
  "hanging" from a super-hires Workbench was the test's input (the
  command typed without its Return); a lores screen (MultiView's own,
  dragged down with Left-Amiga) over the super-hires Workbench: both kinds
  of line right in one 1440-wide frame (host).

### The user's demos and games (2026-10-23)

`tools/smoke.py` runs a folder of titles on one host build and flags the
ones that look broken (CLAUDE.md, Testing). Faults found and fixed, each
checked against the game collection:

- **PC-relative reads unmasked**: Musashi hands their addresses over
  without the 24-bit mask (with `CONFIG_M68K_PROGRAM_READS`); Desert
  Dream crashed the host. Masked.
- **VERTB's request before its interrupt**: Seeing Is Believing's loader
  polls INTREQR with Kickstart's VERTB interrupt on; the request now shows
  from line 0's start, the interrupt 40 clocks later.
- **An instruction's long operand across a 64 KB bank** read zeros
  (the 68EC020's immediate reads); each word from its own bank now.
- **The drive's motor latch**: /MTR in the write before the selection
  counts too (WinUAE); Last Action Hero's loader turned the motor on in
  one write and selected the drive in the next (black screen).
- **An interrupt during the last bus cycle's DMA wait** comes after the
  next instruction (the 68000 decides as that cycle begins, WinUAE):
  Lionheart's VERTB poll from slow RAM, held by the copper's list at the
  top of the frame, never saw the bit. The Tab5: its intro at 100%, core
  1 63%.
- **CRA/CRB reads bring the CIA up to date**: a polled one-shot (no
  interrupt) cleared START only when something else synced the CIA. It
  Can't Be Done's loader waits 400 times for a 2 ms one-shot between
  track reads (each took a frame: minutes of black, now it plays
  through); Project-X's New Levels (its disk request instead of black),
  Exile (loads in half the time).

- **The 68000's prefetch and the CIAs' E clock**: Voyage's Copylock-style
  key (CLAUDE.md, the lessons): the opcode fetch through Musashi's
  prefetch (esp32emu's new `CONFIG_M68K_PREFETCH`), the 68000's CIA
  accesses 10-19 clocks by the E clock's phase. SysInfo 1.00 times an
  A600 (1.03 before: the word fetched ahead at each taken branch costs
  its slot), xSysInfo 1.06 as before. The tracks now have their gap
  first, after the index (WinUAE): 9 Fingers' loader, which starts its
  DMA after the index, missed sector 0 with the slower CIA reads. Host
  and Tab5 validation completed 2026-10-24 (see the follow-up below).

Aerial Racers' race on the Tab5 after all of them: 98-102%, core 1
70-81%. The sample of 288 OCS game ADFs: 52 flagged instead of 95 (most
of the difference the alert detector's false alarms, red titles; Last
Action Hero, Lionheart, Project-X and Navy Seals now run; Summer Games,
Cube-X and Neighbours fewer dark pictures; the rest the same scenes at
other moments). The remaining smoke-run fixes are described below.

### Under Pressure follow-up, 2026-11-22

Darkage's A1200 demo (`DKG-UnderP.lha` on Aminet) reaches its credits
and returns to the Shell on FS-UAE 3.1.66 and on the corrected host
build. Both used PAL, Kickstart 3.1, 2 MiB Chip RAM and 8 MiB Fast RAM.

- Some cliff-scene line coordinates are negative. The demo multiplies Y
  as an unsigned word, and the resulting byte stores reach `$CFFxxx`
  and `$D00xxx`. They hit custom registers in the old A1200 map, setting
  `BPLCON0` to `$FFFF` and flooding `INTREQ` near frame 9750. The A1200
  now decodes its custom registers in `$DF0000–$DFFFFF`. The earlier
  models retain their existing mirror rules. [Commodore A1200 service addendum, memory map](https://www.valoroso.it/file-share/documenti-manuali/Commodore-Amiga-1200-service-addendum-schematics.pdf)
- The later blue scene programs `FMODE=$4003` and modulos `-40` and `0`.
  With BSCAN2 enabled, the modulo is selected for all planes by the
  beam line's parity relative to `DIWSTRT`. The old per-plane selection
  displayed stripes and stale lines. Both variants of the display
  window's parity are covered by the host regression, and the blue
  scene now agrees visually with FS-UAE. [Commodore AA chipset specification](https://shanson.com/spencer/Amiga-AA-Chipset.pdf)

The new memory-map and scan-doubling checks fail on the old host build
and pass on the corrected one. The eight-title RP9 comparison produced
identical frames for the five A500 games. The three A1200 titles
(Christmas Breakdown, Maniac Ball and Tactical Manager 2) reached the
same screens at different moments.

The Tab5 ran the original executable from a compact bootable HDF with
the same PAL A1200 configuration. It passed the previous freeze near
frame 9750, rendered the blue scene, showed the credits, and returned to
the AmigaDOS prompt by frame 15600. The heavier scenes ran at about 33 fps.

### Lemmings and Pango Fun, 2026-11-22

- **All New World of Lemmings** (Hoodlum AGA disks, PAL A1200, Kickstart
  3.1, 2 MiB chip RAM). After Space on the title, a write of `$4F` to
  CIA-A port A turned the ROM overlay back on. Subsequent exceptions
  read ROM contents as vectors. Gayle now handles the overlay on both
  the A600 and the A1200 as a reset latch cleared by the first CIA-A
  write. The older A500 and A2000 keep the port-controlled overlay.
  That change cleared the exception loop. The game then hit a block 925
  read error with no fast RAM. `AMI_DISKLOG` recorded the retries with
  no selected spinning drive. FS-UAE 3.1.66 reached the map room with the
  same images. [WinUAE CIA behavior](https://github.com/tonioni/WinUAE/blob/master/cia.cpp), [MAME Gayle overlay behavior](https://github.com/mamedev/mame/blob/master/src/mame/amiga/amiga_m.cpp)
- **Pango Fun** (Heresy beta, PAL A500, 512 KiB chip and 512 KiB slow
  RAM). The intro turns Copper DMA off while replacing its list.
  On resumption, the Copper fetched from the overwritten list at
  `$7616` and enabled a level 4 audio interrupt with a stray write to
  `INTENA`. Kickstart dispatched to a null handler and raised Guru
  `$00000004`. The Copper now defers the vertical-blank reload while
  its DMA is off and uses the current `COP1LC` when it can fetch again.
  FS-UAE passed the Guru point and reached the credits. The corrected
  host reached the first level with all three disk 1 variants, under
  Kickstart 1.2 and 1.3. The Tab5 also reached the first level with
  Kickstart 1.3. [FS-UAE 3.1.66 Copper restart](https://github.com/FrodeSolheim/fs-uae/blob/v3.1.66/src/custom.cpp)

The host regression exercises both Gayle models, the A500 port-controlled
overlay and the deferred Copper reload. It fails on the previous code
and passes on the corrected build. Under Pressure still returns to the
Shell. Seven of eight RP9 titles matched frame for frame at the sampled
points. Traps 'n Treasures showed the same title and scrolling intro at
slightly different points in their animations. The Tab5's previous
Amiga settings and disks were restored after both runs. Local disk
images, traces and screenshots are in `build/todo10-compat-2026-11-22/`.

#### Lemmings disk error follow-up

The title's post-Space loader uses odd-address 68EC020 data operands.
For example, at `$05FA16` it reads a word at `$092B73` whose two guest
bytes are `$3E,$B3`. A native word load at that odd address joins bytes
from opposite ends of their storage words. The corrupted data led to
stray CIA writes, a stopped DF0 motor and the misleading block 925
requester. The 68EC020 callbacks now split odd words into two bytes and
odd longwords into a byte, an aligned word and a byte. Each piece uses
its own memory bank and wraps at 24 bits. [Motorola M68020 User's Manual, section 5.2.2](https://www.nxp.com/docs/en/reference-manual/M68020UM.pdf)

The new host checks cover odd reads and writes, adjacent byte
preservation, the 64 KiB bank boundary and the 24-bit wrap. All seven
checks fail against the previous callbacks. In the eight-title RP9
comparison every sampled frame matched. Under Pressure still shows the
cliff, blue scene and credits before returning to the Shell, now by
about frame 9900 on the host.

Lemmings reaches the map room with two disk 1 variants, both with and
without 4 MiB fast RAM on the host. The third variant has a bad OFS
checksum on block 15 of the image. With the Shadow disk in DF1, the
first Shadow level loaded on the host. The Tab5 with Kickstart 3.1,
2 MiB chip RAM and no fast RAM reached the map room and opened the map
on a mouse click. The board's previous configuration and disks were
restored.

### DMS compression mode validation, 2026-11-23

DiskMasher 1.01 from Fred Fish disk 406 packed two cylinders of a
synthetic double-density floppy in each of modes 2 (Quick), 3 (Medium)
and 4 (Deep). The command for each was
`DMS READ <name>.dms FROM DF0: CMODE <2|3|4> LOW 0 HIGH 1 NOZERO NOVAL`.
DiskMasher 1.11 also packed all 80 cylinders of a second synthetic
floppy with `CMODE HEAVY1 NOZERO NOVAL`. The Heavy1 and Heavy2 checks
also included the full Gory Story and Golden Fleece disks. [DiskMasher 1.01 distribution](https://ftp.funet.fi/pub/amiga/fish/401-500/ff406/), [DiskMasher 1.11 distribution](https://ftp.fau.de/aminet/util/arc/dms111.run), [IF Archive Amiga disks](https://www.ifarchive.org/indexes/if-archive/games/amiga/)

xDMS 1.3.2 and the web converter reproduced the original bytes for
all generated disks. Both cylinders in each partial archive had the
continuation flag set. `tools/amifiles_test.js` compared the converter's
880 KiB ADFs with the xDMS output, padded with zeros for the partial
disks. The browser registration path produced the same bytes in a Node
VM. This run did not exercise the Tab5 upload path; that was checked on
2026-11-24 ([below](#dms-through-the-live-web-page-2026-11-24)). Local
archives and expected ADFs are in
`build/dms-validation-2026-11-23/{legacy,packed}/`.
[xDMS 1.3.2](https://zakalwe.fi/~shd/foss/xdms/)

### Workbench display changes on the Tab5, 2026-11-24

TODO item 12. On an A1200 with Kickstart 3.1, Workbench 3.1 from the hard
disk was switched through AGA lores (256 colours), hires interlaced
(256 colours) and super-hires (16 colours), and MultiView's lores screen
was dragged over the super-hires Workbench.

- **Method.** `tools/wbmodes.py` makes the Extras disk with the prefs and
  the picture. The mode changes are `screenmode FROM … USE`, typed into
  Execute Command, so no clicks are needed.
- **Frames.** All six steps' emulated frames on the Tab5 equal the host's
  in every pixel, including the 1440-wide super-hires and mixed frames.
  The board ran at 49–50 fps with core 1 at 20–40%.
- **Panel.** Super-hires is the frame's columns 84–1363, one to one. The
  other frames fill the centred 960 × 720 area.
- **Probes.** `laceboot`, `shresboot` (A600, 2.05) and `agaboot shres`
  passed their checks on the board's frames, identical to the host's.
- **Uploads.** Web uploads failed while the A1200 ran: the SD driver ran
  out of internal RAM for its DMA buffer. That became TODO item 15, fixed
  the same day (below).

[Workbench display changes on the Tab5](workbench-modes-tab5-2026-11-24.md)

### Uploads while the A1200 runs, 2026-11-24

TODO item 15. With the A1200 running Workbench 3.1 (2 MB chip, 8 MB fast),
every web upload failed, even a 900 KB one: "Can't write to the SD card"
or "Upload failed". The board's log showed `allocate_dma_buf: not enough
mem` from the SD driver. The fix is in esp32emu (`components/emu`).

- **Cause.** The Amiga's allocations leave about 3.4 MB of PSRAM free.
  That is below the web server's 4 MB reserve, so no file is taken whole
  into PSRAM; every transfer goes in 8 KB pieces. Those pieces were
  internal DMA memory, and the SD driver allocated another internal DMA
  buffer for each transfer that wasn't aligned to the cache line, which is
  most of them (FATFS's sector windows and file caches). Internal RAM's
  low point during a transfer was 29 KB, and the driver's allocation
  failed outright. The setup screen has the same PSRAM but more internal
  RAM (158 KB free against 113 KB), which is why uploads worked there.
  The restart seen once during a 50 MB upload did not happen again in
  three 50 MB uploads and three downloads; its cause wasn't found.
- **Fix.** `sdcard.c` reserves the driver's bounce buffer once, at mount,
  in PSRAM (`host.dma_aligned_buffer`, 8 KB, aligned to 128 bytes): the
  P4's SD host reads and writes PSRAM directly. `web.c` keeps the pieces
  in PSRAM too, aligned the same way, and passes them to `read()` and
  `write()` past stdio, whose 128-byte buffer had FATFS move one sector
  at a time. An upload's pieces are filled to 8 KB before each write.
- **Checked on the Tab5.** A 900 KB and a 50 MB image of random bytes,
  each uploaded and downloaded back, byte for byte the same:

  | Machine | Upload 900 KB | Upload 50 MB | Download 50 MB |
  | --- | ---: | ---: | ---: |
  | A1200 running Workbench (before: failed) | 0.8 s | 40 s | 143 s |
  | A500 running, Kickstart 1.3 | 0.8 s | 39 s | 145 s |
  | Setup screen | 0.8 s | 39 s | 152 s |

  The machine stayed at 98–100% speed during the transfers. The 50 MB
  upload took 49 s from the setup screen before the change.
- **Downloads are limited by WiFi.** A timing log showed that reading
  50 MB from the card took 4.7 s and sending it took 126 s, about
  0.4 MB/s. Pieces of 64 KB instead of 8 KB saved about 18%, which was
  not enough to keep.
- **MaTouch.** The web change applies to the S3 too. On esp32coco, the
  same build compared with the old `web.c` gave the same times (8 MB up
  37 s, down 16–18 s) and identical files.
- **Every esp32emu project** built for both boards.

### DMS through the live web page, 2026-11-24

TODO item 13. The DMS validation archives went through the Tab5's own web
page in Chrome: the page unpacked each one in the browser and uploaded the
ADF to the board. Each ADF was then downloaded back from the board and
compared with the reference.

- **Method.** A separate headless Chrome 158 (the flatpak, with a
  throwaway profile) opened the board's page. A Node script set each
  archive on the page's file input over the DevTools protocol
  (`DOM.setFileInputFiles`), which runs the same path as choosing a file,
  and read the page's log lines. The archives were renamed with a
  `dmschk_` prefix so they couldn't replace anything on the card.
- **Results.** Every good archive was unpacked and uploaded, and every
  ADF came back from the board identical to its reference:

  | Archive | Tracks | Mode | Page |
  | --- | ---: | --- | --- |
  | quick | 2 | Quick | 0.8 s |
  | medium | 2 | Medium | 1.0 s |
  | deep | 2 | Deep | 1.3 s |
  | heavy1 | 80 | Heavy1 | 1.0 s |
  | Gory Story | 80 | Heavy1 | 1.0 s |
  | Golden Fleece | 80 | Heavy2 | 1.0 s |

  "Page" is the time from choosing the file to the page's "is on the
  board" line, unpacking and upload included. The two-track archives
  give 880 KB ADFs padded with zeros, as on the host.
- **A bad track checksum.** A copy of heavy1 had track 40's checksum of
  its unpacked bytes changed by one, and that track header's CRC
  recomputed, so only the unpacked check could catch it. The page said
  "track 40 didn't unpack right (mode 5)" and sent nothing: the image
  isn't on the card (a GET gives 404, `/api/disks` doesn't list it).
- **Booting.** Gory Story's uploaded ADF in DF0: of the A1200 (Kickstart
  3.1, 2 MB chip, 8 MB fast, the Workbench HD in DH0:) booted to its
  title page ("Listing of gory.doc") at 98–100% speed. The emulated frame
  equals the host A1200's in every pixel (RGB565), and the panel shows
  it. On the host both bootable disks also boot on an A500 with 1.3
  (Golden Fleece to its logo).
- The driver script and the board's pictures are in
  `build/dms-validation-2026-11-23/tab5-2026-11-24/`. The test images
  were deleted from the card afterwards and DF0: given back its
  Workbench 3.1 disk.

### Title replays: floppy or memory first, 2026-11-24

TODO item 14. A title that dies at its loader can look like a floppy
fault when the CPU caused it: All New World of Lemmings' "read error on
disk block 925" came from the 68EC020's odd-address reads (2026-11-22).
The replays check titles that once failed and say which side parted from
a good run first.

**The replays** (`tools/smoke.py replay`, `tools/replays.json`). Each
title runs on its machine with its input to a fixed frame:

| Replay | Machine | Checkpoint |
| --- | --- | --- |
| lemmings (Hoodlum, AGA) | A1200, 3.1, 2 MB chip | the map room at 6000 |
| pango (Heresy beta) | A500, 1.3, 512 + 512 KB | the first level at 5500 |
| magician (PDX crack) | A500, 1.3, 512 KB | the first room at 5500 |
| magician-slow (Angels crack) | A500, 1.3, 512 + 512 KB | the first room at 8000 |

A reference run from a good build (`replay record`) keeps the
checkpoint's picture and three event streams. A check passes when 90% of
the picture's 8 × 8 blocks are alike. Wherever it parts from the
reference, the earliest difference by instruction count names the side
that went wrong first:

- **The CPU's drive commands:** each change of CIA-B's drive lines and
  each DMA start (cylinder, side, length, destination). Different first:
  the CPU or memory went wrong, and the floppy did as it was told.
- **The floppy's data:** a checksum of each DMA's words when it ends.
  Different with the same commands: the floppy gave other data.
- **The CPU's path:** an address executed that the reference never ran
  (`AMI_PCCOVER`), or an exception it never took.

The host build gained the hooks for this: the core calls
`amiga_disk_end_hook` and `amiga_drive_hook` (NULL on the board), and
`AMI_DISKLOG` logs both, with the instruction count on each line.

**Checked with one fix reverted at a time.** The current build passes all
four, each the same as its reference all through:

| Build | Fails | First difference |
| --- | --- | --- |
| without the 68EC020's odd-address reads | lemmings (the block 925 requester) | drive commands, frame 2791 (lines `$7F` against `$ED`); the data at 2796; the code at 3722 |
| without the Copper's deferred reload | pango (Guru `#00000004`) | the CPU's path, frame 2551 (Kickstart's exception code, an illegal instruction at `$000004`); the drive commands at 2912 |
| without Gayle's overlay latch | none | — |

So the Lemmings failure that looked like a floppy fault is blamed on the
CPU. Gayle's latch no longer shows in Lemmings, because the stray CIA-A
write that turned the overlay on came from the odd-address reads.
`mem_test` still covers the latch.

**The Magician with slow RAM** (the smoke run's "fails only with slow
RAM"). The PDX crack on an A500 with 1.3 starts the game without slow RAM
and shows "Software Failure", Guru `#00000003`, with it. The address
error is at `$06242A`, frame 2146, on the title before any input. Where
the two runs part:

1. **Both runs:** the boot code at `$030000` reads 100 KB through
   trackdisk.device into `$35000`. It takes the machine over with TRAP #0,
   and copies the code to `$57400`. The crack replaced the original
   `beq` at `$03006C` with `bra`. The original chose the code's address
   from an OS pointer's high bits (`and.l #$F00000`): `$C13000` in slow
   RAM, `$A0000` with 1 MB of chip RAM, and `$57400` only on 512 KB.
2. **The first difference in the game:** at frame 1799 the game checks
   memory for itself (`cmp.l #$C00000,ExecBase` / `sge $64B7A` at
   `$06230E`). Kickstart 1.3 puts ExecBase in slow RAM when there is
   some, so the flag is set.
3. **Frame 1951:** with the flag set, the game takes its slow-RAM path
   (`tst.w $64B7A` at `$062404`). A trackloader call at `$062420` loads
   data over the code after the call, because the crack placed the code
   there.
4. **Frame 2146:** the loader returns into the loaded data, which raises
   the address error, and the Guru follows.

The raw instruction logs first part earlier, at a wait loop's passes in
frame 1799 (`tools/pcdiff.py`). That's timing, not a different path; the
code each run alone executes shows the real split.

With the `bra` patched back to `beq` (the boot block's checksum fixed),
the game runs with slow RAM to its first room. The Angels crack, which
keeps the original addresses, does too. So this is the PDX crack's fault,
and a real A500 with trapdoor RAM would fail the same way. The emulator
is right.

New host options for finding such places: `AMI_WATCHLOG`, `AMI_STOPDIS`,
`AMI_PCLOG` (`tools/pcdiff.py`) and `AMI_PCCOVER`. The images (TOSEC
names in `replays.json`) and the references are in `build/replays/`; the
runs are in `build/todo14-2026-11-24/`.

### Faster Tab5 downloads, 2026-11-24

TODO item 16. A 50 MB image took ~2.5 minutes to download from the Tab5
(0.35–0.4 MB/s) while uploads took 40 s. The fix is in esp32emu (the Tab5
board's `sdkconfig.defaults`, `web.c`).

- **Not the card, the pieces or the CPU.** A temporary download sent from
  memory, with no card reads, ran at the same 0.4 MB/s, chunked or as one
  raw stream, with the transfer task at priority 5, 10 or 19, and with
  the cores idle. WiFi modem sleep off and esp_hosted's throttle off made
  no difference either.
- **Lost packets.** The PC (wired) queued about one TCP segment in ten
  out of order. esp_hosted's packet counters showed the host handing the
  C6 every packet: no drops, ~2.5% resent. So the C6 lost them. It had
  16 Wi-Fi transmit buffers (`CONFIG_WIFI_RMT_STATIC_TX_BUFFER_NUM`,
  passed to it at `esp_wifi_init`), while TCP's 64 KB send window keeps
  ~45 segments in flight.
- **The fix.** 64 buffers, the most allowed (~100 KB of the C6's RAM;
  Bluetooth still starts, with both paired devices): 2.3 MB/s from
  memory, nothing out of order (32 buffers: 0.73 MB/s). Downloads from the
  card now go in 64 KB pieces on the Tab5 (1.95 MB/s against 1.75 with
  8 KB ones).
- **Checked on the Tab5.** 1 MB and 50 MB of random bytes uploaded and
  downloaded back, identical:

  | | 50 MB down | 50 MB up | 1 MB down |
  | --- | ---: | ---: | ---: |
  | Before, A1200 running | 143 s | 40 s | — |
  | Setup screen | 27.2 s | 45.8 s | 0.77 s |
  | A1200 running (100% speed) | 27.5 s | 40.8 s | 0.63 s |

  Uploads are unchanged and lose nothing (no resends seen by the PC).
- **MaTouch.** esp32coco with the new `web.c`: 900 KB up and down,
  identical, at the same speed as before (its WiFi is the S3's own).
- **Every esp32emu project** built for both boards; each project's
  `build-tab5/sdkconfig` had its 16 changed to 64, because defaults don't
  override a value already there.

### The A1200 load survey, 2026-11-25

TODO item 17. 26 A1200 packages ran on the Tab5, each on its manifest's
machine, with the host pass's input at the same frames. Item 9's
assembler waited on a title that falls behind: A1200 titles do.

- **Speed.**
  - Seven spent most of their run at 53-66%: Deluxe Galaga, Virtual
    Karting 2, Leading Lap, Black Viper, Tactical Manager 2, Fatman in
    play and Gea Lux in play.
  - Six more had stretches at 75-90%.
  - Thirteen stayed at 93% or above. Aerial Racers' race is at 100%,
    core 1 ~68%.
- **Why: wait loops.** Many titles spend 20-99% of their instructions in
  two-instruction wait loops the poll skip doesn't cover. They test RAM
  flags and counters (Soccer Kid 82%, Starbase 13 70%, Deluxe Galaga
  59%), VHPOSR outside the `#$F4` gate (Black Viper's briefing 99%,
  Fatman 74%) or CIA registers (Black Viper's race: CIA-B's ICR 49%).
  That is TODO item 18.
- **Why: the 68EC020 itself.** Other titles run 24-56k instructions a
  frame with no waiting. Core 1 manages ~15-20k: about 1 µs an
  instruction, of which Musashi's instruction-word reads through the cache
  model are 17-36% and its dispatch 10-21%. That is item 19, the
  assembler's entry with its condition met.
- **Host and board.** The board's pictures are the host's at the same
  frames (in play where the input gets there), except Fatman, Tactical
  Manager 2 and Deluxe Galaga's last picture, which differ in detail. The
  harness needed:
  - the exact frame from the console's `i` (presses timed from the fps
    lines came tens of frames off);
  - presses keeping their length;
  - a package's second disk as its own ADF in DF1: (the runtime keeps a
    file in one drive). For users that is item 20.

[The A1200 load survey on the Tab5](a1200-load-survey-2026-11-25.md)

### Wait loops without the CPU, 2026-11-25

TODO item 18. The survey's titles spent up to 99% of their instructions in
two-instruction waits that the poll skip missed: RAM flags, VHPOSR outside
its gate and CIA registers.

- **The skip** (`wait.c`). `run_cpu()` checks as a line begins, and in the
  line's other slices while the CPU is in a loop.
  - A loop is a test (TST, BTST, CMPI, CMP or MOVE to Dn) and a Bcc back
    to it, or a branch to itself. It is decoded once and kept with its
    words.
  - The test is worked out from memory as it is (`mem_peek()`, the chips'
    and the CIAs' peeks). If the branch will be taken and no interrupt is
    due, the slice passes without the CPU.
  - What these loops read changes only where a slice ends. The exception
    is a CIA timer with its interrupt off: a skip on ICR, CRA or CRB ends
    at its next underflow.
- **Pango Fun.** The replay found `poll_wait()` skipping a wait's exit
  pass, which made the intro miss line 0 for good. It now skips only while
  the loop will go on (the lessons in CLAUDE.md).
- **Host.** `wait_test.c` has four cases, each failing with its part
  taken out. SysInfo's screens are the same on the A500 and the A1200,
  and the replays pass 4 of 4. 72 of the collection's 96 games are
  identical; the rest show the same scenes at other moments.
- **Tab5.**
  - Four A500 titles still run at 100%. Their core 1 is within 2 points of
    before, and Kid Pool's is 4 points lower. The check costs 0.3-0.7%.
  - In the A1200 survey, the time behind fell from 638 s to 405 s.
  - Black Viper, Fatman, Soccer Kid and The Lost Prince now run at full
    speed (they were at 61-86%). Deluxe Galaga in play went from ~60% to
    81-98%.
  - The titles that compute are unchanged: they are item 19's.

[Wait loops without the CPU](wait-loops-2026-11-25.md)

### A faster 68EC020, the first steps, 2026-11-25

TODO item 19, under way. Three of the slow titles were waiting after all,
in loops longer than two instructions.

- **Longer waits** (`wait.c`). Loops of up to 16 instructions are
  decoded, and a pass is worked out from the registers and the peeks.
  - Moves, stack pushes and pops, EXT, SWAP, AND, OR, shifts, tests,
    Scc and forward branches are covered.
  - VHPOSR's horizontal byte is marked unstable, and no branch may rest
    on it.
  - A pass that comes back to the same state (a fixed point) skips the
    slice. One pass later counts too, and then the CPU is set to it.
  - ICR isn't peeked while a flag is pending.
  - Results: Tactical Manager 2 65% → 100%, AmiBee in play 53% → 95-99%.
- **A cheaper fetch** (`mem.c`). One compare a word, the state reached
  from gp: 17 P4 instructions a second word and 25 a cached first word
  (19 and 28). 2-4 points of speed in the titles that compute; the host
  collection the same, 96 of 96.
- **Hyperion** turned out to be a coin toss on timing in every build: a
  read whose DSKLEN lands within a sector's second sync word loses
  sector 0 (TODO 21).

[A faster 68EC020, the first steps](faster-020-2026-11-25.md)

### Voyage follow-up, 2026-10-24

The host comparisons exposed two further issues. Hyperion's DMA could
capture words from before its DSKLEN write within the same line; bringing
DMA up to its start/stop and enable changes fixes its short final sector.
Steel Empire's slow-RAM probe mistook write-only custom mirrors for RAM
and chose a stack at $D80000; the 68000's RAM reads/writes now drive the
floating chip-bus word too. OCS DENISEID uses the undriven-bus $FFFF
fallback until individual DMA bus values are modeled (Rink a Dink's AGA
check must not see a CPU opcode). Both have ROM-free host regression tests;
the shared core also tests prefetch on/off. The write tests pass for plain,
extended AmigaDOS and raw ADFs, including raw-track promotion. All 24
firmware builds passed. The Tab5 was flashed and checked: Aerial Racers'
race ran at 98-101% speed, core 1 68-89% busy across 21 samples over
40 seconds; Hyperion loaded on an A500 with 512 KB chip + 512 KB slow
RAM and Kickstart 1.2. The saved A1200 Workbench setup was restored. See
`validation-2026-10-24.md` for the checks and their limits.

### Speed in C, 2026-11-07 to 2026-11-10

Plain C changes, local to this project (`TODO.md` 9: assembler only if
a game falls behind, C first). Each one keeps the pictures, sound and
timing exactly. The host collection (96 RP9 games), the long runs and the
recordings were compared before and after; the Tab5 was profiled with the
console's PC sampler.

- **Planar expansion** (`display.c`): each eight-pixel lookup is split into
  two 32-bit halves, with no variable 64-bit shifts on the RV32 core.
  Isolated complete-row draws are 3.1–12.5% cheaper; no whole-emulator
  gain is claimed (`planar-expansion-experiment-2026-11-07.md`).
- **Unshifted A-to-D blitter copies** (`blitter.c`): a guarded fast path
  for BLTCON0 `$09F0`. Large copies cost 65–67% less in isolation; an OCS
  control screen measured 0.3–0.7% slower, accepted
  (`blitter-copy-experiment-2026-11-07.md`).
- **Real-software validation** of both: 672 of 672 collection pictures
  and 230 final disk files the same, plus a blitter census of nine titles
  (`software-profile-2026-11-07.md`).
- **The 68EC020's instruction fetch** (`mem.c`): its next word comes
  through a pointer while it runs on through plain memory. The fetch is
  ~7% cheaper in Aerial Racers' race, and the A500 is unchanged
  (`fetch-experiment-2026-11-10.md`).
- **The 68000's bus cycles** (`mem.c`): Musashi's memory callbacks work an
  access to plain memory out inline, with no stack frame. Nothing in the
  way or only taken slots are handled inline; a blit, the CIAs and the
  chips' registers still go through `bus_cycle()`. `bus_take()` keeps the
  slots' table up to date, and the bank pointers lose their bank's address.
  On the Tab5:
  - Kick Off 2's credits: core 1 16.7 → 9.9 ms a frame.
  - Kid Pool's title, which ran at 94–95% with core 1 at 100%: now 100%
    with 5% idle.
  - The A1200's data callbacks: 5.3% → 3.9% of core 1.

  New ROM-free checks in `make test` compare the inline paths with
  `bus_cycle()`, and both with the slot model
  (`bus-cycle-experiment-2026-11-10.md`).
- **VHPOSR's vertical-byte poll** (`custom.c`): Kid Pool's title
  `cmpi.b #-$c,$dff006` / `bne` skips the slice's rest after the read,
  only while the branch still loops and only for CMPI.B `# $F4` in low
  chip RAM. Collection 96/96 the same. Tab5 title: core 1 load 85–86% →
  53–55% at 100% speed (idle in the profile 6% → 39%;
  `poll-vhpos-experiment-2026-11-11.md`).
- **The A500 load survey** (2026-11-11, Tab5, KS 1.2, 512 + 512 KB;
  `build/a500-load-2026-11-11/`): no title fell behind. Speed and core 1
  (speed / core 1):

  | Title | Speed / core 1 |
  | --- | --- |
  | Kick Off 2's menu | ~100% / 45% |
  | Kid Pool's title | ~100% / 55% |
  | F1 GP | ~100% / 52–71% |
  | Toobin | ~100% / 55–69% |
  | Arkanoid | ~100% / 65% |
  | Lure | ~100% / 50% |
  | X-Balls | ~99% / 84–85% |

  X-Balls was the hottest. Its profile: idle 4.9%, `m68k_read_memory_16`
  15%, `display_fetch` 9%, `bus_take` 8%.

  On the way, Son Shu Shi's RP9 failed to insert on the Tab5 (`no
  sonshushi1.adf in it`): a one-shot inflate of a ~2 MB extended ADF,
  next to the image buffer when PSRAM was tight. Floppy RP9s now stream
  through `rp9_extract` as hard disk ones do (checked on the Tab5: disk 1
  of 2). The A1200's survey followed on 2026-11-25 (above).
