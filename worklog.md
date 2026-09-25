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

---
Task ID: 7
Agent: main (Super Z)
Task: Milestone 2a — mgbax direct-sound FIFO audio + APU save-state completeness

Work Log:
- Verified milestone-1 state after session restart: clean Release rebuild, 235/235
  tests, ASan/UBSan clean (suite + 4 CLI smokes), -Werror clean. cmake reinstalled
  (4.4.3 via pip; not on PATH after environment reset).
- Implemented GBA direct-sound FIFO channels A/B in apu.c: 32-byte hardware FIFOs,
  one byte popped per selected-timer (0/1) overflow, DMA request raised when the
  queue drops to <= 16 bytes.
- dma.c: gba_dma_fifo_request services trigger=3 channels (A: ch1/ch2, B: ch2/ch3,
  highest priority first) with a 4-word fixed-destination transfer; count register
  ignored per hardware contract.
- mem.c: word stores to $40000A0-AF are single 4-byte FIFO append events (request
  level evaluated once per event, not per halfword); 8-bit FIFO writes routed
  through a new gba_apu_io_write8.
- timer.c: overflow notifications (incl. cascade overflows) set timers.ovf_bits,
  consumed by the APU each step — this is the FIFO sampling clock.
- Mixing: SOUNDCNT_X bit7 gates PSG; SOUNDCNT_L per-channel routing + (v+1)/8
  per-side master volume; SOUNDCNT_H PSG 25/50/100%; FIFO A/B 50/100% volume with
  L/R enable routing; saturated int16 mix. SOUNDBIAS (DC offset) not applied —
  documented.
- Noise channel output implemented (was structurally present but silent): LFSR
  shift clock 32*(R+1)<<S cycles, 15-bit/7-bit width modes, restart semantics.
- Save states: APU state was NEVER serialized before (defect) — full APU +
  timers.ovf_bits serialization added; state tests now perturb/verify APU fields.
- BUG FOUND + FIXED (register map): square-1 envelope volume was read from
  SOUND1CNT_L bits 12-14; per spec it is the 4-bit initial volume in
  SOUND1CNT_H ($62) bits 12-15 (0x60 bits 12-14 are sweep-time). Square-2 and
  noise volumes widened to 4 bits (0-15). Regression-covered by psg tests.
- New suite tests/mgbax/apu.c: 10 tests (pop order, timer select, full-discard,
  reset bits, DMA refill + channel eligibility, routing/volumes, master gate +
  active-flag readback, 25/50/100% scaling, spec-derived LFSR walk, FIFO audio
  determinism across cores). Existing determinism test updated to the register-
  accurate setup.
- Results: 241 tests, 0 failed assertions; ASan/UBSan suite + GBA smoke clean;
  -Werror clean.

Stage Summary:
- mgbax FIFO/direct-sound gap closed; audio output now covers DMA sound + PSG.
- Two real defects fixed (missing APU state serialization; wrong envelope-volume
  register), each covered by tests.

---
Task ID: 8
Agent: main (Super Z)
Task: Milestone 2b — supersnes HDMA + DMA control-path fixes

Work Log:
- Implemented HDMA (direct + indirect) in dma.c: per-scanline passes executed
  during each line's HBlank (hooked into the PPU line wrap before rasterize),
  block headers (1-byte for unit 1, 2-byte otherwise; repeat flag), per-block
  indirect address load (bank byte in table only for unit-4 modes, $43x7
  otherwise), V=0 reload of the A2 pointer from the $43x2-4 write backup.
- BUG FOUND + FIXED: $420B (MDMAEN) writes were mis-wired into nmitimen —
  GP DMA via the register path never executed (s->dma.mdmaen was only set
  white-box in tests) and the write corrupted the NMI-enable bit. Now:
  $420B -> dma.mdmaen + immediate execution; $420C -> dma.hdmaen.
- BUG FOUND + FIXED: DMA control-bit mapping diverged from hardware
  (engine used bits 3-4 as step codes). Now per spec: bit7 direction,
  bit6 indirect, bit4 fixed address, bit3 decrement, bits 0-2 transfer mode;
  GP DMA channels self-clear fully after a transfer (was &= 0xF0).
- BUG FOUND + FIXED (test framework): T_MAX_SUITES 32 silently dropped suite
  registrations beyond 32 — the suite count had reached 34 and gba_apu/
  gba_state were silently skipped. Capacity raised to 64 and overflow now
  aborts loudly instead of discarding tests.
- Serialization: all HDMA per-channel state added to SNES save states.
- New suite tests/supersnes/hdma.c (5 tests): direct mode to VRAM, repeat
  blocks, indirect addressing, register path incl. NMITIMEN regression,
  V=0 reload.
- Results: 250 tests, 0 failed assertions; ASan/UBSan suite + 4 CLI smokes
  clean; -Werror clean; clean-from-scratch rebuild: 0 warnings.

Stage Summary:
- SNES HDMA functional gap closed; DMA register path corrected to hardware
  semantics; test-runner overflow defect eliminated.
- Documented approximations: HDMA time not subtracted from CPU execution;
  mode 2-4 register patterns simplified to consecutive B-bus addresses;
  HDMA for V=0 itself not performed (first visible line uses reset values).

---
Task ID: 9
Agent: main (Super Z)
Task: Milestone 3 — AArch rebrand, cross-platform CLI (Windows 7+/macOS 10.x+/
Linux/Android 6+), new cores (FinalBurn Genesis real implementation; Beatle PSX,
mDS-A, ms-32, SuperSaturn, m64-B, superCastPro as honest skeletons), push to
github.com/aarvsn/AArch

Work Log:
- Verified milestone-2 baseline after restart: clean Release rebuild, 250/250
  tests, tree clean. Pushed milestones 1-2 to github.com/aarvsn/AArch (remote
  canonical case). First push was REJECTED: the provided token lacks the
  GitHub `workflow` scope and the history contained
  .github/workflows/ci.yml. Rewrote local history (filter-branch) to drop the
  workflow file, relocated it to ci/github-ci.yml with an explanatory commit,
  pushed successfully. Note recorded in README + CI file header: copy
  ci/github-ci.yml to .github/workflows/ci.yml with a scoped token to
  activate CI.
- Rebranded: CMake project "AArch" v0.3.0, CLI binary `aarch`
  (tools/emu-cli -> tools/aarch), public header doc comments. Public ABI kept
  at version 1; additions are purely additive.
- emu.h additions: EMU_ENOTIMPL result code; 7 new core accessors;
  emu_core_status_t (working/partial/skeleton); emu_core_registry() +
  emu_core_info_t; emu_rom_probe(); emu_core_status_str().
- src/common/registry.c: registry of 11 cores with honest statuses and
  per-build guarded vtable accessors; ROM signature probing (iNES, N64
  z64/v64/n64, PS-X EXE, SEGA SEGASATURN/SEGAKATANA IP.BIN, SEGA 32X over
  Genesis, Genesis "SEGA" domestic field, NDS logo at 0x160, GB logo at
  0x104, GBA logo at 0x04, SNES LoROM/HiROM checksum complement,
  ISO9660 PLAYSTATION system id).
- NEW CORE finalburn (Genesis/Mega Drive), ~5400 lines:
  * m68k.c: full official 68000 set (MOVE families, immediate ALU, bit ops,
    MOVEP, quick forms, Bcc/DBcc/Scc, shifts/rotates reg+mem, MUL/DIV,
    LINK/UNLK, MOVEM incl. predecrement reversed mask, TAS, EXG/SWAP/EXT,
    PEA/LEA, TRAP/CHK/TRAPV, MOVE SR/CCR/USP, STOP/RTE/RTR/RESET, ABCD/SBCD/
    NBCD two-nibble decimal, exceptions incl. trace + IPL 1-7 + STOP wake,
    supervisor/user SP switching). Approximations documented: coarse cycle
    counts, unaligned accesses aligned silently, RESET = no-op.
  * z80.c: full documented Z80 (base/CB/ED/DD/FD, DDCB/FDCB, IM0/1/2,
    block ops, index registers, DAA, 16-bit INC/DEC).
  * vdp.c: VRAM/CRAM/VSRAM + auto-increment, register writes, planes
    A/B/window with priority, sprites (H32/H40 limits, link walk, flips,
    first-come priority, collision/overflow flags), whole/per-line H-scroll,
    whole/per-column V-scroll, backdrop, DMA (68K->VDP, VRAM fill, VRAM
    copy) with approximate timing and 68K stall, V-int/H-int with status
    read clearing, HV counters, display blanking. CRAM layout
    0000BBB0GGG0RRR.
  * psg.c: SN76489 3 tone + noise (15-bit LFSR, white/periodic, tone2-
    clocked rates), 4-bit attenuation; envelope mode not implemented
    (documented).
  * ym2612.c: 6x4-op FM, 8 algorithms (datasheet connectivity), feedback,
    detune/multiple/TL, AR/DR/SR/RR + key scaling + sustain level, key
    on/off, timers A/B with IRQ, DAC, L/R panning. Integer-only: sine via
    symplectic circle walk, dB envelope via integer 0.75dB/unit chain.
    Tuning anchor FNUM=1024/OCT=4/MULT=1 = 440 Hz (phase increment 8659 at
    master/144; regression-tested). LFO/SSG-EG/CH3-special off (documented).
  * cart.c: ROM load, power-of-two mask/modulo mirroring, SRAM header
    detection ("RA" at 0x1B0) + 64 KiB SRAM.
  * md.c: 68K bus (ROM/RAM mirrors/Z80 window/IO/VDP/YM/busreq/reset/bank),
    Z80 bus bridge (RAM/YM/bank register/PSG/H counter), 3-button pad with
    TH multiplexing ($A10003 data, $A10009 ctrl), frame loop (262 lines x
    488 68K cycles, Z80 credit accounting, YM master/144 sampling, PSG
    z80/16 ticks, 48 kHz Bresenham output), vtable, explicit save states
    (full machine incl. both CPUs, VDP, PSG, YM2612 envelopes/phases, SRAM).
- SKELETON CORES: src/common/skeleton.{c,h} shared honest scaffolding +
  beatle-psx / mds-a / ms-32 / supersaturn / m64-b / supercastpro cores:
  format validation on load, image held, run_frame/save/load return
  EMU_ENOTIMPL. Contract-tested.
- CLI: aarch binary with core auto-detection via emu_rom_probe, --core
  override, --list with per-core status, skeleton runs exit 3 with an honest
  "not implemented" message, distinct exit codes (0/1/2/3/4).
- CMake: project AArch, options for all 11 cores, MSVC /W4 path, Windows 7
  (_WIN32_WINNT=0x0601), macOS deployment target 10.12 default, registry
  compile definitions via generator expressions; toolchains/mingw-w64.cmake
  for Linux->Windows cross builds; Android via the NDK toolchain file
  (documented, API 23).
- CI (ci/github-ci.yml): Linux x64 (release/Werror/sanitizers + core smokes),
  Linux ARM64 (ubuntu-24.04-arm), Windows MSVC, macOS, Android NDK compile
  matrix (arm64-v8a/armeabi-v7a/x86_64 at API 23).
- Tests: 67 new (finalburn m68k 20, z80 10, vdp 8, audio 9 incl. frame
  determinism with audio byte-comparison, state 7; romdetect 8; skeleton
  contracts 5). Total 317 tests, 0 failed assertions.

Bugs found and fixed during test bring-up (each verified by a failing test):
- finalburn/cart.c: fb_cart_read8 divided by rom_size 0 during reset-vector
  fetch at create time (SIGFPE) -> guard no-cart open bus.
- finalburn/m68k.c abs.W addressing returned PC-relative base + ext instead
  of the absolute word (LEA/JSR/MOVEM abs forms jumped to wrong addresses).
- finalburn/m68k.c immediate group marked ADDI (sub 3) as illegal.
- finalburn/m68k.c SUB borrow formula used (a & r) instead of (~a & r):
  NEG/CMP/SUB borrow flags wrong (NEG.B #5 must set C/X).
- finalburn/m68k.c TRAP #n (0x4E40-0x4EFF) was not decoded at all.
- finalburn/m68k.c NOP mask (op & 0xFFF9) == 0x4E71 also matched RTS
  (0x4E75) -> RTS executed as NOP, call/return broken. Exact-match now.
- finalburn/m68k.c EXG decode mask dropped bit 6: (op & 0xF138) == 0xC140
  never matched; ABCD mask collided with EXG Dx,Dy.
- finalburn/m68k.c op_bcd wrote the result to the source register (Dy)
  instead of the destination (Dx), and SBCD operand order was swapped.
- finalburn/z80.c 16-bit INC/DEC rr (0x03/0x13/0x23/0x33/0x0B/...) missing.
- finalburn/z80.c ED block: switch mask (op & 0xC7) made ADC-HL/LDIR/CPIR
  cases unreachable; direction/repeat bits (3/4) were swapped.
- finalburn/z80.c IM decode: (op >> 3) & 3 mapped ED 56 to IM 2.
- finalburn/skeletons: create wrappers never set base.vtable ->
  emu_core_destroy(NULL deref) segfaulted; regression-tested via
  emu_core_destroy in the skeleton contract suite.
- finalburn/vdp.c: register-write commands (bit15 set) were treated as
  pending two-word data commands, so registers were never set.
- finalburn/ym2612.c envelopes started at attenuation 0 (full volume)
  after reset; correct contract is silent until key-on.
- finalburn/psg.c noise counter started at 0 and underflowed (never
  shifted) -> periodic noise stayed silent.

Stage Summary:
- Milestone 3 complete: AArch rebrand, 11-core registry (5 working/partial
  real cores + 6 honest skeletons), cross-platform CLI build paths for
  Windows 7+/macOS 10.12+/Linux/Android 6+ with CI matrix, 317/317 tests,
  ASan/UBSan clean (suite + all 5 core smokes), -Werror clean, finalburn
  ~3150 fps (Release, synthetic ROM).
- finalburn known limitations are documented in README (FIFO, DMA timing,
  shadow/highlight, interlace, LFO, SSG-EG, PSG envelope mode, coarse 68K
  cycles, silent-aligned unaligned accesses).
- Remaining systems (PSX/DS/32X/Saturn/N64/DC) are honest skeletons with
  detection + validation only; CPU/GPU work is future milestones.

---
Task ID: 4
Agent: Super Z (lead engineer)
Task: Simplify/professionalize README, remove GLM artifacts, Termux support, implement skeleton cores

Work Log:
- Removed sandbox artifacts from the git index and disk: .env,
  download/README.md, scripts/dbg_*.c scratch files; .gitignore refreshed.
- Added scripts/build-termux.sh (native Termux build + ctest + optional
  install) and a CMake install rule for the CLI and public header.
- Implemented beatle-psx (skeleton -> partial, ~2600 lines): MIPS R3000A
  interpreter (full MIPS I, load/branch delay slots, COP0 exceptions/RFE,
  AdEL/AdES/Syscall/BP/RI/CPU/Ov traps), GTE (all documented commands,
  UNR division table, FLAG saturation contract, MVMVA cv=2 hardware bug),
  GPU (1 MiB VRAM, fills, mono/gouraud/textured tris+quads, lines, rects,
  CLUT 4/8/15bpp, semi-transparency, dither, mask, 15/24-bit scanout),
  DMA (burst/sync/linked-list/OTC), root counters, IRQ controller,
  simplified digital pad, PS-X EXE loader (no BIOS image required).
- Wrote a shared SH-2 interpreter (src/common/sh2.{c,h}, ~700 lines) with
  encodings verified against the Hitachi SH-1/SH-2 Programming Manual
  (downloaded from the Sega docs archive); includes DIV0S/DIV0U/DIV1
  hardware division steps verbatim from the manual pseudocode, DMULS/
  DMULU, MAC.L/MAC.W, all shift/rotate forms, delayed branches.
- Implemented ms-32 (skeleton -> partial): composed 32X adapter on the
  finalburn Genesis machine via a new A15100h register-window hook;
  two SH-2s, COMM ports, RES/INTS/vectors/HCOUNT, VDP packed-pixel
  framebuffer + fill command, save states extending the MD blob.
- Bugs found and fixed by tests (each verified failing first):
  psx create() missing vtable (NULL deref); exception EPC used the
  advanced PC; load-delay state wrongly flushed on exception; GPU fill
  color/param counts; GTE real opcodes for AVSZ3/AVSZ4/OP (fake vs real
  opcode fields); RTPS MAC SAR semantics; MVMVA identity expectations;
  SH-2 reset step size (4 vs 2 bytes); BRA/BSR 12-bit displacement;
  group-4 dispatch on the low byte incl. MAC.W Rm field; DIV0U decode
  (was aliased to NOP); TRAPA/illegal-opcode immediate branches;
  SWAP.B upper-halfword loss; missing STS MACH/MACL/PR forms;
  NEGC borrow; ADDC/SUBC carry handling; ms-32 const-bus static write.
- README.md rewritten: shorter, professional, honest status table,
  Termux first for Android.
- Tests: 366 total, 0 failed assertions; clean under ASan+UBSan and
  -Werror. (317 -> 366: psx cpu/gte/gpu/state 33, sh2 12, ms32 5 minus
  one skeleton-contract retirement each for psx/32x.)

Stage Summary:
- Skeleton cores implemented: beatle-psx (partial: CPU+GTE+GPU+DMA+timers,
  homebrew PS-X EXEs run), ms-32 (partial: SH-2 x2 + adapter + VDP over
  the complete Genesis machine).
- Remaining skeletons: mds-a (DS), supersaturn (Saturn; the shared SH-2
  engine is now available for it), m64-b (N64), supercastpro (DC).
- Termux path documented and scripted; sandbox artifacts removed from
  the repository.

---
Task ID: 5
Agent: Super Z (lead engineer)
Task: New push token; continue next steps (implement remaining skeleton cores)

Work Log:
- Restored sandbox toolchain (cmake via pip); verified 366 tests green before changes.
- Discovered local branch had diverged from origin: the local commit re-added the
  sandbox .env and reverted .gitignore (sandbox artifact regression). Reset local
  to origin/main (content otherwise identical, verified by diff).
- Reference work: downloaded official Sega Saturn manuals from the antime.kapsi.fi
  Sega docs archive - VDP1 User's Manual (ST-013-R3), SCU User's Manual (ST-097-R5),
  SMPC User's Manual (ST-169-R1), Disc Format Standards (ST-040-R4, IP.BIN System ID
  header + boot sequence). VDP2/CD manuals not located; those subsystems stayed stubs.
- supersaturn: skeleton -> partial (~1400 lines): dual SH-2 (shared interpreter),
  memory map per SCU manual figure 1.3/1.5, SCU registers, direct+indirect DMA
  (field decode cross-checked with Yabause; end flag = bit 31 of the indirect
  header read-address word), interrupt controller with the official vector/level
  table, timers, SMPC INTBACK digital-pad reporting, SINIT slave start, VDP1
  command processor (jump/call/return, normal+scaled sprites with nine zoom
  points, polygon fill via edge functions, lines; color modes 0-5 with CRAM
  banks / VRAM LUTs / RGB555; SPD/ECD/mesh; system+user clipping; erase/write +
  1-cycle frame change), IP.BIN boot model (AIP -> WorkRAM-H 0x06002000, stacks
  from System ID, VBR = 0x06000000, interrupts masked at boot).
- sh2.c: added sh2_irq_vector() (controller-supplied vectors, VBR+0x600+vec*4)
  and the missing group-0 MOV.B/W Rm,@(R0,Rn) (found by the new Saturn tests).
- m64-b: skeleton -> partial (~1200 lines): R4300i (MIPS III) big-endian
  interpreter (full MIPS I + MIPS III 64-bit shifts/loads/stores, LL/SC, CP0
  with Count/Compare + exception model + ERET), documented no-PIF boot model
  (ROM[0..0x1000) -> SP DMEM, PC = 0xA4000040, r29 = 0xA4001FF0, Status =
  ERL|BEV), 4 MiB RDRAM, PI cart->RDRAM DMA, SI 64-byte PIF DMA, MI interrupt
  latch/mask, VI with progressing half-line counter + VI_INTR + framebuffer
  output (RGBA5551 / RGBA8), PIF ROM region RAM-backed for vectors.
- Bugs found and fixed by tests (each verified failing first):
  supersaturn: SCU DMA field decode/lane semantics (rewritten to edge-triggered
  dispatch), indirect-mode end-flag execution order, VDP1 edge-function formula
  (x-components where y belonged), framebuffer stores raw 16-bit color codes
  (conversion moved to render), display window origin, polygon CMDPMOD clipping
  passthrough, NULL-ROM handling, IRQ storm at boot (IMS all-masked at boot,
  VBR initialized by the boot model).
  sh2.c: missing MOV.B/W Rm,@(R0,Rn) encodings 0x0004/0x0005.
  m64-b: boot copy destination (DMEM not IMEM, full first KB), PI cart address
  mask (0x1FFFFFFE strips kseg bits), RGBA5551 extraction (bits 14-10/9-5/4-0),
  exception entry now clears ERL, ERET implemented.
  (My own hand-assembled test bugs - BRA/BSR confusion, RTE encoding, register
  fields - were also caught by the suite and fixed in the tests.)
- registry/README: supersaturn and m64-b -> partial with honest capability notes.
- Tests: 366 -> 387 (saturn 14, m64b 7); 0 failed assertions; ASan+UBSan clean
  (fixed a test-side leak the sanitizers caught); -Werror clean.
- Pushed both milestones with the new token (token scrubbed from the remote URL
  after each push): af8ce24 (supersaturn), 7c936d5 (m64-b).

Stage Summary:
- Registry now: 5 working / 6 partial / 2 skeleton.
- Remaining skeletons: mds-a (Nintendo DS) and supercastpro (Dreamcast). Both
  require new CPU interpreters (ARM946E-S + ARM7TDMI; SH-4) and were left
  honest skeletons this session - recommended next steps.
- Saturn and N64 cores run machine-level code (tested with hand-assembled
  programs through the documented boot models); retail software needs the
  documented stubs implemented (VDP2/SCSP/CD for Saturn; RSP/AI for N64).

---
Task ID: 6
Agent: Super Z (lead engineer)
Task: Fill the gaps - implement the last two skeleton cores (mds-a DS, supercastpro DC), push with the new token

Work Log:
- Restored the sandbox toolchain (cmake 4.4.3 via pip) and verified the baseline: 387
  tests green on the committed tree; reset the working tree (file-mode noise) and
  removed the .env sandbox artifact that had reappeared on disk.
- Wrote src/common/arm.{c,h}: shared ARMv4T + ARMv5TE interpreter (~1200 lines) for
  both DS CPUs - full ARM condition/flag model, register-specified and immediate
  shifts incl. RRX, MUL/MLA/UMULL/UMLAL/SMULL/SMLAL, SWP, halfword/signed transfers
  (H-bit discriminator vs the multiply block), LDM/STM with user-bank transfers and
  SPSR exception return, MSR/MRS, BX/BLX, CLZ/QADD/QSUB/QDADD/QDSUB/SMULxy/SMLAxy/
  SMULWy/SMLAWy (v5TE flag), banked r8-r12/r13/r14/SPSRs, SWI/UND/PABT/DABT/IRQ/FIQ
  vectoring with correct LR conventions, complete Thumb v1 set (formats 1-19 with
  BL long form), optional CP15 MRC/MCR hooks.
- Bugs the DS tests caught (each reproduced failing first): halfword-transfer check
  collided with the multiply block tail (1001) - H bit added as discriminator; CLZ
  reads Rd from [19:16]; MSR(register) mask needed [11:4]==0 or CLZ aliased into it;
  BX mask value was wrong (0x01200010 vs 0x012FFF10); MCR/MRC decoded under the
  LDC/STC UND branch; UMULL signedness bit inverted; UMULL test encoding itself was
  wrong; KEYINPUT was placed in the high half of the word; IPCSYNC send/IRQ bits
  reworked (bit14 = IRQ enable, remote latch); I/O range had to be removed from the
  RAM pointer helper so dynamic registers (KEYINPUT/VCOUNT/IE/IF/IME/IPCSYNC) get
  handled at all; frame-end VCount wraps to 0; unrendered framebuffer rows are
  opaque black; the ARM9 v5te flag was never set in ds_create.
- mds-a (skeleton -> partial, src/mds-a/ds.{c,h} ~1000 lines): dual shared-ARM
  machine, documented direct-boot from the .nds header (ARM9/ARM7 code copied to
  header addresses, System mode, masked IRQ/FIQ, documented stacks), 4 MiB main RAM,
  ITCM/DTCM with CP15 c9 region registers (defaults 0x0100000E/0x08000006) and
  accepted-and-stored c1/c3/c7/c8/c10, VRAM banks A-G at default linear addresses,
  shared+ARM7 WRAM, per-CPU 4x16-bit timers (prescalers, cascade, IRQ), IPCSYNC
  send/recv crossing between CPUs, IE/IF/IME per CPU with VBlank/VCount/Timer/IPC
  sources, KEYINPUT active-low (A..Y bits 0-11), 263-line frame model at
  67.028/33.514 MHz, dual-engine BG bitmap scanout: modes 3 and 5 (page bit) from
  VRAM A (top) and B (bottom), 256x384 XRGB8888 output. Save states serialize both
  CPUs + machine. Not implemented (documented): 3D, sprites, sound, touch, card bus,
  Wi-Fi, cache modeling.
- Wrote src/common/sh4.{c,h}: shared SH-4 interpreter (~900 lines) - full SH-2-family
  integer set, DT, banked R0-R7 via SR.RB, extended system registers
  (SSR/SPC/SGR/DBR/FPUL/FPSCR with the documented 5A/6A STS/LDS codes), TRAPA
  vectors VBR+0x100+imm*4, controller-supplied interrupt vectors with BL/I gating,
  and the single-precision FPU subset: FADD/FSUB/FMUL/FDIV/FCMP.EQ/FCMP.GT/FMAC,
  FABS/FNEG/FSQRT/FLDI0/FLDI1, FLOAT/FTRC, all FMOV.S forms, FIPR, FTRV (XMTRX in
  XF), FPSCR.FR bank switch and DN denormal flush. Documented simplifications:
  PR=1 executes as single, rounding mode stored, unsupported FPU ops are illegal.
- supercastpro (skeleton -> partial, src/supercastpro/dc.{c,h} ~600 lines): SH-4
  machine with P0/P1/P2/P3/P4 decode (P4 on-chip module area for TMU/INTC/SCIF,
  URAM + P4 mirror, boot ROM mirror, DC peripheral window), 16 MiB RAM, 8 MiB VRAM,
  AICA wave RAM, flash stub, TMU channels 0-2 (TSTR/TCOR/TCNT/TCR, underflow +
  TUNI0-2 interrupts), PVR2 display-controller scanout via FB_R_CTRL/FB_R_SIZE/
  FB_R_SOF1 (RGB565/888/0888, per-format byte stride), documented direct-boot from
  IP.BIN (boot LBA at 0x300, byte count at 0x308 -> 0x8C010000, SR = BL|MD,
  r15 = 0x8CFF0000). Save states serialize the CPU + machine. Not implemented
  (documented): Tile Accelerator, AICA sound, GD-ROM, G2 DMA, Maple.
- Bugs the DC tests caught: SH4_SR_MASK was missing RB/BL (banked-register test
  caught it); FTRC read its source from the m field instead of n; P4 generic
  peripheral window shadowed the on-chip TMU area (decode order); boot copy went to
  RAM offset 0x00100000 instead of 0x00010000 (0x8C010000 - 0x8C000000); RGB565
  scanout used a 4-byte stride. Test-side hand-assembly errors were also caught and
  fixed (SUB operand order, mov r1,r0 vs mov r0,r1, MOVA/literal placement, 8-bit
  immediate sign extension, LDS-VBR vs LDC-SR, TMU test had SR.I masking TUNI0).
- libm linked for sqrtf (UNIX only). registry/README/emu.h: mds-a and supercastpro
  -> partial with honest capability notes; skeleton-contract tests retired in favor
  of a registry-completeness test.
- Tests: 387 -> 410 (ds 13, dc 11 minus 2 skeleton-contract retirements plus a
  registry test); 0 failed assertions; clean under ASan+UBSan and -Werror; CLI
  --list shows 11 cores (5 working / 6 partial / 0 skeleton).
- Pushed with the new token (scrubbed from the remote URL after the push):
  43ec650 (mds-a + supercastpro + shared ARM/SH-4 interpreters).

Stage Summary:
- Registry now: 5 working / 6 partial / 0 skeleton. Every core named in milestone 3
  is at least partial with a documented gap list.
- Shared interpreters in src/common: SH-2, ARM (v4T/v5TE), SH-4.
- Suggested next steps: DS 2D compositing (sprites/text BGs) and card DMA; DC Tile
  Accelerator TA-list parsing and AICA ARM7; GBA-style save-state fuzzing for the
  new cores; retail-software boot paths remain gated on the documented stubs.

---
Task ID: 7
Agent: Super Z (lead engineer)
Task: Fill the gaps - repo hygiene (.env), zero-warning test build, note fixes, push with the new token

Work Log:
- Verified the baseline on the committed tree (restored cmake 4.4.3 via pip):
  410 tests, 0 failed assertions; found one warning - run_skel() unused in
  tests/common/cores.c (leftover after the skeleton-contract retirement).
- Deleted the dead run_skel() and the stale skeleton-contract header comment;
  renamed the suite skeletons -> registry (test_main.c declaration + call
  follow). Zero warnings after the change.
- Fixed the supersaturn registry note: it embedded "(320x224, 44100 Hz)"
  inline while the CLI already appends the same info from core caps -
  duplicated suffix removed.
- README: the finalburn row pointed at "README notes in the repo wiki of
  history" (a page that does not exist). Replaced with the documented m68k
  approximations: coarse cycle counts, unaligned accesses aligned silently,
  RESET is a no-op.
- .env: found tracked AND on disk locally (sandbox artifact reappearing).
  The remote already had it removed (9ad5a6f, prior session); local branch
  was stale and contained a divergent duplicate of the session-6 worklog
  commit (fee615f vs remote 9ad5a6f).
- Push reconciliation WITHOUT force: fetched, confirmed worklog content
  identical at both tips, rebased, then normalized the 59 file-mode-noise
  files (sandbox had restored executable bits) to the remote's canonical
  modes, adopted the remote's stricter .gitignore (upload/, download/,
  db/, .env, tool-results/, build*/, cmake-build-*/, *.o, *.a), soft-reset
  to the remote tip and committed the real delta as one commit.
- Pushed with the new token via a one-shot URL (token never written to
  .git/config): 9ad5a6f..b7fd2a5 main -> main. ls-remote confirms remote
  main == local HEAD; zero unpushed commits.
- Token scope check: x-oauth-scopes: repo only (no workflow scope) - CI
  stays at ci/github-ci.yml per its header note; activating it requires
  copying to .github/workflows/ci.yml with a workflow-scoped token or in
  the GitHub UI.

Stage Summary:
- Registry: 5 working / 6 partial / 0 skeleton; 410 tests, 0 failed
  assertions; zero warnings; ASan/UBSan status unchanged from session 6.
- Milestone-3 hygiene tasks complete: no GLM/sandbox artifacts tracked
  (.env deleted, .gitignore hardened against regeneration), README
  professional and accurate, Termux path documented + script verified in
  scripts/build-termux.sh, CI parked at ci/github-ci.yml with instructions.
- Remaining (documented, honest): DS 2D compositing + card DMA; DC Tile
  Accelerator + AICA; Saturn VDP2/SCSP/CD; N64 RSP/AI; PSX CD/SPU; 32X
  RLE/autosprites/PWM; retail-software boot gated on those stubs.

---
Task ID: 8
Agent: Super Z (lead engineer)
Task: DS 2D compositing for mds-a (text/affine/extended BGs + OBJs), sanitizer pass

Work Log:
- Wrote src/mds-a/ds2d.c (~640 lines): per-engine 2D composition from the
  hardware reference. BG layer types per DISPCNT mode (0-5; mode 4 kept ==
  mode 3 as documented, modes 6/7 unsupported -> layers off), text BGs
  (4/8bpp, char base bits 2-3, screen base bits 8-12 in 2K blocks, sizes
  with s+1/s+2/s+3 split blocks, h/v flips, per-BG scrolls), affine BGs
  (8bpp, char base bits 2-7, BYTE map entries, 16-128 tile maps, wrap bit
  13, center-reference .8 transform with the mgbax +0.5 convention),
  extended-affine BGs (formats 0-6; tiled formats documented approximation:
  tile data from region start; 16bpp bitmaps replace the old raw mode-3/5
  scanout, 0x0000 transparent, bit 15 alpha flag ignored), OBJs (1D/2D
  mapping, 2D wraps at map rows 32/16 entries per engine, 8bpp tiles take
  two numbers, h/v flips, rot/scale via OAM+0x300 param sets, X wrap 512 /
  Y wrap 256, double-size box extends half a size), priority composition
  (prio 0..3, OBJs before BGs at equal priority, OAM order tie-break),
  backdrop = BG pal entry 0, master brightness down/up (factor bits 0-5).
- ds.c: engine B registers moved to the real 0x04001000 page (ARM9 io page
  grew to 8 KiB), palette block at 0x06880000 (4 x 512B slots), OAM at
  0x07000000/0x07000400, proper sub-word io reads (old code returned the
  low byte/halfword of the aligned word for any offset), 16-bit io write
  path, timer registers wired to the timer units on both CPUs (32/16-bit;
  control write with enable reloads immediately - documented model),
  save-state magic bumped to A2SM (format rev 2: palette/OAM serialized).
- Renderer bugs the tests caught (each reproduced failing first):
  text char base was decoded from the PRIORITY field (cnt & 3 instead of
  (cnt >> 2) & 3); palette lookup indexed bytes instead of entries (x2);
  affine map entries read as u16 (they are bytes); 2D OBJ stride used the
  sprite width instead of the map row (32/16 entries, engine-dependent,
  8bpp rows half); OBJ 1D flag read DISPCNT bit 14 (it is bit 13);
  1D stride multiplied pixel width instead of tile width.
- mgbax: three pre-existing UB sites surfaced by this session's sanitizer
  run (previously reported clean): ARM branch sign-extension overflow
  (cpu.c branch handler), three Thumb branch sign-extensions, and two APU
  FIFO negative left shifts (fifo_cur[x] << 8 -> * 256). All replaced with
  unsigned-shift/arithmetic-right-shift or multiplication idioms.
- Tests: 410 -> 419. New: text_bg_tiles_and_scroll (4bpp/8bpp, flips,
  palette banks, scroll), bg_priority_and_backdrop (cross-priority,
  equal-priority BG order, backdrop), obj_sprites (placement, hflip,
  disable bit, OBJ-vs-BG ties both ways, 1D vs 2D row addressing, 8bpp
  full palette), obj_rotation_scaling (identity, 2x shrink, 90 degrees),
  affine_bg_transform (identity, 2x shrink with 128px-map centering,
  out-of-range vs wrap), engine_b_registers (0x04001000 page + palette
  slot), timer_registers_via_bus (32/16-bit writes, reload semantics,
  ARM7 path), master_brightness (down 21/63, up on black), forced_blank.
  All-zero OAM decodes as 128 sprites at (0,0) sharing tile 0 - tests
  disable unused slots first (as real software does).
- 419 tests, 0 failed assertions; zero warnings; ASan+UBSan clean (the
  earlier runtime errors were the mgbax UB fixes above, now gone).
- Sandbox note: the environment reset mid-session again (cmake wiped, .env
  and 2-line .gitignore restored, file modes flipped). Recovered each time;
  the accidental `git add -A` commit that picked up build-asan/ was reset
  locally before any push. Final commit contains only the 9 real files.
- Pushed with the one-shot token URL (never written to .git/config).

Stage Summary:
- mds-a: 2D engines now full per-engine composition; registry/README
  updated (still partial: 3D, blending/windows, sound, touch, card bus,
  VRAM banking, modes 6/7 remain documented gaps).
- Suggested next steps: DC Tile Accelerator + AICA ARM7; card-bus DMA for
  DS; alpha blending + windows for the DS 2D engines (BLDCNT/BLDALPHA/
  WININ/WINOUT are already in the io page, just unmodeled); GBA-style
  save-state fuzzing for the new 2D paths.

---
Task ID: 9
Agent: Super Z (lead engineer)
Task: post-session-8 verification and hygiene after context reset

Work Log:
- Context was reset mid-conversation; verified session 8 state from git and
  worklog instead of re-deriving it: commits c179f70 (ds2d) and bf3ec04
  (worklog) present locally, remote main matched bf3ec04 (nothing pending).
- Normalized 70 sandbox mode-noise files (chmod from git ls-tree -r HEAD).
- Deleted sandbox-restored .github/workflows/ci.yml (token has no workflow
  scope; real CI stays at ci/github-ci.yml) and 8 untracked scripts/dbg_*.c
  scratch files (verified zero build references first).
- .gitignore: added .github/ with a comment explaining the CI parking spot,
  so a future `git add -A` cannot pick the workflow file up.
- Full regression from scratch: Release rebuild zero warnings, 419 tests /
  0 failed assertions, --list shows 11 cores with the updated mds-a note.
- Pushed d9c15ef with the one-shot token URL; token not in .git/config.
- Note: GitHub reports the canonical URL is now github.com/aarvsn/AArch
  (case change, redirects fine; one-shot push URLs unaffected).

Stage Summary:
- Repo state after session 8 + this cleanup: 5 working / 6 partial, 419
  tests, zero warnings, working tree clean, remote main == local HEAD.
- Same suggested next steps as session 8: DC Tile Accelerator + AICA ARM7;
  DS card-bus DMA; DS alpha blending + windows (BLDCNT/WININ/WINOUT);
  GBA-style save-state fuzzing for the new 2D paths.

---
Task ID: 10
Agent: Super Z (lead engineer)
Task: supercastpro Dreamcast - Tile Accelerator subset + AICA (ARM7 + PCM voices)

Work Log:
- Wrote src/supercastpro/ta.c (~300 lines): TA parameter FIFO (32-bit
  writes to 0x10000000-0x11FFFFFF, 64K words, sticky overflow), STARTRENDER
  (PVR 0x034) triggers a synchronous parse-and-rasterize pass into the VRAM
  framebuffer (FB_W_CTRL pack 0/1 = RGB565, 3 = RGB888, 4 = ARGB8888;
  FB_W_LINESTRIDE in 32-bit words; the target always covers the full
  640x480 tile space). Documented parameter subset: control words
  0x80/0x81/0x82/0x87 skipped, 0x83 ends, polygon headers 0x88-0x8C /
  0x90-0x94 / 0x98-0x9C with the real word counts (3/4/4/5/5, +1 for
  transparent/punch), sprites 0x8D/0x95/0x9D (3/4/4); vertices = tag word
  0xE0-0xEF + X/Y/Z floats + ARGB color, strips end at the next non-vertex
  word (real termination semantics). Screen mapping sx = x + 320,
  sy = 240 - y; painter's-order rasterizer (stream order, documented
  contract: lists submitted opaque -> punch -> transparent), strips
  triangulated, sprites axis-aligned A/B/C rects with the pixel-center
  half-open rule; flat color from the first vertex.
- Wrote src/supercastpro/aica.c (~270 lines): ARM7TDMI (shared interpreter,
  v4T) at 33.8688 MHz running from wave RAM (ARM7 map 0x00000000, local
  registers at 0x00800000); SH-4 side registers at 0x00700000-0x00707FFF.
  64 raw channel register blocks decoded live: SA (fmt bits 14:11, addr
  hi/lo), LSA/LEA (byte addresses, LEA exclusive), CA readback = pos>>16,
  pitch octave+FSC (rate = 44100 * 2^oct * (1 + FSC/1024)), loop/KYONB at
  +0x30, volume 0 = 0 dB (linear attenuation documented) and pan at
  +0x38/+0x3C. KYONEX (0x2800 bit 14) latches all KYONBs (key-on resets
  the phase); MCIPD (0x2898 bit 14) pulses the ARM7 IRQ line; MCIRE
  (0x289C bit 9) is the ARM7 -> SH-4 flag doorbell. Mixer: 44100 Hz
  stereo saturated sum, nearest sample, byte-domain position advancing
  bytes-per-sample * rate/44100; one-shot stops at LEA, loops wrap to LSA.
  PCM16 LE + PCM8 unsigned; ADPCM decoded as silence (pending).
- dc.c: AICA/TA windows in the bus (8-bit AICA access merges into the
  containing 16-bit register; TA FIFO write-only), dc_step = SH-4 + one
  ARM7 instruction (test coupling), run_frame adds the ARM7 budget
  (33.8688 MHz/60) and a 735-frame audio callback push, save state bumped
  to format rev 2 (0x44434232): + TA FIFO, ARM7 state, channel registers
  and live voice state; wave RAM member renamed aica_ram (name collision
  with the new subsystem struct).
- Tests: 419 -> 437 (dc_ta 9 + dc_aica 9). Each expectation derived from
  the dc.h documented model (hand-built parameter streams, hand-assembled
  ARM7 programs, exact integer gain math). Renderer/mixer bugs the tests
  caught (each reproduced failing first): TA dispatch used the wrong
  ParaType bit masks (opaque low nibble is 8-C, not 0-4) and the vertex
  tag mask read the low byte instead of the top byte (0xE0000000);
  sprite rects needed min/max normalization and the pixel-center rule;
  the write target must not depend on FB_R_SIZE.
- Shared-core bug found by the AICA IRQ test: common/arm.c MSR (register
  and immediate) passed the raw fsxc nibble << 16 to msr_write, which
  interpreted it as a full CPSR bit mask - every ARM-mode MSR write was a
  silent no-op (and SPSR writes hit bits 16-19). Fixed by expanding the
  fsxc nibble to byte masks inside msr_write. mgbax was unaffected (its
  core has its own interpreter); no prior test exercised common MSR.
- Known remaining gaps (documented in dc.h): Thumb MSR/MRS in common/arm.c
  (ARM-mode MSR works; the Thumb PSR-transfer format is not decoded yet),
  TA textures/UVs, modifier volumes, user tile clip, per-tile depth sort,
  Gouraud/alpha; AICA ADPCM/AEG/LFO/DSP; GD-ROM, G2 DMA, Maple.
- Sandbox reset mid-session (cmake wiped twice). Full regression:
  Release rebuild 0 warnings, 437 tests / 0 failed assertions, ASan+UBSan
  build also 0 warnings and fully green; --list note updated.

Stage Summary:
- supercastpro keeps registry status "partial" but now covers the two
  biggest homebrew unlocks: flat-shaded 3D submission through the TA
  parameter stream and ARM7 sound drivers on AICA.
- Suggested next steps: TA textures (RGB565/ARGB4444, twiddled and
  linear) + per-tile depth grouping; AICA ADPCM + the AEG envelope;
  DS card-bus DMA; DS alpha blending + windows (BLDCNT/WININ/WINOUT are
  already in the io page); store-queue FIFO path for the TA.

---
Task ID: 10 (push reconciliation + history repair)
Agent: Super Z (lead engineer)
Task: Push sessions 9-10 to GitHub; verify and repair local history first

Work Log:
- Found that a post-session `commit --amend` had folded sandbox noise
  into the local Task 9 worklog commit (48951f2 -> bd1389a): it
  re-tracked .env, reverted the .gitignore protections, and flipped 63
  files to mode 755. Verified against the original 48951f2 that the
  noise carried zero legitimate content (worklog.md byte-identical, all
  other diffs mode-only except .env/.gitignore). This existed only
  locally and was never pushed.
- Repaired the local history (nothing unpushed was published, so no
  force-push was involved): reset to d9c15ef, normalized all modes back
  to 100644, re-created the Task 9 worklog commit and the Task 10
  commit (same message/content minus .env and the mode noise). .env is
  untracked again and covered by .gitignore; working tree clean.
- Restored the sandbox-wiped toolchain (cmake 4.4.3 via pip) and
  re-verified on the repaired tree: from-scratch Release build with
  0 warnings, 437 tests / 0 failed assertions, --list note correct
  (supercastpro: PVR2 scanout + TA subset + AICA ARM7/PCM voices).
- Push reconciliation with a fresh repo-scoped token: token-checked
  ls-remote showed remote main at 48951f2 - session 9's push HAD
  landed; the local origin/main tracking ref was simply never updated
  by that one-shot-URL push (stale bookkeeping, not a lost push; the
  earlier tracking-reflog inference "remote = bf3ec04" was wrong).
  Reset local onto 48951f2, cherry-picked the Task 10 commit (2c9122b,
  tree byte-identical to the rebuilt b923038), also normalizing the
  worklog.md mode (755 -> 644) that had slipped into the original
  48951f2, then re-applied this entry with corrected facts.

Stage Summary:
- Pushed with the one-shot token URL (token used only in throwaway
  URLs, never written to .git/config; x-oauth-scopes: repo only).
  Remote main == local HEAD; published history: 48951f2 + 2c9122b
  (supercastpro TA subset + AICA ARM7/PCM voices, 437 tests) + this
  worklog entry.
- supercastpro keeps registry status "partial" but now covers the two
  biggest homebrew unlocks: flat-shaded 3D submission through the TA
  parameter stream and ARM7 sound drivers on AICA.
