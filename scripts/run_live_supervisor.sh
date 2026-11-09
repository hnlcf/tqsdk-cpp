#!/usr/bin/env bash
# 最小 live supervisor：run_live.sh 退出后按退避重启。
#
# 用法:
#   ./scripts/run_live_supervisor.sh
#   ./scripts/run_live_supervisor.sh --restart-delay 10
#   ./scripts/run_live_supervisor.sh --max-restarts 3
#
# 停止:
#   Ctrl-C；或 touch run/live/supervisor.stop 后等当前子进程退出。
#
# 注意：这不是 systemd；生产环境仍建议使用 systemd/supervisor 管理。

set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_DIR="${TQSDK_RUN_DIR:-$ROOT/run/live}"
STOP_FILE="$RUN_DIR/supervisor.stop"
RESTART_DELAY=5
MAX_RESTARTS=0
CHILD_PID=""
STOPPING=0

usage() {
    sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
    --restart-delay)
        RESTART_DELAY="${2:?--restart-delay requires seconds}"
        shift 2
        ;;
    --max-restarts)
        MAX_RESTARTS="${2:?--max-restarts requires count}"
        shift 2
        ;;
    -h|--help)
        usage
        exit 0
        ;;
    *)
        break
        ;;
    esac
done

mkdir -p "$RUN_DIR"
rm -f "$STOP_FILE"

stop_child() {
    STOPPING=1
    if [[ -n "$CHILD_PID" ]] && kill -0 "$CHILD_PID" 2>/dev/null; then
        kill -TERM "$CHILD_PID" 2>/dev/null || true
    fi
}
trap stop_child INT TERM

restarts=0
while (( STOPPING == 0 )); do
    if [[ -f "$STOP_FILE" ]]; then
        echo "supervisor: stop file found: $STOP_FILE"
        break
    fi

    echo "supervisor: starting run_live.sh (restart=$restarts)"
    "$ROOT/scripts/run_live.sh" --no-ui "$@" &
    CHILD_PID=$!
    set +e
    wait "$CHILD_PID"
    rc=$?
    set -e
    CHILD_PID=""

    if (( STOPPING != 0 )); then
        break
    fi
    if (( MAX_RESTARTS > 0 && restarts >= MAX_RESTARTS )); then
        echo "supervisor: max restarts reached ($MAX_RESTARTS), exiting rc=$rc" >&2
        exit "$rc"
    fi
    ((restarts += 1))
    echo "supervisor: run_live.sh exited rc=$rc; restarting in ${RESTART_DELAY}s" >&2
    sleep "$RESTART_DELAY"
done

rm -f "$STOP_FILE"
echo "supervisor: stopped"
