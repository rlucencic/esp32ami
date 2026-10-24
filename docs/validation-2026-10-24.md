# Voyage follow-up validation, 2026-10-24

The work resumed from TODO 11, left uncommitted on 2026-10-23.
Host and Tab5 checks are complete; TODO 11 has been removed.

## Yesterday's completed work

The 13 commits dated 2026-10-23 added the 68EC020 instruction cache and
poll-loop skipping; corrected sprite clipping and mixed-resolution
screens; fixed 24-bit PC-relative reads and instruction longs crossing a
64 KB bank; corrected vertical-blank requests and interrupt sampling;
fixed the floppy motor latch and CIA control-register synchronization;
and added the smoke runner and notes about titles still failing.

The subsequent uncommitted work enabled 68000 instruction prefetch,
added CIA E-clock waits, and moved the generated track gap before sector
0. Voyage's trace decoder then ran correctly, and the gap restored
9 Fingers' index-based loader.

## Follow-up fixes

- Advance floppy DMA to DSKLEN start/stop and DMACON/ADKCON changes before
  applying them. Previously a DMA armed partway through a line could
  capture words that had already passed. With the gap first, Hyperion
  could miss a sync, checksum a partial final sector and seek track
  $FFFF. Keep pointer/status timing at the existing line granularity:
  advancing every pointer/status access too corrupted 9 Fingers' stream.
- Track the 68000's chip/slow RAM reads and writes in the floating bus
  word, including instruction fetches. Steel Empire's cracked loader
  otherwise mistook write-only custom mirrors for RAM and chose a stack
  at $D80000. It now reaches the language menu.
- OCS's missing DENISEID uses $FFFF as the undriven-register fallback.
  Individual DMA bus values at the read's slot are not modeled; returning
  a CPU opcode instead made Rink a Dink falsely detect AGA. ECS and AGA
  retain $FFFC and $00F8. This is an approximation, also used by
  [WinUAE outside its compatible/cycle-exact 68000 mode](https://github.com/tonioni/WinUAE/blob/master/custom.cpp).

## Checks

- All 24 ESP-IDF firmware builds passed: ten projects on S3 and P4, plus
  Mac II, Mac III, Archimedes and Amiga on P4. The final Amiga firmware was
  rebuilt after the follow-up fixes; prefetch is enabled in its sdkconfig.
- `make -C ../esp32emu/components/musashi/test run run-prefetch` passed.
  New cases cover a self-modified next opcode, an extension word fetching
  the next opcode, and a taken branch refetching changed code. Both option
  settings are checked.
- `make -C components/amiga/host test` passed. Generated-disk tests cover
  a mid-line DMA start, repeated/backwards synchronization and frame
  rebasing. Removing the start synchronization makes the test fail.
  Bus tests cover byte/word/long reads and writes, ROM isolation, a RAM
  probe through custom mirrors, and OCS/ECS/AGA IDs.
- The 96 RP9 games were compared against the last committed build at
  1000-frame intervals through 7000 frames. 43 titles had identical
  pictures; all 53 changed comparisons were inspected. The differences
  were loading/animation timing or the same scenes; Hyperion loads again.
- A fresh deterministic sample of 288 ADFs was run on the A500 before and
  after the changes: 7000 frames, seven pictures, click/fire/Space/Help.
  The same 128 titles were flagged. This is **not yesterday's sample**:
  its old temporary manifest was unavailable. This mixed sample includes
  AGA disks and nonbootable add-ons, so the absolute flag count does not
  measure A500 compatibility. The saved manifest makes it repeatable.
- All 111 demo RP9s were compared over 15000 frames, one picture every
  250 frames. Changed contact sheets were inspected. Voyage reaches its
  final scene instead of the alert; 9 Fingers reaches its credits with
  intact pictures. Rink a Dink was caught by this sweep and retested after
  the OCS ID correction. The final full rerun flags the same 33 titles as
  the baseline, with no newly flagged titles; Rink a Dink is the only
  picture change from the already reviewed follow-up run.
  Voyage still triggers the heuristic's illegal-instruction flag while
  playing: the flag alone is not a pass/fail decision.
- Aerial Racers reaches an actual race on the host after fire at frame
  6000 and disk changes at 6800 and 8500. This does not establish Tab5
  performance.
- Fresh write disks based on Workbench 1.3.3 passed: plain ADF, extended
  AmigaDOS tracks and raw 6400-word tracks all created `Written` identical
  to `s:startup-sequence`. TD_RAWWRITE under Kickstart 2.04 produced all
  2002 expected words on track 100, promoted it to raw, and preserved the
  other 159 tracks.
- `git diff --check` passed in both repositories.

Local evidence is in `build/validation-20261024/`: build logs,
`build-results.json` (the ST S3 retry is logged separately),
`collection-checked.txt`, `ocs-checked/results.json`, demo result folders,
contact sheets, write-test disks and binaries. Large logs are compressed;
collection PPM hashes were retained when pictures were converted to PNG.
Original collection files were only read; emulator runs used copies.
Temporary collection-run disk copies were removed after validation,
reclaiming 6.44 GiB; manifests, results, logs and pictures were retained.

## Tab5 completion

The user connected the Tab5 after the host checks. Full firmware and
Kickstart flashing completed with verified hashes. Aerial Racers loaded
all three disks and ran an actual race at 98-101% speed; core 1 was
68-89% busy across 21 samples over 40 seconds. Screenshots before and
after the sample confirm gameplay. These are new measurements, replacing
the previous 98-102%, core 1 70-81% figures for this follow-up.

Hyperion loaded through its title and mission briefing into gameplay on
an A500 with Kickstart 1.2, 512 KB chip and 512 KB slow RAM. The serial
log and screenshots are saved with the host evidence (`tab5-*`). Its temporary uploaded package
was removed after testing.

The user's setup was restored afterwards: `R 3 7 2048 0 0`, DF0
`Workbench3.1.adf`, DF1 `Extras3.1.adf`, DH0 `BLANK0003.hdf`.
The shared Musashi changes (`0e23ac3`) are committed before the Amiga integration.
The pre-existing failures in TODO 10 remain separate work.
