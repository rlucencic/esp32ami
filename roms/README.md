# Kickstart ROM images

Put images of the Kickstart ROMs here (they are copyrighted and not part of
this repository; Amiga Forever's and FS-UAE's sets have them). The file
names are the ones in those sets:

| File | Kickstart | Size | Flash partition |
|---|---|---|---|
| `amiga-os-120.rom` | 1.2 (33.180): A500, A2000 | 256 KB | `kick_120` |
| `amiga-os-130.rom` | 1.3 (34.5): A500, A2000 | 256 KB | `kick_130` |
| `amiga-os-204.rom` | 2.04 (37.175): A500+ | 512 KB | `kick_204` |
| `amiga-os-205-a600.rom` | 2.05 (37.350): A600 | 512 KB | `kick_205_a600` |
| `amiga-os-310-a600.rom` | 3.1 (40.63): A500, A600, A2000 | 512 KB | `kick_310_a600` |
| `amiga-os-300-a1200.rom` | 3.0 (39.106): A1200 | 512 KB | `kick_300_a1200` |
| `amiga-os-310-a1200.rom` | 3.1 (40.68): A1200 | 512 KB | `kick_310_a1200` |
| `aros-20250422.rom`, `aros-20250422-ext.rom` | AROS (free; the setup screen's Kickstart "AROS", with its extension ROM at $E00000) | 512 KB each | `kick_aros`, `kick_aros_ext` |

`idf.py flash` writes them to their partitions. Without flashing, the
emulator also finds them in a `roms` folder on the SD card, under any file
name (by the version in them). Amiga Forever's encrypted images (with
`rom.key`) need decrypting first.
