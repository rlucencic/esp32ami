# esp32ami: Amiga 500, 2000 and 600 on the M5Stack Tab5

An Amiga 500, 2000 or 600 (OCS/ECS, a 68000 at 7.09 MHz PAL) on the M5Stack
Tab5 (ESP32-P4) only: the user's call, the MaTouch's S3 hasn't the speed for
the 68000 and the custom chips together. The A1200 (AGA, a 68020) is a later
phase if the P4 has the speed. Plan and decisions: `docs/architecture.md`
(its phases table; phases 1 and 2 are done, see Progress there). ESP-IDF
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
  `roms/README.md`, FS-UAE's file names) to partitions `kick_130` (256K),
  `kick_204`, `kick_205_a600`, `kick_310_a600`, `kick_aros`, `kick_aros_ext`
  (512K each); `app-flash` only the app. The Tab5 is shared with the other
  projects: after another one, a full `flash`. NVS (WiFi, BLE pairings) is
  shared; this project's settings are namespace `ami` (`model`, `kick`,
  `chip_kb`, `slow_kb`, `ntsc`, `rtc_ofs`: the clock's offset in seconds),
  its drives `amifloppy`.
- Disk images: `.adf` in the SD card's `floppies/` folder (shared with
  esp32arc: its 800 KB ADFS images are refused by size). Upload with
  `curl -T disk.adf http://amiga.local/disks/`, insert with
  `curl -X PUT --data "disk.adf" http://amiga.local/api/drive/1` (2: DF1:).
- `../esp32emu/tools/emu_console.py PORT log N | lcd F.png | send "cmd" | info`
  with the IDF venv's Python (`-I`). `curl http://amiga.local/api/status`
  (`board.load`: the cores' busy %).
- Flashing restarts the board and ends the user's session: say so first.

Console commands: the shared ones (`emu/console.h`) and `R <model> <kick>
[chip KB] [slow KB]` (`main/main.c`): restart as model 0 A500, 1 A2000, 2
A600 with Kickstart 0 1.3, 1 2.04, 2 2.05, 3 3.1 (the model's own: the A600
2.05 or 3.1, the others 1.3, 2.04 or 3.1), chip RAM 512/1024 (2048 the
A600), slow RAM 0/512 (not the A600).

Setup screen on the Tab5: Start `t 1065 637`; rows Model (y 213), Kickstart
(268), Chip RAM (323), Slow RAM (378), Video (433); three-button rows at x
367, 604, 841, two-button ones at 426, 781. The status lines show the WiFi
name and IP: keep them out of published images.

## Layout

| Path | What |
|---|---|
| `main/main.c` | The Amiga as an `emu_machine_t`: memory, start, setup rows, drives (DF0:, DF1:, written tracks to the card), the clock's offset, status, console |
| `main/rom_store.c` | Kickstarts from their flash partitions, else files in the SD card's `roms/` with that version |
| `main/settings.c` | NVS `ami`, which Kickstart and RAM each model takes, its defaults |
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
  line; CIA-B's TOD counts lines, CIA-A's frames. No chip RAM contention
  yet (the 68000 always has the bus).
- **Memory** (`mem.c`): 64 KB banks to host pointers for reads and writes
  (chip RAM mirrored by its size over $000000-$1FFFFF, the ROM over it for
  reads while OVL is set, slow RAM at $C00000, the ROM at $F80000 and
  256 KB ones mirrored); NULL banks take the slow path: CIAs ($A00000-
  $BFFFFF: CIA-A when A12 is low on odd bytes, CIA-B when A13 is low on
  even ones; a byte write goes to both halves of the bus), custom chips
  ($DFF000, and $C00000-$D7FFFF without slow RAM: the mirror Kickstart 1.x
  tests for). No bus errors; empty space reads 0 (autoconfig too: no
  boards). Words in host order, byte n at n ^ 1 (the ST's convention).
- **Paula's interrupts** (`amiga.c`, `custom.c`): INTENA/INTREQ to levels
  1-6, autovectored; the CIAs' lines are levels (PORTS/EXTER set again
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
  is decoded back into the image's sectors when the write ends. Real speed
  (Workbench 1.3 takes ~65 s to its desktop, as a real A500).
- **The clock** (`rtc.c`): an MSM6242B at $DC0000 on every model (register
  n at $DC0000 + 4n, the low nibble), the host's local time plus an offset
  that setting it changes (the board keeps it in NVS). HOLD freezes the
  digits for setting. Kickstart 1.x gets years from 2000 as 100 and more
  (see the lessons). The host build's clock counts frames.
- **Copper** (`copper.c`): restarted from COP1LC at line 0; MOVE 4 colour
  clocks (8 cycles), WAIT's end found ahead (`wait_until()`: the masks, the
  8-bit vertical compare that wraps after line 255, BFD), SKIP; the danger
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
  EHB, HAM, colours changed at their pixels. Interlace shows the field
  being drawn (both fields: later).
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
  [;...]` (an empty name ejects). macOS's `setjmp` saves the
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
- **On the board**: status via the web API, pictures via the console's
  `lcd`, the setup screen via `t x y`.

## Open items

- Phases 3-7 (`docs/architecture.md`): keyboard/mouse/joystick, sprites
  (no pointer yet), Paula's audio, chip RAM contention and
  blitter timing, A2000/A600 specifics (RTC, autoconfig fast RAM, Gayle
  IDE, the hard disk board), NTSC details, interlace's two fields, the A1200.
- AROS needs its extension ROM mapped at $E00000 (it stays black now); not
  on the setup screen yet.
- Floppies at real speed only (a faster option if loading proves painful);
  `.adz`/`.dms` converters for the web page (the user's collection has
  DMS files beside the ADFs); Amiga Test Kit's floppy tests need input
  (phase 3).
- Not on GitHub yet; no git repository yet.

## Working with the user

- Expects changes built, flashed and checked on the board, then a short
  report of what was verified and what wasn't.
- Keep their private data (WiFi name, IP, SD card contents) out of anything
  meant for publishing.
