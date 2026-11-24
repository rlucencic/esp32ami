# esp32ami: to do

In order: "what's next" is the first item here. Each one says why, where,
how, how it's checked, and roughly how big it is. Done items move to
`docs/architecture.md` (Progress, "The to-do list") and leave this list;
the others keep their numbers (done: 1, an RP9 setting up its machine;
2, the blitter's timing slot by slot; 3, the 68000 on the chip bus slot by
slot; 4, Paula's attach modes; 5, ECS super-hires; 6, writing extended
ADFs; 7, AGA's untested and missing parts; 8, the 68EC020's instruction
cache; 10, titles from the smoke runs; 11, the Voyage prefetch, CIA and
floppy follow-up; 12, Workbench's display changes on the Tab5).

## Next work

15. **Make uploads work while the A1200 runs.** Medium. With the A1200
    (2 MB chip, 8 MB fast) running Workbench 3.1 on the Tab5, every web
    upload fails, even a 900 KB ADF: the SD driver can't get its DMA bounce
    buffer (`sdmmc_cmd: allocate_dma_buf: not enough mem`; internal RAM's
    largest block 57 KB). The board restarted once during a 50 MB upload.
    From the setup screen after a restart (77 KB) uploads work.
    [The run](docs/workbench-modes-tab5-2026-11-24.md#uploads-failed-with-the-a1200-running)
    Find who holds internal RAM there (`heap_caps_print_heap_info`), then
    give the uploads' writes a DMA-capable buffer, or keep one for the
    driver, in esp32emu's `web.c`/`sdcard.c`. Check 1 MB and 50 MB uploads
    on the A1200 and the A500, running and from the setup screen, with
    downloads compared. esp32emu: every project built.

13. **Check DMS through the live web page.** Small. Use the [DMS validation
    archives](docs/architecture.md#dms-compression-mode-validation-2026-11-23)
    in the Tab5 browser workflow. Download each resulting ADF, compare its
    bytes to the host reference and boot a full bootable image from DF0:.
    Include an archive with a bad track checksum and check that it leaves no
    uploaded image.

14. **Distinguish floppy and memory failures in title replays.** Medium. Extend
    `tools/smoke.py` replays with a fixed checkpoint for All New World of
    Lemmings and a control for Pango Fun. Reproduce The Magician with and
    without slow RAM and record the first diverging guest event before changing
    timing or memory decoding.

## 9. Waiting on something

Nothing here can be done as things are; each says what it waits for.

- The clock's saved offset keeps a clock set while the board's time was
  wrong (unlikely with the Tab5's RTC): a remark, nothing to do.
- The 68000's bus cycles where a real one has them: the slot model takes
  an instruction's back to back from its start (SysInfo 1.00, xSysInfo
  1.06 times an A600); xSysInfo's 1.00 needs each bus cycle's place in the
  instruction (a bus-cycle 68000 core, not Musashi): a project of its
  own. Then the 2 clocks a 68000 on an odd colour clock waits could come
  back (`mem.c`: they made two loaders fail with the back-to-back cycles).
- Assembler (RISC-V, PIE) for hot paths: only if a game falls behind
  (Aerial Racers' race: core 1 71-78% with the 68EC020's cache and the
  poll skip). C first: the 68EC020's instruction fetch is done (2026-11-10,
  ~7% cheaper), the 68000's bus cycles inline in the callbacks
  (Kick Off 2's credits: core 1 16.7 -> 9.9 ms a frame;
  `docs/bus-cycle-experiment-2026-11-10.md`), and Kid Pool's VHPOSR
  V-byte wait is skipped (title core 1 85% -> 54% at 100%;
  `docs/poll-vhpos-experiment-2026-11-11.md`). What's left on an A500 is
  spread out: the callbacks' remaining instructions, `bus_take()` a slot
  at a time, other chip-register paths. Tab5 A500 survey 2026-11-11
  (KS 1.2, 512+512 KB; `build/a500-load-2026-11-11/`): Kick Off 2 menu
  ~100%/45%, Kid Pool title ~100%/55%, F1 GP ~100%/52–71%, Toobin
  ~100%/55–69%, Arkanoid ~100%/65%, Lure ~100%/50%, X-Balls ~99%/84–85%
  (hottest; profile idle 4.9%, `m68k_read_memory_16` 15%, `display_fetch`
  9%, `bus_take` 8%).   None fell behind; assembler still waits on a title
  that does. Son Shu Shi's RP9 insert failed on the Tab5
  (`no sonshushi1.adf in it`: one-shot inflate of a ~2 MB
  extended ADF next to the image buffer when PSRAM was tight);
  floppy RP9s now stream via `rp9_extract` like hard-disk ones
  (2026-11-11, checked on the Tab5: disk 1 of 2).
- Sprites on lines inside the window without bitplanes: Denise (WinUAE)
  wants a BPL1DAT write on a line before showing them; here they show.
  Needs a game or a test on a real Amiga that shows the difference.
