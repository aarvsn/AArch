# emu-fw — lightweight multi-system emulator framework
#
# CMake build. Options:
#   EMU_BUILD_MGBX / EMU_BUILD_BEATLE_NES_REDUX / EMU_BUILD_SUPERSNES / EMU_BUILD_MGBAX
#       Build individual cores (all default ON; a "minimal" build is achieved by
#       disabling all but one core).
#   EMU_BUILD_TESTS / EMU_BUILD_CLI       Build test binaries / command line tool (default ON)
#   EMU_WERROR                            Treat warnings as errors (used in CI)
#   EMU_SANITIZE_ADDRESS / EMU_SANITIZE_UNDEFINED
#       Build with ASan / UBSan (gcc/clang only)
#
# Warnings: -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes.
# Warnings are fixed at their source; no global suppression.
cmake_minimum_required(VERSION 3.16)

project(emu-fw
    VERSION 0.1.0
    DESCRIPTION "Lightweight, modular multi-system emulator framework"
    LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(EMU_BUILD_MGBX            "Build mgbx Game Boy core"             ON)
option(EMU_BUILD_BEATLE_NES_REDUX "Build beatle-nes-redux NES core"     ON)
option(EMU_BUILD_SUPERSNES       "Build supersnes SNES core"            ON)
option(EMU_BUILD_MGBAX           "Build mgbax GBA core"                 ON)
option(EMU_BUILD_TESTS           "Build tests"                          ON)
option(EMU_BUILD_CLI             "Build emu-cli command line tool"      ON)
option(EMU_WERROR                "Treat compiler warnings as errors"    OFF)
option(EMU_SANITIZE_ADDRESS      "Enable AddressSanitizer"              OFF)
option(EMU_SANITIZE_UNDEFINED    "Enable UndefinedBehaviorSanitizer"    OFF)

if(EMU_BUILD_TESTS)
    enable_testing()
endif()

# ---- warning / sanitizer configuration -------------------------------------

set(EMU_WARNING_FLAGS -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes)
if(EMU_WERROR)
    list(APPEND EMU_WARNING_FLAGS -Werror)
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

# ---- common -----------------------------------------------------------------

add_library(emu_common STATIC
    src/common/util.c
)
target_include_directories(emu_common PUBLIC include)
target_include_directories(emu_common PRIVATE src)
emu_apply_flags(emu_common)

# ---- cores ------------------------------------------------------------------

set(EMU_CORE_TARGETS "")

if(EMU_BUILD_MGBX)
    add_library(core_mgbx STATIC
        src/mgbx/mgbx.c
        src/mgbx/cpu.c
        src/mgbx/mem.c
        src/mgbx/cart.c
        src/mgbx/ppu.c
        src/mgbx/timer.c
        src/mgbx/joypad.c
        src/mgbx/apu.c
    )
    target_link_libraries(core_mgbx PUBLIC emu_common)
    target_include_directories(core_mgbx PRIVATE src)
    emu_apply_flags(core_mgbx)
    list(APPEND EMU_CORE_TARGETS core_mgbx)
endif()

if(EMU_BUILD_BEATLE_NES_REDUX)
    add_library(core_beatle_nes_redux STATIC
        src/beatle-nes-redux/nes.c
        src/beatle-nes-redux/cpu.c
        src/beatle-nes-redux/bus.c
        src/beatle-nes-redux/cart.c
        src/beatle-nes-redux/ppu.c
        src/beatle-nes-redux/apu.c
    )
    target_link_libraries(core_beatle_nes_redux PUBLIC emu_common)
    target_include_directories(core_beatle_nes_redux PRIVATE src)
    emu_apply_flags(core_beatle_nes_redux)
    list(APPEND EMU_CORE_TARGETS core_beatle_nes_redux)
endif()

if(EMU_BUILD_SUPERSNES)
    add_library(core_supersnes STATIC
        src/supersnes/snes.c
        src/supersnes/cpu.c
        src/supersnes/mem.c
        src/supersnes/cart.c
        src/supersnes/ppu.c
        src/supersnes/dma.c
        src/supersnes/apu.c
    )
    target_link_libraries(core_supersnes PUBLIC emu_common)
    target_include_directories(core_supersnes PRIVATE src)
    emu_apply_flags(core_supersnes)
    list(APPEND EMU_CORE_TARGETS core_supersnes)
endif()

if(EMU_BUILD_MGBAX)
    add_library(core_mgbax STATIC
        src/mgbax/gba.c
        src/mgbax/cpu.c
        src/mgbax/mem.c
        src/mgbax/cart.c
        src/mgbax/ppu.c
        src/mgbax/timer.c
        src/mgbax/dma.c
        src/mgbax/apu.c
        src/mgbax/swi.c
    )
    target_link_libraries(core_mgbax PUBLIC emu_common)
    target_include_directories(core_mgbax PRIVATE src)
    emu_apply_flags(core_mgbax)
    list(APPEND EMU_CORE_TARGETS core_mgbax)
endif()

# ---- CLI --------------------------------------------------------------------

if(EMU_BUILD_CLI)
    if(NOT EMU_CORE_TARGETS)
        message(FATAL_ERROR "emu-cli requires at least one core to be enabled")
    endif()
    add_executable(emu-cli tools/emu-cli/main.c)
    if(EMU_BUILD_MGBX)
        target_compile_definitions(emu-cli PRIVATE EMU_BUILD_MGBX=1)
    endif()
    if(EMU_BUILD_BEATLE_NES_REDUX)
        target_compile_definitions(emu-cli PRIVATE EMU_BUILD_BEATLE_NES_REDUX=1)
    endif()
    if(EMU_BUILD_SUPERSNES)
        target_compile_definitions(emu-cli PRIVATE EMU_BUILD_SUPERSNES=1)
    endif()
    if(EMU_BUILD_MGBAX)
        target_compile_definitions(emu-cli PRIVATE EMU_BUILD_MGBAX=1)
    endif()
    target_link_libraries(emu-cli PRIVATE emu_common ${EMU_CORE_TARGETS})
    emu_apply_flags(emu-cli)
endif()

# ---- tests --------------------------------------------------------------------

if(EMU_BUILD_TESTS)
    add_subdirectory(tests)
endif()
