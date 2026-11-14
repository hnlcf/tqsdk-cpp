#!/usr/bin/env bash
# ASan build + core tests. Output stays under artifacts/asan by default.
set -u

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD=${TQSDK_ASAN_BUILD_DIR:-$ROOT/artifacts/asan/build}
VCPKG_ROOT=${VCPKG_ROOT:-/home/hnlcf/dev/vcpkg}
LOG=${TQSDK_ASAN_LOG:-$ROOT/artifacts/asan/asan.log}
mkdir -p "$(dirname "$LOG")" "$BUILD"
exec >>"$LOG" 2>&1
echo "[$(date -Is)] ASan build start root=$ROOT build=$BUILD"
echo "VCPKG_ROOT=$VCPKG_ROOT"

if ! command -v cmake >/dev/null || ! command -v clang++ >/dev/null; then
    echo "cmake/clang++ missing"
    exit 2
fi

cmake -S "$ROOT" -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address -fuse-ld=mold" \
    -DCMAKE_SHARED_LINKER_FLAGS="-fuse-ld=mold" \
    -DTQSDK_ENABLE_CTP=ON \
    -DTQSDK_BUILD_TESTS=ON \
    -DTQSDK_BUILD_EXAMPLES=OFF
rc=$?
echo "configure_rc=$rc"
((rc == 0)) || exit "$rc"

cmake --build "$BUILD" --target tqsdk_tests -j 4
rc=$?
echo "build_rc=$rc"
((rc == 0)) || exit "$rc"

ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1 \
    ctest --test-dir "$BUILD" --output-on-failure
rc=$?
echo "asan_ctest_rc=$rc"
echo "[$(date -Is)] ASan finished rc=$rc"
exit "$rc"
