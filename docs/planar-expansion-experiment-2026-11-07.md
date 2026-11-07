# Amiga planar expansion experiment — 2026-11-07

Retain an original portable-C change that splits each eight-pixel lookup
entry into two 32-bit halves. The P4 renderer avoids variable 64-bit shifts,
reducing isolated complete-row draw cost by 3.1–12.5% in active-plane cases.
This is a renderer result; whole-emulator measurements are reported below.

## Change and reference

Reference: esp32ami `6f37fbe`, shared esp32emu `a548a84`. Only
`components/amiga/display.c` changes. No shared runtime or CPU timing code
changes. Tests use Tab5 ESP32-P4 at 360 MHz, 32 MB PSRAM at 200 MHz,
ESP-IDF 5.5.6 / GCC 14.2, the normal project optimization settings.
Amiga is P4-only; no S3 runtime result is implied.

Each lookup byte contains only zero or one. For planes 0–7, shifting those
bits cannot carry across a byte, including the 32-bit boundary. Independent
low/high shifts therefore reproduce the original 64-bit result exactly.
The expansion still ORs each plane into the same odd/even index buffers;
palette conversion, HAM, dual playfields, sprites, scrolling and output
composition are unchanged. There is no hand-written assembly.

Full-firmware `render` shrinks from 3,322 to 3,242 bytes (80 bytes saved).
`display_init` remains 326 bytes. The internal-RAM lookup remains 2,048
bytes; no allocations, tables or emulated DMA/timing work are added.

## Correctness

- Actual-source reference/candidate draw wrappers pass 163,328 complete
  pixel/index-buffer/canary comparisons under ASan and UBSan. Coverage
  includes OCS/ECS/AGA, 0–6/8 planes, lores/hires/super-hires, words 0–112,
  HAM and dual-playfield flags, FMODE 0–3, scrolling, palette changes,
  attached/wide sprites, narrow/wide output and changing previous row widths.
- An independent per-pixel oracle passes every 16-bit source word at every
  plane shift: 524,288 word/plane cases.
- Fourteen generated-disk host pictures match byte for byte: blit, lace,
  ECS super-hires and eleven AGA cases (fmode, shres, spr32, sprpos, clx7,
  clx7off, clx7reset, clxwide, clxwide0, sprborder, sprborder2).
- Existing blitter timing, ECS super-hires and AGA fmode/shres/spr32/sprpos
  pattern checkers pass for both builds. Blitter busy intervals remain
  18, 29, 40 and 65 lines. Host floppy-timing and chip-bus regressions pass.
- AGA cold boots require 5,000 frames in these host fixtures. At 1,500
  frames the reference was still in ROM startup; that early capture was
  discarded and both builds were rerun with the same longer limit.
- Reference and candidate full P4 firmware builds pass. Production source
  keeps its normal disk-selection namespace and contains no temporary probes.

Only generated ADF copies are mounted. Original user disks are untouched.
Hardware test apps use a separate disk-selection namespace. A complete
pre-test Mac3 flash backup is verified and stored outside the repositories.

## Isolated P4 row timing

Both actual renderer versions run in one minimal firmware. There are 64
distinct PSRAM row records per case; palettes, output and lookup tables are
in internal RAM. A pass draws all 64 rows four times, with a volatile
checksum. Seven ABBA rounds alternate reference/candidate/candidate/reference.
Interrupts remain enabled; guest CPU, LCD, audio and networking are absent.
These are complete-row costs, including composition, not expansion-only
or application timings.

| Case | Reference cycles/row | Candidate cycles/row | Cost change |
| --- | ---: | ---: | ---: |
| OCS six lores | 12478 | 11250 | -9.84% |
| OCS four hires | 17539 | 15910 | -9.29% |
| ECS two super-hires | 26815 | 25206 | -6.00% |
| OCS HAM6 | 22821 | 21724 | -4.80% |
| OCS dual | 15613 | 14399 | -7.78% |
| AGA eight lores | 21346 | 19419 | -9.03% |
| AGA eight hires | 32645 | 28787 | -11.82% |
| AGA eight super-hires | 56857 | 49764 | -12.48% |
| AGA HAM8 | 31472 | 29776 | -5.39% |
| AGA dual | 29162 | 25877 | -11.27% |
| AGA sprites/changes | 50642 | 49094 | -3.06% |
| No planes | 3579 | 3575 | -0.11% |
| AGA wide HAM8 max words | 139390 | 131388 | -5.74% |

## Full firmware

PAL workloads are synthetic guest programs injected after a fresh ROM
boot: continuously changing six-plane OCS lores, four-plane OCS hires,
eight-plane AGA lores and repeated blitter A-to-D copies. A500 uses KS 1.3,
512 KB chip RAM; A1200 uses KS 3.1, 2 MB chip RAM. No slow/fast RAM or guest
disks are selected. Dynamic fill loops exercise the presenter; separate
one-fill/self-loop programs provide deterministic LCD comparisons.

For primary OCS/AGA throughput, two roughly 30-second windows per build
run without profiling. Report host-time frame-counter rates, not panel FPS.
`amiga_screen_signature()` returns a content hash; its changes cannot be
interpreted as an update counter or speed measurement.

| Workload | Reference FPS (two windows) | Candidate FPS (two windows) | Pooled change |
| --- | --- | --- | ---: |
| OCS six-plane lores | 48.698, 48.750 | 48.717, 48.685 | −0.047% |
| AGA eight-plane lores | 28.820, 28.799 | 28.756, 28.770 | −0.161% |

The pooled reference/candidate rates are 48.724/48.701 FPS for OCS and
28.810/28.763 FPS for AGA. This single fresh run per build has no broad
statistical confidence; it establishes no whole-emulator speed increase.

Each profile contains 32,000 samples at nominal 20 kHz (50 microseconds),
three captures per workload/core, resolved against the exact flashed ELF.
Profiler arguments selecting output-symbol count are not sampling intervals.
These sample shares are relative to each core's complete capture, including
idle time, not isolated function timings.

| Presenter function / workload | Reference share | Candidate share |
| --- | ---: | ---: |
| `render`, OCS six-plane lores | 11.7–11.9% | 10.9–11.0% |
| `render`, OCS four-plane hires | 14.1–14.6% | 13.2–13.3% |
| `render`, AGA eight-plane lores | 20.2–21.2% | 19.1–20.0% |
| `present_task`, OCS six-plane lores | 34.5–35.4% | 35.2–35.4% |
| `present_task`, OCS four-plane hires | 35.1–35.4% | 35.3–35.4% |
| `present_task`, AGA eight-plane lores | 30.4–32.9% | 31.2–33.7% |

OCS core 1 remains dominated by `bus_cycle` (reference 29.7–30.0%,
candidate 29.4–29.6%) and `display_fetch` (10.7–10.9% / 10.7–11.0%).
Presenter savings do not remove that guest-side bottleneck.

Separate deterministic 1280×720 LCD captures match exactly for OCS six
planes, OCS hires and AGA eight planes. Retain the change for lower measured
row cost and smaller code; no panel-FPS or general application gain is claimed.

After measurement, the board was reflashed with the uninstrumented
candidate test app and restarted as A500 / Kickstart 1.3 / 512 KB chip RAM.
The ordinary ROM disk-wait picture and 50 Hz status were verified. No
synthetic guest code remains running and no disks are mounted. The test
app retains the isolated disk-selection namespace; the production source
and separately verified production build use the normal namespace.

## Limits and next target

Synthetic workloads and generated disks do not establish performance or
compatibility in games, Workbench or applications. Physical input and
audio-by-ear coverage remain open. Previously paused title investigations
remain paused. Shared source is unchanged, so the 24-sibling build matrix
is unnecessary for this local P4-only change.

The synthetic copy workload puts 19.4–19.6% of reference core-1 samples
in blitter `start` (candidate 19.6–19.7%). Next investigate a narrowly guarded unshifted A-to-D copy path,
preserving masks, overlap direction, chip-memory wrapping and BBUSY timing.
General CPU/chip-bus work needs separate representative profiling.

Scratch fixtures, raw logs, binaries and backups remain outside git under
`/tmp/amiga-video-2026-11-07`.
