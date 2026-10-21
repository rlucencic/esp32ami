# esp32ami: Amiga 500, 2000 and 600 on the M5Stack Tab5

An Amiga 500, 2000 or 600 (OCS/ECS, a 68000 at 7.09 MHz PAL) on the M5Stack
Tab5 (ESP32-P4) only: the user's call, the MaTouch's S3 hasn't the speed for
the 68000 and the custom chips together. The A1200 (AGA, a 68020) is a later
phase if the P4 has the speed. Plan and decisions: `docs/architecture.md`
(its phases table; phases 1 to 3 are done, 4 under way: see Progress
there). ESP-IDF
v5.5.4, C.

The board side (tasks, LCD, touch, SD, WiFi, BLE, setup screen, web page,
console) and the 68000 core (Musashi) are the shared library in
`../esp32emu`; its CLAUDE.md has the runtime and the board-level lessons.
Changes there need all its projects built (`esp32emu/CLAUDE.md`).

The chipset is our own (MIT), written from the Hardware Reference Manual
and the chips' documented behaviour; UAE, WinUAE, FS-UAE, vAmiga and PUAE
(GPL) only to compare behaviour against, never copied.

## Build, flash, debug

The Tab5 only (`/dev/cu.usbmodem3101`): `CMakeLists.txt` stops a build for
any other target. `sdkconfig.defaults` is the siblings' base (S3 symbols
give harmless "unknown kconfig symbol" warnings), plus `CONFIG_M68K_68000`;
the Tab5's settings from `../esp32emu` go on top.

```sh
bash -c '. $IDF_PATH/export.sh >/dev/null 2>&1; \
  export IDF_PYTHON_ENV_PATH=/Users/robertlucencic/.espressif/tools/python/v5.5.4/venv; \
  export PATH=$IDF_PYTHON_ENV_PATH/bin:$PATH; \
  idf.py -B build-tab5 -DIDF_TARGET=esp32p4 -DSDKCONFIG=$PWD/build-tab5/sdkconfig build && \
  idf.py -B build-tab5 -DIDF_TARGET=esp32p4 -DSDKCONFIG=$PWD/build-tab5/sdkconfig -p /dev/cu.usbmodem3101 flash'
```

- `flash` also writes the Kickstarts from `roms/` (not in git;
  `roms/README.md`, FS-UAE's file names) to partitions `kick_120`,
  `kick_130` (256K each),
  `kick_204`, `kick_205_a600`, `kick_310_a600`, `kick_aros`, `kick_aros_ext`
  (512K each); `app-flash` only the app. The Tab5 is shared with the other
  projects: after another one, a full `flash`. NVS (WiFi, BLE pairings) is
  shared; this project's settings are namespace `ami` (`model`, `kick`,
  `chip_kb`, `slow_kb`, `fast_kb`, `ntsc`, `rtc_ofs`: the clock's offset
  in seconds),
  its drives `amifloppy`.
- Disk images: `.adf`, `.hdf` and Amiga Forever's `.rp9` games in the SD
  card's `floppies/` folder (shared with esp32arc: its 800 KB ADFS images
  are refused by size; its hard disks are `.hdf` too:
  leave them). Drives DF0:, DF1: (ADFs, RP9s with floppies; the API's 1,
  2) and DH0: (HDFs, RP9s with a hard disk; 3). Uploads up to 256 MB.
  Upload with
  `curl -T disk.adf http://amiga.local/disks/`, insert with
  `curl -X PUT --data "disk.adf" http://amiga.local/api/drive/1` (2: DF1:).
- `../esp32emu/tools/emu_console.py PORT log N | lcd F.png | send "cmd" | info`
  with the IDF venv's Python (`-I`). `curl http://amiga.local/api/status`
  (`board.load`: the cores' busy %).
- Flashing restarts the board and ends the user's session: say so first.

Console commands: the shared ones (`emu/console.h`) and the Amiga's
(`main/main.c`): `k <hex>` tap a key by its raw code ($44 Return, $40
Space, $45 Esc, $50-$59 F1-F10, $5F Help; letters by position: the
Workbench 1.3 disk's German keymap puts Y on $31), `K <hex> 0|1`, `m dx
dy btn` (the mouse; 1 left, 2 right, 4 middle; held until the next `m`;
moves over 100 spread over 25 ms steps: one `m 0 0 1` then `m 0 0 0` is a
click, two pairs a double-click), `j <hex>` the joystick (1 up, 2 down, 4
left, 8 right, 10 fire, 20 second button), `R <model> <kick> [chip KB]
[slow KB] [fast KB]`: restart as model 0 A500, 1 A2000, 2 A600 with Kickstart 0
1.3, 1 2.04, 2 2.05, 3 3.1, 4 1.2 (the model's own: the A600 2.05 or 3.1,
the others 1.2, 1.3, 2.04 or 3.1), chip RAM 512/1024 (2048 the A600), slow RAM
0/512 (not the A600), fast RAM 0/1024/2048/4096/8192. Workbench 1.3 from the console: the pointer starts
top left; `m 300 100 0` then `m 291 0 0` reaches the disk icon (counts
are hires pixels across, half lines down), the Shell icon in its window
is `m -523 136 0` from there.

Input on the board: Bluetooth/USB keyboards by position (F11 Help,
Windows/Command the Amiga keys, Caps Lock toggles; F12 makes the cursor
keys and keypad the joystick in port 2, Alt fire), mice in port 1, touch as
a touchpad (move, tap left click, tap then touch again to drag, hold still
0.5 s for the right button: the menus).

Setup screen on the Tab5: Start `t 1065 637`; the six rows scroll (the
shared setup screen's box): Model (y 227), Kickstart (298), Chip RAM
(369), Slow RAM (440), Fast RAM and Video below (`t 425 440 800 425 240`
drags them up: then Chip RAM 258, Slow RAM 329, Fast RAM 400, Video 470);
three-button rows at x 367, 604, 841, four-button ones 337, 514, 692,
870, two-button ones 426, 781, Fast RAM's five 320, 461, 603, 745, 887.
The status lines show the WiFi name and IP: keep them out of published
images.

## Layout

| Path | What |
|---|---|
| `main/main.c` | The Amiga as an `emu_machine_t`: memory, start, setup rows, drives (DF0:, DF1:, written tracks to the card), the clock's offset, key map, mouse, joystick keys, touch, sound, status, console |
| `main/rom_store.c` | Kickstarts from their flash partitions, else files in the SD card's `roms/` with that version |
| `main/settings.c` | NVS `ami`, which Kickstart and RAM each model takes, its defaults (A2000: 1 MB chip, 2 MB fast) |
| `components/amiga/` | The machine, platform independent (`include/amiga.h`) |
| `components/amiga/host/` | Host build for testing on the Mac (see Testing) |
| `tools/adf.py` | Reads ADFs and patches files in them (test disks, see Testing) |
| `roms/` | The Kickstarts (copyrighted, not distributed) |
| `partitions.csv` | nvs, phy, factory 8 MB, the six ROMs (type 0x40), storage |

## The core (`components/amiga`)

- **Time**: the 68000's clocks since the frame began (2 a colour clock, 454
  a line; PAL 313 lines, 312 in interlace's short frames; NTSC 263/262).
  At a frame's end every kept time moves back by the frame's length
  (`amiga_run_frame()`); the E clock (CIA timers, CPU / 10) keeps its phase
  in `ami.e_phase`.
- **The frame loop** (`amiga.c`): per line, the 68000 runs up to the next
  event: the bitplane fetch, the copper's next step, the blitter's end, a
  CIA timer's underflow (with its interrupt enabled). A write that makes an
  earlier event (COPJMP, a blit, a CIA timer) or raises the interrupt
  level ends the CPU's slice (`ami_cpu_wake()`: Musashi samples the IRQ
  lines only as a slice starts). At the line's end the display records the
  line; CIA-B's TOD counts lines, CIA-A's frames.
- **The 68000 and the chip bus**: its instructions take a multiple of 4
  clocks (the cycle table rounded up at init: a bus cycle starts on a
  2-colour-clock slot), and DMA in its even slots costs it time in
  `ami.stall`, taken before it runs again: lores planes 5-6 and hires
  planes 3-4 a slot per word fetched, the copper two per instruction, the
  blitter its channels' slots (3/4 of them without BLTPRI), spread over
  its lines; `SLOT_WAIT` (3) clocks a slot (it wants the bus about 3/4 of
  the time); at most 4 lines owed, dropped when a slice begins with the
  PC in fast RAM or the ROM (the 68000's own bus; data accesses from
  there to chip RAM aren't counted). SysInfo 4.4: 1.00 times a real A600
  (chip and slow RAM), 1.00 times a B2000 (fast RAM); xSysInfo 0.12: 1.09
  and 1.00 (bare Musashi was 1.46 times an A600).
- **Memory** (`mem.c`): 64 KB banks to host pointers for reads and writes
  (chip RAM mirrored by its size over $000000-$1FFFFF, the ROM over it for
  reads while OVL is set, slow RAM at $C00000, fast RAM where autoconfig
  put it, the ROM at $F80000 and 256 KB ones mirrored); NULL banks take
  the slow path: CIAs ($A00000-
  $BFFFFF: CIA-A when A12 is low on odd bytes, CIA-B when A13 is low on
  even ones; a byte write goes to both halves of the bus), custom chips
  ($DFF000, and $C00000-$D7FFFF without slow RAM, above it with: the
  mirror Kickstart 1.x tests for). Write-only and missing custom registers
  read the last word the 68000 put on the chip bus (`ami.bus`: Dr. Mario
  counts in $DFF0FA). No bus errors; empty space reads 0. Words in host
  order, byte n at n ^ 1 (the ST's convention).
- **Gayle** (`gayle.c`, the A600's; on the other models while a hard disk
  is in): its IDE port at $DA0000 (register (address >> 2) & 7, the
  control block with bit 12: $DA2000-$DA201C and $DA3018 are what
  scsi.device uses), interrupt status/enable at $DA9000/$DAA000 (level 2,
  sticky like the CIAs'), the ID at $DE1000 ($D0, a bit a read). Two ATA
  drives on HDF images (read and written in place, `read()`/`write()` past
  stdio from a buffer aligned to the cache line): CHS through a geometry
  that divides the image exactly (Hannibal's 14336 blocks: 16/16/56) or
  LBA, READ/WRITE (MULTIPLE), IDENTIFY, INITIALIZE DEVICE PARAMETERS, SET
  MULTIPLE, the rest accepted; two lines a sector, then the interrupt.
  Kickstart 2.05 and 3.1 (the A600's ROMs) mount an RDB disk on the
  master (their FFS); they never look for a slave. An RP9's hard disk is
  unpacked once into `.<name>.rp9.hdf` beside it (3.3 s for 7 MB on the
  board; the package replaced or deleted: the copies go, `disk_removed()`).
- **Autoconfig** (`autoconf.c`): the boards at $E80000 one after another
  (nibbles, all but er_Type inverted; the address written low nibble
  first, the high one configures; $4C shuts up), unconfigured by a reset
  (the RESET instruction too). Now one board: fast RAM, 1-8 MB (Zorro II,
  "into the memory list", Commodore's prototype ID $07DB): Kickstart 1.3
  and later put it at $200000 and add it to the free memory. The board
  allocates 8 MB at power-on whatever the setting.
- **Paula's interrupts** (`amiga.c`, `custom.c`): INTENA/INTREQ to levels
  1-6, autovectored (VERTB 8 clocks into line 0: `VERTB_DELAY`); the CIAs' lines are levels (PORTS/EXTER set again
  while a CIA still asserts after INTREQ is cleared).
- **CIAs** (`cia.c`): timers (one-shot: writing the high byte loads and
  starts), TOD with latch and halt, alarm, ICR, SDR (output shifted at
  once), the ports: CIA-A PRA bit 0 OVL (`mem_update_map()`), CIA-B PRB the
  drives' lines (`floppy.c`: select, motor latched at select, step,
  direction, side; /CHNG low until a step with a disk in; the ID read
  through /RDY; DF0: and DF1: present). CIA-B's FLAG: the index pulse.
- **Floppies** (`floppy.c`): ADF images kept in memory (PSRAM on the
  board), written tracks back to the file at `amiga_floppy_flush()` (the
  board: every second once the disk was still for 2 s). The disk turns at
  300 rpm: 6334 MFM words a track, 224 clocks a word, moved on at each
  line's end. The track under the head is made from the image in
  AmigaDOS's format (11 sectors: sync words, header, checksums, odd/even
  MFM); Paula's disk DMA (DSKLEN written twice, WORDSYNC waits for DSKSYNC
  and doesn't transfer it, DSKBLK at the end, DSKSYN as the sync word
  passes, DSKBYTR roughly) reads it or writes over it, and a written track
  is decoded back into the image's sectors when the write ends. Paula
  syncs bit by bit (a shift register over the stream: syncs at any bit
  offset, as Son Shu Shi's loader needs). Speed per disk: 8x for an ADF
  with an AmigaDOS filesystem (`dos_filesystem()`: the root block's type
  and checksum) and an RP9 marked `turbo-floppy`, else real (300 rpm: a
  custom loader may time the disk). Workbench 1.3 reaches its desktop in
  ~40 s (~65 s at real speed, as an A500).
  **Extended ADFs** (UAE-1ADF: raw MFM tracks, any length up to 8192 words)
  are read-only, at real speed. **RP9** (`rp9.c`, Amiga Forever's
  packages: a ZIP with `rp9-manifest.xml`): the floppies unpacked (stored
  or deflated: miniz's `tinfl` on the board, its 10 KB state on the heap,
  zlib on the host) into hidden `.<name>.rp9.<n>.adf` beside it and used
  as ADFs; inserting the same package again gives its next disk (the drive
  detail says which). Hard disk images in an RP9 wait for phase 5.
- **Input** (`input.c`): key codes through CIA-A's serial port (rotated,
  inverted), the next only after the handshake (CIA-A's serial port to
  output and back) or 143 ms; $FD, $FE at reset; Ctrl and both Amiga keys:
  $78, then a warm reset 10 frames later (the CPU and chips reset, memory
  kept). The mouse in JOY0DAT's counters, its buttons on CIA-A PRA bit 6
  and POTGO's pins (a button pulls a pin low even driven high as an
  output); the joystick in JOY1DAT, fire on PRA bit 7.
- **Sound** (`audio.c`): Paula's four channels per line (DMA with the
  buffer reload and the interrupt at each, or AUDxDAT written), each
  sample's time averaged into 44.1 kHz stereo (0+3 left, 1+2 right), the
  A500's fixed low-pass and the LED's filter (CIA-A PRA bit 1 low), into a
  ring the runtime's speaker task drains (and paces the emulation by).
- **The clock** (`rtc.c`): an MSM6242B at $DC0000 on every model (register
  n at $DC0000 + 4n, the low nibble), the host's local time plus an offset
  that setting it changes (the board keeps it in NVS). HOLD freezes the
  digits for setting. Kickstart 1.x gets years from 2000 as 100 and more
  (see the lessons). The host build's clock counts frames.
- **Copper** (`copper.c`): restarted from COP1LC at line 0; MOVE 4 colour
  clocks (8 cycles), WAIT's end found ahead (`wait_until()`: the masks, the
  8-bit vertical compare that wraps after line 255, BFD), a cycle to wake
  up after it (2 colour clocks: see the lessons), SKIP; the danger
  rule (OCS $80+, $40+ with CDANG; ECS $40+, all with CDANG). Writes the
  68000 can't notice (colours, bitplane pointers and modulos, BPLCON1-3,
  sprites) run ahead of the CPU up to the line's end or its fetch; others
  wait for the CPU to get there (`copper_next()` is the write's time).
- **Blitter** (`blitter.c`): area mode (minterms with fast paths, shifts,
  masks, descending, inclusive/exclusive fill through tables), line mode
  (octants as SUD/SUL/AUL, SING, texture), ECS BLTSIZV/BLTSIZH/BLTCON0L.
  Done at once; BBUSY and the interrupt wait for the real duration (words
  x colour clocks per word by channels; lines 4 a pixel).
- **Display** (`display.c`): Agnus's vertical window flip-flop (opens at
  VSTART, closes at VSTOP, reset at line 0; no DMA in vertical blank), the
  fetch at DDFSTRT's position (words: ((stop-start)/8+1), x2 in hires;
  first pixel lores DDFSTRT*2+17, hires +9; whole units: `(stop - start
  + 7) / 8 + 1`), modulos after it. ECS
  DIWHIGH (written after DIWSTRT/DIWSTOP) gives the window's high bits.
  Each line becomes a **record** (`row_t`): the fetched words, BPLCON0-2,
  the window, the colours as the line began, each colour write with the
  pixel the beam was at (`4h - 210` in picture pixels). Three frames of
  records, latest/pinned as the PC's pictures; a record with the same key
  (hash of its inputs) as the one already in that slot isn't rewritten,
  the frame's signature is the keys' hash. The presenter (core 0) draws
  records (`amiga_render_line_rgb565()`): planes to indices by a table
  (odd and even planes apart for their scroll delays), dual playfield,
  EHB, HAM, colours changed at their pixels, then the sprites (the
  record holds those armed on the line: number, position, DATA/DATB;
  attached pairs, lower numbers in front, BPLCON2's PF1P/PF2P against
  the playfield under each pixel, clipped to the window). **Collisions**
  (CLXCON, CLXDAT): worked out at each line's end, but only while a
  program reads CLXDAT (for 4 frames after each read): the armed sprites'
  pixels against each other and against the playfields (a playfield
  matches where CLXCON's enabled planes have its values; a plane not
  fetched is 0), odd planes against even, inside the window. Sprite DMA:
  control words on the line before the first bitplane line (25 PAL) and
  at each VSTOP, data words from VSTART. Interlace shows the field being
  drawn (both fields: later).
- **The picture**: 720 x 568 (lores pixels 108-467 doubled, lines 29-312
  doubled), 4:3 at 960 x 720 on the Tab5 (`view_width` 800).

## Lessons (each one cost a debugging session)

- **ECS's DIWHIGH matters**: Kickstart 3.1's copper list opens a window at
  line 1 with VSTOP 2 through DIWHIGH = 0 (OCS rules would make VSTOP 258):
  without it the window stayed open, the next screen's fetch began a line
  early, and its bitplane pointers (one word before the bitmap, for the
  scroll word) showed the bitmap a row late with garbage in the last line.
- **Drawing pixels on core 1 cost too much**: rendering every line into
  RGB565 frames took ~15% of core 1, copying them into PSRAM another ~29%
  (82% busy at the idle insert-disk screen when every line changed). The
  line records written instead cost ~3% for a fully changing picture, and
  the presenter draws them on core 0.

- **The fetch counts whole units, DDFSTOP's too**: Kickstart 1.3's hires
  screens use $3C-$D0 with modulo 0 on 80-byte rows: 40 words, where
  `(stop - start) / 8 + 1` gave 38 and sheared every row by 4 bytes
  (Workbench's text came out as dots). Rounding up fits 1.3, 3.1's $40-$D0
  (38 words, its modulo -6 says so) and the HRM's values.
- **1.3's SetClock wants a ticking clock and can't count past 1999**: it
  reads the seconds, waits 1.5 s and reads again (a clock that doesn't
  move isn't there: the host build, ~150x real time, must count emulated
  time), and it counts years up from 78 to the clock's two digits, so
  2026 ("26") came out as 21-Oct-78. It takes the tens digit as any 4-bit
  value: with Kickstart 1.x the clock gives 2026 as 12, 6 (126: 48 years
  after 1978), and SetClock SAVE's digits come back right either way.
  Kickstart 2.0+'s battclock reads plain digits (2026 right).

- **The right mouse button and POTGO**: input.device and Test Kit drive
  the pot pins as outputs set high ($FF00) and still see the button pull
  the line low; honouring the button only on input pins made it dead.
- **Kickstart 1.2 sizes slow RAM by the custom chips' mirror**: without
  the mirror above the 512 KB of slow RAM it took more of $C00000-$D7FFFF
  for RAM, put its stack at $DC0000 (the clock) and showed the yellow
  screen. 1.3 doesn't mind either way.
- **VERTB at line 0's very first clock hung F1 GP Circuits**: it waits at
  line 0; raised 8 clocks in (the interrupt reaching the 68000), it runs.
- **The copper wakes up a cycle after a WAIT**: without that cycle the
  instruction after `WAIT $FFDF` (line 255's last position) was compared
  still in line 255, where any later WAIT ($2C07, for line 300) passed at
  once (255 > $2C): everything PAL programs put below line 256 (Kick
  Off's panel, high score tables' last rows, Test Kit's PAL frame) showed
  43 or more lines early, or not at all. With it the next compare falls
  in line 256 (vertical 0).
- **Gayle swaps the IDE bus's bytes**: a sector reads in its own byte
  order (an HDF's), so IDENTIFY's 16-bit words arrive swapped and
  scsi.device swaps them back. Sent in the Amiga's order, the geometry
  came out as 4096 heads and 0 sectors: INITIALIZE DEVICE PARAMETERS
  selected the absent slave and the boot waited forever.
- **SD writes from an unaligned buffer go sector by sector**: unpacking
  an RP9's hard disk wrote at 0.35 MB/s (20 s for 7 MB; the inflating took
  0.25 s) until the output was gathered into 64 KB aligned to the cache
  line and written past stdio (3.3 s), as the runtime's uploads do.
- **Inflating on the httpd's stack rebooted the board**: miniz's
  `tinfl_decompressor` is ~10.5 KB; inserting an RP9 from the web page
  (the web server's 6 KB stack) crashed the board. On the heap now.

## Testing

- **Host build**: `make -C components/amiga/host run KICK=roms/amiga-os-130.rom`,
  or `build/ami_host <kickstart.rom> [frames] [out.ppm]`, far faster than
  real time. Environment: `AMI_CHIP_KB` (512), `AMI_SLOW_KB` (512; 0 none),
  `AMI_ECS`, `AMI_NTSC`, `AMI_SERIAL` (the serial port's output),
  `AMI_SHOTS=<n>` (a picture every n frames), `AMI_LOG=<n>` (the state
  every n frames), `AMI_RING=<n>` (the last instructions at the end),
  `AMI_STOP=<pc>[:<frame>]` (the 60 instructions before and the registers),
  `AMI_TRACE=<pc>:<n>`, `AMI_EXC` (exceptions; `all` with interrupts),
  `AMI_DEBUG=<port>` (the debugger, real time), `AMI_DF0`/`AMI_DF1` (ADF
  images: they get written, use copies), `AMI_INSERT=<frame>:<drive>:<adf>
  [;...]` (an empty name ejects), `AMI_INPUT=<frame>:m:<dx>:<dy>:<buttons>
  | <frame>:k:<code hex>:<1|0> | <frame>:j:<bits hex> [;...]` (scripted
  mouse, keys, joystick; mouse moves over 100 spread over frames),
  `AMI_WAV=<file>` (the sound). macOS's `setjmp` saves the
  signal mask: Musashi's two per `m68k_execute()` show up as
  `sigprocmask` in host profiles (not on the ESP32).
- Kickstart 1.3 (34.5) shows its insert-disk hand after ~210 frames (4.2
  s); 2.04, 2.05 and 3.1 their rainbow tick and drive (3.1 animates the
  disk); checked on the host and the Tab5.
- Workbench 1.3 (with 1.3) reaches its desktop after ~3500 frames (70 s),
  2.05 (with 2.05, A600, 1 MB) and 3.1 (with 3.1) their Workbench window
  within 6000. Original ADFs: the user's collection,
  a local folder etc. (WB-1_3D.ADF is a German 1.3.3).
- **Self-testing disks** (`tools/adf.py`, a copy of a Workbench disk with
  startup-sequence lines replaced by as many bytes): writes (`copy
  s:Startup-Sequence to df0:Written`, then compare the two files in the
  image), the clock (`Date >DF0:When`; `Date 01-Jan-25`, `SetClock SAVE`,
  `SetClock LOAD`, `Date >DF0:When`). Checked on the host and the Tab5
  (the board: the image downloaded back from the web page).
- **Hard disks**: `AMI_MODEL=2 AMI_ECS=1 AMI_CHIP_KB=1024 AMI_SLOW_KB=0
  AMI_FAST_KB=1024 AMI_HD0=<copy of an HDF>` with the A600's 2.05 or 3.1.
  Amiga Forever's workbench-135.hdf (an RDB disk, FFS in the RDB) boots to
  Workbench 1.3.5's desktop, on the host and the Tab5. The RP9 games' own
  disks (af-application.hdf, volume AF-Application) aren't bootable alone:
  no C: or S:Startup-Sequence; Amiga Forever boots its system disk
  (`<boot type="hdf">135</boot>`: workbench-135.hdf), whose startup runs
  `AF-Application:S/user-startup`. Alone they stop at the `1>` prompt.
  `scratchpad`-style tools: an RDB lister and an FFS tree lister were
  enough to see that (not in the repo).
- **Amiga Test Kit** 1.21 (Keir Fraser, public domain,
  `github.com/keirf/amiga-stuff` releases; `AmigaTestKit.adf` is on the
  Tab5's card): F-keys pick the tests ($50 F1 ...), Esc goes up, Ctrl+Left
  Alt to the main menu. Passed on the host: CIA timers (all), chipset IDs
  (OCS: Denise $FFFF, Agnus $0000), memory (1 MB found, Kickstart's and
  the direct scan; rounds without errors), keyboard (codes), controller
  ports (mouse, both buttons, joystick), audio (its Protracker module in
  both channels), the clock ("6242/7242 at $DC0000", 2026), floppy (IDs,
  all 160 tracks read, the write test on a blank disk, READY 400 ms after
  the motor). Not run yet: its video tests.
- **Collisions**: `tools/clxboot.py out.adf <clxcon hex> <plane: 40 bytes
  hex, 80 for hires> [n:hstart:data ...]` makes a boot disk that takes the
  machine over: one plane's line repeated, manual sprites, every CLXDAT
  read ORed into $7F000 (`AMI_DUMP=7F000:2:out.bin`, 700 frames). Cases
  checked (expected values from the HRM's rules, lores and hires):
  overlapping or not by one pixel, MVBP 0/1, sprite pairs, ENSP1.
- Amiga Test Kit's video tests (F6): all six draw; F1's PAL/NTSC frame
  (markers at lines 44, 243, 299) as its source describes.
- `say hello` in Workbench 1.3's Shell (narrator.device) as a sound test;
  `AMI_WAV` records it on the host.
- **On the board**: status via the web API, pictures via the console's
  `lcd`, the setup screen via `t x y`, the Amiga via `k`/`m`/`j`.

## Open items

- Phases 4-7 (`docs/architecture.md`): games (the rest of phase 4:
  blitter timing per slot), A2000/A600
  specifics (autoconfig fast RAM, Gayle IDE, the hard disk board, the RP9s
  with hard disk images), NTSC details, interlace's two fields, the A1200
  (the 25 A1200 RP9s). The chip bus model is an average (`SLOT_WAIT`), not
  per access: a slot-exact one (the 68000's accesses against the line's
  DMA slots) would bring SysInfo and xSysInfo to 1.0; the Tab5 has the
  room (core 1 ~25% in SysInfo).
- AROS needs its extension ROM mapped at $E00000 (it stays black now); not
  on the setup screen yet.
- `.adz`/`.dms` converters for the web page (the user's collection has
  DMS files beside the ADFs); extended ADFs can't be written.
- The RP9 games with a hard disk need their system disk beside it: next,
  one virtual disk made of the system HDF's and the game's partitions (an
  RDB of our own: Gayle's scsi.device mounts the master only), then our
  own hard disk board for Kickstart 1.2-2.04 (their RP9s want 1.3).
- A local git repository (`main`), not on GitHub yet. The attach modes
  of Paula's channels, absolute touch positioning (the pointer under the
  finger) are not done. Collisions are checked by the test disks only:
  the one game that reads CLXDAT (X-Balls) behaves the same without
  them. Sprites show (and collide) outside the vertical window too:
  whether OCS Denise hides them there isn't settled.

## Working with the user

- Expects changes built, flashed and checked on the board, then a short
  report of what was verified and what wasn't.
- Keep their private data (WiFi name, IP, SD card contents) out of anything
  meant for publishing.
