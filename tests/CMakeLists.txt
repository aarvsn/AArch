# emu-fw tests: one runner binary containing all enabled core suites.
# Suites print PASS/FAIL per test; exit code = failed assertions.

set(EMU_TEST_SOURCES
    test_main.c
    testutil.c
    common/api_lifecycle.c
    common/state.c
    common/romdetect.c
    common/cores.c
    sh2/sh2.c
    ms32/x32.c
)

set(EMU_TEST_CORE_DEFS "")
set(EMU_TEST_CORE_LIBS "")

if(EMU_BUILD_MGBX)
    list(APPEND EMU_TEST_SOURCES
        mgbx/cpu.c
        mgbx/cart.c
        mgbx/mem.c
        mgbx/ppu.c
        mgbx/timer.c
        mgbx/apu.c
        mgbx/state.c
    )
    list(APPEND EMU_TEST_CORE_DEFS EMU_BUILD_MGBX=1)
    list(APPEND EMU_TEST_CORE_LIBS core_mgbx)
endif()

if(EMU_BUILD_BEATLE_NES_REDUX)
    list(APPEND EMU_TEST_SOURCES
        nes/cpu.c
        nes/cart.c
        nes/ppu.c
        nes/apu.c
        nes/state.c
    )
    list(APPEND EMU_TEST_CORE_DEFS EMU_BUILD_BEATLE_NES_REDUX=1)
    list(APPEND EMU_TEST_CORE_LIBS core_beatle_nes_redux)
endif()

if(EMU_BUILD_SUPERSNES)
    list(APPEND EMU_TEST_SOURCES
        supersnes/cpu.c
        supersnes/cart.c
        supersnes/dma.c
        supersnes/hdma.c
        supersnes/ppu.c
        supersnes/state.c
    )
    list(APPEND EMU_TEST_CORE_DEFS EMU_BUILD_SUPERSNES=1)
    list(APPEND EMU_TEST_CORE_LIBS core_supersnes)
endif()

if(EMU_BUILD_MGBAX)
    list(APPEND EMU_TEST_SOURCES
        mgbax/arm.c
        mgbax/thumb.c
        mgbax/mem.c
        mgbax/timer.c
        mgbax/dma.c
        mgbax/ppu.c
        mgbax/apu.c
        mgbax/state.c
    )
    list(APPEND EMU_TEST_CORE_DEFS EMU_BUILD_MGBAX=1)
    list(APPEND EMU_TEST_CORE_LIBS core_mgbax)
endif()

if(EMU_BUILD_FINALBURN)
    list(APPEND EMU_TEST_SOURCES
        finalburn/fbh.c
        finalburn/m68k.c
        finalburn/z80.c
        finalburn/vdp.c
        finalburn/audio.c
        finalburn/state.c
    )
    list(APPEND EMU_TEST_CORE_DEFS EMU_BUILD_FINALBURN=1)
    list(APPEND EMU_TEST_CORE_LIBS core_finalburn)
endif()

if(EMU_BUILD_BEATLE_PSX)
    list(APPEND EMU_TEST_SOURCES
        psx/psxh.c
        psx/cpu.c
        psx/gte.c
        psx/gpu.c
        psx/state.c
    )
    list(APPEND EMU_TEST_CORE_DEFS EMU_BUILD_BEATLE_PSX=1)
    list(APPEND EMU_TEST_CORE_LIBS core_beatle_psx)
endif()

if(EMU_BUILD_MDS_A)
    list(APPEND EMU_TEST_CORE_DEFS EMU_BUILD_MDS_A=1)
    list(APPEND EMU_TEST_CORE_LIBS core_mds_a)
endif()

if(EMU_BUILD_MS_32)
    list(APPEND EMU_TEST_CORE_DEFS EMU_BUILD_MS_32=1)
    list(APPEND EMU_TEST_CORE_LIBS core_ms_32)
endif()

if(EMU_BUILD_SUPERSATURN)
    list(APPEND EMU_TEST_SOURCES saturn/saturn.c)
    list(APPEND EMU_TEST_CORE_DEFS EMU_BUILD_SUPERSATURN=1)
    list(APPEND EMU_TEST_CORE_LIBS core_supersaturn)
endif()

if(EMU_BUILD_M64_B)
    list(APPEND EMU_TEST_CORE_DEFS EMU_BUILD_M64_B=1)
    list(APPEND EMU_TEST_CORE_LIBS core_m64_b)
endif()

if(EMU_BUILD_SUPERCASTPRO)
    list(APPEND EMU_TEST_CORE_DEFS EMU_BUILD_SUPERCASTPRO=1)
    list(APPEND EMU_TEST_CORE_LIBS core_supercastpro)
endif()

add_executable(emu-tests ${EMU_TEST_SOURCES})
target_compile_definitions(emu-tests PRIVATE ${EMU_TEST_CORE_DEFS})
target_include_directories(emu-tests PRIVATE ${CMAKE_SOURCE_DIR}/tests ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(emu-tests PRIVATE emu_common ${EMU_TEST_CORE_LIBS})
emu_apply_flags(emu-tests)

add_test(NAME emu-tests COMMAND emu-tests)
