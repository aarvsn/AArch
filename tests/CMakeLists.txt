# emu-fw tests: one runner binary containing all enabled core suites.
# Suites print PASS/FAIL per test; exit code = failed assertions.

set(EMU_TEST_SOURCES
    test_main.c
    testutil.c
    common/api_lifecycle.c
    common/state.c
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
        mgbax/state.c
    )
    list(APPEND EMU_TEST_CORE_DEFS EMU_BUILD_MGBAX=1)
    list(APPEND EMU_TEST_CORE_LIBS core_mgbax)
endif()

add_executable(emu-tests ${EMU_TEST_SOURCES})
target_compile_definitions(emu-tests PRIVATE ${EMU_TEST_CORE_DEFS})
target_include_directories(emu-tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(emu-tests PRIVATE emu_common ${EMU_TEST_CORE_LIBS})
emu_apply_flags(emu-tests)

add_test(NAME emu-tests COMMAND emu-tests)
