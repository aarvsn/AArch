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
