# emu-fw worklog

Project: emu-fw — lightweight, modular, multi-system emulator framework.
Repository started empty (fresh restart). All code in this pass written from scratch.

## Starting repository state (2026-09-16)

- Repository root: /home/z/my-project
- Contents at start: system placeholders only (download/README.md, .env, skills/, upload/, .git)
- No source code, no build system, no tests, no prior worklog. Nothing to recover or preserve.
- Toolchain found: gcc 14.2.0, g++ 14.2.0, GNU Make 4.4.1, git. cmake NOT present initially
  (installed CMake 4.4.3 via pip into the active virtualenv). No clang, no ARM cross-compiler,
  no multilib (32-bit builds not possible in this environment — static portability analysis only).
- Environment: x86_64 Linux 5.10, 2 CPUs, 4 GB RAM.

## Design decisions (established before implementation)

- Language: C11, no external dependencies. Cores in plain C.
- Public interface: include/emu/emu.h — one small core vtable
  (create/destroy/load_rom/reset/run_frame/framebuffer/set_input/set_audio_callback/
  state_size/save_state/load_state). Each core embeds emu_core_t as its first member.
- Video: XRGB8888 framebuffers, fixed sizes per system.
  GB 160x144, NES 256x240, SNES 256x224, GBA 240x160.
- Audio: cores push int16 stereo PCM via a user callback; fixed output rate per core.
- Save states: explicit serialization via common buffer writer/reader helpers
  (src/common/state.c). Never serialize pointers. Size path and save path share one
  serializer, so reported size always equals serialized size. Undersized buffers
  return EMU_ENOSPACE without writing.
- Cores are independent; the common layer contains no system-specific emulation logic.
- Determinism: integer-only math, no uninitialized reads, no host-endian state format
  (multi-byte fields serialized little-endian byte-wise), no UB-as-optimization.

## Baseline build/tests

- Baseline before this pass: no build system existed; nothing to measure.

---
Task ID: 1
Agent: main (Super Z)
Task: Repository inspection, toolchain baseline, worklog initialization

Work Log:
- Inspected /home/z/my-project: only system placeholders; confirmed fresh restart.
- Checked toolchain: gcc 14.2/make/git present; cmake absent; installed cmake 4.4.3 via pip.
- Confirmed no clang, no arm-none-eabi toolchain, no multilib (records for portability step).
- Initialized worklog.md with baseline facts and design decisions.

Stage Summary:
- Real baseline: empty repository. All code to be created from scratch.
- Toolchain: gcc 14.2 + CMake 4.4.3; sanitizer-capable (gcc -fsanitize); no ARM cross-compiler.

---
Task ID: 2
Agent: main (Super Z)
Task: Build system + mgbx Game Boy core + tests

Work Log:
- Created CMake build (per-core options, warnings, sanitizer flags), include/emu/emu.h public API,
  common util (CRC32, state serialization helpers), tiny test framework with suite registry.
- Implemented mgbx: SM83 CPU (full official opcode set, flags, DAA, halt bug, EI delay, interrupt
  dispatch), bus, MBC1/2/3/5 mappers + deterministic RTC, scanline PPU with dot timing, timer
  (edge-based with DIV-write quirk), joypad, 4-channel PSG APU, OAM DMA with CPU stall,
  explicit save states.
- Wrote 74 tests (CPU/cart/mem/PPU/timer/APU/state + common lifecycle/state contract).

Bugs discovered and fixed during test bring-up:
- mgbx/cart.c: gb_cart_load never allocated cart RAM (writes silently dropped) -> allocate + zero.
- mgbx/cart.c: RTC reads returned latched values before any latch -> return live until first latch.
- mgbx/cpu.c: DAA in subtract mode ADDED the adjustment -> subtract.
- mgbx/mem.c: WRAM echo addressing used addr-0xC000 (OOB for E000-FDFF) -> addr & 0x1FFF.
- mgbx/cpu.c: OAM DMA passed raw index to OAM write (0xFE00-based) -> segfault -> add base.
- mgbx/timer.c: TIMA tick bits off by one (half frequency) -> bits 7/1/3/5.
- mgbx/timer.c: TMA reload happened in the same M-cycle as overflow -> delay one M-cycle.
- tests/testutil.c: GB header fields written at absolute offsets from hdr pointer.

Stage Summary:
- mgbx milestone complete: build clean, 74/74 tests pass, emu-cli smoke checks pass.
- Test count baseline established: 74 tests total.
