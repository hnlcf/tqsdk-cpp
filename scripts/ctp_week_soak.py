#!/usr/bin/env python3
"""连续交易周 CTP session soak 编排器。

默认只做 dry-run；真正启动必须显式 --execute。脚本本身不报单，复用
ctp_session_soak.py 的只读采样逻辑，并把 5 个交易日的结果归档到独立目录。

示例：
    python3 scripts/ctp_week_soak.py --days 5
    python3 scripts/ctp_week_soak.py --days 5 --execute

真正的 5 个交易日验证应在交易周开始时执行；节假日/结算断线由 monitor 的
expected_disconnect 记录，不计为故障。
"""
from __future__ import annotations

import argparse
import json
import signal
import subprocess
import sys
from collections import defaultdict
from datetime import datetime
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent


def now_tag() -> str:
    return datetime.now().strftime("%Y%m%d_%H%M%S")


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--days", type=int, default=5, help="连续交易日数量")
    p.add_argument("--duration", type=float, default=None,
                   help="总采样时长秒；默认 days*86400")
    p.add_argument("--interval", type=float, default=15.0)
    p.add_argument("--node-url", default="http://127.0.0.1:18081")
    p.add_argument("--out-dir", type=Path, default=ROOT / "artifacts" / "ctp_week")
    p.add_argument("--gw-bin", default=str(ROOT / "build-ui/tqsdk-gw"))
    p.add_argument("--node-bin", default=str(ROOT / "build-ui/tqsdk-node"))
    p.add_argument("--gw-config", default=str(ROOT / "run/live/gw.toml"))
    p.add_argument("--node-config", default=str(ROOT / "run/live/node.toml"))
    p.add_argument("--execute", action="store_true", help="真正启动 gw/node 并采样")
    p.add_argument("--force", action="store_true", help="忽略已有进程告警")
    return p.parse_args()


def running_processes() -> list[str]:
    try:
        out = subprocess.check_output(
            ["bash", "-lc", "pgrep -af 'build-ui/tqsdk-(gw|node)|build-ctp/tqsdk-(gw|node)' || true"],
            text=True,
        )
    except OSError:
        return []
    rows = []
    for line in out.splitlines():
        if "pgrep -af" in line:
            continue
        rows.append(line.strip())
    return rows


def summarize(jsonl: Path, summary_path: Path) -> dict[str, Any]:
    rows: list[dict[str, Any]] = []
    if jsonl.exists():
        for line in jsonl.read_text(encoding="utf-8", errors="replace").splitlines():
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError:
                continue
    by_day: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        day = str(row.get("ts", ""))[:10]
        by_day[day].append(row)
    days = []
    for day in sorted(by_day):
        samples = by_day[day]
        ready = sum(1 for row in samples if row.get("ready"))
        expected = sum(1 for row in samples if row.get("expected_disconnect"))
        phases = sorted({str(row.get("phase", "unknown")) for row in samples})
        days.append({
            "date": day,
            "samples": len(samples),
            "ready_samples": ready,
            "expected_disconnect_samples": expected,
            "phases": phases,
        })
    result: dict[str, Any] = {
        "jsonl": str(jsonl),
        "samples": len(rows),
        "days": days,
    }
    if summary_path.exists():
        try:
            result["monitor"] = json.loads(summary_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            result["monitor_error"] = "invalid summary json"
    return result


def main() -> int:
    args = parse_args()
    duration = args.duration if args.duration is not None else args.days * 86400.0
    args.out_dir.mkdir(parents=True, exist_ok=True)
    tag = now_tag()
    jsonl = args.out_dir / f"ctp_week_{tag}.jsonl"
    gw_log = args.out_dir / f"ctp_week_{tag}.gw.log"
    node_log = args.out_dir / f"ctp_week_{tag}.node.log"
    plan = {
        "days": args.days,
        "duration_s": duration,
        "interval_s": args.interval,
        "node_url": args.node_url,
        "jsonl": str(jsonl),
        "gw_log": str(gw_log),
        "node_log": str(node_log),
        "execute": args.execute,
    }
    print(json.dumps(plan, ensure_ascii=False, indent=2), flush=True)
    if not args.execute:
        print("dry-run only; add --execute on a trading week to start the soak", file=sys.stderr)
        return 0

    active = running_processes()
    if active and not args.force:
        print("existing tqsdk processes detected; use --force only after inspecting them:",
              file=sys.stderr)
        for line in active:
            print("  " + line, file=sys.stderr)
        return 2
    for path in (args.gw_bin, args.node_bin, args.gw_config, args.node_config):
        if not Path(path).is_file():
            print(f"missing required file: {path}", file=sys.stderr)
            return 2

    cmd = [
        sys.executable,
        str(ROOT / "scripts" / "ctp_session_soak.py"),
        "--manage-processes",
        "--node-url", args.node_url,
        "--duration", str(duration),
        "--interval", str(args.interval),
        "--out", str(jsonl),
        "--gw-bin", args.gw_bin,
        "--node-bin", args.node_bin,
        "--gw-config", args.gw_config,
        "--node-config", args.node_config,
        "--gw-log", str(gw_log),
        "--node-log", str(node_log),
    ]
    print(json.dumps({"command": cmd}, ensure_ascii=False), flush=True)
    child = subprocess.Popen(cmd, cwd=ROOT)
    interrupted = False

    def stop_child(signum: int, _frame: Any) -> None:
        nonlocal interrupted
        interrupted = True
        if child.poll() is None:
            child.send_signal(signal.SIGTERM)

    old_int = signal.signal(signal.SIGINT, stop_child)
    old_term = signal.signal(signal.SIGTERM, stop_child)
    try:
        rc = child.wait()
    finally:
        signal.signal(signal.SIGINT, old_int)
        signal.signal(signal.SIGTERM, old_term)

    summary_path = jsonl.with_suffix(".summary.json")
    report = summarize(jsonl, summary_path)
    report["exit_code"] = rc
    report["interrupted"] = interrupted
    report["plan"] = plan
    report_path = args.out_dir / f"ctp_week_{tag}.report.json"
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n",
                           encoding="utf-8")
    print(json.dumps({"report": str(report_path), "summary": report}, ensure_ascii=False,
                     indent=2), flush=True)

    monitor = report.get("monitor", {})
    if rc != 0 or monitor.get("unexpected_gaps", 0) != 0 or monitor.get("fatal"):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
