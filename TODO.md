# esp32ami: to do

In order: "what's next" is the first item here. Each one says why, where,
how, how it's checked, and roughly how big it is. Done items move to
`docs/architecture.md` (Progress, "The to-do list") and leave this list;
the others keep their numbers (done: 1, an RP9 setting up its machine;
2, the blitter's timing slot by slot; 3, the 68000 on the chip bus slot by
slot; 4, Paula's attach modes; 5, ECS super-hires; 6, writing extended
ADFs; 7, AGA's untested and missing parts; 8, the 68EC020's instruction
cache; 11, the Voyage prefetch, CIA and floppy follow-up).

## 10. Titles that still fail (the smoke runs)

Why: found running the user's demos and game ADFs with `tools/smoke.py`
(CLAUDE.md, Testing and Open items); each could be a fault of ours.
Check each with `smoke.py` on its folder (`ONLY=<name>`) and the
collection for the rest. Roughly a session each, in this order:

- Under Pressure (A1200 demo): reproduced 2026-11-11 from Aminet
  `DKG-UnderP.lha` (exe on a bootable 32 MB HDF;
  `build/todo10-2026-11-11/`), A1200, Kickstart 3.1, 2 MB chip + 8 MB
  fast, `AMI_TIME=1729641600`. Same failure class as the 2026-10-24 RP9
  run (addresses differ by load). Soft line drawer `$224a54`:
  `mulu.w D5,D1` with pitch `$A0` treats a negative start Y as unsigned;
  `st (A0)` at `$224a82` then writes `$FF` into a custom mirror
  (`$CFFxxx` / `$D00xxx` → BPLCON0 `$FFFF` via the byte-duplicating
  custom path). First negative Y at frame 9681 (endpoints (60,-3) /
  (57,28)); smash at 9748 with Y=-7; INTREQ floods `$5FDF` and the run
  hangs in 9750. Projection `$224ae2` writes screen verts to `$5E0094`
  (center constants at `$224B54` = 60,30); edge list `$2F0438`; draw
  `$222386` → `$224A2E` has no clip. WAIT_CHIP 5 still produces neg Y
  and the BPLCON0 smash (only the frame numbers move): do not lower the
  global wait. Angles+Z at `$2EFD3E` → sine table `$2EF33C` → D5/D6;
  center at `$224B54` is animated (`$226274`). At frame 9748 both angles
  are `$b518` so D5==D6 is a real pose, not register corruption. The
  `$224880` blend is only a framebuffer fade (`move.l (A0)+,D0` /
  `dbra`), not the angles. `$8b46` at `$21ff3c` is PACK D6,D5,#0
  (68020; in the exe after `lsr.l #8,D5`), not OR/illegal — Musashi
  runs it; the host disassembler still prints ILLEGAL for PACK.
  Camera script `$226260`: center x/y absolute from a stream, each
  angle `+= 4*center`, Z word absolute; stepped when `$21ff8e` matches
  `$22696c` (CPU-wait sensitive: WAIT 5 reaches the same poses ~60
  frames earlier). Soft drawer `$224a2e` ends the edge list with
  `bmi` only — no Y clip. Buffer base `$2fffd0` (fast); `mulu.w` of a
  negative Y lands in a custom mirror from the first neg Y (frame
  9681, vertex `(60,-3)` among otherwise sane verts). Projection
  `$224ae2`: `muls` rotate, `asr #4`, `divs` by `(depth.w + Z + 1000)`;
  that out-of-range point has rot `0,-330876,93156`, divisor 615, Z
  `-390` — the screen math checks out. Object at `$2f0404` includes
  `(0,-21,0)` (terminator `$7fff`): the off-screen screen point is a
  real model vertex, not corrupted data. Dispatcher `$2268f0` runs the
  camera every turn (phase `$21ff8e` / shadow `$22696c` only gate a
  table refresh); turns are paced by the `$21ff*` effect, so camera
  rate follows CPU/chip wait — hence WAIT 5 vs 8 only shifts frames.
  WAIT 12 (diagnostic only) still hits the same neg-Y class (~frame
  9811). WinUAE is not on this host: next, compare phase/camera rate
  to a real A1200 or WinUAE, or decide the missing soft-clip is a
  title quirk under otherwise-correct timing. Needs A1200 + 4–8 MB
  fast (readme); we use 8. Host probes: `AMI_NEGY=224a60`,
  `AMI_PROJ=224b3e` (D2/D3/Z/rot), `AMI_PROJENTRY=224ae2`,
  `AMI_ANG=2efd3e`, `AMI_PROJFROM`/`TO`, `AMI_TRACE`+`AMI_TRACE_FRAME`,
  `AMI_LOGFROM`. Work folder `build/todo10-2026-11-11/`.
- All New World of Lemmings (AGA crack, no fast RAM): an OS hook at
  $C6720 gets overwritten, then a jump through it every ~14 frames once
  Space is pressed at the title. Find the writer with
  `AMI_WATCH=C6720` (host; optional `:<frame>`), then whether a real
  A1200 without fast RAM would do the same. Needs the title's disk
  (not in the 96-game RP9 set on this machine).
- Killer (A1200): out of scope. Aminet `CNCD_Killer.readme`: needs at
  least a 68040/25 MHz (fun on a 68060). Our 68EC020 will not run it.
- Pango Fun (a beta): a Guru on every machine; see whether the beta is
  just broken (another dump, WinUAE's behaviour described anywhere).
  Needs the title's disk.

## 9. Waiting on something

Nothing here can be done as things are; each says what it waits for.

- DMS modes 2-5 (quick, medium, deep, heavy 1): written after xDMS,
  untested. Needs an archive packed in one of them (none in the
  collection; no DMS packer at hand).
- The clock's saved offset keeps a clock set while the board's time was
  wrong (unlikely with the Tab5's RTC): a remark, nothing to do.
- The 68000's bus cycles where a real one has them: the slot model takes
  an instruction's back to back from its start (SysInfo 1.03, xSysInfo
  1.06 times an A600); 1.00 needs each bus cycle's place in the
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
  at a time, other chip-register paths. Which A500 titles still fall
  behind on the Tab5 isn't known (none measured but these).
- Sprites on lines inside the window without bitplanes: Denise (WinUAE)
  wants a BPL1DAT write on a line before showing them; here they show.
  Needs a game or a test on a real Amiga that shows the difference.
