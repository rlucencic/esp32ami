# Deluxe Galaga's collisions (2026-11-28)

TODO 19's item after the resumed slices: Deluxe Galaga was the one title
still falling behind on the Tab5 (its credits, frames ~2100-4300, at
91.5-99.4%), with the collisions (`display.c`'s `collide()`, `pf_line()`,
`bits32()`) at 9.5% of core 1 there.

## Measured first

`AMI_CLXSTAT` (host only) over the credits, frames 3133-3938:

- CLXDAT read once a frame (805), CLXCON written three times a frame (the
  last $13C8: planes 1-4 enabled, plane 4 wanted 1, the others 0; sprite
  1 with 0). Each read clears the bits, so every frame starts again.
- 228620 lines worked out, 131882 of them with bit 0 (the odd playfield
  against the even one) still clear: each worked both playfields out over
  the window (`pf_line()`: each side's planes ANDed, then moved to the
  window by its scroll, a word a 16 pixels), and found it 664 times in
  805 frames. Lines with sprites shown: 16576, 4801 of them working the
  playfields out for a sprite.
- 91937 lines had nothing left to find (bit 0 set, the sprites' bits
  too).
- On the host's cycle counter: 628 cycles a line, 107 M of the credits'
  143 M in bit 0. (`clock_gettime()` around each line first: it costs
  ~0.7 µs on this PC and the times were its own.)

## The code (`display.c`)

- `pf_both()`: bit 0 on the fetched words in place when both playfields'
  pixels lie where the fetch put them alike: lores, and the same scroll
  for the sides with an enabled plane fetched. A pixel then matches both
  where every enabled plane has its MVBP value, so the enabled planes'
  words are ANDed over the words the window covers (four or eight at once:
  the planes left over point to words of 0 taken as matching), the end
  words masked to the window; outside the fetch, and planes not fetched,
  are 0 as before. Hires and super-hires keep `pf_line()`: a lores pixel
  may match one side in one half and the other in the other.
- `pf_span()`: a sprite's 16 pixels against each playfield from the
  planes' words there (2-3 words a plane, any resolution) instead of
  both playfields over the window; `pf_line()`'s results are used when
  bit 0 made them already.
- A line with bit 0 found and no sprite armed returns at once.
- `bits32()` inline (the board's profile had it as a function of its own,
  2.1%).

Host only: `AMI_CLXCHECK` works every line out both ways and stops at a
difference; `AMI_CLXSLOW` takes the old way for bit 0.

## Checked

- The host's cycles in the credits: 628 → 275 a line (bit 0 107 M → 28
  M).
- `AMI_CLXCHECK` on Deluxe Galaga's credits and its later stretch (frames
  10000-11400): 260867 lines through `pf_both()` and the sprites' spans
  through `pf_span()`, no difference; the survey (26) and the collection
  (96) with it on, the pictures the same as the build before.
- 160 of `tools/clxboot.py`'s boot disks (lores and hires planes: none,
  single pixels at either end and inside, patterns; CLXCON with no plane,
  plane 1, plane 2 (never fetched: 1 plane) with either value, both,
  more, ENSP1 and ENSP3; sprites inside, at and past the window's edges,
  pairs), the build before against this one with the check on: every
  CLXDAT the same. `tools/agaboot.py`'s clx7, clx7off, clx7reset,
  clxwide and clxwide0: right.

## The Tab5

Deluxe Galaga's board pass (median, slowest window without the pass's
pictures and profiles; core 1's median):

| | Before | Now |
|---|---|---|
| Speed | 99.6% (94.1%) | 99.8% (95.3%) |
| The credits, frames 2100-4400 | 91.5-99.4% | 95.3-100.4% |
| Windows under 97.5% | 14 of 58 | 2 of 57 |
| Core 1 | 85% | 83% |

- Its profile in the credits: the collisions 9.5% of core 1 (`pf_line()`
  4.3%, `collide()` 3.1%, `bits32()` 2.1%) → 4.2% (`collide()` with
  `pf_both()` inlined 4.1%, `pf_span()` 0.1%).
- Left under 97.5%: frames ~3950 (95.3%, past the credits) and ~11150
  (96.7%), not profiled.
