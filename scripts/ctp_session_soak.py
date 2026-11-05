#!/usr/bin/env python3
"""CTP 交易时段 soak。

CTP 交易账户在每日结算时会预期断开，不能用连续 24h 在线判断可靠性。本工具
按交易时段采样 /readyz、account、positions、orders，把结算/闭市期间的断线
标记为 expected，把交易时段内的断线单独计为 unexpected。

可独立使用（连接已运行的 node）：
    python3 scripts/ctp_session_soak.py --node-url http://127.0.0.1:18081 \
        --duration 21600 --out artifacts/ctp_session_soak.jsonl

也可以让脚本管理 gw/node：
    python3 scripts/ctp_session_soak.py --manage-processes \
        --gw-config run/live/gw.toml --node-config run/live/node.toml \
        --duration 21600 --out artifacts/ctp_session_soak.jsonl

脚本只读，不下单；每天交易时段可分别运行，跨结算断线由 expected 字段记录。
"""
from __future__ import annotations

import argparse
import json
import re
import signal
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent


def http_json(base: str, path: str, timeout: float = 5.0) -> tuple[int, Any]:
    request = urllib.request.Request(base.rstrip("/") + path, headers={"Accept": "application/json"})
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            raw = response.read()
            return response.status, json.loads(raw) if raw else {}
    except urllib.error.HTTPError as exc:
        raw = exc.read()
        try:
            body = json.loads(raw) if raw else {}
        except json.JSONDecodeError:
            body = {"error": raw.decode("utf-8", "replace")}
        return exc.code, body
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        return 0, {"error": str(exc)}


def iso_now() -> str:
    return datetime.now(timezone.utc).astimezone().isoformat(timespec="seconds")


class Runner:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.stop = False
        self.gw: subprocess.Popen[bytes] | None = None
        self.node: subprocess.Popen[bytes] | None = None
        self.gw_log: Any = None
        self.node_log: Any = None
        self.gw_port = 14330
        self.rows: list[dict[str, Any]] = []
        self.expected_gaps = 0
        self.unexpected_gaps = 0
        self.last_ready: bool | None = None
        self.last_phase: str = "unknown"
        self.ready_since: float | None = None
        self.down_since: float | None = None
        self.down_expected = False
        self.out_file: Any = None

    def handle_signal(self, _signum: int, _frame: Any) -> None:
        self.stop = True

    def _gw_port_from_node_config(self) -> int:
        text = Path(self.args.node_config).read_text(encoding="utf-8")
        match = re.search(r"^\s*tcp_port\s*=\s*(\d+)", text, re.MULTILINE)
        return int(match.group(1)) if match else 14330

    def _wait_gateway_port(self, timeout: float = 15.0) -> bool:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline and not self.stop:
            try:
                with socket.create_connection(("127.0.0.1", self.gw_port), timeout=0.5):
                    return True
            except OSError:
                time.sleep(0.1)
        return False

    def start_processes(self) -> None:
        if not self.args.manage_processes:
            return
        gw_bin = self.args.gw_bin
        node_bin = self.args.node_bin
        for path in (gw_bin, node_bin):
            if not Path(path).is_file():
                raise SystemExit(f"missing binary: {path}")
        self.args.gw_log.parent.mkdir(parents=True, exist_ok=True)
        self.args.node_log.parent.mkdir(parents=True, exist_ok=True)
        self.gw_log = self.args.gw_log.open("w", encoding="utf-8")
        self.node_log = self.args.node_log.open("w", encoding="utf-8")
        self.gw_port = self._gw_port_from_node_config()
        self.gw = subprocess.Popen(
            [gw_bin, "-c", self.args.gw_config],
            cwd=ROOT,
            stdout=self.gw_log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        if not self._wait_gateway_port():
            self.args.fatal = "gateway port did not open"
            raise SystemExit(self.args.fatal)
        self.node = subprocess.Popen(
            [node_bin, "-c", self.args.node_config],
            cwd=ROOT,
            stdout=self.node_log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        print(json.dumps({"started": {"gw": self.gw.pid, "node": self.node.pid},
                          "gw_log": str(self.args.gw_log), "node_log": str(self.args.node_log)}),
              flush=True)

    def stop_processes(self) -> None:
        for proc in (self.node, self.gw):
            if proc is None:
                continue
            if proc.poll() is None:
                proc.terminate()
        for proc in (self.node, self.gw):
            if proc is None:
                continue
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        for handle in (self.node_log, self.gw_log):
            if handle is not None:
                handle.close()

    def wait_initial(self) -> bool:
        deadline = time.monotonic() + self.args.startup_timeout
        while time.monotonic() < deadline and not self.stop:
            status, body = http_json(self.args.node_url, "/readyz", 2.0)
            if status == 200 and body.get("ready"):
                print(json.dumps({"initial_ready": True, "body": body}), flush=True)
                return True
            if self.node is not None and self.node.poll() is not None:
                print("node exited during startup", file=sys.stderr)
                return False
            time.sleep(1)
        print(json.dumps({"initial_ready": False}), flush=True)
        return False

    def sample(self) -> None:
        status, ready = http_json(self.args.node_url, "/readyz", 3.0)
        _, account = http_json(self.args.node_url, "/account", 3.0)
        _, positions = http_json(self.args.node_url, "/positions", 3.0)
        _, orders = http_json(self.args.node_url, "/orders", 3.0)
        td = ready.get("td", {}) if isinstance(ready, dict) else {}
        phase = str(td.get("phase", "unknown"))
        is_ready = bool(ready.get("ready")) if isinstance(ready, dict) else False
        expected = bool(td.get("expected_disconnect"))
        now = time.monotonic()

        if is_ready:
            if self.last_ready is False and self.down_since is not None:
                gap = now - self.down_since
                # 以断线发生时的日历阶段为准；恢复时若已进入 open，不能把
                # 结算期间的预期断线误算成故障。
                if self.down_expected or self.last_phase in {"closed", "settlement"}:
                    self.expected_gaps += 1
                else:
                    self.unexpected_gaps += 1
                    print(json.dumps({"unexpected_gap_s": round(gap, 2), "phase": self.last_phase}),
                          flush=True)
            self.ready_since = now
            self.down_since = None
            self.down_expected = False
        else:
            if self.last_ready is True:
                self.down_since = now
                self.down_expected = expected

        position_count = len(positions.get("positions", {})) if isinstance(positions, dict) else 0
        order_count = len(orders.get("orders", {})) if isinstance(orders, dict) else 0
        row = {
            "ts": iso_now(),
            "ready_http": status,
            "ready": is_ready,
            "phase": phase,
            "expected_disconnect": expected,
            "md_connected": bool(ready.get("md", {}).get("connected")) if isinstance(ready, dict) else False,
            "td_connected": bool(td.get("connected")),
            "balance": account.get("balance") if isinstance(account, dict) else None,
            "available": account.get("available") if isinstance(account, dict) else None,
            "margin": account.get("margin") if isinstance(account, dict) else None,
            "position_count": position_count,
            "order_count": order_count,
        }
        self.rows.append(row)
        if self.out_file is not None:
            self.out_file.write(json.dumps(row, ensure_ascii=False) + "\n")
            self.out_file.flush()
        self.last_ready = is_ready
        self.last_phase = phase

    def summary(self) -> dict[str, Any]:
        return {
            "started_at": self.args.started_at,
            "finished_at": iso_now(),
            "samples": len(self.rows),
            "expected_gaps": self.expected_gaps,
            "unexpected_gaps": self.unexpected_gaps,
            "rows": str(self.args.out),
            "fatal": self.args.fatal,
        }

    def run(self) -> int:
        self.out_file = self.args.out.open("w", encoding="utf-8")
        self.start_processes()
        if not self.wait_initial():
            self.args.fatal = "initial readiness timeout"
            return 2
        deadline = time.monotonic() + self.args.duration
        next_sample = time.monotonic()
        while not self.stop and time.monotonic() < deadline:
            now = time.monotonic()
            if now < next_sample:
                time.sleep(min(1.0, next_sample - now))
                continue
            self.sample()
            next_sample += self.args.interval
            if len(self.rows) % 20 == 0:
                print(json.dumps(self.rows[-1], ensure_ascii=False), flush=True)
            if self.node is not None and self.node.poll() is not None:
                self.args.fatal = "node exited"
                break
            if self.gw is not None and self.gw.poll() is not None:
                self.args.fatal = "gateway exited"
                break
        return 0 if not self.args.fatal else 1


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--node-url", default="http://127.0.0.1:18081")
    parser.add_argument("--duration", type=float, default=6 * 3600)
    parser.add_argument("--interval", type=float, default=15.0)
    parser.add_argument("--startup-timeout", type=float, default=90.0)
    parser.add_argument("--out", type=Path, default=Path("artifacts/ctp_session_soak.jsonl"))
    parser.add_argument("--manage-processes", action="store_true")
    parser.add_argument("--gw-bin", default=str(ROOT / "build-ui/tqsdk-gw"))
    parser.add_argument("--node-bin", default=str(ROOT / "build-ui/tqsdk-node"))
    parser.add_argument("--gw-config", default=str(ROOT / "run/live/gw.toml"))
    parser.add_argument("--node-config", default=str(ROOT / "run/live/node.toml"))
    parser.add_argument("--gw-log", type=Path, default=Path("artifacts/ctp_session_gw.log"))
    parser.add_argument("--node-log", type=Path, default=Path("artifacts/ctp_session_node.log"))
    args = parser.parse_args()
    args.started_at = iso_now()
    args.fatal = ""
    return args


def main() -> int:
    args = parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    runner = Runner(args)
    signal.signal(signal.SIGINT, runner.handle_signal)
    signal.signal(signal.SIGTERM, runner.handle_signal)
    try:
        rc = runner.run()
    finally:
        runner.stop_processes()
        if runner.out_file is not None:
            runner.out_file.close()
        summary_path = args.out.with_suffix(".summary.json")
        summary_path.write_text(json.dumps(runner.summary(), ensure_ascii=False, indent=2) + "\n")
        print(json.dumps(runner.summary(), ensure_ascii=False, indent=2), flush=True)
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
