#!/usr/bin/env python3
"""P6 campaign daily reconciliation loop.

Runs ctp_daily_reconcile.py at 09:10 and 15:20 local time for N calendar days,
skipping weekends. It is read-only and is intended to run detached while the
5-day CTP session soak is active.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from datetime import datetime, timedelta
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_TIMES = ("09:10", "15:20")


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--node-url", default="http://127.0.0.1:18081")
    p.add_argument("--out-dir", type=Path, default=ROOT / "artifacts/p6_campaign_20261109/p62_reconcile")
    p.add_argument("--days", type=int, default=5)
    p.add_argument("--times", default=",".join(DEFAULT_TIMES))
    return p.parse_args()


def next_weekday(start: datetime) -> datetime:
    day = start
    while day.weekday() >= 5:
        day += timedelta(days=1)
    return day


def next_slot(now: datetime, times: list[tuple[int, int]]) -> datetime:
    for _ in range(14):
        day = next_weekday(now)
        for hour, minute in times:
            slot = day.replace(hour=hour, minute=minute, second=0, microsecond=0)
            if slot > now:
                return slot
        now = (day + timedelta(days=1)).replace(hour=0, minute=0, second=0, microsecond=0)
    raise RuntimeError("no reconciliation slot found")


def main() -> int:
    args = parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    times = []
    for item in args.times.split(","):
        hour, minute = item.strip().split(":")
        times.append((int(hour), int(minute)))
    log_path = args.out_dir / "reconcile_loop.log"
    log = log_path.open("a", encoding="utf-8")
    print(json.dumps({"started": datetime.now().astimezone().isoformat(timespec="seconds"),
                      "times": args.times, "days": args.days,
                      "out_dir": str(args.out_dir)}, ensure_ascii=False), file=log, flush=True)
    deadline = datetime.now().astimezone() + timedelta(days=args.days)
    while True:
        now = datetime.now().astimezone()
        if now >= deadline:
            break
        slot = next_slot(now, times)
        if slot >= deadline:
            break
        wait = max(0.0, (slot - now).total_seconds())
        print(json.dumps({"sleep_until": slot.isoformat(), "wait_s": round(wait, 1)},
                         ensure_ascii=False), file=log, flush=True)
        time.sleep(wait)
        cmd = [sys.executable, str(ROOT / "scripts/ctp_daily_reconcile.py"),
               "--node-url", args.node_url, "--out-dir", str(args.out_dir)]
        result = subprocess.run(cmd, cwd=ROOT, text=True, capture_output=True)
        print(json.dumps({"slot": slot.isoformat(), "rc": result.returncode},
                         ensure_ascii=False), file=log, flush=True)
        if result.stdout:
            print(result.stdout, file=log, flush=True)
        if result.stderr:
            print(result.stderr, file=log, flush=True)
        # 防止同一分钟重复命中
        time.sleep(61)
    log.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
