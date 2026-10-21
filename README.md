<h1 align="center">esp32ami</h1>

<p align="center">
  <b>An Amiga 500, 2000 or 600 on an M5Stack Tab5.</b><br>
  A 68000 at 7.09 MHz with the OCS/ECS chipset (Agnus, Denise, Paula, two 8520 CIAs), Kickstart 1.3, 2.04, 2.05 or 3.1, on the ESP32-P4.
</p>

<p align="center">
  <b>Early work in progress:</b> Workbench boots and can be used with a keyboard and mouse; games
  are the next step. The plan is in <a href="docs/architecture.md">docs/architecture.md</a>.
</p>

## What works

- Kickstart 1.3, 2.04, 2.05 and 3.1, at full speed on the Tab5; Workbench 1.3, 2.05 and 3.1 boot.
- Two floppy drives (DF0:, DF1:) for ADF images on the SD card, read and written (at a real
  drive's speed), chosen on the setup screen or the web page.
- The battery-backed clock, on every model (Kickstart 1.3's SetClock shows years after 1999 too).
- Bluetooth or USB keyboard and mouse, the touch screen as a touchpad, a joystick on the cursor
  keys (F12); the mouse pointer (sprites); Paula's four sound channels through the Tab5's speaker.
- Amiga Test Kit's CIA, memory, keyboard, controller, audio, clock and floppy tests pass.
- The memory map (chip RAM up to 2 MB, the A500's slow RAM, the ROM overlay), the CIAs,
  Paula's interrupts, the copper, the blitter (area and line modes, fill), Denise's bitplanes
  (lores and hires, dual playfield, extra half-brite, HAM).
- The setup screen: model (A500, A2000, A600), Kickstart, chip RAM, slow RAM, PAL or NTSC.

Next: games (chip RAM timing, blitter timing, copper precision, collisions), then the A2000's and
A600's own hardware and hard disks (see the phases in the architecture document).

## Building

ESP-IDF v5.5.4, with [esp32emu](https://github.com/rlucencic/esp32emu) (the shared board
runtime and the 68000 core) cloned next to this project. The Tab5 only:

```sh
idf.py -B build-tab5 -DIDF_TARGET=esp32p4 -DSDKCONFIG=$PWD/build-tab5/sdkconfig build
idf.py -B build-tab5 -DIDF_TARGET=esp32p4 -DSDKCONFIG=$PWD/build-tab5/sdkconfig -p PORT flash
```

Kickstart ROMs are copyrighted and not included: put your own in `roms/` (see
[roms/README.md](roms/README.md)) before `flash`, or in a `roms` folder on the SD card.

A desktop build of the emulator core runs on a Mac or Linux for testing:
`make -C components/amiga/host run KICK=roms/amiga-os-130.rom`.

## Licence

MIT (see [LICENSE](LICENSE)). The chipset emulation is written for this project; Musashi
(the 68000 core, in esp32emu) is MIT too.
