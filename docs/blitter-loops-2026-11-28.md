# Deluxe Galaga's dips: the blitter (2026-11-28)

TODO 19's last look: Deluxe Galaga was the one title still dipping on the
Tab5 (slowest windows 94.1-97.2% over four passes), right after its
credits and at varying places in play. Its game differs from run to run
on the board, so the dips move.

## Where the time went

- **The host first** (`AMI_FRAMESTAT`, a frame at a time): after the
  click that starts the game (frame 4000) the CPU's work falls fivefold
  (1.29 M RISC-V instructions a frame in the credits, 215-265 k after it),
  with a little warm-up (39 translations and 364 instructions interpreted
  a frame for 50 frames). So the dip after the click wasn't the 68k code.
- **The board**: core 1 at 100% from the click to ~frame 4230 in all three
  passes looked at, 64-77% after. The pass's own profile there came after
  a 3-second picture, past the dip (25% idle). A variant of the pass
  (`build/dips-2026-11-28/board_prof.py`, not in git) took profiles at
  fixed frames (4050, 4150) and wherever core 1 stayed at 99% for 3 s,
  without pictures.
- **The profiles**: the blitter (`blitter.c`'s `start()`, its loops
  inlined) 10.8-16.0% of core 1, `wait.c` (the loops waiting for it)
  7-8%, the translated code only 12.6-16%. The host hides it: its caches
  are large, and its blitter takes a tenth of a frame.
- **The blits** (`AMI_BLITSTAT`, frames 8000-11500 on the host): ~5600
  words a frame (~50 in the credits). Clears (D alone, minterm $00) 41.8%
  of the words, 260 a blit; cookie cuts ($FCA, A and B shifted) 38.3%,
  139 a blit; unshifted cookie cuts 11.7%; a descending fill copy ($9F0)
  7.3%; lines 0.9%. Narrow bobs: each row of each channel a new PSRAM
  line. Inside `start()` the samples fell on the general loop's own work
  (a channel's use bit tested a word, its state reloaded from the stack,
  the minterm through a chain of compares) more than on the reads.

## The code (`blitter.c`)

- `blit_const()`: D alone with minterm $00 or $FF, no fill: every word
  the same, a row a `memset` where it doesn't wrap chip RAM, else word by
  word; BZERO from the value.
- `blit_cookie()`: A, B, C and D with minterm $CA, ascending, no fill:
  the general loop's steps without its tests (A, B, C read, then D
  written, word by word: in place they meet as before; the first and last
  word's masks; the shifts' words carried across rows as there).
- The general loop for the rest; `blit_generic` (host) forces it.

## Checked

- `host/blit_test.c` (in `make test`): random blits over a 256 KB chip
  RAM (rows wrap round its end), D over C or a word beside it in a
  quarter of the cases, each done by the special loops and by the general
  one from the same memory and registers; the memory, the pointers, the
  data registers, BZERO and the colour clocks compared. 3 seeds x 20000
  cases the same. It caught a clear's row a word short, a descending clear
  two bytes off, the cookie cut's shift words dropped at each row and C
  read a word ahead of D's write (the last only once D could lie over C).
- The collection (96, every model), the survey (26) and the replays (4)
  the same as the build before; `tools/blitboot.py`'s timing 29, 40, 65
  lines as before.

## The Tab5

- The profiles in the dips (the same frames, the variant pass): the
  blitter 10.8% → 6.7% (frame 4050), 16.0% → 8.8% (4150), 12.8-14.1% → 5.2%
  in play; `wait.c` the same (~8%).
- Deluxe Galaga's own pass (median, slowest window without the pass's
  pictures and profiles):

| Pass | Median | Slowest | In play, slowest | Windows under 97.5% |
|---|---|---|---|---|
| The resumes | 99.6% | 94.1% | 95.0% | 9 of 50 |
| The collisions | 99.8% | 95.3% | 96.7% | 2 of 50 |
| The resumes' records | 99.8% | 94.5% | 94.5% | 1 of 50 |
| The divides | 99.8% | 97.2% | 97.2% | 1 of 50 |
| The blitter's loops | 99.8% | 98.8% | 98.8% | 0 of 50 |

  One pass of a game that differs from run to run: the dips' cause cut
  in half on the profiles, and none left below 97.5% in it.

## Left

- `wait.c`'s ~8% of core 1 while the game waits for its blits (the loops
  worked out a line at a time).
- The fill copy ($9F0, 7% of the words) and the A500's blits: the
  general loop.
