#!/usr/bin/env bash
# P6.2 Tuesday close-yesterday job. Runs against the still-running P6.1 node.
set -u

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
DIR=${P6_CAMPAIGN_DIR:-$ROOT/artifacts/p6_campaign_20261109}
NODE_URL=${NODE_URL:-http://127.0.0.1:18081}
SYMBOL=${SYMBOL:-SHFE.rb2701}
LOG=$DIR/p62_close_yesterday.log
mkdir -p "$DIR"
exec >>"$LOG" 2>&1
echo "[$(date -Is)] close-yesterday job start node=$NODE_URL symbol=$SYMBOL"
for ((i = 1; i <= 180; i++)); do
    if curl -fsS -m 3 "$NODE_URL/readyz" >/dev/null 2>&1; then
        break
    fi
    sleep 1
done
if ! curl -fsS -m 3 "$NODE_URL/readyz" >/dev/null 2>&1; then
    echo "node not ready; skip close-yesterday"
    echo 2 >"$DIR/p62_close_yesterday.rc"
    exit 2
fi
NODE_URL="$NODE_URL" SYMBOL="$SYMBOL" VOLUME=1 \
    PENDING="$DIR/close_yesterday_pending.json" \
    "$ROOT/scripts/ctp_close_yesterday_test.sh"
rc=$?
echo "close_yesterday_rc=$rc"
echo "$rc" >"$DIR/p62_close_yesterday.rc"
if ((rc != 0)); then
    echo "$rc" >"$DIR/had_failure"
fi
exit "$rc"
