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
- ECS Denise: super hires (1280 pixels, drawn at hires width), the
  border blank bits, DENISEID.

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
| 000000 | Chip RAM (512 KB - 2 MB); at reset the ROM (OVL) until CIA-A clears it |
| 200000 | Zorro II fast RAM (autoconfig: up to 8 MB) |
| A00000-BEFFFF | Nothing (the ST's lesson: unmapped space must behave as the real board's) |
| BFD000, BFE001 | CIA-B (even addresses: the data bus's high byte), CIA-A (odd addresses: the low byte), each register every 256 bytes |
| C00000 | Slow RAM (the A500's trapdoor 512 KB) |
| D80000-DFFFFF | Gayle (A600: DA0000 IDE, DE1000 ID), the A2000's RTC (DC0000), custom chips (DFF000) |
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
