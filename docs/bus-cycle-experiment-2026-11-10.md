# Amiga 68000 bus cycle experiment — 2026-11-10

The 68000's accesses to plain memory now work out their bus cycles inline
in Musashi's callbacks (`components/amiga/mem.c`, `bus_cycles_quick()`)
instead of through the out-of-line `bus_cycle()`, and `bus_take()` keeps
the slots' table up to date instead of the table being worked out again
after every change. The waits are exactly the same. On the Tab5:

- **Kick Off 2's credits screen (A500):** core 1 busy 16.7 → 9.9 ms a frame
  (idle 16.7% → 50.4%).
- **Kid Pool's title (A500, slots taken on most lines):** the emulation
  went from 94–95% speed with core 1 at 100% to 100–102% with 5% idle.
- **Aerial Racers' race (A1200):** the 68EC020's data callbacks 5.3% → 3.9%
  of core 1.

No shared source changes.

## Reference and scope

Reference: esp32ami `4bf2b5e` (the 68EC020 fetch, `docs/fetch-experiment-2026-11-10.md`),
esp32emu `21fd388`. Tab5 (ESP32-P4), ESP-IDF 5.5.6, GCC 14.2, the normal
`-O2` component settings. Plain C; Musashi unchanged.

Every 68000 access (each instruction word and each datum) went through a
callback in IRAM, which saved five registers and called `bus_cycle()` in
flash. `bus_cycle()` counted the instruction's bus cycles (Musashi's
remaining cycles changing marks a new instruction). Then, for a bank on
the chip bus, it worked out the access's time and its wait: the blit, the
line's taken slots, the E clock. On Kick Off 2's credits screen the two
were 46% of core 1 (`bus_cycle()` 28–29%, `m68k_read_memory_16()` 17%),
and the memory callbacks together 49%.

A word from chip RAM with nothing in the way took 89 P4 instructions: 39
in the callback and 50 in `bus_cycle()`. One from the ROM took about 57.
Almost all of them were the globals' addresses, the stack frame and checks
that came to nothing. The slots' table (each colour clock's wait) was
worked out again, about 1,000 instructions, at the first access after any
slot was taken. With the copper interleaving with the CPU, that could be
once per copper instruction.

How often each case comes up was measured with a host build with
counters: 17 A500 RP9s of the collection, to frame 7000, with the
collection's inputs.

| Case | Share of the 68000's accesses |
| --- | ---: |
| Off the chip bus, or on it with nothing in the way | 30–97% (median ~65%) |
| On the chip bus, on a line with slots taken (five lores planes, hires ones, the copper) | 2–66% |
| A blit under way | 0–5% |
| The CIAs | 0–9% |

The rest were the chips' registers. Of the chip-bus accesses that reached
`bus_cycle()`, 41–99% waited for nothing. Eight slot-bound titles, from
frame 5500 to 6500 with no input, had 10–69% of their accesses on lines
with slots taken (Satan's crack intro 69%, Kid Pool's title 58%).

## The change

- **One block of state** (`s_m`): the bus cycle count, the remaining
  cycles after the last access, the last access's time and wait, the bank
  tables (`kind`, the 68EC020's `wait`, `rmap`, `wmap`) and the slots'
  table `wait_at`. One base address covers them all (every offset is under
  2048).
- **Bank pointers less their bank's address**: address a's byte is
  `rmap[a >> 16] + a`, so the `a & 0xFFFF` goes (two P4 instructions an
  access, the 68EC020's too). A real bank's entry is never NULL, since
  host addresses are far above 16 MB.
- **Level 1, nothing can wait**: `s_m.no_wait` is `ON_CHIP_BUS` while no
  slot of the line is taken and no blit is under way or started in it,
  else 0. It is set at the line's start, and cleared by `bus_take()` and by
  `bus_blit_started()`, which the blitter calls as a blit starts. A bank
  whose kind is at most that only counts its bus cycles (`n`, `after`,
  `last_wait` 0), and one compare decides it. `last_at` is skipped:
  `bus_waited_over()` reads it only with a wait.
- **Level 2, only taken slots**: on the chip bus with no blit started in
  the line and the CPU running, the access's colour clock from the line's
  start gives `wait_at`'s wait, then the same counting. A long's second
  word comes 4 clocks plus the first word's wait later.
- **Level 3, the rest**: a blit started in the line (perhaps ended), the
  CIAs, the chips' registers and anything else in the slow path, the CPU
  not running. These go out of line through `read16_000()` and its kin,
  which call `bus_cycle()` first: exactly the old code.
- **The table kept as slots are taken**: a slot newly taken adds 4 clocks
  to its own wait and to those of the taken slots in a row before it (and
  the odd colour clocks between). The line's start clears it when anything
  was taken. The 68EC020 skips it all (it keeps its averages).
- **No stack frame in the callbacks**: the first two levels are inline and
  the third is a tail call. GCC tail-calls only a function returning the
  same type, so the helpers and `slow_read8/16()` return `unsigned int`.
  With `uint16_t`, every path had the frame for the call.

P4 instructions per access, for one starting a new instruction (one in
the same instruction takes one more):

| Access | Before | After |
| --- | ---: | ---: |
| 68000, chip RAM word, nothing in the way | 89 | 29 |
| 68000, ROM word | ~57 | 26 |
| 68000, chip RAM word on a line with slots taken (no wait) | ~101 | 55 |
| 68EC020, chip RAM word with its wait | ~43 | 19 |

`bus_take()` grew from ~12 to ~30 instructions a slot. Code: the
callbacks and their out-of-line paths add 414 bytes of IRAM. The 3 KB
block now sits in .data, because `after` starts at -1.

## Correctness

All checks ran on the host build, reference and candidate binaries side
by side, with the same `AMI_TIME`, inputs and disk changes. Each build
below was checked before its board run.

- `tools/collection.py`: all 96 RP9 games of Amiga Forever 11 Plus on their
  manifests' machines (71 A500/A2000, 25 A1200), 7 pictures each to frame
  7000: 96 of 96 the same.
- Nine titles to frame 15000, a picture every 250: 540 of 540 the same.
  On the A1200: Aerial Racers (through its three disks into the race),
  Deluxe Galaga, Soccer Kid, Football Glory, Football Glory Indoors and
  Virtual Karting 2. On the 68000: Kick Off 2, Hannibal and Datastorm.
- The sound of the 15000 frames (`AMI_WAV`, 300 s), b4 and the final build:
  Aerial Racers, Deluxe Galaga and Datastorm byte-identical, none silent.
  Kick Off 2 is silent there; its recording and the four final pictures
  were identical too.
- `make -C components/amiga/host test`, two new checks:
  - **The inline levels against `bus_cycle()`.** One instruction's
    accesses (bytes, words and longs; chip RAM, slow RAM and the ROM; a
    new instruction among them) run on a line with three slots taken in a
    row and single ones. They start at every point from 24 clocks before
    the line to 480 into it, on two lines, once inline and once through
    `bus_cycle()` (a blit started, none under way). Each access's remaining
    cycles must match, and so must the times `bus_waited_over()` takes. One
    case is checked against the slot model worked out by hand.
  - **The slot model.** 300 lines with up to 80 slots taken in random
    order, repeats and slots past the line included. An access at every
    colour clock, both ways, must wait 4 clocks for each taken even slot in
    a row from it. The old lazy table passes this check too.
- Mistakes put into the code, one at a time:

  | Mistake | Result |
  | --- | --- |
  | A long's second word without the first's wait | Fails |
  | The wait's time without the line's start | Fails |
  | The instruction's count never reset | Fails |
  | The table's last 8 colour clocks dropped | Fails |
  | `bus_take()` leaving `no_wait` set | Fails |
  | The table update starting from 0 | Fails |
  | The table update without the odd clocks | Fails |
  | The table not cleared at the line's start | Fails |
  | The guard against a slot taken twice dropped | Passes: it writes the same values |

## Board measurements

Builds: the reference (`4bf2b5e`), b4 (the inline levels, the lazy table)
and b5 (final: the table kept up to date). Each run wrote its app and
started the machine from the setup screen with the package in DF0:. Each
took two captures (A500) or three (A1200) of 32,000 core-1 PCs at 20 kHz,
with the time per emulated frame from the fps lines.

**Kick Off 2** (A500, Kickstart 1.2, 512 + 512 KB, no input, from frame
6000, its credits screen). The LCD pictures of all four runs are
identical. The second reference run came last, about 35 minutes after the
first: there was no drift.

| Build | Core 1 busy, ms a frame | Idle | Memory callbacks | `bus_cycle()` |
| --- | ---: | ---: | ---: | ---: |
| Reference, run 1 | 16.67 | 16.7% | 49.5% | 29.0% |
| Reference, run 2 | 16.65 | 16.7% | 49.3% | 28.8% |
| b4 | 9.90 | 50.5% | 16.0% | 0.1% |
| **b5 (final)** | 9.87 | 50.4% | 16.0% | 0.0% |

**Kid Pool's title** (A500, Kickstart 1.2, 512 + 512 KB, no input, from
frame 6000). The pictures differ only in the pulsing arrows beside START,
captured at other moments.

| Build | Speed | Idle | Core 1 busy, ms a frame | Memory callbacks, ms a frame | `bus_take()`, ms a frame |
| --- | ---: | ---: | ---: | ---: | ---: |
| Reference | 94–95% | 0.0% | 21.2 | 9.7 | 0.34 |
| b4 | 100–101% | 2.5% | 19.4 | 6.5 | 0.41 |
| **b5 (final)** | 100–102% | 5.4% | 18.8 | 5.3 | 0.91 |

The reference couldn't keep up. With b5 the memory callbacks cost 45%
less per frame. The code they don't touch (`display_fetch`, Paula, the
line records' `memcpy`) costs 4–25% more per frame at full speed than in
the reference's 94%. The causes aren't established: both cores do more
frames a second, and the PSRAM is shared. Kick Off 2, at the same speed in
both builds, shows no such change.

**Aerial Racers' race** (A1200, Kickstart 3.1, 2 MB chip RAM). Fire at
frame 6100, the package inserted again at 7012 and 8682 (disks 2 and 3),
three captures from frame 10700. The race isn't deterministic: memcpy
swings between 6% and 14% between captures, and idle between 3% and 15%.
The data callbacks are steady across captures:

| Build | Data callbacks | Of the CPU emulation | `m68k_read_memory_32` | `_16` | `_8` | `m68k_write_memory_16` |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Reference | 5.3% | 10.9% | 2.35% | 0.45% | 0.71% | 0.99% |
| **b5 (final)** | 3.9% | 8.2% | 1.86% | 0.23% | 0.29% | 0.53% |

The fetch's share, 17.2% → 16.7%, is unchanged within the noise. All
runs kept 100% speed.

b4's race run is left out because it measured the wrong thing. It came
right after the reference race, with Aerial Racers still in DF0:. The
script's insert then gave disk 2, which Kickstart 3.1 booted to a Shell
(the log shows disks 3 and 1 at the later inserts). The race runs that
count each followed an A500 run. A race script should insert another
package first.

## Next

On Kick Off 2's credits screen, core 1 is now Musashi and the callbacks'
remaining instructions. `m68k_read_memory_16()` takes 12.4%, about 0.4%
of core 1 per instruction of its 29, and `ami.bus`'s address is two of
those. On Kid Pool, the cost is spread out: `bus_take()` 0.9 ms a frame
(the display's slots taken one call each), and the chips' registers
through `bus_cycle()`. Those were largely byte reads of the beam's
position in a `cmpi.b` loop (`m68k_op_cmpi_8_al`, `custom_read()` and
`beam()` in the profile), which `poll_wait()` didn't cover: done next
(`docs/poll-vhpos-experiment-2026-11-11.md`). The A1200's race is unchanged in its large
items: the fetch, `m68k_execute`, Lisa's fetch and Paula.

The board was restored to its MSX. The full 16 MB backup taken before
(the same bytes as the one taken before the fetch trial, with its NVS:
the Amiga's settings and drives as they were) was written and
hash-verified. Its setup screen matches the earlier restore except for
the clock, and its Disk BASIC screen is identical. The test packages were
deleted from the card. Backup, binaries, profiles and logs stay in
`build/buscycle-2026-11-10/` (not in git).
