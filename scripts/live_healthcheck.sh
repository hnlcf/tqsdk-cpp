#!/usr/bin/env bash
# 检查 live gw/node 的 HTTP 健康、ready 和交易阶段。
#
# 用法:
#   ./scripts/live_healthcheck.sh
#   ./scripts/live_healthcheck.sh --wait-ready 120
#   ./scripts/live_healthcheck.sh --require-trading
#
# 返回:
#   0  健康检查通过
#   1  HTTP/healthz 失败
#   2  --wait-ready 超时
#   3  --require-trading 但当前不可交易

set -Eeuo pipefail

BASE_URL="${TQSDK_NODE_URL:-http://127.0.0.1:18081}"
WAIT_READY=0
REQUIRE_TRADING=0
TIMEOUT=3.0

usage() {
    sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
    --base-url)
        BASE_URL="${2:?--base-url requires value}"
        shift 2
        ;;
    --wait-ready)
        WAIT_READY="${2:?--wait-ready requires seconds}"
        shift 2
        ;;
    --require-trading)
        REQUIRE_TRADING=1
        shift
        ;;
    --timeout)
        TIMEOUT="${2:?--timeout requires seconds}"
        shift 2
        ;;
    -h|--help)
        usage
        exit 0
        ;;
    *)
        echo "unknown argument: $1" >&2
        usage >&2
        exit 2
        ;;
    esac
done

BASE_URL="${BASE_URL%/}"
curl_json() {
    curl -fsS --max-time "$TIMEOUT" "$BASE_URL$1"
}

echo "base: $BASE_URL"
if ! health="$(curl_json /healthz 2>/dev/null)"; then
    echo "healthz: FAIL (node 不可达: $BASE_URL)" >&2
    exit 1
fi
if ! jq -e '.ok == true' >/dev/null <<<"$health"; then
    echo "healthz: FAIL body=$health" >&2
    exit 1
fi
echo "healthz: PASS"

ready_deadline=$((SECONDS + WAIT_READY))
while :; do
    ready_body="$(curl -sS --max-time "$TIMEOUT" -w '\n%{http_code}' "$BASE_URL/readyz" 2>/dev/null || true)"
    ready_code="$(tail -n1 <<<"$ready_body")"
    ready_json="$(sed '$d' <<<"$ready_body")"
    if [[ "$ready_code" == "200" ]] && jq -e '.ready == true' >/dev/null 2>&1 <<<"$ready_json"; then
        echo "readyz: PASS"
        break
    fi
    if (( SECONDS >= ready_deadline )); then
        if (( WAIT_READY > 0 )); then
            echo "readyz: TIMEOUT body=$ready_json" >&2
            exit 2
        fi
        echo "readyz: NOT_READY body=$ready_json"
        break
    fi
    sleep 1
done

status_body="$(curl_json /trading-status 2>/dev/null || true)"
if [[ -n "$status_body" ]]; then
    phase="$(jq -r '.phase // "unknown"' <<<"$status_body")"
    trading="$(jq -r '.trading // false' <<<"$status_body")"
    session="$(jq -r '.session // "unknown"' <<<"$status_body")"
    expected="$(jq -r '.expected_disconnect // false' <<<"$status_body")"
    echo "trading-status: phase=$phase session=$session trading=$trading expected_disconnect=$expected"
    if (( REQUIRE_TRADING )) && [[ "$trading" != "true" ]]; then
        echo "trading-status: FAIL (--require-trading)" >&2
        exit 3
    fi
else
    echo "trading-status: UNAVAILABLE"
    if (( REQUIRE_TRADING )); then
        exit 3
    fi
fi

echo "healthcheck: PASS"
