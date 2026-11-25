# Wait loops without the CPU, 2026-11-25

TODO item 18. The A1200 survey (`a1200-load-survey-2026-11-25.md`) found
titles that fall behind while most of their instructions are a
two-instruction wait:

- a RAM flag the title's interrupt code sets;
- VHPOSR outside `poll_vhpos_v()`'s gate;
- a CIA register.

On the Tab5 such a loop costs about a microsecond of core 1 per
instruction, the same as useful work. These loops now pass without the
CPU.

## The change

- **`wait.c` (new): the skip.**
  - `run_cpu()` asks `wait_skip()` as a line begins. In the line's other
    slices it asks only while the CPU is in a loop: a check in every slice
    cost an A500 title about a point of core 1.
  - Near the PC the last check found, the code is decoded once. A loop is
    a test and a Bcc back to it, or a branch to itself:
    - the tests: TST, BTST #n / Dn, CMPI, CMP `<ea>,Dn` and MOVE
      `<ea>,Dn`;
    - their operands: Dn, (An), (d16,An), abs.W, abs.L and (d16,PC).
  - The loop is kept with its words, which are compared each time it is
    used, because code changes.
  - The test is worked out from memory as it is, without the reads' side
    effects. If the branch will be taken, no interrupt is due (the IPL
    above SR's mask, or 7) and no trace bit is set, the slice's time passes
    without the CPU.
  - PCs found to be no loop are kept in a table and forgotten each frame.
- **Why the skip is safe.** What these loops test changes only where the
  CPU's slice ends:
  - RAM changes through the CPU itself, whose interrupts come as a slice
    starts; through the blitter, which runs at once at a write that ends
    the slice; and through disk DMA, at lines' ends.
  - The chips' registers (`custom_peek()`: DMACONR, VPOSR, VHPOSR's
    vertical byte, POTGOR, INTENAR, INTREQR) change at events and lines'
    ends.
  - A CIA's port A, TOD and SDR (`cia_peek()`) change at lines' ends. TOD
    counts only while it isn't latched.
  - The exception is a CIA timer with its interrupt off. It underflows
    anywhere, so a skip on ICR, CRA or CRB ends at that timer's next
    underflow (`next_underflow()`, split out of `plan()`).
- **`mem_peek()`** reads plain memory, those registers and the CIAs at an
  address.
- **`poll_wait()`** (`custom.c`) still passes the rest of the first
  slice. Now it does so only while the loop will go on
  (`wait_poll_loops()`); see Pango Fun below.

## Pango Fun: the exit pass

The first version failed one replay:

- **What happened.** Pango Fun's intro never ended, and its disk never
  loaded again.
  - Its main loop waits for line 0 with `tst.b 5(a5) / bne` (V8), then
    `tst.b 6(a5) / bne` back (the vertical byte).
  - In line 0, `poll_wait()` passed the rest of the first slice before the
    V8 read, although that read ended the loop. The read came at clock 40,
    with VERTB's interrupt.
  - The handler then ran before the second test, and past line 0, every
    frame.
- **Why it had worked.** The frame's last instruction, overshooting the
  frame's end, had read line 0's V8 by luck. `wait.c` passes line 312
  exactly, and so took that luck away.
- **The fix.** `poll_wait()` now skips only while the loop will go on: on
  the exit pass the time belongs to the code after the wait.

## Host checks

- **`make -C components/amiga/host test`** runs the new `wait_test.c`:
  small 68000 programs in chip RAM behind a stub ROM.
  - A RAM flag set by the VERTB handler: the loop ends in line 0, the wait
    takes under 2000 instructions where a frame took ~11800, VERTB comes
    every frame, and `bra.s *` passes too.
  - Pango Fun's wait for line 0 with a 44-line handler: line 0 is reached
    in every frame, and the wait for line 100 passes.
  - A one-shot CIA-B timer polled through ICR with its interrupt off: the
    loop ends 2560-2620 colour clocks after the start for 512 E ticks.
    Without the limit it ended after 2708, at the next line.
  - A kept loop's branch turned into a NOP by a blit the copper starts: the
    loop ends at line 150.

  Each check fails with its part taken out: the skip, the CIA limit, the
  code comparison, `poll_wait()`'s fix, or the interrupt test.
- **SysInfo 4.4.**
  - A500 with Kickstart 1.3: 535 Dhrystones, 1.00 times an A600, chip
    speed 1.12.
  - A1200 with 3.1: 1260 Dhrystones, 1.03 times an A1200.
  - Both screens are identical to the build before in every pixel.
- **The replays:** 4 of 4 pass at 98-99%. The first data difference is a
  full-track read starting at another rotational position.
- **The collection (96 games):** 72 identical to the build before. The 24
  others show the same scenes at other moments:
  - Charr's scroller, Ziriax's stars and Tactical Manager 2's random news.
  - Deluxe Pacman's unwatched game ends sooner in the old build.
  - Warm Up was caught between two scenes.
  - Aerial Racers' menu misses the collection's 5-frame fire. The menu
    reads the button every 7 frames, now 2 frames out of phase. The survey
    holds fire long enough, and its host pass races as before.
- **Instructions a frame** at the survey's stretches (`AMI_PCLOG`, the same
  input at the same frames):

  | Title, frames | Before | After |
  | --- | ---: | ---: |
  | Black Viper, briefing 3141 | 27.9k | 0.6k |
  | Black Viper, race 14000 | 13.8k | 5.0k |
  | Fatman, title menu 6921 | 25.6k | 0.5k |
  | Fatman, in play 14722 | 21.1k | 5.7k |
  | Soccer Kid, in play 13034 | 23.0k | 4.3k |
  | Starbase 13, intro 5358 | 21.6k | 6.6k |
  | Deluxe Galaga, in play 9000 | 22.7k | 9.4k |
  | The Lost Prince, in play 9802 | 20.5k | 12.0k |
  | Christmas Breakdown, intro 3735 | 17.2k | 11.9k |
  | Deluxe Pacman, in play 14566 | 23.3k | 17.0k |
  | Zero Gravity, in play 11156 | 22.4k | 20.6k |
  | Virtual Karting 2, race 9000 | 41.7k | 36.6k |
  | Aerial Racers, race 11300 | 14.8k | 14.5k |
  | Deluxe Galaga, instructions 3713 | 37.1k | 37.1k |
  | Leading Lap, race 9000 | 36.4k | 36.7k |
  | Tactical Manager 2, 9000 | 30.8k | 30.8k |
  | Gea Lux, in play 9000 | 56.4k | 56.4k |

  The last four never wait. They are item 19's.

## On the Tab5

**A500 titles** (Kickstart 1.2, 512 + 512 KB, no input), core 1 over 20 s
at a fixed frame:

| Title, frame | Before | After | Speed |
| --- | ---: | ---: | --- |
| X-Balls, 5000 | 87% (84-88) | 89% (85-90) | 100% |
| Kick Off 2, 6000 | 47% | 49% | 100% |
| Kid Pool, 6000 | 54% | 50% | 100% |
| Arkanoid, 5000 | 70% | 69% | 100% |

The check itself is `wait_skip`, `find`, `kept` and `m68k_get_reg`:

- once a line, 0.3-0.7% of core 1;
- once a slice (the first build), 1.2%.

**The A1200 survey again.** The same 26 titles were run with item 17's
harness, the same input at the same frames, against a new host pass made
with this build. Speed and core 1 were measured past frame 3000, as
before. Each cell gives before → after:

| Title | Worst | Median | Behind | Core 1 median |
| --- | ---: | ---: | ---: | ---: |
| Black Viper | 57.1 → 99.2% | 62.0 → 99.9% | 64.9 → 0.4 s | 100 → 26% |
| Fatman | 62.1 → 83.4% | 83.5 → 100% | 35.9 → 0.7 s | 100 → 48% |
| Soccer Kid | 76.9 → 95.2% | 92.8 → 99.7% | 16.4 → 0.9 s | 100 → 55% |
| The Lost Prince | 84.4 → 92.8% | 86.0 → 99.9% | 29.3 → 1.1 s | 100 → 97% |
| Starbase 13 | 82.3 → 98.7% | 99.7 → 99.8% | 2.6 → 0.7 s | 96 → 98% |
| Sword | 92.4 → 99.4% | 99.8 → 99.9% | 2.4 → 0.4 s | 86 → 78% |
| Deluxe Galaga | 47.4 → 47.2% | 58.3 → 83.5% | 97.4 → 34.7 s | 100 → 100% |
| Deluxe Pacman | 86.7 → 86.6% | 99.7 → 99.8% | 5.2 → 2.6 s | 84 → 72% |
| Tactical Manager 2 | 61.6 → 64.3% | 65.5 → 65.6% | 68.7 → 47.6 s | 100 → 100% |
| Virtual Karting 2 | 49.6 → 49.5% | 52.8 → 54.2% | 107.3 → 105.6 s | 100 → 100% |
| Leading Lap | 56.7 → 56.7% | 63.0 → 62.3% | 78.2 → 74.5 s | 100 → 100% |
| AmiBee | 53.2 → 52.1% | 96.0 → 68.4% | 33.6 → 39.4 s | 87 → 100% |
| Gea Lux | 62.7 → 63.3% | 89.9 → 85.2% | 30.9 → 29.8 s | 39 → 38% |
| Metal Kombat | 65.6 → 66.7% | 99.6 → 99.6% | 5.9 → 3.1 s | 72 → 50% |
| Zero Gravity | 74.8 → 74.8% | 92.8 → 94.3% | 16.3 → 14.3 s | 100 → 100% |
| Charr | 83.5 → 82.2% | 84.4 → 83.6% | 20.0 → 20.5 s | 100 → 100% |
| Christmas Breakdown | 89.6 → 86.5% | 93.7 → 90.9% | 16.0 → 20.5 s | 100 → 100% |
| Megaball | 93.4 → 90.7% | 99.7 → 99.7% | 2.5 → 3.0 s | 88 → 90% |

The other eight were at 97% or more before and still are: Aerial Racers
(the race 100%, core 1 68 → 70%), Football Glory and Indoors, Maniac
Ball, Lethal Formula, Overdose, Scorched Tanks and Diamond Caves 2.

- **Overall.** The 26 titles fell 638 s behind real time before and 405 s
  after. Core 1 spent 3144 s at 99% or more before and 2041 s after.
- **Fixed by the skip.** Six titles' waits were the cost, and they now
  run at full speed.
  - Black Viper's briefing and race went from ~61% to 100%.
  - Fatman's play went from ~65% to 100%.
  - Soccer Kid went from 84-97% to 100%, and The Lost Prince from 86%.
  - Starbase 13 and Sword lost their dips.
  - Black Viper, Fatman and Soccer Kid have core 1 to spare now.
- **Deluxe Galaga** in play went from 58-63% to 81-98%. Its instructions
  screen (47%) runs 37k instructions a frame without waiting.
- **Unchanged: titles that compute.** Virtual Karting 2, Leading Lap,
  Charr's menu, Gea Lux and AmiBee in play, Metal Kombat's 3D intro and
  Zero Gravity in play were within a point or two, frame for frame.
  - Tactical Manager 2 is the same 65% where the host shows its main
    screen.
  - Its later 98% comes from the board's run reaching a match that the
    host's never does. It is not the skip.
- **Medians that moved without the speed.** The median is taken over 5 s
  windows, so a slow stretch counts more the longer it takes.
  - AmiBee's and Gea Lux's speeds are the same as before, 1000 frames at
    a time.
  - AmiBee's last 1000 frames came at 76% instead of 100%, around its game
    over. That gave the slow stretches the majority of the windows.
- **Christmas Breakdown's play is ~3 points slower:** 91% against 94%,
  from frame 5000 on, in two runs.
  - It computes ~25k instructions a frame there, the same in both builds
    on the host, with almost no loops. The scene is the same.
  - A build that never runs the check, with the layout otherwise the
    same, gave 92-93%.
  - So about half the drop is the check itself: more than its 0.4% of the
    profile, so probably the cache.
  - The other half is the moved data (wait.c's tables went into internal
    RAM before `m68ki_cpu`) or `poll_wait()`'s test.
  - Virtual Karting 2 and Gea Lux, which compute too, came out 1-3 points
    faster.
- **The check's own cost** in the board's profiles (`wait_skip`, `find`,
  `kept`, the peeks, `next_underflow`, `wait_poll_loops`):
  - 0.2-0.8% of core 1 in most titles, including those that compute;
  - 1.6-2.9% where loops are being skipped (Aerial Racers' race, Overdose,
    Black Viper), which have core 1 to spare.
  - `m68k_get_reg` and `poll_wait` (up to 2% in Tactical Manager 2) cost
    the same as before.
- **Pictures.** The board's pictures beside the new host pass show the
  same scenes at the same frames.
  - The differences are of the old survey's kinds: moments a little
    apart; Fatman, Tactical Manager 2 and Deluxe Galaga in other places by
    the end.
  - A few board pictures have a single row that differs from its twin (a
    line is two rows). This is the screenshot catching a line while it
    changes, not the emulation: 19 of 95 pictures before and 19 of 92
    after.
- **What is left is item 19:** the titles that compute 24-56k
  instructions a frame.

## Files

`build/waitloop-2026-11-25/` (not in git) holds:

- the host builds before and after;
- the collection and replay runs;
- SysInfo's screens;
- the A500 runs (`ab500.py`) and the survey rerun (`survey/`: `board/`
  the 26 titles, `board-xmas/` Christmas Breakdown again,
  `board-xmas-off/` with the check never run).
