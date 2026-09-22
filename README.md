# AArch

AArch is a modular multi-system emulator written in C11. One small public
interface, one CLI binary, eleven cores — no external dependencies.

The same source builds for **Windows 7+**, **macOS 10.12+**, **Linux**,
and **Android 6.0+** on x86, x86-64, ARM and AArch64. Cores use fixed-width
integers and byte-wise save-state serialization, so behavior is identical
across host platforms and endianness.

## Status

| Core | System | Status | Notes |
|------|--------|--------|-------|
| mgbx | Game Boy / DMG | working | SM83, MBC1/2/3/5+RTC, dot PPU, PSG, save states |
| beatle-nes-redux | NES | working | 6502, mappers 0/1/2/3/4, dot PPU, APU+DMC, save states |
| supersnes | SNES | partial | 65C816, LoROM/HiROM, DMA+HDMA, PPU modes 0/1/7; S-SMP/DSP stub, PPU modes 2-6 missing |
| mgbax | Game Boy Advance | working | ARM7TDMI+Thumb, HLE BIOS, PPU modes 0-4, DMA, PSG+FIFO audio |
| finalburn | Genesis / Mega Drive | partial | 68000, Z80, VDP mode-4, PSG, YM2612; coarse cycle counts, unaligned accesses aligned silently, RESET is a no-op |
| ms-32 | Sega 32X | partial | SH-2 x2 + adapter (COMM, DREQ, interrupts) + VDP packed-pixel over finalburn; RLE mode, autosprites and PWM audio pending |
| beatle-psx | Sony PlayStation | partial | R3000A + GTE + GPU (1 MiB VRAM, textured/semi-transparent rendering) + DMA + timers, PS-X EXE loader (no BIOS); CD-ROM and SPU not implemented |
| supersaturn | Sega Saturn | partial | SH-2 x2, SCU (direct+indirect DMA, interrupts, timers), VDP1 (sprites/polygons/clipping, bank+LUT+RGB colors), SMPC INTBACK, IP.BIN boot (no BIOS); VDP2 compositing, SCSP sound and CD block are stubs |
| mds-a | Nintendo DS | partial | ARM946E-S + ARM7TDMI interpreters (full ARM + Thumb), direct-boot from the .nds header (no BIOS), dual timers with cascade, IPC sync, per-CPU IRQ model, 2D BG bitmap modes 3/5 scanout (dual-screen 256x384 output); 3D engine, sprite compositing, sound, touch and card bus are stubs |
| m64-b | Nintendo 64 | partial | R4300i (MIPS III) interpreter + CP0/exceptions, PI DMA, SI PIF DMA, VI framebuffer output (16/32 bpp), no-PIF boot (IPL3 from cart runs in DMEM); RSP, AI audio and TLB not implemented |
| supercastpro | Sega Dreamcast | partial | SH-4 interpreter (integer + single-precision FPU subset incl. FIPR/FTRV), TMU with TUNI interrupts, direct-boot from IP.BIN (no BIOS), PVR2 display-controller scanout (RGB565/888/0888); Tile Accelerator, AICA sound, GD-ROM and Maple input are stubs |

**working** — boots, runs, produces audio, save states, suite green.
**partial** — implements a documented subset of the hardware; software
limited to that subset runs, remaining gaps are listed in the table.

## Building

Requires CMake 3.16+ and a C11 compiler.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

### Windows 7+ (MinGW-w64)

```sh
pacman -S mingw-w64-x86_64-toolchain cmake   # MSYS2
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

MSVC 2019+ also works (`-G "Visual Studio 16 2019"`). `_WIN32_WINNT` is
pinned to 0x0601.

### macOS 10.12+

Any C11 toolchain. Override the deployment target with
`-DCMAKE_OSX_DEPLOYMENT_TARGET=10.13` if needed.

### Android via Termux

No root and no NDK needed — the cores build natively on the device:

```sh
pkg install clang cmake make
git clone <this repo> && cd aarch
sh scripts/build-termux.sh            # builds and runs the test suite
sh scripts/build-termux.sh --install  # installs `aarch` to $PREFIX/bin
```

### Android 6.0+ (NDK)

```sh
cmake -S . -B build-android \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-23 \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-android -j
```

All ABIs (arm64-v8a, armeabi-v7a, x86, x86_64) build from the same source.
The deliverable is a plain CLI executable.

## Using the CLI

```sh
aarch --list                                       # cores + honest status
aarch --rom game.gb --frames 600 --fb-out f.ppm    # core auto-detected
aarch --core ms-32 --rom game.32x --benchmark
aarch --core mgbx --rom game.gb --state-out sav.bin
```

Without `--core` the ROM is identified by signature probing. Exit codes:
0 ok, 1 runtime failure, 2 usage/IO error, 3 not implemented, 4 smoke
failure.

`--smoke` runs in-process checks: two identical runs must produce identical
framebuffer CRCs, and a save-state roundtrip must resume bit-exactly.

## Interface

`include/emu/emu.h` is the only header a frontend needs: one opaque core
handle with `create / destroy / load_rom / reset / run_frame / framebuffer /
set_input / set_audio_callback / state_size / save_state / load_state`.
Video is XRGB8888; audio is int16 stereo PCM via callback. Save states are
explicit little-endian serialization — never raw pointers.
`emu_core_registry()` reports every core with an honest status,
`emu_rom_probe()` sniffs ROM signatures.

## Tests

`tests/` is organized per system plus common suites (API lifecycle, state
contract, ROM detection, registry). Expected values derive from
hardware specifications — hand-assembled opcodes and datasheet formulas,
never emulator internals. Current status: **410 tests, 0 failed
assertions**, clean under ASan+UBSan and `-Werror`.

## License

See the repository settings for licensing; contributed code is expected to
be original and dependency-free.
