# Workbench display changes on the Tab5, 2026-11-24

TODO item 12. On an A1200 with Kickstart 3.1, Workbench 3.1 was switched
through AGA lores, interlace and super-hires, and then a lores screen was
dragged over it. The emulated frames and the panel were captured on the
Tab5 and compared with the host build. The interlace and super-hires
probes were run on the board as well.

## Setup

The machine was an A1200 with Kickstart 3.1 (`amiga-os-310-a1200.rom`,
40.68), 2 MB chip RAM and no fast RAM.

- **DH0:** a copy of the Workbench 3.1 hard disk `BLANK0003.hdf`, made on
  the Tab5 on 2026-10-21. Its volume is `HD`, and its saved screen mode is
  PAL 640 × 256 with 4 colours.
- **DF0:** an Extras 3.1 copy from `tools/wbmodes.py`. It holds three
  ScreenMode prefs files and a 320 × 256 lores ILBM, written over files the
  test doesn't need.

Each mode was set without the mouse. Right-Amiga+E opens Execute Command,
and Right-Amiga+X clears its field, which keeps the last command. Then this
was typed, using the prefs file for each mode:

```
extras3.1:prefs/screenmode from extras3.1:prefs/input.info use
```

`locale.info` was used for interlace and `sound.info` for super-hires. Then

```
sys:utilities/multiview extras3.1:prefs/palette screen
```

opened the picture on a screen of its own. Left-Amiga was held while the
mouse dragged that screen 120 lines down.

On the host, the run used the same keys through `AMI_INPUT`. The pictures
were taken at frames 800, 1600, 2400, 3200, 4000 and 4400. On the Tab5,
the console's `K`, `T` and `m` commands did the same. The emulated frame
was captured with `emu_console.py`'s `s` and the panel with `L`.

## Results

| Step | The board's Screen row | fps | Core 1 | Frame vs host |
| --- | --- | ---: | ---: | --- |
| Boot | Hires, 2 planes, 640 × 256 | 50 | 20% | identical |
| AGA lores | Lores, 8 planes, 256 colours, 320 × 256, FMODE 3 | 50 | 30% | identical |
| Interlace | Hires interlaced, 8 planes, 640 × 512, FMODE 3 | 49 | 40% | identical |
| Super-hires | 4 planes, 16 colours, 1280 × 256, FMODE 3; frame 1440 wide | 50 | 33% | identical |
| MultiView | Lores, 5 planes, 32 colours, the picture's screen | 50 | 22% | identical |
| Dragged | 2 modes on screen; frame 1440 wide | 50 | 25% | identical |

"Identical" means every pixel of the 720 × 568 or 1440 × 568 frame, in
RGB565.

**On the panel:**
- **Super-hires and the dragged frame** fill the panel's rows 76–643.
  Their pixels equal frame columns 84–1363 exactly, as specified: the
  1280 columns from lores pixel `$81`, one to one.
- **The 720-wide frames** fill the centred 960 × 720 area. Scaled back to
  720 × 568, they differ from the frame by an RMS of 8–10 out of 255, which
  is the resampling.

## Probes

| Probe | Machine | Board vs host frame | Check on the board's frame |
| --- | --- | --- | --- |
| `tools/laceboot.py` | A1200, Kickstart 3.1 | identical | 256 white rows alternating with black over all 512 lines |
| `ShresTest.adf` (`tools/shresboot.py`) | A600, ECS, Kickstart 2.05, 1 MB | identical | `shresboot.py check`: 0 wrong |
| `AgaShres.adf` (`tools/agaboot.py shres`) | A1200, Kickstart 3.1 | identical | `agaboot.py check shres`: 0 wrong |

Both super-hires probes on the panel equal their frame's columns 84–1363
exactly.

The interlace probe on the panel is mostly grey: 382 of column 640's 720
rows. 568 rows scaled to 720 can't keep a pattern that alternates every
line. Workbench's interlaced text stays readable.

## Uploads failed with the A1200 running

With the A1200 running Workbench (2 MB chip, 8 MB fast), every web upload
failed with "Upload failed" or "Can't write to the SD card", even a 900 KB
one. The board's log shows the reason:

```
sdmmc_cmd: allocate_dma_buf: not enough mem, err=0x101
diskio_sdmmc: sdmmc_read_blocks failed (0x101)
```

Internal RAM's largest free block was 57 KB. The board also restarted
once, during an attempted 50 MB upload. After a restart into the setup
screen, with 77 KB the largest free block, uploads worked: 50 MB in 49 s.
`/api/new` made a blank disk while the machine was running.

This became TODO item 15 and was fixed the same day in esp32emu: the SD
driver's bounce buffer and the web server's transfer pieces are now in
PSRAM. See [the fix](architecture.md#uploads-while-the-a1200-runs-2026-11-24).

## Afterwards

The board was set back to the user's A1200: Kickstart 3.1, 2 MB chip,
8 MB fast, `Workbench3.1.adf` in DF0:, `Extras3.1.adf` in DF1: and
`BLANK0003.hdf` in DH0:. The test images were deleted. The ScreenMode
`use`s went to `ENV:`, in RAM, and the hard disk under test was a copy.
