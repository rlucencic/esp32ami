# A faster 68EC020, the handlers' side, 2026-11-26

TODO item 19, after `faster-020-loop-2026-11-26.md`. That step left the
loop around each instruction at 28 P4 instructions; its next step was the
handlers: count which handlers and effective-address helpers take the
samples, then try the commonest in the loop.

The harness is the one before: the six A1200 titles over 15000 frames on
the Tab5 (the median speed past frame 3000, the slowest 5 s in brackets)
and the four A500 titles (core 1 over 20 s at a fixed frame).

## The count

The four titles that compute (Deluxe Galaga, Gea Lux, Leading Lap,
Virtual Karting 2), 32000 samples of core 1 each, in the build committed
before (`final` there):

- 419 handlers took samples, 21% of core 1 between them.
- Each title has its own few. Virtual Karting 2: `move.b
  (d8,An,Xn),(Am)+` 5.2%, its index helper `m68ki_get_ea_ix()` 3.5%,
  `subx.l` 2.4%, `dbf` 2.2%. Gea Lux: `dbf` 4.6%, `cmp.w Dn,Dn` 3.4%,
  `move.w (An)+,Dn` 2.6%. Deluxe Galaga: the two `movem.l` 4.3%.
- The commonest across the four, `dbf`, takes 1.9% on average. No handful
  of handlers is worth a place in the loop.

What every handler pays instead:

- the address of Musashi's state (`m68ki_cpu`): `lui` and `addi` at the
  top of nearly every handler and helper;
- the data callbacks' stack frame: the word and long ones saved two or
  three registers on every access, the 68000's too;
- the effective-address helpers' frames (`m68ki_get_ea_ix()` saved five
  registers for the full extension format's calls on every use);
- the 68000's address-error check, asked of the 68EC020 too.

## Step 13: the data callbacks without a stack frame

`m68k_read_memory_16/32()` and `m68k_write_memory_16/32()` opened a frame
on every path: the 68EC020's odd accesses split into byte and word calls
the callback had to come back from (the lesson "a call anywhere in a
function costs every path its stack frame"; the split came with the
odd-address fix for All New World of Lemmings). The splits and a long
across a bank's end are out of line now (`read16_odd()`, `read32_odd()`,
`read32_split()`, the writes the same), returning the callbacks' type:
every call in a callback is a tail call, no frame. A 68EC020 word write to
fast RAM: 29 P4 instructions → 21. The 68000's paths lose the frame too.

The data callbacks' share of core 1: Deluxe Galaga 13.7% → 11.6%, Leading
Lap 11.1% → 9.0%, Virtual Karting 2 8.0% → 6.9%.

## Step 14: Musashi's state within the global pointer's reach

On RISC-V the linker reaches anything within 2 KB of the global pointer
in one instruction (the `lui` deleted). `m68ki_cpu` (2.6 KB) and the cycle
counts were in `.bss`, out of reach. Now (esp32emu, `M68K_STATE_ATTR` in
`m68kconf.h`, ESP-IDF on RISC-V) they are in section `.sdata.m68k`, and
`m68ki_cpu` is 560 bytes: its 2 KB 68030 MMU page cache (esp32mac3's) is
apart, `m68ki_mmu_last`. On the Amiga `m68ki_cpu` is 168 bytes below gp;
every reference to it and to `m68ki_remaining_cycles` is relaxed (2104 and
all). The other P4 projects' state lands 440-456 bytes below gp, all
relaxed.

Not plain `.sdata`: that section comes first, at the window's very edge
(gp - 2048), and the linker relaxes a symbol below gp only with a margin
for alignment left: none of the 2104 references were.

The A500 gains too: X-Balls' core 1 83% → 81%.

## Step 15: the index helper's brief format

`m68ki_get_ea_ix()` saved five registers on entry for the 68020's full
extension format, which makes up to four calls. The full format is out of
line now (`m68ki_get_ea_ix_full()`, a tail call), and the brief format,
the one games use, saves three around its one call (the extension word).

## Step 16: the address-error check, the address first

Musashi checks a word or long access for an odd address on the 68000 and
68010 (`m68ki_check_address_error_010_less()`): with the type chosen at
run time (`M68K_RUNTIME`, the Amiga) that was the type's load and a test,
then the address's test. It tests the address's bit first now: an even
access costs one test on either CPU (the 68000 saved three instructions, the
68EC020 one). A fixed type folds away as before (the ST, the Macs, the QL).

## The builds on the Tab5

`committed` is the build committed before (`faster-020-loop`'s `final`),
`13` step 13, `13-14` steps 13 and 14, `13-16` all four (kept). Steps 15
and 16 were measured together: a separate run of 15 was dropped (each
board run takes 40 minutes, and the small steps sit inside the runs'
spread).

| Title | committed | 13 | 13-14 | 13-16 |
| --- | ---: | ---: | ---: | ---: |
| Charr (its menu) | 99.9 (99.4) | 99.8 (99.6) | 99.8 (99.4) | 99.9 (99.5) |
| Christmas Breakdown | 99.9 (99.4) | 99.9 (99.5) | 99.8 (99.5) | 99.8 (99.4) |
| Deluxe Galaga | 95.1 (64.3) | 96.9 (65.4) | 95.0 (66.0) | 98.1 (66.6) |
| Gea Lux (play slowest) | 99.6 (92.8) | 99.6 (90.0) | 99.6 (93.1) | 99.6 (95.1) |
| Leading Lap | 96.8 (84.8) | 96.4 (85.2) | 98.6 (86.8) | 99.5 (88.0) |
| Virtual Karting 2 | 78.0 (73.1) | 80.1 (73.5) | 80.9 (75.7) | 81.0 (75.6) |

Core 1 in the titles at full speed: Charr's menu 84% → 81%, Christmas
Breakdown 88% → 83%. The 15000 frames took Virtual Karting 2 366 s → 355
s, Deluxe Galaga 340 → 334, Leading Lap 320 → 314.

| A500 title (core 1) | committed | 13 | 13-14 | 13-16 |
| --- | ---: | ---: | ---: | ---: |
| X-Balls | 84% | 83% | 81% | 80% |
| Kick Off 2 | 46% | 45% | 44% | 43% |
| Kid Pool | 49% | 49% | 49% | 49% |
| Arkanoid | 38% | 38% | 38% | 38% |

All A500 titles at 100% in every build.

- Step 13: Virtual Karting 2 78% → 80%, Deluxe Galaga's median 95% → 97%.
- Step 14: Leading Lap 96% → 99%, Virtual Karting 2's slowest 5 s 74% →
  76%, the A500's core 1 1-2 points.
- Steps 15 and 16: Deluxe Galaga's median 95% → 98%, Gea Lux's slowest
  93% → 95%, Leading Lap → 99.5%.
- All of them: Virtual Karting 2 78% → 81%, Leading Lap 97% → 99.5% (its
  slowest 85% → 88%), Deluxe Galaga 95% → 98% (its slow stretch 64% →
  67%), Gea Lux's slowest 93% → 95%.

## Checks (host)

- The collection's pictures, each step against the one before (13 against
  the committed build, 15 against 13, 16 against 15): 96 of 96 the same
  each time.
- `make -C components/amiga/host test` and Musashi's `run run-prefetch
  run-020 run-hostloop` after each step.
- The four replays pass (the `13-16` build).
- Musashi's test built for the 68030 fails the same 7 checks with and
  without the changes (the test expects the 68000's exception frames).
- esp32mac3 (the 68030 and its MMU): the LC II and LC III ROMs on its host
  build, 3000 frames each: the same picture, PC and tick count with and
  without the changes. Mac OS itself (which turns the MMU on) not run: no
  system disk here.
- The other Musashi firmwares build (esp32mac2, esp32mac3, esp32ste,
  esp32mac, esp32ql on the Tab5; esp32ste, esp32mac, esp32ql on the
  MaTouch).

## What is left

- Virtual Karting 2's race, ~76-81%.
- Deluxe Galaga's credits and instructions, ~67% (its median 98%).
- Leading Lap at its slowest, ~88% (its median 99.5%).
- Gea Lux in play, ~95% at its slowest.
- AmiBee's 5000-7000 and Metal Kombat's 3D intro, not measured since
  2026-11-25.

The C side is near its floor:

- The loop's 28 instructions are what Musashi's interface asks for: the
  PC, PPC, IR and cycles in memory, the jump table, the trace mark, the
  cache's timing.
- Extension words inline in the handlers save ~10 instructions on a
  longword's second word and lose ~4 on a first: ~3 a word on average,
  at 695 call sites. Step 10's window measured that and came out a wash.
- The handlers' time is spread over hundreds of them.

Smaller items left: `s_m` within gp's reach (one instruction per data
access; it is 3 KB, and the window holds ~1.5 KB more); DBcc's extension
word read inline (GCC left `m68ki_read_imm_16()` out of line in 224
handlers, the DBcc ones among them); both taken up below. A bigger step would change how
instructions run: a translating core for the 68EC020's hot code, a project
of its own.

## The two small items, afterwards

- **`s_m` within gp's reach: not possible.** Placed 652 bytes above gp
  (in `.gnu.linkonce.s.*`, which ESP-IDF's script puts after every
  `.sdata.*`, so Musashi's state stayed where it was), none of its 39
  references were relaxed. The linker relaxes a reference only if the
  whole object, its size plus a margin for alignment, lies within the
  window: `s_m` is 3 KB, the window has ~1.4 KB left. Splitting it gains
  nothing (the callbacks need its 2 KB of bank tables). The same rule
  explains plain `.sdata` above. Reverted.
- **The extension-word reads always inline** (esp32emu,
  `M68KI_IMM_INLINE`, without the prefetch): GCC had left
  `m68ki_read_imm_16/32()` out of line in 268 handlers (the DBcc ones
  among them), a call more per extension word. Now none; ~24 KB more
  code. On the Tab5 (`small`, against `13-16`):

  | Title | 13-16 | small |
  | --- | ---: | ---: |
  | Charr (core 1) | 81% | 80% |
  | Christmas Breakdown (core 1) | 83% | 84% |
  | Deluxe Galaga | 98.1 (66.6) | 97.8 (67.1) |
  | Gea Lux (play slowest) | 99.6 (95.1) | 100.0 (94.4) |
  | Leading Lap | 99.5 (88.0) | 98.8 (87.9) |
  | Virtual Karting 2 | 81.0 (75.6) | 81.1 (76.5) |

  No change beyond the runs' spread (the A500's core 1 the same within a
  point); kept, no slower. The collection the same, 96 of 96.

## Files

`build/small-2026-11-26/` (the inline reads: firmware, host binary,
collection, board run). `build/handlers-2026-11-26/` (not in git): each build's firmware and ELF
(`frames` step 13, `state` 13-14, `ix` 13-15, `aerr` 13-16) and host binary,
the collection runs and the replays, the board runs (`board-<build>/`,
`ab500-<build>.log`).
