#!/usr/bin/env bash
# P6 campaign Monday 09:00 launcher.
#
# Phases:
#   P6.1  5-day read-only CTP session soak (detached)
#   P6.2  SimNow acceptance, optional market/counterparty orders, overnight
#         position for Tuesday close-yesterday job
#   P6.3  ctest, ASan build/tests, 72h SimTd soak, fault/regression notes
#
# This script is designed to run unattended from at/systemd. It does not ask
# questions. Set P62_WRITE=0 to run the CTP week read-only.
set -u

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
DIR=${P6_CAMPAIGN_DIR:-$ROOT/artifacts/p6_campaign_20261109}
NODE_URL=${NODE_URL:-http://127.0.0.1:18081}
SYMBOL=${P62_SYMBOL:-SHFE.rb2701}
OPTION=${P62_OPTION:-SHFE.ao2701P2200}
P62_WRITE=${P62_WRITE:-1}
P62_OPEN_OVERNIGHT=${P62_OPEN_OVERNIGHT:-1}
P63_SOAK_DURATION=${P63_SOAK_DURATION:-259200}
mkdir -p "$DIR"
exec >>"$DIR/controller.log" 2>&1

log() { printf '[%s] %s\n' "$(date -Is)" "$*"; }
phase() {
    local name=$1
    shift
    log "PHASE START $name"
    "$@"
    local rc=$?
    log "PHASE END $name rc=$rc"
    printf '%s\n' "$rc" >"$DIR/$name.rc"
    if ((rc != 0)); then
        printf '%s\n' "$rc" >"$DIR/had_failure"
    fi
    return 0
}

log "P6 campaign start dir=$DIR node=$NODE_URL symbol=$SYMBOL"
printf '%s\n' "$$" >"$DIR/controller.pid"
printf '%s\n' "$(date -Is)" >"$DIR/started_at"
for tool in curl jq python3 setsid; do
    command -v "$tool" >/dev/null || { log "missing $tool"; exit 2; }
done

# ---------------------------------------------------------------- preflight
phase p6_preflight "$ROOT/scripts/ctp_week_preflight.sh"

# ---------------------------------------------------------------- P6.1
# The runner manages its own gw/node. It stays alive for the whole campaign.
if pgrep -af 'ctp_week_soak.py.*--execute' >/dev/null 2>&1; then
    log "ctp_week_soak already running; skip duplicate start"
else
    setsid -f python3 "$ROOT/scripts/ctp_week_soak.py" \
        --days 5 --execute \
        --node-url "$NODE_URL" \
        --out-dir "$DIR/p61_week" \
        >"$DIR/p61_week_soak.out" 2>&1
    log "P6.1 detached: $?"
fi

wait_ready() {
    local timeout=${1:-240}
    local i body phase ready
    for ((i = 1; i <= timeout; i++)); do
        body=$(curl -sS -m 3 "$NODE_URL/readyz" 2>/dev/null || true)
        ready=$(printf '%s' "$body" | jq -r '.ready // false' 2>/dev/null || echo false)
        phase=$(printf '%s' "$body" | jq -r '.td.phase // "unknown"' 2>/dev/null || echo unknown)
        if [[ "$ready" == "true" ]]; then
            log "READY after ${i}s phase=$phase body=$(printf '%s' "$body" | jq -c '{ready,td,md}' 2>/dev/null || true)"
            return 0
        fi
        sleep 1
    done
    log "READY TIMEOUT after ${timeout}s"
    return 1
}

wait_trade_phase() {
    local timeout=${1:-300}
    local i body ready phase
    for ((i = 1; i <= timeout; i++)); do
        body=$(curl -sS -m 3 "$NODE_URL/readyz" 2>/dev/null || true)
        ready=$(printf '%s' "$body" | jq -r '.ready // false' 2>/dev/null || echo false)
        phase=$(printf '%s' "$body" | jq -r '.td.phase // "unknown"' 2>/dev/null || echo unknown)
        if [[ "$ready" == "true" && ("$phase" == "open" || "$phase" == "break") ]]; then
            log "TRADE WINDOW ready phase=$phase after ${i}s"
            return 0
        fi
        sleep 1
    done
    log "TRADE WINDOW TIMEOUT phase last=$phase"
    return 1
}

wait_ready 240 || true
wait_trade_phase 300 || true

# Daily read-only reconciliation at 09:10 and 15:20.
setsid -f python3 "$ROOT/scripts/p6_daily_reconcile_loop.py" \
    --node-url "$NODE_URL" --days 5 \
    --out-dir "$DIR/p62_reconcile" \
    >"$DIR/p62_reconcile_loop.out" 2>&1
log "P6.2 daily reconcile loop detached: $?"

# ---------------------------------------------------------------- P6.2
phase p62_acceptance_readonly python3 "$ROOT/scripts/simnow_acceptance.py" \
    --node-url "$NODE_URL" --symbol "$SYMBOL" \
    --metrics --ws --ready-timeout 120 \
    --json-out "$DIR/p62_acceptance_readonly.json"

if [[ "$P62_WRITE" == "1" ]]; then
    phase p62_acceptance_write python3 "$ROOT/scripts/simnow_acceptance.py" \
        --node-url "$NODE_URL" --symbol "$SYMBOL" \
        --orders --yes --include-market --include-counterparty \
        --cleanup-market --order-timeout 30 \
        --json-out "$DIR/p62_acceptance_write.json"
else
    log "P62_WRITE=0; write acceptance skipped"
fi

if [[ "$P62_OPEN_OVERNIGHT" == "1" ]]; then
    POS=$(curl -sS -m 5 "$NODE_URL/positions" 2>/dev/null |
        jq -r --arg s "$SYMBOL" '.positions[$s].volume_long // 0' 2>/dev/null || echo 0)
    if [[ "${POS:-0}" =~ ^[0-9]+$ ]] && ((POS > 0)); then
        log "overnight position already present for $SYMBOL volume_long=$POS; skip open"
    else
        phase p62_open_overnight env NODE_URL="$NODE_URL" SYMBOL="$SYMBOL" VOLUME=1 \
            PENDING="$DIR/close_yesterday_pending.json" \
            "$ROOT/scripts/ctp_open_overnight_position.sh"
    fi
fi

# ---------------------------------------------------------------- P6.3
phase p63_ctest ctest --test-dir "$ROOT/build-ui" --output-on-failure

if [[ "${P63_ASAN:-1}" == "1" ]]; then
    setsid -f "$ROOT/scripts/p6_asan_job.sh" >"$DIR/p63_asan.out" 2>&1
    log "P6.3 ASan build/test detached: $?"
else
    log "P63_ASAN=0; ASan skipped"
fi

setsid -f python3 "$ROOT/scripts/soak_two_process.py" \
    --duration "$P63_SOAK_DURATION" \
    --load-threads 4 \
    --reconnect-interval 120 \
    --gw-bin "$ROOT/build-ui/tqsdk-gw" \
    --node-bin "$ROOT/build-ui/tqsdk-node" \
    --out "$DIR/p63_soak_72h.csv" \
    >"$DIR/p63_soak_72h.out" 2>&1
log "P6.3 72h SimTd soak detached: $?"

cat >"$DIR/p63_fuzz_skip.txt" <<'EOF'
P6.3 fuzz: no dedicated libFuzzer target is present in this tree yet.
Current coverage comes from ProtocolCodec malformed tests and TCP/UDS
malformed frame tests in ctest. Add a libFuzzer corpus/target before claiming
the fuzz milestone complete.
EOF

log "P6 campaign start finished; P6.1/P6.3 children continue in background"
printf '%s\n' "$(date -Is)" >"$DIR/started_finished_at"
exit 0
