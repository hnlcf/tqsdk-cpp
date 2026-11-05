#!/usr/bin/env bash
# 次日开盘后自动启动隔离的 CTP gw/node，并执行平昨测试。
# 由用户/at/systemd 在结算后的交易日调用；退出时只关闭自己启动的进程。
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
NODE_URL=${NODE_URL:-http://127.0.0.1:28083}
GW_CONFIG=${GW_CONFIG:-/tmp/ctp_overnight_close_gw.toml}
NODE_CONFIG=${NODE_CONFIG:-/tmp/ctp_overnight_close_node.toml}
GW_BIN=${GW_BIN:-$ROOT/build-ui/tqsdk-gw}
NODE_BIN=${NODE_BIN:-$ROOT/build-ui/tqsdk-node}
LOG=${LOG:-$ROOT/artifacts/ctp_overnight_close_runner.log}
mkdir -p "$(dirname "$LOG")"
exec >>"$LOG" 2>&1
echo "[$(date -Is)] start close-yesterday runner"

GW=
NODE=
cleanup() {
    [[ -n "$NODE" ]] && kill -TERM "$NODE" 2>/dev/null || true
    [[ -n "$GW" ]] && kill -TERM "$GW" 2>/dev/null || true
    sleep 2
    [[ -n "$NODE" ]] && kill -KILL "$NODE" 2>/dev/null || true
    [[ -n "$GW" ]] && kill -KILL "$GW" 2>/dev/null || true
}
trap cleanup EXIT

"$GW_BIN" -c "$GW_CONFIG" &
GW=$!
for _ in $(seq 1 100); do
    if ss -ltn "sport = :$(awk -F= '/^[[:space:]]*tcp_port[[:space:]]*=/{gsub(/[[:space:]]/,"",$2); print $2; exit}' "$GW_CONFIG")" 2>/dev/null | grep -q LISTEN; then
        break
    fi
    if ! kill -0 "$GW" 2>/dev/null; then
        echo "gateway exited before listening" >&2
        exit 1
    fi
    sleep 0.1
done
"$NODE_BIN" -c "$NODE_CONFIG" &
NODE=$!
for i in $(seq 1 120); do
    code=$(curl -s -m 2 -o /tmp/ctp_overnight_ready.json -w '%{http_code}' "$NODE_URL/readyz" || true)
    if [[ "$code" == "200" ]]; then
        echo "ready after ${i}s: $(cat /tmp/ctp_overnight_ready.json)"
        break
    fi
    if ! kill -0 "$NODE" 2>/dev/null; then
        echo "node exited before ready" >&2
        exit 1
    fi
    sleep 1
done
NODE_URL="$NODE_URL" "$ROOT/scripts/ctp_close_yesterday_test.sh"
RC=$?
echo "[$(date -Is)] close-yesterday runner finished rc=$RC"
exit "$RC"
