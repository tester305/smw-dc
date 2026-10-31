# SNES Mario (Super Mario World) for the Sega Dreamcast

Same idea as smb-dc, one console generation up. Instead of a 6502 core running the original code, this runs the
**snesrev/smw C reimplementation** of the game logic natively on the SH-4. Only the PPU renderer and the audio DSP
(both from LakeSnes, optimised upstream) sit underneath it, so there is no 65816 or SPC700 interpreter.

The SDL front end of upstream (`main.c`, `config.c`, `opengl.c`) is replaced by a KallistiOS one in `dc/`:

| file | what it does |
|---|---|
| `dc/dc_platform.c/.h` | KOS only: progressive 320x240 RGB565 framebuffer output, AICA stream audio at 32 kHz stereo, maple pads |
| `dc/dc_game.c` | game glue: asset loading, PPU/audio setup, frame loop with automatic frame skip |
| `dc/patch_smw.py` | makes upstream safe for the SH-4 (misaligned 16/32-bit RAM accesses raise address errors there) |
| `build.sh` / `Makefile` | fetches upstream at a pinned commit, patches it, extracts assets from your ROM, builds `smw_dc.elf` |

## Build

1. Put your **USA Super Mario World ROM, unheadered (512 KB)** next to `build.sh` as `smw.sfc`.
2. `. /opt/toolchains/dc/kos/environ.sh` then `./build.sh smw.sfc` -> `smw_dc.elf`
3. Run in Flycast, or `make run`, or `mkdcdisc -e smw_dc.elf -o smw.cdi`.

Needs python3 (stdlib only). The ROM is only read by `assets/restool.py` to produce `smw_assets.dat`, which goes into the romdisk.

### GitHub Actions

A plain secret is capped at 48 KB and the ROM is 512 KB, so the SMB trick (gzip+base64 secret) does not work here.
Use a **private** repo and either:

* commit `smw.sfc` directly, or
* commit `smw.sfc.gpg` (`gpg --symmetric --cipher-algo AES256 smw.sfc`) and add the passphrase as the `SMW_ROM_PASSPHRASE` secret.

The ELF/CDI artifacts contain the extracted game data, so keep the repo private. A new repo has an empty cache, so the
first run rebuilds the SH-4 toolchain (long); later runs reuse it.

## Controls

D-pad / stick = move, A = B (jump), B = A (spin jump), X = Y (run / fireball), Y = X, L / R triggers = L / R,
**both triggers together = Select**, Start = Start, A+B+X+Y+Start = quit to BIOS. A second controller is player 2.

Hold-and-press **A+B+X+Y** (without Start) toggles a timing overlay: logic ms x10, draw ms x10, skipped frames.

## Status / known risks

* Compiles for SH-4 and the KOS API calls match current KallistiOS headers, but it has **not been run on hardware or Flycast yet**.
* Speed is unmeasured. If a frame does not fit in ~16 ms the next picture is skipped so game speed holds at 60 logic frames/s.
* The unaligned-access patch covers every `*(uint16 *)` / `*(uint32 *)` cast and `WORD()/DWORD()`. A misaligned pointer passed
  into a function taking a plain `uint16 *` would still fault; KOS prints the PC, and `sh-elf-addr2line` finds the line.
* Cartridge SRAM is stored as a BIOS-visible VMU package at `/vmu/a1/SMW.SRM` (controller port 1, VMU slot 1). The VMU must be formatted and have enough free blocks. If no save exists, the game starts fresh.
* Player 2 is read from the second Dreamcast controller and feeds SMW's native second joypad input. As in the original game, two-player mode alternates turns; it is not simultaneous co-op.
* Upstream also contains the All-Stars SMB1 / Lost Levels reimplementations (not built here).
