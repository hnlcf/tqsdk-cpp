#!/usr/bin/env bash
# CTP 报单在途故障测试（真实 SimNow，仅下不会成交的远月/深价限价单）：
#   1. 启动隔离 gw/node；
#   2. 提交一个远离盘口的 BUY LIMIT，确认订单存活；
#   3. 杀掉 gateway，等待 node 保持存活；
#   4. 重启 gateway，确认订单从 CTP/本地缓存恢复；
#   5. 撤单并确认无持仓副作用。
# 默认使用 24330/28081，避免和常规 live 实例冲突。
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
NODE_URL=${NODE_URL:-http://127.0.0.1:28081}
GW_CONFIG=${GW_CONFIG:-/tmp/ctp_order_fault_gw.toml}
NODE_CONFIG=${NODE_CONFIG:-/tmp/ctp_order_fault_node.toml}
GW_BIN=${GW_BIN:-$ROOT/build-ui/tqsdk-gw}
NODE_BIN=${NODE_BIN:-$ROOT/build-ui/tqsdk-node}
SYMBOL=${SYMBOL:-SHFE.rb2701}
GW_LOG=${GW_LOG:-/tmp/ctp_order_fault_gw.log}
NODE_LOG=${NODE_LOG:-/tmp/ctp_order_fault_node.log}

command -v jq >/dev/null || { echo "jq required" >&2; exit 2; }
GW=
NODE=
OID=
cleanup() {
    # 失败/中断时尽量撤销在途测试单，避免留下真实 CTP 挂单。
    if [[ -n "${OID:-}" ]]; then
        curl -sS -m 3 -X DELETE "$NODE_URL/order/$OID" >/dev/null 2>&1 || true
    fi
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
    sleep 0.2
    kill -0 "$GW" 2>/dev/null || { echo "gateway failed to start" >&2; exit 1; }
}
start_node() {
    "$NODE_BIN" -c "$NODE_CONFIG" >>"$NODE_LOG" 2>&1 &
    NODE=$!
}
wait_ready() {
    local label=$1
    for i in $(seq 1 90); do
        code=$(curl -s -m 2 -o /tmp/ctp_order_fault_ready.json -w '%{http_code}' \
            "$NODE_URL/readyz" || true)
        if [[ "$code" == "200" ]]; then
            echo "ready[$label] ${i}s"
            return 0
        fi
        if ! kill -0 "$NODE" 2>/dev/null; then
            echo "node exited" >&2
            return 1
        fi
        sleep 1
    done
    return 1
}
wait_order() {
    local oid=$1
    for _ in $(seq 1 20); do
        curl -sS -m 3 "$NODE_URL/orders" |
            jq -c --arg oid "$oid" '.orders[$oid] // empty'
        sleep 1
    done | tail -1
}

rm -f "$GW_LOG" "$NODE_LOG"
start_gw
start_node
wait_ready initial || exit 1
BEFORE=$(curl -sS -m 5 "$NODE_URL/positions" |
    jq -c --arg s "$SYMBOL" '.positions[$s] // {volume_long:0,volume_short:0}')

QUOTE=$(curl -sS -m 5 "$NODE_URL/quote/$SYMBOL")
LAST=$(printf '%s' "$QUOTE" | jq -r '.last_price // empty')
LOWER=$(printf '%s' "$QUOTE" | jq -r '.lower_limit // empty')
if [[ -z "$LAST" || "$LAST" == "null" ]]; then
    echo "no quote for $SYMBOL" >&2
    exit 1
fi
# 深价买单：低于最新价 50，但必须高于跌停价；不会立即成交。
PRICE=$(awk -v p="$LAST" -v lo="${LOWER:-0}" 'BEGIN { x=p-50; if (lo>0 && x<lo+1) x=lo+1; printf "%.2f", x }')
PAYLOAD=$(jq -n --arg s "$SYMBOL" --argjson p "$PRICE" \
    '{instrument_id:$s,direction:"BUY",offset:"OPEN",price_type:"LIMIT",limit_price:$p,time_cond:"GFD",volume_cond:"ANY",volume:1}')
echo "quote_last=$LAST price=$PRICE payload=$PAYLOAD"
ACK=$(curl -sS -m 10 -H 'Content-Type: application/json' -d "$PAYLOAD" "$NODE_URL/order")
OID=$(printf '%s' "$ACK" | jq -r '.order_id // empty')
echo "ack=$ACK"
if [[ -z "$OID" ]]; then
    exit 1
fi
ORDER_BEFORE=$(wait_order "$OID")
echo "order_before_kill=$ORDER_BEFORE"
if [[ "$(printf '%s' "$ORDER_BEFORE" | jq -r 'if .is_dead == false then "false" else "true" end')" != "false" ]]; then
    echo "order did not remain alive; test inconclusive" >&2
    exit 2
fi

echo "=== kill gateway with live order ==="
OLD_GW=$GW
kill -TERM "$OLD_GW" 2>/dev/null || true
for _ in $(seq 1 20); do kill -0 "$OLD_GW" 2>/dev/null || break; sleep 1; done
kill -KILL "$OLD_GW" 2>/dev/null || true
GW=
kill -0 "$NODE" 2>/dev/null || { echo "node died with gateway" >&2; exit 1; }
start_gw
wait_ready after_gateway_restart || { tail -60 "$NODE_LOG" >&2; exit 1; }
ORDER_AFTER=$(wait_order "$OID")
echo "order_after_restart=$ORDER_AFTER"
if [[ -z "$ORDER_AFTER" || "$(printf '%s' "$ORDER_AFTER" | jq -r '.order_id // empty')" != "$OID" ]]; then
    echo "order was not recovered after gateway restart" >&2
    exit 1
fi

if [[ "$(printf '%s' "$ORDER_AFTER" | jq -r 'if .is_dead == false then "false" else "true" end')" == "false" ]]; then
    echo "cancel recovered order"
    curl -sS -m 5 -X DELETE "$NODE_URL/order/$OID" >/tmp/ctp_order_fault_cancel.json || true
    ORDER_AFTER=$(wait_order "$OID")
fi
echo "order_final=$ORDER_AFTER"
AFTER=$(curl -sS -m 5 "$NODE_URL/positions" |
    jq -c --arg s "$SYMBOL" '.positions[$s] // {volume_long:0,volume_short:0}')
echo "positions_before=$BEFORE"
echo "positions_after=$AFTER"
jq -n --arg before "$BEFORE" --arg after "$AFTER" --arg order "$ORDER_AFTER" \
    '{order:$order,positions_before:$before,positions_after:$after}' >/tmp/ctp_order_fault_result.json
cat /tmp/ctp_order_fault_result.json
