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

---
Task ID: 3
Agent: main (Super Z)
Task: beatle-nes-redux NES core + tests

Work Log:
- Implemented 6502 (2A03) CPU: full official set, standard cycle tables with
  page-cross penalties, interrupt model at instruction granularity.
- Bus with RAM mirroring, PPU registers, OAM DMA (513-cycle stall), controller
  shift register, iNES parser, mappers 0/1/2/3/4, dot-timed PPU (loopy v/t/x/w,
  VBlank/NMI edges, BG fetch walk driving MMC3 A12 edges, simplified sprite
  evaluation, sprite 0 hit), APU (2 pulse, triangle, noise, DMC without CPU
  stall, frame sequencer), explicit save states.
- 51 NES test assertions + common suite; total 125 tests passing.

Bugs discovered and fixed during test bring-up (emulator side):
- cpu.c: zero-page addressing helpers returned the operand's ADDRESS instead
  of reading the operand byte (a_zp/a_zpx/a_zpy); a_abs/a_absx/a_absy read
  only one operand byte and used the address as the low byte. All fixed.
- cart.c: NROM 16K images did not mirror at $C000; MMC3 odd-address register
  writes ($C001/$E001) were ignored by an over-broad even-address gate;
  MMC3 IRQ reload now takes effect on the next A12 edge.
- apu.c: frame sequencer treated absolute cycle targets as relative (events
  fired at wrong rates); 5-step quarter/half frame assignment corrected.
- nes.c: PRG RAM content was read in load_state but never written by
  serialize (state blob corruption on load).

Stage Summary:
- NES milestone complete: build clean, all 125 tests pass, emu-cli smoke ok.

---
Task ID: 4
Agent: main (Super Z)
Task: supersnes SNES core + tests

Work Log:
- Implemented 65C816 CPU: full official set, emulation/native modes (XCE),
  8/16-bit M/X widths, direct page with emulation high-byte forcing, long
  addressing, decimal ADC/SBC, MVN/MVP, WAI/STP, COP/BRK vectors, PEA/PEI/PER,
  TRB/TSB, stack emulation quirks. Cycle counts are documented approximations.
- Memory map (WRAM mirrors, PPU/APU/CPU-I/O regions), LoROM/HiROM cart with
  score-based header detection, general-purpose DMA (modes 0-3), hardware
  multiply/divide, controller auto-read, PPU modes 0/1/7 + sprites with
  per-line rendering, explicit save states.
- S-SMP/DSP audio = documented stub (ports answer fixed values; silent).

Bugs discovered and fixed during test bring-up (emulator side):
- snes.c: power_on re-initialized the cartridge after load, wiping the ROM.
- cpu.c: ALU group decode used (op>>5)&7 but the dispatch switched on
  {0,2,4,6,8,A,C,E} - LDA/CMP/STA groups never matched. Fixed to 0..7.
- cpu.c: (dp) opcode set initially wrong ($22/$42/... collide with COP/JSL);
  correct set is $12/$32/$52/$72/$92/$B2/$D2/$F2.
- dma.c: B-bus writes used bank $21 instead of bank 0 ($210018 vs $002118).
- ppu.c: rendered the line AFTER incrementing the line counter (off-by-one);
  mode 7 background disabled by max_bg=0; VRAM write latch model corrected
  to VMAIN bit7 semantics ($2118 latch / $2119 commit).

Stage Summary:
- SNES milestone complete: 156 tests total passing, smoke checks pass.
- Known limitations documented: S-SMP/DSP stub, no HDMA, PPU modes 2-6,
  color math, windows, mosaic unimplemented.

---
Task ID: 5
Agent: main (Super Z)
Task: mgbax GBA core completion + full test suite bring-up

Work Log:
- Baseline verification: repo contained substantial uncommitted GBA work that
  had NEVER been built or run (worklog had no GBA entry; tests/mgbax/*.c were
  empty stubs; the test binary did not link - t_register_mgbax undefined).
- Fixed build blockers: timer.c missing <string.h> + sign-compare; swi.c
  wrong function name (irq_dispatch_return), missing <stdlib.h>, dead code.
- Core bugs found and fixed (each verified by a regression test):
  * cart.c: ROM reads used the full 32-bit address without stripping the
    region base - the CPU fetched open-bus zeros and could never boot. Now
    masks the 32 MiB window (addr & 0x01FFFFFF).
  * cpu.c: gba_cpu_step pre-advances PC, but EVERY handler advanced PC
    again at its end - every instruction double-advanced and skipped its
    successor. Removed ~40 handler-end bumps; branches assign PC directly.
  * cpu.c: Thumb format 2 (add/sub), format 4 (ALU) and format 6/7
    (register-offset load/store) had Rd and Rm fields swapped.
  * cpu.c: Thumb 011-group decode used (instr>>11)&3 (bits 12-11) instead
    of (instr>>10)&3 (L=bit11, B=bit10) - LDR-word executed as STRB,
    STRB as LDR; offset scaling now keyed off the B bit (byte: imm,
    word: imm*4).
  * cpu.c: hi-reg/BX guard tested bit11; format 5 is 0x4400-0x47FF
    (bits[15:10] = 010001). All hi-reg ops and BX were decoding as NOPs.
  * cpu.c: ARM operand2 PC reads were 4 too low (immediate shift reads
    instr+8, register-specified shift reads instr+12); LDR/STR Rn=PC base
    was instr+4 instead of instr+8; LDM DA first-address off by one word;
    LDM{PC} no longer interworks (ARMv4T); SBC borrow compare now 64-bit
    (b+borrow wraps when b=0xFFFFFFFF); sub_with_carry edge case fixed.
  * cpu.c: BL second halfword LR was 2 over (LR = pair address + 4).
  * swi.c/gba.c: HLE IRQ dispatcher did not save the interrupted PC or
    CPSR - the "return" computed PC from the game's stale LR. Rewritten to
    the hardware contract: switch to IRQ mode, CPSR->SPSR_irq, push
    {r0-r3,r12,lr=next+4} on the IRQ stack, BIOS return sentinel, restore
    CPSR from SPSR on return. Power-on now initializes BIOS-equivalent
    stacks (SP_irq=$03007FA0, SP_svc=$03007FE0, SP_sys=$03007F00).
  * cpu.c: IntrWait/Halt wake conditions split (IntrWait wakes only on the
    requested IF flags; Halt on any enabled IRQ).
  * dma.c: channel decode used (addr>>4)&3 which is wrong for the 12-byte
    register stride - ch0 decoded as ch3 and all register offsets were
    garbage. Now ((addr-0x040000B0)/12)&3. IO dispatch window corrected to
    [0xB0,0xE0) (was [0xB8,0xE0)). DST_RELOAD now restores a latched DAD
    (dad_latch added to state + serialization).
  * apu.c: frequency timer advanced once per CPU step call instead of per
    cycle; register interface used wrong addresses (0x72/0x7C are sweep/
    noise regs; freq/restart live at 0x64/0x6C, envelope/volume at
    0x60/0x68/0x78); restart now loads volume/duty phase.
  * ppu.c: sprite tile fetch masked tile_off with 0xFFFF, truncating the
    OBJ region at VRAM 0x10000; now uses the 0x1FFFF mask with the
    0x18000 mirror. Affine BG limit comparison sign fixed.
  * mem.c: gba_bus_read32 performed the unaligned rotation itself AND the
    CPU rotated again; the bus now reads word-aligned and only the CPU
    applies the LDR rotation.
- Common: ALL FOUR cores' save_state wrote partial bytes before flagging
  overflow, violating the "nothing written on EMU_ENOSPACE" contract.
  All four now pre-check capacity against state_size() (regression-tested
  for mgbax; mgbx/nes/snes tests still pass).
- Tests: rewrote tests/mgbax/arm.c (the old file had never executed and
  contained many wrong encodings) and wrote thumb.c, mem.c, timer.c,
  dma.c, ppu.c, state.c + the t_register_mgbax aggregator. Every encoding
  hand-assembled from the ARM ARM; expectations derived from the spec.
- Result: 234 tests total, 0 failed assertions.

Stage Summary:
- mgbax milestone complete: builds clean, all suites green, 234 tests.
- 15+ core defects fixed; each has a regression test that failed first.

---
Task ID: 6
Agent: main (Super Z)
Task: Audits (UB/memory/warnings), portability, performance, docs, CI, final verification

Work Log:
- STEP 8 (audio determinism): added gba_ppu/frame_determinism_audio - two
  state-identical cores run interleaved frames; framebuffer CRCs must match
  per frame and the delivered stereo streams must be byte-identical. Also
  covers: non-zero PSG output (duty 2, volume 7 -> 7168-amplitude samples),
  sample-count bounds, stereo pairing, timer-IRQ dispatch with no handler
  installed (CPU keeps running). Found+fixed en route: the APU IO dispatch
  window was [0x80,0xB0) and missed the PSG registers at 0x60-0x7F (channel
  could never be configured); buffer-cap artifacts in the test design.
- STEP 10 (memory ownership): every allocation has one owner (cores own
  their state buffers; cart owns its ROM copy; tests free every buffer).
  LeakSanitizer reports zero leaks across the full suite and all smokes.
- STEP 11 (UB audit): ASan+UBSan Debug build runs the full suite and all
  four CLI smokes with exit 0, no runtime errors. Found+fixed en route:
  tests/mgbx/cart.c wrote tiny[0x147] into a 0x80-byte array (OOB on the
  stack); rewritten to build a full-size buffer and pass a truncated size.
- STEP 12 (warnings): full tree builds with -Wall -Wextra -Wpedantic
  -Wshadow -Wstrict-prototypes -Werror: zero warnings.
- STEP 13/14 (tests): suite organized as tests/common (API lifecycle,
  state contract) + tests/mgbx, tests/nes, tests/supersnes, tests/mgbax;
  235 tests, 0 failed assertions.
- STEP 15 (portability): static audit only (no ARM cross compiler, no
  multilib in this environment - no false claims). Results: no `long` in
  any core (only emu-cli's ftell, negative-checked); no bare `char`
  arithmetic; no struct-pointer casts (no unaligned/aliasing risk); no
  float/double in any core; fixed-width integers throughout; save states
  serialize explicit little-endian bytes (host-endian independent).
- STEP 16 (performance): emu-cli --benchmark, Release build, synthetic
  NOP-loop ROMs, 600 frames, x86_64 Linux 2 CPUs: mgbx ~3470 fps,
  beatle-nes-redux ~2510 fps, supersnes ~790 fps, mgbax ~300 fps.
  Synthetic workload; relative indicator only.
- STEP 17 (docs): README.md written - build options, CLI usage, public
  interface, honest per-core accuracy notes and known limitations,
  benchmark methodology. No marketing language.
- STEP 19 (CI): .github/workflows/ci.yml - three configurations (Release,
  Werror, ASan+UBSan): configure, build, ctest.
- STEP 20 (final verification): deleted all build dirs; verified minimal
  single-core build (mgbx only) passes; full clean Release build: 0
  warnings, 235/235 tests; sanitizer suite exit 0; all four cores pass
  --smoke under sanitizers; Werror build clean; save-state write + reload
  and PPM framebuffer output verified via emu-cli.

Stage Summary:
- Framework complete at milestone 1: 4 cores, 235 tests green, ASan/UBSan
  clean, -Werror clean, CI in place, docs truthful.
- Remaining limitations are documented in README.md (S-SMP/DSP stub, FIFO
  audio, scanline PPU approximations, mappers beyond 0/1/2/3/4, etc).
