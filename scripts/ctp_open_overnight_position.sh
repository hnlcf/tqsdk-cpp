#!/usr/bin/env bash
# 为“平昨”验收准备一个可控的 SHFE.rb2701 多头测试仓。
# 开 1 手后不自动平仓，必须经过当日结算，次日用 ctp_close_yesterday_test.sh 平掉。
# 这个脚本用于 SimNow 测试账户；生产账户不要运行。
set -u

NODE_URL=${NODE_URL:-http://127.0.0.1:18081}
SYMBOL=${SYMBOL:-SHFE.rb2701}
VOLUME=${VOLUME:-1}
PENDING=${PENDING:-$PWD/artifacts/ctp_close_yesterday_pending.json}
command -v jq >/dev/null || { echo "jq is required" >&2; exit 2; }

if ! curl -fsS -m 3 "$NODE_URL/readyz" >/dev/null; then
    echo "node is not ready at $NODE_URL" >&2
    exit 2
fi
QUOTE=$(curl -fsS -m 5 "$NODE_URL/quote/$SYMBOL")
ASK=$(printf '%s' "$QUOTE" | jq -r '.ask_price1 // empty')
if [[ -z "$ASK" || "$ASK" == "null" ]]; then
    echo "no valid ask for $SYMBOL" >&2
    exit 1
fi
PAYLOAD=$(jq -n --arg s "$SYMBOL" --argjson p "$ASK" --argjson v "$VOLUME" \
    '{instrument_id:$s,direction:"BUY",offset:"OPEN",price_type:"LIMIT",limit_price:$p,time_cond:"IOC",volume_cond:"ANY",volume:$v}')
ACK=$(curl -fsS -m 10 -H 'Content-Type: application/json' -d "$PAYLOAD" "$NODE_URL/order")
OID=$(printf '%s' "$ACK" | jq -r '.order_id // empty')
echo "order=$ACK"
if [[ -z "$OID" ]]; then
    exit 1
fi
for _ in $(seq 1 20); do
    ORDER=$(curl -fsS -m 5 "$NODE_URL/orders" | jq -c --arg oid "$OID" '.orders[$oid]')
    [[ "$(printf '%s' "$ORDER" | jq -r '.is_dead // false')" == "true" ]] && break
    sleep 1
done
echo "terminal_order=$ORDER"
if [[ "$(printf '%s' "$ORDER" | jq -r '.is_error // false')" == "true" ]]; then
    echo "open-overnight rejected: $(printf '%s' "$ORDER" | jq -r '.last_msg // "unknown"')" >&2
    exit 1
fi
if [[ "$(printf '%s' "$ORDER" | jq -r '.is_dead // false')" != "true" ]]; then
    echo "open-overnight order did not reach terminal state; refusing pending marker" >&2
    exit 1
fi
POS=$(curl -fsS -m 5 "$NODE_URL/positions" | jq -c --arg s "$SYMBOL" '.positions[$s] // empty')
echo "position=$POS"
POS_VOL=$(printf '%s' "$POS" | jq -r '.volume_long // 0' 2>/dev/null || echo 0)
if ! [[ "$POS_VOL" =~ ^[0-9]+$ ]] || ((POS_VOL < VOLUME)); then
    echo "open-overnight filled position is insufficient (volume_long=$POS_VOL requested=$VOLUME)" >&2
    exit 1
fi
mkdir -p "$(dirname "$PENDING")"
jq -n --arg symbol "$SYMBOL" --arg order_id "$OID" --argjson volume "$VOLUME" \
    '{purpose:"close-yesterday",symbol:$symbol,order_id:$order_id,volume:$volume,opened_at:(now|todate)}' \
    >"$PENDING"
echo "pending=$PENDING"
echo "IMPORTANT: keep this test position through daily settlement; close next trading day."
