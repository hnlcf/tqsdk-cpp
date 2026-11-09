#!/usr/bin/env bash
# Start gw/node and the TQSDK Board web dev server.
#
# Usage:
#   ./scripts/run_web.sh --build
#
# Extra arguments after the wrapper are passed to run_live.sh. The wrapper
# starts backend with --no-ui, waits for node, then runs Vite in foreground.
set -Eeuo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WEB_DIR="$ROOT/web"
WEB_PORT=${TQSDK_WEB_PORT:-5173}
HTTP_PORT=${TQSDK_HTTP_PORT:-18081}
LOG=${TQSDK_WEB_LOG:-$ROOT/run/live/logs/web.log}
mkdir -p "$(dirname "$LOG")"

BACK_PID=""
cleanup() {
    trap - EXIT INT TERM
    if [[ -n "$BACK_PID" ]]; then
        kill -TERM "$BACK_PID" 2>/dev/null || true
        sleep 1
        kill -KILL "$BACK_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM

if [[ ! -d "$WEB_DIR/node_modules" ]]; then
    echo "installing web dependencies with pnpm ..."
    (cd "$WEB_DIR" && pnpm install)
fi

echo "starting gw/node (web mode) ..."
"$ROOT/scripts/run_live.sh" --no-ui "$@" >"$LOG" 2>&1 &
BACK_PID=$!

for ((i = 1; i <= 90; i++)); do
    if curl -fsS -m 2 "http://127.0.0.1:$HTTP_PORT/readyz" >/dev/null 2>&1; then
        break
    fi
    if ! kill -0 "$BACK_PID" 2>/dev/null; then
        echo "backend exited during startup; see $LOG" >&2
        tail -80 "$LOG" >&2 || true
        exit 1
    fi
    sleep 1
done

echo "TQSDK Board: http://127.0.0.1:$WEB_PORT"
echo "node:       http://127.0.0.1:$HTTP_PORT"
echo "backend log: $LOG"
cd "$WEB_DIR"
VITE_PROXY_TARGET="http://127.0.0.1:$HTTP_PORT" \
    pnpm dev --host 127.0.0.1 --port "$WEB_PORT" --strictPort
