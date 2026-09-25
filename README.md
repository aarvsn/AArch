# AArch

AArch is a modular multi-system emulator written in **C11**.

It provides one small public API, one CLI, and multiple console cores with **no external runtime dependencies**.

Supports:

- Windows 7+
- macOS 10.12+
- Linux
- Android 6.0+
- x86, x86-64, ARM and ARM64

## Cores

| Core | System | Status |
|---|---|---|
| `mgbx` | Game Boy | Working |
| `beatle-nes-redux` | NES | Working |
| `supersnes` | SNES | Partial |
| `mgbax` | Game Boy Advance | Working |
| `finalburn` | Genesis / Mega Drive | Partial |
| `ms-32` | Sega 32X | Partial |
| `beatle-psx` | PlayStation | Partial |
| `supersaturn` | Sega Saturn | Partial |
| `mds-a` | Nintendo DS | Partial |
| `m64-b` | Nintendo 64 | Partial |
| `supercastpro` | Dreamcast | Partial |

**Working** means the core can run supported software, produce audio, and pass its tests.

**Partial** means the core implements a documented subset of the hardware.

## Building

Requires **CMake 3.16+** and a **C11 compiler**.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```
## Android / Termux
```sh
pkg install clang cmake make
sh scripts/build-termux.sh
```
No root or NDK is required for Termux builds.

## Android NDK
```sh
cmake -S . -B build-android \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-23 \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build-android -j
```
## CLI
```
aarch --list
aarch --rom game.gb --frames 600 --fb-out frame.ppm
aarch --core ms-32 --rom game.32x --benchmark
aarch --core mgbx --rom game.gb --state-out save.bin
```
ROMs can be detected automatically, or a core can be selected with --core.

## Exit codes:

- 0 — success
- 1 — runtime error
- 2 — usage / I/O error
- 3 — not implemented
- 4 — smoke test failure

## API

### Frontends only need:

``include/emu/emu.h``

### The API provides:

- Core creation and destruction
- ROM loading
- Reset and frame execution
- Framebuffer access
- Input
- Audio callbacks
- Save/load states

## Notes

- Video uses XRGB8888 and audio uses 16-bit stereo PCM.
- Save states use explicit serialization and do not store raw pointers.

## License

See the repository license settings.
Contributions should be original and dependency-free.
