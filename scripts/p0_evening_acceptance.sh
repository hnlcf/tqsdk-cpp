#!/usr/bin/env bash
# 等待 SimNow ready/trading 后执行 P0 探针。
#
# 只读:
#   ./scripts/p0_evening_acceptance.sh
#
# 写场景（显式确认）:
#   ./scripts/p0_evening_acceptance.sh --write --yes
#
# 额外 TargetPosTask:
#   ./scripts/p0_evening_acceptance.sh --write --yes --target-write
#
# 若需要无限等待 ready，可设置 TQSDK_READY_WAIT=0；
# 默认每次最多等待 4 小时，然后由循环重新检查。

set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASE_URL="${TQSDK_NODE_URL:-http://127.0.0.1:18081}"
READY_WAIT="${TQSDK_READY_WAIT:-14400}"
POLL_SECONDS="${TQSDK_READY_POLL:-30}"
HEALTH="$ROOT/scripts/live_healthcheck.sh"
ACCEPT="$ROOT/scripts/p0_simnow_acceptance.py"

while :; do
    echo "waiting for ready/trading at $BASE_URL ..."
    set +e
    "$HEALTH" --base-url "$BASE_URL" --wait-ready "$READY_WAIT" --require-trading
    rc=$?
    set -e
    if (( rc == 0 )); then
        break
    fi
    if (( rc != 2 && rc != 3 )); then
        echo "healthcheck failed rc=$rc" >&2
        exit "$rc"
    fi
    sleep "$POLL_SECONDS"
done

echo "P0 acceptance starting"
exec python3 "$ACCEPT" --node-url "$BASE_URL" "$@"
