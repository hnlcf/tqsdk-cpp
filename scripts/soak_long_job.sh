#!/usr/bin/env bash
# Long-running two-process soak wrapper (24h/72h).
#
# Examples:
#   ./scripts/soak_long_job.sh --duration 86400
#   ./scripts/soak_long_job.sh --duration 259200
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
DURATION=86400
BUILD_DIR=${TQSDK_SOAK_BUILD_DIR:-"$ROOT/build-ui"}
OUT_DIR=${TQSDK_SOAK_OUT_DIR:-"$ROOT/artifacts/soak_$(date +%Y%m%d_%H%M%S)"}
RECONNECT=600
LOAD_THREADS=8
ORDER_INTERVAL=5

while (($# > 0)); do
    case "$1" in
    --duration) DURATION="$2"; shift 2 ;;
    --out-dir) OUT_DIR="$2"; shift 2 ;;
    --reconnect-interval) RECONNECT="$2"; shift 2 ;;
    --load-threads) LOAD_THREADS="$2"; shift 2 ;;
    --order-interval) ORDER_INTERVAL="$2"; shift 2 ;;
    -h|--help)
        sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

mkdir -p "$OUT_DIR"
LOG="$OUT_DIR/soak.log"
echo "[$(date -Is)] duration=${DURATION}s out=$OUT_DIR" | tee "$LOG"
python3 "$ROOT/scripts/soak_two_process.py" \
    --duration "$DURATION" \
    --reconnect-interval "$RECONNECT" \
    --load-threads "$LOAD_THREADS" \
    --order-interval "$ORDER_INTERVAL" \
    --gw-bin "$BUILD_DIR/tqsdk-gw" \
    --node-bin "$BUILD_DIR/tqsdk-node" \
    --out "$OUT_DIR/soak.csv" 2>&1 | tee -a "$LOG"

echo "[$(date -Is)] soak PASS; artifacts: $OUT_DIR" | tee -a "$LOG"
