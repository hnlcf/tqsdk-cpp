#!/usr/bin/env bash
# P6.2 Monday 13:35 partial-fill probe.
set -u

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
DIR=${P6_CAMPAIGN_DIR:-$ROOT/artifacts/p6_campaign_20261109}
NODE_URL=${NODE_URL:-http://127.0.0.1:18081}
SYMBOL=${P62_OPTION:-SHFE.ao2701P2200}
LOG=$DIR/p62_partial_fill.log
mkdir -p "$DIR"
exec >>"$LOG" 2>&1
echo "[$(date -Is)] partial fill job start node=$NODE_URL symbol=$SYMBOL"

for ((i = 1; i <= 180; i++)); do
    if curl -fsS -m 3 "$NODE_URL/readyz" >/dev/null 2>&1; then
        break
    fi
    sleep 1
done
if ! curl -fsS -m 3 "$NODE_URL/readyz" >/dev/null 2>&1; then
    echo "node not ready; skip partial fill"
    echo 2 >"$DIR/p62_partial_fill.rc"
    echo 2 >"$DIR/had_failure"
    exit 2
fi

AVAILABLE=$(curl -sS -m 5 "$NODE_URL/account" 2>/dev/null |
    jq -r '.available // empty' 2>/dev/null || true)
if [[ "$AVAILABLE" =~ ^-?[0-9]+([.][0-9]+)?$ ]]; then
    if ! awk -v a="$AVAILABLE" -v m="${P62_MIN_AVAILABLE:-20000}" 'BEGIN { exit !(a >= m) }'; then
        echo "insufficient available funds ($AVAILABLE); skip partial fill probe"
        echo 3 >"$DIR/p62_partial_fill.rc"
        echo "not run: available=$AVAILABLE" >"$DIR/p62_partial_fill_skip.txt"
        exit 0
    fi
fi

NODE_URL="$NODE_URL" SYMBOL="$SYMBOL" MAX_VOLUME=40 \
    OUT="$DIR/p62_partial_fill.txt" \
    "$ROOT/scripts/ctp_partial_fill_probe.sh"
rc=$?
echo "partial_fill_rc=$rc"
echo "$rc" >"$DIR/p62_partial_fill.rc"
# rc=3 means no quote / not determinable, not a product failure.
if ((rc != 0 && rc != 3)); then
    echo "$rc" >"$DIR/had_failure"
fi
exit 0
