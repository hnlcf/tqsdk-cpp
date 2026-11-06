#!/usr/bin/env bash
# Schedule the P6 campaign with at(1). Run once; use --force to reschedule.
set -Eeuo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
MONDAY=${P6_START_DATE:-$(date -d 'next monday' +%Y%m%d)}
TUESDAY=$(date -d "$MONDAY +1 day" +%Y%m%d)
SATURDAY=$(date -d "$MONDAY +5 days" +%Y%m%d)
DIR=${P6_CAMPAIGN_DIR:-$ROOT/artifacts/p6_campaign_$MONDAY}
MARKER=$DIR/scheduled_at.txt
FORCE=0
[[ "${1:-}" == "--force" ]] && FORCE=1

command -v at >/dev/null || { echo "at is required" >&2; exit 2; }
systemctl is-active atd >/dev/null 2>&1 || {
    echo "atd is not active; start it before scheduling" >&2
    exit 2
}
mkdir -p "$DIR"
if [[ -f "$MARKER" && "$FORCE" != "1" ]]; then
    echo "already scheduled: $MARKER (use --force to add another set)" >&2
    cat "$MARKER" >&2
    exit 2
fi

schedule() {
    local when=$1
    shift
    local cmd="cd '$ROOT' && P6_CAMPAIGN_DIR='$DIR' $*"
    printf '%s\n' "$cmd" | at -t "$when" 2>&1 | tee -a "$MARKER"
}

{
    echo "campaign_dir=$DIR"
    echo "monday=$MONDAY tuesday=$TUESDAY saturday=$SATURDAY"
    echo "scheduled_by=$(whoami) at $(date -Is)"
} >"$MARKER"

# 09:00 start P6.1/P6.2/P6.3
schedule "${MONDAY}0900" "'$ROOT/scripts/p6_campaign_start.sh' >>'$DIR/at_start.log' 2>&1"

# 13:35 partial-fill probe against the same P6.1 node
schedule "${MONDAY}1335" "'$ROOT/scripts/p6_partial_fill_job.sh' >>'$DIR/at_partial.log' 2>&1"

# Tuesday 09:35 close-yesterday against the same P6.1 node
schedule "${TUESDAY}0935" "'$ROOT/scripts/p6_close_yesterday_job.sh' >>'$DIR/at_close.log' 2>&1"

# Saturday 10:00: P6.1 has ended; run recovery and final report
schedule "${SATURDAY}1000" "'$ROOT/scripts/p6_week_followup.sh' >>'$DIR/at_followup.log' 2>&1"

echo
atq
