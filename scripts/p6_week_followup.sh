#!/usr/bin/env bash
# P6 week follow-up after the 5-day soak ends.
#
# Runs read-only gateway/node restart recovery and emits a final campaign
# report. The real order-in-flight fault test is intentionally not run on a
# weekend because it needs a live quote; record it as deferred instead.
set -u

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
DIR=${P6_CAMPAIGN_DIR:-$ROOT/artifacts/p6_campaign_20261109}
NODE_URL=${NODE_URL:-http://127.0.0.1:18081}
LOG=$DIR/p6_followup.log
mkdir -p "$DIR"
exec >>"$LOG" 2>&1
echo "[$(date -Is)] follow-up start"

# Wait for ctp_week_soak and its gw/node to stop.
for ((i = 1; i <= 120; i++)); do
    if ! pgrep -af 'ctp_week_soak.py.*--execute' >/dev/null 2>&1 &&
       ! curl -fsS -m 2 "$NODE_URL/healthz" >/dev/null 2>&1; then
        break
    fi
    sleep 10
done

"$ROOT/scripts/ctp_recovery_test.sh" >>"$DIR/p6_recovery.log" 2>&1
recovery_rc=$?
echo "recovery_rc=$recovery_rc"
echo "$recovery_rc" >"$DIR/p6_recovery.rc"
if ((recovery_rc != 0)); then
    echo "$recovery_rc" >"$DIR/had_failure"
fi

cat >"$DIR/p62_order_fault_deferred.txt" <<'EOF'
Order-in-flight fault test deferred: it needs a live quote and would replace
the P6.1 gateway. Run on the next trading day:

  NODE_URL=http://127.0.0.1:28081 ./scripts/ctp_order_fault_test.sh

Do not run it concurrently with the P6.1 week soak.
EOF

python3 "$ROOT/scripts/p6_report.py" --dir "$DIR" || true
echo "[$(date -Is)] follow-up finished recovery_rc=$recovery_rc"
