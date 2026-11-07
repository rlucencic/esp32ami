# Amiga unshifted blitter copy experiment — 2026-11-07

A guarded portable-C path specializes A-only, unshifted A-to-D copies.
The isolated P4 benchmark reduces large-copy cost about 65–67%, with
smaller copies also improving. Integration results and decision follow below.

## Reference and scope

Reference: esp32ami `060c3bf`, shared esp32emu `aaadea2`. The planar renderer
optimization is present in both builds. Only local `components/amiga/blitter.c`
changes; no shared runtime, CPU or presenter source changes. This project
is P4-only. Tests use Tab5/P4 at 360 MHz, PSRAM at 200 MHz, ESP-IDF 5.5.6,
GCC 14.2 and the normal `-O2` chipset compilation settings.

The selector requires BLTCON0 exactly `0x09F0`: A and D enabled, B and C
disabled, minterm A, A shift zero. Line and inclusive/exclusive fill modes
are excluded. Descending direction, first/last masks, signed modulos and
otherwise-unused BLTCON1 bits remain supported. All other operations take
the original general path.

Every source read precedes its corresponding destination write. This keeps
forward/backward overlapping copies and chip-address wrapping identical;
there is no bulk memcpy or memmove. The A data register retains the last
raw word before masking, B/C state stays untouched, final pointers advance
exactly as before, and an OR accumulator determines the zero flag. A
single-word row receives both edge masks. The original two colour clocks
per word are returned, leaving busy scheduling, interrupts and CPU-steal
accounting unchanged.

The inlined full-firmware blitter `start` grows from 2,662 to 2,964 bytes
(+302 bytes). `blitter_write` remains 706 bytes. No table, allocation or
internal-RAM storage is added; the existing fill table remains 2 KB.
No hand-written assembly is introduced.

## Correctness

- Actual-source reference/candidate wrappers pass 100,000 comparisons under
  ASan and UBSan: complete 8 KB chip-memory contents, all blitter registers
  and busy/end/left/steal fields, plus CPU-wake counts. There are 75,000
  eligible copies and 25,000 randomized fallback configurations.
- Cases vary ascending/descending direction, partial/full/zero masks,
  width-one rows, signed modulos, odd and overflowing pointers, same-address
  copies, overlap by one word and address wrapping. Sizes include normal
  OCS widths, 2,048-word ECS widths and 32,768-row heights. Zero-filled
  sources check the zero flag. Fallback coverage includes shifts, channels,
  minterms, fill and line modes. Memory accesses retain the existing chip
  helper in both implementations.
- Fourteen generated-disk host pictures match exactly: blit, lace,
  ECS super-hires and eleven AGA cases (fmode, shres, spr32, sprpos, clx7,
  clx7off, clx7reset, clxwide, clxwide0, sprborder, sprborder2).
- Existing blitter timing, ECS super-hires and AGA fmode/shres/spr32/sprpos
  pattern checks pass for both builds. The generated blitter timing test
  still measures 18, 29, 40 and 65 busy lines.
- Host floppy-timing/chip-bus regression checks, isolated P4 benchmark and
  full candidate/normal-production P4 firmware builds pass.

The host fixtures use the same ROMs and frame counts as the preceding
[planar trial](planar-expansion-experiment-2026-11-07.md), including 5,000
frames for AGA cold boots. Only generated ADF copies are mounted; original
user disks remain untouched. Board apps use an isolated disk-selection
namespace; production source and its verified build retain the normal one.
Shared source is unchanged, so no 24-sibling build matrix is needed.

## Isolated P4 timing

Both complete `start` implementations run in one firmware with separate
2 MB PSRAM chip-memory buffers. Each measurement performs eight blits,
resetting the input register structure for each. Seven ABBA rounds alternate
reference/candidate/candidate/reference. Values are medians of the seven
paired-round costs per blit, including common setup/checksum overhead.
Interrupts are enabled; guest CPU, LCD, audio and networking are absent.
The benchmark does not measure emulated BBUSY duration or application speed.

Most cases are 20 words × 128 rows. Exceptions are width-one × 128,
2×2 and line mode (128 steps). Edge masks are `1357/ECA8`, descending uses
BLTCON1 bit 1, overlap places D one word after A, wrapping starts both
pointers near the 2 MB limit, and modulo uses A +18 / D −8 bytes per row.

| Case | Reference cycles/blit | Candidate cycles/blit | Cost change |
| --- | ---: | ---: | ---: |
| Copy full masks | 139021 | 49033 | -64.73% |
| Copy edge masks | 139022 | 49031 | -64.73% |
| Copy descending | 147732 | 49032 | -66.81% |
| Copy overlap | 138976 | 48974 | -64.76% |
| Copy wrap | 138972 | 48974 | -64.76% |
| Copy width one | 10455 | 5086 | -51.35% |
| Copy 2x2 | 950 | 652 | -31.37% |
| Copy modulos | 139001 | 49017 | -64.74% |
| Shift fallback | 139020 | 136117 | -2.09% |
| Cookie-cut fallback | 252539 | 252491 | -0.02% |
| B-copy fallback | 128382 | 126470 | -1.49% |
| Line fallback | 8087 | 8066 | -0.25% |

Fallback differences include compiler layout effects; they are not new
specialized algorithms. Full-firmware controls are needed before retention.

## Full firmware

The A500 uses Kickstart 1.3, 512 KB chip RAM, PAL and no slow/fast RAM.
A synthetic guest program repeatedly copies 20 words × 128 rows, increments
a source counter, polls BBUSY, then starts the next copy. Four displayed
planes are initialized explicitly. A six-plane OCS CPU-fill loop is the
control: it performs no blits. Both run without mounted guest disks.

Two roughly 30-second profiler-free windows per build measure the source
counter and, separately, the frame counter. Counter read timestamps use
the midpoint of each debugger request (recorded RTT 11–114 ms); counts are
an approximate workload rate. These are one fresh run per build in
reference/candidate order, without broad statistical confidence.

| Workload rate | Reference | Candidate | Pooled change |
| --- | ---: | ---: | ---: |
| Repeated copy, copies/s | 474.111 | 475.389 | +0.270% |
| Repeated copy, emulated FPS | 49.934 | 49.911 | −0.047% |
| OCS six-plane control, emulated FPS | 48.778 | 48.426 | −0.723% |

Copy-rate windows are 473.858/474.363 copies/s for reference and
475.150/475.627 for candidate. Copy-frame windows are 49.946/49.923 and
49.910/49.912 FPS. The cap and unchanged emulated busy duration limit the
throughput result; these small differences establish no application speed
increase. Amiga screen signatures are content hashes, not update counters.

Three 32,000-sample captures per workload/core use nominal 20 kHz sampling
(50 microseconds), resolved against the exact flashed ELF. Profile shares
include idle time and are not direct function timing comparisons.

| Function / workload / core | Reference share | Candidate share |
| --- | ---: | ---: |
| Blitter `start`, repeated copy, core 1 | 19.6% | 7.0–7.1% |
| Idle, repeated copy, core 1 | 47.7% | 59.7–60.1% |
| `bus_cycle`, repeated copy, core 1 | 4.1–4.4% | 4.1–4.4% |
| `render`, repeated copy, core 0 | 16.2–16.3% | 16.0–16.4% |
| `bus_cycle`, OCS control, core 1 | 29.3–29.7% | 29.3–29.8% |
| `render`, OCS control, core 0 | 10.9–11.0% | 10.7–11.4% |
| `present_task`, OCS control, core 0 | 35.2–36.1% | 35.4–37.3% |

The copy workload's lower host execution cost appears as extra core-1
idle headroom while preserving emulated timing. For deterministic LCD
comparisons, the guest performs one copy/fill and loops in place. The
1280×720 repeated-copy picture matches reference exactly.
The OCS control LCD also matches exactly. All 24 captures contain the
requested 32,000 samples and resolve to the intended core's functions.

The initial OCS control windows are 48.774/48.783 FPS for reference and
48.310/48.542 for candidate. Because this control never calls the changed
copy path, repeat it after fresh boots in reverse candidate/reference
order, again with two profiler-free 30-second windows. This checks the
small integration difference rather than assuming it is measurement noise.

| Fresh-boot OCS control | Reference FPS | Candidate FPS | Change |
| --- | ---: | ---: | ---: |
| Pooled two-window rate | 48.676 | 48.539 | -0.283% |

Reference windows are 48.742/48.611 FPS and candidate windows
48.523/48.555 FPS. The smaller negative difference repeats, so retain the
observed control cost as a limitation. These two sequences do not establish
its cause or a broad confidence interval; code layout and system activity
are possible contributors, not verified explanations.

## Decision and remaining work

Retain for the large reduction in copy work and extra core-1 headroom,
with the measured OCS control cost (−0.28% to −0.72%) explicitly accepted.
No general application-speed or panel-FPS increase is claimed. The next
blitter candidate is cookie-cut/minterm dispatch, but it needs independent
profiling on representative copied-disk software before another fast path.

The board was reflashed with the uninstrumented candidate test app,
restarted as A500 / Kickstart 1.3 / 512 KB chip RAM, and verified at its
ordinary ROM disk-wait picture in 720×568 PAL mode. No synthetic guest
program is running and no disks are mounted. The board app keeps the
isolated disk-selection namespace; production source/build retain the
normal namespace and contain no temporary probes.

Generated fixtures and synthetic workloads do not establish game or
Workbench compatibility or general application speed. Physical input and
audio-by-ear coverage remain open; paused title investigations stay paused.
No S3 runtime result is implied. Full flash backups, fixtures, firmware and
raw logs remain outside git under `/tmp/amiga-blit-2026-11-07` and the
preceding trial's scratch directory.
