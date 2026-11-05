#!/usr/bin/env bash
# SimNow 部分成交探针（只读行情 + 小额期权订单，自动清理）。
#
# 该探针只是为了观察 SimNow 是否能稳定构造部分成交；如果盘口被一次吃完或
# 非交易时段拒绝，会明确记录结果，不伪造 PASS。
# 默认使用 SHFE.ao2701P2200；NODE_URL 由外部传入。
set -u

NODE_URL=${NODE_URL:-http://127.0.0.1:18081}
SYMBOL=${SYMBOL:-SHFE.ao2701P2200}
MAX_VOLUME=${MAX_VOLUME:-40}
OUT=${OUT:-$PWD/artifacts/ctp_partial_fill_probe.txt}
command -v jq >/dev/null || { echo "jq required" >&2; exit 2; }
mkdir -p "$(dirname "$OUT")"
exec > >(tee -a "$OUT") 2>&1

echo "[$(date -Is)] partial fill probe $SYMBOL node=$NODE_URL"
ORDER=
cleanup() {
    if [[ -n "$ORDER" ]]; then
        curl -sS -m 3 -X DELETE "$NODE_URL/order/$ORDER" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

Q=$(curl -sS -m 5 "$NODE_URL/quote/$SYMBOL" || true)
ASK=$(printf '%s' "$Q" | jq -r '.ask_price1 // empty' 2>/dev/null || true)
BID=$(printf '%s' "$Q" | jq -r '.bid_price1 // empty' 2>/dev/null || true)
AV=$(printf '%s' "$Q" | jq -r '.ask_volume1 // 0' 2>/dev/null || true)
if [[ -z "$ASK" || "$ASK" == "null" || -z "$BID" || "$BID" == "null" ]]; then
    echo "no valid option quote; skip (not a failure of order path)"
    exit 3
fi
QTY=$((AV + 1))
(( QTY < 2 )) && QTY=2
(( QTY > MAX_VOLUME )) && QTY=$MAX_VOLUME
echo "quote ask=$ASK bid=$BID ask_volume=$AV probe_volume=$QTY"

PAYLOAD=$(jq -n --arg s "$SYMBOL" --argjson p "$ASK" --argjson v "$QTY" \
    '{instrument_id:$s,direction:"BUY",offset:"OPEN",price_type:"LIMIT",limit_price:$p,time_cond:"GFD",volume_cond:"ANY",volume:$v}')
ACK=$(curl -sS -m 10 -H 'Content-Type: application/json' -d "$PAYLOAD" "$NODE_URL/order")
ORDER=$(printf '%s' "$ACK" | jq -r '.order_id // empty')
echo "ack=$ACK"
if [[ -z "$ORDER" ]]; then
    exit 1
fi

ORD=
for _ in $(seq 1 8); do
    sleep 1
    ORD=$(curl -sS -m 4 "$NODE_URL/orders" | jq -c --arg id "$ORDER" '.orders[$id] // empty')
    dead=$(printf '%s' "$ORD" | jq -r '.is_dead // false' 2>/dev/null || echo false)
    [[ "$dead" == "true" ]] && break
done
echo "order=$ORD"
FILLED=$(printf '%s' "$ORD" | jq -r '(.volume_orign // 0) - (.volume_left // 0)' 2>/dev/null || echo 0)
LEFT=$(printf '%s' "$ORD" | jq -r '.volume_left // 0' 2>/dev/null || echo 0)
DEAD=$(printf '%s' "$ORD" | jq -r '.is_dead // false' 2>/dev/null || echo false)
if [[ "$DEAD" != "true" && "$LEFT" != "0" ]]; then
    echo "cancel remainder=$LEFT"
    curl -sS -m 5 -X DELETE "$NODE_URL/order/$ORDER" >/dev/null 2>&1 || true
    sleep 2
fi
if (( FILLED > 0 )); then
    CLOSE=$(jq -n --arg s "$SYMBOL" --argjson p "$BID" --argjson v "$FILLED" \
        '{instrument_id:$s,direction:"SELL",offset:"CLOSE",price_type:"LIMIT",limit_price:$p,time_cond:"IOC",volume_cond:"ANY",volume:$v}')
    CA=$(curl -sS -m 10 -H 'Content-Type: application/json' -d "$CLOSE" "$NODE_URL/order")
    CID=$(printf '%s' "$CA" | jq -r '.order_id // empty')
    echo "close_ack=$CA"
    for _ in $(seq 1 8); do
        sleep 1
        CO=$(curl -sS -m 4 "$NODE_URL/orders" | jq -c --arg id "$CID" '.orders[$id] // empty')
        [[ "$(printf '%s' "$CO" | jq -r '.is_dead // false' 2>/dev/null || echo false)" == "true" ]] && break
    done
    echo "close_order=$CO"
fi
POS=$(curl -sS -m 5 "$NODE_URL/positions" | jq -c --arg s "$SYMBOL" '.positions[$s] // empty')
echo "position=$POS"
if (( FILLED > 0 && FILLED < QTY )); then
    echo "RESULT=PARTIAL filled=$FILLED requested=$QTY"
elif (( FILLED == QTY )); then
    echo "RESULT=FULL filled=$FILLED requested=$QTY (SimNow did not expose partial)"
else
    echo "RESULT=NO_FILL order_status=$(printf '%s' "$ORD" | jq -r '.status // "?"' 2>/dev/null || echo '?')"
fi
ORDER=
