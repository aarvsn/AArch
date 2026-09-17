#!/data/data/com.termux/files/usr/bin/sh
# Build and (optionally) install AArch natively on Android via Termux.
#
# No root and no Android NDK required: Termux ships clang + cmake, and the
# AArch cores are portable C11 with no dependencies, so the same source tree
# that builds on desktop Linux builds in place on the device.
#
# Usage (inside Termux):
#   pkg install -y clang cmake make
#   sh scripts/build-termux.sh            # build + run the test suite
#   sh scripts/build-termux.sh --install  # also install `aarch` to $PREFIX/bin
#
# Notes:
#   - Termux itself requires a recent Android release; the NDK route in
#     README.md ("Android 6.0+ (NDK)") is the supported path for API 23+
#     devices. This script targets the Termux environment on the device.
#   - ROMs are conventionally placed under ~/storage/shared/ after running
#     `termux-setup-storage` once.

set -eu

cd "$(dirname "$0")/.."

BUILD_DIR=build-termux

echo "== AArch Termux build =="
command -v clang >/dev/null 2>&1 || {
    echo "error: clang not found. Run: pkg install clang cmake make" >&2
    exit 1
}
command -v cmake >/dev/null 2>&1 || {
    echo "error: cmake not found. Run: pkg install cmake" >&2
    exit 1
}

NPROC=$(nproc 2>/dev/null || echo 2)

cmake -S . -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_INSTALL_PREFIX="$PREFIX"
cmake --build "$BUILD_DIR" -j "$NPROC"

echo "== running test suite =="
ctest --test-dir "$BUILD_DIR" --output-on-failure

BIN="$BUILD_DIR/aarch"
echo "== smoke check =="
"$BIN" --list

if [ "${1:-}" = "--install" ]; then
    cmake --install "$BUILD_DIR"
    echo "installed: $PREFIX/bin/aarch"
fi

echo "done: $BIN"
