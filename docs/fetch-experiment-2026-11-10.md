# Amiga instruction fetch experiment — 2026-11-10

The 68EC020's instruction words now come through a pointer to the next
word while it runs on through plain memory (`components/amiga/mem.c`,
`s_ic.next_word`), not through the bank table and the mask each time. The
cache's and the chip bus's timing are counted exactly as before. In Aerial
Racers' race on the Tab5, the fetch's share of the CPU emulation falls
from 34.0–34.1% to 32.4% (every capture: 33.6–34.8% against 32.1–32.7%):
the fetch about 7% cheaper, about 1.2 points of core 1. The 68000's path is
the one it was, one instruction later: an A500 screen measures the same.
No shared source changes.

## Reference and scope

Reference: esp32ami `600a2e5`, esp32emu `b02f7f0`. Tab5 (ESP32-P4),
ESP-IDF 5.5.6, GCC 14.2, the normal `-O2` component settings. Plain C, no
assembly; esp32emu (Musashi) unchanged, so no sibling builds are affected.

With `CONFIG_M68K_PREFETCH` every instruction word is one call of the
Amiga's `m68k_read_immediate_16()`: the word after each opcode and each
extension word. The 68EC020's path took 22 P4 instructions for a
longword's second word, 36 for a first word in the cache and 48 for a miss
with the chip wait: the bank's pointer, `a & 0xFFFF`, `s_wait` and each
global's address every time, the common case behind taken branches. The
68000 went straight on to `m68k_read_memory_16()`.

Profiled in the race, the samples are spread evenly over the function's
instructions, not on the word's load: the cost is the instructions. Nearly
every fetch is sequential, about half are a longword's second word, and
about half of the cache checks miss (64 longwords: the race's loops are
bigger).

### The change

- `s_ic.next`: the last word's address + 2. That is the old `s_ic_prev`'s
  information (a longword's second word is `a == next` with bit 1 set; a
  bank's first word is never one), so the cache's timing is unchanged.
  `next_word` is that word's host address while it lies in the same 64 KB
  bank of plain memory, `next_wait` its bank's `s_wait`. A fetch at `next`
  takes the word through the pointer: 19 P4 instructions for a second
  word (straight-line), 28 for a cached first word, 39 for a miss. Anything
  else (a branch, the slow path's banks, a bank's end) the general way,
  which sets the pointer up again.
- The cache's tags are the longword's address | 1, line n at byte n * 4
  (the address's bits 7-2), one to one with the old `(a >> 2) + 1` at
  index `(a >> 2) & 63`; empty is still 0.
- `mem_update_map()` clears the pointer (any map change: OVL, autoconfig,
  the hard disk board) and keeps `s_ic.cpu020` (`ami.cpu_shift`) beside
  it: the 68000 is tested first and goes on to `m68k_read_memory_16()` as
  before (5 P4 instructions to get there, 4 before).
- The state in one block (one base address on the P4).

Code: `m68k_read_immediate_16()` 198 → 306 bytes (IRAM), the state 264 →
276 bytes; `m68k_read_memory_16()` and `bus_cycle()` as they were.

## Correctness

All on the host build, reference and candidate binaries side by side
(the same `AMI_TIME`, inputs and disk changes), for every variant below
before its board run, and for the final one:

- `make -C components/amiga/host test`: floppy timing and chip bus checks.
- `tools/collection.py`: all 96 RP9 games of Amiga Forever 11 Plus on their
  manifests' machines (71 A500/A2000, 25 A1200), 7 pictures each to frame
  7000: 96 of 96 the same.
- Nine titles to frame 15000, a picture every 250 (Aerial Racers through
  its three disks into the race, Deluxe Galaga, Soccer Kid, Football
  Glory, Football Glory Indoors, Virtual Karting 2 on the A1200; Kick Off
  2, Hannibal, Datastorm on the 68000): 540 of 540 the same.
- The sound of the 15000 frames (`AMI_WAV`, 300 s): Aerial Racers, Deluxe
  Galaga and Datastorm identical byte for byte, none silent; Kick Off 2
  (silent there) and all four final pictures identical too.

## Board measurements

**The 68EC020**: Aerial Racers' RP9 (an uploaded copy) on its A1200
(Kickstart 3.1, 2 MB chip RAM): fire at frame 6100, the package inserted
again at 7012 and 8682 (disks 2 and 3), from frame 10700 in the race (an
LCD picture each run) three captures of 32000 core-1 PCs at 20 kHz, ~11 s
apart. The race isn't deterministic on the board: every share of all
samples moves with the scene; the fetch's share of the CPU emulation's
own samples (Musashi's functions and the fetch) doesn't.

| Build | Fetch, % of all | Fetch + `m68k_execute` | Musashi + fetch | Fetch / CPU emulation |
| --- | ---: | ---: | ---: | ---: |
| Reference, run 1 | 14.53 | 30.28 | 42.73 | 34.0 (33.7–34.4) |
| Reference, run 2 | 14.55 | 30.32 | 42.63 | 34.1 (33.6–34.8) |
| v7 (68000 test after), run 1 | 12.23 | 28.19 | 40.51 | 30.2 (30.0–30.4) |
| v7, run 2 | 12.62 | 29.27 | 42.00 | 30.1 (29.8–30.3) |
| **v10 (final)** | 13.45 | 29.10 | 41.47 | 32.4 (32.1–32.7) |

The two reference runs, half an hour apart, bracket the others: no drift.
For the same instruction mix the final fetch costs ~7% less and the CPU
emulation ~2.4% less. Idle (13.8–14.3% reference, 15.7% final) moves too
much with the race to show it. Every run kept 100% speed.

**The 68000**: Kick Off 2's RP9 on its A500 (Kickstart 1.2, 512 + 512 KB),
no input, two captures from frame 6000 (its credits screen: the same LCD
picture in every run).

| Build | `m68k_read_immediate_16` | `m68k_read_memory_16` | `bus_cycle` | Idle |
| --- | ---: | ---: | ---: | ---: |
| Reference, run 1 | 2.27 | 16.84 | 28.3 | 16.22 |
| Reference, run 2 | 2.09 | 16.52 | 28.4 | 16.60 |
| v7 | 4.01 | 17.38 | 28.6 | 13.36 |
| v9 (a 68000 pair, `fetch68`) | 3.97 | 4.18 + 14.26 | 25.1 | 13.31 |
| **v10 (final)** | 2.64 | 16.80 | 29.1 | 16.62 |

## Rejected on the way

- **v7, the 68000 tested after the 68EC020's fast path**: the larger
  68EC020 gain (30.1% instead of 32.4%: 16, 25 and 36 instructions), but
  the 68000 went through two loads and a compare first, 4 instructions a
  word: 1.7 points of core 1 and 3 of idle on the A500 screen, where the
  A500 is 84% busy. Testing the 68000 first costs the 68EC020 2–3
  instructions a word instead.
- **v9, a pointer pair for the 68000 too** (`fetch68()`, its bank's
  `s_contended` kind beside it, the bus cycle with the kind passed in):
  the 68000's fetch work came out even (the frame and the call to the
  out-of-line `bus_cycle` in flash outweighed the lookups saved), and the
  per-line functions in flash ran 10–20% slower (layout); idle −3 points.
- **Inlining the fetch into Musashi** (a machine header its `m68kconf.h`
  includes, its prefetch through a host macro; in esp32emu):
  `m68k_read_immediate_16` fell from 14.5% to 6.5% of core 1, but
  `m68k_execute` rose from 12.4% to 18.3% and the out-of-line
  `m68ki_read_imm_16` (in flash) from 2.9% to 4.9%: fetch plus execute
  30.3% → 30.8%. On the P4 each instruction costs about the same wherever
  it runs (0.25–0.35% of core 1 an instruction run once per 68020
  instruction in the race); a call is two of them. The inlined copies laid
  the common case out of line, behind taken branches. Reverted: Musashi
  untouched.
- Taking only the precomputed `base + a` per bank and one block of state:
  2–4 instructions; folded in.

## Next

`bus_cycle()` (the 68000's slot model in `mem.c`) is 28% of core 1 on
Kick Off 2's credits screen: ~50 P4 instructions an access with no DMA in
the way, out of line in flash, for every 68000 access (instruction words
and data). The blitter trial's OCS control measured the same (29.3–29.7%).
It is the A500's largest single cost and the next target, in C first, its
timing exact (the slot model's lessons in CLAUDE.md).

The board was restored to its MSX: the full 16 MB backup taken before
(with its NVS: the Amiga's settings and drives as they were), written and
hash-verified; its setup screen and Disk BASIC match the earlier restore's
pictures. The test uploads are deleted from the card. Backup, binaries,
pictures, profiles and logs stay in `build/fetch-2026-11-10/` (not in git).
