# Amiga VHPOSR V-byte poll skip — 2026-11-11

Kid Pool's title waits with `cmpi.b #-$c,$dff006` / `bne` (VHPOSR's
vertical byte). That path wasn't covered by `poll_wait()` (DMACONR,
VPOSR word, INTREQR only), so after the bus-cycle inlining it still burned
most of core 1 on the title. A gated skip of the CPU's remaining slice
after that byte read brings idle from ~6% to ~39% at full speed on the
Tab5, with the 96-game collection pictures unchanged.

## Change

In `components/amiga/custom.c`:

- Byte custom reads go through `custom_read_addr()` (`mem.c` already
  used it).
- After a VHPOSR vertical-byte read (`$dff006`), `poll_vhpos_v()` may
  call `ami_cpu_wait()` for the slice's rest.
- Skip only while a two-instruction loop will still branch back (same
  idea as `poll_wait()`: the exit pass must keep its cycles).
- Gate retained after host differentials: CMPI.B with immediate `$F4`,
  BEQ/BNE, and PC in low chip RAM (`pc < $20000`). Kid Pool's wait is at
  `$152C4`. Broader gates (any immediate, higher PCs) broke Warm Up,
  Ziriax, Tactical Manager 2 and others (disk/copper phase).

VHPOSR's horizontal byte (`$dff007`) is not skipped: it changes every
colour clock.

## Host checks

- `make -C components/amiga/host test` passes.
- `tools/collection.py` against a host without the skip: **96 of 96**
  the same (`build/pollwait-2026-11-11/collection-full/`).

## Tab5 A/B (Kid Pool title)

A500, Kickstart 1.2, 512 + 512 KB, Kid Pool RP9 in DF0:, no input, from
~2 minutes after reset (title). Reference: same tree with
`poll_vhpos_v` disabled (`if (0)`). Candidate: the gated skip. Board was
MSX before; full Amiga flash, then MSX restored from
`build/pollwait-2026-11-11/msx-before-full.bin` (hash-verified).

| Build | Speed | Core 1 load (`board.load[1]`) | Idle in profile (`esp_cpu_wait_for_intr`) |
| --- | ---: | ---: | ---: |
| Reference | 98–102% | 85–86% | 5.9% |
| Candidate | 98–102% | 53–55% | 38.8% |

Profile (8000 samples, core 1), reference → candidate:

| Symbol | Reference | Candidate |
| --- | ---: | ---: |
| `m68k_read_memory_16` | 18.6% | 4.5% |
| `beam` | 2.3% | 0.5% |
| `bus_cycle` | 3.4% | 0.4% |
| `m68k_op_cmpi_8_al` | 1.5% | (out of top 25) |
| `m68k_op_bne_8` | 1.4% | (out of top 25) |
| `esp_cpu_wait_for_intr` | 5.9% | 38.8% |

Kick Off 2 on the candidate (control, not a VHPOSR `$F4` wait): still
~100% speed with core 1 around 46% once past the early boot.

## Work folder

`build/pollwait-2026-11-11/` (not in git): host binaries, collection
logs, Tab5 reference/candidate `.bin`/`.elf`, profiles, status samples,
MSX full-flash backup.
