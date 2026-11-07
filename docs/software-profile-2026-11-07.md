# Amiga software validation and blitter census — 2026-11-07

The recent planar expansion and unshifted-copy optimizations preserve all
672 checked pictures across the local 96-game collection. A longer,
instrumented nine-title run identifies real blitter configurations for
choosing the next optimization, rather than assuming synthetic-copy
results describe games.

## Builds and method

Compare the pre-planar/pre-copy host build (esp32ami `6f37fbe`) with the
current code (`9552ba8`); shared runtime code is unchanged, with current
documentation at `a3c8c51`. The two code changes are described in the
[planar](planar-expansion-experiment-2026-11-07.md) and
[copy](blitter-copy-experiment-2026-11-07.md) reports. This follow-up adds
no production instrumentation or emulator source changes.

The existing `tools/collection.py` runs 96 RP9 game packages on their
manifest-selected machines, each from an independent copy. Hard-disk
packages receive independent Workbench 1.3.5/3.1.1 system-disk copies.
Kickstarts come from the local ROM folder. `AMI_TIME=1792656000`, frames
7,000, pictures every 1,000 frames, eight host jobs. Scripted input is the
existing collection sequence: mouse click at frames 4000/4005, fire at
4500/4505, Space at 5000/5005 and Return at 5800/5805.

All 192 runs reach 7,000 frames; every expected picture exists. All 672
paired checkpoints match byte for byte. The final 230 disk/package files
also match across builds (RP9 packages and extracted ADF/HDF/system files).
Original assets are never mounted or modified. These results establish
agreement with the baseline at the checked points, not that all games
are fully playable or free of existing emulation defects.

## Longer runs and operation census

Nine titles run for 15,000 frames on three builds: baseline, optimized and
a temporary host-only optimized build with a blitter-operation counter.
All three match at frames 5,000, 10,000 and 15,000 for every title: 27
three-way picture comparisons. The probe records complete BLTCON0/1 pairs,
operation counts and processed words in separate 5,000-frame windows.
Line operations count one word per step. Probe tables overflow-check and
none overflow. The probe lives outside the repository and board firmware.

Most titles use the same collection input. Aerial Racers instead receives
fire at 6000/6005 and its copied package is reinserted at 6800 and 8500 to
advance to disks 2 and 3. The final picture shows its race. Exile shows
its opening game scene, Maniac Ball a playfield and Epsilon 9 an adventure
room; Arkanoid and the remaining titles are title/menu/attract screens.
No claim is made that the generic input starts gameplay in every title.

For frames 10,000–14,999:

| Title | Processed blitter words | Eligible A-to-D copy share | Cookie-cut share | Equal-shift ABCD cookie-cut share |
| --- | ---: | ---: | ---: | ---: |
| Aerial Racers | 684,378 | 45.6% | 31.6% | 31.6% |
| Arkanoid | 0 | — | — | — |
| Datastorm | 42,825,000 | 77.1% | 8.8% | 8.8% |
| Exile | 6,105,416 | 17.1% | 34.3% | 26.0% |
| Deluxe Galaga | 11,235,911 | 0.0% | 40.9% | 40.9% |
| Maniac Ball | 1,384,680 | 0.0% | 100.0% | 0.0% |
| Epsilon 9 | 17,971,200 | 66.7% | 33.3% | 33.3% |
| Football Glory | 0 | — | — | — |
| Football Glory Indoors | 0 | — | — | — |

Percentages describe processed words within that title/window, not CPU
cycles, elapsed time or whole-emulator load. Zero words means no blitter
operations were observed in that final window; CPU drawing and display
rendering can still be active. Large fills/copies can dominate word count
without being the best remaining CPU target.

Eligible copies use the retained guard (`BLTCON0=09F0`, no line/fill).
Cookie-cut means minterm `CA`, with arbitrary channels/shifts, area mode.
The final column additionally requires all A/B/C/D channels, equal A/B
shifts and no line/fill. A-only unshifted specialization does not cover
most of these cookie-cut operations. Maniac Ball uses cookie-cut with A
disabled, so its constant A data and mask semantics must be retained in
any broader specialization. Shifted configurations such as `9FCA/9000`
are common across the selected runs; testing only `0FCA/0000` would miss
important cases.

## Tab5 race profile

The existing uninstrumented optimized P4 app runs a copied Aerial Racers
RP9 from the isolated test disk namespace. The package's offered A1200 /
Kickstart 3.1 configuration is applied. Fire is sent at observed frame
6095, and reinserting the same copied package at 7012 and 8682 advances
disks 2 and 3. Sampling begins after frame 10652; an LCD capture verifies
the race scene. These events follow the documented sequence, with serial
polling rather than exact host-frame injection.

Three captures per core contain 32,000 samples at nominal 20 kHz
(50 microseconds), resolved against the exact flashed ELF from the copy
trial. Blitter execution is absent from the initial top-30 lists, so three
additional core-1 captures request 200 symbols. All counts/core labels are
validated. Output-symbol count is not the sampling interval.

| Function / core | Sample share range |
| --- | ---: |
| Instruction reads, `m68k_read_immediate_16`, core 1 | 14.6–15.1% |
| CPU dispatch, `m68k_execute`, core 1 | 12.2–12.8% |
| `display_fetch`, core 1 | 8.1–8.6% |
| `audio_line`, core 1 | 7.0–7.9% |
| Blitter `start`, core 1 | 0.1–0.3% |
| Idle, core 1 | 14.0–17.9% |
| `render`, core 0 | 24.6–25.9% |
| `present_task`, core 0 | 8.2–8.9% |
| Idle, core 0 | 52.8–54.3% |

Shares include idle time. Core-1 ranges use the wider captures; core-0
ranges use its original three captures. Race position and activity change
between captures. These are optimized-build observations, not an A/B
performance comparison, a confidence interval or panel-FPS measurements.
The board log reports emulated rates around the nominal 50 Hz; no general
speedup is claimed.

After profiling, both test floppy drives were ejected and the uniquely
named uploaded RP9 was deleted through the disk API. The board was
restarted as A500 / Kickstart 1.3 / 512 KB chip RAM, and its ordinary
720×568 PAL disk-wait picture was verified. No guest disks are mounted.
The firmware is unchanged from the retained copy trial and uses the
isolated test namespace; original disk selections remain untouched.

## Next optimization decision

Defer another blitter specialization. Cookie-cut is a common fraction of
blitter work in the selected titles, but the complete blitter occupies only
0.1–0.3% of core 1 in the observed race. Even removing all of that work
would release little core capacity in this particular workload. Other
titles and phases may differ; the census identifies candidates, not a
universal bottleneck.

If blitter work resumes, investigate shifted cookie-cut dispatch and the
A-disabled form, preserving all data-register, channel, mask and timing
semantics. An unshifted-only kernel would exclude many recorded operations.
Amiga CPU instruction reads, chip/display fetching and presenter rendering
have larger sample shares here, but changing their timing or CPU semantics
requires separate targeted validation. No assembly rewrite is justified by
this blitter profile.

Continue the cross-project investigation with MSX2 bitmap conversion and
VDP command operations, measuring before selecting a kernel. The general
Amiga feature/compatibility TODO remains unchanged.

Physical input and audio-by-ear coverage remain open. The paused Under
Pressure investigation is untouched; it is not included in these game
runs. Fixtures, copied disks, full pictures, hashes, logs and temporary
probe sources remain outside git under `/tmp/amiga-software-2026-11-07`.
