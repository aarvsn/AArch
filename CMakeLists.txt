# AArch — multi-system CLI emulator framework
#
# CMake build. Options:
#   EMU_BUILD_MGBX / EMU_BUILD_BEATLE_NES_REDUX / EMU_BUILD_SUPERSNES /
#   EMU_BUILD_MGBAX / EMU_BUILD_FINALBURN / EMU_BUILD_BEATLE_PSX /
#   EMU_BUILD_MDS_A / EMU_BUILD_MS_32 / EMU_BUILD_SUPERSATURN /
#   EMU_BUILD_M64_B / EMU_BUILD_SUPERCASTPRO
#       Build individual cores (all default ON; a "minimal" build is achieved
#       by disabling all but one core).
#   EMU_BUILD_TESTS / EMU_BUILD_CLI   Build test binaries / aarch CLI (default ON)
#   EMU_WERROR                        Treat warnings as errors (used in CI)
#   EMU_SANITIZE_ADDRESS / EMU_SANITIZE_UNDEFINED
#       Build with ASan / UBSan (gcc/clang only)
#
# Platform targets:
#   Windows 7+    MinGW-w64 or MSVC 2019+ (C11); _WIN32_WINNT pinned to 0x0601
#   macOS 10.12+  any C11 toolchain; deployment target defaults to 10.12
#   Linux         any C11 toolchain
#   Android 6.0+  NDK toolchain file, ANDROID_PLATFORM=android-23
#                 (see README.md "Building on Android")
#
# Warnings: GNU/Clang: -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes;
# MSVC: /W4. Warnings are fixed at their source; no global suppression.
cmake_minimum_required(VERSION 3.16)

project(AArch
    VERSION 0.3.0
    DESCRIPTION "AArch: lightweight, modular multi-system CLI emulator"
    LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(EMU_BUILD_MGBX            "Build mgbx Game Boy core"             ON)
option(EMU_BUILD_BEATLE_NES_REDUX "Build beatle-nes-redux NES core"     ON)
option(EMU_BUILD_SUPERSNES       "Build supersnes SNES core"            ON)
option(EMU_BUILD_MGBAX           "Build mgbax GBA core"                 ON)
option(EMU_BUILD_FINALBURN       "Build finalburn Genesis core"         ON)
option(EMU_BUILD_BEATLE_PSX      "Build beatle-psx skeleton core"       ON)
option(EMU_BUILD_MDS_A           "Build mds-a skeleton core"            ON)
option(EMU_BUILD_MS_32           "Build ms-32 skeleton core"            ON)
option(EMU_BUILD_SUPERSATURN     "Build supersaturn skeleton core"      ON)
option(EMU_BUILD_M64_B           "Build m64-b skeleton core"            ON)
option(EMU_BUILD_SUPERCASTPRO    "Build supercastpro skeleton core"     ON)
option(EMU_BUILD_TESTS           "Build tests"                          ON)
option(EMU_BUILD_CLI             "Build aarch command line tool"        ON)
option(EMU_WERROR                "Treat compiler warnings as errors"    OFF)
option(EMU_SANITIZE_ADDRESS      "Enable AddressSanitizer"              OFF)
option(EMU_SANITIZE_UNDEFINED    "Enable UndefinedBehaviorSanitizer"    OFF)

if(EMU_BUILD_TESTS)
    enable_testing()
endif()

# ---- platform configuration ---------------------------------------------------

if(WIN32)
    # Windows 7 is the minimum supported release.
    add_compile_definitions(_WIN32_WINNT=0x0601 WINVER=0x0601)
endif()

if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "Android")
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        # Minimum supported macOS release (El Capitan).
        set(CMAKE_OSX_DEPLOYMENT_TARGET 10.12 CACHE STRING "macOS deployment target" FORCE)
    endif()
endif()

# ---- warning / sanitizer configuration ---------------------------------------

set(EMU_WARNING_FLAGS "")
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set(EMU_WARNING_FLAGS -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes)
elseif(MSVC)
    set(EMU_WARNING_FLAGS /W4)
endif()
if(EMU_WERROR)
    if(MSVC)
        list(APPEND EMU_WARNING_FLAGS /WX)
    else()
        list(APPEND EMU_WARNING_FLAGS -Werror)
    endif()
endif()

set(EMU_SAN_FLAGS "")
if(EMU_SANITIZE_ADDRESS)
    list(APPEND EMU_SAN_FLAGS -fsanitize=address -fno-omit-frame-pointer)
endif()
if(EMU_SANITIZE_UNDEFINED)
    list(APPEND EMU_SAN_FLAGS -fsanitize=undefined -fno-sanitize-recover=all
                              -fno-sanitize=shift-base)
endif()

function(emu_apply_flags target)
    target_compile_options(${target} PRIVATE ${EMU_WARNING_FLAGS} ${EMU_SAN_FLAGS})
    if(EMU_SAN_FLAGS)
        target_link_options(${target} PRIVATE ${EMU_SAN_FLAGS})
    endif()
endfunction()

# ---- common -------------------------------------------------------------------

add_library(emu_common STATIC
    src/common/util.c
    src/common/registry.c
    src/common/skeleton.c
)
target_compile_definitions(emu_common PRIVATE
    $<$<BOOL:${EMU_BUILD_MGBX}>:EMU_BUILD_MGBX=1>
    $<$<BOOL:${EMU_BUILD_BEATLE_NES_REDUX}>:EMU_BUILD_BEATLE_NES_REDUX=1>
    $<$<BOOL:${EMU_BUILD_SUPERSNES}>:EMU_BUILD_SUPERSNES=1>
    $<$<BOOL:${EMU_BUILD_MGBAX}>:EMU_BUILD_MGBAX=1>
    $<$<BOOL:${EMU_BUILD_FINALBURN}>:EMU_BUILD_FINALBURN=1>
    $<$<BOOL:${EMU_BUILD_BEATLE_PSX}>:EMU_BUILD_BEATLE_PSX=1>
    $<$<BOOL:${EMU_BUILD_MDS_A}>:EMU_BUILD_MDS_A=1>
    $<$<BOOL:${EMU_BUILD_MS_32}>:EMU_BUILD_MS_32=1>
    $<$<BOOL:${EMU_BUILD_SUPERSATURN}>:EMU_BUILD_SUPERSATURN=1>
    $<$<BOOL:${EMU_BUILD_M64_B}>:EMU_BUILD_M64_B=1>
    $<$<BOOL:${EMU_BUILD_SUPERCASTPRO}>:EMU_BUILD_SUPERCASTPRO=1>
)
target_include_directories(emu_common PUBLIC include)
target_include_directories(emu_common PRIVATE src)
emu_apply_flags(emu_common)

# ---- cores --------------------------------------------------------------------

set(EMU_CORE_TARGETS "")

macro(emu_add_core name dir)
    add_library(core_${name} STATIC ${ARGN})
    target_link_libraries(core_${name} PUBLIC emu_common)
    target_include_directories(core_${name} PRIVATE src)
    emu_apply_flags(core_${name})
    list(APPEND EMU_CORE_TARGETS core_${name})
endmacro()

if(EMU_BUILD_MGBX)
    emu_add_core(mgbx mgbx
        src/mgbx/mgbx.c src/mgbx/cpu.c src/mgbx/mem.c src/mgbx/cart.c
        src/mgbx/ppu.c src/mgbx/timer.c src/mgbx/joypad.c src/mgbx/apu.c)
endif()

if(EMU_BUILD_BEATLE_NES_REDUX)
    emu_add_core(beatle_nes_redux beatle-nes-redux
        src/beatle-nes-redux/nes.c src/beatle-nes-redux/cpu.c
        src/beatle-nes-redux/bus.c src/beatle-nes-redux/cart.c
        src/beatle-nes-redux/ppu.c src/beatle-nes-redux/apu.c)
endif()

if(EMU_BUILD_SUPERSNES)
    emu_add_core(supersnes supersnes
        src/supersnes/snes.c src/supersnes/cpu.c src/supersnes/mem.c
        src/supersnes/cart.c src/supersnes/ppu.c src/supersnes/dma.c
        src/supersnes/apu.c)
endif()

if(EMU_BUILD_MGBAX)
    emu_add_core(mgbax mgbax
        src/mgbax/gba.c src/mgbax/cpu.c src/mgbax/mem.c src/mgbax/cart.c
        src/mgbax/ppu.c src/mgbax/timer.c src/mgbax/dma.c src/mgbax/apu.c
        src/mgbax/swi.c)
endif()

if(EMU_BUILD_FINALBURN)
    emu_add_core(finalburn finalburn
        src/finalburn/md.c src/finalburn/m68k.c src/finalburn/z80.c
        src/finalburn/vdp.c src/finalburn/psg.c src/finalburn/ym2612.c
        src/finalburn/cart.c)
endif()

if(EMU_BUILD_BEATLE_PSX)
    emu_add_core(beatle_psx beatle-psx src/beatle-psx/psx.c)
endif()
if(EMU_BUILD_MDS_A)
    emu_add_core(mds_a mds-a src/mds-a/ds.c)
endif()
if(EMU_BUILD_MS_32)
    emu_add_core(ms_32 ms-32 src/ms-32/x32.c)
endif()
if(EMU_BUILD_SUPERSATURN)
    emu_add_core(supersaturn supersaturn src/supersaturn/saturn.c)
endif()
if(EMU_BUILD_M64_B)
    emu_add_core(m64_b m64-b src/m64-b/n64.c)
endif()
if(EMU_BUILD_SUPERCASTPRO)
    emu_add_core(supercastpro supercastpro src/supercastpro/dc.c)
endif()

# ---- CLI ----------------------------------------------------------------------

if(EMU_BUILD_CLI)
    if(NOT EMU_CORE_TARGETS)
        message(FATAL_ERROR "aarch requires at least one core to be enabled")
    endif()
    add_executable(aarch tools/aarch/main.c)
    target_link_libraries(aarch PRIVATE emu_common ${EMU_CORE_TARGETS})
    emu_apply_flags(aarch)
endif()

# ---- tests --------------------------------------------------------------------

if(EMU_BUILD_TESTS)
    add_subdirectory(tests)
endif()
