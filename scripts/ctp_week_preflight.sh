#!/usr/bin/env bash
# CTP 交易周测试前置检查（只读）。
#
# 用法：
#   ./scripts/ctp_week_preflight.sh
#   NODE_URL=http://127.0.0.1:18081 ./scripts/ctp_week_preflight.sh
#   ALLOW_RUNNING=1 ./scripts/ctp_week_preflight.sh
#
# 退出码：0 通过；1 有阻塞项。
set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NODE_URL="${NODE_URL:-http://127.0.0.1:18081}"
BUILD_DIR="${TQSDK_BUILD_DIR:-$ROOT/build-ui}"
CONFIG_FILE="${TQSDK_LIVE_CONFIG:-$ROOT/config/live.local.env}"
GW_BIN="${GW_BIN:-$BUILD_DIR/tqsdk-gw}"
NODE_BIN="${NODE_BIN:-$BUILD_DIR/tqsdk-node}"
ALLOW_RUNNING="${ALLOW_RUNNING:-0}"
errors=0
warnings=0

ok() { printf 'PASS  %s\n' "$*"; }
warn() { printf 'WARN  %s\n' "$*"; warnings=$((warnings + 1)); }
fail() { printf 'FAIL  %s\n' "$*"; errors=$((errors + 1)); }

printf 'CTP week preflight: %s\n\n' "$(date -Is)"

for f in "$GW_BIN" "$NODE_BIN"; do
    if [[ -x "$f" ]]; then ok "binary $f"; else fail "missing executable $f"; fi
done

if [[ -f "$CONFIG_FILE" ]]; then
    ok "live config $CONFIG_FILE"
else
    warn "live config not found: $CONFIG_FILE (env vars may provide credentials)"
fi

for var in CTP_BROKER_ID CTP_USER_ID CTP_PASSWORD; do
    if [[ -n "${!var:-}" ]]; then
        ok "env $var is set"
    else
        warn "env $var is empty (may be in $CONFIG_FILE)"
    fi
done

if pgrep -af 'tqsdk-(gw|node)' | grep -v 'ctp_week_preflight' >/dev/null 2>&1; then
    if [[ "$ALLOW_RUNNING" == "1" ]]; then
        warn "existing gw/node processes detected (ALLOW_RUNNING=1)"
    else
        fail "existing gw/node processes detected; stop them before week soak"
    fi
else
    ok "no existing gw/node process"
fi

if command -v jq >/dev/null 2>&1; then ok "jq available"; else fail "jq is required"; fi

if curl -fsS -m 3 "$NODE_URL/healthz" >/dev/null 2>&1; then
    ok "node healthz"
    READY="$(curl -sS -m 5 "$NODE_URL/readyz" || true)"
    printf '      readyz: %s\n' "$READY"
    HOLIDAY="$(curl -sS -m 5 "$NODE_URL/holiday/status" || true)"
    printf '      holiday: %s\n' "$HOLIDAY"
else
    warn "node not running at $NODE_URL (expected before start)"
fi

printf '\nsummary: %d error(s), %d warning(s)\n' "$errors" "$warnings"
if (( errors > 0 )); then
    exit 1
fi
exit 0
