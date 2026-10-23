# esp32ami: Amiga 500, 2000, 600 and 1200 on the M5Stack Tab5

An Amiga 500, 2000 or 600 (OCS/ECS, a 68000 at 7.09 MHz PAL) or 1200 (AGA,
a 68EC020 at 14.19 MHz) on the M5Stack
Tab5 (ESP32-P4) only: the user's call, the MaTouch's S3 hasn't the speed for
the 68000 and the custom chips together. Plan and decisions: `docs/architecture.md`
(its phases table; phases 1 to 6 are done, 7, the A1200, under way: see
Progress there). **What's next: `TODO.md`**, the remaining work sketched
and in order; asked "what's next", name its first item.
ESP-IDF
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
  `kick_204`, `kick_205_a600`, `kick_310_a600`, `kick_aros`, `kick_aros_ext`,
  `kick_300_a1200`, `kick_310_a1200` (512K each; the last two taken from
  the unused `storage` partition, 2026-10-21); `app-flash` only the app. The Tab5 is shared with the other
  projects: after another one, a full `flash`. NVS (WiFi, BLE pairings) is
  shared; this project's settings are namespace `ami` (`model`, `kick`,
  `chip_kb`, `slow_kb`, `fast_kb`, `ntsc`, `rtc_ofs`: the clock's offset
  in seconds),
  its drives `amifloppy`.
- Disk images: `.adf`, `.hdf` and Amiga Forever's `.rp9` games in the SD
  card's `floppies/` folder (shared with esp32arc: its 800 KB ADFS images
  are refused by size; its hard disks are `.hdf` too:
  leave them). Drives DF0:, DF1: (ADFs, RP9s with floppies; the API's 1,
  2) and DH0:, DH1: (HDFs, RP9s with a hard disk; 3, 4). Uploads up to
  256 MB.
  Blank disks: 880 KB floppies, 20, 50 or 100 MB hard disks (an RDB with
  one partition, the old filesystem's type, to be formatted on the Amiga;
  `curl -X POST "http://amiga.local/api/new?size=52125696"`).
  The web page unpacks DMS archives and gzipped images (.adz, .hdz) in
  the browser (`main/web/amifiles.js`) and uploads the ADF/HDF.
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
Workbench 1.3 disk's German keymap puts Y on $31), `K <hex> 0|1`, `T
<text>` (typed with the US keymap's keys, one every 4 frames, `\n` for
Return: `T newshell\n` in Workbench 3.1's Execute Command), `m dx
dy btn` (the mouse; 1 left, 2 right, 4 middle; held until the next `m`;
moves over 100 spread over 25 ms steps: one `m 0 0 1` then `m 0 0 0` is a
click, two pairs a double-click), `j <hex>` the joystick (1 up, 2 down, 4
left, 8 right, 10 fire, 20 second button), `R <model> <kick> [chip KB]
[slow KB] [fast KB]`: restart as model 0 A500, 1 A2000, 2 A600, 3 A1200 with Kickstart 0
1.3, 1 2.04, 2 2.05, 3 3.1, 4 1.2, 5 AROS, 6 3.0 (A1200), 7 3.1 (A1200) (the
model's own: the A600 2.05, 3.1 or AROS, the A1200 6, 7 or AROS, the others 1.2,
1.3, 2.04, 3.1 or AROS), chip RAM 512/1024 (2048 the A600; the A1200 only 2048), slow RAM
0/512 (not the A600 or A1200), fast RAM 0/1024/2048/4096/8192. Workbench 1.3 from the console: the pointer starts
top left; `m 300 100 0` then `m 291 0 0` reaches the disk icon (counts
are hires pixels across, half lines down), the Shell icon in its window
is `m -523 136 0` from there.

Input on the board: Bluetooth/USB keyboards by position (F11 Help,
Windows/Command the Amiga keys, Caps Lock toggles; F12 makes the cursor
keys and keypad the joystick in port 2, Alt fire), mice in port 1, touch as
esp32emu's touchpad (move, tap left click, tap then touch again to drag, hold still
0.5 s for the right button: the menus).

Setup screen on the Tab5: Start `t 1065 637`; the six rows scroll (the
shared setup screen's box): Model (y 227: A500, A2000, A600, A1200 at x 337,
514, 692, 870), Kickstart (298), Chip RAM
(369), Slow RAM (440), Fast RAM and Video below (`t 425 440 800 425 240`
drags them up: then Chip RAM 258, Slow RAM 329, Fast RAM 400, Video 470);
three-button rows at x 367, 604, 841, four-button ones 337, 514, 692,
870, two-button ones 426, 781, Fast RAM's five 320, 461, 603, 745, 887.
The status lines show the WiFi name and IP: keep them out of published
images.

## Layout

| Path | What |
|---|---|
| `main/main.c` | The Amiga as an `emu_machine_t`: memory, start, setup rows, drives (DF0:, DF1:, written tracks to the card), the clock's offset, key map, mouse, joystick keys, the touchpad (super-hires pixels halved), sound, status, console |
| `main/rom_store.c` | Kickstarts from their flash partitions, else files in the SD card's `roms/` with that version |
| `main/settings.c` | NVS `ami`, which Kickstart and RAM each model takes, its defaults (A2000: 1 MB chip, 2 MB fast) |
| `components/amiga/` | The machine, platform independent (`include/amiga.h`) |
| `components/amiga/host/` | Host build for testing on the Mac (see Testing) |
| `tools/adf.py` | Reads ADFs and patches files in them (test disks, see Testing) |
| `tools/rdb.py`, `tools/ffsls.py` | An HDF's RDB, its first partition's files |
| `tools/clxboot.py` | Boot disks that test sprite collisions |
| `tools/laceboot.py` | A boot disk that tests interlace (white long frames, black short ones) |
| `tools/blitboot.py` | A boot disk that times a blit with 0, 4, 6 lores and 4 hires planes (`--check` compares a dump with the slots worked out) |
| `tools/attachboot.py` | Boot disks that test Paula's attach modes (`check` measures the host's recorded sound) |
| `tools/shresboot.py` | A boot disk that tests ECS super-hires: colours, scroll, sprites, mixed lines (`check` reads a picture) |
| `tools/extadf.py` | Extended ADFs from ADFs (AmigaDOS or raw tracks) and back; a boot disk that writes a raw track |
| `tools/agaboot.py` | Boot disks that test AGA: FMODE 2, super-hires colours, 32-pixel sprites, 35 ns positions, CLXCON2, wide sprites' collisions |
| `tools/mkinstall.py` | A Workbench floppy that formats DH0: and copies itself onto it (the hard disk test) |
| `tools/hdrom.py` | Assembles the hard disk board's boot ROM into `components/amiga/hdrom.h` |
| `tools/collection.py` | Runs a folder of RP9 games on two host builds and compares their pictures (see Testing) |
| `main/web/amifiles.js` | The web page's converters (served as /machine.js): DMS archives, gzipped images |
| `tools/amifiles_test.js` | Its test: `node tools/amifiles_test.js [folder of DMS+ADF pairs]` |
| `roms/` | The Kickstarts (copyrighted, not distributed) |
| `partitions.csv` | nvs, phy, factory 8 MB, the eight ROMs (type 0x40), storage |

## The core (`components/amiga`)

- **Time**: the 68000's clocks since the frame began (2 a colour clock, 454
  a line; PAL 313 lines, 312 in interlace's short frames; NTSC 263/262).
  The beam's standard (`ami_ntsc_beam()`): the setup's Video choice
  (`ami.ntsc`: VPOSR's Agnus ID, Paula's clock), on ECS BEAMCON0's PAL bit
  instead, written by programs at any time (graphics.library sets it from
  GfxBase's DisplayFlags; Kickstart 3.1's Early Startup Control toggles it
  with a key, for its own screen only). NTSC's lines alternate 227 and
  228 colour clocks (227.5 on average, 59.83 frames a second), the turns
  running on across frames (`ami_long_line()`, `ami_line_start()`,
  `ami_beam_at()`; VPOSR's LOL on ECS; decided at the frame's start);
  PAL's arithmetic is the old one (Kick Off 2's pictures bit for bit).
  At a frame's end every kept time moves back by the frame's length
  (`amiga_run_frame()`); the E clock (CIA timers, CPU / 10) keeps its phase
  in `ami.e_phase`.
- **The frame loop** (`amiga.c`): per line, the 68000 runs up to the next
  event: the bitplane fetch, the copper's next step, the blitter's end, a
  CIA timer's underflow (with its interrupt enabled). A write that makes an
  earlier event (COPJMP, a blit, a CIA timer) or raises the interrupt
  level ends the CPU's slice (`ami_cpu_wake()`: Musashi samples the IRQ
  lines only as a slice starts). At the line's end the display records the
  line; CIA-B's TOD counts lines, CIA-A's frames. **Poll loops**
  (`custom.c`, `poll_wait()`): a read of DMACONR, VPOSR or INTREQR from a
  loop of two instructions, the read and a Bcc back to it
  (`mem_poll_loop()`: Musashi's disassembler for its length, cached by PC),
  lets the slice's rest pass at once: those change only where a slice ends
  (an event, the line's end), so the loop leaves at most a pass sooner. A
  loop that does more (a speed test counting its passes) runs as it is;
  SysInfo the same before and after on both CPUs. For the host's time:
  with the 68EC020's cache Aerial Racers' `btst #0,$dff005 / bne` ran 2.6
  times the instructions (117 M in the collection's 7000 frames, 33 M with
  the skip, 45 M before the cache).
- **The 68000 and the chip bus, slot by slot** (`mem.c`): its
  instructions take a multiple of 4 clocks (the cycle table rounded up at
  init: a bus cycle starts on a 2-colour-clock slot). Each line has a map
  of the even colour clocks the DMA takes from it (`bus_take()`):
  `display_fetch()` marks lores planes 5-6, hires 3-4 and super-hires 2 at
  their places in each 8-clock unit from DDFSTRT (the HRM's order), the
  copper its two a instruction; refresh, disk, audio, sprites and up to 4
  lores planes use odd ones. Each access of the 68000's to chip RAM, slow
  RAM or the chips (and their mirrors) is at its instruction's start plus
  4 clocks a bus cycle before it in that instruction (Musashi's remaining
  cycles changing marks a new one) and waits past the taken slots from
  there (`s_wait_at[]`, worked out when the map changed); a blit under way
  costs an access 6 clocks (BLTPRI off) or the rest of the blit (on).
  Fast RAM and the ROM: no wait. The 68EC020 keeps its averages
  (`ami.stall`: `SLOT_WAIT` a slot for the copper and the blitter).
  SysInfo 4.4: 1.03 times a real A600 (chip and slow RAM), its chip speed
  1.12; xSysInfo 0.12: 1.06, chip RAM 2.19 MB/s (the averages before:
  1.00 and 1.09, chip speed 1.16 and 2.26 MB/s; bare Musashi was 1.46).
  The same on the Tab5 (core 1 25% in SysInfo); the host ~23% slower.
- **The A1200's 68EC020**: Musashi's type chosen at run time
  (`CONFIG_M68K_RUNTIME`, esp32emu; `-DM68K_RUNTIME` in the host build: no
  slower for the 68000, host and Tab5), set before the tables' copies (they
  take its cycles). 14.19 MHz: two of its clocks a time unit
  (`ami.cpu_shift`, the odd one kept in `ami.cpu_rem`; `ami_now()`), no
  4-clock rounding. It waits for the chip bus (`mem.c`): `WAIT_CHIP` (8: two
  chip bus cycles) of its clocks an access to chip RAM (32 bits at once),
  `WAIT_CUSTOM` (8) the chips, `WAIT_CIA` (20) a CIA; ROM and fast RAM none
  (`ami_cpu_wait()`: the slice ends sooner, the time counts: inline, taken
  from Musashi's remaining cycles). Its **instruction cache**: 64
  longwords, direct-mapped, filled while CACR's E bit is set (F freezes,
  a write with C empties it; `run_cpu()` passes CACR to `mem_cacr()`
  before each slice); an instruction word from it costs no wait, from
  outside a longword's wait for both its words; only the time (the words
  always from memory: no stale code). Instruction reads come apart from
  data through `CONFIG_M68K_PROGRAM_READS` (esp32emu; `-DM68K_PROGRAM_READS`
  in the host build): `m68k_read_immediate_16/32()`, the 68000 straight to
  its usual reads. SysInfo 4.4: 1.04 times an A1200 (1270 Dhrystones,
  14.60 MHz; with the cache and the old wait of 5: 1.38; without the
  cache: 1.03, 11.30 MHz, the instruction waits making up for the cheap
  data ones). No bus errors on the Amiga: `CONFIG_M68K_NO_BUS_ERRORS`
  (`-DM68K_NO_BUS_ERRORS` in the host build) spares the 68EC020 Musashi's
  copy of its 16 registers before every instruction (the host's Aerial
  Racers run 37% faster, the pictures the same). Before the cache: SysInfo
  1.03 times an A1200 (1256 Dhrystones; without the waits 3630, 2.98 times,
  and the Tab5 at 55% in a game), "AGA ALICE - 2Meg", "AGA LISA CHIP".
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
- **Gayle** (`gayle.c`, the A600's and A1200's; on the other models while a hard disk
  is in): its IDE port at $DA0000 (register (address >> 2) & 7, the
  control block with bit 12: $DA2000-$DA201C and $DA3018 are what
  scsi.device uses), interrupt status/enable at $DA9000/$DAA000 (level 2,
  sticky like the CIAs'), the ID at $DE1000 ($D0, the A1200's $D1, a bit a read). Two ATA
  drives on HDF images (read and written in place, `read()`/`write()` past
  stdio from a buffer aligned to the cache line): CHS through a geometry
  that divides the image exactly (Hannibal's 14336 blocks: 16/16/56) or
  LBA, READ/WRITE (MULTIPLE), IDENTIFY, INITIALIZE DEVICE PARAMETERS, SET
  MULTIPLE, the rest accepted; two lines a sector, then the interrupt.
  Kickstart 2.05 and 3.1 (the A600's ROMs) mount an RDB disk on the
  master (their FFS); they never look for a slave, and Gayle answers for
  the master (DH0:) alone: DH1: is our board's.
- **Our hard disk board** (`hdboard.c`: both drives with Kickstart
  1.3-2.04, no IDE driver there; not with 1.2, see the lessons; with 2.05 and 3.1 DH1: alone, Gayle
  having DH0:; a disk is never mounted by both): autoconfig,
  Zorro II, 256 KB in the I/O space ($E90000), present while a hard disk
  is in (decided as the chain gets to it). Its boot ROM (`hdrom.h`, made by
  `tools/hdrom.py`, a two-pass assembler for the few instructions it
  needs; run it after changing the ROM, check with Musashi's disassembler)
  calls native routines through ILLEGAL ($4AFC) + a function word,
  honoured only while the PC is in the ROM. The DiagArea's DiagPoint has
  the Resident tag in expansion's copy filled in (FN_DIAG); the Resident's
  init (after expansion) builds the device in the board's RAM (its jump
  table to the ROM's stubs), AddDevice, then per partition MakeDosNode and
  a BootNode into eb_MountList (bootable ones at their priority, others
  -128; DH1:'s one lower than its RDB says while DH0: has a disk, so DH0:
  boots; names the RDB's unless taken, DH0:'s ones through Gayle included
  (read from its RDB), else the first free DHn); BootPoint starts
  dos.library. The device's commands are native
  (CMD_READ/WRITE, TD_*64, TD_GETGEOMETRY, ...: done at once, ReplyMsg
  unless IOF_QUICK). Kickstart 1.x has only the old filesystem in ROM: the
  partition's from the RDB (FSHD/LSEG) is loaded into the board's RAM (a
  hunk loader: CODE, DATA, BSS, RELOC32, RELOC32SHORT), its seglist and
  GlobalVec (-1) and at least a 4 KB stack patched into the DeviceNode.
  1.2 can't autoboot from any board (1.3 brought that).
- **Hard disk images** (`hdf.c`, under Gayle and our own board): a
  drive's blocks are pieces (files, made-up blocks, zeros). A plain HDF
  with an RDB is one piece. A bare partition (a filesystem from block 0,
  e.g. DOS\5) gets an RDB of ours in front (Amiga Forever's defaults: 2
  reserved, 32 blocks a track, one surface), so scsi.device mounts it. An
  RP9 game's disk is unpacked once into `.<name>.rp9.hdf` (3.3 s for 7 MB
  on the board; replaced or deleted package: the copies go,
  `disk_removed()`) and, with `<boot type="hdf">NNN</boot>` and
  `workbench-NNN.hdf` in the folder, joined with it into one disk: our RDB
  (the PART blocks, both disks' FSHD/LSEG chains renumbered), the system
  partitions (bootable, DH0...), the game's (not bootable, its volume
  AF-Application), each at a cylinder of its own geometry, the size a
  multiple of 16 x 63. The web page's DH0: detail says which.
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
  **High-density ADFs** (1802240 bytes, 22 sectors a track): the drive
  turns at 150 rpm (12668 words a turn, the same bit rate) and its ID says
  $AAAAAAAA while one is in (else all ones); Kickstart 3.1's trackdisk
  reads and writes them (Christmas Breakdown; a file written by a patched
  startup-sequence read back from the image, host and Tab5); their root
  block is 1760 (`dos_filesystem()`, `space.c`, `tools/adf.py`).
  **Extended ADFs** (UAE-1ADF: raw MFM tracks, any length up to 12668 words)
  at real speed, written too (`write_ext_track()`): a written AmigaDOS
  track (type 0) back into its sectors when all 11 decode with their
  checksums right, anything else as the turn's raw MFM (a raw track in
  place; an AmigaDOS one becomes raw: the tracks after it move in the
  2.5 MB buffer and the flush writes the whole image, which only grows).
  **RP9** (`rp9.c`, Amiga Forever's
  packages: a ZIP with `rp9-manifest.xml`): the floppies unpacked (stored
  or deflated: miniz's `tinfl` on the board, its 10 KB state on the heap,
  zlib on the host) into hidden `.<name>.rp9.<n>.adf` beside it and used
  as ADFs; inserting the same package again gives its next disk (the drive
  detail says which). **The machine a package names**
  (`amiga_rp9_machine()`: `<system>`, the Kickstart as `<systemrom>` or
  `<rom type="system">`, `<ram type="chip|fast|z3">`): a-500 an A500 with
  512 + 512 KB, a-500plus one with 1 MB chip RAM (ECS follows), a-2000
  an A2000 with 512 + 512 KB, a-600 an A600, a-1200 and a-4000 an A1200;
  fast RAM as asked (8 MB at most). A package in a boot drive (DF0:, DH0:)
  naming another machine than the settings: `main.c` offers it through
  esp32emu's disk offers (the setup screen's Disks page and the web page:
  "Kick Off 2 wants an A500: 512 KB chip RAM, 512 KB slow RAM, Kickstart
  1.2" and a button), read from the manifest as it goes in; taking it sets
  the settings as the console's `R` does and restarts (the user's choice
  over an automatic restart, 2026-10-22). The host takes it by itself
  unless `AMI_MODEL` is set, and with a ROM folder for its first argument
  the Kickstart too (FS-UAE's names).
- **Input** (`input.c`): key codes through CIA-A's serial port (rotated,
  inverted), the next only after the handshake (CIA-A's serial port to
  output and back) or 143 ms; $FD, $FE at reset; Ctrl and both Amiga keys:
  $78, then a warm reset 10 frames later (the CPU and chips reset, memory
  kept). The mouse in JOY0DAT's counters, its buttons on CIA-A PRA bit 6
  and POTGO's pins (a button pulls a pin low even driven high as an
  output); the joystick in JOY1DAT, fire on PRA bit 7.
- **Sound** (`audio.c`): Paula's four channels per line (DMA with the
  buffer reload and the interrupt at each, or AUDxDAT written; a channel
  whose DMA goes off finishes its word, then the interrupt; attached
  through ADKCON, a channel is silent and its words set the next one's
  volume and/or period, each in place of a byte: a volume word at the
  low byte's end, a period word at the high byte's, after WinUAE's state
  machine and the HRM's alternating volume/period), each
  sample's time averaged into 44.1 kHz stereo (0+3 left, 1+2 right), the
  A500's fixed low-pass and the LED's filter (CIA-A PRA bit 1 low), into a
  ring the runtime's speaker task drains (and paces the emulation by).
- **The clock** (`rtc.c`): an MSM6242B at $DC0000 on every model (register
  n at $DC0000 + 4n, the low nibble), the host's local time plus an offset
  that setting it changes (the board keeps it in NVS). HOLD freezes the
  digits for setting; releasing it sets the time only if a digit was
  written meanwhile (battclock reads under HOLD: a read across a second
  used to set the clock back a second and write NVS). The host build
  prints each new offset (`clock offset now`). Kickstart 1.x gets years from 2000 as 100 and more
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
  Done at once; BBUSY and the interrupt wait for the real duration: words
  x colour clocks per word by channels (lines 4 a pixel), each a bus slot
  it gets only where the line's other DMA leaves one (`plan_end()`: a
  line's colour clocks less refresh 4, disk 3, a slot an audio channel,
  sprites 16 and the bitplane fetch, `display_dma_slots()`, spread over
  the line; the copper's few not counted). `tools/blitboot.py` times a
  4096-slot blit: 18, 29, 40 and 66 lines with no planes, 4 and 6 lores
  and 4 hires planes, as worked out (18.4, 28.6, 39.8, 65.0; before: 18
  whatever the display).
- **Display** (`display.c`): Agnus's vertical window flip-flop (opens at
  VSTART, closes at VSTOP, reset at line 0; no DMA in vertical blank), the
  fetch at DDFSTRT's position (words: ((stop-start)/8+1), x2 in hires,
  x4 in super-hires; first pixel lores DDFSTRT*2+17, hires +9, super-hires
  +5; whole units: `(stop - start + 7) / 8 + 1`), modulos after it. ECS
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
  the playfield under each pixel, clipped to the window: none above or
  below it either, as on Denise, which shows them only after a line's
  BPL1DAT write (WinUAE's `sprites_hidden`; the Deniser project lists
  showing them as its bug); BRDSPRT, ECS with ECSENA or AGA, shows them in
  all the border). **Collisions**
  (CLXCON, CLXDAT): worked out at each line's end, but only while a
  program reads CLXDAT (for 4 frames after each read): the armed sprites'
  pixels against each other and against the playfields (a playfield
  matches where CLXCON's enabled planes have its values; a plane not
  fetched is 0), odd planes against even, inside the window; AGA's
  planes 7-8 through CLXCON2 (cleared by a CLXCON write), wide sprites
  all their pixels, hires and super-hires sprite pixels (SPRES) to the
  lores pixel they're in. Sprite DMA:
  control words on the line before the first bitplane line (25 PAL) and
  at each VSTOP, data words from VSTART (also a VSTART written while the
  DMA was off at line 25: Aerial Racers' cars). **Interlace**: a frame with
  BPLCON0's LACE on at any time toggles LOF; each field (long: even
  picture lines, short: odd) has its own three frames of line records, and
  the presenter weaves them once both have been drawn (frames without
  LACE go to the long field's buffers, their lines doubled). Sprites show in both
  fields (their lines doubled, as on the real one). **Super-hires** (ECS,
  BPLCON0 bit 6, masked off with OCS; at most 2 planes, fetching like 4
  hires ones, plane 2 in the 68000's slots): a frame with such a line is
  drawn 1440 wide (`amiga_screen_width()`, the frame's flag like its LACE
  one), its other lines' pixels doubled. Colours a hires pixel, as
  WinUAE has them: its two super-hires pixels a, b look up register 4b+a,
  a shows each gun's upper 2 bits, b the lower 2 (doubled: 64 colours;
  graphics.library fills the registers so each pixel gets its colour,
  Workbench's blue turns teal). Sprites: registers 16-31 the same way, a
  lores pixel's two halves paired (their colour's low 2 bits), 4 pixels a
  lores one, SPRxCTL bit 4 a 70 ns step, sprites 4-7 not drawn (WinUAE);
  dual playfield drawn as one playfield (WinUAE too). BPLCON1 counts
  lores pixels, 4 super-hires ones. graphics.library fetches from DDFSTRT $38 with
  the pointers 3 words early for DIWSTRT $81: the +5 delay puts the
  bitmap's first pixel on the window's (any other would shift it 16).
  **AGA** (the A1200: `ami.aga`; VPOSR's Alice ID $23/$33, DENISEID $00F8):
  colours kept as 24 bits for every chipset (OCS/ECS: their nibbles
  doubled, the same RGB565 as before: their pictures bit for bit), 256 of
  them (BPLCON3's bank for a register write, LOCT its low nibbles, without
  it both); the record holds the palette's hash (each colour's, XORed,
  kept up as they change) instead of the colours' hash. Up to 8 planes
  (BPU3) in any resolution, fetched in FMODE's 16/32/64 bits: in blocks
  of max(fw, r) words a plane in 8 x that / r colour clocks (fw the words
  a fetch, r 1, 2, 4 for lores, hires, super-hires: OCS's 8-clock units),
  whole blocks plus the one DDFSTOP falls in; the first pixel 16 x fw /
  that + 1 lores pixels after DDFSTRT x 2 (OCS's 17, 9, 5). Found from
  Workbench 3.1's hires FMODE 3 screen ($38-$D8: 44 words, modulo 72 with
  two interleaved planes, on its window at 17) and Deluxe Pacman's lores
  one ($38-$D0: 24 words, modulo -8). BPLCON1's 8-bit delays in super-hires pixels
(as far as a fetch reaches, at least OCS's 16 lores pixels: in FMODE 0
the top two bits do nothing, Aerial Racers' race sets them),
  HAM8 (the low two planes say what the top six do), EHB unless KILLEHB,
  BPLCON4's XOR and the sprites' colour banks, dual playfield's second
  colours from BPLCON3's offset, BRDRBLNK; super-hires in its own colours;
  sprites 16/32/64 pixels (FMODE: the control words the first of each
  fetch) in lores, hires or super-hires pixels (SPRES), placed to the
  super-hires pixel (SPRxCTL bits 4 and 3, 70 and 35 ns: "in all
  resolutions"; ECS uses bit 4 on super-hires lines), drawn on a grid
  of super-hires pixels. No bitplane stall counted for AGA.
- **The picture**: 720 x 568 (lores pixels 108-467 doubled, lines 29-312
  doubled), 4:3 at 960 x 720 on the Tab5 (`view_width` 800). NTSC:
  720 x 484 (lines 21-262, `amiga_screen_height()`), scaled to the same
  960 x 720 (taller pixels, as on an NTSC screen); the presenter follows a
  switch between frames. Super-hires frames (1440 x 568): one to one on
  the panel, not stretched (the user's call): the standard window's 1280
  columns (from lores pixel $81: the side overscan cut), all 568 rows
  centred (the overscan above and below in view, black bars), through
  `emu_screen_t`'s `view_w`/`view_h`/`src_x`/`src_w` (`screen_mode()` in
  `main.c`). 1280 x 256's lines are doubled like the others, so both
  modes fill 1280 x 512 (flatter than on a monitor). Touch: picture x
  scaled back to hires pixels.

## Lessons (each one cost a debugging session)

- **A faster emulated CPU costs host time in its waiting loops**: the
  68EC020's instruction cache made code from it free of chip waits, so a
  game's VBL poll loop ran 2.6 times as many instructions per frame; the
  Tab5 fell to 86-94% in Aerial Racers' race with core 1 at 100% (73-81%
  before). Skipping two-instruction poll loops to their slice's end
  (`poll_wait()`) brought it back.

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
- **Kickstart 2.0+'s shell refuses a file without the E bit; 1.3's
  doesn't**: Hannibal's executable (an RP9 for 1.3) is "RW-D". The joined
  disk boots Workbench 1.3.5 and runs the game's user-startup, but under
  3.1 it ends in "file is not executable": the 1.3 games need Kickstart
  1.3, so our own hard disk board (with it: Hannibal's title and menu).
- **SD writes from an unaligned buffer go sector by sector**: unpacking
  an RP9's hard disk wrote at 0.35 MB/s (20 s for 7 MB; the inflating took
  0.25 s) until the output was gathered into 64 KB aligned to the cache
  line and written past stdio (3.3 s), as the runtime's uploads do.
- **Inflating on the httpd's stack rebooted the board**: miniz's
  `tinfl_decompressor` is ~10.5 KB; inserting an RP9 from the web page
  (the web server's 6 KB stack) crashed the board. On the heap now.
- **An A600 with Kickstart 3.1 and no hard disk is black for 15-20 s**:
  the ROM's scsi.device searches the IDE port (status 0, never ready; 3.x
  ROMs on real machines wait like this, an A4000 ~30 s), then the
  insert-disk screen, or Early Startup Control. It looked like a hang.
- **A changed CFLAGS in the host Makefile doesn't rebuild the objects**:
  Musashi's objects depend on its headers, not on the Makefile, so adding
  `-DM68K_RUNTIME` left a fixed 68000 behind, and the A1200's Kickstart
  rebooted in a loop on its 68020 code (MOVEC, DIVU.L "illegal"). `make
  clean` after changing the flags.
- **Lisa's ID carries the chip RAM's bus width**: graphics.library's
  SetChipRev takes GfxBase's MemType from DENISEID's bits 9-8, inverted
  (`(~id & $300) >> 8`). $FFF8 there made it a 16-bit bus: Workbench's
  ScreenMode offered hires with 16 colours at most, and no FMODE. $00F8
  gives 32 bits, double CAS (3), as on an A1200: 256-colour hires in
  FMODE 3. (SysInfo read the IDs right all along; the host's `AMI_DUMP` of
  chip RAM and GfxBase's bytes at $EC/$ED showed it.)
- **`__builtin_ctz(0)` hung the board, not the host**: the slot map's
  scan stepped over a run of taken slots with `ctz(~word)`; with a whole
  word taken (4 hires planes take every even slot in the fetch window)
  that is ctz(0), undefined: the Mac's gives 32, the P4's moved nothing,
  and core 1 spun in `slot_wait()` (0 frames a second, Kick Off 2's
  loading screen). A table of waits a colour clock replaced the scan.
- **The 68000's phase on odd colour clocks made two loaders fail**: a
  PAL line has 227 colour clocks, so the 68000's even slots fall on the
  other parity from line to line; charging the 2 clocks a real one waits
  then brought SysInfo and xSysInfo both to 1.03, but Hyperion's loader
  stepped its drive 21359 times (a track read gone wrong) and Lethal
  Xcess stayed on a logo: without it (the cycle table's rounding keeps the
  phase) both run. Musashi knows an instruction's bus cycles, not where
  in it they fall; the slot model takes them back to back from its start.
- **Kickstart 1.2 with our hard disk board crashes in a loop**: its
  expansion.library copies a word-wide DiagArea with exec's CopyMem, then
  calls AllocMem through what it takes for its own base at $24 but is
  exec's: a jump to $00FE79 in chip RAM, an address error, the alert
  ("HELP" at 0), a reset, again (a grey screen; the CPU sampled in the
  early delay at $FC00DE or the alert's at $FC30F4). Found when an RP9's
  offer restarted the board as an A500 with 1.2 while a hard disk was in
  DH0:; `AMI_STOP=fe79` on the host showed the path. The board isn't
  offered to 1.2 (`hdboard_wanted()`), which can't boot from one anyway.
- **A drive's ID: the nth selection after its motor goes off shows bit
  31 - n**, not bit 31 first as the HRM's loop reads: with $AAAAAAAA given
  bit 31 first, Kickstart 3.1 read the HD drive as something it didn't
  know and never touched the disk (no DMA at all, the insert-disk
  screen). Double-density drives (all ones) can't tell. UAE has it so
  (its comment: checked on a real A1200).
- **A channel whose DMA goes off still raises its interrupt**: Aerial
  Racers' replay stops channels the common way (period 1, INTREQ cleared,
  DMA off, then INTREQR polled for each channel's interrupt). Paula
  finishes the word playing, then requests it; ours stopped the channel
  silently, and the game sat on its title for good, at interrupt level 2.
  It looked like a wait for disk 2 (not one key or disk change got a
  reaction), and the board's "core 1 ~95% in Aerial Racers' title" was the
  68EC020 spinning in that loop. `AMI_STOP=<pc>` on the loop's PC showed
  the routine.

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
  | <frame>:k:<code hex>:<1|0> | <frame>:j:<bits hex> | <frame>:t:<text>
  [;...]` (scripted mouse, keys, joystick, typed text up to the next `;`;
  mouse moves over 100 spread over frames),
  `AMI_WAV=<file>` (the sound), `AMI_TIME=<seconds since 1970>` (the
  clock's start: runs that must match; else the Mac's time, and games
  seeded from the clock differ from run to run). macOS's `setjmp` saves the
  signal mask: Musashi's two per `m68k_execute()` show up as
  `sigprocmask` in host profiles (not on the ESP32).
- **The debugger** (`amiga.c`'s `emudbg_machine_t`): the 68000, `where`,
  reset, the picture and typing (`amiga_type()`). Its picture comes from
  its own task (on the board priority 3 on core 0, below the presenter's
  5): it shares the drawing's state with the presenter, so
  `display_dbg_line()` draws a line again if the presenter drew meanwhile
  and `render_line` forgets its cached row while drawing; without a
  presenter pinning frames (the host) `display_dbg_frame()` pins the
  latest. Checked on the host (`AMI_DEBUG`) and the Tab5 over WiFi: typing
  into a Shell and the picture back, whole lines while the text scrolled
  (between the protocol's 11-line pieces the picture moves on, as with
  every machine that runs meanwhile).
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
  Workbench 1.3.5's desktop, on the host and the Tab5 (an A600 with 3.1
  through Gayle; an A500 with 1.3 or 2.04 through our board: `AMI_FAST_KB=1024
  AMI_HD0=...` with amiga-os-130.rom). Hannibal's RP9 (with workbench-135.hdf
  beside it) runs on an A500 with 1.3, the host and the Tab5. The RP9 games' own
  disks (af-application.hdf, volume AF-Application) aren't bootable alone:
  no C: or S:Startup-Sequence; Amiga Forever boots its system disk
  (`<boot type="hdf">135</boot>`: workbench-135.hdf), whose startup runs
  `AF-Application:S/user-startup`. Alone they stop at the `1>` prompt.
  `tools/rdb.py disk.hdf` lists an RDB (partitions, filesystems),
  `tools/ffsls.py disk.hdf [depth]` the first partition's tree.
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
- **The blitter's timing**: `tools/blitboot.py out.adf`, then
  `AMI_DF0=<copy> AMI_DUMP=7F000:8:dump.bin ami_host roms/amiga-os-130.rom
  700` and `tools/blitboot.py --check dump.bin`: the lines a 4096-slot
  blit takes with each display, against the slots a line leaves it.
- **Writing extended ADFs** (`tools/extadf.py`): Workbench 1.2's ADF
  (336 free blocks; 1.3.4's and 3.1's are full) with the startup-sequence
  `copy s:startup-sequence to df0:Written` (`tools/adf.py`), made into an
  extended ADF of AmigaDOS tracks (`make`) and one of raw 6400-word
  tracks (`make ... raw`), 4000 frames with 1.3, decoded back (`adf`):
  Written equals the startup-sequence in both, the same blocks changed
  as in the plain ADF (only dates differ), the raw tracks kept in place.
  `rawboot out.adf 100`: its boot block writes two syncs and 2000 words to
  track 100 through trackdisk's TD_RAWWRITE (2.04, 900 frames; 1.3's
  trackdisk answers IOERR_BADLENGTH): the track turns raw, the image
  903052 -> 910088 bytes, the words in a row (`find`), the other tracks
  the same. Both on the host and the Tab5 (images downloaded back,
  2026-10-22).
- **AGA's corners** (`tools/agaboot.py <case> out.adf`, then `AMI_MODEL=3
  AMI_CHIP_KB=2048 AMI_SLOW_KB=0 AMI_DF0=<copy> AMI_DUMP=7F000:2:clx.bin
  ami_host roms/amiga-os-310-a1200.rom 2500 out.ppm` and `tools/agaboot.py
  check <case> out.ppm|clx.bin`; the boot block loads its payload through
  trackdisk, which runs from ~frame 1800 after the A1200's IDE wait):
  fmode (4 planes in FMODE 0-3, 1 and 2 the same), shres (4 planes,
  24-bit colours), spr32 (FMODE $4 and $8), sprpos (35/70 ns steps,
  SPRES super-hires), clx7/clx7off/clx7reset (CLXCON2 and its clearing),
  clxwide/clxwide0 (a 64-pixel sprite's last 16 pixels). All right on
  the host (2026-10-22); the build before failed sprpos's steps, clx7off
  and clxwide. A sprite written by hand stays armed above and below the
  vertical window, where no planes are fetched (TODO 9's open question):
  clx7's copper arms it on lines 50-59 only.
- **Paula's attach modes**: `tools/attachboot.py out.adf vol|per|both|off3|none`,
  then `AMI_DF0=<copy> AMI_WAV=out.wav ami_host roms/amiga-os-130.rom 500`
  and `tools/attachboot.py check <mode> out.wav`: channel 0's words to
  channel 1's volume, period or both, each stretch's frequency and level
  against the words (`off3`: channel 3 with its bits, silent). All right
  (2026-10-22); the code before it got 25 and 37 of them wrong.
- **Interlace**: `tools/laceboot.py out.adf`: the woven picture's lines
  alternate white and black (host and Tab5). Workbench 1.3 interlaced:
  `devs/system-configuration` byte 185 bit 0 (LaceWB) set with
  `tools/adf.py`; 640 x 512 on the host and the Tab5 (100%, core 1 22%).
- **Super-hires**: Workbench 3.1 from HD on the A600 host (`AMI_MODEL=2
  AMI_ECS=1 ... AMI_HD0=<copy> AMI_DF1=<Extras 3.1>`), Right-Amiga+E
  (`k:67` with `k:12`), `t:extras3.1:prefs/screenmode\n`, at frame 4400
  `m:105:92:0` and a click picks PAL:1280 x 256 (`m:105:110:0` the
  interlaced one), then `m:-60:290:0` and a click is Save: the picture
  1440 x 568, Workbench's 1280 pixels exactly at columns 84-1363, the
  text sharp, the pointer in its three colours (2026-10-21). On the Tab5
  the user went through every mode ScreenMode offers (lores, hires,
  super-hires, interlaced or not) and found them all right on the panel.
  The exact rules: `tools/shresboot.py out.adf`, then `AMI_MODEL=2
  AMI_ECS=1 AMI_DF0=<copy> ami_host roms/amiga-os-205-a600.rom 1200
  out.ppm` and `tools/shresboot.py check out.ppm` (or the board's
  screenshot as PPM): bands of lores, hires and super-hires lines in a
  frame, a palette graphics.library wouldn't set; 11 checks (the pairs'
  colours, BPLCON1 3 = 12 pixels, dual playfield, sprite 0's colours and
  70 ns step, sprite 4 hidden), all right on the host and the Tab5
  (2026-10-22; the code before failed 5). Workbench 3.1 in super-hires
  (the recipe above, 4800 frames): the same picture as before. A lores
  screen over it (2026-10-23): a 320 x 256 lores ILBM written over
  `Prefs/Palette` on a copy of Extras 3.1 (`tools/adf.py`), Execute
  `sys:utilities/multiview extras3.1:prefs/palette screen` (MultiView on a
  screen of its own), then Left-Amiga held and the mouse dragged 120 down
  with the button pressed: the frame 1440 wide, Workbench's super-hires
  lines above, the picture's lores ones (4 pixels each) below, both at
  columns 84-1363. (Typed commands need `\n` as the two characters:
  `echo` turns it into a newline, which `$(cat ...)` drops at the end:
  the command then never runs, which first looked like the Overscan
  editor hanging.)
- **NTSC**: `AMI_NTSC`: Workbench 1.3 (640 x 200) and 3.1 from HD at
  720 x 484, interlace too. Kickstart 3.1's **Early Startup Control**:
  both mouse buttons held from power-on (`AMI_INPUT=0:m:0:0:3`, the
  console's `m 0 0 3`) until it shows: a key toggles NTSC and PAL (the
  picture 568 to 484 lines; on the Tab5 60.0 fps, 100%); its Boot button
  returns to PAL (graphics.library, as the ROM does).
- Amiga Test Kit's video tests (F6): all six draw; F1's PAL/NTSC frame
  (markers at lines 44, 243, 299) as its source describes.
- **Installing on a blank hard disk** (phase 5's check): `AMI_NEWHD=<path>:50`
  makes one, `tools/mkinstall.py` a Workbench floppy whose startup formats
  DH0: and copies itself onto it (Return at Format's prompt: frame ~700
  with 3.1, ~900 with 1.3); then the floppy out (`AMI_INSERT=<f>:0:`) and
  Ctrl-Amiga-Amiga: it boots from DH0:. Its startup-sequence is
  `mkinstall.py`'s minimal one (ENV: empty, no IPrefs: the prefs editors
  fail, "ENV:Sys/screenmode.prefs, object not found"): copy the
  Workbench floppy's own over it (`copy df0:s/startup-sequence dh0:s/`,
  done on the Tab5's HD 2026-10-21). Checked: an A600 with 3.1 (Gayle,
  FFS QUICK: ~2200 sectors written one a command, ~4 ms each, as a real
  A600 does) and an A500 with 1.3 (our board, the old filesystem: Format
  formats and verifies every cylinder, ~16000 frames for 20 MB), both to
  their Workbench from HD; on the Tab5 the 3.1 one (a blank 50 MB disk
  made through the web API, Return by the console's `k 44`, 100%, core 1
  ~45% while writing). The copy brings the Workbench floppy's Disk.info
  (a floppy icon): Commodore's installer puts Install 3.1's
  `Update/Disk.info` (the hard drive icon) there, done afterwards on the
  Tab5 through Execute Command (`copy df0:update/disk.info dh0:`). Two hard disks: Workbench 3.1's HD in DH0: and
  workbench-135.hdf in DH1: (its RDB calls it DH0 too: renamed DH1)
  show both volumes under 3.1 (Gayle and our board; host and Tab5) and
  under 1.3 (our board for both; host).
- **The web page's converters**: `node tools/amifiles_test.js` makes
  DMS archives (stored and RLE tracks) and an .adz itself and unpacks
  them; with the user's collection's folder it unpacks its 27 DMS archives (all mode 6, heavy 2) and
  compares them with the ADFs beside them: 25 identical; WB2_0's
  WB-2_05.ADF is really a Workbench 2.1 disk and WB2_1's WB-2_1.ADF no
  AmigaDOS disk at all (the DMS archives unpack to Workbench2.0 and
  Workbench2.1, every track's checksum right). In Chrome on the board's
  page (the file staged inside a workspace folder: the browser tool takes
  no others): a .dms and an .adz unpacked, uploaded, downloaded back
  identical. Modes 2-5 (quick, medium, deep, heavy 1) are written after
  xDMS but no archive here uses them.
- **The collection before and after a change**: copy the old
  `ami_host` aside, build the new one, then `tools/collection.py <folder
  of RP9s> <out> roms <folder with workbench-135/311.hdf> old=<old>
  new=<new>`: every game on its manifest's machine, 7 pictures each,
  8 at once (~1 minute for the 96); the games that differ are listed.
  Its runs set `AMI_TIME`: one build against itself gives 96 of 96 the
  same (Epsilon 9 and Maniac Ball differed under load before: seeded from
  the clock, which then started at the Mac's time).
- `say hello` in Workbench 1.3's Shell (narrator.device) as a sound test;
  `AMI_WAV` records it on the host.
- **On the board**: status via the web API, pictures via the console's
  `lcd`, the setup screen via `t x y`, the Amiga via `k`/`m`/`j`.
- **By hand** (the user, 2026-10-21): a Bluetooth keyboard and mouse and
  the Tab5 Keyboard work. Not yet: sound on the speaker, USB keyboards and
  mice, games played by hand.
- **Touchpad, now esp32emu's** (Tab5, 2026-10-21, Workbench 3.1 on the A600,
  `t x y ms x2 y2` drags measured on screenshots): slow drags move the
  pointer as far as the finger (148 for 150, 156 for 158 picture pixels),
  fast ones further (Intuition's acceleration); a double tap away from the
  pointer opened Workbench3.1, a tap and a drag moved its window by the
  finger's move, the long press showed the menu bar while held; the setup
  corner still opens the setup screen.
- **The A1200** (2026-10-21): the host (`AMI_MODEL=3 AMI_CHIP_KB=2048
  AMI_SLOW_KB=0` with `amiga-os-310-a1200.rom`): Kickstart 3.1 to its
  insert-disk screen, Workbench 3.1 from the A600's HDF ("Kickstart 40.68,
  Workbench 40.42"); the collection's 25 A1200 RP9s (floppy ones in DF0:,
  hard disk ones with `workbench-311.hdf` beside them; Starbase 13 and
  Tactical Manager 2 want `workbench-135.hdf`): all show their pictures at
  1500-4500 frames (Christmas Breakdown, a 1.76 MB high-density floppy,
  since 2026-10-22: its menu at ~5000). SysInfo 4.4 from its floppy (the SPEED button `m:295:394`):
  1.03 times an A1200. The OCS/ECS pictures (Kick Off 2, Workbench 1.3
  and 3.1, super-hires) bit for bit as before the AGA rework. The Tab5:
  the setup row's A1200, Workbench 3.1 from the user's HDF (100%, cores
  ~20%), Aerial Racers' AGA title (100-102%, core 1 93-98%: the game
  stuck there, see the lessons). **Aerial Racers** (2026-10-22, host):
  title at ~3600 frames, its menu, fire at 6000 starts a race, the
  package inserted again at 6800 and 8500 gives disks 2 and 3, the race
  from ~9500 (the cars are sprites, the playfield lores FMODE 0 with
  BPLCON1 $CC22). The Tab5 (the console's `j 10`/`j 0` for fire, the
  package inserted again through the web API): its menu at 100% with core
  1 79-81% busy (98% before the inline wait), the race at 98-100% with
  core 1 90-99%. The race's profile: Musashi's dispatch 24%, the 68020's
  reads 13% + `chip_read16` 4%, Lisa's fetch (`display_fetch`) 9%, Paula
  7%, memcpy (line records) 6%. Without bus errors (and the DMA reads,
  `beam()`, `ami_now()` inline): the menu core 1 60-66%, the race 100-102%
  speed with core 1 73-81%; its profile: the 68020's reads 13%, the
  dispatch 12% (the register copy was half of it), Lisa's fetch 9% (chip
  RAM in PSRAM: its misses, not the calls), Paula 6%, memcpy 3%.

## Open items

- The work still to do, sketched and in order: `TODO.md` (the 68EC020's
  instruction cache, then small things). The
  items below are the state of things; `TODO.md` says what to do next.
- Public on GitHub (`github.com/rlucencic/esp32ami`, pushed to `main`,
  since 2026-10-21). Licence: MIT (`LICENSE`). README images
  (`docs/images`, 2026-10-21): `workbench31.png` (A600, 3.1 from an HDF,
  the HD window), `workbench13.png` (A500, Workbench 1.3.5 from Amiga
  Forever's HDF through our board), `kickoff2.png` (its RP9, practice
  match), `sysinfo.png` (A500, 1.00 x A600), `aros.png` (Workbook with
  the 3.1 floppy), all from the host build; `tab5-workbench.png` (the
  panel's picture), `tab5-setup.png` (status lines cut), `web.png` (a
  local mock with example data, halved).
- Phases 4-7 (`docs/architecture.md`): games (phase 4's blitter timing
  per slot: done, `TODO.md` 2), the A1200's rest (phase 7, below). The
  68000's bus is slot by slot now (`TODO.md` 3: SysInfo 1.03, xSysInfo
  1.06); the rest would need where an instruction's bus cycles fall.
- AROS (the setup screen's Kickstart "AROS", every model): its ROM at
  $F80000 and its extension ROM at $E00000 (`amiga_config_t.extension`,
  partitions `kick_aros`/`kick_aros_ext` or files in the card's roms/
  that say "AROS ROM"/"AROS EXT" at $18). Boots to its "Waiting for
  bootable media" screen, the Workbench 3.1 floppy to its own Workbook
  1.0 (host and Tab5: 100%, core 1 25%), DH0: through Gayle (its ata
  driver), DH1: through our board.
- Assembler (RISC-V with the P4's PIE SIMD; this project is Tab5 only)
  for hot paths is wanted: `../esp32emu/CLAUDE.md`, "To do". Profiles on
  the Tab5 (`docs/architecture.md`, phase 6): Kick Off 2 scrolling at
  100% with core 1 56% busy (68000 memory reads 13%, Denise's fetch 7%,
  memcpy of line records 6%, blitter 2.4%) and core 0 ~65% (the
  presenter's scaling 35%, Denise's drawing 14%).
- Extended ADFs are written (the test disks); no game was seen writing
  to one: Son Shu Shi, the collection's only extended ADF (154 raw
  tracks, 12 AmigaDOS), only reads in its 140 s. A game whose disk turns
  AmigaDOS tracks raw has a pause while the whole image (~2 MB) is
  written, once. DMS modes 2-5 untested (no archives).
- The RP9 games with a hard disk: joined with their system disk (above).
  The six for 1.3 run through our board (host, KS 1.3 with 1 MB fast
  RAM: Hannibal's menu, also on the Tab5; Epsilon 9's intro, Flight of the
  Amazon Queen in a scene, Starbase 13 and Tactical Manager their title
  screens; Ants its menu as its manifest's A500+: ECS, 1 MB chip RAM,
  which the board now offers from the manifest); the ten for
  3.1 are A1200 games (all show their AGA pictures now, Football Glory
  too); three name no system disk.
- ECS super-hires follows WinUAE where the HRM says nothing (the colour
  pairs, sprites 4-7 hidden, dual playfield as one playfield): not
  compared with a real ECS Denise. A lores screen dragged over the
  super-hires Workbench is right on the host (Testing); not tried on the
  Tab5.
- Paula's attach modes are checked by the test disks only: Datastorm, the
  collection's one user, sets them while its sound is off (its recording
  the same with and without them). Absolute touch
  positioning (the pointer under the finger) is not wanted: the user
  finds the touchpad better (the pointer's tip in sight, a tap anywhere
  clicks). Collisions are checked by the test disks only:
  the one game that reads CLXDAT (X-Balls) behaves the same without
  them. Sprites inside the window on lines without bitplanes still show:
  Denise (WinUAE) would want a BPL1DAT write on the line first, which
  wasn't settled for those lines (above and below the window it is).
- **Clock** (the clock audit, 2026-10-21: `../esp32emu/CLAUDE.md`, To do):
  the saved offset would keep a clock set while the board's time was
  wrong (unlikely with the Tab5's RTC). (HOLD's release: fixed.)
- **The A1200's rest** (phase 7): Workbench in 256 colours is checked on
  the host only (640 x 256, FMODE 3). Checked by eye on the host in the
  collection's games that use them (found with a log of each mode's first
  use, 2026-10-22): HAM8 (Deluxe Pacman's Hall of Fame, frame ~6200),
  64-pixel sprites (its score table, Soccer Kid), FMODE 1 (Football Glory
  and Indoors from ~900, Leading Lap ~4300, Soccer Kid), the scroll's top
  bits (Deluxe Galaga in FMODE 3, Soccer Kid in FMODE 1). No game there
  uses FMODE 2, 32-pixel sprites or AGA super-hires, CLXCON2 or wide
  sprites' collisions: `tools/agaboot.py` checks them (Testing). Virtual
  Karting 2 sets the sprites' 35/70 ns bits: its sprite text 1-3
  super-hires pixels further right since they count; the 68EC020's data
  waits are an average (8 clocks, no slots), its instruction cache times
  only (no stale code, FC2 not in the tags); the A1200's 32-bit trapdoor fast RAM is the
  Zorro II board at $200000. Core 1 is 73-81% busy in Aerial Racers' race
  (100%; profile in Testing; 90-99% before the changes here). Done: `chip_read16()` inline
  (every DMA word) and read directly in `display_fetch()`, `beam()` (VPOSR
  polls) from the current line without a division, `ami_now()` without a
  call, no bus errors in Musashi (see the 68EC020 above).
  Deleting an RP9
  from the web page removes its unpacked disks too (`disk_removed()`:
  checked 2026-10-22 by the SD card's free space, 917504 bytes back; the
  web server refuses names starting with a dot, so it can't show them).

## Working with the user

- Expects changes built, flashed and checked on the board, then a short
  report of what was verified and what wasn't.
- Asks "what's next": answer with `TODO.md`'s first item (its sketch).
  Keep the list in order: a finished item goes to `docs/architecture.md`'s
  Progress and leaves `TODO.md`; new work goes in where it belongs.
- Keep their private data (WiFi name, IP, SD card contents) out of anything
  meant for publishing.
