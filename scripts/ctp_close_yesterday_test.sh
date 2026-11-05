#!/usr/bin/env bash
# 平昨验收：使用一个在上一交易日结算后仍保留的 SHFE.rb2701 多头测试仓，
# 发送 Offset=CLOSE 的 SELL 1 手，验证 CTP 实际走 CloseYesterday 且历史仓减少。
#
# 前置条件：
#   - 测试仓由 scripts/ctp_open_overnight_position.sh 在交易日开仓；
#   - 经过当日结算后，position.volume_long_his >= 1；
#   - node 在 NODE_URL 可用。
#
# 用法：NODE_URL=http://127.0.0.1:18081 ./scripts/ctp_close_yesterday_test.sh
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
POS=$(curl -fsS -m 5 "$NODE_URL/positions")
BEFORE=$(printf '%s' "$POS" | jq -c --arg s "$SYMBOL" '.positions[$s] | {
    volume_long,volume_long_today,volume_long_his,
    volume_short,volume_short_today,volume_short_his,margin
}')
echo "before=$BEFORE"
HIS=$(printf '%s' "$POS" | jq -r --arg s "$SYMBOL" '.positions[$s].volume_long_his // 0')
if (( HIS < VOLUME )); then
    echo "no settled historical long position yet (volume_long_his=$HIS); run after daily settlement" >&2
    exit 3
fi
if [[ -f "$PENDING" ]]; then
    echo "pending metadata: $(cat "$PENDING")"
fi

PAYLOAD=$(jq -n --arg s "$SYMBOL" --argjson v "$VOLUME" \
    '{instrument_id:$s,direction:"SELL",offset:"CLOSE",price_type:"BEST",time_cond:"IOC",volume_cond:"ANY",volume:$v}')
echo "submit=$PAYLOAD"
ACK=$(curl -fsS -m 10 -H 'Content-Type: application/json' -d "$PAYLOAD" "$NODE_URL/order")
echo "ack=$ACK"
OID=$(printf '%s' "$ACK" | jq -r '.order_id // empty')
if [[ -z "$OID" ]]; then
    echo "order submission failed" >&2
    exit 1
fi
ORDER=
for _ in $(seq 1 30); do
    ORDER=$(curl -fsS -m 5 "$NODE_URL/orders" | jq -c --arg oid "$OID" '.orders[$oid]')
    if [[ "$(printf '%s' "$ORDER" | jq -r '.is_dead // false')" == "true" ]]; then
        break
    fi
    sleep 1
done
echo "order=$ORDER"
if [[ "$(printf '%s' "$ORDER" | jq -r '.is_error // false')" == "true" ]]; then
    echo "close-yesterday rejected" >&2
    exit 1
fi
AFTER_POS=$(curl -fsS -m 5 "$NODE_URL/positions")
AFTER=$(printf '%s' "$AFTER_POS" | jq -c --arg s "$SYMBOL" '.positions[$s] | {
    volume_long,volume_long_today,volume_long_his,
    volume_short,volume_short_today,volume_short_his,margin
}')
echo "after=$AFTER"
AFTER_HIS=$(printf '%s' "$AFTER_POS" | jq -r --arg s "$SYMBOL" '.positions[$s].volume_long_his // 0')
if (( AFTER_HIS > HIS - VOLUME )); then
    echo "historical position did not decrease as expected" >&2
    exit 1
fi
echo "PASS close yesterday: volume_long_his $HIS -> $AFTER_HIS"
