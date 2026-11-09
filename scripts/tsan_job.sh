#!/usr/bin/env bash
# TSan build + core tests. Kept separate from normal/ASan builds.
set -u

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD=${TQSDK_TSAN_BUILD_DIR:-$ROOT/artifacts/tsan/build}
LOG=${TQSDK_TSAN_LOG:-$ROOT/artifacts/tsan/tsan.log}
VCPKG_ROOT=${VCPKG_ROOT:-/home/hnlcf/dev/vcpkg}
mkdir -p "$(dirname "$LOG")" "$BUILD"
exec >>"$LOG" 2>&1
echo "[$(date -Is)] TSan build start root=$ROOT build=$BUILD"

if ! command -v cmake >/dev/null || ! command -v clang++ >/dev/null; then
    echo "cmake/clang++ missing"
    exit 2
fi

cmake -S "$ROOT" -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread -fuse-ld=mold" \
    -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=thread -fuse-ld=mold" \
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

TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 \
    ctest --test-dir "$BUILD" --output-on-failure
rc=$?
echo "tsan_ctest_rc=$rc"
echo "[$(date -Is)] TSan finished rc=$rc"
exit "$rc"
