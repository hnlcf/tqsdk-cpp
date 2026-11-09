#!/usr/bin/env bash
# Local CI entrypoint: compile, C++ tests, web checks, optionally sanitizers.
#
#   ./scripts/ci_local.sh
#   ./scripts/ci_local.sh --asan
#   ./scripts/ci_local.sh --tsan
#   ./scripts/ci_local.sh --asan --tsan
#
# This is intentionally dependency-light; CI runners can call the same script
# after installing cmake/vcpkg/pnpm.

set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${TQSDK_CI_BUILD_DIR:-$ROOT/build-ui}"
RUN_ASAN=0
RUN_TSAN=0

while [[ $# -gt 0 ]]; do
    case "$1" in
    --asan) RUN_ASAN=1; shift ;;
    --tsan) RUN_TSAN=1; shift ;;
    -h|--help)
        sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

echo "== build =="
cmake --build "$BUILD_DIR" --target tqsdk_tests tqsdk-gw tqsdk-node -j 4

echo "== C++ tests =="
ctest --test-dir "$BUILD_DIR" --output-on-failure -j 2

echo "== web =="
(
    cd "$ROOT/web"
    pnpm typecheck
    pnpm test
    pnpm build
)

if (( RUN_ASAN )); then
    echo "== ASan =="
    "$ROOT/scripts/p6_asan_job.sh"
fi
if (( RUN_TSAN )); then
    echo "== TSan =="
    "$ROOT/scripts/tsan_job.sh"
fi

echo "ci_local: PASS"
