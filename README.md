# emu-fw — lightweight multi-system emulator framework

A modular, dependency-free multi-system emulator framework written in C11.
Four independent cores plug into one small public interface:

| Core                | System                    | Status |
|---------------------|---------------------------|--------|
| `mgbx`              | Game Boy / DMG            | CPU (SM83, full official set), MBC1/2/3/5 + RTC, scanline PPU, 4-channel PSG, timer, OAM DMA, save states |
| `beatle-nes-redux`  | NES / Famicom             | 6502 (full official set), mappers 0/1/2/3/4, dot-timed PPU (loopy v/t/x, sprite 0 hit, MMC3 A12 IRQs), APU with DMC (no CPU stall), save states |
| `supersnes`         | SNES (LoROM/HiROM)        | 65C816 (full official set, emulation/native, decimal mode), PPU modes 0/1/7 + sprites, GP DMA, hardware multiply/divide, save states |
| `mgbax`             | Game Boy Advance          | ARM7TDMI ARM + Thumb (full official sets), HLE BIOS (no BIOS ROM bundled: SWI services + IRQ dispatcher), modes 0-4 text/affine/bitmap PPU + sprites, 4-channel DMA, timers, PSG (FIFO audio unimplemented), save states |

## Building

Requires CMake >= 3.16 and a C11 compiler.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure   # runs the test suite
```

Options (all default ON unless noted):

- `EMU_BUILD_MGBX` / `EMU_BUILD_BEATLE_NES_REDUX` / `EMU_BUILD_SUPERSNES` / `EMU_BUILD_MGBAX` — build individual cores; a minimal build enables just one
- `EMU_BUILD_TESTS` — build the `emu-tests` runner
- `EMU_BUILD_CLI` — build `emu-cli`
- `EMU_WERROR` — treat warnings as errors (used by CI)
- `EMU_SANITIZE_ADDRESS` / `EMU_SANITIZE_UNDEFINED` — ASan / UBSan builds

Warnings are fixed at their source; the tree builds clean under
`-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Werror`.

## Command line tool

```sh
emu-cli --core mgbx --rom game.gb --frames 600 --fb-out frame.ppm
emu-cli --core mgbax --rom game.gba --frames 600 --benchmark
emu-cli --core beatle-nes-redux --rom game.nes --frames 60 --smoke
emu-cli --list
```

`--smoke` runs in-process checks: two identical runs must produce identical
framebuffer CRCs, and a save-state roundtrip must resume bit-exactly.

## Public interface

`include/emu/emu.h` is the only header a frontend needs: one opaque core
handle with `create/destroy/load_rom/reset/run_frame/framebuffer/set_input/
set_audio_callback/state_size/save_state/load_state`. Video is XRGB8888
(GB 160x144, NES 256x240, SNES 256x224, GBA 240x160); audio is int16 stereo
PCM pushed through a user callback at a fixed per-core rate.

Save states use explicit serialization (little-endian byte-wise, never
raw pointers), so blobs are host-independent. `state_size()` always equals
the size actually written; undersized buffers return `EMU_ENOSPACE`
without writing.

## Tests

`tests/` contains one runner with suites per core plus common API/contract
suites (235 tests). Every expected value is derived from the system
specification or hand-assembled instruction encodings — never from the
emulator's own helper logic. The suite runs clean under ASan and UBSan.

## Accuracy notes and known limitations

Reported honestly, per core:

- **mgbx**: PPU is scanline-based (no mid-scanline effects, no window
  internal timing details); APU lacks frame-counter-driven length clocking
  subtleties; MBC3 RTC is deterministic (advances with emulation time).
- **beatle-nes-redux**: DMC does not steal CPU cycles; sprite evaluation
  is simplified; PPU open bus and decay are not modeled.
- **supersnes**: S-SMP/DSP audio is a documented stub (silent); no HDMA;
  PPU modes 2-6, color math, windows and mosaic are unimplemented; cycle
  counts are documented approximations.
- **mgbax**: FIFO channel audio (direct sound) unimplemented; timing is
  instruction-level with flat per-access cycle costs, not bus-cycle
  accurate; no BIOS ROM is bundled (HLE BIOS layer instead); premultiplied
  OBJ priority simplification (OBJ-over-BG handled OBJ-first).

## Performance

Measured with `emu-cli --benchmark` (Release, synthetic NOP-loop ROMs,
600 frames, x86_64 Linux, 2 CPUs). Real games will differ; treat these as
relative indicators only:

| Core  | fps   | vs 60 Hz realtime |
|-------|-------|-------------------|
| mgbx  | ~3400 | ~57x |
| NES   | ~2500 | ~42x |
| SNES  | ~790  | ~13x |
| GBA   | ~300  | ~5x  |

## Repository layout

```
include/emu/emu.h       public core interface
src/common/             CRC32, endian helpers, state writer/reader
src/mgbx/               Game Boy core
src/beatle-nes-redux/   NES core
src/supersnes/          SNES core
src/mgbax/              GBA core
tools/emu-cli/          command line frontend
tests/                  suite runner + per-core suites + common suites
worklog.md              engineering work log (findings, fixes, results)
```
