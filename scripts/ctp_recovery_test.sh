#!/usr/bin/env bash
# CTP 恢复验收：直接启动 gw/node，分别重启 gateway 和 node，验证 node 存活、
# readyz 恢复以及 account/positions/orders 快照一致。
#
# 用法（需先准备 run/live/gw.toml 和 node.toml，且 18081/14330 未被占用）：
#   ./scripts/ctp_recovery_test.sh
# 可通过环境变量覆盖：
#   NODE_URL=http://127.0.0.1:18081
#   GW_CONFIG=/path/gw.toml NODE_CONFIG=/path/node.toml
#   GW_BIN=./build-ui/tqsdk-gw NODE_BIN=./build-ui/tqsdk-node
#
# 该脚本只做只读查询和进程重启，不下单；退出时清理自己启动的进程。
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
NODE_URL=${NODE_URL:-http://127.0.0.1:18081}
GW_CONFIG=${GW_CONFIG:-$ROOT/run/live/gw.toml}
NODE_CONFIG=${NODE_CONFIG:-$ROOT/run/live/node.toml}
GW_BIN=${GW_BIN:-$ROOT/build-ui/tqsdk-gw}
NODE_BIN=${NODE_BIN:-$ROOT/build-ui/tqsdk-node}
GW_LOG=${GW_LOG:-/tmp/tqsdk_ctp_recovery_gw.log}
NODE_LOG=${NODE_LOG:-/tmp/tqsdk_ctp_recovery_node.log}

if [[ ! -x "$GW_BIN" || ! -x "$NODE_BIN" ]]; then
    echo "missing binary: $GW_BIN or $NODE_BIN" >&2
    exit 2
fi
if [[ ! -f "$GW_CONFIG" || ! -f "$NODE_CONFIG" ]]; then
    echo "missing config: $GW_CONFIG or $NODE_CONFIG" >&2
    exit 2
fi
command -v jq >/dev/null || {
    echo "jq is required" >&2
    exit 2
}
if curl -fsS -m 2 "$NODE_URL/healthz" >/dev/null 2>&1; then
    echo "node already listening at $NODE_URL; stop it first" >&2
    exit 2
fi

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

start_gw() {
    "$GW_BIN" -c "$GW_CONFIG" >>"$GW_LOG" 2>&1 &
    GW=$!
    echo "gw=$GW"
}
start_node() {
    "$NODE_BIN" -c "$NODE_CONFIG" >>"$NODE_LOG" 2>&1 &
    NODE=$!
    echo "node=$NODE"
}
wait_ready() {
    local label=$1 timeout=${2:-90} code
    for i in $(seq 1 "$timeout"); do
        code=$(curl -s -m 2 -o /tmp/tqsdk_ctp_recovery_ready.json -w '%{http_code}' \
            "$NODE_URL/readyz" || true)
        if [[ "$code" == "200" ]]; then
            echo "ready[$label] ${i}s $(jq -c '{ready,md,td}' /tmp/tqsdk_ctp_recovery_ready.json)"
            return 0
        fi
        if [[ -n "$NODE" ]] && ! kill -0 "$NODE" 2>/dev/null; then
            echo "node exited while waiting for $label" >&2
            return 1
        fi
        sleep 1
    done
    echo "timeout waiting for $label" >&2
    return 1
}
snapshot() {
    local tag=$1
    echo "snapshot[$tag]"
    curl -sS -m 5 "$NODE_URL/positions" |
        jq -c '.positions | with_entries(.value |= {volume_long,volume_short,margin})'
    curl -sS -m 5 "$NODE_URL/account" | jq -c '{balance,available,margin}'
    curl -sS -m 5 "$NODE_URL/orders" | jq -c '{orders:(.orders|keys|length)}'
}

rm -f "$GW_LOG" "$NODE_LOG"
start_gw
start_node
wait_ready initial || exit 1
snapshot before

echo "=== gateway restart ==="
kill -TERM "$GW" 2>/dev/null || true
GW=
sleep 3
code=$(curl -s -m 2 -o /tmp/tqsdk_ctp_recovery_down.json -w '%{http_code}' \
    "$NODE_URL/readyz" || true)
echo "after_gw_down http=$code body=$(cat /tmp/tqsdk_ctp_recovery_down.json 2>/dev/null)"
kill -0 "$NODE" 2>/dev/null && echo "node_alive_after_gw_down=1" || {
    echo "node_alive_after_gw_down=0" >&2
    exit 1
}
start_gw
wait_ready after_gw_restart || {
    tail -80 "$NODE_LOG" >&2
    exit 1
}
snapshot after_gw_restart

echo "=== node restart ==="
kill -TERM "$NODE" 2>/dev/null || true
NODE=
sleep 2
start_node
wait_ready after_node_restart || {
    tail -80 "$NODE_LOG" >&2
    exit 1
}
snapshot after_node_restart

echo "=== gw log tail ==="
tail -8 "$GW_LOG"
echo "=== node log tail ==="
tail -15 "$NODE_LOG"
