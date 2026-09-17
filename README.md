# AArch — multi-system CLI emulator

AArch is a modular, dependency-free multi-system emulator written in C11.
The same CLI binary builds for **Windows 7+**, **macOS 10.12+**, **Linux**,
and **Android 6.0+ (API 23)** on x86, x86-64, ARM and AArch64 — all cores are
portable C with fixed-width integers, no floating point in core code, and
host-endian-independent save states.

## Cores

| Core | System | Status | Notes |
|------|--------|--------|-------|
| `mgbx` | Game Boy / DMG | working | SM83 CPU (full official set), MBC1/2/3/5 + RTC, scanline PPU, 4-channel PSG, timer, OAM DMA, save states |
| `beatle-nes-redux` | NES / Famicom | working | 6502 (full official set), mappers 0/1/2/3/4, dot-timed PPU (loopy v/t/x, sprite 0 hit, MMC3 A12 IRQs), APU with DMC (no CPU stall), save states |
| `supersnes` | SNES | partial | 65C816 (full official set, emulation/native, decimal mode), LoROM/HiROM, PPU modes 0/1/7 + sprites, GP DMA + HDMA (direct/indirect, repeat blocks), hardware multiply/divide, save states. **Gaps:** S-SMP/DSP audio is a stub (ports answer fixed values, silent output), PPU modes 2-6, color math, windows, mosaic unimplemented |
| `mgbax` | Game Boy Advance | working | ARM7TDMI ARM + Thumb (full official sets), HLE BIOS (no BIOS ROM bundled: SWI services + IRQ dispatcher), modes 0-4 text/affine/bitmap PPU + sprites, 4-channel DMA, timers, PSG + direct-sound FIFO audio, save states |
| `finalburn` | Genesis / Mega Drive | partial | 68000 (broad subset), Z80 (full documented set: base/CB/ED/DD/FD, IM0/1/2), VDP mode-4 (planes A/B/window, sprites, H/V scroll, DMA, V-int/H-int), PSG SN76489, YM2612 (6x4-op FM, 8 algorithms, DAC, timers), SRAM, save states. See the subset notes below |
| `beatle-psx` | PlayStation | skeleton | ROM/disc detection only (`PS-X EXE`, ISO9660 `PLAYSTATION`). CPU/GPU/SPU not implemented; `run_frame` returns `EMU_ENOTIMPL` |
| `mds-a` | Nintendo DS | skeleton | NDS header validation (Nintendo logo at 0x160) only. Dual-ARM not implemented |
| `ms-32` | Sega 32X | skeleton | `SEGA 32X` header validation only. SH-2 x2 + Genesis adapter not implemented |
| `supersaturn` | Sega Saturn | skeleton | IP.BIN (`SEGA SEGASATURN`) detection only. SH-2 x2 / VDP1/VDP2 / SCU not implemented |
| `m64-b` | Nintendo 64 | skeleton | z64/v64/n64 byte-order detection only. VR4300 / RCP not implemented |
| `supercastpro` | Dreamcast | skeleton | IP.BIN (`SEGA SEGAKATANA`) detection only. SH-4 / PowerVR2 / AICA not implemented |

Status definitions: **working** = boots, runs, produces audio, save states,
suite green. **partial** = runs with the documented gaps above. **skeleton**
= format detection and validation only; it refuses to emulate rather than
pretending. Skeleton cores exist so the CLI can identify and validate media
for systems whose emulation has not been written yet.

## finalburn (Genesis) subset notes

The 68000 core implements the full official instruction set except:
BCD forms are implemented (ABCD/SBCD/NBCD), trace/exceptions (illegal,
line-A/F, div-by-zero, CHK, TRAPV, privilege, TRAP, IPL 1-7, STOP wake) are
implemented. Cycle counts are coarse (base + per-access penalty; MUL/DIV
formulas approximate the manual's ranges). Unaligned word/long accesses are
silently aligned instead of raising an address error.

VDP approximations: the FIFO is modeled as always empty (writes never
stall), DMA timing is a fixed cycles-per-word rate (8 in vblank, 16 on
display), H-scroll cell mode falls back to per-line, shadow/highlight and
interlace are not implemented, sprite pixel limits are first-come.

YM2612 approximations: LFO and SSG-EG are ignored, CH3 special mode runs at
its normal frequency, the dB envelope curve uses an integer 0.75 dB/unit
chain, timer rates are coarse. The tuning anchor is the documented
FNUM=1024 / OCT=4 / MULT=1 = 440 Hz (phase increment 8659 at master/144).

PSG: the volume-envelope mode (attenuation bit 4) is not implemented —
Genesis titles write explicit volumes.

## Building

Requires CMake >= 3.16 and a C11 compiler.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure   # runs the test suite
```

### Windows 7+ (MinGW-w64)

```sh
pacman -S mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake   # MSYS2
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```
`_WIN32_WINNT` is pinned to 0x0601 (Windows 7). MSVC 2019 or newer also
works (`cmake -G "Visual Studio 16 2019"`); warning flags adapt to `/W4`.

### macOS 10.12+

Any C11 toolchain works. The deployment target defaults to 10.12; override
with `-DCMAKE_OSX_DEPLOYMENT_TARGET=10.13` if needed.

### Linux / ARM Linux

Standard build as above. There are no architecture-specific code paths:
cores use fixed-width integers and byte-wise serialization only, so the
same source builds for aarch64, armv7, riscv64 etc.

### Android 6.0+ (NDK)

```sh
cmake -S . -B build-android \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-23 \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-android -j
# deploy: adb push build-android/aarch /data/local/tmp/
# run:   adb shell /data/local/tmp/aarch --list
```
API 23 is the floor (Android 6.0); all ABIs (arm64-v8a, armeabi-v7a, x86,
x86_64) build from the same source. The binary is a plain executable — run
it from a terminal (adb shell or a terminal emulator app). There is no
Gradle project by design; the deliverable is the CLI binary.

## Command line tool

```sh
aarch --list                                      # cores + honest status
aarch --rom game.gb --frames 600 --fb-out f.ppm   # core auto-detected
aarch --core finalburn --rom game.md --benchmark
aarch --core beatle-nes-redux --rom game.nes --smoke
aarch --core mgbx --rom game.gb --state-out sav.bin
```

Without `--core` the ROM is identified by signature probing (iNES header,
Genesis/32X header strings, NDS/GBA/GB Nintendo logo, N64 byte order,
`SEGA SEGASATURN` / `SEGA SEGAKATANA` / `PS-X EXE` / ISO9660). `--core`
always overrides the probe. Running a skeleton core prints an honest
"not implemented" error and exits with status 3.

`--smoke` runs in-process checks: two identical runs must produce identical
framebuffer CRCs, and a save-state roundtrip must resume bit-exactly.

Exit codes: 0 ok, 1 runtime failure, 2 usage/IO error, 3 not implemented,
4 smoke failure.

## Public interface

`include/emu/emu.h` is the only header a frontend needs: one opaque core
handle with `create/destroy/load_rom/reset/run_frame/framebuffer/set_input/
set_audio_callback/state_size/save_state/load_state`. Video is XRGB8888
(fixed sizes per system); audio is int16 stereo PCM pushed via callback
when `run_frame` completes. `emu_core_registry()` lists every known core
with an honest `emu_core_status_t`, and `emu_rom_probe()` sniffs ROM
signatures. Save states are explicit little-endian byte serialization —
never raw pointers or host-endian structs.

## Tests

`tests/` is organized per system (`tests/mgbx/`, `tests/nes/`,
`tests/supersnes/`, `tests/mgbax/`, `tests/finalburn/`) plus common suites
(API lifecycle, state contract, ROM detection, skeleton contracts).
Expected values are derived from the hardware specifications
(hand-assembled opcodes, datasheet formulas), never from emulator
internals. Every bug fixed during development has a regression test that
failed first. Current status: **317 tests, 0 failed assertions**, clean
under ASan+UBSan and `-Werror`.

## CI

`ci/github-ci.yml` defines the CI matrix: Linux x64 (release / Werror /
sanitizers), Linux ARM64, Windows (MSVC), macOS, and Android NDK compile
checks (arm64-v8a, armeabi-v7a, x86_64 at API 23). The file lives at
`ci/` rather than `.github/workflows/` because the repository push token
lacked the GitHub `workflow` scope; copy it to
`.github/workflows/ci.yml` to activate.

## Performance

Measured with `aarch --benchmark`, Release build, synthetic self-running
ROMs, 600 frames, x86-64 Linux (2 CPUs). These are throughput indicators
for the CPU-side emulation, not accuracy claims:

| Core | fps |
|------|-----|
| mgbx | ~3400 |
| beatle-nes-redux | ~2200 |
| supersnes | ~770 |
| mgbax | ~280 |
| finalburn | ~3150 |
