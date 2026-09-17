# Cross-compile AArch for Windows 7+ from a Linux host using MinGW-w64.
# Usage:
#   cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=toolchains/mingw-w64.cmake \
#         -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-win -j
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

# Windows 7 is the minimum supported release.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
